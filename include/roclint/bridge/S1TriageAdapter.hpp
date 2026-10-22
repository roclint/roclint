#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::bridge {

enum class ScorerEngine {
    JevCloud,
    JuliaLocal
};

enum class SemanticAvailability {
    FullRankedCloud,
    FullRankedLocal,
    DegradedUnranked
};

enum class ExecutionStatus {
    ExecutedModel,
    ExecutedMock,
    DegradedUnranked,
    Failed
};

enum class InputValidity {
    Valid,
    Empty,
    WhitespaceOnly
};

struct S1TriageDecision {
    InputValidity inputValidity{InputValidity::Valid};
    bool isAmbiguous{false};
    double ambiguityScore{0.0};
    std::string dimension{"action"};
    std::string rationale{};
    double latencyMs{0.0};
    ScorerEngine engine{ScorerEngine::JuliaLocal};
    bool fastPathClear{false};

    [[nodiscard]] bool isExecutable() const noexcept {
        return inputValidity == InputValidity::Valid;
    }
};

struct AlternativeProbability {
    std::string alternativeId{};
    double probability{0.0};
};

struct GroupEvaluationScore {
    std::string groupId{};
    std::optional<std::string> winningAlternativeId{std::nullopt};
    std::vector<AlternativeProbability> probabilities{};
    double ambiguityScore{0.0};
    bool noulCertainty{false};
    bool isScored{false};
};

struct SnapshotEvaluationResponse {
    bool success{false};
    std::string errorMessage{};
    std::vector<GroupEvaluationScore> groupScores{};
    double totalLatencyMs{0.0};
    ScorerEngine engine{ScorerEngine::JuliaLocal};
    std::optional<ScorerEngine> executedEngine{std::nullopt};
    ExecutionStatus executionStatus{ExecutionStatus::DegradedUnranked};
    std::string modelVersion{"none (unscored/degraded)"};
    bool modelInferenceExecuted{false};
};

class S1TriageAdapter {
public:
    using MockTriageHandler = std::function<S1TriageDecision(std::string_view prompt, ScorerEngine engine)>;
    using MockSnapshotHandler = std::function<SnapshotEvaluationResponse(const core::analysis::NormalizedAnalysis&, ScorerEngine)>;

    explicit S1TriageAdapter(ScorerEngine defaultEngine = ScorerEngine::JuliaLocal);

    [[nodiscard]] ScorerEngine engine() const noexcept { return engine_; }
    [[nodiscard]] ScorerEngine configuredEngine() const noexcept { return engine_; }
    void setEngine(ScorerEngine engine) noexcept { engine_ = engine; }

    [[nodiscard]] SemanticAvailability availability() const noexcept;
    void setCloudAvailable(bool available) noexcept { cloudAvailable_ = available; }
    [[nodiscard]] bool isCloudAvailable() const noexcept { return cloudAvailable_; }

    [[nodiscard]] bool isLocalModelLoaded() const noexcept { return localModelLoaded_; }
    void setLocalModelLoaded(bool loaded) noexcept { localModelLoaded_ = loaded; }

    [[nodiscard]] S1TriageDecision triagePrompt(std::string_view prompt);

    [[nodiscard]] SnapshotEvaluationResponse evaluateSnapshot(const core::analysis::NormalizedAnalysis& analysis);

    void setMockTriageHandler(MockTriageHandler handler) { mockTriageHandler_ = std::move(handler); }
    void setMockSnapshotHandler(MockSnapshotHandler handler) { mockSnapshotHandler_ = std::move(handler); }

private:
    ScorerEngine engine_{ScorerEngine::JuliaLocal};
    bool cloudAvailable_{true};
    bool localModelLoaded_{false};
    MockTriageHandler mockTriageHandler_{nullptr};
    MockSnapshotHandler mockSnapshotHandler_{nullptr};
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
        case ExecutionStatus::Failed: return "FAILED";
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
