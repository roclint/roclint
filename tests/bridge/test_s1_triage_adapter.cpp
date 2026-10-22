#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#define ROCLINT_CHECK(cond)                                                          \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__        \
                      << " -> " << #cond << std::endl;                              \
            std::exit(1);                                                            \
        }                                                                            \
    } while (false)

using namespace roclint::bridge;
using namespace roclint::core::analysis;

void testFastTriageClearPrompt() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);

    std::string prompt = "Add a blue border to DashboardWindow.qml";
    auto decision = adapter.triagePrompt(prompt);

    ROCLINT_CHECK(decision.inputValidity == InputValidity::Valid);
    ROCLINT_CHECK(!decision.isAmbiguous);
    ROCLINT_CHECK(decision.ambiguityScore < 0.70);
    ROCLINT_CHECK(decision.fastPathClear);
    ROCLINT_CHECK(decision.latencyMs >= 0.0);
}

void testFastTriageAmbiguousAction() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);

    std::string prompt = "Optimize timeout handling or refactor connection pool";
    auto decision = adapter.triagePrompt(prompt);

    ROCLINT_CHECK(decision.inputValidity == InputValidity::Valid);
    ROCLINT_CHECK(decision.isAmbiguous);
    ROCLINT_CHECK(decision.ambiguityScore >= 0.70);
    ROCLINT_CHECK(decision.dimension == "action");
    ROCLINT_CHECK(!decision.fastPathClear);
}

void testFastTriageAmbiguousTarget() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);

    std::string prompt = "Which component should handle error states: UI or host controller?";
    auto decision = adapter.triagePrompt(prompt);

    ROCLINT_CHECK(decision.inputValidity == InputValidity::Valid);
    ROCLINT_CHECK(decision.isAmbiguous);
    ROCLINT_CHECK(decision.ambiguityScore >= 0.70);
    ROCLINT_CHECK(decision.dimension == "target");
    ROCLINT_CHECK(!decision.fastPathClear);
}

void testFastTriageRuleConflict() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);

    std::string prompt = "Implement stateless JWT auth with sticky session override";
    auto decision = adapter.triagePrompt(prompt);

    ROCLINT_CHECK(decision.inputValidity == InputValidity::Valid);
    ROCLINT_CHECK(decision.isAmbiguous);
    ROCLINT_CHECK(decision.ambiguityScore >= 0.70);
    ROCLINT_CHECK(decision.dimension == "rule_conflict");
    ROCLINT_CHECK(!decision.fastPathClear);
}

void testPrecedencePhraseIsNotRuleConflict() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);

    std::string prompt = "Give accessibility requirements priority over visual styling.";
    auto decision = adapter.triagePrompt(prompt);

    ROCLINT_CHECK(decision.inputValidity == InputValidity::Valid);
    ROCLINT_CHECK(!decision.isAmbiguous);
    ROCLINT_CHECK(decision.ambiguityScore < 0.70);
    ROCLINT_CHECK(decision.dimension != "rule_conflict");
    ROCLINT_CHECK(decision.fastPathClear);

    std::string prompt2 = "Prefer SQLite storage over in-memory storage";
    auto decision2 = adapter.triagePrompt(prompt2);
    ROCLINT_CHECK(decision2.inputValidity == InputValidity::Valid);
    ROCLINT_CHECK(!decision2.isAmbiguous);
    ROCLINT_CHECK(decision2.fastPathClear);

    std::string prompt3 = "Prefer stateless token auth over sticky session storage";
    auto decision3 = adapter.triagePrompt(prompt3);
    ROCLINT_CHECK(decision3.inputValidity == InputValidity::Valid);
    ROCLINT_CHECK(!decision3.isAmbiguous);
    ROCLINT_CHECK(decision3.fastPathClear);
    ROCLINT_CHECK(decision3.dimension != "rule_conflict");
}

void testEmptyAndWhitespaceInputValidation() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);

    auto decisionEmpty = adapter.triagePrompt("");
    ROCLINT_CHECK(decisionEmpty.inputValidity == InputValidity::Empty);
    ROCLINT_CHECK(!decisionEmpty.isAmbiguous);
    ROCLINT_CHECK(!decisionEmpty.fastPathClear);
    ROCLINT_CHECK(decisionEmpty.ambiguityScore == 0.0);

    auto decisionWhitespace = adapter.triagePrompt("   \t\r\n   ");
    ROCLINT_CHECK(decisionWhitespace.inputValidity == InputValidity::WhitespaceOnly);
    ROCLINT_CHECK(!decisionWhitespace.isAmbiguous);
    ROCLINT_CHECK(!decisionWhitespace.fastPathClear);
    ROCLINT_CHECK(decisionWhitespace.ambiguityScore == 0.0);
}

void testEngineAvailabilityTaxonomy() {
    S1TriageAdapter localAdapter(ScorerEngine::JuliaLocal);
    ROCLINT_CHECK(localAdapter.configuredEngine() == ScorerEngine::JuliaLocal);
    ROCLINT_CHECK(!localAdapter.isLocalModelLoaded());
    ROCLINT_CHECK(localAdapter.availability() == SemanticAvailability::DegradedUnranked);

    localAdapter.setLocalModelLoaded(true);
    ROCLINT_CHECK(localAdapter.isLocalModelLoaded());
    ROCLINT_CHECK(localAdapter.availability() == SemanticAvailability::FullRankedLocal);

    S1TriageAdapter cloudAdapter(ScorerEngine::JevCloud);
    ROCLINT_CHECK(cloudAdapter.configuredEngine() == ScorerEngine::JevCloud);
    ROCLINT_CHECK(cloudAdapter.availability() == SemanticAvailability::FullRankedCloud);

    cloudAdapter.setCloudAvailable(false);
    ROCLINT_CHECK(cloudAdapter.availability() == SemanticAvailability::DegradedUnranked);
}

void testExplicitUnscoredResultWhenUnranked() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);
    ROCLINT_CHECK(!adapter.isLocalModelLoaded());

    NormalizedAnalysis analysis;
    analysis.id = "norm_unscored";
    analysis.baseRevisionId = "rev_001";

    ChoiceGroup group;
    group.id = "group_target";
    group.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "FloatingCard.qml",
        .whyMaterial = "Presentation layer",
        .evidence = {}
    });
    group.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "HostController.cpp",
        .whyMaterial = "Native C++ backend",
        .evidence = {}
    });
    analysis.choiceGroups.push_back(group);

    auto resp = adapter.evaluateSnapshot(analysis);
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.executionStatus == ExecutionStatus::DegradedUnranked);
    ROCLINT_CHECK(!resp.modelInferenceExecuted);
    ROCLINT_CHECK(resp.groupScores.size() == 1);

    const auto& score = resp.groupScores.front();
    ROCLINT_CHECK(score.groupId == "group_target");
    ROCLINT_CHECK(!score.winningAlternativeId.has_value());
    ROCLINT_CHECK(!score.isScored);

    ROCLINT_CHECK(score.probabilities.size() == 2);
    ROCLINT_CHECK(score.probabilities[0].alternativeId == "alt_1");
    ROCLINT_CHECK(score.probabilities[1].alternativeId == "alt_2");
    ROCLINT_CHECK(std::abs(score.probabilities[0].probability - 0.5) < 1e-4);
    ROCLINT_CHECK(std::abs(score.probabilities[1].probability - 0.5) < 1e-4);

    ChoiceGroup permutedGroup = group;
    std::swap(permutedGroup.alternatives[0], permutedGroup.alternatives[1]);
    NormalizedAnalysis permutedAnalysis = analysis;
    permutedAnalysis.choiceGroups[0] = permutedGroup;

    auto resp2 = adapter.evaluateSnapshot(permutedAnalysis);
    const auto& score2 = resp2.groupScores.front();
    ROCLINT_CHECK(!score2.winningAlternativeId.has_value());
    ROCLINT_CHECK(std::abs(score2.probabilities[0].probability - 0.5) < 1e-4);
    ROCLINT_CHECK(std::abs(score2.probabilities[1].probability - 0.5) < 1e-4);
}

void testOfflineJuliaExecutionWithModelLoaded() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);
    adapter.setLocalModelLoaded(true);
    ROCLINT_CHECK(adapter.availability() == SemanticAvailability::FullRankedLocal);

    NormalizedAnalysis analysis;
    analysis.id = "norm_001";
    analysis.baseRevisionId = "rev_001";

    ChoiceGroup group;
    group.id = "group_target";
    group.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "FloatingCard.qml",
        .whyMaterial = "Presentation layer",
        .evidence = {}
    });
    group.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "HostController.cpp",
        .whyMaterial = "Native C++ backend",
        .evidence = {}
    });
    analysis.choiceGroups.push_back(group);

    auto resp = adapter.evaluateSnapshot(analysis);
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.engine == ScorerEngine::JuliaLocal);
    ROCLINT_CHECK(resp.executionStatus == ExecutionStatus::ExecutedModel);
    ROCLINT_CHECK(resp.modelInferenceExecuted);
    ROCLINT_CHECK(resp.modelVersion == "SupersonicLabs/Julia-1");
    ROCLINT_CHECK(resp.groupScores.size() == 1);

    const auto& score = resp.groupScores.front();
    ROCLINT_CHECK(score.groupId == "group_target");
    ROCLINT_CHECK(score.isScored);
    ROCLINT_CHECK(score.winningAlternativeId.has_value());
    ROCLINT_CHECK(score.winningAlternativeId.value() == "alt_1");
    ROCLINT_CHECK(score.probabilities.size() == 2);

    double totalProb = score.probabilities[0].probability + score.probabilities[1].probability;
    ROCLINT_CHECK(std::abs(totalProb - 1.0) < 1e-4);
}

void testJevCloudFailureDegradedMode() {
    S1TriageAdapter adapter(ScorerEngine::JevCloud);
    ROCLINT_CHECK(adapter.availability() == SemanticAvailability::FullRankedCloud);

    adapter.setCloudAvailable(false);
    ROCLINT_CHECK(adapter.availability() == SemanticAvailability::DegradedUnranked);

    NormalizedAnalysis analysis;
    analysis.id = "norm_cloud_fail";
    analysis.baseRevisionId = "rev_cloud_fail";

    auto resp = adapter.evaluateSnapshot(analysis);
    ROCLINT_CHECK(!resp.success);
    ROCLINT_CHECK(resp.executionStatus == ExecutionStatus::Failed);
    ROCLINT_CHECK(resp.errorMessage.find("unavailable") != std::string::npos);
    ROCLINT_CHECK(resp.groupScores.empty());
}

void testCustomMockHandler() {
    S1TriageAdapter adapter(ScorerEngine::JuliaLocal);

    adapter.setMockTriageHandler([](std::string_view, ScorerEngine) {
        S1TriageDecision dec;
        dec.inputValidity = InputValidity::Valid;
        dec.isAmbiguous = true;
        dec.ambiguityScore = 0.99;
        dec.dimension = "custom_rule";
        dec.rationale = "Custom mock triggered";
        dec.fastPathClear = false;
        return dec;
    });

    auto dec = adapter.triagePrompt("Any arbitrary input");
    ROCLINT_CHECK(dec.isAmbiguous);
    ROCLINT_CHECK(dec.ambiguityScore == 0.99);
    ROCLINT_CHECK(dec.dimension == "custom_rule");
    ROCLINT_CHECK(dec.rationale == "Custom mock triggered");

    adapter.setMockSnapshotHandler([](const NormalizedAnalysis&, ScorerEngine) {
        SnapshotEvaluationResponse resp;
        resp.success = true;
        resp.executionStatus = ExecutionStatus::ExecutedMock;
        resp.modelVersion = "custom_mock_v1";
        GroupEvaluationScore score;
        score.groupId = "g1";
        score.isScored = true;
        score.winningAlternativeId = "alt_mock";
        score.probabilities.push_back({"alt_mock", 0.90});
        score.probabilities.push_back({"alt_other", 0.10});
        resp.groupScores.push_back(score);
        return resp;
    });

    NormalizedAnalysis a;
    auto resp = adapter.evaluateSnapshot(a);
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.executionStatus == ExecutionStatus::ExecutedMock);
    ROCLINT_CHECK(resp.groupScores.size() == 1);
    ROCLINT_CHECK(resp.groupScores[0].winningAlternativeId == "alt_mock");
}

int main() {
    std::cout << "[TEST] Running S1TriageAdapter Unit Tests..." << std::endl;

    testFastTriageClearPrompt();
    testFastTriageAmbiguousAction();
    testFastTriageAmbiguousTarget();
    testFastTriageRuleConflict();
    testPrecedencePhraseIsNotRuleConflict();
    testEmptyAndWhitespaceInputValidation();
    testEngineAvailabilityTaxonomy();
    testExplicitUnscoredResultWhenUnranked();
    testOfflineJuliaExecutionWithModelLoaded();
    testJevCloudFailureDegradedMode();
    testCustomMockHandler();

    std::cout << "[PASS] All S1TriageAdapter tests passed successfully!" << std::endl;
    return 0;
}
