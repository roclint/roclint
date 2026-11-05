#include "roclint/storage/PayloadCodec.hpp"
#include "support/storage_fixtures.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

using namespace roclint;
using namespace roclint::core;
namespace an = roclint::core::analysis;

namespace {
const std::string prompt = "The service needs tests.";
an::NormalizedAnalysis normalized(int count) {
    auto builder = fx::AnalysisBuilder("sess_assumptions", "rev_sess_assumptions_1", prompt);
    for (int index = 0; index < count; ++index) builder.assumption("Material premise " + std::to_string(index), "service", true);
    builder.assumption("Incidental premise", "tests", false);
    return builder.build();
}

an::SnapshotEvaluationResponse scored(const an::NormalizedAnalysis& input) {
    auto result = bridge::S1TriageAdapter::degradedResponse(input, "");
    result.engine = an::ScorerEngine::JuliaLocal;
    result.executedEngine = result.engine;
    result.executionStatus = an::ExecutionStatus::ExecutedModel;
    result.modelInferenceExecuted = true;
    result.modelVersion = "fixture_model@1";
    for (std::size_t index = 0; index < result.assumptionScores.size() && index < an::kMaxAssumptionChecks; ++index) {
        auto& score = result.assumptionScores[index];
        score.isScored = true;
        score.uncertaintyScore = index == 1 ? 0.9 : 0.2;
        score.unscoredReason.clear();
    }
    return result;
}

void testSuggestedAlternativeEvidenceRoundTrip() {
    an::NormalizedAnalysis input;
    input.sessionId = "sess_suggestion";
    input.baseRevisionId = "rev_sess_suggestion_1";
    an::MissingDecision missing;
    missing.id = an::mintMissingDecisionId(0);
    missing.question = "Which database should the migration target?";
    missing.blocking = true;
    missing.suggestedAlternativeEvidence = "An excerpt of the deployment configuration would resolve this.";
    input.missingDecisions.push_back(missing);
    an::sealSnapshot(input);

    const auto encoded = storage::encodeAnalysis(input);
    const auto decoded = storage::decodeAnalysis(encoded);
    ROCLINT_CHECK(decoded.ok());
    ROCLINT_CHECK(decoded.value() == input);
    ROCLINT_CHECK(decoded.value().missingDecisions.front().suggestedAlternativeEvidence ==
                  missing.suggestedAlternativeEvidence);
    ROCLINT_CHECK(an::isSnapshotSealed(decoded.value()));

    auto tampered = input;
    tampered.missingDecisions.front().suggestedAlternativeEvidence = "A different evidence path.";
    ROCLINT_CHECK(!an::isSnapshotSealed(tampered));
}

void testPlanCapCoverageAndStrictValidation() {
    const auto input = normalized(5);
    const auto plan = an::buildScoringPlan(input);
    ROCLINT_CHECK(plan.groupIds.empty() && plan.assumptionIds.size() == 4);
    ROCLINT_CHECK(plan.assumptionIds.front() == "asm_0" && plan.assumptionIds.back() == "asm_3");
    const auto response = bridge::S1TriageAdapter::validateLocalEvaluation(input, scored(input));
    ROCLINT_CHECK(response.isRanked() && response.assumptionScores.size() == 5);
    ROCLINT_CHECK(!response.assumptionScores[4].isScored && response.assumptionScores[4].unscoredReason == "batch_limit");
    const auto snapshot = an::buildAnalysisSnapshot(input, response, prompt, "discovery", "scoring");
    ROCLINT_CHECK(snapshot.ok() && snapshot.value().payloadVersion == 3);
    ROCLINT_CHECK(snapshot.value().assumptionOrder.front() == "asm_1");
    ROCLINT_CHECK(snapshot.value().assumptionOrder.back() == "asm_4");
    ROCLINT_CHECK(storage::decodeAnalysisSnapshot(storage::encodeAnalysisSnapshot(snapshot.value())).value() == snapshot.value());
    auto excessive = scored(input);
    excessive.assumptionScores[4].isScored = true;
    excessive.assumptionScores[4].uncertaintyScore = 0.99;
    excessive.assumptionScores[4].unscoredReason.clear();
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(input, excessive, prompt, "discovery", "scoring").ok());
    const auto bounded = bridge::S1TriageAdapter::validateLocalEvaluation(input, excessive);
    ROCLINT_CHECK(!bounded.assumptionScores[4].isScored);
    auto duplicate = scored(input);
    duplicate.assumptionScores.push_back(duplicate.assumptionScores.front());
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(input, duplicate, prompt, "discovery", "scoring").ok());
    ROCLINT_CHECK(!bridge::S1TriageAdapter::validateLocalEvaluation(input, duplicate).assumptionScores[0].isScored);
    auto foreign = scored(input);
    foreign.assumptionScores.front().assumptionId = "foreign";
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(input, foreign, prompt, "discovery", "scoring").ok());
    auto stalePlan = scored(input);
    stalePlan.planVersion = 1;
    ROCLINT_CHECK(!bridge::S1TriageAdapter::validateLocalEvaluation(input, stalePlan).isRanked());
}

void testScoresNeverConfirmAssumptionsAndSurviveReopen() {
    const auto input = normalized(1);
    auto evaluation = scored(input);
    evaluation.assumptionScores.front().uncertaintyScore = 0.001;
    const auto snapshot = an::buildAnalysisSnapshot(input, evaluation, prompt, "discovery", "scoring").value();
    const auto policy = an::MaterialityPolicyV1::evaluatePrompt(input, prompt);
    intent::WorkingIntentState working;
    const auto decision = intent::InterventionStateMachine::evaluate(input, policy, evaluation, working);
    ROCLINT_CHECK(decision.verdict == intent::InterventionVerdict::Clarify);
    ROCLINT_CHECK(decision.activeItemKind == intent::ActiveItemKind::Assumption);
    ROCLINT_CHECK(working.assumptionDecisions.empty());
    intent::InterventionStateMachine::recordPresented(decision, working);
    ROCLINT_CHECK(intent::InterventionStateMachine::applyAssumptionDecision(input, decision, working, true).ok());
    const auto confirmed = intent::IntentProvenanceBuilder::buildConfirmedIntent(snapshot, policy, working, 3).value();
    ROCLINT_CHECK(confirmed.items.size() == 1 && confirmed.items[0].kind == intent::IntentResolutionKind::AcceptedAssumption);
    auto changedScores = evaluation;
    changedScores.assumptionScores.front().uncertaintyScore = 0.95;
    const auto next = an::buildAnalysisSnapshot(input, changedScores, prompt, "discovery", "scoring").value();
    ROCLINT_CHECK(next.id != snapshot.id);
    ROCLINT_CHECK(!intent::IntentProvenanceBuilder::isIntentValid(confirmed, input.baseRevisionId, RevisionStatus::Clean, next).ok());
    const auto path = std::filesystem::temp_directory_path() /
        ("roclint-assumption-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
    {
        storage::Database db(path);
        storage::PromptRepository repository(db);
        ROCLINT_CHECK(repository.migrate().ok());
        PromptRevision root(SessionId(input.sessionId), RevisionId(input.baseRevisionId), std::nullopt, prompt, 1);
        ROCLINT_CHECK(repository.createSessionWithRootRevision(root, 1).ok());
        fx::recordSnapshotAttempts(repository, snapshot);
        ROCLINT_CHECK(repository.insertAnalysisSnapshot(snapshot, 2).ok());
        ROCLINT_CHECK(repository.insertIntentVersion(confirmed, 3).ok());
    }
    {
        storage::Database db(path);
        storage::PromptRepository repository(db);
        ROCLINT_CHECK(repository.migrate().ok());
        const auto restored = repository.restoreSession(SessionId(input.sessionId)).value();
        ROCLINT_CHECK(restored.analysis && *restored.analysis == snapshot);
        ROCLINT_CHECK(restored.intent && *restored.intent == confirmed);
    }
    std::filesystem::remove(path);
}
void testLegacySnapshotHashAndPayloadRemainUnchanged() {
    // Fixed v2 payload/hash from the pre-assumption canonical encoder, not generated by v3 code.
    const std::string payload = R"LEGACY({"schema":"analysis_snapshot.v2","revision_text_hash":"dfb57a34b68c93f46b8d2c0f579bc0e3bdb51ee49512697b743b44eb5feaed9e","id":"analysis_6c9c18558d7761db0834ea0812f4b74cac008786f6201ad318fe2fc5540c5823","snapshot_hash":"6c9c18558d7761db0834ea0812f4b74cac008786f6201ad318fe2fc5540c5823","normalized_payload":"{\"schema\":\"analysis.v1\",\"id\":\"snap_7a388bd7848f266aa7ab1ed3a269764fbf38fd5e246aa3cb03a4e3567994675f\",\"session_id\":\"sess_legacy\",\"base_revision_id\":\"rev_sess_legacy_1\",\"snapshot_hash\":\"7a388bd7848f266aa7ab1ed3a269764fbf38fd5e246aa3cb03a4e3567994675f\",\"choice_groups\":[],\"assumptions\":[],\"missing_decisions\":[],\"conflicts\":[]}","discovery_attempt_id":"legacy_discovery","scorer_attempt_id":"","presentation_policy":"scorer_presentation.v1","evaluation":{"success":true,"error":"","degraded_reason":"Legacy scorer unavailable","normalized_id":"snap_7a388bd7848f266aa7ab1ed3a269764fbf38fd5e246aa3cb03a4e3567994675f","engine":0,"execution_status":2,"executed_engine":null,"model_version":"none","model_inference_executed":false,"latency_ms":0,"confidence_threshold":0.7,"threshold_version":"v1","groups":[]},"presentation":[]})LEGACY";
    const auto restored = storage::decodeAnalysisSnapshot(payload);
    ROCLINT_CHECK(restored.ok());
    ROCLINT_CHECK(restored.value().payloadVersion == 2 && restored.value().evaluation.planVersion == 1);
    ROCLINT_CHECK(restored.value().snapshotHash == "6c9c18558d7761db0834ea0812f4b74cac008786f6201ad318fe2fc5540c5823");
    ROCLINT_CHECK(restored.value().assumptionOrder.empty());
    ROCLINT_CHECK(storage::decodeAnalysisSnapshot(storage::encodeAnalysisSnapshot(restored.value())).value() == restored.value());
    auto parsed = json::parse(payload).value();
    auto evaluation = *parsed.find("evaluation");
    evaluation.set("plan_version", json::Value::number(2));
    parsed.set("evaluation", std::move(evaluation));
    ROCLINT_CHECK(!storage::decodeAnalysisSnapshot(json::serialize(parsed)).ok());
}
} // namespace

int main() {
    testLegacySnapshotHashAndPayloadRemainUnchanged();
    testSuggestedAlternativeEvidenceRoundTrip();
    testPlanCapCoverageAndStrictValidation();
    testScoresNeverConfirmAssumptionsAndSurviveReopen();
    std::cout << "Assumption batch limits, scoring evidence, user authority and history passed.\n";
}
