#pragma once

#include "roclint/core/Json.hpp"
#include "roclint/core/Result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core {

enum class ApplicabilityStatus : std::uint8_t {
    Active,
    Inactive,
    Unknown
};

enum class ValidationOutcome : std::uint8_t {
    Pass,
    Fail,
    Unresolved
};

enum class AuditExecutionStatus : std::uint8_t {
    Completed,
    Unavailable,
    Malformed,
    DeterministicOnly
};

struct ValidationFinding {
    std::string constraintId;
    std::string explanation;

    bool operator==(const ValidationFinding&) const = default;
};

struct ValidationResult {
    ValidationOutcome outcome = ValidationOutcome::Unresolved;
    AuditExecutionStatus executionStatus = AuditExecutionStatus::Completed;

    std::vector<ValidationFinding> violations;
    std::vector<ValidationFinding> unresolved;

    bool operator==(const ValidationResult&) const = default;
};

[[nodiscard]] const char* toString(ApplicabilityStatus status) noexcept;
[[nodiscard]] const char* toString(ValidationOutcome outcome) noexcept;
[[nodiscard]] const char* toString(AuditExecutionStatus status) noexcept;

[[nodiscard]] Result<ApplicabilityStatus> parseApplicabilityStatus(std::string_view text) noexcept;
[[nodiscard]] Result<ValidationOutcome> parseValidationOutcome(std::string_view text) noexcept;
[[nodiscard]] Result<AuditExecutionStatus> parseAuditExecutionStatus(std::string_view text) noexcept;

[[nodiscard]] json::Value toJson(const ValidationFinding& finding);
[[nodiscard]] Result<ValidationFinding> validationFindingFromJson(const json::Value& val);

[[nodiscard]] json::Value toJson(const ValidationResult& result);
[[nodiscard]] Result<ValidationResult> validationResultFromJson(const json::Value& val);

} // namespace roclint::core
