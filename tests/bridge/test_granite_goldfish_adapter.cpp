#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include "support/check.hpp"

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
            "violations": [{"constraint_id": "c_breach", "explanation": "Negative boundary violated"}]
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
        return std::string(R"({"outcome": "Pass"})");
    };

    // The candidate explicitly violates the negative prohibition.
    // Deterministic C++ pre-gating must catch this and return Fail immediately
    // WITHOUT invoking transport!
    const auto result = adapter.audit(req, countingTransport);

    ROCLINT_CHECK(result.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(result.violations.size() == 1);
    ROCLINT_CHECK(result.violations[0].constraintId == "c_neg_01");
    ROCLINT_CHECK(transportCalls == 0); // Fast-fail: model was never invoked!
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
            ]
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
    }

    // 2. Transport error (model timed out or process crashed)
    {
        GraniteTransport failingTransport = [](std::string_view) -> Result<std::string> {
            return makeError(ErrorCode::ProviderUnavailable, "Granite runtime offline");
        };

        const auto result = adapter.audit(req, failingTransport);
        // Graceful fallback to deterministic result
        ROCLINT_CHECK(result.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(result.violations.empty());
    }

    // 3. Unparseable JSON from model
    {
        GraniteTransport garbageTransport = [](std::string_view) -> Result<std::string> {
            return std::string("Sorry, I cannot help with that as an AI language model.");
        };

        const auto result = adapter.audit(req, garbageTransport);
        // Preserves deterministic pass, but marks unresolved note regarding model response
        ROCLINT_CHECK(result.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(result.violations.empty());
        ROCLINT_CHECK(!result.unresolved.empty());
        ROCLINT_CHECK(result.unresolved[0].constraintId == "goldfish_audit");
    }
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
    std::cout << "[PASS] All granite goldfish adapter tests passed successfully.\n";
    return 0;
}
