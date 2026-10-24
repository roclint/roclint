#include "roclint/intent/InterventionStateMachine.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>
#include <algorithm>
#include <limits>

using namespace roclint::core;
using namespace roclint::core::intent;
using roclint::bridge::ExecutionStatus;
using roclint::bridge::GroupEvaluationScore;

namespace {

void testFixtureGroupsAreEligible() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    ROCLINT_CHECK(p.policy.isEligible());
    for (const auto& g : p.policy.groupResults) {
        ROCLINT_CHECK(g.isEligible());
    }
}

void testClearPromptReachesQuiet() {
    const std::string text = "Print hello world in Python.";
    fx::Pipeline p(text, fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text).build());
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::PromptAlreadyClear);
    ROCLINT_CHECK(p.decision.remainingBudget == InterventionStateMachine::kMaxClarificationBudget);
}

void testBudgetAccountingAcrossThreeRounds() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.remainingBudget == 3);
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_0");

    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_1").ok());
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.remainingBudget == 2);
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_1");

    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_1_0").ok());
    p.reevaluate();
    // Round 2 complete: one question left.
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.remainingBudget == 1);
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_2");

    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_2_0").ok());
    p.reevaluate();
    ROCLINT_CHECK(p.state.roundsCompleted == 3);
    ROCLINT_CHECK(p.decision.remainingBudget == 0);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::AllMaterialItemsResolved);
}

void testBudgetExhaustionWithItemsLeftGoesManual() {
    const std::string text = "Make it faster, smaller, safer, and nicer to read.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .group("Faster how?", {"Latency", "Throughput"}, "faster")
                 .group("Smaller how?", {"Binary size", "Memory use"}, "smaller")
                 .group("Safer how?", {"Memory safety", "Input validation"}, "safer")
                 .group("Nicer how?", {"Shorter functions", "Better names"}, "nicer to read")
                 .build();
    fx::Pipeline p(text, a);
    for (int round = 0; round < 3; ++round) {
        ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
        ROCLINT_CHECK(InterventionStateMachine::applySkip(p.analysis, p.decision, p.state).ok());
        p.reevaluate();
    }
    ROCLINT_CHECK(p.decision.remainingBudget == 0);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::ClarificationBudgetExceeded);
    // No further transition is accepted once the budget is spent.
    ROCLINT_CHECK(!InterventionStateMachine::applySkip(p.analysis, p.decision, p.state).ok());
}

void testSkipsAndAssumptionsConsumeBudget() {
    const std::string text = "Deploy the service to staging tonight.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .assumption("Staging uses the same database as production", "staging")
                 .assumption("Tonight means after 18:00 local time", "tonight")
                 .build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.activeItemKind == ActiveItemKind::Assumption);
    ROCLINT_CHECK(*p.decision.activeItemId == "asm_0");

    ROCLINT_CHECK(InterventionStateMachine::applyAssumptionDecision(p.analysis, p.decision, p.state, false).ok());
    p.reevaluate();
    ROCLINT_CHECK(p.decision.remainingBudget == 2);
    ROCLINT_CHECK(*p.decision.activeItemId == "asm_1");

    ROCLINT_CHECK(InterventionStateMachine::applySkip(p.analysis, p.decision, p.state).ok());
    p.reevaluate();
    ROCLINT_CHECK(p.state.roundsCompleted == 2);
    ROCLINT_CHECK(p.decision.remainingBudget == 1);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::MaterialItemsDeferred);
}

void testExplicitManualAnswerAfterThreeChoices() {
    auto a = fx::threeGroupAnalysis();
    analysis::MissingDecision missing;
    missing.id = "mdc_extra";
    missing.question = "Confirm the final interpretation";
    missing.blocking = true;
    a.missingDecisions.push_back(missing);
    fx::Pipeline p(fx::kThreeGroupPrompt, a);
    for (int round = 0; round < 3; ++round) {
        ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
        const auto group = *p.decision.activeItemId;
        const auto found = std::find_if(p.analysis.choiceGroups.begin(), p.analysis.choiceGroups.end(),
                                        [&](const auto& g) { return g.id == group; });
        ROCLINT_CHECK(found != p.analysis.choiceGroups.end());
        ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, found->alternatives[0].id).ok());
        p.reevaluate();
    }
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::ClarificationBudgetExceeded);
    ROCLINT_CHECK(!InterventionStateMachine::applySkip(p.analysis, p.decision, p.state).ok());
    ROCLINT_CHECK(InterventionStateMachine::applyCustomText(p.analysis, p.decision, p.state,
                   "Use the choices I confirmed; keep the explicit color constraints.").ok());
    p.reevaluate();
    ROCLINT_CHECK(p.state.roundsCompleted == 3);
    ROCLINT_CHECK(p.state.confirmedAlternatives.size() == 3);
    ROCLINT_CHECK(p.state.customResolutions.count("mdc_extra") == 1);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);
}

void testNonBlockingConflictDoesNotBlock() {
    const std::string text = "Keep the API stable but feel free to rename things.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .conflict("Renaming may break API stability", false)
                 .build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(p.policy.blockingConflictCount == 0);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);
}

void testBlockingConflictGoesManual() {
    const std::string text = "Use only tabs. Use only spaces.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .group("Which indentation?", {"Tabs", "Spaces"}, "only tabs")
                 .conflict("Tabs and spaces are both required exclusively", true)
                 .build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::BlockingConflictPresent);
    ROCLINT_CHECK(*p.decision.activeItemId == "cfl_0");
}

void testBlockingMissingDecisionTriggersIntervention() {
    const std::string text = "Write a migration script.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .missing("Which database engine is the target?", true)
                 .missing("Should the script log progress?", false)
                 .build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::UnresolvedMissingDecision);
    ROCLINT_CHECK(p.decision.activeItemKind == ActiveItemKind::MissingDecision);
    ROCLINT_CHECK(*p.decision.activeItemId == "mdc_0");

    // Skipping a blocking decision leaves it deferred and blocks the rewrite path.
    ROCLINT_CHECK(InterventionStateMachine::applySkip(p.analysis, p.decision, p.state).ok());
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::BlockingItemDeferred);
}

void testRejectedGroupIsNeverActive() {
    const std::string text = "Sort the list and then print it.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 // Ungrounded (no evidence): Rejected by policy.
                 .group("Sort how?", {"Ascending", "Descending"}, "")
                 // Single alternative: Rejected by policy.
                 .group("Print where?", {"Stdout"}, "print it")
                 .build();
    fx::Pipeline p(text, a);
    for (const auto& g : p.policy.groupResults) {
        ROCLINT_CHECK(g.verdict == analysis::PolicyVerdict::Rejected);
    }
    ROCLINT_CHECK(!p.decision.activeItemId.has_value() ||
                  (*p.decision.activeItemId != "grp_0" && *p.decision.activeItemId != "grp_1"));
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Manual);

    // A rejected group alongside an eligible one: the eligible one is active.
    const std::string text2 = "Sort the list and then print it to the screen.";
    auto b = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text2)
                 .group("Sort how?", {"Ascending", "Descending"}, "")
                 .group("Which screen?", {"Terminal", "Log window"}, "the screen")
                 .build();
    fx::Pipeline q(text2, b);
    ROCLINT_CHECK(q.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(*q.decision.activeItemId == "grp_1");
    // Choosing an alternative of the rejected group is refused.
    ROCLINT_CHECK(!InterventionStateMachine::applyChoice(q.analysis, q.decision, q.state, "alt_0_0").ok());
}

void testConfirmChoiceRejectsForeignAlternative() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    const auto before = p.state.roundsCompleted;
    auto st = InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_1_0");
    ROCLINT_CHECK(!st.ok());
    ROCLINT_CHECK(st.error().code == ErrorCode::InvalidInput);
    ROCLINT_CHECK(p.state.roundsCompleted == before);
    ROCLINT_CHECK(!InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_9_9").ok());
}

void testUnrankedOrderingIsByEarliestByte() {
    // Groups are declared out of textual order; unranked ordering must follow the prompt.
    const std::string text = "First alpha, then beta, finally gamma.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .group("Gamma?", {"G1", "G2"}, "gamma")
                 .group("Alpha?", {"A1", "A2"}, "alpha")
                 .group("Beta?", {"B1", "B2"}, "beta")
                 .build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(!p.eval.isRanked());
    const auto ordered = InterventionStateMachine::orderChoiceGroups(p.analysis, p.eval);
    ROCLINT_CHECK(ordered.size() == 3);
    ROCLINT_CHECK(ordered[0]->question == "Alpha?");
    ROCLINT_CHECK(ordered[1]->question == "Beta?");
    ROCLINT_CHECK(ordered[2]->question == "Gamma?");
    ROCLINT_CHECK(*p.decision.activeItemId == ordered[0]->id);
}

void testNanSafeRankedOrdering() {
    const std::string text = "First alpha, then beta, finally gamma.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .group("Alpha?", {"A1", "A2"}, "alpha")
                 .group("Beta?", {"B1", "B2"}, "beta")
                 .group("Gamma?", {"G1", "G2"}, "gamma")
                 .build();
    auto eval = roclint::bridge::S1TriageAdapter::degradedResponse(a, "");
    eval.executionStatus = ExecutionStatus::ExecutedMock;
    eval.success = true;
    eval.groupScores.clear();
    auto score = [](std::string id, double s) {
        GroupEvaluationScore g;
        g.groupId = std::move(id);
        g.isScored = true;
        g.ambiguityScore = s;
        return g;
    };
    eval.groupScores.push_back(score("grp_0", std::numeric_limits<double>::quiet_NaN()));
    eval.groupScores.push_back(score("grp_1", 0.4));
    eval.groupScores.push_back(score("grp_2", 0.9));

    const auto ordered = InterventionStateMachine::orderChoiceGroups(a, eval);
    ROCLINT_CHECK(ordered[0]->id == "grp_2");
    ROCLINT_CHECK(ordered[1]->id == "grp_1");
    ROCLINT_CHECK(ordered[2]->id == "grp_0"); // NaN sanitized to 0

    // Scores bound to another snapshot are ignored: byte order applies.
    eval.analysisSnapshotId = "snap_other";
    const auto unbound = InterventionStateMachine::orderChoiceGroups(a, eval);
    ROCLINT_CHECK(unbound[0]->id == "grp_0");
}

void testScoreBindingMismatchGoesManual() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    p.eval.analysisSnapshotId = "snap_other";
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::ScoreBindingMismatch);
}

void testProviderFailureReachesManual() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    p.eval.success = false;
    p.eval.errorMessage = "scorer crashed";
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::ProviderUnavailable);
}

void testStaleDecisionIsRejected() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    const auto old = p.decision;
    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_0").ok());
    p.reevaluate();
    // Reusing the round-0 decision after round 1 completed is refused.
    auto st = InterventionStateMachine::applySkip(p.analysis, old, p.state);
    ROCLINT_CHECK(!st.ok());
    ROCLINT_CHECK(st.error().code == ErrorCode::StaleVersion);
}

void testManualRequest() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    InterventionStateMachine::applyManualRequest(p.state);
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Manual);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::UserRequestedManual);
}

} // namespace

int main() {
    std::cout << "[Test InterventionStateMachine] Starting...\n";
    testFixtureGroupsAreEligible();
    testClearPromptReachesQuiet();
    testBudgetAccountingAcrossThreeRounds();
    testBudgetExhaustionWithItemsLeftGoesManual();
    testSkipsAndAssumptionsConsumeBudget();
    testExplicitManualAnswerAfterThreeChoices();
    testNonBlockingConflictDoesNotBlock();
    testBlockingConflictGoesManual();
    testBlockingMissingDecisionTriggersIntervention();
    testRejectedGroupIsNeverActive();
    testConfirmChoiceRejectsForeignAlternative();
    testUnrankedOrderingIsByEarliestByte();
    testNanSafeRankedOrdering();
    testScoreBindingMismatchGoesManual();
    testProviderFailureReachesManual();
    testStaleDecisionIsRejected();
    testManualRequest();
    std::cout << "[Test InterventionStateMachine] All tests PASSED.\n";
    return 0;
}
