#pragma once

#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/core/Result.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
    MaterialItemsDeferred,
    UnresolvedChoiceGroup,
    UnresolvedMissingDecision,
    UnresolvedMaterialAssumption,
    BlockingConflictPresent,
    BlockingItemDeferred,
    ProviderUnavailable,
    ScoreBindingMismatch,
    InvalidAnalysisStructure,
    PolicyRequiresManualReview,
    ClarificationBudgetExceeded,
    UserRequestedManual
};

enum class ActiveItemKind {
    None,
    ChoiceGroup,
    Assumption,
    MissingDecision,
    Conflict
};

struct InterventionDecision {
    InterventionVerdict verdict{InterventionVerdict::Quiet};
    InterventionReasonCode reason{InterventionReasonCode::PromptAlreadyClear};
    std::optional<std::string> activeItemId{std::nullopt};
    ActiveItemKind activeItemKind{ActiveItemKind::None};
    std::size_t roundsCompleted{0};
    std::size_t remainingBudget{0};
    std::string diagnosticMessage{};

    [[nodiscard]] bool requiresUserIntervention() const noexcept {
        return verdict == InterventionVerdict::Clarify;
    }
};

// User decisions accumulated against one analysis snapshot. Mutated only through the
// InterventionStateMachine::apply* transitions, each of which consumes one clarification round.
struct WorkingIntentState {
    std::size_t roundsCompleted{0};
    std::map<std::string, std::string> confirmedAlternatives{}; // groupId -> alternativeId
    std::map<std::string, std::string> customResolutions{};     // itemId -> user-authored text
    std::map<std::string, bool> assumptionDecisions{};          // assumptionId -> accepted
    std::vector<std::string> keptOriginalItemIds{};
    std::vector<std::string> skippedItemIds{};
    std::vector<std::string> presentedItemIds{};
    bool manualModeRequested{false};

    [[nodiscard]] bool isResolved(const std::string& itemId) const;
    [[nodiscard]] bool isSkipped(const std::string& itemId) const;
};

struct InterventionStateMachine {
    static constexpr std::size_t kMaxClarificationBudget = 3;

    [[nodiscard]] static InterventionDecision evaluate(const analysis::NormalizedAnalysis& analysis,
                                                       const analysis::OverallPolicyResult& policyResult,
                                                       const bridge::SnapshotEvaluationResponse& snapshotEval,
                                                       const WorkingIntentState& workingState);

    // Groups ordered by sanitized S1 score (NaN/out-of-range/unscored treated as 0.0), then by
    // earliest evidence byte, then by ID. Scores bound to another snapshot are ignored.
    [[nodiscard]] static std::vector<const analysis::ChoiceGroup*> orderChoiceGroups(
        const analysis::NormalizedAnalysis& analysis,
        const bridge::SnapshotEvaluationResponse& snapshotEval);

    // Transitions. Each validates that the target is the decision's active item and that budget
    // remains, then records the action and consumes one round.
    [[nodiscard]] static Status applyChoice(const analysis::NormalizedAnalysis& analysis,
                                            const InterventionDecision& decision,
                                            WorkingIntentState& state,
                                            std::string_view alternativeId);
    [[nodiscard]] static Status applyCustomText(const analysis::NormalizedAnalysis& analysis,
                                                const InterventionDecision& decision,
                                                WorkingIntentState& state,
                                                std::string_view text);
    [[nodiscard]] static Status applyKeepOriginal(const analysis::NormalizedAnalysis& analysis,
                                                  const InterventionDecision& decision,
                                                  WorkingIntentState& state);
    [[nodiscard]] static Status applyAssumptionDecision(const analysis::NormalizedAnalysis& analysis,
                                                        const InterventionDecision& decision,
                                                        WorkingIntentState& state,
                                                        bool accepted);
    [[nodiscard]] static Status applySkip(const analysis::NormalizedAnalysis& analysis,
                                          const InterventionDecision& decision,
                                          WorkingIntentState& state);
    static void applyManualRequest(WorkingIntentState& state);
    static void recordPresented(const InterventionDecision& decision, WorkingIntentState& state);
};

[[nodiscard]] ActiveItemKind classifyItem(const analysis::NormalizedAnalysis& analysis, std::string_view itemId);

[[nodiscard]] const char* toString(InterventionVerdict verdict) noexcept;
[[nodiscard]] const char* toString(InterventionReasonCode reason) noexcept;
[[nodiscard]] const char* toString(ActiveItemKind kind) noexcept;

} // namespace roclint::core::intent
