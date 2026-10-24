#include "HostController.hpp"
#include "HostContext.hpp"
#include "ScreenPlacement.hpp"
#include "support/check.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <iostream>

using namespace roclint::host;

namespace {

void testScreenPlacement() {
    const int cardW = 820;
    const int cardH = 520;
    const QRect screenBounds(0, 0, 1920, 1080);

    auto center = ScreenPlacement::calculateClampedPosition(QPoint(960, 540), cardW, cardH, screenBounds, 12);
    ROCLINT_CHECK(center.x == 960 - cardW / 2);
    ROCLINT_CHECK(center.y == 540 - 60);
    ROCLINT_CHECK(!center.clamped);

    auto topLeft = ScreenPlacement::calculateClampedPosition(QPoint(10, 10), cardW, cardH, screenBounds, 12);
    ROCLINT_CHECK(topLeft.x == 12);
    ROCLINT_CHECK(topLeft.y == 12);
    ROCLINT_CHECK(topLeft.clamped);

    auto bottomRight = ScreenPlacement::calculateClampedPosition(QPoint(1910, 1070), cardW, cardH, screenBounds, 12);
    ROCLINT_CHECK(bottomRight.x == 1920 - cardW - 12);
    ROCLINT_CHECK(bottomRight.y == 1080 - cardH - 12);
    ROCLINT_CHECK(bottomRight.clamped);

    auto fallback = ScreenPlacement::calculateClampedPosition(QPoint(960, 540), cardW, cardH, QRect(), 12);
    ROCLINT_CHECK(fallback.x == 100);
    ROCLINT_CHECK(fallback.y == 100);
    std::cout << "  [PASS] Card placement clamps to the screen.\n";
}

void testHostContextDisclosureAndClear() {
    HostController controller(QStringLiteral(":memory:"));
    controller.setLegacyDiscoveryForTesting(true);
    controller.markResident();
    HostContextDetector::setMockObservation(HostContextObservation{
        HostContextCategory::CodeEditor, QStringLiteral("Visual Studio Code"), QStringLiteral("Code.exe")});

    int contextChanges = 0;
    QObject::connect(&controller, &HostController::hostContextChanged, [&]() { ++contextChanges; });

    controller.showCard();
    ROCLINT_CHECK(controller.hasHostContext());
    ROCLINT_CHECK(controller.hostContextCategory() == "CODE_EDITOR");
    ROCLINT_CHECK(controller.hostContextDisplayName() == "Visual Studio Code");
    ROCLINT_CHECK(contextChanges >= 1);

    controller.clearHostContext();
    ROCLINT_CHECK(!controller.hasHostContext());
    ROCLINT_CHECK(controller.hostContextCategory() == "UNKNOWN");
    ROCLINT_CHECK(controller.hostContextDisplayName().isEmpty());
    HostContextDetector::resetMock();

    // An unlisted frontmost app is disclosed as unknown rather than guessed.
    HostContextDetector::setMockObservation(HostContextObservation{
        HostContextDetector::mapIdentityToCategory(QStringLiteral("SearchApp.exe")), QStringLiteral("Search"),
        QStringLiteral("SearchApp.exe")});
    controller.hideCard();
    controller.showCard();
    ROCLINT_CHECK(!controller.hasHostContext());
    ROCLINT_CHECK(controller.hostContextCategory() == "UNKNOWN");
    HostContextDetector::resetMock();
    std::cout << "  [PASS] Host context is disclosed, clearable, and never guessed.\n";
}

void testClipboardCaptureOnInvocation() {
    HostController controller(QStringLiteral(":memory:"));
    controller.setLegacyDiscoveryForTesting(true);
    controller.markResident();
    const QString sample = QStringLiteral("Refactor SessionManager to handle async token refreshes");
    if (auto* cb = QGuiApplication::clipboard()) {
        cb->setText(sample);
    }
    QString captured;
    QObject::connect(&controller, &HostController::draftCaptured, [&](const QString& text) { captured = text; });

    controller.showCard();
    ROCLINT_CHECK(controller.capturedDraftText().isEmpty());
    ROCLINT_CHECK(captured.isEmpty());

    // Dismissing the card changes neither the clipboard nor the core state.
    const auto version = controller.stateVersion();
    controller.hideCard();
    ROCLINT_CHECK(!controller.isCardVisible());
    ROCLINT_CHECK(controller.stateVersion() == version);
    ROCLINT_CHECK(controller.sessionCount() == 0);
    if (auto* cb = QGuiApplication::clipboard()) {
        ROCLINT_CHECK(cb->text() == sample);
    }
    std::cout << "  [PASS] Invocation never imports unrelated clipboard text; dismissing changes nothing.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    std::cout << "[Test Invocation Binding] Starting...\n";
    testScreenPlacement();
    testHostContextDisclosureAndClear();
    testClipboardCaptureOnInvocation();
    std::cout << "[Test Invocation Binding] All tests PASSED.\n";
    return 0;
}
