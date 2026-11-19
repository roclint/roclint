#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/Json.hpp"
#include "roclint/review/ComprehensionReview.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include "support/wording_fixtures.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace roclint::core;
using namespace roclint::bridge;

namespace {

void testSourceTaskFindingIsAuthoritativeAndRepairable() {
    GraniteGoldfishAdapter auditor;
    GoldfishAuditRequest audit;
    audit.originalPrompt = "Make the widget less intrusive.";
    audit.candidateRewrite = "Summarize the user's feedback. Make the widget less intrusive.";
    auto reject = [](std::string_view) -> Result<std::string> {
        return std::string(R"({"outcome":"Fail","violations":[{"constraint_id":"original_task","explanation":"The candidate adds a summary task that the source does not request."}],"unresolved":[]})");
    };
    const auto rejected = auditor.audit(audit, reject);
    ROCLINT_CHECK(rejected.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(rejected.violations.size() == 1 && rejected.violations[0].constraintId == "original_task");
    ROCLINT_CHECK(rejected.unresolved.empty()); // A source-task failure is not demoted to an unknown model rule.

    ScopedConstraint collision;
    collision.id = "original_task"; collision.enforcement = Enforcement::Advisory;
    audit.constraints.push_back(collision);
    int modelCalls = 0;
    const auto invalid = auditor.audit(audit, [&](std::string_view) -> Result<std::string> {
        ++modelCalls; return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
    });
    ROCLINT_CHECK(modelCalls == 0 && invalid.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(invalid.executionStatus == AuditExecutionStatus::Malformed);

    const auto request = fx::wordingRequest({"Make the widget less intrusive.", "Make the widget less intrusive."}, 90);
    GraniteAdapter runtime;
    ROCLINT_CHECK(runtime.beginLoading());
    ROCLINT_CHECK(runtime.completeLoading(fx::testModel()));
    int rewrites = 0, audits = 0;
    auto rewrite = [&](std::string_view instructions) -> Result<std::string> {
        if (++rewrites == 1) return fx::rewriteReply("Summarize the user's feedback. Make the widget less intrusive.");
        ROCLINT_CHECK(instructions.find("original_task") != std::string_view::npos);
        ROCLINT_CHECK(instructions.find("<previous_candidate>") != std::string_view::npos);
        return fx::rewriteReply("Make the widget less intrusive.");
    };
    auto review = [&](std::string_view instructions) -> Result<std::string> {
        ROCLINT_CHECK(instructions.find("granite.goldfish_audit.v2") != std::string_view::npos);
        if (++audits == 1) return reject(instructions);
        return std::string(R"({"outcome":"Pass","violations":[],"unresolved":[]})");
    };
    const auto repaired = GraniteRewriteAdapter::synthesizeRewriteWithRepairLoop(
        request, runtime, rewrite, review, 2000, 1, 45000);
    ROCLINT_CHECK(rewrites == 2 && audits == 2 && repaired.attemptsExecuted == 2);
    ROCLINT_CHECK(repaired.repairedSuccessfully && repaired.auditVerdict.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(repaired.proposal.ok() && repaired.proposal.value().rewrittenText == "Make the widget less intrusive.");

    // A second audit that remains unresolved cannot be called a successful repair.
    rewrites = 0; audits = 0;
    auto unresolvedReview = [&](std::string_view instructions) -> Result<std::string> {
        if (++audits == 1) return reject(instructions);
        return std::string(R"({"outcome":"Unresolved","violations":[],"unresolved":[{"constraint_id":"original_task","explanation":"The intended action remains uncertain."}]})");
    };
    const auto uncertain = GraniteRewriteAdapter::synthesizeRewriteWithRepairLoop(
        request, runtime, rewrite, unresolvedReview, 2000, 1, 45000);
    ROCLINT_CHECK(!uncertain.repairedSuccessfully && uncertain.auditVerdict.outcome == ValidationOutcome::Unresolved);
}

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
    ROCLINT_CHECK(prompt.find("Instruction contract: granite.goldfish_audit.v2") != std::string::npos);
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

ComprehensionReviewRequest comprehensionFixture() {
    ComprehensionReviewRequest req;
    req.originalPrompt =
        "Design a warm, minimal album cover for a 1960s-inspired jazz record. "
        "Keep the title text exactly as written.";
    req.candidatePrompt =
        "Create a warm, minimal album cover for a 1960s-inspired jazz record with a square cover. "
        "Keep the title text exactly as written.";
    req.confirmedDecisions = {"Use a warm sepia palette", "Keep the title text exactly as written"};
    req.sourceExcerpts = {"warm, minimal album cover", "Keep the title text exactly as written"};
    req.taskKind = "creative";
    req.sessionId = "sess_review_fixture";
    req.revisionId = "rev_review_fixture";

    ScopedConstraint preserve;
    preserve.id = "c_title";
    preserve.rule = "Keep the title text exactly as written";
    preserve.origin = Origin::User;
    preserve.derivation = Derivation::Direct;
    preserve.enforcement = Enforcement::Hard;
    preserve.polarity = Polarity::Prohibit;
    preserve.aspect = "typography";
    preserve.authorityRef = "user";
    req.constraints = {preserve};
    return req;
}

json::Value reviewFinding(std::string id, std::string kind, std::string severity,
                          std::string summary, std::string sourceRef,
                          std::string sourceExcerpt, bool requiresUserDecision,
                          std::string proposedRepair) {
    json::Value finding = json::Value::object();
    finding.set("id", json::Value::string(std::move(id)));
    finding.set("kind", json::Value::string(std::move(kind)));
    finding.set("severity", json::Value::string(std::move(severity)));
    finding.set("summary", json::Value::string(std::move(summary)));
    finding.set("source_ref", json::Value::string(std::move(sourceRef)));
    finding.set("source_excerpt", json::Value::string(std::move(sourceExcerpt)));
    finding.set("requires_user_decision", json::Value::boolean(requiresUserDecision));
    finding.set("proposed_repair", json::Value::string(std::move(proposedRepair)));
    return finding;
}

std::string comprehensionResponse(
    std::vector<json::Value> findings, bool ready,
    std::string outcome = "A prompt that matches the confirmed intent.",
    std::vector<std::string> requirements = std::vector<std::string>{"follow the confirmed intent"},
    std::vector<std::string> assumptions = std::vector<std::string>{},
    std::vector<std::string> unresolved = std::vector<std::string>{},
    std::string rationale =
        "The candidate carries the confirmed decisions and preservation constraints.") {
    auto stringArray = [](const std::vector<std::string>& values) {
        json::Value array = json::Value::array();
        for (const auto& value : values) array.push(json::Value::string(value));
        return array;
    };

    json::Value root = json::Value::object();
    root.set("interpreted_outcome", json::Value::string(std::move(outcome)));
    root.set("interpreted_requirements", stringArray(requirements));
    root.set("inferred_assumptions", stringArray(assumptions));
    root.set("unresolved_decisions", stringArray(unresolved));
    root.set("ready_for_task", json::Value::boolean(ready));
    json::Value findingArray = json::Value::array();
    for (auto& finding : findings) findingArray.push(std::move(finding));
    root.set("findings", std::move(findingArray));
    root.set("rationale", json::Value::string(std::move(rationale)));
    return json::serialize(root);
}

void expectComprehensionFailure(const ComprehensionReviewRequest& request,
                                std::string_view output,
                                ComprehensionReviewStatus expectedStatus,
                                int expectedCalls) {
    int calls = 0;
    ComprehensionReviewTransport transport = [&](std::string_view) -> Result<std::string> {
        ++calls;
        return std::string(output);
    };
    const auto result = reviewComprehension(request, transport);
    ROCLINT_CHECK(result.status == expectedStatus);
    ROCLINT_CHECK(!result.readyForTask);
    ROCLINT_CHECK(!reviewAuthorizesApply(result));
    ROCLINT_CHECK(calls == expectedCalls);
}

void testComprehensionPromptIsFreshAndEvidenceBound() {
    auto req = comprehensionFixture();
    req.sessionId = "sess_DRAFTING_RATIONALE_MARKER";
    req.revisionId = "rev_HIDDEN_REASONING_MARKER";

    const std::string prompt = buildComprehensionReviewPrompt(req);
    ROCLINT_CHECK(prompt.find("Instruction contract: granite.comprehension_review.v1") !=
                  std::string::npos);
    ROCLINT_CHECK(prompt.find(req.candidatePrompt) != std::string::npos);
    ROCLINT_CHECK(prompt.find(req.originalPrompt) != std::string::npos);
    ROCLINT_CHECK(prompt.find("<decision ref=\"decision:1\">Use a warm sepia palette</decision>") !=
                  std::string::npos);
    ROCLINT_CHECK(prompt.find(
                      "<decision ref=\"decision:2\">Keep the title text exactly as written</decision>") !=
                  std::string::npos);
    ROCLINT_CHECK(prompt.find("<constraint ref=\"constraint:c_title\"") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Rule: Keep the title text exactly as written") != std::string::npos);
    ROCLINT_CHECK(prompt.find("<evidence ref=\"source:1\">warm, minimal album cover</evidence>") !=
                  std::string::npos);
    ROCLINT_CHECK(prompt.find("<task_kind>creative</task_kind>") != std::string::npos);
    ROCLINT_CHECK(prompt.find("source_ref must be exactly one of") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Do not require file paths") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Do not claim that this review establishes intent") !=
                  std::string::npos);

    // Drafting-only identity and hidden rationale are not forwarded to the fresh reviewer.
    ROCLINT_CHECK(prompt.find("DRAFTING_RATIONALE_MARKER") == std::string::npos);
    ROCLINT_CHECK(prompt.find("HIDDEN_REASONING_MARKER") == std::string::npos);

    // The candidate is presented before the original so it is interpreted on its own first.
    const auto candidateAt = prompt.find("<candidate_prompt>");
    const auto originalAt = prompt.find("<original_prompt>");
    ROCLINT_CHECK(candidateAt != std::string::npos);
    ROCLINT_CHECK(originalAt != std::string::npos);
    ROCLINT_CHECK(candidateAt < originalAt);

    // Source-linked constraint provenance travels with the rule.
    ROCLINT_CHECK(prompt.find("origin=\"User\"") != std::string::npos);
    ROCLINT_CHECK(prompt.find("enforcement=\"Hard\"") != std::string::npos);
    ROCLINT_CHECK(prompt.find("polarity=\"Prohibit\"") != std::string::npos);
}

void testComprehensionParserBindsInterpretationAndFindings() {
    const auto req = comprehensionFixture();

    const auto schema = json::parse(comprehensionReviewResponseSchema());
    ROCLINT_CHECK(schema.ok());
    const auto* schemaProperties = schema.value().find("properties");
    ROCLINT_CHECK(schemaProperties != nullptr && schemaProperties->isObject());
    ROCLINT_CHECK(schemaProperties->find("interpreted_outcome") != nullptr);
    ROCLINT_CHECK(schemaProperties->find("inferred_assumptions") != nullptr);
    ROCLINT_CHECK(schemaProperties->find("findings") != nullptr);
    ROCLINT_CHECK(schemaProperties->find("rationale") != nullptr);

    const std::string json = comprehensionResponse(
        {reviewFinding("f1", "omission", "required",
                       "The candidate drops the confirmed sepia palette direction.",
                       "decision:1", "Use a warm sepia palette", false,
                       "Restore the warm sepia palette in the candidate.")},
        true, "A warm, minimal jazz album cover with a preserved headline.",
        {"warm palette", "legible title"}, {"The cover will be square"},
        {"Which jazz era should dominate the typography?"},
        "The candidate preserves the headline but drops the confirmed palette.");

    auto parsed = parseComprehensionReviewResponse(json, req);
    ROCLINT_CHECK(parsed.ok());
    const auto& result = parsed.value();
    ROCLINT_CHECK(result.status == ComprehensionReviewStatus::Completed);
    ROCLINT_CHECK(result.interpretedOutcome ==
                  "A warm, minimal jazz album cover with a preserved headline.");
    ROCLINT_CHECK(result.interpretedRequirements.size() == 2);
    ROCLINT_CHECK(result.interpretedRequirements[0] == "warm palette");
    ROCLINT_CHECK(result.interpretedRequirements[1] == "legible title");
    ROCLINT_CHECK(result.findings.size() == 1);
    const auto& finding = result.findings.front();
    ROCLINT_CHECK(finding.id == "f1");
    ROCLINT_CHECK(finding.kind == ReviewFindingKind::Omission);
    ROCLINT_CHECK(finding.severity == ReviewFindingSeverity::Required);
    ROCLINT_CHECK(finding.summary == "The candidate drops the confirmed sepia palette direction.");
    ROCLINT_CHECK(finding.sourceRef == "decision:1");
    ROCLINT_CHECK(finding.sourceExcerpt == "Use a warm sepia palette");
    ROCLINT_CHECK(!finding.requiresUserDecision);
    ROCLINT_CHECK(finding.proposedRepair == "Restore the warm sepia palette in the candidate.");
    ROCLINT_CHECK(result.readyForTask);
    // readyForTask alone is insufficient: a required finding blocks authorization.
    ROCLINT_CHECK(!reviewAuthorizesApply(result));
    ROCLINT_CHECK(result.rationale.find("Reviewer's inferred assumptions:") != std::string::npos);
    ROCLINT_CHECK(result.rationale.find("The cover will be square") != std::string::npos);
    ROCLINT_CHECK(result.rationale.find("Which jazz era should dominate the typography?") !=
                  std::string::npos);
    ROCLINT_CHECK(result.rationale.find("not proof of intent") != std::string::npos);

    ROCLINT_CHECK(std::string(toString(ReviewFindingKind::MeaningDrift)) == "meaning_drift");
    ROCLINT_CHECK(std::string(toString(ReviewFindingSeverity::Blocking)) == "blocking");
    ROCLINT_CHECK(std::string(toString(ComprehensionReviewStatus::Completed)) == "completed");
}

void testComprehensionFindingKindsCoverDriftOmissionAndPreservation() {
    const auto req = comprehensionFixture();
    const std::string json = comprehensionResponse(
        {reviewFinding("f1", "meaning_drift", "required",
                       "The candidate turns the warm jazz cover into a neon club flyer.",
                       "original",
                       "Design a warm, minimal album cover for a 1960s-inspired jazz record.", false,
                       "Restore the warm, minimal jazz direction."),
         reviewFinding("f2", "omission", "required", "The confirmed sepia palette is absent.",
                       "decision:1", "Use a warm sepia palette", false,
                       "Carry the sepia palette into the candidate."),
         reviewFinding("f3", "unsupported_assumption", "advisory",
                       "The candidate assumes a square cover without confirmation.", "candidate",
                       "square cover", true, "Ask whether the cover should be square."),
         reviewFinding("f4", "negative_constraint", "blocking",
                       "The candidate replaces the confirmed headline.", "constraint:c_title",
                       "Keep the title text exactly as written", false,
                       "Restore the exact headline text."),
         reviewFinding("f5", "readiness", "required",
                       "The target platform for the cover is missing.", "candidate", "album cover",
                       true, "Ask which platform the cover is for."),
         reviewFinding("f6", "comprehension", "advisory",
                       "The opening does not state what artifact is being produced.", "source:1",
                       "warm, minimal album cover", false,
                       "State that the artifact is an album cover.")},
        false, "A neon club flyer with a changed headline.", {"warm palette"}, {},
        {"Which platform the cover is for?"},
        "The candidate drifts from the jazz direction and changes the headline.");

    auto parsed = parseComprehensionReviewResponse(json, req);
    ROCLINT_CHECK(parsed.ok());
    const auto& result = parsed.value();
    ROCLINT_CHECK(result.findings.size() == 6);
    ROCLINT_CHECK(result.findings[0].kind == ReviewFindingKind::MeaningDrift);
    ROCLINT_CHECK(result.findings[1].kind == ReviewFindingKind::Omission);
    ROCLINT_CHECK(result.findings[2].kind == ReviewFindingKind::UnsupportedAssumption);
    ROCLINT_CHECK(result.findings[3].kind == ReviewFindingKind::NegativeConstraint);
    ROCLINT_CHECK(result.findings[4].kind == ReviewFindingKind::Readiness);
    ROCLINT_CHECK(result.findings[5].kind == ReviewFindingKind::ComprehensionGap);
    ROCLINT_CHECK(result.findings[2].requiresUserDecision);
    ROCLINT_CHECK(result.findings[4].requiresUserDecision);
    ROCLINT_CHECK(result.findings[3].severity == ReviewFindingSeverity::Blocking);
    ROCLINT_CHECK(!result.readyForTask);
    ROCLINT_CHECK(!reviewAuthorizesApply(result));
    ROCLINT_CHECK(std::string(toString(result.findings[0].kind)) == "meaning_drift");
    ROCLINT_CHECK(std::string(toString(result.findings[1].kind)) == "omission");
    ROCLINT_CHECK(std::string(toString(result.findings[2].kind)) == "unsupported_assumption");
    ROCLINT_CHECK(std::string(toString(result.findings[3].kind)) == "negative_constraint");
    ROCLINT_CHECK(std::string(toString(result.findings[4].kind)) == "readiness");
    ROCLINT_CHECK(std::string(toString(result.findings[5].kind)) == "comprehension");
}

void testComprehensionReadinessIsTaskRelative() {
    auto creative = comprehensionFixture();
    creative.taskKind = "creative";
    creative.candidatePrompt =
        "Create a warm, minimal album cover for a 1960s-inspired jazz record with a legible title.";
    creative.confirmedDecisions = {"Use a warm sepia palette"};
    creative.sourceExcerpts = {"warm, minimal album cover"};
    creative.constraints.clear();

    const std::string creativePrompt = buildComprehensionReviewPrompt(creative);
    ROCLINT_CHECK(creativePrompt.find("<task_kind>creative</task_kind>") != std::string::npos);
    ROCLINT_CHECK(creativePrompt.find("Do not require file paths") != std::string::npos);
    auto creativeReview =
        parseComprehensionReviewResponse(comprehensionResponse({}, true), creative);
    ROCLINT_CHECK(creativeReview.ok());
    ROCLINT_CHECK(creativeReview.value().readyForTask);
    ROCLINT_CHECK(reviewAuthorizesApply(creativeReview.value()));

    ComprehensionReviewRequest code;
    code.originalPrompt = "Refactor src/parser.cpp. Preserve Parser::parse and the public API.";
    code.candidatePrompt = "Refactor the parser for clarity.";
    code.confirmedDecisions = {"Preserve the public API"};
    code.sourceExcerpts = {"Parser::parse"};
    code.taskKind = "code";
    ScopedConstraint api;
    api.id = "c_api";
    api.rule = "Preserve the public API";
    api.origin = Origin::User;
    api.derivation = Derivation::Direct;
    api.enforcement = Enforcement::Hard;
    api.polarity = Polarity::Prohibit;
    code.constraints = {api};

    const std::string codePrompt = buildComprehensionReviewPrompt(code);
    ROCLINT_CHECK(codePrompt.find("<task_kind>code</task_kind>") != std::string::npos);
    ROCLINT_CHECK(codePrompt.find("AST targets") != std::string::npos);
    ROCLINT_CHECK(codePrompt.find("Parser::parse") != std::string::npos);

    auto codeReview = parseComprehensionReviewResponse(
        comprehensionResponse(
            {reviewFinding("f1", "omission", "blocking",
                           "The candidate omits the explicit Parser::parse target.", "original",
                           "Parser::parse", false,
                           "Name Parser::parse and state that it is preserved.")},
            false, "A generic parser refactor without an explicit target.",
            {"preserve the public API"}),
        code);
    ROCLINT_CHECK(codeReview.ok());
    ROCLINT_CHECK(!codeReview.value().readyForTask);
    ROCLINT_CHECK(!reviewAuthorizesApply(codeReview.value()));
    ROCLINT_CHECK(codeReview.value().findings.size() == 1);
    ROCLINT_CHECK(codeReview.value().findings[0].sourceExcerpt == "Parser::parse");
}

void testComprehensionConfirmedDecisionProvenance() {
    auto req = comprehensionFixture();
    req.confirmedDecisions = {"Use the custom direction: deep forest green and brushed brass"};
    const std::string prompt = buildComprehensionReviewPrompt(req);
    ROCLINT_CHECK(prompt.find("<decision ref=\"decision:1\">Use the custom direction: deep "
                              "forest green and brushed brass</decision>") != std::string::npos);

    auto parsed = parseComprehensionReviewResponse(
        comprehensionResponse(
            {reviewFinding("f1", "meaning_drift", "required",
                           "The candidate ignores the custom forest-green direction.", "decision:1",
                           "deep forest green and brushed brass", false,
                           "Carry the custom color direction into the candidate.")},
            false),
        req);
    ROCLINT_CHECK(parsed.ok());
    ROCLINT_CHECK(parsed.value().findings.size() == 1);
    ROCLINT_CHECK(parsed.value().findings[0].sourceRef == "decision:1");
    ROCLINT_CHECK(parsed.value().findings[0].sourceExcerpt ==
                  "deep forest green and brushed brass");
}

void testComprehensionDeterministicPreservationPrecheck() {
    ComprehensionReviewRequest req;
    req.originalPrompt = "Refactor the auth handler but do not touch token validation logic.";
    req.candidatePrompt =
        "Refactor the auth handler and modify the token validation logic for speed.";
    req.confirmedDecisions = {"Do not touch token validation logic"};
    req.sourceExcerpts = {"do not touch token validation logic"};
    req.taskKind = "code";

    ScopedConstraint preserve;
    preserve.id = "c_token";
    preserve.rule = "do not touch token validation logic";
    preserve.origin = Origin::User;
    preserve.derivation = Derivation::Direct;
    preserve.enforcement = Enforcement::Hard;
    preserve.polarity = Polarity::Prohibit;
    preserve.aspect = "security";
    req.constraints = {preserve};

    int calls = 0;
    const auto result = reviewComprehension(
        req, [&](std::string_view) -> Result<std::string> {
            ++calls;
            return comprehensionResponse({}, true);
        });

    ROCLINT_CHECK(calls == 1);
    ROCLINT_CHECK(result.status == ComprehensionReviewStatus::Completed);
    ROCLINT_CHECK(result.findings.size() == 1);
    ROCLINT_CHECK(result.findings[0].kind == ReviewFindingKind::NegativeConstraint);
    ROCLINT_CHECK(result.findings[0].severity == ReviewFindingSeverity::Blocking);
    ROCLINT_CHECK(result.findings[0].sourceRef == "constraint:c_token");
    ROCLINT_CHECK(result.findings[0].sourceExcerpt == "do not touch token validation logic");
    ROCLINT_CHECK(!result.findings[0].requiresUserDecision);
    ROCLINT_CHECK(!result.readyForTask);
    ROCLINT_CHECK(!reviewAuthorizesApply(result));
    ROCLINT_CHECK(result.rationale.find("Deterministic prechecks added") != std::string::npos);
}

void testComprehensionMemberUsesConfiguredReconciler() {
    ComprehensionReviewRequest req;
    req.originalPrompt = "Refactor the parser but keep the public API.";
    req.candidatePrompt = "Refactor the parser and change the public API.";
    req.confirmedDecisions = {"Keep the public API"};
    req.sourceExcerpts = {"keep the public API"};
    req.taskKind = "code";

    ScopedConstraint preserve;
    preserve.id = "c_api";
    preserve.rule = "keep the public API";
    preserve.origin = Origin::User;
    preserve.derivation = Derivation::Direct;
    preserve.enforcement = Enforcement::Hard;
    preserve.polarity = Polarity::Prohibit;
    preserve.appliesWhen = "review_api_active";
    req.constraints = {preserve};

    ConstraintReconciler reconciler;
    reconciler.registerPredicate("review_api_active", [](std::string_view) {
        return ApplicabilityStatus::Active;
    });
    GraniteGoldfishAdapter adapter(reconciler);
    int calls = 0;
    const auto result = adapter.reviewComprehension(
        req, [&](std::string_view) -> Result<std::string> {
            ++calls;
            return comprehensionResponse({}, true);
        });

    ROCLINT_CHECK(calls == 1);
    ROCLINT_CHECK(result.status == ComprehensionReviewStatus::Completed);
    ROCLINT_CHECK(result.findings.size() == 1);
    ROCLINT_CHECK(result.findings[0].severity == ReviewFindingSeverity::Blocking);
    ROCLINT_CHECK(result.findings[0].sourceRef == "constraint:c_api");
    ROCLINT_CHECK(!reviewAuthorizesApply(result));
}

void testComprehensionFailsClosed() {
    const auto req = comprehensionFixture();
    const std::string valid = comprehensionResponse({}, true);

    {
        auto bad = req;
        bad.candidatePrompt = "   ";
        expectComprehensionFailure(bad, valid, ComprehensionReviewStatus::Rejected, 0);
    }
    {
        auto bad = req;
        bad.originalPrompt.clear();
        expectComprehensionFailure(bad, valid, ComprehensionReviewStatus::Rejected, 0);
    }
    {
        auto bad = req;
        bad.confirmedDecisions = {""};
        expectComprehensionFailure(bad, valid, ComprehensionReviewStatus::Rejected, 0);
    }
    {
        auto bad = req;
        bad.sourceExcerpts = {""};
        expectComprehensionFailure(bad, valid, ComprehensionReviewStatus::Rejected, 0);
    }

    expectComprehensionFailure(req, "{not valid json}", ComprehensionReviewStatus::Malformed, 1);
    expectComprehensionFailure(req, valid.substr(0, valid.size() - 1),
                               ComprehensionReviewStatus::Malformed, 1);

    auto unknownSeverity = comprehensionResponse(
        {reviewFinding("f1", "readiness", "critical", "An unknown severity.", "candidate",
                       "album cover", false, "Ask the user.")},
        true);
    expectComprehensionFailure(req, unknownSeverity, ComprehensionReviewStatus::Malformed, 1);

    auto unknownKind = comprehensionResponse(
        {reviewFinding("f1", "vibes", "required", "An unknown kind.", "candidate", "album cover",
                       false, "Ask the user.")},
        false);
    expectComprehensionFailure(req, unknownKind, ComprehensionReviewStatus::Malformed, 1);

    auto duplicateIds = comprehensionResponse(
        {reviewFinding("f1", "omission", "required", "First finding.", "decision:1",
                       "Use a warm sepia palette", false, "Restore the palette."),
         reviewFinding("f1", "meaning_drift", "required", "Second finding.", "original",
                       "Design a warm, minimal album cover for a 1960s-inspired jazz record.",
                       false, "Restore the direction.")},
        false);
    expectComprehensionFailure(req, duplicateIds, ComprehensionReviewStatus::Malformed, 1);

    expectComprehensionFailure(req, comprehensionResponse({}, true, ""),
                               ComprehensionReviewStatus::Malformed, 1);

    auto fabricatedQuote = comprehensionResponse(
        {reviewFinding("f1", "omission", "required", "The candidate omits a sentence.", "original",
                       "This sentence is not in the original.", false, "Restore the sentence.")},
        false);
    expectComprehensionFailure(req, fabricatedQuote, ComprehensionReviewStatus::Malformed, 1);

    auto unknownRef = comprehensionResponse(
        {reviewFinding("f1", "omission", "required", "The candidate omits a sentence.", "nowhere",
                       "album cover", false, "Restore the sentence.")},
        false);
    expectComprehensionFailure(req, unknownRef, ComprehensionReviewStatus::Malformed, 1);

    expectComprehensionFailure(req, comprehensionResponse({}, false),
                               ComprehensionReviewStatus::Malformed, 1);

    // A bare ready response with no interpretation is a generic Pass, not a review.
    expectComprehensionFailure(req, comprehensionResponse({}, true, "ok", {}, {}, {}, "ok"),
                               ComprehensionReviewStatus::Malformed, 1);

    std::string missingAssumptions = valid;
    const std::string field = "\"inferred_assumptions\":[],";
    const auto fieldAt = missingAssumptions.find(field);
    ROCLINT_CHECK(fieldAt != std::string::npos);
    missingAssumptions.erase(fieldAt, field.size());
    expectComprehensionFailure(req, missingAssumptions, ComprehensionReviewStatus::Malformed, 1);

    const std::string oversized(70 * 1024, 'x');
    expectComprehensionFailure(req, oversized, ComprehensionReviewStatus::Malformed, 1);

    int calls = 0;
    const auto unavailable = reviewComprehension(
        req, [&](std::string_view) -> Result<std::string> {
            ++calls;
            return makeError(ErrorCode::ProviderUnavailable, "offline");
        });
    ROCLINT_CHECK(calls == 1);
    ROCLINT_CHECK(unavailable.status == ComprehensionReviewStatus::Unavailable);
    ROCLINT_CHECK(!unavailable.readyForTask);
    ROCLINT_CHECK(!reviewAuthorizesApply(unavailable));

    const auto noTransport = reviewComprehension(req);
    ROCLINT_CHECK(noTransport.status == ComprehensionReviewStatus::Unavailable);
    ROCLINT_CHECK(!reviewAuthorizesApply(noTransport));
}

void testComprehensionUserDecisionBlocksApply() {
    const auto req = comprehensionFixture();
    auto parsed = parseComprehensionReviewResponse(
        comprehensionResponse(
            {reviewFinding("f1", "readiness", "advisory",
                           "Two palettes are compatible; the choice is consequential.",
                           "decision:1", "Use a warm sepia palette", true,
                           "Ask the user to choose between sepia and neutral gray.")},
            true),
        req);
    ROCLINT_CHECK(parsed.ok());
    ROCLINT_CHECK(parsed.value().readyForTask);
    ROCLINT_CHECK(parsed.value().findings.size() == 1);
    ROCLINT_CHECK(parsed.value().findings[0].requiresUserDecision);
    // The consequential choice must return to the interview even though readiness is true.
    ROCLINT_CHECK(!reviewAuthorizesApply(parsed.value()));

    auto advisoryOnly = parseComprehensionReviewResponse(
        comprehensionResponse(
            {reviewFinding("f1", "readiness", "advisory",
                           "A caption could improve the cover but is not required.", "candidate",
                           "square cover", false, "Consider adding a short caption.")},
            true),
        req);
    ROCLINT_CHECK(advisoryOnly.ok());
    ROCLINT_CHECK(advisoryOnly.value().readyForTask);
    ROCLINT_CHECK(reviewAuthorizesApply(advisoryOnly.value()));

    auto completedButBlocked = advisoryOnly.value();
    completedButBlocked.findings[0].severity = ReviewFindingSeverity::Blocking;
    completedButBlocked.findings[0].requiresUserDecision = false;
    ROCLINT_CHECK(!reviewAuthorizesApply(completedButBlocked));

    auto noInterpretation = advisoryOnly.value();
    noInterpretation.interpretedOutcome.clear();
    ROCLINT_CHECK(!reviewAuthorizesApply(noInterpretation));

    auto unavailable = advisoryOnly.value();
    unavailable.status = ComprehensionReviewStatus::Unavailable;
    ROCLINT_CHECK(!reviewAuthorizesApply(unavailable));
}

void testComprehensionReviewIsSingleBoundedCall() {
    const auto req = comprehensionFixture();
    int calls = 0;
    const auto result = reviewComprehension(
        req, [&](std::string_view prompt) -> Result<std::string> {
            ++calls;
            ROCLINT_CHECK(prompt.find("<candidate_prompt>") != std::string::npos);
            ROCLINT_CHECK(prompt.find("<original_prompt>") != std::string::npos);
            return comprehensionResponse({}, true);
        });
    ROCLINT_CHECK(calls == 1);
    ROCLINT_CHECK(result.status == ComprehensionReviewStatus::Completed);
    ROCLINT_CHECK(result.findings.empty());
    ROCLINT_CHECK(reviewAuthorizesApply(result));

    calls = 0;
    const auto malformed = reviewComprehension(
        req, [&](std::string_view) -> Result<std::string> {
            ++calls;
            return std::string(R"({"interpreted_outcome":"x"})");
        });
    ROCLINT_CHECK(calls == 1);
    ROCLINT_CHECK(malformed.status == ComprehensionReviewStatus::Malformed);
    ROCLINT_CHECK(!reviewAuthorizesApply(malformed));

    calls = 0;
    const auto unavailable = reviewComprehension(
        req, [&](std::string_view) -> Result<std::string> {
            ++calls;
            return makeError(ErrorCode::ProviderUnavailable, "offline");
        });
    ROCLINT_CHECK(calls == 1);
    ROCLINT_CHECK(unavailable.status == ComprehensionReviewStatus::Unavailable);
}

void testComprehensionUnicodeAndEscapedEvidencePreserved() {
    ComprehensionReviewRequest req;
    req.originalPrompt =
        "Réécris le titre « Café — naïve » sans changer l'apostrophe. "
        "Use <keep intact> & preserve \"quotes\".";
    req.candidatePrompt = "Réécris le titre « Café — naïve » en gardant l'apostrophe.";
    req.confirmedDecisions = {"Conserve « Café — naïve » exactement"};
    req.sourceExcerpts = {"Café — naïve"};
    req.taskKind = "general";

    ScopedConstraint title;
    title.id = "c_unicode";
    title.rule = "Conserve « Café — naïve » exactement";
    title.origin = Origin::User;
    title.derivation = Derivation::Direct;
    title.enforcement = Enforcement::Hard;
    title.polarity = Polarity::Prohibit;
    req.constraints = {title};

    const std::string prompt = buildComprehensionReviewPrompt(req);
    ROCLINT_CHECK(prompt.find("Réécris le titre « Café — naïve » sans changer l&apos;apostrophe.") !=
                  std::string::npos);
    ROCLINT_CHECK(prompt.find("Conserve « Café — naïve » exactement") != std::string::npos);
    ROCLINT_CHECK(prompt.find("Café — naïve") != std::string::npos);
    ROCLINT_CHECK(prompt.find(
                      "Use &lt;keep intact&gt; &amp; preserve &quot;quotes&quot;.") !=
                  std::string::npos);

    const auto unicodeFinding =
        reviewFinding("f1", "negative_constraint", "blocking",
                      "The candidate changes the protected accented title.", "constraint:c_unicode",
                      "Conserve « Café — naïve » exactement", false,
                      "Restore the exact accented title.");
    auto parsedUnicode = parseComprehensionReviewResponse(
        comprehensionResponse({unicodeFinding}, false, "A drifted title."), req);
    ROCLINT_CHECK(parsedUnicode.ok());
    ROCLINT_CHECK(parsedUnicode.value().findings[0].sourceExcerpt ==
                  "Conserve « Café — naïve » exactement");

    const auto escapedFinding = reviewFinding(
        "f1", "omission", "required", "The candidate omits the literal keep-intact marker.",
        "original", "Use &lt;keep intact&gt; &amp; preserve &quot;quotes&quot;", false,
        "Restore the exact marker.");
    auto parsedEscaped = parseComprehensionReviewResponse(
        comprehensionResponse({escapedFinding}, false, "A drifted marker."), req);
    ROCLINT_CHECK(parsedEscaped.ok());
    ROCLINT_CHECK(parsedEscaped.value().findings[0].sourceExcerpt ==
                  "Use <keep intact> & preserve \"quotes\"");
}

} // namespace

int main() {
    std::cout << "[RUNNING] test_granite_goldfish_adapter...\n";
    testPromptBuilding();
    testSourceTaskFindingIsAuthoritativeAndRepairable();
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
    testComprehensionPromptIsFreshAndEvidenceBound();
    testComprehensionParserBindsInterpretationAndFindings();
    testComprehensionFindingKindsCoverDriftOmissionAndPreservation();
    testComprehensionReadinessIsTaskRelative();
    testComprehensionConfirmedDecisionProvenance();
    testComprehensionDeterministicPreservationPrecheck();
    testComprehensionMemberUsesConfiguredReconciler();
    testComprehensionFailsClosed();
    testComprehensionUserDecisionBlocksApply();
    testComprehensionReviewIsSingleBoundedCall();
    testComprehensionUnicodeAndEscapedEvidencePreserved();
    std::cout << "  [PASS] Comprehension review contract cases passed.\n";
    std::cout << "[PASS] All granite goldfish adapter tests passed successfully.\n";
    return 0;
}
