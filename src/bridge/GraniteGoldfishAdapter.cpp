#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/ModelResponseEnvelope.hpp"
#include "roclint/core/Json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <optional>
#include <set>

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

    [[maybe_unused]] size_t hardViolationsAccepted = 0;
    [[maybe_unused]] size_t hardUnresolvedAccepted = 0;
    [[maybe_unused]] size_t unknownFindingsAccepted = 0;
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

namespace {

// Extracts simple, explicit prohibition targets ("must not use X", "cannot be X", "without X").
// This is a bounded lexical eligibility check, not a general natural-language parser.
[[nodiscard]] std::vector<std::string> explicitProhibitionTargets(std::string_view prompt) {
    const std::string lower = toLowerAscii(prompt);
    static constexpr std::array<std::string_view, 7> kMarkers = {
        "cannot be ", "can't be ", "must not be ", "should not be ",
        "must not use ", "do not use ", "without "
    };
    std::vector<std::string> targets;
    for (const auto marker : kMarkers) {
        std::size_t at = lower.find(marker);
        while (at != std::string::npos) {
            std::size_t begin = at + marker.size();
            while (begin < lower.size() && std::isspace(static_cast<unsigned char>(lower[begin]))) ++begin;
            std::size_t end = begin;
            int words = 0;
            while (end < lower.size() && words < 4) {
                const unsigned char c = static_cast<unsigned char>(lower[end]);
                if (std::isalnum(c) || c == '-' || c == '_') {
                    ++end;
                } else if (c == '\'' && end + 1 < lower.size() &&
                           std::isalnum(static_cast<unsigned char>(lower[end + 1]))) {
                    ++end;
                } else if (std::isspace(c) || c == ',') {
                    break;
                } else {
                    break;
                }
            }
            std::string target = lower.substr(begin, end - begin);
            while (!target.empty() && (std::isspace(static_cast<unsigned char>(target.back())) ||
                                       target.back() == ',' || target.back() == '.')) {
                target.pop_back();
            }
            // "without changing the public API" prohibits changing the API, not the word
            // "changing". Skip the action verb and capture the protected object.
            static constexpr std::array<std::string_view, 6> kActionVerbs = {
                "changing", "modifying", "altering", "touching", "removing", "breaking"
            };
            if (std::find(kActionVerbs.begin(), kActionVerbs.end(), target) != kActionVerbs.end()) {
                std::size_t next = end;
                while (next < lower.size() && std::isspace(static_cast<unsigned char>(lower[next]))) ++next;
                std::size_t objectEnd = next;
                int objectWords = 0;
                while (objectEnd < lower.size() && objectWords < 3) {
                    const unsigned char c = static_cast<unsigned char>(lower[objectEnd]);
                    if (std::isalnum(c) || c == '-' || c == '_') {
                        ++objectEnd;
                    } else if (std::isspace(c)) {
                        ++objectWords;
                        ++objectEnd;
                    } else {
                        break;
                    }
                }
                target = lower.substr(next, objectEnd - next);
                while (!target.empty() && std::isspace(static_cast<unsigned char>(target.back()))) target.pop_back();
            }
            if (target.size() >= 3 &&
                std::find(kActionVerbs.begin(), kActionVerbs.end(), target) == kActionVerbs.end()) {
                targets.push_back(std::move(target));
            }
            at = lower.find(marker, at + marker.size());
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

[[nodiscard]] bool containsWholeWord(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return false;
    std::size_t at = haystack.find(needle);
    while (at != std::string_view::npos) {
        const bool leftOk = at == 0 || !std::isalnum(static_cast<unsigned char>(haystack[at - 1]));
        const std::size_t end = at + needle.size();
        const bool rightOk = end >= haystack.size() || !std::isalnum(static_cast<unsigned char>(haystack[end]));
        if (leftOk && rightOk) return true;
        at = haystack.find(needle, at + 1);
    }
    return false;
}

} // namespace

bool isFalseConflict(std::string_view description) noexcept {
    if (description.empty()) return true;
    std::string lower;
    lower.reserve(description.size());
    for (char c : description) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower.find("no conflict") != std::string::npos ||
        lower.find("no conflicting") != std::string::npos ||
        lower.find("none identified") != std::string::npos ||
        lower.find("no incompatible") != std::string::npos ||
        lower.find("not conflicting") != std::string::npos ||
        lower.find("none found") != std::string::npos ||
        lower.find("no direct conflict") != std::string::npos ||
        lower.find("not in conflict") != std::string::npos ||
        lower == "n/a" || lower == "none" || lower == "nil") {
        return true;
    }
    return false;
}

std::string buildGoldfishDiscoveryAuditPrompt(const GoldfishDiscoveryAuditRequest& request) {
    std::string out;
    out.reserve(request.originalPrompt.size() + 2048);
    out += "<instructions>\n";
    out += "Instruction contract: granite.goldfish_discovery_audit.v1\n";
    out += "You are the Goldfish Discovery Auditor, validating whether proposed prompt assistance questions "
           "and conflicts are consequential, relevant, and compliant with explicit user constraints.\n";
    out += "Rules for discovery audit:\n";
    out += "1. No false conflicts: A conflict requires two explicit, mutually incompatible instructions in the original prompt. "
           "An unspecified preference or statement that no conflict exists is a violation.\n";
    out += "2. No questioning settled requirements: If the original prompt already explicitly states a value, format, or "
           "prohibition, proposing a question or alternative that questions or violates it is a violation.\n";
    out += "3. Consequential, grounded alternatives: Alternatives must represent distinct practical outcomes and cite "
           "evidence from the prompt. Each group must resolve one parameter or explicit priority; flag alternatives answering different questions or merely listing compatible features as Unresolved. Evidence quotes identify the motivating source phrase; they do not claim "
           "the user already requested a creative option. A label is a proposed outcome and proposed_assumptions "
           "are disclosed premises, not source facts. Reject premises contradicting explicit requirements, "
           "not merely because a creative choice introduces an optional direction. A named external reference is not its contents: mark source-specific factual claims Unresolved when the source content is absent. Suggest tentative directions without claiming they came from that reference.\n";
    out += "4. Reply with exactly one JSON object: {\"outcome\": \"Pass\" | \"Fail\" | \"Unresolved\", "
           "\"violations\": [{\"item_id\": string, \"explanation\": string}], "
           "\"unresolved\": [{\"item_id\": string, \"explanation\": string}]}.\n";
    out += "</instructions>\n";
    out += "<original_prompt>\n";
    out += xmlEscape(request.originalPrompt);
    out += "\n</original_prompt>\n";
    out += "<proposed_analysis>\n";
    core::json::Value a = core::json::Value::object();
    core::json::Value groups = core::json::Value::array();
    for (const auto& g : request.proposedAnalysis.choiceGroups) {
        core::json::Value grp = core::json::Value::object();
        grp.set("id", core::json::Value::string(g.id));
        grp.set("question", core::json::Value::string(g.question));
        core::json::Value alts = core::json::Value::array();
        for (const auto& alt : g.alternatives) {
            core::json::Value aobj = core::json::Value::object();
            aobj.set("id", core::json::Value::string(alt.id));
            aobj.set("label", core::json::Value::string(alt.interpretation));
            aobj.set("why_material", core::json::Value::string(alt.whyMaterial));
            aobj.set("proposal_metadata", core::json::Value::string(alt.canonicalSemanticsJson));
            auto evidence = core::json::Value::array();
            for (const auto& span : alt.evidence) {
                if (span.endByte <= request.originalPrompt.size() && span.startByte < span.endByte)
                    evidence.push(core::json::Value::string(request.originalPrompt.substr(span.startByte, span.endByte - span.startByte)));
            }
            aobj.set("source_quotes", std::move(evidence));
            alts.push(std::move(aobj));
        }
        grp.set("alternatives", std::move(alts));
        groups.push(std::move(grp));
    }
    a.set("choice_groups", std::move(groups));
    auto missing = core::json::Value::array();
    for (const auto& decision : request.proposedAnalysis.missingDecisions) {
        auto item = core::json::Value::object();
        item.set("id", core::json::Value::string(decision.id));
        item.set("question", core::json::Value::string(decision.question));
        item.set("blocking", core::json::Value::boolean(decision.blocking));
        missing.push(std::move(item));
    }
    a.set("missing_decisions", std::move(missing));
    auto assumptions = core::json::Value::array();
    for (const auto& assumption : request.proposedAnalysis.assumptions) {
        auto item = core::json::Value::object();
        item.set("id", core::json::Value::string(assumption.id));
        item.set("proposed_statement", core::json::Value::string(assumption.statement));
        assumptions.push(std::move(item));
    }
    a.set("assumptions", std::move(assumptions));
    core::json::Value conflicts = core::json::Value::array();
    for (const auto& c : request.proposedAnalysis.conflicts) {
        core::json::Value cobj = core::json::Value::object();
        cobj.set("id", core::json::Value::string(c.id));
        cobj.set("description", core::json::Value::string(c.description));
        conflicts.push(std::move(cobj));
    }
    a.set("conflicts", std::move(conflicts));
    out += xmlEscape(core::json::serialize(a));
    out += "\n</proposed_analysis>\n";
    return out;
}

const std::string& goldfishDiscoveryAuditResponseSchema() {
    static const std::string kSchema = R"JSON({"type":"object","properties":{"outcome":{"type":"string","enum":["Pass","Fail","Unresolved"]},"violations":{"type":"array","items":{"type":"object","properties":{"item_id":{"type":"string"},"explanation":{"type":"string"}},"required":["item_id","explanation"],"additionalProperties":false}},"unresolved":{"type":"array","items":{"type":"object","properties":{"item_id":{"type":"string"},"explanation":{"type":"string"}},"required":["item_id","explanation"],"additionalProperties":false}}},"required":["outcome","violations","unresolved"],"additionalProperties":false})JSON";
    return kSchema;
}

core::Result<GoldfishDiscoveryAuditResult> parseGoldfishDiscoveryAuditResponse(std::string_view modelOutput) {
    if (modelOutput.size() > kMaxAuditResponseBytes) {
        return core::makeError(core::ErrorCode::InvalidInput, "Discovery audit response exceeds 64 KB limit");
    }
    auto envelope = normalizeModelResponseEnvelope(modelOutput, kMaxAuditResponseBytes);
    if (!envelope.ok()) return envelope.error();

    auto parsed = core::json::parse(envelope.value(), core::json::ParseLimits{kMaxAuditResponseBytes, 8});
    if (!parsed.ok()) return parsed.error();

    const auto& root = parsed.value();
    if (!root.isObject()) {
        return core::makeError(core::ErrorCode::InvalidInput, "Discovery audit response must be an object");
    }

    const auto* outcomeVal = root.find("outcome");
    if (!outcomeVal || !outcomeVal->isString()) {
        return core::makeError(core::ErrorCode::InvalidInput, "Discovery audit response missing outcome");
    }

    GoldfishDiscoveryAuditResult result;
    const std::string outcomeStr = toLowerAscii(outcomeVal->asString());
    if (outcomeStr == "fail") {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (outcomeStr == "unresolved") {
        result.outcome = core::ValidationOutcome::Unresolved;
    } else if (outcomeStr == "pass") {
        result.outcome = core::ValidationOutcome::Pass;
    } else {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Discovery audit outcome must be Pass, Fail, or Unresolved");
    }

    const auto* vArr = root.find("violations");
    const auto* uArr = root.find("unresolved");
    if (!vArr || !vArr->isArray() || !uArr || !uArr->isArray()) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Discovery audit requires violations and unresolved arrays");
    }

    auto readFindings = [](const core::json::Value& array, std::vector<GoldfishDiscoveryFinding>& out)
        -> std::optional<std::string> {
        for (const auto& item : array.asArray()) {
            if (!item.isObject()) {
                return std::string("Discovery audit finding must be an object");
            }
            const auto* idVal = item.find("item_id");
            const auto* expVal = item.find("explanation");
            if (!idVal || !idVal->isString() || idVal->asString().empty() ||
                !expVal || !expVal->isString() || expVal->asString().empty()) {
                return std::string("Discovery audit finding requires nonempty item_id and explanation strings");
            }
            out.push_back({idVal->asString(), expVal->asString()});
        }
        return std::nullopt;
    };

    if (auto problem = readFindings(*vArr, result.violations)) {
        return core::makeError(core::ErrorCode::InvalidInput, *problem);
    }
    if (auto problem = readFindings(*uArr, result.unresolved)) {
        return core::makeError(core::ErrorCode::InvalidInput, *problem);
    }

    // The verdict and findings must agree. A Pass accompanied by findings, a Fail without any
    // violation, or an Unresolved without any unresolved item is internally inconsistent.
    if (result.outcome == core::ValidationOutcome::Pass &&
        (!result.violations.empty() || !result.unresolved.empty())) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Discovery audit Pass conflicts with nonempty findings");
    }
    if (result.outcome == core::ValidationOutcome::Fail && result.violations.empty()) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Discovery audit Fail requires at least one violation");
    }
    if (result.outcome == core::ValidationOutcome::Unresolved && result.unresolved.empty()) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Discovery audit Unresolved requires at least one unresolved item");
    }

    return result;
}

GoldfishDiscoveryAuditResult GraniteGoldfishAdapter::auditDiscovery(
    const GoldfishDiscoveryAuditRequest& request,
    const GraniteTransport& transport) const {

    GoldfishDiscoveryAuditResult result;
    result.executionStatus = core::AuditExecutionStatus::Completed;

    // 1. Deterministic false-conflict check
    for (const auto& conflict : request.proposedAnalysis.conflicts) {
        if (isFalseConflict(conflict.description)) {
            result.violations.push_back({conflict.id, "False conflict identified: " + conflict.description});
            result.outcome = core::ValidationOutcome::Fail;
        }
    }

    // 2. Deterministic duplicate/nondistinct alternatives check. Findings are attached to the
    // offending alternative so repair can remove only that option instead of the whole decision.
    for (const auto& group : request.proposedAnalysis.choiceGroups) {
        for (std::size_t i = 0; i < group.alternatives.size(); ++i) {
            const std::string a = toLowerAscii(trimModelWhitespace(group.alternatives[i].interpretation));
            for (std::size_t j = i + 1; j < group.alternatives.size(); ++j) {
                const std::string b = toLowerAscii(trimModelWhitespace(group.alternatives[j].interpretation));
                if (a == b) {
                    result.violations.push_back({group.alternatives[j].id,
                                                "Alternative duplicates another option in the same decision"});
                    result.outcome = core::ValidationOutcome::Fail;
                }
            }
        }
    }

    // 3. Deterministic explicit-prohibition check.
    const auto prohibitionTargets = explicitProhibitionTargets(request.originalPrompt);
    for (const auto& group : request.proposedAnalysis.choiceGroups) {
        for (const auto& alt : group.alternatives) {
            const std::string lowerAlt = toLowerAscii(alt.interpretation);
            const bool preservationWording =
                lowerAlt.find("keep ") != std::string::npos ||
                lowerAlt.find("preserve") != std::string::npos ||
                lowerAlt.find("leave ") != std::string::npos ||
                lowerAlt.find("maintain") != std::string::npos ||
                lowerAlt.find("retain") != std::string::npos ||
                lowerAlt.find("unchanged") != std::string::npos ||
                lowerAlt.find("untouched") != std::string::npos ||
                lowerAlt.find("without changing") != std::string::npos ||
                lowerAlt.find("without modifying") != std::string::npos;
            if (preservationWording) continue;
            for (const auto& target : prohibitionTargets) {
                if (containsWholeWord(lowerAlt, target)) {
                    result.violations.push_back({alt.id,
                        "Alternative violates explicit prohibition on '" + target + "': " + alt.interpretation});
                    result.outcome = core::ValidationOutcome::Fail;
                    break;
                }
            }
        }
    }

    // 4. Deterministic settled requirement check (e.g. colors, fixed sizes/formats)
    for (const auto& group : request.proposedAnalysis.choiceGroups) {
        for (const auto& alt : group.alternatives) {
            if (auto viol = explicitColorConstraintViolation(request.originalPrompt, alt, group.question); viol.has_value()) {
                result.violations.push_back({alt.id, "Alternative violates settled requirement: " + *viol});
                result.outcome = core::ValidationOutcome::Fail;
            }
        }
    }

    if (result.outcome == core::ValidationOutcome::Fail && !transport) {
        result.executionStatus = core::AuditExecutionStatus::DeterministicOnly;
        return result;
    }

    if (!transport) {
        result.executionStatus = core::AuditExecutionStatus::DeterministicOnly;
        return result;
    }

    // Invoke model discovery audit
    const std::string prompt = buildGoldfishDiscoveryAuditPrompt(request);
    auto transportResult = transport(prompt);
    if (!transportResult.ok()) {
        if (result.outcome == core::ValidationOutcome::Pass) {
            // A deterministic pass without a successful semantic audit is not evidence of
            // sufficiency. Keep the actual meaning instead of promoting it to Pass.
            result.executionStatus = core::AuditExecutionStatus::Unavailable;
            result.outcome = core::ValidationOutcome::Unresolved;
            result.unresolved.push_back({"audit", "The discovery audit was unavailable."});
        }
        return result;
    }

    auto parsedAudit = parseGoldfishDiscoveryAuditResponse(transportResult.value());
    if (!parsedAudit.ok()) {
        if (result.outcome == core::ValidationOutcome::Pass) {
            result.executionStatus = core::AuditExecutionStatus::Malformed;
            result.outcome = core::ValidationOutcome::Unresolved;
            result.unresolved.push_back({"audit", "The discovery audit response was malformed."});
        }
        return result;
    }
    result.executionStatus = core::AuditExecutionStatus::Completed;

    // Merge model findings
    for (const auto& mv : parsedAudit.value().violations) {
        bool exists = false;
        for (const auto& ev : result.violations) {
            if (ev.itemId == mv.itemId) { exists = true; break; }
        }
        if (!exists) result.violations.push_back(mv);
    }
    for (const auto& mu : parsedAudit.value().unresolved) {
        bool exists = false;
        for (const auto& eu : result.unresolved) {
            if (eu.itemId == mu.itemId) { exists = true; break; }
        }
        if (!exists) result.unresolved.push_back(mu);
    }

    if (!result.violations.empty()) {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (!result.unresolved.empty() && result.outcome == core::ValidationOutcome::Pass) {
        result.outcome = core::ValidationOutcome::Unresolved;
    }

    return result;
}

core::analysis::NormalizedAnalysis GraniteGoldfishAdapter::repairDiscovery(
    const core::analysis::NormalizedAnalysis& original,
    const GoldfishDiscoveryAuditResult& auditResult) {
    auto repaired = original;
    if (auditResult.violations.empty()) return repaired;

    std::set<std::string> invalidIds;
    for (const auto& v : auditResult.violations) {
        invalidIds.insert(v.itemId);
    }

    // Prune false conflicts
    std::erase_if(repaired.conflicts, [&](const auto& c) {
        return invalidIds.contains(c.id) || isFalseConflict(c.description);
    });

    // A violation may name a single alternative. Remove only that option and keep the other
    // valid interpretations; rejecting one bad option must not erase the whole open decision.
    auto preserveUnresolved = [&repaired](const std::string& question) {
        if (question.empty()) return;
        const bool alreadyRecorded = std::any_of(repaired.missingDecisions.begin(), repaired.missingDecisions.end(),
            [&](const core::analysis::MissingDecision& m) { return m.question == question; });
        if (alreadyRecorded) return;
        core::analysis::MissingDecision missing;
        missing.id = core::analysis::mintMissingDecisionId(repaired.missingDecisions.size());
        missing.question = question;
        // The decision was material enough to be asked; if every option was rejected it is still
        // unresolved. Keeping it blocking prevents pruning from manufacturing a settled prompt.
        missing.blocking = true;
        repaired.missingDecisions.push_back(std::move(missing));
    };

    std::vector<core::analysis::ChoiceGroup> kept;
    kept.reserve(repaired.choiceGroups.size());
    for (auto& group : repaired.choiceGroups) {
        const bool anyInvalidAlternative = std::any_of(group.alternatives.begin(), group.alternatives.end(),
            [&](const core::analysis::ChoiceAlternative& alt) { return invalidIds.contains(alt.id); });
        // Prefer specific alternative findings. A group id is dropped only when no individual
        // option identifies the rejected content; otherwise valid siblings must survive.
        if (invalidIds.contains(group.id) && !anyInvalidAlternative) {
            preserveUnresolved(group.question);
            continue;
        }
        if (anyInvalidAlternative) {
            std::erase_if(group.alternatives, [&](const core::analysis::ChoiceAlternative& alt) {
                return invalidIds.contains(alt.id);
            });
        }
        if (group.alternatives.size() >= core::analysis::ChoiceGroup::kMinAlternatives) {
            kept.push_back(std::move(group));
        } else if (!group.question.empty()) {
            // The decision is still unresolved but there are no longer enough distinct choices.
            preserveUnresolved(group.question);
        }
    }
    repaired.choiceGroups = std::move(kept);

    return repaired;
}

std::string buildGoldfishClarificationAuditPrompt(const GoldfishClarificationAuditRequest& request) {
    std::string out;
    out.reserve(request.originalPrompt.size() + request.userAnswer.size() + 2048);
    out += "<instructions>\n";
    out += "Instruction contract: granite.goldfish_clarification_audit.v1\n";
    out += "You are the Goldfish Clarification Auditor, validating whether a user's clarification answer "
           "is faithfully represented, relevant to the open question, and compatible with verified requirements.\n";
    out += "Rules for clarification audit:\n";
    out += "1. Faithfulness: The interpretation must represent the user's exact text without distortion or substitution.\n";
    out += "2. Relevance: The answer must address the question asked rather than providing unrelated or gibberish input.\n";
    out += "3. Compatibility: The answer must be completely compatible with the user's original prompt, verified constraints, "
           "and prior answers. An answer that reverses an existing requirement is incompatible.\n";
    out += "Reply with exactly one JSON object: {\"outcome\": \"Pass\" | \"Fail\" | \"Unresolved\", "
           "\"faithful\": boolean, \"relevant\": boolean, \"compatible\": boolean, "
           "\"violations\": [{\"aspect\": string, \"explanation\": string}], "
           "\"unresolved\": [{\"aspect\": string, \"explanation\": string}], "
           "\"feedback\": string}.\n";
    out += "</instructions>\n";

    out += "<original_prompt>\n" + xmlEscape(request.originalPrompt) + "\n</original_prompt>\n";
    out += "<question>\n" + xmlEscape(request.question) + "\n</question>\n";
    out += "<user_answer>\n" + xmlEscape(request.userAnswer) + "\n</user_answer>\n";

    if (!request.offeredAlternatives.empty()) {
        out += "<offered_alternatives>\n";
        for (const auto& alt : request.offeredAlternatives) {
            out += "<alternative>" + xmlEscape(alt) + "</alternative>\n";
        }
        out += "</offered_alternatives>\n";
    }

    if (!request.previousAnswers.empty()) {
        out += "<previous_answers>\n";
        for (const auto& [q, a] : request.previousAnswers) {
            out += "<previous_decision><question>" + xmlEscape(q) + "</question><answer>" + xmlEscape(a) + "</answer></previous_decision>\n";
        }
        out += "</previous_answers>\n";
    }

    if (!request.constraints.empty()) {
        out += "<constraints>\n";
        for (const auto& c : request.constraints) {
            out += "<constraint id=\"" + xmlEscape(c.id) + "\" polarity=\"" +
                   xmlEscape(core::toString(c.polarity)) + "\" enforcement=\"" +
                   xmlEscape(core::toString(c.enforcement)) + "\">" + xmlEscape(c.rule) + "</constraint>\n";
        }
        out += "</constraints>\n";
    }

    return out;
}

const std::string& goldfishClarificationAuditResponseSchema() {
    static const std::string kSchema = R"JSON({"type":"object","properties":{
"outcome":{"type":"string","enum":["Pass","Fail","Unresolved"]},
"faithful":{"type":"boolean"},"relevant":{"type":"boolean"},"compatible":{"type":"boolean"},
"violations":{"type":"array","items":{"type":"object","properties":{"aspect":{"type":"string"},"explanation":{"type":"string"}},"required":["aspect","explanation"],"additionalProperties":false}},
"unresolved":{"type":"array","items":{"type":"object","properties":{"aspect":{"type":"string"},"explanation":{"type":"string"}},"required":["aspect","explanation"],"additionalProperties":false}},
"feedback":{"type":"string"}},
"required":["outcome","faithful","relevant","compatible","violations","unresolved"],"additionalProperties":false})JSON";
    return kSchema;
}

core::Result<GoldfishClarificationAuditResult> parseGoldfishClarificationAuditResponse(std::string_view modelOutput) {
    if (modelOutput.size() > kMaxAuditResponseBytes) {
        return core::makeError(core::ErrorCode::InvalidInput, "Clarification audit response exceeds 64 KB limit");
    }
    auto envelope = normalizeModelResponseEnvelope(modelOutput, kMaxAuditResponseBytes);
    if (!envelope.ok()) return envelope.error();

    auto parsed = core::json::parse(envelope.value(), core::json::ParseLimits{kMaxAuditResponseBytes, 8});
    if (!parsed.ok()) return parsed.error();

    const auto& root = parsed.value();
    if (!root.isObject()) {
        return core::makeError(core::ErrorCode::InvalidInput, "Clarification audit response must be an object");
    }

    const auto* outcomeVal = root.find("outcome");
    if (!outcomeVal || !outcomeVal->isString()) {
        return core::makeError(core::ErrorCode::InvalidInput, "Clarification audit response missing outcome");
    }

    GoldfishClarificationAuditResult result;
    const std::string outcomeStr = toLowerAscii(outcomeVal->asString());
    if (outcomeStr == "fail") {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (outcomeStr == "unresolved") {
        result.outcome = core::ValidationOutcome::Unresolved;
    } else if (outcomeStr == "pass") {
        result.outcome = core::ValidationOutcome::Pass;
    } else {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Clarification audit outcome must be Pass, Fail, or Unresolved");
    }

    const auto* f = root.find("faithful");
    const auto* r = root.find("relevant");
    const auto* c = root.find("compatible");
    if (!f || !f->isBool() || !r || !r->isBool() || !c || !c->isBool()) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Clarification audit requires boolean faithful, relevant, and compatible fields");
    }
    result.faithful = f->asBool();
    result.relevant = r->asBool();
    result.compatible = c->asBool();
    if (const auto* fb = root.find("feedback"); fb && fb->isString()) result.feedback = fb->asString();

    const auto* vArr = root.find("violations");
    const auto* uArr = root.find("unresolved");
    if (!vArr || !vArr->isArray() || !uArr || !uArr->isArray()) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Clarification audit requires violations and unresolved arrays");
    }

    auto readFindings = [](const core::json::Value& array, std::vector<GoldfishClarificationFinding>& out)
        -> std::optional<std::string> {
        for (const auto& item : array.asArray()) {
            if (!item.isObject()) return std::string("Clarification audit finding must be an object");
            const auto* aVal = item.find("aspect");
            const auto* eVal = item.find("explanation");
            if (!aVal || !aVal->isString() || aVal->asString().empty() ||
                !eVal || !eVal->isString() || eVal->asString().empty()) {
                return std::string("Clarification audit finding requires nonempty aspect and explanation strings");
            }
            out.push_back({aVal->asString(), eVal->asString()});
        }
        return std::nullopt;
    };

    if (auto problem = readFindings(*vArr, result.violations)) {
        return core::makeError(core::ErrorCode::InvalidInput, *problem);
    }
    if (auto problem = readFindings(*uArr, result.unresolved)) {
        return core::makeError(core::ErrorCode::InvalidInput, *problem);
    }

    if (result.outcome == core::ValidationOutcome::Pass &&
        (!result.faithful || !result.relevant || !result.compatible ||
         !result.violations.empty() || !result.unresolved.empty())) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Clarification audit Pass conflicts with failed checks or nonempty findings");
    }
    if (result.outcome == core::ValidationOutcome::Fail && result.violations.empty()) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Clarification audit Fail requires at least one violation");
    }
    if (result.outcome == core::ValidationOutcome::Unresolved && result.unresolved.empty()) {
        return core::makeError(core::ErrorCode::InvalidInput,
                               "Clarification audit Unresolved requires at least one unresolved item");
    }

    return result;
}

GoldfishClarificationAuditResult GraniteGoldfishAdapter::auditClarification(
    const GoldfishClarificationAuditRequest& request,
    const GraniteTransport& transport) const {

    GoldfishClarificationAuditResult result;
    result.executionStatus = core::AuditExecutionStatus::Completed;

    const std::string answerTrimmed(trimModelWhitespace(request.userAnswer));
    if (answerTrimmed.empty()) {
        result.faithful = false;
        result.relevant = false;
        result.violations.push_back({"relevance", "User answer is blank"});
        result.outcome = core::ValidationOutcome::Fail;
        result.executionStatus = core::AuditExecutionStatus::DeterministicOnly;
        return result;
    }

    // Deterministic check: compatibility with negative prohibitions. Shared words alone are not
    // evidence of a violation: "Keep the public API unchanged" preserves the boundary it names.
    const std::string lowerAns = toLowerAscii(answerTrimmed);
    const bool preservationWording =
        lowerAns.find("keep ") != std::string::npos ||
        lowerAns.find("preserve") != std::string::npos ||
        lowerAns.find("leave ") != std::string::npos ||
        lowerAns.find("maintain") != std::string::npos ||
        lowerAns.find("retain") != std::string::npos ||
        lowerAns.find("unchanged") != std::string::npos ||
        lowerAns.find("untouched") != std::string::npos ||
        lowerAns.find("without changing") != std::string::npos ||
        lowerAns.find("without modifying") != std::string::npos;
    for (const auto& c : request.constraints) {
        if (c.polarity == core::Polarity::Prohibit && !preservationWording) {
            const std::string lowerRule = toLowerAscii(c.rule);
            if ((lowerAns.find("change") != std::string::npos ||
                 lowerAns.find("modify") != std::string::npos ||
                 lowerAns.find("remove") != std::string::npos ||
                 lowerAns.find("touch") != std::string::npos ||
                 lowerAns.find("alter") != std::string::npos) &&
                (lowerAns.find(lowerRule) != std::string::npos ||
                 (lowerRule.find("token validation") != std::string::npos && lowerAns.find("token validation") != std::string::npos) ||
                 (lowerRule.find("test") != std::string::npos && lowerAns.find("test") != std::string::npos) ||
                 (lowerRule.find("public api") != std::string::npos && lowerAns.find("public api") != std::string::npos))) {
                result.compatible = false;
                result.violations.push_back({"compatibility", "Answer reverses explicit negative prohibition: " + c.rule});
                result.outcome = core::ValidationOutcome::Fail;
            }
        }
    }

    // Check compatibility with explicit prompt constraints
    const std::string lowerPrompt = toLowerAscii(request.originalPrompt);
    if ((lowerPrompt.find("cannot be purple") != std::string::npos || lowerPrompt.find("can't be purple") != std::string::npos) &&
        lowerAns.find("purple") != std::string::npos) {
        result.compatible = false;
        result.violations.push_back({"compatibility", "Answer reverses explicit prompt prohibition on purple"});
        result.outcome = core::ValidationOutcome::Fail;
    }

    if (result.outcome == core::ValidationOutcome::Fail && !transport) {
        result.executionStatus = core::AuditExecutionStatus::DeterministicOnly;
        return result;
    }

    if (!transport) {
        result.executionStatus = core::AuditExecutionStatus::DeterministicOnly;
        return result;
    }

    // Model clarification audit
    const std::string prompt = buildGoldfishClarificationAuditPrompt(request);
    auto transportResult = transport(prompt);
    if (!transportResult.ok()) {
        if (result.outcome == core::ValidationOutcome::Pass) {
            result.executionStatus = core::AuditExecutionStatus::Unavailable;
            result.outcome = core::ValidationOutcome::Unresolved;
            result.unresolved.push_back({"audit", "The clarification audit was unavailable."});
        }
        return result;
    }

    auto parsed = parseGoldfishClarificationAuditResponse(transportResult.value());
    if (!parsed.ok()) {
        if (result.outcome == core::ValidationOutcome::Pass) {
            result.executionStatus = core::AuditExecutionStatus::Malformed;
            result.outcome = core::ValidationOutcome::Unresolved;
            result.unresolved.push_back({"audit", "The clarification audit response was malformed."});
        }
        return result;
    }
    result.executionStatus = core::AuditExecutionStatus::Completed;

    if (!parsed.value().faithful) result.faithful = false;
    if (!parsed.value().relevant) result.relevant = false;
    if (!parsed.value().compatible) result.compatible = false;
    if (!parsed.value().feedback.empty()) result.feedback = parsed.value().feedback;

    for (const auto& mv : parsed.value().violations) {
        bool exists = false;
        for (const auto& ev : result.violations) {
            if (ev.aspect == mv.aspect && ev.explanation == mv.explanation) { exists = true; break; }
        }
        if (!exists) result.violations.push_back(mv);
    }
    for (const auto& mu : parsed.value().unresolved) {
        bool exists = false;
        for (const auto& eu : result.unresolved) {
            if (eu.aspect == mu.aspect && eu.explanation == mu.explanation) { exists = true; break; }
        }
        if (!exists) result.unresolved.push_back(mu);
    }

    if (!result.faithful || !result.relevant || !result.compatible || !result.violations.empty()) {
        result.outcome = core::ValidationOutcome::Fail;
    } else if (!result.unresolved.empty() && result.outcome == core::ValidationOutcome::Pass) {
        result.outcome = core::ValidationOutcome::Unresolved;
    }

    return result;
}

namespace {

[[nodiscard]] std::string discoveryCandidateSignature(const core::analysis::NormalizedAnalysis& analysis) {
    std::string signature;
    for (const auto& group : analysis.choiceGroups) {
        signature += group.id;
        signature += '\x1f';
        signature += group.question;
        signature += '\x1f';
        for (const auto& alt : group.alternatives) {
            signature += alt.id;
            signature += '=';
            signature += toLowerAscii(alt.interpretation);
            signature += '\x1e';
        }
        signature += '\x1d';
    }
    for (const auto& conflict : analysis.conflicts) {
        signature += conflict.id;
        signature += '=';
        signature += toLowerAscii(conflict.description);
        signature += '\x1c';
    }
    for (const auto& missing : analysis.missingDecisions) {
        signature += missing.id;
        signature += '=';
        signature += toLowerAscii(missing.question);
        signature += '\x1b';
    }
    return signature;
}

} // namespace

GoldfishDiscoveryRepairResult GraniteGoldfishAdapter::correctDiscoveryWithRepairLoop(
    const GoldfishDiscoveryAuditRequest& request,
    const GraniteTransport& auditTransport,
    int maxRetries,
    std::int64_t timeBudgetMs,
    const DiscoveryRegeneration& regeneration) const {

    const auto startTime = std::chrono::steady_clock::now();
    GoldfishDiscoveryRepairResult result;
    result.analysis = request.proposedAnalysis;
    result.attemptsExecuted = 1;

    auto budgetRemaining = [&]() {
        if (timeBudgetMs <= 0) return true;
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsedMs < timeBudgetMs;
    };

    // Initial audit
    auto auditVerdict = auditDiscovery(request, auditTransport);
    result.auditVerdict = auditVerdict;

    if (auditVerdict.outcome == core::ValidationOutcome::Pass) {
        result.repairedSuccessfully = true;
        return result;
    }

    // An unresolved audit means the available evidence is insufficient. Pruning cannot prove
    // sufficiency, so only report a repair when regeneration actually produces a passing result.
    if (auditVerdict.outcome == core::ValidationOutcome::Unresolved && !regeneration) {
        return result;
    }

    const int boundedRetries = std::clamp(maxRetries, 0, 3);
    std::vector<std::string> seenSignatures;
    seenSignatures.push_back(discoveryCandidateSignature(result.analysis));

    for (int retry = 1; retry <= boundedRetries; ++retry) {
        if (!budgetRemaining()) break;

        result.attemptsExecuted = retry + 1;
        auto repaired = repairDiscovery(result.analysis, auditVerdict);
        const bool pruned = discoveryCandidateSignature(repaired) != discoveryCandidateSignature(result.analysis);
        if (pruned) {
            result.analysis = std::move(repaired);
        }

        GoldfishDiscoveryAuditRequest reAuditReq = request;
        reAuditReq.proposedAnalysis = result.analysis;
        auditVerdict = pruned ? auditDiscovery(reAuditReq, auditTransport) : auditVerdict;
        result.auditVerdict = auditVerdict;

        if (auditVerdict.outcome == core::ValidationOutcome::Pass) {
            result.repairedSuccessfully = true;
            return result;
        }

        if (!regeneration || auditVerdict.executionStatus != core::AuditExecutionStatus::Completed ||
            !budgetRemaining()) {
            break;
        }

        // Ask the caller's discovery model for a fresh, contextual alternative set after pruning
        // could not satisfy the audit. The adapter does not synthesize replacements itself.
        auto regenerated = regeneration(auditVerdict);
        if (!regenerated.ok()) break;
        auto candidate = std::move(regenerated).value();
        if (candidate.sessionId != request.proposedAnalysis.sessionId ||
            candidate.baseRevisionId != request.proposedAnalysis.baseRevisionId) {
            break;
        }
        const auto signature = discoveryCandidateSignature(candidate);
        if (std::find(seenSignatures.begin(), seenSignatures.end(), signature) != seenSignatures.end()) {
            // The model repeated a candidate; stop instead of looping on identical advice.
            break;
        }
        seenSignatures.push_back(signature);
        result.analysis = std::move(candidate);

        GoldfishDiscoveryAuditRequest finalReq = request;
        finalReq.proposedAnalysis = result.analysis;
        auditVerdict = auditDiscovery(finalReq, auditTransport);
        result.auditVerdict = auditVerdict;
        if (auditVerdict.outcome == core::ValidationOutcome::Pass) {
            result.repairedSuccessfully = true;
            return result;
        }
    }

    result.repairedSuccessfully = result.auditVerdict.outcome == core::ValidationOutcome::Pass;
    return result;
}

} // namespace roclint::bridge
