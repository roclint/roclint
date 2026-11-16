#pragma once

#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/AnalysisSnapshot.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::intent {

enum class IntentResolutionKind {
    SelectedAlternative,
    CustomText,
    UseOriginal,
    AcceptedAssumption,
    DeclinedAssumption
};

struct ConfirmedItem {
    std::string itemId{};
    IntentResolutionKind kind{IntentResolutionKind::SelectedAlternative};
    std::string chosenAlternativeId{}; // SelectedAlternative only
    std::string chosenValue{};         // Human-readable wording of the decision
    std::string customText{};          // CustomText only
    std::string rationale{};

    bool operator==(const ConfirmedItem&) const = default;
};

// An explicit interview decision that is not part of the sealed analysis snapshot
// (model follow-up question or review finding). The host records the user's raw
// answer; it is included as an authoritative confirmed decision for the rewrite.
struct SupplementalDecision {
    std::string itemId{};
    std::string text{};        // Verbatim user answer / selected proposal wording
    std::string rationale{};
    std::string provenance{};  // "interview_followup" | "review_finding"

    bool operator==(const SupplementalDecision&) const = default;
};

// C06 intent version: bound to one revision and one sealed analysis snapshot.
struct ConfirmedIntent {
    std::string id{};          // intent_<intentHash>
    std::string intentHash{};
    std::string sessionId{};
    std::string baseRevisionId{};
    std::string snapshotId{};
    std::string snapshotHash{};
    std::vector<std::string> visibleItemIds{};
    std::vector<ConfirmedItem> items{};
    std::vector<std::string> unresolvedItemIds{};
    std::vector<std::string> rejectedItemIds{};
    std::int64_t createdAtMs{0};

    [[nodiscard]] bool hasDecisionFor(std::string_view itemId) const;
    [[nodiscard]] std::optional<ConfirmedItem> getDecision(std::string_view itemId) const;

    bool operator==(const ConfirmedIntent&) const = default;
};

class IntentProvenanceBuilder {
public:
    [[nodiscard]] static std::string computeIntentHash(const ConfirmedIntent& intent);

    // Builds the intent strictly from explicit user actions recorded in the working state.
    // Material items without an explicit resolution are listed as unresolved, never invented.
    // Supplemental decisions are explicit interview answers captured by the host; they are
    // appended verbatim and never inferred. A snapshot item with the same ID wins.
    [[nodiscard]] static Result<ConfirmedIntent> buildConfirmedIntent(const analysis::AnalysisSnapshot& snapshot,
                                                                      const analysis::OverallPolicyResult& policy,
                                                                      const WorkingIntentState& state,
                                                                      std::int64_t createdAtMs,
                                                                      const std::vector<SupplementalDecision>& supplemental = {});

    [[nodiscard]] static Status isIntentValid(const ConfirmedIntent& intent,
                                              std::string_view currentRevisionId,
                                              RevisionStatus revisionStatus,
                                              const analysis::AnalysisSnapshot& currentSnapshot);
};

[[nodiscard]] const char* toString(IntentResolutionKind kind) noexcept;

} // namespace roclint::core::intent
