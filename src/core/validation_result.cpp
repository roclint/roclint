#include "roclint/core/validation_result.hpp"

namespace roclint::core {

const char* toString(ApplicabilityStatus status) noexcept {
    switch (status) {
        case ApplicabilityStatus::Active: return "Active";
        case ApplicabilityStatus::Inactive: return "Inactive";
        case ApplicabilityStatus::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* toString(ValidationOutcome outcome) noexcept {
    switch (outcome) {
        case ValidationOutcome::Pass: return "Pass";
        case ValidationOutcome::Fail: return "Fail";
        case ValidationOutcome::Unresolved: return "Unresolved";
    }
    return "Unresolved";
}

Result<ApplicabilityStatus> parseApplicabilityStatus(std::string_view text) noexcept {
    if (text == "Active") return ApplicabilityStatus::Active;
    if (text == "Inactive") return ApplicabilityStatus::Inactive;
    if (text == "Unknown") return ApplicabilityStatus::Unknown;
    return makeError(ErrorCode::InvalidInput, "Unknown ApplicabilityStatus: " + std::string(text));
}

Result<ValidationOutcome> parseValidationOutcome(std::string_view text) noexcept {
    if (text == "Pass") return ValidationOutcome::Pass;
    if (text == "Fail") return ValidationOutcome::Fail;
    if (text == "Unresolved") return ValidationOutcome::Unresolved;
    return makeError(ErrorCode::InvalidInput, "Unknown ValidationOutcome: " + std::string(text));
}

json::Value toJson(const ValidationFinding& finding) {
    auto obj = json::Value::object();
    obj.set("constraint_id", json::Value::string(finding.constraintId));
    obj.set("explanation", json::Value::string(finding.explanation));
    return obj;
}

Result<ValidationFinding> validationFindingFromJson(const json::Value& val) {
    if (!val.isObject()) {
        return makeError(ErrorCode::InvalidInput, "ValidationFinding JSON must be an object");
    }
    const auto* idVal = val.find("constraint_id");
    const auto* expVal = val.find("explanation");
    if (!idVal || !idVal->isString() ||
        !expVal || !expVal->isString()) {
        return makeError(ErrorCode::InvalidInput, "Missing or invalid ValidationFinding fields");
    }
    ValidationFinding finding;
    finding.constraintId = idVal->asString();
    finding.explanation = expVal->asString();
    return finding;
}

json::Value toJson(const ValidationResult& result) {
    auto obj = json::Value::object();
    obj.set("outcome", json::Value::string(toString(result.outcome)));

    auto violationsArr = json::Value::array();
    for (const auto& finding : result.violations) {
        violationsArr.push(toJson(finding));
    }
    obj.set("violations", std::move(violationsArr));

    auto unresolvedArr = json::Value::array();
    for (const auto& finding : result.unresolved) {
        unresolvedArr.push(toJson(finding));
    }
    obj.set("unresolved", std::move(unresolvedArr));

    return obj;
}

Result<ValidationResult> validationResultFromJson(const json::Value& val) {
    if (!val.isObject()) {
        return makeError(ErrorCode::InvalidInput, "ValidationResult JSON must be an object");
    }
    const auto* outcomeVal = val.find("outcome");
    const auto* violationsVal = val.find("violations");
    const auto* unresolvedVal = val.find("unresolved");

    if (!outcomeVal || !outcomeVal->isString() ||
        !violationsVal || !violationsVal->isArray() ||
        !unresolvedVal || !unresolvedVal->isArray()) {
        return makeError(ErrorCode::InvalidInput, "Missing or invalid ValidationResult fields");
    }

    auto outcome = parseValidationOutcome(outcomeVal->asString());
    if (!outcome.ok()) return outcome.error();

    ValidationResult res;
    res.outcome = outcome.value();

    for (const auto& itemVal : violationsVal->asArray()) {
        auto findingRes = validationFindingFromJson(itemVal);
        if (!findingRes.ok()) return findingRes.error();
        res.violations.push_back(std::move(findingRes.value()));
    }

    for (const auto& itemVal : unresolvedVal->asArray()) {
        auto findingRes = validationFindingFromJson(itemVal);
        if (!findingRes.ok()) return findingRes.error();
        res.unresolved.push_back(std::move(findingRes.value()));
    }

    return res;
}

} // namespace roclint::core
