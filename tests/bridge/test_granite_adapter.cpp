#include "roclint/bridge/WordSurfaceCheck.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include "roclint/core/Json.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "support/check.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace roclint::core;
using namespace roclint::bridge;

namespace {

const std::string kPrompt = "Update the timeout handling in the session layer.";
constexpr const char* kSession = "sess_g";
constexpr const char* kRevision = "rev_sess_g_1";

Result<GraniteOptionDiscoveryResponse> parse(std::string_view json, std::string_view text = kPrompt) {
    return parseDiscoveryResponse(json, kSession, kRevision, text);
}

const char* kTwoGroupResponse = R"({
  "status": "options_generated",
  "choice_groups": [
    {"id": "provider_g", "dimension": "scope", "question": "Which timeout?", "material": true,
     "alternatives": [
       {"id": "provider_a", "label": "Connect timeout", "why_material": "Changes dial behaviour", "evidence": ["timeout handling"]},
       {"id": "provider_b", "label": "Idle timeout", "why_material": "Changes keep-alive", "evidence": ["timeout handling"]}
     ]}
  ],
  "assumptions": [{"statement": "There is one session layer", "material": true, "evidence": ["session layer"]}],
  "missing_decisions": [{"question": "What should the new value be?", "blocking": true,
    "suggested_alternative_evidence": "A timeout configuration excerpt would resolve this."}],
  "conflicts": []
})";

void testReadableDiscoveryChoices() {
    const auto result = parse(R"({"kind":"clarification","groups":[{"focus":"controls","question":"Which controls?","source":"game","options":[{"text":"gameplay_mechanics","effect":"define_core_actions","assume":""},{"text":"touch_controls","effect":"Use the named setting","assume":""}]}]})",
                              "Make a game using touch_controls.");
    ROCLINT_CHECK(result.ok());
    const auto& options = result.value().analysis.choiceGroups.at(0).alternatives;
    ROCLINT_CHECK(options.at(0).interpretation == "gameplay mechanics");
    ROCLINT_CHECK(options.at(0).whyMaterial == "define core actions");
    ROCLINT_CHECK(options.at(1).interpretation == "touch_controls");
}

void testParsesAndMintsHostIds() {
    auto r = parse(kTwoGroupResponse);
    ROCLINT_CHECK(r.ok());
    const auto& resp = r.value();
    ROCLINT_CHECK(resp.outcome == DiscoveryOutcome::OptionsGenerated);
    const auto& a = resp.analysis;
    ROCLINT_CHECK(a.sessionId == kSession);
    ROCLINT_CHECK(a.baseRevisionId == kRevision);
    ROCLINT_CHECK(roclint::core::analysis::isSnapshotSealed(a));
    ROCLINT_CHECK(a.choiceGroups.size() == 1);
    // Provider IDs are ignored.
    ROCLINT_CHECK(a.choiceGroups[0].id == "grp_0");
    ROCLINT_CHECK(a.choiceGroups[0].alternatives[0].id == "alt_0_0");
    ROCLINT_CHECK(a.choiceGroups[0].alternatives[1].id == "alt_0_1");
    ROCLINT_CHECK(a.assumptions[0].id == "asm_0");
    ROCLINT_CHECK(a.missingDecisions[0].id == "mdc_0");
    ROCLINT_CHECK(a.missingDecisions[0].blocking);
    ROCLINT_CHECK(a.missingDecisions[0].suggestedAlternativeEvidence ==
                  "A timeout configuration excerpt would resolve this.");
    // Evidence resolved to exact byte spans of the frozen text.
    const auto& span = a.choiceGroups[0].alternatives[0].evidence.at(0);
    ROCLINT_CHECK(kPrompt.substr(span.startByte, span.endByte - span.startByte) == "timeout handling");
    ROCLINT_CHECK(resp.ungroundedEvidenceCount == 0);
}

void testUngroundedEvidenceIsDroppedNotApproximated() {
    auto r = parse(R"({"status": "options_generated", "choice_groups": [
        {"question": "Q?", "alternatives": [
            {"label": "A", "why_material": "w", "evidence": ["Timeout Handling"]},
            {"label": "B", "why_material": "w", "evidence": ["not in the prompt"]}]}]})");
    ROCLINT_CHECK(r.ok());
    ROCLINT_CHECK(r.value().ungroundedEvidenceCount == 2);
    for (const auto& alt : r.value().analysis.choiceGroups[0].alternatives) {
        ROCLINT_CHECK(alt.evidence.empty());
    }
}

void testUnicodeEscapesDecodeBeforeGrounding() {
    const std::string text = "Translate the caf\xc3\xa9 menu.";
    // Built without raw strings: MSVC expands universal-character-names even inside R"(...)".
    const std::string escaped = "caf\\u00e9 menu";
    const std::string json = "{\"status\": \"options_generated\", \"choice_groups\": [{\"question\": \"Which language?\", "
                             "\"alternatives\": [{\"label\": \"French\", \"why_material\": \"w\", \"evidence\": [\"" +
                             escaped +
                             "\"]}, {\"label\": \"English\", \"why_material\": \"w\", \"evidence\": [\"" + escaped +
                             "\"]}]}]}";
    ROCLINT_CHECK(json.find("\\u00e9") != std::string::npos);
    auto r = parse(json, text);
    ROCLINT_CHECK(r.ok());
    ROCLINT_CHECK(r.value().ungroundedEvidenceCount == 0);
    const auto& span = r.value().analysis.choiceGroups[0].alternatives[0].evidence.at(0);
    ROCLINT_CHECK(text.substr(span.startByte, span.endByte - span.startByte) == "caf\xc3\xa9 menu");
}

void testRejectsTruncatedAndMalformedOutput() {
    ROCLINT_CHECK(!parse(R"({"status": "options_generated", "choice_groups": [)").ok());
    ROCLINT_CHECK(!parse("Sure! Here is the JSON: {}").ok());
    ROCLINT_CHECK(!parse(R"(["status"])").ok());
    ROCLINT_CHECK(!parse(R"({"status": "maybe"})").ok());
    ROCLINT_CHECK(!parse(R"({"choice_groups": []})").ok()); // status missing
    ROCLINT_CHECK(!parse(R"({"status": "options_generated", "choice_groups": [{"question": "Q?"}]})").ok());
    ROCLINT_CHECK(!parse(R"({"status": "options_generated", "choice_groups": [
        {"question": "Q?", "material": "yes", "alternatives": []}]})").ok());
    ROCLINT_CHECK(!parse(R"({"status": "no_ambiguity", "status": "no_ambiguity"})").ok());
}

void testResponseEnvelopes() {
    const std::string valid = R"({"status":"no_ambiguity"})";
    for (const auto& opening : {std::string("```json\n"), std::string("```\r\n")})
        ROCLINT_CHECK(parse(" \t" + opening + valid + "\r\n```\n").ok());
    for (const auto& output : {"Here: " + valid, valid + " done", "```json\n" + valid,
            "```python\n" + valid + "\n```", "```json\n" + valid + "\n```\n```\n{}\n```",
            std::string(R"({"status":null})"), std::string(R"({"status":""})"),
            std::string(R"({"status":42})"), std::string(R"({"status":"success"})")})
        ROCLINT_CHECK(!parse(output).ok());
    ROCLINT_CHECK(!parse("```json\n" + valid + std::string(kMaxDiscoveryResponseBytes, ' ') + "\n```").ok());
}

void testRejectsOversizedResponse() {
    std::string big = R"({"status": "no_ambiguity", "pad": ")";
    big += std::string(kMaxDiscoveryResponseBytes, 'x');
    big += "\"}";
    auto r = parse(big);
    ROCLINT_CHECK(!r.ok());
    ROCLINT_CHECK(r.error().code == ErrorCode::InvalidInput);
}

void testDiscoveryGroupLimitMatchesProductContract() {
    const std::string group = R"({"question":"Which timeout?","alternatives":[
        {"label":"Connect","why_material":"Dial behaviour","evidence":["timeout handling"]},
        {"label":"Idle","why_material":"Keep-alive","evidence":["timeout handling"]}]})";
    std::string groups;
    for (std::size_t count = 1; count <= 4; ++count) {
        if (!groups.empty()) groups += ',';
        groups += group;
        const auto result = parse("{\"status\":\"options_generated\",\"choice_groups\":[" + groups + "]}");
        if (count <= 3) {
            ROCLINT_CHECK(result.ok());
        } else {
            ROCLINT_CHECK(!result.ok());
            ROCLINT_CHECK(result.error().code == ErrorCode::InvalidInput);
        }
    }
}

void testStatusRules() {
    auto clear = parse(R"({"status": "no_ambiguity", "choice_groups": []})");
    ROCLINT_CHECK(clear.ok());
    ROCLINT_CHECK(clear.value().outcome == DiscoveryOutcome::NoAmbiguity);
    ROCLINT_CHECK(clear.value().analysis.choiceGroups.empty());

    // needs_clarification may not carry groups.
    ROCLINT_CHECK(!parse(R"({"status": "needs_clarification", "clarification_question": "What?",
        "choice_groups": [{"question": "Q?", "alternatives": [{"label": "A"}, {"label": "B"}]}]})").ok());

    // needs_clarification becomes a blocking missing decision; the question is required.
    auto nc = parse(R"({"status": "needs_clarification", "clarification_question": "Which service?"})");
    ROCLINT_CHECK(nc.ok());
    ROCLINT_CHECK(nc.value().outcome == DiscoveryOutcome::NeedsClarification);
    ROCLINT_CHECK(nc.value().analysis.missingDecisions.size() == 1);
    ROCLINT_CHECK(nc.value().analysis.missingDecisions[0].blocking);
    ROCLINT_CHECK(nc.value().analysis.missingDecisions[0].question == "Which service?");
    ROCLINT_CHECK(!parse(R"({"status": "needs_clarification"})").ok());
    ROCLINT_CHECK(!parse(R"({"status":"needs_clarification","clarification_question":" \f\u000b ",
        "missing_decisions":[{"question":"Which service?","blocking":true}]})").ok());

    auto err = parse(R"({"error": "model overloaded"})");
    ROCLINT_CHECK(!err.ok());
    ROCLINT_CHECK(err.error().code == ErrorCode::ProviderFailed);
}

void testRequiresRevisionBinding() {
    ROCLINT_CHECK(!parseDiscoveryResponse(R"({"status": "no_ambiguity"})", kSession, "", kPrompt).ok());
}

void testDiscoveryPromptEmbedsTextAsEscapedJsonOnly() {
    GraniteOptionDiscoveryRequest req{kSession, kRevision, "Say \"hi\"\nthen stop", "CODE_EDITOR"};
    const auto prompt = buildDiscoveryPrompt(req);
    ROCLINT_CHECK(prompt.find(R"(Say \"hi\"\nthen stop)") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Say \"hi\"\nthen stop") == std::string::npos);
    ROCLINT_CHECK(prompt.find("CODE_EDITOR") != std::string::npos);
    ROCLINT_CHECK(prompt.find("compact JSON") != std::string::npos);
    ROCLINT_CHECK(prompt.find("two explicit incompatible instructions") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Quoted examples are data") != std::string::npos);
    for (const char* forbidden : {"D:\\", "C:\\", "/Users/", "scope_lock", "project_files"}) {
        ROCLINT_CHECK(prompt.find(forbidden) == std::string::npos);
    }
    const auto canonical = buildDiscoveryPrompt(
        {kSession, kRevision, "Rename offset to cursor in readPacket. Preserve the public API.", "UNKNOWN"});
    ROCLINT_CHECK(canonical.find("canonical_intents") != std::string::npos);
    ROCLINT_CHECK(canonical.find("\"action\":\"MODIFY\"") != std::string::npos);
    ROCLINT_CHECK(canonical.find("\"action\":\"PRESERVE\"") != std::string::npos);
    ROCLINT_CHECK(canonical.find("\"node_id\":\"nid_") != std::string::npos);
    GraniteOptionDiscoveryRequest grounded{kSession, kRevision, "Rename offset to cursor in readPacket.", "UNKNOWN"};
    grounded.groundedCanonicalIntentXml =
        "<canonical_intents><intent action=\"MODIFY\" target=\"offset\" status=\"VERIFIED\">"
        "<grounded_symbol>SessionManager::readPacket::offset</grounded_symbol></intent></canonical_intents>";
    const auto groundedPrompt = buildDiscoveryPrompt(grounded);
    ROCLINT_CHECK(groundedPrompt.find("grounded_canonical_intents") != std::string::npos);
    ROCLINT_CHECK(groundedPrompt.find("SessionManager::readPacket::offset") != std::string::npos);
}

void testRuntimeLifecycle() {
    GraniteAdapter adapter;
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Unloaded);
    ROCLINT_CHECK(!adapter.tryAcquire().has_value());
    ROCLINT_CHECK(!adapter.completeLoading({}));
    ROCLINT_CHECK(adapter.beginLoading());
    ROCLINT_CHECK(!adapter.beginLoading());
    ROCLINT_CHECK(!adapter.unload()); // rejected while Loading
    ROCLINT_CHECK(adapter.cancelLoading());
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Unloaded);
    ROCLINT_CHECK(adapter.modelInfo().modelId.empty());
    ROCLINT_CHECK(!adapter.completeLoading(GraniteModelInfo{"stale", "", "", ""}));
    ROCLINT_CHECK(!adapter.cancelLoading());
    ROCLINT_CHECK(adapter.beginLoading());
    ROCLINT_CHECK(adapter.completeLoading(GraniteModelInfo{"granite", "sha256:abc", "Q8_0", kLoopbackOllamaTransport}));
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    ROCLINT_CHECK(adapter.modelInfo().digest == "sha256:abc");
    {
        auto lease = adapter.tryAcquire();
        ROCLINT_CHECK(lease.has_value());
        ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Busy);
        ROCLINT_CHECK(!adapter.tryAcquire().has_value());
        ROCLINT_CHECK(!adapter.unload()); // rejected while Busy
        ROCLINT_CHECK(!adapter.cancelLoading());
    }
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    ROCLINT_CHECK(adapter.unload());
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Unloaded);
    ROCLINT_CHECK(adapter.modelInfo().digest.empty());

    ROCLINT_CHECK(adapter.beginLoading());
    adapter.failLoading("model not found");
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Failed);
    ROCLINT_CHECK(adapter.lastError() == "model not found");
    ROCLINT_CHECK(adapter.recover());
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Unloaded);
}

void makeReady(GraniteAdapter& adapter) {
    ROCLINT_CHECK(adapter.beginLoading());
    ROCLINT_CHECK(adapter.completeLoading(GraniteModelInfo{"granite", "sha256:abc", "Q8_0", kLoopbackOllamaTransport}));
}

void testExplicitColorBoundsRejectGeneratedReversals() {
    auto check = [](const std::string& prompt, const std::string& evidence, const std::string& label) {
        analysis::ChoiceAlternative alternative;
        alternative.interpretation = label;
        const auto start = prompt.find(evidence);
        ROCLINT_CHECK(start != std::string::npos);
        alternative.evidence.push_back({start, start + evidence.size()});
        return explicitColorConstraintViolation(prompt, alternative).has_value();
    };
    const std::string prompt = "The dog will be black but it cant be blue.";
    ROCLINT_CHECK(check(prompt, "blue", "Blue dog"));
    ROCLINT_CHECK(check(prompt, "blue", "Blue tones allowed with restrictions"));
    ROCLINT_CHECK(check(prompt, "blue", "Only certain shades of blue are prohibited"));
    ROCLINT_CHECK(!check(prompt, "blue", "No blue parts allowed"));
    ROCLINT_CHECK(!check(prompt, "blue", "Blue colors completely disallowed"));
    ROCLINT_CHECK(!check(prompt, "blue", "Any blue coloration is not permitted"));
    ROCLINT_CHECK(check(prompt, "black", "Dark brown or near-black shade"));
    ROCLINT_CHECK(check(prompt, "black", "Not black but blue"));
    ROCLINT_CHECK(!check(prompt, "black", "Black dog without blue"));
    ROCLINT_CHECK(!check(prompt, "dog", "Blue dog")); // No evidence on the fixed property: no inferred lock.
    ROCLINT_CHECK(!check("Summarize this quoted note: \"The dog cannot be blue.\"", "blue", "Blue dog"));
    ROCLINT_CHECK(!check("Summarize this quoted note: 'The dog cannot be blue.'", "blue", "Blue dog"));
    ROCLINT_CHECK(check("The icon must not be red.", "red", "Red icon"));
    ROCLINT_CHECK(check("The icon cannot be green.", "green", "Green icon"));
    ROCLINT_CHECK(check("The icon must be orange.", "orange", "Purple icon"));
    ROCLINT_CHECK(!check("The API will be black-box tested.", "black", "White-box testing"));
    analysis::ChoiceAlternative scope;
    scope.interpretation = "Any blue coloration on the dog";
    const auto blue = prompt.find("blue");
    scope.evidence.push_back({blue, blue + 4});
    ROCLINT_CHECK(!explicitColorConstraintViolation(prompt, scope, "What does blue prohibit?"));
    scope.interpretation = "Only certain shades of blue are prohibited";
    ROCLINT_CHECK(explicitColorConstraintViolation(prompt, scope, "What does blue prohibit?"));
}

void testDiscoverOptionsThroughTransport() {
    GraniteOptionDiscoveryRequest req{kSession, kRevision, kPrompt, "UNKNOWN"};

    GraniteAdapter none;
    ROCLINT_CHECK(!none.discoverOptions(req, GraniteTransport{}).ok());

    // Not loaded: rejected without calling the transport.
    bool called = false;
    auto notReady = none.discoverOptions(req, [&](std::string_view) -> Result<std::string> {
        called = true;
        return std::string(kTwoGroupResponse);
    });
    ROCLINT_CHECK(!notReady.ok());
    ROCLINT_CHECK(notReady.error().code == ErrorCode::ProviderUnavailable);
    ROCLINT_CHECK(!called);
}

void testAdvisoryClassifierDisagreementPreservesOriginal() {
    GraniteAdapter adapter;
    makeReady(adapter);
    GraniteOptionDiscoveryRequest request{kSession, kRevision, kPrompt, "UNKNOWN"};
    analysis::PromptTriage triage;
    triage.sessionId = kSession; triage.revisionId = kRevision;
    triage.revisionTextHash = crypto::Sha256::hexDigest(kPrompt);
    triage.stateVersion = 1; triage.requestGroupId = "grp_test"; triage.attemptId = "att_test";
    triage.modelId = "test_fixture"; triage.modelRevision = "test_fixture";
    triage.weightsSha256 = std::string(64, 'a'); triage.configurationId = "test_fixture";
    triage.execution = analysis::TriageExecution::Mock;
    triage.categoryScores = {0.8, 0.1, 0.1, 0.1};
    auto sealed = analysis::sealPromptTriage(triage, kPrompt);
    ROCLINT_CHECK(sealed.ok());
    request.promptTriage = sealed.value();
    const GraniteTransport empty = [](std::string_view) -> Result<std::string> {
        return std::string(R"({"status":"no_ambiguity","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""})");
    };
    const auto clear = adapter.discoverOptions(request, empty);
    ROCLINT_CHECK(clear.ok() && clear.value().outcome == DiscoveryOutcome::NoAmbiguity);
    ROCLINT_CHECK(clear.value().analysis.choiceGroups.empty());
    const auto rejected = adapter.discoverOptions(request, [](std::string_view) -> Result<std::string> {
        return std::string(R"({"status":"options_generated","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""})");
    });
    ROCLINT_CHECK(!rejected.ok() && rejected.error().code == ErrorCode::ValidationFailed);
    ROCLINT_CHECK(rejected.error().message.find("retry Analyze") != std::string::npos);
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    const auto retry = adapter.discoverOptions(request, [](std::string_view) -> Result<std::string> {
        return std::string(kTwoGroupResponse);
    });
    ROCLINT_CHECK(retry.ok() && !retry.value().analysis.choiceGroups.empty());
    request.promptTriage.reset(); // Retain the historical non-routed parser contract.
    ROCLINT_CHECK(adapter.discoverOptions(request, empty).ok());
}

void testSelectiveReviewContract() {
    for (const auto& text : {std::string("Give the landing page a friendlier vibe."),
            std::string("cool cool; caf\xc3\xa9 and \"quoted\" text."), std::string(64000, 'x') + " unique-tail"}) {
        const auto boundSchema = discoveryResponseSchema(text);
        ROCLINT_CHECK(boundSchema.size() <= 65536);
        const auto bound = json::parse(boundSchema);
        ROCLINT_CHECK(bound.ok());
        const auto& branch = bound.value().find("oneOf")->asArray()[1];
        const auto* strings = branch.find("properties")->find("groups")->find("items")->find("properties")->find("source");
        ROCLINT_CHECK(strings->find("enum") && strings->find("enum")->asArray().size() <= 64);
        for (const auto& quote : strings->find("enum")->asArray())
            ROCLINT_CHECK(analysis::findUniqueSpan(text, quote.asString()).status == analysis::SpanMatchStatus::ExactMatch);
        ROCLINT_CHECK(buildDiscoveryPrompt({kSession, kRevision, text}).find("evidence_quotes") != std::string::npos);
    }
    const auto schema = json::parse(discoveryResponseSchema());
    ROCLINT_CHECK(schema.ok() && schema.value().find("oneOf"));
    ROCLINT_CHECK(schema.value().find("oneOf")->asArray().size() == 9);
    bool clarificationRequiresContent = false;
    for (const auto& branch : schema.value().find("oneOf")->asArray()) {
        const auto* properties = branch.find("properties");
        const auto kind = properties->find("assistance_kind")->find("const")->asString();
        const auto status = properties->find("status")->find("const")->asString();
        if (kind != "clarification" || status != "options_generated") continue;
        for (const char* field : {"choice_groups", "assumptions", "missing_decisions", "conflicts"}) {
            const auto* value = properties->find(field);
            const auto* minItems = value->find("minItems");
            if (minItems != nullptr && minItems->asNumber() >= 1) clarificationRequiresContent = true;
        }
    }
    ROCLINT_CHECK(clarificationRequiresContent);
    for (const auto& branch : schema.value().find("oneOf")->asArray()) {
        const auto* properties = branch.find("properties");
        const auto kind = properties->find("assistance_kind")->find("const")->asString();
        if (kind == "creative" || kind == "refinement" || kind == "decomposition") {
            ROCLINT_CHECK(properties->find("choice_groups")->find("minItems")->asNumber() == 1);
            const auto* groups = properties->find("choice_groups");
            ROCLINT_CHECK(groups->find("maxItems")->asNumber() == 2);
            ROCLINT_CHECK(groups->find("items")->find("properties")->find("alternatives")->find("maxItems")->asNumber() == 2);
            for (const auto* field : {"assumptions", "missing_decisions", "conflicts"})
                ROCLINT_CHECK(properties->find(field)->find("const")->asArray().empty());
        }
    }
    using Kind = analysis::AssistanceKind;
    const auto creative = R"({"status":"options_generated","assistance_kind":"creative","choice_groups":[{"question":"Explore a direction?","alternatives":[{"label":"Bold","why_material":"Visual emphasis","evidence":["modern"]},{"label":"Restrained","why_material":"Visual emphasis","evidence":["modern"]}]}]})";
    const auto parsed = parse(creative, "Make it modern");
    ROCLINT_CHECK(parsed.ok() && parsed.value().assistanceKind == Kind::Creative);
    for (const auto* kind : {"unknown", "", "none"}) {
        std::string malformed(creative); malformed.replace(malformed.find("creative"), 8, kind);
        ROCLINT_CHECK(!parse(malformed, "Make it modern").ok());
    }
    for (const auto* value : {"null", "42"}) {
        std::string malformed(creative); malformed.replace(malformed.find("\"creative\""), 10, value);
        ROCLINT_CHECK(!parse(malformed, "Make it modern").ok());
    }
    ROCLINT_CHECK(!parse(R"({"status":"no_ambiguity","assistance_kind":"creative"})").ok());
    std::string concealed(creative);
    concealed.insert(concealed.size() - 1, R"(,"conflicts":[{"description":"Unresolved conflict","blocking":true}])");
    ROCLINT_CHECK(!parse(concealed, "Make it modern").ok());
    GraniteAdapter adapter; makeReady(adapter);
    analysis::PromptTriage triage;
    triage.sessionId = kSession; triage.revisionId = kRevision;
    triage.stateVersion = 1; triage.requestGroupId = "group"; triage.attemptId = "triage";
    triage.modelId = "fixture"; triage.modelRevision = "fixture"; triage.configurationId = "fixture";
    triage.weightsSha256 = std::string(64, 'a'); triage.execution = analysis::TriageExecution::Mock;
    triage.categoryScores = {.1, .1, .1, .1};
    GraniteOptionDiscoveryRequest req{kSession, kRevision, kPrompt};
    triage.revisionTextHash = crypto::Sha256::hexDigest(kPrompt);
    const auto evidence = analysis::sealPromptTriage(triage, kPrompt);
    ROCLINT_CHECK(evidence.ok());
    req.promptTriage = evidence.value();
    int calls = 0;
    const GraniteTransport transport = [&](std::string_view) -> Result<std::string> { ++calls; return std::string(R"({"status":"no_ambiguity","assistance_kind":"none"})"); };
    ROCLINT_CHECK(!adapter.discoverOptions(req, transport).ok() && calls == 0);
    req.reviewTrigger = static_cast<analysis::ReviewTrigger>(99);
    ROCLINT_CHECK(!adapter.discoverOptions(req, transport).ok() && calls == 0);
    req.reviewTrigger = analysis::ReviewTrigger::ExplicitReview;
    ROCLINT_CHECK(adapter.discoverOptions(req, transport).ok() && calls == 1);
    const auto explicitPrompt = buildDiscoveryPrompt(req);
    ROCLINT_CHECK(explicitPrompt.find("explicitly requested deeper review") != std::string::npos);
    ROCLINT_CHECK(explicitPrompt.find("s1_classification") == std::string::npos);
    req.reviewTrigger = analysis::ReviewTrigger::Automatic;
    ROCLINT_CHECK(buildDiscoveryPrompt(req).find("s1_classification") != std::string::npos);
    ROCLINT_CHECK(req.promptTriage->ambiguityScore == .1);
}

void testDesignDiscoveryQuotesSpecificPartsOfTheDraft() {
    const std::string draft = "Make an app that uses small text font aswell it is inspired by a fusion of "
                              "1990s and 1960s beatles era album degisn like the revolver album";
    const auto instruction = buildDiscoveryPrompt({kSession, kRevision, draft});
    for (const auto* canned : {"Example response:", "How expressive should the playful style be?",
                               "Bold layout with playful visual accents", "Minimal layout with subtle playful details"})
        ROCLINT_CHECK(instruction.substr(0, instruction.rfind("Request:\n")).find(canned) == std::string::npos);
    const auto request = json::parse(std::string_view(instruction).substr(instruction.rfind("Request:\n") + 9));
    ROCLINT_CHECK(request.ok());
    const auto schema = json::parse(discoveryResponseSchema(draft));
    ROCLINT_CHECK(schema.ok());
    const auto& branch = schema.value().find("oneOf")->asArray()[1];
    const auto* strings = branch.find("properties")->find("groups")->find("items")->find("properties")->find("source");
    const auto& quotes = strings->find("enum")->asArray();
    ROCLINT_CHECK(json::serialize(*strings->find("enum")) == json::serialize(*request.value().find("evidence_quotes")));
    for (const auto* expected : {"small text", "1990s and 1960s", "revolver album"}) {
        bool found = false;
        for (const auto& quote : quotes) found = found || quote.asString() == expected;
        ROCLINT_CHECK(found);
    }
}

void testClarificationChecksUseModelDecisionAndPreserveContext() {
    ClarificationCheckRequest request{"Use small text in the app.", "What size should the text be?", "12 pt"};
    request.alternatives = {"11 pt", "12 pt"};
    request.previousAnswers = {{"Which audience?", "Desktop users"}};
    const auto prompt = buildClarificationCheckPrompt(request);
    const auto input = json::parse(std::string_view(prompt).substr(prompt.rfind("Request:\n") + 9));
    ROCLINT_CHECK(input.ok() && input.value().find("user_answer")->asString() == "12 pt");
    ROCLINT_CHECK(input.value().find("previous_answers")->asArray()[0].find("answer")->asString() == "Desktop users");
    GraniteAdapter adapter; makeReady(adapter);
    int calls = 0;
    const auto accepted = adapter.checkClarification(request, [&](std::string_view instruction) -> Result<std::string> {
        ++calls;
        ROCLINT_CHECK(instruction == prompt);
        return std::string(R"({"meaning":"The answer settles the question.","answers_question":true,"compatible":true,"feedback":""})");
    });
    ROCLINT_CHECK(accepted.ok() && accepted.value().accepted && calls == 1);
    request.answer = "iondwfwoefneiofneionfeio";
    const auto rejected = adapter.checkClarification(request, [](std::string_view) -> Result<std::string> {
        return std::string(R"({"meaning":"The answer does not settle the question.","answers_question":false,"compatible":true,"feedback":"What text size did you mean?"})");
    });
    ROCLINT_CHECK(rejected.ok() && !rejected.value().accepted && !rejected.value().feedback.empty());
    for (const auto* invalid : {"{}", R"({"accepted":"true","feedback":""})", R"({"accepted":true})",
            R"({"meaning":"","answers_question":false,"compatible":true,"feedback":"   "})",
            R"({"meaning":"Clear answer","answers_question":"true","compatible":true,"feedback":""})",
            R"({"meaning":"Clear answer","answers_question":true,"compatible":true,"feedback":"Assuming 12 pt"})"})
        ROCLINT_CHECK(!parseClarificationCheckResponse(invalid).ok());
    const auto conflicting = parseClarificationCheckResponse(R"({"meaning":"The answer reverses a requirement.","answers_question":true,"compatible":false,"feedback":""})");
    ROCLINT_CHECK(conflicting.ok() && !conflicting.value().accepted && !conflicting.value().feedback.empty());
    ROCLINT_CHECK(!adapter.checkClarification(request, [](std::string_view) -> Result<std::string> {
        return makeError(ErrorCode::ProviderFailed, "Synthetic timeout");
    }).ok());
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    request.answer = " ";
    ROCLINT_CHECK(!adapter.checkClarification(request, [&](std::string_view) -> Result<std::string> {
        ++calls; return std::string(R"({"meaning":"The answer settles the question.","answers_question":true,"compatible":true,"feedback":""})");
    }).ok() && calls == 1);
}

void testPreModelConflictDetection() {
    {
        GraniteAdapter adapter;
        makeReady(adapter);
        GraniteOptionDiscoveryRequest req{kSession, kRevision,
            "The refresh interval must be 15 seconds. The refresh interval must be 60 seconds.", "UNKNOWN"};
        int calls = 0;
        const auto result = adapter.discoverOptions(req, [&](std::string_view) -> Result<std::string> {
            ++calls;
            return std::string(R"({"status":"no_ambiguity","assistance_kind":"none"})");
        });
        ROCLINT_CHECK(result.ok());
        ROCLINT_CHECK(calls == 0);
        ROCLINT_CHECK(result.value().outcome == DiscoveryOutcome::OptionsGenerated);
        ROCLINT_CHECK(result.value().assistanceKind == analysis::AssistanceKind::Clarification);
        ROCLINT_CHECK(result.value().analysis.conflicts.size() == 1);
        ROCLINT_CHECK(result.value().analysis.choiceGroups.empty());
        ROCLINT_CHECK(analysis::isSnapshotSealed(result.value().analysis));
        ROCLINT_CHECK(result.value().analysis.conflicts.front().id == "cfl_0");
        ROCLINT_CHECK(result.value().analysis.conflicts.front().blocking);
        ROCLINT_CHECK(result.value().analysis.conflicts.front().description.find("15 seconds") != std::string::npos);
        ROCLINT_CHECK(result.value().analysis.conflicts.front().description.find("60 seconds") != std::string::npos);
        ROCLINT_CHECK(result.value().latencyMs < 50.0);
        std::cout << "  [INFO] Pre-model conflict latency=" << result.value().latencyMs << " ms\n";
        ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    }
    {
        // Repeated fragments cannot be resolved to one authoritative span, so the
        // deterministic path must fall through to the model instead of guessing.
        GraniteAdapter adapter;
        makeReady(adapter);
        GraniteOptionDiscoveryRequest req{kSession, kRevision,
            "The timeout must be 15 seconds. The timeout must be 60 seconds. The timeout must be 15 seconds.",
            "UNKNOWN"};
        int calls = 0;
        const auto result = adapter.discoverOptions(req, [&](std::string_view) -> Result<std::string> {
            ++calls;
            return std::string(R"({"status":"no_ambiguity","assistance_kind":"none"})");
        });
        ROCLINT_CHECK(result.ok());
        ROCLINT_CHECK(calls == 1);
        ROCLINT_CHECK(result.value().outcome == DiscoveryOutcome::NoAmbiguity);
    }
}

void testTransportFailureFailsRuntimeButMalformedOutputDoesNot() {
    GraniteOptionDiscoveryRequest req{kSession, kRevision, kPrompt, "UNKNOWN"};
    {
        GraniteAdapter adapter;
        makeReady(adapter);
        auto ok = adapter.discoverOptions(req, [](std::string_view) -> Result<std::string> {
            return std::string(kTwoGroupResponse);
        });
        ROCLINT_CHECK(ok.ok());
        ROCLINT_CHECK(ok.value().model.digest == "sha256:abc");
        ROCLINT_CHECK(ok.value().model.transport == std::string(kLoopbackOllamaTransport));
        ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    }
    {
        GraniteAdapter adapter;
        makeReady(adapter);
        auto bad = adapter.discoverOptions(req, [](std::string_view) -> Result<std::string> {
            return std::string("{\"status\": ");
        });
        ROCLINT_CHECK(!bad.ok());
        ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    }
    {
        GraniteAdapter adapter;
        makeReady(adapter);
        auto down = adapter.discoverOptions(req, [](std::string_view) -> Result<std::string> {
            return makeError(ErrorCode::ProviderUnavailable, "connection refused");
        });
        ROCLINT_CHECK(!down.ok());
        ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Failed);
    }
    {
        GraniteAdapter adapter;
        makeReady(adapter);
        auto threw = adapter.discoverOptions(req, [](std::string_view) -> Result<std::string> {
            throw std::runtime_error("boom");
        });
        ROCLINT_CHECK(!threw.ok());
        ROCLINT_CHECK(threw.error().code == ErrorCode::ProviderFailed);
        ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Failed);
    }
    {
        GraniteAdapter adapter;
        makeReady(adapter);
        GraniteOptionDiscoveryRequest crlf{kSession, kRevision, "a\r\nb", "UNKNOWN"};
        ROCLINT_CHECK(!adapter.discoverOptions(crlf, [](std::string_view) -> Result<std::string> {
                          return std::string("{}");
                      }).ok());
        GraniteOptionDiscoveryRequest unbound{kSession, "", kPrompt, "UNKNOWN"};
        ROCLINT_CHECK(!adapter.discoverOptions(unbound, [](std::string_view) -> Result<std::string> {
                          return std::string("{}");
                      }).ok());
    }
}

void testContextualWordRefinementContract() {
    WordRefinementRequest request;
    request.sessionId = "sess_1";
    request.revisionId = "rev_1";
    request.draftText = "Make the dashboard feel faster for people on slow laptops.";
    request.selectedText = "faster";
    request.selectionStart = static_cast<int>(request.draftText.find("faster"));
    request.selectionEnd = request.selectionStart + 6;
    request.intervention = WordIntervention::Exploration;
    request.approvedDecisions = {"Performance target -> keep startup under two seconds"};
    request.explicitConstraints = {"Keep the public API unchanged"};
    request.astContextXml = "<symbol name=\"refreshDashboard\"/>";

    const auto prompt = buildWordRefinementPrompt(request);
    ROCLINT_CHECK(prompt.find("granite.word_refinement.v4") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Make the dashboard feel faster") != std::string::npos);
    ROCLINT_CHECK(prompt.find("surrounding_before") != std::string::npos);
    ROCLINT_CHECK(prompt.find("surrounding_after") != std::string::npos);
    ROCLINT_CHECK(prompt.find("approved_decisions") != std::string::npos);
    ROCLINT_CHECK(prompt.find("keep startup under two seconds") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Keep the public API unchanged") != std::string::npos);
    ROCLINT_CHECK(prompt.find("refreshDashboard") != std::string::npos);
    ROCLINT_CHECK(prompt.find("local_refinement") == std::string::npos);
    ROCLINT_CHECK(prompt.find("exploration") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Do not invent") != std::string::npos);

    const std::string valid = R"({
      "needs_clarification": false,
      "clarification_question": "",
      "candidates": [
        {"original_span": "faster", "replacement": "snappier on first paint",
         "explanation": "Optimizes how quickly the first useful view appears.",
         "assumption": "The complaint is about first paint, not batch jobs.",
         "requires_clarification": false},
        {"original_span": "faster", "replacement": "quicker to refresh data",
         "explanation": "Optimizes refresh latency once the dashboard is open.",
         "assumption": "", "requires_clarification": false}
      ]})";
    auto parsed = parseWordRefinementResponse(valid, request);
    ROCLINT_CHECK(parsed.ok());
    ROCLINT_CHECK(parsed.value().candidates.size() == 2);
    ROCLINT_CHECK(parsed.value().candidates[0].id == "wcand_1");
    ROCLINT_CHECK(parsed.value().candidates[1].id == "wcand_2");
    ROCLINT_CHECK(parsed.value().candidates[0].originalSpan == "faster");
    ROCLINT_CHECK(parsed.value().candidates[0].explanation.find("first useful view") != std::string::npos);

    auto wrongSpan = parseWordRefinementResponse(
        R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"quick","replacement":"fast","explanation":"e","assumption":"","requires_clarification":false}]})",
        request);
    ROCLINT_CHECK(!wrongSpan.ok());

    auto sameReplacement = parseWordRefinementResponse(
        R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"faster","replacement":"faster","explanation":"e","assumption":"","requires_clarification":false}]})",
        request);
    ROCLINT_CHECK(!sameReplacement.ok());

    auto missingField = parseWordRefinementResponse(
        R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"faster","replacement":"snappier","explanation":"e","requires_clarification":false}]})",
        request);
    ROCLINT_CHECK(!missingField.ok());

    auto clarification = parseWordRefinementResponse(
        R"({"needs_clarification":true,"clarification_question":"Is this about first paint or data refresh?","candidates":[]})",
        request);
    ROCLINT_CHECK(clarification.ok());
    ROCLINT_CHECK(clarification.value().needsClarification);
    ROCLINT_CHECK(clarification.value().clarificationQuestion.find("first paint") != std::string::npos);
    auto choices = parseWordRefinementResponse(
        R"({"action":"ask","question":"Which result matters?","choices":["Lower response latency","Higher batch throughput"]})", request);
    ROCLINT_CHECK(choices.ok() && choices.value().clarificationChoices.size() == 2 && choices.value().candidates.empty());
    ROCLINT_CHECK(!parseWordRefinementResponse(
        R"({"action":"ask","question":"Which result matters?","choices":["Same answer","Same answer"]})", request).ok());
    ROCLINT_CHECK(!parseWordRefinementResponse(
        R"({"action":"ask","question":"Which result matters?","choices":["One","Two","Three","Four"]})", request).ok());
    ROCLINT_CHECK(!parseWordRefinementResponse(
        R"({"action":"ask","question":"Which result matters?","choices":[42]})", request).ok());

    auto emptyQuestion = parseWordRefinementResponse(
        R"({"needs_clarification":true,"clarification_question":"","candidates":[]})", request);
    ROCLINT_CHECK(!emptyQuestion.ok());

    ROCLINT_CHECK(!parseWordRefinementResponse(
        R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"faster","replacement":"...","explanation":"...","assumption":"...","requires_clarification":false}]})", request).ok());
    const std::string noop = R"({"needs_clarification":false,"clarification_question":"","candidates":[]})";
    ROCLINT_CHECK(parseWordRefinementResponse(noop, request).ok());
    auto schema = roclint::core::json::parse(wordRefinementResponseSchema());
    ROCLINT_CHECK(schema.ok());
    ROCLINT_CHECK(schema.value().find("oneOf")->asArray()[0].find("properties")->find("action")->find("const")->asString() == "keep");
    const auto boundWord=json::parse(wordRefinementResponseSchema(request.draftText));
    const auto& quoteList=boundWord.value().find("oneOf")->asArray()[0].find("properties")->find("source")->find("enum")->asArray();
    for(const auto& quote:quoteList)ROCLINT_CHECK(analysis::findUniqueSpan(request.draftText,quote.asString()).status==analysis::SpanMatchStatus::ExactMatch);
    ROCLINT_CHECK(prompt.find("Every response must include at least one") == std::string::npos);
    // Offsets are UTF-16, while the bridge stores UTF-8; emoji takes two units.
    request.draftText = "\xf0\x9f\x8e\xac Caf\xc3\xa9 feels faster today.";
    request.selectionStart = 14;
    request.selectionEnd = 20;
    ROCLINT_CHECK(parseWordRefinementResponse(valid, request).ok());
    const auto unicodePrompt = buildWordRefinementPrompt(request);
    ROCLINT_CHECK(unicodePrompt.find("\"surrounding_after\":\" today.\"") != std::string::npos);
    request.selectionStart = 1; // Middle of the emoji's surrogate pair must fail closed.
    request.selectionEnd = 2;
    request.selectedText = "\xf0\x9f\x8e\xac";
    ROCLINT_CHECK(!parseWordRefinementResponse(noop, request).ok());

    std::cout << "  [PASS] Contextual word refinement carries context, identity, and strict spans.\n";
}

void testSourceQuotesAndProposalsAreDistinct() {
    const std::string draft = "Make a cinematic portrait of a fern.";
    const auto response = parseDiscoveryResponse(R"({"status":"options_generated","assistance_kind":"creative","choice_groups":[{"dimension":"composition","question":"How should the fern fill the frame?","material":true,"alternatives":[{"label":"A close portrait with a blurred background","why_material":"Isolates the fern","evidence":["cinematic"],"proposed_assumptions":["Use shallow depth of field."]},{"label":"A wide shot showing its environment","why_material":"Shows the setting","evidence":["cinematic"],"proposed_assumptions":[]}]}],"assumptions":[],"missing_decisions":[],"conflicts":[],"clarification_question":""})", kSession,kRevision,draft);
    ROCLINT_CHECK(response.ok() && response.value().ungroundedEvidenceCount == 0);
    const auto& alt = response.value().analysis.choiceGroups[0].alternatives[0];
    ROCLINT_CHECK(alt.whyMaterial.find("Proposed assumption:") != std::string::npos);
    ROCLINT_CHECK(alt.canonicalSemanticsJson.find("shallow depth") != std::string::npos);
    GoldfishDiscoveryAuditRequest auditRequest;
    auditRequest.originalPrompt = draft;
    auditRequest.proposedAnalysis = response.value().analysis;
    const auto auditPrompt = buildGoldfishDiscoveryAuditPrompt(auditRequest);
    ROCLINT_CHECK(auditPrompt.find("source_quotes") != std::string::npos);
    ROCLINT_CHECK(auditPrompt.find("Use shallow depth of field") != std::string::npos);
    ROCLINT_CHECK(draft.substr(alt.evidence[0].startByte, alt.evidence[0].endByte-alt.evidence[0].startByte) == "cinematic");
    auto schema = json::parse(discoveryResponseSchema(draft,1));
    const auto& props = *schema.value().find("oneOf")->asArray()[1].find("properties");
    ROCLINT_CHECK(props.find("groups")->find("maxItems")->asNumber() == 1);
    ROCLINT_CHECK(props.find("groups")->find("items")->find("properties")->find("question")->find("maxLength")->asNumber() == 200);
    const auto instruction = buildDiscoveryPrompt({kSession,kRevision,draft});
    ROCLINT_CHECK(instruction.find("verbatim source quotation") != std::string::npos);
    GraniteOptionDiscoveryRequest request{kSession,kRevision,"Make demo::Queue::push faster. Do not add logging."};
    request.groundedCanonicalIntentXml = "<canonical_intents/>";
    const auto groundedPrompt = buildDiscoveryPrompt(request);
    ROCLINT_CHECK(groundedPrompt.find("general_constraint") != std::string::npos);
    ROCLINT_CHECK(groundedPrompt.find("Do not add logging") != std::string::npos);
}

void testContractRecoveryIsBoundedAndBudgeted() {
    GraniteAdapter adapter; makeReady(adapter);
    GraniteOptionDiscoveryRequest request{kSession,kRevision,kPrompt};
    int calls=0;
    auto repaired = adapter.discoverOptions(request,[&](std::string_view prompt)->Result<std::string>{
        ++calls;
        if(calls==1) return std::string("{bad");
        ROCLINT_CHECK(prompt.find("previous response was unusable") != std::string_view::npos);
        return std::string(kTwoGroupResponse);
    });
    ROCLINT_CHECK(repaired.ok() && calls==2);
    calls=0;
    auto failed = adapter.discoverOptions(request,[&](std::string_view)->Result<std::string>{++calls;return std::string("{bad");});
    ROCLINT_CHECK(!failed.ok() && calls==2 && adapter.state()==GraniteRuntimeState::Ready);
    calls=0;
    request.canContinue=[&]{return calls==0;};
    auto expired=adapter.discoverOptions(request,[&](std::string_view)->Result<std::string>{++calls;return std::string("{bad");});
    ROCLINT_CHECK(!expired.ok() && calls==1);
    calls=0; request.canContinue=[] {return false;};
    ROCLINT_CHECK(!adapter.discoverOptions(request,[&](std::string_view)->Result<std::string>{++calls;return std::string(kTwoGroupResponse);}).ok());
    ROCLINT_CHECK(calls==0);
}

void testWordEditsRequireCompleteDraftAudit() {
    WordRefinementRequest request;
    request.draftText="Apply an atmospheric treatment to the image. Keep the logo unchanged.";
    request.selectedText="atmospheric";request.selectionStart=9;request.selectionEnd=20;
    request.explicitConstraints={"Keep the logo unchanged."};
    auto proposed=parseWordRefinementResponse(R"({"needs_clarification":false,"clarification_question":"","candidates":[{"original_span":"atmospheric","replacement":"a clear, airy atmosphere","explanation":"Adds space","assumption":"","requires_clarification":false},{"original_span":"atmospheric","replacement":"airy, softly lit","explanation":"Makes lighting concrete","assumption":"","requires_clarification":false}]})",request);
    ROCLINT_CHECK(proposed.ok());
    const auto prompt=buildWordRefinementAuditPrompt(request,proposed.value());
    ROCLINT_CHECK(prompt.find("Apply an a clear, airy atmosphere treatment") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Apply an airy, softly lit treatment") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Keep the logo unchanged") != std::string::npos);
    const auto audit=auditWordRefinementResponse(R"({"verdicts":[{"id":"wcand_2","grammar":"Pass","preservation":"Pass","usefulness":"Pass"},{"id":"wcand_1","grammar":"Fail","preservation":"Fail","usefulness":"Fail"}]})",request,proposed.value());
    ROCLINT_CHECK(audit.ok() && audit.value().candidates.size()==1 && audit.value().candidates[0].id=="wcand_2");
    const auto falsePass=auditWordRefinementResponse(R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"},{"id":"wcand_2","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})",request,proposed.value());
    ROCLINT_CHECK(falsePass.ok() && falsePass.value().candidates.size()==1 && falsePass.value().candidates[0].id=="wcand_2");
    ROCLINT_CHECK(!auditWordRefinementResponse(R"({"verdicts":[{"id":"wcand_1","outcome":"Pass"},{"id":"wcand_2","outcome":"Pass"}]})",request,proposed.value()).ok());
    for (const auto* bad : {"{}", R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})",R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"},{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})",R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"},{"id":"foreign","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})"})
        ROCLINT_CHECK(!auditWordRefinementResponse(bad,request,proposed.value()).ok());
    auto unresolved=auditWordRefinementResponse(R"({"verdicts":[{"id":"wcand_1","grammar":"Unresolved","preservation":"Unresolved","usefulness":"Unresolved"},{"id":"wcand_2","grammar":"Fail","preservation":"Fail","usefulness":"Fail"}]})",request,proposed.value());
    ROCLINT_CHECK(unresolved.ok() && unresolved.value().candidates.empty());
}

void testWordSurfaceRecoveryIsBoundedAndEvidenceChecked() {
    WordRefinementRequest request;
    request.draftText = "Keep the output unchnaged"; request.selectedText = "unchnaged";
    request.selectionStart = 16; request.selectionEnd = 25;
    const auto repair = parseWordSurfaceCheckResponse(
        R"({"status":"repair","replacement":"unchanged","explanation":"Correct spelling","source":""})", request);
    ROCLINT_CHECK(repair.ok() && repair.value().handled && repair.value().response.candidates.size() == 1);
    ROCLINT_CHECK(repair.value().response.candidates[0].replacement == "unchanged");
    const auto clean = parseWordSurfaceCheckResponse(
        R"({"status":"clean","replacement":"","explanation":"","source":""})", request);
    ROCLINT_CHECK(clean.ok() && !clean.value().handled);
    const auto fixed = parseWordSurfaceCheckResponse(
        R"({"status":"protected","replacement":"","explanation":"Exact spelling required","source":"Keep the output unchnaged"})", request);
    ROCLINT_CHECK(fixed.ok() && fixed.value().handled && fixed.value().response.candidates.empty());
    for (const auto* invalid : {
        R"({"status":"clean","replacement":"unchanged","explanation":"","source":""})",
        R"({"status":"protected","replacement":"","explanation":"Exact","source":"invented source"})",
        R"({"status":"protected","replacement":"","explanation":"Exact","source":"unchnaged"})",
        R"({"status":"repair","replacement":"unchnaged","explanation":"No change","source":""})",
        R"({"status":"repair","replacement":"unchanged","explanation":"","source":""})",
        R"({"status":"unknown","replacement":"","explanation":"","source":""})", "{}"})
        ROCLINT_CHECK(!parseWordSurfaceCheckResponse(invalid, request).ok());
    ++request.selectionStart;
    ROCLINT_CHECK(!parseWordSurfaceCheckResponse(
        R"({"status":"clean","replacement":"","explanation":"","source":""})", request).ok());
}

void testWordCodeIdentitySurvivesFalseModelApproval() {
    for (const auto* draft : {"Use Cache::reaad exactly as named", "Call cache->reaad next",
                              "Call cache.reaad next", "Use `reaad` as named", "Set the literal label to \"reaad\" exactly"}) {
        WordRefinementRequest request;
        request.draftText = draft; request.selectedText = "reaad";
        request.selectionStart = static_cast<int>(request.draftText.find("reaad"));
        request.selectionEnd = request.selectionStart + 5;
        const auto parsed = parseWordRefinementResponse(
            R"({"action":"replace","options":[{"text":"read","effect":"Correct spelling","assume":""}]})", request);
        ROCLINT_CHECK(parsed.ok() && parsed.value().candidates.empty() && !parsed.value().needsClarification);
        WordRefinementResponse forged;
        forged.candidates.push_back({"wcand_1", "reaad", "read", "Spelling", "", false});
        const auto audited = auditWordRefinementResponse(
            R"({"verdicts":[{"id":"wcand_1","grammar":"Pass","preservation":"Pass","usefulness":"Pass"}]})",
            request, forged);
        ROCLINT_CHECK(audited.ok() && audited.value().candidates.empty());
    }
    for (const auto& draft : {std::string("Make Cache::run unchnaged"),
                              std::string("Keep ") + std::string(12000, 'x') + " unchnaged"}) {
        WordRefinementRequest request;
        request.draftText = draft; request.selectedText = "unchnaged";
        request.selectionStart = static_cast<int>(draft.find(request.selectedText));
        request.selectionEnd = request.selectionStart + 9;
        const auto parsed = parseWordRefinementResponse(
            R"({"action":"replace","options":[{"text":"unchanged","effect":"Correct spelling","assume":""}]})", request);
        ROCLINT_CHECK(parsed.ok() && parsed.value().candidates.size() == 1);
    }
}

void testInterviewFollowUpCarriesBriefWithoutRepeatingSettledDecisions() {
    GraniteOptionDiscoveryRequest request;
    request.sessionId = kSession;
    request.revisionId = kRevision;
    request.promptString = "Make the dashboard feel faster for people on slow laptops.";
    request.maximumQuestions = 1;
    request.interviewFollowUp = true;
    request.settledDecisions = {"Which kind of speed matters? -> Lower latency per call"};
    request.openChoices = {"Which browsing behavior matters most?"};
    const auto prompt = buildDiscoveryPrompt(request);
    ROCLINT_CHECK(prompt.find("interview follow-up") != std::string::npos);
    ROCLINT_CHECK(prompt.find("settled_decisions") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Lower latency per call") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Which browsing behavior matters most?") != std::string::npos);
    ROCLINT_CHECK(prompt.find("never question, repeat or contradict") != std::string::npos);
    ROCLINT_CHECK(prompt.find("\"maximum_questions\":1") != std::string::npos);
    // The substantial interview uses the richer bounded contract, not the HUD's
    // ultra-terse 5-word option limit.
    ROCLINT_CHECK(prompt.find("at most 32 words") != std::string::npos);
    ROCLINT_CHECK(prompt.find("at most 9 words") == std::string::npos);
    ROCLINT_CHECK(prompt.find("typographic") != std::string::npos);
    ROCLINT_CHECK(prompt.find("not fewer features or shortened labels") != std::string::npos);
    ROCLINT_CHECK(prompt.find("do not question a settled value") != std::string::npos);

    auto firstQuestion = request;
    firstQuestion.interviewFollowUp = false;
    firstQuestion.substantialInterview = true;
    const auto firstPrompt = buildDiscoveryPrompt(firstQuestion);
    ROCLINT_CHECK(firstPrompt.find("first question of the substantial interview") != std::string::npos);
    ROCLINT_CHECK(firstPrompt.find("at most 32 words") != std::string::npos);
    ROCLINT_CHECK(firstPrompt.find("at most 9 words") == std::string::npos);

    GraniteOptionDiscoveryRequest automatic;
    automatic.sessionId = kSession;
    automatic.revisionId = kRevision;
    automatic.promptString = request.promptString;
    const auto automaticPrompt = buildDiscoveryPrompt(automatic);
    ROCLINT_CHECK(automaticPrompt.find("settled_decisions") == std::string::npos);
    ROCLINT_CHECK(automaticPrompt.find("at most 9 words") != std::string::npos);
    ROCLINT_CHECK(automaticPrompt.find("at most 32 words") == std::string::npos);

    // A mocked typography follow-up parses into two materially distinct directions
    // with their practical consequences.
    GraniteOptionDiscoveryRequest typography;
    typography.sessionId = kSession;
    typography.revisionId = kRevision;
    typography.promptString = "Design a music app with compact text and easy browsing.";
    typography.maximumQuestions = 1;
    typography.interviewFollowUp = true;
    typography.substantialInterview = true;
    const auto typographyReply = std::string(R"({"kind":"creative","groups":[{"focus":"typography","question":"Should compact text mean smaller type with generous leading, or a tighter hierarchy with denser labels?","source":"compact text","options":[{"text":"Smaller type with generous leading","effect":"Keeps every feature and full labels while improving scan comfort.","assume":""},{"text":"Tighter hierarchy with denser labels","effect":"Fits more controls but makes small labels harder to read.","assume":""}]}]})");
    auto typographyParsed = parseDiscoveryResponse(typographyReply, kSession, kRevision, typography.promptString);
    ROCLINT_CHECK(typographyParsed.ok());
    const auto& group = typographyParsed.value().analysis.choiceGroups.front();
    ROCLINT_CHECK(group.alternatives.size() == 2);
    ROCLINT_CHECK(group.alternatives[0].interpretation != group.alternatives[1].interpretation);
    ROCLINT_CHECK(!group.alternatives[0].whyMaterial.empty());
    ROCLINT_CHECK(!group.alternatives[1].whyMaterial.empty());
    ROCLINT_CHECK(group.alternatives[0].whyMaterial != group.alternatives[1].whyMaterial);

    const auto reply = std::string(R"({"kind":"creative","groups":[{"focus":"browsing","question":"Which browsing behavior matters most?","source":"slow laptops","options":[{"text":"fast A-Z jump list","effect":"one-click artist browsing","assume":""},{"text":"search-first layout","effect":"type to find anything","assume":""}]}]})");
    auto parsed = parseDiscoveryResponse(reply, kSession, kRevision, request.promptString);
    ROCLINT_CHECK(parsed.ok());
    ROCLINT_CHECK(parsed.value().analysis.choiceGroups.size() == 1);
    ROCLINT_CHECK(parsed.value().analysis.choiceGroups[0].question == "Which browsing behavior matters most?");
    ROCLINT_CHECK(parsed.value().analysis.choiceGroups[0].alternatives.size() == 2);
    ROCLINT_CHECK(!parsed.value().analysis.choiceGroups[0].alternatives[0].evidence.empty());
    ROCLINT_CHECK(parsed.value().analysis.choiceGroups[0].alternatives[0].evidence[0].startByte ==
                  request.promptString.find("slow laptops"));

    // A fabricated quotation is not evidence and cannot ground the follow-up.
    const auto ungrounded = std::string(R"({"kind":"creative","groups":[{"focus":"browsing","question":"Which browsing behavior matters most?","source":"not in the draft","options":[{"text":"fast A-Z jump list","effect":"one-click artist browsing","assume":""},{"text":"search-first layout","effect":"type to find anything","assume":""}]}]})");
    auto ungroundedParsed = parseDiscoveryResponse(ungrounded, kSession, kRevision, request.promptString);
    ROCLINT_CHECK(ungroundedParsed.ok());
    ROCLINT_CHECK(ungroundedParsed.value().analysis.choiceGroups.empty() ||
                  ungroundedParsed.value().analysis.choiceGroups[0].alternatives[0].evidence.empty());
}

void testCompactWireContractsRetainProvenanceAndSpanSafety() {
    const auto compact=R"({"kind":"creative","groups":[{"focus":"framing","question":"Which framing?","source":"cinematic","options":[{"text":"A close portrait","effect":"Isolates the subject","assume":"Use shallow focus"},{"text":"A wide environmental shot","effect":"Shows its surroundings","assume":""}]}]})";
    const auto parsed=parseDiscoveryResponse(compact,kSession,kRevision,"Make a cinematic fern portrait.");
    ROCLINT_CHECK(parsed.ok() && parsed.value().assistanceKind==analysis::AssistanceKind::Creative);
    ROCLINT_CHECK(parsed.value().analysis.choiceGroups[0].alternatives[0].canonicalSemanticsJson.find("shallow focus")!=std::string::npos);
    ROCLINT_CHECK(parseDiscoveryResponse(R"({"kind":"none"})",kSession,kRevision,kPrompt).value().outcome==DiscoveryOutcome::NoAmbiguity);
    ROCLINT_CHECK(parseDiscoveryResponse(R"({"kind":"question","question":"Which audience?"})",kSession,kRevision,kPrompt).value().analysis.missingDecisions.size()==1);
    ROCLINT_CHECK(!parseDiscoveryResponse(R"({"kind":"none","status":"options_generated"})",kSession,kRevision,kPrompt).ok());
    ROCLINT_CHECK(!parseDiscoveryResponse(R"({"kind":"none","error":"failed"})",kSession,kRevision,kPrompt).ok());
    ROCLINT_CHECK(!parseDiscoveryResponse(R"({"kind":"creative","groups":[]})",kSession,kRevision,kPrompt).ok());
    WordRefinementRequest request;request.draftText="Make the page faster.";request.selectedText="faster";request.selectionStart=14;request.selectionEnd=20;
    const auto edit=parseWordRefinementResponse(R"({"action":"replace","options":[{"text":"quicker to load","effect":"Specifies initial loading","assume":""}]})",request);
    ROCLINT_CHECK(edit.ok() && edit.value().candidates[0].originalSpan=="faster" && edit.value().candidates[0].id=="wcand_1");
    ROCLINT_CHECK(parseWordRefinementResponse(R"({"action":"ask","question":"Loading or interaction speed?"})",request).value().needsClarification);
    ROCLINT_CHECK(!parseWordRefinementResponse(R"({"action":"keep"})",request).ok());
    ROCLINT_CHECK(!parseWordRefinementResponse(R"({"action":"keep","source":"faster","reason":"Understood"})",request).ok());
    ROCLINT_CHECK(!parseWordRefinementResponse(R"({"action":"keep","source":"under two seconds","reason":"Fixed timing"})",request).ok());
    request.approvedDecisions={"Startup must finish within two seconds"};
    ROCLINT_CHECK(parseWordRefinementResponse(R"({"action":"keep","source":"Startup must finish within two seconds","reason":"The approved timing settles the goal"})",request).value().candidates.empty());
    ROCLINT_CHECK(!parseWordRefinementResponse(R"({"action":"replace","options":[]})",request).ok());
    request.selectionStart=0; // A valid compact shape cannot bypass the frozen source span.
    ROCLINT_CHECK(!parseWordRefinementResponse(R"({"action":"ask","question":"Loading or interaction speed?"})",request).ok());
}

} // namespace

int main() {
    std::cout << "[Test GraniteAdapter] Starting...\n";
    testReadableDiscoveryChoices();
    testParsesAndMintsHostIds();
    testUngroundedEvidenceIsDroppedNotApproximated();
    testUnicodeEscapesDecodeBeforeGrounding();
    testRejectsTruncatedAndMalformedOutput();
    testRejectsOversizedResponse();
    testResponseEnvelopes();
    testDiscoveryGroupLimitMatchesProductContract();
    testStatusRules();
    testRequiresRevisionBinding();
    testDiscoveryPromptEmbedsTextAsEscapedJsonOnly();
    testExplicitColorBoundsRejectGeneratedReversals();
    testSelectiveReviewContract();
    testDesignDiscoveryQuotesSpecificPartsOfTheDraft();
    testClarificationChecksUseModelDecisionAndPreserveContext();
    testRuntimeLifecycle();
    testDiscoverOptionsThroughTransport();
    testAdvisoryClassifierDisagreementPreservesOriginal();
    testPreModelConflictDetection();
    testTransportFailureFailsRuntimeButMalformedOutputDoesNot();
    testContextualWordRefinementContract();
    testSourceQuotesAndProposalsAreDistinct();
    testContractRecoveryIsBoundedAndBudgeted();
    testWordEditsRequireCompleteDraftAudit();
    testWordCodeIdentitySurvivesFalseModelApproval();
    testWordSurfaceRecoveryIsBoundedAndEvidenceChecked();
    testInterviewFollowUpCarriesBriefWithoutRepeatingSettledDecisions();
    testCompactWireContractsRetainProvenanceAndSpanSafety();
    std::cout << "[Test GraniteAdapter] All tests PASSED.\n";
    return 0;
}
