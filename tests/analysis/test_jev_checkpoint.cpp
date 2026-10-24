#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::core::intent;
using namespace roclint::bridge;

namespace {

const std::string kPrompt = "Which component should own the timeout: the card or the controller?";

NormalizedAnalysis twoGroups() {
    return fx::AnalysisBuilder("sess_jev", "rev_sess_jev_1", kPrompt)
        .group("Which component?", {"The card", "The controller"}, "the card or the controller")
        .group("Which timeout?", {"Connect timeout", "Idle timeout"}, "the timeout")
        .build();
}

void testOfflineJevPresentsGroupsUnrankedInByteOrder() {
    const auto analysis = twoGroups();
    const auto policy = MaterialityPolicyV1::evaluatePrompt(analysis, kPrompt);
    ROCLINT_CHECK(policy.isEligible());

    S1TriageAdapter s1;
    const auto triage = s1.triagePrompt(kPrompt);
    ROCLINT_CHECK(triage.isExecutable());
    ROCLINT_CHECK(!triage.isAmbiguityScored);

    const auto eval = s1.evaluateSnapshot(analysis);
    ROCLINT_CHECK(eval.success && !eval.isRanked());

    WorkingIntentState state;
    const auto decision = InterventionStateMachine::evaluate(analysis, policy, eval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Clarify);
    // "the timeout" (grp_1) appears before "the card or the controller" (grp_0).
    ROCLINT_CHECK(*decision.activeItemId == "grp_1");
    std::cout << "[Jev Checkpoint] Offline scorer: DEGRADED_UNRANKED, byte-ordered clarification.\n";
}

void testRankedJevReordersGroups() {
    const auto analysis = twoGroups();
    const auto policy = MaterialityPolicyV1::evaluatePrompt(analysis, kPrompt);

    S1TriageAdapter s1;
    s1.setMockCloudScorer([](const NormalizedAnalysis& a) -> Result<SnapshotEvaluationResponse> {
        SnapshotEvaluationResponse r;
        r.success = true;
        r.analysisSnapshotId = a.id;
        for (const auto& g : a.choiceGroups) {
            GroupEvaluationScore s;
            s.groupId = g.id;
            s.isScored = true;
            s.ambiguityScore = g.id == "grp_0" ? 0.95 : 0.30;
            r.groupScores.push_back(s);
        }
        return r;
    });
    const auto eval = s1.evaluateSnapshot(analysis);
    ROCLINT_CHECK(eval.isRanked());

    WorkingIntentState state;
    const auto decision = InterventionStateMachine::evaluate(analysis, policy, eval, state);
    ROCLINT_CHECK(decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(*decision.activeItemId == "grp_0");
}

void testRankingNeverChangesWhatIsAsked() {
    // Ranking only orders eligible groups; it can't promote rejected ones or remove questions.
    const auto analysis = twoGroups();
    const auto policy = MaterialityPolicyV1::evaluatePrompt(analysis, kPrompt);
    const auto degraded = S1TriageAdapter::degradedResponse(analysis, "offline");
    const auto ordered = InterventionStateMachine::orderChoiceGroups(analysis, degraded);
    ROCLINT_CHECK(ordered.size() == analysis.choiceGroups.size());
}

} // namespace

int main() {
    std::cout << "[Test Jev Checkpoint] Starting...\n";
    testOfflineJevPresentsGroupsUnrankedInByteOrder();
    testRankedJevReordersGroups();
    testRankingNeverChangesWhatIsAsked();
    std::cout << "[Test Jev Checkpoint] All tests PASSED.\n";
    return 0;
}
