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

struct RewriteProposal {
    std::string id{};
    std::string baseRevisionId{};
    std::string confirmedIntentId{};
    std::uint64_t sourceSnapshotHash{0};
    std::string rewriteProfileId{"GENERIC@1"};
    std::string compiledPrompt{};
    std::string diffText{};
    ProposalStatus status{ProposalStatus::ProposalReady};
    std::vector<ConstraintItem> constraints{};
    std::uint64_t proposalHash{0};
    std::uint64_t createdAtTimestamp{0};

    [[nodiscard]] bool hasConstraint(std::string_view constraintId) const noexcept;
    [[nodiscard]] std::optional<ConstraintItem> getConstraint(std::string_view constraintId) const noexcept;
    [[nodiscard]] bool allRequiredConstraintsMet() const noexcept;

    bool operator==(const RewriteProposal&) const = default;
};

class ProposalHasher {
public:
    [[nodiscard]] static std::uint64_t computeProposalHash(const RewriteProposal& proposal) noexcept;
};

[[nodiscard]] const char* toString(ProposalStatus status) noexcept;
[[nodiscard]] const char* toString(ConstraintStatus status) noexcept;

} // namespace roclint::core::rewrite
