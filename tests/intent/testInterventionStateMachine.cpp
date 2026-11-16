#include "roclint/intent/InterventionStateMachine.hpp"
#include "roclint/intent/IntentBrief.hpp"
#include "roclint/intent/SettledRequirementPolicy.hpp"
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

namespace {

BriefItemKind kindOf(const IntentBrief& brief, const std::string& id) {
    const auto* item = brief.find(id);
    ROCLINT_CHECK(item != nullptr);
    return item->kind;
}

IntentBrief briefFor(const fx::Pipeline& p, const std::vector<InterviewRecord>& records = {}) {
    IntentBriefInputs inputs;
    inputs.sessionId = p.analysis.sessionId;
    inputs.revisionId = p.analysis.baseRevisionId;
    inputs.revisionText = p.text;
    inputs.analysis = &p.analysis;
    inputs.policy = &p.policy;
    inputs.workingIntent = &p.state;
    inputs.interviewRecords = records;
    return IntentBriefBuilder::build(inputs);
}

} // namespace

void testIntentBriefSeparatesRequirementsDecisionsAndOpenChoices() {
    const std::string text =
        "Keep the dashboard compact for everyday browsing. "
        "Do not remove the navigation bar. "
        "I'd prefer smaller type over tighter spacing.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .group("What should compact mean?", {"Smaller type", "Tighter spacing"}, "compact")
                 .assumption("The dashboard is for desktop only", "everyday browsing")
                 .missing("Which screen size matters?", true)
                 .build();
    fx::Pipeline p(text, a);
    ROCLINT_CHECK(p.decision.verdict == InterventionVerdict::Clarify);
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_0");

    auto before = briefFor(p);
    ROCLINT_CHECK(before.hasKind(BriefItemKind::SourceRequirement));
    ROCLINT_CHECK(before.hasKind(BriefItemKind::Prohibition));
    ROCLINT_CHECK(before.hasKind(BriefItemKind::Preference));
    ROCLINT_CHECK(before.hasKind(BriefItemKind::ProposedAssumption));
    ROCLINT_CHECK(!before.displaySummary.empty());
    // The display summary is a count, not a replacement for the verbatim source.
    ROCLINT_CHECK(before.displaySummary.find("smaller type") == std::string::npos);
    bool sawRequirement = false;
    for (const auto& item : before.items) {
        if (item.kind == BriefItemKind::SourceRequirement) {
            sawRequirement = true;
            ROCLINT_CHECK(item.sourceExcerpt.find("compact") != std::string::npos);
        }
        if (item.kind == BriefItemKind::Prohibition) {
            ROCLINT_CHECK(item.sourceExcerpt.find("Do not remove") != std::string::npos);
            ROCLINT_CHECK(item.status == BriefItemStatus::Confirmed);
        }
        if (item.kind == BriefItemKind::Preference) {
            ROCLINT_CHECK(item.status == BriefItemStatus::Confirmed);
            ROCLINT_CHECK(item.sourceExcerpt.find("prefer") != std::string::npos);
        }
    }
    ROCLINT_CHECK(sawRequirement);

    ROCLINT_CHECK(InterventionStateMachine::applyInterviewChoice(p.analysis, p.state, "grp_0", "alt_0_0").ok());
    p.reevaluate();
    auto after = briefFor(p);
    ROCLINT_CHECK(kindOf(after, "grp_0") == BriefItemKind::ConfirmedDecision);
    ROCLINT_CHECK(after.find("grp_0")->status == BriefItemStatus::Confirmed);
    ROCLINT_CHECK(after.find("grp_0")->text == "Smaller type");
    // An already-settled source requirement survives the decision.
    bool requirementSurvived = false;
    for (const auto& item : after.items) {
        if (item.kind == BriefItemKind::SourceRequirement &&
            item.sourceExcerpt.find("Keep the dashboard compact") != std::string::npos) {
            requirementSurvived = true;
        }
    }
    ROCLINT_CHECK(requirementSurvived);
    const auto openIds = after.openChoiceIds();
    ROCLINT_CHECK(std::find(openIds.begin(), openIds.end(), "mdc_0") != openIds.end());
    ROCLINT_CHECK(std::find(openIds.begin(), openIds.end(), "asm_0") == openIds.end());
}

void testInterviewSkipAndUnknownStayOpenWithoutConsumingBudget() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    const auto rounds = p.state.roundsCompleted;
    ROCLINT_CHECK(InterventionStateMachine::applyInterviewSkip(p.analysis, p.state, "grp_0").ok());
    ROCLINT_CHECK(p.state.roundsCompleted == rounds);
    ROCLINT_CHECK(p.state.isSkipped("grp_0"));
    ROCLINT_CHECK(!p.state.isResolved("grp_0"));
    p.reevaluate();
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_1");

    ROCLINT_CHECK(InterventionStateMachine::applyInterviewUnknown(
                      p.analysis, p.state, "grp_1", "I don't know yet").ok());
    ROCLINT_CHECK(p.state.hasUncertainty("grp_1"));
    ROCLINT_CHECK(p.state.uncertaintyNotes["grp_1"] == "I don't know yet");
    ROCLINT_CHECK(!p.state.isResolved("grp_1"));
    ROCLINT_CHECK(p.state.roundsCompleted == rounds);
    p.reevaluate();
    ROCLINT_CHECK(*p.decision.activeItemId == "grp_2");

    auto brief = briefFor(p);
    const auto* skipped = brief.find("grp_0");
    ROCLINT_CHECK(skipped != nullptr && skipped->kind == BriefItemKind::OpenChoice &&
                  skipped->status == BriefItemStatus::Open);
    const auto* unknown = brief.find("grp_1");
    ROCLINT_CHECK(unknown != nullptr && unknown->kind == BriefItemKind::OpenChoice &&
                  unknown->status == BriefItemStatus::Open);
    ROCLINT_CHECK(unknown->rawAnswer == "I don't know yet");
    // A skip or unknown is never a confirmed decision.
    for (const auto* decision : brief.confirmedDecisions()) {
        ROCLINT_CHECK(decision->analysisItemId != "grp_0" && decision->analysisItemId != "grp_1");
    }
}

void testDecisionChangeSupersedesAndPreservesProvenance() {
    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    ROCLINT_CHECK(InterventionStateMachine::applyInterviewChoice(p.analysis, p.state, "grp_0", "alt_0_0").ok());
    p.reevaluate();
    ROCLINT_CHECK(InterventionStateMachine::applyDecisionChange(
                      p.analysis, p.state, "grp_0", "alt_0_1", 5000).ok());
    ROCLINT_CHECK(p.state.confirmedAlternatives["grp_0"] == "alt_0_1");
    ROCLINT_CHECK(p.state.supersededDecisions.size() == 1);
    ROCLINT_CHECK(p.state.supersededDecisions[0].previousValue == "Lower latency per call");
    ROCLINT_CHECK(p.state.supersededDecisions[0].previousKind == "SELECTED_ALTERNATIVE");

    auto brief = briefFor(p);
    const auto* current = brief.find("grp_0");
    ROCLINT_CHECK(current != nullptr && current->kind == BriefItemKind::ConfirmedDecision);
    ROCLINT_CHECK(current->text == "Higher batch throughput");
    bool sawSuperseded = false;
    for (const auto& item : brief.items) {
        if (item.kind != BriefItemKind::SupersededDecision) continue;
        sawSuperseded = true;
        ROCLINT_CHECK(item.status == BriefItemStatus::Superseded);
        ROCLINT_CHECK(item.rawAnswer == "Lower latency per call");
        ROCLINT_CHECK(item.analysisItemId == "grp_0");
    }
    ROCLINT_CHECK(sawSuperseded);

    // A custom correction replaces the alternative and stores the verbatim answer.
    ROCLINT_CHECK(InterventionStateMachine::applyDecisionChange(
                      p.analysis, p.state, "grp_0", "I'd prefer the batch path for nightly imports", 6000).ok());
    ROCLINT_CHECK(p.state.confirmedAlternatives.count("grp_0") == 0);
    ROCLINT_CHECK(p.state.customResolutions["grp_0"] == "I'd prefer the batch path for nightly imports");
    ROCLINT_CHECK(p.state.supersededDecisions.size() == 2);
    auto custom = briefFor(p);
    ROCLINT_CHECK(custom.find("grp_0")->kind == BriefItemKind::Preference);
    ROCLINT_CHECK(custom.find("grp_0")->rawAnswer == "I'd prefer the batch path for nightly imports");
}

void testUnicodeSourceExcerptsRemainVerbatim() {
    const std::string text =
        "Keep the caf\xC3\xA9 header compact, and do not move the \xE2\x80\x9Clogo\xE2\x80\x9D.";
    auto a = fx::AnalysisBuilder("sess_a", "rev_sess_a_1", text)
                 .group("What should compact mean here?", {"Caf\xC3\xA9 header type", "Caf\xC3\xA9 header spacing"},
                        "caf\xC3\xA9 header compact")
                 .build();
    fx::Pipeline p(text, a);
    auto brief = briefFor(p);
    bool sawUnicode = false;
    for (const auto& item : brief.items) {
        if (item.sourceExcerpt.find("caf\xC3\xA9") == std::string::npos) continue;
        sawUnicode = true;
        ROCLINT_CHECK(text.find(item.sourceExcerpt) != std::string::npos);
        if (!item.sourceRef.empty() && item.sourceRef.find("#bytes:") != std::string::npos) {
            const auto at = item.sourceRef.find("#bytes:");
            const auto dash = item.sourceRef.find('-', at);
            const auto start = std::stoul(item.sourceRef.substr(at + 7, dash - at - 7));
            const auto end = std::stoul(item.sourceRef.substr(dash + 1));
            ROCLINT_CHECK(end <= text.size());
            ROCLINT_CHECK(text.substr(start, end - start) == item.sourceExcerpt);
        }
    }
    ROCLINT_CHECK(sawUnicode);
}

void testFollowUpDecisionsAndSupersessionStayInTheBrief() {
    InterviewRecord record;
    record.id = "iv_q_1";
    record.question = "Which browsing behavior matters most?";
    record.consequence = "Changes how the library is arranged.";
    record.origin = "model_followup";
    record.disposition = "custom";
    record.rawAnswer = "I'd prefer a fast A-Z jump list";
    record.asked = true;
    record.previousAnswers.push_back("A search-first layout");

    fx::Pipeline p(fx::kThreeGroupPrompt, fx::threeGroupAnalysis());
    auto brief = briefFor(p, {record});
    const auto* decision = brief.find("iv_q_1");
    ROCLINT_CHECK(decision != nullptr && decision->kind == BriefItemKind::Preference);
    ROCLINT_CHECK(decision->rawAnswer == "I'd prefer a fast A-Z jump list");
    ROCLINT_CHECK(decision->status == BriefItemStatus::Confirmed);
    bool sawSuperseded = false;
    for (const auto& item : brief.items) {
        if (item.kind == BriefItemKind::SupersededDecision && item.analysisItemId == "iv_q_1") {
            sawSuperseded = true;
            ROCLINT_CHECK(item.rawAnswer == "A search-first layout");
        }
    }
    ROCLINT_CHECK(sawSuperseded);
}

void testSettledRequirementPolicyKeepsExactValues() {
    analysis::PromptTriage triage;
    triage.categoryScores = {0.10, 0.12, 0.20, 0.9999};
    triage.primaryCategory = analysis::PromptCategory::RuleConflict;
    const std::string prompt =
        "Respond to 95% of requests within 500 ms, and keep the schema unchanged.";
    ROCLINT_CHECK(hasSettledExactValues(prompt));
    const auto tokens = exactRequirementTokens(prompt);
    ROCLINT_CHECK(tokens.size() == 2);
    ROCLINT_CHECK(std::find(tokens.begin(), tokens.end(), "95%") != tokens.end());
    ROCLINT_CHECK(std::find(tokens.begin(), tokens.end(), "500 ms") != tokens.end());

    // A lone RULE_CONFLICT head with exact values and no deterministic conflict
    // keeps the original rather than asking the user to redefine them.
    ROCLINT_CHECK(shouldKeepOriginalForRuleConflictOnly(triage, prompt, false));
    // A deterministic explicit conflict still authorizes the question.
    ROCLINT_CHECK(!shouldKeepOriginalForRuleConflictOnly(triage, prompt, true));
    // Another head at or above its threshold is real ambiguity evidence.
    auto action = triage;
    action.categoryScores[0] = 0.85;
    ROCLINT_CHECK(!shouldKeepOriginalForRuleConflictOnly(action, prompt, false));
    // No exact values means the policy does not apply.
    ROCLINT_CHECK(!shouldKeepOriginalForRuleConflictOnly(triage, "Make the mood calmer.", false));
    // Decades are word suffixes, not time units. "1990s" must not be treated as an
    // exact numeric requirement or trigger the settled-value policy.
    const std::string decades = "Use 1990s and 1960s references for the visual mood.";
    ROCLINT_CHECK(exactRequirementTokens(decades).empty());
    ROCLINT_CHECK(!hasSettledExactValues(decades));
    ROCLINT_CHECK(!shouldKeepOriginalForRuleConflictOnly(
        triage, "Use 1990s references for the visual mood.", false));
    const auto msTokens = exactRequirementTokens("Respond within 500 milliseconds.");
    ROCLINT_CHECK(msTokens.size() == 1 && msTokens.front() == "500 milliseconds");

    // The rewrite guard catches dropped values while accepting "500ms" for "500 ms".
    const auto kept = droppedExactRequirements(prompt, "Respond within 500ms to 95% of requests.");
    ROCLINT_CHECK(kept.empty());
    const auto dropped = droppedExactRequirements(prompt, "Respond quickly to requests and keep the schema.");
    ROCLINT_CHECK(dropped.size() == 2);

    ROCLINT_CHECK(looksLikeMetaInstruction("Make the mood calmer.",
                                           "Determine the specific mood categories for the user."));
    ROCLINT_CHECK(!looksLikeMetaInstruction("Determine the specific tone.",
                                            "Determine the specific tone with warmer words."));
    ROCLINT_CHECK(!looksLikeMetaInstruction("Make the mood calmer.",
                                            "Use calmer, warmer language throughout."));
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "[Test InterventionStateMachine] Starting...\n";
    const auto run = [](const char* name, auto check) {
        std::cout << "  [RUN] " << name << '\n';
        check();
    };
    run("testFixtureGroupsAreEligible", testFixtureGroupsAreEligible);
    run("testClearPromptReachesQuiet", testClearPromptReachesQuiet);
    run("testBudgetAccountingAcrossThreeRounds", testBudgetAccountingAcrossThreeRounds);
    run("testBudgetExhaustionWithItemsLeftGoesManual", testBudgetExhaustionWithItemsLeftGoesManual);
    run("testSkipsAndAssumptionsConsumeBudget", testSkipsAndAssumptionsConsumeBudget);
    run("testExplicitManualAnswerAfterThreeChoices", testExplicitManualAnswerAfterThreeChoices);
    run("testNonBlockingConflictDoesNotBlock", testNonBlockingConflictDoesNotBlock);
    run("testBlockingConflictGoesManual", testBlockingConflictGoesManual);
    run("testBlockingMissingDecisionTriggersIntervention", testBlockingMissingDecisionTriggersIntervention);
    run("testRejectedGroupIsNeverActive", testRejectedGroupIsNeverActive);
    run("testConfirmChoiceRejectsForeignAlternative", testConfirmChoiceRejectsForeignAlternative);
    run("testUnrankedOrderingIsByEarliestByte", testUnrankedOrderingIsByEarliestByte);
    run("testNanSafeRankedOrdering", testNanSafeRankedOrdering);
    run("testScoreBindingMismatchGoesManual", testScoreBindingMismatchGoesManual);
    run("testProviderFailureReachesManual", testProviderFailureReachesManual);
    run("testStaleDecisionIsRejected", testStaleDecisionIsRejected);
    run("testManualRequest", testManualRequest);
    run("testIntentBriefSeparatesRequirementsDecisionsAndOpenChoices",
        testIntentBriefSeparatesRequirementsDecisionsAndOpenChoices);
    run("testInterviewSkipAndUnknownStayOpenWithoutConsumingBudget",
        testInterviewSkipAndUnknownStayOpenWithoutConsumingBudget);
    run("testDecisionChangeSupersedesAndPreservesProvenance",
        testDecisionChangeSupersedesAndPreservesProvenance);
    run("testUnicodeSourceExcerptsRemainVerbatim", testUnicodeSourceExcerptsRemainVerbatim);
    run("testFollowUpDecisionsAndSupersessionStayInTheBrief",
        testFollowUpDecisionsAndSupersessionStayInTheBrief);
    run("testSettledRequirementPolicyKeepsExactValues",
        testSettledRequirementPolicyKeepsExactValues);
    std::cout << "[Test InterventionStateMachine] All tests PASSED.\n";
    return 0;
}
