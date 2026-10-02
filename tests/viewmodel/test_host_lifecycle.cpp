#include "HostController.hpp"
#include "support/check.hpp"

#include <iostream>

using cadroc::host::CardVisibility;
using cadroc::host::HostController;
using cadroc::host::HostState;

int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "[Cadroc Host Lifecycle Test] Starting...\n";

    HostController controller;

    int showRequests = 0;
    int hideRequests = 0;
    int quitRequests = 0;
    int visibilityNotifications = 0;
    int stateNotifications = 0;
    QObject::connect(&controller, &HostController::showCardRequested, [&]() { ++showRequests; });
    QObject::connect(&controller, &HostController::hideCardRequested, [&]() { ++hideRequests; });
    QObject::connect(&controller, &HostController::quitRequested, [&]() { ++quitRequests; });
    QObject::connect(&controller, &HostController::cardVisibilityChanged, [&]() { ++visibilityNotifications; });
    QObject::connect(&controller, &HostController::hostStateChanged, [&]() { ++stateNotifications; });

    // 1. Initial State Check
    CADROC_CHECK(controller.hostState() == HostState::Starting);
    CADROC_CHECK(controller.hostStateString() == "STARTING");
    CADROC_CHECK(controller.cardVisibility() == CardVisibility::Hidden);
    CADROC_CHECK(!controller.isCardVisible());
    std::cout << "  [PASS] Initial starting and hidden state verified.\n";

    // 2. Starting -> Resident transition is observable and one-shot
    controller.markResident();
    CADROC_CHECK(controller.hostState() == HostState::Resident);
    CADROC_CHECK(controller.hostStateString() == "RESIDENT");
    CADROC_CHECK(stateNotifications == 1);
    controller.markResident();
    CADROC_CHECK(stateNotifications == 1);
    std::cout << "  [PASS] markResident transitions Starting -> Resident once.\n";

    // 3. Show Card
    controller.showCard();
    CADROC_CHECK(showRequests == 1);
    CADROC_CHECK(visibilityNotifications == 1);
    CADROC_CHECK(controller.cardVisibility() == CardVisibility::Focused);
    CADROC_CHECK(controller.isCardVisible());
    std::cout << "  [PASS] showCard transitions to Focused and emits signal.\n";

    // 4. Repeated Show (Single Instance Invariant): re-requests focus, no spurious notify
    controller.showCard();
    CADROC_CHECK(showRequests == 2);
    CADROC_CHECK(visibilityNotifications == 1);
    CADROC_CHECK(controller.cardVisibility() == CardVisibility::Focused);
    std::cout << "  [PASS] Repeated showCard preserves single-instance Focused state.\n";

    // 5. Hide Card (Leaves Host Resident)
    controller.hideCard();
    CADROC_CHECK(hideRequests == 1);
    CADROC_CHECK(controller.cardVisibility() == CardVisibility::Hidden);
    CADROC_CHECK(!controller.isCardVisible());
    // Critical B00 lifetime invariant: host state must still be Resident
    CADROC_CHECK(controller.hostState() == HostState::Resident);
    controller.hideCard();
    CADROC_CHECK(hideRequests == 1);
    std::cout << "  [PASS] hideCard leaves host resident (B00 lifetime contract).\n";

    // 6. Toggle Card
    controller.toggleCard();
    CADROC_CHECK(controller.isCardVisible());
    controller.toggleCard();
    CADROC_CHECK(!controller.isCardVisible());
    std::cout << "  [PASS] toggleCard alternates visibility cleanly.\n";

    // 7. Window-system sync (e.g. minimize/restore) updates state without show/hide requests
    controller.showCard();
    const int showsBeforeSync = showRequests;
    const int hidesBeforeSync = hideRequests;
    controller.syncCardVisibility(CardVisibility::Visible);
    CADROC_CHECK(controller.cardVisibility() == CardVisibility::Visible);
    controller.syncCardVisibility(CardVisibility::Hidden); // minimized
    CADROC_CHECK(!controller.isCardVisible());
    CADROC_CHECK(showRequests == showsBeforeSync);
    CADROC_CHECK(hideRequests == hidesBeforeSync);
    controller.toggleCard(); // tray click on a minimized card must restore it
    CADROC_CHECK(showRequests == showsBeforeSync + 1);
    CADROC_CHECK(controller.cardVisibility() == CardVisibility::Focused);
    std::cout << "  [PASS] syncCardVisibility tracks external changes; toggle restores.\n";

    // 8. Explicit Quit hides the card, enters Quitting, and is terminal
    const int hidesBeforeQuit = hideRequests;
    controller.requestQuit();
    CADROC_CHECK(controller.hostState() == HostState::Quitting);
    CADROC_CHECK(!controller.isCardVisible());
    CADROC_CHECK(hideRequests == hidesBeforeQuit + 1);
    CADROC_CHECK(quitRequests == 1);
    controller.requestQuit();
    CADROC_CHECK(quitRequests == 1);
    const int showsAfterQuit = showRequests;
    controller.showCard();
    controller.syncCardVisibility(CardVisibility::Focused);
    CADROC_CHECK(showRequests == showsAfterQuit);
    CADROC_CHECK(!controller.isCardVisible());
    std::cout << "  [PASS] requestQuit hides card, emits quitRequested once, blocks re-show.\n";

    std::cout << "[Cadroc Host Lifecycle Test] ALL CHECKS PASSED.\n";
    return 0;
}
