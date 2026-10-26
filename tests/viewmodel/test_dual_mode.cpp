#include "HostController.hpp"
#include "HostContext.hpp"
#include "support/MockAccessibilityBridge.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>

#include <functional>
#include <iostream>
#include <memory>

using roclint::host::AccessibilityAccess;
using roclint::host::FieldKind;
using roclint::host::HostController;
using roclint::host::InjectionOutcome;
using roclint::host::ObservedField;
using roclint::test::MockAccessibilityBridge;
using namespace roclint::core;
namespace bridge = roclint::bridge;

namespace {

const QString kPrompt = QString::fromStdString(fx::kChainPrompt);

const std::string kAmbiguousReply = R"({"status": "options_generated",
  "choice_groups": [{"dimension": "performance", "question": "What should faster mean?", "material": true,
    "alternatives": [
      {"label": "Lower latency per call", "why_material": "Optimizes single requests", "evidence": ["runs faster"]},
      {"label": "Higher throughput", "why_material": "Optimizes batches", "evidence": ["runs faster"]}]}],
  "assumptions": [], "missing_decisions": [], "conflicts": [], "clarification_question": ""})";

const std::string kClearReply = R"({"status": "no_ambiguity", "choice_groups": [], "assumptions": [],
  "missing_decisions": [], "conflicts": [], "clarification_question": ""})";

const std::string kRewrite = "Refactor the parser for lower latency per call.";

struct Harness {
    MockAccessibilityBridge* bridge{nullptr};
    std::unique_ptr<HostController> controller;
    std::shared_ptr<int> calls = std::make_shared<int>(0);
    std::shared_ptr<int> rewriteCalls = std::make_shared<int>(0);

    explicit Harness(AccessibilityAccess access = AccessibilityAccess::Granted,
                     const std::string& discoveryReply = kAmbiguousReply) {
        // Native desktop focus must not decide a controller unit test's invocation path.
        roclint::host::HostContextDetector::setMockObservation(roclint::host::HostContextObservation{});
        auto mock = std::make_unique<MockAccessibilityBridge>();
        mock->accessValue = access;
        bridge = mock.get();
        controller = std::make_unique<HostController>(QStringLiteral(":memory:"), std::move(mock));
        ROCLINT_CHECK(controller->waitForStorageForTesting());
        controller->setLegacyDiscoveryForTesting(true);
        auto c = calls;
        auto r = rewriteCalls;
        const std::string rewrite = fx::rewriteReply(kRewrite);
        controller->setTransportOverrideForTesting(
            [c, r, discoveryReply, rewrite](std::string_view instruction) -> Result<std::string> {
                ++*c;
                if (instruction.find("<instructions>") != std::string_view::npos) {
                    ++*r;
                    return rewrite;
                }
                return discoveryReply;
            });
        ROCLINT_CHECK(controller->markModelReadyForTesting(fx::testModel()));
    }

    HostController& c() { return *controller; }
    ~Harness() { roclint::host::HostContextDetector::resetMock(); }
};

bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 3000) {
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

void pumpFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

ObservedField cursorField(const QString& text, quint64 token = 7) {
    ObservedField f;
    f.fieldToken = token;
    f.appIdentifier = QStringLiteral("Cursor.exe");
    f.appDisplayName = QStringLiteral("Cursor");
    f.processId = 4242;
    f.kind = FieldKind::GuiEditor;
    f.text = text;
    f.caretOffset = static_cast<int>(text.size());
    f.caretRect = QRect(500, 400, 2, 18);
    return f;
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

// Observes the ambiguous prompt in Cursor and waits for the background check's badge.
void observeAndWaitForBadge(Harness& h, const QString& prompt = kPrompt) {
    h.bridge->observe(cursorField(prompt));
    ROCLINT_CHECK(pumpUntil([&]() { return h.c().isAmbientBadgeVisible(); }));
}

void testAmbientRouteWhenPermissionGranted() {
    Harness h;
    ROCLINT_CHECK(!h.c().isAmbientActive());
    ROCLINT_CHECK(!h.bridge->observing);
    h.c().setAmbientModeEnabled(true);
    ROCLINT_CHECK(h.c().isAmbientActive());
    ROCLINT_CHECK(h.bridge->observing);
    ROCLINT_CHECK(h.c().accessibilityStatus() == "GRANTED");

    observeAndWaitForBadge(h);
    ROCLINT_CHECK(h.c().ambientBadgeLabel() == "1 question");
    ROCLINT_CHECK(h.c().ambientBadgeX() > 500 && h.c().ambientBadgeY() > 400);
    ROCLINT_CHECK(*h.calls == 1);
    // The background check leaves nothing on disk.
    ROCLINT_CHECK(h.c().sessionCount() == 0);
    ROCLINT_CHECK(!h.c().isCardVisible());

    // The hotkey opens the card on the captured prompt, with the manual text box hidden.
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().isCardVisible());
    ROCLINT_CHECK(h.c().isAmbientCapture());
    ROCLINT_CHECK(h.c().isAmbientAttached());
    ROCLINT_CHECK(h.c().captureTargetName() == "Cursor");
    ROCLINT_CHECK(h.c().deliveryLabel() == "Delivers into Cursor");
    ROCLINT_CHECK(h.c().currentRevisionText() == kPrompt);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    ROCLINT_CHECK(h.c().isClarifying());
    ROCLINT_CHECK(h.c().activeAlternatives().size() == 2);
    // The cached reply was bound to the frozen revision without asking the model again.
    ROCLINT_CHECK(*h.calls == 1);
    ROCLINT_CHECK(h.c().sessionCount() == 1);
    auto attempts = h.c().repositoryForTesting()->getModelAttempts(SessionId(h.c().currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
    ROCLINT_CHECK(attempts.value()[0].status == roclint::storage::AttemptStatus::Succeeded);
    ROCLINT_CHECK(h.c().analysisForTesting()->baseRevisionId == h.c().currentRevisionId().toStdString());
    std::cout << "  [PASS] Granted + enabled: background check, badge, hotkey opens the captured prompt.\n";
}

void testWidgetFallbackWhenPermissionDenied() {
    Harness h(AccessibilityAccess::NotGranted);
    h.c().setAmbientModeEnabled(true);
    ROCLINT_CHECK(!h.c().isAmbientActive());
    ROCLINT_CHECK(!h.bridge->observing);
    ROCLINT_CHECK(h.c().accessibilityStatus() == "NOT_GRANTED");
    ROCLINT_CHECK(h.c().ambientStatus().contains(QStringLiteral("Accessibility permission")));

    h.bridge->observe(cursorField(kPrompt));
    pumpFor(HostController::kAmbientDebounceMs * 2);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    ROCLINT_CHECK(*h.calls == 0);

    setClipboard(QStringLiteral("Draft copied from somewhere else"));
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().isCardVisible());
    ROCLINT_CHECK(!h.c().isAmbientCapture());
    ROCLINT_CHECK(!h.c().isAmbientAttached());
    ROCLINT_CHECK(h.c().capturedDraftText().isEmpty());
    ROCLINT_CHECK(h.c().deliveryLabel() == "Delivers to clipboard");
    h.c().hideCard();

    ROCLINT_CHECK(h.c().requestAccessibilityPermission());
    ROCLINT_CHECK(h.bridge->accessRequests == 1);
    h.bridge->setAccess(AccessibilityAccess::Granted);
    ROCLINT_CHECK(h.c().isAmbientActive());
    ROCLINT_CHECK(h.bridge->observing);
    std::cout << "  [PASS] Denied permission: widget mode with clipboard capture; granting switches routes.\n";
}

void testAmbientOffUsesWidgetEvenWithPermission() {
    Harness h;
    ROCLINT_CHECK(!h.c().isAmbientModeEnabled());
    h.bridge->observe(cursorField(kPrompt));
    pumpFor(HostController::kAmbientDebounceMs * 2);
    ROCLINT_CHECK(*h.calls == 0);
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().isCardVisible() && !h.c().isAmbientCapture());
    std::cout << "  [PASS] Ambient mode off: no observation is used, the widget opens.\n";
}

void testIneligibleFieldsAreNeverRead() {
    Harness h;
    h.c().setAmbientModeEnabled(true);

    auto password = cursorField(kPrompt);
    password.isPassword = true;
    auto notepad = cursorField(kPrompt);
    notepad.appIdentifier = QStringLiteral("notepad.exe");
    auto elevated = cursorField(kPrompt);
    elevated.elevationMismatch = true;
    for (const auto& f : {password, notepad}) {
        h.bridge->observe(f);
        pumpFor(HostController::kAmbientDebounceMs * 2);
        ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    }
    h.bridge->observe(elevated);
    pumpFor(HostController::kAmbientDebounceMs * 2);
    ROCLINT_CHECK(h.c().ambientStatus().contains(QStringLiteral("administrator")));
    ROCLINT_CHECK(*h.calls == 0);
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(!h.c().isAmbientCapture());
    h.c().hideCard();

    // Empty fields aren't prompts; short prompts still need ambiguity checks.
    h.bridge->observe(cursorField(QStringLiteral("  \n")));
    pumpFor(HostController::kAmbientDebounceMs * 2);
    ROCLINT_CHECK(*h.calls == 0);
    std::cout << "  [PASS] Password, unsupported, elevated and short fields are never checked.\n";
}

void testClearPromptShowsNoBadge() {
    Harness h(AccessibilityAccess::Granted, kClearReply);
    h.c().setAmbientModeEnabled(true);
    h.bridge->observe(cursorField(kPrompt));
    ROCLINT_CHECK(pumpUntil([&]() { return h.c().ambientStatus().contains(QStringLiteral("looked clear")); }));
    ROCLINT_CHECK(*h.calls == 1);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    // The same text is never checked twice.
    h.bridge->observe(cursorField(kPrompt));
    pumpFor(HostController::kAmbientDebounceMs * 2);
    ROCLINT_CHECK(*h.calls == 1);
    std::cout << "  [PASS] A clear prompt stays silent and is not re-checked.\n";
}

void testDismissedBadgeStaysDismissed() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    observeAndWaitForBadge(h);
    h.c().dismissAmbientBadge();
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    h.bridge->observe(cursorField(kPrompt));
    pumpFor(HostController::kAmbientDebounceMs * 2);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    std::cout << "  [PASS] A dismissed badge doesn't come back for the same text.\n";
}

// Opens the card from Cursor, answers the question and produces an approvable rewrite.
void reachApprovableRewrite(Harness& h) {
    h.c().setAmbientModeEnabled(true);
    observeAndWaitForBadge(h);
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().confirmChoice(QStringLiteral("alt_0_0"), h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().synthesizeRewrite(h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().canApprove());
}

void testApprovedRewriteIsInjectedInPlace() {
    Harness h;
    reachApprovableRewrite(h);
    setClipboard(QStringLiteral("untouched"));
    QString injected;
    QObject::connect(h.controller.get(), &HostController::promptInjected,
                     [&](const QString& text, const QString&) { injected = text; });

    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().approveAndCopyRewrite(h.c().stateVersion())));
    ROCLINT_CHECK(h.c().isCardVisible()); // No dismissal before the asynchronous readback result.
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    const auto& request = h.bridge->injections[0];
    ROCLINT_CHECK(request.text == QString::fromStdString(kRewrite));
    ROCLINT_CHECK(request.expectedCurrentText == kPrompt);
    ROCLINT_CHECK(request.fieldToken == 7);
    ROCLINT_CHECK(request.kind == FieldKind::GuiEditor);
    ROCLINT_CHECK(pumpUntil([&]() { return !injected.isEmpty(); }));
    ROCLINT_CHECK(!h.c().isCardVisible());
    ROCLINT_CHECK(injected == QString::fromStdString(kRewrite));
    ROCLINT_CHECK(clipboardText() == "untouched");
    auto approval = h.c().repositoryForTesting()->getCurrentApproval(SessionId(h.c().currentSessionIdString().toStdString())).waitForTesting();
    ROCLINT_CHECK(approval.ok() && approval.value().has_value());
    std::cout << "  [PASS] Approve & Insert persists the approval and injects into the captured field.\n";
}

void testConnectingEditorPreservesManualRewriteAndAnswers() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    h.c().showCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().confirmChoice(QStringLiteral("alt_0_0"), h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().synthesizeRewrite(h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
    ROCLINT_CHECK(h.c().canApprove() && !h.c().isAmbientCapture());
    const auto version = h.c().stateVersion();
    const auto proposal = *h.c().proposalForTesting();
    const auto intent = *h.c().confirmedIntentForTesting();
    const auto revision = h.c().currentRevisionId();
    const auto rounds = h.c().roundsCompleted();
    const int calls = *h.calls;
    QString instructions;
    QObject::connect(&h.c(), &HostController::insertionTargetSelectionRequested, [&](const QString& text) { instructions = text; });
    setClipboard(QStringLiteral("Keep this clipboard"));
    ROCLINT_CHECK(h.c().beginInsertionTargetSelection(version));
    ROCLINT_CHECK(!h.c().isCardVisible() && instructions.contains(h.c().shortcutLabel()));
    ROCLINT_CHECK(!h.c().hasApproval() && h.bridge->injections.empty());
    auto field = cursorField(QStringLiteral("Existing text in the destination composer"), 88);
    field.appIdentifier = QStringLiteral("ChatGPT.exe"); field.appDisplayName = QStringLiteral("Codex");
    h.bridge->observe(field);
    roclint::host::HostContextDetector::setMockObservation({
        roclint::host::HostContextObservation{roclint::host::HostContextCategory::CodeEditor,
                                             field.appDisplayName, field.appIdentifier}});
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().isAmbientCapture() && h.c().isCardVisible());
    ROCLINT_CHECK(h.c().stateVersion() == version && h.c().currentRevisionId() == revision);
    ROCLINT_CHECK(h.c().roundsCompleted() == rounds && h.c().confirmedIntentForTesting()->intentHash == intent.intentHash);
    ROCLINT_CHECK(h.c().proposalForTesting()->proposalHash == proposal.proposalHash);
    ROCLINT_CHECK(h.c().currentRevisionText() == kPrompt && *h.calls == calls);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().approveAndCopyRewrite(version)));
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    ROCLINT_CHECK(h.bridge->injections[0].text == QString::fromStdString(kRewrite));
    ROCLINT_CHECK(h.bridge->injections[0].fieldToken == 88 && h.bridge->injections[0].expectedCurrentText == field.text);
    ROCLINT_CHECK(clipboardText() == "Keep this clipboard");
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().isCardVisible(); }));
}

void testInjectionFailureFallsBackToClipboard() {
    Harness h;
    reachApprovableRewrite(h);
    h.bridge->nextOutcome = InjectionOutcome::Failed;
    setClipboard(QStringLiteral("before"));
    QString fallback;
    QObject::connect(h.controller.get(), &HostController::deliveryFellBack,
                     [&](const QString& message) { fallback = message; });

    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().approveAndCopyRewrite(h.c().stateVersion())));
    ROCLINT_CHECK(pumpUntil([&]() { return !fallback.isEmpty(); }));
    ROCLINT_CHECK(h.c().isCardVisible());
    ROCLINT_CHECK(clipboardText() == QString::fromStdString(kRewrite));
    ROCLINT_CHECK(h.c().lastCopiedText() == QString::fromStdString(kRewrite));
    ROCLINT_CHECK(h.c().lastError().contains(QStringLiteral("clipboard")));
    ROCLINT_CHECK(fallback.contains(QStringLiteral("Cursor")));
    std::cout << "  [PASS] A failed injection falls back to the clipboard and says so.\n";
}

void testHardBypassInWidgetMode() {
    Harness h;
    h.c().showCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    setClipboard(QStringLiteral("old clipboard"));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(QStringLiteral("ship it exactly like this"), 0)));
    ROCLINT_CHECK(clipboardText() == "ship it exactly like this");
    ROCLINT_CHECK(h.c().isCardVisible());
    ROCLINT_CHECK(h.c().analysisMessage().contains("No editor is bound"));
    ROCLINT_CHECK(*h.calls == 0);
    ROCLINT_CHECK(!h.c().analysisForTesting().has_value());
    ROCLINT_CHECK(h.bridge->injections.empty());
    std::cout << "  [PASS] Shift+Enter in widget mode copies the raw text with no model call.\n";
}

void testHardBypassInAmbientMode() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    observeAndWaitForBadge(h);
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().isClarifying());
    const int callsBefore = *h.calls;
    setClipboard(QStringLiteral("untouched"));

    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(h.c().currentRevisionText(), h.c().stateVersion())));
    ROCLINT_CHECK(h.c().isCardVisible());
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    ROCLINT_CHECK(h.bridge->injections[0].text == kPrompt);
    ROCLINT_CHECK(!h.c().analysisForTesting().has_value());
    ROCLINT_CHECK(!h.c().isClarifying());
    pumpFor(100);
    ROCLINT_CHECK(!h.c().isCardVisible());
    ROCLINT_CHECK(*h.calls == callsBefore);
    ROCLINT_CHECK(*h.rewriteCalls == 0);
    ROCLINT_CHECK(clipboardText() == "untouched");
    std::cout << "  [PASS] Shift+Enter in ambient mode injects the raw prompt and skips analysis and rewrite.\n";
}

void testCodexShortPromptCapturePreservesWhitespace() {
    Harness h;
    h.c().setLegacyDiscoveryForTesting(false);
    h.c().setAmbientModeEnabled(true);
    auto field = cursorField(QStringLiteral("  fix it\n"));
    field.appIdentifier = QStringLiteral("ChatGPT.exe");
    field.appDisplayName = QStringLiteral("Codex");
    h.bridge->observe(field);
    ROCLINT_CHECK(pumpUntil([&]() { return h.c().isAmbientBadgeVisible(); }));
    ROCLINT_CHECK(*h.calls == 0); // Typing never runs Granite ahead of S1.
    h.c().showCard(); // Tray and hotkey must use the same captured target.
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().isAmbientCapture());
    ROCLINT_CHECK(h.c().currentRevisionText() == field.text);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(field.text, h.c().stateVersion())));
    ROCLINT_CHECK(h.c().isCardVisible());
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    ROCLINT_CHECK(h.bridge->injections[0].expectedCurrentText == field.text);
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().isCardVisible(); }));
    ROCLINT_CHECK(*h.rewriteCalls == 0);
    std::cout << "  [PASS] Codex short drafts retain whitespace and their insertion target.\n";
}

void testStaleInsertionKeepsDraftAndRejectsDuplicateDelivery() {
    Harness h;
    reachApprovableRewrite(h);
    h.bridge->nextOutcome = InjectionOutcome::FieldChanged;
    const auto first = h.c().approveAndCopyRewrite(h.c().stateVersion());
    const auto duplicate = h.c().approveAndCopyRewrite(h.c().stateVersion());
    ROCLINT_CHECK(h.c().waitForCommandForTesting(first));
    ROCLINT_CHECK(!h.c().waitForCommandForTesting(duplicate));
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().lastCopiedText().isEmpty(); }));
    ROCLINT_CHECK(h.c().isCardVisible());
    ROCLINT_CHECK(h.c().ambientStatus().contains(QStringLiteral("clipboard")));
}

void testDeliveryRemainsBoundWhenOtherFieldsReceiveFocus() {
    Harness h;
    reachApprovableRewrite(h);
    h.bridge->observe(cursorField(QStringLiteral("An unrelated draft in another field"), 99));
    h.bridge->lose();
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().approveAndCopyRewrite(h.c().stateVersion())));
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    const auto& request = h.bridge->injections.front();
    ROCLINT_CHECK(request.fieldToken == 7 && request.expectedCurrentText == kPrompt);
    ROCLINT_CHECK(request.text == QString::fromStdString(kRewrite));
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().isCardVisible(); }));
}

void testTypingIsObservedWhileWidgetRemainsOpen() {
    Harness h;
    h.c().setLegacyDiscoveryForTesting(false);
    h.c().setAmbientModeEnabled(true);
    h.c().showCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    auto field = cursorField(QStringLiteral("Check this new draft"));
    field.appIdentifier = QStringLiteral("ChatGPT.exe");
    h.bridge->observe(field);
    roclint::host::HostContextDetector::setMockObservation({
        roclint::host::HostContextObservation{roclint::host::HostContextCategory::CodeEditor,
                                             QStringLiteral("Codex"), field.appIdentifier}});
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    roclint::host::HostContextDetector::resetMock();
    ROCLINT_CHECK(h.c().isCardVisible() && h.c().isAmbientCapture());
    ROCLINT_CHECK(h.c().currentRevisionText() == field.text);
}

void testInvocationRefreshesFieldBeforeStealingFocus() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    roclint::host::HostContextDetector::setMockObservation({
        roclint::host::HostContextObservation{roclint::host::HostContextCategory::CodeEditor,
                                             QStringLiteral("Codex"), QStringLiteral("ChatGPT.exe")}});
    setClipboard(QStringLiteral("Unrelated clipboard must never be imported"));
    h.c().showCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.bridge->refreshRequests == 1);
    ROCLINT_CHECK(!h.c().isCardVisible());
    auto field = cursorField(kPrompt);
    field.appIdentifier = QStringLiteral("ChatGPT.exe");
    h.bridge->observe(field);
    ROCLINT_CHECK(pumpUntil([&]() { return h.c().isCardVisible(); }));
    ROCLINT_CHECK(h.c().isCardVisible() && h.c().isAmbientCapture());
    ROCLINT_CHECK(h.c().currentRevisionText() == kPrompt);
    ROCLINT_CHECK(clipboardText() == "Unrelated clipboard must never be imported");
    roclint::host::HostContextDetector::resetMock();
}

void testOpenHudFollowsFocusedDraftUntilHudEdit() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    auto field = cursorField(kPrompt);
    h.bridge->observe(field);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
    const auto frozen = h.c().currentRevisionText();
    QString visibleDraft;
    int updates = 0;
    QObject::connect(&h.c(), &HostController::ambientDraftUpdated, [&](const QString& text) {
        visibleDraft = text; ++updates;
    });
    const auto calls = *h.calls;
    field.text = QStringLiteral("  A longer external draft\n");
    h.bridge->observe(field);
    ROCLINT_CHECK(updates == 1 && visibleDraft == field.text);
    ROCLINT_CHECK(h.c().currentRevisionText() == frozen); // Frozen evidence never changes in place.
    ROCLINT_CHECK(h.c().isDraftDirty() && !h.c().isClarifying());
    field.text.clear();
    h.bridge->observe(field);
    ROCLINT_CHECK(updates == 2 && visibleDraft.isEmpty());
    auto otherField = field;
    otherField.fieldToken++;
    otherField.appIdentifier = QStringLiteral("ChatGPT.exe");
    otherField.appDisplayName = QStringLiteral("Codex");
    otherField.processId++;
    otherField.text = QStringLiteral("A different prompt box");
    h.bridge->observe(otherField);
    ROCLINT_CHECK(updates == 3 && visibleDraft == otherField.text);
    ROCLINT_CHECK(h.c().deliveryLabel() == "Delivers into Codex");
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().noteDraftEdited(h.c().stateVersion()))); // Human now edits in the HUD.
    field.text = QStringLiteral("External text after the HUD edit");
    h.bridge->observe(field);
    ROCLINT_CHECK(updates == 3); // Do not erase the human's HUD draft.
    pumpFor(HostController::kAmbientDebounceMs + 30);
    ROCLINT_CHECK(*h.calls == calls); // No inference on keystrokes.
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(QStringLiteral("The user's local HUD draft"), h.c().stateVersion())));
    ROCLINT_CHECK(h.bridge->injections.back().expectedCurrentText == otherField.text); // Retain the stale guard.
}

void testEmptyComposerIsStillAnInsertionTarget() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    auto field = cursorField(QString());
    field.appIdentifier = QStringLiteral("ChatGPT.exe");
    field.appDisplayName = QStringLiteral("Codex");
    h.bridge->observe(field);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    h.c().showCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().isAmbientCapture());
    ROCLINT_CHECK(h.c().currentRevisionText().isEmpty());
    ROCLINT_CHECK(*h.calls == 0);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(QStringLiteral("A draft authored in the HUD"), 0)));
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    ROCLINT_CHECK(h.bridge->injections[0].expectedCurrentText.isEmpty());
    ROCLINT_CHECK(h.bridge->injections[0].text == "A draft authored in the HUD");
}

void testInvocationWaitsForFreshFieldInsteadOfCachedTarget() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    h.bridge->observe(cursorField(QStringLiteral("A stale field from earlier"), 19));
    h.bridge->echoOnRefresh = false;
    roclint::host::HostContextDetector::setMockObservation({
        roclint::host::HostContextObservation{roclint::host::HostContextCategory::CodeEditor,
                                             QStringLiteral("Cursor"), QStringLiteral("Cursor.exe")}});
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.bridge->refreshRequests == 1 && !h.c().isCardVisible());
    auto fresh = cursorField(kPrompt, 27);
    h.bridge->observe(fresh);
    ROCLINT_CHECK(pumpUntil([&]() { return h.c().isCardVisible(); }));
    ROCLINT_CHECK(h.c().isAmbientCapture() && h.c().isCardVisible());
    ROCLINT_CHECK(h.c().currentRevisionText() == kPrompt);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(QStringLiteral("Changed in the HUD"), h.c().stateVersion())));
    ROCLINT_CHECK(h.bridge->injections.front().fieldToken == 27);
    ROCLINT_CHECK(h.bridge->injections.front().expectedCurrentText == kPrompt);
    roclint::host::HostContextDetector::resetMock();
}

void testLateEditorBindsWithoutAnotherHotkeyAndPreservesHudEdits() {
    for (const bool editedHud : {false, true}) {
        Harness h;
        h.c().setAmbientModeEnabled(true);
        h.bridge->echoOnRefresh = false;
        roclint::host::HostContextDetector::setMockObservation({
            roclint::host::HostContextObservation{roclint::host::HostContextCategory::CodeEditor,
                                                 QStringLiteral("Cursor"), QStringLiteral("Cursor.exe")}});
        h.c().showCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
        pumpFor(1350);
        ROCLINT_CHECK(h.c().isCardVisible() && !h.c().isAmbientCapture());
        ROCLINT_CHECK(!h.c().lastError().isEmpty());
        QString adopted;
        int updates = 0;
        QObject::connect(&h.c(), &HostController::ambientDraftUpdated, [&](const QString& text) {
            adopted = text; ++updates;
        });
        if (editedHud) ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().noteDraftEdited(h.c().stateVersion())));
        auto field = cursorField(QStringLiteral("A field exposed after the HUD opened"), 67);
        h.bridge->observe(field);
        ROCLINT_CHECK(h.c().isAmbientCapture() && h.c().lastError().isEmpty());
        ROCLINT_CHECK(updates == (editedHud ? 0 : 1));
        if (!editedHud) ROCLINT_CHECK(adopted == field.text);
        ROCLINT_CHECK(*h.calls == 0 && h.bridge->refreshRequests == 1);
        const auto output = editedHud ? QStringLiteral("Preserved HUD draft") : field.text;
        ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(output, h.c().stateVersion())));
        ROCLINT_CHECK(h.bridge->injections.back().fieldToken == 67);
        ROCLINT_CHECK(h.bridge->injections.back().expectedCurrentText == field.text);
    }
}

void testRestoringHistoryPausesLiveFollowing() {
    for (const bool wholeSession : {false, true}) {
        Harness h;
        h.c().setAmbientModeEnabled(true);
        auto field = cursorField(kPrompt);
        h.bridge->observe(field);
        ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
        ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
        const auto revision = h.c().currentRevisionId();
        int updates = 0;
        QObject::connect(&h.c(), &HostController::ambientDraftUpdated, [&](const QString&) { ++updates; });
        if (wholeSession) ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().restoreSession(h.c().currentSessionIdString(), h.c().stateVersion())));
        else ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().restoreRevision(revision, h.c().stateVersion())));
        field.text = QStringLiteral("External typing after a history restore");
        h.bridge->observe(field);
        ROCLINT_CHECK(updates == 0 && h.c().currentRevisionText() == kPrompt && !h.c().isDraftDirty());
    }
}

void testTerminalDraftIsCapturedAndReplaced() {
    Harness h(AccessibilityAccess::Granted, kClearReply);
    h.c().setAmbientModeEnabled(true);
    ObservedField term;
    term.fieldToken = 11;
    term.appIdentifier = QStringLiteral("WindowsTerminal.exe");
    term.appDisplayName = QStringLiteral("WindowsTerminal");
    term.kind = FieldKind::Terminal;
    term.text = QString::fromUtf8("Tests: 3 passed > ok\n\xE2\x9D\xAF fix the bugs in the project");
    term.caretOffset = static_cast<int>(term.text.size());
    h.bridge->observe(term);
    ROCLINT_CHECK(pumpUntil([&]() { return h.c().ambientStatus().contains(QStringLiteral("looked clear")); }));
    ROCLINT_CHECK(*h.calls == 1);

    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
    ROCLINT_CHECK(h.c().currentRevisionText() == "fix the bugs in the project");
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(h.c().currentRevisionText(), h.c().stateVersion())));
    ROCLINT_CHECK(h.bridge->injections.size() == 1);
    const auto& r = h.bridge->injections[0];
    ROCLINT_CHECK(r.kind == FieldKind::Terminal);
    ROCLINT_CHECK(r.fieldToken == 11);
    ROCLINT_CHECK(r.expectedCurrentText == "fix the bugs in the project");
    std::cout << "  [PASS] Terminal drafts are isolated from output and replaced through the terminal path.\n";
}

void testPlatformBridgeStartsAndStopsWithoutObserving() {
#if defined(Q_OS_WIN)
    auto bridgeImpl = roclint::host::createPlatformAccessibilityBridge();
    ROCLINT_CHECK(pumpUntil([&]() { return bridgeImpl->access() == AccessibilityAccess::Granted; }, 5000));
    // Unknown or stale targets are refused without touching any other application.
    bool finished = false;
    InjectionOutcome outcome = InjectionOutcome::Injected;
    QObject::connect(bridgeImpl.get(), &roclint::host::IAccessibilityBridge::injectionFinished,
                     [&](quint64, InjectionOutcome o, const QString&) {
                         finished = true;
                         outcome = o;
                     });
    roclint::host::InjectionRequest stale;
    stale.requestId = 1;
    stale.fieldToken = 999;
    stale.text = QStringLiteral("never written");
    bridgeImpl->inject(stale);
    ROCLINT_CHECK(pumpUntil([&]() { return finished; }, 5000));
    ROCLINT_CHECK(outcome == InjectionOutcome::FieldChanged);
    bridgeImpl.reset();
    std::cout << "  [PASS] The Windows UI Automation bridge starts, refuses stale targets, and shuts down.\n";
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    std::cout << "[Test Dual Mode] Starting...\n";
    testAmbientRouteWhenPermissionGranted();
    testWidgetFallbackWhenPermissionDenied();
    testAmbientOffUsesWidgetEvenWithPermission();
    testIneligibleFieldsAreNeverRead();
    testClearPromptShowsNoBadge();
    testDismissedBadgeStaysDismissed();
    testApprovedRewriteIsInjectedInPlace();
    testConnectingEditorPreservesManualRewriteAndAnswers();
    testInjectionFailureFallsBackToClipboard();
    testHardBypassInWidgetMode();
    testHardBypassInAmbientMode();
    testTerminalDraftIsCapturedAndReplaced();
    testCodexShortPromptCapturePreservesWhitespace();
    testStaleInsertionKeepsDraftAndRejectsDuplicateDelivery();
    testDeliveryRemainsBoundWhenOtherFieldsReceiveFocus();
    testTypingIsObservedWhileWidgetRemainsOpen();
    testOpenHudFollowsFocusedDraftUntilHudEdit();
    testInvocationRefreshesFieldBeforeStealingFocus();
    testEmptyComposerIsStillAnInsertionTarget();
    testInvocationWaitsForFreshFieldInsteadOfCachedTarget();
    testLateEditorBindsWithoutAnotherHotkeyAndPreservesHudEdits();
    testRestoringHistoryPausesLiveFollowing();
    testPlatformBridgeStartsAndStopsWithoutObserving();
    std::cout << "[Test Dual Mode] All tests PASSED.\n";
    return 0;
}
