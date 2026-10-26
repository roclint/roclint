#include "GlobalShortcutManager.hpp"
#include "HostContext.hpp"
#include "HostController.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include "support/MockAccessibilityBridge.hpp"

#include <QClipboard>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace roclint::host;
using namespace roclint::core;

namespace {
const std::string kDiscovery = R"({"status":"options_generated","choice_groups":[{
 "dimension":"performance","question":"Which kind of speed matters?","material":true,
 "alternatives":[{"label":"Lower latency per call","why_material":"Changes individual requests","evidence":["runs faster"]},
 {"label":"Higher batch throughput","why_material":"Changes batches","evidence":["runs faster"]}]}],
 "assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""})";
const QString kRewritten = QStringLiteral("Refactor the parser. Lower latency per call.");

void prepare(HostController& c, std::atomic<int>& calls, int delayMs = 0) {
    c.setTransportOverrideForTesting([&calls, delayMs](std::string_view prompt) -> Result<std::string> {
        ++calls;
        if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        return prompt.find("<instructions>") == std::string_view::npos
                   ? kDiscovery : fx::rewriteReply(kRewritten.toStdString());
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(fx::testModel()));
}

void savePreview(QQuickWindow* window, const QString& name) {
    const QString directory = qEnvironmentVariable("ROCLINT_TEST_ARTIFACT_DIR");
    if (directory.isEmpty()) return;
    ROCLINT_CHECK(QDir().mkpath(directory));
    QTest::qWait(100);
    ROCLINT_CHECK(window->grabWindow().save(directory + QLatin1Char('/') + name));
}

void testNativeGlobalShortcutDispatch() {
#ifdef Q_OS_WIN
    GlobalShortcutManager manager(nullptr);
    int activations = 0;
    QObject::connect(&manager, &GlobalShortcutManager::activated, [&]() { ++activations; });
    ROCLINT_CHECK(manager.dispatchNativeHotkeyForTesting("windows_dispatcher_MSG", 0x0312, 1));
    QCoreApplication::processEvents();
    ROCLINT_CHECK(activations == 1);
    ROCLINT_CHECK(manager.dispatchNativeHotkeyForTesting("windows_generic_MSG", 0x0312, 1));
    QCoreApplication::processEvents();
    ROCLINT_CHECK(activations == 2);
    ROCLINT_CHECK(!manager.dispatchNativeHotkeyForTesting("windows_dispatcher_MSG", 0x0100, 1));
    ROCLINT_CHECK(!manager.dispatchNativeHotkeyForTesting("windows_dispatcher_MSG", 0x0312, 2));
    ROCLINT_CHECK(!manager.dispatchNativeHotkeyForTesting("unrelated_event", 0x0312, 1));
    QCoreApplication::processEvents();
    ROCLINT_CHECK(activations == 2);
#endif
}

void testCaptureAndSettings() {
    QSettings().clear();
    {
        HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        c.setHostContextEnabled(false);
        c.waitForCommandForTesting(c.setRewriteProfile(QStringLiteral("OPENAI_STRUCTURED"), c.stateVersion()));
        HostContextDetector::setMockObservation(HostContextObservation{
            HostContextCategory::CodeEditor, QStringLiteral("VS Code"), QStringLiteral("Code.exe")});
        QGuiApplication::clipboard()->setText(QStringLiteral("first draft"));
        c.showCard();
        ROCLINT_CHECK(!c.hasHostContext());
        ROCLINT_CHECK(c.capturedDraftText().isEmpty());
        c.hideCard();
        QGuiApplication::clipboard()->clear();
        c.showCard();
        ROCLINT_CHECK(c.capturedDraftText().isEmpty());
        ROCLINT_CHECK(c.sessionCount() == 0);
    }
    HostContextDetector::resetMock();
    {
        HostController restored(QStringLiteral(":memory:"));
    ROCLINT_CHECK(restored.waitForStorageForTesting());
        restored.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(!restored.hostContextEnabled());
        ROCLINT_CHECK(restored.rewriteProfile() == "OPENAI_STRUCTURED@1");
        ROCLINT_CHECK(restored.activeModel() == "native-granite:Q8_0");
        ROCLINT_CHECK(restored.nativeGraniteSelected());
        ROCLINT_CHECK(restored.nativeGraniteResidency() == "UNLOADED");
        ROCLINT_CHECK(restored.modelRuntimeState() == "UNLOADED");
        ROCLINT_CHECK(!restored.isDownloadingModel() && !restored.isInferring());
    }
    QSettings().clear();
    ROCLINT_CHECK(GlobalShortcutManager::normalizeShortcut("Ctrl+Shift+L") == "Ctrl+Shift+L");
    ROCLINT_CHECK(!GlobalShortcutManager::normalizeShortcut("Alt+F9").isEmpty());
    ROCLINT_CHECK(GlobalShortcutManager::normalizeShortcut("L").isEmpty());
    ROCLINT_CHECK(GlobalShortcutManager::normalizeShortcut("Shift+L").isEmpty());
    ROCLINT_CHECK(GlobalShortcutManager::normalizeShortcut("Ctrl+L, Ctrl+K").isEmpty());
    std::cout << "  [PASS] Empty capture, host-hint opt-out, sticky profile, and shortcut validation.\n";
}

void testHintChangeRejectsInFlightResult() {
    std::atomic<int> calls{0};
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    prepare(c, calls, 80);
    HostContextDetector::setMockObservation(HostContextObservation{
        HostContextCategory::CodeEditor, QStringLiteral("VS Code"), QStringLiteral("Code.exe")});
    c.showCard();
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(QString::fromStdString(fx::kChainPrompt), c.stateVersion())));
    c.clearHostContext();
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(!c.analysisForTesting().has_value());
    const auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value()[0].status == roclint::storage::AttemptStatus::DiscardedStale);
    ROCLINT_CHECK(attempts.value()[0].modelDetail.find("host_context_category=CODE_EDITOR") != std::string::npos);
    HostContextDetector::resetMock();
    std::cout << "  [PASS] Clearing a hint discards a pending result and retains attempt configuration.\n";
}

void testModelSwitchDuringInference() {
    std::atomic<int> calls{0};
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    prepare(c, calls, 120);
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(QUrl(QStringLiteral("http://127.0.0.1:1"))));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(QString::fromStdString(fx::kChainPrompt), c.stateVersion())));
    for (int n = 0; n < 500 && calls == 0; ++n) QTest::qWait(1);
    ROCLINT_CHECK(calls == 1 && c.modelRuntimeState() == "BUSY");
    c.setActiveModel(QStringLiteral("different-model"));
    ROCLINT_CHECK(c.modelRuntimeState() == "BUSY");
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    ROCLINT_CHECK(!c.analysisForTesting().has_value());
    ROCLINT_CHECK(c.modelRuntimeState() == "FAILED"); // the new model was actually probed, not assumed ready
    const auto attempts = c.repositoryForTesting()->getModelAttempts(SessionId(c.currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value()[0].status == roclint::storage::AttemptStatus::DiscardedStale);
    ROCLINT_CHECK(attempts.value()[0].modelId == fx::testModel().modelId);
    QSettings().clear();
    std::cout << "  [PASS] Switching a busy model discards its result and rechecks the new identity.\n";
}

void testKeyboardWorkflow() {
    QSettings().clear();
    HostContextDetector::setMockObservation(HostContextObservation{
        HostContextCategory::CodeEditor, QStringLiteral("VS Code"), QStringLiteral("Code.exe")});
    std::atomic<int> calls{0};
    HostController c(QStringLiteral(":memory:"));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    prepare(c, calls);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &c);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window != nullptr);
    auto* editor = window->findChild<QQuickItem*>(QStringLiteral("promptInput"));
    auto* choices = window->findChild<QQuickItem*>(QStringLiteral("clarifyPanel"));
    ROCLINT_CHECK(editor != nullptr && choices != nullptr);
    QObject::connect(&c, &HostController::hideCardRequested, window, &QQuickWindow::hide);
    QGuiApplication::clipboard()->setText(QString::fromStdString(fx::kChainPrompt));
    c.showCard();
    window->show();
    window->requestActivate();
    ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    auto* settingsControl = window->findChild<QQuickItem*>(QStringLiteral("openSettingsControl"));
    ROCLINT_CHECK(settingsControl != nullptr);
    settingsControl->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    ROCLINT_CHECK(c.isDashboardVisible());
    c.hideDashboard();
    editor->forceActiveFocus();
    QTest::qWait(750);
    ROCLINT_CHECK(calls == 0); // capture is editable before the first model request
    ROCLINT_CHECK(editor->property("text").toString().isEmpty());
    QTest::keyClick(window, Qt::Key_1);
    ROCLINT_CHECK(editor->property("text").toString().contains('1'));
    const QString clipboardBeforeTyping = QGuiApplication::clipboard()->text();
    QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
    ROCLINT_CHECK(editor->property("text").toString().contains('\n'));
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == clipboardBeforeTyping);
    editor->setProperty("text", QString::fromStdString(fx::kChainPrompt));

    QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.isClarifying());
    ROCLINT_CHECK(c.roundsCompleted() == 0);
    // The final chord redirects to unresolved choices; it never selects the first alternative.
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    ROCLINT_CHECK(c.roundsCompleted() == 0 && !c.hasConfirmedIntent());
    ROCLINT_CHECK(choices->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_1);
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.hasConfirmedIntent());
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == "untouched");

    // The final explicit choice starts the preview without a second Analyze/rewrite command.
    for (int i = 0; i < 300 && c.proposalStatus() == "NONE"; ++i) QTest::qWait(10);
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.hasProposal());
    ROCLINT_CHECK(c.canApprove());
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == "untouched");
    savePreview(window, QStringLiteral("review-default.png"));
    window->resize(560, 400);
    // Geometry and polish propagate asynchronously through the retained upstream shell.
    QTest::qWait(100);
    savePreview(window, QStringLiteral("review-compact.png"));
    auto* review = window->findChild<QQuickItem*>(QStringLiteral("reviewPanel"));
    ROCLINT_CHECK(review != nullptr && review->width() <= window->width() - 28);
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.hasApproval());
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == kRewritten);
    ROCLINT_CHECK(c.approvalLabel() == "Wording reviewed");

    // A model/configuration change revokes an approval immediately.
    ROCLINT_CHECK(c.setModelRuntimeUrlForTesting(QUrl(QStringLiteral("http://127.0.0.1:1"))));
    c.setActiveModel(QStringLiteral("another-installed-model"));
    ROCLINT_CHECK(!c.hasApproval() && !c.hasProposal());
    ROCLINT_CHECK(c.waitForIdleForTesting(15000));
    std::cout << "  [PASS] Real QML keyboard flow preserves typing, clarification and explicit approval.\n";

    engine.load(QUrl(QStringLiteral("qrc:/shell/DashboardWindow.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 2);
    auto* settings = qobject_cast<QQuickWindow*>(engine.rootObjects().last());
    ROCLINT_CHECK(settings != nullptr);
    settings->setProperty("currentTab", 1);
    settings->show();
    savePreview(settings, QStringLiteral("settings.png"));
    HostContextDetector::resetMock();
}
void testReviewRequiresExplicitActivation() {
    QSettings().clear();
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    controller.setTransportOverrideForTesting([](std::string_view prompt) -> Result<std::string> {
        return prompt.find("<instructions>") == std::string_view::npos ? kDiscovery : fx::rewriteReply("Make the parser quicker.");
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(QString::fromStdString(fx::kChainPrompt), 0)));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice(QStringLiteral("alt_0_0"), controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.pendingReviewCount() == 1);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window != nullptr);
    window->show();
    std::function<QQuickItem*(QQuickItem*)> findCheck = [&](QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == "constraintReview_intent_item_grp_0") return item;
        for (auto* child : item->childItems()) if (auto* found = findCheck(child)) return found;
        return nullptr;
    };
    QTest::qWait(50);
    auto* check = findCheck(window->contentItem());
    ROCLINT_CHECK(check != nullptr);
    const auto observed = controller.stateVersion();
    ROCLINT_CHECK(check->setProperty("checked", true));
    QTest::qWait(10);
    ROCLINT_CHECK(controller.pendingReviewCount() == 1 && controller.stateVersion() == observed);
    ROCLINT_CHECK(check->setProperty("checked", false));
    check->forceActiveFocus(Qt::TabFocusReason);
    QTest::qWait(60);
    QTest::keyClick(window, Qt::Key_Space);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.pendingReviewCount() == 0 && controller.stateVersion() == observed + 1);
    ROCLINT_CHECK(controller.canApprove() && !controller.hasApproval());
    ROCLINT_CHECK(controller.lastErrorCode().isEmpty());
    std::cout << "  [PASS] Programmatic checkbox changes do not acknowledge; keyboard activation does.\n";
}

void testResidencySettingsKeyboard() {
    QSettings().clear();
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/DashboardWindow.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window != nullptr);
    window->show(); window->requestActivate();
    ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    QTest::qWait(50);
    std::function<QQuickItem*(QQuickItem*, const QString&)> find = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
        if (item->objectName() == name) return item;
        for (auto* child : item->childItems()) if (auto* found = find(child, name)) return found;
        return nullptr;
    };
    auto* lowMemory = find(window->contentItem(), QStringLiteral("graniteResidency_LOW_MEMORY"));
    auto* keepWarm = find(window->contentItem(), QStringLiteral("graniteResidency_KEEP_WARM"));
    ROCLINT_CHECK(lowMemory && keepWarm && lowMemory->isVisible());
    lowMemory->forceActiveFocus(); QTest::keyClick(window, Qt::Key_Space);
    ROCLINT_CHECK(controller.graniteResidencyPreset() == "LOW_MEMORY");
    keepWarm->forceActiveFocus(); QTest::keyClick(window, Qt::Key_Return);
    ROCLINT_CHECK(controller.graniteResidencyPreset() == "KEEP_WARM");
    ROCLINT_CHECK(controller.modelRuntimeState() == "UNLOADED" && controller.stateVersion() == 0 && controller.sessionCount() == 0);
    savePreview(window, QStringLiteral("runtime-residency.png"));
}
void testOverBudgetAssumptionsRemainVisibleAndUnconfirmed() {
    QSettings().clear();
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    controller.setScorerEngine("jev");
    controller.s1AdapterForTesting().setMockCloudScorer([](const analysis::NormalizedAnalysis& input) -> Result<roclint::bridge::SnapshotEvaluationResponse> {
        auto response = roclint::bridge::S1TriageAdapter::degradedResponse(input, "");
        response.modelVersion = "fixture_assumption_scorer@1";
        for (std::size_t index = 0; index < 4; ++index) {
            auto& score = response.assumptionScores[index];
            score.isScored = true;
            score.uncertaintyScore = 0.2 + 0.1 * static_cast<double>(index);
            score.unscoredReason.clear();
        }
        return response;
    });
    controller.setTransportOverrideForTesting([](std::string_view) -> Result<std::string> {
        auto response = json::Value::object();
        response.set("status", json::Value::string("no_ambiguity"));
        auto assumptions = json::Value::array();
        for (int index = 0; index < 5; ++index) {
            auto item = json::Value::object();
            item.set("statement", json::Value::string("Material premise " + std::to_string(index) + " <b>literal text</b>"));
            item.set("material", json::Value::boolean(true));
            auto evidence = json::Value::array();
            evidence.push(json::Value::string("service"));
            item.set("evidence", std::move(evidence));
            assumptions.push(std::move(item));
        }
        response.set("assumptions", std::move(assumptions));
        return json::serialize(response);
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt("The service needs tests.", 0)));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.isManual() && !controller.hasConfirmedIntent());
    ROCLINT_CHECK(controller.assumptionReviews().size() == 5);
    ROCLINT_CHECK(controller.assumptionReviews()[0].toMap().value("id").toString() == "asm_3");
    ROCLINT_CHECK(controller.finalSnapshotForTesting()->assumptionOrder.front() == "asm_3");
    ROCLINT_CHECK(controller.assumptionReviews()[4].toMap().value("scoring").toString().contains("four-assumption batch limit"));
    ROCLINT_CHECK(controller.workingIntentForTesting().assumptionDecisions.empty());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window != nullptr);
    window->show();
    QTest::qWait(50);
    std::function<QQuickItem*(QQuickItem*)> findLast = [&](QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == "assumptionReview_asm_4") return item;
        for (auto* child : item->childItems()) if (auto* found = findLast(child)) return found;
        return nullptr;
    };
    auto* last = findLast(window->contentItem());
    ROCLINT_CHECK(last && last->isVisible());
    ROCLINT_CHECK(last->property("text").toString().contains("Material premise 4"));
    ROCLINT_CHECK(last->property("text").toString().contains("<b>literal text</b>"));
    ROCLINT_CHECK(last->property("textFormat").toInt() == 0); // PlainText, including model-supplied markup.
    ROCLINT_CHECK(last->property("text").toString().contains("Unscored: four-assumption batch limit"));
    ROCLINT_CHECK(!controller.isRewriteEnabled());
    std::cout << "  [PASS] Over-budget material assumptions remain visible, unscored and unconfirmed.\n";
}
void testLiveCapturedDraftAndCancelRetryInQml() {
    QSettings().clear();
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    observed->accessValue = AccessibilityAccess::Granted;
    HostController controller(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    std::atomic<int> calls{0};
    prepare(controller, calls, 180);
    controller.setAmbientModeEnabled(true);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* editor = window->findChild<QQuickItem*>(QStringLiteral("promptInput"));
    auto* action = window->findChild<QObject*>(QStringLiteral("analysisActionLabel"));
    ROCLINT_CHECK(editor && action);
    ObservedField field;
    field.fieldToken = 77; field.processId = 444;
    field.appIdentifier = QStringLiteral("ChatGPT.exe"); field.appDisplayName = QStringLiteral("Codex");
    field.kind = FieldKind::GuiEditor;
    observed->observe(field);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.openFromAmbient()));
    window->show();
    window->requestActivate();
    ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    QTest::qWait(150);
    field.text = QString::fromStdString(fx::kChainPrompt);
    observed->observe(field);
    ROCLINT_CHECK(editor->property("text").toString() == field.text && calls == 0);
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "analyzeNow"));
    QTRY_VERIFY_WITH_TIMEOUT(controller.isAnalyzing(), 3000);
    ROCLINT_CHECK(action->property("text").toString().contains(QStringLiteral("CANCEL ANALYSIS")));
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "handleEnter"));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(!controller.isAnalyzing());
    ROCLINT_CHECK(controller.analysisStatus() == "IDLE" && !controller.hasProposal());
    ROCLINT_CHECK(editor->property("text").toString() == field.text);
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "analyzeNow"));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.analysisStatus() == "READY");
    const auto frozen = controller.currentRevisionText();
    field.text += QStringLiteral(" Keep the existing interface.");
    observed->observe(field);
    ROCLINT_CHECK(editor->property("text").toString() == field.text);
    ROCLINT_CHECK(controller.currentRevisionText() == frozen && controller.isDraftDirty());
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_A);
    const auto localDraft = editor->property("text").toString();
    ROCLINT_CHECK(localDraft != field.text); // A captured draft is editable through real keyboard input.
    field.text += QStringLiteral(" External typing continues.");
    observed->observe(field);
    ROCLINT_CHECK(editor->property("text").toString() == localDraft);
}

void testManualGraniteRewriteConnectsEditorWithoutLosingReview() {
    QSettings().clear();
    HostContextDetector::setMockObservation(HostContextObservation{});
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController controller(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    controller.setAmbientModeEnabled(true);
    int modelCalls = 0;
    controller.setTransportOverrideForTesting([&](std::string_view request) -> Result<std::string> {
        ++modelCalls;
        if (request.find("<instructions>") != std::string_view::npos) return fx::rewriteReply(kRewritten.toStdString());
        return std::string(R"({"status":"options_generated","choice_groups":[],"assumptions":[],"missing_decisions":[{"question":"Which performance goal should take priority?","blocking":true}],"conflicts":[],"clarification_question":""})");
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window);
    QObject::connect(&controller, &HostController::hideCardRequested, window, &QQuickWindow::hide);
    QObject::connect(&controller, &HostController::showCardRequested, window, [window]() { window->show(); window->requestActivate(); });
    controller.showCard();
    window->show(); window->requestActivate();
    ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    const auto original = QString::fromStdString(fx::kChainPrompt);
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "setEditorText", Q_ARG(QVariant, original)));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(original, 0)));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.analysisStatus() == "READY" && controller.activeItemId() == "mdc_0");
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.submitCustomIntent(QStringLiteral("Prefer lower latency per call."), controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.hasProposal() && controller.pendingReviewCount() > 0 && !controller.canApprove());
    const auto version = controller.stateVersion();
    const auto proposalHash = controller.proposalForTesting()->proposalHash;
    const auto intentHash = controller.confirmedIntentForTesting()->intentHash;
    const auto pending = controller.pendingReviewCount();
    const auto calls = modelCalls;
    QGuiApplication::clipboard()->setText(QStringLiteral("Leave clipboard unchanged"));
    QTest::qWait(40);
    auto* connect = window->findChild<QQuickItem*>(QStringLiteral("connectRewriteEditorControl"));
    auto* approve = window->findChild<QQuickItem*>(QStringLiteral("approveRewriteControl"));
    ROCLINT_CHECK(connect && approve && connect->isVisible());
    ROCLINT_CHECK(approve->property("label").toString() == "Approve & Copy");
    connect->forceActiveFocus(); QTest::qWait(75);
    savePreview(window, QStringLiteral("manual-rewrite-connect.png"));
    QTest::keyClick(window, Qt::Key_Space);
    ROCLINT_CHECK(!controller.isCardVisible() && !controller.hasApproval());
    ObservedField field;
    field.fieldToken = 91; field.processId = 4242; field.kind = FieldKind::GuiEditor;
    field.appIdentifier = QStringLiteral("ChatGPT.exe"); field.appDisplayName = QStringLiteral("Codex");
    field.text = QStringLiteral("Existing unsent Codex draft");
    observed->observe(field);
    HostContextDetector::setMockObservation(HostContextObservation{HostContextCategory::CodeEditor, field.appDisplayName, field.appIdentifier});
    controller.toggleCard();
    QTest::qWait(40);
    ROCLINT_CHECK(controller.isAmbientCapture() && controller.isCardVisible());
    ROCLINT_CHECK(approve->property("label").toString() == "Approve & Insert");
    ROCLINT_CHECK(controller.stateVersion() == version && controller.pendingReviewCount() == pending && !controller.canApprove());
    ROCLINT_CHECK(controller.proposalForTesting()->proposalHash == proposalHash && controller.confirmedIntentForTesting()->intentHash == intentHash);
    ROCLINT_CHECK(controller.currentRevisionText() == original && modelCalls == calls && observed->injections.empty());
    savePreview(window, QStringLiteral("manual-rewrite-connected.png"));
    std::function<QQuickItem*(QQuickItem*)> findReview = [&](QQuickItem* item) -> QQuickItem* {
        if (item->objectName().startsWith(QStringLiteral("constraintReview_")) && item->isVisible()) return item;
        for (auto* child : item->childItems()) if (auto* found = findReview(child)) return found;
        return nullptr;
    };
    auto* check = findReview(window->contentItem());
    ROCLINT_CHECK(check && !check->property("checked").toBool());
    check->forceActiveFocus(); QTest::keyClick(window, Qt::Key_Space);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.canApprove());
    approve->forceActiveFocus(); QTest::keyClick(window, Qt::Key_Space);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    QTest::qWait(30);
    ROCLINT_CHECK(controller.hasApproval() && observed->injections.size() == 1);
    ROCLINT_CHECK(observed->injections[0].text == kRewritten && observed->injections[0].expectedCurrentText == field.text);
    ROCLINT_CHECK(QGuiApplication::clipboard()->text() == "Leave clipboard unchanged" && modelCalls == calls);
    const auto savedSession = controller.currentSessionIdString();
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.restoreSessionForReview(savedSession, controller.stateVersion())));
    ROCLINT_CHECK(controller.isCardVisible() && !controller.isAmbientCapture());
    ROCLINT_CHECK(controller.proposalForTesting()->proposalHash == proposalHash && modelCalls == calls);
    ROCLINT_CHECK(approve->property("label").toString() == "Approve & Copy");
    HostContextDetector::resetMock();
}

void testClipboardFallbackDoesNotDismissQmlCard() {
    QSettings().clear();
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* modelLabel = window->findChild<QObject*>(QStringLiteral("modelRuntimeLabel"));
    const auto labelText = modelLabel ? modelLabel->property("text").toString() : QString();
    ROCLINT_CHECK(modelLabel && (labelText == "[ MODEL NOT LOADED ]" || labelText == "[ READY · STANDBY ]"));
    ROCLINT_CHECK(window);
    controller.showCard();
    window->show();
    controller.promptCopied(QStringLiteral("recoverable draft"));
    QTest::qWait(50);
    ROCLINT_CHECK(controller.isCardVisible());
    ROCLINT_CHECK(window->isVisible());
    engine.load(QUrl(QStringLiteral("qrc:/shell/DashboardWindow.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 2);
    auto* settings = engine.rootObjects().last();
    auto* captureStatus = settings->findChild<QObject*>(QStringLiteral("ambientCaptureStatus"));
    auto* captureToggle = settings->findChild<QObject*>(QStringLiteral("ambientCaptureToggle"));
    ROCLINT_CHECK(captureStatus && captureToggle);
    controller.setAmbientModeEnabled(true);
    QTest::qWait(25);
    ROCLINT_CHECK(captureToggle->property("checked").toBool());
    ROCLINT_CHECK(captureStatus->property("text").toString() == "[STATUS: ENABLED]");
    controller.setAmbientModeEnabled(false);
    QTest::qWait(25);
    ROCLINT_CHECK(!captureToggle->property("checked").toBool());
    ROCLINT_CHECK(captureStatus->property("text").toString() == "[STATUS: DISABLED]");
    std::cout << "  [PASS] Clipboard recovery leaves the actual QML card open.\n";
}

void testManualRemainderCanFinishAndGeneratePreview() {
    QSettings().clear();
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    int rewriteCalls = 0;
    const std::string discovery = R"({"status":"options_generated","choice_groups":[
      {"dimension":"speed","question":"Faster how?","material":true,"alternatives":[
       {"label":"Lower latency","why_material":"Single request speed","evidence":["faster"]},
       {"label":"Higher throughput","why_material":"Batch speed","evidence":["faster"]}]},
      {"dimension":"size","question":"Smaller how?","material":true,"alternatives":[
       {"label":"Binary size","why_material":"Disk use","evidence":["smaller"]},
       {"label":"Memory use","why_material":"Runtime use","evidence":["smaller"]}]},
      {"dimension":"safety","question":"Safer how?","material":true,"alternatives":[
       {"label":"Memory safety","why_material":"Runtime safety","evidence":["safer"]},
       {"label":"Input validation","why_material":"Input safety","evidence":["safer"]}]}],
      "assumptions":[],"missing_decisions":[{"question":"Which interpretation should be final?","blocking":true}],"conflicts":[],"clarification_question":""})";
    controller.setTransportOverrideForTesting([&](std::string_view prompt) -> Result<std::string> {
        if (prompt.find("<instructions>") != std::string_view::npos) {
            ++rewriteCalls;
            return fx::rewriteReply("Reduce latency, binary size, and memory errors.");
        }
        return discovery;
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window);
    window->show(); window->requestActivate();
    const QString original = QStringLiteral("Make it faster, smaller, and safer.");
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(original, 0)));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    for (int i = 0; i < 3; ++i) {
        ROCLINT_CHECK(controller.isClarifying());
        const auto id = controller.activeAlternatives().first().toMap().value("id").toString();
        ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice(id, controller.stateVersion())));
    }
    ROCLINT_CHECK(controller.isManual());
    ROCLINT_CHECK(rewriteCalls == 0);
    ROCLINT_CHECK(window->property("needsManualAnswer").toBool());
    const auto beforeManualFocus = controller.stateVersion();
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "enterAction"));
    QTest::qWait(25);
    ROCLINT_CHECK(controller.stateVersion() == beforeManualFocus && controller.roundsCompleted() == 3);
    QTest::qWait(25);
    auto* custom = window->findChild<QQuickItem*>(QStringLiteral("customAnswer"));
    ROCLINT_CHECK(custom && custom->isVisible());
    custom->setProperty("text", QStringLiteral("Use the three interpretations I confirmed above."));
    custom->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    for (int i = 0; i < 300 && rewriteCalls == 0; ++i) QTest::qWait(10);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(rewriteCalls == 1 && controller.hasConfirmedIntent());
    ROCLINT_CHECK(controller.roundsCompleted() == 3);
    ROCLINT_CHECK(controller.currentRevisionText() == original);
    ROCLINT_CHECK(!controller.hasApproval());
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.restoreSession(controller.currentSessionIdString(), controller.stateVersion())));
    ROCLINT_CHECK(controller.hasConfirmedIntent() && controller.roundsCompleted() == 3);
    ROCLINT_CHECK(rewriteCalls == 1); // Restoring completed history never generates a new preview.
    std::cout << "  [PASS] The visible manual remainder completes intent and starts one reviewable rewrite.\n";
}

void testForbiddenColorOptionIsBlockedInUiAndController() {
    QSettings().clear();
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    controller.setTransportOverrideForTesting([](std::string_view) -> Result<std::string> {
        return std::string(R"({"status":"options_generated","choice_groups":[{"dimension":"color","question":"Which dog color?","material":true,"alternatives":[
          {"label":"Black dog","why_material":"Required color","evidence":["black"]},
          {"label":"Blue dog","why_material":"Invalid generated reversal","evidence":["blue"]}]}],
          "assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""})");
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(QStringLiteral("The dog will be black but it cant be blue."), 0)));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.activeAlternatives().size() == 2);
    ROCLINT_CHECK(controller.activeAlternatives()[0].toMap().value("permitted").toBool());
    ROCLINT_CHECK(!controller.activeAlternatives()[1].toMap().value("permitted").toBool());
    const auto version = controller.stateVersion();
    ROCLINT_CHECK(!controller.waitForCommandForTesting(controller.confirmChoice(QStringLiteral("alt_0_1"), version)));
    ROCLINT_CHECK(controller.stateVersion() == version && controller.roundsCompleted() == 0);
    ROCLINT_CHECK(controller.lastError().contains(QStringLiteral("color requirement")));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window);
    window->show();
    QTest::qWait(25);
    std::function<QQuickItem*(QQuickItem*)> findForbidden = [&](QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == "alternative1") return item;
        for (auto* child : item->childItems()) if (auto* found = findForbidden(child)) return found;
        return nullptr;
    };
    auto* forbidden = findForbidden(window->contentItem());
    ROCLINT_CHECK(forbidden && !forbidden->isEnabled());
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice(QStringLiteral("alt_0_0"), version)));
    ROCLINT_CHECK(controller.hasConfirmedIntent());
}
} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
#ifdef Q_OS_WIN
    // The offscreen plugin does not enumerate Windows fonts like the normal desktop plugin.
    const QString fonts = qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/");
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("segoeui.ttf"));
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("consola.ttf"));
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("seguisym.ttf"));
    QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    QTemporaryDir settingsDirectory;
    ROCLINT_CHECK(settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("RoclintAlignmentTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ProductAlignment"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    testNativeGlobalShortcutDispatch();
    testCaptureAndSettings();
    testHintChangeRejectsInFlightResult();
    testModelSwitchDuringInference();
    testKeyboardWorkflow();
    testReviewRequiresExplicitActivation();
    testOverBudgetAssumptionsRemainVisibleAndUnconfirmed();
    testResidencySettingsKeyboard();
    testClipboardFallbackDoesNotDismissQmlCard();
    testManualGraniteRewriteConnectsEditorWithoutLosingReview();
    testLiveCapturedDraftAndCancelRetryInQml();
    testManualRemainderCanFinishAndGeneratePreview();
    testForbiddenColorOptionIsBlockedInUiAndController();
    std::cout << "[Product Alignment] All tests PASSED.\n";
    return 0;
}
