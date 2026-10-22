#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"

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

using namespace roclint::core::analysis;
using namespace roclint::core::intent;
using namespace roclint::bridge;

void testClearPromptReachesQuiet() {
    NormalizedAnalysis analysis;
    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::EligibleForChoice;
    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = true;

    WorkingIntentState state;
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);

    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::PromptAlreadyClear);
    ROCLINT_CHECK(!decision.activeItemId.has_value());
    ROCLINT_CHECK(decision.remainingBudget == 3);
}

void testMaterialChoiceGroupReachesClarify() {
    NormalizedAnalysis analysis;
    ChoiceGroup group;
    group.id = "group_target";
    group.material = true;
    group.alternatives.push_back(ChoiceAlternative{.id = "alt_1", .interpretation = "Frontend"});
    group.alternatives.push_back(ChoiceAlternative{.id = "alt_2", .interpretation = "Backend"});
    analysis.choiceGroups.push_back(group);

    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::EligibleForChoice;

    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = true;
    snapshotEval.groupScores.push_back(GroupEvaluationScore{
        .groupId = "group_target",
        .winningAlternativeId = "alt_1",
        .ambiguityScore = 0.85
    });

    WorkingIntentState state;
    ROCLINT_CHECK(state.confirmedAlternatives.empty()); // Guard 2: Zero preselection

    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::UnresolvedChoiceGroup);
    ROCLINT_CHECK(decision.activeItemId == "group_target");
    ROCLINT_CHECK(decision.requiresUserIntervention());
}

void testTotalOrderingChoiceGroups() {
    NormalizedAnalysis analysis;

    ChoiceGroup g1;
    g1.id = "group_action";
    g1.material = true;
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_a1",
        .interpretation = "Fast timeout",
        .evidence = {SourceSpan{10, 20}}
    });

    ChoiceGroup g2;
    g2.id = "group_target";
    g2.material = true;
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_t1",
        .interpretation = "FloatingCard",
        .evidence = {SourceSpan{40, 55}}
    });

    analysis.choiceGroups = {g1, g2};

    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::EligibleForChoice;

    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = true;
    // g2 has higher S1 score (0.95 vs 0.80)
    snapshotEval.groupScores.push_back(GroupEvaluationScore{.groupId = "group_action", .ambiguityScore = 0.80});
    snapshotEval.groupScores.push_back(GroupEvaluationScore{.groupId = "group_target", .ambiguityScore = 0.95});

    WorkingIntentState state;
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);

    // Guard 3: Higher S1 score wins over earlier byte offset
    ROCLINT_CHECK(decision.activeItemId == "group_target");

    // When S1 scores tie, earlier byte offset wins (10 < 40)
    snapshotEval.groupScores[0].ambiguityScore = 0.90;
    snapshotEval.groupScores[1].ambiguityScore = 0.90;
    auto decisionTie = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(decisionTie.activeItemId == "group_action");
}

void testRound3OffByOneTrap() {
    NormalizedAnalysis analysis;
    for (int i = 1; i <= 3; ++i) {
        ChoiceGroup g;
        g.id = "g" + std::to_string(i);
        g.material = true;
        g.alternatives.push_back(ChoiceAlternative{.id = "alt_" + std::to_string(i)});
        analysis.choiceGroups.push_back(g);
    }

    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::EligibleForChoice;

    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = true;

    WorkingIntentState state;
    state.confirmChoice("g1", "alt_1");
    state.confirmChoice("g2", "alt_2");
    state.confirmChoice("g3", "alt_3");

    ROCLINT_CHECK(state.roundsCompleted == 3);

    // Guard 1: All items resolved must reach Quiet (not ClarificationBudgetExceeded!)
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::AllMaterialItemsResolved);
    ROCLINT_CHECK(!decision.activeItemId.has_value());
    ROCLINT_CHECK(decision.remainingBudget == 0);
}

void testClarificationBudgetExceeded() {
    NormalizedAnalysis analysis;
    for (int i = 1; i <= 4; ++i) {
        ChoiceGroup g;
        g.id = "g" + std::to_string(i);
        g.material = true;
        g.alternatives.push_back(ChoiceAlternative{.id = "alt_" + std::to_string(i)});
        analysis.choiceGroups.push_back(g);
    }

    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::EligibleForChoice;

    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = true;

    WorkingIntentState state;
    state.confirmChoice("g1", "alt_1");
    state.confirmChoice("g2", "alt_2");
    state.confirmChoice("g3", "alt_3");

    // 4th item remains unresolved after 3 rounds completed
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::ClarificationBudgetExceeded);
    ROCLINT_CHECK(decision.activeItemId == "g4");
}

void testBlockingConflictPrioritized() {
    NormalizedAnalysis analysis;

    ChoiceGroup g;
    g.id = "g1";
    g.material = true;
    analysis.choiceGroups.push_back(g);

    Conflict c;
    c.id = "conflict_01";
    c.description = "Stateless vs stateful session contradiction";
    analysis.conflicts.push_back(c);

    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::EligibleForChoice;

    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = true;

    WorkingIntentState state;
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);

    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::BlockingConflictPresent);
    ROCLINT_CHECK(decision.activeItemId == "conflict_01");
}

void testProviderFailureReachesManual() {
    NormalizedAnalysis analysis;
    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::EligibleForChoice;

    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = false;
    snapshotEval.errorMessage = "Jev Cloud API timeout (HTTP 504)";

    WorkingIntentState state;
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);

    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::ProviderUnavailable);
    ROCLINT_CHECK(!decision.activeItemId.has_value());
}

void testStructuralRejectionReachesManual() {
    NormalizedAnalysis analysis;
    OverallPolicyResult policyResult;
    policyResult.overallVerdict = PolicyVerdict::Rejected;
    policyResult.primaryReasonCode = ReasonCode::InvalidUtf8SpanBoundary;

    SnapshotEvaluationResponse snapshotEval;
    snapshotEval.success = true;

    WorkingIntentState state;
    auto decision = InterventionStateMachine::evaluate(analysis, policyResult, snapshotEval, state);

    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(decision.reason == InterventionReasonCode::InvalidAnalysisStructure);
    ROCLINT_CHECK(!decision.activeItemId.has_value());
}

int main() {
    std::cout << "[TEST] Running InterventionStateMachine Unit Tests..." << std::endl;

    testClearPromptReachesQuiet();
    testMaterialChoiceGroupReachesClarify();
    testTotalOrderingChoiceGroups();
    testRound3OffByOneTrap();
    testClarificationBudgetExceeded();
    testBlockingConflictPrioritized();
    testProviderFailureReachesManual();
    testStructuralRejectionReachesManual();

    std::cout << "[PASS] All InterventionStateMachine tests passed successfully!" << std::endl;
    return 0;
}
