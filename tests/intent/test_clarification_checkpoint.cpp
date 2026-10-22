#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::core::intent;
using namespace roclint::bridge;

void testScenario1ClearControl() {
    std::string prompt = "Add a blue border to FloatingCard.qml";

    S1TriageAdapter s1;
    auto triage = s1.triagePrompt(prompt);
    ROCLINT_CHECK(!triage.isAmbiguous);
    ROCLINT_CHECK(triage.fastPathClear);

    WorkingIntentState state;
    NormalizedAnalysis emptyAnalysis;
    OverallPolicyResult policyResult{.overallVerdict = PolicyVerdict::EligibleForChoice};
    SnapshotEvaluationResponse snapshotEval{.success = true};

    auto decision = InterventionStateMachine::evaluate(emptyAnalysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::PromptAlreadyClear);
    std::cout << "  [I06 Scenario 1: Clear Control] Reaches Quiet without intervention.\n";
}

void testScenario2MaterialChoiceZeroPreselection() {
    std::string prompt = "Which component should handle connection timeout: FloatingCard.qml or HostController.cpp?";

    S1TriageAdapter s1(ScorerEngine::JuliaLocal);
    auto triage = s1.triagePrompt(prompt);
    ROCLINT_CHECK(triage.isAmbiguous);

    GraniteAdapter granite;
    auto discovery = granite.discoverOptions({
        .sessionId = "sess_i06",
        .revisionId = "rev_i06",
        .promptString = prompt,
        .s1Category = triage.dimension
    });
    ROCLINT_CHECK(discovery.success && discovery.choiceGroup.has_value());

    NormalizedAnalysis analysis;
    analysis.id = "snap_i06";
    analysis.baseRevisionId = "rev_i06";

    auto group = *discovery.choiceGroup;
    auto match1 = findUniqueSpan(prompt, "FloatingCard.qml");
    if (match1.span) group.alternatives[0].evidence.push_back(*match1.span);
    auto match2 = findUniqueSpan(prompt, "HostController.cpp");
    if (match2.span) group.alternatives[1].evidence.push_back(*match2.span);

    analysis.choiceGroups.push_back(group);

    auto policyResult = MaterialityPolicyV1::evaluatePrompt(analysis, prompt);
    auto snapshotEval = s1.evaluateSnapshot(analysis);

    WorkingIntentState state;
    ROCLINT_CHECK(state.confirmedAlternatives.empty()); // Zero preselection

    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::UnresolvedChoiceGroup);
    ROCLINT_CHECK(decision.activeItemId.has_value());
    ROCLINT_CHECK(*decision.activeItemId == analysis.choiceGroups.front().id);
    std::cout << "  [I06 Scenario 2: Material Choice] Reaches Clarify with zero preselection.\n";
}

void testScenario3ThreeRoundCapBehavior() {
    NormalizedAnalysis analysis;
    for (int i = 1; i <= 4; ++i) {
        ChoiceGroup g;
        g.id = "group_" + std::to_string(i);
        g.material = true;
        g.alternatives.push_back(ChoiceAlternative{.id = "alt_" + std::to_string(i) + "a"});
        g.alternatives.push_back(ChoiceAlternative{.id = "alt_" + std::to_string(i) + "b"});
        analysis.choiceGroups.push_back(g);
    }

    OverallPolicyResult policyResult{.overallVerdict = PolicyVerdict::EligibleForChoice};
    SnapshotEvaluationResponse snapshotEval{.success = true};

    WorkingIntentState state;
    for (int round = 1; round <= 3; ++round) {
        auto dec = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
        ROCLINT_CHECK(dec.verdict == InterventionVerdict::Clarify);
        ROCLINT_CHECK(dec.roundsCompleted == round - 1);
        ROCLINT_CHECK(dec.remainingBudget == 4 - round);

        state.confirmChoice("group_" + std::to_string(round), "alt_" + std::to_string(round) + "a");
    }

    // 4th question is blocked by the 3-round cap
    auto capDecision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(capDecision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(capDecision.reason == InterventionReasonCode::ClarificationBudgetExceeded);
    ROCLINT_CHECK(capDecision.activeItemId == "group_4");
    std::cout << "  [I06 Scenario 3: Three-Round Cap] Blocks 4th round and offers manual fallback.\n";
}

void testScenario4ProviderFailure() {
    NormalizedAnalysis analysis;
    OverallPolicyResult policyResult{.overallVerdict = PolicyVerdict::EligibleForChoice};
    SnapshotEvaluationResponse snapshotEval{
        .success = false,
        .errorMessage = "Inference server 503 Service Unavailable"
    };

    WorkingIntentState state;
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::ProviderUnavailable);
    std::cout << "  [I06 Scenario 4: Provider Failure] Reaches Manual with typed error code.\n";
}

int main() {
    std::cout << "[TEST] Running I06 Clarification Checkpoint Walkthrough...\n";

    testScenario1ClearControl();
    testScenario2MaterialChoiceZeroPreselection();
    testScenario3ThreeRoundCapBehavior();
    testScenario4ProviderFailure();

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL I06 CLARIFICATION WALKTHROUGH SCENARIOS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
