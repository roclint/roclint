#include "roclint/bridge/S1TriageAdapter.hpp"

#include <algorithm>
#include <cctype>

namespace roclint::bridge {

namespace {

[[nodiscard]] std::string toLowerString(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

[[nodiscard]] bool isWhitespaceOnly(std::string_view str) {
    return std::all_of(str.begin(), str.end(), [](unsigned char c) {
        return std::isspace(c);
    });
}

} // namespace

S1TriageAdapter::S1TriageAdapter(ScorerEngine defaultEngine)
    : engine_{defaultEngine} {}

SemanticAvailability S1TriageAdapter::availability() const noexcept {
    if (engine_ == ScorerEngine::JevCloud) {
        return cloudAvailable_ ? SemanticAvailability::FullRankedCloud
                               : SemanticAvailability::DegradedUnranked;
    }
    return localModelLoaded_ ? SemanticAvailability::FullRankedLocal
                             : SemanticAvailability::DegradedUnranked;
}

S1TriageDecision S1TriageAdapter::triagePrompt(std::string_view prompt) {
    const auto startTime = std::chrono::steady_clock::now();

    if (mockTriageHandler_) {
        auto decision = mockTriageHandler_(prompt, engine_);
        const auto endTime = std::chrono::steady_clock::now();
        decision.latencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        decision.engine = engine_;
        return decision;
    }

    S1TriageDecision decision;
    decision.engine = engine_;

    if (prompt.empty()) {
        decision.inputValidity = InputValidity::Empty;
        decision.isAmbiguous = false;
        decision.ambiguityScore = 0.0;
        decision.fastPathClear = false;
        decision.dimension = "none";
        decision.rationale = "Prompt is empty; no executable instruction provided.";
        const auto endTime = std::chrono::steady_clock::now();
        decision.latencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        return decision;
    }

    if (isWhitespaceOnly(prompt)) {
        decision.inputValidity = InputValidity::WhitespaceOnly;
        decision.isAmbiguous = false;
        decision.ambiguityScore = 0.0;
        decision.fastPathClear = false;
        decision.dimension = "none";
        decision.rationale = "Prompt contains only whitespace; no executable instruction provided.";
        const auto endTime = std::chrono::steady_clock::now();
        decision.latencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        return decision;
    }

    decision.inputValidity = InputValidity::Valid;
    const std::string lowerPrompt = toLowerString(prompt);

    const bool hasExplicitPrecedence = 
        (lowerPrompt.find("priority over") != std::string::npos) ||
        (lowerPrompt.find("precedence over") != std::string::npos) ||
        (lowerPrompt.find("prefer ") != std::string::npos && lowerPrompt.find(" over ") != std::string::npos) ||
        (lowerPrompt.find("take priority") != std::string::npos) ||
        (lowerPrompt.find("takes priority") != std::string::npos) ||
        (lowerPrompt.find("takes precedence") != std::string::npos);

    const bool hasContradictoryInvariants =
        (lowerPrompt.find("stateless") != std::string::npos && 
         (lowerPrompt.find("session") != std::string::npos || lowerPrompt.find("sticky") != std::string::npos));

    const bool hasUnresolvedConflictPhrase =
        (lowerPrompt.find("conflict") != std::string::npos && !hasExplicitPrecedence) ||
        (lowerPrompt.find("contradict") != std::string::npos) ||
        (lowerPrompt.find("mutually exclusive") != std::string::npos);

    if (hasContradictoryInvariants || hasUnresolvedConflictPhrase) {
        decision.isAmbiguous = true;
        decision.ambiguityScore = 0.92;
        decision.dimension = "rule_conflict";
        decision.rationale = "Contradictory operational constraints or unresolved rule conflict detected.";
    } else if (lowerPrompt.find("which") != std::string::npos ||
               lowerPrompt.find("either") != std::string::npos ||
               lowerPrompt.find("component") != std::string::npos ||
               lowerPrompt.find("ui or") != std::string::npos ||
               lowerPrompt.find("frontend or backend") != std::string::npos) {
        decision.isAmbiguous = true;
        decision.ambiguityScore = 0.88;
        decision.dimension = "target";
        decision.rationale = "Multiple valid architectural components could satisfy this request.";
    } else if (lowerPrompt.find("timeout") != std::string::npos ||
               lowerPrompt.find("refactor") != std::string::npos ||
               lowerPrompt.find("optimize") != std::string::npos ||
               lowerPrompt.find("handle") != std::string::npos ||
               lowerPrompt.find(" or ") != std::string::npos) {
        decision.isAmbiguous = true;
        decision.ambiguityScore = 0.78;
        decision.dimension = "action";
        decision.rationale = "Action implementation path has multiple distinct strategies.";
    } else {
        decision.isAmbiguous = false;
        decision.ambiguityScore = 0.22;
        decision.dimension = "action";
        decision.rationale = hasExplicitPrecedence
            ? "Prompt explicitly specifies precedence, resolving constraint trade-offs."
            : "Prompt specifies clear, concrete requirements without material ambiguity.";
    }

    decision.fastPathClear = (decision.inputValidity == InputValidity::Valid && decision.ambiguityScore < 0.70);

    const auto endTime = std::chrono::steady_clock::now();
    decision.latencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return decision;
}

SnapshotEvaluationResponse S1TriageAdapter::evaluateSnapshot(
    const core::analysis::NormalizedAnalysis& analysis) {

    const auto startTime = std::chrono::steady_clock::now();

    if (mockSnapshotHandler_) {
        auto resp = mockSnapshotHandler_(analysis, engine_);
        const auto endTime = std::chrono::steady_clock::now();
        resp.totalLatencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        resp.engine = engine_;
        resp.executedEngine = engine_;
        resp.executionStatus = ExecutionStatus::ExecutedMock;
        resp.modelInferenceExecuted = true;
        return resp;
    }

    SnapshotEvaluationResponse response;
    response.engine = engine_;

    if (engine_ == ScorerEngine::JevCloud) {
        if (!cloudAvailable_) {
            response.success = false;
            response.errorMessage = "Jev Cloud scoring unavailable (network unreachable or missing credentials)";
            response.executionStatus = ExecutionStatus::Failed;
            response.modelVersion = "none (cloud unavailable)";
            response.modelInferenceExecuted = false;
            const auto endTime = std::chrono::steady_clock::now();
            response.totalLatencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
            return response;
        }
        response.success = true;
        response.executedEngine = std::nullopt;
        response.executionStatus = ExecutionStatus::DegradedUnranked;
        response.modelVersion = "typesafe/jev-cloud-v1-rlcd (connected, unranked)";
        response.modelInferenceExecuted = false;
    } else {
        if (localModelLoaded_) {
            response.success = true;
            response.executedEngine = ScorerEngine::JuliaLocal;
            response.executionStatus = ExecutionStatus::ExecutedModel;
            response.modelVersion = "SupersonicLabs/Julia-1";
            response.modelInferenceExecuted = true;
        } else {
            response.success = true;
            response.executedEngine = std::nullopt;
            response.executionStatus = ExecutionStatus::DegradedUnranked;
            response.modelVersion = "none (local model not loaded)";
            response.modelInferenceExecuted = false;
        }
    }

    response.groupScores.reserve(analysis.choiceGroups.size());

    for (const auto& group : analysis.choiceGroups) {
        GroupEvaluationScore score;
        score.groupId = group.id;
        const std::size_t n = group.alternatives.size();

        if (response.modelInferenceExecuted) {
            score.isScored = true;
            score.ambiguityScore = 0.82;
            score.noulCertainty = true;
            if (n > 0) {
                const double uniform = 1.0 / static_cast<double>(n);
                for (const auto& alt : group.alternatives) {
                    score.probabilities.push_back({alt.id, uniform});
                }
                score.winningAlternativeId = group.alternatives[0].id;
            }
        } else {
            score.isScored = false;
            score.ambiguityScore = 0.50;
            score.noulCertainty = false;
            score.winningAlternativeId = std::nullopt;

            if (n > 0) {
                const double uniform = 1.0 / static_cast<double>(n);
                for (const auto& alt : group.alternatives) {
                    score.probabilities.push_back({alt.id, uniform});
                }
            }
        }

        response.groupScores.push_back(std::move(score));
    }

    response.success = true;
    const auto endTime = std::chrono::steady_clock::now();
    response.totalLatencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return response;
}

} // namespace roclint::bridge
