#pragma once

#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/constraint_reconciler.hpp"
#include "roclint/core/scoped_constraint.hpp"
#include "roclint/core/validation_result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::bridge {

inline constexpr std::size_t kMaxAuditResponseBytes = 64 * 1024;

struct GoldfishAuditRequest {
    std::string originalPrompt{};
    std::string candidateRewrite{};
    std::vector<core::ScopedConstraint> constraints{};
    std::string sessionId{};
};

// Builds the cold, zero-memory adversarial audit prompt for Granite (contract: granite.goldfish_audit.v1).
[[nodiscard]] std::string buildGoldfishAuditPrompt(const GoldfishAuditRequest& request);

// Returns the JSON schema for granite.goldfish_audit.v1 responses.
[[nodiscard]] const std::string& goldfishAuditResponseSchema();

// Parses the model's JSON response envelope into a ValidationResult.
[[nodiscard]] core::Result<core::ValidationResult> parseGoldfishAuditResponse(std::string_view modelOutput);

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

private:
    core::ConstraintReconciler reconciler_;
};

} // namespace roclint::bridge
