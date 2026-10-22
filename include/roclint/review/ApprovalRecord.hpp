#pragma once

#include "roclint/core/prompt_state/RevisionId.hpp"
#include "roclint/core/prompt_state/SessionId.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace roclint::core::review {

struct ApprovalRecord {
    std::string id{};
    SessionId sessionId{""};
    RevisionId revisionId{""};
    std::string snapshotId{};
    std::uint64_t snapshotHash{0};
    std::string intentId{};
    std::string proposalId{};
    std::uint64_t proposalHash{0};
    std::string approvedText{};
    int editGeneration{0};
    std::int64_t createdAtMs{0};
    bool isCurrent{true};
    std::uint64_t bindingHash{0};

    [[nodiscard]] std::uint64_t computeBindingHash() const noexcept;

    bool operator==(const ApprovalRecord&) const noexcept = default;
};

class ApprovalFactory {
public:
    [[nodiscard]] static ApprovalRecord createApproval(
        const SessionId& sessionId,
        const RevisionId& revisionId,
        std::string_view snapshotId,
        std::uint64_t snapshotHash,
        std::string_view intentId,
        std::string_view proposalId,
        std::uint64_t proposalHash,
        std::string_view approvedText,
        int editGeneration = 0,
        std::int64_t nowMs = 0) noexcept;

    [[nodiscard]] static bool isApprovalValid(
        const ApprovalRecord& approval,
        std::string_view currentRevisionId,
        std::string_view currentProposalId,
        std::uint64_t currentProposalHash) noexcept;
};

} // namespace roclint::core::review
