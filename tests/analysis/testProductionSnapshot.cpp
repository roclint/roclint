#include "roclint/storage/PayloadCodec.hpp"
#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "support/fixtures.hpp"

#include <iostream>

int main() {
#if defined(ROCLINT_ENABLE_TEST_MOCKS)
#error This boundary check must link production libraries without mock seams.
#endif
    namespace an = roclint::core::analysis;
    an::PromptTriage triage;
    triage.sessionId = "sess_prod_triage"; triage.revisionId = "rev_sess_prod_triage_1";
    triage.revisionTextHash = roclint::core::crypto::Sha256::hexDigest("Request.");
    triage.stateVersion = 1; triage.requestGroupId = "grp_triage"; triage.attemptId = "att_triage";
    triage.modelId = "fixture-shaped-model-output"; triage.modelRevision = "revision";
    triage.weightsSha256 = std::string(64, 'a'); triage.configurationId = "configuration";
    triage.categoryScores = {0.1, 0.2, 0.3, 0.4}; triage.execution = an::TriageExecution::Mock;
    ROCLINT_CHECK(!an::sealPromptTriage(triage, "Request.").ok());
    triage.execution = an::TriageExecution::Model;
    ROCLINT_CHECK(an::sealPromptTriage(triage, "Request.").ok());
    const auto normalized = fx::chainAnalysis("sess_production", "rev_sess_production_1");
    an::SnapshotEvaluationResponse evaluation;
    evaluation.success = true;
    evaluation.analysisSnapshotId = normalized.id;
    evaluation.engine = an::ScorerEngine::JuliaLocal;
    evaluation.executedEngine = evaluation.engine;
    evaluation.executionStatus = an::ExecutionStatus::ExecutedModel;
    evaluation.modelInferenceExecuted = true;
    evaluation.modelVersion = "fixture-shaped-result@1";
    an::GroupEvaluationScore score;
    score.groupId = "grp_0";
    score.isScored = true;
    score.ambiguityScore = 0.8;
    score.winningAlternativeId = "alt_0_1";
    score.probabilities = std::vector<an::AlternativeProbability>{{"alt_0_0", 0.2}, {"alt_0_1", 0.8}};
    evaluation.groupScores.push_back(score);
    const auto realShape = an::buildAnalysisSnapshot(normalized, evaluation, fx::kChainPrompt, "discovery", "scoring");
    ROCLINT_CHECK(realShape.ok());
    ROCLINT_CHECK(roclint::storage::decodeAnalysisSnapshot(roclint::storage::encodeAnalysisSnapshot(realShape.value())).ok());
    auto mock = realShape.value();
    mock.evaluation.executionStatus = an::ExecutionStatus::ExecutedMock;
    mock.evaluation.modelInferenceExecuted = false;
    mock.snapshotHash = an::computeSnapshotHash(mock);
    mock.id = "analysis_" + mock.snapshotHash;
    ROCLINT_CHECK(an::isSnapshotSealed(mock));
    ROCLINT_CHECK(!an::validateAnalysisSnapshot(mock).ok());
    ROCLINT_CHECK(!roclint::storage::decodeAnalysisSnapshot(roclint::storage::encodeAnalysisSnapshot(mock)).ok());
    std::cout << "Production libraries reject sealed mock-scored artifacts, including saved payloads.\n";
}
