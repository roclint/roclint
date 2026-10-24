#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::intent;

namespace {

void testIntentGoesStaleWhenThePromptChanges() {
    const SessionId session("sess_i");
    RevisionState revisions(
        PromptRevision(session, RevisionState::makeRevisionId(session, 1), std::nullopt, fx::kChainPrompt, 1));
    const std::string rev1 = revisions.currentRevision().revisionId().value();

    fx::Pipeline p(fx::kChainPrompt, fx::chainAnalysis(session.value(), rev1));
    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_0").ok());
    p.reevaluate();
    const auto intent = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 2).value();
    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent, rev1, revisions.status(), fx::finalSnapshot(p.analysis, p.text, p.eval)).ok());

    // Typing marks the draft dirty: the intent can't be used even before a commit.
    ROCLINT_CHECK(revisions.noteDraftEdit(revisions.stateVersion()).ok());
    auto dirty = IntentProvenanceBuilder::isIntentValid(intent, rev1, revisions.status(), fx::finalSnapshot(p.analysis, p.text, p.eval));
    ROCLINT_CHECK(!dirty.ok() && dirty.error().code == ErrorCode::StaleVersion);

    // Freezing the edit creates a new revision; the old intent and analysis are both stale.
    auto rev2 = revisions.commitEdit(revisions.stateVersion(), fx::kChainPrompt + " Keep the API.", 3);
    ROCLINT_CHECK(rev2.ok());
    auto stale = IntentProvenanceBuilder::isIntentValid(intent, rev2.value().value(), revisions.status(), fx::finalSnapshot(p.analysis, p.text, p.eval));
    ROCLINT_CHECK(!stale.ok() && stale.error().code == ErrorCode::StaleVersion);
}

void testNoAmbiguityYieldsEmptyValidIntent() {
    const std::string text = "Print hello world in Python.";
    auto parsed = roclint::bridge::parseDiscoveryResponse(R"({"status": "no_ambiguity"})", "sess_i",
                                                          "rev_sess_i_1", text);
    ROCLINT_CHECK(parsed.ok());
    fx::Pipeline p(text, parsed.value().analysis);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);
    ROCLINT_CHECK(p.decision.reason == InterventionReasonCode::PromptAlreadyClear);
    const auto intent = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1).value();
    ROCLINT_CHECK(intent.items.empty());
    ROCLINT_CHECK(intent.unresolvedItemIds.empty());
    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent, "rev_sess_i_1", RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval)).ok());
}

void testModelClarificationRequestBlocksIntent() {
    const std::string text = "Fix it.";
    auto parsed = roclint::bridge::parseDiscoveryResponse(
        R"({"status": "needs_clarification", "clarification_question": "What should be fixed?"})", "sess_i",
        "rev_sess_i_1", text);
    ROCLINT_CHECK(parsed.ok());
    fx::Pipeline p(text, parsed.value().analysis);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(p.decision.activeItemKind == ActiveItemKind::MissingDecision);

    // An intent built before the user answers can't pass validation.
    const auto early = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1).value();
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(early, "rev_sess_i_1", RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval)).ok());

    ROCLINT_CHECK(InterventionStateMachine::applyCustomText(p.analysis, p.decision, p.state, "The login crash").ok());
    const auto answered = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 2).value();
    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(answered, "rev_sess_i_1", RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval)).ok());
}

} // namespace

int main() {
    std::cout << "[Test Intent Checkpoint] Starting...\n";
    testIntentGoesStaleWhenThePromptChanges();
    testNoAmbiguityYieldsEmptyValidIntent();
    testModelClarificationRequestBlocksIntent();
    std::cout << "[Test Intent Checkpoint] All tests PASSED.\n";
    return 0;
}
