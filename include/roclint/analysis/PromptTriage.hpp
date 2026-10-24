#pragma once

#include "roclint/core/Result.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace roclint::core::analysis {

inline constexpr double kPromptTriageThreshold = 0.70;
inline constexpr const char* kPromptTriageTemplateId = "julia.prompt_triage.categories.v2";
enum class PromptCategory { Action, Target, Rule, RuleConflict };
enum class PromptTriageRoute { Original, Clarify };
enum class TriageExecution { NotExecuted, Model, Mock };

// Four independent Boolean classifier heads, in Action/Target/Rule/RuleConflict order.
// Their scores are not a categorical probability distribution and must not be normalized together.
// The core derives the primary category and gate score as the greatest head score.
struct PromptTriage {
    std::string id;
    std::string triageHash;
    std::string sessionId;
    std::string revisionId;
    std::string revisionTextHash;
    std::uint64_t stateVersion{0};
    std::string requestGroupId;
    std::string attemptId;
    std::string provider{"julia_local"};
    std::string modelId;
    std::string modelRevision;
    std::string weightsSha256;
    std::string configurationId;
    std::string templateId{kPromptTriageTemplateId};
    TriageExecution execution{TriageExecution::NotExecuted};
    std::array<double, 4> categoryScores{};
    double ambiguityScore{0}; // Derived from categoryScores; not per-Granite-alternative evidence.
    PromptCategory primaryCategory{PromptCategory::Action};
    double latencyMs{0};
    bool operator==(const PromptTriage&) const = default;
};

[[nodiscard]] const char* toString(PromptCategory category) noexcept;
[[nodiscard]] std::string computePromptTriageHash(const PromptTriage& triage);
[[nodiscard]] Status validatePromptTriage(const PromptTriage& triage);
// Requires the model's input binding to match the frozen bytes. Derives only policy fields,
// seals the result, and rejects missing execution/provenance or invalid model scores.
[[nodiscard]] Result<PromptTriage> sealPromptTriage(PromptTriage triage, std::string_view frozenPrompt);
[[nodiscard]] Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage);

} // namespace roclint::core::analysis
