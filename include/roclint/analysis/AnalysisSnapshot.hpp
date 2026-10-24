#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/core/Result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::analysis {

enum class ScorerEngine { JevCloud, JuliaLocal };
enum class ExecutionStatus { ExecutedModel, ExecutedMock, DegradedUnranked, NotRequested };
inline constexpr double kS1ConfidenceThreshold = 0.70;
inline constexpr const char* kS1ThresholdVersion = "v1";
inline constexpr std::size_t kMaxAssumptionChecks = 4;
inline constexpr std::size_t kMaxScoredGroups = 3;

struct ScoringPlan {
    std::vector<std::string> groupIds;
    std::vector<std::string> assumptionIds;
};

struct AssumptionEvaluationScore {
    std::string assumptionId{};
    std::optional<double> uncertaintyScore{std::nullopt};
    bool noulCertainty{false};
    bool isScored{false};
    std::string unscoredReason{};
    bool operator==(const AssumptionEvaluationScore&) const = default;
};

struct AlternativeProbability {
    std::string alternativeId{};
    double probability{0.0};
    bool operator==(const AlternativeProbability&) const = default;
};

struct GroupEvaluationScore {
    std::string groupId{};
    std::optional<std::string> winningAlternativeId{std::nullopt};
    std::optional<std::vector<AlternativeProbability>> probabilities{std::nullopt};
    std::optional<double> ambiguityScore{std::nullopt};
    bool noulCertainty{false};
    bool isScored{false};
    bool operator==(const GroupEvaluationScore&) const = default;
};

// Transport evaluation of a NormalizedAnalysis. The legacy analysisSnapshotId field identifies
// that pre-score payload, not the final AnalysisSnapshot produced after evaluation.
struct SnapshotEvaluationResponse {
    bool success{false};
    std::string errorMessage{};
    std::string degradedReason{};
    std::string analysisSnapshotId{};
    std::vector<GroupEvaluationScore> groupScores{};
    int planVersion{2};
    std::vector<AssumptionEvaluationScore> assumptionScores{};
    double totalLatencyMs{0.0};
    ScorerEngine engine{ScorerEngine::JevCloud};
    std::optional<ScorerEngine> executedEngine{std::nullopt};
    ExecutionStatus executionStatus{ExecutionStatus::DegradedUnranked};
    std::string modelVersion{"none"};
    bool modelInferenceExecuted{false};
    double confidenceThreshold{kS1ConfidenceThreshold};
    std::string thresholdVersion{kS1ThresholdVersion};
    [[nodiscard]] bool isRanked() const noexcept {
        return executionStatus == ExecutionStatus::ExecutedModel || executionStatus == ExecutionStatus::ExecutedMock;
    }
    bool operator==(const SnapshotEvaluationResponse&) const = default;
};

struct GroupPresentation {
    std::string groupId;
    std::vector<std::string> alternativeIds;
    bool operator==(const GroupPresentation&) const = default;
};

// C03: immutable, post-evaluation semantic artifact. Its identity covers discovery, validated
// scoring (or explicit degradation), attempts and the exact deterministic presentation order.
struct AnalysisSnapshot {
    int payloadVersion{3}; // v2 history retains its original hash algorithm and group-only evidence.
    std::string id{}; // analysis_<snapshotHash>; distinct from the pre-score payload identity
    std::string snapshotHash{};
    std::string revisionTextHash{}; // SHA-256 of the complete canonical frozen prompt bytes
    NormalizedAnalysis normalized{};
    SnapshotEvaluationResponse evaluation{};
    std::string discoveryAttemptId{};
    std::string scorerAttemptId{}; // Empty only when no scorer operation was started
    std::string presentationPolicyVersion{"scorer_presentation.v2"};
    std::vector<GroupPresentation> presentation{};
    std::vector<std::string> assumptionOrder{};
    std::optional<PromptTriage> promptTriage{}; // v4: classifier evidence precedes any Granite discovery.
    bool operator==(const AnalysisSnapshot&) const = default;
};

[[nodiscard]] std::vector<GroupPresentation> buildPresentation(const NormalizedAnalysis& normalized,
                                                             const SnapshotEvaluationResponse& evaluation);
[[nodiscard]] ScoringPlan buildScoringPlan(const NormalizedAnalysis& normalized);
[[nodiscard]] std::vector<std::size_t> orderedAssumptionIndices(const NormalizedAnalysis& normalized,
                                                               const SnapshotEvaluationResponse& evaluation);
[[nodiscard]] std::string computeSnapshotHash(const AnalysisSnapshot& snapshot);
[[nodiscard]] bool isSnapshotSealed(const AnalysisSnapshot& snapshot);
// Validates binding, finite probabilities, complete score coverage and presentation. Does not
// sanitize corrupt saved evidence or reinterpret it using today's selected runtime.
[[nodiscard]] Status validateAnalysisSnapshot(const AnalysisSnapshot& snapshot);
[[nodiscard]] Result<AnalysisSnapshot> buildAnalysisSnapshot(NormalizedAnalysis normalized,
                                                            SnapshotEvaluationResponse evaluation,
                                                            std::string_view frozenPrompt,
                                                            std::string discoveryAttemptId,
                                                            std::string scorerAttemptId = {});

// Diagram route: Julia classifies raw prompt; Granite only supplies choices for Clarify.
// Alternative ranking is deliberately NotRequested; triage scores are never copied onto choices.
[[nodiscard]] Result<AnalysisSnapshot> buildRoutedAnalysisSnapshot(NormalizedAnalysis normalized,
    PromptTriage triage, std::string_view frozenPrompt, std::string discoveryAttemptId = {});

} // namespace roclint::core::analysis
