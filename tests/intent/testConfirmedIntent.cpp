#include "roclint/intent/ConfirmedIntent.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::intent;

namespace {

void testBuildsOnlyFromExplicitActions() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_1").ok());
    p.reevaluate();

    auto built = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1000);
    ROCLINT_CHECK(built.ok());
    const auto& intent = built.value();
    ROCLINT_CHECK(intent.sessionId == "sess_a");
    ROCLINT_CHECK(intent.baseRevisionId == "rev_sess_a_1");
    ROCLINT_CHECK(intent.snapshotId == fx::finalSnapshot(p.analysis, p.text, p.eval).id);
    ROCLINT_CHECK(intent.snapshotHash == fx::finalSnapshot(p.analysis, p.text, p.eval).snapshotHash);
    ROCLINT_CHECK(intent.id == "intent_" + intent.intentHash);

    ROCLINT_CHECK(intent.items.size() == 1);
    const auto chosen = intent.getDecision("grp_0");
    ROCLINT_CHECK(chosen.has_value());
    ROCLINT_CHECK(chosen->kind == IntentResolutionKind::SelectedAlternative);
    ROCLINT_CHECK(chosen->chosenAlternativeId == "alt_0_1");
    ROCLINT_CHECK(chosen->chosenValue == "Higher batch throughput");

    // Material groups the user never answered are unresolved, never guessed.
    ROCLINT_CHECK(intent.unresolvedItemIds.size() == 2);
    ROCLINT_CHECK(!intent.hasDecisionFor("grp_1"));
    ROCLINT_CHECK(!intent.hasDecisionFor("grp_2"));

    // Presented items are recorded in order.
    ROCLINT_CHECK(intent.visibleItemIds.size() == 2);
    ROCLINT_CHECK(intent.visibleItemIds[0] == "grp_0");
    ROCLINT_CHECK(intent.visibleItemIds[1] == "grp_1");
}

void testResolutionKinds() {
    const std::string text = "Ship the release notes to customers by Friday.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .group("Which customers?", {"All customers", "Enterprise only"}, "customers")
                 .assumption("Friday means this week", "by Friday")
                 .missing("Which channel?", true)
                 .build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_0");
    ROCLINT_CHECK(InterventionStateMachine::applyKeepOriginal(p.analysis, p.decision, p.state).ok());
    p.reevaluate();
    ROCLINT_CHECK(*p.decision.activeItemId == "mdc_0");
    ROCLINT_CHECK(InterventionStateMachine::applyCustomText(p.analysis, p.decision, p.state, "Email newsletter").ok());
    p.reevaluate();
    ROCLINT_CHECK(*p.decision.activeItemId == "asm_0");
    ROCLINT_CHECK(InterventionStateMachine::applyAssumptionDecision(p.analysis, p.decision, p.state, false).ok());
    p.reevaluate();
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Quiet);

    auto built = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1000);
    ROCLINT_CHECK(built.ok());
    const auto& intent = built.value();
    ROCLINT_CHECK(intent.getDecision("grp_0")->kind == IntentResolutionKind::UseOriginal);
    ROCLINT_CHECK(intent.getDecision("mdc_0")->kind == IntentResolutionKind::CustomText);
    ROCLINT_CHECK(intent.getDecision("mdc_0")->customText == "Email newsletter");
    ROCLINT_CHECK(intent.getDecision("asm_0")->kind == IntentResolutionKind::DeclinedAssumption);
    ROCLINT_CHECK(intent.rejectedItemIds.size() == 1 && intent.rejectedItemIds[0] == "asm_0");
    ROCLINT_CHECK(intent.unresolvedItemIds.empty());
    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent, "rev_sess_a_1", RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval)).ok());
}

void testRejectsForgedWorkingState() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());

    auto foreignAlt = p.state;
    foreignAlt.confirmedAlternatives["grp_0"] = "alt_1_0";
    ROCLINT_CHECK(!IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, foreignAlt, 1).ok());

    auto unknownGroup = p.state;
    unknownGroup.confirmedAlternatives["grp_9"] = "alt_9_0";
    ROCLINT_CHECK(!IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, unknownGroup, 1).ok());

    auto emptyCustom = p.state;
    emptyCustom.customResolutions["grp_0"] = "";
    ROCLINT_CHECK(!IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, emptyCustom, 1).ok());

    auto badAssumption = p.state;
    badAssumption.assumptionDecisions["grp_0"] = true;
    ROCLINT_CHECK(!IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, badAssumption, 1).ok());

    auto unsealed = fx::finalSnapshot(p.analysis, p.text, p.eval);
    unsealed.normalized.choiceGroups[0].question = "tampered";
    ROCLINT_CHECK(!IntentProvenanceBuilder::buildConfirmedIntent(unsealed, p.policy, p.state, 1).ok());
}

void testHashIsDeterministicAndExcludesTimestamp() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_0").ok());
    auto a = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1000).value();
    auto b = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 9999).value();
    ROCLINT_CHECK(a.intentHash == b.intentHash);
    ROCLINT_CHECK(a.id == b.id);

    auto c = a;
    c.items[0].chosenValue = "something else";
    ROCLINT_CHECK(IntentProvenanceBuilder::computeIntentHash(c) != a.intentHash);
}

void testValidityChecks() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    ROCLINT_CHECK(InterventionStateMachine::applyChoice(p.analysis, p.decision, p.state, "alt_0_0").ok());
    const auto intent = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1000).value();
    const std::string rev = "rev_sess_a_1";

    ROCLINT_CHECK(IntentProvenanceBuilder::isIntentValid(intent, rev, RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval)).ok());

    auto dirty = IntentProvenanceBuilder::isIntentValid(intent, rev, RevisionStatus::DraftDirty, fx::finalSnapshot(p.analysis, p.text, p.eval));
    ROCLINT_CHECK(!dirty.ok() && dirty.error().code == ErrorCode::StaleVersion);

    auto newRev = IntentProvenanceBuilder::isIntentValid(intent, "rev_sess_a_2", RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval));
    ROCLINT_CHECK(!newRev.ok() && newRev.error().code == ErrorCode::StaleVersion);

    auto tampered = intent;
    tampered.items[0].chosenAlternativeId = "alt_0_1";
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(tampered, rev, RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval)).ok());

    // A re-analysis of the same revision yields a different snapshot: the intent is stale.
    auto reanalysis = fx::AnalysisBuilder("sess_a", rev, fx::kThreeGroupPrompt)
                          .group("Which kind of speed matters?", {"Latency", "Throughput"}, "runs faster")
                          .build();
    auto snap = IntentProvenanceBuilder::isIntentValid(intent, rev, RevisionStatus::Clean, fx::finalSnapshot(reanalysis, p.text));
    ROCLINT_CHECK(!snap.ok() && snap.error().code == ErrorCode::StaleVersion);

    // Cross-session: the same revision label in another session is rejected.
    auto other = fx::threeGroupAnalysis("sess_b", rev);
    ROCLINT_CHECK(!IntentProvenanceBuilder::isIntentValid(intent, rev, RevisionStatus::Clean, fx::finalSnapshot(other, p.text)).ok());
}

void testBlockingItemsMustBeResolved() {
    const std::string text = "Write a migration script.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text).missing("Which database?", true).build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(InterventionStateMachine::applySkip(p.analysis, p.decision, p.state).ok());
    auto built = IntentProvenanceBuilder::buildConfirmedIntent(fx::finalSnapshot(p.analysis, p.text, p.eval), p.policy, p.state, 1);
    ROCLINT_CHECK(built.ok());
    ROCLINT_CHECK(built.value().unresolvedItemIds.size() == 1);
    auto st = IntentProvenanceBuilder::isIntentValid(built.value(), "rev_sess_a_1", RevisionStatus::Clean, fx::finalSnapshot(p.analysis, p.text, p.eval));
    ROCLINT_CHECK(!st.ok() && st.error().code == ErrorCode::PreconditionFailed);
}

} // namespace

int main() {
    std::cout << "[Test ConfirmedIntent] Starting...\n";
    testBuildsOnlyFromExplicitActions();
    testResolutionKinds();
    testRejectsForgedWorkingState();
    testHashIsDeterministicAndExcludesTimestamp();
    testValidityChecks();
    testBlockingItemsMustBeResolved();
    std::cout << "[Test ConfirmedIntent] All tests PASSED.\n";
    return 0;
}
