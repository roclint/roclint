#include "roclint/bridge/S1TriageAdapter.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <set>

namespace roclint::bridge {

namespace {

[[nodiscard]] bool isWhitespaceOnly(std::string_view str) {
    return std::all_of(str.begin(), str.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

[[nodiscard]] double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

[[nodiscard]] bool isUnitInterval(double v) noexcept {
    return std::isfinite(v) && v >= 0.0 && v <= 1.0;
}

GroupEvaluationScore unscored(const std::string& groupId) {
    GroupEvaluationScore score;
    score.groupId = groupId;
    return score;
}

// Validates a scorer response against the analysis it claims to rank. Any group whose score is
// malformed is downgraded to unscored; a snapshot binding mismatch degrades the whole response.
SnapshotEvaluationResponse sanitize(SnapshotEvaluationResponse resp, const core::analysis::NormalizedAnalysis& analysis,
                                    ScorerEngine engine, bool realInference) {
    if (resp.analysisSnapshotId != analysis.id || resp.planVersion != 2) {
        auto degraded = S1TriageAdapter::degradedResponse(analysis, "Scorer response is bound to a different analysis snapshot");
        degraded.engine = engine;
        return degraded;
    }

    const auto plan = core::analysis::buildScoringPlan(analysis);
    std::vector<GroupEvaluationScore> clean;
    clean.reserve(analysis.choiceGroups.size());
    bool anyScored = false;
    for (const auto& group : analysis.choiceGroups) {
        const auto it = std::find_if(resp.groupScores.begin(), resp.groupScores.end(),
                                     [&](const GroupEvaluationScore& s) { return s.groupId == group.id; });
        if (std::find(plan.groupIds.begin(), plan.groupIds.end(), group.id) == plan.groupIds.end() || it == resp.groupScores.end() || !it->isScored) {
            clean.push_back(unscored(group.id));
            continue;
        }
        GroupEvaluationScore score = *it;
        bool ok = score.ambiguityScore.has_value() && isUnitInterval(*score.ambiguityScore);
        if (std::count_if(resp.groupScores.begin(), resp.groupScores.end(),
                         [&](const auto& item) { return item.groupId == group.id; }) != 1) ok = false;
        if (realInference && (!score.probabilities.has_value() || !score.winningAlternativeId.has_value())) ok = false;
        if (ok && score.probabilities.has_value()) {
            double sum = 0.0;
            std::set<std::string> seen;
            for (const auto& p : *score.probabilities) {
                const bool known = std::any_of(group.alternatives.begin(), group.alternatives.end(),
                                               [&](const core::analysis::ChoiceAlternative& a) {
                                                   return a.id == p.alternativeId;
                                               });
                if (!known || !isUnitInterval(p.probability) || !seen.insert(p.alternativeId).second) {
                    ok = false;
                    break;
                }
                sum += p.probability;
            }
            if (ok && std::fabs(sum - 1.0) > 0.01) {
                ok = false;
            }
            if (realInference && seen.size() != group.alternatives.size()) ok = false;
        }
        if (ok && score.winningAlternativeId.has_value()) {
            ok = std::any_of(group.alternatives.begin(), group.alternatives.end(),
                             [&](const core::analysis::ChoiceAlternative& a) {
                                 return a.id == *score.winningAlternativeId;
                             });
            if (ok && realInference && score.probabilities.has_value()) {
                const auto best = std::max_element(score.probabilities->begin(), score.probabilities->end(),
                    [](const auto& a, const auto& b) { return a.probability < b.probability; });
                const auto winner = std::find_if(score.probabilities->begin(), score.probabilities->end(),
                    [&](const auto& p) { return p.alternativeId == *score.winningAlternativeId; });
                ok = best != score.probabilities->end() && winner != score.probabilities->end() &&
                     winner->probability >= best->probability;
            }
        }
        if (!ok) {
            clean.push_back(unscored(group.id));
            continue;
        }
        anyScored = true;
        clean.push_back(std::move(score));
    }

    std::vector<AssumptionEvaluationScore> assumptions;
    for (const auto index : core::analysis::orderedAssumptionIndices(analysis)) {
        const auto& assumption = analysis.assumptions[index];
        if (!assumption.material) continue;
        AssumptionEvaluationScore score;
        score.assumptionId = assumption.id;
        const bool planned = std::find(plan.assumptionIds.begin(), plan.assumptionIds.end(), assumption.id) != plan.assumptionIds.end();
        score.unscoredReason = planned ? "scorer_unavailable_or_invalid" : "batch_limit";
        const auto found = std::find_if(resp.assumptionScores.begin(), resp.assumptionScores.end(), [&](const auto& item) { return item.assumptionId == assumption.id; });
        const auto count = std::count_if(resp.assumptionScores.begin(), resp.assumptionScores.end(), [&](const auto& item) { return item.assumptionId == assumption.id; });
        if (planned && count == 1 && found->isScored && found->uncertaintyScore && isUnitInterval(*found->uncertaintyScore) && found->unscoredReason.empty()) {
            score = *found;
            anyScored = true;
        }
        assumptions.push_back(std::move(score));
    }
    resp.assumptionScores = std::move(assumptions);
    resp.groupScores = std::move(clean);
    resp.success = true;
    resp.modelInferenceExecuted = realInference && anyScored;
    resp.engine = engine;
    resp.confidenceThreshold = kS1ConfidenceThreshold;
    resp.thresholdVersion = kS1ThresholdVersion;
    if (anyScored) {
        resp.executionStatus = realInference ? ExecutionStatus::ExecutedModel : ExecutionStatus::ExecutedMock;
        resp.executedEngine = engine;
    } else {
        resp.executionStatus = ExecutionStatus::DegradedUnranked;
        resp.executedEngine = std::nullopt;
        if (realInference) resp.degradedReason = "Julia's scores did not pass snapshot and probability validation";
    }
    return resp;
}

} // namespace

core::Result<ScorerEngine> S1TriageAdapter::parseEngine(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (const char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "jev" || lower == "jev_cloud" || lower == "jevcloud") {
        return ScorerEngine::JevCloud;
    }
    if (lower == "julia" || lower == "julia_local" || lower == "julialocal") return ScorerEngine::JuliaLocal;
    return core::makeError(core::ErrorCode::InvalidInput, "Unknown scorer engine '" + std::string(name) + "'");
}

SemanticAvailability S1TriageAdapter::availability() const noexcept {
    if (engine_ == ScorerEngine::JuliaLocal) {
        return localModelReady_ ? SemanticAvailability::FullRankedLocal : SemanticAvailability::DegradedUnranked;
    }
#if defined(ROCLINT_ENABLE_TEST_MOCKS)
    if (mockCloudScorer_) {
        return SemanticAvailability::FullRankedCloud;
    }
#endif
    return SemanticAvailability::DegradedUnranked;
}

S1TriageDecision S1TriageAdapter::triagePrompt(std::string_view prompt) const {
    const auto startTime = std::chrono::steady_clock::now();
    S1TriageDecision decision;
    decision.engine = engine_;
    decision.isAmbiguityScored = false;
    decision.ambiguityScore = std::nullopt;
    decision.fastPathClear = false;

    if (prompt.empty()) {
        decision.inputValidity = InputValidity::Empty;
        decision.rationale = "Prompt is empty.";
    } else if (isWhitespaceOnly(prompt)) {
        decision.inputValidity = InputValidity::WhitespaceOnly;
        decision.rationale = "Prompt contains only whitespace.";
    } else {
        decision.inputValidity = InputValidity::Valid;
        decision.rationale = "Prompt accepted for analysis; ranking requires supplied choices and an actual scorer run.";
    }
    decision.latencyMs = elapsedMs(startTime);
    return decision;
}

SnapshotEvaluationResponse S1TriageAdapter::degradedResponse(const core::analysis::NormalizedAnalysis& analysis,
                                                             std::string reason) {
    SnapshotEvaluationResponse response;
    response.success = true;
    response.degradedReason = std::move(reason);
    response.analysisSnapshotId = analysis.id;
    response.executionStatus = ExecutionStatus::DegradedUnranked;
    response.executedEngine = std::nullopt;
    response.modelInferenceExecuted = false;
    response.modelVersion = "none";
    response.groupScores.reserve(analysis.choiceGroups.size());
    for (const auto& group : analysis.choiceGroups) {
        response.groupScores.push_back(unscored(group.id));
    }
    const auto plan = core::analysis::buildScoringPlan(analysis);
    for (const auto index : core::analysis::orderedAssumptionIndices(analysis)) {
        const auto& assumption = analysis.assumptions[index];
        if (!assumption.material) continue;
        AssumptionEvaluationScore score;
        score.assumptionId = assumption.id;
        score.unscoredReason = std::find(plan.assumptionIds.begin(), plan.assumptionIds.end(), assumption.id) == plan.assumptionIds.end() ? "batch_limit" : "scorer_unavailable";
        response.assumptionScores.push_back(std::move(score));
    }
    return response;
}

SnapshotEvaluationResponse S1TriageAdapter::evaluateSnapshot(const core::analysis::NormalizedAnalysis& analysis) const {
    const auto startTime = std::chrono::steady_clock::now();

    if (engine_ == ScorerEngine::JuliaLocal) {
        auto response = degradedResponse(analysis, "Julia has not scored this snapshot. Options remain unranked.");
        response.engine = engine_;
        return response;
    }

#if defined(ROCLINT_ENABLE_TEST_MOCKS)
    if (mockCloudScorer_) {
        auto result = mockCloudScorer_(analysis);
        SnapshotEvaluationResponse response =
            result.ok() ? sanitize(std::move(result).value(), analysis, engine_, false)
                        : degradedResponse(analysis, "Jev cloud scorer failed: " + result.error().message);
        response.engine = engine_;
        response.totalLatencyMs = elapsedMs(startTime);
        return response;
    }
#endif

    auto response = degradedResponse(analysis, "Jev cloud scorer is not configured in this build");
    response.engine = engine_;
    response.totalLatencyMs = elapsedMs(startTime);
    return response;
}

SnapshotEvaluationResponse S1TriageAdapter::validateLocalEvaluation(
    const core::analysis::NormalizedAnalysis& analysis, SnapshotEvaluationResponse response) {
    if (response.engine != ScorerEngine::JuliaLocal || !response.modelInferenceExecuted ||
        response.executionStatus != ExecutionStatus::ExecutedModel || response.modelVersion.empty()) {
        auto degraded = degradedResponse(analysis, "Julia did not return a verified model inference result");
        degraded.engine = ScorerEngine::JuliaLocal;
        return degraded;
    }
    return sanitize(std::move(response), analysis, ScorerEngine::JuliaLocal, true);
}

} // namespace roclint::bridge
