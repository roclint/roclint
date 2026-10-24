#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/review/ApprovalRecord.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "roclint/storage/Database.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "support/check.hpp"
#include "support/storage_fixtures.hpp"

#include <iostream>
#include <set>

using namespace roclint::core;
using namespace roclint::bridge;
using namespace roclint::storage;

namespace {

// Full flow: commit -> discovery -> clarification -> intent -> rewrite -> review -> approval,
// persisting each artifact and re-validating after restore.
void testEndToEndApprovalAndRestore() {
    Database db;
    ROCLINT_CHECK(db.open(":memory:").ok());
    PromptRepository repo(db);
    ROCLINT_CHECK(repo.migrate().ok());

    const SessionId sid("sess_flow");
    RevisionState revisions(
        PromptRevision(sid, RevisionState::makeRevisionId(sid, 1), std::nullopt, fx::kChainPrompt, 10));
    ROCLINT_CHECK(repo.createSessionWithRootRevision(revisions.currentRevision(), 10).ok());
    const std::string rev = revisions.currentRevision().revisionId().value();

    GraniteAdapter runtime;
    ROCLINT_CHECK(runtime.beginLoading());
    ROCLINT_CHECK(runtime.completeLoading(fx::testModel()));

    auto discovered = runtime.discoverOptions(
        GraniteOptionDiscoveryRequest{sid.value(), rev, fx::kChainPrompt, "CODE_EDITOR"},
        [](std::string_view) -> Result<std::string> {
            return std::string(R"({"status": "options_generated", "choice_groups": [
              {"question": "Which kind of speed matters?", "alternatives": [
                {"label": "Lower latency per call", "why_material": "Optimizes single requests", "evidence": ["runs faster"]},
                {"label": "Higher batch throughput", "why_material": "Optimizes bulk jobs", "evidence": ["runs faster"]}]}]})");
        });
    ROCLINT_CHECK(discovered.ok());
    fx::Pipeline p(fx::kChainPrompt, discovered.value().analysis);
    fx::recordSnapshotAttempts(repo, fx::finalSnapshot(p.analysis, p.text, p.eval));
    ROCLINT_CHECK(repo.insertAnalysisSnapshot(fx::finalSnapshot(p.analysis, p.text, p.eval), 20).ok());

    ROCLINT_CHECK(intent::InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_0").ok());
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == intent::InterventionVerdict::Quiet);
    const auto confirmed = intent::IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 30).value();
    ROCLINT_CHECK(repo.insertIntentVersion(confirmed, 30).ok());

    GraniteRewriteRequest req;
    req.sessionId = sid.value();
    req.revisionId = rev;
    req.originalText = fx::kChainPrompt;
    auto finalAnalysis = fx::finalSnapshot(p.analysis, p.text, p.eval);
    req.analysis = finalAnalysis;
    req.confirmedIntent = confirmed;
    req.rewriteProfileId = "OPENAI_STRUCTURED@1";
    req.proposalEditGeneration = revisions.proposalEditGeneration();
    auto proposal = GraniteRewriteAdapter::synthesizeRewrite(
        req, runtime,
        [](std::string_view) -> Result<std::string> {
            return fx::rewriteReply("# Goal\nRefactor the parser for lower latency per call.");
        },
        40);
    ROCLINT_CHECK(proposal.ok());
    ROCLINT_CHECK(revisions.noteProposalEdit(revisions.stateVersion()).ok());
    ROCLINT_CHECK(repo.advanceSessionState(revisions, revisions.stateVersion() - 1, 40).ok());
    ROCLINT_CHECK(repo.insertProposal(proposal.value(), 40).ok());

    const auto validation =
        rewrite::RewriteValidator::validateProposal(proposal.value(), sid.value(), rev, confirmed, {});
    ROCLINT_CHECK(validation.isValid);

    review::ApprovalContext ctx;
    ctx.sessionId = sid.value();
    ctx.revisionId = rev;
    ctx.stateVersion = revisions.stateVersion();
    ctx.draftEditGeneration = revisions.draftEditGeneration();
    ctx.proposalEditGeneration = proposal.value().proposalEditGeneration;
    ctx.revisionStatus = revisions.status();
    ctx.analysis = &finalAnalysis;
    ctx.intent = &confirmed;
    ctx.proposal = &proposal.value();
    ctx.validatorVersion = rewrite::kValidatorVersion;
    ctx.policyVersion = analysis::MaterialityPolicyV1::kPolicyVersion;
    auto approval = review::ApprovalFactory::createApproval(ctx, 50);
    ROCLINT_CHECK(approval.ok());
    ROCLINT_CHECK(review::ApprovalFactory::isApprovalValid(approval.value(), ctx));
    ROCLINT_CHECK(repo.insertApproval(approval.value()).ok());

    // Restore and re-validate every link from storage alone.
    auto restored = repo.restoreSession(sid).value();
    ROCLINT_CHECK(restored.approval.has_value());
    review::ApprovalContext again = ctx;
    again.analysis = &*restored.analysis;
    again.intent = &*restored.intent;
    again.proposal = &*restored.proposal;
    again.stateVersion = restored.session.stateVersion;
    again.draftEditGeneration = restored.session.draftEditGeneration;
    ROCLINT_CHECK(intent::IntentProvenanceBuilder::isIntentValid(*restored.intent, rev, RevisionStatus::Clean,
                                                                 *restored.analysis)
                      .ok());
    ROCLINT_CHECK(rewrite::RewriteValidator::validateProposal(*restored.proposal, sid.value(), rev, *restored.intent, {})
                      .isValid);
    ROCLINT_CHECK(review::ApprovalFactory::isApprovalValid(*restored.approval, again));

    // Typing in the draft makes the approval unusable immediately.
    ROCLINT_CHECK(revisions.noteDraftEdit(revisions.stateVersion()).ok());
    ctx.revisionStatus = revisions.status();
    ctx.draftEditGeneration = revisions.draftEditGeneration();
    ROCLINT_CHECK(!review::ApprovalFactory::isApprovalValid(approval.value(), ctx));
    std::cout << "[Approval Checkpoint] commit -> discovery -> intent -> rewrite -> approval -> restore OK\n";
}

void testReviewItemsMustBeAcknowledgedBeforeApproval() {
    fx::Chain c("sess_a", 1, "Make the parser quicker.");
    const auto pending = rewrite::RewriteValidator::validateProposal(c.proposal, c.session, c.revision, c.intent, {});
    ROCLINT_CHECK(pending.structurallyValid && !pending.isValid);
    std::set<std::string> acks(pending.pendingAcknowledgments.begin(), pending.pendingAcknowledgments.end());
    ROCLINT_CHECK(rewrite::RewriteValidator::validateProposal(c.proposal, c.session, c.revision, c.intent, acks).isValid);
}

} // namespace

int main() {
    std::cout << "[Test Approval Checkpoint] Starting...\n";
    testEndToEndApprovalAndRestore();
    testReviewItemsMustBeAcknowledgedBeforeApproval();
    std::cout << "[Test Approval Checkpoint] All tests PASSED.\n";
    return 0;
}
