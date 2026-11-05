#pragma once

#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

enum class ProposalStatus {
    ProposalReady,
    NeedsClarification,
    StaleProposal,
    Rejected
};

// C05 constraint states. Only deterministic checks may claim MechanicallyVerified.
enum class ConstraintStatus {
    MechanicallyVerified,
    UserConfirmed,
    SemanticReviewRequired,
    Failed
};

struct ConstraintItem {
    std::string id{};
    std::string description{};
    ConstraintStatus status{ConstraintStatus::SemanticReviewRequired};
    std::string evidence{};

    bool operator==(const ConstraintItem&) const = default;
};

// C06 rewrite proposal: bound to an exact revision, intent version, profile, and model identity.
struct RewriteProposal {
    std::string id{};             // prop_<proposalHash>
    std::string proposalHash{};
    std::string sessionId{};
    std::string baseRevisionId{};
    std::string intentId{};
    std::string intentHash{};
    std::string snapshotId{};
    std::string snapshotHash{};
    std::string astSnapshotHash{}; // Verified AST evidence; empty retains the v1 hash domain
    std::string rewriteProfileId{"GENERIC@1"};
    std::string modelId{};
    std::string modelDigest{};
    std::string quantization{};
    std::string transport{};
    std::string originalText{};
    std::string rewrittenText{};
    std::string diffText{};
    std::string clarificationQuestion{};
    ProposalStatus status{ProposalStatus::ProposalReady};
    std::vector<ConstraintItem> constraints{};
    std::uint64_t proposalEditGeneration{0};
    std::int64_t createdAtMs{0};

    [[nodiscard]] bool hasConstraint(std::string_view constraintId) const;
    [[nodiscard]] std::optional<ConstraintItem> getConstraint(std::string_view constraintId) const;
    [[nodiscard]] bool allRequiredConstraintsMet() const;

    bool operator==(const RewriteProposal&) const = default;
};

class ProposalHasher {
public:
    // Canonical SHA-256 over every bound field (createdAtMs and the derived id/hash excluded).
    [[nodiscard]] static std::string computeProposalHash(const RewriteProposal& proposal);

    // Sets proposalHash and id = "prop_" + proposalHash.
    static void seal(RewriteProposal& proposal);
};

[[nodiscard]] const char* toString(ProposalStatus status) noexcept;
[[nodiscard]] const char* toString(ConstraintStatus status) noexcept;

} // namespace roclint::core::rewrite
