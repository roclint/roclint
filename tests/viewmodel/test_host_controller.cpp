#include "roclint/rewrite/StructuredEntityExtractor.hpp"
#include "HostController.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include "support/MockAccessibilityBridge.hpp"

#include <roclint/core/crypto/Sha256.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSemaphore>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using roclint::host::HostController;
using namespace roclint::core;
namespace bridge = roclint::bridge;

namespace {

const QString kPrompt = QString::fromStdString(fx::kChainPrompt);

const std::string kDiscoveryReply = R"({"status": "options_generated",
  "choice_groups": [{"dimension": "performance", "question": "What should faster mean?", "material": true,
    "alternatives": [
      {"label": "Lower latency per call", "why_material": "Optimizes single requests", "evidence": ["runs faster"]},
      {"label": "Higher throughput", "why_material": "Optimizes batches", "evidence": ["runs faster"]}]}]})";

const std::string kVerifiedRewrite = "Refactor the parser for lower latency per call.";
const std::string kUnverifiedRewrite = "Make the parser quicker for single requests.";

// Answers discovery and rewrite requests like a loopback Ollama runtime and records what was sent.
struct ScriptedRuntime {
    std::string rewriteText{kVerifiedRewrite};
    std::string wordRefinementReply{};
    std::string wordSurfaceReply{R"({"status":"clean","replacement":"","explanation":"","source":""})"};
    std::string discoveryReply{kDiscoveryReply};
    std::shared_ptr<std::vector<std::string>> discoveryReplies =
        std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::vector<std::string>> followUpReplies =
        std::make_shared<std::vector<std::string>>(std::initializer_list<std::string>{"{\"kind\":\"none\"}"});
    std::chrono::milliseconds discoveryDelay{0};
    std::chrono::milliseconds followUpDelay{0};
    std::shared_ptr<std::atomic_bool> rejectCustom = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::string>> sent = std::make_shared<std::vector<std::string>>();

    bridge::GraniteTransport transport() const {
        auto m = mutex;
        auto log = sent;
        const std::string rewrite = fx::rewriteReply(rewriteText);
        const auto delay = discoveryDelay;
        const auto followUpDelayCopy = followUpDelay;
        const std::string wordReply = wordRefinementReply;
        const std::string surfaceReply = wordSurfaceReply;
        const std::string discovery = discoveryReply;
        auto replies = followUpReplies;
        auto baseReplies = discoveryReplies;
        auto reject = rejectCustom;
        return [m, log, rewrite, delay, followUpDelayCopy, wordReply, surfaceReply, discovery, replies, baseReplies, reject]
               (std::string_view instruction) -> Result<std::string> {
            {
                std::lock_guard<std::mutex> lock(*m);
                log->emplace_back(instruction);
            }
            if (instruction.find("granite.word_surface_check.v1") != std::string_view::npos)
                return surfaceReply;
            if (instruction.find("granite.word_refinement.v4") != std::string_view::npos && !wordReply.empty()) {
                return wordReply;
            }
            if (instruction.find("granite.goldfish_word_audit.v3") != std::string_view::npos) {
                const bool second = instruction.find("wcand_2") != std::string_view::npos;
                return std::string(second
                    ? R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"},{"id":"wcand_2","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})"
                    : R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})");
            }
            if (instruction.find("granite.goldfish_discovery_audit.v1") != std::string_view::npos) {
                return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
            }
            if (instruction.find("granite.goldfish_audit.v2") != std::string_view::npos) {
                return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
            }
            if (instruction.find("granite.clarification_check.v1") != std::string_view::npos) {
                if (reject->load()) {
                    return std::string(R"({"meaning":"The answer does not address the question.","answers_question":false,"compatible":true,"feedback":"That answer doesn't settle this question yet."})");
                }
                return std::string(R"({"meaning":"The answer settles the question.","answers_question":true,"compatible":true,"feedback":""})");
            }
            if (instruction.find("interview follow-up") != std::string_view::npos) {
                if (followUpDelayCopy.count() > 0) {
                    std::this_thread::sleep_for(followUpDelayCopy);
                }
                std::lock_guard<std::mutex> lock(*m);
                if (replies->empty()) return std::string("{\"kind\":\"none\"}");
                std::string reply = replies->front();
                replies->erase(replies->begin());
                return reply;
            }
            if (!baseReplies->empty() &&
                instruction.find("substantial interview") != std::string_view::npos) {
                std::lock_guard<std::mutex> lock(*m);
                if (!baseReplies->empty()) {
                    std::string reply = baseReplies->front();
                    baseReplies->erase(baseReplies->begin());
                    return reply;
                }
            }
            if (instruction.find("<instructions>") != std::string_view::npos) {
                return rewrite;
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
            return discovery;
        };
    }

    void queueFollowUps(std::initializer_list<std::string> replies) const {
        std::lock_guard<std::mutex> lock(*mutex);
        followUpReplies->assign(replies);
    }

    void queueDiscoveries(std::initializer_list<std::string> replies) const {
        std::lock_guard<std::mutex> lock(*mutex);
        discoveryReplies->assign(replies);
    }

    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(*mutex);
        return *sent;
    }
};

// A minimal Ollama HTTP server on loopback, served from the GUI thread while tests pump events.
class FakeOllama {
public:
    bool available{true};
    int tagRequests{0};

    FakeOllama() {
        ROCLINT_CHECK(server_.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
            while (auto* socket = server_.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                    buffer->append(socket->readAll());
                    const auto headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) {
                        return;
                    }
                    const QByteArray head = buffer->left(headerEnd).toLower();
                    qsizetype length = 0;
                    if (const auto at = head.indexOf("content-length:"); at >= 0) {
                        length = head.mid(at + 15, head.indexOf("\r\n", at) - at - 15).trimmed().toLongLong();
                    }
                    if (buffer->size() < headerEnd + 4 + length) {
                        return;
                    }
                    respond(socket, head);
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }

private:
    void respond(QTcpSocket* socket, const QByteArray& head) {
        namespace json = roclint::core::json;
        QByteArray status = "200 OK";
        std::string body;
        if (!available) {
            status = "503 Service Unavailable";
            body = R"({"error":"starting"})";
        } else if (head.startsWith("get /api/tags")) {
            ++tagRequests;
            body = R"({"models":[{"name":"granite4.2:8b","digest":")" + std::string(64, 'b') +
                   R"(","size":1,"details":{"quantization_level":"Q4_K_M","parameter_size":"8.8B"}}]})";
        } else if (head.startsWith("post /api/generate")) {
            json::Value v = json::Value::object();
            v.set("done", json::Value::boolean(true));
            v.set("response", json::Value::string(kDiscoveryReply));
            body = json::serialize(v);
        } else {
            status = "404 Not Found";
            body = R"({"error":"not found"})";
        }
        QByteArray reply = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nConnection: close\r\n" +
                           "Content-Length: " + QByteArray::number(static_cast<qsizetype>(body.size())) + "\r\n\r\n";
        reply.append(body.data(), static_cast<qsizetype>(body.size()));
        socket->write(reply);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
};

bridge::GraniteModelInfo modelInfo() {
    auto m = fx::testModel();
    bridge::GraniteModelInfo info;
    info.modelId = m.modelId;
    info.digest = m.digest;
    info.quantization = m.quantization;
    info.transport = m.transport;
    return info;
}

void connectRuntime(HostController& c, const ScriptedRuntime& runtime) {
    c.setTransportOverrideForTesting(runtime.transport());
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    ROCLINT_CHECK(c.modelRuntimeState() == "READY");
}

void analyzeAndAnswer(HostController& c) {
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "READY");
    ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
    ROCLINT_CHECK(c.activeItemId() == "grp_0");
    ROCLINT_CHECK(c.activeAlternatives().size() == 2);
    ROCLINT_CHECK(!c.hasConfirmedIntent());
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("alt_0_0"), c.stateVersion())));
    ROCLINT_CHECK(c.interventionVerdict() == "QUIET");
    ROCLINT_CHECK(c.hasConfirmedIntent());
    ROCLINT_CHECK(c.isRewriteEnabled());
}

void synthesize(HostController& c) {
    c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.proposalStatus() == "READY_FOR_REVIEW");
    ROCLINT_CHECK(c.hasProposal());
}

bridge::ComprehensionReviewResult approvingReviewResult() {
    bridge::ComprehensionReviewResult result;
    result.status = bridge::ComprehensionReviewStatus::Completed;
    result.interpretedOutcome =
        "The rewrite asks for a lower-latency parser refactor while keeping the original target.";
    result.interpretedRequirements = {"Keep the parser target", "Improve single-request latency"};
    result.readyForTask = true;
    result.rationale = "Fresh-context test review: the candidate preserves the confirmed decision.";
    return result;
}

void waitForReviewState(HostController& c, const QString& expected, int timeoutMs = 5000) {
    // The review is a queued command plus a worker; wait for both before judging
    // the state so a previous same-state review cannot mask the new result.
    ROCLINT_CHECK(c.waitForIdleForTesting(timeoutMs));
    QElapsedTimer timer;
    timer.start();
    while (c.comprehensionReviewState() != expected && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    ROCLINT_CHECK(c.comprehensionReviewState() == expected);
}

void completeReview(HostController& c) {
    c.setComprehensionReviewForTesting([](const bridge::ComprehensionReviewRequest&) {
        return approvingReviewResult();
    });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(c.canApplyReviewedOutput());
}

void setClipboard(const QString& text) {
    if (auto* cb = QGuiApplication::clipboard()) {
        cb->setText(text);
    }
}

QString clipboardText() {
    auto* cb = QGuiApplication::clipboard();
    return cb ? cb->text() : QString();
}

void testLineEndingsAreNormalizedOnCommit() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(QStringLiteral("line one\r\nline two\rline three"), 0)));
    ROCLINT_CHECK(c.currentRevisionText() == "line one\nline two\nline three");
    ROCLINT_CHECK(c.stateVersion() == 1);
    ROCLINT_CHECK(c.currentRevisionId() == QStringLiteral("rev_") + c.currentSessionIdString() + QStringLiteral("_1"));
    const auto history = c.getSessionHistory(c.currentSessionIdString());
    ROCLINT_CHECK(history.size() == 1);
    ROCLINT_CHECK(history[0].toMap()[QStringLiteral("text")].toString() == "line one\nline two\nline three");
    std::cout << "  [PASS] CRLF and bare CR are normalized at the command boundary.\n";
}

void testStaleExpectedVersionIsRejected() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.commitPrompt(QStringLiteral("first"), 7)));
    ROCLINT_CHECK(c.stateVersion() == 0);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(QStringLiteral("first"), 0)));
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.commitPrompt(QStringLiteral("second"), 0)));
    ROCLINT_CHECK(!c.lastError().isEmpty());
    ROCLINT_CHECK(c.currentRevisionText() == "first");
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.noteDraftEdited(99)));
    ROCLINT_CHECK(!c.isDraftDirty());
    std::cout << "  [PASS] Commands with a stale expected version are rejected.\n";
}

void testEditAndCommitClearDownstreamState() {
    ScriptedRuntime runtime;
    {
        HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        analyzeAndAnswer(c);
        synthesize(c);

        ROCLINT_CHECK(c.waitForCommandForTesting(c.noteDraftEdited(c.stateVersion())));
        ROCLINT_CHECK(c.isDraftDirty());
        ROCLINT_CHECK(c.analysisStatus() == "IDLE");
        ROCLINT_CHECK(!c.analysisForTesting().has_value());
        ROCLINT_CHECK(c.workingIntentForTesting().roundsCompleted == 0);
        ROCLINT_CHECK(c.activeAlternatives().isEmpty());
        ROCLINT_CHECK(c.interventionVerdict().isEmpty());
        ROCLINT_CHECK(!c.isClarifying());
        ROCLINT_CHECK(!c.hasConfirmedIntent());
        ROCLINT_CHECK(!c.hasProposal());
        ROCLINT_CHECK(c.proposalStatus() == "NONE");
        ROCLINT_CHECK(!c.isRewriteEnabled());
    }
    {
        HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        analyzeAndAnswer(c);
        synthesize(c);
        const auto version = c.stateVersion();
        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(kPrompt + QStringLiteral(" Keep the public API."), version)));
        ROCLINT_CHECK(c.stateVersion() == version + 1);
        ROCLINT_CHECK(!c.analysisForTesting().has_value());
        ROCLINT_CHECK(!c.confirmedIntentForTesting().has_value());
        ROCLINT_CHECK(!c.proposalForTesting().has_value());
        ROCLINT_CHECK(!c.hasApproval());
    }
    std::cout << "  [PASS] Editing or committing clears analysis, decision, intent and proposal.\n";
}

void testRewriteRefusesWithoutIntentOrWithDirtyDraft() {
    ScriptedRuntime runtime;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    setClipboard(QStringLiteral("untouched"));

    // No analysis at all.
    ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(kPrompt, 0)));
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(!c.lastError().isEmpty());

    // Analysis is waiting on a question: no confirmed intent yet.
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(!c.hasProposal());

    // Intent confirmed, then the draft is edited.
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("alt_0_0"), c.stateVersion())));
    ROCLINT_CHECK(c.hasConfirmedIntent());
    ROCLINT_CHECK(c.waitForCommandForTesting(c.noteDraftEdited(c.stateVersion())));
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.approveAndCopyRewrite(c.stateVersion())));

    ROCLINT_CHECK(c.lastCopiedText().isEmpty());
    ROCLINT_CHECK(clipboardText() == "untouched");
    for (const auto& request : runtime.requests()) {
        ROCLINT_CHECK(request.find("granite.rewrite.format.v9") == std::string::npos);
    }
    std::cout << "  [PASS] Rewrite refuses without a valid intent or with a dirty draft, and never copies.\n";
}

void testConfirmChoiceRejectsForeignAlternative() {
    ScriptedRuntime runtime;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.isClarifying());

    ROCLINT_CHECK(!c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("alt_1_0"), c.stateVersion())));
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("injected"), c.stateVersion())));
    ROCLINT_CHECK(!c.lastError().isEmpty());
    ROCLINT_CHECK(c.roundsCompleted() == 0);
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(!c.hasConfirmedIntent());
    std::cout << "  [PASS] confirmChoice rejects alternatives that are not on the active item.\n";
}

void testApproveAndCopyPersistsBeforeCopying() {
    ScriptedRuntime runtime;
    runtime.rewriteText = kUnverifiedRewrite;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    analyzeAndAnswer(c);

    setClipboard(QStringLiteral("before"));
    synthesize(c);
    // Proposing never copies.
    ROCLINT_CHECK(clipboardText() == "before");
    ROCLINT_CHECK(c.lastCopiedText().isEmpty());
    ROCLINT_CHECK(c.proposalText() == QString::fromStdString(kUnverifiedRewrite));
    ROCLINT_CHECK(c.proposalForTesting()->transport == fx::testModel().transport);
    ROCLINT_CHECK(c.proposalForTesting()->modelDigest == fx::testModel().digest);

    // The intent wording is not mechanically present, so a human must review it first.
    ROCLINT_CHECK(c.pendingReviewCount() == 1);
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.approveAndCopyRewrite(c.stateVersion())));
    ROCLINT_CHECK(clipboardText() == "before");
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.acknowledgeConstraint(QStringLiteral("rewrite_nonempty"), true, c.stateVersion())));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.acknowledgeConstraint(QStringLiteral("intent_item_grp_0"), true, c.stateVersion())));
    ROCLINT_CHECK(c.pendingReviewCount() == 0);
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    const auto session = SessionId(c.currentSessionIdString().toStdString());
    bool approvalStoredBeforeCopy = false;
    QObject::connect(&c, &HostController::promptCopied, [&](const QString&) {
        auto current = c.repositoryForTesting()->getCurrentApproval(session).waitForTesting();
        approvalStoredBeforeCopy = current.ok() && current.value().has_value();
    });

    ROCLINT_CHECK(c.waitForCommandForTesting(c.approveAndCopyRewrite(c.stateVersion())));
    ROCLINT_CHECK(approvalStoredBeforeCopy);
    ROCLINT_CHECK(c.hasApproval());
    ROCLINT_CHECK(c.approvalLabel() == "Wording reviewed");
    ROCLINT_CHECK(c.lastCopiedText() == QString::fromStdString(kUnverifiedRewrite));
    ROCLINT_CHECK(clipboardText() == QString::fromStdString(kUnverifiedRewrite));

    auto stored = c.repositoryForTesting()->getCurrentApproval(session).waitForTesting();
    ROCLINT_CHECK(stored.ok() && stored.value().has_value());
    ROCLINT_CHECK(stored.value()->approvedText == kUnverifiedRewrite);
    ROCLINT_CHECK(stored.value()->proposalHash == c.proposalForTesting()->proposalHash);

    // Changing the rewrite style discards the proposal and its approval.
    c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("ANTHROPIC_STRUCTURED@1"), c.stateVersion()));
    ROCLINT_CHECK(!c.hasProposal());
    ROCLINT_CHECK(!c.hasApproval());
    auto after = c.repositoryForTesting()->getCurrentApproval(session).waitForTesting();
    ROCLINT_CHECK(after.ok() && !after.value().has_value());
    std::cout << "  [PASS] Approve & Copy validates, persists the approval, then copies.\n";
}

void testStaleDiscoveryResultIsDiscarded() {
    ScriptedRuntime runtime;
    runtime.discoveryDelay = std::chrono::milliseconds(300);
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.isAnalyzing());
    ROCLINT_CHECK(c.waitForCommandForTesting(c.noteDraftEdited(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));

    ROCLINT_CHECK(!c.analysisForTesting().has_value());
    ROCLINT_CHECK(!c.isClarifying());
    auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value()[0].status == roclint::storage::AttemptStatus::DiscardedStale);
    ROCLINT_CHECK(attempts.value()[0].transport == "test_override_transport");
    std::cout << "  [PASS] A discovery result for an outdated draft is discarded and recorded as stale.\n";
}

void testOutboundPromptsCarryNoLocalContext() {
    QTemporaryDir dir;
    ROCLINT_CHECK(dir.isValid());
    const QString dbPath = dir.path() + QStringLiteral("/history.db");
    ScriptedRuntime runtime;
    HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    analyzeAndAnswer(c);
    synthesize(c);

    const auto requests = runtime.requests();
    // Discovery, the stage-specific discovery audit, and rewrite all legitimately run.
    ROCLINT_CHECK(requests.size() >= 3);
    const std::string tempRoot = QDir::toNativeSeparators(dir.path()).toStdString();
    const std::string tempRootSlash = dir.path().toStdString();
    for (const auto& r : requests) {
        ROCLINT_CHECK(r.find(tempRoot) == std::string::npos);
        ROCLINT_CHECK(r.find(tempRootSlash) == std::string::npos);
        ROCLINT_CHECK(r.find("history.db") == std::string::npos);
        ROCLINT_CHECK(r.find(".cpp") == std::string::npos);
        ROCLINT_CHECK(r.find("scope lock") == std::string::npos);
        ROCLINT_CHECK(r.find("Refactor the parser so it runs faster.") != std::string::npos);
    }
    std::cout << "  [PASS] Outbound prompts carry only the user's text and confirmed intent.\n";
}

void testApprovalSurvivesReopen() {
    QTemporaryDir dir;
    ROCLINT_CHECK(dir.isValid());
    const QString dbPath = dir.path() + QString::fromUtf8("/r" "\xC3\xA9" "sum" "\xC3\xA9" " history.db");
    ScriptedRuntime runtime;
    QString sessionId;
    std::string proposalHash;
    {
        HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        analyzeAndAnswer(c);
        synthesize(c);
        // A discarded earlier proposal advances the generation; the restored one must still bind.
        ROCLINT_CHECK(c.waitForCommandForTesting(c.discardProposal(c.stateVersion())));
        synthesize(c);
        ROCLINT_CHECK(c.pendingReviewCount() == 0);
        completeReview(c);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.approveAndCopyRewrite(c.stateVersion())));
        sessionId = c.currentSessionIdString();
        proposalHash = c.proposalForTesting()->proposalHash;
    }
    {
        HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(sessionId, c.stateVersion())));
        ROCLINT_CHECK(c.analysisStatus() == "READY");
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(c.hasProposal());
        ROCLINT_CHECK(c.proposalForTesting()->proposalHash == proposalHash);
        ROCLINT_CHECK(c.proposalStatus() == "READY_FOR_REVIEW");
        ROCLINT_CHECK(c.hasApproval());
        ROCLINT_CHECK(c.approvalLabel() == "Wording reviewed");

        // Restoring the open session again keeps the stored approval current.
        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(sessionId, c.stateVersion())));
        ROCLINT_CHECK(c.hasApproval());
        auto stored = c.repositoryForTesting()->getCurrentApproval(SessionId(sessionId.toStdString())).waitForTesting();
        ROCLINT_CHECK(stored.ok() && stored.value().has_value());
    }
    {
        // A proposal discarded before quitting is not resurrected.
        HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(sessionId, c.stateVersion())));
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.discardProposal(c.stateVersion())));
    }
    {
        HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(sessionId, c.stateVersion())));
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(!c.hasProposal());
        ROCLINT_CHECK(!c.hasApproval());
    }
    std::cout << "  [PASS] Proposal and approval survive reopen; discarded ones stay discarded.\n";
}

void testHardBypassCopiesCommittedText() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.hardBypass(QStringLiteral("ship it\r\nnow"), 0)));
    ROCLINT_CHECK(c.lastCopiedText() == "ship it\nnow");
    ROCLINT_CHECK(clipboardText() == "ship it\nnow");
    ROCLINT_CHECK(c.stateVersion() == 2); // Root creation, then explicit bypass invalidation.
    ROCLINT_CHECK(!c.hasApproval());
    std::cout << "  [PASS] Hard bypass commits and copies the user's own text only.\n";
}

void testManualModeDisablesRewrite() {
    ScriptedRuntime runtime;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.switchToManual(c.stateVersion())));
    ROCLINT_CHECK(c.isManual());
    ROCLINT_CHECK(!c.hasConfirmedIntent());
    ROCLINT_CHECK(!c.isRewriteEnabled());
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    std::cout << "  [PASS] Manual mode never produces an intent or a rewrite.\n";
}

void testAnalysisReconnectsToTheRuntime() {
    FakeOllama ollama;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(ollama.url()));
    ROCLINT_CHECK(c.modelRuntimeState() == "UNLOADED");
    c.setActiveModel(QStringLiteral("granite4.2:8b"));

    // An explicit model selection starts connecting; analysis waits for it, then runs.
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.isAnalyzing());
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(c.modelRuntimeState() == "READY");
    ROCLINT_CHECK(c.analysisStatus() == "READY");
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(c.activeAlternatives().size() == 2);
    auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value()[0].status == roclint::storage::AttemptStatus::Succeeded);
    ROCLINT_CHECK(attempts.value()[0].transport == bridge::kLoopbackOllamaTransport);
    std::cout << "  [PASS] Analysis connects to an unchecked runtime and then runs.\n";
}

void testAnalysisRecoversAfterRuntimeFailure() {
    FakeOllama ollama;
    ollama.available = false;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(ollama.url()));
    c.setActiveModel(QStringLiteral("granite4.2:8b"));

    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(c.modelRuntimeState() == "FAILED");
    ROCLINT_CHECK(c.analysisStatus() == "FAILED");
    ROCLINT_CHECK(!c.lastError().contains(QStringLiteral("HTTP 503")));
    ROCLINT_CHECK(c.lastError().contains(QStringLiteral("Retry explicitly")));
    ROCLINT_CHECK(c.activeAlternatives().isEmpty());

    // Once the runtime is back, the next request reconnects instead of repeating the old failure.
    ollama.available = true;
    const int checksBefore = ollama.tagRequests;
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(ollama.tagRequests > checksBefore);
    ROCLINT_CHECK(c.modelRuntimeState() == "READY");
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(c.lastError().isEmpty());
    std::cout << "  [PASS] A failed runtime is re-checked on the next request and recovers.\n";
}

void testUnreachableRuntimeExplainsWhatToDo() {
    quint16 freePort = 0;
    {
        QTcpServer probe;
        ROCLINT_CHECK(probe.listen(QHostAddress::LocalHost, 0));
        freePort = probe.serverPort();
    }
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(freePort))));
    c.setActiveModel(QStringLiteral("granite4.2:8b"));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(c.analysisStatus() == "FAILED");
    ROCLINT_CHECK(c.lastError().contains(QStringLiteral("Retry")));
    ROCLINT_CHECK(!c.lastError().contains(QStringLiteral("FAILED")));
    ROCLINT_CHECK(!c.isClarifying());
    std::cout << "  [PASS] An unreachable runtime fails visibly with an actionable message.\n";
}

void testMalformedModelReplyIsExplainedPlainly() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    c.setTransportOverrideForTesting([](std::string_view) -> Result<std::string> {
        return std::string(R"({"status": "needs_clarification", "choice_groups": [], "clarification_question": ""})");
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "FAILED");
    ROCLINT_CHECK(c.lastError().startsWith(QStringLiteral("The local model returned an unusable reply.")));
    ROCLINT_CHECK(c.lastError().contains(QStringLiteral("Retry explicitly")));
    ROCLINT_CHECK(!c.lastError().contains(QStringLiteral("Detail:")));
    ROCLINT_CHECK(!c.lastError().contains(QStringLiteral("clarification_question")));
    ROCLINT_CHECK(c.currentRevisionText().toStdString() == kPrompt.toStdString());
    ROCLINT_CHECK(!c.isClarifying());
    ROCLINT_CHECK(c.modelRuntimeState() == "READY");
    std::cout << "  [PASS] A malformed model reply fails visibly with a plain explanation.\n";
}

void testModelDiagnosticsStayInternal() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        c.setTransportOverrideForTesting([scenario](std::string_view) -> Result<std::string> {
            if (scenario == 0) return std::string(R"({"status":"RAW_DIAGNOSTIC_SENTINEL"})");
            if (scenario == 1) return std::string(R"({"error":"RAW_DIAGNOSTIC_SENTINEL"})");
            throw std::runtime_error("RAW_DIAGNOSTIC_SENTINEL");
        });
        ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(c.analysisStatus() == "FAILED");
        ROCLINT_CHECK(c.currentRevisionText() == kPrompt);
        ROCLINT_CHECK(c.lastError().contains("Retry explicitly"));
        ROCLINT_CHECK(!c.lastError().contains("RAW_DIAGNOSTIC_SENTINEL"));
        ROCLINT_CHECK(!c.analysisMessage().contains("RAW_DIAGNOSTIC_SENTINEL"));
        for (const auto& item : c.availableModels())
            ROCLINT_CHECK(!item.toMap().value("status").toString().contains("RAW_DIAGNOSTIC_SENTINEL"));
    }
}

void testDelayedCommandsCannotMutateTheNewState() {
    ScriptedRuntime runtime;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    const auto beforeChoice = c.stateVersion();
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("alt_0_0"), beforeChoice)));
    ROCLINT_CHECK(c.stateVersion() == beforeChoice + 1);
    const auto intent = *c.confirmedIntentForTesting();
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.skipCurrentItem(beforeChoice)));
    ROCLINT_CHECK(c.lastErrorCode() == "STALE_VERSION");
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.switchToManual(beforeChoice)));
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.synthesizeRewrite(beforeChoice)));
    ROCLINT_CHECK(c.stateVersion() == beforeChoice + 1 && *c.confirmedIntentForTesting() == intent);
    synthesize(c);
    completeReview(c);
    const auto beforeApproval = c.stateVersion();
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.approveAndCopyRewrite(beforeChoice)));
    ROCLINT_CHECK(!c.hasApproval());
    ROCLINT_CHECK(c.waitForCommandForTesting(c.approveAndCopyRewrite(beforeApproval)));
    const auto approvedVersion = c.stateVersion();
    ROCLINT_CHECK(approvedVersion == beforeApproval + 1);
    const auto copied = QGuiApplication::clipboard()->text();
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.approveAndCopyRewrite(beforeApproval)));
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.discardProposal(beforeApproval)));
    ROCLINT_CHECK(c.stateVersion() == approvedVersion && c.hasApproval());
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == copied);
    ROCLINT_CHECK(c.repositoryForTesting()->getSession(SessionId(c.currentSessionIdString().toStdString())).waitForTesting().value().stateVersion == approvedVersion);

    // Another repository writer advances the stored state while this controller still sees its old version.
    const auto restored = c.repositoryForTesting()->restoreSession(SessionId(c.currentSessionIdString().toStdString())).waitForTesting().value();
    RevisionState external(restored.latestRevision.revision, restored.session.highestRevisionSeq, restored.session.stateVersion,
                           restored.session.draftEditGeneration, restored.session.proposalEditGeneration);
    ROCLINT_CHECK(external.advanceState(approvedVersion).ok());
    ROCLINT_CHECK(c.repositoryForTesting()->advanceSessionState(external, approvedVersion, 100).waitForTesting().ok());
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.discardProposal(approvedVersion)));
    ROCLINT_CHECK(c.stateVersion() == approvedVersion && !c.hasApproval() && c.storageNeedsRecovery());
}

void testAnalysisGroupDeadlineAndExplicitRetry() {
    ScriptedRuntime runtime;
    runtime.discoveryDelay = std::chrono::milliseconds(300);
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    c.setAnalysisGroupTimeoutForTesting(60);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    QElapsedTimer elapsed; elapsed.start();
    while (c.analysisStatus() != "FAILED" && elapsed.elapsed() < 1000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    ROCLINT_CHECK(c.analysisStatus() == "FAILED" && elapsed.elapsed() < 250);
    ROCLINT_CHECK(!c.finalSnapshotForTesting() && !c.hasConfirmedIntent());
    const auto firstVersion = c.stateVersion();
    auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value().front().status == roclint::storage::AttemptStatus::Failed);
    ROCLINT_CHECK(attempts.value().front().errorMessage.find("ANALYSIS_GROUP_DEADLINE") != std::string::npos);

    // An explicit retry while the expired transport drains must resume once it finishes.
    runtime.discoveryDelay = std::chrono::milliseconds(0);
    c.setTransportOverrideForTesting(runtime.transport());
    c.setAnalysisGroupTimeoutForTesting(1500);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "READY" && c.finalSnapshotForTesting());
    ROCLINT_CHECK(c.stateVersion() > firstVersion && !c.hasConfirmedIntent());
    attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 2);
    int failed = 0, succeeded = 0;
    for (const auto& attempt : attempts.value()) {
        failed += attempt.status == roclint::storage::AttemptStatus::Failed;
        succeeded += attempt.status == roclint::storage::AttemptStatus::Succeeded;
    }
    ROCLINT_CHECK(failed == 1 && succeeded == 1);
}

void testSubmitClarificationDeferral() {
    // 1. isDeferralPhrase detection
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("   ")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("idk")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("IDK")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral(" idk ")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("idk lol")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("idk tbh")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("you pick")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("u pick")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("you decide")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("?")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("??")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("...")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("doesn't matter")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("doesnt matter")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("either")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("both")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("no preference")));
    ROCLINT_CHECK(HostController::isDeferralPhrase(QStringLiteral("any")));
    ROCLINT_CHECK(!HostController::isDeferralPhrase(QStringLiteral("I prefer lower latency")));
    ROCLINT_CHECK(!HostController::isDeferralPhrase(QStringLiteral("idkwhat")));

    // 2. High confidence auto-selects recommended option
    {
        ScriptedRuntime runtime;
        HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
        ROCLINT_CHECK(c.activeItemId() == "grp_0");

        c.setRecommendedOptionForTesting(QStringLiteral("Lower latency per call"), 0.84);

        QString deferredMsg;
        bool wasAutoSelected = false;
        QObject::connect(&c, &HostController::clarificationDeferred, [&](const QString& msg, bool autoSelected) {
            deferredMsg = msg;
            wasAutoSelected = autoSelected;
        });

        ROCLINT_CHECK(c.waitForCommandForTesting(c.submitClarification(QStringLiteral("idk lol"), c.stateVersion())));
        ROCLINT_CHECK(wasAutoSelected);
        ROCLINT_CHECK(deferredMsg.contains("Used your delegated choice: Lower latency per call"));
        ROCLINT_CHECK(c.ambientStatus().contains("Used your delegated choice: Lower latency per call"));
        ROCLINT_CHECK(c.interventionVerdict() == "QUIET");
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(c.confirmedIntentForTesting().has_value());
        const auto& intent = c.confirmedIntentForTesting().value();
        const auto decision = intent.getDecision("grp_0");
        ROCLINT_CHECK(decision.has_value());
        ROCLINT_CHECK(decision->kind == intent::IntentResolutionKind::SelectedAlternative);
        ROCLINT_CHECK(decision->chosenAlternativeId == "alt_0_0");
    }

    // 3. Low confidence falls back to keeping original
    {
        ScriptedRuntime runtime;
        HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
        ROCLINT_CHECK(c.activeItemId() == "grp_0");

        c.setRecommendedOptionForTesting(QStringLiteral("Lower latency per call"), 0.55);

        QString deferredMsg;
        bool wasAutoSelected = true;
        QObject::connect(&c, &HostController::clarificationDeferred, [&](const QString& msg, bool autoSelected) {
            deferredMsg = msg;
            wasAutoSelected = autoSelected;
        });

        ROCLINT_CHECK(c.waitForCommandForTesting(c.submitClarification(QStringLiteral("you decide"), c.stateVersion())));
        ROCLINT_CHECK(!wasAutoSelected);
        ROCLINT_CHECK(deferredMsg == "Kept your original wording; no option was selected.");
        ROCLINT_CHECK(c.ambientStatus() == "Kept your original wording; no option was selected.");
        ROCLINT_CHECK(c.interventionVerdict() == "QUIET");
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(c.confirmedIntentForTesting().has_value());
        const auto& intent = c.confirmedIntentForTesting().value();
        const auto decision = intent.getDecision("grp_0");
        ROCLINT_CHECK(decision.has_value());
        ROCLINT_CHECK(decision->kind == intent::IntentResolutionKind::UseOriginal);
    }

    // 4. Non-deferral submits genuine custom intent
    {
        ScriptedRuntime runtime;
        HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
        ROCLINT_CHECK(c.activeItemId() == "grp_0");

        c.setRecommendedOptionForTesting(QStringLiteral("Lower latency per call"), 0.90);

        bool deferralFired = false;
        QObject::connect(&c, &HostController::clarificationDeferred, [&](const QString&, bool) {
            deferralFired = true;
        });

        ROCLINT_CHECK(c.waitForCommandForTesting(c.submitClarification(QStringLiteral("Optimize for memory footprint"), c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(!deferralFired);
        ROCLINT_CHECK(c.interventionVerdict() == "QUIET");
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(c.confirmedIntentForTesting().has_value());
        const auto& intent = c.confirmedIntentForTesting().value();
        const auto decision = intent.getDecision("grp_0");
        ROCLINT_CHECK(decision.has_value());
        ROCLINT_CHECK(decision->kind == intent::IntentResolutionKind::CustomText);
        ROCLINT_CHECK(decision->customText == "Optimize for memory footprint");
    }
    std::cout << "  [PASS] Free-text clarification deferrals auto-select or fall back cleanly.\n";
}

void testCustomAnswersRequireCurrentModelApproval() {
    HostController c(":memory:");
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    std::atomic<int> checks{0};
    std::atomic<bool> delay{false};
    std::string reply = R"({"meaning":"The answer does not settle the question.","answers_question":false,"compatible":true,"feedback":"What performance goal did you mean?"})";
    c.setTransportOverrideForTesting([&](std::string_view prompt) -> Result<std::string> {
        if (prompt.find("granite.clarification_check.v1") == std::string_view::npos) return kDiscoveryReply;
        ++checks;
        ROCLINT_CHECK(QThread::currentThread() != c.thread());
        ROCLINT_CHECK(prompt.find("What should faster mean?") != std::string_view::npos);
        if (delay) QThread::msleep(100);
        if (reply == "timeout") return makeError(ErrorCode::ValidationFailed, "Synthetic timeout");
        return reply;
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000) && c.isClarifying());
    const auto version = c.stateVersion();
    for (const auto* answer : {"iondwfwoefneiofneionfeio", "I had toast for breakfast"}) {
        ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent(answer, version)));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(c.stateVersion() == version && c.roundsCompleted() == 0 && c.isClarifying());
        ROCLINT_CHECK(!c.hasConfirmedIntent() && !c.hasProposal() && !c.hasApproval());
        ROCLINT_CHECK(c.clarificationFeedback() == "What performance goal did you mean?");
    }
    reply = "malformed";
    ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent("Lower latency", version)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000) && !c.hasConfirmedIntent() && c.stateVersion() == version);
    ROCLINT_CHECK(c.clarificationFeedback().contains("couldn't check"));
    reply = "timeout";
    ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent("Lower latency", version)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000) && !c.hasConfirmedIntent() && c.stateVersion() == version);
    ROCLINT_CHECK(c.roundsCompleted() == 0 && c.clarificationFeedback().contains("couldn't check"));
    reply = R"({"meaning":"The answer settles the question.","answers_question":true,"compatible":true,"feedback":""})";
    delay = true;
    ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent("Prefer lower latency per call.", version)));
    ROCLINT_CHECK(c.isCheckingClarification());
    ROCLINT_CHECK(!c.waitForCommandForTesting(c.submitCustomIntent("Second concurrent answer", version)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.hasConfirmedIntent() && c.roundsCompleted() == 1 && c.stateVersion() == version + 1);
    ROCLINT_CHECK(c.confirmedIntentForTesting()->getDecision("grp_0")->customText == "Prefer lower latency per call.");
    ROCLINT_CHECK(checks == 5);

    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent("Lower latency", c.stateVersion())));
    ROCLINT_CHECK(c.isCheckingClarification());
    ROCLINT_CHECK(c.waitForCommandForTesting(c.noteDraftEdited(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.isDraftDirty() && !c.hasConfirmedIntent() && !c.isCheckingClarification());
    ROCLINT_CHECK(c.clarificationFeedback().isEmpty());
    const auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().back().stage == "clarification_check" &&
        attempts.value().back().status == roclint::storage::AttemptStatus::DiscardedStale);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent("Lower latency", c.stateVersion())));
    ROCLINT_CHECK(c.isCheckingClarification());
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice("alt_0_1", c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000) && c.isRewriteEnabled());
    const auto chosen = c.confirmedIntentForTesting()->getDecision("grp_0");
    ROCLINT_CHECK(chosen->chosenAlternativeId == "alt_0_1" && chosen->customText.empty());
    std::cout << "  [PASS] Custom answers require current model approval; rejected, malformed, duplicate and stale checks cannot settle intent.\n";
}

void testChoicePolarityUnderRuleConflict() {
    ScriptedRuntime runtime;
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kPrompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
    ROCLINT_CHECK(c.activeItemId() == "grp_0");
    ROCLINT_CHECK(c.activeAlternatives().size() == 2);

    // Set recommended option to the safe rule-compliant override
    c.setRecommendedOptionForTesting(QStringLiteral("[Safe Rule-Compliant Override] Lower latency per call"), 0.85);

    // An invented prefix is not a current alternative and must not select the first option.
    ROCLINT_CHECK(c.waitForCommandForTesting(c.submitClarification(QStringLiteral("idk"), c.stateVersion())));
    ROCLINT_CHECK(c.interventionVerdict() == "QUIET");
    ROCLINT_CHECK(c.hasConfirmedIntent());
    ROCLINT_CHECK(c.confirmedIntentForTesting().has_value());
    const auto& intent = c.confirmedIntentForTesting().value();
    const auto decision = intent.getDecision("grp_0");
    ROCLINT_CHECK(decision.has_value());
    ROCLINT_CHECK(decision->kind == intent::IntentResolutionKind::UseOriginal);
    ROCLINT_CHECK(decision->chosenAlternativeId.empty());
    std::cout << "  [PASS] Unmatched recommendations preserve original wording instead of selecting the first option.\n";
}

void testStrictNegativeGuardrailsPassThrough() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    const QString prompt = QStringLiteral("Implement private in-flight promise deduplicator. Do not alter the public interface in apiClient.ts.");
    ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(prompt, 0)));
    ROCLINT_CHECK(c.currentRevisionText() == prompt);
    ROCLINT_CHECK(c.stateVersion() == 1);
    std::cout << "  [PASS] Strict negative guardrails on affirmative tasks pass through cleanly.\n";
}

void testCommandPaletteAndAssistanceModes() {
    // 1. Static command prefix parsing
    {
        auto r1 = HostController::parseCommandPrefix(QStringLiteral("/tech Render packet forwarding"));
        ROCLINT_CHECK(r1.hasCommand);
        ROCLINT_CHECK(r1.mode == HostController::AssistanceMode::Tech);
        ROCLINT_CHECK(r1.cleanText == QStringLiteral("Render packet forwarding"));

        auto r2 = HostController::parseCommandPrefix(QStringLiteral("/STRICT  Refactor auth logic "));
        ROCLINT_CHECK(r2.hasCommand);
        ROCLINT_CHECK(r2.mode == HostController::AssistanceMode::Strict);
        ROCLINT_CHECK(r2.cleanText == QStringLiteral("Refactor auth logic"));

        auto r3 = HostController::parseCommandPrefix(QStringLiteral("/create"));
        ROCLINT_CHECK(r3.hasCommand);
        ROCLINT_CHECK(r3.mode == HostController::AssistanceMode::Create);
        ROCLINT_CHECK(r3.cleanText.isEmpty());

        auto r4 = HostController::parseCommandPrefix(QStringLiteral("/inspect Show AST symbols"));
        ROCLINT_CHECK(r4.hasCommand);
        ROCLINT_CHECK(r4.mode == HostController::AssistanceMode::Inspect);
        ROCLINT_CHECK(r4.cleanText == QStringLiteral("Show AST symbols"));

        auto r5 = HostController::parseCommandPrefix(QStringLiteral("/quiet Do minimal check"));
        ROCLINT_CHECK(r5.hasCommand);
        ROCLINT_CHECK(r5.mode == HostController::AssistanceMode::Quiet);
        ROCLINT_CHECK(r5.cleanText == QStringLiteral("Do minimal check"));

        auto r6 = HostController::parseCommandPrefix(QStringLiteral("/auto Normal prompt"));
        ROCLINT_CHECK(r6.hasCommand);
        ROCLINT_CHECK(r6.mode == HostController::AssistanceMode::Auto);
        ROCLINT_CHECK(r6.cleanText == QStringLiteral("Normal prompt"));

        // Non-command words starting with slash or normal text
        auto r7 = HostController::parseCommandPrefix(QStringLiteral("/technicality is not a command"));
        ROCLINT_CHECK(!r7.hasCommand);
        ROCLINT_CHECK(r7.cleanText == QStringLiteral("/technicality is not a command"));

        auto r8 = HostController::parseCommandPrefix(QStringLiteral("Standard prompt without prefix"));
        ROCLINT_CHECK(!r8.hasCommand);
        ROCLINT_CHECK(r8.cleanText == QStringLiteral("Standard prompt without prefix"));
    }

    // 2. Integration with HostController session dispatch
    {
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        ROCLINT_CHECK(c.assistanceMode() == QStringLiteral("auto"));
        ROCLINT_CHECK(!c.isInspectMode());

        // Commit prompt with /strict prefix
        const QString strictPrompt = QStringLiteral("/strict Refactor database pool without altering schema");
        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(strictPrompt, 0)));
        ROCLINT_CHECK(c.assistanceMode() == QStringLiteral("strict"));
        ROCLINT_CHECK(c.currentRevisionText() == QStringLiteral("Refactor database pool without altering schema"));

        // Switch to /inspect
        c.setAssistanceMode(QStringLiteral("inspect"));
        ROCLINT_CHECK(c.assistanceMode() == QStringLiteral("inspect"));
        ROCLINT_CHECK(c.isInspectMode());

        // Switch back to /auto
        c.setAssistanceMode(QStringLiteral("auto"));
        ROCLINT_CHECK(c.assistanceMode() == QStringLiteral("auto"));
        ROCLINT_CHECK(!c.isInspectMode());
    }
    std::cout << "  [PASS] Command palette prefix detection and assistance modes work accurately.\n";
}

void testGoldfishAuditPipelineGating() {
    // 1. Test extractSessionConstraints
    {
        intent::ConfirmedIntent intent;
        intent.id = "intent_1";
        intent::ConfirmedItem item1;
        item1.itemId = "grp_perf";
        item1.kind = intent::IntentResolutionKind::SelectedAlternative;
        item1.chosenValue = "Lower latency per call";
        intent.items.push_back(item1);

        const std::string prompt = "Refactor query builder, but do not touch the token validation logic. Ensure Layer 3 packet forwarding is preserved.";
        const auto constraints = HostController::extractSessionConstraints(prompt, intent);

        ROCLINT_CHECK(constraints.size() >= 3);
        // User choice constraint
        ROCLINT_CHECK(constraints[0].id == "intent_grp_perf");
        ROCLINT_CHECK(constraints[0].rule == "Lower latency per call");
        ROCLINT_CHECK(constraints[0].enforcement == Enforcement::Hard);
        ROCLINT_CHECK(constraints[0].polarity == Polarity::Require);

        // Prohibit boundary constraint
        bool foundProhibit = false;
        for (const auto& c : constraints) {
            if (c.polarity == Polarity::Prohibit) {
                foundProhibit = true;
                ROCLINT_CHECK(c.rule.find("token validation") != std::string::npos);
                ROCLINT_CHECK(c.enforcement == Enforcement::Hard);
            }
        }
        ROCLINT_CHECK(foundProhibit);

        // Domain invariant
        bool foundDomain = false;
        for (const auto& c : constraints) {
            if (c.origin == Origin::Domain) {
                foundDomain = true;
                ROCLINT_CHECK(c.appliesWhen == "depicts_ipv4_forwarding");
                ROCLINT_CHECK(c.authorityRef == "RFC 1812");
            }
        }
        ROCLINT_CHECK(foundDomain);
    }

    // 2. Test live proposal gating under Goldfish audit
    {
        ScriptedRuntime runtime;
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);

        const QString prompt = QStringLiteral("Refactor the query builder so it runs faster, but do not touch the token validation logic.");
        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(prompt, 0)));

        // Setup confirmed intent
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(prompt, c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("alt_0_0"), c.stateVersion())));
        ROCLINT_CHECK(c.hasConfirmedIntent());

        // Now mock a rewrite that breaches the negative prohibition!
        const std::string breachingRewrite = "Refactor the query builder for lower latency per call and modify the token validation logic for efficiency.";
        c.setTransportOverrideForTesting([&](std::string_view instruction) -> Result<std::string> {
            if (instruction.find("granite.goldfish_audit.v2") != std::string_view::npos) {
                return std::string(R"({"outcome": "Fail", "violations": [{"constraint_id": "prohibit_boundary_1", "explanation": "Candidate rewrite modifies token validation logic"}]})");
            }
            return fx::rewriteReply(breachingRewrite);
        });

        // Trigger rewrite synthesis
        ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));

        // Verify that Goldfish audit rejected the rewrite!
        ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("VALIDATION_FAILED"));
        ROCLINT_CHECK(c.auditOutcome() == QStringLiteral("Fail"));
        ROCLINT_CHECK(!c.auditViolations().isEmpty());
        ROCLINT_CHECK(c.proposalMessage().contains(QStringLiteral("Audit rejected rewrite")));

        // Now mock a compliant rewrite that preserves the negative boundary!
        const std::string compliantRewrite = "Refactor the query builder for lower latency per call, without touching the token validation logic.";
        c.setTransportOverrideForTesting([&](std::string_view instruction) -> Result<std::string> {
            if (instruction.find("granite.goldfish_audit.v2") != std::string_view::npos) {
                return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
            }
            return fx::rewriteReply(compliantRewrite);
        });

        // Retry rewrite
        ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));

        // Compliant rewrite passes audit and is ReadyForReview!
        ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("READY_FOR_REVIEW"));
        ROCLINT_CHECK(c.auditOutcome() == QStringLiteral("Pass"));
        ROCLINT_CHECK(c.auditViolations().isEmpty());
        ROCLINT_CHECK(c.hasProposal());
    }
    std::cout << "  [PASS] Goldfish audit proposal gating and constraint extraction verified.\n";
}

void testMultiCandidateCreativeExpansion() {
    ScriptedRuntime runtime;
    runtime.rewriteText = "Refactor the parser for lower latency per call, streamlining execution paths and minimizing per-request overhead.";
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    analyzeAndAnswer(c);

    synthesize(c);
    ROCLINT_CHECK(c.hasProposal());
    ROCLINT_CHECK(c.hasCreativeAlternatives());
    const auto alts = c.creativeAlternatives();
    ROCLINT_CHECK(alts.size() >= 2);
    ROCLINT_CHECK(c.selectedAlternativeIndex() == 0);

    const auto initialProposalHash = c.proposalForTesting()->proposalHash;
    const auto initialRewrittenText = c.proposalText();

    // Select alternative 1
    c.selectAlternative(1);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.selectedAlternativeIndex() == 1);
    ROCLINT_CHECK(c.proposalText() != initialRewrittenText);
    ROCLINT_CHECK(c.proposalForTesting()->proposalHash != initialProposalHash);
    ROCLINT_CHECK(c.proposalText() == alts[1].toMap().value("rewrittenText").toString());

    // Selecting out-of-bounds does not alter selection
    c.selectAlternative(99);
    ROCLINT_CHECK(c.selectedAlternativeIndex() == 1);

    // Editing the draft invalidates creative alternatives
    ROCLINT_CHECK(c.waitForCommandForTesting(c.noteDraftEdited(c.stateVersion())));
    ROCLINT_CHECK(!c.hasCreativeAlternatives());
    ROCLINT_CHECK(c.creativeAlternatives().isEmpty());
    ROCLINT_CHECK(c.selectedAlternativeIndex() == 0);

    std::cout << "  [PASS] Multi-candidate creative expansion expands, switches, reseals, and invalidates.\n";
}

void testTokenSpeedModesAndMetrics() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    ROCLINT_CHECK(c.tokenSpeedMode() == "BOTH");

    c.setTokenSpeedMode("GENERATION");
    ROCLINT_CHECK(c.tokenSpeedMode() == "GENERATION");

    c.setTokenSpeedMode("DECODE");
    ROCLINT_CHECK(c.tokenSpeedMode() == "DECODE");

    c.setTokenSpeedMode("BOTH");
    ROCLINT_CHECK(c.tokenSpeedMode() == "BOTH");

    std::cout << "  [PASS] Token speed modes (BOTH, GENERATION, DECODE) switch and persist accurately.\n";
}

void testF07ReviewReuseAndVersionAdvancement() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    std::atomic<int> auditCalls{0};
    c.setTransportOverrideForTesting([&](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            ++auditCalls;
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"intent_grp_0","explanation":"Manual review required"}]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply("Refactor the parser for lower latency per call, streamlining execution paths and minimizing per-request overhead.");
        }
        return kDiscoveryReply;
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    ROCLINT_CHECK(c.pendingReviewCount() == 1);
    for (const auto& v : c.proposalConstraints()) {
        const auto item = v.toMap();
        if (item.value("needsReview").toBool()) {
            ROCLINT_CHECK(c.waitForCommandForTesting(c.acknowledgeConstraint(item.value("id").toString(), true, c.stateVersion())));
        }
    }
    ROCLINT_CHECK(c.pendingReviewCount() == 0);
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    const auto prevVersion = c.stateVersion();
    const auto prevHash = c.proposalForTesting()->proposalHash;
    const auto prevCalls = auditCalls.load();

    // Select alternative 1
    c.selectAlternative(1);

    // The proposal changes immediately, but only a committed save advances the version.
    ROCLINT_CHECK(c.stateVersion() == prevVersion);
    ROCLINT_CHECK(c.proposalForTesting()->proposalHash != prevHash);
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.stateVersion() == prevVersion + 1);
    ROCLINT_CHECK(c.pendingReviewCount() == 1);
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(auditCalls.load() > prevCalls);

    std::cout << "  [PASS] F07: Stale review acknowledgments cleared and state version advanced on selection.\n";
}

void testF08SelectionPersistenceAndApproval() {
    QTemporaryDir tempDir;
    ROCLINT_CHECK(tempDir.isValid());
    const QString dbPath = tempDir.filePath("f08_test.sqlite3");

    HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    std::atomic<int> auditCalls{0};
    auto discovery = kDiscoveryReply;
    const auto at = discovery.find("Lower latency per call");
    if (at != std::string::npos) {
        discovery.replace(at, std::string("Lower latency per call").size(), "lower call latency");
    }
    c.setTransportOverrideForTesting([&](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            ++auditCalls;
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply("Refactor the parser for lower call latency and clearer error paths.");
        }
        return discovery;
    });

    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    const auto baseHash = c.proposalForTesting()->proposalHash;
    const auto session = c.currentSessionIdString();

    // Select alternative 1
    c.selectAlternative(1);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    const auto selectedHash = c.proposalForTesting()->proposalHash;
    const auto selectedId = c.proposalForTesting()->id;
    ROCLINT_CHECK(selectedHash != baseHash);
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    // Approve and copy rewrite
    const auto approved = c.waitForCommandForTesting(c.approveAndCopyRewrite(c.stateVersion()));
    ROCLINT_CHECK(approved);
    ROCLINT_CHECK(c.lastError().isEmpty());

    // Restore session and verify selected candidate is retained
    ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(session, c.stateVersion())));
    ROCLINT_CHECK(c.proposalForTesting()->id == selectedId);
    ROCLINT_CHECK(c.proposalForTesting()->proposalHash == selectedHash);
    ROCLINT_CHECK(c.proposalForTesting()->proposalHash != baseHash);
    ROCLINT_CHECK(c.auditOutcome() == QStringLiteral("Pass"));
    ROCLINT_CHECK(c.auditExecutionStatus() == QStringLiteral("Completed"));

    std::cout << "  [PASS] F08: Selected alternative persisted, approved without FK error, and restored cleanly.\n";
}

void testF09UnresolvedHardAuditGating() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    c.setTransportOverrideForTesting([](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"intent_grp_0","explanation":"Cannot verify the hard performance requirement"}]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply(kVerifiedRewrite);
        }
        return kDiscoveryReply;
    });

    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    // F09: Unresolved hard invariant must NOT allow unearned approval
    ROCLINT_CHECK(c.auditOutcome() == QStringLiteral("Unresolved"));
    ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("READY_FOR_REVIEW"));
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.pendingReviewCount() == 1);

    // User explicitly reviews and acknowledges the uncertainty
    bool acknowledged = false;
    for (const auto& v : c.proposalConstraints()) {
        const auto item = v.toMap();
        if (item.value("needsReview").toBool()) {
            ROCLINT_CHECK(c.waitForCommandForTesting(c.acknowledgeConstraint(item.value("id").toString(), true, c.stateVersion())));
            acknowledged = true;
        }
    }
    ROCLINT_CHECK(acknowledged);
    ROCLINT_CHECK(c.pendingReviewCount() == 0);
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    std::cout << "  [PASS] F09: Hard unresolved audit findings gate approval until explicitly acknowledged.\n";
}

void testAsyncSelectionNativeTicketAndOffGuiExecution() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    auto guiAudits = std::make_shared<std::atomic<int>>(0);
    auto workerAudits = std::make_shared<std::atomic<int>>(0);

    c.setTransportOverrideForTesting([guiAudits, workerAudits](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
                ++(*guiAudits);
            } else {
                ++(*workerAudits);
            }
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply(kVerifiedRewrite);
        }
        return kDiscoveryReply;
    });

    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    // Initial audit ran during synthesize
    ROCLINT_CHECK(workerAudits->load() == 1);
    ROCLINT_CHECK(guiAudits->load() == 0);

    // Select candidate 1
    c.selectAlternative(1);

    // Immediately on GUI thread: status is Generating, cannot approve, zero GUI audits
    ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("GENERATING"));
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(guiAudits->load() == 0);

    // Wait for async selection audit to finish
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(guiAudits->load() == 0);
    ROCLINT_CHECK(workerAudits->load() == 2);
    ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("READY_FOR_REVIEW"));
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());
    ROCLINT_CHECK(c.auditExecutionStatus() == QStringLiteral("Completed"));

    // Verify creativeAlternatives map exposes auditExecutionStatus honestly
    const auto alts = c.creativeAlternatives();
    ROCLINT_CHECK(!alts.isEmpty());
    ROCLINT_CHECK(alts[1].toMap().value("auditExecutionStatus").toString() == QStringLiteral("Completed"));

    std::cout << "  [PASS] Async selection: Native attempt ticket bound, audited off GUI thread, honesty exposed.\n";
}

void testAsyncSelectionDelayedTransportResponsiveness() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    c.setTransportOverrideForTesting([](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            QThread::msleep(100);
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply(kVerifiedRewrite);
        }
        return kDiscoveryReply;
    });

    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    c.selectAlternative(1);
    ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("GENERATING"));
    ROCLINT_CHECK(!c.hasProposal());
    ROCLINT_CHECK(!c.canApprove());

    // UI remains responsive while background audit is in flight
    QCoreApplication::processEvents();

    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("READY_FOR_REVIEW"));
    ROCLINT_CHECK(c.hasProposal());
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    std::cout << "  [PASS] Async selection: Delayed transport keeps UI responsive and blocks approval until complete.\n";
}

void testAsyncSelectionStaleSelectionRejection() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    std::atomic<int> auditCount{0};
    c.setTransportOverrideForTesting([&](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            ++auditCount;
            QThread::msleep(80);
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply(kVerifiedRewrite);
        }
        return kDiscoveryReply;
    });

    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    const auto alts = c.creativeAlternatives();
    ROCLINT_CHECK(alts.size() >= 2);

    // Select a different candidate, then return to the base while its audit is pending.
    const auto version = c.stateVersion();
    c.selectAlternative(1);
    c.selectAlternative(0);
    ROCLINT_CHECK(c.stateVersion() == version);

    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.stateVersion() == version + 1);

    // The latest alternative should win; the earlier completion must be rejected as stale.
    ROCLINT_CHECK(c.selectedAlternativeIndex() == 1);
    ROCLINT_CHECK(c.proposalText() == alts[1].toMap().value("rewrittenText").toString());
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    std::cout << "  [PASS] Async selection: Rapid selection rejects stale audit results.\n";
}

void testDraftEditDuringCandidateAudit() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    auto releaseAudit = std::make_shared<QSemaphore>();
    auto auditCalls = std::make_shared<std::atomic<int>>(0);
    c.setTransportOverrideForTesting([releaseAudit, auditCalls](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
            if (++*auditCalls == 2) ROCLINT_CHECK(releaseAudit->tryAcquire(1, 5000));
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) return fx::rewriteReply(kVerifiedRewrite);
        return kDiscoveryReply;
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    const auto version = c.stateVersion();
    c.selectAlternative(1);
    ROCLINT_CHECK(c.stateVersion() == version && !c.canApprove());
    const auto draft = c.noteDraftEdited(version);
    const auto saved = c.waitForCommandForTesting(draft, 2000);
    releaseAudit->release();
    ROCLINT_CHECK(saved);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.isStorageAvailable() && c.stateVersion() == version + 1);
    ROCLINT_CHECK(!c.hasProposal() && !c.canApprove());
    ROCLINT_CHECK(auditCalls->load() == 2);
}

void testDraftEditDuringCandidateSave(bool failCandidateSave) {
    QTemporaryDir tempDir;
    ROCLINT_CHECK(tempDir.isValid());
    const auto dbPath = tempDir.filePath("candidate_and_draft.sqlite3");
    HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    c.setTransportOverrideForTesting([](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos)
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        if (in.find("<instructions>") != std::string_view::npos) return fx::rewriteReply(kVerifiedRewrite);
        return kDiscoveryReply;
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);
    const auto version = c.stateVersion();
    const SessionId session(c.currentSessionIdString().toStdString());
    auto before = c.repositoryForTesting()->getSession(session).waitForTesting();
    ROCLINT_CHECK(before.ok());

    roclint::storage::Database writer(std::filesystem::path(dbPath.toStdString()));
    if (failCandidateSave) {
        ROCLINT_CHECK(writer.execute(
            "CREATE TRIGGER reject_candidate BEFORE INSERT ON proposals "
            "BEGIN SELECT RAISE(ABORT, 'simulated candidate save failure'); END;").ok());
    }
    ROCLINT_CHECK(writer.execute("BEGIN IMMEDIATE;").ok());
    c.selectAlternative(1);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!c.candidateWriteInFlightForTesting() && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    ROCLINT_CHECK(c.candidateWriteInFlightForTesting() && !c.canApprove());
    const auto firstDraft = c.noteDraftEdited(version);
    const auto secondDraft = c.noteDraftEdited(c.stateVersion());
    ROCLINT_CHECK(firstDraft && secondDraft);
    // Let the draft debounce fire while SQLite is still waiting for the writer.
    ROCLINT_CHECK(!c.waitForCommandForTesting(firstDraft, 250));
    ROCLINT_CHECK(c.candidateWriteInFlightForTesting());
    ROCLINT_CHECK(writer.execute("COMMIT;").ok());
    ROCLINT_CHECK(c.waitForCommandForTesting(firstDraft));
    ROCLINT_CHECK(c.waitForCommandForTesting(secondDraft));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));

    auto after = c.repositoryForTesting()->getSession(session).waitForTesting();
    ROCLINT_CHECK(after.ok());
    ROCLINT_CHECK(c.stateVersion() == version + (failCandidateSave ? 2 : 3));
    ROCLINT_CHECK(after.value().stateVersion == c.stateVersion());
    ROCLINT_CHECK(after.value().draftEditGeneration == before.value().draftEditGeneration + 2);
    ROCLINT_CHECK(after.value().proposalEditGeneration == before.value().proposalEditGeneration);
    ROCLINT_CHECK(c.isStorageAvailable() && !c.hasProposal() && !c.canApprove());
}

void testAsyncSelectionSaveFailurePreventsApproval() {
    QTemporaryDir tempDir;
    ROCLINT_CHECK(tempDir.isValid());
    const QString dbPath = tempDir.filePath("save_fail_test.sqlite3");

    HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    c.setTransportOverrideForTesting([](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply(kVerifiedRewrite);
        }
        return kDiscoveryReply;
    });

    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    // Corrupt / drop proposals table behind the scenes
    {
        roclint::storage::Database db(std::filesystem::path(dbPath.toStdString()));
        ROCLINT_CHECK(db.execute("DROP TABLE approvals; DROP TABLE proposals;").ok());
    }

    // Now select alternative 1
    c.selectAlternative(1);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));

    // Save failure must transition proposal to Failed and NOT expose approval
    ROCLINT_CHECK(c.proposalStatus() == QStringLiteral("FAILED"));
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(!c.hasProposal());

    std::cout << "  [PASS] Async selection: SQLite persistence failure prevents publishing approvable state.\n";
}

void testSessionRestoreUncertaintyPreservation() {
    QTemporaryDir tempDir;
    ROCLINT_CHECK(tempDir.isValid());
    const QString dbPath = tempDir.filePath("restore_uncertainty_test.sqlite3");

    QString session;
    [[maybe_unused]] quint64 version = 0;
    {
        HostController c(dbPath);
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);

        c.setTransportOverrideForTesting([](std::string_view in) -> Result<std::string> {
            if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
                return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"intent_grp_0","explanation":"Cannot verify the hard performance requirement"}]})");
            }
            if (in.find("<instructions>") != std::string_view::npos) {
                return fx::rewriteReply(kVerifiedRewrite);
            }
            return kDiscoveryReply;
        });

        ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
        analyzeAndAnswer(c);
        synthesize(c);

        ROCLINT_CHECK(c.auditOutcome() == QStringLiteral("Unresolved"));
        ROCLINT_CHECK(!c.canApprove());
        ROCLINT_CHECK(c.pendingReviewCount() == 1);
        for (const auto& value : c.proposalConstraints()) {
            const auto item = value.toMap();
            ROCLINT_CHECK(item.value("evidence").toString() != item.value("id").toString());
            ROCLINT_CHECK(!item.value("description").toString().startsWith("Audit uncertainty:"));
        }

        session = c.currentSessionIdString();
        version = c.stateVersion();
    }

    // Restore in a fresh controller
    {
        HostController c(dbPath);
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);

        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(session, c.stateVersion())));

        // Unresolved audit uncertainty must be preserved on restore
        ROCLINT_CHECK(c.hasProposal());
        ROCLINT_CHECK(c.auditOutcome() == QStringLiteral("Unresolved"));
        ROCLINT_CHECK(!c.canApprove());
        ROCLINT_CHECK(c.pendingReviewCount() == 1);
    }

    // Now test restore when auditResult is absent in the database
    {
        roclint::storage::Database db(std::filesystem::path(dbPath.toStdString()));
        ROCLINT_CHECK(db.execute("UPDATE proposals SET payload_json = REPLACE(payload_json, '\"audit_result\":', '\"old_audit\":');").ok());

        HostController c(dbPath);
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);

        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(session, c.stateVersion())));

        // Missing audit evidence must enforce hard-uncertainty gating
        ROCLINT_CHECK(c.hasProposal());
        ROCLINT_CHECK(!c.canApprove());
        ROCLINT_CHECK(c.pendingReviewCount() > 0);
    }

    std::cout << "  [PASS] Session restore: Audit uncertainty preserved; absent evidence enforces hard-uncertainty gating.\n";
}

void testExactIdAcknowledgmentBinding() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    c.setTransportOverrideForTesting([](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v2") != std::string_view::npos) {
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"intent_grp_0","explanation":"Hard latency invariant"}]})");
        }
        if (in.find("<instructions>") != std::string_view::npos) {
            return fx::rewriteReply(kVerifiedRewrite);
        }
        return kDiscoveryReply;
    });

    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    analyzeAndAnswer(c);
    synthesize(c);

    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.pendingReviewCount() == 1);

    // Substring or mismatched IDs must NOT acknowledge intent_grp_0
    (void)c.waitForCommandForTesting(c.acknowledgeConstraint(QStringLiteral("grp_0"), true, c.stateVersion()));
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.pendingReviewCount() == 1);

    (void)c.waitForCommandForTesting(c.acknowledgeConstraint(QStringLiteral("intent_grp_00"), true, c.stateVersion()));
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.pendingReviewCount() == 1);

    // Exact ID "intent_grp_0" or canonically mapped "intent_item_grp_0" succeeds
    ROCLINT_CHECK(c.waitForCommandForTesting(c.acknowledgeConstraint(QStringLiteral("intent_grp_0"), true, c.stateVersion())));
    ROCLINT_CHECK(c.pendingReviewCount() == 0);
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    std::cout << "  [PASS] Exact ID binding: Substrings do not conflate; exact & canonical mapping binds correctly.\n";
}

void testUnavailableBridgeReportsExplicitRangeResult() {
    auto bridge = roclint::host::createUnavailableAccessibilityBridge();
    bool done = false;
    roclint::host::InjectionOutcome outcome = roclint::host::InjectionOutcome::Failed;
    QObject::connect(bridge.get(), &roclint::host::IAccessibilityBridge::rangeInjectionFinished,
                     [&](quint64 id, roclint::host::InjectionOutcome value, const QString&) {
        if (id == 7) { done = true; outcome = value; }
    });
    roclint::host::RangeInjectionRequest request;
    request.requestId = 7;
    request.startOffset = 0;
    request.endOffset = 3;
    request.originalRangeText = QStringLiteral("abc");
    request.replacementText = QStringLiteral("xyz");
    request.expectedCurrentText = QStringLiteral("abc");
    bridge->injectRange(request);
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    ROCLINT_CHECK(done);
    ROCLINT_CHECK(outcome == roclint::host::InjectionOutcome::NotSupported);

    std::cout << "  [PASS] Unsupported range surfaces report an explicit completion result.\n";
}

void testMechanicallyExactEditDoesNotAcquireAConfirmationQuestion() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    c.setTransportOverrideForTesting([](std::string_view instruction) -> Result<std::string> {
        if (instruction.find("granite.goldfish_discovery_audit.v1") != std::string_view::npos)
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        if (instruction.find("granite.discovery.selective.v11") != std::string_view::npos)
            return std::string(R"({"status":"needs_clarification","assistance_kind":"clarification",
                "choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[],
                "clarification_question":"Please confirm the exact node identifier for the button."})");
        return fx::rewriteReply(kVerifiedRewrite);
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));

    const QString prompt = QStringLiteral(
        "Change the button label from 'Save' to 'Store' in SettingsPage.qml; do not change any other text.");
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(prompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "READY");
    ROCLINT_CHECK(!c.isClarifying());
    ROCLINT_CHECK(c.analysisForTesting().has_value());
    ROCLINT_CHECK(c.analysisForTesting()->choiceGroups.empty());
    ROCLINT_CHECK(c.analysisForTesting()->missingDecisions.empty());
    ROCLINT_CHECK(c.analysisForTesting()->conflicts.empty());

    std::cout << "  [PASS] An exact quoted mechanical edit does not acquire an invented confirmation question.\n";
}

void testRecommendationCannotCrossDecisionGroups() {
    const std::string twoGroups = R"({"status":"options_generated","assistance_kind":"clarification",
      "choice_groups":[
        {"dimension":"latency","question":"What should faster mean?","material":true,
         "alternatives":[
           {"label":"Lower latency per call","why_material":"Single requests finish sooner","evidence":["runs faster"]},
           {"label":"Higher throughput","why_material":"More work completes per interval","evidence":["runs faster"]}]},
        {"dimension":"cache location","question":"Where should the cache live?","material":true,
         "alternatives":[
           {"label":"In-process memory","why_material":"Fast but local to one process","evidence":["uses a cache"]},
           {"label":"Shared cache service","why_material":"Reusable across processes","evidence":["uses a cache"]}]}],
      "assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""})";
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    c.setTransportOverrideForTesting([twoGroups](std::string_view instruction) -> Result<std::string> {
        if (instruction.find("granite.goldfish_audit.v2") != std::string_view::npos)
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        if (instruction.find("granite.discovery.selective.v11") != std::string_view::npos)
            return twoGroups;
        return fx::rewriteReply(kVerifiedRewrite);
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));

    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(
        QStringLiteral("Refactor the parser so it runs faster and uses a cache."), c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.activeItemId() == "grp_0");
    c.setRecommendedOptionForTesting(QStringLiteral("Lower latency per call"), 0.92);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("alt_0_0"), c.stateVersion())));
    ROCLINT_CHECK(c.activeItemId() == "grp_1");

    bool autoSelected = true;
    QObject::connect(&c, &HostController::clarificationDeferred,
                     [&](const QString&, bool automatic) { autoSelected = automatic; });
    ROCLINT_CHECK(c.waitForCommandForTesting(c.submitClarification(QStringLiteral("idk"), c.stateVersion())));
    ROCLINT_CHECK(!autoSelected);
    ROCLINT_CHECK(c.workingIntentForTesting().confirmedAlternatives.find("grp_1") ==
                  c.workingIntentForTesting().confirmedAlternatives.end());

    std::cout << "  [PASS] A recommendation bound to one decision group never auto-selects another.\n";
}

void testDiscoveryAuditPreservesRemainingPaletteDecision() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    std::atomic<int> auditRound{0};
    c.setTransportOverrideForTesting([&](std::string_view instruction) -> Result<std::string> {
        if (instruction.find("granite.goldfish_discovery_audit.v1") != std::string_view::npos) {
            if (auditRound.fetch_add(1) == 0) {
                return std::string(R"({"outcome":"Fail","violations":[{"item_id":"alt_0_0","explanation":"Alternative violates the explicit purple prohibition"}],"unresolved":[]})");
            }
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        }
        if (instruction.find("granite.discovery.selective.v11") != std::string_view::npos) {
            return std::string(R"({"status":"options_generated","assistance_kind":"clarification",
              "choice_groups":[{"dimension":"background color","question":"Which background should the banner use?","material":true,
                "alternatives":[
                  {"label":"Purple gradient","why_material":"Uses the forbidden color","evidence":["palette"]},
                  {"label":"Ocean blue","why_material":"A safe cool background","evidence":["palette"]},
                  {"label":"Forest green","why_material":"A safe natural background","evidence":["palette"]}]}],
              "assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""})");
        }
        return fx::rewriteReply(kVerifiedRewrite);
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));

    const QString prompt = QStringLiteral("Design a palette. The background cannot be purple.");
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(prompt, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "READY");
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(c.analysisForTesting().has_value());
    const auto& analysis = *c.analysisForTesting();
    ROCLINT_CHECK(analysis.choiceGroups.size() == 1);
    const auto& palette = analysis.choiceGroups.front();
    ROCLINT_CHECK(palette.question.find("background") != std::string::npos);
    ROCLINT_CHECK(palette.alternatives.size() == 2);
    for (const auto& alt : palette.alternatives) {
        ROCLINT_CHECK(alt.interpretation.find("Purple") == std::string::npos);
        ROCLINT_CHECK(alt.interpretation == "Ocean blue" || alt.interpretation == "Forest green");
    }
    // Rejecting purple must not erase the unresolved blue/green decision or declare the prompt complete.
    ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
    ROCLINT_CHECK(!c.hasConfirmedIntent());
    ROCLINT_CHECK(auditRound.load() >= 1);

    std::cout << "  [PASS] Discovery audit prunes only the forbidden palette option and keeps the open decision.\n";
}

bool waitForWordSuggestion(HostController& c, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!c.hasWordRefinement() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(10);
    }
    return c.hasWordRefinement();
}

void testWordCompanionBindsSelectedCandidateAndRejectsStaleRequests() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* bridgePtr = bridge.get();
    HostController c(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());

    ScriptedRuntime runtime;
    runtime.wordRefinementReply = R"({
      "needs_clarification": false,
      "clarification_question": "",
      "candidates": [
        {"original_span": "faster", "replacement": "snappier on first paint",
         "explanation": "Improves how quickly the first useful view appears.",
         "assumption": "The complaint is about first paint.", "requires_clarification": false},
        {"original_span": "faster", "replacement": "quicker to refresh data",
         "explanation": "Improves refresh latency once the dashboard is open.",
         "assumption": "The complaint is about refresh.", "requires_clarification": false}
      ]})";
    connectRuntime(c, runtime);
    c.setAmbientModeEnabled(true);

    QString text = QStringLiteral("Make the dashboard feel faster for people on slow laptops.");
    const int wordStart = text.indexOf(QStringLiteral("faster"));
    ROCLINT_CHECK(wordStart > 0);
    roclint::host::ObservedField field;
    field.fieldToken = 4242;
    field.appIdentifier = QStringLiteral("Code.exe");
    field.appDisplayName = QStringLiteral("Visual Studio Code");
    field.processId = 1234;
    field.kind = roclint::host::FieldKind::GuiEditor;
    field.text = text;
    field.caretOffset = wordStart + 6;
    field.caretRect = QRect(120, 160, 2, 18);
    field.fieldRect = QRect(60, 120, 620, 120);
    bridgePtr->observe(field);

    ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    ROCLINT_CHECK(c.wordRefinementCandidates().size() == 2);
    ROCLINT_CHECK(c.wordRefinementOriginal() == QStringLiteral("faster"));
    ROCLINT_CHECK(c.wordRefinementSelectedCandidateId() == QStringLiteral("wcand_1"));
    ROCLINT_CHECK(c.wordRefinementReplacement() == QStringLiteral("snappier on first paint"));
    ROCLINT_CHECK(c.wordRefinementAssumption().contains(QStringLiteral("first paint")));

    // Selecting a non-default candidate must send that exact replacement, not the default.
    const quint64 stale = c.applyWordRefinement(c.stateVersion() + 1, QStringLiteral("wcand_2"));
    ROCLINT_CHECK(stale == 0);
    ROCLINT_CHECK(bridgePtr->rangeInjections.empty());

    const quint64 requestId = c.applyWordRefinement(c.stateVersion(), QStringLiteral("wcand_2"));
    ROCLINT_CHECK(requestId != 0);
    ROCLINT_CHECK(bridgePtr->rangeInjections.size() == 1);
    const auto& request = bridgePtr->rangeInjections.front();
    ROCLINT_CHECK(request.fieldToken == field.fieldToken);
    ROCLINT_CHECK(request.originalRangeText == QStringLiteral("faster"));
    ROCLINT_CHECK(request.replacementText == QStringLiteral("quicker to refresh data"));
    ROCLINT_CHECK(request.expectedCurrentText == text);
    ROCLINT_CHECK(request.startOffset == wordStart);
    ROCLINT_CHECK(request.endOffset == wordStart + 6);

    // A second click while the first request is pending must not enqueue another write.
    ROCLINT_CHECK(c.applyWordRefinement(c.stateVersion(), QStringLiteral("wcand_1")) == 0);
    ROCLINT_CHECK(bridgePtr->rangeInjections.size() == 1);

    // A changed draft invalidates the frozen suggestion before it can be applied.
    QString changed = text + QStringLiteral(" Keep the public API unchanged.");
    field.text = changed;
    field.caretOffset = changed.size();
    bridgePtr->observe(field);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    ROCLINT_CHECK(c.applyWordRefinement(c.stateVersion(), QStringLiteral("wcand_1")) == 0);
    ROCLINT_CHECK(bridgePtr->rangeInjections.size() == 1);

    std::cout << "  [PASS] Word companion applies the selected candidate and rejects stale/mismatched requests.\n";
}

void testFeedbackOpinionsAreNotMutationProhibitions() {
    using roclint::core::Polarity;
    const roclint::core::intent::ConfirmedIntent intent;
    for (const auto& prompt : {
        "I don't like how this UI interrupts my typing.",
        "I dont like this cluttered UI; improve its layout.",
        "I do not enjoy these intrusive popups. Make them less distracting."
    }) {
        const auto constraints = HostController::extractSessionConstraints(prompt, intent);
        ROCLINT_CHECK(std::none_of(constraints.begin(), constraints.end(), [](const auto& c) {
            return c.polarity == Polarity::Prohibit && c.aspect == "boundary";
        }));
    }
    for (const auto& prompt : {
        "I don't like the layout, but do not change the API.",
        "I don't like the layout and don't change public signatures.",
        "I don't want the API changed.",
        "Do not like or share any posts.",
        "Improve this UI without changing its keyboard shortcuts."
    }) {
        const auto constraints = HostController::extractSessionConstraints(prompt, intent);
        ROCLINT_CHECK(std::any_of(constraints.begin(), constraints.end(), [](const auto& c) {
            return c.polarity == Polarity::Prohibit && c.aspect == "boundary";
        }));
    }
}

void testWordApplyFeedbackIsReadbackBoundAndFollowsGeometry() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* native = bridge.get(); native->autoCompleteRangeInjection = false;
    HostController c(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    ScriptedRuntime runtime;
    runtime.wordRefinementReply = R"({"action":"replace","options":[{"text":"quicker to load","effect":"Specifies loading","assume":""}]})";
    connectRuntime(c, runtime); c.setAmbientModeEnabled(true);
    roclint::host::ObservedField field;
    field.fieldToken = 7491; field.processId = 1234;
    field.appIdentifier = "Code.exe"; field.appDisplayName = "Visual Studio Code";
    field.text = "Make the dashboard feel faster"; field.caretOffset = field.text.size();
    field.caretRect = QRect(200, 160, 2, 20); field.fieldRect = QRect(80, 100, 620, 180);
    native->observe(field);
    ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    ROCLINT_CHECK(c.wordCompanionState() == "ready");
    c.setAmbientBadgeSuggestionsOnly(true);
    ROCLINT_CHECK(c.wordReplacementReady() && c.isAmbientBadgeVisible());
    c.setAmbientBadgeSuggestionsOnly(false);
    ROCLINT_CHECK(!c.wordCompanionVisible() && c.isAmbientBadgeVisible() && !c.isCardVisible());
    ROCLINT_CHECK(native->rangeInjections.empty());
    c.activateAmbientIndicator();
    ROCLINT_CHECK(c.wordCompanionVisible() && !c.isAmbientBadgeVisible() && !c.isCardVisible());
    // A geometry-only observation moves the existing suggestion, without a new model request.
    field.caretRect.translate(400, 200); field.fieldRect.translate(400, 200);
    native->observe(field); QCoreApplication::processEvents();
    ROCLINT_CHECK(c.wordRefinementAnchorRect() == field.caretRect);
    const auto first = c.applyWordRefinement(c.stateVersion());
    ROCLINT_CHECK(first && c.wordCompanionState() == "applying" && c.wordCompanionVisible());
    ROCLINT_CHECK(c.wordCompanionMessage().contains("Applying"));
    ROCLINT_CHECK(c.applyWordRefinement(c.stateVersion()) == 0 && native->rangeInjections.size() == 1);
    emit native->rangeInjectionFinished(first + 99, roclint::host::InjectionOutcome::Injected, {});
    ROCLINT_CHECK(c.wordCompanionState() == "applying");
    emit native->rangeInjectionFinished(first, roclint::host::InjectionOutcome::Failed, "Readback mismatch.");
    ROCLINT_CHECK(c.wordCompanionState() == "failed" && c.wordCompanionVisible());
    ROCLINT_CHECK(c.wordCompanionMessage() == "Readback mismatch.");
    ROCLINT_CHECK(!c.hasWordRefinement());
    c.retryWordRefinement();
    ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    ROCLINT_CHECK(c.wordCompanionState() == "ready" && c.wordCompanionMessage().isEmpty());
    ROCLINT_CHECK(native->rangeInjections.size() == 1); // Retry checks; it never writes.
    const auto second = c.applyWordRefinement(c.stateVersion()); ROCLINT_CHECK(second);
    const auto request = native->rangeInjections.back();
    field.text = request.expectedCurrentText.left(request.startOffset) + request.replacementText + request.expectedCurrentText.mid(request.endOffset);
    field.caretOffset = field.text.size(); native->observe(field);
    ROCLINT_CHECK(c.wordCompanionState() == "applying"); // A text echo alone is not success.
    emit native->rangeInjectionFinished(second, roclint::host::InjectionOutcome::Injected, {});
    ROCLINT_CHECK(c.wordCompanionState() == "applied" && c.wordCompanionVisible() && !c.hasWordRefinement());
    ROCLINT_CHECK(c.wordCompanionMessage() == "Applied to your draft.");
    native->observe(field); ROCLINT_CHECK(c.wordCompanionState() == "applied");
    field.text = "Please make the dashboard feel faster"; field.caretOffset = field.text.size();
    native->observe(field); ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    ROCLINT_CHECK(!c.wordCompanionVisible() && c.isAmbientBadgeVisible());
    c.activateAmbientIndicator();
    QThread::msleep(1900); QCoreApplication::processEvents();
    ROCLINT_CHECK(c.hasWordRefinement() && c.wordCompanionState() == "ready"); // Old success timer is stale.
    const auto third = c.applyWordRefinement(c.stateVersion()); ROCLINT_CHECK(third);
    ++field.fieldToken; native->observe(field);
    emit native->rangeInjectionFinished(third, roclint::host::InjectionOutcome::Injected, {});
    ROCLINT_CHECK(c.wordCompanionState() != "applied" && !c.wordCompanionVisible());
    native->observe(field); ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    const auto fourth = c.applyWordRefinement(c.stateVersion()); ROCLINT_CHECK(fourth);
    c.dismissWordRefinement();
    emit native->rangeInjectionFinished(fourth, roclint::host::InjectionOutcome::Injected, {});
    ROCLINT_CHECK(!c.wordCompanionVisible()); // Dismissal is not undone by late completion.
}

void testWordQuestionCanRecoverToAnAuditedCorrection() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController c(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    ScriptedRuntime runtime;
    runtime.wordRefinementReply = R"({"action":"ask","question":"Was that spelling intentional?"})";
    runtime.wordSurfaceReply = R"({"status":"repair","replacement":"unchanged","explanation":"Correct spelling","source":""})";
    connectRuntime(c, runtime);
    c.setAmbientModeEnabled(true);
    roclint::host::ObservedField field;
    field.fieldToken = 4299; field.appIdentifier = "Code.exe";
    field.appDisplayName = "Visual Studio Code"; field.kind = roclint::host::FieldKind::GuiEditor;
    field.text = "Keep all existing tests unchnaged on Windows.";
    field.caretOffset = field.text.size(); field.fieldRect = QRect(60, 120, 620, 120);
    field.wordStartOffset = field.text.indexOf("unchnaged");
    field.wordEndOffset = field.wordStartOffset + 9;
    field.wordRects = {QRect(250, 125, 80, 18)};
    observed->observe(field);
    ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    ROCLINT_CHECK(!c.wordRefinementNeedsClarification());
    ROCLINT_CHECK(c.wordRefinementReplacement() == "unchanged");
    {
        std::lock_guard<std::mutex> lock(*runtime.mutex);
        ROCLINT_CHECK(std::count_if(runtime.sent->begin(), runtime.sent->end(), [](const auto& prompt) {
            return prompt.find("granite.word_surface_check.v1") != std::string::npos;
        }) == 1);
        ROCLINT_CHECK(std::any_of(runtime.sent->begin(), runtime.sent->end(), [](const auto& prompt) {
            return prompt.find("granite.goldfish_word_audit.v3") != std::string::npos &&
                   prompt.find("Keep all existing tests unchanged") != std::string::npos;
        }));
    }
    ROCLINT_CHECK(observed->rangeInjections.empty());
    ROCLINT_CHECK(c.applyWordRefinement(c.stateVersion(), "wcand_1") != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(observed->rangeInjections.size() == 1);
}

void testPhraseFlowChecksAnswersAndSupportsGuardedUndo() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController host(":memory:", std::move(bridge));
    ROCLINT_CHECK(host.waitForStorageForTesting());
    ScriptedRuntime runtime;
    runtime.wordRefinementReply = R"({"action":"ask","question":"Which interaction feels slow?","choices":["Initial loading","Data refresh"]})";
    auto fallback = runtime.transport();
    host.setTransportOverrideForTesting([fallback](std::string_view prompt) -> Result<std::string> {
        if (prompt.find("granite.word_refinement.v4") != std::string_view::npos &&
            prompt.find("Which interaction feels slow? -> Data refresh") != std::string_view::npos)
            return std::string(R"({"action":"replace","options":[{"text":"quicker to refresh","effect":"Specifies refresh responsiveness","assume":""}]})");
        return fallback(prompt);
    });
    ROCLINT_CHECK(host.markModelReadyForTesting(modelInfo()));
    host.setAmbientModeEnabled(true);
    roclint::host::ObservedField field;
    field.fieldToken = 7612; field.processId = 123; field.appIdentifier = "Code.exe";
    field.text = "Make the dashboard feel faster.";
    const int start = field.text.indexOf("faster");
    field.caretOffset = static_cast<int>(field.text.size()); // Caret after terminal punctuation.
    field.fieldRect = QRect(40, 100, 500, 160); field.caretRect = QRect(220, 140, 2, 20);
    observed->observe(field);
    ROCLINT_CHECK(waitForWordSuggestion(host, 5000));
    ROCLINT_CHECK(host.wordClarificationChoices() == QStringList({"Initial loading", "Data refresh"}));
    ROCLINT_CHECK(!host.wordMarkVisible()); // No provider range: honest badge fallback.
    field.wordStartOffset = start; field.wordEndOffset = start + 6; field.wordRects = {QRect(170, 140, 50, 20)};
    observed->observe(field); QCoreApplication::processEvents();
    ROCLINT_CHECK(!host.wordMarkVisible() && host.wordRefinementAnchorRect() == field.wordRects.constFirst());
    field.wordStartOffset = start - 1;
    observed->observe(field); QCoreApplication::processEvents();
    ROCLINT_CHECK(!host.wordMarkVisible()); // Another span's bounds cannot label this suggestion.
    field.wordStartOffset = start; observed->observe(field);
    host.activateAmbientIndicator();
    ROCLINT_CHECK(host.wordCompanionVisible() && !host.wordMarkVisible()); // A question is not an inline replacement.
    runtime.rejectCustom->store(true);
    ROCLINT_CHECK(!host.submitWordClarification("Data refresh", host.stateVersion() + 1));
    ROCLINT_CHECK(host.submitWordClarification("iondwfwoefneiofneionfeio", host.stateVersion()));
    ROCLINT_CHECK(host.waitForIdleForTesting(5000));
    ROCLINT_CHECK(host.wordRefinementNeedsClarification() && host.wordCompanionState() == "ready");
    ROCLINT_CHECK(!host.wordCompanionMessage().isEmpty() && observed->rangeInjections.empty() && !host.isInterviewActive());
    runtime.rejectCustom->store(false);
    ROCLINT_CHECK(host.submitWordClarification("Data refresh", host.stateVersion()));
    ROCLINT_CHECK(host.waitForIdleForTesting(5000));
    ROCLINT_CHECK(host.hasWordRefinement() && !host.wordRefinementNeedsClarification());
    ROCLINT_CHECK(host.wordRefinementReplacement() == "quicker to refresh" && observed->rangeInjections.empty());
    ROCLINT_CHECK(host.wordMarkVisible() && host.wordRefinementAnchorRect() == field.wordRects.constFirst());
    ROCLINT_CHECK(host.applyWordRefinement(host.stateVersion()) != 0);
    QCoreApplication::processEvents();
    const QString before = field.text;
    field.text.replace(start, 6, "quicker to refresh"); field.caretOffset = start + 18;
    observed->observe(field); QCoreApplication::processEvents();
    ROCLINT_CHECK(host.wordCanUndo());
    ROCLINT_CHECK(host.undoWordRefinement(host.stateVersion() + 1) == 0);
    ROCLINT_CHECK(host.undoWordRefinement(host.stateVersion()) != 0);
    ROCLINT_CHECK(observed->rangeInjections.size() == 2);
    const auto undo = observed->rangeInjections.back();
    ROCLINT_CHECK(undo.expectedCurrentText == field.text && undo.originalRangeText == "quicker to refresh" && undo.replacementText == "faster");
    QCoreApplication::processEvents();
    ROCLINT_CHECK(host.wordCompanionState() == "undone" && !host.wordCanUndo());
    ROCLINT_CHECK(observed->injections.empty());
}

void testAcceptedWordAnswerSurvivesLocalFailureAndStaysDraftBound() {
    for (int mode = 0; mode < 6; ++mode) {
        auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
        auto* observed = bridge.get();
        HostController host(":memory:", std::move(bridge));
        ROCLINT_CHECK(host.waitForStorageForTesting());
        host.setLegacyDiscoveryForTesting(true);
        ScriptedRuntime runtime;
        runtime.discoveryReply = R"({"status":"no_ambiguity","choice_groups":[]})";
        runtime.discoveryDelay = std::chrono::milliseconds(40); // Handoff precedes analysis completion.
        runtime.wordRefinementReply = R"({"action":"ask","question":"Which interaction feels slow?"})";
        runtime.rejectCustom->store(mode == 4);
        auto fallback = runtime.transport();
        host.setTransportOverrideForTesting([fallback, mode](std::string_view prompt) -> Result<std::string> {
            if (mode == 5 && prompt.find("granite.goldfish_word_audit.v3") != std::string_view::npos)
                return std::string(R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Fail","usefulness":"Fail"}]})");
            if (prompt.find("granite.word_refinement.v4") != std::string_view::npos &&
                prompt.find("Which interaction feels slow? -> Data refresh") != std::string_view::npos) {
                if (mode == 0 || mode == 3) return makeError(ErrorCode::ProviderUnavailable, "Synthetic infill failure");
                if (mode == 1) return std::string(R"({"action":"ask","question":"Which interaction feels slow?"})");
                if (mode == 5) return std::string(R"({"action":"replace","options":[{"text":"quicker to refresh","effect":"Refresh response","assume":""}]})");
                return std::string("malformed synthetic output");
            }
            return fallback(prompt);
        });
        ROCLINT_CHECK(host.markModelReadyForTesting(modelInfo()));
        host.setAmbientModeEnabled(true);
        roclint::host::ObservedField field;
        field.fieldToken = 7800 + mode; field.processId = 345; field.appIdentifier = "Code.exe";
        field.text = "Make the dashboard feel faster.";
        field.caretOffset = field.text.indexOf("faster") + 6;
        field.fieldRect = QRect(40, 100, 500, 160);
        observed->observe(field);
        ROCLINT_CHECK(waitForWordSuggestion(host, 5000));
        host.activateAmbientIndicator();
        const QString answer = mode == 4 ? "iondwfwoefneiofneionfeio" : "Data refresh";
        ROCLINT_CHECK(host.submitWordClarification(answer, host.stateVersion()));
        ROCLINT_CHECK(host.waitForIdleForTesting(5000));
        ROCLINT_CHECK(observed->rangeInjections.empty() && observed->injections.empty());
        ROCLINT_CHECK(host.interviewRecordsForTesting().empty()); // No implicit full interview or confirmation.
        if (mode == 4) {
            ROCLINT_CHECK(host.wordRefinementNeedsClarification());
        } else {
            ROCLINT_CHECK(host.wordCompanionState() == "failed" && !host.hasWordRefinement());
            ROCLINT_CHECK(!host.wordRefinementNeedsClarification() && host.wordRefinementReplacement().isEmpty());
            ROCLINT_CHECK(host.wordCompanionMessage().contains("Your answer is kept"));
        }
        ROCLINT_CHECK(host.applyWordRefinement(host.stateVersion()) == 0);
        if (mode == 3) {
            field.text = "Make the dashboard feel faster. Keep the existing colors.";
            field.caretOffset = field.text.indexOf("faster") + 6;
            observed->observe(field);
            ROCLINT_CHECK(waitForWordSuggestion(host, 5000));
        }
        ROCLINT_CHECK(host.openRewriteFromCompanion(host.stateVersion() + 1) == 0);
        const auto command = host.openRewriteFromCompanion(host.stateVersion());
        ROCLINT_CHECK(command != 0 && host.waitForCommandForTesting(command));
        ROCLINT_CHECK(host.waitForIdleForTesting(5000));
        ROCLINT_CHECK(host.hasConfirmedIntent() && !host.isInterviewActive());
        const bool shouldCarryAnswer = mode < 3 || mode == 5;
        const auto& records = host.interviewRecordsForTesting();
        ROCLINT_CHECK(records.size() == (shouldCarryAnswer ? 1 : 0));
        if (shouldCarryAnswer) {
            ROCLINT_CHECK(records[0].question == "Which interaction feels slow?");
            ROCLINT_CHECK(records[0].rawAnswer == "Data refresh" && records[0].origin == "word_clarification");
            const auto decision = host.confirmedIntentForTesting()->getDecision(records[0].id);
            ROCLINT_CHECK(decision && decision->customText == "Data refresh");
        }
        if (mode == 0) {
            const auto rewriteCommand = host.rewriteWithoutQuestions(host.stateVersion());
            ROCLINT_CHECK(rewriteCommand && host.waitForCommandForTesting(rewriteCommand));
            ROCLINT_CHECK(host.waitForIdleForTesting(5000));
            std::lock_guard<std::mutex> lock(*runtime.mutex);
            ROCLINT_CHECK(std::any_of(runtime.sent->begin(), runtime.sent->end(), [](const auto& prompt) {
                return prompt.find("granite.rewrite.format.v9") != std::string::npos &&
                    prompt.find("Data refresh") != std::string::npos &&
                    prompt.find("Which interaction feels slow?") != std::string::npos;
            }));
        }
        ROCLINT_CHECK(observed->rangeInjections.empty() && observed->injections.empty());
    }
    std::cout << "  [PASS] Accepted phrase answers survive failed/repeated/invalid infill; rejected and stale answers never carry over.\n";
}

void testWordCompanionNoopClearingAndClarification() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController c(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    ScriptedRuntime runtime;
    runtime.wordRefinementReply = R"({"needs_clarification":true,"clarification_question":"Which interaction feels slow?","candidates":[]})";
    connectRuntime(c, runtime);
    c.setAmbientModeEnabled(true);
    roclint::host::ObservedField field;
    field.fieldToken = 4242;
    field.appIdentifier = QStringLiteral("Code.exe");
    field.appDisplayName = QStringLiteral("Visual Studio Code");
    field.kind = roclint::host::FieldKind::GuiEditor;
    field.text = QStringLiteral("Make the dashboard feel faster for people on slow laptops.");
    field.caretOffset = field.text.indexOf(QStringLiteral("faster")) + 6;
    field.fieldRect = QRect(60, 120, 620, 120);
    observed->observe(field);
    ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    ROCLINT_CHECK(c.wordRefinementNeedsClarification());
    ROCLINT_CHECK(c.wordRefinementClarificationQuestion().contains(QStringLiteral("Which interaction")));
    ROCLINT_CHECK(c.wordRefinementReplacement().isEmpty());
    ROCLINT_CHECK(c.applyWordRefinement(c.stateVersion(), QStringLiteral("wcand_1")) == 0);
    int notifications = 0;
    QObject::connect(&c, &HostController::wordRefinementChanged, &c, [&]() { ++notifications; });
    field.text.replace(QStringLiteral("faster"), QStringLiteral("mo"));
    field.caretOffset = field.text.indexOf(QStringLiteral("mo")) + 2;
    observed->observe(field);
    QCoreApplication::processEvents();
    ROCLINT_CHECK(!c.hasWordRefinement());
    ROCLINT_CHECK(notifications > 0);

    field.text = QStringLiteral("Make the dashboard feel faster for people on slow laptops.");
    field.caretOffset = field.text.indexOf(QStringLiteral("faster")) + 6;
    observed->observe(field);
    ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    const QString nonempty = field.text;
    field.text.clear();
    field.caretOffset = 0;
    observed->observe(field);
    ROCLINT_CHECK(!c.hasWordRefinement());
    field.text = nonempty;
    field.caretOffset = field.text.indexOf(QStringLiteral("faster")) + 6;
    observed->observe(field);
    ROCLINT_CHECK(waitForWordSuggestion(c, 5000));
    c.setAmbientModeEnabled(false);
    ROCLINT_CHECK(!c.hasWordRefinement());
    c.setAmbientModeEnabled(true);

    runtime.wordRefinementReply = R"({"needs_clarification":false,"clarification_question":"","candidates":[]})";
    c.setTransportOverrideForTesting(runtime.transport());
    field.text.replace(QStringLiteral("mo"), QStringLiteral("faster"));
    field.caretOffset = field.text.indexOf(QStringLiteral("faster")) + 6;
    observed->observe(field);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 900) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(!c.hasWordRefinement());
    std::cout << "  [PASS] No-op is quiet; clarification-only survives; short-word clearing notifies UI.\n";
}

void testWordAuditFailureNeverOffersAnEdit() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed=bridge.get();
    HostController c(":memory:",std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    std::atomic<int> audited{0};
    c.setTransportOverrideForTesting([&](std::string_view prompt)->Result<std::string>{
        if(prompt.find("granite.goldfish_word_audit.v3")!=std::string_view::npos) {
            ROCLINT_CHECK(prompt.find("Apply an a clear atmosphere treatment")!=std::string_view::npos);
            ++audited;
            return std::string(R"({"verdicts":[{"id":"wcand_1","grammar":"Fail","preservation":"Fail","usefulness":"Fail"}]})");
        }
        return std::string(R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"atmospheric","replacement":"a clear atmosphere","explanation":"Adds clarity","assumption":"","requires_clarification":false}]})");
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    c.setAmbientModeEnabled(true);
    roclint::host::ObservedField field;
    field.fieldToken=6543;field.appIdentifier="Code.exe";field.kind=roclint::host::FieldKind::GuiEditor;
    field.text="Apply an atmospheric treatment to the image.";
    field.caretOffset=field.text.indexOf("atmospheric")+11;
    field.fieldRect=QRect(60,120,620,120);
    observed->observe(field);
    QElapsedTimer timer;timer.start();
    while(audited.load()==0 && timer.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(5);}
    ROCLINT_CHECK(audited.load()==1 && c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(!c.hasWordRefinement() && observed->rangeInjections.empty());
}

void testWordCompanionCoalescesAndDropsStaleWork() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController c(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    ScriptedRuntime runtime;
    std::atomic<int> calls{0}, active{0}, peak{0};
    const auto fallback = runtime.transport();
    c.setTransportOverrideForTesting([&](std::string_view prompt) -> roclint::core::Result<std::string> {
        if (prompt.find("granite.word_refinement.v4") == std::string_view::npos) return fallback(prompt);
        ++calls;
        const int current = ++active;
        peak.store(std::max(peak.load(), current));
        QThread::msleep(1400);
        --active;
        const bool latest = prompt.find("responsive") != std::string_view::npos;
        return std::string(latest
            ? R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"responsive","replacement":"quick to respond to clicks","explanation":"Names interaction response.","assumption":"","requires_clarification":false}]})"
            : R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"faster","replacement":"quick to load","explanation":"Names loading.","assumption":"","requires_clarification":false}]})");
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    c.setAmbientModeEnabled(true);
    roclint::host::ObservedField field;
    field.fieldToken = 4343;
    field.appIdentifier = QStringLiteral("Code.exe");
    field.kind = roclint::host::FieldKind::GuiEditor;
    field.text = QStringLiteral("Make the dashboard feel faster for people on slow laptops.");
    field.caretOffset = field.text.indexOf(QStringLiteral("faster")) + 6;
    field.fieldRect = QRect(60, 120, 620, 120);
    observed->observe(field);
    QElapsedTimer timer; timer.start();
    while (calls.load() == 0 && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ROCLINT_CHECK(calls.load() == 1);
    field.text.replace(QStringLiteral("faster"), QStringLiteral("cinematic"));
    field.caretOffset = field.text.indexOf(QStringLiteral("cinematic")) + 9;
    observed->observe(field);
    // Let the debounce expire while the superseded request is still running.
    timer.restart();
    while (timer.elapsed() < 650) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ROCLINT_CHECK(calls.load() == 1);
    field.text.replace(QStringLiteral("cinematic"), QStringLiteral("responsive"));
    field.caretOffset = field.text.indexOf(QStringLiteral("responsive")) + 10;
    observed->observe(field);
    bool staleShown = false;
    QObject::connect(&c, &HostController::wordRefinementChanged, &c, [&]() {
        if (c.hasWordRefinement() && c.wordRefinementOriginal() != QStringLiteral("responsive")) staleShown = true;
    });
    ROCLINT_CHECK(waitForWordSuggestion(c, 6000));
    ROCLINT_CHECK(calls.load() == 2 && peak.load() == 1 && !staleShown);
    ROCLINT_CHECK(c.wordRefinementOriginal() == QStringLiteral("responsive"));
    observed->lose();
    ROCLINT_CHECK(!c.hasWordRefinement());
    // Field loss during the quiet period cancels the pending request entirely.
    observed->observe(field);
    observed->lose();
    timer.restart();
    while (timer.elapsed() < 650) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ROCLINT_CHECK(calls.load() == 2);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    // Quit during active work must wait for cancellation/completion and never publish it.
    field.text += QStringLiteral(" Keep existing behavior.");
    observed->observe(field);
    timer.restart();
    while (calls.load() < 3 && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ROCLINT_CHECK(calls.load() == 3);
    bool quitCompleted = false;
    QObject::connect(&c, &HostController::quitRequested, &c, [&]() { quitCompleted = true; });
    c.requestQuit();
    ROCLINT_CHECK(!quitCompleted);
    timer.restart();
    while (!quitCompleted && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ROCLINT_CHECK(quitCompleted && active.load() == 0 && !c.hasWordRefinement());
    std::cout << "  [PASS] Rapid edits coalesce to latest snapshot: calls=2 peak=1; stale work never shown.\n";
}

void testIdleWaitIncludesDeferredModelWork() {
    HostController controller(":memory:");
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    ScriptedRuntime runtime;
    runtime.discoveryDelay=std::chrono::milliseconds(120);
    connectRuntime(controller,runtime);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    quint64 command=0;
    QTimer::singleShot(0,&controller,[&] {command=controller.analyzePrompt(kPrompt,controller.stateVersion());});
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(command!=0 && controller.analysisStatus()=="READY" && controller.isClarifying());
    std::cout << "  [PASS] Idle waiting includes work dispatched by a queued UI/model command.\n";
}

// ----------------------------------------------------------------------------------------------
// Shared intent brief, substantial interview and comprehension review
// ----------------------------------------------------------------------------------------------

const QString kInterviewPrompt =
    QStringLiteral("Make the dashboard feel faster for people on slow laptops, and keep browsing easy.");

const std::string kPerformanceDiscovery = R"({"status": "options_generated",
  "choice_groups": [{"dimension": "performance", "question": "What should faster mean?", "material": true,
    "alternatives": [
      {"label": "Lower latency per click", "why_material": "Each interaction feels immediate", "evidence": ["slow laptops"]},
      {"label": "Higher throughput", "why_material": "More updates complete at once", "evidence": ["slow laptops"]}]}]})";

const std::string kTwoGroupInterviewDiscovery = R"({"status": "options_generated",
  "choice_groups": [
    {"dimension": "performance", "question": "What should faster mean?", "material": true,
     "alternatives": [
       {"label": "Lower latency per click", "why_material": "Each interaction feels immediate", "evidence": ["slow laptops"]},
       {"label": "Higher throughput", "why_material": "More updates complete at once", "evidence": ["slow laptops"]}]},
    {"dimension": "navigation", "question": "What should keep browsing easy mean?", "material": true,
     "alternatives": [
       {"label": "Fast A-Z jump list", "why_material": "One click to an artist", "evidence": ["keep browsing easy"]},
       {"label": "Search-first layout", "why_material": "Type to find anything", "evidence": ["keep browsing easy"]}]}]})";

const std::string kAdaptiveFollowUp = R"({"kind":"creative","groups":[
  {"focus":"layout","question":"Should the dashboard stay dense or become airier?","source":"dashboard",
   "options":[
     {"text":"Denser overview","effect":"More content visible at once","assume":""},
     {"text":"Airier spacing","effect":"Less content but easier scanning","assume":""}]}]})";

const char* kInterviewRewrite =
    "Make the dashboard respond with lower latency per click, keep browsing easy with a fast A-Z jump list, "
    "use airier spacing, and keep the dashboard understandable.";

void waitForInterviewQuestion(HostController& c, const QString& fragment, int timeoutMs = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (c.interviewQuestion().contains(fragment) && !c.isInterviewBusy()) return;
        QThread::msleep(2);
    }
    ROCLINT_CHECK(c.interviewQuestion().contains(fragment));
}

void waitForInterviewIdle(HostController& c, int timeoutMs = 8000) {
    ROCLINT_CHECK(c.waitForIdleForTesting(timeoutMs));
    QElapsedTimer timer;
    timer.start();
    while (c.isInterviewBusy() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    ROCLINT_CHECK(!c.isInterviewBusy());
}

void waitForAnalysisSettled(HostController& c, int timeoutMs = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (c.analysisStatus() != "ANALYZING" && c.analysisStatus() != "IDLE") return;
        QThread::msleep(2);
    }
    ROCLINT_CHECK(c.analysisStatus() != "ANALYZING");
}

void openInterview(HostController& c) {
    const quint64 accepted = c.openInterviewFromCompanion(c.stateVersion());
    ROCLINT_CHECK(accepted != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.isInterviewActive());
}

void answerInterview(HostController& c, const QString& text, const QString& disposition = QStringLiteral("answer")) {
    const quint64 accepted = c.submitInterviewResponse(text, disposition, c.stateVersion());
    ROCLINT_CHECK(accepted != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    waitForInterviewIdle(c);
}

void testInterviewConnectedFlowCarriesDecisionsIntoRewriteAndReview() {
    ScriptedRuntime runtime;
    runtime.discoveryReply = kTwoGroupInterviewDiscovery;
    runtime.rewriteText = kInterviewRewrite;
    runtime.queueFollowUps({kAdaptiveFollowUp, "{\"kind\":\"none\"}"});
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);

    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
    ROCLINT_CHECK(c.interviewTurnsCompleted() == 0);

    openInterview(c);
    waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
    ROCLINT_CHECK(c.interviewChoices().size() == 2);
    ROCLINT_CHECK(c.interviewConsequence().contains(QStringLiteral("immediate")));
    ROCLINT_CHECK(c.intentOpenChoiceIds().contains(QStringLiteral("grp_0")));

    answerInterview(c, QStringLiteral("Lower latency per click"));
    waitForInterviewQuestion(c, QStringLiteral("What should keep browsing easy mean?"));
    ROCLINT_CHECK(c.interviewTurnsCompleted() == 1);
    ROCLINT_CHECK(c.workingIntentForTesting().confirmedAlternatives.at("grp_0") == "alt_0_0");
    const auto afterFirst = c.intentBriefItems();
    bool sawDecision = false;
    bool sawRequirement = false;
    for (const auto& item : afterFirst) {
        const auto map = item.toMap();
        if (map.value("id").toString() == "grp_0") {
            sawDecision = map.value("kind").toString() == "confirmed_decision" &&
                          map.value("status").toString() == "confirmed";
        }
        if (map.value("kind").toString() == "source_requirement" &&
            map.value("sourceExcerpt").toString().contains("slow laptops")) {
            sawRequirement = true;
        }
    }
    ROCLINT_CHECK(sawDecision);
    ROCLINT_CHECK(sawRequirement);

    answerInterview(c, QStringLiteral("Fast A-Z jump list"));
    waitForInterviewQuestion(c, QStringLiteral("dense or become airier"));
    ROCLINT_CHECK(c.interviewTurnsCompleted() == 2);
    // The follow-up was asked with the current brief, not a fresh questionnaire.
    const auto requests = runtime.requests();
    bool sawBrief = false;
    for (const auto& request : requests) {
        if (request.find("interview follow-up") == std::string::npos) continue;
        sawBrief = request.find("Lower latency per click") != std::string::npos &&
                   request.find("Fast A-Z jump list") != std::string::npos;
    }
    ROCLINT_CHECK(sawBrief);

    answerInterview(c, QStringLiteral("Airier spacing"));
    waitForInterviewIdle(c);
    ROCLINT_CHECK(c.interviewTurnsCompleted() == 3);
    ROCLINT_CHECK(c.hasConfirmedIntent());
    const auto payload = c.getConfirmedIntentPayload();
    bool sawFollowUpDecision = false;
    for (const auto& item : payload.value("items").toList()) {
        const auto map = item.toMap();
        if (map.value("itemId").toString().startsWith("iv_q_")) {
            sawFollowUpDecision = true;
            ROCLINT_CHECK(map.value("customText").toString() == "Airier spacing");
        }
    }
    ROCLINT_CHECK(sawFollowUpDecision);
    // Re-entry must not restart the explanation or drop answers.
    const auto turns = c.interviewTurnsCompleted();
    const auto reentryVersion = c.openInterviewFromCompanion(c.stateVersion());
    ROCLINT_CHECK(reentryVersion == c.stateVersion());
    ROCLINT_CHECK(c.interviewTurnsCompleted() == turns);
    ROCLINT_CHECK(c.isInterviewActive());

    // Rewrite is produced from the same confirmed decisions.
    c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.proposalStatus() == "READY_FOR_REVIEW");
    ROCLINT_CHECK(c.comprehensionReviewState() == "not_run");
    ROCLINT_CHECK(!c.canApplyReviewedOutput());
    ROCLINT_CHECK(!c.canApprove());

    struct CapturedReview {
        bridge::ComprehensionReviewRequest request;
        int calls{0};
    };
    auto captured = std::make_shared<CapturedReview>();
    c.setComprehensionReviewForTesting([captured](const bridge::ComprehensionReviewRequest& request) {
        captured->request = request;
        ++captured->calls;
        return approvingReviewResult();
    });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(captured->calls == 1);
    ROCLINT_CHECK(captured->request.candidatePrompt == kInterviewRewrite);
    ROCLINT_CHECK(captured->request.originalPrompt == kInterviewPrompt.toStdString());
    ROCLINT_CHECK(captured->request.taskKind == "performance");
    ROCLINT_CHECK(!captured->request.sessionId.empty());
    ROCLINT_CHECK(!captured->request.revisionId.empty());
    const auto decisions = captured->request.confirmedDecisions;
    ROCLINT_CHECK(std::find(decisions.begin(), decisions.end(), "Lower latency per click") != decisions.end());
    ROCLINT_CHECK(std::find(decisions.begin(), decisions.end(), "Fast A-Z jump list") != decisions.end());
    ROCLINT_CHECK(std::find(decisions.begin(), decisions.end(), "Airier spacing") != decisions.end());
    const auto excerpts = captured->request.sourceExcerpts;
    ROCLINT_CHECK(!excerpts.empty());
    bool sawSourceEvidence = false;
    for (const auto& excerpt : excerpts) {
        if (excerpt.find("slow laptops") != std::string::npos ||
            excerpt.find("keep browsing easy") != std::string::npos) {
            sawSourceEvidence = true;
        }
    }
    ROCLINT_CHECK(sawSourceEvidence);
    ROCLINT_CHECK(c.canApplyReviewedOutput());
    ROCLINT_CHECK(c.canApprove());
    setClipboard(QStringLiteral("before"));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.approveAndCopyRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.hasApproval());
    ROCLINT_CHECK(clipboardText() == QString::fromUtf8(kInterviewRewrite));

    std::cout << "  [PASS] Interview: local answer -> adaptive follow-up -> rewrite -> fresh review gate.\n";
}

void testInterviewCustomUnknownAndSkipNeverConfirm() {
    {
        ScriptedRuntime runtime;
        runtime.discoveryReply = kPerformanceDiscovery;
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        openInterview(c);
        waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));

        // An unusual spelling is accepted by the relevance check; the host has no
        // word-to-answer menu and does not reject unusual wording by itself.
        answerInterview(c, QStringLiteral("latensy-focussed clicks"), QStringLiteral("custom"));
        ROCLINT_CHECK(c.workingIntentForTesting().customResolutions.at("grp_0") == "latensy-focussed clicks");
        ROCLINT_CHECK(c.workingIntentForTesting().confirmedAlternatives.empty());
        ROCLINT_CHECK(c.interviewTurnsCompleted() == 1);
        ROCLINT_CHECK(!c.isInterviewBusy());
        bool sawRaw = false;
        for (const auto& item : c.intentBriefItems()) {
            const auto map = item.toMap();
            if (map.value("id").toString() != "grp_0") continue;
            sawRaw = map.value("rawAnswer").toString() == "latensy-focussed clicks";
        }
        ROCLINT_CHECK(sawRaw);
    }

    {
        ScriptedRuntime runtime;
        runtime.discoveryReply = kTwoGroupInterviewDiscovery;
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        openInterview(c);
        waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));

        answerInterview(c, QStringLiteral("I don't know yet"), QStringLiteral("unknown"));
        ROCLINT_CHECK(c.workingIntentForTesting().hasUncertainty("grp_0"));
        ROCLINT_CHECK(!c.workingIntentForTesting().isResolved("grp_0"));
        waitForInterviewQuestion(c, QStringLiteral("keep browsing easy"));
        answerInterview(c, QString(), QStringLiteral("skip"));
        ROCLINT_CHECK(c.workingIntentForTesting().isSkipped("grp_1"));
        ROCLINT_CHECK(!c.workingIntentForTesting().isResolved("grp_1"));
        ROCLINT_CHECK(c.interviewTurnsCompleted() == 2);
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(c.confirmedItems().isEmpty());
        const auto unresolved = c.unresolvedItems();
        ROCLINT_CHECK(unresolved.contains(QStringLiteral("grp_0")));
        ROCLINT_CHECK(unresolved.contains(QStringLiteral("grp_1")));
        const auto open = c.intentOpenChoiceIds();
        ROCLINT_CHECK(open.contains(QStringLiteral("grp_0")));
        ROCLINT_CHECK(open.contains(QStringLiteral("grp_1")));
        bool sawUnknownRaw = false;
        for (const auto& item : c.intentBriefItems()) {
            const auto map = item.toMap();
            if (map.value("id").toString() == "grp_0") {
                sawUnknownRaw = map.value("rawAnswer").toString() == "I don't know yet" &&
                                map.value("status").toString() == "open";
            }
        }
        ROCLINT_CHECK(sawUnknownRaw);
        ROCLINT_CHECK(!c.canApprove());
    }

    {
        ScriptedRuntime runtime;
        runtime.discoveryReply = kPerformanceDiscovery;
        runtime.rejectCustom->store(true);
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        openInterview(c);
        waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));

        const auto beforeFeedbackVersion = c.stateVersion();
        const quint64 accepted = c.submitInterviewResponse(QStringLiteral("zzzz qqqq"),
                                                           QStringLiteral("custom"), c.stateVersion());
        ROCLINT_CHECK(accepted != 0);
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        waitForInterviewIdle(c);
        ROCLINT_CHECK(c.stateVersion() == beforeFeedbackVersion); // no decision was published
        ROCLINT_CHECK(c.workingIntentForTesting().customResolutions.empty());
        ROCLINT_CHECK(!c.interviewFeedback().isEmpty());
        ROCLINT_CHECK(c.interviewQuestion().contains(QStringLiteral("What should faster mean?")));
        ROCLINT_CHECK(c.interviewTurnsCompleted() == 0);
        ROCLINT_CHECK(!c.hasConfirmedIntent());

        runtime.rejectCustom->store(false);
        answerInterview(c, QStringLiteral("Lower latency per click"), QStringLiteral("custom"));
        ROCLINT_CHECK(c.workingIntentForTesting().customResolutions.at("grp_0") == "Lower latency per click");
        ROCLINT_CHECK(c.interviewTurnsCompleted() == 1);
    }

    std::cout << "  [PASS] Custom answers are relevance-checked; unknown and skip never confirm a value.\n";
}

void testChangeIntentDecisionSupersedesAndInvalidatesReview() {
    ScriptedRuntime runtime;
    runtime.discoveryReply = kPerformanceDiscovery;
    runtime.rewriteText = kInterviewRewrite;
    runtime.queueFollowUps({"{\"kind\":\"none\"}"});
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    openInterview(c);
    waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
    answerInterview(c, QStringLiteral("Lower latency per click"));
    c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    completeReview(c);
    ROCLINT_CHECK(c.canApprove());

    const quint64 accepted = c.changeIntentDecision(QStringLiteral("grp_0"),
                                                    QStringLiteral("Hier throughput"),
                                                    c.stateVersion());
    ROCLINT_CHECK(accepted != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.workingIntentForTesting().supersededDecisions.size() == 1);
    ROCLINT_CHECK(c.workingIntentForTesting().supersededDecisions[0].previousValue == "Lower latency per click");
    ROCLINT_CHECK(c.workingIntentForTesting().customResolutions.at("grp_0") == "Hier throughput");
    ROCLINT_CHECK(!c.hasProposal());
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.comprehensionReviewState() == "stale");
    bool sawSuperseded = false;
    bool sawCurrent = false;
    for (const auto& item : c.intentBriefItems()) {
        const auto map = item.toMap();
        if (map.value("kind").toString() == "superseded_decision") {
            sawSuperseded = map.value("rawAnswer").toString() == "Lower latency per click";
        }
        if (map.value("id").toString() == "grp_0") {
            sawCurrent = map.value("status").toString() == "confirmed" &&
                         map.value("rawAnswer").toString() == "Hier throughput";
        }
    }
    ROCLINT_CHECK(sawSuperseded);
    ROCLINT_CHECK(sawCurrent);

    // A stale expectedVersion is refused without changing state.
    const auto version = c.stateVersion();
    ROCLINT_CHECK(c.changeIntentDecision(QStringLiteral("grp_0"), QStringLiteral("Again"), version + 1) == 0);
    ROCLINT_CHECK(c.stateVersion() == version);

    std::cout << "  [PASS] Changed preference supersedes the prior decision and invalidates the proposal/review.\n";
}

void testComprehensionReviewGatingFindingsAndReturnToInterview() {
    ScriptedRuntime runtime;
    runtime.discoveryReply = kPerformanceDiscovery;
    runtime.rewriteText = kInterviewRewrite;
    runtime.queueFollowUps({"{\"kind\":\"none\"}"});
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    openInterview(c);
    waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
    answerInterview(c, QStringLiteral("Lower latency per click"));
    c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));

    // Missing reviewer seam: failed closed, never authorizes apply.
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("failed"));
    ROCLINT_CHECK(!c.canApplyReviewedOutput());
    ROCLINT_CHECK(!c.canApprove());

    bridge::ComprehensionReviewResult drift;
    drift.status = bridge::ComprehensionReviewStatus::Completed;
    drift.interpretedOutcome = "The rewrite keeps the speed change but drops support for slow laptops.";
    drift.readyForTask = false;
    bridge::ComprehensionFinding finding;
    finding.id = "finding_drift_1";
    finding.kind = bridge::ReviewFindingKind::MeaningDrift;
    finding.severity = bridge::ReviewFindingSeverity::Required;
    finding.summary = "The rewrite drops the slow-laptop target.";
    finding.sourceRef = "grp_0";
    finding.sourceExcerpt = "slow laptops";
    finding.requiresUserDecision = true;
    finding.proposedRepair = "Keep support for people on slow laptops in the rewritten prompt.";
    drift.findings.push_back(finding);
    c.setComprehensionReviewForTesting([drift](const bridge::ComprehensionReviewRequest&) { return drift; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(!c.canApplyReviewedOutput());
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.comprehensionFindings().size() == 1);
    const auto findingMap = c.comprehensionFindings().front().toMap();
    ROCLINT_CHECK(findingMap.value("id").toString() == "finding_drift_1");
    ROCLINT_CHECK(findingMap.value("severity").toString() == "required");
    ROCLINT_CHECK(findingMap.value("kind").toString() == "meaning_drift");
    ROCLINT_CHECK(findingMap.value("requiresUserDecision").toBool());
    ROCLINT_CHECK(findingMap.value("proposedRepair").toString().contains("slow laptops"));
    ROCLINT_CHECK(c.comprehensionInterpretation().contains("drops support"));

    // A finding that needs a user decision cannot be acknowledged into applicability.
    ROCLINT_CHECK(c.resolveComprehensionFinding(QStringLiteral("finding_drift_1"),
                                                QStringLiteral("acknowledge"), c.stateVersion()) == 0);
    const quint64 returned = c.resolveComprehensionFinding(QStringLiteral("finding_drift_1"),
                                                           QStringLiteral("return_to_interview"), c.stateVersion());
    ROCLINT_CHECK(returned != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.isInterviewActive());
    ROCLINT_CHECK(c.interviewQuestion() == "What should the rewrite say here?");
    ROCLINT_CHECK(c.interviewChoices().isEmpty());
    ROCLINT_CHECK(c.comprehensionReviewState() == "waiting_for_answer");
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) == 0);
    ROCLINT_CHECK(!c.canApplyReviewedOutput());

    // Skip leaves the review choice open and apply blocked.
    answerInterview(c, QString(), QStringLiteral("skip"));
    bool sawOpenReviewChoice = false;
    for (const auto& id : c.intentOpenChoiceIds()) {
        if (id.startsWith(QStringLiteral("iv_review_"))) sawOpenReviewChoice = true;
    }
    ROCLINT_CHECK(sawOpenReviewChoice);
    ROCLINT_CHECK(!c.canApplyReviewedOutput());
    ROCLINT_CHECK(!c.canApprove());

    // Only the user's own wording becomes a decision.
    const QString repairLabel = QStringLiteral("Keep support for people on slow laptops.");
    answerInterview(c, repairLabel, QStringLiteral("custom"));
    ROCLINT_CHECK(!c.hasProposal());
    bool sawRepairDecision = false;
    for (const auto& item : c.getConfirmedIntentPayload().value("items").toList()) {
        const auto map = item.toMap();
        if (map.value("itemId").toString().startsWith("iv_review_")) {
            sawRepairDecision = map.value("customText").toString() == repairLabel;
        }
    }
    ROCLINT_CHECK(sawRepairDecision);

    std::cout << "  [PASS] Review findings gate apply; require-decision findings return to the interview.\n";
}

void testComprehensionReviewMalformedAndAdvisoryHandling() {
    ScriptedRuntime runtime;
    runtime.discoveryReply = kPerformanceDiscovery;
    runtime.rewriteText = kInterviewRewrite;
    runtime.queueFollowUps({"{\"kind\":\"none\"}"});
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    openInterview(c);
    waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
    answerInterview(c, QStringLiteral("Lower latency per click"));
    c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));

    bridge::ComprehensionReviewResult unavailable;
    unavailable.status = bridge::ComprehensionReviewStatus::Unavailable;
    c.setComprehensionReviewForTesting([unavailable](const auto&) { return unavailable; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("failed"));
    ROCLINT_CHECK(!c.canApplyReviewedOutput());

    bridge::ComprehensionReviewResult emptyInterpretation;
    emptyInterpretation.status = bridge::ComprehensionReviewStatus::Completed;
    emptyInterpretation.readyForTask = true;
    c.setComprehensionReviewForTesting([emptyInterpretation](const auto&) { return emptyInterpretation; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("failed"));

    bridge::ComprehensionReviewResult duplicate;
    duplicate.status = bridge::ComprehensionReviewStatus::Completed;
    duplicate.interpretedOutcome = "A plausible reading.";
    duplicate.readyForTask = true;
    bridge::ComprehensionFinding duplicateFinding;
    duplicateFinding.id = "dup";
    duplicateFinding.summary = "First";
    duplicate.findings.push_back(duplicateFinding);
    duplicateFinding.summary = "Second";
    duplicate.findings.push_back(duplicateFinding);
    c.setComprehensionReviewForTesting([duplicate](const auto&) { return duplicate; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("failed"));

    bridge::ComprehensionReviewResult advisory;
    advisory.status = bridge::ComprehensionReviewStatus::Completed;
    advisory.interpretedOutcome = "The rewrite preserves the requested outcome.";
    advisory.readyForTask = true;
    bridge::ComprehensionFinding advisoryFinding;
    advisoryFinding.id = "advisory_1";
    advisoryFinding.kind = bridge::ReviewFindingKind::Readiness;
    advisoryFinding.severity = bridge::ReviewFindingSeverity::Advisory;
    advisoryFinding.summary = "No file paths are needed for this creative task.";
    advisory.findings.push_back(advisoryFinding);
    c.setComprehensionReviewForTesting([advisory](const auto&) { return advisory; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(c.canApplyReviewedOutput());
    ROCLINT_CHECK(c.canApprove());

    bridge::ComprehensionReviewResult required;
    required.status = bridge::ComprehensionReviewStatus::Completed;
    required.interpretedOutcome = "The rewrite preserves the requested outcome with one gap.";
    required.readyForTask = true;
    bridge::ComprehensionFinding requiredFinding;
    requiredFinding.id = "required_1";
    requiredFinding.kind = bridge::ReviewFindingKind::Omission;
    requiredFinding.severity = bridge::ReviewFindingSeverity::Required;
    requiredFinding.summary = "The rewrite does not mention the navigation requirement.";
    requiredFinding.requiresUserDecision = false;
    required.findings.push_back(requiredFinding);
    c.setComprehensionReviewForTesting([required](const auto&) { return required; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(!c.canApplyReviewedOutput());
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.resolveComprehensionFinding(QStringLiteral("required_1"),
                                                QStringLiteral("acknowledge"), c.stateVersion()) != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.canApplyReviewedOutput());
    ROCLINT_CHECK(c.canApprove());

    bridge::ComprehensionReviewResult blocking;
    blocking.status = bridge::ComprehensionReviewStatus::Completed;
    blocking.interpretedOutcome = "The rewrite targets a different feature.";
    blocking.readyForTask = true;
    bridge::ComprehensionFinding blockingFinding;
    blockingFinding.id = "blocking_1";
    blockingFinding.kind = bridge::ReviewFindingKind::ComprehensionGap;
    blockingFinding.severity = bridge::ReviewFindingSeverity::Blocking;
    blockingFinding.summary = "The requested feature is missing.";
    blocking.findings.push_back(blockingFinding);
    c.setComprehensionReviewForTesting([blocking](const auto&) { return blocking; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(!c.canApplyReviewedOutput());
    ROCLINT_CHECK(c.resolveComprehensionFinding("blocking_1", "confirm_intent", c.stateVersion()) == 0);
    ROCLINT_CHECK(c.resolveComprehensionFinding(QStringLiteral("blocking_1"),
                                                QStringLiteral("acknowledge"), c.stateVersion()) == 0);
    ROCLINT_CHECK(c.resolveComprehensionFinding(QStringLiteral("blocking_1"),
                                                QStringLiteral("keep_original"), c.stateVersion()) != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(!c.hasProposal());
    ROCLINT_CHECK(!c.canApprove());

    std::cout << "  [PASS] Review fails closed on malformed/missing output; required and blocking gates stay strict.\n";
}

void testWaterBottleReviewDoesNotRequireUnspecifiedDesignDetails() {
    const QString draft = QStringLiteral(
        "I want to make a website that is about a water bottle company it is innovative it will use "
        "modern designs and also the water bottle will use copper and aluminum for the cap and the bottle "
        "is glass and the price is 5.99 the website should also focus on being minialistic while feeling "
        "like a tech company depsite being a water bottle company this website/water botttle company "
        "is aimed at selling a premium product to tech bros");
    ScriptedRuntime runtime;
    runtime.discoveryReply = R"({"status":"no_ambiguity","choice_groups":[]})";
    runtime.rewriteText = "Build a product website for an innovative water bottle company selling a "
        "premium glass bottle with a copper and aluminum cap for $5.99. The audience is tech bros. "
        "Keep the visual design minimalist and modern, with a tech-company feel.";
    HostController c(":memory:");
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.reviewPrompt(draft, c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.setRewriteProfile("GENERIC@1", c.stateVersion())));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.rewriteWithoutQuestions(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.hasProposal());
    const auto& candidate = c.proposalForTesting()->rewrittenText;
    for (const auto* requirement : {"glass", "copper", "aluminum", "$5.99", "tech bros", "minimalist"})
        ROCLINT_CHECK(candidate.find(requirement) != std::string::npos);

    auto candidateReview = approvingReviewResult();
    candidateReview.readyForTask = false;
    bridge::ComprehensionFinding optionalDesign;
    optionalDesign.id = "water_bottle_design";
    optionalDesign.kind = bridge::ReviewFindingKind::Readiness;
    optionalDesign.severity = bridge::ReviewFindingSeverity::Blocking;
    optionalDesign.summary = "The minimalist design does not specify exact fonts and colors.";
    optionalDesign.sourceRef = "candidate";
    optionalDesign.sourceExcerpt = "Keep the visual design minimalist and modern, with a tech-company feel.";
    optionalDesign.requiresUserDecision = true;
    optionalDesign.proposedRepair = "Ask the user to specify a color palette.";
    candidateReview.findings = {optionalDesign};
    c.setComprehensionReviewForTesting([candidateReview](const auto&) { return candidateReview; });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(!c.canApprove());
    ROCLINT_CHECK(c.resolveComprehensionFinding("water_bottle_design", "confirm_intent", c.stateVersion()) != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.canApprove() && c.comprehensionFindings().isEmpty());

    ROCLINT_CHECK(!c.isInterviewActive() && !c.hasApproval());
    std::cout << "  [PASS] Water bottle review keeps product facts and accepts the user's design confirmation.\n";
}

void testInterviewStaleResultAndWorkspaceSwitchInvalidate() {
    ScriptedRuntime runtime;
    runtime.discoveryReply = kPerformanceDiscovery;
    runtime.followUpDelay = std::chrono::milliseconds(250);
    runtime.queueFollowUps({kAdaptiveFollowUp});
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    openInterview(c);
    waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
    answerInterview(c, QStringLiteral("Lower latency per click"));
    // The adaptive follow-up is now in flight with a delay. Editing the draft must
    // invalidate it without publishing a stale question.
    const auto editVersion = c.stateVersion();
    c.noteDraftEdited(editVersion);
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    QThread::msleep(400);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    ROCLINT_CHECK(!c.isInterviewActive());
    ROCLINT_CHECK(c.interviewQuestion().isEmpty());

    // A ready review becomes stale when the workspace identity changes.
    ScriptedRuntime runtime2;
    runtime2.discoveryReply = kPerformanceDiscovery;
    runtime2.rewriteText = kInterviewRewrite;
    HostController w(QStringLiteral(":memory:"));
    ROCLINT_CHECK(w.waitForStorageForTesting());
    w.setLegacyDiscoveryForTesting(true);
    connectRuntime(w, runtime2);
    ROCLINT_CHECK(w.waitForCommandForTesting(w.analyzePrompt(kInterviewPrompt, 0)));
    ROCLINT_CHECK(w.waitForIdleForTesting(8000));
    openInterview(w);
    waitForInterviewQuestion(w, QStringLiteral("What should faster mean?"));
    answerInterview(w, QStringLiteral("Lower latency per click"));
    w.waitForCommandForTesting(w.setRewriteProfile(QStringLiteral("GENERIC@1"), w.stateVersion()));
    ROCLINT_CHECK(w.waitForCommandForTesting(w.synthesizeRewrite(w.stateVersion())));
    ROCLINT_CHECK(w.waitForIdleForTesting(8000));
    completeReview(w);
    ROCLINT_CHECK(w.canApplyReviewedOutput());
    QTemporaryDir workspace;
    ROCLINT_CHECK(workspace.isValid());
    w.startCodebaseRefreshForTesting(workspace.path());
    ROCLINT_CHECK(w.waitForIdleForTesting(8000));
    ROCLINT_CHECK(!w.canApplyReviewedOutput());
    ROCLINT_CHECK(!w.canApprove());

    std::cout << "  [PASS] Stale follow-ups and workspace switches invalidate derived interview/review state.\n";
}

const QString kExactPrompt =
    QStringLiteral("Respond to 95% of requests within 500 ms and keep the schema unchanged.");

const std::string kExactDiscovery = R"({"status": "options_generated",
  "choice_groups": [{"dimension": "response path", "question": "Which response path should serve the 500 ms target?", "material": true,
    "alternatives": [
      {"label": "Prefer cached responses", "why_material": "Reuses recent results to hold the target", "evidence": ["500 ms"]},
      {"label": "Prefer fresh requests", "why_material": "Always recomputes at the target", "evidence": ["500 ms"]}]}]})";

void testExplicitInterviewRevivesQuietOrOriginalRoute() {
    {
        // Automatic discovery says no question (an empty ORIGINAL/QUIET snapshot).
        // Opening the interview must start a fresh explicit discovery, not fall
        // through to a dead end.
        ScriptedRuntime runtime;
        runtime.discoveryReply = R"({"kind":"none"})";
        runtime.queueDiscoveries({kPerformanceDiscovery});
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kInterviewPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        ROCLINT_CHECK(c.interventionVerdict() == "QUIET");
        ROCLINT_CHECK(c.interviewQuestion().isEmpty());
        openInterview(c);
        waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
        ROCLINT_CHECK(c.isInterviewActive());
        bool explicitDiscovery = false;
        bool richContract = false;
        for (const auto& request : runtime.requests()) {
            if (request.find("granite.discovery.selective.v11") == std::string::npos) continue;
            if (request.find("interview follow-up") != std::string::npos) continue;
            if (request.find("This is the first question of the substantial interview") != std::string::npos) {
                explicitDiscovery = true;
                richContract = request.find("at most 32 words") != std::string::npos &&
                               request.find("at most 9 words") == std::string::npos;
            }
        }
        ROCLINT_CHECK(explicitDiscovery);
        ROCLINT_CHECK(richContract);
        answerInterview(c, QStringLiteral("Lower latency per click"));
        ROCLINT_CHECK(c.interviewTurnsCompleted() == 1);
    }
    {
        // No automatic analysis at all (Granite never called): opening the explicit
        // interview starts a bounded analysis for the captured draft.
        ScriptedRuntime runtime;
        runtime.discoveryReply = kPerformanceDiscovery;
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(kInterviewPrompt, 0)));
        ROCLINT_CHECK(c.currentRevisionText() == kInterviewPrompt);
        ROCLINT_CHECK(c.analysisStatus() != "READY");
        openInterview(c);
        waitForAnalysisSettled(c, 10000);
        waitForInterviewIdle(c, 10000);
        waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
        ROCLINT_CHECK(c.analysisStatus() == "READY");
        ROCLINT_CHECK(c.interviewFrozenWordForTesting().isEmpty());
        answerInterview(c, QStringLiteral("Lower latency per click"));
        ROCLINT_CHECK(c.workingIntentForTesting().confirmedAlternatives.at("grp_0") == "alt_0_0");
    }
    std::cout << "  [PASS] Explicit interview revives a QUIET/ORIGINAL route with bounded rich discovery.\n";
}

void testExactRequirementsSurviveOrBlockTheRewrite() {
    {
        ScriptedRuntime runtime;
        runtime.discoveryReply = kExactDiscovery;
        runtime.rewriteText = "Respond quickly to requests and keep the schema unchanged.";
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kExactPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        openInterview(c);
        waitForInterviewQuestion(c, QStringLiteral("500 ms target"));
        answerInterview(c, QStringLiteral("Prefer cached responses"));
        c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        ROCLINT_CHECK(!c.hasProposal());
        ROCLINT_CHECK(c.proposalStatus() == "VALIDATION_FAILED");
        ROCLINT_CHECK(c.proposalMessage().contains(QStringLiteral("exact requirement")));
        ROCLINT_CHECK(!c.canApprove());
        // The exact requirement remains a source requirement in the brief.
        bool sawExact = false;
        for (const auto& item : c.intentBriefItems()) {
            const auto map = item.toMap();
            if (map.value("sourceExcerpt").toString().contains("500 ms") &&
                map.value("sourceExcerpt").toString().contains("95%")) {
                sawExact = true;
            }
        }
        ROCLINT_CHECK(sawExact);
    }
    {
        ScriptedRuntime runtime;
        runtime.discoveryReply = kExactDiscovery;
        runtime.rewriteText =
            "Respond to 95% of requests within 500 ms, keep the schema unchanged, and prefer cached responses.";
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kExactPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        openInterview(c);
        waitForInterviewQuestion(c, QStringLiteral("500 ms target"));
        answerInterview(c, QStringLiteral("Prefer cached responses"));
        c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        ROCLINT_CHECK(c.proposalStatus() == "READY_FOR_REVIEW");
        completeReview(c);
        ROCLINT_CHECK(c.canApprove());
        setClipboard(QStringLiteral("before"));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.approveAndCopyRewrite(c.stateVersion())));
        const auto copied = clipboardText();
        ROCLINT_CHECK(copied.contains("500 ms"));
        ROCLINT_CHECK(copied.contains("95%"));
    }
    {
        // An identical rewrite is not presented as a proposal.
        ScriptedRuntime runtime;
        runtime.discoveryReply = kExactDiscovery;
        runtime.rewriteText = kExactPrompt.toStdString();
        runtime.queueFollowUps({"{\"kind\":\"none\"}"});
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kExactPrompt, 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        openInterview(c);
        waitForInterviewQuestion(c, QStringLiteral("500 ms target"));
        answerInterview(c, QStringLiteral("Prefer cached responses"));
        c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        ROCLINT_CHECK(!c.hasProposal());
        ROCLINT_CHECK(c.proposalText().isEmpty());
        ROCLINT_CHECK(!c.canApprove());
        ROCLINT_CHECK(c.proposalStatus() == "VALIDATION_FAILED" || c.proposalStatus() == "FAILED");
    }
    std::cout << "  [PASS] Exact values survive or block the rewrite; identical output is not a proposal.\n";
}

const QString kMusicPrompt = QStringLiteral(
    "Design a music app with compact text, a fusion of 1990s and 1960s Beatles/Revolver-era references, "
    "and easy browsing.");

const std::string kMusicDiscovery = R"({"status": "options_generated",
  "choice_groups": [{"dimension": "typography", "question": "What should compact text mean?", "material": true,
    "alternatives": [
      {"label": "Smaller type sizes", "why_material": "Shows more text without removing features", "evidence": ["compact text"]},
      {"label": "Fewer visible controls", "why_material": "Shows less interface at once", "evidence": ["compact text"]}]}]})";

const char* kMusicCustomAnswer =
    "smaller type sizes and tighter line spacing, not fewer features or shortened labels; keep browsing one tap away";

void testMusicCustomAnswerAndNegativeConstraintReachReview() {
    ScriptedRuntime runtime;
    runtime.discoveryReply = kMusicDiscovery;
    runtime.rewriteText =
        "Design a music app with smaller type sizes and tighter line spacing, a fusion of 1990s and 1960s "
        "Beatles/Revolver-era references, and easy browsing with one-tap access.";
    runtime.queueFollowUps({"{\"kind\":\"none\"}"});
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(kMusicPrompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    openInterview(c);
    waitForInterviewQuestion(c, QStringLiteral("compact text"));
    answerInterview(c, QString::fromUtf8(kMusicCustomAnswer), QStringLiteral("custom"));
    ROCLINT_CHECK(c.workingIntentForTesting().customResolutions.at("grp_0") == kMusicCustomAnswer);
    bool rawExact = false;
    for (const auto& item : c.intentBriefItems()) {
        const auto map = item.toMap();
        if (map.value("id").toString() == "grp_0") {
            rawExact = map.value("rawAnswer").toString() == QString::fromUtf8(kMusicCustomAnswer);
        }
    }
    ROCLINT_CHECK(rawExact);
    const auto payload = c.getConfirmedIntentPayload();
    bool intentExact = false;
    for (const auto& item : payload.value("items").toList()) {
        const auto map = item.toMap();
        if (map.value("itemId").toString() == "grp_0") {
            intentExact = map.value("customText").toString() == QString::fromUtf8(kMusicCustomAnswer);
        }
    }
    ROCLINT_CHECK(intentExact);
    c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.proposalStatus() == "READY_FOR_REVIEW");
    // The confirmed negative wording was supplied to the rewrite request verbatim.
    bool rewriteSawAnswer = false;
    for (const auto& request : runtime.requests()) {
        if (request.find("<instructions>") == std::string::npos) continue;
        if (request.find(kMusicCustomAnswer) != std::string::npos) rewriteSawAnswer = true;
    }
    ROCLINT_CHECK(rewriteSawAnswer);

    struct Captured {
        bridge::ComprehensionReviewRequest request;
        int calls{0};
    };
    auto captured = std::make_shared<Captured>();
    bridge::ComprehensionReviewResult drift;
    drift.status = bridge::ComprehensionReviewStatus::Completed;
    drift.interpretedOutcome = "The rewrite makes the interface denser and may drop features or labels.";
    drift.readyForTask = false;
    bridge::ComprehensionFinding omission;
    omission.id = "music_negative_1";
    omission.kind = bridge::ReviewFindingKind::NegativeConstraint;
    omission.severity = bridge::ReviewFindingSeverity::Blocking;
    omission.summary = "The rewrite does not preserve 'not fewer features or shortened labels'.";
    omission.sourceRef = "grp_0";
    omission.sourceExcerpt = kMusicCustomAnswer;
    omission.requiresUserDecision = true;
    omission.proposedRepair =
        "Keep every feature and full labels; use smaller type and tighter spacing instead.";
    drift.findings.push_back(omission);
    c.setComprehensionReviewForTesting([captured, drift](const bridge::ComprehensionReviewRequest& request) {
        captured->request = request;
        ++captured->calls;
        return drift;
    });
    ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
    waitForReviewState(c, QStringLiteral("ready"));
    ROCLINT_CHECK(captured->calls == 1);
    ROCLINT_CHECK(std::find(captured->request.confirmedDecisions.begin(),
                            captured->request.confirmedDecisions.end(), kMusicCustomAnswer) !=
                  captured->request.confirmedDecisions.end());
    ROCLINT_CHECK(!c.canApplyReviewedOutput());
    ROCLINT_CHECK(!c.canApprove());
    bool sawBlocking = false;
    for (const auto& item : c.comprehensionFindings()) {
        const auto map = item.toMap();
        if (map.value("kind").toString() == "negative_constraint" &&
            map.value("severity").toString() == "blocking") {
            sawBlocking = true;
        }
    }
    ROCLINT_CHECK(sawBlocking);
    // A blocking/required contradiction that needs a decision returns to the interview.
    ROCLINT_CHECK(c.resolveComprehensionFinding(QStringLiteral("music_negative_1"),
                                                QStringLiteral("acknowledge"), c.stateVersion()) == 0);
    ROCLINT_CHECK(c.resolveComprehensionFinding(QStringLiteral("music_negative_1"),
                                                QStringLiteral("return_to_interview"), c.stateVersion()) != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    ROCLINT_CHECK(c.isInterviewActive());
    ROCLINT_CHECK(c.interviewQuestion() == "What should the rewrite say here?");
    ROCLINT_CHECK(!c.canApplyReviewedOutput());

    std::cout << "  [PASS] Music custom answer/negative constraint reach rewrite and review; drift blocks apply.\n";
}

roclint::core::analysis::PromptTriage sealedRuleConflictTriage(HostController& c, const QString& prompt) {
    roclint::core::analysis::PromptTriage triage;
    triage.sessionId = c.currentSessionIdString().toStdString();
    triage.revisionId = c.currentRevisionId().toStdString();
    triage.revisionTextHash = crypto::Sha256::hexDigest(prompt.toStdString());
    triage.stateVersion = c.stateVersion();
    triage.requestGroupId = "grp_test_triage";
    triage.attemptId = "att_test_triage";
    triage.provider = "julia_local";
    triage.modelId = "julia-test";
    triage.modelRevision = "test-revision";
    triage.weightsSha256 = std::string(64, 'a');
    triage.configurationId = "test-config";
    triage.execution = roclint::core::analysis::TriageExecution::Mock;
    triage.categoryScores = {0.10, 0.12, 0.20, 0.9999};
    auto sealed = roclint::core::analysis::sealPromptTriage(triage, prompt.toStdString());
    ROCLINT_CHECK(sealed.ok());
    return std::move(sealed).value();
}

void testNumericConflictCannotBypassDiscovery() {
    const QString prompt = QStringLiteral(
        "Set the timeout to exactly 500 ms. The timeout must be at least 1 second.");
    ROCLINT_CHECK(rewrite::StructuredEntityExtractor::analyze(prompt.toStdString())
                      .detectedExplicitConflicts.empty());
    ScriptedRuntime runtime;
    runtime.discoveryReply = R"({"kind":"conflict","description":
      "The same timeout cannot be exactly 500 ms and at least 1 second."})";
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(prompt, 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    const auto triage = sealedRuleConflictTriage(c, prompt);
    ROCLINT_CHECK(c.startDiscoveryWithTriageForTesting(triage) != 0);
    ROCLINT_CHECK(c.waitForIdleForTesting(8000));
    bool discoveryCalled = false;
    for (const auto& request : runtime.requests()) {
        if (request.find("granite.discovery.selective.v11") != std::string::npos)
            discoveryCalled = true;
    }
    ROCLINT_CHECK(discoveryCalled);
    ROCLINT_CHECK(c.analysisStatus() == "READY");
    ROCLINT_CHECK(c.analysisForTesting().has_value());
    ROCLINT_CHECK(!c.analysisForTesting()->conflicts.empty());
    ROCLINT_CHECK(c.interventionVerdict() != "QUIET");
    std::cout << "  [PASS] Exact quantities do not silence a numeric conflict missed by deterministic extraction.\n";
}

void testTaskParaphrasesReachReviewAndMetaOutputRemainsBlocked() {
    struct Case { const char* original; const char* candidate; bool meta; };
    for (const auto& item : {
             Case{"Find and fix the route causing duplicate orders.",
                  "Identify which endpoint produces duplicate orders and fix it.", false},
             Case{"Explain the retry logic in this code.",
                  "Identify which failures are retried and explain the backoff policy.", false},
             Case{"Make this dashboard faster.",
                  "Determine the specific mood categories for the user.", true}}) {
        ScriptedRuntime runtime;
        runtime.discoveryReply = R"({"status":"options_generated","choice_groups":[{
          "dimension":"scope","question":"Which scope should this task cover?","material":true,
          "alternatives":[
            {"label":"Only the requested task","why_material":"Keeps the scope focused","evidence":[")" +
            std::string(item.original) + R"("]},
            {"label":"Include related behavior","why_material":"Expands the scope","evidence":[")" +
            std::string(item.original) + R"("]}]}]})";
        runtime.rewriteText = item.candidate;
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(QString::fromUtf8(item.original), 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice(QStringLiteral("alt_0_0"), c.stateVersion())));
        c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion()));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(8000));
        // Both are unapproved previews; vocabulary alone cannot decide task meaning.
        ROCLINT_CHECK(c.hasProposal());
        ROCLINT_CHECK(c.proposalText() == QString::fromUtf8(item.candidate));
        ROCLINT_CHECK(!c.canApprove() && !c.canApplyReviewedOutput());
        c.setComprehensionReviewForTesting([item](const bridge::ComprehensionReviewRequest& request) {
            ROCLINT_CHECK(request.originalPrompt == item.original && request.candidatePrompt == item.candidate);
            auto result = approvingReviewResult();
            result.interpretedOutcome = item.candidate;
            if (item.meta) {
                result.readyForTask = false;
                bridge::ComprehensionFinding finding;
                finding.id = "task_replaced_by_meta";
                finding.kind = bridge::ReviewFindingKind::MeaningDrift;
                finding.severity = bridge::ReviewFindingSeverity::Blocking;
                finding.summary = "Asks for unrelated clarification instead of the requested dashboard improvement.";
                finding.sourceRef = "candidate";
                finding.sourceExcerpt = item.candidate;
                result.findings.push_back(std::move(finding));
            }
            return result;
        });
        ROCLINT_CHECK(c.requestComprehensionReview(c.stateVersion()) != 0);
        waitForReviewState(c, QStringLiteral("ready"));
        ROCLINT_CHECK(c.canApplyReviewedOutput() == !item.meta);
        if (item.meta) {
            ROCLINT_CHECK(!c.canApprove());
            ROCLINT_CHECK(!c.comprehensionFindings().isEmpty());
        }
    }
    std::cout << "  [PASS] Task paraphrases reach semantic review; blocking meta findings cannot authorize apply.\n";
}

void testWordPopupExpandsIntoInterviewWithoutRestarting() {
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* bridgePtr = bridge.get();
    HostController c(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    ScriptedRuntime runtime;
    runtime.discoveryReply = kPerformanceDiscovery;
    runtime.wordRefinementReply = R"({
      "needs_clarification": false,
      "clarification_question": "",
      "candidates": [
        {"original_span": "faster", "replacement": "snappier on first paint",
         "explanation": "Improves how quickly the first useful view appears.",
         "assumption": "", "requires_clarification": false}
      ]})";
    connectRuntime(c, runtime);
    c.setAmbientModeEnabled(true);
    QString text = kInterviewPrompt;
    const int wordStart = text.indexOf(QStringLiteral("faster"));
    ROCLINT_CHECK(wordStart > 0);
    roclint::host::ObservedField field;
    field.fieldToken = 9191;
    field.appIdentifier = QStringLiteral("Code.exe");
    field.appDisplayName = QStringLiteral("Visual Studio Code");
    field.processId = 4321;
    field.kind = roclint::host::FieldKind::GuiEditor;
    field.text = text;
    field.caretOffset = wordStart + 6;
    field.caretRect = QRect(120, 160, 2, 18);
    field.fieldRect = QRect(60, 120, 620, 120);
    bridgePtr->observe(field);
    ROCLINT_CHECK(waitForWordSuggestion(c, 8000));
    c.exploreInCompanion();
    ROCLINT_CHECK(c.waitForIdleForTesting(10000));
    waitForInterviewIdle(c, 10000);
    ROCLINT_CHECK(c.isInterviewActive());
    waitForInterviewQuestion(c, QStringLiteral("What should faster mean?"));
    ROCLINT_CHECK(c.interviewFrozenWordForTesting() ==
                  QStringLiteral("faster|%1|%2").arg(wordStart).arg(wordStart + 6));
    bool sawWordContext = false;
    for (const auto& item : c.intentBriefItems()) {
        const auto map = item.toMap();
        if (map.value("id").toString() != "word_selection") continue;
        sawWordContext = map.value("kind").toString() == "proposed_assumption" &&
                         map.value("status").toString() == "proposed" &&
                         map.value("sourceExcerpt").toString() == "faster" &&
                         map.value("text").toString().contains("snappier on first paint") &&
                         map.value("sourceRef").toString() ==
                             QStringLiteral("word:%1-%2").arg(wordStart).arg(wordStart + 6);
    }
    ROCLINT_CHECK(sawWordContext);
    const auto question = c.interviewQuestion();
    const auto turns = c.interviewTurnsCompleted();
    const auto reentry = c.openInterviewFromCompanion(c.stateVersion());
    ROCLINT_CHECK(reentry == c.stateVersion());
    ROCLINT_CHECK(c.interviewQuestion() == question);
    ROCLINT_CHECK(c.interviewTurnsCompleted() == turns);
    answerInterview(c, QStringLiteral("Lower latency per click"));
    ROCLINT_CHECK(c.workingIntentForTesting().confirmedAlternatives.at("grp_0") == "alt_0_0");

    std::cout << "  [PASS] Word popup expands into the interview without losing the frozen span or restarting.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    std::cout << "[Test Host Controller] Starting...\n";
    testLineEndingsAreNormalizedOnCommit();
    testStaleExpectedVersionIsRejected();
    testEditAndCommitClearDownstreamState();
    testRewriteRefusesWithoutIntentOrWithDirtyDraft();
    testConfirmChoiceRejectsForeignAlternative();
    testApproveAndCopyPersistsBeforeCopying();
    testStaleDiscoveryResultIsDiscarded();
    testOutboundPromptsCarryNoLocalContext();
    testApprovalSurvivesReopen();
    testHardBypassCopiesCommittedText();
    testManualModeDisablesRewrite();
    testAnalysisReconnectsToTheRuntime();
    testAnalysisRecoversAfterRuntimeFailure();
    testUnreachableRuntimeExplainsWhatToDo();
    testMalformedModelReplyIsExplainedPlainly();
    testModelDiagnosticsStayInternal();
    testDelayedCommandsCannotMutateTheNewState();
    testAnalysisGroupDeadlineAndExplicitRetry();
    testSubmitClarificationDeferral();
    testCustomAnswersRequireCurrentModelApproval();
    testChoicePolarityUnderRuleConflict();
    testStrictNegativeGuardrailsPassThrough();
    testCommandPaletteAndAssistanceModes();
    testGoldfishAuditPipelineGating();
    testMultiCandidateCreativeExpansion();
    testTokenSpeedModesAndMetrics();
    testF07ReviewReuseAndVersionAdvancement();
    testF08SelectionPersistenceAndApproval();
    testF09UnresolvedHardAuditGating();
    testAsyncSelectionNativeTicketAndOffGuiExecution();
    testAsyncSelectionDelayedTransportResponsiveness();
    testAsyncSelectionStaleSelectionRejection();
    testDraftEditDuringCandidateAudit();
    testDraftEditDuringCandidateSave(false);
    testDraftEditDuringCandidateSave(true);
    testAsyncSelectionSaveFailurePreventsApproval();
    testSessionRestoreUncertaintyPreservation();
    testExactIdAcknowledgmentBinding();
    testWordCompanionBindsSelectedCandidateAndRejectsStaleRequests();
    testPhraseFlowChecksAnswersAndSupportsGuardedUndo();
    testAcceptedWordAnswerSurvivesLocalFailureAndStaysDraftBound();
    testWordCompanionNoopClearingAndClarification();
    testWordQuestionCanRecoverToAnAuditedCorrection();
    testFeedbackOpinionsAreNotMutationProhibitions();
    testWordApplyFeedbackIsReadbackBoundAndFollowsGeometry();
    testWordCompanionCoalescesAndDropsStaleWork();
    testWordAuditFailureNeverOffersAnEdit();
    testIdleWaitIncludesDeferredModelWork();
    testInterviewConnectedFlowCarriesDecisionsIntoRewriteAndReview();
    testInterviewCustomUnknownAndSkipNeverConfirm();
    testChangeIntentDecisionSupersedesAndInvalidatesReview();
    testComprehensionReviewGatingFindingsAndReturnToInterview();
    testComprehensionReviewMalformedAndAdvisoryHandling();
    testWaterBottleReviewDoesNotRequireUnspecifiedDesignDetails();
    testInterviewStaleResultAndWorkspaceSwitchInvalidate();
    testExplicitInterviewRevivesQuietOrOriginalRoute();
    testExactRequirementsSurviveOrBlockTheRewrite();
    testMusicCustomAnswerAndNegativeConstraintReachReview();
    testNumericConflictCannotBypassDiscovery();
    testTaskParaphrasesReachReviewAndMetaOutputRemainsBlocked();
    testWordPopupExpandsIntoInterviewWithoutRestarting();
    testUnavailableBridgeReportsExplicitRangeResult();
    testMechanicallyExactEditDoesNotAcquireAConfirmationQuestion();
    testRecommendationCannotCrossDecisionGroups();
    testDiscoveryAuditPreservesRemainingPaletteDecision();
    {
        HostController thinkingController(":memory:");
        ROCLINT_CHECK(thinkingController.waitForStorageForTesting());
        ROCLINT_CHECK(!thinkingController.thinkingEnabled());
        thinkingController.setThinkingEnabled(true);
        ROCLINT_CHECK(thinkingController.thinkingEnabled());
        thinkingController.setThinkingEnabled(false);
        ROCLINT_CHECK(!thinkingController.thinkingEnabled());
    }
    std::cout << "[Test Host Controller] All tests PASSED.\n";
    return 0;
}
