#pragma once

#include <cstddef>

namespace roclint::core::analysis {

// Product decisions are separate from the classifier's unchanged evidence.
enum class ReviewTrigger { Automatic, ExplicitReview };
enum class AssistanceKind { None, Clarification, Creative, Refinement, Decomposition };

inline constexpr std::size_t kMaxOptionalChoiceGroups = 2;

struct ReviewPolicy {
    ReviewTrigger trigger{ReviewTrigger::Automatic};
    AssistanceKind assistance{AssistanceKind::None};
    bool operator==(const ReviewPolicy&) const = default;
};

[[nodiscard]] constexpr bool isOptionalAssistance(AssistanceKind kind) {
    return kind == AssistanceKind::Creative || kind == AssistanceKind::Refinement ||
           kind == AssistanceKind::Decomposition;
}

} // namespace roclint::core::analysis
