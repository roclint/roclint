#include "HostController.hpp"
#include <cassert>
#include <iostream>

int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "[Cadroc Host Lifecycle Test] Starting...\n";

    cadroc::host::HostController controller;

    // 1. Initial State Check
    assert(controller.hostState() == cadroc::host::HostState::Resident);
    assert(controller.hostStateString() == "RESIDENT");
    assert(controller.cardVisibility() == cadroc::host::CardVisibility::Hidden);
    assert(!controller.isCardVisible());
    std::cout << "  [PASS] Initial resident and hidden state verified.\n";

    // 2. Show Card
    bool showSignalEmitted = false;
    QObject::connect(&controller, &cadroc::host::HostController::showCardRequested, [&]() {
        showSignalEmitted = true;
    });

    controller.showCard();
    assert(showSignalEmitted);
    assert(controller.cardVisibility() == cadroc::host::CardVisibility::Focused);
    assert(controller.isCardVisible());
    std::cout << "  [PASS] showCard transitions to Focused and emits signal.\n";

    // 3. Repeated Show (Single Instance Invariant)
    controller.showCard();
    assert(controller.cardVisibility() == cadroc::host::CardVisibility::Focused);
    assert(controller.isCardVisible());
    std::cout << "  [PASS] Repeated showCard preserves single-instance Focused state.\n";

    // 4. Hide Card (Leaves Host Resident)
    bool hideSignalEmitted = false;
    QObject::connect(&controller, &cadroc::host::HostController::hideCardRequested, [&]() {
        hideSignalEmitted = true;
    });

    controller.hideCard();
    assert(hideSignalEmitted);
    assert(controller.cardVisibility() == cadroc::host::CardVisibility::Hidden);
    assert(!controller.isCardVisible());
    // Critical B00 lifetime invariant: host state must still be Resident
    assert(controller.hostState() == cadroc::host::HostState::Resident);
    std::cout << "  [PASS] hideCard leaves host resident (B00 lifetime contract).\n";

    // 5. Toggle Card
    controller.toggleCard();
    assert(controller.isCardVisible());
    controller.toggleCard();
    assert(!controller.isCardVisible());
    std::cout << "  [PASS] toggleCard alternates visibility cleanly.\n";

    std::cout << "[Cadroc Host Lifecycle Test] ALL CHECKS PASSED.\n";
    return 0;
}
