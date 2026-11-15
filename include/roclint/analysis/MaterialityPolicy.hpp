#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/analysis/SourceSpan.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::analysis {

enum class PolicyVerdict {
    EligibleForChoice,
    NeedsManualReview,
    Rejected
};

enum class ReasonCode {
    Ok,
    InsufficientAlternatives,
    ExcessiveAlternatives,
    DuplicateAlternativeLabels,
    UngroundedMateriality,
    NotMaterial,
    InvalidUtf8SpanBoundary,
    UnsafeSpanOverlap,
    DirectConflictPresent,
    ExceedsAssumptionCap,
    EmptyPromptBuffer
};

struct GroupEvaluationResult {
    std::string groupId{};
    PolicyVerdict verdict{PolicyVerdict::Rejected};
    ReasonCode reasonCode{ReasonCode::Ok};
    std::string diagnosticMessage{};

    [[nodiscard]] bool isEligible() const noexcept {
        return verdict == PolicyVerdict::EligibleForChoice;
    }
};

struct OverallPolicyResult {
    PolicyVerdict overallVerdict{PolicyVerdict::Rejected};
    ReasonCode primaryReasonCode{ReasonCode::Ok};
    std::vector<GroupEvaluationResult> groupResults{};

    std::size_t assumptionCount{0};
    std::size_t conflictCount{0};
    std::size_t blockingConflictCount{0};
    bool hasCrossGroupOverlap{false};

    [[nodiscard]] bool isEligible() const noexcept {
        return overallVerdict == PolicyVerdict::EligibleForChoice;
    }

    // Returns nullptr when no verdict exists for the group.
    [[nodiscard]] const GroupEvaluationResult* findGroup(std::string_view groupId) const noexcept;
};

// Human-authored materiality policy (C03). Changing any rule requires bumping kPolicyVersion.
class MaterialityPolicyV1 {
public:
    // v2: overlapping evidence is excused only by explicit, distinct decision dimensions.
    static constexpr const char* kPolicyVersion = "materiality_policy_v2";
    static constexpr std::size_t kMaxAssumptionBudget = 3;
    static constexpr std::size_t kMinAlternatives = 2;
    static constexpr std::size_t kMaxAlternatives = 3;

    [[nodiscard]] static GroupEvaluationResult evaluateGroup(const ChoiceGroup& group, std::string_view promptBuffer);

    [[nodiscard]] static OverallPolicyResult evaluatePrompt(const NormalizedAnalysis& analysis,
                                                            std::string_view promptBuffer);

    [[nodiscard]] static bool spansOverlap(const SourceSpan& a, const SourceSpan& b) noexcept;
};

[[nodiscard]] const char* toString(ReasonCode code) noexcept;
[[nodiscard]] const char* toString(PolicyVerdict verdict) noexcept;

} // namespace roclint::core::analysis
