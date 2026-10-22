#include "roclint/analysis/MaterialityPolicy.hpp"

#include <algorithm>
#include <cctype>

namespace roclint::core::analysis {

namespace {

[[nodiscard]] std::string_view trimWhitespace(std::string_view str) noexcept {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    return str;
}

[[nodiscard]] std::string toLowerString(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

} // namespace

bool MaterialityPolicyV1::spansOverlap(const SourceSpan& a, const SourceSpan& b) noexcept {
    if (a.empty() || b.empty()) {
        return false;
    }
    return (std::max(a.startByte, b.startByte) < std::min(a.endByte, b.endByte));
}

GroupEvaluationResult MaterialityPolicyV1::evaluateGroup(
    const ChoiceGroup& group,
    std::string_view promptBuffer) noexcept {

    GroupEvaluationResult result;
    result.groupId = group.id;

    if (group.alternatives.size() < kMinAlternatives) {
        result.verdict = PolicyVerdict::Rejected;
        result.reasonCode = ReasonCode::InsufficientAlternatives;
        result.diagnosticMessage = "ChoiceGroup contains fewer than 2 alternatives; cannot form a choice card.";
        return result;
    }

    if (group.alternatives.size() > kMaxAlternatives) {
        result.verdict = PolicyVerdict::NeedsManualReview;
        result.reasonCode = ReasonCode::ExcessiveAlternatives;
        result.diagnosticMessage = "ChoiceGroup exceeds maximum capacity of 3 alternatives; requires manual review.";
        return result;
    }

    bool hasAtLeastOneSpan = false;

    for (std::size_t i = 0; i < group.alternatives.size(); ++i) {
        const auto& alt = group.alternatives[i];

        if (alt.id.empty()) {
            result.verdict = PolicyVerdict::Rejected;
            result.reasonCode = ReasonCode::UngroundedMateriality;
            result.diagnosticMessage = "ChoiceAlternative is missing a deterministic ID.";
            return result;
        }

        const auto trimmedLabel = trimWhitespace(alt.interpretation);
        if (trimmedLabel.empty()) {
            result.verdict = PolicyVerdict::Rejected;
            result.reasonCode = ReasonCode::UngroundedMateriality;
            result.diagnosticMessage = "ChoiceAlternative has an empty or whitespace-only label.";
            return result;
        }

        if (alt.whyMaterial.empty() && alt.canonicalSemanticsJson.empty()) {
            result.verdict = PolicyVerdict::Rejected;
            result.reasonCode = ReasonCode::UngroundedMateriality;
            result.diagnosticMessage = "ChoiceAlternative '" + alt.id + "' lacks whyMaterial rationale.";
            return result;
        }

        for (const auto& span : alt.evidence) {
            hasAtLeastOneSpan = true;
            if (!isValidSpan(promptBuffer, span)) {
                result.verdict = PolicyVerdict::Rejected;
                result.reasonCode = ReasonCode::InvalidUtf8SpanBoundary;
                result.diagnosticMessage = "SourceSpan [" + std::to_string(span.startByte) + ", " +
                                           std::to_string(span.endByte) + ") is out-of-bounds or slices a UTF-8 codepoint.";
                return result;
            }
        }

        const std::string lowerI = toLowerString(trimmedLabel);
        for (std::size_t j = i + 1; j < group.alternatives.size(); ++j) {
            const auto trimmedJ = trimWhitespace(group.alternatives[j].interpretation);
            if (lowerI == toLowerString(trimmedJ)) {
                result.verdict = PolicyVerdict::Rejected;
                result.reasonCode = ReasonCode::DuplicateAlternativeLabels;
                result.diagnosticMessage = "Duplicate or trivially identical alternative labels detected: '" +
                                           std::string(trimmedLabel) + "'";
                return result;
            }
        }
    }

    if (!hasAtLeastOneSpan && !promptBuffer.empty()) {
        result.verdict = PolicyVerdict::Rejected;
        result.reasonCode = ReasonCode::UngroundedMateriality;
        result.diagnosticMessage = "ChoiceGroup has no attached SourceSpan evidence in prompt buffer.";
        return result;
    }

    result.verdict = PolicyVerdict::EligibleForChoice;
    result.reasonCode = ReasonCode::Ok;
    result.diagnosticMessage = "ChoiceGroup is structurally valid and eligible for 1-click Quick-Fix HUD rendering.";
    return result;
}

OverallPolicyResult MaterialityPolicyV1::evaluatePrompt(
    const NormalizedAnalysis& analysis,
    std::string_view promptBuffer) noexcept {

    OverallPolicyResult result;
    result.assumptionCount = analysis.assumptions.size();
    result.conflictCount = analysis.conflicts.size();
    result.hasCrossGroupOverlap = false;

    if (promptBuffer.empty()) {
        result.overallVerdict = PolicyVerdict::Rejected;
        result.primaryReasonCode = ReasonCode::EmptyPromptBuffer;
        return result;
    }

    result.groupResults.reserve(analysis.choiceGroups.size());
    for (const auto& group : analysis.choiceGroups) {
        result.groupResults.push_back(evaluateGroup(group, promptBuffer));
    }

    for (std::size_t i = 0; i < analysis.choiceGroups.size(); ++i) {
        for (std::size_t j = i + 1; j < analysis.choiceGroups.size(); ++j) {
            bool groupsCollide = false;

            for (const auto& altA : analysis.choiceGroups[i].alternatives) {
                for (const auto& spanA : altA.evidence) {
                    for (const auto& altB : analysis.choiceGroups[j].alternatives) {
                        for (const auto& spanB : altB.evidence) {
                            if (spansOverlap(spanA, spanB)) {
                                groupsCollide = true;
                                break;
                            }
                        }
                        if (groupsCollide) break;
                    }
                    if (groupsCollide) break;
                }
                if (groupsCollide) break;
            }

            if (groupsCollide) {
                result.hasCrossGroupOverlap = true;

                result.groupResults[i].verdict = PolicyVerdict::NeedsManualReview;
                result.groupResults[i].reasonCode = ReasonCode::UnsafeSpanOverlap;
                result.groupResults[i].diagnosticMessage =
                    "Cross-group span overlap with ChoiceGroup '" + analysis.choiceGroups[j].id +
                    "'; 1-click text splicing disabled.";

                result.groupResults[j].verdict = PolicyVerdict::NeedsManualReview;
                result.groupResults[j].reasonCode = ReasonCode::UnsafeSpanOverlap;
                result.groupResults[j].diagnosticMessage =
                    "Cross-group span overlap with ChoiceGroup '" + analysis.choiceGroups[i].id +
                    "'; 1-click text splicing disabled.";
            }
        }
    }

    if (result.assumptionCount > kMaxAssumptionBudget) {
        result.overallVerdict = PolicyVerdict::NeedsManualReview;
        result.primaryReasonCode = ReasonCode::ExceedsAssumptionCap;
        return result;
    }

    if (result.conflictCount > 0) {
        result.overallVerdict = PolicyVerdict::NeedsManualReview;
        result.primaryReasonCode = ReasonCode::DirectConflictPresent;
        return result;
    }

    if (result.hasCrossGroupOverlap) {
        result.overallVerdict = PolicyVerdict::NeedsManualReview;
        result.primaryReasonCode = ReasonCode::UnsafeSpanOverlap;
        return result;
    }

    if (result.groupResults.empty()) {
        result.overallVerdict = PolicyVerdict::EligibleForChoice;
        result.primaryReasonCode = ReasonCode::Ok;
        return result;
    }

    bool anyEligible = false;
    for (const auto& gRes : result.groupResults) {
        if (gRes.verdict == PolicyVerdict::EligibleForChoice) {
            anyEligible = true;
            break;
        }
    }

    if (anyEligible) {
        result.overallVerdict = PolicyVerdict::EligibleForChoice;
        result.primaryReasonCode = ReasonCode::Ok;
    } else {
        bool anyManualReview = false;
        for (const auto& gRes : result.groupResults) {
            if (gRes.verdict == PolicyVerdict::NeedsManualReview) {
                anyManualReview = true;
                result.primaryReasonCode = gRes.reasonCode;
                break;
            }
        }
        if (anyManualReview) {
            result.overallVerdict = PolicyVerdict::NeedsManualReview;
        } else {
            result.overallVerdict = PolicyVerdict::Rejected;
            if (!result.groupResults.empty()) {
                result.primaryReasonCode = result.groupResults.front().reasonCode;
            }
        }
    }

    return result;
}

const char* toString(ReasonCode code) noexcept {
    switch (code) {
        case ReasonCode::Ok: return "OK";
        case ReasonCode::InsufficientAlternatives: return "INSUFFICIENT_ALTERNATIVES";
        case ReasonCode::ExcessiveAlternatives: return "EXCESSIVE_ALTERNATIVES";
        case ReasonCode::DuplicateAlternativeLabels: return "DUPLICATE_ALTERNATIVE_LABELS";
        case ReasonCode::UngroundedMateriality: return "UNGROUNDED_MATERIALITY";
        case ReasonCode::InvalidUtf8SpanBoundary: return "INVALID_UTF8_SPAN_BOUNDARY";
        case ReasonCode::UnsafeSpanOverlap: return "UNSAFE_SPAN_OVERLAP";
        case ReasonCode::DirectConflictPresent: return "DIRECT_CONFLICT_PRESENT";
        case ReasonCode::ExceedsAssumptionCap: return "EXCEEDS_ASSUMPTION_CAP";
        case ReasonCode::EmptyPromptBuffer: return "EMPTY_PROMPT_BUFFER";
    }
    return "UNKNOWN";
}

const char* toString(PolicyVerdict verdict) noexcept {
    switch (verdict) {
        case PolicyVerdict::EligibleForChoice: return "ELIGIBLE_FOR_CHOICE";
        case PolicyVerdict::NeedsManualReview: return "NEEDS_MANUAL_REVIEW";
        case PolicyVerdict::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}

} // namespace roclint::core::analysis
