#pragma once

#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace roclint::core::intent {

enum class InterventionVerdict {
    Quiet,
    Clarify,
    Manual
};

enum class InterventionReasonCode {
    PromptAlreadyClear,
    AllMaterialItemsResolved,
    UnresolvedChoiceGroup,
    UnresolvedMaterialAssumption,
    BlockingConflictPresent,
    ProviderUnavailable,
    InvalidAnalysisStructure,
    PolicyRequiresManualReview,
    ClarificationBudgetExceeded
};

struct InterventionDecision {
    InterventionVerdict verdict{InterventionVerdict::Quiet};
    InterventionReasonCode reason{InterventionReasonCode::PromptAlreadyClear};
    std::optional<std::string> activeItemId{std::nullopt};
    std::size_t roundsCompleted{0};
    std::size_t remainingBudget{3};
    std::string diagnosticMessage{};

    [[nodiscard]] bool requiresUserIntervention() const noexcept {
        return verdict == InterventionVerdict::Clarify;
    }
};

struct WorkingIntentState {
    std::size_t roundsCompleted{0};
    std::vector<std::string> resolvedItemIds{};
    std::unordered_map<std::string, std::string> confirmedAlternatives{};

    [[nodiscard]] bool isResolved(const std::string& itemId) const noexcept;
    void confirmChoice(const std::string& groupId, const std::string& alternativeId);
    void markResolved(const std::string& itemId);
};

class InterventionStateMachine {
public:
    static constexpr std::size_t kMaxClarificationBudget = 3;

    [[nodiscard]] static InterventionDecision evaluate(
        const analysis::NormalizedAnalysis& analysis,
        const analysis::OverallPolicyResult& policyResult,
        const bridge::SnapshotEvaluationResponse& snapshotEval,
        const WorkingIntentState& workingState) noexcept;

    [[nodiscard]] static std::vector<const analysis::ChoiceGroup*> orderChoiceGroups(
        const std::vector<analysis::ChoiceGroup>& groups,
        const bridge::SnapshotEvaluationResponse& snapshotEval) noexcept;
};

[[nodiscard]] const char* toString(InterventionVerdict verdict) noexcept;
[[nodiscard]] const char* toString(InterventionReasonCode reason) noexcept;

} // namespace roclint::core::intent
