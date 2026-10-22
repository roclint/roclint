#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string_view>

using namespace roclint::bridge;
using namespace roclint::core::analysis;

void testSerializeRequest() {
    GraniteOptionDiscoveryRequest req{
        .sessionId = "sess_123",
        .revisionId = "rev_456",
        .promptString = "Refactor timeout handling",
        .s1Category = "action",
        .hostContextCategory = "CODE_EDITOR"
    };

    std::string json = serializeDiscoveryRequest(req);
    ROCLINT_CHECK(!json.empty());
    ROCLINT_CHECK(json.find("\"session_id\": \"sess_123\"") != std::string::npos);
    ROCLINT_CHECK(json.find("\"s1_category\": \"action\"") != std::string::npos);
    ROCLINT_CHECK(json.find("\"host_context_category\": \"CODE_EDITOR\"") != std::string::npos);
    ROCLINT_CHECK(json.find("\"max_alternatives\": 3") != std::string::npos);
}

void testParseValidResponse() {
    std::string_view validJson = R"({
        "status": "options_generated",
        "choice_group": {
            "dimension": "action",
            "question": "How should token expiry be handled?",
            "alternatives": [
                {
                    "id": "opt_a",
                    "label": "Auto-refresh in background",
                    "why_material": "Preserves active session"
                },
                {
                    "id": "opt_b",
                    "label": "Force re-login immediately",
                    "why_material": "Zero-trust security"
                }
            ]
        }
    })";

    auto resp = parseDiscoveryResponse(validJson, "rev_001");
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.errorMessage.empty());
    ROCLINT_CHECK(resp.choiceGroup.has_value());

    const auto& group = *resp.choiceGroup;
    ROCLINT_CHECK(group.dimension == "action");
    ROCLINT_CHECK(group.question == "How should token expiry be handled?");
    ROCLINT_CHECK(group.alternatives.size() == 2);
    ROCLINT_CHECK(group.alternatives[0].id == "opt_a");
    ROCLINT_CHECK(group.alternatives[0].interpretation == "Auto-refresh in background");
    ROCLINT_CHECK(group.alternatives[1].id == "opt_b");
    ROCLINT_CHECK(group.alternatives[1].interpretation == "Force re-login immediately");

    // Pass parsed group to A06 validator to verify it passes cardinality
    NormalizedAnalysis analysis;
    analysis.id = "analysis_test";
    analysis.baseRevisionId = "rev_001";
    analysis.choiceGroups.push_back(group);

    auto valResult = validateNormalizedAnalysis(analysis, "Sample prompt");
    ROCLINT_CHECK(valResult.isValid());
}

void testParseMalformedResponses() {
    // Missing root brace
    auto res1 = parseDiscoveryResponse("invalid json", "rev_1");
    ROCLINT_CHECK(!res1.success);
    ROCLINT_CHECK(!res1.errorMessage.empty());

    // Missing choice_group
    std::string_view noGroup = R"({"status": "no_options"})";
    auto res2 = parseDiscoveryResponse(noGroup, "rev_1");
    ROCLINT_CHECK(!res2.success);
    ROCLINT_CHECK(!res2.choiceGroup.has_value());

    // Explicit error from model
    std::string_view errorJson = R"({"error": "Prompt exceeds context window limit"})";
    auto res3 = parseDiscoveryResponse(errorJson, "rev_1");
    ROCLINT_CHECK(!res3.success);
    ROCLINT_CHECK(res3.errorMessage.find("context window") != std::string::npos);
}

void testAdapterLifecycleAndBuiltInInference() {
    GraniteAdapter adapter;
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);
    ROCLINT_CHECK(adapter.quantization() == "Q8_0");

    GraniteOptionDiscoveryRequest req{
        .sessionId = "sess_001",
        .revisionId = "rev_001",
        .promptString = "Update timeout logic",
        .s1Category = "target",
        .hostContextCategory = "CODE_EDITOR"
    };

    auto resp = adapter.discoverOptions(req);
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.choiceGroup.has_value());
    ROCLINT_CHECK(resp.choiceGroup->dimension == "target");
    ROCLINT_CHECK(resp.choiceGroup->alternatives.size() == 2);
    ROCLINT_CHECK(resp.latencyMs >= 0.0);
    ROCLINT_CHECK(resp.quantization == "Q8_0");
    ROCLINT_CHECK(adapter.state() == GraniteRuntimeState::Ready);

    // Test Failed State
    adapter.setState(GraniteRuntimeState::Failed);
    auto failedResp = adapter.discoverOptions(req);
    ROCLINT_CHECK(!failedResp.success);
    ROCLINT_CHECK(failedResp.errorMessage.find("FAILED") != std::string::npos);
}

void testCustomMockHandler() {
    GraniteAdapter adapter;
    adapter.setMockHandler([](const GraniteOptionDiscoveryRequest& req) {
        return R"({
            "status": "options_generated",
            "choice_group": {
                "dimension": "rule_conflict",
                "question": "Which rule has priority?",
                "alternatives": [
                    {"id": "alt_1", "label": "Rule A: Strict Persistence"},
                    {"id": "alt_2", "label": "Rule B: Speed First"}
                ]
            }
        })";
    });

    GraniteOptionDiscoveryRequest req;
    req.revisionId = "rev_custom";
    auto resp = adapter.discoverOptions(req);
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.choiceGroup->dimension == "rule_conflict");
    ROCLINT_CHECK(resp.choiceGroup->alternatives.size() == 2);
    ROCLINT_CHECK(resp.choiceGroup->alternatives[0].interpretation == "Rule A: Strict Persistence");
}

int main() {
    std::cout << "[Test GraniteAdapter] Starting B04 test suite...\n";
    testSerializeRequest();
    testParseValidResponse();
    testParseMalformedResponses();
    testAdapterLifecycleAndBuiltInInference();
    testCustomMockHandler();
    std::cout << "[Test GraniteAdapter] All tests PASSED successfully!\n";
    return 0;
}
