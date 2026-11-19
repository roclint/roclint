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
    const QRect native(0, 0, 2880, 1800), logical(0, 0, 1440, 900);
    ROCLINT_CHECK(ScreenPlacement::mapNativeRect(QRect(432, 100, 2, 28), native, logical) == QRect(216, 50, 1, 14));
    // Screen origins are mapped relative to the selected monitor, not divided
    // globally. Windows mixed-DPI desktops retain native monitor positions.
    ROCLINT_CHECK(ScreenPlacement::mapNativeRect(QRect(3000, 150, 30, 30),
        QRect(2880, 0, 1920, 1080), QRect(2880, 0, 1280, 720)) == QRect(2960, 100, 20, 20));
    ROCLINT_CHECK(ScreenPlacement::mapNativeRect(QRect(-1800, -100, 20, 20),
        QRect(-1920, -200, 1920, 1080), QRect(-1920, -200, 960, 540)) == QRect(-1860, -150, 10, 10));
    ROCLINT_CHECK(ScreenPlacement::mapNativeRect(QRect(400, 100, 1, 25), native, logical).width() == 1);
    ROCLINT_CHECK(ScreenPlacement::mapNativeRect(QRect(), native, logical).isEmpty());
    const int cardW = 820;
    const int cardH = 520;
    const QRect screenBounds(0, 0, 1920, 1080);
    for (const auto& field : {QRect(500,300,600,200), QRect(1300,900,600,100), QRect(1300,1000,600,60)}) {
        const auto position = ScreenPlacement::calculateIndicatorPosition(field, screenBounds);
        const QRect icon(position.x, position.y, 28, 28);
        ROCLINT_CHECK(screenBounds.adjusted(8,8,-8,-8).contains(icon));
        ROCLINT_CHECK(!icon.intersects(field)); // Never obscure typing or Send when outside space exists.
    }
    const auto filled = ScreenPlacement::calculateIndicatorPosition(screenBounds, screenBounds);
    ROCLINT_CHECK(filled.clamped && screenBounds.contains(QRect(filled.x, filled.y, 28, 28)));

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

    // Anchor positioning for Grammarly-style corner badge
    const QRect promptRect(500, 300, 600, 200);
    auto anchorBadge = ScreenPlacement::calculateAnchorPosition(promptRect, 140, 26, screenBounds, 12);
    ROCLINT_CHECK(anchorBadge.x == promptRect.right() - 140 - 6);
    ROCLINT_CHECK(anchorBadge.y == promptRect.bottom() - 26 - 4);
    ROCLINT_CHECK(!anchorBadge.clamped);

    // Anchoring near bottom-right screen border clamps inside margins
    const QRect nearEdgePrompt(1850, 1000, 200, 100);
    auto edgeBadge = ScreenPlacement::calculateAnchorPosition(nearEdgePrompt, 140, 26, screenBounds, 12);
    ROCLINT_CHECK(edgeBadge.x <= screenBounds.left() + screenBounds.width() - 140 - 12);
    ROCLINT_CHECK(edgeBadge.y <= screenBounds.top() + screenBounds.height() - 26 - 12);
    ROCLINT_CHECK(edgeBadge.clamped);

    // Reopening an expanded/resized HUD must use its retained dimensions.
    for (const auto size : {QSize(620,250), QSize(820,520), QSize(560,400)}) {
        const auto position=ScreenPlacement::calculateAnchorPosition(nearEdgePrompt,size.width(),size.height(),screenBounds,12);
        ROCLINT_CHECK(screenBounds.adjusted(12,12,-12,-12).contains(QRect(QPoint(position.x,position.y),size)));
        const QRect secondary(-1920,-200,1920,1080);
        const auto other=ScreenPlacement::calculateAnchorPosition(QRect(-100,800,90,60),size.width(),size.height(),secondary,12);
        ROCLINT_CHECK(secondary.adjusted(12,12,-12,-12).contains(QRect(QPoint(other.x,other.y),size)));
    }

    const QSize reviewSize(480, 380);
    const auto reviewBounds = [&](const QRect& field, const QRect& screen) {
        const auto p = ScreenPlacement::calculateReviewPosition(field, reviewSize, screen);
        return QRect(QPoint(p.x, p.y), reviewSize);
    };
    const QRect bottomComposer(100, 520, 1040, 120);
    const QRect wideScreen(0, 0, 1280, 660);
    const auto above = reviewBounds(bottomComposer, wideScreen);
    ROCLINT_CHECK(wideScreen.contains(above) && above.bottom() < bottomComposer.top());
    const QRect tallComposer(1400, 100, 500, 880);
    const auto beside = reviewBounds(tallComposer, screenBounds);
    ROCLINT_CHECK(screenBounds.contains(beside) && beside.right() < tallComposer.left());
    const auto detached = reviewBounds(QRect(), QRect(-1280, 0, 1280, 660));
    ROCLINT_CHECK(detached.topLeft() == QPoint(-492, 12));

    const QRect shortScreen(0, 0, 1144, 472);
    const QRect shortComposer(100, 353, 980, 100);
    const auto shortPlacement = reviewBounds(shortComposer, shortScreen);
    ROCLINT_CHECK(shortScreen.contains(shortPlacement) && shortPlacement.bottom() < shortComposer.bottom() - 30);

    std::cout << "  [PASS] Card and anchor placement clamp to the screen.\n";
}

void testHostContextDisclosureAndClear() {
    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());
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
    ROCLINT_CHECK(controller.waitForStorageForTesting());
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
