#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/constraint_reconciler.hpp"
#include "roclint/core/scoped_constraint.hpp"
#include "roclint/core/validation_result.hpp"
#include "roclint/review/ComprehensionReview.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::bridge {

inline constexpr std::size_t kMaxAuditResponseBytes = 64 * 1024;
inline constexpr std::size_t kMaxComprehensionReviewResponseBytes = 64 * 1024;

struct GoldfishAuditRequest {
    // Reserved for semantic retention of originalPrompt's task. Callers must not
    // reuse this ID for an independently supplied constraint.
    static constexpr std::string_view originalTaskConstraintId = "original_task";
    std::string originalPrompt{};
    std::string candidateRewrite{};
    std::vector<core::ScopedConstraint> constraints{};
    std::string sessionId{};
};

// Checks if a conflict description is a false conflict (e.g. "no conflicting requirements identified", "n/a", etc.)
[[nodiscard]] bool isFalseConflict(std::string_view description) noexcept;

struct GoldfishDiscoveryAuditRequest {
    std::string originalPrompt{};
    core::analysis::NormalizedAnalysis proposedAnalysis{};
    std::string sessionId{};
};

struct GoldfishDiscoveryFinding {
    std::string itemId{};
    std::string explanation{};
};

struct GoldfishDiscoveryAuditResult {
    core::ValidationOutcome outcome{core::ValidationOutcome::Pass};
    core::AuditExecutionStatus executionStatus{core::AuditExecutionStatus::Completed};
    std::vector<GoldfishDiscoveryFinding> violations{};
    std::vector<GoldfishDiscoveryFinding> unresolved{};
};

// Builds the cold, zero-memory adversarial audit prompt for Granite (contract: granite.goldfish_audit.v2).
[[nodiscard]] std::string buildGoldfishAuditPrompt(const GoldfishAuditRequest& request);

// Returns the JSON schema for granite.goldfish_audit.v2 responses.
[[nodiscard]] const std::string& goldfishAuditResponseSchema();

// Parses the model's JSON response envelope into a ValidationResult.
[[nodiscard]] core::Result<core::ValidationResult> parseGoldfishAuditResponse(std::string_view modelOutput);

// Builds the discovery audit prompt (contract: granite.goldfish_discovery_audit.v1)
[[nodiscard]] std::string buildGoldfishDiscoveryAuditPrompt(const GoldfishDiscoveryAuditRequest& request);

// Returns the JSON schema for granite.goldfish_discovery_audit.v1 responses.
[[nodiscard]] const std::string& goldfishDiscoveryAuditResponseSchema();

// Parses the model's discovery audit JSON response.
[[nodiscard]] core::Result<GoldfishDiscoveryAuditResult> parseGoldfishDiscoveryAuditResponse(std::string_view modelOutput);

struct GoldfishClarificationAuditRequest {
    std::string originalPrompt{};
    std::string question{};
    std::string userAnswer{};
    std::vector<std::string> offeredAlternatives{};
    std::vector<std::pair<std::string, std::string>> previousAnswers{};
    std::vector<core::ScopedConstraint> constraints{};
    std::string sessionId{};
};

struct GoldfishClarificationFinding {
    std::string aspect{}; // "faithfulness", "relevance", "compatibility"
    std::string explanation{};
};

struct GoldfishClarificationAuditResult {
    core::ValidationOutcome outcome{core::ValidationOutcome::Pass};
    core::AuditExecutionStatus executionStatus{core::AuditExecutionStatus::Completed};
    bool faithful{true};
    bool relevant{true};
    bool compatible{true};
    std::vector<GoldfishClarificationFinding> violations{};
    std::vector<GoldfishClarificationFinding> unresolved{};
    std::string feedback{};
};

// Builds the clarification audit prompt (contract: granite.goldfish_clarification_audit.v1)
[[nodiscard]] std::string buildGoldfishClarificationAuditPrompt(const GoldfishClarificationAuditRequest& request);

// Returns the JSON schema for granite.goldfish_clarification_audit.v1 responses.
[[nodiscard]] const std::string& goldfishClarificationAuditResponseSchema();

// Parses the model's clarification audit JSON response.
[[nodiscard]] core::Result<GoldfishClarificationAuditResult> parseGoldfishClarificationAuditResponse(std::string_view modelOutput);

struct GoldfishDiscoveryRepairResult {
    core::analysis::NormalizedAnalysis analysis{};
    GoldfishDiscoveryAuditResult auditVerdict{};
    int attemptsExecuted{1};
    bool repairedSuccessfully{false};
};

class GraniteGoldfishAdapter {
public:
    explicit GraniteGoldfishAdapter(core::ConstraintReconciler reconciler = core::ConstraintReconciler{})
        : reconciler_(std::move(reconciler)) {}

    [[nodiscard]] core::ConstraintReconciler& reconciler() noexcept { return reconciler_; }
    [[nodiscard]] const core::ConstraintReconciler& reconciler() const noexcept { return reconciler_; }

    // Performs a cold audit of the candidate rewrite against verified constraints.
    // Deterministic C++ rules are evaluated first. If a deterministic violation is found,
    // the audit returns ValidationOutcome::Fail immediately.
    // If transport is provided and deterministic check passes or is unresolved,
    // invokes the model transport and merges findings.
    [[nodiscard]] core::ValidationResult audit(
        const GoldfishAuditRequest& request,
        const GraniteTransport& transport = nullptr) const;

    // Stage-appropriate audit for discovery analysis (questions, alternatives, conflicts).
    // Checks for false conflicts, questions on settled requirements,
    // duplicate/ungrounded alternatives, and violations of stated constraints.
    [[nodiscard]] GoldfishDiscoveryAuditResult auditDiscovery(
        const GoldfishDiscoveryAuditRequest& request,
        const GraniteTransport& transport = nullptr) const;

    // Stage-appropriate audit for clarification answers (faithfulness to user's exact answer,
    // relevance to question, compatibility with original prompt and prior decisions).
    [[nodiscard]] GoldfishClarificationAuditResult auditClarification(
        const GoldfishClarificationAuditRequest& request,
        const GraniteTransport& transport = nullptr) const;

    // Fresh-context comprehension/constraint review for substantial rewrites. The prompt sees
    // only the candidate, original source, authoritative confirmed decisions, source-linked
    // constraints, verbatim source evidence and task kind. One transport call at most, no repair
    // loop, and no side effects. Deterministic prechecks can add grounded negative-boundary
    // findings but cannot create requirements. The free function in
    // roclint/review/ComprehensionReview.hpp is the canonical entry point; this member uses the
    // adapter's registered applicability predicates.
    [[nodiscard]] ComprehensionReviewResult reviewComprehension(
        const ComprehensionReviewRequest& request,
        const ComprehensionReviewTransport& transport = {}) const;

    // Repairs/prunes proposed analysis using audit findings (e.g. removing false conflicts).
    [[nodiscard]] static core::analysis::NormalizedAnalysis repairDiscovery(
        const core::analysis::NormalizedAnalysis& original,
        const GoldfishDiscoveryAuditResult& auditResult);

    // Produces a replacement analysis from actionable audit findings. The host supplies this when
    // it can ask the discovery model again; the adapter itself never invents replacements.
    using DiscoveryRegeneration = std::function<core::Result<core::analysis::NormalizedAnalysis>(
        const GoldfishDiscoveryAuditResult&)>;

    // Bounded discovery repair loop: prunes/repairs discovery analysis with maxRetries and timeBudgetMs.
    // When regeneration is supplied it is used only after deterministic pruning still leaves
    // violations. Repeated candidates stop the loop immediately. timeBudgetMs is checked before
    // every model call; callers must also enforce their own end-to-end deadline for the whole
    // discovery stage, because a synchronous transport call cannot be interrupted.
    [[nodiscard]] GoldfishDiscoveryRepairResult correctDiscoveryWithRepairLoop(
        const GoldfishDiscoveryAuditRequest& request,
        const GraniteTransport& auditTransport = nullptr,
        int maxRetries = 2,
        std::int64_t timeBudgetMs = 25000,
        const DiscoveryRegeneration& regeneration = {}) const;

private:
    core::ConstraintReconciler reconciler_;
};

} // namespace roclint::bridge
