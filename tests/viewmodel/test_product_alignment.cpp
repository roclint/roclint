#include "GlobalShortcutManager.hpp"
#include "HostContext.hpp"
#include "HostController.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include "support/MockAccessibilityBridge.hpp"
#include "roclint/storage/PayloadCodec.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextDocument>
#include "roclint/storage/Database.hpp"
#include <QtTest/QTest>

#include <atomic>
#include <algorithm>
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
        if (prompt.find("granite.goldfish_audit.v1") != std::string_view::npos) {
            return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
        }
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
    // Geometry may already be correct while the renderer still has the previous
    // resized frame. Capture only after two complete scene-graph frames.
    QTest::qWait(300);
    for (int frame = 0; frame < 2; ++frame) {
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
        window->update();
        ROCLINT_CHECK(!rendered.isEmpty() || rendered.wait(2000));
    }
    ROCLINT_CHECK(window->grabWindow().save(directory + QLatin1Char('/') + name));
}

QQuickItem* findVisual(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems()) if (auto* found = findVisual(child, name)) return found;
    return nullptr;
}

bool fullyInViewport(QQuickWindow* window, QQuickItem* item) {
    if (!item || !item->isVisible() || item->width() <= 0 || item->height() <= 0) return false;
    const auto rect = item->mapRectToItem(window->contentItem(), item->boundingRect());
    auto visible = QRectF(0, 0, window->width(), window->height());
    for (auto* parent = item->parentItem(); parent; parent = parent->parentItem())
        if (parent->clip()) visible = visible.intersected(parent->mapRectToItem(window->contentItem(), parent->boundingRect()));
    return visible.adjusted(-1, -1, 1, 1).contains(rect);
}

void seedOptionalReview(HostController& c) {
    namespace an = roclint::core::analysis;
    const std::string prompt = "Make the homepage cool.";
    ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(QString::fromStdString(prompt), 0)));
    const auto session = c.currentSessionIdString().toStdString();
    const auto revision = c.currentRevisionId().toStdString();
    auto normalized = fx::AnalysisBuilder(session, revision, prompt)
        .group("Explore a visual direction?", {"Bold and playful", "Calm and minimal"}, "cool").build();
    an::PromptTriage triage;
    triage.sessionId = session; triage.revisionId = revision;
    triage.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    triage.stateVersion = c.stateVersion(); triage.requestGroupId = "group_ui"; triage.attemptId = "triage_ui";
    triage.modelId = "fixture"; triage.modelRevision = "fixture"; triage.weightsSha256 = std::string(64, 'a');
    triage.configurationId = "fixture"; triage.execution = an::TriageExecution::Mock; triage.categoryScores = {.2,.1,.1,.1};
    const auto sealed = an::sealPromptTriage(triage, prompt); ROCLINT_CHECK(sealed.ok());
    triage = sealed.value();
    auto* repo = c.repositoryForTesting();
    roclint::storage::ModelAttemptRecord classifier{triage.attemptId, session, revision, "julia_triage"};
    classifier.modelId = triage.modelId; classifier.transport = "fixture";
    ROCLINT_CHECK(repo->beginModelAttempt(classifier).waitForTesting().ok());
    ROCLINT_CHECK(repo->completeModelAttempt(classifier.attemptId, roclint::storage::AttemptStatus::Succeeded,
        triage.modelId, "fixture; prompt_triage_payload=" + roclint::storage::encodePromptTriage(triage), "", 2).waitForTesting().ok());
    roclint::storage::ModelAttemptRecord discovery{"discovery_ui", session, revision, "discovery"};
    discovery.transport = "fixture";
    ROCLINT_CHECK(repo->beginModelAttempt(discovery).waitForTesting().ok());
    ROCLINT_CHECK(repo->completeModelAttempt(discovery.attemptId, roclint::storage::AttemptStatus::Succeeded,
        "fixture", "fixture; review_trigger=explicit_review", "", 3).waitForTesting().ok());
    const auto snapshot = an::buildRoutedAnalysisSnapshot(normalized, triage, prompt, discovery.attemptId, std::nullopt,
        an::ReviewPolicy{an::ReviewTrigger::ExplicitReview, an::AssistanceKind::Creative});
    ROCLINT_CHECK(snapshot.ok());
    ROCLINT_CHECK(repo->insertAnalysisSnapshot(snapshot.value(), 4).waitForTesting().ok());
    ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(QString::fromStdString(session), c.stateVersion())));
    ROCLINT_CHECK(c.isOptionalHelp() && c.isClarifying() && c.finalSnapshotForTesting()->payloadVersion == 5);
}

void testOptionalPresentationFromPersistedV5() {
    QSettings().clear();
    auto bridge=std::make_unique<roclint::test::MockAccessibilityBridge>(); auto* observed=bridge.get();
    observed->accessValue=AccessibilityAccess::Granted;
    HostController c(":memory:",std::move(bridge)); ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setAmbientModeEnabled(false); seedOptionalReview(c);
    c.setAmbientModeEnabled(true);
    ObservedField field; field.fieldToken=903; field.processId=444; field.kind=FieldKind::GuiEditor;
    field.appIdentifier="ChatGPT.exe"; field.appDisplayName="Codex"; field.text=c.currentRevisionText(); observed->observe(field);
    HostContextDetector::setMockObservation(HostContextObservation{HostContextCategory::CodeEditor,field.appDisplayName,field.appIdentifier});
    QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("hostController", &c);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml")); ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    c.showCard(); window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(QMetaObject::invokeMethod(window,"toggleDetailedExpanded")); QTest::qWait(50);
    auto* badge = window->findChild<QObject*>("hudStateLabel");
    auto* action = window->findChild<QObject*>("analysisActionLabel");
    auto* card = findVisual(window->contentItem(), "clarifyPanel");
    auto* state = window->findChild<QObject*>("hudStateBadge");
    ROCLINT_CHECK(badge && badge->property("text").toString() == "OPTIONAL");
    ROCLINT_CHECK(action && action->property("text").toString() == "Optional ideas");
    ROCLINT_CHECK(card && state);
    ROCLINT_CHECK(window->findChild<QObject*>("intentReviewPanel")->property("questionTone") == window->property("borderColor"));
    ROCLINT_CHECK(!c.hasConfirmedIntent() && !c.hasProposal() && !c.hasApproval());
    savePreview(window, "optional-ideas.png");
    auto* details = window->findChild<QObject*>("intentReviewPanel"); details->setProperty("showDiagnostics", true);
    QTest::qWait(20); ROCLINT_CHECK(badge->property("text").toString() == "OPTIONAL");
    ROCLINT_CHECK(c.currentRevisionText() == "Make the homepage cool.");
    details->setProperty("showDiagnostics", false);
    c.setTransportOverrideForTesting([](std::string_view) -> Result<std::string> { return fx::rewriteReply("Build a bold and playful homepage."); });
    ROCLINT_CHECK(c.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice("alt_0_0",c.stateVersion())));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(badge->property("text").toString()=="REVIEW" && !c.hasApproval());
    HostContextDetector::resetMock();
    std::cout << "  [PASS] Persisted v5 optional help has neutral, consistent presentation without inferred choices.\n";
}

void testReviewGeometryAndExplicitExpansion() {
    struct Size { int width; int height; bool ambient; };
    for (const auto size : {Size{820,520,false}, Size{560,400,false}, Size{620,250,true}, Size{420,200,true}}) {
        QSettings().clear();
        auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>(); auto* observed = bridge.get();
        observed->accessValue = AccessibilityAccess::Granted;
        HostController c(":memory:", std::move(bridge)); ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setAmbientModeEnabled(size.ambient);
        c.setLegacyDiscoveryForTesting(true);
        const std::string discovery = R"({"status":"options_generated","choice_groups":[
            {"question":"Which speed?","alternatives":[{"label":"Latency","why_material":"Speed","evidence":["runs faster"]},{"label":"Throughput","why_material":"Speed","evidence":["runs faster"]}]},
            {"question":"Which docs?","alternatives":[{"label":"README","why_material":"Scope","evidence":["where needed"]},{"label":"All docs","why_material":"Scope","evidence":["where needed"]}]},
            {"question":"Which cleanup?","alternatives":[{"label":"Names","why_material":"Scope","evidence":["clean up the tests"]},{"label":"Dead tests","why_material":"Scope","evidence":["clean up the tests"]}]}]})";
        std::atomic<int> calls{0};
        std::string rewritten = "Refactor the parser to be quicker, improve documentation, and tidy tests.";
        if (size.width == 560) for (int i=0; i<80; ++i) rewritten += "\nA long synthetic diff line that wraps across the available review width.";
        c.setTransportOverrideForTesting([&](std::string_view prompt) -> Result<std::string> {
            ++calls; return prompt.find("<instructions>") == std::string_view::npos ? discovery : fx::rewriteReply(rewritten);
        });
        ROCLINT_CHECK(c.markModelReadyForTesting(fx::testModel()));
        ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(QString::fromStdString(fx::kThreeGroupPrompt), 0)));
        ROCLINT_CHECK(c.waitForIdleForTesting(3000));
        for (int i=0; i<3; ++i) {
            const QString answer = QStringLiteral("Keep this explicitly supplied answer with enough descriptive wording to wrap in a narrow review: choice %1 remains the user's decision.").arg(i);
            ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent(answer, c.stateVersion())));
        }
        ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
        ROCLINT_CHECK(c.waitForIdleForTesting(3000));
        ROCLINT_CHECK(c.pendingReviewCount() == 3);
        if (size.ambient) {
            c.setAmbientModeEnabled(true);
            ObservedField field; field.fieldToken=902; field.processId=444; field.kind=FieldKind::GuiEditor;
            field.appIdentifier="ChatGPT.exe"; field.appDisplayName="Codex"; field.text=QString::fromStdString(fx::kThreeGroupPrompt);
            observed->observe(field);
            HostContextDetector::setMockObservation(HostContextObservation{HostContextCategory::CodeEditor, field.appDisplayName, field.appIdentifier});
        }
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("hostController", &c);
        engine.load(QUrl("qrc:/shell/FloatingCard.qml")); ROCLINT_CHECK(engine.rootObjects().size() == 1);
        auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first()); c.showCard(); window->show();
        ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
        window->resize(size.width,size.height); QTest::qWait(80);
        if (size.ambient) {
            ROCLINT_CHECK(!window->property("isDetailedExpanded").toBool());
            ROCLINT_CHECK(c.waitForCommandForTesting(c.discardProposal(c.stateVersion())));
            ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
            ROCLINT_CHECK(c.waitForIdleForTesting(3000));
            ROCLINT_CHECK(!window->property("isDetailedExpanded").toBool()); // Real result arrival cannot expand.
        }
        window->requestActivate(); QTest::qWait(50);
        const int modelCalls=calls;
        auto* primary=window->findChild<QQuickItem*>("approveRewriteControl"); ROCLINT_CHECK(primary && fullyInViewport(window,primary));
        for (int index=0; index<3; ++index) {
            primary->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window,Qt::Key_Space); QTest::qWait(100);
            if (size.ambient) ROCLINT_CHECK(window->property("isDetailedExpanded").toBool() && window->width()==820 && window->height()==520);
            const auto suffix=QStringLiteral("intent_item_grp_%1").arg(index);
            auto* check=findVisual(window->contentItem(),"constraintReview_"+suffix);
            auto* label=findVisual(window->contentItem(),"reviewLabel_"+suffix);
            if (!fullyInViewport(window,check) || !fullyInViewport(window,label))
                std::cerr << "Review geometry failed at " << size.width << 'x' << size.height << " item " << index << '\n';
            ROCLINT_CHECK(fullyInViewport(window,check) && fullyInViewport(window,label));
            ROCLINT_CHECK(check->hasActiveFocus() && c.pendingReviewCount()==3-index && !c.canApprove());
            ROCLINT_CHECK(fullyInViewport(window,primary));
            if (index==0) savePreview(window,QString("review-checks-%1x%2.png").arg(size.width).arg(size.height));
            QTest::keyClick(window,Qt::Key_Space);
            ROCLINT_CHECK(c.waitForIdleForTesting(3000));
            ROCLINT_CHECK(!c.hasApproval() && observed->injections.empty() && calls==modelCalls);
        }
        ROCLINT_CHECK(c.canApprove() && primary->property("label").toString().startsWith("Approve &"));
        auto* diff=findVisual(window->contentItem(),"diffView");
        auto* viewport=window->findChild<QQuickItem*>("workflowViewport");
        ROCLINT_CHECK(diff && viewport);
        const double diffCap=std::max(80.0,std::min(240.0,viewport->height()*0.4));
        ROCLINT_CHECK(diff->height()<=diffCap+1.0);
        ROCLINT_CHECK(QMetaObject::invokeMethod(window,"openReview"));
        QElapsedTimer diffTimer; diffTimer.start();
        while ((!fullyInViewport(window,diff) || !diff->hasActiveFocus()) && diffTimer.elapsed() < 1000) {
            QTest::qWait(50);
        }
        if (!fullyInViewport(window,diff) || !diff->hasActiveFocus()) {
            const auto rect=diff->mapRectToItem(window->contentItem(),diff->boundingRect());
            std::cerr << "Diff review geometry: " << size.width << 'x' << size.height
                      << " rect=" << rect.x() << ',' << rect.y() << ',' << rect.width() << ',' << rect.height()
                      << " visible=" << diff->isVisible() << " focus=" << diff->hasActiveFocus() << '\n';
            std::cerr << "Viewport: height=" << viewport->height() << " contentY=" << viewport->property("contentY").toDouble()
                      << " contentHeight=" << viewport->property("contentHeight").toDouble() << '\n';
            savePreview(window,QStringLiteral("review-geometry-failure.png"));
        }
        ROCLINT_CHECK(fullyInViewport(window,diff) && diff->hasActiveFocus());
        if (size.width==560) {
            ROCLINT_CHECK(diff->property("contentHeight").toDouble()>diff->height());
            QTest::keyClick(window,Qt::Key_End); QTest::qWait(100);
            ROCLINT_CHECK(diff->property("contentY").toDouble()>0);
        } else {
            ROCLINT_CHECK(std::abs(diff->height()-diff->property("contentHeight").toDouble())<=1.0);
        }
        ROCLINT_CHECK(!c.hasApproval() && observed->injections.empty() && calls==modelCalls);
        ROCLINT_CHECK(QMetaObject::invokeMethod(window,"revertToDraft"));
        ROCLINT_CHECK(c.waitForIdleForTesting(3000));
        QTest::qWait(50);
        auto* editor=window->findChild<QQuickItem*>("promptInput");
        ROCLINT_CHECK(editor && editor->isVisible() && editor->property("text").toString()==QString::fromStdString(fx::kThreeGroupPrompt));
        ROCLINT_CHECK(!c.hasProposal() && !c.hasApproval() && calls==modelCalls);
        if (size.ambient) ROCLINT_CHECK(window->property("isDetailedExpanded").toBool());
        HostContextDetector::resetMock();
    }
    std::cout << "  [PASS] Checks and labels are in the viewport, long diffs stay bounded, and only explicit Review expands compact HUDs.\n";
}

void testProductionReviewPresentation() {
    QSettings().clear();
    std::atomic<int> calls{0};
    HostController c(":memory:");
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setLegacyDiscoveryForTesting(true);
    prepare(c, calls);
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(QString::fromStdString(fx::kChainPrompt), 0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice("alt_0_0", c.stateVersion())));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.hasProposal() && !c.hasApproval());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController", &c);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* panel = window->findChild<QObject*>("intentReviewPanel");
    ROCLINT_CHECK(panel && !panel->property("showDiagnostics").toBool());
    c.showCard();
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "setEditorText", Q_ARG(QVariant, QVariant(QString::fromStdString(fx::kChainPrompt)))));
    window->show();
    ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    QTest::qWait(50);
    std::function<bool(QQuickItem*)> mechanicalVisible = [&](QQuickItem* item) {
        if (item->isVisible() && item->property("text").toString().contains("[MECHANICALLY_VERIFIED]")) return true;
        for (auto* child : item->childItems()) if (mechanicalVisible(child)) return true;
        return false;
    };
    ROCLINT_CHECK(!mechanicalVisible(window->contentItem()));
    savePreview(window, "selective-review-default.png");
    panel->setProperty("showDiagnostics", true);
    QTest::qWait(50);
    ROCLINT_CHECK(mechanicalVisible(window->contentItem()));
    panel->setProperty("showDiagnostics", false);
    window->resize(560, 400);
    savePreview(window, "selective-review-compact.png");
    ROCLINT_CHECK(!c.hasApproval());
    std::cout << "  [PASS] Production HUD keeps mechanical metadata hidden.\n";
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

void testCaptureControlsEnableAmbientBadge() {
    QSettings().clear();
    auto bridge=std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed=bridge.get();
    HostController c(":memory:",std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setAmbientModeEnabled(false);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController",&c);
    engine.load(QUrl("qrc:/shell/DashboardWindow.qml"));
    ROCLINT_CHECK(engine.rootObjects().size()==1);
    auto* dashboard=engine.rootObjects().first();
    auto activate=[&](const char* objectName,const QString& expression) {
        auto* control=dashboard->findChild<QObject*>(objectName);
        ROCLINT_CHECK(control);
        QQmlExpression action(engine.rootContext(),control,expression);
        action.evaluate();
        ROCLINT_CHECK(!action.hasError());
    };
    activate("ambientEnableAction","clicked(null)");
    ROCLINT_CHECK(c.isAmbientModeEnabled() && observed->observing);
    ROCLINT_CHECK(QSettings().value("ambient/enabled").toBool());
    ObservedField field;
    field.fieldToken=71; field.processId=4242; field.kind=FieldKind::GuiEditor;
    field.appIdentifier="Cursor.exe"; field.appDisplayName="Cursor";
    field.text="Refactor the parser so it runs faster.";
    field.caretRect=QRect(500,400,2,18);
    observed->observe(field);
    for (int n=0;n<300 && !c.isAmbientBadgeVisible();++n) QTest::qWait(10);
    ROCLINT_CHECK(c.isAmbientBadgeVisible() && !c.isCardVisible());
    activate("ambientDisableAction","clicked(null)");
    ROCLINT_CHECK(!c.isAmbientModeEnabled() && !observed->observing && !c.isAmbientBadgeVisible());
    auto* toggle=dashboard->findChild<QObject*>("ambientCaptureToggle");
    ROCLINT_CHECK(toggle && toggle->setProperty("checked",true));
    activate("ambientCaptureToggle","toggled()");
    ROCLINT_CHECK(c.isAmbientModeEnabled() && observed->observing);
    ROCLINT_CHECK(toggle->setProperty("checked",false));
    activate("ambientCaptureToggle","toggled()");
    ROCLINT_CHECK(!c.isAmbientModeEnabled() && !observed->observing);
    std::cout << "  [PASS] Real QML capture settings enable observing and the amber badge; disable stops both.\n";
}

void testCompactHudControlsStayInsideWindow() {
    QSettings().clear();
    auto bridge=std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed=bridge.get();
    HostController c(":memory:",std::move(bridge));
    ROCLINT_CHECK(c.waitForStorageForTesting());
    c.setAmbientModeEnabled(true);
    c.setLegacyDiscoveryForTesting(true);
    const std::string discovery = R"({"status":"options_generated","choice_groups":[
        {"question":"Which speed?","alternatives":[{"label":"Latency","why_material":"Speed","evidence":["runs faster"]},{"label":"Throughput","why_material":"Speed","evidence":["runs faster"]}]},
        {"question":"Which docs?","alternatives":[{"label":"README","why_material":"Scope","evidence":["where needed"]},{"label":"All docs","why_material":"Scope","evidence":["where needed"]}]},
        {"question":"Which cleanup?","alternatives":[{"label":"Names","why_material":"Scope","evidence":["clean up the tests"]},{"label":"Dead tests","why_material":"Scope","evidence":["clean up the tests"]}]}]})";
    c.setTransportOverrideForTesting([discovery](std::string_view prompt) -> Result<std::string> {
        return prompt.find("<instructions>") == std::string_view::npos ? discovery
            : fx::rewriteReply("Refactor the parser, update documentation, and tidy tests.");
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(QString::fromStdString(fx::kThreeGroupPrompt),0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    for (int i=0; i<3; ++i)
        ROCLINT_CHECK(c.waitForCommandForTesting(c.submitCustomIntent(
            QStringLiteral("Preserve this explicit semantic decision %1.").arg(i), c.stateVersion())));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000) && c.pendingReviewCount()==3);
    ObservedField field;
    field.fieldToken=88; field.processId=4242; field.kind=FieldKind::GuiEditor;
    field.appIdentifier="Cursor.exe"; field.appDisplayName=QString(180,'W');
    field.text=QString::fromStdString(fx::kThreeGroupPrompt);
    field.fieldRect=QRect(1200,900,600,100);
    observed->observe(field);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController",&c);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    ROCLINT_CHECK(engine.rootObjects().size()==1);
    auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->setProperty("autoAnalysisDelayMs",60000);
    const QVariantMap project{{"name",QString(200,'W')},{"path",""}};
    window->setProperty("projectList",QVariantList{project});
    c.showCard(); window->show();
    ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000) && c.pendingReviewCount()==3);
    auto* model=window->findChild<QObject*>("modelRuntimeLabel");
    auto* state=window->findChild<QObject*>("hudStateLabel");
    auto* destination=window->findChild<QObject*>("deliveryDestinationLabel");
    auto* remaining=window->findChild<QObject*>("pendingReviewLabel");
    ROCLINT_CHECK(model && state && destination && remaining);
    // Exercise worst-case presentation text independently of capture ownership.
    destination->setProperty("text","Delivers into "+QString(180,'W'));
    // This is a real, durably saved proposal. Do not force visible/text and race a QML
    // binding that may be invalidated by an asynchronously opening capture session.
    ROCLINT_CHECK(remaining->property("visible").toBool());
    ROCLINT_CHECK(remaining->property("text").toString()=="3 checks left");
    for (const auto size : {QSize(620,250),QSize(420,200),QSize(560,400),QSize(820,520)}) {
        window->resize(size); QTest::qWait(100);
        for (const auto& label : {QString("[ LOADING MODEL ]"),QString("[ INFERRING ON A VERY LONG ACCELERATOR NAME ]")}) {
            model->setProperty("text",label);
            state->setProperty("text","[ NO SUGGESTION ]");
            QTest::qWait(50);
            for (const auto* name : {"expandToggleButton","pinButton","setButton","minButton","closeButton",
                                    "engineBadge","hudStateBadge","bypassBtn","approveRewriteControl","hideBtn","deliveryDestinationLabel","pendingReviewLabel"}) {
                auto* item=window->findChild<QQuickItem*>(name);
                if (!fullyInViewport(window,item)) {
                    const auto rect = item->mapRectToItem(window->contentItem(), item->boundingRect());
                    std::cerr << "HUD control " << name << " at " << size.width() << 'x' << size.height()
                        << " visible=" << item->isVisible() << " geometry=" << rect.x() << ',' << rect.y()
                        << ',' << rect.width() << ',' << rect.height() << '\n';
                }
                ROCLINT_CHECK(fullyInViewport(window,item));
            }
        }
        savePreview(window,QString("hud-controls-%1x%2.png").arg(size.width()).arg(size.height()));
    }
    std::cout << "  [PASS] Header and footer controls fit compact/resized HUDs with long project, source and model labels.\n";
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

void testReviewAcknowledgementStorageFailure() {
    QSettings().clear(); QTemporaryDir directory; const auto path=directory.filePath("review.db");
    HostController c(path); ROCLINT_CHECK(c.waitForStorageForTesting()); c.setAmbientModeEnabled(false);
    c.setLegacyDiscoveryForTesting(true);
    c.setTransportOverrideForTesting([](std::string_view prompt) -> Result<std::string> {
        return prompt.find("<instructions>") == std::string_view::npos ? kDiscovery : fx::rewriteReply("Make the parser quicker.");
    });
    ROCLINT_CHECK(c.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.analyzePrompt(QString::fromStdString(fx::kChainPrompt),0)));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.confirmChoice("alt_0_0",c.stateVersion())));
    ROCLINT_CHECK(c.waitForCommandForTesting(c.synthesizeRewrite(c.stateVersion())));
    ROCLINT_CHECK(c.waitForIdleForTesting(3000));
    ROCLINT_CHECK(c.pendingReviewCount()==1 && !c.canApprove());
    QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("hostController",&c);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml")); ROCLINT_CHECK(engine.rootObjects().size()==1);
    auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(QMetaObject::invokeMethod(window,"setEditorText",Q_ARG(QVariant,QVariant(QString::fromStdString(fx::kChainPrompt)))));
    window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    ROCLINT_CHECK(QMetaObject::invokeMethod(window,"openReview")); QTest::qWait(75);
    roclint::storage::Database blocker(std::filesystem::path(path.toStdString()));
    ROCLINT_CHECK(blocker.execute("BEGIN IMMEDIATE").ok());
    int ticks=0; QTimer heartbeat; QObject::connect(&heartbeat,&QTimer::timeout,[&]() { ++ticks; }); heartbeat.start(2);
    bool completed=false;
    QObject::connect(&c,&HostController::storageCommandFinished,[&](quint64,const QString& name,bool success,const QString&,const QString&) {
        if (name != "acknowledgeConstraint") return;
        ROCLINT_CHECK(!success && !completed); completed=true;
        ROCLINT_CHECK(blocker.execute("ROLLBACK").ok());
    });
    auto* check=findVisual(window->contentItem(),"constraintReview_intent_item_grp_0");
    ROCLINT_CHECK(check && check->hasActiveFocus()); QTest::keyClick(window,Qt::Key_Space);
    ROCLINT_CHECK(c.waitForIdleForTesting(30000));
    ROCLINT_CHECK(completed && ticks>100 && c.storageNeedsRecovery() && !c.canApprove() && !c.hasApproval() && !c.hasProposal());
    ROCLINT_CHECK(c.currentRevisionText()==QString::fromStdString(fx::kChainPrompt));
    ROCLINT_CHECK(window->findChild<QQuickItem*>("promptInput")->property("text").toString()==c.currentRevisionText());
    ROCLINT_CHECK(!window->findChild<QQuickItem*>("approveRewriteControl")->isEnabled());
    std::cout << "  [PASS] A failed acknowledgement keeps GUI timers running, preserves the draft and cannot enable approval.\n";
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
    ROCLINT_CHECK(!last->property("text").toString().contains("Unscored:"));
    auto* panel = window->findChild<QObject*>(QStringLiteral("intentReviewPanel"));
    ROCLINT_CHECK(panel && !panel->property("showDiagnostics").toBool());
    panel->setProperty("showDiagnostics", true);
    QCoreApplication::processEvents();
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
    ROCLINT_CHECK(action->property("text").toString() == QStringLiteral("Cancel analysis"));
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

void testHudPresentationSurvivesDraftAndDelivery() {
    QSettings().clear();
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController controller(QStringLiteral(":memory:"), std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    std::atomic<int> calls{0}; prepare(controller, calls, 100);
    controller.setAmbientModeEnabled(true);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window);
    auto* editor = window->findChild<QQuickItem*>(QStringLiteral("promptInput"));
    ROCLINT_CHECK(editor);
    QObject::connect(&controller, &HostController::hideCardRequested, window, &QQuickWindow::hide);
    QObject::connect(&controller, &HostController::showCardRequested, window, [window]() { window->show(); });
    ObservedField field;
    field.fieldToken = 91; field.processId = 444; field.kind = FieldKind::GuiEditor;
    field.appIdentifier = QStringLiteral("ChatGPT.exe"); field.appDisplayName = QStringLiteral("Codex");
    field.text = QString::fromStdString(fx::kChainPrompt);
    observed->observe(field);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.openFromAmbient()));
    ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.isClarifying());
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "toggleDetailedExpanded"));
    ROCLINT_CHECK(window->property("isDetailedExpanded").toBool());

    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice("alt_0_0", controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.canApprove());
    int delivered = 0;
    QObject::connect(&controller, &HostController::promptInjected, [&]() { ++delivered; });
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.approveAndCopyRewrite(controller.stateVersion())));
    QTRY_COMPARE(delivered, 1);
    ROCLINT_CHECK(controller.isCardVisible() && window->isVisible());
    ROCLINT_CHECK(window->property("isDetailedExpanded").toBool());

    // Editing invalidates approval/results without altering presentation.
    editor->forceActiveFocus(); QTest::keyClick(window, Qt::Key_End); QTest::keyClick(window, Qt::Key_A);
    ROCLINT_CHECK(controller.isDraftDirty() && !controller.hasApproval() && !controller.hasProposal());
    ROCLINT_CHECK(!controller.analysisForTesting().has_value());
    ROCLINT_CHECK(controller.isCardVisible() && window->isVisible() && window->property("isDetailedExpanded").toBool());
    const auto localDraft = editor->property("text").toString();
    window->hide(); window->show();
    ROCLINT_CHECK(editor->property("text").toString() == localDraft);
    window->showMinimized(); QTRY_COMPARE(window->visibility(), QWindow::Minimized);
    window->showNormal();
    ROCLINT_CHECK(editor->property("text").toString() == localDraft && window->property("isDetailedExpanded").toBool());

    // Returning to the source app must not implicitly dismiss the HUD.
    window->requestActivate(); QTRY_VERIFY(window->isActive());
    QQuickWindow other; other.resize(200, 100); other.show(); other.requestActivate();
    QTRY_VERIFY(other.isActive()); QTRY_VERIFY(!window->isActive());
    ROCLINT_CHECK(controller.isCardVisible() && window->isVisible());
    other.hide();

    // Clearing the source composer for the next prompt keeps the expanded HUD.
    field.text.clear(); observed->observe(field);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.openFromAmbient()));
    ROCLINT_CHECK(controller.currentRevisionId().isEmpty() && editor->property("text").toString().isEmpty());
    ROCLINT_CHECK(controller.isCardVisible() && window->isVisible() && window->property("isDetailedExpanded").toBool());

    // Hide/reopen and compact/reopen both preserve the last explicit layout choice.
    controller.hideCard(); ROCLINT_CHECK(!window->isVisible());
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.openFromAmbient()));
    ROCLINT_CHECK(window->isVisible() && window->property("isDetailedExpanded").toBool());
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "toggleDetailedExpanded"));
    ROCLINT_CHECK(!window->property("isDetailedExpanded").toBool());
    controller.hideCard();
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.openFromAmbient()));
    ROCLINT_CHECK(window->isVisible() && !window->property("isDetailedExpanded").toBool());

    // A hidden HUD stays hidden when an in-flight analysis finishes.
    window->setProperty("isDetailedExpanded", true);
    QMetaObject::invokeMethod(window, "setEditorText", Q_ARG(QVariant, QString::fromStdString(fx::kChainPrompt)));
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "analyzeNow"));
    QTRY_VERIFY(controller.isAnalyzing());
    controller.hideCard();
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(!controller.isCardVisible() && !window->isVisible() && window->property("isDetailedExpanded").toBool());
    std::cout << "  [PASS] HUD presentation survives delivery, editing, focus changes and new prompts; explicit hide stays authoritative.\n";
}

void testTypingDebouncesAnalysisInQml() {
    QSettings().clear();
    QTemporaryDir directory;
    const auto path = directory.filePath("history.db");
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController controller(path, std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    std::atomic<int> calls{0}; prepare(controller, calls, 220);
    controller.setAmbientModeEnabled(true);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController", &controller);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* editor = window->findChild<QQuickItem*>("promptInput");
    ROCLINT_CHECK(editor && window->setProperty("autoAnalysisDelayMs", 120));
    QObject::connect(&controller, &HostController::hideCardRequested, window, &QQuickWindow::hide);
    ObservedField field;
    field.fieldToken = 101; field.processId = 444; field.kind = FieldKind::GuiEditor;
    field.appIdentifier = "ChatGPT.exe"; field.appDisplayName = "Codex";
    observed->observe(field);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.openFromAmbient()));
    window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    window->setProperty("isDetailedExpanded", true);

    // Source-editor bursts schedule just the latest captured text after a pause.
    field.text = QString::fromStdString(fx::kChainPrompt); observed->observe(field);
    QTest::qWait(40); field.text += " Preserve the interface."; observed->observe(field);
    QTest::qWait(40); field.text += " Keep tests."; observed->observe(field);
    QTest::qWait(50); ROCLINT_CHECK(calls == 0);
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.currentRevisionText() == field.text && controller.isClarifying());
    ROCLINT_CHECK(!controller.hasProposal() && !controller.hasApproval() && observed->injections.empty());
    QTest::qWait(260); ROCLINT_CHECK(calls == 1); // A result must not start another check.

    // HUD typing waits behind draft persistence without losing its one pending check.
    roclint::storage::Database blocker(std::filesystem::path(path.toStdString()));
    ROCLINT_CHECK(blocker.execute("BEGIN IMMEDIATE").ok());
    const auto nextText = field.text + " Keep names.";
    editor->setProperty("text", nextText);
    ROCLINT_CHECK(controller.isDraftDirty() && !controller.analysisForTesting());
    QTest::qWait(220); ROCLINT_CHECK(calls == 1 && controller.storageBusy());
    ROCLINT_CHECK(blocker.execute("ROLLBACK").ok());
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 2, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.currentRevisionText() == nextText);

    // Editing during inference discards the old result and checks the new draft once.
    editor->setProperty("text", nextText + " First edit.");
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 3, 3000);
    const auto latest = nextText + " Latest edit.";
    editor->setProperty("text", latest);
    ROCLINT_CHECK(!controller.analysisForTesting() && !controller.hasApproval());
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 4, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.currentRevisionText() == latest && controller.isClarifying());

    // An explicit cancel consumes the check, without retrying the same draft.
    editor->setProperty("text", latest + " Cancel this check.");
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 5, 3000);
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "handleEnter"));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    QTest::qWait(300); ROCLINT_CHECK(calls == 5 && controller.analysisStatus() == "IDLE");

    // A failed reply is not automatically retried; a fresh edit can start a new check.
    controller.setTransportOverrideForTesting([&](std::string_view) -> Result<std::string> { ++calls; return std::string("{bad JSON"); });
    editor->setProperty("text", latest + " New check.");
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 6, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.analysisStatus() == "FAILED");
    QTest::qWait(300); ROCLINT_CHECK(calls == 6);
    prepare(controller, calls);
    editor->setProperty("text", latest + " Fresh draft.");
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 7, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));

    // Hide and blank drafts cancel pending checks. Showing or restoring text doesn't retry.
    editor->setProperty("text", latest + " Hide before checking."); window->hide();
    QTest::qWait(300); ROCLINT_CHECK(calls == 7);
    window->show(); QTest::qWait(200); ROCLINT_CHECK(calls == 7);
    editor->setProperty("text", ""); QTest::qWait(200); ROCLINT_CHECK(calls == 7);
    QMetaObject::invokeMethod(window, "setEditorText", Q_ARG(QVariant, latest));
    QTest::qWait(200); ROCLINT_CHECK(calls == 7);
    ROCLINT_CHECK(window->isVisible() && window->property("isDetailedExpanded").toBool());
    ROCLINT_CHECK(!controller.hasProposal() && !controller.hasApproval() && observed->injections.empty());
    std::cout << "  [PASS] Typing debounces one analysis, waits for storage, rejects stale results and never retries cancelled/failed drafts.\n";
}

void testEditPromptFromProposalUsesNewHudDraft() {
    QSettings().clear();
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController controller(":memory:", std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    controller.setAmbientModeEnabled(false);
    std::atomic<int> calls{0}; prepare(controller, calls);
    const auto original = QString::fromStdString(fx::kChainPrompt);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(original, controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice("alt_0_0", controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.hasProposal());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController", &controller);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    ROCLINT_CHECK(!engine.rootObjects().isEmpty());
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* editor = window->findChild<QQuickItem*>("promptInput");
    auto* panel = window->findChild<QObject*>("intentReviewPanel");
    ROCLINT_CHECK(window && editor && panel);
    window->setProperty("autoAnalysisDelayMs", 60000);
    window->setProperty("isDetailedExpanded", true);
    window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    QMetaObject::invokeMethod(window, "setEditorText", Q_ARG(QVariant, original));
    ROCLINT_CHECK(!window->property("showPromptEditor").toBool());

    // This is the panel action used in the captured-editor journey, not the
    // footer's separate discard action. It must actually leave preview mode.
    ROCLINT_CHECK(QMetaObject::invokeMethod(panel, "editDraftRequested"));
    const auto requireEventually = [](auto predicate) {
        QElapsedTimer timeout; timeout.start();
        while (!predicate() && timeout.elapsed() < 3000) QTest::qWait(10);
        ROCLINT_CHECK(predicate());
    };
    requireEventually([&]() { return !controller.hasProposal(); });
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    requireEventually([&]() { return editor->isVisible(); });
    requireEventually([&]() { return editor->hasActiveFocus(); });
    ROCLINT_CHECK(!controller.hasApproval() && observed->injections.empty());

    const QString rough = "pls refactor Queue::push so its easier to read, dont add blocking waits & keep the same producer ordering";
    editor->setProperty("text", rough);
    ROCLINT_CHECK(controller.isDraftDirty());
    QString rewriteInstructions;
    controller.setTransportOverrideForTesting([&rewriteInstructions](std::string_view instructions) -> Result<std::string> {
        if (instructions.find("<instructions>") == std::string_view::npos)
            return std::string(R"({"status":"no_ambiguity","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[]})");
        rewriteInstructions = QString::fromUtf8(instructions.data(), static_cast<qsizetype>(instructions.size()));
        return fx::rewriteReply("Refactor Queue::push for readability. Do not add blocking waits. Preserve the existing producer ordering.");
    });
    ROCLINT_CHECK(QMetaObject::invokeMethod(window, "analyzeNow"));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    ROCLINT_CHECK(controller.currentRevisionText() == rough && !controller.isDraftDirty());
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.hasProposal());
    ROCLINT_CHECK(rewriteInstructions.contains("Queue::push") && !rewriteInstructions.contains(QString::fromStdString(fx::kChainPrompt)));
    ROCLINT_CHECK(!controller.hasApproval() && observed->injections.empty());
    controller.setTransportOverrideForTesting([rough](std::string_view) -> Result<std::string> {
        return fx::rewriteReply(rough.toStdString());
    });
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.hasProposal());
    ROCLINT_CHECK(controller.proposalMessage().contains("No wording changes suggested"));
    auto* heading = window->findChild<QObject*>("reviewHeading");
    ROCLINT_CHECK(heading && heading->property("text").toString().contains("No wording changes suggested"));
    ROCLINT_CHECK(!controller.hasApproval() && observed->injections.empty());
    std::cout << "  [PASS] Panel edit leaves proposal preview, focuses the draft and rewrites the newly frozen text.\n";
}

void testExplicitClarificationAndDeferredRewrite() {
    QSettings().clear();
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController controller(":memory:", std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    controller.setAmbientModeEnabled(false);
    const QString draft = "Make Reader::open better. Make Queue::push faster.";
    const std::string choices = R"({"status":"options_generated","assistance_kind":"clarification","choice_groups":[
      {"question":"What should improve in Reader::open?","material":true,"alternatives":[
        {"label":"Readability","why_material":"Structure","evidence":["better"]},
        {"label":"Reliability","why_material":"Error handling","evidence":["better"]},
        {"label":"Performance","why_material":"Execution cost","evidence":["better"]}]},
      {"question":"Which kind of speed matters?","material":true,"alternatives":[
        {"label":"Lower latency","why_material":"Individual calls","evidence":["faster"]},
        {"label":"Higher throughput","why_material":"Batches","evidence":["faster"]}]}]})";
    int discoveryCalls = 0, rewriteCalls = 0;
    QString instructions;
    controller.setTransportOverrideForTesting([&](std::string_view prompt) -> Result<std::string> {
        if (prompt.find("granite.goldfish_audit.v1") != std::string_view::npos) {
            return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
        }
        if (prompt.find("<instructions>") != std::string_view::npos) {
            ++rewriteCalls;
            instructions = QString::fromUtf8(prompt.data(), static_cast<qsizetype>(prompt.size()));
            return fx::rewriteReply(draft.toStdString());
        }
        if (++discoveryCalls == 1) return std::string(R"({"status":"no_ambiguity","choice_groups":[]})");
        return choices;
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.analyzePrompt(draft, controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && !controller.isClarifying());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController", &controller);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    ROCLINT_CHECK(!engine.rootObjects().isEmpty());
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    ROCLINT_CHECK(window);
    window->setProperty("autoAnalysisDelayMs", 60000);
    QMetaObject::invokeMethod(window, "setEditorText", Q_ARG(QVariant, draft));
    window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    auto* clarify = window->findChild<QQuickItem*>("reviewPromptButton");
    auto* skip = window->findChild<QQuickItem*>("rewriteWithoutQuestionsButton");
    ROCLINT_CHECK(clarify && skip && clarify->isVisible() && clarify->property("label").toString() == "Clarify");
    ROCLINT_CHECK(QMetaObject::invokeMethod(clarify, "clicked"));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.isClarifying());
    ROCLINT_CHECK(controller.activeAlternatives().size() == 3 && rewriteCalls == 0);
    ROCLINT_CHECK(skip->isVisible() && skip->isEnabled());
    savePreview(window, QStringLiteral("clarification-options.png"));
    ROCLINT_CHECK(QMetaObject::invokeMethod(skip, "clicked"));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.hasProposal());
    ROCLINT_CHECK(rewriteCalls == 1 && instructions.contains("<open_questions>") && instructions.contains("Which kind of speed matters?"));
    ROCLINT_CHECK(controller.getConfirmedIntentPayload()["items"].toList().isEmpty());
    ROCLINT_CHECK(controller.getConfirmedIntentPayload()["unresolvedItemIds"].toStringList().size() == 2);
    ROCLINT_CHECK(!controller.hasApproval() && observed->injections.empty() && clarify->isVisible());

    // Clarification stays available even from a completed rewrite preview.
    ROCLINT_CHECK(QMetaObject::invokeMethod(clarify, "clicked"));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.isClarifying() && !controller.hasProposal());
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.submitCustomIntent("Improve readability only", controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    const auto stale = controller.stateVersion() - 1;
    ROCLINT_CHECK(!controller.waitForCommandForTesting(controller.rewriteWithoutQuestions(stale)));
    ROCLINT_CHECK(rewriteCalls == 1);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.rewriteWithoutQuestions(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.hasProposal());
    ROCLINT_CHECK(instructions.contains("Improve readability only"));
    ROCLINT_CHECK(controller.getConfirmedIntentPayload()["items"].toList().size() == 1);
    ROCLINT_CHECK(controller.getConfirmedIntentPayload()["unresolvedItemIds"].toStringList().size() == 1);
    ROCLINT_CHECK(!controller.hasApproval() && observed->injections.empty());
    // A reentrant edit during publication must not start a rewrite of old text.
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.reviewPrompt(draft, controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.isClarifying());
    bool editOnPublication = true;
    QObject::connect(&controller, &HostController::intentChanged, [&]() {
        if (editOnPublication && controller.hasConfirmedIntent()) {
            editOnPublication = false;
            ROCLINT_CHECK(controller.noteDraftEdited(controller.stateVersion()));
        }
    });
    ROCLINT_CHECK(!controller.waitForCommandForTesting(controller.rewriteWithoutQuestions(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.isDraftDirty() && rewriteCalls == 2);

    HostController conflict(":memory:");
    ROCLINT_CHECK(conflict.waitForStorageForTesting());
    conflict.setLegacyDiscoveryForTesting(true);
    conflict.setTransportOverrideForTesting([](std::string_view) -> Result<std::string> {
        return std::string(R"({"status":"needs_clarification","choice_groups":[],"conflicts":[{"description":"Three and five exclusive bullets","blocking":true}]})");
    });
    ROCLINT_CHECK(conflict.markModelReadyForTesting(fx::testModel()));
    ROCLINT_CHECK(conflict.waitForCommandForTesting(conflict.analyzePrompt("Use exactly three bullets. Use exactly five bullets.", conflict.stateVersion())));
    ROCLINT_CHECK(conflict.waitForIdleForTesting(3000));
    const auto conflictVersion = conflict.stateVersion();
    ROCLINT_CHECK(!conflict.waitForCommandForTesting(conflict.rewriteWithoutQuestions(conflictVersion)));
    ROCLINT_CHECK(conflict.stateVersion() == conflictVersion && !conflict.hasProposal() && !conflict.hasApproval());
    std::cout << "  [PASS] Clarify stays available; explicit skip preserves unanswered questions and custom choices without delivery.\n";
}

void testAutoAnalysisResumesAfterDelivery() {
    QSettings().clear();
    auto bridge = std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed = bridge.get();
    HostController controller(":memory:", std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    std::atomic<int> calls{0}; prepare(controller, calls);
    controller.setAmbientModeEnabled(true);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController", &controller);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* editor = window->findChild<QQuickItem*>("promptInput");
    ROCLINT_CHECK(editor && window->setProperty("autoAnalysisDelayMs", 120));
    ObservedField field;
    field.fieldToken = 102; field.processId = 444; field.kind = FieldKind::GuiEditor;
    field.appIdentifier = "ChatGPT.exe"; field.appDisplayName = "Codex";
    observed->observe(field);
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.openFromAmbient()));
    window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    window->setProperty("isDetailedExpanded", true);
    field.text = QString::fromStdString(fx::kChainPrompt); observed->observe(field);
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 1, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    controller.pauseAmbientFollowing();
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice("alt_0_0", controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.canApprove() && calls == 2);
    // Editing the bound source supersedes an unapproved review and schedules a fresh check.
    field.text += " Preserve behavior."; observed->observe(field);
    ROCLINT_CHECK(editor->property("text").toString() == field.text && !controller.hasProposal() && !controller.hasConfirmedIntent());
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 3, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000));
    controller.pauseAmbientFollowing();
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.confirmChoice("alt_0_0", controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.synthesizeRewrite(controller.stateVersion())));
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.canApprove() && calls == 4);
    int delivered = 0;
    QObject::connect(&controller, &HostController::promptInjected, [&]() { ++delivered; });
    ROCLINT_CHECK(controller.waitForCommandForTesting(controller.approveAndCopyRewrite(controller.stateVersion())));
    QTRY_COMPARE(delivered, 1);
    field.text = kRewritten; observed->observe(field);
    QTest::qWait(260); ROCLINT_CHECK(calls == 4 && controller.hasApproval());
    field.text.clear(); observed->observe(field);
    ROCLINT_CHECK(editor->property("text").toString().isEmpty() && !controller.hasApproval() && !controller.hasProposal());
    field.text = QString::fromStdString(fx::kChainPrompt) + " Preserve behavior."; observed->observe(field);
    QTRY_COMPARE_WITH_TIMEOUT(calls.load(), 5, 3000);
    ROCLINT_CHECK(controller.waitForIdleForTesting(3000) && controller.currentRevisionText() == field.text);
    ROCLINT_CHECK(!controller.hasProposal() && !controller.hasApproval() && observed->injections.size() == 1);
    ROCLINT_CHECK(window->isVisible() && window->property("isDetailedExpanded").toBool());
    std::cout << "  [PASS] Delivery echoes do not trigger analysis; new source typing checks the next prompt automatically.\n";
}

void testGenerationMeterUsesMeasuredProgress() {
    QSettings().clear();
    HostController c(":memory:"); ROCLINT_CHECK(c.waitForStorageForTesting());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController",&c);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    ROCLINT_CHECK(engine.rootObjects().size()==1);
    auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* meter=window->findChild<QQuickItem*>("generationMeter");
    ROCLINT_CHECK(meter && !window->findChild<QObject*>("reviewDiagnosticsButton"));
    // Keep this animation fixture on one rate; the HUD also supports colored
    // generation/decode labels. Assert visible copy independently of its markup.
    meter->setProperty("tokenSpeedMode",QStringLiteral("GENERATION"));
    const auto plainRate=[&]() {
        QTextDocument label;
        label.setHtml(meter->property("rateText").toString());
        return label.toPlainText();
    };
    auto* wave=window->findChild<QObject*>("referencePixelWave");
    ROCLINT_CHECK(wave && wave->property("source").toUrl().toString().endsWith("assets/pixel-signal.png"));
    window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    ROCLINT_CHECK(meter->property("rateText").toString()=="— tok/s" && !meter->property("recentActivity").toBool());
    meter->setProperty("processing",true);
    ROCLINT_CHECK(meter->property("rateText").toString()=="Preparing…");
    meter->setProperty("tokensPerSecond",200.0);
    meter->setProperty("tokenCount",12.0); // A burst shorter than one display tick.
    const double started=meter->property("activityStartedAt").toDouble();
    ROCLINT_CHECK(started>0 && meter->property("recentActivity").toBool());
    meter->setProperty("tokensPerSecond",210.0);
    ROCLINT_CHECK(QMetaObject::invokeMethod(meter,"refreshRate",Q_ARG(QVariant,QVariant(started+100)),Q_ARG(QVariant,QVariant(false))));
    ROCLINT_CHECK(meter->property("displayedRate").toDouble()==200.0); // Readable, throttled number.
    ROCLINT_CHECK(QMetaObject::invokeMethod(meter,"refreshRate",Q_ARG(QVariant,QVariant(started+600)),Q_ARG(QVariant,QVariant(false))));
    ROCLINT_CHECK(meter->property("displayedRate").toDouble()==210.0);
    meter->setProperty("preparing",true); // Warmup hides the cached rate without erasing recent activity.
    ROCLINT_CHECK(meter->property("rateText").toString()=="Preparing…" && meter->property("recentActivity").toBool());
    meter->setProperty("preparing",false);
    meter->setProperty("processing",false);
    ROCLINT_CHECK(plainRate()=="Last · 210.0 tok/s" && meter->property("recentActivity").toBool());
    ROCLINT_CHECK(QMetaObject::invokeMethod(meter,"advance",Q_ARG(QVariant,QVariant(started+1700))));
    ROCLINT_CHECK(meter->property("activityLevel").toDouble()==1.0); // Completion never cuts the burst short.
    ROCLINT_CHECK(QMetaObject::invokeMethod(meter,"advance",Q_ARG(QVariant,QVariant(started+2150))));
    ROCLINT_CHECK(meter->property("activityLevel").toDouble()>0 && meter->property("activityLevel").toDouble()<1);
    ROCLINT_CHECK(QMetaObject::invokeMethod(meter,"advance",Q_ARG(QVariant,QVariant(started+2600))));
    ROCLINT_CHECK(!meter->property("recentActivity").toBool());
    meter->setProperty("tokenCount",1.0); // New request/cancellation counter reset clears history.
    ROCLINT_CHECK(!meter->property("recentActivity").toBool() && meter->property("displayedRate").toDouble()==0);
    meter->setProperty("reducedMotion",true);
    meter->setProperty("tokensPerSecond",123.4);
    meter->setProperty("tokenCount",20.0);
    ROCLINT_CHECK(!meter->property("recentActivity").toBool() && meter->property("displayedRate").toDouble()==123.4);
    savePreview(window,QStringLiteral("pixel-wave-reduced-motion.png"));
    meter->setProperty("tokenCount",0.0);
    meter->setProperty("reducedMotion",false);
    meter->setProperty("processing",true);
    meter->setProperty("tokensPerSecond",123.4);
    meter->setProperty("tokenCount",24.0);
    meter->setProperty("processing",false); // No simulated ongoing inference in the preview.
    QElapsedTimer burst; burst.start();
    const int offsets[]={0,300,600,1200,1800,2500};
    for (int i=0;i<6;++i) {
        const int wait=offsets[i]-static_cast<int>(burst.elapsed());
        if (wait>0) QTest::qWait(wait);
        savePreview(window,QStringLiteral("pixel-wave-burst-%1.png").arg(i));
    }
    while (meter->property("recentActivity").toBool() && burst.elapsed() < 3200) QTest::qWait(50);
    ROCLINT_CHECK(!meter->property("recentActivity").toBool());
    std::cout << "  [PASS] Reference pixel wave preserves sub-frame bursts for 1.8s + 0.7s fade, throttles rates, labels completion, and respects reduced motion.\n";
}

void testUserRequestedRewriteFromNewDraft() {
    QSettings().clear();
    auto bridge=std::make_unique<roclint::test::MockAccessibilityBridge>();
    auto* observed=bridge.get();
    HostController controller(":memory:",std::move(bridge));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setLegacyDiscoveryForTesting(true);
    controller.setAmbientModeEnabled(false);
    int discoveries=0, rewrites=0;
    const QString draft="Refactor Reader::open for readability. Preserve behavior and caller-owned handles.";
    controller.setTransportOverrideForTesting([&](std::string_view request) -> Result<std::string> {
        if (request.find("granite.goldfish_audit.v1") != std::string_view::npos) {
            return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
        }
        if (request.find("<instructions>")!=std::string_view::npos) {
            ++rewrites; return fx::rewriteReply(draft.toStdString());
        }
        ++discoveries; return std::string(R"({"status":"no_ambiguity","choice_groups":[]})");
    });
    ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("hostController",&controller);
    engine.load(QUrl("qrc:/shell/FloatingCard.qml"));
    ROCLINT_CHECK(engine.rootObjects().size()==1);
    auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->setProperty("autoAnalysisDelayMs",60000);
    auto* editor=window->findChild<QQuickItem*>("promptInput");
    auto* rewrite=window->findChild<QQuickItem*>("rewriteWithoutQuestionsButton");
    ROCLINT_CHECK(editor && rewrite);
    window->show(); ROCLINT_CHECK(QTest::qWaitForWindowExposed(window));
    ROCLINT_CHECK(!rewrite->isEnabled());
    editor->setProperty("text",draft);
    ROCLINT_CHECK(rewrite->isVisible() && rewrite->isEnabled());
    ROCLINT_CHECK(QMetaObject::invokeMethod(rewrite,"clicked"));
    QTRY_VERIFY_WITH_TIMEOUT(controller.hasProposal(),5000);
    ROCLINT_CHECK(discoveries==1 && rewrites==1 && controller.currentRevisionText()==draft);
    ROCLINT_CHECK(!controller.hasApproval() && observed->injections.empty());
    controller.hideCard();
    controller.activateCard();
    ROCLINT_CHECK(controller.isCardVisible() && controller.hasProposal() && controller.currentRevisionText()==draft);
    ROCLINT_CHECK(discoveries==1 && rewrites==1 && !controller.hasApproval() && observed->injections.empty());
    savePreview(window,QStringLiteral("requested-rewrite.png"));
    std::cout << "  [PASS] User-requested rewrite checks the new HUD draft then creates one unapproved preview.\n";
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
        if (request.find("granite.goldfish_audit.v1") != std::string_view::npos) {
            return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
        }
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
    ROCLINT_CHECK(approve->property("label").toString() == "Review rewrite");
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
    ROCLINT_CHECK(approve->property("label").toString() == "Review rewrite");
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
    ROCLINT_CHECK(controller.canApprove());
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
    ROCLINT_CHECK(modelLabel && (labelText == "Model not loaded" || labelText == "Ready · standby"));
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
        if (prompt.find("granite.goldfish_audit.v1") != std::string_view::npos) {
            return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
        }
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

void testInspectModeVisualInspector() {
    QSettings().clear();
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/shell/FloatingCard.qml")));
    ROCLINT_CHECK(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* card = window->findChild<QQuickItem*>(QStringLiteral("auditInspectorCard"));
    auto* badge = window->findChild<QQuickItem*>(QStringLiteral("hudModeBadge"));
    auto* modeLabel = window->findChild<QQuickItem*>(QStringLiteral("hudModeLabel"));
    ROCLINT_CHECK(card && badge && modeLabel);

    // In auto mode without audit failure, inspector card is hidden
    ROCLINT_CHECK(!controller.isInspectMode());
    ROCLINT_CHECK(!card->isVisible());
    ROCLINT_CHECK(!badge->isVisible());

    // Switch to inspect mode: badge and inspector card appear immediately
    controller.setAssistanceMode(QStringLiteral("inspect"));
    QTest::qWait(50);
    ROCLINT_CHECK(controller.isInspectMode());
    ROCLINT_CHECK(card->isVisible());
    ROCLINT_CHECK(card->property("isExpanded").toBool());
    ROCLINT_CHECK(badge->isVisible());
    ROCLINT_CHECK(modeLabel->property("text").toString() == QStringLiteral("/INSPECT"));

    // Switch back to auto mode: inspector card hides cleanly
    controller.setAssistanceMode(QStringLiteral("auto"));
    QTest::qWait(50);
    ROCLINT_CHECK(!controller.isInspectMode());
    ROCLINT_CHECK(!card->isVisible());

    // Verify that failure recovery action buttons exist in the hierarchy
    auto* clarifyBtn = window->findChild<QQuickItem*>(QStringLiteral("auditClarifyBoundaryButton"));
    auto* strictBtn = window->findChild<QQuickItem*>(QStringLiteral("auditStrictRewriteButton"));
    auto* revertBtn = window->findChild<QQuickItem*>(QStringLiteral("auditRevertDraftButton"));
    ROCLINT_CHECK(clarifyBtn && strictBtn && revertBtn);

    std::cout << "  [PASS] Inspect mode visual inspector and HUD badge react accurately.\n";
}

int main(int argc, char* argv[]) {
    // Opt in to native platform window/focus verification without changing the CI default.
    if (!qEnvironmentVariableIsSet("ROCLINT_NATIVE_UI_TEST")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QUICK_BACKEND", "software");
    }
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
    if (qEnvironmentVariableIsSet("ROCLINT_QT_ASSERTION_GUARD_PROBE")) {
        (void)QTest::qVerify(false, "intentional guard probe", "", __FILE__, __LINE__);
        ROCLINT_CHECK(!QTest::currentTestFailed());
        return 0;
    }
    if (qEnvironmentVariableIsSet("ROCLINT_NATIVE_UI_TEST")) {
        testCompactHudControlsStayInsideWindow();
        testCaptureControlsEnableAmbientBadge();
        testOptionalPresentationFromPersistedV5();
        testReviewGeometryAndExplicitExpansion();
        testProductionReviewPresentation();
        testHudPresentationSurvivesDraftAndDelivery();
        testTypingDebouncesAnalysisInQml();
        testAutoAnalysisResumesAfterDelivery();
        ROCLINT_CHECK(!QTest::currentTestFailed());
        std::cout << "[Native HUD] Presentation regression PASSED.\n";
        return 0;
    }
    if (qEnvironmentVariableIsSet("ROCLINT_EDIT_DRAFT_REGRESSION_ONLY")) {
        testEditPromptFromProposalUsesNewHudDraft();
        return 0;
    }
    if (qEnvironmentVariableIsSet("ROCLINT_GENERATION_METER_ONLY")) {
        testGenerationMeterUsesMeasuredProgress();
        ROCLINT_CHECK(!QTest::currentTestFailed());
        return 0;
    }
    if (qEnvironmentVariableIsSet("ROCLINT_CLARIFICATION_REGRESSION_ONLY")) {
        testExplicitClarificationAndDeferredRewrite();
        ROCLINT_CHECK(!QTest::currentTestFailed());
        return 0;
    }
    testOptionalPresentationFromPersistedV5();
    testReviewGeometryAndExplicitExpansion();
    testProductionReviewPresentation();
    testNativeGlobalShortcutDispatch();
    testCompactHudControlsStayInsideWindow();
    testCaptureControlsEnableAmbientBadge();
    testCaptureAndSettings();
    testHintChangeRejectsInFlightResult();
    testModelSwitchDuringInference();
    testKeyboardWorkflow();
    testReviewRequiresExplicitActivation();
    testReviewAcknowledgementStorageFailure();
    testOverBudgetAssumptionsRemainVisibleAndUnconfirmed();
    testResidencySettingsKeyboard();
    testClipboardFallbackDoesNotDismissQmlCard();
    testGenerationMeterUsesMeasuredProgress();
    testUserRequestedRewriteFromNewDraft();
    testManualGraniteRewriteConnectsEditorWithoutLosingReview();
    testLiveCapturedDraftAndCancelRetryInQml();
    testHudPresentationSurvivesDraftAndDelivery();
    testTypingDebouncesAnalysisInQml();
    testEditPromptFromProposalUsesNewHudDraft();
    testExplicitClarificationAndDeferredRewrite();
    testAutoAnalysisResumesAfterDelivery();
    testManualRemainderCanFinishAndGeneratePreview();
    testForbiddenColorOptionIsBlockedInUiAndController();
    testInspectModeVisualInspector();
    ROCLINT_CHECK(!QTest::currentTestFailed());
    std::cout << "[Product Alignment] All tests PASSED.\n";
    return 0;
}
