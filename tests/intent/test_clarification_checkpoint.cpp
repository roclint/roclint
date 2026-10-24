#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::intent;

namespace {

// Model output for a prompt with one choice group and one blocking missing decision.
const std::string kPrompt = "Add caching to the API layer.";
const char* kModelReply = R"({"status": "options_generated",
  "choice_groups": [{"dimension": "storage", "question": "Where should the cache live?", "material": true,
    "alternatives": [
      {"label": "In process memory", "why_material": "Lost on restart", "evidence": ["caching"]},
      {"label": "Shared Redis", "why_material": "Shared across instances", "evidence": ["caching"]}]}],
  "missing_decisions": [{"question": "How long may entries live?", "blocking": true}]})";

void testClarificationWalkthroughFromModelOutput() {
    auto parsed = roclint::bridge::parseDiscoveryResponse(kModelReply, "sess_c", "rev_sess_c_1", kPrompt);
    ROCLINT_CHECK(parsed.ok());
    fx::Pipeline p(kPrompt, parsed.value().analysis);

    // Round 1: the choice group.
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.activeItemKind == ActiveItemKind::ChoiceGroup);
    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_1").ok());
    p.reevaluate();

    // Round 2: the blocking question, answered in the user's own words.
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.activeItemKind == ActiveItemKind::MissingDecision);
    ROCLINT_CHECK(p.decision.remainingBudget == 2);
    ROCLINT_CHECK(!InterventionStateMachine::applyCustomText(p.analysis, p.decision, p.state, "").ok());
    ROCLINT_CHECK(InterventionStateMachine::applyCustomText(p.analysis, p.decision, p.state, "Five minutes").ok());
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::AllMaterialItemsResolved);

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1);
    ROCLINT_CHECK(intent.ok());
    ROCLINT_CHECK(intent.value().getDecision("grp_0")->chosenValue == "Shared Redis");
    ROCLINT_CHECK(intent.value().getDecision("mdc_0")->customText == "Five minutes");
    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent.value(), "rev_sess_c_1", RevisionStatus::Clean,
                                                         fx::finalSnapshot(p.analysis, p.text, p.eval))
                      .ok());
}

void testResolvedItemCannotBeAnsweredTwice() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    const auto first = p.decision;
    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, first, p.state, "alt_0_0").ok());
    // Replaying the same click against the old decision is rejected and does not consume budget.
    const auto rounds = p.state.roundsCompleted;
    ROCLINT_CHECK(!InterventionStateMachine::applyChoice(p.analysis, first, p.state, "alt_0_1").ok());
    ROCLINT_CHECK(p.state.roundsCompleted == rounds);
    ROCLINT_CHECK(p.state.confirmedAlternatives.at("grp_0") == "alt_0_0");
}

void testCustomTextOnChoiceGroup() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    ROCLINT_CHECK(InterventionStateMachine::applyCustomText(p.analysis, p.decision, p.state,
                                                            "Faster cold start")
                      .ok());
    p.reevaluate();
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_1");
    auto intent = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1).value();
    ROCLINT_CHECK(intent.getDecision("grp_0")->kind == IntentResolutionKind::CustomText);
}

} // namespace

int main() {
    std::cout << "[Test Clarification Checkpoint] Starting...\n";
    testClarificationWalkthroughFromModelOutput();
    testResolvedItemCannotBeAnsweredTwice();
    testCustomTextOnChoiceGroup();
    std::cout << "[Test Clarification Checkpoint] All tests PASSED.\n";
    return 0;
}
