#pragma once

#include "roclint/analysis/AnalysisSnapshot.hpp"
#include "roclint/core/Result.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::bridge {

// Compatibility names for the canonical semantic scoring types.
using core::analysis::ScorerEngine;
using core::analysis::ExecutionStatus;
using core::analysis::AlternativeProbability;
using core::analysis::GroupEvaluationScore;
using core::analysis::AssumptionEvaluationScore;
using core::analysis::SnapshotEvaluationResponse;
using core::analysis::kS1ConfidenceThreshold;
using core::analysis::kS1ThresholdVersion;

enum class SemanticAvailability {
    FullRankedCloud,
    FullRankedLocal,
    DegradedUnranked
};

enum class InputValidity {
    Valid,
    Empty,
    WhitespaceOnly
};

// Input validation only. No ambiguity score is ever produced without a scorer run.
struct S1TriageDecision {
    InputValidity inputValidity{InputValidity::Valid};
    bool isAmbiguityScored{false};
    std::optional<double> ambiguityScore{std::nullopt};
    bool fastPathClear{false};
    std::string rationale{};
    double latencyMs{0.0};
    ScorerEngine engine{ScorerEngine::JevCloud};

    [[nodiscard]] bool isExecutable() const noexcept {
        return inputValidity == InputValidity::Valid;
    }
};

class S1TriageAdapter {
public:
    explicit S1TriageAdapter(ScorerEngine engine = ScorerEngine::JevCloud) : engine_(engine) {}

    // Accepts Jev and Julia aliases; unknown names are errors.
    [[nodiscard]] static core::Result<ScorerEngine> parseEngine(std::string_view name);

    [[nodiscard]] ScorerEngine engine() const noexcept { return engine_; }
    void setEngine(ScorerEngine engine) noexcept { engine_ = engine; }

    [[nodiscard]] SemanticAvailability availability() const noexcept;
    void setLocalModelReady(bool ready) noexcept { localModelReady_ = ready; }

    // Validates a real, asynchronously produced Julia result against the exact input snapshot.
    [[nodiscard]] static SnapshotEvaluationResponse validateLocalEvaluation(
        const core::analysis::NormalizedAnalysis& analysis, SnapshotEvaluationResponse response);

    [[nodiscard]] S1TriageDecision triagePrompt(std::string_view prompt) const;

    [[nodiscard]] SnapshotEvaluationResponse evaluateSnapshot(const core::analysis::NormalizedAnalysis& analysis) const;

    // Builds the DEGRADED_UNRANKED response for an analysis: every group unscored, no probabilities.
    [[nodiscard]] static SnapshotEvaluationResponse degradedResponse(const core::analysis::NormalizedAnalysis& analysis,
                                                                     std::string reason);

#if defined(ROCLINT_ENABLE_TEST_MOCKS)
    using MockCloudScorer =
        std::function<core::Result<SnapshotEvaluationResponse>(const core::analysis::NormalizedAnalysis&)>;
    void setMockCloudScorer(MockCloudScorer scorer) { mockCloudScorer_ = std::move(scorer); }
#endif

private:
    ScorerEngine engine_{ScorerEngine::JevCloud};
    bool localModelReady_{false};
#if defined(ROCLINT_ENABLE_TEST_MOCKS)
    MockCloudScorer mockCloudScorer_{nullptr};
#endif
};

[[nodiscard]] constexpr std::string_view toString(ScorerEngine engine) noexcept {
    switch (engine) {
        case ScorerEngine::JevCloud: return "JEV_CLOUD";
        case ScorerEngine::JuliaLocal: return "JULIA_LOCAL";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view toString(SemanticAvailability avail) noexcept {
    switch (avail) {
        case SemanticAvailability::FullRankedCloud: return "FULL_RANKED_CLOUD";
        case SemanticAvailability::FullRankedLocal: return "FULL_RANKED_LOCAL";
        case SemanticAvailability::DegradedUnranked: return "DEGRADED_UNRANKED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view toString(ExecutionStatus status) noexcept {
    switch (status) {
        case ExecutionStatus::ExecutedModel: return "EXECUTED_MODEL";
        case ExecutionStatus::ExecutedMock: return "EXECUTED_MOCK";
        case ExecutionStatus::DegradedUnranked: return "DEGRADED_UNRANKED";
        case ExecutionStatus::NotRequested: return "NOT_REQUESTED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view toString(InputValidity validity) noexcept {
    switch (validity) {
        case InputValidity::Valid: return "VALID";
        case InputValidity::Empty: return "EMPTY";
        case InputValidity::WhitespaceOnly: return "WHITESPACE_ONLY";
    }
    return "UNKNOWN";
}

} // namespace roclint::bridge
