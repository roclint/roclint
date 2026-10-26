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
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, c.stateVersion()));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "READY");
    ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
    ROCLINT_CHECK(c.activeItemId() == "grp_0");
    ROCLINT_CHECK(c.activeAlternatives().size() == 2);
    ROCLINT_CHECK(!c.hasConfirmedIntent());
    ROCLINT_CHECK(c.confirmChoice(QStringLiteral("alt_0_0"), c.stateVersion()));
    ROCLINT_CHECK(c.interventionVerdict() == "QUIET");
    ROCLINT_CHECK(c.hasConfirmedIntent());
    ROCLINT_CHECK(c.isRewriteEnabled());
}

void synthesize(HostController& c) {
    c.setRewriteProfile(QStringLiteral("GENERIC@1"), c.stateVersion());
    ROCLINT_CHECK(c.synthesizeRewrite(c.stateVersion()));
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
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.commitPrompt(QStringLiteral("line one\r\nline two\rline three"), 0));
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
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(!c.commitPrompt(QStringLiteral("first"), 7));
    ROCLINT_CHECK(c.stateVersion() == 0);
    ROCLINT_CHECK(c.commitPrompt(QStringLiteral("first"), 0));
    ROCLINT_CHECK(!c.commitPrompt(QStringLiteral("second"), 0));
    ROCLINT_CHECK(!c.lastError().isEmpty());
    ROCLINT_CHECK(c.currentRevisionText() == "first");
    ROCLINT_CHECK(!c.noteDraftEdited(99));
    ROCLINT_CHECK(!c.isDraftDirty());
    std::cout << "  [PASS] Commands with a stale expected version are rejected.\n";
}

void testEditAndCommitClearDownstreamState() {
    ScriptedRuntime runtime;
    {
        HostController c(QStringLiteral(":memory:"));
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        analyzeAndAnswer(c);
        synthesize(c);

        ROCLINT_CHECK(c.noteDraftEdited(c.stateVersion()));
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
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        analyzeAndAnswer(c);
        synthesize(c);
        const auto version = c.stateVersion();
        ROCLINT_CHECK(c.commitPrompt(kPrompt + QStringLiteral(" Keep the public API."), version));
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
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    setClipboard(QStringLiteral("untouched"));

    // No analysis at all.
    ROCLINT_CHECK(c.commitPrompt(kPrompt, 0));
    ROCLINT_CHECK(!c.synthesizeRewrite(c.stateVersion()));
    ROCLINT_CHECK(!c.lastError().isEmpty());

    // Analysis is waiting on a question: no confirmed intent yet.
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, c.stateVersion()));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(!c.synthesizeRewrite(c.stateVersion()));
    ROCLINT_CHECK(!c.hasProposal());

    // Intent confirmed, then the draft is edited.
    ROCLINT_CHECK(c.confirmChoice(QStringLiteral("alt_0_0"), c.stateVersion()));
    ROCLINT_CHECK(c.hasConfirmedIntent());
    ROCLINT_CHECK(c.noteDraftEdited(c.stateVersion()));
    ROCLINT_CHECK(!c.synthesizeRewrite(c.stateVersion()));
    ROCLINT_CHECK(!c.approveAndCopyRewrite(c.stateVersion()));

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
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.isClarifying());

    ROCLINT_CHECK(!c.confirmChoice(QStringLiteral("alt_1_0"), c.stateVersion()));
    ROCLINT_CHECK(!c.confirmChoice(QStringLiteral("injected"), c.stateVersion()));
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
    ROCLINT_CHECK(!c.approveAndCopyRewrite(c.stateVersion()));
    ROCLINT_CHECK(clipboardText() == "before");
    ROCLINT_CHECK(!c.acknowledgeConstraint(QStringLiteral("rewrite_nonempty"), true, c.stateVersion()));
    ROCLINT_CHECK(c.acknowledgeConstraint(QStringLiteral("intent_item_grp_0"), true, c.stateVersion()));
    ROCLINT_CHECK(c.pendingReviewCount() == 0);
    ROCLINT_CHECK(c.canApprove());

    const auto session = SessionId(c.currentSessionIdString().toStdString());
    bool approvalStoredBeforeCopy = false;
    QObject::connect(&c, &HostController::promptCopied, [&](const QString&) {
        auto current = c.repositoryForTesting()->getCurrentApproval(session);
        approvalStoredBeforeCopy = current.ok() && current.value().has_value();
    });

    ROCLINT_CHECK(c.approveAndCopyRewrite(c.stateVersion()));
    ROCLINT_CHECK(approvalStoredBeforeCopy);
    ROCLINT_CHECK(c.hasApproval());
    ROCLINT_CHECK(c.approvalLabel() == "Wording reviewed");
    ROCLINT_CHECK(c.lastCopiedText() == QString::fromStdString(kUnverifiedRewrite));
    ROCLINT_CHECK(clipboardText() == QString::fromStdString(kUnverifiedRewrite));

    auto stored = c.repositoryForTesting()->getCurrentApproval(session);
    ROCLINT_CHECK(stored.ok() && stored.value().has_value());
    ROCLINT_CHECK(stored.value()->approvedText == kUnverifiedRewrite);
    ROCLINT_CHECK(stored.value()->proposalHash == c.proposalForTesting()->proposalHash);

    // Changing the rewrite style discards the proposal and its approval.
    c.setRewriteProfile(QStringLiteral("ANTHROPIC_STRUCTURED@1"), c.stateVersion());
    ROCLINT_CHECK(!c.hasProposal());
    ROCLINT_CHECK(!c.hasApproval());
    auto after = c.repositoryForTesting()->getCurrentApproval(session);
    ROCLINT_CHECK(after.ok() && !after.value().has_value());
    std::cout << "  [PASS] Approve & Copy validates, persists the approval, then copies.\n";
}

void testStaleDiscoveryResultIsDiscarded() {
    ScriptedRuntime runtime;
    runtime.discoveryDelay = std::chrono::milliseconds(300);
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.isAnalyzing());
    ROCLINT_CHECK(c.noteDraftEdited(c.stateVersion()));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));

    ROCLINT_CHECK(!c.analysisForTesting().has_value());
    ROCLINT_CHECK(!c.isClarifying());
    auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString()));
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
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    analyzeAndAnswer(c);
    synthesize(c);

    const auto requests = runtime.requests();
    ROCLINT_CHECK(requests.size() == 2);
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
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        analyzeAndAnswer(c);
        synthesize(c);
        // A discarded earlier proposal advances the generation; the restored one must still bind.
        ROCLINT_CHECK(c.discardProposal(c.stateVersion()));
        synthesize(c);
        ROCLINT_CHECK(c.pendingReviewCount() == 0);
        ROCLINT_CHECK(c.approveAndCopyRewrite(c.stateVersion()));
        sessionId = c.currentSessionIdString();
        proposalHash = c.proposalForTesting()->proposalHash;
    }
    {
        HostController c(dbPath);
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.restoreSession(sessionId, c.stateVersion()));
        ROCLINT_CHECK(c.analysisStatus() == "READY");
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(c.hasProposal());
        ROCLINT_CHECK(c.proposalForTesting()->proposalHash == proposalHash);
        ROCLINT_CHECK(c.proposalStatus() == "READY_FOR_REVIEW");
        ROCLINT_CHECK(c.hasApproval());
        ROCLINT_CHECK(c.approvalLabel() == "Wording reviewed");

        // Restoring the open session again keeps the stored approval current.
        ROCLINT_CHECK(c.restoreSession(sessionId, c.stateVersion()));
        ROCLINT_CHECK(c.hasApproval());
        auto stored = c.repositoryForTesting()->getCurrentApproval(SessionId(sessionId.toStdString()));
        ROCLINT_CHECK(stored.ok() && stored.value().has_value());
    }
    {
        // A proposal discarded before quitting is not resurrected.
        HostController c(dbPath);
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.restoreSession(sessionId, c.stateVersion()));
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.discardProposal(c.stateVersion()));
    }
    {
        HostController c(dbPath);
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.restoreSession(sessionId, c.stateVersion()));
        ROCLINT_CHECK(c.hasConfirmedIntent());
        ROCLINT_CHECK(!c.hasProposal());
        ROCLINT_CHECK(!c.hasApproval());
    }
    std::cout << "  [PASS] Proposal and approval survive reopen; discarded ones stay discarded.\n";
}

void testHardBypassCopiesCommittedText() {
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.hardBypass(QStringLiteral("ship it\r\nnow"), 0));
    ROCLINT_CHECK(c.lastCopiedText() == "ship it\nnow");
    ROCLINT_CHECK(clipboardText() == "ship it\nnow");
    ROCLINT_CHECK(c.stateVersion() == 2); // Root creation, then explicit bypass invalidation.
    ROCLINT_CHECK(!c.hasApproval());
    std::cout << "  [PASS] Hard bypass commits and copies the user's own text only.\n";
}

void testManualModeDisablesRewrite() {
    ScriptedRuntime runtime;
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.switchToManual(c.stateVersion()));
    ROCLINT_CHECK(c.isManual());
    ROCLINT_CHECK(!c.hasConfirmedIntent());
    ROCLINT_CHECK(!c.isRewriteEnabled());
    ROCLINT_CHECK(!c.synthesizeRewrite(c.stateVersion()));
    std::cout << "  [PASS] Manual mode never produces an intent or a rewrite.\n";
}

void testAnalysisReconnectsToTheRuntime() {
    FakeOllama ollama;
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(ollama.url()));
    ROCLINT_CHECK(c.modelRuntimeState() == "UNLOADED");
    c.setActiveModel(QStringLiteral("granite4.2:8b"));

    // An explicit model selection starts connecting; analysis waits for it, then runs.
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.isAnalyzing());
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(c.modelRuntimeState() == "READY");
    ROCLINT_CHECK(c.analysisStatus() == "READY");
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(c.activeAlternatives().size() == 2);
    auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString()));
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value()[0].status == roclint::storage::AttemptStatus::Succeeded);
    ROCLINT_CHECK(attempts.value()[0].transport == bridge::kLoopbackOllamaTransport);
    std::cout << "  [PASS] Analysis connects to an unchecked runtime and then runs.\n";
}

void testAnalysisRecoversAfterRuntimeFailure() {
    FakeOllama ollama;
    ollama.available = false;
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(ollama.url()));
    c.setActiveModel(QStringLiteral("granite4.2:8b"));

    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(c.modelRuntimeState() == "FAILED");
    ROCLINT_CHECK(c.analysisStatus() == "FAILED");
    ROCLINT_CHECK(c.lastError().contains(QStringLiteral("HTTP 503")));
    ROCLINT_CHECK(c.activeAlternatives().isEmpty());

    // Once the runtime is back, the next request reconnects instead of repeating the old failure.
    ollama.available = true;
    const int checksBefore = ollama.tagRequests;
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, c.stateVersion()));
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
    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(freePort))));
    c.setActiveModel(QStringLiteral("granite4.2:8b"));
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(c.analysisStatus() == "FAILED");
    ROCLINT_CHECK(c.lastError().contains(QStringLiteral("Make sure Ollama is running")));
    ROCLINT_CHECK(!c.lastError().contains(QStringLiteral("FAILED")));
    ROCLINT_CHECK(!c.isClarifying());
    std::cout << "  [PASS] An unreachable runtime fails visibly with an actionable message.\n";
}

void testMalformedModelReplyIsExplainedPlainly() {
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    c.setTransportOverrideForTesting([](std::string_view) -> Result<std::string> {
        return std::string(R"({"status": "needs_clarification", "choice_groups": [], "clarification_question": ""})");
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(modelInfo()));
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "FAILED");
    ROCLINT_CHECK(c.lastError().startsWith(QStringLiteral("The local model returned an unusable reply.")));
    ROCLINT_CHECK(c.lastError().contains(QStringLiteral("Try again or rephrase")));
    ROCLINT_CHECK(!c.isClarifying());
    ROCLINT_CHECK(c.modelRuntimeState() == "READY");
    std::cout << "  [PASS] A malformed model reply fails visibly with a plain explanation.\n";
}

void testDelayedCommandsCannotMutateTheNewState() {
    ScriptedRuntime runtime;
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    const auto beforeChoice = c.stateVersion();
    ROCLINT_CHECK(c.confirmChoice(QStringLiteral("alt_0_0"), beforeChoice));
    ROCLINT_CHECK(c.stateVersion() == beforeChoice + 1);
    const auto intent = *c.confirmedIntentForTesting();
    ROCLINT_CHECK(!c.skipCurrentItem(beforeChoice));
    ROCLINT_CHECK(c.lastErrorCode() == "STALE_VERSION");
    ROCLINT_CHECK(!c.switchToManual(beforeChoice));
    ROCLINT_CHECK(!c.synthesizeRewrite(beforeChoice));
    ROCLINT_CHECK(c.stateVersion() == beforeChoice + 1 && *c.confirmedIntentForTesting() == intent);
    synthesize(c);
    const auto beforeApproval = c.stateVersion();
    ROCLINT_CHECK(!c.approveAndCopyRewrite(beforeChoice));
    ROCLINT_CHECK(!c.hasApproval());
    ROCLINT_CHECK(c.approveAndCopyRewrite(beforeApproval));
    const auto approvedVersion = c.stateVersion();
    ROCLINT_CHECK(approvedVersion == beforeApproval + 1);
    const auto copied = QGuiApplication::clipboard()->text();
    ROCLINT_CHECK(!c.approveAndCopyRewrite(beforeApproval));
    ROCLINT_CHECK(!c.discardProposal(beforeApproval));
    ROCLINT_CHECK(c.stateVersion() == approvedVersion && c.hasApproval());
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == copied);
    ROCLINT_CHECK(c.repositoryForTesting()->getSession(SessionId(c.currentSessionIdString().toStdString())).value().stateVersion == approvedVersion);

    // Another repository writer advances the stored state while this controller still sees its old version.
    const auto restored = c.repositoryForTesting()->restoreSession(SessionId(c.currentSessionIdString().toStdString())).value();
    RevisionState external(restored.latestRevision.revision, restored.session.highestRevisionSeq, restored.session.stateVersion,
                           restored.session.draftEditGeneration, restored.session.proposalEditGeneration);
    ROCLINT_CHECK(external.advanceState(approvedVersion).ok());
    ROCLINT_CHECK(c.repositoryForTesting()->advanceSessionState(external, approvedVersion, 100).ok());
    ROCLINT_CHECK(!c.discardProposal(approvedVersion));
    ROCLINT_CHECK(c.stateVersion() == approvedVersion && c.hasProposal());
}

void testAnalysisGroupDeadlineAndExplicitRetry() {
    ScriptedRuntime runtime;
    runtime.discoveryDelay = std::chrono::milliseconds(300);
    HostController c(QStringLiteral(":memory:"));
    c.setLegacyDiscoveryForTesting(true);
    connectRuntime(c, runtime);
    c.setAnalysisGroupTimeoutForTesting(60);
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, 0));
    QElapsedTimer elapsed; elapsed.start();
    while (c.analysisStatus() != "FAILED" && elapsed.elapsed() < 1000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    ROCLINT_CHECK(c.analysisStatus() == "FAILED" && elapsed.elapsed() < 250);
    ROCLINT_CHECK(!c.finalSnapshotForTesting() && !c.hasConfirmedIntent());
    const auto firstVersion = c.stateVersion();
    auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString()));
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value().front().status == roclint::storage::AttemptStatus::Failed);
    ROCLINT_CHECK(attempts.value().front().errorMessage.find("ANALYSIS_GROUP_DEADLINE") != std::string::npos);

    // An explicit retry while the expired transport drains must resume once it finishes.
    runtime.discoveryDelay = std::chrono::milliseconds(0);
    c.setTransportOverrideForTesting(runtime.transport());
    c.setAnalysisGroupTimeoutForTesting(1500);
    ROCLINT_CHECK(c.analyzePrompt(kPrompt, c.stateVersion()));
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    ROCLINT_CHECK(c.analysisStatus() == "READY" && c.finalSnapshotForTesting());
    ROCLINT_CHECK(c.stateVersion() > firstVersion && !c.hasConfirmedIntent());
    attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString()));
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
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.analyzePrompt(kPrompt, c.stateVersion()));
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

        ROCLINT_CHECK(c.submitClarification(QStringLiteral("idk lol"), c.stateVersion()));
        ROCLINT_CHECK(wasAutoSelected);
        ROCLINT_CHECK(deferredMsg.contains("Auto-selected: Lower latency per call (Julia 84% confident)"));
        ROCLINT_CHECK(c.ambientStatus().contains("Auto-selected: Lower latency per call (Julia 84% confident)"));
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
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.analyzePrompt(kPrompt, c.stateVersion()));
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

        ROCLINT_CHECK(c.submitClarification(QStringLiteral("you decide"), c.stateVersion()));
        ROCLINT_CHECK(!wasAutoSelected);
        ROCLINT_CHECK(deferredMsg == "Kept original — Julia wasn't confident enough to auto-pick");
        ROCLINT_CHECK(c.ambientStatus() == "Kept original — Julia wasn't confident enough to auto-pick");
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
        c.setLegacyDiscoveryForTesting(true);
        connectRuntime(c, runtime);
        ROCLINT_CHECK(c.analyzePrompt(kPrompt, c.stateVersion()));
        ROCLINT_CHECK(c.waitForIdleForTesting(5000));
        ROCLINT_CHECK(c.interventionVerdict() == "CLARIFY");
        ROCLINT_CHECK(c.activeItemId() == "grp_0");

        c.setRecommendedOptionForTesting(QStringLiteral("Lower latency per call"), 0.90);

        bool deferralFired = false;
        QObject::connect(&c, &HostController::clarificationDeferred, [&](const QString&, bool) {
            deferralFired = true;
        });

        ROCLINT_CHECK(c.submitClarification(QStringLiteral("Optimize for memory footprint"), c.stateVersion()));
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
    testDelayedCommandsCannotMutateTheNewState();
    testAnalysisGroupDeadlineAndExplicitRetry();
    testSubmitClarificationDeferral();
    std::cout << "[Test Host Controller] All tests PASSED.\n";
    return 0;
}
