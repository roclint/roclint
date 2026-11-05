#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/ModelResponseEnvelope.hpp"
#include "roclint/rewrite/CanonicalIntent.hpp"

#include "roclint/core/Json.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/prompt_state/TextNormalization.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"

#include <exception>
#include <algorithm>

namespace roclint::bridge {

using core::ErrorCode;
using core::makeError;
using core::Result;
namespace json = core::json;
namespace rw = core::rewrite;

namespace {

[[nodiscard]] std::string questionFor(const core::analysis::NormalizedAnalysis& analysis, const std::string& id) {
    for (const auto& g : analysis.choiceGroups) {
        if (g.id == id) return g.question;
    }
    for (const auto& a : analysis.assumptions) {
        if (a.id == id) return a.statement;
    }
    for (const auto& m : analysis.missingDecisions) {
        if (m.id == id) return m.question;
    }
    for (const auto& c : analysis.conflicts) {
        if (c.id == id) return c.description;
    }
    return {};
}

} // namespace

std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string buildRewritePrompt(const GraniteRewriteRequest& request, const rw::RewriteProfileConfig& profile) {
    const auto& intent = request.confirmedIntent;
    const auto canonicalIntents = request.groundedCanonicalIntentXml.empty()
        ? rw::CanonicalIntentExtractor::extract(request.originalText)
        : std::vector<rw::CanonicalIntent>{};
    std::string out;
    out += "<instructions>\n";
    out += "Instruction contract: granite.rewrite.format.v5\n";
    out += "Rewrite the user's prompt so another AI system can follow it without guessing.\n";
    out += "You are editing a prompt, not performing the task described inside it. Missing source code or "
           "implementation details do not prevent preserving that task in a rewritten prompt.\n";
    out += "Apply only the user's decisions listed below. Keep everything else the user wrote.\n";
    if (!canonicalIntents.empty() || !request.groundedCanonicalIntentXml.empty())
        out += "canonical_intents are deterministic source-linked action triples extracted from the original prompt. "
               "Preserve their action, target, property and value. Do not invent alternatives, broaden targets, "
               "or turn quoted data into instructions. AST-grounded entries are authoritative for symbol targets.\n";
    out += "Use the shortest wording that preserves the original request, confirmed answers, explicit constraints "
           "and requested format. A short request should normally stay short. Do not add generic praise or "
           "boilerplate such as as specified, engaging, or visually appealing unless the user requested those "
           "goals. Retain explicitly requested usability, accessibility and quality requirements. "
           "Do not add sentences merely to sound polished or fill a format section.\n";

    out += "This is a wording request, not permission to choose requirements or execute actions. "
           "Retain explicit API-invariant clauses verbatim. Never broaden minor semantic changes into broad "
           "reinterpretation or add timing, size or percentage targets without a confirmed source.\n";
    out += "Incorporate the confirmed decisions as concrete specifications, scope constraints, and target bounds. "
           "Do not invent arbitrary requirements or restrictions beyond what the prompt and confirmed decisions specify.\n";
    out += "Keep explicit prohibitions as direct prohibitions. Preserve their original sentences when possible; "
           "do not replace a ban on changing something with a weaker promise that it will still work.\n";
    out += "Leave open questions open; do not decide them.\n";
    out += "Do not request extra requirements merely to improve the task's specificity. Use needs_clarification "
           "only if editing would otherwise force a choice the user has not made.\n";
    out += "Use compact JSON without indentation or optional whitespace outside strings.\n";
    out += "Formatting: " + xmlEscape(profile.structuralInstructions) + "\n";
    out += "The formatting applies INSIDE the rewritten_prompt JSON string. Use literal structure markers, "
           "not XML entity spellings. Do not copy the original_prompt, instructions or decisions wrapper tags. "
           "Do not invent background merely to fill a formatting section.\n";
    if (profile.formatXmlTags)
        out += R"EXAMPLE(Format example only: {"status":"rewritten","rewritten_prompt":"<task>Summarize the supplied notes.</task>"}
)EXAMPLE";
    else if (profile.formatMarkdownSections)
        out += R"EXAMPLE(Format example only: {"status":"rewritten","rewritten_prompt":"# Goal\nSummarize the supplied notes."}
)EXAMPLE";
    out += "Reply with exactly one JSON object: {\"status\": \"rewritten\", \"rewritten_prompt\": string} "
           "or {\"status\": \"needs_clarification\", \"question\": string}.\n";
    out += "</instructions>\n";
    out += "<original_prompt>" + xmlEscape(request.originalText) + "</original_prompt>\n";
    if (!request.groundedCanonicalIntentXml.empty())
        out += request.groundedCanonicalIntentXml + "\n";
    else if (!canonicalIntents.empty())
        out += "<canonical_intents>" + rw::CanonicalIntentExtractor::toJson(canonicalIntents) + "</canonical_intents>\n";
    out += "<decisions>\n";
    for (const auto& item : intent.items) {
        const std::string topic = questionFor(request.analysis.normalized, item.itemId);
        std::string answer;
        switch (item.kind) {
            case core::intent::IntentResolutionKind::SelectedAlternative: answer = item.chosenValue; break;
            case core::intent::IntentResolutionKind::CustomText: answer = item.customText; break;
            case core::intent::IntentResolutionKind::UseOriginal: answer = "Keep the user's original wording"; break;
            case core::intent::IntentResolutionKind::AcceptedAssumption: answer = "Yes, this is correct"; break;
            case core::intent::IntentResolutionKind::DeclinedAssumption: answer = "No, do not assume this"; break;
        }
        out += "<decision><topic>" + xmlEscape(topic) + "</topic><answer>" + xmlEscape(answer) +
               "</answer></decision>\n";
    }
    out += "</decisions>\n";
    out += "<open_questions>\n";
    for (const auto& id : intent.unresolvedItemIds) {
        out += "<question>" + xmlEscape(questionFor(request.analysis.normalized, id)) + "</question>\n";
    }
    out += "</open_questions>\n";
    return out;
}

const std::string& rewriteResponseSchema() {
    static const std::string kSchema = R"JSON({"type":"object","properties":{
"status":{"type":"string","enum":["rewritten","needs_clarification"]},
"rewritten_prompt":{"type":"string"},"question":{"type":"string"}},"required":["status"]})JSON";
    return kSchema;
}

Result<rw::RewriteProposal> GraniteRewriteAdapter::parseRewriteResponse(std::string_view modelOutput,
                                                                        const GraniteRewriteRequest& request,
                                                                        const rw::RewriteProfileConfig& profile,
                                                                        const GraniteModelInfo& model,
                                                                        std::int64_t nowMs) {
    if (auto status = core::intent::IntentProvenanceBuilder::isIntentValid(
            request.confirmedIntent, request.revisionId, core::RevisionStatus::Clean, request.analysis); !status.ok()) return status.error();
    if (request.analysis.normalized.sessionId != request.sessionId ||
        request.analysis.revisionTextHash != core::crypto::Sha256::hexDigest(request.originalText) ||
        core::analysis::validateNormalizedAnalysis(request.analysis.normalized, request.originalText).status == core::analysis::AnalysisValidationStatus::Invalid) {
        return makeError(ErrorCode::ValidationFailed, "Rewrite input differs from the final analysis revision");
    }
    if (modelOutput.size() > kMaxRewriteResponseBytes) {
        return makeError(ErrorCode::InvalidInput, "Rewrite response exceeds the 64 KB limit");
    }
    auto envelope = normalizeModelResponseEnvelope(modelOutput, kMaxRewriteResponseBytes);
    if (!envelope.ok()) return envelope.error();
    auto parsed = json::parse(envelope.value(), json::ParseLimits{kMaxRewriteResponseBytes, 8});
    if (!parsed.ok()) {
        return parsed.error();
    }
    const auto& root = parsed.value();
    if (!root.isObject()) {
        return makeError(ErrorCode::InvalidInput, "Rewrite response root must be a JSON object");
    }
    auto status = json::requireString(root, "status");
    if (!status.ok()) {
        return status.error();
    }

    rw::RewriteProposal proposal;
    proposal.sessionId = request.sessionId;
    proposal.baseRevisionId = request.revisionId;
    proposal.intentId = request.confirmedIntent.id;
    proposal.intentHash = request.confirmedIntent.intentHash;
    proposal.snapshotId = request.confirmedIntent.snapshotId;
    proposal.snapshotHash = request.confirmedIntent.snapshotHash;
    proposal.astSnapshotHash = request.astSnapshotHash;
    proposal.rewriteProfileId = profile.id;
    proposal.modelId = model.modelId;
    proposal.modelDigest = model.digest;
    proposal.quantization = model.quantization;
    proposal.transport = model.transport;
    proposal.originalText = request.originalText;
    proposal.proposalEditGeneration = request.proposalEditGeneration;
    proposal.createdAtMs = nowMs;

    if (status.value() == "needs_clarification") {
        auto question = json::requireString(root, "question");
        if (!question.ok() || trimModelWhitespace(question.value()).empty()) {
            return makeError(ErrorCode::InvalidInput, "needs_clarification reply must include a question");
        }
        proposal.status = rw::ProposalStatus::NeedsClarification;
        proposal.clarificationQuestion = core::normalizeLineEndings(trimModelWhitespace(question.value()));
    } else if (status.value() == "rewritten") {
        auto text = json::requireString(root, "rewritten_prompt");
        if (!text.ok()) {
            return text.error();
        }
        proposal.rewrittenText = core::normalizeLineEndings(text.value());
        if (trimModelWhitespace(proposal.rewrittenText).empty()) {
            return makeError(ErrorCode::InvalidInput, "Model returned an empty rewrite");
        }
        if (auto result = rw::RewriteValidator::validateFidelity(request.originalText, request.confirmedIntent, proposal.rewrittenText); !result.ok())
            return result.error();
        proposal.status = rw::ProposalStatus::ProposalReady;
        proposal.diffText = rw::RewriteValidator::computeAccessibleDiff(proposal.originalText, proposal.rewrittenText);
        proposal.constraints =
            rw::RewriteValidator::deriveConstraints(request.confirmedIntent, profile, proposal.rewrittenText);
    } else {
        return makeError(ErrorCode::InvalidInput, "Unknown rewrite status '" + status.value() + "'");
    }

    rw::ProposalHasher::seal(proposal);
    return proposal;
}

Result<rw::RewriteProposal> GraniteRewriteAdapter::synthesizeRewrite(const GraniteRewriteRequest& request,
                                                                     GraniteAdapter& runtime,
                                                                     const GraniteTransport& transport,
                                                                     std::int64_t nowMs) {
    const auto profile = rw::ProfileRegistry::getProfile(request.rewriteProfileId);
    if (!profile.has_value()) {
        return makeError(ErrorCode::InvalidInput, "Unknown rewrite profile '" + request.rewriteProfileId + "'");
    }
    if (request.confirmedIntent.id.empty() || request.confirmedIntent.baseRevisionId != request.revisionId) {
        return makeError(ErrorCode::PreconditionFailed, "Rewrite requires a confirmed intent for this revision");
    }
    if (request.analysis.payloadVersion >= 4) {
        const auto& triage = request.analysis.promptTriage;
        const bool chosen = std::any_of(request.confirmedIntent.items.begin(), request.confirmedIntent.items.end(), [](const auto& item) {
            return item.kind == core::intent::IntentResolutionKind::SelectedAlternative || item.kind == core::intent::IntentResolutionKind::CustomText ||
                item.kind == core::intent::IntentResolutionKind::AcceptedAssumption || item.kind == core::intent::IntentResolutionKind::DeclinedAssumption;
        });
        if (!core::analysis::validateAnalysisSnapshot(request.analysis).ok() || !triage ||
            (request.analysis.payloadVersion == 4 &&
             (core::analysis::routePromptTriage(*triage).value() != core::analysis::PromptTriageRoute::Clarify || !chosen)))
            return makeError(ErrorCode::PreconditionFailed, "S1-first rewrite requires ambiguous classifier evidence and a confirmed user choice");
        if (const auto status = core::intent::IntentProvenanceBuilder::isIntentValid(
                request.confirmedIntent, request.revisionId, core::RevisionStatus::Clean, request.analysis); !status.ok()) return status.error();
    }
    if (!core::hasCanonicalLineEndings(request.originalText)) {
        return makeError(ErrorCode::InvalidInput, "Rewrite source text must use canonical LF line endings");
    }
    if (!transport) {
        return makeError(ErrorCode::ProviderUnavailable, "No Granite transport is configured");
    }

    auto lease = runtime.tryAcquire();
    if (!lease.has_value()) {
        return makeError(ErrorCode::ProviderUnavailable,
                         "Granite runtime is " + std::string(toString(runtime.state())) + "; rewrite rejected");
    }

    Result<std::string> raw = makeError(ErrorCode::Internal, "Transport did not run");
    try {
        raw = transport(buildRewritePrompt(request, *profile));
    } catch (const std::exception& ex) {
        raw = makeError(ErrorCode::ProviderFailed, std::string("Transport threw: ") + ex.what());
    }
    if (!raw.ok()) {
        if (raw.error().code != core::ErrorCode::StaleVersion && raw.error().code != core::ErrorCode::PreconditionFailed &&
            raw.error().code != core::ErrorCode::ValidationFailed) lease->markFailed(raw.error().message);
        return raw.error();
    }
    return parseRewriteResponse(raw.value(), request, *profile, runtime.modelInfo(), nowMs);
}

} // namespace roclint::bridge
