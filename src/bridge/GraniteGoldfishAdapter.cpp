#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/ModelResponseEnvelope.hpp"
#include "roclint/core/Json.hpp"

#include <algorithm>
#include <cctype>

namespace roclint::bridge {

namespace {

[[nodiscard]] std::string toLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

std::string buildGoldfishAuditPrompt(const GoldfishAuditRequest& request) {
    std::string out;
    out.reserve(request.originalPrompt.size() + request.candidateRewrite.size() + 2048);

    out += "<instructions>\n";
    out += "Instruction contract: granite.goldfish_audit.v1\n";
    out += "You are the Goldfish Auditor, an independent, cold, zero-memory adversarial validation pass.\n";
    out += "You have NO memory of prior conversations, design discussions, or user context.\n";
    out += "Your role is to strictly verify whether the CANDIDATE REWRITE satisfies all HARD requirements "
           "and respects all NEGATIVE BOUNDARY PROHIBITIONS from the ORIGINAL PROMPT and VERIFIED CONSTRAINTS.\n\n";
    out += "Rules for audit:\n";
    out += "1. Hard constraints (Enforcement: Hard) MUST be satisfied in the candidate rewrite. "
           "If a hard requirement is omitted, contradicted, or weakened, report a violation.\n";
    out += "2. Negative prohibitions (Polarity: Prohibit) MUST NOT be breached. "
           "If a constraint forbids modifying or touching an aspect, and the candidate rewrite alters it, "
           "report a violation.\n";
    out += "3. Inferred / Advisory choices (Enforcement: Advisory, Origin: Model) have ZERO VETO POWER. "
           "If the candidate rewrite omits an advisory option or chooses an alternate aesthetic/implementation, "
           "this is NOT a violation.\n";
    out += "4. Active domain invariants (Origin: Domain) must be respected if their conditions apply.\n";
    out += "5. Epistemic humility: If the candidate rewrite text does not contain enough information to determine "
           "whether a constraint is satisfied, report it under \"unresolved\". Never guess, invent facts, or "
           "assume compliance without evidence.\n";
    out += "6. Reply with exactly one JSON object: {\"outcome\": \"Pass\" | \"Fail\" | \"Unresolved\", "
           "\"violations\": [{\"constraint_id\": string, \"explanation\": string}], "
           "\"unresolved\": [{\"constraint_id\": string, \"explanation\": string}]}.\n";
    out += "</instructions>\n";

    out += "<original_prompt>\n";
    out += xmlEscape(request.originalPrompt);
    out += "\n</original_prompt>\n";

    out += "<candidate_rewrite>\n";
    out += xmlEscape(request.candidateRewrite);
    out += "\n</candidate_rewrite>\n";

    out += "<verified_constraints>\n";
    for (const auto& c : request.constraints) {
        out += "<constraint id=\"" + xmlEscape(c.id) + "\" origin=\"" +
               xmlEscape(core::toString(c.origin)) + "\" derivation=\"" +
               xmlEscape(core::toString(c.derivation)) + "\" enforcement=\"" +
               xmlEscape(core::toString(c.enforcement)) + "\" polarity=\"" +
               xmlEscape(core::toString(c.polarity)) + "\" aspect=\"" +
               xmlEscape(c.aspect) + "\">\n";
        out += "Rule: " + xmlEscape(c.rule) + "\n";
        if (c.appliesWhen.has_value()) {
            out += "AppliesWhen: " + xmlEscape(*c.appliesWhen) + "\n";
        }
        if (c.authorityRef.has_value()) {
            out += "Authority: " + xmlEscape(*c.authorityRef) + "\n";
        }
        out += "</constraint>\n";
    }
    out += "</verified_constraints>\n";

    return out;
}

const std::string& goldfishAuditResponseSchema() {
    static const std::string kSchema = R"JSON({"type":"object","properties":{"outcome":{"type":"string","enum":["Pass","Fail","Unresolved"]},"violations":{"type":"array","items":{"type":"object","properties":{"constraint_id":{"type":"string"},"explanation":{"type":"string"}},"required":["constraint_id","explanation"],"additionalProperties":false}},"unresolved":{"type":"array","items":{"type":"object","properties":{"constraint_id":{"type":"string"},"explanation":{"type":"string"}},"required":["constraint_id","explanation"],"additionalProperties":false}}},"required":["outcome","violations","unresolved"],"additionalProperties":false})JSON";
    return kSchema;
}

core::Result<core::ValidationResult> parseGoldfishAuditResponse(std::string_view modelOutput) {
    if (modelOutput.size() > kMaxAuditResponseBytes) {
        return core::makeError(core::ErrorCode::InvalidInput, "Audit response exceeds the 64 KB limit");
    }

    auto envelope = normalizeModelResponseEnvelope(modelOutput, kMaxAuditResponseBytes);
    if (!envelope.ok()) {
        return envelope.error();
    }

    auto parsed = core::json::parse(envelope.value(), core::json::ParseLimits{kMaxAuditResponseBytes, 8});
    if (!parsed.ok()) {
        return parsed.error();
    }

    const auto& root = parsed.value();
    if (!root.isObject()) {
        return core::makeError(core::ErrorCode::InvalidInput, "Audit response root must be a JSON object");
    }

    // Accept "outcome", "verdict", or "status" (mandatory string)
    const core::json::Value* outcomeVal = nullptr;
    if (const auto* val = root.find("outcome"); val != nullptr) {
        outcomeVal = val;
    } else if (const auto* val2 = root.find("verdict"); val2 != nullptr) {
        outcomeVal = val2;
    } else if (const auto* val3 = root.find("status"); val3 != nullptr) {
        outcomeVal = val3;
    }

    if (outcomeVal == nullptr) {
        return core::makeError(core::ErrorCode::InvalidInput, "Audit response missing required outcome field");
    }
    if (!outcomeVal->isString()) {
        return core::makeError(core::ErrorCode::InvalidInput, "Audit response outcome field must be a string");
    }

    const std::string outcomeRaw = outcomeVal->asString();
    if (outcomeRaw.empty()) {
        return core::makeError(core::ErrorCode::InvalidInput, "Audit response outcome field cannot be empty");
    }

    std::string outcomeStr = toLowerAscii(outcomeRaw);
    core::ValidationResult result;
    if (outcomeStr == "fail") {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (outcomeStr == "unresolved") {
        result.outcome = core::ValidationOutcome::Unresolved;
    } else if (outcomeStr == "pass") {
        result.outcome = core::ValidationOutcome::Pass;
    } else {
        return core::makeError(core::ErrorCode::InvalidInput, "Unknown audit outcome: " + outcomeRaw);
    }

    // Helper to validate and extract required string field with allowed aliases
    auto requireFieldString = [](const core::json::Value& obj,
                                 std::initializer_list<const char*> keys,
                                 const char* fieldName) -> core::Result<std::string> {
        const core::json::Value* matched = nullptr;
        for (const char* key : keys) {
            if (const auto* v = obj.find(key); v != nullptr) {
                matched = v;
                break;
            }
        }
        if (matched == nullptr) {
            return core::makeError(core::ErrorCode::InvalidInput,
                                   std::string("Finding item missing required field: ") + fieldName);
        }
        if (!matched->isString()) {
            return core::makeError(core::ErrorCode::InvalidInput,
                                   std::string("Finding item field '") + fieldName + "' must be a string");
        }
        if (matched->asString().empty()) {
            return core::makeError(core::ErrorCode::InvalidInput,
                                   std::string("Finding item field '") + fieldName + "' cannot be empty");
        }
        return matched->asString();
    };

    // Mandatory violations array validation
    const auto* vArr = root.find("violations");
    if (vArr == nullptr) {
        return core::makeError(core::ErrorCode::InvalidInput, "Audit response missing required 'violations' array");
    }
    if (!vArr->isArray()) {
        return core::makeError(core::ErrorCode::InvalidInput, "'violations' must be an array");
    }
    for (const auto& item : vArr->asArray()) {
        if (!item.isObject()) {
            return core::makeError(core::ErrorCode::InvalidInput, "violations item must be a JSON object");
        }
        auto idRes = requireFieldString(item, {"constraint_id", "id"}, "constraint_id");
        if (!idRes.ok()) return idRes.error();

        auto expRes = requireFieldString(item, {"explanation", "reason", "description"}, "explanation");
        if (!expRes.ok()) return expRes.error();

        core::ValidationFinding finding;
        finding.constraintId = std::move(idRes).value();
        finding.explanation = std::move(expRes).value();
        result.violations.push_back(std::move(finding));
    }

    // Mandatory unresolved array validation
    const auto* uArr = root.find("unresolved");
    if (uArr == nullptr) {
        return core::makeError(core::ErrorCode::InvalidInput, "Audit response missing required 'unresolved' array");
    }
    if (!uArr->isArray()) {
        return core::makeError(core::ErrorCode::InvalidInput, "'unresolved' must be an array");
    }
    for (const auto& item : uArr->asArray()) {
        if (!item.isObject()) {
            return core::makeError(core::ErrorCode::InvalidInput, "unresolved item must be a JSON object");
        }
        auto idRes = requireFieldString(item, {"constraint_id", "id"}, "constraint_id");
        if (!idRes.ok()) return idRes.error();

        auto expRes = requireFieldString(item, {"explanation", "reason", "description"}, "explanation");
        if (!expRes.ok()) return expRes.error();

        core::ValidationFinding finding;
        finding.constraintId = std::move(idRes).value();
        finding.explanation = std::move(expRes).value();
        result.unresolved.push_back(std::move(finding));
    }

    // Honest reconciliation: violations force Fail; unresolved forces Unresolved (if not Fail)
    if (!result.violations.empty()) {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (!result.unresolved.empty()) {
        if (result.outcome == core::ValidationOutcome::Pass) {
            result.outcome = core::ValidationOutcome::Unresolved;
        }
    }

    return result;
}

core::ValidationResult GraniteGoldfishAdapter::audit(
    const GoldfishAuditRequest& request,
    const GraniteTransport& transport) const {

    // Step 1: Pre-gate with deterministic C++ reconciler
    auto reconcilerResult = reconciler_.reconcile(
        request.originalPrompt,
        request.constraints,
        request.candidateRewrite);

    // If deterministic rules already flag a violation, fail-fast without invoking the model
    if (reconcilerResult.outcome == core::ValidationOutcome::Fail) {
        reconcilerResult.executionStatus = core::AuditExecutionStatus::DeterministicOnly;
        return reconcilerResult;
    }

    // If no model transport is provided, return deterministic evaluation
    if (!transport) {
        reconcilerResult.executionStatus = core::AuditExecutionStatus::DeterministicOnly;
        return reconcilerResult;
    }

    // Step 2: Build adversarial zero-memory prompt and invoke transport
    const std::string prompt = buildGoldfishAuditPrompt(request);
    auto transportResult = transport(prompt);
    if (!transportResult.ok()) {
        // Model call failed (timeout or unavailable): graceful degradation to unresolved with provenance
        core::ValidationResult fallback = reconcilerResult;
        fallback.executionStatus = core::AuditExecutionStatus::Unavailable;
        fallback.unresolved.push_back(core::ValidationFinding{
            "goldfish_audit",
            "Model audit unavailable: " + transportResult.error().message
        });
        fallback.outcome = core::ValidationOutcome::Unresolved;
        return fallback;
    }

    // Step 3: Parse model audit response
    auto parsedAudit = parseGoldfishAuditResponse(transportResult.value());
    if (!parsedAudit.ok()) {
        // Model returned unparseable output; preserve deterministic checks and record unresolved finding
        core::ValidationResult fallback = reconcilerResult;
        fallback.executionStatus = core::AuditExecutionStatus::Malformed;
        fallback.unresolved.push_back(core::ValidationFinding{
            "goldfish_audit",
            "Model response could not be parsed: " + parsedAudit.error().message
        });
        fallback.outcome = core::ValidationOutcome::Unresolved;
        return fallback;
    }

    // Step 4: Authority and finding validation against frozen constraints (F04 + R05)
    std::unordered_map<std::string, const core::ScopedConstraint*> constraintMap;
    for (const auto& c : request.constraints) {
        constraintMap[c.id] = &c;
    }

    core::ValidationResult combined = reconcilerResult;
    combined.executionStatus = core::AuditExecutionStatus::Completed;

    const bool hadModelFailVerdict = (parsedAudit.value().outcome == core::ValidationOutcome::Fail);
    const bool hadModelUnresolvedVerdict = (parsedAudit.value().outcome == core::ValidationOutcome::Unresolved);
    const size_t modelViolationsCount = parsedAudit.value().violations.size();
    const size_t modelUnresolvedCount = parsedAudit.value().unresolved.size();

    size_t hardViolationsAccepted = 0;
    size_t hardUnresolvedAccepted = 0;
    size_t unknownFindingsAccepted = 0;
    size_t advisoryViolationsFiltered = 0;
    size_t advisoryUnresolvedFiltered = 0;
    size_t inactiveViolationsFiltered = 0;
    size_t inactiveUnresolvedFiltered = 0;

    for (const auto& mv : parsedAudit.value().violations) {
        auto it = constraintMap.find(mv.constraintId);
        if (it == constraintMap.end()) {
            // Unknown constraint ID: cannot be authoritative hard Fail, record as unresolved note to retain honest uncertainty
            bool exists = false;
            for (const auto& eu : combined.unresolved) {
                if (eu.constraintId == mv.constraintId) { exists = true; break; }
            }
            if (!exists) {
                combined.unresolved.push_back(core::ValidationFinding{
                    mv.constraintId.empty() ? "goldfish_audit" : mv.constraintId,
                    "Model reported violation for unknown constraint: " + mv.explanation
                });
                ++unknownFindingsAccepted;
            }
            continue;
        }

        const auto* c = it->second;
        // Advisory constraints have ZERO veto power
        if (c->enforcement == core::Enforcement::Advisory) {
            ++advisoryViolationsFiltered;
            continue;
        }

        // Check applicability for conditional constraints
        if (reconciler_.evaluateApplicability(*c, request.candidateRewrite) == core::ApplicabilityStatus::Inactive) {
            ++inactiveViolationsFiltered;
            continue;
        }

        bool exists = false;
        for (const auto& ev : combined.violations) {
            if (ev.constraintId == mv.constraintId) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            combined.violations.push_back(mv);
            ++hardViolationsAccepted;
        }
    }

    for (const auto& mu : parsedAudit.value().unresolved) {
        auto it = constraintMap.find(mu.constraintId);
        if (it == constraintMap.end()) {
            // Unknown constraint ID: record as unresolved finding to retain honest uncertainty
            bool exists = false;
            for (const auto& eu : combined.unresolved) {
                if (eu.constraintId == mu.constraintId) { exists = true; break; }
            }
            if (!exists) {
                combined.unresolved.push_back(mu);
                ++unknownFindingsAccepted;
            }
            continue;
        }

        const auto* c = it->second;
        // Advisory uncertainty must not gain veto power
        if (c->enforcement == core::Enforcement::Advisory) {
            ++advisoryUnresolvedFiltered;
            continue;
        }

        // Inactive conditional constraints must not block
        if (reconciler_.evaluateApplicability(*c, request.candidateRewrite) == core::ApplicabilityStatus::Inactive) {
            ++inactiveUnresolvedFiltered;
            continue;
        }

        bool exists = false;
        for (const auto& eu : combined.unresolved) {
            if (eu.constraintId == mu.constraintId) {
                exists = true;
                break;
            }
        }
        for (const auto& ev : combined.violations) {
            if (ev.constraintId == mu.constraintId) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            combined.unresolved.push_back(mu);
            ++hardUnresolvedAccepted;
        }
    }

    // Step 5: Final verdict calculation (F01, F04, R05)
    const size_t totalReported = modelViolationsCount + modelUnresolvedCount;
    const size_t totalFiltered = advisoryViolationsFiltered + advisoryUnresolvedFiltered +
                                 inactiveViolationsFiltered + inactiveUnresolvedFiltered;

    if (!combined.violations.empty()) {
        combined.outcome = core::ValidationOutcome::Fail;
    } else if (!combined.unresolved.empty()) {
        combined.outcome = core::ValidationOutcome::Unresolved;
    } else if (hadModelFailVerdict) {
        if (totalReported > 0 && totalFiltered == totalReported) {
            // All findings reported were Advisory or Inactive; they carry zero veto power
            combined.outcome = core::ValidationOutcome::Pass;
        } else {
            // Explicit Fail with no findings or unexplained findings
            combined.outcome = core::ValidationOutcome::Fail;
            combined.violations.push_back(core::ValidationFinding{
                "goldfish_audit",
                "Model audit returned an explicit Fail verdict."
            });
        }
    } else if (hadModelUnresolvedVerdict) {
        if (totalReported > 0 && totalFiltered == totalReported) {
            // All findings reported were Advisory or Inactive; Advisory uncertainty has zero veto power
            combined.outcome = core::ValidationOutcome::Pass;
        } else {
            // Explicit Unresolved verdict with no specific findings (unexplained)
            combined.outcome = core::ValidationOutcome::Unresolved;
            combined.unresolved.push_back(core::ValidationFinding{
                "goldfish_audit",
                "Model audit returned an explicit Unresolved verdict."
            });
        }
    } else {
        combined.outcome = core::ValidationOutcome::Pass;
    }

    return combined;
}

} // namespace roclint::bridge
