#include "HostController.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include "support/MockAccessibilityBridge.hpp"

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
    std::chrono::milliseconds discoveryDelay{0};
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::string>> sent = std::make_shared<std::vector<std::string>>();

    bridge::GraniteTransport transport() const {
        auto m = mutex;
        auto log = sent;
        const std::string rewrite = fx::rewriteReply(rewriteText);
        const auto delay = discoveryDelay;
        const std::string wordReply = wordRefinementReply;
        return [m, log, rewrite, delay, wordReply](std::string_view instruction) -> Result<std::string> {
            {
                std::lock_guard<std::mutex> lock(*m);
                log->emplace_back(instruction);
            }
            if (instruction.find("granite.word_refinement.v2") != std::string_view::npos && !wordReply.empty()) {
                return wordReply;
            }
            if (instruction.find("granite.goldfish_word_audit.v2") != std::string_view::npos) {
                const bool second = instruction.find("wcand_2") != std::string_view::npos;
                return std::string(second
                    ? R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"},{"id":"wcand_2","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})"
                    : R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})");
            }
            if (instruction.find("granite.goldfish_discovery_audit.v1") != std::string_view::npos) {
                return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
            }
            if (instruction.find("granite.goldfish_audit.v1") != std::string_view::npos) {
                return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
            }
            if (instruction.find("granite.clarification_check.v1") != std::string_view::npos)
                return std::string(R"({"meaning":"The answer settles the question.","answers_question":true,"compatible":true,"feedback":""})");
            if (instruction.find("<instructions>") != std::string_view::npos) {
                return rewrite;
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
            return kDiscoveryReply;
        };
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
            if (instruction.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
            if (instruction.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
    ROCLINT_CHECK(c.canApprove());

    std::cout << "  [PASS] Async selection: Delayed transport keeps UI responsive and blocks approval until complete.\n";
}

void testAsyncSelectionStaleSelectionRejection() {
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);

    std::atomic<int> auditCount{0};
    c.setTransportOverrideForTesting([&](std::string_view in) -> Result<std::string> {
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
    ROCLINT_CHECK(c.selectedAlternativeIndex() == 0);
    ROCLINT_CHECK(c.proposalText() == alts[0].toMap().value("rewrittenText").toString());
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
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
            if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
        if (in.find("granite.goldfish_audit.v1") != std::string_view::npos) {
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
        if (instruction.find("granite.goldfish_audit.v1") != std::string_view::npos)
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
        if(prompt.find("granite.goldfish_word_audit.v2")!=std::string_view::npos) {
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
        if (prompt.find("granite.word_refinement.v2") == std::string_view::npos) return fallback(prompt);
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
    testWordCompanionNoopClearingAndClarification();
    testWordCompanionCoalescesAndDropsStaleWork();
    testWordAuditFailureNeverOffersAnEdit();
    testIdleWaitIncludesDeferredModelWork();
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
