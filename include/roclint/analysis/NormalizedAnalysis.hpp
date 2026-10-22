#pragma once

#include "roclint/analysis/SourceSpan.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::analysis {

// Represents a literal text excerpt validated against a prompt buffer.
struct LiteralEvidence {
    SourceSpan span{};
    std::string excerpt{};

    bool operator==(const LiteralEvidence&) const = default;
};

// A single concrete option within a ChoiceGroup.
struct ChoiceAlternative {
    std::string id{};                     // Deterministic local ID; never provider-owned UUID
    std::string interpretation{};         // Human-readable title/description
    std::string whyMaterial{};            // Materiality rationale explaining the structural difference
    std::string canonicalSemanticsJson{}; // Optional structured payload
    std::vector<SourceSpan> evidence{};

    bool operator==(const ChoiceAlternative&) const = default;
};

// A discrete ambiguous decision requiring user clarity (strictly 2 to 3 alternatives).
struct ChoiceGroup {
    static constexpr std::size_t kMinAlternatives = 2;
    static constexpr std::size_t kMaxAlternatives = 3;

    std::string id{};
    std::string dimension{};              // e.g. "target", "action", "scope", "rule_conflict"
    std::string question{};               // e.g. "Which component should handle timeouts?"
    std::vector<ChoiceAlternative> alternatives{}; // Strictly 2 to 3 alternatives
    bool material{true};

    bool operator==(const ChoiceGroup&) const = default;
};

// An unstated premise identified in the prompt.
struct Assumption {
    std::string id{};
    std::string statement{};
    bool material{false};
    std::vector<SourceSpan> evidence{};

    bool operator==(const Assumption&) const = default;
};

// A missing architectural or functional specification required before code generation.
struct MissingDecision {
    std::string id{};
    std::string question{};
    bool blocking{false};

    bool operator==(const MissingDecision&) const = default;
};

// An internal contradiction or incompatible requirements in the prompt.
struct Conflict {
    std::string id{};
    std::string description{};
    bool blocking{true};

    bool operator==(const Conflict&) const = default;
};

// The canonical container for all semantic items produced for a PromptRevision.
struct NormalizedAnalysis {
    std::string id{};
    std::string baseRevisionId{};
    std::vector<ChoiceGroup> choiceGroups{};
    std::vector<Assumption> assumptions{};
    std::vector<MissingDecision> missingDecisions{};
    std::vector<Conflict> conflicts{};

    bool operator==(const NormalizedAnalysis&) const = default;
};

// Outcome status of semantic analysis validation 
enum class AnalysisValidationStatus {
    Valid,                // Fully compliant with Contract C03.
    RequiresManualReview, // Cardinality or structural bounds violate invariants; cannot auto-prompt.
    Invalid               // Corrupted data, out-of-bounds spans, or missing revision link.
};

struct AnalysisValidationResult {
    AnalysisValidationStatus status{AnalysisValidationStatus::Valid};
    std::vector<std::string> issues{};

    [[nodiscard]] bool isValid() const noexcept {
        return status == AnalysisValidationStatus::Valid;
    }

    [[nodiscard]] bool requiresManualReview() const noexcept {
        return status == AnalysisValidationStatus::RequiresManualReview;
    }
};

// Generates a deterministic local ID from item type, index, and content (Contract C03).
[[nodiscard]] std::string generateDeterministicId(std::string_view prefix, std::size_t index, std::string_view content);

// Validates an entire NormalizedAnalysis snapshot against its base prompt text and C03 invariants.
[[nodiscard]] AnalysisValidationResult validateNormalizedAnalysis(const NormalizedAnalysis& analysis, std::string_view promptText) noexcept;

} 
