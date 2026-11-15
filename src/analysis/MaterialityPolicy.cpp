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

[[nodiscard]] bool groupsCollide(const ChoiceGroup& a, const ChoiceGroup& b) noexcept {
    bool hasOverlap = false;
    bool hasPartialOverlap = false;
    for (const auto& altA : a.alternatives) {
        for (const auto& spanA : altA.evidence) {
            for (const auto& altB : b.alternatives) {
                for (const auto& spanB : altB.evidence) {
                    if (MaterialityPolicyV1::spansOverlap(spanA, spanB)) {
                        hasOverlap = true;
                        const bool aContainsB = spanA.startByte <= spanB.startByte && spanA.endByte >= spanB.endByte;
                        const bool bContainsA = spanB.startByte <= spanA.startByte && spanB.endByte >= spanA.endByte;
                        hasPartialOverlap |= !aContainsB && !bContainsA;
                    }
                }
            }
        }
    }
    if (!hasOverlap) return false;
    if (hasPartialOverlap) return true;

    // Different wording does not prove different decisions. Two groups may be paraphrases of the
    // same latency question while sharing a "faster" span. Only an explicit, distinct decision
    // dimension supplied by the model makes overlapping evidence legitimate exploration.
    // Missing or identical dimensions stay conservative: overlapping evidence collides.
    const std::string dimensionA = toLowerString(trimWhitespace(a.dimension));
    const std::string dimensionB = toLowerString(trimWhitespace(b.dimension));
    if (!dimensionA.empty() && !dimensionB.empty() && dimensionA != dimensionB) {
        bool duplicateLabels = false;
        for (const auto& altA : a.alternatives) {
            const std::string la = toLowerString(trimWhitespace(altA.interpretation));
            for (const auto& altB : b.alternatives) {
                const std::string lb = toLowerString(trimWhitespace(altB.interpretation));
                if (!la.empty() && la == lb) {
                    duplicateLabels = true;
                    break;
                }
            }
            if (duplicateLabels) break;
        }
        if (!duplicateLabels) {
            return false;
        }
    }
    return true;
}

} // namespace

const GroupEvaluationResult* OverallPolicyResult::findGroup(std::string_view groupId) const noexcept {
    for (const auto& g : groupResults) {
        if (g.groupId == groupId) {
            return &g;
        }
    }
    return nullptr;
}

bool MaterialityPolicyV1::spansOverlap(const SourceSpan& a, const SourceSpan& b) noexcept {
    if (a.empty() || b.empty()) {
        return false;
    }
    return (std::max(a.startByte, b.startByte) < std::min(a.endByte, b.endByte));
}

GroupEvaluationResult MaterialityPolicyV1::evaluateGroup(const ChoiceGroup& group, std::string_view promptBuffer) {
    GroupEvaluationResult result;
    result.groupId = group.id;

    auto reject = [&](ReasonCode code, std::string message) {
        result.verdict = PolicyVerdict::Rejected;
        result.reasonCode = code;
        result.diagnosticMessage = std::move(message);
        return result;
    };

    if (!group.material) {
        return reject(ReasonCode::NotMaterial, "ChoiceGroup is not marked material; it does not warrant a question.");
    }

    if (group.alternatives.size() < kMinAlternatives) {
        return reject(ReasonCode::InsufficientAlternatives,
                      "ChoiceGroup contains fewer than 2 alternatives; cannot form a choice card.");
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
            return reject(ReasonCode::UngroundedMateriality, "ChoiceAlternative is missing a host-minted ID.");
        }

        const auto trimmedLabel = trimWhitespace(alt.interpretation);
        if (trimmedLabel.empty()) {
            return reject(ReasonCode::UngroundedMateriality, "ChoiceAlternative has an empty or whitespace-only label.");
        }

        if (alt.whyMaterial.empty() && alt.canonicalSemanticsJson.empty()) {
            return reject(ReasonCode::UngroundedMateriality,
                          "ChoiceAlternative '" + alt.id + "' lacks whyMaterial rationale.");
        }

        for (const auto& span : alt.evidence) {
            hasAtLeastOneSpan = true;
            if (span.empty() || !isValidSpan(promptBuffer, span)) {
                return reject(ReasonCode::InvalidUtf8SpanBoundary,
                              "SourceSpan [" + std::to_string(span.startByte) + ", " + std::to_string(span.endByte) +
                                  ") is empty, out-of-bounds, or slices a UTF-8 codepoint.");
            }
        }

        const std::string lowerI = toLowerString(trimmedLabel);
        for (std::size_t j = i + 1; j < group.alternatives.size(); ++j) {
            const auto trimmedJ = trimWhitespace(group.alternatives[j].interpretation);
            if (lowerI == toLowerString(trimmedJ)) {
                return reject(ReasonCode::DuplicateAlternativeLabels,
                              "Duplicate or trivially identical alternative labels detected: '" +
                                  std::string(trimmedLabel) + "'");
            }
        }
    }

    if (!hasAtLeastOneSpan) {
        return reject(ReasonCode::UngroundedMateriality,
                      "ChoiceGroup has no attached SourceSpan evidence in the prompt buffer.");
    }

    result.verdict = PolicyVerdict::EligibleForChoice;
    result.reasonCode = ReasonCode::Ok;
    result.diagnosticMessage = "ChoiceGroup is structurally valid and eligible for a choice card.";
    return result;
}

OverallPolicyResult MaterialityPolicyV1::evaluatePrompt(const NormalizedAnalysis& analysis,
                                                        std::string_view promptBuffer) {
    OverallPolicyResult result;
    result.assumptionCount = analysis.assumptions.size();
    result.conflictCount = analysis.conflicts.size();
    result.blockingConflictCount = static_cast<std::size_t>(
        std::count_if(analysis.conflicts.begin(), analysis.conflicts.end(),
                      [](const Conflict& c) { return c.blocking; }));
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

    auto demoteForOverlap = [&](std::size_t idx, const std::string& otherId) {
        auto& g = result.groupResults[idx];
        if (g.verdict == PolicyVerdict::Rejected) {
            return; // Rejected is sticky.
        }
        g.verdict = PolicyVerdict::NeedsManualReview;
        g.reasonCode = ReasonCode::UnsafeSpanOverlap;
        g.diagnosticMessage = "Evidence overlaps with ChoiceGroup '" + otherId + "'; requires manual review.";
    };

    for (std::size_t i = 0; i < analysis.choiceGroups.size(); ++i) {
        for (std::size_t j = i + 1; j < analysis.choiceGroups.size(); ++j) {
            if (result.groupResults[i].verdict == PolicyVerdict::Rejected ||
                result.groupResults[j].verdict == PolicyVerdict::Rejected) {
                continue;
            }
            if (groupsCollide(analysis.choiceGroups[i], analysis.choiceGroups[j])) {
                result.hasCrossGroupOverlap = true;
                demoteForOverlap(i, analysis.choiceGroups[j].id);
                demoteForOverlap(j, analysis.choiceGroups[i].id);
            }
        }
    }

    if (result.assumptionCount > kMaxAssumptionBudget) {
        result.overallVerdict = PolicyVerdict::NeedsManualReview;
        result.primaryReasonCode = ReasonCode::ExceedsAssumptionCap;
        return result;
    }

    if (result.blockingConflictCount > 0) {
        result.overallVerdict = PolicyVerdict::NeedsManualReview;
        result.primaryReasonCode = ReasonCode::DirectConflictPresent;
        return result;
    }

    if (result.groupResults.empty()) {
        result.overallVerdict = PolicyVerdict::EligibleForChoice;
        result.primaryReasonCode = ReasonCode::Ok;
        return result;
    }

    const bool anyEligible = std::any_of(result.groupResults.begin(), result.groupResults.end(),
                                         [](const GroupEvaluationResult& g) { return g.isEligible(); });
    if (anyEligible) {
        result.overallVerdict = PolicyVerdict::EligibleForChoice;
        result.primaryReasonCode = ReasonCode::Ok;
        return result;
    }

    const auto manual = std::find_if(result.groupResults.begin(), result.groupResults.end(),
                                     [](const GroupEvaluationResult& g) {
                                         return g.verdict == PolicyVerdict::NeedsManualReview;
                                     });
    if (manual != result.groupResults.end()) {
        result.overallVerdict = PolicyVerdict::NeedsManualReview;
        result.primaryReasonCode = manual->reasonCode;
    } else {
        result.overallVerdict = PolicyVerdict::Rejected;
        result.primaryReasonCode = result.groupResults.front().reasonCode;
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
        case ReasonCode::NotMaterial: return "NOT_MATERIAL";
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
