#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace roclint::core::review {

// C06 approval: "Wording reviewed". Persists explicit proof fields plus a binding hash.
struct ApprovalRecord {
    std::string id{};             // appr_<bindingHash>
    std::string sessionId{};
    std::string revisionId{};
    std::uint64_t stateVersion{0};
    std::string snapshotId{};
    std::string snapshotHash{};
    std::string intentId{};
    std::string intentHash{};
    std::string proposalId{};
    std::string proposalHash{};
    std::string approvedText{};
    std::uint64_t draftEditGeneration{0};
    std::uint64_t proposalEditGeneration{0};
    std::string validatorVersion{};
    std::string policyVersion{};
    std::string rewriteProfile{};
    std::int64_t createdAtMs{0};
    bool isCurrent{true};
    std::string bindingHash{};

    [[nodiscard]] std::string computeBindingHash() const;

    bool operator==(const ApprovalRecord&) const = default;
};

// Live state an approval is created from and validated against.
struct ApprovalContext {
    std::string sessionId{};
    std::string revisionId{};
    std::uint64_t stateVersion{0};
    std::uint64_t draftEditGeneration{0};
    std::uint64_t proposalEditGeneration{0};
    RevisionStatus revisionStatus{RevisionStatus::Clean};
    const analysis::AnalysisSnapshot* analysis{nullptr};
    const intent::ConfirmedIntent* intent{nullptr};
    const rewrite::RewriteProposal* proposal{nullptr};
    std::string validatorVersion{};
    std::string policyVersion{};
};

class ApprovalFactory {
public:
    [[nodiscard]] static Result<ApprovalRecord> createApproval(const ApprovalContext& context, std::int64_t nowMs);

    [[nodiscard]] static Status validate(const ApprovalRecord& approval, const ApprovalContext& context);

    [[nodiscard]] static bool isApprovalValid(const ApprovalRecord& approval, const ApprovalContext& context) {
        return validate(approval, context).ok();
    }
};

} // namespace roclint::core::review
