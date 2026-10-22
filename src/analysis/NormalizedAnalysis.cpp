#include "roclint/analysis/NormalizedAnalysis.hpp"

#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace roclint::core::analysis {

std::string generateDeterministicId(std::string_view prefix, std::size_t index, std::string_view content) {
    // 64-bit FNV-1a hash
    std::uint64_t hash = 14695981039346656037ULL;
    for (char c : content) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }

    std::ostringstream oss;
    oss << prefix << "_" << index << "_" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

AnalysisValidationResult validateNormalizedAnalysis(const NormalizedAnalysis& analysis, std::string_view promptText) noexcept {
    AnalysisValidationResult result{};

    if (analysis.baseRevisionId.empty()) {
        result.status = AnalysisValidationStatus::Invalid;
        result.issues.emplace_back("Base revision ID is missing or empty.");
    }

    for (std::size_t i = 0; i < analysis.choiceGroups.size(); ++i) {
        const auto& group = analysis.choiceGroups[i];

        if (group.id.empty()) {
            result.status = AnalysisValidationStatus::Invalid;
            result.issues.emplace_back("ChoiceGroup at index " + std::to_string(i) + " has an empty ID.");
        }

        // Cardinality Check 
        if (group.alternatives.size() < ChoiceGroup::kMinAlternatives ||
            group.alternatives.size() > ChoiceGroup::kMaxAlternatives) {
            if (result.status != AnalysisValidationStatus::Invalid) {
                result.status = AnalysisValidationStatus::RequiresManualReview;
            }
            result.issues.emplace_back("ChoiceGroup '" + group.id + "' violates cardinality: must have 2 or 3 alternatives, found " +
                                       std::to_string(group.alternatives.size()));
        }

        // Exact-duplicate check
        std::unordered_set<std::string> seenInterpretations;
        for (std::size_t j = 0; j < group.alternatives.size(); ++j) {
            const auto& alt = group.alternatives[j];
            if (alt.interpretation.empty()) {
                if (result.status != AnalysisValidationStatus::Invalid) {
                    result.status = AnalysisValidationStatus::RequiresManualReview;
                }
                result.issues.emplace_back("ChoiceAlternative at index " + std::to_string(j) + " in group '" + group.id + "' has an empty interpretation.");
            } else if (!seenInterpretations.insert(alt.interpretation).second) {
                if (result.status != AnalysisValidationStatus::Invalid) {
                    result.status = AnalysisValidationStatus::RequiresManualReview;
                }
                result.issues.emplace_back("ChoiceGroup '" + group.id + "' contains duplicate alternative interpretation: '" + alt.interpretation + "'");
            }

            // Evidence spans check against frozen prompt
            for (const auto& span : alt.evidence) {
                if (!isValidSpan(promptText, span)) {
                    result.status = AnalysisValidationStatus::Invalid;
                    result.issues.emplace_back("ChoiceAlternative '" + alt.id + "' contains invalid or out-of-bounds SourceSpan [" +
                                               std::to_string(span.startByte) + ", " + std::to_string(span.endByte) + ")");
                }
            }
        }
    }

    // Validate assumptions evidence spans
    for (const auto& assumption : analysis.assumptions) {
        for (const auto& span : assumption.evidence) {
            if (!isValidSpan(promptText, span)) {
                result.status = AnalysisValidationStatus::Invalid;
                result.issues.emplace_back("Assumption '" + assumption.id + "' contains invalid or out-of-bounds SourceSpan [" +
                                           std::to_string(span.startByte) + ", " + std::to_string(span.endByte) + ")");
            }
        }
    }

    return result;
}

} 
