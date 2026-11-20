#include "HostController.hpp"
#include "HostContext.hpp"
#include "support/MockAccessibilityBridge.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QMutex>

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
                     const std::string& discoveryReply = kAmbiguousReply,
                     const QString& database = QStringLiteral(":memory:")) {
        // Native desktop focus must not decide a controller unit test's invocation path.
        roclint::host::HostContextDetector::setMockObservation(roclint::host::HostContextObservation{});
        auto mock = std::make_unique<MockAccessibilityBridge>();
        mock->accessValue = access;
        bridge = mock.get();
        controller = std::make_unique<HostController>(database, std::move(mock));
        ROCLINT_CHECK(controller->waitForStorageForTesting());
        controller->setLegacyDiscoveryForTesting(true);
        auto c = calls;
        auto r = rewriteCalls;
        const std::string rewrite = fx::rewriteReply(kRewrite);
        controller->setTransportOverrideForTesting(
            [c, r, discoveryReply, rewrite](std::string_view instruction) -> Result<std::string> {
                if (instruction.find("granite.goldfish_audit.v2") != std::string_view::npos) {
                    return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
                }
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

void completeApprovingReview(HostController& controller);

void testIndependentFieldSessions(bool insertAfterFocusLoss) {
    Harness h;
    h.c().setFieldSessionsForTesting(true);
    h.c().setAmbientModeEnabled(true);
    auto first = cursorField(kPrompt);
    first.appIdentifier = QStringLiteral("com.google.antigravity");
    auto second = cursorField("Write a different prompt here", 42);
    second.appIdentifier = QStringLiteral("com.microsoft.VSCode");
    second.processId++;
    h.bridge->observe(first);
    const auto opening = h.c().openFromAmbient();
    ROCLINT_CHECK(h.c().waitForCommandForTesting(opening));
    const auto firstSession = h.c().currentSessionIdString();
    h.bridge->observe(second);
    ROCLINT_CHECK(h.c().otherFieldVisible() && h.c().focusedFieldDraft() == second.text);
    second.text.clear();
    h.bridge->observe(second);
    ROCLINT_CHECK(h.c().focusedFieldDraft().isEmpty());
    second.text = QStringLiteral("Retyped in a different field");
    h.bridge->observe(second);
    ROCLINT_CHECK(h.c().focusedFieldDraft() == second.text);
    ROCLINT_CHECK(pumpUntil([&] { return h.c().isClarifying(); }));
    const auto question = h.c().activeQuestion();
    const auto options = h.c().activeAlternatives();
    ROCLINT_CHECK(h.c().sessionCount() == 1);
    h.c().reviewFocusedField();
    ROCLINT_CHECK(pumpUntil([&] { return !h.c().otherFieldVisible() && h.c().currentRevisionText() == second.text; }));
    ROCLINT_CHECK(h.c().currentSessionIdString() != firstSession);
    h.bridge->observe(first);
    ROCLINT_CHECK(h.c().focusedFieldDraft() == first.text);
    ROCLINT_CHECK(pumpUntil([&] { return !h.c().otherFieldVisible() && h.c().currentSessionIdString() == firstSession; }));
    roclint::host::HostContextDetector::setMockObservation({{
        roclint::host::HostContextCategory::CodeEditor, first.appDisplayName, first.appIdentifier}});
    h.bridge->lose();
    h.bridge->lose();
    ROCLINT_CHECK(h.c().retainedFieldReviewVisible() && h.c().focusedFieldDraft().isEmpty());
    ROCLINT_CHECK(h.c().activeQuestion() == question && h.c().activeAlternatives() == options);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().confirmChoice("alt_0_0", h.c().stateVersion())));
    ROCLINT_CHECK(h.c().hasConfirmedIntent());
    h.bridge->observe(second);
    ROCLINT_CHECK(pumpUntil([&] { return !h.c().otherFieldVisible() && h.c().currentSessionIdString() != firstSession; }));
    h.bridge->observe(first);
    ROCLINT_CHECK(pumpUntil([&] { return !h.c().otherFieldVisible() && h.c().hasConfirmedIntent(); }));
    if (insertAfterFocusLoss) {
        ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().synthesizeRewrite(h.c().stateVersion())));
        ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
        completeApprovingReview(h.c());
        h.bridge->lose();
        ROCLINT_CHECK(h.c().retainedFieldReviewVisible());
        ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().approveAndCopyRewrite(h.c().stateVersion())));
        ROCLINT_CHECK(h.bridge->injections.size() == 1);
        const auto& delivery = h.bridge->injections.front();
        ROCLINT_CHECK(delivery.appIdentifier == first.appIdentifier && delivery.processId == first.processId &&
                      delivery.fieldToken == first.fieldToken && delivery.expectedCurrentText == first.text &&
                      delivery.text == QString::fromStdString(kRewrite));
        return;
    }
    first.text += " New requirements.";
    h.bridge->observe(first);
    ROCLINT_CHECK(!h.c().hasConfirmedIntent() && h.c().focusedFieldDraft() == first.text);
    auto sibling = first;
    ++sibling.fieldToken;
    h.bridge->observe(sibling);
    ROCLINT_CHECK(h.c().otherFieldVisible());
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
}

void testQuietIndicatorFollowsAnotherEditorAfterReview() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    h.c().setFieldSessionsForTesting(true);
    auto first = cursorField(kPrompt);
    h.bridge->observe(first);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    const auto session = h.c().currentSessionIdString();
    h.c().hideCard();
    h.c().setLegacyDiscoveryForTesting(false);
    auto next = cursorField("A second draft", 42);
    next.appIdentifier = QStringLiteral("com.microsoft.VSCode");
    next.processId++;
    h.bridge->observe(next);
    ROCLINT_CHECK(h.c().isAmbientBadgeVisible() && h.c().focusedFieldDraft() == next.text);
    next.text.clear();
    h.bridge->observe(next);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    ROCLINT_CHECK(h.c().currentSessionIdString() == session);
}

void testChoiceWhileAnotherEditorIsWorking(bool editWhileQueued) {
    Harness h;
    h.c().setFieldSessionsForTesting(true);
    h.c().setAmbientModeEnabled(true);
    auto chat = cursorField(kPrompt);
    chat.appDisplayName = "ChatGPT";
    h.bridge->observe(chat);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
    ROCLINT_CHECK(pumpUntil([&] { return h.c().isClarifying(); }));
    const auto chatSession = h.c().currentSessionIdString();
    QSemaphore started, release;
    h.c().setTransportOverrideForTesting([&](std::string_view prompt) -> Result<std::string> {
        if (prompt.find("retro console") != std::string_view::npos) {
            started.release();
            release.tryAcquire(1, 5000);
            return kClearReply;
        }
        if (prompt.find("<instructions>") != std::string_view::npos) return fx::rewriteReply(kRewrite);
        return kClearReply;
    });
    auto retro = cursorField("Design a retro console", 42);
    retro.appIdentifier = "com.google.antigravity";
    retro.appDisplayName = "Antigravity";
    h.bridge->observe(retro);
    h.c().reviewFocusedField();
    ROCLINT_CHECK(pumpUntil([&] { return started.available() > 0; }));
    h.bridge->observe(chat);
    ROCLINT_CHECK(h.c().pipelineEditorName() == "Antigravity" && !h.c().focusedFieldChoices().isEmpty());
    h.c().queueFocusedFieldChoice(h.c().focusedFieldChoices().first().toMap());
    ROCLINT_CHECK(h.c().fieldReviewQueued());
    if (editWhileQueued) {
        chat.text += " Updated scope.";
        h.bridge->observe(chat);
        ROCLINT_CHECK(!h.c().fieldReviewQueued());
    }
    release.release(10);
    ROCLINT_CHECK(pumpUntil([&] { return h.c().currentSessionIdString() == chatSession && !h.c().otherFieldVisible(); }, 5000));
    ROCLINT_CHECK(h.c().hasConfirmedIntent() == !editWhileQueued);
    ROCLINT_CHECK(h.bridge->injections.empty());
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
}

void testExplicitReviewPrecedesCoalescedEdits() {
    Harness h(AccessibilityAccess::Granted, kClearReply);
    h.c().setFieldSessionsForTesting(true);
    h.c().setAmbientModeEnabled(true);
    auto automatic = cursorField("Initial automatic field", 3);
    const auto running = cursorField("Hold the running review", 4);
    const auto explicitField = cursorField("Prioritize my explicit review", 5);
    h.bridge->observe(automatic);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    QSemaphore started, release;
    std::atomic_bool held{false};
    QMutex mutex;
    QStringList order;
    h.c().setTransportOverrideForTesting([&](std::string_view instruction) -> Result<std::string> {
        if (instruction.find("granite.goldfish_audit.v2") != std::string_view::npos ||
            instruction.find("granite.goldfish_discovery_audit.v1") != std::string_view::npos)
            return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
        if (instruction.find("Hold the running review") != std::string_view::npos && !held.exchange(true)) {
            started.release();
            release.tryAcquire(1, 5000);
        }
        QMutexLocker lock(&mutex);
        for (const auto phrase : {"Hold the running review", "Prioritize my explicit review", "Intermediate text", "Latest edit"})
            if (instruction.find(phrase) != std::string_view::npos) order.append(QString::fromLatin1(phrase));
        return kClearReply;
    });
    h.bridge->observe(running);
    h.c().reviewFocusedField();
    ROCLINT_CHECK(pumpUntil([&] { return started.available() > 0; }));
    h.bridge->observe(automatic);
    automatic.text = "Intermediate text";
    h.bridge->observe(automatic);
    automatic.text = "Latest edit";
    h.bridge->observe(automatic);
    ROCLINT_CHECK(h.c().focusedFieldDraft() == automatic.text);
    pumpFor(850); // The automatic check becomes eligible while the explicit review still owns the worker.
    h.bridge->observe(explicitField);
    h.c().reviewFocusedField();
    ROCLINT_CHECK(pumpUntil([&] { return h.c().fieldReviewQueued(); }));
    release.release();
    const bool finished = pumpUntil([&] { QMutexLocker lock(&mutex); return order.size() >= 3; }, 5000);
    if (!finished) {
        QMutexLocker lock(&mutex);
        qWarning() << "Unexpected field review order" << order << h.c().lastError() << h.c().currentRevisionText() << h.c().analysisMessage();
    }
    ROCLINT_CHECK(finished);
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    QMutexLocker lock(&mutex);
    ROCLINT_CHECK(order == QStringList({running.text, explicitField.text, automatic.text}));
}

void testFieldResumeAfterRestart() {
    QTemporaryDir directory;
    ROCLINT_CHECK(directory.isValid());
    const auto database = directory.filePath("fields.sqlite");
    QString savedId;
    auto field = cursorField(kPrompt);
    {
        Harness h(AccessibilityAccess::Granted, kAmbiguousReply, database);
        h.c().setFieldSessionsForTesting(true);
        h.c().setAmbientModeEnabled(true);
        h.bridge->observe(field);
        ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
        savedId = h.c().currentSessionIdString();
        field.text += " This part is still a draft.";
        h.bridge->observe(field);
        bool quit = false;
        QObject::connect(&h.c(), &HostController::quitRequested, [&] { quit = true; });
        h.c().requestQuit();
        ROCLINT_CHECK(pumpUntil([&] { return quit; }, 5000));
    }
    Harness h(AccessibilityAccess::Granted, kAmbiguousReply, database);
    h.c().setFieldSessionsForTesting(true);
    h.c().setAmbientModeEnabled(true);
    auto fresh = cursorField("A new prompt after reopening", 81);
    h.bridge->observe(fresh);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
    ROCLINT_CHECK(pumpUntil([&] { return !h.c().matchingFieldDrafts().isEmpty(); }));
    ROCLINT_CHECK(h.c().currentSessionIdString() != savedId);
    QString draft;
    QObject::connect(&h.c(), &HostController::revisionRestored, [&](QString text) { draft = text; });
    h.c().resumeFieldDraft(savedId);
    ROCLINT_CHECK(pumpUntil([&] { return h.c().currentSessionIdString() == savedId && draft == field.text; }));
    ROCLINT_CHECK(!h.c().isAmbientAttached() && !h.c().isClarifying() && h.bridge->injections.empty());
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
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

void testQuietBadgeSurvivesTypingAndSameWindowReadGaps() {
    int hiddenUpdates = 0;
    Harness h;
    h.c().setLegacyDiscoveryForTesting(false);
    h.c().setAmbientModeEnabled(true);
    auto field = cursorField(kPrompt);
    field.fieldRect = QRect(480, 370, 310, 110);
    h.bridge->observe(field);
    ROCLINT_CHECK(h.c().isAmbientBadgeVisible());
    h.c().setAmbientBadgeSuggestionsOnly(true);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    h.c().setAmbientBadgeSuggestionsOnly(false);
    ROCLINT_CHECK(h.c().isAmbientBadgeVisible());
    const QPoint initialPosition(h.c().ambientBadgeX(), h.c().ambientBadgeY());
    // A queued click on an expired underline must never open the full HUD.
    h.c().activateAmbientIndicator();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
    ROCLINT_CHECK(!h.c().isCardVisible() && !h.c().wordCompanionVisible());
    const auto updates = QObject::connect(&h.c(), &HostController::ambientBadgeChanged, &h.c(), [&]() {
        if (!h.c().isAmbientBadgeVisible()) ++hiddenUpdates;
    });
    field.text += QStringLiteral(" Keep the API unchanged.");
    field.caretOffset = static_cast<int>(field.text.size());
    h.bridge->observe(field);
    ROCLINT_CHECK(h.c().isAmbientBadgeVisible());
    ROCLINT_CHECK(QPoint(h.c().ambientBadgeX(), h.c().ambientBadgeY()) == initialPosition);

    emit h.bridge->fieldReadInterrupted();
    ROCLINT_CHECK(h.c().isAmbientBadgeVisible());
    ROCLINT_CHECK(h.c().applyWordRefinement(h.c().stateVersion()) == 0);
    h.c().activateAmbientIndicator();
    ROCLINT_CHECK(!h.c().isCardVisible() && !h.c().wordCompanionVisible());
    ++field.fieldToken; // The provider may recreate a composer node with the same text.
    h.bridge->observe(field);
    ROCLINT_CHECK(h.c().isAmbientBadgeVisible() && hiddenUpdates == 0);
    QObject::disconnect(updates);

    emit h.bridge->fieldReadInterrupted();
    pumpFor(400);
    emit h.bridge->fieldReadInterrupted();
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().isAmbientBadgeVisible(); }, 450));
    h.bridge->observe(field);
    ROCLINT_CHECK(h.c().isAmbientBadgeVisible());
    emit h.bridge->fieldReadInterrupted();
    h.bridge->lose();
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    h.bridge->observe(field);
    h.bridge->observe(cursorField(QString(), field.fieldToken + 1));
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    h.bridge->observe(field);
    h.c().dismissAmbientBadge();
    h.bridge->observe(field);
    ROCLINT_CHECK(!h.c().isAmbientBadgeVisible());
    std::cout << "  [PASS] The quiet badge stays anchored through edits and short read interruptions.\n";
}

// Opens the card from Cursor, answers the question and produces an approvable rewrite.
void completeApprovingReview(HostController& controller) {
    controller.setComprehensionReviewForTesting(
        [](const roclint::bridge::ComprehensionReviewRequest&) {
            roclint::bridge::ComprehensionReviewResult result;
            result.status = roclint::bridge::ComprehensionReviewStatus::Completed;
            result.interpretedOutcome = "The rewrite preserves the requested parser refactor.";
            result.readyForTask = true;
            return result;
        });
    ROCLINT_CHECK(controller.requestComprehensionReview(controller.stateVersion()) != 0);
    ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
    ROCLINT_CHECK(controller.comprehensionReviewState() == "ready");
}

void reachApprovableRewrite(Harness& h) {
    h.c().setAmbientModeEnabled(true);
    observeAndWaitForBadge(h);
    h.c().toggleCard();
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().confirmChoice(QStringLiteral("alt_0_0"), h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().synthesizeRewrite(h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    completeApprovingReview(h.c());
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
    ROCLINT_CHECK(h.c().isCardVisible());
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
    completeApprovingReview(h.c());
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
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().lastCopiedText().isEmpty(); }));
    ROCLINT_CHECK(h.c().isCardVisible());
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

void testExplicitHideSurvivesDeliveryCompletion() {
    for (const auto outcome : {InjectionOutcome::Injected, InjectionOutcome::Failed}) {
        Harness h; reachApprovableRewrite(h);
        h.bridge->nextOutcome = outcome;
        QObject::connect(h.controller.get(), &HostController::approvalChanged, [&]() {
            if (h.c().hasApproval()) h.c().hideCard();
        });
        ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().approveAndCopyRewrite(h.c().stateVersion())));
        ROCLINT_CHECK(pumpUntil([&]() { return !h.c().lastCopiedText().isEmpty(); }));
        ROCLINT_CHECK(!h.c().isCardVisible());
    }
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
    ROCLINT_CHECK(h.c().isCardVisible());
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
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().lastCopiedText().isEmpty(); }));
    ROCLINT_CHECK(h.c().isCardVisible());
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
    ROCLINT_CHECK(pumpUntil([&]() { return !h.c().lastCopiedText().isEmpty(); }));
    ROCLINT_CHECK(h.c().isCardVisible());
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

void testCaptureSwitchAndRestoreFollowLiveEdits() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    h.bridge->observe(cursorField(kPrompt));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    auto editor = cursorField(QStringLiteral("A new VS Code draft"), 44);
    editor.appIdentifier = QStringLiteral("com.microsoft.VSCode");
    editor.appDisplayName = QStringLiteral("Code");
    ++editor.processId;
    roclint::host::HostContextDetector::setMockObservation({
        roclint::host::HostContextObservation{roclint::host::HostContextCategory::CodeEditor,
                                             editor.appDisplayName, editor.appIdentifier}});
    h.bridge->echoOnRefresh = false;
    h.c().toggleCard();
    h.bridge->observe(editor);
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
    ROCLINT_CHECK(h.c().captureTargetName() == editor.appDisplayName);
    QString visible = editor.text;
    QObject::connect(&h.c(), &HostController::ambientDraftUpdated, [&](const QString& text) { visible = text; });
    h.c().minimizeCard();
    editor.text.chop(1);
    h.bridge->observe(editor);
    h.c().toggleCard();
    ROCLINT_CHECK(visible == editor.text);
    editor.text.clear();
    h.bridge->observe(editor);
    ROCLINT_CHECK(visible.isEmpty());
    editor.text = QStringLiteral("Replacement draft");
    h.bridge->observe(editor);
    ROCLINT_CHECK(visible == editor.text);
    roclint::host::HostContextDetector::resetMock();
}

void testSourceEditSupersedesReviewWithoutChangingDestination() {
    Harness h; reachApprovableRewrite(h);
    h.c().pauseAmbientFollowing(); // Reviewing a decision does not author a replacement draft.
    int updates = 0; QString visible;
    QObject::connect(h.controller.get(), &HostController::ambientDraftUpdated, [&](const QString& text) { ++updates; visible = text; });
    h.bridge->lose();
    h.bridge->observe(cursorField(kPrompt));
    ROCLINT_CHECK(h.c().hasProposal() && updates == 0); // Mere focus return keeps review intact.
    h.bridge->observe(cursorField("An unrelated prompt box", 99));
    ROCLINT_CHECK(h.c().hasProposal() && updates == 0);
    auto source = cursorField(kPrompt + " Preserve behavior.");
    h.bridge->observe(source);
    ROCLINT_CHECK(updates == 1 && visible == source.text && h.c().isDraftDirty());
    ROCLINT_CHECK(!h.c().hasProposal() && !h.c().hasConfirmedIntent() && !h.c().hasApproval());
    ROCLINT_CHECK(h.c().currentRevisionText() == kPrompt && h.c().isCardVisible());
    source.text += " Keep the interface."; h.bridge->observe(source);
    ROCLINT_CHECK(updates == 2 && visible == source.text); // Capture edits do not become HUD-owned edits.
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().noteDraftEdited(h.c().stateVersion())));
    source.text += " Another source change."; h.bridge->observe(source);
    ROCLINT_CHECK(updates == 2); // Actual HUD edits remain authoritative.
}

void testClearingBoundComposerInvalidatesEachReviewStage() {
    for (int stage=0; stage<3; ++stage) {
        Harness h;
        bool delivered = false;
        QObject::connect(h.controller.get(), &HostController::promptInjected, [&](const QString&, const QString&) { delivered = true; });
        if (stage == 0) {
            h.c().setAmbientModeEnabled(true); h.bridge->observe(cursorField(kPrompt));
            ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().openFromAmbient()));
            ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
            ROCLINT_CHECK(h.c().isClarifying());
        } else {
            reachApprovableRewrite(h);
            if (stage == 2) {
                ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().approveAndCopyRewrite(h.c().stateVersion())));
                ROCLINT_CHECK(pumpUntil([&]() { return delivered; }));
            }
        }
        int updates=0; QString visible="not cleared";
        QObject::connect(h.controller.get(), &HostController::ambientDraftUpdated, [&](const QString& text) { ++updates; visible=text; });
        const auto calls=*h.calls;
        h.bridge->observe(cursorField(QString()));
        ROCLINT_CHECK(updates == 1 && visible.isEmpty() && h.c().isDraftDirty());
        ROCLINT_CHECK(!h.c().hasProposal() && !h.c().hasApproval() && !h.c().hasConfirmedIntent() && h.c().isCardVisible());
        ROCLINT_CHECK(h.c().currentRevisionText() == kPrompt && *h.calls == calls);
    }
    std::cout << "  [PASS] Clearing the same connected composer invalidates clarification, review and approval without hiding the HUD.\n";
}

void testIndependentClipboardReviewDoesNotAdoptSourceChanges() {
    Harness h;
    h.c().setAmbientModeEnabled(true);
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().analyzePrompt(kPrompt, 0)));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().confirmChoice("alt_0_0", h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().synthesizeRewrite(h.c().stateVersion())));
    ROCLINT_CHECK(h.c().waitForIdleForTesting(3000));
    h.c().showCard();
    ROCLINT_CHECK(!h.c().isAmbientCapture() && h.c().hasProposal());
    int updates=0;
    QObject::connect(h.controller.get(), &HostController::ambientDraftUpdated, [&](const QString&) { ++updates; });
    h.bridge->observe(cursorField("make")); h.bridge->observe(cursorField(QString()));
    ROCLINT_CHECK(updates == 0 && h.c().hasProposal() && h.c().isCardVisible() && !h.c().isDraftDirty());
    ROCLINT_CHECK(h.c().currentRevisionText() == kPrompt && h.c().deliveryLabel() == "Delivers to clipboard");
    std::cout << "  [PASS] An independent clipboard review remains authoritative when a different app's composer changes.\n";
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

void testStaleBypassRetainsTypedErrorWithoutDelivery() {
    Harness h;
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().commitPrompt(kPrompt,0)));
    const auto oldVersion=h.c().stateVersion();
    const auto newer=kPrompt+" Preserve behavior.";
    ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().commitPrompt(newer,oldVersion)));
    const auto version=h.c().stateVersion();
    setClipboard("Leave clipboard unchanged");
    ROCLINT_CHECK(!h.c().waitForCommandForTesting(h.c().hardBypass("An older draft",oldVersion)));
    ROCLINT_CHECK(h.c().lastErrorCode()=="STALE_VERSION" && h.c().isStorageAvailable());
    ROCLINT_CHECK(h.c().stateVersion()==version && h.c().currentRevisionText()==newer);
    ROCLINT_CHECK(h.bridge->injections.empty() && clipboardText()=="Leave clipboard unchanged");
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
    const auto beforeDeliveryVersion = h.c().stateVersion();
    const auto bypassId = h.c().hardBypass(QStringLiteral("Changed in the HUD"), beforeDeliveryVersion);
    const bool bypassCompleted = h.c().waitForCommandForTesting(bypassId);
    if (!bypassCompleted) {
        // A completed model result may legitimately advance the version before this queued
        // gesture starts. It must reject cleanly without treating that as a database failure.
        ROCLINT_CHECK(h.c().lastErrorCode() == "STALE_VERSION");
        ROCLINT_CHECK(h.c().isStorageAvailable() && h.bridge->injections.empty());
        ROCLINT_CHECK(h.c().waitForIdleForTesting(5000));
        // Explicit simulated user retry; the controller must never replay delivery itself.
        ROCLINT_CHECK(h.c().waitForCommandForTesting(h.c().hardBypass(
            QStringLiteral("Changed in the HUD"), h.c().stateVersion())));
    }
    ROCLINT_CHECK(h.c().waitForIdleForTesting(5000) && h.c().isStorageAvailable());
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
    testIndependentFieldSessions(false);
    testIndependentFieldSessions(true);
    testQuietIndicatorFollowsAnotherEditorAfterReview();
    testChoiceWhileAnotherEditorIsWorking(false);
    testChoiceWhileAnotherEditorIsWorking(true);
    testExplicitReviewPrecedesCoalescedEdits();
    testFieldResumeAfterRestart();
    testAmbientRouteWhenPermissionGranted();
    testWidgetFallbackWhenPermissionDenied();
    testAmbientOffUsesWidgetEvenWithPermission();
    testIneligibleFieldsAreNeverRead();
    testClearPromptShowsNoBadge();
    testDismissedBadgeStaysDismissed();
    testQuietBadgeSurvivesTypingAndSameWindowReadGaps();
    testApprovedRewriteIsInjectedInPlace();
    testConnectingEditorPreservesManualRewriteAndAnswers();
    testInjectionFailureFallsBackToClipboard();
    testExplicitHideSurvivesDeliveryCompletion();
    testHardBypassInWidgetMode();
    testHardBypassInAmbientMode();
    testTerminalDraftIsCapturedAndReplaced();
    testCodexShortPromptCapturePreservesWhitespace();
    testStaleInsertionKeepsDraftAndRejectsDuplicateDelivery();
    testDeliveryRemainsBoundWhenOtherFieldsReceiveFocus();
    testTypingIsObservedWhileWidgetRemainsOpen();
    testOpenHudFollowsFocusedDraftUntilHudEdit();
    testCaptureSwitchAndRestoreFollowLiveEdits();
    testSourceEditSupersedesReviewWithoutChangingDestination();
    testClearingBoundComposerInvalidatesEachReviewStage();
    testIndependentClipboardReviewDoesNotAdoptSourceChanges();
    testInvocationRefreshesFieldBeforeStealingFocus();
    testEmptyComposerIsStillAnInsertionTarget();
    testStaleBypassRetainsTypedErrorWithoutDelivery();
    testInvocationWaitsForFreshFieldInsteadOfCachedTarget();
    testLateEditorBindsWithoutAnotherHotkeyAndPreservesHudEdits();
    testRestoringHistoryPausesLiveFollowing();
    testPlatformBridgeStartsAndStopsWithoutObserving();
    std::cout << "[Test Dual Mode] All tests PASSED.\n";
    return 0;
}
