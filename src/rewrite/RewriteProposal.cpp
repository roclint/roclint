#include "roclint/rewrite/RewriteProposal.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>

namespace roclint::core::rewrite {

bool RewriteProposal::hasConstraint(std::string_view constraintId) const {
    return std::any_of(constraints.begin(), constraints.end(),
                       [&](const ConstraintItem& c) { return c.id == constraintId; });
}

std::optional<ConstraintItem> RewriteProposal::getConstraint(std::string_view constraintId) const {
    for (const auto& c : constraints) {
        if (c.id == constraintId) {
            return c;
        }
    }
    return std::nullopt;
}

bool RewriteProposal::allRequiredConstraintsMet() const {
    return std::none_of(constraints.begin(), constraints.end(),
                        [](const ConstraintItem& c) { return c.status == ConstraintStatus::Failed; });
}

std::string ProposalHasher::computeProposalHash(const RewriteProposal& p) {
    // Ungrounded proposals keep the historical v1 domain byte-for-byte so stored
    // hashes remain verifiable. Verified AST evidence opts into v2 and binds the
    // exact workspace/file/signature snapshot used for grounding.
    const bool astBound = !p.astSnapshotHash.empty();
    crypto::CanonicalHasher h(astBound ? "roclint.rewrite_proposal.v2"
                                       : "roclint.rewrite_proposal.v1");
    h.field(p.sessionId)
        .field(p.baseRevisionId)
        .field(p.intentId)
        .field(p.intentHash)
        .field(p.snapshotId)
        .field(p.snapshotHash)
        .field(p.rewriteProfileId)
        .field(p.modelId)
        .field(p.modelDigest)
        .field(p.quantization)
        .field(p.transport)
        .field(p.originalText)
        .field(p.rewrittenText)
        .field(p.diffText)
        .field(p.clarificationQuestion)
        .field(toString(p.status))
        .number(p.proposalEditGeneration);
    if (astBound) h.field(p.astSnapshotHash);
    h.number(p.constraints.size());
    for (const auto& c : p.constraints) {
        h.field(c.id).field(c.description).field(toString(c.status)).field(c.evidence);
    }
    return h.finalizeHex();
}

void ProposalHasher::seal(RewriteProposal& proposal) {
    proposal.proposalHash = computeProposalHash(proposal);
    proposal.id = "prop_" + proposal.proposalHash;
}

const char* toString(ProposalStatus status) noexcept {
    switch (status) {
        case ProposalStatus::ProposalReady: return "PROPOSAL_READY";
        case ProposalStatus::NeedsClarification: return "NEEDS_CLARIFICATION";
        case ProposalStatus::StaleProposal: return "STALE_PROPOSAL";
        case ProposalStatus::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}

const char* toString(ConstraintStatus status) noexcept {
    switch (status) {
        case ConstraintStatus::MechanicallyVerified: return "MECHANICALLY_VERIFIED";
        case ConstraintStatus::UserConfirmed: return "USER_CONFIRMED";
        case ConstraintStatus::SemanticReviewRequired: return "SEMANTIC_REVIEW_REQUIRED";
        case ConstraintStatus::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

} // namespace roclint::core::rewrite
