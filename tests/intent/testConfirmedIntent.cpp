#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"

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
using namespace roclint::core::intent;

void testProvenanceHashStability() {
    NormalizedAnalysis snap1;
    snap1.id = "snap_001";
    snap1.baseRevisionId = "rev_001";

    ChoiceGroup g1;
    g1.id = "g1";
    g1.dimension = "target";
    g1.question = "Which file?";
    g1.alternatives.push_back(ChoiceAlternative{.id = "alt_1", .interpretation = "FloatingCard.qml", .whyMaterial = "UI"});
    g1.alternatives.push_back(ChoiceAlternative{.id = "alt_2", .interpretation = "HostController.cpp", .whyMaterial = "Backend"});
    snap1.choiceGroups.push_back(g1);

    auto hash1 = IntentProvenanceBuilder::computeSnapshotHash(snap1);
    auto hash2 = IntentProvenanceBuilder::computeSnapshotHash(snap1);
    ROCLINT_CHECK(hash1 == hash2);
    ROCLINT_CHECK(hash1 != 0);

    // Mutating snapshot changes hash
    NormalizedAnalysis snap2 = snap1;
    snap2.choiceGroups[0].alternatives[0].interpretation = "DashboardWindow.qml";
    auto hash3 = IntentProvenanceBuilder::computeSnapshotHash(snap2);
    ROCLINT_CHECK(hash1 != hash3);
}

void testBuildConfirmedIntent() {
    NormalizedAnalysis snap;
    snap.id = "snap_001";
    snap.baseRevisionId = "rev_001";

    ChoiceGroup g;
    g.id = "g_target";
    g.dimension = "target";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_1", .interpretation = "FloatingCard.qml"});
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_2", .interpretation = "HostController.cpp"});
    snap.choiceGroups.push_back(g);

    std::vector<ConfirmedItem> explicitItems = {
        ConfirmedItem{
            .itemId = "g_target",
            .kind = IntentResolutionKind::SelectedAlternative,
            .chosenValue = "alt_1",
            .rationale = "Target FloatingCard presentation layer"
        }
    };

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent(
        "rev_001",
        snap,
        explicitItems,
        "Keep public signals unchanged"
    );

    ROCLINT_CHECK(!intent.id.empty());
    ROCLINT_CHECK(intent.baseRevisionId == "rev_001");
    ROCLINT_CHECK(intent.sourceSnapshotId == "snap_001");
    ROCLINT_CHECK(intent.sourceSnapshotHash != 0);
    ROCLINT_CHECK(intent.isCurrent);
    ROCLINT_CHECK(intent.customInstructions == "Keep public signals unchanged");
    ROCLINT_CHECK(intent.items.size() == 1);
    ROCLINT_CHECK(intent.hasDecisionFor("g_target"));
    ROCLINT_CHECK(!intent.hasDecisionFor("unresolved_item"));

    auto decision = intent.getDecision("g_target");
    ROCLINT_CHECK(decision.has_value());
    ROCLINT_CHECK(decision->chosenValue == "alt_1");
    ROCLINT_CHECK(decision->kind == IntentResolutionKind::SelectedAlternative);
}

void testStaleIntentInvalidationOnEdit() {
    NormalizedAnalysis snap;
    snap.id = "snap_001";
    snap.baseRevisionId = "rev_001";

    ChoiceGroup g;
    g.id = "g1";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_1", .interpretation = "Option A"});
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_2", .interpretation = "Option B"});
    snap.choiceGroups.push_back(g);

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_001", snap, {
        ConfirmedItem{.itemId = "g1", .kind = IntentResolutionKind::SelectedAlternative, .chosenValue = "alt_1"}
    });

    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent, "rev_001", snap));

    // If user edits prompt creating rev_002, intent is stale
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(intent, "rev_002", snap));

    // If snapshot changes, intent is invalid
    NormalizedAnalysis mutatedSnap = snap;
    mutatedSnap.choiceGroups[0].alternatives[0].interpretation = "Option Modified";
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(intent, "rev_001", mutatedSnap));

    // If isCurrent is marked false (DRAFT_DIRTY event)
    intent.isCurrent = false;
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(intent, "rev_001", snap));
}

void testAntiSilentConfirmation() {
    NormalizedAnalysis snap;
    snap.id = "snap_001";
    snap.baseRevisionId = "rev_001";

    ChoiceGroup g1;
    g1.id = "g1";
    ChoiceGroup g2;
    g2.id = "g2";
    snap.choiceGroups = {g1, g2};

    // User only explicitly confirmed g1
    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_001", snap, {
        ConfirmedItem{.itemId = "g1", .kind = IntentResolutionKind::SelectedAlternative, .chosenValue = "alt_1"}
    });

    ROCLINT_CHECK(intent.hasDecisionFor("g1"));
    ROCLINT_CHECK(!intent.hasDecisionFor("g2")); // Hidden/unacted item NOT confirmed
}

void testResolutionKinds() {
    ROCLINT_CHECK(std::string(toString(IntentResolutionKind::SelectedAlternative)) == "SELECTED_ALTERNATIVE");
    ROCLINT_CHECK(std::string(toString(IntentResolutionKind::CustomText)) == "CUSTOM_TEXT");
    ROCLINT_CHECK(std::string(toString(IntentResolutionKind::UseOriginal)) == "USE_ORIGINAL");
    ROCLINT_CHECK(std::string(toString(IntentResolutionKind::Withdrawn)) == "WITHDRAWN");
}

int main() {
    std::cout << "[TEST] Running ConfirmedIntent Provenance Unit Tests...\n";

    testProvenanceHashStability();
    testBuildConfirmedIntent();
    testStaleIntentInvalidationOnEdit();
    testAntiSilentConfirmation();
    testResolutionKinds();

    std::cout << "[PASS] All ConfirmedIntent tests passed successfully!\n";
    return 0;
}
