#include "roclint/review/ApprovalRecord.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::review;

namespace {

void testCreateBindsEveryField() {
    fx::Chain c;
    const auto ctx = c.context(2, 5);
    auto created = ApprovalFactory::createApproval(ctx, 1234);
    ROCLINT_CHECK(created.ok());
    const auto& a = created.value();
    ROCLINT_CHECK(a.sessionId == c.session);
    ROCLINT_CHECK(a.revisionId == c.revision);
    ROCLINT_CHECK(a.stateVersion == 2);
    ROCLINT_CHECK(a.draftEditGeneration == 5);
    ROCLINT_CHECK(a.proposalEditGeneration == c.proposal.proposalEditGeneration);
    ROCLINT_CHECK(a.snapshotHash == c.snapshot.snapshotHash);
    ROCLINT_CHECK(a.intentHash == c.intent.intentHash);
    ROCLINT_CHECK(a.proposalHash == c.proposal.proposalHash);
    ROCLINT_CHECK(a.approvedText == c.proposal.rewrittenText);
    ROCLINT_CHECK(a.rewriteProfile == c.proposal.rewriteProfileId);
    ROCLINT_CHECK(a.validatorVersion == std::string(rewrite::kValidatorVersion));
    ROCLINT_CHECK(a.isCurrent);
    ROCLINT_CHECK(a.id == "appr_" + a.bindingHash);
    ROCLINT_CHECK(a.bindingHash == a.computeBindingHash());
    ROCLINT_CHECK(ApprovalFactory::isApprovalValid(a, ctx));
}

void testLiveStateChangesInvalidate() {
    fx::Chain c;
    const auto a = c.approve(2);

    auto draftGen = c.context(2, 1);
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, draftGen));

    auto version = c.context(3);
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, version));

    auto dirty = c.context(2);
    dirty.revisionStatus = RevisionStatus::DraftDirty;
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, dirty));

    auto revision = c.context(2);
    revision.revisionId = "rev_sess_a_2";
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, revision));

    auto validator = c.context(2);
    validator.validatorVersion = "rewrite_validator_v1";
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, validator));

    auto policy = c.context(2);
    policy.policyVersion = "materiality_policy_v0";
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, policy));

    auto noProposal = c.context(2);
    noProposal.proposal = nullptr;
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, noProposal));
}

void testProposalGenerationChangeInvalidates() {
    fx::Chain c;
    const auto a = c.approve(2);
    // A regenerated proposal (new generation) for the same intent is a different artifact.
    fx::Chain regenerated("sess_a", 1, "Refactor the parser for lower latency per call.", "GENERIC@1", 1);
    ROCLINT_CHECK(regenerated.proposal.proposalHash != c.proposal.proposalHash);
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, regenerated.context(2)));

    auto ctx = c.context(2);
    ctx.proposalEditGeneration = 1;
    auto st = ApprovalFactory::validate(a, ctx);
    ROCLINT_CHECK(!st.ok() && st.error().code == ErrorCode::StaleVersion);
}

void testApprovedTextChangeInvalidates() {
    fx::Chain c;
    auto a = c.approve(2);
    auto edited = a;
    edited.approvedText += " (edited)";
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(edited, c.context(2)));

    // Re-sealing the edited record still fails: the text no longer matches the proposal.
    edited.bindingHash = edited.computeBindingHash();
    edited.id = "appr_" + edited.bindingHash;
    auto st = ApprovalFactory::validate(edited, c.context(2));
    ROCLINT_CHECK(!st.ok() && st.error().code == ErrorCode::StaleVersion);

    // A different rewrite of the same intent can't reuse the approval.
    fx::Chain other("sess_a", 1, "Refactor the parser: lower latency per call matters most.");
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, other.context(2)));
}

void testInvalidatedApprovalIsRejected() {
    fx::Chain c;
    auto a = c.approve(2);
    a.isCurrent = false;
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(a, c.context(2)));
}

void testCrossSessionRecordsAreRejected() {
    fx::Chain a("sess_a");
    fx::Chain b("sess_b");
    const auto approvalA = a.approve(2);
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(approvalA, b.context(2)));

    // Mixed inputs: session B's proposal with session A's intent and analysis.
    auto mixed = a.context(2);
    mixed.proposal = &b.proposal;
    ROCLINT_CHECK(!ApprovalFactory::createApproval(mixed, 1).ok());

    auto otherSession = a.context(2);
    otherSession.sessionId = "sess_b";
    ROCLINT_CHECK(!ApprovalFactory::createApproval(otherSession, 1).ok());
}

void testOnlyReadyProposalsCanBeApproved() {
    fx::Chain c;
    auto proposal = c.proposal;
    proposal.status = rewrite::ProposalStatus::NeedsClarification;
    rewrite::ProposalHasher::seal(proposal);
    auto ctx = c.context(2);
    ctx.proposal = &proposal;
    ROCLINT_CHECK(!ApprovalFactory::createApproval(ctx, 1).ok());

    auto tampered = c.proposal;
    tampered.rewrittenText = "unsealed edit";
    ctx.proposal = &tampered;
    ROCLINT_CHECK(!ApprovalFactory::createApproval(ctx, 1).ok());
}

} // namespace

int main() {
    std::cout << "[Test ApprovalRecord] Starting...\n";
    testCreateBindsEveryField();
    testLiveStateChangesInvalidate();
    testProposalGenerationChangeInvalidates();
    testApprovedTextChangeInvalidates();
    testInvalidatedApprovalIsRejected();
    testCrossSessionRecordsAreRejected();
    testOnlyReadyProposalsCanBeApproved();
    std::cout << "[Test ApprovalRecord] All tests PASSED.\n";
    return 0;
}
