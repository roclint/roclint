#pragma once

#include "roclint/analysis/SourceSpan.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::analysis {

// A single concrete option within a ChoiceGroup.
struct ChoiceAlternative {
    std::string id{};                     // Host-minted ID (alt_<groupIdx>_<altIdx>); never provider-owned
    std::string interpretation{};         // Human-readable label shown to the user
    std::string whyMaterial{};            // Materiality rationale explaining the structural difference
    std::string canonicalSemanticsJson{}; // Optional structured payload
    std::vector<SourceSpan> evidence{};   // Spans validated against the frozen revision text

    bool operator==(const ChoiceAlternative&) const = default;
};

// A discrete ambiguous decision requiring user clarity (strictly 2 to 3 alternatives).
struct ChoiceGroup {
    static constexpr std::size_t kMinAlternatives = 2;
    static constexpr std::size_t kMaxAlternatives = 3;

    std::string id{};                     // Host-minted ID (grp_<idx>)
    std::string dimension{};
    std::string question{};
    std::vector<ChoiceAlternative> alternatives{};
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

// A missing specification required before rewriting.
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

// Validated pre-score discovery for one frozen PromptRevision. This is not the final AnalysisSnapshot.
struct NormalizedAnalysis {
    std::string id{};             // Legacy pre-score identity: snap_<snapshotHash>
    std::string sessionId{};
    std::string baseRevisionId{};
    std::string snapshotHash{};   // Canonical SHA-256 over every field except id/snapshotHash
    std::vector<ChoiceGroup> choiceGroups{};
    std::vector<Assumption> assumptions{};
    std::vector<MissingDecision> missingDecisions{};
    std::vector<Conflict> conflicts{};

    bool operator==(const NormalizedAnalysis&) const = default;
};

enum class AnalysisValidationStatus {
    Valid,                // Mechanically valid pre-score structure.
    RequiresManualReview, // Cardinality or structural bounds violate invariants; cannot auto-prompt.
    Invalid               // Corrupted data, duplicate IDs, out-of-bounds spans, or missing revision link.
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

[[nodiscard]] std::string mintGroupId(std::size_t groupIndex);
[[nodiscard]] std::string mintAlternativeId(std::size_t groupIndex, std::size_t altIndex);
[[nodiscard]] std::string mintAssumptionId(std::size_t index);
[[nodiscard]] std::string mintMissingDecisionId(std::size_t index);
[[nodiscard]] std::string mintConflictId(std::size_t index);

// Length-prefixed SHA-256 over the session, base revision, and every item field including spans.
[[nodiscard]] std::string computeSnapshotHash(const NormalizedAnalysis& analysis);

// Computes snapshotHash and sets id = "snap_" + snapshotHash.
void sealSnapshot(NormalizedAnalysis& analysis);

// True when id/snapshotHash match a fresh recomputation.
[[nodiscard]] bool isSnapshotSealed(const NormalizedAnalysis& analysis);

// Indices into analysis.assumptions ordered by first evidence byte (unanchored last), then by ID
// byte order. Every gate that selects an assumption must use this ordering.
[[nodiscard]] std::vector<std::size_t> orderedAssumptionIndices(const NormalizedAnalysis& analysis);

// Validates an entire NormalizedAnalysis snapshot against its frozen base prompt text and C03 invariants.
[[nodiscard]] AnalysisValidationResult validateNormalizedAnalysis(const NormalizedAnalysis& analysis,
                                                                  std::string_view promptText);

} // namespace roclint::core::analysis
