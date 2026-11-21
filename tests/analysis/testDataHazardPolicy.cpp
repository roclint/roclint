#include "roclint/analysis/DataHazardPolicy.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#define ROCLINT_CHECK(cond)                                                          \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__        \
                      << " -> " << #cond << std::endl;                              \
            std::exit(1);                                                            \
        }                                                                            \
    } while (false)

using namespace roclint::core::analysis;

void testInventoryRaceConditionDetected() {
    const std::string prompt = "When a customer clicks buy, check if the item stock is greater than 0, and if it is, subtract 1 from stock and create their order receipt";
    ROCLINT_CHECK(DataHazardPolicy::hasHazard(prompt, DataHazardType::ConcurrencyRaceCondition));

    const auto hazards = DataHazardPolicy::scanPrompt(prompt);
    ROCLINT_CHECK(!hazards.empty());
    ROCLINT_CHECK(hazards[0].type == DataHazardType::ConcurrencyRaceCondition);
    ROCLINT_CHECK(hazards[0].primaryOptionText == "Atomic transaction with row lock");
    ROCLINT_CHECK(hazards[0].secondaryOptionText == "Optimistic locking with version check");
}

void testInventoryWithAtomicGuardNotFlagged() {
    const std::string prompt = "When a customer clicks buy, use an atomic database transaction with row lock to check stock and decrement inventory";
    ROCLINT_CHECK(!DataHazardPolicy::hasHazard(prompt, DataHazardType::ConcurrencyRaceCondition));
}

void testWildcardKeystrokeSearchDetected() {
    const std::string prompt = "Add a search bar that filters users by typing any letter of their name in real-time as the user types";
    ROCLINT_CHECK(DataHazardPolicy::hasHazard(prompt, DataHazardType::WildcardKeystrokeSearch));
}

void testDebouncedSearchNotFlagged() {
    const std::string prompt = "Add a search bar that filters users as user types but debounce by 300ms";
    ROCLINT_CHECK(!DataHazardPolicy::hasHazard(prompt, DataHazardType::WildcardKeystrokeSearch));
}

void testCascadeDeleteDetected() {
    const std::string prompt = "When a customer deletes their account, delete all their data from the database to clean the database";
    ROCLINT_CHECK(DataHazardPolicy::hasHazard(prompt, DataHazardType::UnboundedCascadeDelete));
}

void testIdorDetected() {
    const std::string prompt = "Create a profile settings page where users view account by passing user id in the url param";
    ROCLINT_CHECK(DataHazardPolicy::hasHazard(prompt, DataHazardType::InsecureDirectObjectReference));
}

int main() {
    std::cout << "Running DataHazardPolicy tests..." << std::endl;
    testInventoryRaceConditionDetected();
    testInventoryWithAtomicGuardNotFlagged();
    testWildcardKeystrokeSearchDetected();
    testDebouncedSearchNotFlagged();
    testCascadeDeleteDetected();
    testIdorDetected();
    std::cout << "All DataHazardPolicy tests passed!" << std::endl;
    return 0;
}
