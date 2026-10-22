#pragma once

#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"

#include <string>
#include <string_view>

namespace roclint::core::rewrite {

struct ValidationResult {
    bool isValid{false};
    ProposalStatus finalStatus{ProposalStatus::ProposalReady};
    std::string rejectionReason{};

    bool operator==(const ValidationResult&) const = default;
};

class RewriteValidator {
public:
    [[nodiscard]] static ValidationResult validateProposal(
        const RewriteProposal& proposal,
        std::string_view currentRevisionId,
        const intent::ConfirmedIntent& currentIntent) noexcept;

    [[nodiscard]] static std::string computeAccessibleDiff(
        std::string_view originalPrompt,
        std::string_view compiledPrompt) noexcept;
};

} // namespace roclint::core::rewrite
