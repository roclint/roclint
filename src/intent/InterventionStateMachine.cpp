#include "roclint/intent/InterventionStateMachine.hpp"

#include "roclint/core/prompt_state/TextNormalization.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace roclint::core::intent {

namespace {

[[nodiscard]] bool contains(const std::vector<std::string>& v, const std::string& id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

[[nodiscard]] const analysis::ChoiceGroup* findGroup(const analysis::NormalizedAnalysis& analysis, std::string_view id) {
    for (const auto& g : analysis.choiceGroups) {
        if (g.id == id) {
            return &g;
        }
    }
    return nullptr;
}

[[nodiscard]] std::size_t remainingFor(const WorkingIntentState& state) noexcept {
    const auto used = std::min(state.roundsCompleted, InterventionStateMachine::kMaxClarificationBudget);
    return InterventionStateMachine::kMaxClarificationBudget - used;
}

// Common preconditions for every transition; returns the active item ID on success.
Result<std::string> requireActive(const analysis::NormalizedAnalysis& analysis,
                                  const InterventionDecision& decision,
                                  const WorkingIntentState& state) {
    if (!decision.activeItemId.has_value() || decision.activeItemId->empty()) {
        return makeError(ErrorCode::PreconditionFailed, "There is no active item to resolve");
    }
    if (decision.roundsCompleted != state.roundsCompleted) {
        return makeError(ErrorCode::StaleVersion, "Decision was computed for an earlier clarification round");
    }
    if (remainingFor(state) == 0 &&
        !(decision.verdict == InterventionVerdict::Manual &&
          decision.reason == InterventionReasonCode::ClarificationBudgetExceeded)) {
        return makeError(ErrorCode::PreconditionFailed, "Clarification budget is exhausted");
    }
    if (classifyItem(analysis, *decision.activeItemId) == ActiveItemKind::None) {
        return makeError(ErrorCode::InvalidInput, "Active item does not belong to the current analysis");
    }
    if (state.isResolved(*decision.activeItemId)) {
        return makeError(ErrorCode::Conflict, "Item '" + *decision.activeItemId + "' is already resolved");
    }
    return *decision.activeItemId;
}

} // namespace

bool WorkingIntentState::isResolved(const std::string& itemId) const {
    return confirmedAlternatives.count(itemId) != 0 || customResolutions.count(itemId) != 0 ||
           assumptionDecisions.count(itemId) != 0 || contains(keptOriginalItemIds, itemId);
}

bool WorkingIntentState::isSkipped(const std::string& itemId) const {
    return !isResolved(itemId) && contains(skippedItemIds, itemId);
}

bool WorkingIntentState::hasUncertainty(const std::string& itemId) const {
    return uncertaintyNotes.count(itemId) != 0;
}

bool WorkingIntentState::isDeferred(const std::string& itemId) const {
    return isSkipped(itemId) || (!isResolved(itemId) && hasUncertainty(itemId));
}

ActiveItemKind classifyItem(const analysis::NormalizedAnalysis& analysis, std::string_view itemId) {
    for (const auto& g : analysis.choiceGroups) {
        if (g.id == itemId) return ActiveItemKind::ChoiceGroup;
    }
    for (const auto& a : analysis.assumptions) {
        if (a.id == itemId) return ActiveItemKind::Assumption;
    }
    for (const auto& m : analysis.missingDecisions) {
        if (m.id == itemId) return ActiveItemKind::MissingDecision;
    }
    for (const auto& c : analysis.conflicts) {
        if (c.id == itemId) return ActiveItemKind::Conflict;
    }
    return ActiveItemKind::None;
}

std::vector<const analysis::ChoiceGroup*> InterventionStateMachine::orderChoiceGroups(
    const analysis::NormalizedAnalysis& analysis,
    const bridge::SnapshotEvaluationResponse& snapshotEval) {
    std::vector<const analysis::ChoiceGroup*> ordered;
    for (const auto& presentation : analysis::buildPresentation(analysis, snapshotEval)) {
        if (const auto* group = findGroup(analysis, presentation.groupId)) ordered.push_back(group);
    }
    return ordered;
}

InterventionDecision InterventionStateMachine::evaluate(const analysis::NormalizedAnalysis& analysis,
                                                        const analysis::OverallPolicyResult& policyResult,
                                                        const bridge::SnapshotEvaluationResponse& snapshotEval,
                                                        const WorkingIntentState& state) {
    InterventionDecision decision;
    decision.roundsCompleted = state.roundsCompleted;
    decision.remainingBudget = remainingFor(state);

    auto finish = [&](InterventionVerdict verdict, InterventionReasonCode reason, std::optional<std::string> activeId,
                      std::string message) {
        decision.verdict = verdict;
        decision.reason = reason;
        decision.activeItemId = std::move(activeId);
        decision.activeItemKind = decision.activeItemId.has_value() ? classifyItem(analysis, *decision.activeItemId)
                                                                    : ActiveItemKind::None;
        decision.diagnosticMessage = std::move(message);
        return decision;
    };

    if (!snapshotEval.success) {
        return finish(InterventionVerdict::Manual, InterventionReasonCode::ProviderUnavailable, std::nullopt,
                      "Ranking provider failed: " + snapshotEval.errorMessage);
    }
    if (snapshotEval.analysisSnapshotId != analysis.id) {
        return finish(InterventionVerdict::Manual, InterventionReasonCode::ScoreBindingMismatch, std::nullopt,
                      "Ranking response does not belong to the current analysis.");
    }
    if (state.manualModeRequested) {
        return finish(InterventionVerdict::Manual, InterventionReasonCode::UserRequestedManual, std::nullopt,
                      "Manual editing was requested.");
    }
    if (policyResult.overallVerdict == analysis::PolicyVerdict::Rejected) {
        return finish(InterventionVerdict::Manual, InterventionReasonCode::InvalidAnalysisStructure, std::nullopt,
                      "Analysis failed structural or materiality validation.");
    }

    // Tier 1: blocking conflicts.
    for (const auto& conflict : analysis.conflicts) {
        if (conflict.blocking && !state.isResolved(conflict.id) && !state.isDeferred(conflict.id)) {
            return finish(InterventionVerdict::Manual, InterventionReasonCode::BlockingConflictPresent, conflict.id,
                          "Conflicting requirements need a manual decision: " + conflict.description);
        }
    }

    auto verdictFor = [&](const std::string& groupId) {
        const auto* g = policyResult.findGroup(groupId);
        return g == nullptr ? analysis::PolicyVerdict::Rejected : g->verdict;
    };

    std::vector<const analysis::ChoiceGroup*> eligibleGroups;
    std::vector<const analysis::ChoiceGroup*> manualGroups;
    for (const auto* g : orderChoiceGroups(analysis, snapshotEval)) {
        if (!g->material || state.isResolved(g->id) || state.isDeferred(g->id)) {
            continue;
        }
        switch (verdictFor(g->id)) {
            case analysis::PolicyVerdict::EligibleForChoice: eligibleGroups.push_back(g); break;
            case analysis::PolicyVerdict::NeedsManualReview: manualGroups.push_back(g); break;
            case analysis::PolicyVerdict::Rejected: break;
        }
    }

    std::vector<const analysis::MissingDecision*> blockingMissing;
    for (const auto& md : analysis.missingDecisions) {
        if (md.blocking && !state.isResolved(md.id) && !state.isDeferred(md.id)) {
            blockingMissing.push_back(&md);
        }
    }

    std::vector<const analysis::Assumption*> materialAssumptions;
    for (const auto idx : analysis::orderedAssumptionIndices(analysis, snapshotEval)) {
        const auto& a = analysis.assumptions[idx];
        if (a.material && !state.isResolved(a.id) && !state.isDeferred(a.id)) {
            materialAssumptions.push_back(&a);
        }
    }

    std::optional<std::string> nextCandidate;
    if (!eligibleGroups.empty()) {
        nextCandidate = eligibleGroups.front()->id;
    } else if (!blockingMissing.empty()) {
        nextCandidate = blockingMissing.front()->id;
    } else if (!materialAssumptions.empty()) {
        nextCandidate = materialAssumptions.front()->id;
    } else if (!manualGroups.empty()) {
        nextCandidate = manualGroups.front()->id;
    }

    if (nextCandidate.has_value() && decision.remainingBudget == 0) {
        return finish(InterventionVerdict::Manual, InterventionReasonCode::ClarificationBudgetExceeded, nextCandidate,
                      "All " + std::to_string(kMaxClarificationBudget) +
                          " clarification rounds are used; edit the prompt directly.");
    }

    if (policyResult.overallVerdict == analysis::PolicyVerdict::NeedsManualReview &&
        policyResult.primaryReasonCode != analysis::ReasonCode::DirectConflictPresent) {
        return finish(InterventionVerdict::Manual, InterventionReasonCode::PolicyRequiresManualReview, nextCandidate,
                      std::string("Materiality policy requires manual review: ") +
                          analysis::toString(policyResult.primaryReasonCode));
    }

    if (!eligibleGroups.empty()) {
        return finish(InterventionVerdict::Clarify, InterventionReasonCode::UnresolvedChoiceGroup,
                      eligibleGroups.front()->id, "Choose how to resolve: " + eligibleGroups.front()->question);
    }
    if (!blockingMissing.empty()) {
        return finish(InterventionVerdict::Clarify, InterventionReasonCode::UnresolvedMissingDecision,
                      blockingMissing.front()->id, "Missing decision: " + blockingMissing.front()->question);
    }
    if (!materialAssumptions.empty()) {
        return finish(InterventionVerdict::Clarify, InterventionReasonCode::UnresolvedMaterialAssumption,
                      materialAssumptions.front()->id, "Confirm assumption: " + materialAssumptions.front()->statement);
    }
    if (!manualGroups.empty()) {
        return finish(InterventionVerdict::Manual, InterventionReasonCode::PolicyRequiresManualReview,
                      manualGroups.front()->id, "This question needs manual review: " + manualGroups.front()->question);
    }

    for (const auto& c : analysis.conflicts) {
        if (c.blocking && state.isDeferred(c.id)) {
            return finish(InterventionVerdict::Manual, InterventionReasonCode::BlockingItemDeferred, c.id,
                          "A blocking conflict was skipped and still needs a decision.");
        }
    }
    for (const auto& md : analysis.missingDecisions) {
        if (md.blocking && state.isDeferred(md.id)) {
            return finish(InterventionVerdict::Manual, InterventionReasonCode::BlockingItemDeferred, md.id,
                          "A blocking decision was skipped and still needs an answer.");
        }
    }

    bool anyDeferred = std::any_of(state.skippedItemIds.begin(), state.skippedItemIds.end(),
                                   [&](const std::string& id) { return state.isSkipped(id); });
    if (!anyDeferred) {
        anyDeferred = std::any_of(state.uncertaintyNotes.begin(), state.uncertaintyNotes.end(),
                                  [&](const auto& entry) {
                                      return classifyItem(analysis, entry.first) != ActiveItemKind::None &&
                                             !state.isResolved(entry.first);
                                  });
    }
    if (anyDeferred) {
        return finish(InterventionVerdict::Quiet, InterventionReasonCode::MaterialItemsDeferred, std::nullopt,
                      "Remaining questions were skipped or marked unknown and stay unresolved.");
    }

    return finish(InterventionVerdict::Quiet,
                  state.roundsCompleted == 0 ? InterventionReasonCode::PromptAlreadyClear
                                             : InterventionReasonCode::AllMaterialItemsResolved,
                  std::nullopt, "No open material questions.");
}

Status InterventionStateMachine::applyChoice(const analysis::NormalizedAnalysis& analysis,
                                             const InterventionDecision& decision,
                                             WorkingIntentState& state,
                                             std::string_view alternativeId) {
    if (remainingFor(state) == 0) return makeError(ErrorCode::PreconditionFailed, "Clarification budget is exhausted");
    auto active = requireActive(analysis, decision, state);
    if (!active.ok()) return active.error();
    if (decision.verdict != InterventionVerdict::Clarify) {
        return makeError(ErrorCode::PreconditionFailed, "Choices can only be confirmed on a clarification card");
    }
    const auto* group = findGroup(analysis, active.value());
    if (group == nullptr) {
        return makeError(ErrorCode::InvalidInput, "The active item is not a choice group");
    }
    const bool belongs = std::any_of(group->alternatives.begin(), group->alternatives.end(),
                                     [&](const analysis::ChoiceAlternative& a) { return a.id == alternativeId; });
    if (!belongs) {
        return makeError(ErrorCode::InvalidInput, "Alternative '" + std::string(alternativeId) +
                                                      "' does not belong to the active group '" + group->id + "'");
    }
    state.confirmedAlternatives[group->id] = std::string(alternativeId);
    ++state.roundsCompleted;
    return Status::success();
}

Status InterventionStateMachine::applyCustomText(const analysis::NormalizedAnalysis& analysis,
                                                 const InterventionDecision& decision,
                                                 WorkingIntentState& state,
                                                 std::string_view text) {
    auto active = requireActive(analysis, decision, state);
    if (!active.ok()) return active.error();
    const std::string normalized = normalizeLineEndings(text);
    const auto first = normalized.find_first_not_of(" \t\n");
    if (first == std::string::npos) {
        return makeError(ErrorCode::InvalidInput, "Custom answer cannot be blank");
    }
    const auto last = normalized.find_last_not_of(" \t\n");
    state.customResolutions[active.value()] = normalized.substr(first, last - first + 1);
    // The automatic question budget never blocks an explicit manual correction. Keep the
    // counter bounded and preserve the user's exact resolution in confirmed intent.
    if (remainingFor(state) > 0) ++state.roundsCompleted;
    return Status::success();
}

Status InterventionStateMachine::applyKeepOriginal(const analysis::NormalizedAnalysis& analysis,
                                                   const InterventionDecision& decision,
                                                   WorkingIntentState& state) {
    if (remainingFor(state) == 0) return makeError(ErrorCode::PreconditionFailed, "Clarification budget is exhausted");
    auto active = requireActive(analysis, decision, state);
    if (!active.ok()) return active.error();
    if (classifyItem(analysis, active.value()) != ActiveItemKind::ChoiceGroup) {
        return makeError(ErrorCode::InvalidInput, "Only choice groups can keep the original wording");
    }
    state.keptOriginalItemIds.push_back(active.value());
    ++state.roundsCompleted;
    return Status::success();
}

Status InterventionStateMachine::applyAssumptionDecision(const analysis::NormalizedAnalysis& analysis,
                                                         const InterventionDecision& decision,
                                                         WorkingIntentState& state,
                                                         bool accepted) {
    if (remainingFor(state) == 0) return makeError(ErrorCode::PreconditionFailed, "Clarification budget is exhausted");
    auto active = requireActive(analysis, decision, state);
    if (!active.ok()) return active.error();
    if (decision.verdict != InterventionVerdict::Clarify ||
        classifyItem(analysis, active.value()) != ActiveItemKind::Assumption) {
        return makeError(ErrorCode::InvalidInput, "The active item is not an assumption awaiting confirmation");
    }
    state.assumptionDecisions[active.value()] = accepted;
    ++state.roundsCompleted;
    return Status::success();
}

Status InterventionStateMachine::applySkip(const analysis::NormalizedAnalysis& analysis,
                                           const InterventionDecision& decision,
                                           WorkingIntentState& state) {
    if (remainingFor(state) == 0) return makeError(ErrorCode::PreconditionFailed, "Clarification budget is exhausted");
    auto active = requireActive(analysis, decision, state);
    if (!active.ok()) return active.error();
    if (!contains(state.skippedItemIds, active.value())) {
        state.skippedItemIds.push_back(active.value());
    }
    ++state.roundsCompleted;
    return Status::success();
}

namespace {

[[nodiscard]] std::string trimmedUserText(std::string_view text) {
    const std::string normalized = normalizeLineEndings(text);
    const auto first = normalized.find_first_not_of(" \t\n");
    if (first == std::string::npos) return {};
    const auto last = normalized.find_last_not_of(" \t\n");
    return normalized.substr(first, last - first + 1);
}

[[nodiscard]] std::string lowerCopy(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

[[nodiscard]] bool isYesAnswer(std::string_view lower) {
    return lower == "yes" || lower == "y" || lower == "true" || lower == "accept" ||
           lower == "accepted" || lower == "agree" || lower.starts_with("yes,") ||
           lower.starts_with("yes ");
}

[[nodiscard]] bool isNoAnswer(std::string_view lower) {
    return lower == "no" || lower == "n" || lower == "false" || lower == "decline" ||
           lower == "declined" || lower == "disagree" || lower.starts_with("no,") ||
           lower.starts_with("no ");
}

void eraseResolutionFor(WorkingIntentState& state, const std::string& itemId) {
    state.confirmedAlternatives.erase(itemId);
    state.customResolutions.erase(itemId);
    state.assumptionDecisions.erase(itemId);
    state.uncertaintyNotes.erase(itemId);
    std::erase(state.keptOriginalItemIds, itemId);
    std::erase(state.skippedItemIds, itemId);
}

[[nodiscard]] std::string previousDecisionValue(const analysis::NormalizedAnalysis& analysis,
                                                const WorkingIntentState& state,
                                                const std::string& itemId,
                                                std::string& previousKind) {
    if (const auto chosen = state.confirmedAlternatives.find(itemId); chosen != state.confirmedAlternatives.end()) {
        previousKind = "SELECTED_ALTERNATIVE";
        for (const auto& group : analysis.choiceGroups) {
            if (group.id != itemId) continue;
            for (const auto& alt : group.alternatives) {
                if (alt.id == chosen->second) return alt.interpretation;
            }
        }
        return chosen->second;
    }
    if (const auto custom = state.customResolutions.find(itemId); custom != state.customResolutions.end()) {
        previousKind = "CUSTOM_TEXT";
        return custom->second;
    }
    if (const auto assumption = state.assumptionDecisions.find(itemId); assumption != state.assumptionDecisions.end()) {
        previousKind = assumption->second ? "ACCEPTED_ASSUMPTION" : "DECLINED_ASSUMPTION";
        return assumption->second ? "Accepted the assumption" : "Rejected the assumption";
    }
    if (const auto note = state.uncertaintyNotes.find(itemId); note != state.uncertaintyNotes.end()) {
        previousKind = "UNKNOWN";
        return note->second;
    }
    if (contains(state.keptOriginalItemIds, itemId)) {
        previousKind = "USE_ORIGINAL";
        return "Keep the original wording";
    }
    previousKind.clear();
    return {};
}

} // namespace

Status InterventionStateMachine::applyInterviewChoice(const analysis::NormalizedAnalysis& analysis,
                                                      WorkingIntentState& state,
                                                      std::string_view itemId,
                                                      std::string_view alternativeId) {
    const auto* group = findGroup(analysis, itemId);
    if (group == nullptr) {
        return makeError(ErrorCode::InvalidInput, "The interview item is not a choice group");
    }
    const auto alternative = std::find_if(group->alternatives.begin(), group->alternatives.end(),
                                          [&](const analysis::ChoiceAlternative& candidate) {
                                              return candidate.id == alternativeId;
                                          });
    if (alternative == group->alternatives.end()) {
        return makeError(ErrorCode::InvalidInput, "That option does not belong to the current question");
    }
    eraseResolutionFor(state, group->id);
    state.confirmedAlternatives[group->id] = std::string(alternativeId);
    return Status::success();
}

Status InterventionStateMachine::applyInterviewCustomText(const analysis::NormalizedAnalysis& analysis,
                                                          WorkingIntentState& state,
                                                          std::string_view itemId,
                                                          std::string_view text) {
    const auto kind = classifyItem(analysis, itemId);
    if (kind == ActiveItemKind::None) {
        return makeError(ErrorCode::InvalidInput, "That answer does not belong to the current interview");
    }
    const auto normalized = trimmedUserText(text);
    if (normalized.empty()) {
        return makeError(ErrorCode::InvalidInput, "Custom answer cannot be blank");
    }
    if (normalized.size() > 4096) {
        return makeError(ErrorCode::InvalidInput, "Keep the answer within 4096 UTF-8 bytes");
    }
    eraseResolutionFor(state, std::string(itemId));
    state.customResolutions[std::string(itemId)] = normalized;
    return Status::success();
}

Status InterventionStateMachine::applyInterviewAssumption(const analysis::NormalizedAnalysis& analysis,
                                                          WorkingIntentState& state,
                                                          std::string_view itemId,
                                                          bool accepted) {
    if (classifyItem(analysis, itemId) != ActiveItemKind::Assumption) {
        return makeError(ErrorCode::InvalidInput, "The interview item is not an assumption");
    }
    eraseResolutionFor(state, std::string(itemId));
    state.assumptionDecisions[std::string(itemId)] = accepted;
    return Status::success();
}

Status InterventionStateMachine::applyInterviewSkip(const analysis::NormalizedAnalysis& analysis,
                                                    WorkingIntentState& state,
                                                    std::string_view itemId) {
    if (classifyItem(analysis, itemId) == ActiveItemKind::None) {
        return makeError(ErrorCode::InvalidInput, "That question does not belong to the current interview");
    }
    if (state.isResolved(std::string(itemId))) {
        return makeError(ErrorCode::Conflict, "That item is already decided. Change the decision explicitly first.");
    }
    if (!contains(state.skippedItemIds, std::string(itemId))) {
        state.skippedItemIds.emplace_back(itemId);
    }
    return Status::success();
}

Status InterventionStateMachine::applyInterviewUnknown(const analysis::NormalizedAnalysis& analysis,
                                                        WorkingIntentState& state,
                                                        std::string_view itemId,
                                                        std::string_view rawText) {
    if (classifyItem(analysis, itemId) == ActiveItemKind::None) {
        return makeError(ErrorCode::InvalidInput, "That question does not belong to the current interview");
    }
    eraseResolutionFor(state, std::string(itemId));
    state.uncertaintyNotes[std::string(itemId)] = trimmedUserText(rawText);
    return Status::success();
}

Status InterventionStateMachine::applyDecisionChange(const analysis::NormalizedAnalysis& analysis,
                                                     WorkingIntentState& state,
                                                     std::string_view itemId,
                                                     std::string_view newText,
                                                     std::int64_t nowMs) {
    const auto kind = classifyItem(analysis, itemId);
    if (kind == ActiveItemKind::None) {
        return makeError(ErrorCode::InvalidInput, "That decision does not belong to the current analysis");
    }
    const auto normalized = trimmedUserText(newText);
    if (normalized.empty()) {
        return makeError(ErrorCode::InvalidInput, "The corrected decision cannot be blank");
    }
    if (normalized.size() > 4096) {
        return makeError(ErrorCode::InvalidInput, "Keep the corrected decision within 4096 UTF-8 bytes");
    }

    std::string previousKind;
    const auto previous = previousDecisionValue(analysis, state, std::string(itemId), previousKind);
    if (!previous.empty() && previous != normalized) {
        state.supersededDecisions.push_back(SupersededIntentDecision{
            std::string(itemId), previous, previousKind, nowMs});
    }

    eraseResolutionFor(state, std::string(itemId));
    if (kind == ActiveItemKind::ChoiceGroup) {
        const auto* group = findGroup(analysis, itemId);
        const auto lower = lowerCopy(normalized);
        for (const auto& alt : group->alternatives) {
            if (alt.id == normalized || lowerCopy(alt.interpretation) == lower) {
                state.confirmedAlternatives[std::string(itemId)] = alt.id;
                return Status::success();
            }
        }
    } else if (kind == ActiveItemKind::Assumption) {
        const auto lower = lowerCopy(normalized);
        if (isYesAnswer(lower)) {
            state.assumptionDecisions[std::string(itemId)] = true;
            return Status::success();
        }
        if (isNoAnswer(lower)) {
            state.assumptionDecisions[std::string(itemId)] = false;
            return Status::success();
        }
    }
    state.customResolutions[std::string(itemId)] = normalized;
    return Status::success();
}

void InterventionStateMachine::applyManualRequest(WorkingIntentState& state) {
    state.manualModeRequested = true;
}

void InterventionStateMachine::recordPresented(const InterventionDecision& decision, WorkingIntentState& state) {
    if (decision.activeItemId.has_value() && !contains(state.presentedItemIds, *decision.activeItemId)) {
        state.presentedItemIds.push_back(*decision.activeItemId);
    }
}

const char* toString(InterventionVerdict verdict) noexcept {
    switch (verdict) {
        case InterventionVerdict::Quiet: return "QUIET";
        case InterventionVerdict::Clarify: return "CLARIFY";
        case InterventionVerdict::Manual: return "MANUAL";
    }
    return "UNKNOWN";
}

const char* toString(InterventionReasonCode reason) noexcept {
    switch (reason) {
        case InterventionReasonCode::PromptAlreadyClear: return "PROMPT_ALREADY_CLEAR";
        case InterventionReasonCode::AllMaterialItemsResolved: return "ALL_MATERIAL_ITEMS_RESOLVED";
        case InterventionReasonCode::MaterialItemsDeferred: return "MATERIAL_ITEMS_DEFERRED";
        case InterventionReasonCode::UnresolvedChoiceGroup: return "UNRESOLVED_CHOICE_GROUP";
        case InterventionReasonCode::UnresolvedMissingDecision: return "UNRESOLVED_MISSING_DECISION";
        case InterventionReasonCode::UnresolvedMaterialAssumption: return "UNRESOLVED_MATERIAL_ASSUMPTION";
        case InterventionReasonCode::BlockingConflictPresent: return "BLOCKING_CONFLICT_PRESENT";
        case InterventionReasonCode::BlockingItemDeferred: return "BLOCKING_ITEM_DEFERRED";
        case InterventionReasonCode::ProviderUnavailable: return "PROVIDER_UNAVAILABLE";
        case InterventionReasonCode::ScoreBindingMismatch: return "SCORE_BINDING_MISMATCH";
        case InterventionReasonCode::InvalidAnalysisStructure: return "INVALID_ANALYSIS_STRUCTURE";
        case InterventionReasonCode::PolicyRequiresManualReview: return "POLICY_REQUIRES_MANUAL_REVIEW";
        case InterventionReasonCode::ClarificationBudgetExceeded: return "CLARIFICATION_BUDGET_EXCEEDED";
        case InterventionReasonCode::UserRequestedManual: return "USER_REQUESTED_MANUAL";
    }
    return "UNKNOWN";
}

const char* toString(ActiveItemKind kind) noexcept {
    switch (kind) {
        case ActiveItemKind::None: return "NONE";
        case ActiveItemKind::ChoiceGroup: return "CHOICE_GROUP";
        case ActiveItemKind::Assumption: return "ASSUMPTION";
        case ActiveItemKind::MissingDecision: return "MISSING_DECISION";
        case ActiveItemKind::Conflict: return "CONFLICT";
    }
    return "UNKNOWN";
}

} // namespace roclint::core::intent
