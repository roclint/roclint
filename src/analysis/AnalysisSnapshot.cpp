#include "roclint/analysis/AnalysisSnapshot.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>

namespace roclint::core::analysis {
namespace {
bool unit(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }
bool knownEngine(ScorerEngine engine) { return engine == ScorerEngine::JuliaLocal || engine == ScorerEngine::JevCloud; }
const GroupEvaluationScore* scoreFor(const SnapshotEvaluationResponse& evaluation, const std::string& id) {
    const auto it = std::find_if(evaluation.groupScores.begin(), evaluation.groupScores.end(),
                                 [&](const auto& score) { return score.groupId == id; });
    return it == evaluation.groupScores.end() ? nullptr : &*it;
}
double probability(const GroupEvaluationScore* score, const std::string& id) {
    if (score && score->isScored && score->probabilities) {
        for (const auto& value : *score->probabilities) if (value.alternativeId == id && unit(value.probability)) return value.probability;
    }
    return -1;
}
void hashDouble(crypto::CanonicalHasher& hash, double value) { hash.number(std::bit_cast<std::uint64_t>(value == 0 ? 0.0 : value)); }
Status invalid(const char* message) { return makeError(ErrorCode::ValidationFailed, message); }
} // namespace

ScoringPlan buildScoringPlan(const NormalizedAnalysis& normalized) {
    ScoringPlan plan;
    for (const auto& group : normalized.choiceGroups) {
        if (group.material && group.alternatives.size() >= ChoiceGroup::kMinAlternatives &&
            group.alternatives.size() <= ChoiceGroup::kMaxAlternatives && plan.groupIds.size() < kMaxScoredGroups) plan.groupIds.push_back(group.id);
    }
    for (const auto index : orderedAssumptionIndices(normalized)) {
        if (normalized.assumptions[index].material && plan.assumptionIds.size() < kMaxAssumptionChecks) plan.assumptionIds.push_back(normalized.assumptions[index].id);
    }
    return plan;
}

std::vector<std::size_t> orderedAssumptionIndices(const NormalizedAnalysis& normalized, const SnapshotEvaluationResponse& evaluation) {
    auto indices = orderedAssumptionIndices(normalized);
    if (evaluation.planVersion != 2 || !evaluation.isRanked() || evaluation.analysisSnapshotId != normalized.id) return indices;
    auto uncertainty = [&](std::size_t index) {
        for (const auto& score : evaluation.assumptionScores) {
            if (score.assumptionId == normalized.assumptions[index].id && score.isScored && score.uncertaintyScore && unit(*score.uncertaintyScore)) return *score.uncertaintyScore;
        }
        return -1.0;
    };
    std::stable_sort(indices.begin(), indices.end(), [&](auto a, auto b) { return uncertainty(a) > uncertainty(b); });
    return indices;
}

std::vector<GroupPresentation> buildPresentation(const NormalizedAnalysis& normalized,
                                                 const SnapshotEvaluationResponse& evaluation) {
    std::vector<GroupPresentation> out;
    const bool ranked = evaluation.isRanked() && evaluation.analysisSnapshotId == normalized.id;
    for (const auto& group : normalized.choiceGroups) {
        GroupPresentation presentation{group.id, {}};
        for (const auto& alternative : group.alternatives) presentation.alternativeIds.push_back(alternative.id);
        const auto* score = ranked ? scoreFor(evaluation, group.id) : nullptr;
        std::sort(presentation.alternativeIds.begin(), presentation.alternativeIds.end(), [&](const auto& a, const auto& b) {
            const double pa = probability(score, a), pb = probability(score, b);
            return pa != pb ? pa > pb : a < b;
        });
        out.push_back(std::move(presentation));
    }
    std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
        const auto* sa = ranked ? scoreFor(evaluation, a.groupId) : nullptr;
        const auto* sb = ranked ? scoreFor(evaluation, b.groupId) : nullptr;
        const double pa = sa && sa->isScored && sa->ambiguityScore && unit(*sa->ambiguityScore) ? *sa->ambiguityScore : 0;
        const double pb = sb && sb->isScored && sb->ambiguityScore && unit(*sb->ambiguityScore) ? *sb->ambiguityScore : 0;
        if (pa != pb) return pa > pb;
        auto earliest = [&](const std::string& id) {
            std::size_t first = std::numeric_limits<std::size_t>::max();
            for (const auto& group : normalized.choiceGroups) if (group.id == id) {
                for (const auto& alternative : group.alternatives) for (const auto& span : alternative.evidence) first = std::min(first, span.startByte);
            }
            return first;
        };
        const auto firstA = earliest(a.groupId), firstB = earliest(b.groupId);
        return firstA != firstB ? firstA < firstB : a.groupId < b.groupId;
    });
    return out;
}

std::string computeSnapshotHash(const AnalysisSnapshot& snapshot) {
    crypto::CanonicalHasher hash(snapshot.payloadVersion == 2 ? "roclint.analysis_snapshot.v2" :
        snapshot.payloadVersion == 3 ? "roclint.analysis_snapshot.v3" : "roclint.analysis_snapshot.v4");
    hash.field(computeSnapshotHash(snapshot.normalized)).field(snapshot.normalized.id).field(snapshot.revisionTextHash)
        .field(snapshot.discoveryAttemptId).field(snapshot.scorerAttemptId).field(snapshot.presentationPolicyVersion);
    const auto& evaluation = snapshot.evaluation;
    hash.flag(evaluation.success).field(evaluation.errorMessage).field(evaluation.degradedReason)
        .field(evaluation.analysisSnapshotId).number(static_cast<std::uint64_t>(evaluation.engine))
        .number(static_cast<std::uint64_t>(evaluation.executionStatus)).flag(evaluation.executedEngine.has_value());
    if (evaluation.executedEngine) hash.number(static_cast<std::uint64_t>(*evaluation.executedEngine));
    hash.field(evaluation.modelVersion).flag(evaluation.modelInferenceExecuted).field(evaluation.thresholdVersion);
    hashDouble(hash, evaluation.confidenceThreshold);
    hashDouble(hash, evaluation.totalLatencyMs);
    hash.number(evaluation.groupScores.size());
    for (const auto& score : evaluation.groupScores) {
        hash.field(score.groupId).flag(score.isScored).flag(score.noulCertainty).flag(score.ambiguityScore.has_value());
        if (score.ambiguityScore) hashDouble(hash, *score.ambiguityScore);
        hash.flag(score.winningAlternativeId.has_value());
        if (score.winningAlternativeId) hash.field(*score.winningAlternativeId);
        hash.flag(score.probabilities.has_value());
        if (score.probabilities) {
            hash.number(score.probabilities->size());
            for (const auto& value : *score.probabilities) {
                hash.field(value.alternativeId);
                hashDouble(hash, value.probability);
            }
        }
    }
    hash.number(snapshot.presentation.size());
    for (const auto& group : snapshot.presentation) {
        hash.field(group.groupId).number(group.alternativeIds.size());
        for (const auto& id : group.alternativeIds) hash.field(id);
    }
    if (snapshot.payloadVersion >= 3) {
        hash.number(static_cast<std::uint64_t>(evaluation.planVersion)).number(evaluation.assumptionScores.size());
        for (const auto& score : evaluation.assumptionScores) {
            hash.field(score.assumptionId).flag(score.isScored).flag(score.noulCertainty).field(score.unscoredReason).flag(score.uncertaintyScore.has_value());
            if (score.uncertaintyScore) hashDouble(hash, *score.uncertaintyScore);
        }
        hash.number(snapshot.assumptionOrder.size());
        for (const auto& id : snapshot.assumptionOrder) hash.field(id);
    }
    if (snapshot.payloadVersion >= 4) {
        hash.flag(snapshot.promptTriage.has_value());
        if (snapshot.promptTriage) hash.field(snapshot.promptTriage->id).field(computePromptTriageHash(*snapshot.promptTriage));
    }
    return hash.finalizeHex();
}

bool isSnapshotSealed(const AnalysisSnapshot& snapshot) {
    const auto hash = computeSnapshotHash(snapshot);
    return isSnapshotSealed(snapshot.normalized) && snapshot.snapshotHash == hash && snapshot.id == "analysis_" + hash;
}

Status validateAnalysisSnapshot(const AnalysisSnapshot& snapshot) {
    if (snapshot.payloadVersion != 2 && snapshot.payloadVersion != 3 && snapshot.payloadVersion != 4) return invalid("Unsupported final analysis version");
    if (!isSnapshotSealed(snapshot)) return invalid("Final analysis hash does not match its contents");
    const auto textHash = crypto::normalizeSha256Digest(snapshot.revisionTextHash);
    if (!textHash.ok() || textHash.value() != snapshot.revisionTextHash) return invalid("Missing canonical revision text hash");
    const auto& evaluation = snapshot.evaluation;
    if (snapshot.payloadVersion == 4) {
        if (!snapshot.promptTriage || !validatePromptTriage(*snapshot.promptTriage).ok()) return invalid("Missing valid classifier evidence");
        const auto& triage = *snapshot.promptTriage;
        if (triage.sessionId != snapshot.normalized.sessionId || triage.revisionId != snapshot.normalized.baseRevisionId ||
            triage.revisionTextHash != snapshot.revisionTextHash) return invalid("Classification belongs to another frozen prompt");
        const auto routeRes = routePromptTriage(triage);
        if (!routeRes.ok()) return routeRes.error();
        // Both Original (clearly below threshold) and UncertainBuffer (dead band advisory)
        // retain the original prompt without invoking Granite discovery.
        const bool clear = (routeRes.value() == PromptTriageRoute::Original ||
                            routeRes.value() == PromptTriageRoute::UncertainBuffer);
        const bool empty = snapshot.normalized.choiceGroups.empty() && snapshot.normalized.assumptions.empty() &&
            snapshot.normalized.missingDecisions.empty() && snapshot.normalized.conflicts.empty();
        if ((clear && (!snapshot.discoveryAttemptId.empty() || !empty)) ||
            (!clear && (snapshot.discoveryAttemptId.empty() || empty))) return invalid("Final analysis does not follow the classifier route");
        if (!snapshot.scorerAttemptId.empty() || !evaluation.success || evaluation.executionStatus != ExecutionStatus::NotRequested ||
            evaluation.engine != ScorerEngine::JuliaLocal || evaluation.executedEngine || evaluation.modelInferenceExecuted ||
            evaluation.modelVersion != "none" || !evaluation.errorMessage.empty() || !evaluation.degradedReason.empty() ||
            evaluation.totalLatencyMs != 0 || evaluation.analysisSnapshotId != snapshot.normalized.id ||
            evaluation.confidenceThreshold != kS1ConfidenceThreshold || evaluation.thresholdVersion != kS1ThresholdVersion ||
            evaluation.planVersion != 2 || !evaluation.groupScores.empty() || !evaluation.assumptionScores.empty() ||
            snapshot.presentationPolicyVersion != "prompt_triage_presentation.v1" ||
            snapshot.presentation != buildPresentation(snapshot.normalized, evaluation)) return invalid("Routed analysis cannot claim alternative scoring");
        std::vector<std::string> expectedOrder;
        for (const auto index : orderedAssumptionIndices(snapshot.normalized))
            if (snapshot.normalized.assumptions[index].material) expectedOrder.push_back(snapshot.normalized.assumptions[index].id);
        if (snapshot.assumptionOrder != expectedOrder) return invalid("Invalid routed assumption order");
        return Status::success();
    }
    if (snapshot.promptTriage) return invalid("Historical analysis cannot claim classifier evidence");
    if (!evaluation.success || evaluation.analysisSnapshotId != snapshot.normalized.id || !knownEngine(evaluation.engine) ||
        !std::isfinite(evaluation.totalLatencyMs) || evaluation.totalLatencyMs < 0 ||
        evaluation.confidenceThreshold != kS1ConfidenceThreshold || evaluation.thresholdVersion != kS1ThresholdVersion ||
        snapshot.presentationPolicyVersion != (snapshot.payloadVersion == 2 ? "scorer_presentation.v1" : "scorer_presentation.v2") || snapshot.discoveryAttemptId.empty()) {
        return invalid("Final analysis has invalid scoring bindings or policy metadata");
    }
    const bool ranked = evaluation.isRanked();
    if (snapshot.payloadVersion == 2 && (evaluation.planVersion != 1 || !evaluation.assumptionScores.empty() || !snapshot.assumptionOrder.empty())) return invalid("Legacy analysis cannot claim new assumption evidence");
    if (snapshot.payloadVersion == 3 && evaluation.planVersion != 2) return invalid("Final analysis uses an unsupported scoring plan");
    const auto plan = buildScoringPlan(snapshot.normalized);
    if (evaluation.executionStatus != ExecutionStatus::ExecutedModel &&
        evaluation.executionStatus != ExecutionStatus::ExecutedMock &&
        evaluation.executionStatus != ExecutionStatus::DegradedUnranked) return invalid("Unknown scoring execution status");
#if !defined(ROCLINT_ENABLE_TEST_MOCKS)
    if (evaluation.executionStatus == ExecutionStatus::ExecutedMock) return invalid("Mock scoring cannot enter production history");
#endif
    if (ranked && (!evaluation.executedEngine || *evaluation.executedEngine != evaluation.engine ||
                   snapshot.scorerAttemptId.empty() || evaluation.modelVersion.empty() || evaluation.modelVersion == "none")) {
        return invalid("Ranked analysis requires scorer attempt and model identity");
    }
    if ((evaluation.executionStatus == ExecutionStatus::ExecutedModel) != evaluation.modelInferenceExecuted) {
        return invalid("Scoring execution metadata is inconsistent");
    }
    if (!ranked && (evaluation.executedEngine || evaluation.degradedReason.empty())) {
        return invalid("Unranked analysis requires explicit degradation and no executed engine");
    }
    if (evaluation.groupScores.size() != snapshot.normalized.choiceGroups.size()) return invalid("Incomplete score coverage");
    bool anyScored = false;
    std::set<std::string> groups;
    for (const auto& score : evaluation.groupScores) {
        const auto group = std::find_if(snapshot.normalized.choiceGroups.begin(), snapshot.normalized.choiceGroups.end(),
                                        [&](const auto& item) { return item.id == score.groupId; });
        if (group == snapshot.normalized.choiceGroups.end() || !groups.insert(score.groupId).second) return invalid("Unknown or duplicate scored group");
        if (!score.isScored) {
            if (score.probabilities || score.winningAlternativeId || score.ambiguityScore || score.noulCertainty) {
                return invalid("Unscored group carries fabricated score metadata");
            }
            continue;
        }
        anyScored = true;
        if (snapshot.payloadVersion >= 3 && std::find(plan.groupIds.begin(), plan.groupIds.end(), score.groupId) == plan.groupIds.end()) return invalid("Group score exceeds the material scoring plan");
        if (!ranked || !score.ambiguityScore || !unit(*score.ambiguityScore)) return invalid("Invalid ambiguity score");
        if (evaluation.executionStatus == ExecutionStatus::ExecutedModel && (!score.probabilities || !score.winningAlternativeId)) {
            return invalid("Model evaluation requires complete probability evidence");
        }
        if (score.probabilities) {
            std::set<std::string> alternatives;
            double sum = 0, best = -1, winner = -1;
            for (const auto& value : *score.probabilities) {
                const bool known = std::any_of(group->alternatives.begin(), group->alternatives.end(),
                                               [&](const auto& item) { return item.id == value.alternativeId; });
                if (!known || !alternatives.insert(value.alternativeId).second || !unit(value.probability)) return invalid("Invalid probability evidence");
                sum += value.probability;
                best = std::max(best, value.probability);
                if (score.winningAlternativeId && value.alternativeId == *score.winningAlternativeId) winner = value.probability;
            }
            if (alternatives.size() != group->alternatives.size() || std::fabs(sum - 1) > 0.01 ||
                (score.winningAlternativeId && winner < best)) return invalid("Incomplete probabilities or incorrect winning alternative");
        }
    }
    if (snapshot.payloadVersion >= 3) {
        std::set<std::string> material;
        for (const auto& assumption : snapshot.normalized.assumptions) if (assumption.material) material.insert(assumption.id);
        if (evaluation.assumptionScores.size() != material.size()) return invalid("Incomplete material assumption coverage");
        std::set<std::string> seen;
        for (const auto& score : evaluation.assumptionScores) {
            if (!material.contains(score.assumptionId) || !seen.insert(score.assumptionId).second) return invalid("Unknown or duplicate assumption score");
            if (!score.isScored) {
                if (score.uncertaintyScore || score.noulCertainty || score.unscoredReason.empty()) return invalid("Unscored assumption has invalid evidence or missing reason");
                continue;
            }
            anyScored = true;
            if (!ranked || !score.uncertaintyScore || !unit(*score.uncertaintyScore) || !score.unscoredReason.empty() ||
                std::find(plan.assumptionIds.begin(), plan.assumptionIds.end(), score.assumptionId) == plan.assumptionIds.end()) return invalid("Invalid or out-of-budget assumption score");
        }
        std::vector<std::string> expectedOrder;
        for (const auto index : orderedAssumptionIndices(snapshot.normalized, evaluation)) if (snapshot.normalized.assumptions[index].material) expectedOrder.push_back(snapshot.normalized.assumptions[index].id);
        if (snapshot.assumptionOrder != expectedOrder) return invalid("Saved assumption order differs from its policy");
    }
    if (ranked != anyScored) return invalid("Scoring status does not match available evidence");
    if (snapshot.presentation != buildPresentation(snapshot.normalized, evaluation)) return invalid("Saved presentation differs from its policy");
    return Status::success();
}

Result<AnalysisSnapshot> buildAnalysisSnapshot(NormalizedAnalysis normalized, SnapshotEvaluationResponse evaluation,
                                               std::string_view frozenPrompt,
                                               std::string discoveryAttemptId, std::string scorerAttemptId) {
    if (validateNormalizedAnalysis(normalized, frozenPrompt).status == AnalysisValidationStatus::Invalid) return invalid("Analysis does not match the frozen prompt").error();
    AnalysisSnapshot snapshot;
    snapshot.revisionTextHash = crypto::Sha256::hexDigest(frozenPrompt);
    snapshot.normalized = std::move(normalized);
    snapshot.evaluation = std::move(evaluation);
    snapshot.discoveryAttemptId = std::move(discoveryAttemptId);
    snapshot.scorerAttemptId = std::move(scorerAttemptId);
    snapshot.presentation = buildPresentation(snapshot.normalized, snapshot.evaluation);
    for (const auto index : orderedAssumptionIndices(snapshot.normalized, snapshot.evaluation)) {
        if (snapshot.normalized.assumptions[index].material) snapshot.assumptionOrder.push_back(snapshot.normalized.assumptions[index].id);
    }
    snapshot.snapshotHash = computeSnapshotHash(snapshot);
    snapshot.id = "analysis_" + snapshot.snapshotHash;
    if (auto status = validateAnalysisSnapshot(snapshot); !status.ok()) return status.error();
    return snapshot;
}

Result<AnalysisSnapshot> buildRoutedAnalysisSnapshot(NormalizedAnalysis normalized, PromptTriage triage,
    std::string_view frozenPrompt, std::string discoveryAttemptId) {
    if (!validatePromptTriage(triage).ok() || triage.revisionTextHash != crypto::Sha256::hexDigest(frozenPrompt) ||
        validateNormalizedAnalysis(normalized, frozenPrompt).status == AnalysisValidationStatus::Invalid)
        return invalid("Routed analysis does not match its frozen prompt").error();
    AnalysisSnapshot snapshot;
    snapshot.payloadVersion = 4;
    snapshot.revisionTextHash = crypto::Sha256::hexDigest(frozenPrompt);
    snapshot.normalized = std::move(normalized);
    snapshot.promptTriage = std::move(triage);
    snapshot.discoveryAttemptId = std::move(discoveryAttemptId);
    snapshot.evaluation.success = true;
    snapshot.evaluation.analysisSnapshotId = snapshot.normalized.id;
    snapshot.evaluation.engine = ScorerEngine::JuliaLocal;
    snapshot.evaluation.executionStatus = ExecutionStatus::NotRequested;
    snapshot.presentationPolicyVersion = "prompt_triage_presentation.v1";
    snapshot.presentation = buildPresentation(snapshot.normalized, snapshot.evaluation);
    for (const auto index : orderedAssumptionIndices(snapshot.normalized))
        if (snapshot.normalized.assumptions[index].material) snapshot.assumptionOrder.push_back(snapshot.normalized.assumptions[index].id);
    snapshot.snapshotHash = computeSnapshotHash(snapshot);
    snapshot.id = "analysis_" + snapshot.snapshotHash;
    if (auto status = validateAnalysisSnapshot(snapshot); !status.ok()) return status.error();
    return snapshot;
}
} // namespace roclint::core::analysis
