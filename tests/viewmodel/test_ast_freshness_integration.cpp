#include "HostController.hpp"
#include "CodebaseWatcher.hpp"
#include "roclint/storage/CodebaseIndex.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <utility>

using roclint::host::HostController;
using roclint::host::CodebaseWatcher;
using namespace roclint::core;
namespace bridge = roclint::bridge;

namespace roclint::host {
// Test-only delay inside the asynchronous proposal revalidation worker.
void setCodebaseRevalidationDelayForTesting(int milliseconds);
}
namespace {

bridge::GraniteModelInfo testModelInfo() {
    auto model = fx::testModel();
    bridge::GraniteModelInfo info;
    info.modelId = model.modelId;
    info.digest = model.digest;
    info.quantization = model.quantization;
    info.transport = model.transport;
    return info;
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(5);
    }
    return predicate();
}

bool waitForProposalCleared(HostController& controller, int timeoutMs = 15000) {
    return waitFor([&controller]() {
        return !controller.hasProposal() && !controller.hasApproval();
    }, timeoutMs);
}

void writeSource(const QString& path, const QByteArray& text) {
    QFile file(path);
    ROCLINT_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ROCLINT_CHECK(file.write(text) == text.size());
    ROCLINT_CHECK(file.flush());
}

void processFor(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

void setConsentedWorkspaceRoots(const QStringList& roots) {
    QSettings settings;
    settings.setValue(QStringLiteral("workspace/codebaseContextConsentedRoots"), roots);
    settings.sync();
}

// Runs a workspace switch on the UI thread from inside a scripted model reply
// and waits until the host has processed it. The test's main thread keeps
// processing events while it waits for the command/worker.
void runOnUiThreadAndWait(HostController& controller, const std::function<void()>& action) {
    std::atomic<bool> done{false};
    QMetaObject::invokeMethod(&controller, [&]() {
        action();
        done.store(true);
    }, Qt::QueuedConnection);
    QElapsedTimer timer;
    timer.start();
    while (!done.load() && timer.elapsed() < 10000) QThread::msleep(1);
    ROCLINT_CHECK(done.load());
}

QStringList indexCompanionFiles(const QString& indexPath) {
    return {indexPath, indexPath + QStringLiteral("-wal"), indexPath + QStringLiteral("-shm")};
}

bool renameWithRetry(const QString& from, const QString& to) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (QFile::rename(from, to)) return true;
        QThread::msleep(10);
    }
    return false;
}

bool moveIndexAside(const QString& indexPath, const QString& backupPrefix) {
    const auto files = indexCompanionFiles(indexPath);
    for (const auto& file : files) {
        if (!QFile::exists(file)) continue;
        if (!renameWithRetry(file, backupPrefix + file.mid(indexPath.size()))) return false;
    }
    return true;
}

bool restoreIndex(const QString& indexPath, const QString& backupPrefix) {
    if (QFileInfo(indexPath).isDir() && !QDir(indexPath).removeRecursively()) return false;
    QFile::remove(indexPath);
    const auto files = indexCompanionFiles(indexPath);
    for (const auto& file : files) {
        const auto backup = backupPrefix + file.mid(indexPath.size());
        if (!QFile::exists(backup)) continue;
        if (!renameWithRetry(backup, file)) return false;
    }
    return true;
}

int waitForRefreshStartsToSettle(HostController& controller, int quietMs = 500, int timeoutMs = 10000) {
    int last = controller.codebaseRefreshStartsForTesting();
    QElapsedTimer quiet;
    QElapsedTimer total;
    quiet.start();
    total.start();
    while (total.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
        const int current = controller.codebaseRefreshStartsForTesting();
        if (current != last) {
            last = current;
            quiet.restart();
        } else if (quiet.elapsed() >= quietMs) {
            break;
        }
    }
    return last;
}

const char* kDiscoveryReply = R"({"status":"options_generated",
      "choice_groups":[{"dimension":"performance","question":"What should faster mean?","material":true,
        "alternatives":[
          {"label":"Lower latency per call","why_material":"Optimizes single requests","evidence":["runs faster"]},
          {"label":"Higher throughput","why_material":"Optimizes batches","evidence":["runs faster"]}]}]})";

void installScriptedRewrite(HostController& controller, const std::string& rewrite,
                            std::function<void()> beforeRewriteReply = {},
                            std::function<void()> beforeDiscoveryReply = {}) {
    controller.setLegacyDiscoveryForTesting(true);
    controller.setTransportOverrideForTesting(
        [rewrite, beforeRewriteReply = std::move(beforeRewriteReply),
         beforeDiscoveryReply = std::move(beforeDiscoveryReply)](std::string_view instruction) -> Result<std::string> {
            // Audit prompts also contain <instructions>; they must not fire rewrite-side
            // mutations before the actual synthesis has captured its AST evidence.
            if (instruction.find("granite.goldfish_") != std::string_view::npos) {
                return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
            }
            if (instruction.find("granite.rewrite.format.") != std::string_view::npos) {
                if (beforeRewriteReply) beforeRewriteReply();
                return rewrite;
            }
            if (instruction.find("granite.discovery.selective.") != std::string_view::npos) {
                if (beforeDiscoveryReply) beforeDiscoveryReply();
                return std::string(kDiscoveryReply);
            }
            return makeError(ErrorCode::InvalidInput, "Unexpected AST freshness test operation");
        });
    ROCLINT_CHECK(controller.markModelReadyForTesting(testModelInfo()));
}

void acknowledgePendingConstraints(HostController& controller) {
    const auto constraints = controller.proposalConstraints();
    for (const auto& value : constraints) {
        const auto item = value.toMap();
        if (!item.value(QStringLiteral("needsReview")).toBool()) continue;
        ROCLINT_CHECK(controller.waitForCommandForTesting(
            controller.acknowledgeConstraint(item.value(QStringLiteral("id")).toString(), true,
                                             controller.stateVersion())));
    }
}

void approveCurrentProposal(HostController& controller) {
    if (!controller.canApprove()) acknowledgePendingConstraints(controller);
    ROCLINT_CHECK(controller.canApprove());
    if (auto* clipboard = QGuiApplication::clipboard()) clipboard->setText(QStringLiteral("before"));
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.approveAndCopyRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.hasApproval());
}

void prepareConfirmedIntent(HostController& controller, const QString& prompt) {
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(prompt, 0)));
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.analyzePrompt(prompt, controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.analysisStatus() == QStringLiteral("READY"));
    ROCLINT_CHECK(controller.isClarifying());
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.confirmChoice(QStringLiteral("alt_0_0"), controller.stateVersion())));
}

void createProposal(HostController& controller, const QString& prompt) {
    prepareConfirmedIntent(controller, prompt);
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.proposalStatus() == QStringLiteral("READY_FOR_REVIEW"));
    ROCLINT_CHECK(controller.proposalForTesting().has_value());
}

QString indexDatabase(QTemporaryDir& data) {
    return QDir(data.path()).filePath(QStringLiteral("index.db"));
}

void testSavedFileHashGatesGrounding() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    const QString prompt = QStringLiteral("Rename demo::run to step in demo.");
    const QString xml = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(xml.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(xml.contains(QStringLiteral("demo::run")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));
    ROCLINT_CHECK(!controller.groundedEvidence().isEmpty());
    ROCLINT_CHECK(controller.lastGroundingApplyMsForTesting() < 5.0);
    std::cout << "  grounding_apply_ms=" << controller.lastGroundingApplyMsForTesting() << "\n";
    const auto evidence = controller.groundedEvidence().front().toMap();
    ROCLINT_CHECK(evidence.value(QStringLiteral("status")).toString() == QStringLiteral("VERIFIED"));
    ROCLINT_CHECK(evidence.value(QStringLiteral("signature")).toString().contains(QStringLiteral("run")));

    // Changing the saved bytes without a refresh must invalidate the evidence;
    // the watcher notification is only a hint, not the source of truth.
    writeSource(source, "namespace demo { int walk(int count); }\n");
    const QString staleXml = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(staleXml.isEmpty());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));

    // The explicit refresh revalidates the saved file and recovers grounding.
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    const QString refreshed = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename demo::walk to step in demo."));
    ROCLINT_CHECK(refreshed.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(refreshed.contains(QStringLiteral("demo::walk")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] Host grounding validates saved-file hashes and recovers after refresh.\n";
}

void testChangedSavedFileInvalidatesPendingProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    createProposal(controller, prompt);
    ROCLINT_CHECK(!controller.proposalForTesting()->astSnapshotHash.empty());

    // A saved-byte change must clear the pending proposal and approval rather
    // than silently keeping a proposal bound to older evidence.
    writeSource(source, "namespace demo { int walk(int count); }\n");
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(waitForProposalCleared(controller));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));

    std::cout << "  [PASS] Changed saved evidence invalidates the pending grounded proposal.\n";
}

void testIncompletePathsRefreshCannotAuthorizeGrounding() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));

    auto pathFor = [&](const char* name) {
        return QDir(workspace.path()).filePath(QStringLiteral("src/%1").arg(QString::fromLatin1(name)));
    };
    for (const char* name : {"a.hpp", "b.hpp"})
        writeSource(pathFor(name), "namespace demo { int run(int count); }\n");
    writeSource(pathFor("c.hpp"), "namespace filler_c { int value(int count); }\n");
    writeSource(pathFor("d.hpp"), "namespace filler_d { int value(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    const QString initial = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(initial.contains(QStringLiteral("AMBIGUOUS")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."));
    createProposal(controller, prompt);
    ROCLINT_CHECK(controller.hasProposal());

    // An incremental pass that cannot finish its requested paths and then
    // exhausts its bounded unprocessed-path retries must not retain readiness.
    // maximumFiles=1 keeps the requested set larger than one pass; the zero
    // scan budget makes every generation deterministically incomplete so no
    // late duplicate notification can accidentally finish the cycle.
    roclint::storage::CodebaseIndexLimits limits;
    limits.maximumFiles = 1;
    limits.scanBudgetMs = 0;
    controller.setCodebaseRefreshLimitsForTesting(limits);
    writeSource(pathFor("a.hpp"), "namespace demo { int run(int count); }\n// touched\n");
    writeSource(pathFor("b.hpp"), "namespace demo { int run(int count); }\n// touched\n");
    writeSource(pathFor("c.hpp"), "namespace filler_c { int value(int count); }\n// touched\n");
    writeSource(pathFor("d.hpp"), "namespace filler_d { int value(int count); }\n// touched\n");

    const int beforePathsRefresh = controller.codebaseRefreshStartsForTesting();
    ROCLINT_CHECK(waitFor([&]() {
        return !controller.codebaseIndexReadyForTesting() &&
               controller.codebaseRefreshStartsForTesting() >= beforePathsRefresh + 3;
    }));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    // The cycle is bounded: the initial pass plus at most two unprocessed-path
    // retries. A late duplicate notification may start another pass, but no
    // retry loop may begin without new edits.
    const int cycleStarts = controller.codebaseRefreshStartsForTesting();
    ROCLINT_CHECK(cycleStarts - beforePathsRefresh <= 6);
    processFor(800);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(!controller.codebaseIndexReadyForTesting());
    // The incomplete pass or the proposal invalidation may be the last status
    // writer; either way grounding must not be authoritative.
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());
    ROCLINT_CHECK(waitForProposalCleared(controller));
    const QString withheld = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(withheld.isEmpty());

    // A complete recovery must re-establish the authoritative ambiguity.
    controller.setCodebaseRefreshLimitsForTesting(std::nullopt);
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(!controller.hasProposal());
    const QString recovered = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(recovered.contains(QStringLiteral("AMBIGUOUS")));
    ROCLINT_CHECK(!recovered.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] An incomplete maxFiles=1 paths refresh stays non-authoritative and recovers.\n";
}

void testCompetingCandidateInvalidatesPendingProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename run to step so it runs faster.");
    createProposal(controller, prompt);
    ROCLINT_CHECK(!controller.proposalForTesting()->astSnapshotHash.empty());
    approveCurrentProposal(controller);
    ROCLINT_CHECK(controller.hasProposal() && controller.hasApproval());

    // A competing candidate in another file leaves every selected-file hash
    // unchanged, but the current resolution is now ambiguous.
    writeSource(QDir(workspace.path()).filePath(QStringLiteral("src/other.cpp")),
                "namespace other { int run(int count); }\n");
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(waitForProposalCleared(controller));

    const QString current = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(current.contains(QStringLiteral("AMBIGUOUS")));
    ROCLINT_CHECK(!current.contains(QStringLiteral("VERIFIED")));

    std::cout << "  [PASS] A competing candidate clears the pending proposal and approval.\n";
}

void testUnrelatedChangePreservesPendingProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    writeSource(QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp")),
                "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename run to step so it runs faster.");
    createProposal(controller, prompt);
    const std::string proposalId = controller.proposalForTesting()->id;
    const std::string proposalHash = controller.proposalForTesting()->astSnapshotHash;

    writeSource(QDir(workspace.path()).filePath(QStringLiteral("src/unrelated.cpp")),
                "namespace util { int helper(int value); }\n");
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    processFor(400);

    ROCLINT_CHECK(controller.hasProposal());
    ROCLINT_CHECK(controller.proposalForTesting()->id == proposalId);
    ROCLINT_CHECK(controller.proposalForTesting()->astSnapshotHash == proposalHash);
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("STALE"));

    std::cout << "  [PASS] An unrelated source change preserving evidence keeps the proposal.\n";
}

void testRelevantCandidateRemovalClearsProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename run to step so it runs faster.");
    createProposal(controller, prompt);
    ROCLINT_CHECK(!controller.proposalForTesting()->astSnapshotHash.empty());

    ROCLINT_CHECK(QFile::remove(source));
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(waitForProposalCleared(controller));

    const QString current = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(!current.contains(QStringLiteral("VERIFIED")));

    std::cout << "  [PASS] Removing the relevant candidate clears the pending proposal.\n";
}

void testStaleRevalidationCannotInvalidateNewerProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    const QString indexPath = indexDatabase(data);
    controller.setCodebaseIndexPathForTesting(indexPath);
    CodebaseWatcher::Limits watcherLimits;
    watcherLimits.debounceMs = 60000; // keep this ordering test free of watcher scans
    watcherLimits.fallbackRefreshIntervalMs = 0;
    controller.setCodebaseWatcherLimitsForTesting(watcherLimits);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename run to step so it runs faster.");
    createProposal(controller, prompt);
    const std::string firstId = controller.proposalForTesting()->id;
    ROCLINT_CHECK(!controller.proposalForTesting()->astSnapshotHash.empty());

    // Start a slow revalidation of the first proposal, then let the index move
    // and create a newer proposal before that worker finishes.
    roclint::host::setCodebaseRevalidationDelayForTesting(2500);
    const int beforeRefresh = controller.codebaseRefreshStartsForTesting();
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > beforeRefresh &&
               controller.codebaseIndexReadyForTesting();
    }));

    writeSource(source, "namespace demo { int walk(int count); }\n");
    {
        roclint::storage::CodebaseIndex index(std::filesystem::path(indexPath.toStdWString()));
        const roclint::storage::WorkspaceContext context{
            std::filesystem::path(workspace.path().toStdWString()), {}, true};
        ROCLINT_CHECK(index.refresh(context).ok());
    }

    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.proposalStatus() == QStringLiteral("READY_FOR_REVIEW"));
    ROCLINT_CHECK(controller.proposalForTesting().has_value());
    const std::string secondId = controller.proposalForTesting()->id;
    ROCLINT_CHECK(secondId != firstId);

    // The first worker has finished by now (waitForIdle includes it). Its
    // result was generated against the older snapshot generation and must not
    // have cleared or staled the newer proposal.
    processFor(300);
    ROCLINT_CHECK(controller.hasProposal());
    ROCLINT_CHECK(controller.proposalForTesting()->id == secondId);
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("STALE"));
    roclint::host::setCodebaseRevalidationDelayForTesting(0);
    ROCLINT_CHECK(controller.waitForIdleForTesting(6000));

    std::cout << "  [PASS] A superseded revalidation result cannot invalidate a newer proposal.\n";
}

void testPostGenerationEvidenceChangeDiscardsProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller,
        fx::rewriteReply("Rename run to step so it runs faster."),
        [source]() { writeSource(source, "namespace demo { int walk(int count); }\n"); });
    const QString prompt = QStringLiteral("Rename run to step so it runs faster.");
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(prompt, 0)));
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.analyzePrompt(prompt, controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.isClarifying());
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.confirmChoice(QStringLiteral("alt_0_0"), controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(controller.proposalStatus() == QStringLiteral("FAILED"));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));

    std::cout << "  [PASS] Evidence changing during generation still discards the proposal.\n";
}

void testIncompleteScanDegradesAndRecovers() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    roclint::storage::CodebaseIndexLimits limits;
    limits.scanBudgetMs = 0;
    controller.setCodebaseRefreshLimitsForTesting(limits);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(!controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("INCOMPLETE"));

    ROCLINT_CHECK(controller.lastError().contains(QStringLiteral("0-second scan limit")));
    ROCLINT_CHECK(controller.lastError().contains(QStringLiteral("0 source files")));
    ROCLINT_CHECK(controller.lastError().contains(QStringLiteral("smaller project folder")));

    // An explicit refresh uses normal limits and recovers the snapshot.
    controller.setCodebaseRefreshLimitsForTesting(std::nullopt);
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(controller.lastError().isEmpty());
    const QString xml = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename demo::run to step in demo."));
    ROCLINT_CHECK(xml.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    // A newer unrelated error belongs to its own action, not to index recovery.
    controller.setCodebaseRefreshLimitsForTesting(limits);
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(!controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(controller.scopePreamble({}, QStringLiteral("global"), {}).isEmpty());
    const auto unrelatedError = controller.lastError();
    ROCLINT_CHECK(unrelatedError == QStringLiteral("Choose an existing workspace before applying scope."));
    controller.setCodebaseRefreshLimitsForTesting(std::nullopt);
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(controller.lastError() == unrelatedError);

    std::cout << "  [PASS] An incomplete scan withholds grounding, and explicit refresh recovers.\n";
}

void testWorkspaceSwitchClearsOnlyOwnedIndexWarning() {
    QTemporaryDir workspace;
    QTemporaryDir nextWorkspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid() && nextWorkspace.isValid() && data.isValid());
    writeSource(QDir(workspace.path()).filePath("one.hpp"), "int first();\n");
    writeSource(QDir(workspace.path()).filePath("two.hpp"), "int second();\n");
    writeSource(QDir(nextWorkspace.path()).filePath("next.hpp"), "int next();\n");
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    roclint::storage::CodebaseIndexLimits limits;
    limits.maximumFiles = 1;
    controller.setCodebaseRefreshLimitsForTesting(limits);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(!controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(controller.lastError().contains(QStringLiteral("1-source-file limit")));

    controller.setCodebaseRefreshLimitsForTesting(std::nullopt);
    controller.startCodebaseRefreshForTesting(nextWorkspace.path(), true);
    ROCLINT_CHECK(controller.lastError().isEmpty());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(controller.lastError().isEmpty());
    std::cout << "  [PASS] Workspace switch clears its old index warning; file-limit diagnostics are explicit.\n";
}

void testUnavailableCodebaseDoesNotBlockGrounding() {
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(QString());
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(QStringLiteral("Rename run to step.")).isEmpty());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("UNAVAILABLE"));

    std::cout << "  [PASS] Missing codebase consent leaves ordinary prompt help available.\n";
}

void testWorkspaceSwitchInvalidatesProposalAndApproval() {
    QTemporaryDir workspaceA;
    QTemporaryDir workspaceB;
    QTemporaryDir data;
    ROCLINT_CHECK(workspaceA.isValid());
    ROCLINT_CHECK(workspaceB.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspaceA.path()).mkpath(QStringLiteral("src")));
    ROCLINT_CHECK(QDir(workspaceB.path()).mkpath(QStringLiteral("src")));
    const QString sourceA = QDir(workspaceA.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(sourceA, "namespace demo { int run(int count); }\n");
    writeSource(QDir(workspaceB.path()).filePath(QStringLiteral("src/other.cpp")),
                "namespace other { int helper(int value); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspaceA.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspaceA.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    createProposal(controller, prompt);
    approveCurrentProposal(controller);
    ROCLINT_CHECK(controller.hasProposal() && controller.hasApproval());
    ROCLINT_CHECK(controller.canApprove());
    const auto epochInA = controller.workspaceEpochForTesting();

    // B is a genuinely different workspace with no saved consent. The old
    // proposal, analysis, confirmed intent, approval and evidence must all be
    // gone before B can produce anything.
    controller.getProjectFiles(workspaceB.path());
    ROCLINT_CHECK(controller.workspaceEpochForTesting() > epochInA);
    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(!controller.proposalForTesting().has_value());
    ROCLINT_CHECK(!controller.hasApproval());
    ROCLINT_CHECK(!controller.canApprove());
    ROCLINT_CHECK(!controller.analysisForTesting().has_value());
    ROCLINT_CHECK(!controller.confirmedIntentForTesting().has_value());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());

    const auto epochInB = controller.workspaceEpochForTesting();
    controller.getProjectFiles(workspaceB.path());
    ROCLINT_CHECK(controller.workspaceEpochForTesting() == epochInB);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    // Consent for the new workspace enables normal grounding there.
    controller.setCodebaseContextConsentedForTesting(true);
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    const QString xml = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename other::helper to assist."));
    ROCLINT_CHECK(xml.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(xml.contains(QStringLiteral("other::helper")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] Switching workspaces invalidates the old proposal, approval and evidence.\n";
}

void testWorkspaceSwitchDuringDiscoveryDoesNotPublishIntoNewWorkspace() {
    QTemporaryDir workspaceA;
    QTemporaryDir workspaceB;
    QTemporaryDir data;
    ROCLINT_CHECK(workspaceA.isValid());
    ROCLINT_CHECK(workspaceB.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspaceA.path()).mkpath(QStringLiteral("src")));
    ROCLINT_CHECK(QDir(workspaceB.path()).mkpath(QStringLiteral("src")));
    writeSource(QDir(workspaceA.path()).filePath(QStringLiteral("src/engine.cpp")),
                "namespace demo { int run(int count); }\n");
    writeSource(QDir(workspaceB.path()).filePath(QStringLiteral("src/other.cpp")),
                "namespace other { int helper(int value); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspaceA.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspaceA.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    const auto epochInA = controller.workspaceEpochForTesting();

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."), {},
        [&]() {
            runOnUiThreadAndWait(controller, [&]() {
                controller.getProjectFiles(workspaceB.path());
            });
        });
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(prompt, 0)));
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.analyzePrompt(prompt, controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    ROCLINT_CHECK(controller.workspaceEpochForTesting() > epochInA);
    ROCLINT_CHECK(!controller.analysisForTesting().has_value());
    ROCLINT_CHECK(!controller.confirmedIntentForTesting().has_value());
    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());

    std::cout << "  [PASS] A discovery reply from the old workspace does not publish into the new one.\n";
}

void testWorkspaceSwitchDuringRewriteDoesNotPublishIntoNewWorkspace() {
    QTemporaryDir workspaceA;
    QTemporaryDir workspaceB;
    QTemporaryDir data;
    ROCLINT_CHECK(workspaceA.isValid());
    ROCLINT_CHECK(workspaceB.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspaceA.path()).mkpath(QStringLiteral("src")));
    ROCLINT_CHECK(QDir(workspaceB.path()).mkpath(QStringLiteral("src")));
    writeSource(QDir(workspaceA.path()).filePath(QStringLiteral("src/engine.cpp")),
                "namespace demo { int run(int count); }\n");
    writeSource(QDir(workspaceB.path()).filePath(QStringLiteral("src/other.cpp")),
                "namespace other { int helper(int value); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspaceA.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspaceA.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."),
        [&]() {
            runOnUiThreadAndWait(controller, [&]() {
                controller.getProjectFiles(workspaceB.path());
            });
        });
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    prepareConfirmedIntent(controller, prompt);
    const auto epochInA = controller.workspaceEpochForTesting();
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    ROCLINT_CHECK(controller.workspaceEpochForTesting() > epochInA);
    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(!controller.proposalForTesting().has_value());
    ROCLINT_CHECK(!controller.hasApproval());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());

    std::cout << "  [PASS] A rewrite reply from the old workspace does not publish into the new one.\n";
}

void testWorkspaceSwitchBackDoesNotReviveOlderEpoch() {
    QTemporaryDir workspaceA;
    QTemporaryDir workspaceB;
    QTemporaryDir data;
    ROCLINT_CHECK(workspaceA.isValid());
    ROCLINT_CHECK(workspaceB.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspaceA.path()).mkpath(QStringLiteral("src")));
    ROCLINT_CHECK(QDir(workspaceB.path()).mkpath(QStringLiteral("src")));
    writeSource(QDir(workspaceA.path()).filePath(QStringLiteral("src/engine.cpp")),
                "namespace demo { int run(int count); }\n");
    writeSource(QDir(workspaceB.path()).filePath(QStringLiteral("src/other.cpp")),
                "namespace other { int helper(int value); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspaceA.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspaceA.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."),
        [&]() {
            runOnUiThreadAndWait(controller, [&]() {
                controller.getProjectFiles(workspaceB.path());
                controller.getProjectFiles(workspaceA.path());
            });
        });
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    prepareConfirmedIntent(controller, prompt);
    const auto epochInA = controller.workspaceEpochForTesting();
    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(20000));

    // A -> B -> A must advance the epoch twice; the original A ticket cannot
    // revive even though the path is the same again.
    ROCLINT_CHECK(controller.workspaceEpochForTesting() >= epochInA + 2);
    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(!controller.proposalForTesting().has_value());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));

    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    const QString xml = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(xml.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(xml.contains(QStringLiteral("demo::run")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] Returning to an earlier workspace does not revive its old epoch.\n";
}

void testScopeChangeInvalidatesEvidenceAndProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    writeSource(QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp")),
                "namespace demo { int run(int count); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspace.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    createProposal(controller, prompt);
    approveCurrentProposal(controller);
    ROCLINT_CHECK(controller.hasProposal() && controller.hasApproval());
    const auto epochBeforeScope = controller.workspaceEpochForTesting();

    const QString local = controller.scopePreamble(workspace.path(), QStringLiteral("local"),
                                                   QStringLiteral("src/engine.cpp"));
    ROCLINT_CHECK(local.contains(QStringLiteral("Do not modify other files.")));
    ROCLINT_CHECK(controller.workspaceEpochForTesting() > epochBeforeScope);
    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(!controller.hasApproval());
    ROCLINT_CHECK(!controller.canApprove());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());

    // Re-selecting the same active file is not a new epoch.
    const auto epochInLocalScope = controller.workspaceEpochForTesting();
    ROCLINT_CHECK(!controller.scopePreamble(workspace.path(), QStringLiteral("local"),
                                            QStringLiteral("src/engine.cpp")).isEmpty());
    ROCLINT_CHECK(controller.workspaceEpochForTesting() == epochInLocalScope);

    const QString localXml = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename demo::run to step in demo."));
    ROCLINT_CHECK(localXml.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    // Returning to global scope drops the local evidence as well.
    const QString global = controller.scopePreamble(workspace.path(), QStringLiteral("global"), {});
    ROCLINT_CHECK(global.contains(QStringLiteral("Scope: entire project.")));
    ROCLINT_CHECK(controller.workspaceEpochForTesting() > epochInLocalScope);
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());
    const auto epochInGlobalScope = controller.workspaceEpochForTesting();
    ROCLINT_CHECK(!controller.scopePreamble(workspace.path(), QStringLiteral("global"), {}).isEmpty());
    ROCLINT_CHECK(controller.workspaceEpochForTesting() == epochInGlobalScope);

    std::cout << "  [PASS] Active-file/scope changes invalidate evidence and proposals.\n";
}

void testUnavailableIndexFullRefreshFailureClearsProposalAndRecovers() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspace.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    const QString indexPath = indexDatabase(data);
    const QString backup = indexPath + QStringLiteral(".offline");
    controller.setCodebaseIndexPathForTesting(indexPath);
    CodebaseWatcher::Limits watcherLimits;
    watcherLimits.debounceMs = 60000;
    watcherLimits.fallbackRefreshIntervalMs = 0;
    controller.setCodebaseWatcherLimitsForTesting(watcherLimits);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    createProposal(controller, prompt);
    approveCurrentProposal(controller);
    ROCLINT_CHECK(controller.canApprove());

    ROCLINT_CHECK(moveIndexAside(indexPath, backup));
    ROCLINT_CHECK(QDir().mkpath(indexPath));
    writeSource(source, "namespace demo { int walk(int count); }\n");

    const int beforeRefresh = controller.codebaseRefreshStartsForTesting();
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > beforeRefresh;
    }, 10000));
    ROCLINT_CHECK(waitForProposalCleared(controller));
    ROCLINT_CHECK(!controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(!controller.hasApproval());
    ROCLINT_CHECK(!controller.canApprove());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());

    // Restoring the database makes the cache usable again.
    ROCLINT_CHECK(restoreIndex(indexPath, backup));
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    const QString xml = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename demo::walk to step in demo."));
    ROCLINT_CHECK(xml.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(xml.contains(QStringLiteral("demo::walk")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] A failed full refresh on an unavailable index clears proposal/approval and recovers.\n";
}

void testUnavailableIndexPathsRefreshFailureClearsProposalAndRecovers() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspace.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    const QString indexPath = indexDatabase(data);
    const QString backup = indexPath + QStringLiteral(".offline");
    controller.setCodebaseIndexPathForTesting(indexPath);
    CodebaseWatcher::Limits watcherLimits;
    watcherLimits.debounceMs = 40;
    watcherLimits.fallbackRefreshIntervalMs = 60000;
    controller.setCodebaseWatcherLimitsForTesting(watcherLimits);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    createProposal(controller, prompt);
    approveCurrentProposal(controller);
    ROCLINT_CHECK(controller.canApprove());

    ROCLINT_CHECK(moveIndexAside(indexPath, backup));
    ROCLINT_CHECK(QDir().mkpath(indexPath));
    const int beforePaths = controller.codebaseRefreshStartsForTesting();
    writeSource(source, "namespace demo { int walk(int count); }\n");

    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > beforePaths;
    }, 15000));
    ROCLINT_CHECK(waitForProposalCleared(controller));
    ROCLINT_CHECK(!controller.hasProposal());
    ROCLINT_CHECK(!controller.hasApproval());
    ROCLINT_CHECK(!controller.canApprove());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.groundedEvidence().isEmpty());

    ROCLINT_CHECK(restoreIndex(indexPath, backup));
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    const QString xml = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename demo::walk to step in demo."));
    ROCLINT_CHECK(xml.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(xml.contains(QStringLiteral("demo::walk")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] A failed paths refresh on an unavailable index clears proposal/approval and recovers.\n";
}

void testDelayedFailedRevalidationCannotClearNewerProposal() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
    writeSource(source, "namespace demo { int run(int count); }\n");
    setConsentedWorkspaceRoots({QDir::cleanPath(workspace.path())});

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    const QString indexPath = indexDatabase(data);
    const QString backup = indexPath + QStringLiteral(".offline");
    controller.setCodebaseIndexPathForTesting(indexPath);
    CodebaseWatcher::Limits watcherLimits;
    watcherLimits.debounceMs = 60000;
    watcherLimits.fallbackRefreshIntervalMs = 0;
    controller.setCodebaseWatcherLimitsForTesting(watcherLimits);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    installScriptedRewrite(controller, fx::rewriteReply("Rename demo::run to step so it runs faster."));
    const QString prompt = QStringLiteral("Rename demo::run to step so it runs faster.");
    createProposal(controller, prompt);
    const std::string firstId = controller.proposalForTesting()->id;

    // Start a slow validation of the first proposal, then create a newer
    // proposal before the old worker finishes.
    roclint::host::setCodebaseRevalidationDelayForTesting(8000);
    const int beforeRefresh = controller.codebaseRefreshStartsForTesting();
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > beforeRefresh &&
               controller.codebaseIndexReadyForTesting();
    }, 15000));

    writeSource(source, "namespace demo { int walk(int count); }\n");
    {
        roclint::storage::CodebaseIndex index(std::filesystem::path(indexPath.toStdWString()));
        const roclint::storage::WorkspaceContext context{
            std::filesystem::path(workspace.path().toStdWString()), {}, true};
        ROCLINT_CHECK(index.refresh(context).ok());
    }

    ROCLINT_CHECK(controller.waitForCommandForTesting(
        controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(waitFor([&]() {
        return controller.proposalStatus() == QStringLiteral("READY_FOR_REVIEW") &&
               controller.proposalForTesting().has_value() &&
               controller.proposalForTesting()->id != firstId;
    }, 15000));
    const std::string secondId = controller.proposalForTesting()->id;

    // Make the index unreadable before the delayed first validation returns.
    ROCLINT_CHECK(moveIndexAside(indexPath, backup));
    ROCLINT_CHECK(QDir().mkpath(indexPath));
    ROCLINT_CHECK(controller.waitForIdleForTesting(20000));
    processFor(300);

    // The older failed validation belongs to the first proposal and must not
    // clear the newer one.
    ROCLINT_CHECK(controller.hasProposal());
    ROCLINT_CHECK(controller.proposalForTesting().has_value());
    ROCLINT_CHECK(controller.proposalForTesting()->id == secondId);
    if (!controller.canApprove()) acknowledgePendingConstraints(controller);
    ROCLINT_CHECK(controller.canApprove());
    ROCLINT_CHECK(controller.groundingStatus() != QStringLiteral("STALE"));

    roclint::host::setCodebaseRevalidationDelayForTesting(0);
    ROCLINT_CHECK(restoreIndex(indexPath, backup));
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    std::cout << "  [PASS] A delayed failed revalidation cannot clear a newer proposal.\n";
}

void testPermanentWatcherOverflowDoesNotRestart() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("a")));
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("b")));

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    CodebaseWatcher::Limits limits;
    limits.maximumWatchedDirectories = 1;
    limits.debounceMs = 40;
    limits.fallbackRefreshIntervalMs = 60000;
    controller.setCodebaseWatcherLimitsForTesting(limits);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    const int settledStarts = waitForRefreshStartsToSettle(controller, 500, 10000);
    ROCLINT_CHECK(waitForRefreshStartsToSettle(controller, 500, 10000) == settledStarts);

    // Observe a full 10-second window with no source edits. A successful scan
    // must retain the running partial watcher instead of restarting it.
    const int startsBeforeWindow = controller.codebaseRefreshStartsForTesting();
    processFor(10000);
    const int startsAfterWindow = controller.codebaseRefreshStartsForTesting();
    std::cout << "  watcher_overflow|directories=3|max_watched=1|refresh_starts_in_10s="
              << startsAfterWindow << "\n";
    ROCLINT_CHECK(startsAfterWindow == startsBeforeWindow);
    ROCLINT_CHECK(startsAfterWindow <= 3);
    ROCLINT_CHECK(!controller.codebaseWatcherDegradedForTesting());

    std::cout << "  [PASS] Permanent watcher overflow does not restart scans after each refresh.\n";
}

void testPermanentWatcherFailureDegradesAndExplicitRecoveryWatches() {
    QTemporaryDir workspace;
    QTemporaryDir data;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(data.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    const QString source = QDir(workspace.path()).filePath(QStringLiteral("src/a.cpp"));
    writeSource(source, "int alpha;\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    CodebaseWatcher::Limits limits;
    limits.debounceMs = 40;
    limits.fallbackRefreshIntervalMs = 60000;
    controller.setCodebaseWatcherLimitsForTesting(limits);

    CodebaseWatcher::setForceAddPathFailureForTesting(true);
    controller.startCodebaseRefreshForTesting(workspace.path(), true);
    ROCLINT_CHECK(waitFor([&]() { return controller.codebaseWatcherDegradedForTesting(); }, 20000));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    const int degradedStarts = waitForRefreshStartsToSettle(controller, 600, 10000);
    processFor(1000);
    ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() == degradedStarts);
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    CodebaseWatcher::setForceAddPathFailureForTesting(false);
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(!controller.codebaseWatcherDegradedForTesting());
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());

    const int recoveredStarts = waitForRefreshStartsToSettle(controller, 500, 10000);
    writeSource(source, "int beta;\n");
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > recoveredStarts;
    }, 12000));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    std::cout << "  [PASS] Permanent registration failure is bounded, degrades, and recovers explicitly.\n";
}

void testWatcherRecoveryCancelledByWorkspaceSwitchAndQuit() {
    QTemporaryDir workspaceA;
    QTemporaryDir workspaceB;
    QTemporaryDir workspaceC;
    QTemporaryDir data;
    ROCLINT_CHECK(workspaceA.isValid());
    ROCLINT_CHECK(workspaceB.isValid());
    ROCLINT_CHECK(workspaceC.isValid());
    ROCLINT_CHECK(data.isValid());
    for (auto* workspace : {&workspaceA, &workspaceB, &workspaceC})
        ROCLINT_CHECK(QDir(workspace->path()).mkpath(QStringLiteral("src")));
    const QString sourceA = QDir(workspaceA.path()).filePath(QStringLiteral("src/a.cpp"));
    const QString sourceB = QDir(workspaceB.path()).filePath(QStringLiteral("src/b.cpp"));
    writeSource(sourceA, "int alpha;\n");
    writeSource(sourceB, "int beta;\n");

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(indexDatabase(data));
    CodebaseWatcher::Limits limits;
    limits.debounceMs = 40;
    limits.fallbackRefreshIntervalMs = 60000;
    controller.setCodebaseWatcherLimitsForTesting(limits);

    CodebaseWatcher::setForceAddPathFailureForTesting(true);
    controller.startCodebaseRefreshForTesting(workspaceA.path(), true);
    processFor(400); // let the first automatic recovery attempt arm

    // Switching workspaces must cancel the old recovery and start fresh.
    controller.startCodebaseRefreshForTesting(workspaceB.path(), true);
    CodebaseWatcher::setForceAddPathFailureForTesting(false);
    ROCLINT_CHECK(controller.refreshCodebaseIndex());
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(!controller.codebaseWatcherDegradedForTesting());
    const int switchedStarts = waitForRefreshStartsToSettle(controller, 500, 10000);
    writeSource(sourceB, "int gamma;\n");
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > switchedStarts;
    }, 12000));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    // Quitting while a recovery is pending must cancel it.
    CodebaseWatcher::setForceAddPathFailureForTesting(true);
    controller.startCodebaseRefreshForTesting(workspaceC.path(), true);
    processFor(400);
    const int beforeQuit = controller.codebaseRefreshStartsForTesting();
    controller.requestQuit();
    processFor(1500);
    ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() == beforeQuit);
    CodebaseWatcher::setForceAddPathFailureForTesting(false);

    std::cout << "  [PASS] Workspace switching and shutdown cancel pending watcher recovery.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir settingsDirectory;
    ROCLINT_CHECK(settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("RoclintAstFreshnessTests"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();
    std::cout << "[Test AST Freshness Integration] Starting...\n";
    testSavedFileHashGatesGrounding();
    testChangedSavedFileInvalidatesPendingProposal();
    testIncompletePathsRefreshCannotAuthorizeGrounding();
    testCompetingCandidateInvalidatesPendingProposal();
    testUnrelatedChangePreservesPendingProposal();
    testRelevantCandidateRemovalClearsProposal();
    testStaleRevalidationCannotInvalidateNewerProposal();
    testPostGenerationEvidenceChangeDiscardsProposal();
    testIncompleteScanDegradesAndRecovers();
    testWorkspaceSwitchClearsOnlyOwnedIndexWarning();
    testUnavailableCodebaseDoesNotBlockGrounding();
    testWorkspaceSwitchInvalidatesProposalAndApproval();
    testWorkspaceSwitchDuringDiscoveryDoesNotPublishIntoNewWorkspace();
    testWorkspaceSwitchDuringRewriteDoesNotPublishIntoNewWorkspace();
    testWorkspaceSwitchBackDoesNotReviveOlderEpoch();
    testScopeChangeInvalidatesEvidenceAndProposal();
    testUnavailableIndexFullRefreshFailureClearsProposalAndRecovers();
    testUnavailableIndexPathsRefreshFailureClearsProposalAndRecovers();
    testDelayedFailedRevalidationCannotClearNewerProposal();
    testPermanentWatcherOverflowDoesNotRestart();
    testPermanentWatcherFailureDegradesAndExplicitRecoveryWatches();
    testWatcherRecoveryCancelledByWorkspaceSwitchAndQuit();
    std::cout << "[Test AST Freshness Integration] All tests PASSED.\n";
    return 0;
}
