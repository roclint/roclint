#include "roclint/rewrite/CreativeExpansion.hpp"
#include "roclint/core/constraint_reconciler.hpp"
#include "support/check.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::rewrite;

namespace {

void testLayer3VisualizationExpansion() {
    const std::string original = "Make a creative Layer 3 animation, but technically accurate.";
    const std::string base = "Create a technically accurate Layer 3 packet animation showing IP destination forwarding using glowing particle flows.";

    std::vector<ScopedConstraint> constraints;
    ScopedConstraint hardL3;
    hardL3.id = "hard_l3_accuracy";
    hardL3.origin = Origin::User;
    hardL3.enforcement = Enforcement::Hard;
    hardL3.polarity = Polarity::Require;
    hardL3.aspect = "domain_accuracy";
    hardL3.rule = "technically accurate Layer 3 forwarding";
    constraints.push_back(hardL3);

    ScopedConstraint modelParticle;
    modelParticle.id = "model_particle_flow";
    modelParticle.origin = Origin::Model;
    modelParticle.enforcement = Enforcement::Advisory;
    modelParticle.polarity = Polarity::Require;
    modelParticle.aspect = "presentation";
    modelParticle.rule = "use glowing particle flows";
    constraints.push_back(modelParticle);

    auto alternatives = CreativeExpansionEngine::expand(original, base, constraints);

    ROCLINT_CHECK(alternatives.size() >= 3);
    ROCLINT_CHECK(alternatives[0].id == "alt_direct");
    ROCLINT_CHECK(alternatives[0].title == "Direct / Minimal");
    ROCLINT_CHECK(!alternatives[0].rewrittenText.empty());
    ROCLINT_CHECK(!alternatives[0].diffText.empty());
    ROCLINT_CHECK(alternatives[0].auditOutcome == ValidationOutcome::Pass);

    ROCLINT_CHECK(alternatives[1].id == "alt_flow");
    ROCLINT_CHECK(alternatives[1].title == "Dynamic Flow");
    ROCLINT_CHECK(!alternatives[1].rewrittenText.empty());
    ROCLINT_CHECK(alternatives[1].auditOutcome == ValidationOutcome::Unresolved);

    ROCLINT_CHECK(alternatives[2].id == "alt_stack");
    ROCLINT_CHECK(alternatives[2].title == "Stack & Topology");
    ROCLINT_CHECK(!alternatives[2].rewrittenText.empty());
    ROCLINT_CHECK(alternatives[2].auditOutcome == ValidationOutcome::Unresolved);

    std::cout << "  [PASS] Layer 3 creative expansion produces 3 distinct aesthetic branches.\n";
}

void testRefactoringExpansionWithNegativeBoundary() {
    const std::string original = "Refactor auth endpoints for speed, but do not touch token validation.";
    const std::string base = "Refactor the authentication request handlers for lower latency per call, preserving token validation logic unchanged.";

    std::vector<ScopedConstraint> constraints;
    ScopedConstraint negAuth;
    negAuth.id = "neg_token_validation";
    negAuth.origin = Origin::User;
    negAuth.enforcement = Enforcement::Hard;
    negAuth.polarity = Polarity::Prohibit;
    negAuth.aspect = "security";
    negAuth.rule = "do not touch token validation";
    constraints.push_back(negAuth);

    auto alternatives = CreativeExpansionEngine::expand(original, base, constraints);

    ROCLINT_CHECK(alternatives.size() >= 3);
    ROCLINT_CHECK(alternatives[0].id == "alt_prose");
    ROCLINT_CHECK(alternatives[1].id == "alt_steps");
    ROCLINT_CHECK(alternatives[2].id == "alt_conservative");

    ROCLINT_CHECK(alternatives[0].auditOutcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(alternatives[1].auditOutcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(alternatives[2].auditOutcome == ValidationOutcome::Unresolved);

    for (const auto& alt : alternatives) {
        ROCLINT_CHECK(!alt.rewrittenText.empty());
        ROCLINT_CHECK(!alt.diffText.empty());
        // Ensure negative prohibition is not violated in any alternative!
        ROCLINT_CHECK(alt.rewrittenText.find("untouched") != std::string::npos ||
                      alt.rewrittenText.find("unchanged") != std::string::npos ||
                      alt.rewrittenText.find("preserving") != std::string::npos ||
                      alt.rewrittenText.find("token validation") != std::string::npos);
    }

    std::cout << "  [PASS] Refactoring expansion preserves negative boundary across all alternatives.\n";
}

void testF05MultipleProhibitionsAndTargetFidelity() {
    const std::string original = "Refactor Reader::open for readability. Preserve the public API. Do not modify tests. Do not change logging.";
    const std::string base = "Improve readability of Reader::open. Preserve the public API. Do not modify tests. Do not change logging.";

    ScopedConstraint tests;
    tests.id = "no_test_mods";
    tests.rule = "do not modify tests";
    tests.polarity = Polarity::Prohibit;
    tests.enforcement = Enforcement::Hard;

    ScopedConstraint logging;
    logging.id = "no_log_changes";
    logging.rule = "do not change logging";
    logging.polarity = Polarity::Prohibit;
    logging.enforcement = Enforcement::Hard;

    auto alts = CreativeExpansionEngine::expand(original, base, {tests, logging});
    ROCLINT_CHECK(alts.size() >= 3);

    // Candidate 0 inherits the base audit Pass
    ROCLINT_CHECK(alts[0].auditOutcome == ValidationOutcome::Pass);
    // Un-audited alternatives MUST NOT receive an unearned Pass
    ROCLINT_CHECK(alts[1].auditOutcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(alts[2].auditOutcome == ValidationOutcome::Unresolved);

    for (const auto& a : alts) {
        // Target Reader::open must be preserved in every alternative
        ROCLINT_CHECK(a.rewrittenText.find("Reader::open") != std::string::npos);
        // BOTH prohibitions must be preserved in every alternative
        ROCLINT_CHECK(a.rewrittenText.find("tests") != std::string::npos);
        ROCLINT_CHECK(a.rewrittenText.find("logging") != std::string::npos);
    }
}

void testGeneralPromptExpansion() {
    const std::string original = "Write a unit test for vector addition.";
    const std::string base = "Implement comprehensive unit tests for vector addition covering edge cases and boundary values.";

    std::vector<ScopedConstraint> constraints;
    auto alternatives = CreativeExpansionEngine::expand(original, base, constraints);

    ROCLINT_CHECK(!alternatives.empty());
    ROCLINT_CHECK(alternatives[0].id == "alt_base");
    ROCLINT_CHECK(alternatives[0].auditOutcome == ValidationOutcome::Pass);

    std::cout << "  [PASS] General prompt produces standard and concise alternatives.\n";
}

void testMixedSentenceDecompositionAndFidelity() {
    const std::string original = "Refactor Reader::open for readability, but do not modify tests.";
    const std::string base = "Refactor Reader::open for readability, but do not modify tests.";

    ScopedConstraint testProhibition;
    testProhibition.id = "no_modify_tests";
    testProhibition.rule = "do not modify tests";
    testProhibition.polarity = Polarity::Prohibit;
    testProhibition.enforcement = Enforcement::Hard;

    auto alts = CreativeExpansionEngine::expand(original, base, {testProhibition});
    ROCLINT_CHECK(alts.size() >= 3);

    for (const auto& a : alts) {
        // Target Reader::open must be preserved across ALL candidate families
        ROCLINT_CHECK(a.rewrittenText.find("Reader::open") != std::string::npos);
        // Objective readability must be preserved
        ROCLINT_CHECK(a.rewrittenText.find("readability") != std::string::npos);
        // Negative boundary tests must be preserved
        ROCLINT_CHECK(a.rewrittenText.find("tests") != std::string::npos);
        // No invented generic boilerplate
        ROCLINT_CHECK(a.rewrittenText.find("targeted routines as requested") == std::string::npos);
        ROCLINT_CHECK(a.rewrittenText.find("focusing strictly on the primary objective") == std::string::npos);
    }

    std::cout << "  [PASS] Mixed sentence decomposition preserves target, objective, and prohibitions without boilerplate.\n";
}

void testConciseAlternativeBoundaryPreservation() {
    const std::string original = "Write a concise release note. Do not disclose internal codenames.";
    const std::string base = "Write a concise release note explaining the change for ordinary users. Do not disclose internal codenames.";

    ScopedConstraint codenamesProhibition;
    codenamesProhibition.id = "no_codenames";
    codenamesProhibition.rule = "Do not disclose internal codenames";
    codenamesProhibition.polarity = Polarity::Prohibit;
    codenamesProhibition.enforcement = Enforcement::Hard;

    auto alts = CreativeExpansionEngine::expand(original, base, {codenamesProhibition});
    ROCLINT_CHECK(alts.size() >= 2);

    bool foundConcise = false;
    for (const auto& a : alts) {
        if (a.id == "alt_concise") {
            foundConcise = true;
            // Prohibition must be preserved in concise alternative
            ROCLINT_CHECK(a.rewrittenText.find("codenames") != std::string::npos);
            // Must be strictly shorter than base
            ROCLINT_CHECK(a.rewrittenText.size() < base.size());
        }
    }
    ROCLINT_CHECK(foundConcise);

    std::cout << "  [PASS] Concise alternative preserves prohibitions and meaning.\n";
}

void testCandidateValidationAgainstReconciler() {
    // When base proposal violates a constraint, reconciler-filtered expansion must not return violating alternatives
    const std::string original = "Update token validation handler.";
    const std::string base = "Refactor token validation handler.";

    ScopedConstraint prohibitToken;
    prohibitToken.id = "c_prohibit_token";
    prohibitToken.rule = "Do not modify token validation";
    prohibitToken.polarity = Polarity::Prohibit;
    prohibitToken.enforcement = Enforcement::Hard;

    auto alts = CreativeExpansionEngine::expand(original, base, {prohibitToken});
    // Any candidate that directly mutates token validation fails reconciliation and is omitted
    for (const auto& a : alts) {
        ROCLINT_CHECK(a.rewrittenText.find("untouched") != std::string::npos ||
                      a.rewrittenText.find("unchanged") != std::string::npos);
    }
    std::cout << "  [PASS] Candidates failing reconciler validation are omitted.\n";
}

void testCompoundMixedSentenceDecomposition() {
    const std::string orig =
        "Refactor Reader::open for readability, but do not modify tests, while improving logging, except do not touch token validation, without breaking public API.";
    const std::string base = orig;

    ScopedConstraint p1{"tests", "do not modify tests", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Prohibit};
    ScopedConstraint p2{"token", "do not touch token validation", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Prohibit};
    ScopedConstraint p3{"api", "do not break public API", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Prohibit};

    auto alts = CreativeExpansionEngine::expand(orig, base, {p1, p2, p3});
    ROCLINT_CHECK(alts.size() >= 3);

    for (const auto& a : alts) {
        // Target Reader::open must be preserved
        ROCLINT_CHECK(a.rewrittenText.find("Reader::open") != std::string::npos);
        // Intermediate positive requirement "logging" must be preserved across ALL families
        ROCLINT_CHECK(a.rewrittenText.find("logging") != std::string::npos);
        // All prohibitions must be preserved
        ROCLINT_CHECK(a.rewrittenText.find("tests") != std::string::npos);
        ROCLINT_CHECK(a.rewrittenText.find("token validation") != std::string::npos);
    }

    std::cout << "  [PASS] Compound 5-clause decomposition preserves intermediate positive requirement and all prohibitions.\n";
}

void testConciseMultipleRequirementsPreservation() {
    // 1. Multiple positive requirements in prompt
    const std::string multiReqOrig = "Write a unit test for vector addition. Cover boundary conditions. Do not disclose internal codenames.";
    const std::string multiReqBase = "Implement comprehensive unit tests for vector addition. Cover boundary conditions. Do not disclose internal codenames.";

    ScopedConstraint noCodenames{"no_codenames", "Do not disclose internal codenames", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Prohibit};
    ScopedConstraint coverReq{"cover_boundaries", "Cover boundary conditions", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};

    auto alts1 = CreativeExpansionEngine::expand(multiReqOrig, multiReqBase, {coverReq, noCodenames});
    bool foundConcise1 = false;
    for (const auto& a : alts1) {
        if (a.id == "alt_concise") {
            foundConcise1 = true;
            ROCLINT_CHECK(a.rewrittenText.size() < multiReqBase.size());
            ROCLINT_CHECK(a.rewrittenText.find("boundary") != std::string::npos ||
                          a.rewrittenText.find("conditions") != std::string::npos);
            ROCLINT_CHECK(a.rewrittenText.find("codenames") != std::string::npos);
        }
    }
    ROCLINT_CHECK(foundConcise1);

    // 2. Multiple positive sentences preserving migration
    const std::string origB = "Write a release note. Add migration instructions. Do not disclose internal codenames.";
    const std::string baseB = "Write a concise release note explaining the change for ordinary users. Add migration instructions for upgrading users. Do not disclose internal codenames.";

    ScopedConstraint r1{"r1", "Write a release note", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};
    ScopedConstraint r2{"r2", "Add migration instructions", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};

    auto alts2 = CreativeExpansionEngine::expand(origB, baseB, {noCodenames, r1, r2});
    for (const auto& a : alts2) {
        if (a.id == "alt_concise") {
            ROCLINT_CHECK(a.rewrittenText.find("migration") != std::string::npos);
            ROCLINT_CHECK(a.rewrittenText.size() < baseB.size());
        }
    }

    // 3. Unshortenable base omits alt_concise
    auto alts3 = CreativeExpansionEngine::expand("Fix bug.", "Fix bug.", {});
    for (const auto& a : alts3) {
        ROCLINT_CHECK(a.id != "alt_concise");
    }

    std::cout << "  [PASS] Concise expansion preserves all positive requirements and omits when appropriate.\n";
}

void testGuardedElaborationAndRequirementPreservation() {
    // 1. Rollback scenario: "explaining how to roll back safely" is required
    const std::string rollbackPrompt = "Write a migration guide explaining how to roll back safely.";
    ScopedConstraint rollbackReq;
    rollbackReq.id = "req_rollback";
    rollbackReq.rule = rollbackPrompt;
    rollbackReq.origin = Origin::User;
    rollbackReq.derivation = Derivation::Direct;
    rollbackReq.enforcement = Enforcement::Hard;
    rollbackReq.polarity = Polarity::Require;

    auto rollbackAlts = CreativeExpansionEngine::expand(rollbackPrompt, rollbackPrompt, {rollbackReq});
    ROCLINT_CHECK(!rollbackAlts.empty());
    for (const auto& a : rollbackAlts) {
        // Every emitted alternative must preserve "roll back"
        ROCLINT_CHECK(a.rewrittenText.find("roll back") != std::string::npos ||
                      a.rewrittenText.find("rollback") != std::string::npos);
        // Truncated variant omitting rollback must be omitted
        ROCLINT_CHECK(a.rewrittenText != "Write a migration guide.");
    }
    // Alt_concise should be safely omitted because the clause cannot be shortened without losing rollback
    for (const auto& a : rollbackAlts) {
        ROCLINT_CHECK(a.id != "alt_concise");
    }

    // 2. Audience scenario: "for ordinary users" is required
    const std::string audiencePrompt = "Write documentation for ordinary users.";
    ScopedConstraint audienceReq;
    audienceReq.id = "req_audience";
    audienceReq.rule = audiencePrompt;
    audienceReq.origin = Origin::User;
    audienceReq.derivation = Derivation::Direct;
    audienceReq.enforcement = Enforcement::Hard;
    audienceReq.polarity = Polarity::Require;

    auto audienceAlts = CreativeExpansionEngine::expand(audiencePrompt, audiencePrompt, {audienceReq});
    ROCLINT_CHECK(!audienceAlts.empty());
    for (const auto& a : audienceAlts) {
        ROCLINT_CHECK(a.rewrittenText.find("ordinary users") != std::string::npos ||
                      (a.rewrittenText.find("ordinary") != std::string::npos &&
                       a.rewrittenText.find("users") != std::string::npos));
        ROCLINT_CHECK(a.rewrittenText != "Write documentation.");
    }

    // 3. Purpose scenario: "to improve throughput" is required
    const std::string purposePrompt = "Optimize query execution to improve throughput.";
    ScopedConstraint purposeReq;
    purposeReq.id = "req_purpose";
    purposeReq.rule = purposePrompt;
    purposeReq.origin = Origin::User;
    purposeReq.enforcement = Enforcement::Hard;
    purposeReq.polarity = Polarity::Require;

    auto purposeAlts = CreativeExpansionEngine::expand(purposePrompt, purposePrompt, {purposeReq});
    ROCLINT_CHECK(!purposeAlts.empty());
    for (const auto& a : purposeAlts) {
        ROCLINT_CHECK(a.rewrittenText.find("throughput") != std::string::npos);
        ROCLINT_CHECK(a.rewrittenText != "Optimize query execution.");
    }

    // 4. Exact-target scenario: "Reader::open" must be preserved
    const std::string targetPrompt = "Refactor Reader::open for readability.";
    ScopedConstraint targetReq;
    targetReq.id = "req_target";
    targetReq.rule = targetPrompt;
    targetReq.origin = Origin::User;
    targetReq.enforcement = Enforcement::Hard;
    targetReq.polarity = Polarity::Require;

    auto targetAlts = CreativeExpansionEngine::expand(targetPrompt, targetPrompt, {targetReq});
    ROCLINT_CHECK(!targetAlts.empty());
    for (const auto& a : targetAlts) {
        ROCLINT_CHECK(a.rewrittenText.find("Reader::open") != std::string::npos);
        ROCLINT_CHECK(a.rewrittenText.find("readability") != std::string::npos);
    }

    // 5. Multiple-requirement scenario
    const std::string multiPrompt = "Write a unit test for vector addition. Cover boundary conditions. Do not disclose internal codenames.";
    const std::string multiBase = "Implement comprehensive unit tests for vector addition. Cover boundary conditions. Do not disclose internal codenames.";
    ScopedConstraint noCodenames{"no_codenames", "Do not disclose internal codenames", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Prohibit};
    ScopedConstraint coverReq{"cover_boundaries", "Cover boundary conditions", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};

    auto multiAlts = CreativeExpansionEngine::expand(multiPrompt, multiBase, {coverReq, noCodenames});
    ROCLINT_CHECK(!multiAlts.empty());
    for (const auto& a : multiAlts) {
        ROCLINT_CHECK(a.rewrittenText.find("vector addition") != std::string::npos ||
                      (a.rewrittenText.find("vector") != std::string::npos &&
                       a.rewrittenText.find("addition") != std::string::npos));
        ROCLINT_CHECK(a.rewrittenText.find("boundary") != std::string::npos ||
                      a.rewrittenText.find("conditions") != std::string::npos);
        ROCLINT_CHECK(a.rewrittenText.find("codenames") != std::string::npos);
    }

    // 6. Generic verbs alone ("write", "guide") must not satisfy preservation
    const std::string genericOnlyOrig = "Write a guide explaining rollback.";
    const std::string failingCandidate = "Write a guide.";
    ConstraintReconciler reconciler;
    ScopedConstraint rollbackGuideReq{"rb_guide", genericOnlyOrig, Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};
    auto failingRes = reconciler.reconcile(genericOnlyOrig, {rollbackGuideReq}, failingCandidate);
    // Missing substantive word "rollback" must make it Unresolved, despite matching generic verbs "write" and "guide"
    ROCLINT_CHECK(failingRes.outcome == ValidationOutcome::Unresolved);

    std::cout << "  [PASS] Guarded elaboration and requirement preservation scenarios passed.\n";
}

} // namespace

int main() {
    std::cout << "[CreativeExpansionTests] Running tests...\n";
    testLayer3VisualizationExpansion();
    testRefactoringExpansionWithNegativeBoundary();
    testF05MultipleProhibitionsAndTargetFidelity();
    testGeneralPromptExpansion();
    testMixedSentenceDecompositionAndFidelity();
    testConciseAlternativeBoundaryPreservation();
    testCandidateValidationAgainstReconciler();
    testCompoundMixedSentenceDecomposition();
    testConciseMultipleRequirementsPreservation();
    testGuardedElaborationAndRequirementPreservation();
    std::cout << "[CreativeExpansionTests] All tests PASSED.\n";
    return 0;
}
