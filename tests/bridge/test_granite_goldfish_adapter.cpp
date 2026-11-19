#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace roclint::core;
using namespace roclint::bridge;

namespace {

void testPromptBuilding() {
    GoldfishAuditRequest req;
    req.originalPrompt = "Make a creative Layer 3 networking visualization, but technically accurate. <keep intact>";
    req.candidateRewrite = "Render packet forwarding at Layer 3 accurately with interactive paths.";

    ScopedConstraint c1;
    c1.id = "c_net";
    c1.rule = "Must accurately model Layer 3 packet forwarding";
    c1.origin = Origin::User;
    c1.derivation = Derivation::Direct;
    c1.enforcement = Enforcement::Hard;
    c1.polarity = Polarity::Require;
    c1.aspect = "networking";
    c1.appliesWhen = "depicts_ipv4_forwarding";
    c1.authorityRef = "RFC 1812";

    ScopedConstraint c2;
    c2.id = "c_glow";
    c2.rule = "Use glowing animated packets";
    c2.origin = Origin::Model;
    c2.derivation = Derivation::Assumed;
    c2.enforcement = Enforcement::Advisory;
    c2.polarity = Polarity::Require;
    c2.aspect = "presentation";

    req.constraints = {c1, c2};

    const std::string prompt = buildGoldfishAuditPrompt(req);

    // Contract checks
    ROCLINT_CHECK(prompt.find("Instruction contract: granite.goldfish_audit.v1") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Goldfish Auditor") != std::string::npos);
    ROCLINT_CHECK(prompt.find("ZERO VETO POWER") != std::string::npos);

    // Escaping and sections
    ROCLINT_CHECK(prompt.find("<original_prompt>") != std::string::npos);
    ROCLINT_CHECK(prompt.find("&lt;keep intact&gt;") != std::string::npos);
    ROCLINT_CHECK(prompt.find("<candidate_rewrite>") != std::string::npos);
    ROCLINT_CHECK(prompt.find("<verified_constraints>") != std::string::npos);

    // Constraints content
    ROCLINT_CHECK(prompt.find("id=\"c_net\"") != std::string::npos);
    ROCLINT_CHECK(prompt.find("origin=\"User\"") != std::string::npos);
    ROCLINT_CHECK(prompt.find("enforcement=\"Hard\"") != std::string::npos);
    ROCLINT_CHECK(prompt.find("AppliesWhen: depicts_ipv4_forwarding") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Authority: RFC 1812") != std::string::npos);

    ROCLINT_CHECK(prompt.find("id=\"c_glow\"") != std::string::npos);
    ROCLINT_CHECK(prompt.find("enforcement=\"Advisory\"") != std::string::npos);
}

void testResponseParsing() {
    // 1. Clean Pass
    {
        const std::string json = R"({"outcome": "Pass", "violations": [], "unresolved": []})";
        auto res = parseGoldfishAuditResponse(json);
        ROCLINT_CHECK(res.ok());
        ROCLINT_CHECK(res.value().outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.value().violations.empty());
        ROCLINT_CHECK(res.value().unresolved.empty());
    }

    // 2. Clean Fail with markdown fence
    {
        const std::string json = "```json\n"
                                 "{\n"
                                 "  \"outcome\": \"Fail\",\n"
                                 "  \"violations\": [\n"
                                 "    {\"constraint_id\": \"c_net\", \"explanation\": \"Packet forwarding omitted\"}\n"
                                 "  ],\n"
                                 "  \"unresolved\": []\n"
                                 "}\n"
                                 "```";
        auto res = parseGoldfishAuditResponse(json);
        ROCLINT_CHECK(res.ok());
        ROCLINT_CHECK(res.value().outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.value().violations.size() == 1);
        ROCLINT_CHECK(res.value().violations[0].constraintId == "c_net");
        ROCLINT_CHECK(res.value().violations[0].explanation == "Packet forwarding omitted");
    }

    // 3. Unresolved with aliases ("verdict", "id", "reason")
    {
        const std::string json = R"({
            "verdict": "unresolved",
            "violations": [],
            "unresolved": [
                {"id": "c_proof", "reason": "Cannot verify algorithmic termination from prose"}
            ]
        })";
        auto res = parseGoldfishAuditResponse(json);
        ROCLINT_CHECK(res.ok());
        ROCLINT_CHECK(res.value().outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res.value().violations.empty());
        ROCLINT_CHECK(res.value().unresolved.size() == 1);
        ROCLINT_CHECK(res.value().unresolved[0].constraintId == "c_proof");
        ROCLINT_CHECK(res.value().unresolved[0].explanation == "Cannot verify algorithmic termination from prose");
    }

    // 4. Case-insensitivity ("PASS")
    {
        const std::string json = R"({"outcome": "PASS", "violations": [], "unresolved": []})";
        auto res = parseGoldfishAuditResponse(json);
        ROCLINT_CHECK(res.ok());
        ROCLINT_CHECK(res.value().outcome == ValidationOutcome::Pass);
    }

    // 5. Implicit fail when violations are non-empty even if outcome says Pass
    {
        const std::string json = R"({
            "outcome": "Pass",
            "violations": [{"constraint_id": "c_breach", "explanation": "Negative boundary violated"}],
            "unresolved": []
        })";
        auto res = parseGoldfishAuditResponse(json);
        ROCLINT_CHECK(res.ok());
        // Epistemic honesty: presence of violations overrides a claimed Pass
        ROCLINT_CHECK(res.value().outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.value().violations.size() == 1);
    }

    // 6. Malformed JSON error handling
    {
        const std::string json = R"({not valid json})";
        auto res = parseGoldfishAuditResponse(json);
        ROCLINT_CHECK(!res.ok());
    }
}

void testDeterministicPreGateFastFail() {
    GraniteGoldfishAdapter adapter;

    GoldfishAuditRequest req;
    req.originalPrompt = "Refactor the query builder, but do not touch the token validation logic.";
    req.candidateRewrite = "Refactor the query builder and modify the token validation logic for efficiency.";

    ScopedConstraint boundaryRule;
    boundaryRule.id = "c_neg_01";
    boundaryRule.rule = "Do not touch token validation logic";
    boundaryRule.origin = Origin::User;
    boundaryRule.derivation = Derivation::Direct;
    boundaryRule.enforcement = Enforcement::Hard;
    boundaryRule.polarity = Polarity::Prohibit;
    boundaryRule.aspect = "security";

    req.constraints = {boundaryRule};

    int transportCalls = 0;
    GraniteTransport countingTransport = [&](std::string_view) -> Result<std::string> {
        ++transportCalls;
        return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
    };

    // The candidate explicitly violates the negative prohibition.
    // Deterministic C++ pre-gating must catch this and return Fail immediately
    // WITHOUT invoking transport!
    const auto result = adapter.audit(req, countingTransport);

    ROCLINT_CHECK(result.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(result.violations.size() == 1);
    ROCLINT_CHECK(result.violations[0].constraintId == "c_neg_01");
    ROCLINT_CHECK(transportCalls == 0); // Fast-fail: model was never invoked!

    req.candidateRewrite = "Update the documentation for token validation.";
    const auto reference = adapter.audit(req, countingTransport);
    ROCLINT_CHECK(transportCalls == 1);
    ROCLINT_CHECK(reference.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(reference.violations.empty());
    ROCLINT_CHECK(reference.executionStatus == AuditExecutionStatus::Completed);
}

void testAdvisoryZeroVetoPower() {
    GraniteGoldfishAdapter adapter;

    GoldfishAuditRequest req;
    req.originalPrompt = "Make a creative Layer 3 networking visualization, but technically accurate.";
    // Candidate rewrite has accurate Layer 3 networking, but omits glowing animated packets
    // and omits CIDR routing tables (which was a model assumption demoted to Advisory).
    req.candidateRewrite = "Create a technically accurate Layer 3 networking topology visualization showing packet routing.";

    ScopedConstraint hardReq;
    hardReq.id = "c_user_tech";
    hardReq.rule = "Must be technically accurate Layer 3 networking";
    hardReq.origin = Origin::User;
    hardReq.derivation = Derivation::Direct;
    hardReq.enforcement = Enforcement::Hard;
    hardReq.polarity = Polarity::Require;
    hardReq.aspect = "networking";

    // P-01: Demoted laundered constraint
    ScopedConstraint launderedTable;
    launderedTable.id = "c_laundered_table";
    launderedTable.rule = "Must display CIDR routing tables";
    launderedTable.origin = Origin::Model;
    launderedTable.derivation = Derivation::Assumed;
    launderedTable.enforcement = Enforcement::Advisory;
    launderedTable.polarity = Polarity::Require;
    launderedTable.aspect = "data_display";

    // P-04: Model-suggested aesthetic option
    ScopedConstraint glowingPackets;
    glowingPackets.id = "c_glow";
    glowingPackets.rule = "Use glowing animated particles for packets";
    glowingPackets.origin = Origin::Model;
    glowingPackets.derivation = Derivation::Assumed;
    glowingPackets.enforcement = Enforcement::Advisory;
    glowingPackets.polarity = Polarity::Require;
    glowingPackets.aspect = "presentation";

    req.constraints = {hardReq, launderedTable, glowingPackets};

    int transportCalls = 0;
    GraniteTransport mockTransport = [&](std::string_view prompt) -> Result<std::string> {
        ++transportCalls;
        // Verify prompt emphasizes ZERO VETO POWER
        ROCLINT_CHECK(prompt.find("ZERO VETO POWER") != std::string::npos);
        return std::string(R"({"outcome": "Pass", "violations": [], "unresolved": []})");
    };

    const auto result = adapter.audit(req, mockTransport);

    ROCLINT_CHECK(transportCalls == 1);
    ROCLINT_CHECK(result.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(result.violations.empty());
}

void testModelSemanticViolationDetected() {
    GraniteGoldfishAdapter adapter;

    GoldfishAuditRequest req;
    req.originalPrompt = "Implement binary search over sorted arrays and handle empty inputs gracefully.";
    // Substantive words 'binary', 'search', 'sorted', 'empty', 'inputs' are present in the rewrite,
    // so the deterministic lexical check passes. But the semantic meaning is inverted!
    req.candidateRewrite = "Implement binary search over sorted arrays; empty inputs are completely unsupported.";

    ScopedConstraint emptyRule;
    emptyRule.id = "c_empty_inputs";
    emptyRule.rule = "Must handle empty inputs gracefully";
    emptyRule.origin = Origin::User;
    emptyRule.derivation = Derivation::Direct;
    emptyRule.enforcement = Enforcement::Hard;
    emptyRule.polarity = Polarity::Require;
    emptyRule.aspect = "edge_cases";

    req.constraints = {emptyRule};

    // The model auditor catches the semantic contradiction that passed lexical checks
    int transportCalls = 0;
    GraniteTransport mockModel = [&](std::string_view) -> Result<std::string> {
        ++transportCalls;
        return std::string(R"({
            "outcome": "Fail",
            "violations": [
                {"constraint_id": "c_empty_inputs", "explanation": "Candidate rewrite treats empty inputs as unsupported, contradicting requirement"}
            ],
            "unresolved": []
        })");
    };

    const auto result = adapter.audit(req, mockModel);

    ROCLINT_CHECK(transportCalls == 1);
    ROCLINT_CHECK(result.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(result.violations.size() == 1);
    ROCLINT_CHECK(result.violations[0].constraintId == "c_empty_inputs");
    ROCLINT_CHECK(result.violations[0].explanation.find("contradicting requirement") != std::string::npos);
}

void testModelHonestUnknowns() {
    GraniteGoldfishAdapter adapter;

    GoldfishAuditRequest req;
    req.originalPrompt = "Create a high-performance LRU cache in C++20.";
    req.candidateRewrite = "Implement an LRU cache data structure in modern C++.";

    ScopedConstraint perfRule;
    perfRule.id = "c_time_complexity";
    perfRule.rule = "Cache lookup and insertion must be O(1) time complexity";
    perfRule.origin = Origin::Domain;
    perfRule.derivation = Derivation::Entailed;
    perfRule.enforcement = Enforcement::Hard;
    perfRule.polarity = Polarity::Require;
    perfRule.aspect = "complexity";

    req.constraints = {perfRule};

    // Model truthfully reports that it cannot verify O(1) complexity from a short prompt description
    GraniteTransport mockModel = [&](std::string_view) -> Result<std::string> {
        return std::string(R"({
            "outcome": "Unresolved",
            "violations": [],
            "unresolved": [
                {"constraint_id": "c_time_complexity", "explanation": "Prose description does not state data structure details needed to verify O(1) operations"}
            ]
        })");
    };

    const auto result = adapter.audit(req, mockModel);

    ROCLINT_CHECK(result.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(result.violations.empty());
    ROCLINT_CHECK(result.unresolved.size() == 1);
    ROCLINT_CHECK(result.unresolved[0].constraintId == "c_time_complexity");
}

void testGracefulDegradationOffline() {
    GraniteGoldfishAdapter adapter;

    GoldfishAuditRequest req;
    req.originalPrompt = "Summarize the architectural differences between TCP and UDP.";
    req.candidateRewrite = "Compare and contrast TCP and UDP transport protocols.";

    ScopedConstraint rule;
    rule.id = "c_trans";
    rule.rule = "Must compare TCP and UDP";
    rule.origin = Origin::User;
    rule.derivation = Derivation::Direct;
    rule.enforcement = Enforcement::Hard;
    rule.aspect = "networking";

    req.constraints = {rule};

    // 1. Null transport (offline mode)
    {
        const auto result = adapter.audit(req, nullptr);
        ROCLINT_CHECK(result.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(result.violations.empty());
        ROCLINT_CHECK(result.executionStatus == AuditExecutionStatus::DeterministicOnly);
    }

    // 2. Transport error (model timed out or process crashed) - F02: must be Unresolved with Unavailable status
    {
        GraniteTransport failingTransport = [](std::string_view) -> Result<std::string> {
            return makeError(ErrorCode::ProviderUnavailable, "Granite runtime offline");
        };

        const auto result = adapter.audit(req, failingTransport);
        ROCLINT_CHECK(result.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(result.violations.empty());
        ROCLINT_CHECK(result.executionStatus == AuditExecutionStatus::Unavailable);
        ROCLINT_CHECK(result.unresolved.size() == 1);
        ROCLINT_CHECK(result.unresolved[0].constraintId == "goldfish_audit");
    }

    // 3. Unparseable JSON from model - F02: Malformed status
    {
        GraniteTransport garbageTransport = [](std::string_view) -> Result<std::string> {
            return std::string("Sorry, I cannot help with that as an AI language model.");
        };

        const auto result = adapter.audit(req, garbageTransport);
        ROCLINT_CHECK(result.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(result.violations.empty());
        ROCLINT_CHECK(result.executionStatus == AuditExecutionStatus::Malformed);
        ROCLINT_CHECK(!result.unresolved.empty());
        ROCLINT_CHECK(result.unresolved[0].constraintId == "goldfish_audit");
    }
}

void testF01ExplicitOutcomesAndMalformedShapes() {
    GraniteGoldfishAdapter adapter;
    GoldfishAuditRequest req;
    req.originalPrompt = "Refactor Reader::open.";
    req.candidateRewrite = "Refactor Reader::open.";
    req.sessionId = "sess_f01";

    // F01: Explicit Fail with empty array remains Fail
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Fail","violations":[],"unresolved":[]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.executionStatus == AuditExecutionStatus::Completed);
    }

    // F01: Explicit Unresolved with empty array remains Unresolved
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res.executionStatus == AuditExecutionStatus::Completed);
    }

    // F01: Malformed JSON shapes rejected
    {
        auto p1 = parseGoldfishAuditResponse("{}");
        ROCLINT_CHECK(!p1.ok());

        auto p2 = parseGoldfishAuditResponse(R"({"outcome":"maybe","violations":[],"unresolved":[]})");
        ROCLINT_CHECK(!p2.ok());
    }
}

void testF04AdvisoryVetoPrevention() {
    GraniteGoldfishAdapter adapter;
    GoldfishAuditRequest req;
    req.originalPrompt = "Draw a plain chart.";
    req.candidateRewrite = "Draw a plain chart.";
    req.sessionId = "sess_f04";

    ScopedConstraint advisory;
    advisory.id = "optional_glow";
    advisory.rule = "Use glowing particles";
    advisory.origin = Origin::Model;
    advisory.derivation = Derivation::Assumed;
    advisory.enforcement = Enforcement::Advisory;

    req.constraints = {advisory};

    // Model reports violation for Advisory constraint - must NOT veto
    auto mock = [](std::string_view) -> Result<std::string> {
        return std::string(R"({"outcome":"Fail","violations":[{"constraint_id":"optional_glow","explanation":"Omitted glow"}],"unresolved":[]})");
    };
    const auto res = adapter.audit(req, mock);
    ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(res.violations.empty());
}

void testR06ParserBoundarySchemaValidation() {
    // 1. Missing violations array
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 2. Missing unresolved array
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 3. Both arrays missing (e.g. {"outcome":"Pass"})
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass"})");
        ROCLINT_CHECK(!p.ok());
    }

    // 4. Empty finding object in violations
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[{}],"unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 5. Empty finding object in unresolved
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[],"unresolved":[{}]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 6. Wrong field type (non-string constraint_id)
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[{"constraint_id":123,"explanation":"test"}],"unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 7. Wrong field type (non-string explanation)
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[{"constraint_id":"c1","explanation":456}],"unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 8. Empty constraint_id string
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[{"constraint_id":"","explanation":"test"}],"unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 9. Empty explanation string
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[{"constraint_id":"c1","explanation":""}],"unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 10. Missing explanation field
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[{"constraint_id":"c1"}],"unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 11. Missing constraint_id field
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[{"explanation":"desc"}],"unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
    }

    // 12. Non-array violations/unresolved
    {
        auto p = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":"not_array","unresolved":[]})");
        ROCLINT_CHECK(!p.ok());
        auto p2 = parseGoldfishAuditResponse(R"({"outcome":"Pass","violations":[],"unresolved":123})");
        ROCLINT_CHECK(!p2.ok());
    }

    // 13. End-to-end adapter audit: malformed shapes yield Malformed + Unresolved + goldfish_audit finding
    {
        GraniteGoldfishAdapter adapter;
        GoldfishAuditRequest req;
        req.originalPrompt = "Refactor Reader::open.";
        req.candidateRewrite = "Refactor Reader::open.";
        req.sessionId = "sess_r06";

        // Payloads missing required arrays
        auto mock1 = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Pass"})");
        };
        const auto res1 = adapter.audit(req, mock1);
        ROCLINT_CHECK(res1.executionStatus == AuditExecutionStatus::Malformed);
        ROCLINT_CHECK(res1.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res1.unresolved.size() == 1);
        ROCLINT_CHECK(res1.unresolved[0].constraintId == "goldfish_audit");

        // Payloads containing empty finding objects
        auto mock2 = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Pass","violations":[{}],"unresolved":[]})");
        };
        const auto res2 = adapter.audit(req, mock2);
        ROCLINT_CHECK(res2.executionStatus == AuditExecutionStatus::Malformed);
        ROCLINT_CHECK(res2.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res2.unresolved.size() == 1);
        ROCLINT_CHECK(res2.unresolved[0].constraintId == "goldfish_audit");
    }
}

void testR05AdvisoryAuthorityAndHonestUncertainty() {
    GraniteGoldfishAdapter adapter;
    GoldfishAuditRequest req;
    req.originalPrompt = "Draw a plain chart.";
    req.candidateRewrite = "Draw a plain chart.";
    req.sessionId = "sess_r05";

    ScopedConstraint advisory;
    advisory.id = "optional_glow";
    advisory.rule = "Use glowing particles";
    advisory.origin = Origin::Model;
    advisory.derivation = Derivation::Assumed;
    advisory.enforcement = Enforcement::Advisory;

    ScopedConstraint condInactive;
    condInactive.id = "c_token_rule";
    condInactive.rule = "Do not touch token validation";
    condInactive.origin = Origin::User;
    condInactive.derivation = Derivation::Direct;
    condInactive.enforcement = Enforcement::Hard;
    condInactive.polarity = Polarity::Prohibit;
    condInactive.appliesWhen = "touches_token_validation"; // Inactive for "Draw a plain chart."

    req.constraints = {advisory, condInactive};

    // 1. Advisory-only Unresolved: must NOT block approval -> resolves to Pass with 0 findings
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"optional_glow","explanation":"Cannot verify glow"}]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.empty());
        ROCLINT_CHECK(res.executionStatus == AuditExecutionStatus::Completed);
    }

    // 2. Inactive conditional Unresolved: must NOT block approval -> resolves to Pass
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"c_token_rule","explanation":"Token validation status unknown"}]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.empty());
    }

    // 3. Inactive conditional Fail: must NOT block approval -> resolves to Pass
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Fail","violations":[{"constraint_id":"c_token_rule","explanation":"Candidate touched token validation"}],"unresolved":[]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.empty());
    }

    // 4. Unexplained Unresolved (totalReported == 0): must preserve honest uncertainty under goldfish_audit
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res.unresolved.size() == 1);
        ROCLINT_CHECK(res.unresolved[0].constraintId == "goldfish_audit");
    }

    // 5. Unexplained Fail (totalReported == 0): must preserve honest fail under goldfish_audit
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Fail","violations":[],"unresolved":[]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.violations.size() == 1);
        ROCLINT_CHECK(res.violations[0].constraintId == "goldfish_audit");
    }

    // 6. Unknown constraint ID in violations: demoted to unresolved, preserving unknown ID
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Fail","violations":[{"constraint_id":"c_hallucinated_rule","explanation":"Unknown rule violated"}],"unresolved":[]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.size() == 1);
        ROCLINT_CHECK(res.unresolved[0].constraintId == "c_hallucinated_rule");
    }

    // 7. Unknown constraint ID in unresolved: preserved in combined.unresolved
    {
        auto mock = [](std::string_view) -> Result<std::string> {
            return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"c_unknown_rule","explanation":"Cannot verify mystery constraint"}]})");
        };
        const auto res = adapter.audit(req, mock);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res.unresolved.size() == 1);
        ROCLINT_CHECK(res.unresolved[0].constraintId == "c_unknown_rule");
    }
}

void testDiscoveryAuditStageAndRepairLoop() {
    GraniteGoldfishAdapter adapter;
    {
        GoldfishDiscoveryAuditRequest request;
        request.originalPrompt = "Set the timeout to 500 milliseconds.";
        request.proposedAnalysis.missingDecisions.push_back({"md_visible", "What timeout should be used?", true});
        request.proposedAnalysis.assumptions.push_back({"asm_visible", "A different timeout is acceptable", true});
        const auto prompt = buildGoldfishDiscoveryAuditPrompt(request);
        ROCLINT_CHECK(prompt.find("md_visible") != std::string::npos);
        ROCLINT_CHECK(prompt.find("What timeout should be used?") != std::string::npos);
        ROCLINT_CHECK(prompt.find("asm_visible") != std::string::npos);
    }

    // 1. False conflict detection
    GoldfishDiscoveryAuditRequest req1;
    req1.originalPrompt = "Implement an LRU cache with O(1) operations.";
    req1.proposedAnalysis.conflicts.push_back({"conf_1", "No conflicting requirements identified"});
    auto res1 = adapter.auditDiscovery(req1, nullptr);
    ROCLINT_CHECK(res1.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!res1.violations.empty());
    ROCLINT_CHECK(res1.violations[0].itemId == "conf_1");

    // 2. Duplicate alternatives detection
    GoldfishDiscoveryAuditRequest req2;
    req2.originalPrompt = "Configure database connection timeouts.";
    roclint::core::analysis::ChoiceGroup grp;
    grp.id = "grp_timeout";
    grp.question = "Which timeout?";
    grp.alternatives.push_back({"alt_1", "Connect timeout"});
    grp.alternatives.push_back({"alt_2", "connect timeout"}); // duplicate case-insensitive
    req2.proposedAnalysis.choiceGroups.push_back(grp);
    auto res2 = adapter.auditDiscovery(req2, nullptr);
    ROCLINT_CHECK(res2.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!res2.violations.empty());

    // 3. Prohibited color violation
    GoldfishDiscoveryAuditRequest req3;
    req3.originalPrompt = "Design a banner. The background cannot be purple.";
    roclint::core::analysis::ChoiceGroup colorGrp;
    colorGrp.id = "grp_color";
    colorGrp.question = "Which theme?";
    colorGrp.alternatives.push_back({"alt_c1", "Deep purple gradient"});
    colorGrp.alternatives.push_back({"alt_c2", "Ocean blue"});
    req3.proposedAnalysis.choiceGroups.push_back(colorGrp);
    auto res3 = adapter.auditDiscovery(req3, nullptr);
    ROCLINT_CHECK(res3.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!res3.violations.empty());

    // 4. Rejecting one prohibited option must preserve the still-unresolved decision.
    GoldfishDiscoveryAuditRequest repairReq;
    repairReq.originalPrompt = "Design a banner. The background cannot be purple.";
    repairReq.proposedAnalysis.conflicts.push_back({"conf_false", "No conflicting requirements identified"});
    roclint::core::analysis::ChoiceGroup palette;
    palette.id = "grp_palette";
    palette.question = "Which safe background should the banner use?";
    palette.alternatives.push_back({"alt_purple", "Deep purple gradient"});
    palette.alternatives.push_back({"alt_blue", "Ocean blue"});
    palette.alternatives.push_back({"alt_green", "Forest green"});
    repairReq.proposedAnalysis.choiceGroups.push_back(palette);

    // A model may name both the group and the offending option. The specific option wins so the
    // valid blue/green alternatives are not discarded with it.
    GoldfishDiscoveryAuditResult mixedIds;
    mixedIds.outcome = ValidationOutcome::Fail;
    mixedIds.violations.push_back({"grp_palette", "Group contains a prohibited option"});
    mixedIds.violations.push_back({"alt_purple", "Purple violates the explicit prohibition"});
    auto mixedRepair = GraniteGoldfishAdapter::repairDiscovery(repairReq.proposedAnalysis, mixedIds);
    ROCLINT_CHECK(mixedRepair.choiceGroups.size() == 1);
    ROCLINT_CHECK(mixedRepair.choiceGroups.front().alternatives.size() == 2);

    auto repairLoopRes = adapter.correctDiscoveryWithRepairLoop(repairReq, nullptr, 2, 25000);
    ROCLINT_CHECK(repairLoopRes.attemptsExecuted >= 1);
    ROCLINT_CHECK(repairLoopRes.analysis.conflicts.empty());
    ROCLINT_CHECK(repairLoopRes.analysis.choiceGroups.size() == 1);
    const auto& repairedPalette = repairLoopRes.analysis.choiceGroups.front();
    ROCLINT_CHECK(repairedPalette.id == "grp_palette");
    ROCLINT_CHECK(repairedPalette.question == palette.question);
    ROCLINT_CHECK(repairedPalette.alternatives.size() == 2);
    for (const auto& alt : repairedPalette.alternatives) {
        ROCLINT_CHECK(alt.id != "alt_purple");
        ROCLINT_CHECK(alt.id == "alt_blue" || alt.id == "alt_green");
    }
    ROCLINT_CHECK(repairLoopRes.analysis.missingDecisions.empty());

    // 4b. Naming a protected boundary while explicitly preserving it is compliant.
    GoldfishDiscoveryAuditRequest preserveReq;
    preserveReq.originalPrompt = "Refactor the handler without changing the public API.";
    roclint::core::analysis::ChoiceGroup apiGroup;
    apiGroup.id = "grp_api";
    apiGroup.question = "How should the public API be handled?";
    apiGroup.alternatives.push_back({"alt_keep", "Keep the public API unchanged", "Preserves the boundary"});
    apiGroup.alternatives.push_back({"alt_change", "Change the public API to a new shape", "Breaks the boundary"});
    preserveReq.proposedAnalysis.choiceGroups.push_back(apiGroup);
    auto preserveAudit = adapter.auditDiscovery(preserveReq, nullptr);
    ROCLINT_CHECK(preserveAudit.violations.size() == 1);
    ROCLINT_CHECK(preserveAudit.violations.front().itemId == "alt_change");

    // 5. When every option is invalid, the open decision survives as a missing decision rather
    // than silently disappearing from the analysis.
    GoldfishDiscoveryAuditRequest exhaustedReq;
    exhaustedReq.originalPrompt = "Design a banner. The background cannot be purple.";
    roclint::core::analysis::ChoiceGroup onlyBad;
    onlyBad.id = "grp_only_bad";
    onlyBad.question = "Which background should the banner use?";
    onlyBad.alternatives.push_back({"alt_bad_purple", "Purple gradient"});
    onlyBad.alternatives.push_back({"alt_also_purple", "Purple haze"});
    exhaustedReq.proposedAnalysis.choiceGroups.push_back(onlyBad);
    auto exhausted = adapter.correctDiscoveryWithRepairLoop(exhaustedReq, nullptr, 2, 25000);
    ROCLINT_CHECK(exhausted.analysis.choiceGroups.empty());
    ROCLINT_CHECK(exhausted.analysis.missingDecisions.size() == 1);
    ROCLINT_CHECK(exhausted.analysis.missingDecisions.front().question == onlyBad.question);

    std::cout << "  [PASS] Discovery audit and bounded repair loop preserve unresolved decisions.\n";
}

void testDiscoveryAuditParsersAreStrict() {
    // Unknown outcomes must be rejected, not promoted to Pass.
    auto unknown = parseGoldfishDiscoveryAuditResponse(R"({"outcome":"BANANA","violations":[],"unresolved":[]})");
    ROCLINT_CHECK(!unknown.ok());

    auto missingArrays = parseGoldfishDiscoveryAuditResponse(R"({"outcome":"Pass"})");
    ROCLINT_CHECK(!missingArrays.ok());

    auto inconsistentPass = parseGoldfishDiscoveryAuditResponse(
        R"({"outcome":"Pass","violations":[{"item_id":"grp_1","explanation":"bad"}],"unresolved":[]})");
    ROCLINT_CHECK(!inconsistentPass.ok());

    auto failWithoutFinding = parseGoldfishDiscoveryAuditResponse(
        R"({"outcome":"Fail","violations":[],"unresolved":[]})");
    ROCLINT_CHECK(!failWithoutFinding.ok());

    auto malformedFinding = parseGoldfishDiscoveryAuditResponse(
        R"({"outcome":"Fail","violations":[{"item_id":7,"explanation":"bad"}],"unresolved":[]})");
    ROCLINT_CHECK(!malformedFinding.ok());

    auto valid = parseGoldfishDiscoveryAuditResponse(
        R"({"outcome":"Fail","violations":[{"item_id":"grp_1","explanation":"bad"}],"unresolved":[]})");
    ROCLINT_CHECK(valid.ok());
    ROCLINT_CHECK(valid.value().outcome == ValidationOutcome::Fail);

    // Clarification parser has the same contract.
    auto clarifyUnknown = parseGoldfishClarificationAuditResponse(
        R"({"outcome":"BANANA","faithful":true,"relevant":true,"compatible":true,"violations":[],"unresolved":[]})");
    ROCLINT_CHECK(!clarifyUnknown.ok());

    auto clarifyMissingBool = parseGoldfishClarificationAuditResponse(
        R"({"outcome":"Pass","relevant":true,"compatible":true,"violations":[],"unresolved":[]})");
    ROCLINT_CHECK(!clarifyMissingBool.ok());

    auto clarifyInconsistentPass = parseGoldfishClarificationAuditResponse(
        R"({"outcome":"Pass","faithful":false,"relevant":true,"compatible":true,"violations":[],"unresolved":[]})");
    ROCLINT_CHECK(!clarifyInconsistentPass.ok());

    auto clarifyValid = parseGoldfishClarificationAuditResponse(
        R"({"outcome":"Pass","faithful":true,"relevant":true,"compatible":true,"violations":[],"unresolved":[]})");
    ROCLINT_CHECK(clarifyValid.ok());
    ROCLINT_CHECK(clarifyValid.value().outcome == ValidationOutcome::Pass);

    // An unavailable semantic audit stays unresolved; a deterministic-only pass is not proof.
    GoldfishDiscoveryAuditRequest req;
    req.originalPrompt = "Add a retry policy.";
    roclint::core::analysis::ChoiceGroup group;
    group.id = "grp_retry";
    group.question = "What should retry?";
    group.alternatives.push_back({"alt_a", "Retry the request"});
    group.alternatives.push_back({"alt_b", "Retry the batch"});
    req.proposedAnalysis.choiceGroups.push_back(group);
    GraniteTransport unavailable = [](std::string_view) -> Result<std::string> {
        return makeError(ErrorCode::ProviderUnavailable, "offline");
    };
    GraniteGoldfishAdapter adapter;
    const auto unavailableResult = adapter.auditDiscovery(req, unavailable);
    ROCLINT_CHECK(unavailableResult.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(unavailableResult.executionStatus == AuditExecutionStatus::Unavailable);

    const auto deterministicOnly = adapter.auditDiscovery(req, nullptr);
    ROCLINT_CHECK(deterministicOnly.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(deterministicOnly.executionStatus == AuditExecutionStatus::DeterministicOnly);

    GoldfishClarificationAuditRequest clarifyReq;
    clarifyReq.originalPrompt = "Add a retry policy.";
    clarifyReq.question = "What should retry?";
    clarifyReq.userAnswer = "Retry the request.";
    const auto clarifyUnavailable = adapter.auditClarification(clarifyReq, unavailable);
    ROCLINT_CHECK(clarifyUnavailable.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(clarifyUnavailable.executionStatus == AuditExecutionStatus::Unavailable);

    std::cout << "  [PASS] Discovery and clarification audit parsers are strict.\n";
}

void testRepairLoopStopsRepeatedRegeneration() {
    GoldfishDiscoveryAuditRequest req;
    req.originalPrompt = "Design a banner. The background cannot be purple.";
    roclint::core::analysis::ChoiceGroup palette;
    palette.id = "grp_palette";
    palette.question = "Which safe background?";
    palette.alternatives.push_back({"alt_purple", "Purple gradient"});
    palette.alternatives.push_back({"alt_blue", "Ocean blue"});
    palette.alternatives.push_back({"alt_green", "Forest green"});
    req.proposedAnalysis.choiceGroups.push_back(palette);

    int regenerations = 0;
    GraniteGoldfishAdapter adapter;
    GraniteTransport persistentFailure = [](std::string_view) -> Result<std::string> {
        return std::string(R"({"outcome":"Fail","violations":[{"item_id":"audit","explanation":"still unverifiable"}],"unresolved":[]})");
    };
    auto result = adapter.correctDiscoveryWithRepairLoop(
        req, persistentFailure, 3, 25000,
        [&](const GoldfishDiscoveryAuditResult&) -> Result<roclint::core::analysis::NormalizedAnalysis> {
            ++regenerations;
            return req.proposedAnalysis; // repeated identical candidate
        });
    ROCLINT_CHECK(regenerations == 1);
    ROCLINT_CHECK(result.attemptsExecuted <= 2);
    ROCLINT_CHECK(result.analysis.choiceGroups.size() == 1);

    std::cout << "  [PASS] Repeated discovery candidates stop the repair loop.\n";
}

void testClarificationAuditStageAppropriate() {
    GraniteGoldfishAdapter adapter;

    // 1. Blank answer fast-fail
    GoldfishClarificationAuditRequest reqBlank;
    reqBlank.originalPrompt = "Configure network port.";
    reqBlank.question = "Which port number?";
    reqBlank.userAnswer = "   ";
    auto resBlank = adapter.auditClarification(reqBlank, nullptr);
    ROCLINT_CHECK(resBlank.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!resBlank.faithful || !resBlank.relevant);

    // 2. Contradiction of negative prohibition
    GoldfishClarificationAuditRequest reqIncompat;
    reqIncompat.originalPrompt = "Refactor auth endpoints, do not touch token validation.";
    reqIncompat.question = "How should auth endpoints be refactored?";
    reqIncompat.userAnswer = "Modify token validation handlers to cache results.";
    ScopedConstraint negRule;
    negRule.id = "neg_token";
    negRule.rule = "do not touch token validation";
    negRule.polarity = Polarity::Prohibit;
    negRule.enforcement = Enforcement::Hard;
    reqIncompat.constraints = {negRule};

    auto resIncompat = adapter.auditClarification(reqIncompat, nullptr);
    ROCLINT_CHECK(resIncompat.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!resIncompat.compatible);
    ROCLINT_CHECK(!resIncompat.violations.empty());

    // 2b. Preservation wording names the protected boundary without violating it.
    GoldfishClarificationAuditRequest reqPreserve;
    reqPreserve.originalPrompt = "Refactor auth endpoints without changing the public API.";
    reqPreserve.question = "How should the public API be handled?";
    reqPreserve.userAnswer = "Keep the public API unchanged.";
    ScopedConstraint apiRule;
    apiRule.id = "neg_api";
    apiRule.rule = "without changing the public API";
    apiRule.polarity = Polarity::Prohibit;
    apiRule.enforcement = Enforcement::Hard;
    reqPreserve.constraints = {apiRule};
    auto resPreserve = adapter.auditClarification(reqPreserve, nullptr);
    ROCLINT_CHECK(resPreserve.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(resPreserve.compatible);

    // 3. Contradiction of explicit color constraint
    GoldfishClarificationAuditRequest reqColor;
    reqColor.originalPrompt = "Style the button. It cannot be purple.";
    reqColor.question = "What color palette?";
    reqColor.userAnswer = "Make it bright purple.";
    auto resColor = adapter.auditClarification(reqColor, nullptr);
    ROCLINT_CHECK(resColor.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!resColor.compatible);

    // 4. Model transport audit: valid custom answer accepted
    GoldfishClarificationAuditRequest reqValid;
    reqValid.originalPrompt = "Add a metrics exporter.";
    reqValid.question = "Which export protocol?";
    reqValid.userAnswer = "Prometheus OpenMetrics";
    reqValid.offeredAlternatives = {"StatsD", "OTLP"};

    GraniteTransport validTransport = [](std::string_view prompt) -> Result<std::string> {
        ROCLINT_CHECK(prompt.find("Prometheus OpenMetrics") != std::string::npos);
        return std::string(R"({
            "outcome": "Pass",
            "faithful": true,
            "relevant": true,
            "compatible": true,
            "violations": [],
            "unresolved": [],
            "feedback": ""
        })");
    };

    auto resValid = adapter.auditClarification(reqValid, validTransport);
    ROCLINT_CHECK(resValid.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(resValid.faithful && resValid.relevant && resValid.compatible);

    std::cout << "  [PASS] Clarification audit stage-appropriate passed.\n";
}

} // namespace

int main() {
    std::cout << "[RUNNING] test_granite_goldfish_adapter...\n";
    testPromptBuilding();
    testResponseParsing();
    testDeterministicPreGateFastFail();
    testAdvisoryZeroVetoPower();
    testModelSemanticViolationDetected();
    testModelHonestUnknowns();
    testGracefulDegradationOffline();
    testF01ExplicitOutcomesAndMalformedShapes();
    testF04AdvisoryVetoPrevention();
    testR06ParserBoundarySchemaValidation();
    testR05AdvisoryAuthorityAndHonestUncertainty();
    testDiscoveryAuditStageAndRepairLoop();
    testDiscoveryAuditParsersAreStrict();
    testRepairLoopStopsRepeatedRegeneration();
    testClarificationAuditStageAppropriate();
    std::cout << "[PASS] All granite goldfish adapter tests passed successfully.\n";
    return 0;
}
