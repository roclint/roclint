#pragma once

#include "roclint/core/Result.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace roclint::core::analysis {

// v1 compatibility only: used exclusively for reconstructing historical v1 hashes.
// Do not use this constant in new routing or policy code.
inline constexpr double kPromptTriageThreshold = 0.70;
inline constexpr const char* kPromptTriageTemplateId = "julia.prompt_triage.categories.v2";

// Version of the cryptographic seal.
// V1: bakes kPromptTriageThreshold into the hash domain (legacy; read-only for historical records).
// V2: commits only to model evidence (scores, identity, latency, prompt binding); threshold-independent.
enum class TriageHashVersion { V1 = 1, V2 = 2 };

enum class PromptCategory { Action, Target, Rule, RuleConflict };

// Three-tier routing outcome.
// Original:       All head scores safely below per-head thresholds. Granite skipped.
// Clarify:        At least one head score at or above its threshold. Granite invoked.
// UncertainBuffer: No head is clearly Clarify, but at least one is within the dead band
//                  [threshold[i] - uncertaintyDelta, threshold[i]). Non-blocking advisory;
//                  the original wording is retained and explicit re-analyze invokes Granite.
enum class PromptTriageRoute { Original, Clarify, UncertainBuffer };

enum class TriageExecution { NotExecuted, Model, Mock };

// Per-head routing policy. Passed to routePromptTriage(); never enters the sealed record.
// Thresholds are indexed [ACTION=0, TARGET=1, RULE=2, RULE_CONFLICT=3].
struct TriagePolicy {
    // Route to Clarify when categoryScores[i] >= thresholds[i].
    // RULE_CONFLICT is tighter (0.60) because missed conflicts have higher consequence.
    std::array<double, 4> thresholds{0.70, 0.70, 0.70, 0.60};

    // Dead-band half-width δ. A score in [thresholds[i] - uncertaintyDelta, thresholds[i])
    // triggers UncertainBuffer when no head is in the Clarify zone.
    double uncertaintyDelta{0.08};

    // Human-readable policy identifier for logging and status messages.
    std::string policyId{"roclint.triage_policy.v1"};

    [[nodiscard]] static TriagePolicy defaults() noexcept { return {}; }
};

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
    TriageHashVersion hashVersion{TriageHashVersion::V2}; // V1 for historical records; V2 for new seals.
    bool operator==(const PromptTriage&) const = default;

    // Single source of truth for evidence field iteration across hashing and serialization
    template <typename Visitor>
    void visitEvidence(Visitor& v) const {
        v("session_id", sessionId);
        v("revision_id", revisionId);
        v("text_hash", revisionTextHash);
        v("state_version", stateVersion);
        v("request_group_id", requestGroupId);
        v("attempt_id", attemptId);
        v("provider", provider);
        v("model_id", modelId);
        v("model_revision", modelRevision);
        v("weights_sha256", weightsSha256);
        v("configuration_id", configurationId);
        v("template_id", templateId);
        v("execution", execution);
        v("category_scores", categoryScores);
        v("ambiguity", ambiguityScore);
        v("primary_category", primaryCategory);
        v("latency_ms", latencyMs);
    }

    template <typename Visitor>
    void visitEvidence(Visitor& v) {
        v("session_id", sessionId);
        v("revision_id", revisionId);
        v("text_hash", revisionTextHash);
        v("state_version", stateVersion);
        v("request_group_id", requestGroupId);
        v("attempt_id", attemptId);
        v("provider", provider);
        v("model_id", modelId);
        v("model_revision", modelRevision);
        v("weights_sha256", weightsSha256);
        v("configuration_id", configurationId);
        v("template_id", templateId);
        v("execution", execution);
        v("category_scores", categoryScores);
        v("ambiguity", ambiguityScore);
        v("primary_category", primaryCategory);
        v("latency_ms", latencyMs);
    }
};

[[nodiscard]] const char* toString(PromptCategory category) noexcept;
[[nodiscard]] const char* toString(PromptTriageRoute route) noexcept;
[[nodiscard]] std::string computePromptTriageHash(const PromptTriage& triage);
[[nodiscard]] Status validatePromptTriage(const PromptTriage& triage);
// Requires the model's input binding to match the frozen bytes. Derives only policy fields,
// seals the result, and rejects missing execution/provenance or invalid model scores.
[[nodiscard]] Result<PromptTriage> sealPromptTriage(PromptTriage triage, std::string_view frozenPrompt);
// Backwards-compatible overload: applies TriagePolicy::defaults().
[[nodiscard]] Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage);
// Policy-aware overload: per-head thresholds and uncertainty dead band from policy.
[[nodiscard]] Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage, const TriagePolicy& policy);

} // namespace roclint::core::analysis
