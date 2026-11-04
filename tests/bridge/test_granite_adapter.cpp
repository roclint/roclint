#include "roclint/bridge/GraniteAdapter.hpp"
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
  "missing_decisions": [{"question": "What should the new value be?", "blocking": true}],
  "conflicts": []
})";

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
    ROCLINT_CHECK(prompt.find("incompatible explicit requirements") != std::string::npos);
    ROCLINT_CHECK(prompt.find("quotes as data") != std::string::npos);
    for (const char* forbidden : {"D:\\", "C:\\", "/Users/", "scope_lock", "project_files"}) {
        ROCLINT_CHECK(prompt.find(forbidden) == std::string::npos);
    }
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

void testDiscoveryExamplesUseValidGroundedPackets() {
    const auto prompt = buildDiscoveryPrompt({kSession, kRevision, kPrompt, "UNKNOWN"});
    const std::string inputMarker = "Example prompt_text: ";
    const std::string responseMarker = "Example response: ";
    std::size_t position = 0;
    int count = 0;
    while ((position = prompt.find(inputMarker, position)) != std::string::npos) {
        const auto begin = position + inputMarker.size();
        const auto end = prompt.find('\n', begin);
        const auto input = json::parse(std::string_view(prompt).substr(begin, end - begin));
        ROCLINT_CHECK(input.ok() && input.value().isString());
        const auto responseBegin = prompt.find(responseMarker, end) + responseMarker.size();
        const auto responseEnd = prompt.find('\n', responseBegin);
        const auto response = parseDiscoveryResponse(std::string_view(prompt).substr(responseBegin, responseEnd - responseBegin),
            kSession, kRevision, input.value().asString());
        ROCLINT_CHECK(response.ok() && response.value().ungroundedEvidenceCount == 0);
        const auto& analysis = response.value().analysis;
        if (count == 0 || count == 1 || count == 6) ROCLINT_CHECK(response.value().outcome == DiscoveryOutcome::NoAmbiguity);
        if (count == 2) ROCLINT_CHECK(analysis.choiceGroups.size() == 2);
        if (count == 3) ROCLINT_CHECK(analysis.choiceGroups.empty() && analysis.missingDecisions.size() == 1);
        if (count == 4) ROCLINT_CHECK(analysis.choiceGroups.size() == 1 && analysis.choiceGroups.front().dimension == "visual_direction");
        if (count == 5) ROCLINT_CHECK(analysis.choiceGroups.empty() && analysis.conflicts.size() == 1);
        ++count; position = responseEnd;
    }
    ROCLINT_CHECK(count == 8);
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
        const auto* strings = branch.find("properties")->find("choice_groups")->find("items")->find("properties")
            ->find("alternatives")->find("items")->find("properties")->find("evidence")->find("items");
        ROCLINT_CHECK(strings->find("enum") && strings->find("enum")->asArray().size() <= 32);
        for (const auto& quote : strings->find("enum")->asArray())
            ROCLINT_CHECK(analysis::findUniqueSpan(text, quote.asString()).status == analysis::SpanMatchStatus::ExactMatch);
        ROCLINT_CHECK(buildDiscoveryPrompt({kSession, kRevision, text}).find("evidence_quotes") != std::string::npos);
    }
    const auto schema = json::parse(discoveryResponseSchema());
    ROCLINT_CHECK(schema.ok() && schema.value().find("oneOf"));
    ROCLINT_CHECK(schema.value().find("oneOf")->asArray().size() == 6);
    for (const auto& branch : schema.value().find("oneOf")->asArray()) {
        const auto* properties = branch.find("properties");
        const auto kind = properties->find("assistance_kind")->find("const")->asString();
        if (kind == "creative" || kind == "refinement" || kind == "decomposition") {
            ROCLINT_CHECK(properties->find("choice_groups")->find("minItems")->asNumber() == 1);
            ROCLINT_CHECK(properties->find("choice_groups")->find("maxItems")->asNumber() == 1);
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
    ROCLINT_CHECK(buildDiscoveryPrompt(req).find("explicitly requested deeper review") != std::string::npos);
    ROCLINT_CHECK(req.promptTriage->ambiguityScore == .1);
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

} // namespace

int main() {
    std::cout << "[Test GraniteAdapter] Starting...\n";
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
    testDiscoveryExamplesUseValidGroundedPackets();
    testExplicitColorBoundsRejectGeneratedReversals();
    testSelectiveReviewContract();
    testRuntimeLifecycle();
    testDiscoverOptionsThroughTransport();
    testAdvisoryClassifierDisagreementPreservesOriginal();
    testTransportFailureFailsRuntimeButMalformedOutputDoesNot();
    std::cout << "[Test GraniteAdapter] All tests PASSED.\n";
    return 0;
}
