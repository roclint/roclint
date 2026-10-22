#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::core::intent;
using namespace roclint::core::rewrite;
using namespace roclint::bridge;

void testScenario1ConfirmedIntentThroughB08AndA10() {
    std::cout << "--- 1. ConfirmedIntent -> B08 Synthesis -> A10 Validation ---\n";
    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i08_1";
    snapshot.baseRevisionId = "rev_1";

    ChoiceGroup g;
    g.id = "target_component";
    g.question = "Which file should be modified?";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_card", .interpretation = "FloatingCard.qml", .whyMaterial = "UI HUD"});
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_host", .interpretation = "HostController.cpp", .whyMaterial = "Host logic"});
    snapshot.choiceGroups.push_back(g);

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snapshot, {
        ConfirmedItem{
            .itemId = "target_component",
            .kind = IntentResolutionKind::SelectedAlternative,
            .chosenValue = "alt_card",
            .rationale = "Target FloatingCard presentation component"
        }
    });

    GraniteRewriteAdapter adapter;
    GraniteRewriteRequest req{
        .sessionId = "sess_i08",
        .revisionId = "rev_1",
        .promptString = "Refactor connection timeout handling",
        .confirmedIntent = intent,
        .rewriteProfileId = "GENERIC@1",
        .modelQuantization = "Q8_0"
    };

    auto resp = adapter.synthesizeRewrite(req);
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.proposal.has_value());
    const auto& proposal = *resp.proposal;

    ROCLINT_CHECK(proposal.baseRevisionId == "rev_1");
    ROCLINT_CHECK(proposal.confirmedIntentId == intent.id);
    ROCLINT_CHECK(proposal.sourceSnapshotHash == intent.sourceSnapshotHash);
    ROCLINT_CHECK(proposal.status == ProposalStatus::ProposalReady);

    // Validate with A10 Core Validator
    auto valResult = RewriteValidator::validateProposal(proposal, "rev_1", intent);
    ROCLINT_CHECK(valResult.isValid);
    ROCLINT_CHECK(valResult.finalStatus == ProposalStatus::ProposalReady);
    std::cout << "  [PASS] Confirmed intent synthesized by B08 and accepted by A10.\n";
}

void testScenario2StaleProposalRejectedOnDraftEdit() {
    std::cout << "--- 2. Draft Edit Rejects In-Flight / Late Proposal ---\n";
    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i08_2";
    snapshot.baseRevisionId = "rev_1";

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snapshot, {
        ConfirmedItem{.itemId = "g1", .chosenValue = "alt_1"}
    });

    GraniteRewriteAdapter adapter;
    GraniteRewriteRequest req{
        .sessionId = "sess_i08",
        .revisionId = "rev_1",
        .promptString = "Original prompt text",
        .confirmedIntent = intent,
        .rewriteProfileId = "GENERIC@1"
    };

    auto resp = adapter.synthesizeRewrite(req);
    ROCLINT_CHECK(resp.success && resp.proposal.has_value());
    const auto& lateProposal = *resp.proposal;

    // While rewrite was running in background thread, user typed in editor -> revision bumped to rev_2!
    std::string currentRevisionId = "rev_2";

    auto valResult = RewriteValidator::validateProposal(lateProposal, currentRevisionId, intent);
    ROCLINT_CHECK(!valResult.isValid);
    ROCLINT_CHECK(valResult.finalStatus == ProposalStatus::StaleProposal);
    ROCLINT_CHECK(valResult.rejectionReason.find("does not match currentRevisionId") != std::string::npos);
    std::cout << "  [PASS] Late proposal arriving after draft edit is rejected as Stale.\n";
}

void testScenario3NeedsClarificationPath() {
    std::cout << "--- 3. Needs Clarification Provider Flow ---\n";
    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i08_3";
    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snapshot, {});

    GraniteRewriteAdapter adapter;
    GraniteRewriteRequest req{
        .sessionId = "sess_i08",
        .revisionId = "rev_1",
        .promptString = "NEEDS_CLARIFICATION_TRIGGER",
        .confirmedIntent = intent,
        .rewriteProfileId = "GENERIC@1"
    };

    auto resp = adapter.synthesizeRewrite(req);
    ROCLINT_CHECK(resp.success && resp.proposal.has_value());
    const auto& prop = *resp.proposal;
    ROCLINT_CHECK(prop.status == ProposalStatus::NeedsClarification);

    auto val = RewriteValidator::validateProposal(prop, "rev_1", intent);
    ROCLINT_CHECK(!val.isValid);
    ROCLINT_CHECK(val.finalStatus == ProposalStatus::NeedsClarification);
    std::cout << "  [PASS] Provider needs_clarification signal routed correctly without silent bypass.\n";
}

void testScenario4TargetProfileFormattingPreservesConstraints() {
    std::cout << "--- 4. Multi-Profile Synthesis with Invariant Constraints ---\n";
    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i08_4";
    snapshot.baseRevisionId = "rev_1";

    ChoiceGroup g;
    g.id = "component";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_card", .interpretation = "FloatingCard.qml"});
    snapshot.choiceGroups.push_back(g);

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snapshot, {
        ConfirmedItem{.itemId = "component", .chosenValue = "alt_card", .rationale = "Target card"}
    });

    GraniteRewriteAdapter adapter;

    // Profile 1: GENERIC@1
    GraniteRewriteRequest req1{
        .sessionId = "sess_i08",
        .revisionId = "rev_1",
        .promptString = "Update styling",
        .confirmedIntent = intent,
        .rewriteProfileId = "GENERIC@1"
    };
    auto resp1 = adapter.synthesizeRewrite(req1);
    ROCLINT_CHECK(resp1.success && resp1.proposal.has_value());

    // Profile 2: ANTHROPIC_STRUCTURED@1
    GraniteRewriteRequest req2{
        .sessionId = "sess_i08",
        .revisionId = "rev_1",
        .promptString = "Update styling",
        .confirmedIntent = intent,
        .rewriteProfileId = "ANTHROPIC_STRUCTURED@1"
    };
    auto resp2 = adapter.synthesizeRewrite(req2);
    ROCLINT_CHECK(resp2.success && resp2.proposal.has_value());

    // Verify metadata differs
    ROCLINT_CHECK(resp1.proposal->rewriteProfileId == "GENERIC@1");
    ROCLINT_CHECK(resp2.proposal->rewriteProfileId == "ANTHROPIC_STRUCTURED@1");
    ROCLINT_CHECK(resp1.proposal->compiledPrompt != resp2.proposal->compiledPrompt);

    // Both pass A10 validation for the SAME confirmed intent
    auto val1 = RewriteValidator::validateProposal(*resp1.proposal, "rev_1", intent);
    auto val2 = RewriteValidator::validateProposal(*resp2.proposal, "rev_1", intent);
    ROCLINT_CHECK(val1.isValid);
    ROCLINT_CHECK(val2.isValid);
    std::cout << "  [PASS] Different profile structures validate identically against core intent constraints.\n";
}

int main() {
    std::cout << "[TEST] Running I08 Rewrite Checkpoint Integration Walkthrough...\n\n";

    testScenario1ConfirmedIntentThroughB08AndA10();
    testScenario2StaleProposalRejectedOnDraftEdit();
    testScenario3NeedsClarificationPath();
    testScenario4TargetProfileFormattingPreservesConstraints();

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL I08 REWRITE CHECKPOINT WALKTHROUGH SCENARIOS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
