#include "roclint/rewrite/RewriteProposal.hpp"

namespace roclint::core::rewrite {

namespace {

constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

inline std::uint64_t hashString(std::uint64_t h, std::string_view s) noexcept {
    for (unsigned char c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= FNV_PRIME;
    }
    return h;
}

inline std::uint64_t hashU64(std::uint64_t h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xFF);
        h *= FNV_PRIME;
        v >>= 8;
    }
    return h;
}

} // namespace

bool RewriteProposal::hasConstraint(std::string_view constraintId) const noexcept {
    for (const auto& c : constraints) {
        if (c.id == constraintId) {
            return true;
        }
    }
    return false;
}

std::optional<ConstraintItem> RewriteProposal::getConstraint(std::string_view constraintId) const noexcept {
    for (const auto& c : constraints) {
        if (c.id == constraintId) {
            return c;
        }
    }
    return std::nullopt;
}

bool RewriteProposal::allRequiredConstraintsMet() const noexcept {
    for (const auto& c : constraints) {
        if (c.status == ConstraintStatus::Failed) {
            return false;
        }
    }
    return true;
}

std::uint64_t ProposalHasher::computeProposalHash(const RewriteProposal& proposal) noexcept {
    std::uint64_t h = FNV_OFFSET_BASIS;
    h = hashString(h, proposal.id);
    h = hashString(h, proposal.baseRevisionId);
    h = hashString(h, proposal.confirmedIntentId);
    h = hashU64(h, proposal.sourceSnapshotHash);
    h = hashString(h, proposal.rewriteProfileId);
    h = hashString(h, proposal.compiledPrompt);

    for (const auto& c : proposal.constraints) {
        h = hashString(h, c.id);
        h = hashString(h, c.description);
        h = hashU64(h, static_cast<std::uint64_t>(c.status));
        h = hashString(h, c.evidence);
    }
    return h;
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
