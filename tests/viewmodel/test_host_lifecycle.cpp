#include "HostController.hpp"
#include "support/check.hpp"

#include <QTemporaryDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>

using roclint::host::CardVisibility;
using roclint::host::HostController;
using roclint::host::HostState;

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    std::cout << "[Roclint Host Lifecycle Test] Starting...\n";

    HostController controller(QStringLiteral(":memory:"));
    ROCLINT_CHECK(controller.waitForStorageForTesting());

    controller.setLegacyDiscoveryForTesting(true);

    int showRequests = 0;
    int hideRequests = 0;
    int showDashboardRequests = 0;
    int hideDashboardRequests = 0;
    int quitRequests = 0;
    int visibilityNotifications = 0;
    int dashboardVisibilityNotifications = 0;
    int stateNotifications = 0;
    QObject::connect(&controller, &HostController::showCardRequested, [&]() { ++showRequests; });
    QObject::connect(&controller, &HostController::hideCardRequested, [&]() { ++hideRequests; });
    QObject::connect(&controller, &HostController::showDashboardRequested, [&]() { ++showDashboardRequests; });
    QObject::connect(&controller, &HostController::hideDashboardRequested, [&]() { ++hideDashboardRequests; });
    QObject::connect(&controller, &HostController::quitRequested, [&]() { ++quitRequests; });
    QObject::connect(&controller, &HostController::cardVisibilityChanged, [&]() { ++visibilityNotifications; });
    QObject::connect(&controller, &HostController::dashboardVisibilityChanged, [&]() { ++dashboardVisibilityNotifications; });
    QObject::connect(&controller, &HostController::hostStateChanged, [&]() { ++stateNotifications; });

    // 1. Initial State Check
    ROCLINT_CHECK(controller.hostState() == HostState::Starting);
    ROCLINT_CHECK(controller.hostStateString() == "STARTING");
    ROCLINT_CHECK(controller.cardVisibility() == CardVisibility::Hidden);
    ROCLINT_CHECK(!controller.isCardVisible());
    std::cout << "  [PASS] Initial starting and hidden state verified.\n";

    // 2. Starting -> Resident transition is observable and one-shot
    controller.markResident();
    ROCLINT_CHECK(controller.hostState() == HostState::Resident);
    ROCLINT_CHECK(controller.hostStateString() == "RESIDENT");
    ROCLINT_CHECK(stateNotifications == 1);
    controller.markResident();
    ROCLINT_CHECK(stateNotifications == 1);
    std::cout << "  [PASS] markResident transitions Starting -> Resident once.\n";

    // 3. Show Card
    controller.showCard();
    ROCLINT_CHECK(showRequests == 1);
    ROCLINT_CHECK(visibilityNotifications == 1);
    ROCLINT_CHECK(controller.cardVisibility() == CardVisibility::Focused);
    ROCLINT_CHECK(controller.isCardVisible());
    std::cout << "  [PASS] showCard transitions to Focused and emits signal.\n";

    // 4. Repeated Show (Single Instance Invariant): re-requests focus, no spurious notify
    controller.showCard();
    ROCLINT_CHECK(showRequests == 2);
    ROCLINT_CHECK(visibilityNotifications == 1);
    ROCLINT_CHECK(controller.cardVisibility() == CardVisibility::Focused);
    std::cout << "  [PASS] Repeated showCard preserves single-instance Focused state.\n";

    // 5. Hide Card (Leaves Host Resident)
    controller.hideCard();
    ROCLINT_CHECK(hideRequests == 1);
    ROCLINT_CHECK(controller.cardVisibility() == CardVisibility::Hidden);
    ROCLINT_CHECK(!controller.isCardVisible());
    // Critical B00 lifetime invariant: host state must still be Resident
    ROCLINT_CHECK(controller.hostState() == HostState::Resident);
    controller.hideCard();
    ROCLINT_CHECK(hideRequests == 1);
    std::cout << "  [PASS] hideCard leaves host resident (B00 lifetime contract).\n";

    // 6. Toggle Card
    controller.toggleCard();
    ROCLINT_CHECK(controller.isCardVisible());
    controller.toggleCard();
    ROCLINT_CHECK(!controller.isCardVisible());
    std::cout << "  [PASS] toggleCard alternates visibility cleanly.\n";

    // 7. Window-system sync (e.g. minimize/restore) updates state without show/hide requests
    controller.showCard();
    const int showsBeforeSync = showRequests;
    const int hidesBeforeSync = hideRequests;
    controller.syncCardVisibility(CardVisibility::Visible);
    ROCLINT_CHECK(controller.cardVisibility() == CardVisibility::Visible);
    controller.syncCardVisibility(CardVisibility::Hidden); // minimized
    ROCLINT_CHECK(!controller.isCardVisible());
    ROCLINT_CHECK(showRequests == showsBeforeSync);
    ROCLINT_CHECK(hideRequests == hidesBeforeSync);
    controller.toggleCard(); // tray click on a minimized card must restore it
    ROCLINT_CHECK(showRequests == showsBeforeSync + 1);
    ROCLINT_CHECK(controller.cardVisibility() == CardVisibility::Focused);
    std::cout << "  [PASS] syncCardVisibility tracks external changes; toggle restores.\n";

    // 8. Dashboard Lifecycle Checks
    ROCLINT_CHECK(!controller.isDashboardVisible());
    controller.showDashboard();
    ROCLINT_CHECK(controller.isDashboardVisible());
    ROCLINT_CHECK(showDashboardRequests == 1);
    ROCLINT_CHECK(dashboardVisibilityNotifications == 1);

    controller.hideDashboard();
    ROCLINT_CHECK(!controller.isDashboardVisible());
    ROCLINT_CHECK(hideDashboardRequests == 1);
    ROCLINT_CHECK(dashboardVisibilityNotifications == 2);

    controller.toggleDashboard();
    ROCLINT_CHECK(controller.isDashboardVisible());
    controller.toggleDashboard();
    ROCLINT_CHECK(!controller.isDashboardVisible());

    controller.syncDashboardVisibility(true);
    ROCLINT_CHECK(controller.isDashboardVisible());
    controller.syncDashboardVisibility(false);
    ROCLINT_CHECK(!controller.isDashboardVisible());
    std::cout << "  [PASS] Dashboard lifecycle (show, hide, toggle, sync) verified.\n";

    // 9. Explicit Quit hides card and dashboard, enters Quitting, and is terminal
    controller.showCard();
    controller.showDashboard();
    ROCLINT_CHECK(controller.isCardVisible());
    ROCLINT_CHECK(controller.isDashboardVisible());

    const int hidesBeforeQuit = hideRequests;
    const int dashHidesBeforeQuit = hideDashboardRequests;
    controller.requestQuit();
    ROCLINT_CHECK(controller.hostState() == HostState::Quitting);
    ROCLINT_CHECK(!controller.isCardVisible());
    ROCLINT_CHECK(!controller.isDashboardVisible());
    ROCLINT_CHECK(hideRequests == hidesBeforeQuit + 1);
    ROCLINT_CHECK(hideDashboardRequests == dashHidesBeforeQuit + 1);
    { QElapsedTimer deadline; deadline.start();
      while (!quitRequests && deadline.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); } }
    ROCLINT_CHECK(quitRequests == 1);
    controller.requestQuit();
    { QElapsedTimer deadline; deadline.start();
      while (!quitRequests && deadline.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); } }
    ROCLINT_CHECK(quitRequests == 1);

    const int showsAfterQuit = showRequests;
    const int dashShowsAfterQuit = showDashboardRequests;
    controller.showCard();
    controller.showDashboard();
    controller.syncCardVisibility(CardVisibility::Focused);
    controller.syncDashboardVisibility(true);
    ROCLINT_CHECK(showRequests == showsAfterQuit);
    ROCLINT_CHECK(showDashboardRequests == dashShowsAfterQuit);
    ROCLINT_CHECK(!controller.isCardVisible());
    ROCLINT_CHECK(!controller.isDashboardVisible());
    std::cout << "  [PASS] requestQuit hides card & dashboard, emits quitRequested once, blocks re-show.\n";

    // 10. Runtime and scorer status are reported honestly before any model is connected.
    {
        HostController fresh(QStringLiteral(":memory:"));
    ROCLINT_CHECK(fresh.waitForStorageForTesting());
        fresh.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(fresh.isStorageAvailable());
        ROCLINT_CHECK(fresh.modelRuntimeState() == "UNLOADED");
        ROCLINT_CHECK(!fresh.modelEngineStatus().contains(QStringLiteral("Ready")));
        ROCLINT_CHECK(fresh.scorerEngine() == "JULIA_LOCAL");
        ROCLINT_CHECK(fresh.scorerStatus().contains(QStringLiteral("unavailable")));
        ROCLINT_CHECK(!fresh.isAmbiguityScored());

        ROCLINT_CHECK(fresh.setScorerEngine(QStringLiteral("jev")));
        ROCLINT_CHECK(fresh.lastError().isEmpty());
        ROCLINT_CHECK(fresh.setScorerEngine(QStringLiteral("julia")));
        ROCLINT_CHECK(!fresh.setScorerEngine(QStringLiteral("unknown")));
        ROCLINT_CHECK(!fresh.lastError().isEmpty());
        ROCLINT_CHECK(fresh.scorerEngine() == "JULIA_LOCAL");
        std::cout << "  [PASS] No model: runtime UNLOADED, scorer unranked, unknown engines refused.\n";
    }

    // 11. A storage failure is shown, never silently replaced by an in-memory database.
    {
        QTemporaryDir dir;
        ROCLINT_CHECK(dir.isValid());
        HostController broken(dir.path() + QStringLiteral("/missing/sub/dir/history.db"));
    ROCLINT_CHECK(broken.waitForStorageForTesting());
        broken.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(!broken.isStorageAvailable());
        ROCLINT_CHECK(broken.storageNeedsRecovery());
        ROCLINT_CHECK(!broken.waitForCommandForTesting(broken.commitPrompt(QStringLiteral("hello"), 0)));
        ROCLINT_CHECK(!broken.lastError().isEmpty());
        ROCLINT_CHECK(broken.stateVersion() == 0);
        std::cout << "  [PASS] Storage failure is explicit; commands refuse to run.\n";
    }

    // 12. Active work survives minimize/hide and toggleCard restores without capturing unrelated windows.
    {
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        c.setAmbientModeEnabled(true);
        ROCLINT_CHECK(!c.hasActiveWork());

        // Start working on a prompt
        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(QStringLiteral("make my website more faster"), 0)));
        ROCLINT_CHECK(c.hasActiveWork());

        // Minimize / hide the card
        c.showCard();
        ROCLINT_CHECK(c.isCardVisible());
        c.hideCard();
        ROCLINT_CHECK(!c.isCardVisible());
        ROCLINT_CHECK(c.hasActiveWork());

        // Toggle card must restore active work
        c.toggleCard();
        ROCLINT_CHECK(c.isCardVisible());
        ROCLINT_CHECK(c.hasActiveWork());

        // Reset draft clears active work
        c.resetActiveDraft();
        ROCLINT_CHECK(!c.hasActiveWork());
        std::cout << "  [PASS] Active draft survives minimize/hide; toggleCard restores without losing progress.\n";
    }

    // 13. Active work survives minimize/hide and toggleCard without session reset.
    {
        HostController c(QStringLiteral(":memory:"));
        ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(false);
        c.setAmbientModeEnabled(true);

        const QString prompt = QStringLiteral("make my website more faster");
        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(prompt, 0)));
        ROCLINT_CHECK(c.hasActiveWork());

        // Minimize / hide
        c.hideCard();
        ROCLINT_CHECK(!c.isCardVisible());
        ROCLINT_CHECK(c.hasActiveWork());

        // Toggle card must restore without resetting session
        const auto sessionIdBefore = c.currentSessionIdString();
        c.toggleCard();
        ROCLINT_CHECK(c.isCardVisible());
        ROCLINT_CHECK(c.hasActiveWork());
        ROCLINT_CHECK(c.currentSessionIdString() == sessionIdBefore);
        std::cout << "  [PASS] Active work survives minimize/hide and toggleCard without session reset.\n";
    }

    std::cout << "[Roclint Host Lifecycle Test] ALL CHECKS PASSED.\n";
    return 0;
}
