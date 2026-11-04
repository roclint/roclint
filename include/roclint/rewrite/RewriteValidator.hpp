#pragma once

#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

inline constexpr const char* kValidatorVersion = "rewrite_validator_v3";

struct ValidationResult {
    bool structurallyValid{false};   // Binding, hash, diff, and constraint derivation all verified
    bool isValid{false};             // structurallyValid and every review item acknowledged
    ProposalStatus finalStatus{ProposalStatus::Rejected};
    std::string rejectionReason{};
    std::vector<std::string> pendingAcknowledgments{};

    bool operator==(const ValidationResult&) const = default;
};

class RewriteValidator {
public:
    // Narrow checks for known requirement changes, not a semantic-equivalence claim.
    [[nodiscard]] static Status validateFidelity(std::string_view original,
                                                const intent::ConfirmedIntent& intent,
                                                std::string_view rewritten);
    [[nodiscard]] static ValidationResult validateProposal(const RewriteProposal& proposal,
                                                           std::string_view currentSessionId,
                                                           std::string_view currentRevisionId,
                                                           const intent::ConfirmedIntent& currentIntent,
                                                           const std::set<std::string>& acknowledgedConstraintIds);

    // Deterministic constraint derivation. Claims are MechanicallyVerified only when the check
    // below actually proves them; everything else requires semantic review by the user.
    [[nodiscard]] static std::vector<ConstraintItem> deriveConstraints(const intent::ConfirmedIntent& intent,
                                                                       const RewriteProfileConfig& profile,
                                                                       std::string_view rewrittenText);

    // LCS line diff. "  " unchanged, "- " removed, "+ " added, "~ was:"/"~ now:" modified pair.
    [[nodiscard]] static std::string computeAccessibleDiff(std::string_view originalPrompt,
                                                           std::string_view rewrittenPrompt);
};

} // namespace roclint::core::rewrite
