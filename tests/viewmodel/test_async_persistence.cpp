#include "HostController.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
using roclint::host::HostController;
using namespace roclint::core;
namespace {
template<class F> void until(F condition, int timeoutMs = 8000) {
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeoutMs) { QCoreApplication::processEvents(); QThread::msleep(1); }
    ROCLINT_CHECK(condition());
}
void ready(HostController& controller) {
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
}
const QString prompt = QString::fromStdString(fx::kChainPrompt);
void prepareProposal(HostController& controller) {
    controller.setTransportOverrideForTesting([](std::string_view instructions) -> Result<std::string> {
        if (instructions.find("<instructions>") != std::string_view::npos)
            return fx::rewriteReply("Refactor the parser for lower latency per call.");
        return std::string(R"({"status":"options_generated","choice_groups":[{"question":"What should faster mean?","alternatives":[
            {"label":"Lower latency per call","why_material":"Single requests","evidence":["runs faster"]},
            {"label":"Higher throughput","why_material":"Batches","evidence":["runs faster"]}]}]})");
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(prompt, controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice("alt_0_0", controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
    ROCLINT_CHECK(controller.canApprove());
}
void testContentionAndDraftBarrier() {
    QTemporaryDir directory; const auto path = directory.path() + "/history.db";
    HostController controller(path); ready(controller);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(prompt, 0)));
    roclint::storage::Database blocker(std::filesystem::path(path.toStdString()));
    ROCLINT_CHECK(blocker.execute("BEGIN IMMEDIATE").ok());
    int ticks = 0, draftCompletions = 0; QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++ticks; }); heartbeat.start(2);
    QObject::connect(&controller, &HostController::storageCommandFinished,
        [&](quint64, const QString& name, bool success, const QString&, const QString&) {
            if (name == "noteDraftEdited") { ROCLINT_CHECK(success); ++draftCompletions; }
        });
    QElapsedTimer admission; admission.start();
    const auto baseVersion = controller.stateVersion();
    for (int i = 0; i < 512; ++i) ROCLINT_CHECK(controller.noteDraftEdited(controller.stateVersion()) != 0);
    ROCLINT_CHECK(admission.elapsed() < 150 && controller.stateVersion() == baseVersion + 512 && controller.isDraftDirty());
    const auto storedBeforeFlush = controller.repositoryForTesting()->getSession(SessionId(controller.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(storedBeforeFlush.ok() && storedBeforeFlush.value().stateVersion == baseVersion);
    const QString updated = prompt + " Preserve the interface.";
    const auto commit = controller.commitPrompt(updated, controller.stateVersion());
    ROCLINT_CHECK(commit != 0 && controller.currentRevisionText() == prompt);
    QTimer::singleShot(450, [&]() { ROCLINT_CHECK(blocker.execute("ROLLBACK").ok()); });
    ROCLINT_CHECK(controller.waitForCommandForTesting(commit));
    until([&]() { return draftCompletions == 512; });
    ROCLINT_CHECK(ticks >= 20);
    ROCLINT_CHECK(controller.currentRevisionText() == updated && controller.stateVersion() == baseVersion + 513);
    auto history = controller.repositoryForTesting()->getHistory(SessionId(controller.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(history.ok() && history.value().size() == 2);
    auto session = controller.repositoryForTesting()->getSession(SessionId(controller.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(session.ok() && session.value().draftEditGeneration == 512 && session.value().stateVersion == controller.stateVersion());
}
void testPendingApprovalCannotDeliverAfterEdit() {
    QTemporaryDir directory; const auto path = directory.path() + "/history.db";
    HostController controller(path); ready(controller); prepareProposal(controller);
    QGuiApplication::clipboard()->setText("untouched");
    roclint::storage::Database blocker(std::filesystem::path(path.toStdString()));
    ROCLINT_CHECK(blocker.execute("BEGIN IMMEDIATE").ok());
    int ticks = 0; QTimer heartbeat; QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++ticks; }); heartbeat.start(2);
    const auto approval = controller.approveAndCopyRewrite(controller.stateVersion()); ROCLINT_CHECK(approval);
    QTimer::singleShot(100, [&]() { ROCLINT_CHECK(controller.noteDraftEdited(controller.stateVersion())); });
    QTimer::singleShot(400, [&]() { ROCLINT_CHECK(blocker.execute("ROLLBACK").ok()); });
    ROCLINT_CHECK(!controller.waitForCommandForTesting(approval));
    until([&]() { return !controller.storageBusy(); });
    ROCLINT_CHECK(ticks > 20 && controller.storageNeedsRecovery() && !controller.hasApproval());
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == "untouched");
    const QString newer = prompt + " Keep my newer edit.";
    const auto recovery = controller.retryStorage(newer, controller.stateVersion());
    ROCLINT_CHECK(recovery && controller.waitForCommandForTesting(recovery));
    ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
    ROCLINT_CHECK(!controller.storageNeedsRecovery() && controller.isDraftDirty() && !controller.hasApproval());
    ROCLINT_CHECK(controller.capturedDraftText() == newer);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(newer, controller.stateVersion())));
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == "untouched");
}
void testReentrantEditAtApprovalPublication() {
    HostController controller(":memory:"); ready(controller); prepareProposal(controller);
    QGuiApplication::clipboard()->setText("untouched");
    QObject::connect(&controller, &HostController::stateVersionChanged, [&]() {
        if (controller.hasApproval()) ROCLINT_CHECK(controller.noteDraftEdited(controller.stateVersion()));
    });
    const auto approval = controller.approveAndCopyRewrite(controller.stateVersion());
    ROCLINT_CHECK(approval && !controller.waitForCommandForTesting(approval));
    ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
    ROCLINT_CHECK(controller.isDraftDirty() && !controller.hasApproval());
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == "untouched");
}

void testQueueBoundsAndCachedHistory() {
    HostController controller(":memory:"); ready(controller);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(prompt, 0)));
    const auto expected = controller.stateVersion();
    std::vector<quint64> requests;
    for (int i = 0; i < 128; ++i) {
        const auto id = controller.commitPrompt(prompt, expected); ROCLINT_CHECK(id); requests.push_back(id);
    }
    ROCLINT_CHECK(controller.commitPrompt(prompt, expected) == 0);
    for (const auto id : requests) ROCLINT_CHECK(controller.waitForCommandForTesting(id));
    ROCLINT_CHECK(controller.getRecentSessions().size() == 1 && !controller.storageBusy());
}
void testBusyFailureAndExplicitRecovery() {
    QTemporaryDir directory; const auto path = directory.path() + "/history.db";
    HostController controller(path); ready(controller);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(prompt, 0)));
    roclint::storage::Database blocker(std::filesystem::path(path.toStdString()));
    ROCLINT_CHECK(blocker.execute("BEGIN IMMEDIATE").ok());
    int ticks = 0; QTimer heartbeat; QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++ticks; }); heartbeat.start(2);
    const auto version = controller.stateVersion();
    const auto edit = controller.noteDraftEdited(version); ROCLINT_CHECK(edit);
    ROCLINT_CHECK(!controller.waitForCommandForTesting(edit, 10000));
    ROCLINT_CHECK(ticks > 100 && controller.storageNeedsRecovery() && controller.isDraftDirty());
    ROCLINT_CHECK(controller.currentRevisionText() == prompt);
    ROCLINT_CHECK(blocker.execute("ROLLBACK").ok());
    const auto newer = prompt + " Keep this edited draft.";
    const auto beforeRecovery = controller.stateVersion();
    const auto recovery = controller.retryStorage(newer, beforeRecovery); ROCLINT_CHECK(recovery);
    ROCLINT_CHECK(controller.waitForCommandForTesting(recovery));
    ROCLINT_CHECK(controller.stateVersion() > beforeRecovery && controller.capturedDraftText() == newer && controller.isDraftDirty());
}
void testQueuedCommandCannotReplaceNewerDraft() {
    HostController controller(":memory:"); ready(controller);
    const auto request = controller.commitPrompt("Earlier text", 0); ROCLINT_CHECK(request);
    ROCLINT_CHECK(controller.noteDraftEdited(0));
    ROCLINT_CHECK(!controller.waitForCommandForTesting(request));
    ROCLINT_CHECK(controller.stateVersion() == 0 && controller.sessionCount() == 0);
}

void testShutdownDuringContention() {
    QTemporaryDir directory; const auto path = directory.path() + "/history.db";
    HostController controller(path); ready(controller);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.commitPrompt(prompt, 0)));
    roclint::storage::Database blocker(std::filesystem::path(path.toStdString()));
    ROCLINT_CHECK(blocker.execute("BEGIN IMMEDIATE").ok());
    ROCLINT_CHECK(controller.noteDraftEdited(controller.stateVersion()));
    bool quit = false; int ticks = 0; QTimer heartbeat; heartbeat.start(2);
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++ticks; });
    QObject::connect(&controller, &HostController::quitRequested, [&]() { quit = true; });
    QTimer::singleShot(250, [&]() { ROCLINT_CHECK(blocker.execute("ROLLBACK").ok()); });
    QElapsedTimer elapsed; elapsed.start(); controller.requestQuit();
    ROCLINT_CHECK(elapsed.elapsed() < 100);
    until([&]() { return quit; }); ROCLINT_CHECK(ticks > 10);
}
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    testContentionAndDraftBarrier(); testPendingApprovalCannotDeliverAfterEdit();
    testReentrantEditAtApprovalPublication(); testQueueBoundsAndCachedHistory(); testBusyFailureAndExplicitRecovery();
    testQueuedCommandCannotReplaceNewerDraft(); testShutdownDuringContention();
    std::cout << "Async persistence contention, ordering, recovery and shutdown passed.\n";
}
