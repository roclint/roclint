#include "roclint/review/ApprovalRecord.hpp"

#include "roclint/core/crypto/Sha256.hpp"

namespace roclint::core::review {

std::string ApprovalRecord::computeBindingHash() const {
    crypto::CanonicalHasher h("roclint.approval.v1");
    h.field(sessionId)
        .field(revisionId)
        .number(stateVersion)
        .field(snapshotId)
        .field(snapshotHash)
        .field(intentId)
        .field(intentHash)
        .field(proposalId)
        .field(proposalHash)
        .field(approvedText)
        .number(draftEditGeneration)
        .number(proposalEditGeneration)
        .field(validatorVersion)
        .field(policyVersion)
        .field(rewriteProfile);
    return h.finalizeHex();
}

namespace {

Status checkContext(const ApprovalContext& ctx) {
    if (ctx.analysis == nullptr || ctx.intent == nullptr || ctx.proposal == nullptr) {
        return makeError(ErrorCode::PreconditionFailed, "Approval requires a current analysis, intent, and proposal");
    }
    if (ctx.revisionStatus == RevisionStatus::DraftDirty) {
        return makeError(ErrorCode::StaleVersion, "The prompt has unsaved edits");
    }
    const auto& proposal = *ctx.proposal;
    const auto& intent = *ctx.intent;
    const auto& analysis = *ctx.analysis;
    if (auto status = intent::IntentProvenanceBuilder::isIntentValid(intent, ctx.revisionId, ctx.revisionStatus, analysis); !status.ok()) return status;

    if (proposal.status != rewrite::ProposalStatus::ProposalReady) {
        return makeError(ErrorCode::PreconditionFailed, "Only a ready proposal can be approved");
    }
    if (rewrite::ProposalHasher::computeProposalHash(proposal) != proposal.proposalHash ||
        !proposal.id.starts_with("prop_") || proposal.id.empty()) {
        return makeError(ErrorCode::ValidationFailed, "Proposal hash does not match its contents");
    }
    if (proposal.sessionId != ctx.sessionId || intent.sessionId != ctx.sessionId ||
        analysis.normalized.sessionId != ctx.sessionId) {
        return makeError(ErrorCode::ValidationFailed, "Approval inputs belong to different sessions");
    }
    if (proposal.baseRevisionId != ctx.revisionId || intent.baseRevisionId != ctx.revisionId ||
        analysis.normalized.baseRevisionId != ctx.revisionId) {
        return makeError(ErrorCode::StaleVersion, "Approval inputs belong to a different revision");
    }
    if (proposal.intentId != intent.id || proposal.intentHash != intent.intentHash) {
        return makeError(ErrorCode::StaleVersion, "Proposal was produced for a different intent");
    }
    if (intent.snapshotId != analysis.id || intent.snapshotHash != analysis.snapshotHash ||
        proposal.snapshotId != analysis.id || proposal.snapshotHash != analysis.snapshotHash) {
        return makeError(ErrorCode::StaleVersion, "Approval inputs belong to a different analysis snapshot");
    }
    if (proposal.proposalEditGeneration != ctx.proposalEditGeneration) {
        return makeError(ErrorCode::StaleVersion, "Proposal edit generation changed");
    }
    return Status::success();
}

} // namespace

Result<ApprovalRecord> ApprovalFactory::createApproval(const ApprovalContext& ctx, std::int64_t nowMs) {
    if (auto status = checkContext(ctx); !status.ok()) {
        return status.error();
    }
    const auto& proposal = *ctx.proposal;
    ApprovalRecord record;
    record.sessionId = ctx.sessionId;
    record.revisionId = ctx.revisionId;
    record.stateVersion = ctx.stateVersion;
    record.snapshotId = ctx.analysis->id;
    record.snapshotHash = ctx.analysis->snapshotHash;
    record.intentId = ctx.intent->id;
    record.intentHash = ctx.intent->intentHash;
    record.proposalId = proposal.id;
    record.proposalHash = proposal.proposalHash;
    record.approvedText = proposal.rewrittenText;
    record.draftEditGeneration = ctx.draftEditGeneration;
    record.proposalEditGeneration = ctx.proposalEditGeneration;
    record.validatorVersion = ctx.validatorVersion;
    record.policyVersion = ctx.policyVersion;
    record.rewriteProfile = proposal.rewriteProfileId;
    record.createdAtMs = nowMs;
    record.isCurrent = true;
    record.bindingHash = record.computeBindingHash();
    record.id = "appr_" + record.bindingHash;
    return record;
}

Status ApprovalFactory::validate(const ApprovalRecord& a, const ApprovalContext& ctx) {
    if (auto status = checkContext(ctx); !status.ok()) {
        return status;
    }
    if (!a.isCurrent) {
        return makeError(ErrorCode::StaleVersion, "Approval was invalidated");
    }
    if (a.bindingHash != a.computeBindingHash() || a.id != "appr_" + a.bindingHash) {
        return makeError(ErrorCode::ValidationFailed, "Approval binding hash does not match its fields");
    }
    const auto& p = *ctx.proposal;
    auto mismatch = [](const char* field) {
        return makeError(ErrorCode::StaleVersion, std::string("Approval no longer matches: ") + field);
    };
    if (a.sessionId != ctx.sessionId) return mismatch("session");
    if (a.revisionId != ctx.revisionId) return mismatch("revision");
    if (a.stateVersion != ctx.stateVersion) return mismatch("state version");
    if (a.snapshotId != ctx.analysis->id) return mismatch("analysis snapshot");
    if (a.snapshotHash != ctx.analysis->snapshotHash) return mismatch("analysis snapshot hash");
    if (a.intentId != ctx.intent->id) return mismatch("intent");
    if (a.intentHash != ctx.intent->intentHash) return mismatch("intent hash");
    if (a.proposalId != p.id) return mismatch("proposal");
    if (a.proposalHash != p.proposalHash) return mismatch("proposal hash");
    if (a.approvedText != p.rewrittenText) return mismatch("approved text");
    if (a.draftEditGeneration != ctx.draftEditGeneration) return mismatch("draft edit generation");
    if (a.proposalEditGeneration != ctx.proposalEditGeneration) return mismatch("proposal edit generation");
    if (a.validatorVersion != ctx.validatorVersion) return mismatch("validator version");
    if (a.policyVersion != ctx.policyVersion) return mismatch("policy version");
    if (a.rewriteProfile != p.rewriteProfileId) return mismatch("rewrite profile");
    return Status::success();
}

} // namespace roclint::core::review
