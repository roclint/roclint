#include "roclint/rewrite/CreativeExpansion.hpp"
#include "support/check.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::rewrite;

namespace {

void testLayer3VisualizationExpansion() {
    const std::string original = "Make a creative Layer 3 animation, but technically accurate.";
    const std::string base = "Create a Layer 3 packet animation showing IP destination forwarding using glowing particle flows.";

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
    ROCLINT_CHECK(alternatives[1].auditOutcome == ValidationOutcome::Pass);

    ROCLINT_CHECK(alternatives[2].id == "alt_stack");
    ROCLINT_CHECK(alternatives[2].title == "Stack & Topology");
    ROCLINT_CHECK(!alternatives[2].rewrittenText.empty());
    ROCLINT_CHECK(alternatives[2].auditOutcome == ValidationOutcome::Pass);

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

    for (const auto& alt : alternatives) {
        ROCLINT_CHECK(!alt.rewrittenText.empty());
        ROCLINT_CHECK(!alt.diffText.empty());
        ROCLINT_CHECK(alt.auditOutcome == ValidationOutcome::Pass);
        // Ensure negative prohibition is not violated in any alternative!
        ROCLINT_CHECK(alt.rewrittenText.find("untouched") != std::string::npos ||
                      alt.rewrittenText.find("unchanged") != std::string::npos ||
                      alt.rewrittenText.find("preserving") != std::string::npos);
    }

    std::cout << "  [PASS] Refactoring expansion preserves negative boundary across all alternatives.\n";
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

} // namespace

int main() {
    std::cout << "[CreativeExpansionTests] Running tests...\n";
    testLayer3VisualizationExpansion();
    testRefactoringExpansionWithNegativeBoundary();
    testGeneralPromptExpansion();
    std::cout << "[CreativeExpansionTests] All tests PASSED.\n";
    return 0;
}
