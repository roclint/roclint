#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::core::intent;
using namespace roclint::core::rewrite;

void testProposalBindingAndValidation() {
    std::cout << "--- 1. Proposal Identity & Binding Fields ---\n";
    NormalizedAnalysis snap;
    snap.id = "snap_100";
    snap.baseRevisionId = "rev_1";

    ChoiceGroup g;
    g.id = "g_target";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_1", .interpretation = "FloatingCard.qml"});
    snap.choiceGroups.push_back(g);

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snap, {
        ConfirmedItem{.itemId = "g_target", .kind = IntentResolutionKind::SelectedAlternative, .chosenValue = "alt_1"}
    });

    RewriteProposal proposal{
        .id = "prop_100",
        .baseRevisionId = "rev_1",
        .confirmedIntentId = intent.id,
        .sourceSnapshotHash = intent.sourceSnapshotHash,
        .rewriteProfileId = "GENERIC@1",
        .compiledPrompt = "/* ROCLINT [PROFILE: GENERIC@1] */\nTarget: FloatingCard.qml",
        .status = ProposalStatus::ProposalReady
    };
    proposal.constraints.push_back(ConstraintItem{
        .id = "intent_g_target",
        .description = "User selected FloatingCard.qml",
        .status = ConstraintStatus::UserConfirmed,
        .evidence = "FloatingCard.qml"
    });
    proposal.constraints.push_back(ConstraintItem{
        .id = "profile_format",
        .description = "Conforms to GENERIC@1",
        .status = ConstraintStatus::MechanicallyVerified
    });

    auto val = RewriteValidator::validateProposal(proposal, "rev_1", intent);
    ROCLINT_CHECK(val.isValid);
    ROCLINT_CHECK(val.finalStatus == ProposalStatus::ProposalReady);
    std::cout << "  [PASS] Matching proposal passes validation.\n";
}

void testStaleProposalRejection() {
    std::cout << "--- 2. Stale Proposal Rejection (Late Result / Prompt Edit) ---\n";
    NormalizedAnalysis snap;
    snap.id = "snap_100";
    snap.baseRevisionId = "rev_1";

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snap, {
        ConfirmedItem{.itemId = "g1", .chosenValue = "alt_1"}
    });

    RewriteProposal proposal{
        .id = "prop_100",
        .baseRevisionId = "rev_1",
        .confirmedIntentId = intent.id,
        .sourceSnapshotHash = intent.sourceSnapshotHash,
        .compiledPrompt = "Some prompt",
        .status = ProposalStatus::ProposalReady
    };

    // Case A: User edited buffer to rev_2 while proposal was in flight
    auto val1 = RewriteValidator::validateProposal(proposal, "rev_2", intent);
    ROCLINT_CHECK(!val1.isValid);
    ROCLINT_CHECK(val1.finalStatus == ProposalStatus::StaleProposal);
    std::cout << "  [PASS] Late proposal with outdated revisionId rejected as STALE.\n";

    // Case B: Confirmed intent changed
    RewriteProposal wrongIntentProp = proposal;
    wrongIntentProp.confirmedIntentId = "intent_other";
    auto val2 = RewriteValidator::validateProposal(wrongIntentProp, "rev_1", intent);
    ROCLINT_CHECK(!val2.isValid);
    ROCLINT_CHECK(val2.finalStatus == ProposalStatus::StaleProposal);
    std::cout << "  [PASS] Outdated intentId rejected as STALE.\n";

    // Case C: Intent isCurrent marked false (DRAFT_DIRTY)
    intent.isCurrent = false;
    auto val3 = RewriteValidator::validateProposal(proposal, "rev_1", intent);
    ROCLINT_CHECK(!val3.isValid);
    ROCLINT_CHECK(val3.finalStatus == ProposalStatus::StaleProposal);
    std::cout << "  [PASS] Non-current ConfirmedIntent rejected as STALE.\n";
}

void testConstraintValidationAndFailure() {
    std::cout << "--- 3. Constraint States & Enforcement ---\n";
    NormalizedAnalysis snap;
    snap.id = "snap_100";
    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snap, {});

    RewriteProposal proposal{
        .id = "prop_100",
        .baseRevisionId = "rev_1",
        .confirmedIntentId = intent.id,
        .sourceSnapshotHash = intent.sourceSnapshotHash,
        .compiledPrompt = "Compiled prompt text",
        .status = ProposalStatus::ProposalReady
    };

    // Add failing constraint
    proposal.constraints.push_back(ConstraintItem{
        .id = "schema_stability",
        .description = "Database schema lock",
        .status = ConstraintStatus::Failed,
        .evidence = "Column removed without migration"
    });

    auto val = RewriteValidator::validateProposal(proposal, "rev_1", intent);
    ROCLINT_CHECK(!val.isValid);
    ROCLINT_CHECK(val.finalStatus == ProposalStatus::Rejected);
    std::cout << "  [PASS] Proposal with Failed constraint is rejected.\n";
}

void testAccessibleDiffComputation() {
    std::cout << "--- 4. Local Accessible Diff Boundary ---\n";
    std::string original = "Line 1\nLine 2\nLine 3";
    std::string compiled = "/* HEADER */\nLine 1\nLine 2 Modified\nLine 3";

    std::string diff = RewriteValidator::computeAccessibleDiff(original, compiled);
    ROCLINT_CHECK(!diff.empty());
    ROCLINT_CHECK(diff.find("+") != std::string::npos);
    std::cout << "  [PASS] Accessible diff computed locally with non-color indicators.\n";
}

int main() {
    std::cout << "[TEST] Running A10 Rewrite Proposal Binding & Validation Tests...\n\n";

    testProposalBindingAndValidation();
    testStaleProposalRejection();
    testConstraintValidationAndFailure();
    testAccessibleDiffComputation();

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL A10 REWRITE PROPOSAL TESTS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
