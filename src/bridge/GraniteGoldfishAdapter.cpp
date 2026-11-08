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

    // Accept "outcome", "verdict", or "status"
    std::string outcomeStr = "pass";
    if (const auto* val = root.find("outcome"); val && val->isString()) {
        outcomeStr = toLowerAscii(val->asString());
    } else if (const auto* val2 = root.find("verdict"); val2 && val2->isString()) {
        outcomeStr = toLowerAscii(val2->asString());
    } else if (const auto* val3 = root.find("status"); val3 && val3->isString()) {
        outcomeStr = toLowerAscii(val3->asString());
    }

    core::ValidationResult result;
    if (outcomeStr == "fail") {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (outcomeStr == "unresolved") {
        result.outcome = core::ValidationOutcome::Unresolved;
    } else {
        result.outcome = core::ValidationOutcome::Pass;
    }

    // Helper for finding string field with fallbacks
    auto extractField = [](const core::json::Value& obj, std::initializer_list<const char*> keys) -> std::string {
        for (const char* key : keys) {
            if (const auto* v = obj.find(key); v && v->isString()) {
                return v->asString();
            }
        }
        return {};
    };

    // Parse violations array
    if (const auto* vArr = root.find("violations"); vArr && vArr->isArray()) {
        for (const auto& item : vArr->asArray()) {
            if (item.isObject()) {
                core::ValidationFinding finding;
                finding.constraintId = extractField(item, {"constraint_id", "id"});
                finding.explanation = extractField(item, {"explanation", "reason", "description"});
                if (!finding.constraintId.empty() || !finding.explanation.empty()) {
                    result.violations.push_back(std::move(finding));
                }
            }
        }
    }

    // Parse unresolved array
    if (const auto* uArr = root.find("unresolved"); uArr && uArr->isArray()) {
        for (const auto& item : uArr->asArray()) {
            if (item.isObject()) {
                core::ValidationFinding finding;
                finding.constraintId = extractField(item, {"constraint_id", "id"});
                finding.explanation = extractField(item, {"explanation", "reason", "description"});
                if (!finding.constraintId.empty() || !finding.explanation.empty()) {
                    result.unresolved.push_back(std::move(finding));
                }
            }
        }
    }

    // Honest reconciliation: violations force Fail; unresolved forces Unresolved (if no violations)
    if (!result.violations.empty()) {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (!result.unresolved.empty()) {
        result.outcome = core::ValidationOutcome::Unresolved;
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
        return reconcilerResult;
    }

    // If no model transport is provided, return deterministic evaluation
    if (!transport) {
        return reconcilerResult;
    }

    // Step 2: Build adversarial zero-memory prompt and invoke transport
    const std::string prompt = buildGoldfishAuditPrompt(request);
    auto transportResult = transport(prompt);
    if (!transportResult.ok()) {
        // Model call failed (timeout or unavailable): graceful degradation to deterministic result
        return reconcilerResult;
    }

    // Step 3: Parse model audit response
    auto parsedAudit = parseGoldfishAuditResponse(transportResult.value());
    if (!parsedAudit.ok()) {
        // Model returned unparseable output; preserve deterministic checks and record unresolved finding
        core::ValidationResult fallback = reconcilerResult;
        fallback.unresolved.push_back(core::ValidationFinding{
            "goldfish_audit",
            "Model response could not be parsed: " + parsedAudit.error().message
        });
        if (fallback.outcome == core::ValidationOutcome::Pass) {
            fallback.outcome = core::ValidationOutcome::Unresolved;
        }
        return fallback;
    }

    // Step 4: Merge findings
    core::ValidationResult combined = reconcilerResult;

    for (const auto& mv : parsedAudit.value().violations) {
        bool exists = false;
        for (const auto& ev : combined.violations) {
            if (ev.constraintId == mv.constraintId) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            combined.violations.push_back(mv);
        }
    }

    for (const auto& mu : parsedAudit.value().unresolved) {
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
        }
    }

    if (!combined.violations.empty()) {
        combined.outcome = core::ValidationOutcome::Fail;
    } else if (!combined.unresolved.empty()) {
        combined.outcome = core::ValidationOutcome::Unresolved;
    } else {
        combined.outcome = core::ValidationOutcome::Pass;
    }

    return combined;
}

} // namespace roclint::bridge
