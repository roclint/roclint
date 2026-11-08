#include "HostController.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <iostream>
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
    std::chrono::milliseconds discoveryDelay{0};
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::string>> sent = std::make_shared<std::vector<std::string>>();

    bridge::GraniteTransport transport() const {
        auto m = mutex;
        auto log = sent;
        const std::string rewrite = fx::rewriteReply(rewriteText);
        const auto delay = discoveryDelay;
        return [m, log, rewrite, delay](std::string_view instruction) -> Result<std::string> {
            {
                std::lock_guard<std::mutex> lock(*m);
                log->emplace_back(instruction);
            }
            if (instruction.find("granite.goldfish_audit.v1") != std::string_view::npos) {
                return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
            }
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
        ROCLINT_CHECK(request.find("<instructions>") == std::string::npos);
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
    ROCLINT_CHECK(requests.size() == 3);
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
    testChoicePolarityUnderRuleConflict();
    testStrictNegativeGuardrailsPassThrough();
    testCommandPaletteAndAssistanceModes();
    testGoldfishAuditPipelineGating();
    std::cout << "[Test Host Controller] All tests PASSED.\n";
    return 0;
}
