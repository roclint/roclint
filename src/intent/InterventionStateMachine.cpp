#include "roclint/intent/InterventionStateMachine.hpp"

#include <algorithm>
#include <limits>

namespace roclint::core::intent {

bool WorkingIntentState::isResolved(const std::string& itemId) const noexcept {
    if (confirmedAlternatives.find(itemId) != confirmedAlternatives.end()) {
        return true;
    }
    return std::find(resolvedItemIds.begin(), resolvedItemIds.end(), itemId) != resolvedItemIds.end();
}

void WorkingIntentState::confirmChoice(const std::string& groupId, const std::string& alternativeId) {
    confirmedAlternatives[groupId] = alternativeId;
    markResolved(groupId);
    ++roundsCompleted;
}

void WorkingIntentState::markResolved(const std::string& itemId) {
    if (!isResolved(itemId)) {
        resolvedItemIds.push_back(itemId);
    }
}

namespace {

[[nodiscard]] double getS1PriorityScore(
    const std::string& groupId,
    const bridge::SnapshotEvaluationResponse& snapshotEval) noexcept {
    for (const auto& score : snapshotEval.groupScores) {
        if (score.groupId == groupId) {
            return score.ambiguityScore;
        }
    }
    return 0.0;
}

[[nodiscard]] std::size_t getEarliestStartByte(const analysis::ChoiceGroup& group) noexcept {
    std::size_t earliest = std::numeric_limits<std::size_t>::max();
    for (const auto& alt : group.alternatives) {
        for (const auto& span : alt.evidence) {
            if (span.startByte < earliest) {
                earliest = span.startByte;
            }
        }
    }
    return earliest;
}

[[nodiscard]] std::size_t getEarliestAssumptionStartByte(const analysis::Assumption& assumption) noexcept {
    std::size_t earliest = std::numeric_limits<std::size_t>::max();
    for (const auto& span : assumption.evidence) {
        if (span.startByte < earliest) {
            earliest = span.startByte;
        }
    }
    return earliest;
}

} // namespace

std::vector<const analysis::ChoiceGroup*> InterventionStateMachine::orderChoiceGroups(
    const std::vector<analysis::ChoiceGroup>& groups,
    const bridge::SnapshotEvaluationResponse& snapshotEval) noexcept {

    std::vector<const analysis::ChoiceGroup*> ordered;
    ordered.reserve(groups.size());
    for (const auto& g : groups) {
        ordered.push_back(&g);
    }

    std::stable_sort(ordered.begin(), ordered.end(), [&](const analysis::ChoiceGroup* a, const analysis::ChoiceGroup* b) {
        const double scoreA = getS1PriorityScore(a->id, snapshotEval);
        const double scoreB = getS1PriorityScore(b->id, snapshotEval);
        if (scoreA != scoreB) {
            return scoreA > scoreB; // Higher score first
        }

        const std::size_t spanA = getEarliestStartByte(*a);
        const std::size_t spanB = getEarliestStartByte(*b);
        if (spanA != spanB) {
            return spanA < spanB; // Earlier byte in buffer first
        }

        return a->id < b->id; // Lexicographical fallback
    });

    return ordered;
}

InterventionDecision InterventionStateMachine::evaluate(
    const analysis::NormalizedAnalysis& analysis,
    const analysis::OverallPolicyResult& policyResult,
    const bridge::SnapshotEvaluationResponse& snapshotEval,
    const WorkingIntentState& workingState) noexcept {

    InterventionDecision decision;
    decision.roundsCompleted = workingState.roundsCompleted;
    decision.remainingBudget = (workingState.roundsCompleted < kMaxClarificationBudget)
                                   ? (kMaxClarificationBudget - workingState.roundsCompleted)
                                   : 0;

    // 1. Provider Gate
    if (!snapshotEval.success) {
        decision.verdict = InterventionVerdict::Manual;
        decision.reason = InterventionReasonCode::ProviderUnavailable;
        decision.activeItemId = std::nullopt;
        decision.diagnosticMessage = "Upstream inference provider failed: " + snapshotEval.errorMessage;
        return decision;
    }

    // 2. Structural Gate
    if (policyResult.overallVerdict == analysis::PolicyVerdict::Rejected) {
        decision.verdict = InterventionVerdict::Manual;
        decision.reason = InterventionReasonCode::InvalidAnalysisStructure;
        decision.activeItemId = std::nullopt;
        decision.diagnosticMessage = "Analysis failed structural or materiality validation.";
        return decision;
    }

    // 3. Conflict Priority (Tier 1)
    for (const auto& conflict : analysis.conflicts) {
        if (!workingState.isResolved(conflict.id)) {
            decision.verdict = InterventionVerdict::Manual;
            decision.reason = InterventionReasonCode::BlockingConflictPresent;
            decision.activeItemId = conflict.id;
            decision.diagnosticMessage = "Direct conflict requires manual specification: " + conflict.description;
            return decision;
        }
    }

    // Collect unresolved material items
    std::vector<const analysis::ChoiceGroup*> unresolvedEligibleGroups;
    for (const auto& group : analysis.choiceGroups) {
        if (group.material && !workingState.isResolved(group.id)) {
            unresolvedEligibleGroups.push_back(&group);
        }
    }

    std::vector<const analysis::Assumption*> unresolvedMaterialAssumptions;
    for (const auto& assumption : analysis.assumptions) {
        if (assumption.material && !workingState.isResolved(assumption.id)) {
            unresolvedMaterialAssumptions.push_back(&assumption);
        }
    }

    const bool hasUnresolvedItems = !unresolvedEligibleGroups.empty() || !unresolvedMaterialAssumptions.empty();

    // 4. Resolution Gate (Guard 1: Check resolution BEFORE budget cap)
    if (!hasUnresolvedItems) {
        decision.verdict = InterventionVerdict::Quiet;
        decision.reason = (workingState.roundsCompleted == 0)
                              ? InterventionReasonCode::PromptAlreadyClear
                              : InterventionReasonCode::AllMaterialItemsResolved;
        decision.activeItemId = std::nullopt;
        decision.diagnosticMessage = "All material decisions have been confirmed; staying quiet.";
        return decision;
    }

    // Find next candidate item ID for diagnostics and budget cap reporting
    std::string nextCandidateId{};
    if (!unresolvedEligibleGroups.empty()) {
        auto sorted = orderChoiceGroups(analysis.choiceGroups, snapshotEval);
        for (const auto* g : sorted) {
            if (g->material && !workingState.isResolved(g->id)) {
                nextCandidateId = g->id;
                break;
            }
        }
    } else if (!unresolvedMaterialAssumptions.empty()) {
        nextCandidateId = unresolvedMaterialAssumptions.front()->id;
    }

    // 5. Budget Cap Gate (Guard 1: Only triggered when asking a 4th question)
    if (workingState.roundsCompleted >= kMaxClarificationBudget) {
        decision.verdict = InterventionVerdict::Manual;
        decision.reason = InterventionReasonCode::ClarificationBudgetExceeded;
        decision.activeItemId = nextCandidateId.empty() ? std::nullopt : std::optional<std::string>(nextCandidateId);
        decision.diagnosticMessage = "Clarification round budget exceeded (3 rounds completed); offering manual escape.";
        return decision;
    }

    // 6. Policy Review Gate
    if (policyResult.overallVerdict == analysis::PolicyVerdict::NeedsManualReview) {
        decision.verdict = InterventionVerdict::Manual;
        decision.reason = InterventionReasonCode::PolicyRequiresManualReview;
        decision.activeItemId = nextCandidateId.empty() ? std::nullopt : std::optional<std::string>(nextCandidateId);
        decision.diagnosticMessage = "Materiality policy requires manual review.";
        return decision;
    }

    // 7. ChoiceGroup Priority (Tier 2 - Guard 3: Total ordering)
    if (!unresolvedEligibleGroups.empty()) {
        auto sorted = orderChoiceGroups(analysis.choiceGroups, snapshotEval);
        for (const auto* g : sorted) {
            if (g->material && !workingState.isResolved(g->id)) {
                decision.verdict = InterventionVerdict::Clarify;
                decision.reason = InterventionReasonCode::UnresolvedChoiceGroup;
                decision.activeItemId = g->id;
                decision.diagnosticMessage = "Prompting user for decision on choice group '" + g->id + "'.";
                return decision;
            }
        }
    }

    // 8. Assumption Priority (Tier 3)
    if (!unresolvedMaterialAssumptions.empty()) {
        std::vector<const analysis::Assumption*> sortedAssumptions = unresolvedMaterialAssumptions;
        std::stable_sort(sortedAssumptions.begin(), sortedAssumptions.end(),
                         [](const analysis::Assumption* a, const analysis::Assumption* b) {
                             const std::size_t spanA = getEarliestAssumptionStartByte(*a);
                             const std::size_t spanB = getEarliestAssumptionStartByte(*b);
                             if (spanA != spanB) {
                                 return spanA < spanB;
                             }
                             return a->id < b->id;
                         });

        decision.verdict = InterventionVerdict::Clarify;
        decision.reason = InterventionReasonCode::UnresolvedMaterialAssumption;
        decision.activeItemId = sortedAssumptions.front()->id;
        decision.diagnosticMessage = "Prompting user to confirm material assumption '" + sortedAssumptions.front()->id + "'.";
        return decision;
    }

    decision.verdict = InterventionVerdict::Quiet;
    decision.reason = InterventionReasonCode::AllMaterialItemsResolved;
    decision.activeItemId = std::nullopt;
    return decision;
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
        case InterventionReasonCode::UnresolvedChoiceGroup: return "UNRESOLVED_CHOICE_GROUP";
        case InterventionReasonCode::UnresolvedMaterialAssumption: return "UNRESOLVED_MATERIAL_ASSUMPTION";
        case InterventionReasonCode::BlockingConflictPresent: return "BLOCKING_CONFLICT_PRESENT";
        case InterventionReasonCode::ProviderUnavailable: return "PROVIDER_UNAVAILABLE";
        case InterventionReasonCode::InvalidAnalysisStructure: return "INVALID_ANALYSIS_STRUCTURE";
        case InterventionReasonCode::PolicyRequiresManualReview: return "POLICY_REQUIRES_MANUAL_REVIEW";
        case InterventionReasonCode::ClarificationBudgetExceeded: return "CLARIFICATION_BUDGET_EXCEEDED";
    }
    return "UNKNOWN";
}

} // namespace roclint::core::intent
