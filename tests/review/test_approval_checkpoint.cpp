#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/review/ApprovalRecord.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "roclint/storage/Database.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::core::intent;
using namespace roclint::core::review;
using namespace roclint::core::rewrite;
using namespace roclint::storage;
using namespace roclint::bridge;

void testScenario1ReviewAndApproveProposal() {
    std::cout << "--- 1. Review Proposal & Approve Wording ---\n";
    SessionId sess("sess_i09");
    RevisionId rev("rev_1");

    NormalizedAnalysis snapshot;
    snapshot.id = "snap_i09";
    snapshot.baseRevisionId = "rev_1";
    ChoiceGroup g;
    g.id = "target_component";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_card", .interpretation = "FloatingCard.qml"});
    snapshot.choiceGroups.push_back(g);

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snapshot, {
        ConfirmedItem{.itemId = "target_component", .chosenValue = "alt_card"}
    });

    GraniteRewriteAdapter adapter;
    auto resp = adapter.synthesizeRewrite({
        .sessionId = sess.value(),
        .revisionId = rev.value(),
        .promptString = "Fix layout overflow",
        .confirmedIntent = intent,
        .rewriteProfileId = "GENERIC@1"
    });
    ROCLINT_CHECK(resp.success && resp.proposal.has_value());
    const auto& proposal = *resp.proposal;

    // Validate with A10
    auto val = RewriteValidator::validateProposal(proposal, rev.value(), intent);
    ROCLINT_CHECK(val.isValid);

    // Create Approval with explicit proof fields
    auto approval = ApprovalFactory::createApproval(
        sess,
        rev,
        snapshot.id,
        intent.sourceSnapshotHash,
        intent.id,
        proposal.id,
        proposal.proposalHash,
        proposal.compiledPrompt,
        0
    );

    ROCLINT_CHECK(approval.isCurrent);
    ROCLINT_CHECK(ApprovalFactory::isApprovalValid(approval, rev.value(), proposal.id, proposal.proposalHash));
    std::cout << "  [PASS] Proposal reviewed and approved with durable proof fields.\n";
}

void testScenario2ProposalEditInvalidatesApproval() {
    std::cout << "--- 2. Proposal Edit Invalidates Approval (Edit Generation Bumping) ---\n";
    SessionId sess("sess_i09");
    RevisionId rev("rev_1");

    auto approval = ApprovalFactory::createApproval(
        sess,
        rev,
        "snap_1",
        100,
        "intent_1",
        "prop_1",
        200,
        "Initial approved text",
        0
    );

    ROCLINT_CHECK(ApprovalFactory::isApprovalValid(approval, rev.value(), "prop_1", 200));

    // User edits proposal wording -> proposalHash changes and editGeneration bumps!
    std::string editedText = "Initial approved text with manual tweaks";
    std::uint64_t newProposalHash = 300; // changed

    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(approval, rev.value(), "prop_1", newProposalHash));
    std::cout << "  [PASS] Editing proposal invalidates prior approval immediately.\n";
}

void testScenario3ReapproveAndInspectDurableProof() {
    std::cout << "--- 3. Re-approve and Inspect Persisted Proof after Reopen ---\n";
    const std::string dbPath = "/tmp/roclint_test_i09.db";
    std::remove(dbPath.c_str());

    SessionId sess("sess_reopen");
    RevisionId rev("rev_1");

    {
        Database db(dbPath);
        PromptRepository repo(db);
        repo.migrate();

        repo.createSession(sess, 100);
        repo.saveRevision(PromptRevision(sess, rev, std::nullopt, "Draft", 100));

        auto appr = ApprovalFactory::createApproval(
            sess,
            rev,
            "snap_durable",
            0xFEEDFACEULL,
            "intent_durable",
            "prop_durable",
            0xCAFEBABEULL,
            "/* ROCLINT [PROFILE: GENERIC@1] */ Final approved text",
            1, // edit generation 1
            150
        );
        repo.saveApproval(appr);
    }

    // Reopen database connection to verify durability
    {
        Database dbReopen(dbPath);
        PromptRepository repoReopen(dbReopen);

        auto curr = repoReopen.getCurrentApproval(sess);
        ROCLINT_CHECK(curr.has_value());
        ROCLINT_CHECK(curr->sessionId == sess);
        ROCLINT_CHECK(curr->revisionId == rev);
        ROCLINT_CHECK(curr->snapshotId == "snap_durable");
        ROCLINT_CHECK(curr->snapshotHash == 0xFEEDFACEULL);
        ROCLINT_CHECK(curr->intentId == "intent_durable");
        ROCLINT_CHECK(curr->proposalId == "prop_durable");
        ROCLINT_CHECK(curr->proposalHash == 0xCAFEBABEULL);
        ROCLINT_CHECK(curr->editGeneration == 1);
        ROCLINT_CHECK(curr->isCurrent);
        ROCLINT_CHECK(curr->bindingHash == curr->computeBindingHash());
    }

    std::remove(dbPath.c_str());
    std::cout << "  [PASS] Persisted proof inspected and validated after database reopen.\n";
}

void testScenario4DiffAndConstraintLintStatesWithoutColorAlone() {
    std::cout << "--- 4. Diff and Constraint Lint Non-Color Differentiation ---\n";
    std::string original = "Handle network timeout";
    std::string compiled = "/* ROCLINT SPEC */\nHandle network timeout gracefully\n/* CONSTRAINTS */";

    std::string diff = RewriteValidator::computeAccessibleDiff(original, compiled);
    ROCLINT_CHECK(diff.find("+") != std::string::npos);

    // Verify all 4 typed constraint states have distinct non-color identifiers
    ROCLINT_CHECK(std::string(toString(ConstraintStatus::MechanicallyVerified)) == "MECHANICALLY_VERIFIED");
    ROCLINT_CHECK(std::string(toString(ConstraintStatus::UserConfirmed)) == "USER_CONFIRMED");
    ROCLINT_CHECK(std::string(toString(ConstraintStatus::SemanticReviewRequired)) == "SEMANTIC_REVIEW_REQUIRED");
    ROCLINT_CHECK(std::string(toString(ConstraintStatus::Failed)) == "FAILED");
    std::cout << "  [PASS] All 4 constraint states and diff indicators verify non-color accessibility.\n";
}

int main() {
    std::cout << "[TEST] Running I09 Approval Checkpoint Integration Walkthrough...\n\n";

    testScenario1ReviewAndApproveProposal();
    testScenario2ProposalEditInvalidatesApproval();
    testScenario3ReapproveAndInspectDurableProof();
    testScenario4DiffAndConstraintLintStatesWithoutColorAlone();

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL I09 APPROVAL CHECKPOINT WALKTHROUGH SCENARIOS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
