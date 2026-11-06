#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/ModelResponseEnvelope.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/rewrite/StructuredEntityExtractor.hpp"

#include "roclint/core/Json.hpp"
#include "roclint/core/prompt_state/TextNormalization.hpp"

#include <chrono>
#include <exception>
#include <algorithm>
#include <cctype>
#include <regex>

namespace roclint::bridge {

namespace json = core::json;
using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

Result<std::string> readText(const json::Value& obj, std::string_view key, bool required) {
    const json::Value* v = obj.find(key);
    if (v == nullptr || v->isNull()) {
        if (required) {
            return makeError(ErrorCode::InvalidInput, "Missing required field '" + std::string(key) + "'");
        }
        return std::string{};
    }
    if (!v->isString()) {
        return makeError(ErrorCode::InvalidInput, "Field '" + std::string(key) + "' must be a string");
    }
    if (v->asString().size() > kMaxModelStringBytes) {
        return makeError(ErrorCode::InvalidInput, "Field '" + std::string(key) + "' exceeds the size limit");
    }
    std::string text = core::normalizeLineEndings(v->asString());
    if (required && trimModelWhitespace(text).empty()) {
        return makeError(ErrorCode::InvalidInput, "Field '" + std::string(key) + "' must not be blank");
    }
    return text;
}

Result<bool> readFlag(const json::Value& obj, std::string_view key, bool defaultValue) {
    const json::Value* v = obj.find(key);
    if (v == nullptr || v->isNull()) {
        return defaultValue;
    }
    if (!v->isBool()) {
        return makeError(ErrorCode::InvalidInput, "Field '" + std::string(key) + "' must be a boolean");
    }
    return v->asBool();
}

Result<const json::Value::Array*> readArray(const json::Value& obj, std::string_view key, std::size_t maxItems) {
    static const json::Value::Array kEmpty{};
    const json::Value* v = obj.find(key);
    if (v == nullptr || v->isNull()) {
        return &kEmpty;
    }
    if (!v->isArray()) {
        return makeError(ErrorCode::InvalidInput, "Field '" + std::string(key) + "' must be an array");
    }
    if (v->asArray().size() > maxItems) {
        return makeError(ErrorCode::InvalidInput, "Field '" + std::string(key) + "' has too many entries");
    }
    return &v->asArray();
}

// Resolves model-quoted excerpts to unique spans in the frozen text. Unresolvable excerpts are
// counted and dropped, never approximated.
Result<std::vector<core::analysis::SourceSpan>> readEvidence(const json::Value& obj,
                                                             std::string_view frozenText,
                                                             std::size_t& ungrounded) {
    auto arr = readArray(obj, "evidence", kMaxDiscoveryItemsPerKind);
    if (!arr.ok()) {
        return arr.error();
    }
    std::vector<core::analysis::SourceSpan> spans;
    for (const auto& item : *arr.value()) {
        if (!item.isString()) {
            return makeError(ErrorCode::InvalidInput, "Evidence entries must be strings");
        }
        if (item.asString().size() > kMaxModelStringBytes) {
            return makeError(ErrorCode::InvalidInput, "Evidence excerpt exceeds the size limit");
        }
        const std::string excerpt = core::normalizeLineEndings(item.asString());
        const auto match = core::analysis::findUniqueSpan(frozenText, excerpt);
        if (match.status == core::analysis::SpanMatchStatus::ExactMatch && match.span.has_value() &&
            !match.span->empty()) {
            spans.push_back(*match.span);
        } else {
            ++ungrounded;
        }
    }
    return spans;
}

Result<core::analysis::ChoiceGroup> readGroup(const json::Value& g, std::size_t groupIdx, std::string_view frozenText,
                                              std::size_t& ungrounded) {
    if (!g.isObject()) {
        return makeError(ErrorCode::InvalidInput, "choice_groups entries must be objects");
    }
    core::analysis::ChoiceGroup group;
    group.id = core::analysis::mintGroupId(groupIdx);

    auto dimension = readText(g, "dimension", false);
    if (!dimension.ok()) return dimension.error();
    group.dimension = std::move(dimension).value();

    auto question = readText(g, "question", true);
    if (!question.ok()) return question.error();
    group.question = std::move(question).value();

    auto material = readFlag(g, "material", true);
    if (!material.ok()) return material.error();
    group.material = material.value();

    const json::Value* alts = g.find("alternatives");
    if (alts == nullptr || !alts->isArray()) {
        return makeError(ErrorCode::InvalidInput, "choice group is missing an 'alternatives' array");
    }
    if (alts->asArray().size() > kMaxDiscoveryItemsPerKind) {
        return makeError(ErrorCode::InvalidInput, "choice group has too many alternatives");
    }
    for (std::size_t a = 0; a < alts->asArray().size(); ++a) {
        const auto& altValue = alts->asArray()[a];
        if (!altValue.isObject()) {
            return makeError(ErrorCode::InvalidInput, "alternatives entries must be objects");
        }
        core::analysis::ChoiceAlternative alt;
        alt.id = core::analysis::mintAlternativeId(groupIdx, a);
        const bool hasLabel = altValue.find("label") != nullptr;
        auto label = readText(altValue, hasLabel ? "label" : "interpretation", true);
        if (!label.ok()) return label.error();
        alt.interpretation = std::move(label).value();
        auto why = readText(altValue, "why_material", false);
        if (!why.ok()) return why.error();
        alt.whyMaterial = std::move(why).value();
        auto evidence = readEvidence(altValue, frozenText, ungrounded);
        if (!evidence.ok()) return evidence.error();
        alt.evidence = std::move(evidence).value();
        group.alternatives.push_back(std::move(alt));
    }
    return group;
}

} // namespace

std::optional<std::string> explicitColorConstraintViolation(
    std::string_view frozenPrompt, const core::analysis::ChoiceAlternative& alternative, std::string_view question) {
    const std::string prompt(frozenPrompt);
    const std::string colors = "black|white|blue|red|green|yellow|orange|purple|pink|brown|gray|grey";
    static const std::regex negative("\\b(?:cannot|can't|cant|must not|should not|never)\\s+be\\s+(" + colors + ")\\b", std::regex::icase);
    static const std::regex positive("\\b(?:(?:will|must|shall)\\s+be|is)\\s+(" + colors + ")\\b", std::regex::icase);
    static const std::regex namedColor("\\b(" + colors + ")\\b", std::regex::icase);
    static const std::regex negativePrefix(R"(\b(?:no|not|never|without)\s+(?:(?:any|visible|pure|solid)\s+)?$)", std::regex::icase);
    static const std::regex negativeSuffix(R"(^\s+(?:(?:color|colors|colour|colours|coloration|parts|tones|shades)\s+)?(?:(?:is|are)\s+)?(?:(?:completely|entirely|strictly)\s+)?(?:not\s+(?:allowed|permitted|acceptable)|forbidden|disallowed|prohibited|excluded)\b)", std::regex::icase);
    static const std::regex narrowedBan(R"(\b(?:some|certain|specific|except|unless|partially|limited)\b)", std::regex::icase);
    static const std::regex banQuestion(R"(^\s*(?:what|which)\b.*\b(?:prohibit|prohibited|forbid|forbidden|ban|banned|exclude|excluded)\b)", std::regex::icase);
    static const std::regex permission(R"(\b(?:allow|allowed|permitted|permissible|acceptable|optional)\b)", std::regex::icase);
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const std::string candidate = lower(alternative.interpretation);
    const bool implicitBan = std::regex_search(std::string(question), banQuestion) && !std::regex_search(candidate, permission);
    auto quoted = [&](std::size_t position) {
        char quote = 0;
        for (std::size_t i = 0; i < position; ++i) {
            const char c = prompt[i];
            if (quote && c == '\\') { ++i; continue; }
            if (c == '\'' && i > 0 && i + 1 < prompt.size() &&
                std::isalnum(static_cast<unsigned char>(prompt[i - 1])) &&
                std::isalnum(static_cast<unsigned char>(prompt[i + 1]))) continue; // contraction
            if (quote == c) quote = 0;
            else if (!quote && (c == '"' || c == '\'' || c == '`')) quote = c;
        }
        return quote != 0;
    };
    auto negated = [&](const std::smatch& color) {
        const auto position = static_cast<std::size_t>(color.position());
        return std::regex_search(candidate.substr(0, position), negativePrefix) ||
               std::regex_search(candidate.substr(position + color.length()), negativeSuffix);
    };
    for (const bool prohibited : {true, false}) {
        const auto& rule = prohibited ? negative : positive;
        for (std::sregex_iterator it(prompt.begin(), prompt.end(), rule), end; it != end; ++it) {
            const auto& requirement = *it;
            const auto start = static_cast<std::size_t>(requirement.position(1));
            const auto finish = start + requirement.length(1);
            if (finish < prompt.size() && prompt[finish] == '-') continue; // e.g. black-box, not a color lock
            if (quoted(start) || !std::any_of(alternative.evidence.begin(), alternative.evidence.end(),
                [&](const auto& span) { return span.startByte <= start && span.endByte >= finish; })) continue;
            const auto fixedColor = lower(requirement.str(1));
            for (std::sregex_iterator color(candidate.begin(), candidate.end(), namedColor); color != end; ++color) {
                const auto mentioned = lower(color->str());
                const bool weakenedBan = std::regex_search(candidate, narrowedBan);
                const bool invalid = prohibited
                    ? mentioned == fixedColor && ((!negated(*color) && !implicitBan) || weakenedBan)
                    : (mentioned != fixedColor && !negated(*color)) ||
                      (mentioned == fixedColor && (negated(*color) ||
                       std::regex_search(candidate, std::regex("\\b(?:near|almost|off)[- ]+" + fixedColor + "\\b"))));
                if (invalid) return "Generated option conflicts with the explicit color requirement: \"" + requirement.str() + "\". Use a manual answer for an intended scope change.";
            }
        }
    }
    return std::nullopt;
}

namespace {
json::Value evidenceQuotes(std::string_view prompt) {
    auto quotes = json::Value::array();
    if (prompt.empty() || prompt.size() > kMaxDiscoveryResponseBytes) return quotes;
    std::size_t budget = 0;
    std::vector<std::string> seen;
    auto add = [&](std::string_view excerpt) {
        if (excerpt.empty() || excerpt.size() > 256 || seen.size() >= 32 ||
            std::find(seen.begin(), seen.end(), excerpt) != seen.end()) return;
        auto value = json::Value::string(std::string(excerpt));
        const auto size = json::serialize(value).size() + 1;
        if (budget + size > 3072) return;
        const auto match = core::analysis::findUniqueSpan(prompt, excerpt);
        if (match.status != core::analysis::SpanMatchStatus::ExactMatch) return;
        budget += size; seen.emplace_back(excerpt); quotes.push(std::move(value));
    };
    add(prompt);
    // Token boundaries use ASCII whitespace; complete UTF-8 tokens are never split.
    std::vector<std::pair<std::size_t, std::size_t>> tokens;
    for (std::size_t i = 0; i < prompt.size();) {
        while (i < prompt.size() && (prompt[i] == ' ' || prompt[i] == '\n' || prompt[i] == '\t' || prompt[i] == '\r')) ++i;
        const auto begin = i;
        while (i < prompt.size() && prompt[i] != ' ' && prompt[i] != '\n' && prompt[i] != '\t' && prompt[i] != '\r') ++i;
        if (i > begin) tokens.emplace_back(begin, i);
    }
    const auto samples = std::min<std::size_t>(8, tokens.size());
    for (std::size_t sample = 0; sample < samples && seen.size() < 32; ++sample) {
        const auto index = samples == 1 ? 0 : sample * (tokens.size() - 1) / (samples - 1);
        const auto begin = tokens[index].first;
        for (std::size_t length = 1; length <= 3 && index + length <= tokens.size(); ++length)
            add(prompt.substr(begin, tokens[index + length - 1].second - begin));
        auto word = prompt.substr(begin, tokens[index].second - begin);
        while (!word.empty() && static_cast<unsigned char>(word.front()) < 128 && std::ispunct(static_cast<unsigned char>(word.front()))) word.remove_prefix(1);
        while (!word.empty() && static_cast<unsigned char>(word.back()) < 128 && std::ispunct(static_cast<unsigned char>(word.back()))) word.remove_suffix(1);
        add(word);
    }
    return quotes;
}
} // namespace

std::string buildDiscoveryPrompt(const GraniteOptionDiscoveryRequest& req) {
    json::Value request = json::Value::object();
    request.set("session_id", json::Value::string(req.sessionId));
    request.set("revision_id", json::Value::string(req.revisionId));
    request.set("host_context_category", json::Value::string(req.hostContextCategory));
    request.set("prompt_text", json::Value::string(req.promptString));
    const auto canonicalIntents = req.groundedCanonicalIntentXml.empty()
        ? core::rewrite::CanonicalIntentExtractor::extract(req.promptString)
        : std::vector<core::rewrite::CanonicalIntent>{};
    if (!req.groundedCanonicalIntentXml.empty()) {
        request.set("grounded_canonical_intents", json::Value::string(req.groundedCanonicalIntentXml));
    } else if (!canonicalIntents.empty()) {
        auto parsed = json::parse(core::rewrite::CanonicalIntentExtractor::toJson(canonicalIntents),
                                  json::ParseLimits{16 * 1024, 8});
        if (parsed.ok() && parsed.value().isArray()) request.set("canonical_intents", std::move(parsed).value());
    }
    request.set("evidence_quotes", evidenceQuotes(req.promptString));
    if (req.promptTriage) {
        json::Value triage = json::Value::object();
        triage.set("id", json::Value::string(req.promptTriage->id));
        triage.set("hash", json::Value::string(req.promptTriage->triageHash));
        triage.set("ambiguity_score", json::Value::number(req.promptTriage->ambiguityScore));
        triage.set("category", json::Value::string(core::analysis::toString(req.promptTriage->primaryCategory)));
        triage.set("threshold", json::Value::number(core::analysis::kPromptTriageThreshold));
        request.set("s1_classification", std::move(triage));
    }
    if (!req.workspaceFiles.empty()) {
        json::Value files = json::Value::array();
        for (const auto& f : req.workspaceFiles) {
            files.push(json::Value::string(f));
        }
        request.set("workspace_files", std::move(files));
    }
    if (!req.workspaceSymbols.empty()) {
        json::Value syms = json::Value::array();
        for (const auto& s : req.workspaceSymbols) {
            syms.push(json::Value::string(s));
        }
        request.set("workspace_symbols", std::move(syms));
    }
    request.set("is_non_technical", json::Value::boolean(req.isNonTechnicalPrompt));

    std::string out;
    out += "Instruction contract: granite.discovery.selective.v8\n";
    out += "Ask the FEWEST questions necessary. Usually zero or one; three is a ceiling, never a quota. "
           "An explicit color, shape, output format or prohibition is already a decision. Keep it fixed. "
           "Do not turn how to implement that decision into an ambiguity about whether to obey it.\n";
    out += "Literal-property rule: an ordinary stated color, shape, name, quantity or output format means "
           "that exact property. Do not ask what a color name means, invent near-colors, narrow a ban to "
           "some shades, or offer exceptions to a prohibition. These are changes to instructions, not "
           "interpretations. Compare EACH alternative against ALL explicit requirements; discard any "
           "alternative that weakens one. If fewer than two valid alternatives remain, omit that group.\n";
    out += "S1's category is a triage hint, not proof of a conflict. Compatible requirements such as "
           "a required property plus a ban on a different property are not a conflict. Do not invent "
           "a question to justify S1's category.\n";
    if (!canonicalIntents.empty() || !req.groundedCanonicalIntentXml.empty())
        out += "canonical_intents are deterministic source-linked action triples extracted from prompt_text. "
               "Use them as grounding for targets, properties and values; do not invent alternatives or "
               "treat them as new requirements. AST-grounded entries are authoritative for symbol targets.\n";
    out += "Review what help, if any, this prompt needs before it is sent to another AI system.\n";
    out += "Set assistance_kind to none for a usable prompt without questions, clarification for consequential "
           "missing user decisions, or creative/refinement/decomposition for OPTIONAL help. Creative openness "
           "is not an error. Never demand design rules for an intentionally open brief.\n";
    out += "For an underspecified bug, ask what actually happens (nothing, an error, or a wrong result), "
           "not its technical cause. For an interactive feature, ask what it should do only if unresolved; "
           "do not ask whether a fully specified feature should work. Do not invent a backend.\n";
    out += "For a vague request to improve existing code without a chosen outcome, ask which improvement "
           "matters. Offer distinct general goals such as readability, reliability or performance when relevant. "
           "Do not substitute new features or an implementation strategy for an improvement goal. Offer three "
           "choices when three materially distinct readings exist, otherwise two; the user may supply their "
           "own answer or leave the goal open. Never choose a goal on the user's behalf.\n";
    out += "For many requested features, optional decomposition may offer sequencing while retaining ALL goals. "
           "Do not drop features or invent priorities. Optional help uses choice_groups only, without "
           "assumptions, conflicts or missing_decisions. Ask one useful initial question when possible.\n";
    out += req.reviewTrigger == core::analysis::ReviewTrigger::ExplicitReview
        ? "The user explicitly requested deeper review. Low S1 scores do not prevent useful optional help; "
          "return no_ambiguity if no help is needed.\n"
        : "This is an automatic check. Be conservative about interruption. A usable open creative brief can "
          "return no_ambiguity.\n";
    out += "Respond with exactly one JSON object and nothing else, using this schema:\n";
    out += "{\"status\": \"options_generated\" | \"no_ambiguity\" | \"needs_clarification\",\n";
    out += " \"assistance_kind\": \"none\" | \"clarification\" | \"creative\" | \"refinement\" | \"decomposition\",\n";
    out += " \"choice_groups\": [{\"dimension\": string, \"question\": string, \"material\": boolean,\n";
    out += "   \"alternatives\": [{\"label\": string, \"why_material\": string, \"evidence\": [string]}]}],\n";
    out += " \"assumptions\": [{\"statement\": string, \"material\": boolean, \"evidence\": [string]}],\n";
    out += " \"missing_decisions\": [{\"question\": string, \"blocking\": boolean, \"suggested_alternative_evidence\": string}],\n";
    out += " \"conflicts\": [{\"description\": string, \"blocking\": boolean}],\n";
    out += " \"clarification_question\": string}\n";
    out += "When a consequential decision is missing, find phrases in prompt_text that a capable assistant could reasonably read in 2 or 3 "
           "materially different ways, and turn each one into a choice group.\n";
    out += "Rules:\n";
    if (req.promptTriage)
        out += "- S1 has flagged this frozen prompt. Its classification is advisory and may be a false positive. "
               "Return grounded choices or missing decisions only when they materially change the requested result. "
               "If no material decision is missing, return no_ambiguity with empty arrays. Never invent a question "
               "or requirement to satisfy S1. A score is not a user choice.\n";
    out += "- Emit compact JSON without indentation or optional whitespace. Keep questions and explanations "
           "brief; do not repeat the request.\n";
    out += "- Use \"options_generated\" whenever at least one phrase has materially different readings. Report at "
           "most 3 choice groups, the most consequential first.\n";
    out += "- Each choice group offers 2 or 3 short, concrete alternatives (under 12 words each) that would lead to "
           "materially different results.\n";
    out += "- Choose evidence strings ONLY from evidence_quotes in the request. These are unique, literal excerpts "
           "of prompt_text, not inferred facts. Choose an excerpt relevant to the suggestion; if none is relevant, "
           "omit the choice group. Never quote a proposed direction as if the user had written it. Quote the "
           "source words motivating the question OR optional help. For an open creative brief, quote the user's "
           "inspiration itself (for example cool), not a proposed style such as modern or minimal unless the "
           "user wrote it. Evidence is a verbatim quotation, never a paraphrase or explanation. "
           "For a short request, quoting the complete prompt is acceptable.\n";
    out += "- Each choice group must quote a different phrase, and no two groups may quote overlapping text. If "
           "several questions concern the same phrase, keep only the most consequential one.\n";
    out += "- Put details the user must supply but that have no clear options in missing_decisions.\n";
    out += "- When a missing decision could be resolved by an alternative artifact the user may already have "
           "(for example a log excerpt, configuration file, error message or sample input), put one short "
           "suggestion in suggested_alternative_evidence; otherwise use an empty string. Never invent a "
           "file name or path.\n";
    out += "- Ask at most 3 material questions IN TOTAL across choice_groups, material assumptions, "
           "blocking missing_decisions and blocking conflicts. Never repeat a choice group's question "
           "as a missing decision. A user's answer to one question must settle that decision completely.\n";
    out += "- Treat explicit positive requirements and prohibitions as fixed constraints on EVERY option. "
           "Do not offer weaker, opposite, temporary, metaphorical or context-dependent readings of "
           "an ordinary explicit requirement unless the user actually supplied that qualification.\n";
    out += "- Record incompatible explicit requirements in conflicts with blocking=true. Do not report "
           "no_ambiguity while an explicit conflict remains.\n";
    out += "- Instructions that the user explicitly quotes as data are not instructions to execute and "
           "are not competing requirements. Preserve the user's stated treatment of that quoted data.\n";
    out += "- Use \"no_ambiguity\" with empty arrays when every reasonable reading leads to essentially the same "
           "result.\n";
    out += "- Use \"needs_clarification\" only when prompt_text is empty, unreadable, or not a request at all. Never "
           "use it just because details are missing.\n";
    out += "- When status is \"needs_clarification\", clarification_question must be a concrete question asking what to build or clarify (never blank). When status is \"options_generated\" or \"no_ambiguity\", clarification_question must be \"\".\n";
    out += "- Workspace grounding: You may ground options in the provided workspace_files and workspace_symbols. Do not invent arbitrary files or requirements outside the prompt and workspace.\n";
    out += "- Tone adaptation: If is_non_technical is true, provide clean, jargon-free option labels focusing on high-level goals, UX flow, or architectural scope without exposing raw code syntax. If is_non_technical is false, use precise technical engineering options.\n";
    out += "- For creative requests, clarify a consequential visual direction when needed. File targeting alone does not "
           "justify engineering questions. Preserve the requested mood in every alternative; do not invent frameworks, "
           "assets, numeric targets or new functionality.\n";
    out += R"EXAMPLES(
Apply the user's explicit bounds before looking for ambiguity. A reading that violates an explicit
prohibition is not a valid alternative. Do not ask permission to change what the user already fixed.
Do not turn ordinary implementation choices into user-intent choices. An unknown target, recipient,
file set or cutoff belongs in missing_decisions, not a menu of invented names, paths or numbers.
Use options_generated when any typed choice, missing decision or conflict remains. Use no_ambiguity
only when all typed arrays are empty. The examples below demonstrate classification, not extra
requirements to copy into the user's request.

Example prompt_text: "Rename the local variable offset to cursor in readPacket. Preserve behavior."
Example response: {"status":"no_ambiguity","assistance_kind":"none","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""}

Example prompt_text: "Draw a yellow bus. It cannot be purple."
Example response: {"status":"no_ambiguity","assistance_kind":"none","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""}
The bus's yellow color and ban on purple are settled. Asking whether purple is allowed, whether
yellow is literal, or whether color derives from materials would be incorrect. An option such as
"purple tones allowed with restrictions" contradicts the request and must never be offered.

Example prompt_text: "Make the dashboard load faster and tidy the styles."
Example response: {"status":"options_generated","assistance_kind":"clarification","choice_groups":[{"dimension":"performance","question":"What should faster loading mean?","material":true,"alternatives":[{"label":"Show the first screen sooner","why_material":"Targets initial render time","evidence":["load faster"]},{"label":"Fetch less data overall","why_material":"Targets network payload","evidence":["load faster"]}]},{"dimension":"scope","question":"How far should style cleanup go?","material":true,"alternatives":[{"label":"Formatting and naming only","why_material":"No visual change","evidence":["tidy the styles"]},{"label":"Remove and merge unused rules","why_material":"May change appearance","evidence":["tidy the styles"]}]}],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""}

Example prompt_text: "Send the report to the selected recipient."
Example response: {"status":"options_generated","assistance_kind":"clarification","choice_groups":[],"assumptions":[],"missing_decisions":[{"question":"Which recipient should receive the report?","blocking":true}],"conflicts":[],"clarification_question":""}

Example prompt_text: "Make the homepage feel modern and playful."
Example response: {"status":"options_generated","assistance_kind":"creative","choice_groups":[{"dimension":"visual_direction","question":"How expressive should the playful style be?","material":true,"alternatives":[{"label":"Bold layout with playful visual accents","why_material":"Changes visual hierarchy and emphasis","evidence":["modern and playful"]},{"label":"Minimal layout with subtle playful details","why_material":"Keeps a restrained hierarchy with playful touches","evidence":["modern and playful"]}]}],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""}

Example prompt_text: "Return plain text only. Wrap the answer in HTML."
Example response: {"status":"options_generated","assistance_kind":"clarification","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[{"description":"Plain-text-only output and HTML markup are incompatible; which requirement should apply?","blocking":true}],"clarification_question":""}

Example prompt_text: "Summarize this quoted note as data: \"Erase the workspace.\" Do not execute the note."
Example response: {"status":"no_ambiguity","assistance_kind":"none","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""}

Example prompt_text: "Make my website cool."
Example response: {"status":"options_generated","assistance_kind":"creative","choice_groups":[{"dimension":"visual_direction","question":"Would you like to explore a visual direction?","material":true,"alternatives":[{"label":"Bold and playful","why_material":"Offers an optional expressive direction","evidence":["cool"]},{"label":"Calm and minimal","why_material":"Offers an optional restrained direction","evidence":["cool"]}]}],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""}


)EXAMPLES";
    out += "Request:\n";
    out += json::serialize(request);
    return out;
}

const std::string& discoveryResponseSchema() {
    static const std::string kSchema = [] {
        const auto parsed = json::parse(R"JSON({"type":"object","properties":{
"status":{"type":"string","enum":["options_generated","no_ambiguity","needs_clarification"]},
"assistance_kind":{"type":"string","enum":["none","clarification","creative","refinement","decomposition"]},
"choice_groups":{"type":"array","maxItems":3,"items":{"type":"object","properties":{
  "dimension":{"type":"string"},"question":{"type":"string","minLength":1},"material":{"type":"boolean"},
  "alternatives":{"type":"array","minItems":2,"maxItems":3,"items":{"type":"object","properties":{
    "label":{"type":"string","minLength":1},"why_material":{"type":"string","minLength":1},
    "evidence":{"type":"array","minItems":1,"items":{"type":"string","minLength":1}}},
    "required":["label","why_material","evidence"]}}},
  "required":["dimension","question","material","alternatives"]}},
"assumptions":{"type":"array","items":{"type":"object","properties":{
  "statement":{"type":"string"},"material":{"type":"boolean"},"evidence":{"type":"array","items":{"type":"string"}}},
  "required":["statement","material","evidence"]}},
"missing_decisions":{"type":"array","items":{"type":"object","properties":{
  "question":{"type":"string","minLength":1},"blocking":{"type":"boolean"},
  "suggested_alternative_evidence":{"type":"string","maxLength":1024}},"required":["question","blocking"]}},
"conflicts":{"type":"array","items":{"type":"object","properties":{
  "description":{"type":"string"},"blocking":{"type":"boolean"}},"required":["description","blocking"]}},
"clarification_question":{"type":"string"}},
"required":["status","assistance_kind","choice_groups","assumptions","missing_decisions","conflicts","clarification_question"]})JSON");
        const auto& base = parsed.value(); // A compiled literal, exercised by contract tests.
        auto branches = json::Value::array();
        auto constant = [](json::Value value) { return json::Value::object().set("const", std::move(value)); };
        const auto makeBranch = [&](const std::string& category, const char* nonEmptyField) {
            auto branch = base;
            auto properties = *base.find("properties");
            const bool ready = category == "none";
            const bool unreadable = category == "unreadable";
            const bool optional = !ready && !unreadable && category != "clarification";
            properties.set("status", constant(json::Value::string(ready ? "no_ambiguity" : unreadable ? "needs_clarification" : "options_generated")));
            properties.set("assistance_kind", constant(json::Value::string(unreadable ? "clarification" : category)));
            if (ready || unreadable || optional || category == "clarification") {
                for (const char* field : {"assumptions", "missing_decisions", "conflicts"}) {
                    if (nonEmptyField != nullptr && std::string_view(field) == nonEmptyField) continue;
                    properties.set(field, constant(json::Value::array()));
                }
            }
            if (ready || unreadable) properties.set("choice_groups", constant(json::Value::array()));
            if (optional) {
                auto groups = *properties.find("choice_groups");
                groups.set("minItems", json::Value::number(1));
                groups.set("maxItems", json::Value::number(1));
                auto group = *groups.find("items");
                auto groupProperties = *group.find("properties");
                groupProperties.set("material", constant(json::Value::boolean(true)));
                group.set("properties", std::move(groupProperties)); groups.set("items", std::move(group));
                properties.set("choice_groups", std::move(groups));
            }
            if (category == "clarification") {
                // The parser rejects empty options_generated payloads. Each branch
                // requires one non-empty typed array, so the grammar cannot emit a
                // contract-invalid all-empty response.
                if (nonEmptyField != nullptr) {
                    auto field = *properties.find(nonEmptyField);
                    field.set("minItems", json::Value::number(1));
                    properties.set(nonEmptyField, std::move(field));
                }
                if (nonEmptyField == nullptr || std::string_view(nonEmptyField) != "choice_groups")
                    properties.set("choice_groups", constant(json::Value::array()));
            }
            if (!unreadable) properties.set("clarification_question", constant(json::Value::string("")));
            else {
                auto question = json::Value::object();
                question.set("type", json::Value::string("string")); question.set("minLength", json::Value::number(1));
                properties.set("clarification_question", std::move(question));
            }
            branch.set("properties", std::move(properties));
            branch.set("additionalProperties", json::Value::boolean(false));
            branches.push(std::move(branch));
        };
        for (const char* kind : {"none", "creative", "refinement", "decomposition", "unreadable"})
            makeBranch(kind, nullptr);
        for (const char* field : {"choice_groups", "assumptions", "missing_decisions", "conflicts"})
            makeBranch("clarification", field);
        auto schema = json::Value::object(); schema.set("oneOf", std::move(branches));
        return json::serialize(schema);
    }();
    return kSchema;
}

std::string discoveryResponseSchema(std::string_view frozenPromptText) {
    const auto quotes = evidenceQuotes(frozenPromptText);
    if (quotes.asArray().empty()) return discoveryResponseSchema();
    auto schema = json::parse(discoveryResponseSchema()).value();
    auto branches = json::Value::array();
    for (auto branch : schema.find("oneOf")->asArray()) {
        auto properties = *branch.find("properties");
        auto restrictEvidence = [&](json::Value item) {
            auto fields = *item.find("properties");
            auto evidence = *fields.find("evidence");
            auto strings = json::Value::object(); strings.set("type", json::Value::string("string"));
            strings.set("enum", quotes); evidence.set("items", std::move(strings));
            fields.set("evidence", std::move(evidence)); item.set("properties", std::move(fields));
            return item;
        };
        auto groups = *properties.find("choice_groups");
        if (groups.find("items")) {
            auto group = *groups.find("items");
            auto fields = *group.find("properties");
            auto alternatives = *fields.find("alternatives");
            alternatives.set("items", restrictEvidence(*alternatives.find("items")));
            fields.set("alternatives", std::move(alternatives)); group.set("properties", std::move(fields));
            groups.set("items", std::move(group)); properties.set("choice_groups", std::move(groups));
        }
        auto assumptions = *properties.find("assumptions");
        if (assumptions.find("items")) {
            assumptions.set("items", restrictEvidence(*assumptions.find("items")));
            properties.set("assumptions", std::move(assumptions));
        }
        branch.set("properties", std::move(properties)); branches.push(std::move(branch));
    }
    schema.set("oneOf", std::move(branches));
    auto result = json::serialize(schema);
    // The transport's existing schema bound remains authoritative.
    return result.size() <= 64 * 1024 ? result : discoveryResponseSchema();
}

Result<GraniteOptionDiscoveryResponse> parseDiscoveryResponse(std::string_view jsonResponse,
                                                              std::string_view sessionId,
                                                              std::string_view baseRevisionId,
                                                              std::string_view frozenPromptText) {
    if (baseRevisionId.empty()) {
        return makeError(ErrorCode::InvalidInput, "Discovery response cannot be bound to an empty revision ID");
    }
    if (jsonResponse.size() > kMaxDiscoveryResponseBytes) {
        return makeError(ErrorCode::InvalidInput, "Model response of " + std::to_string(jsonResponse.size()) +
                                                      " bytes exceeds the 64 KB limit");
    }

    auto envelope = normalizeModelResponseEnvelope(jsonResponse, kMaxDiscoveryResponseBytes);
    if (!envelope.ok()) return envelope.error();
    auto parsed = json::parse(envelope.value(), json::ParseLimits{kMaxDiscoveryResponseBytes, 16});
    if (!parsed.ok()) {
        return parsed.error();
    }
    const json::Value& root = parsed.value();
    if (!root.isObject()) {
        return makeError(ErrorCode::InvalidInput, "Model response root must be a JSON object");
    }

    if (const json::Value* err = root.find("error"); err != nullptr && !err->isNull()) {
        const std::string message = err->isString() ? err->asString() : std::string("unspecified error");
        return makeError(ErrorCode::ProviderFailed, "Model reported an error: " + message);
    }

    auto status = readText(root, "status", true);
    if (!status.ok()) {
        return status.error();
    }

    GraniteOptionDiscoveryResponse response;
    if (status.value() == "options_generated") {
        response.outcome = DiscoveryOutcome::OptionsGenerated;
    } else if (status.value() == "no_ambiguity") {
        response.outcome = DiscoveryOutcome::NoAmbiguity;
    } else if (status.value() == "needs_clarification") {
        response.outcome = DiscoveryOutcome::NeedsClarification;
    } else {
        return makeError(ErrorCode::InvalidInput, "Unknown status '" + status.value() + "'");
    }

    response.assistanceKind = response.outcome == DiscoveryOutcome::NoAmbiguity
        ? core::analysis::AssistanceKind::None : core::analysis::AssistanceKind::Clarification;
    if (root.find("assistance_kind")) {
        auto kind = readText(root, "assistance_kind", true);
        if (!kind.ok()) return kind.error();
        using Kind = core::analysis::AssistanceKind;
        if (kind.value() == "none") response.assistanceKind = Kind::None;
        else if (kind.value() == "clarification") response.assistanceKind = Kind::Clarification;
        else if (kind.value() == "creative") response.assistanceKind = Kind::Creative;
        else if (kind.value() == "refinement") response.assistanceKind = Kind::Refinement;
        else if (kind.value() == "decomposition") response.assistanceKind = Kind::Decomposition;
        else return makeError(ErrorCode::InvalidInput, "Unknown assistance kind");
        if ((response.outcome == DiscoveryOutcome::NoAmbiguity) != (response.assistanceKind == Kind::None))
            return makeError(ErrorCode::InvalidInput, "Assistance kind contradicts status");
    }

    auto groups = readArray(root, "choice_groups", kMaxDiscoveryGroups);
    if (!groups.ok()) {
        return groups.error();
    }
    if (response.outcome != DiscoveryOutcome::OptionsGenerated && !groups.value()->empty()) {
        return makeError(ErrorCode::InvalidInput,
                         "Status '" + status.value() + "' must not carry choice groups");
    }

    core::analysis::NormalizedAnalysis analysis;
    analysis.sessionId = std::string(sessionId);
    analysis.baseRevisionId = std::string(baseRevisionId);

    std::size_t ungrounded = 0;
    for (std::size_t i = 0; i < groups.value()->size(); ++i) {
        auto group = readGroup((*groups.value())[i], i, frozenPromptText, ungrounded);
        if (!group.ok()) {
            return group.error();
        }
        analysis.choiceGroups.push_back(std::move(group).value());
    }

    auto assumptions = readArray(root, "assumptions", kMaxDiscoveryItemsPerKind);
    if (!assumptions.ok()) return assumptions.error();
    for (std::size_t i = 0; i < assumptions.value()->size(); ++i) {
        const auto& a = (*assumptions.value())[i];
        if (!a.isObject()) return makeError(ErrorCode::InvalidInput, "assumptions entries must be objects");
        core::analysis::Assumption assumption;
        assumption.id = core::analysis::mintAssumptionId(i);
        auto statement = readText(a, "statement", true);
        if (!statement.ok()) return statement.error();
        assumption.statement = std::move(statement).value();
        auto material = readFlag(a, "material", false);
        if (!material.ok()) return material.error();
        assumption.material = material.value();
        auto evidence = readEvidence(a, frozenPromptText, ungrounded);
        if (!evidence.ok()) return evidence.error();
        assumption.evidence = std::move(evidence).value();
        analysis.assumptions.push_back(std::move(assumption));
    }

    auto missing = readArray(root, "missing_decisions", kMaxDiscoveryItemsPerKind);
    if (!missing.ok()) return missing.error();
    for (std::size_t i = 0; i < missing.value()->size(); ++i) {
        const auto& m = (*missing.value())[i];
        if (!m.isObject()) return makeError(ErrorCode::InvalidInput, "missing_decisions entries must be objects");
        core::analysis::MissingDecision md;
        md.id = core::analysis::mintMissingDecisionId(i);
        auto question = readText(m, "question", true);
        if (!question.ok()) return question.error();
        md.question = std::move(question).value();
        auto blocking = readFlag(m, "blocking", false);
        if (!blocking.ok()) return blocking.error();
        md.blocking = blocking.value();
        auto suggestion = readText(m, "suggested_alternative_evidence", false);
        if (!suggestion.ok()) return suggestion.error();
        md.suggestedAlternativeEvidence = trimModelWhitespace(suggestion.value());
        if (md.suggestedAlternativeEvidence.size() > 1024)
            return makeError(ErrorCode::InvalidInput, "Field 'suggested_alternative_evidence' exceeds its 1024-byte limit");
        analysis.missingDecisions.push_back(std::move(md));
    }

    if (response.outcome == DiscoveryOutcome::NeedsClarification) {
        const json::Value* cqVal = root.find("clarification_question");
        if (cqVal == nullptr || cqVal->isNull()) {
            return makeError(ErrorCode::InvalidInput, "Missing required field 'clarification_question'");
        }
        if (!cqVal->isString()) {
            return makeError(ErrorCode::InvalidInput, "Field 'clarification_question' must be a string");
        }
        std::string text = core::normalizeLineEndings(trimModelWhitespace(cqVal->asString()));
        if (text.empty()) {
            return makeError(ErrorCode::InvalidInput, "Field 'clarification_question' must not be blank");
        }
        core::analysis::MissingDecision md;
        md.id = core::analysis::mintMissingDecisionId(analysis.missingDecisions.size());
        md.question = std::move(text);
        md.blocking = true;
        analysis.missingDecisions.push_back(std::move(md));
    }

    auto conflicts = readArray(root, "conflicts", kMaxDiscoveryItemsPerKind);
    if (!conflicts.ok()) return conflicts.error();
    for (std::size_t i = 0; i < conflicts.value()->size(); ++i) {
        const auto& c = (*conflicts.value())[i];
        if (!c.isObject()) return makeError(ErrorCode::InvalidInput, "conflicts entries must be objects");
        core::analysis::Conflict conflict;
        conflict.id = core::analysis::mintConflictId(i);
        auto description = readText(c, "description", true);
        if (!description.ok()) return description.error();
        conflict.description = std::move(description).value();
        auto blocking = readFlag(c, "blocking", true);
        if (!blocking.ok()) return blocking.error();
        conflict.blocking = blocking.value();
        analysis.conflicts.push_back(std::move(conflict));
    }

    if (core::analysis::isOptionalAssistance(response.assistanceKind) &&
        (analysis.choiceGroups.size() != 1 || !analysis.choiceGroups.front().material || !analysis.assumptions.empty() ||
         !analysis.missingDecisions.empty() || !analysis.conflicts.empty()))
        return makeError(ErrorCode::InvalidInput, "Optional help cannot conceal unresolved requirements");
    const bool emptyAnalysis = analysis.choiceGroups.empty() && analysis.assumptions.empty() &&
        analysis.missingDecisions.empty() && analysis.conflicts.empty();
    if (!root.find("assistance_kind") && !emptyAnalysis)
        response.assistanceKind = core::analysis::AssistanceKind::Clarification;
    if (root.find("assistance_kind") && emptyAnalysis != (response.assistanceKind == core::analysis::AssistanceKind::None))
        return makeError(ErrorCode::InvalidInput, "Assistance kind does not match review contents");
    core::analysis::sealSnapshot(analysis);
    response.analysis = std::move(analysis);
    response.ungroundedEvidenceCount = ungrounded;
    return response;
}

GraniteAdapter::BusyLease::BusyLease(BusyLease&& other) noexcept
    : owner_(other.owner_), failed_(other.failed_), reason_(std::move(other.reason_)) {
    other.owner_ = nullptr;
}

GraniteAdapter::BusyLease::~BusyLease() {
    if (owner_ != nullptr) {
        owner_->release(failed_, std::move(reason_));
    }
}

void GraniteAdapter::BusyLease::markFailed(std::string reason) {
    failed_ = true;
    reason_ = std::move(reason);
}

GraniteModelInfo GraniteAdapter::modelInfo() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return info_;
}

std::string GraniteAdapter::lastError() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return lastError_;
}

bool GraniteAdapter::beginLoading() {
    auto expected = GraniteRuntimeState::Unloaded;
    if (state_.compare_exchange_strong(expected, GraniteRuntimeState::Loading)) {
        return true;
    }
    expected = GraniteRuntimeState::Failed;
    return state_.compare_exchange_strong(expected, GraniteRuntimeState::Loading);
}

bool GraniteAdapter::completeLoading(GraniteModelInfo info) {
    if (state_.load() != GraniteRuntimeState::Loading) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        info_ = std::move(info);
        lastError_.clear();
    }
    auto expected = GraniteRuntimeState::Loading;
    return state_.compare_exchange_strong(expected, GraniteRuntimeState::Ready);
}

void GraniteAdapter::failLoading(std::string reason) {
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        lastError_ = std::move(reason);
    }
    auto expected = GraniteRuntimeState::Loading;
    (void)state_.compare_exchange_strong(expected, GraniteRuntimeState::Failed);
}

bool GraniteAdapter::recover() {
    auto expected = GraniteRuntimeState::Failed;
    return state_.compare_exchange_strong(expected, GraniteRuntimeState::Unloaded);
}

bool GraniteAdapter::cancelLoading() {
    auto expected = GraniteRuntimeState::Loading;
    if (!state_.compare_exchange_strong(expected, GraniteRuntimeState::Unloaded)) return false;
    std::lock_guard lock(infoMutex_);
    info_ = GraniteModelInfo{};
    lastError_.clear();
    return true;
}

bool GraniteAdapter::unload() {
    auto expected = GraniteRuntimeState::Ready;
    if (state_.compare_exchange_strong(expected, GraniteRuntimeState::Unloaded)) {
        std::lock_guard lock(infoMutex_);
        info_ = GraniteModelInfo{};
        return true;
    }
    expected = GraniteRuntimeState::Failed;
    return state_.compare_exchange_strong(expected, GraniteRuntimeState::Unloaded);
}

std::optional<GraniteAdapter::BusyLease> GraniteAdapter::tryAcquire() {
    auto expected = GraniteRuntimeState::Ready;
    if (!state_.compare_exchange_strong(expected, GraniteRuntimeState::Busy)) {
        return std::nullopt;
    }
    return BusyLease(this);
}

void GraniteAdapter::release(bool failed, std::string reason) {
    if (failed) {
        std::lock_guard<std::mutex> lock(infoMutex_);
        lastError_ = std::move(reason);
    }
    state_.store(failed ? GraniteRuntimeState::Failed : GraniteRuntimeState::Ready);
}

Result<GraniteOptionDiscoveryResponse> GraniteAdapter::discoverOptions(const GraniteOptionDiscoveryRequest& req,
                                                                       const GraniteTransport& transport) {
    if (req.reviewTrigger != core::analysis::ReviewTrigger::Automatic &&
        req.reviewTrigger != core::analysis::ReviewTrigger::ExplicitReview)
        return makeError(ErrorCode::InvalidInput, "Unknown review trigger");
    if (!transport) {
        return makeError(ErrorCode::ProviderUnavailable, "No Granite transport is configured");
    }
    if (req.revisionId.empty() || req.sessionId.empty()) {
        return makeError(ErrorCode::InvalidInput, "Discovery request must name a session and frozen revision");
    }
    if (!core::hasCanonicalLineEndings(req.promptString)) {
        return makeError(ErrorCode::InvalidInput, "Discovery request text must use canonical LF line endings");
    }
    if (req.promptTriage) {
        const auto route = core::analysis::routePromptTriage(*req.promptTriage);
        if (!route.ok() || (req.reviewTrigger != core::analysis::ReviewTrigger::ExplicitReview &&
            route.value() != core::analysis::PromptTriageRoute::Clarify) ||
            req.promptTriage->sessionId != req.sessionId || req.promptTriage->revisionId != req.revisionId ||
            req.promptTriage->revisionTextHash != core::crypto::Sha256::hexDigest(req.promptString))
            return makeError(ErrorCode::ValidationFailed, "Granite discovery requires bound S1 evidence and an eligible review trigger");
    }

    auto lease = tryAcquire();
    if (!lease.has_value()) {
        return makeError(ErrorCode::ProviderUnavailable,
                         "Granite runtime is " + std::string(toString(state())) + "; request rejected");
    }

    const auto start = std::chrono::steady_clock::now();

    // Deterministic pre-model guard: same-target explicit contradictions do not
    // need a 2-8 second Granite call and must never be reported as no_ambiguity.
    const auto preflight = core::rewrite::StructuredEntityExtractor::analyze(req.promptString);
    if (!preflight.detectedExplicitConflicts.empty()) {
        std::vector<core::analysis::Conflict> conflicts;
        bool resolvable = true;
        for (const auto& detected : preflight.detectedExplicitConflicts) {
            if (conflicts.size() >= kMaxDiscoveryItemsPerKind) break;
            const auto first = core::analysis::findUniqueSpan(req.promptString, detected.requirementA);
            const auto second = core::analysis::findUniqueSpan(req.promptString, detected.requirementB);
            if (first.status != core::analysis::SpanMatchStatus::ExactMatch || !first.span.has_value() ||
                second.status != core::analysis::SpanMatchStatus::ExactMatch || !second.span.has_value()) {
                // An ambiguous or missing source span falls through to the model rather
                // than fabricating a binding to text the host cannot prove.
                resolvable = false;
                break;
            }
            core::analysis::Conflict conflict;
            conflict.id = core::analysis::mintConflictId(conflicts.size());
            conflict.description = "Conflicting requirements for " + detected.subject + ": \"" +
                detected.requirementA.substr(0, 512) + "\" and \"" + detected.requirementB.substr(0, 512) +
                "\". Choose which requirement should apply.";
            conflict.blocking = true;
            conflicts.push_back(std::move(conflict));
        }
        if (resolvable && !conflicts.empty()) {
            core::analysis::NormalizedAnalysis analysis;
            analysis.sessionId = req.sessionId;
            analysis.baseRevisionId = req.revisionId;
            analysis.conflicts = std::move(conflicts);
            core::analysis::sealSnapshot(analysis);

            GraniteOptionDiscoveryResponse response;
            response.outcome = DiscoveryOutcome::OptionsGenerated;
            response.analysis = std::move(analysis);
            response.assistanceKind = core::analysis::AssistanceKind::Clarification;
            response.model = modelInfo();
            response.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            return response;
        }
    }

    Result<std::string> raw = makeError(ErrorCode::Internal, "Transport did not run");
    try {
        raw = transport(buildDiscoveryPrompt(req));
    } catch (const std::exception& ex) {
        raw = makeError(ErrorCode::ProviderFailed, std::string("Transport threw: ") + ex.what());
    }
    if (!raw.ok()) {
        if (raw.error().code != core::ErrorCode::StaleVersion && raw.error().code != core::ErrorCode::PreconditionFailed &&
            raw.error().code != core::ErrorCode::ValidationFailed) lease->markFailed(raw.error().message);
        return raw.error();
    }

    auto parsed = parseDiscoveryResponse(raw.value(), req.sessionId, req.revisionId, req.promptString);
    if (!parsed.ok()) {
        return parsed.error();
    }
    auto response = std::move(parsed).value();
    if (req.promptTriage && response.outcome != DiscoveryOutcome::NoAmbiguity &&
        response.analysis.choiceGroups.empty() && response.analysis.assumptions.empty() &&
        response.analysis.missingDecisions.empty() && response.analysis.conflicts.empty()) {
        // Empty options are inconsistent, but an explicit no_ambiguity result is
        // valid advisory disagreement. Neither result authorizes a rewrite.
        return makeError(ErrorCode::ValidationFailed,
            "Granite did not supply any grounded clarification choices for the ambiguity flagged by Julia. "
            "Edit the draft or retry Analyze. Send as-is still keeps your original wording.");
    }
    response.model = modelInfo();
    response.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return response;
}

} // namespace roclint::bridge
