#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::core::intent;

void testScenario1MultiItemSnapshotResolution() {
    std::cout << "--- Scenario 1: Multi-Item Snapshot Resolution ---\n";
    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i07_multi";
    snapshot.baseRevisionId = "rev_1";

    ChoiceGroup g1;
    g1.id = "group_target";
    g1.dimension = "target_component";
    g1.question = "Which component should handle connection timeout?";
    g1.alternatives.push_back(ChoiceAlternative{.id = "alt_card", .interpretation = "FloatingCard.qml", .whyMaterial = "UI layer"});
    g1.alternatives.push_back(ChoiceAlternative{.id = "alt_host", .interpretation = "HostController.cpp", .whyMaterial = "Backend controller"});

    ChoiceGroup g2;
    g2.id = "group_scope";
    g2.dimension = "edit_scope";
    g2.question = "What is the edit scope?";
    g2.alternatives.push_back(ChoiceAlternative{.id = "alt_local", .interpretation = "Single file", .whyMaterial = "Minimal change"});
    g2.alternatives.push_back(ChoiceAlternative{.id = "alt_global", .interpretation = "Entire project", .whyMaterial = "Broad refactor"});

    snapshot.choiceGroups = {g1, g2};

    // Explicit user selections
    std::vector<ConfirmedItem> explicitItems = {
        ConfirmedItem{
            .itemId = "group_target",
            .kind = IntentResolutionKind::SelectedAlternative,
            .chosenValue = "alt_card",
            .rationale = "Target FloatingCard presentation layer"
        },
        ConfirmedItem{
            .itemId = "group_scope",
            .kind = IntentResolutionKind::SelectedAlternative,
            .chosenValue = "alt_local",
            .rationale = "Minimal single file edit"
        }
    };

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent(
        "rev_1",
        snapshot,
        explicitItems,
        "Keep styling consistent with dark poster HUD theme"
    );

    ROCLINT_CHECK(!intent.id.empty());
    ROCLINT_CHECK(intent.baseRevisionId == "rev_1");
    ROCLINT_CHECK(intent.sourceSnapshotId == "snap_i07_multi");
    ROCLINT_CHECK(intent.sourceSnapshotHash != 0);
    ROCLINT_CHECK(intent.isCurrent);
    ROCLINT_CHECK(intent.items.size() == 2);
    ROCLINT_CHECK(intent.hasDecisionFor("group_target"));
    ROCLINT_CHECK(intent.hasDecisionFor("group_scope"));

    auto dec1 = intent.getDecision("group_target");
    ROCLINT_CHECK(dec1.has_value() && dec1->chosenValue == "alt_card");

    auto dec2 = intent.getDecision("group_scope");
    ROCLINT_CHECK(dec2.has_value() && dec2->chosenValue == "alt_local");

    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent, "rev_1", snapshot));
    std::cout << "  [PASS] Multi-item snapshot resolved and confirmed with durable provenance.\n";
}

void testScenario2AntiSilentConfirmation() {
    std::cout << "--- Scenario 2: Anti-Silent Confirmation (Unacted Items) ---\n";
    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i07_unacted";
    snapshot.baseRevisionId = "rev_1";

    ChoiceGroup visibleGroup;
    visibleGroup.id = "group_visible";
    visibleGroup.question = "Select target component";
    visibleGroup.alternatives.push_back(ChoiceAlternative{.id = "alt_vis_a", .interpretation = "FloatingCard.qml"});
    visibleGroup.alternatives.push_back(ChoiceAlternative{.id = "alt_vis_b", .interpretation = "DashboardWindow.qml"});

    ChoiceGroup collapsedGroup;
    collapsedGroup.id = "group_collapsed";
    collapsedGroup.question = "Select secondary style";
    collapsedGroup.alternatives.push_back(ChoiceAlternative{.id = "alt_col_a", .interpretation = "Minimal"});
    collapsedGroup.alternatives.push_back(ChoiceAlternative{.id = "alt_col_b", .interpretation = "Verbose"});

    snapshot.choiceGroups = {visibleGroup, collapsedGroup};

    // User only interacted with visibleGroup
    std::vector<ConfirmedItem> explicitItems = {
        ConfirmedItem{
            .itemId = "group_visible",
            .kind = IntentResolutionKind::SelectedAlternative,
            .chosenValue = "alt_vis_a",
            .rationale = "User choice"
        }
    };

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snapshot, explicitItems);

    ROCLINT_CHECK(intent.hasDecisionFor("group_visible"));
    // CONTRACT C05 / C06: Inferred, hidden, or unacted items MUST NOT be implicitly confirmed!
    ROCLINT_CHECK(!intent.hasDecisionFor("group_collapsed"));
    ROCLINT_CHECK(intent.items.size() == 1);
    std::cout << "  [PASS] Hidden/collapsed items are NOT implicitly confirmed.\n";
}

void testScenario3StaleIntentOnEdit() {
    std::cout << "--- Scenario 3: Stale Intent on Prompt Edit ---\n";
    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i07_edit";
    snapshot.baseRevisionId = "rev_1";

    ChoiceGroup g;
    g.id = "group_target";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_1", .interpretation = "FloatingCard.qml"});
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_2", .interpretation = "ScreenPlacement.cpp"});
    snapshot.choiceGroups = {g};

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snapshot, {
        ConfirmedItem{.itemId = "group_target", .kind = IntentResolutionKind::SelectedAlternative, .chosenValue = "alt_1"}
    });

    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent, "rev_1", snapshot));

    // 1. User edits prompt buffer -> revision bumps to rev_2
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(intent, "rev_2", snapshot));

    // 2. Snapshot content changes (e.g. underlying buffer text changed)
    NormalizedAnalysis mutatedSnapshot = snapshot;
    mutatedSnapshot.choiceGroups[0].alternatives[0].interpretation = "FloatingCardV2.qml";
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(intent, "rev_1", mutatedSnapshot));

    // 3. Mark intent non-current (DRAFT_DIRTY event)
    intent.isCurrent = false;
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(intent, "rev_1", snapshot));
    std::cout << "  [PASS] Intent becomes immediately stale upon buffer edit or snapshot mutation.\n";
}

int main() {
    std::cout << "[TEST] Running I07 Intent Checkpoint Walkthrough...\n\n";

    testScenario1MultiItemSnapshotResolution();
    testScenario2AntiSilentConfirmation();
    testScenario3StaleIntentOnEdit();

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL I07 INTENT CHECKPOINT WALKTHROUGH SCENARIOS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
