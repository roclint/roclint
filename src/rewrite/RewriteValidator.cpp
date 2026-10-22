#include "roclint/rewrite/RewriteValidator.hpp"

#include <sstream>
#include <vector>

namespace roclint::core::rewrite {

ValidationResult RewriteValidator::validateProposal(
    const RewriteProposal& proposal,
    std::string_view currentRevisionId,
    const intent::ConfirmedIntent& currentIntent) noexcept {

    if (proposal.status == ProposalStatus::NeedsClarification) {
        return ValidationResult{
            .isValid = false,
            .finalStatus = ProposalStatus::NeedsClarification,
            .rejectionReason = "Model requested clarification before synthesizing rewrite"
        };
    }

    if (proposal.status == ProposalStatus::Rejected) {
        return ValidationResult{
            .isValid = false,
            .finalStatus = ProposalStatus::Rejected,
            .rejectionReason = "Proposal was marked rejected by provider"
        };
    }

    if (proposal.baseRevisionId != currentRevisionId) {
        return ValidationResult{
            .isValid = false,
            .finalStatus = ProposalStatus::StaleProposal,
            .rejectionReason = "Proposal baseRevisionId (" + proposal.baseRevisionId +
                               ") does not match currentRevisionId (" + std::string(currentRevisionId) + ")"
        };
    }

    if (proposal.confirmedIntentId != currentIntent.id) {
        return ValidationResult{
            .isValid = false,
            .finalStatus = ProposalStatus::StaleProposal,
            .rejectionReason = "Proposal confirmedIntentId (" + proposal.confirmedIntentId +
                               ") does not match currentIntent.id (" + currentIntent.id + ")"
        };
    }

    if (proposal.sourceSnapshotHash != currentIntent.sourceSnapshotHash) {
        return ValidationResult{
            .isValid = false,
            .finalStatus = ProposalStatus::StaleProposal,
            .rejectionReason = "Snapshot provenance hash mismatch"
        };
    }

    if (!currentIntent.isCurrent) {
        return ValidationResult{
            .isValid = false,
            .finalStatus = ProposalStatus::StaleProposal,
            .rejectionReason = "ConfirmedIntent is no longer current (DRAFT_DIRTY)"
        };
    }

    for (const auto& c : proposal.constraints) {
        if (c.status == ConstraintStatus::Failed) {
            return ValidationResult{
                .isValid = false,
                .finalStatus = ProposalStatus::Rejected,
                .rejectionReason = "Constraint failed: " + c.id + " (" + c.description + ")"
            };
        }
    }

    if (proposal.compiledPrompt.empty()) {
        return ValidationResult{
            .isValid = false,
            .finalStatus = ProposalStatus::Rejected,
            .rejectionReason = "Compiled prompt text cannot be empty"
        };
    }

    return ValidationResult{
        .isValid = true,
        .finalStatus = ProposalStatus::ProposalReady,
        .rejectionReason = ""
    };
}

std::string RewriteValidator::computeAccessibleDiff(
    std::string_view originalPrompt,
    std::string_view compiledPrompt) noexcept {

    auto splitLines = [](std::string_view sv) {
        std::vector<std::string> lines;
        std::string cur;
        for (char c : sv) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            lines.push_back(cur);
        }
        return lines;
    };

    const auto origLines = splitLines(originalPrompt);
    const auto compLines = splitLines(compiledPrompt);

    std::ostringstream oss;

    // Simple line diff with accessible +, -, and space indicators
    std::size_t i = 0;
    std::size_t j = 0;

    while (i < origLines.size() && j < compLines.size()) {
        if (origLines[i] == compLines[j]) {
            oss << "  " << origLines[i] << "\n";
            ++i;
            ++j;
        } else {
            // Check if compLines contains origLines[i] later (insertion before it)
            bool foundLaterInComp = false;
            for (std::size_t k = j + 1; k < compLines.size() && k < j + 5; ++k) {
                if (compLines[k] == origLines[i]) {
                    foundLaterInComp = true;
                    break;
                }
            }

            if (foundLaterInComp) {
                oss << "+ " << compLines[j] << "\n";
                ++j;
            } else {
                oss << "- " << origLines[i] << "\n";
                ++i;
            }
        }
    }

    while (i < origLines.size()) {
        oss << "- " << origLines[i] << "\n";
        ++i;
    }

    while (j < compLines.size()) {
        oss << "+ " << compLines[j] << "\n";
        ++j;
    }

    return oss.str();
}

} // namespace roclint::core::rewrite
