#include "roclint/core/constraint_reconciler.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace roclint::core;

namespace {

void testOrthogonality() {
    ScopedConstraint style;
    style.aspect = "presentation";

    ScopedConstraint data;
    data.aspect = "data_integrity";

    ScopedConstraint duplicateStyle;
    duplicateStyle.aspect = "presentation";

    ScopedConstraint emptyAspect;

    ROCLINT_CHECK(ConstraintReconciler::areOrthogonal(style, data));
    ROCLINT_CHECK(!ConstraintReconciler::areOrthogonal(style, duplicateStyle));
    ROCLINT_CHECK(!ConstraintReconciler::areOrthogonal(style, emptyAspect));
}

void testApplicabilityGating() {
    ConstraintReconciler reconciler;

    ScopedConstraint ipv4Rule;
    ipv4Rule.id = "c_ipv4";
    ipv4Rule.rule = "Destination forwarding follows longest prefix match";
    ipv4Rule.appliesWhen = "depicts_ipv4_forwarding";

    // 1. Context without packet forwarding details -> Inactive
    const std::string highLevelBgp = "Explain the BGP peering architecture between autonomous systems.";
    ROCLINT_CHECK(reconciler.evaluateApplicability(ipv4Rule, highLevelBgp) == ApplicabilityStatus::Inactive);

    // 2. Context with IP next-hop forwarding -> Active
    const std::string forwardingVis = "Render the IP routing next hop forwarding decisions across the subnet.";
    ROCLINT_CHECK(reconciler.evaluateApplicability(ipv4Rule, forwardingVis) == ApplicabilityStatus::Active);

    // 3. Unknown predicate -> Unknown
    ScopedConstraint unknownPredicateRule;
    unknownPredicateRule.appliesWhen = "non_existent_predicate_trigger";
    ROCLINT_CHECK(reconciler.evaluateApplicability(unknownPredicateRule, forwardingVis) == ApplicabilityStatus::Unknown);

    // 4. Unconditional constraint (no appliesWhen) -> Active
    ScopedConstraint unconditional;
    ROCLINT_CHECK(reconciler.evaluateApplicability(unconditional, highLevelBgp) == ApplicabilityStatus::Active);
}

void testAdvisoryZeroVetoPower() {
    ConstraintReconciler reconciler;

    // User hard requirement
    ScopedConstraint userReq;
    userReq.id = "c_user";
    userReq.rule = "Accurate Layer 3 networking visualization";
    userReq.origin = Origin::User;
    userReq.enforcement = Enforcement::Hard;

    // Model advisory requirement (laundering attempt or assumption)
    ScopedConstraint modelAdvisory;
    modelAdvisory.id = "c_model_advisory";
    modelAdvisory.rule = "Display real CIDR routing tables on screen";
    modelAdvisory.origin = Origin::Model;
    modelAdvisory.derivation = Derivation::Assumed;
    modelAdvisory.enforcement = Enforcement::Advisory;

    const std::vector<ScopedConstraint> constraints = {userReq, modelAdvisory};

    // Candidate rewrite provides the visualization, but OMITS the model-suggested routing tables!
    const std::string candidateRewrite = "Create an accurate Layer 3 networking visualization illustrating packet hops.";

    const auto res = reconciler.reconcile("original", constraints, candidateRewrite);

    // Advisory omission must NEVER fail validation!
    ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(res.violations.empty());
}

void testNegativeBoundaryPreservation() {
    ConstraintReconciler reconciler;

    ScopedConstraint prohibitRule;
    prohibitRule.id = "c_prohibit_token";
    prohibitRule.rule = "Do not modify token validation implementation";
    prohibitRule.origin = Origin::User;
    prohibitRule.derivation = Derivation::Direct;
    prohibitRule.enforcement = Enforcement::Hard;
    prohibitRule.polarity = Polarity::Prohibit;

    const std::vector<ScopedConstraint> constraints = {prohibitRule};

    // Scenario A: Candidate rewrite violates prohibition by rewriting token validation
    const std::string violatingRewrite = "Refactor authentication and update the token validation handler.";
    const auto failRes = reconciler.reconcile("original", constraints, violatingRewrite);

    ROCLINT_CHECK(failRes.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!failRes.violations.empty());
    ROCLINT_CHECK(failRes.violations[0].constraintId == "c_prohibit_token");

    // Scenario B: Candidate rewrite respects the negative boundary
    const std::string respectingRewrite = "Refactor session storage while leaving token validation untouched.";
    const auto passRes = reconciler.reconcile("original", constraints, respectingRewrite);

    ROCLINT_CHECK(passRes.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(passRes.violations.empty());

    // Scenario C (F03): Target-scoped negative boundary must not be suppressed by an unrelated preservation phrase
    const std::string unrelatedPreservation = "Keep tests unchanged. Modify token validation.";
    const auto scopedFailRes = reconciler.reconcile("original", constraints, unrelatedPreservation);

    ROCLINT_CHECK(scopedFailRes.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(!scopedFailRes.violations.empty());
    ROCLINT_CHECK(scopedFailRes.violations[0].constraintId == "c_prohibit_token");
}

void testHonestUnknownsOutcome() {
    ConstraintReconciler reconciler;

    ScopedConstraint unresolvableRule;
    unresolvableRule.id = "c_unknown_gate";
    unresolvableRule.rule = "Must verify distributed consensus correctness";
    unresolvableRule.origin = Origin::Domain;
    unresolvableRule.enforcement = Enforcement::Hard;
    unresolvableRule.appliesWhen = "unsupported_formal_proof_predicate";

    const std::vector<ScopedConstraint> constraints = {unresolvableRule};

    const std::string candidateRewrite = "Build a consensus engine with Raft.";
    const auto res = reconciler.reconcile("original", constraints, candidateRewrite);

    // Unknown applicability on Hard constraint yields Unresolved, NOT Fail!
    ROCLINT_CHECK(res.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(res.violations.empty());
    ROCLINT_CHECK(!res.unresolved.empty());
    ROCLINT_CHECK(res.unresolved[0].constraintId == "c_unknown_gate");
}

void testTargetBoundNegativeBoundaryScoping() {
    ConstraintReconciler reconciler;

    ScopedConstraint prohibitToken;
    prohibitToken.id = "c_prohibit_token";
    prohibitToken.rule = "Do not modify token validation";
    prohibitToken.origin = Origin::User;
    prohibitToken.derivation = Derivation::Direct;
    prohibitToken.enforcement = Enforcement::Hard;
    prohibitToken.polarity = Polarity::Prohibit;

    const std::vector<ScopedConstraint> constraints = {prohibitToken};

    // 1. Counterexamples: Unrelated preservation must NOT suppress prohibited mutations on protected target
    const std::string counterexample1 = "Keep tests unchanged then modify token validation.";
    const auto res1 = reconciler.reconcile("original", constraints, counterexample1);
    ROCLINT_CHECK(res1.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(res1.violations.size() == 1);
    ROCLINT_CHECK(res1.violations[0].constraintId == "c_prohibit_token");

    const std::string counterexample2 = "Modify token validation without changing tests.";
    const auto res2 = reconciler.reconcile("original", constraints, counterexample2);
    ROCLINT_CHECK(res2.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(res2.violations.size() == 1);
    ROCLINT_CHECK(res2.violations[0].constraintId == "c_prohibit_token");

    const std::string counterexample3 = "Keep tests unchanged and modify token validation.";
    const auto res3 = reconciler.reconcile("original", constraints, counterexample3);
    ROCLINT_CHECK(res3.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(res3.violations.size() == 1);
    ROCLINT_CHECK(res3.violations[0].constraintId == "c_prohibit_token");

    // 2. Positive controls: Genuine preservation of the protected target must cleanly Pass
    const std::string posControl1 = "Refactor session storage while leaving token validation untouched.";
    const auto posRes1 = reconciler.reconcile("original", constraints, posControl1);
    ROCLINT_CHECK(posRes1.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(posRes1.violations.empty());
    ROCLINT_CHECK(posRes1.unresolved.empty());

    const std::string posControl2 = "Keep token validation unchanged.";
    const auto posRes2 = reconciler.reconcile("original", constraints, posControl2);
    ROCLINT_CHECK(posRes2.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(posRes2.violations.empty());
    ROCLINT_CHECK(posRes2.unresolved.empty());

    const std::string posControl3 = "Modify tests without changing token validation.";
    const auto posRes3 = reconciler.reconcile("original", constraints, posControl3);
    ROCLINT_CHECK(posRes3.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(posRes3.violations.empty());
    ROCLINT_CHECK(posRes3.unresolved.empty());

    const std::string posControl4 = "Do not modify token validation, but update tests.";
    const auto posRes4 = reconciler.reconcile("original", constraints, posControl4);
    ROCLINT_CHECK(posRes4.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(posRes4.violations.empty());
    ROCLINT_CHECK(posRes4.unresolved.empty());

    // 3. Ambiguous scopes: Target is mentioned without clear mutation or preservation -> Unresolved
    const std::string ambiguous1 = "Review token validation architecture.";
    const auto ambRes1 = reconciler.reconcile("original", constraints, ambiguous1);
    ROCLINT_CHECK(ambRes1.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(ambRes1.violations.empty());
    ROCLINT_CHECK(ambRes1.unresolved.size() == 1);
    ROCLINT_CHECK(ambRes1.unresolved[0].constraintId == "c_prohibit_token");

    const std::string ambiguous2 = "Inspect token validation logic for details.";
    const auto ambRes2 = reconciler.reconcile("original", constraints, ambiguous2);
    ROCLINT_CHECK(ambRes2.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(ambRes2.violations.empty());
    ROCLINT_CHECK(ambRes2.unresolved.size() == 1);
    ROCLINT_CHECK(ambRes2.unresolved[0].constraintId == "c_prohibit_token");

    // 4. Contradictory scopes: Both mutation and preservation directives -> Unresolved
    const std::string contradictory1 = "Modify token validation while leaving token validation untouched.";
    const auto contraRes1 = reconciler.reconcile("original", constraints, contradictory1);
    ROCLINT_CHECK(contraRes1.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(contraRes1.violations.empty());
    ROCLINT_CHECK(contraRes1.unresolved.size() == 1);
    ROCLINT_CHECK(contraRes1.unresolved[0].constraintId == "c_prohibit_token");

    const std::string contradictory2 = "Keep token validation unchanged then rewrite token validation.";
    const auto contraRes2 = reconciler.reconcile("original", constraints, contradictory2);
    ROCLINT_CHECK(contraRes2.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(contraRes2.violations.empty());
    ROCLINT_CHECK(contraRes2.unresolved.size() == 1);
    ROCLINT_CHECK(contraRes2.unresolved[0].constraintId == "c_prohibit_token");

    // 5. Unrelated text without target mention -> Pass
    const std::string unrelated = "Refactor session storage for lower latency.";
    const auto unrelRes = reconciler.reconcile("original", constraints, unrelated);
    ROCLINT_CHECK(unrelRes.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(unrelRes.violations.empty());
    ROCLINT_CHECK(unrelRes.unresolved.empty());
}

void testAdversarialBoundaryRemediations() {
    ConstraintReconciler reconciler;

    ScopedConstraint prohibitToken;
    prohibitToken.id = "c_prohibit_token";
    prohibitToken.rule = "Do not modify token validation";
    prohibitToken.origin = Origin::User;
    prohibitToken.derivation = Derivation::Direct;
    prohibitToken.enforcement = Enforcement::Hard;
    prohibitToken.polarity = Polarity::Prohibit;

    ScopedConstraint prohibitImpl;
    prohibitImpl.id = "c_prohibit_impl";
    prohibitImpl.rule = "Do not modify token validation implementation";
    prohibitImpl.origin = Origin::User;
    prohibitImpl.enforcement = Enforcement::Hard;
    prohibitImpl.polarity = Polarity::Prohibit;

    const std::vector<ScopedConstraint> tokenConstraints = {prohibitToken};
    const std::vector<ScopedConstraint> implConstraints = {prohibitImpl};

    // 1. Extended syntactic clause delimiters preventing false preservation
    std::vector<std::string> markerFailCases = {
        "Modify token validation with tests untouched.",
        "Modify token validation leaving tests untouched.",
        "Modify token validation keeping tests untouched.",
        "Modify token validation yet keep tests unchanged.",
        "Modify token validation so that tests remain untouched.",
        "Keep tests untouched - modify token validation.",
        "Ensuring tests remain untouched after token validation is changed.",
        "Tests are preserved before token validation is updated."
    };
    for (const auto& text : markerFailCases) {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.violations.size() == 1);
        ROCLINT_CHECK(res.violations[0].constraintId == "c_prohibit_token");
    }

    // 2. Passive voice preservation with preservation verbs
    std::vector<std::string> passivePassCases = {
        "Tests were modified while token validation was preserved.",
        "Refactor tests while token validation was maintained.",
        "Token validation was not modified while tests were updated."
    };
    for (const auto& text : passivePassCases) {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.empty());
    }

    // 3. Passive voice mutation
    auto passiveMutRes = reconciler.reconcile("original", tokenConstraints, "Token validation has been changed for performance.");
    ROCLINT_CHECK(passiveMutRes.outcome == ValidationOutcome::Fail);
    ROCLINT_CHECK(passiveMutRes.violations.size() == 1);

    // 4. Intervening noun preventing leakage
    auto leakRes = reconciler.reconcile("original", tokenConstraints, "Check token validation with tests untouched.");
    ROCLINT_CHECK(leakRes.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(leakRes.violations.empty());
    ROCLINT_CHECK(leakRes.unresolved.size() == 1);

    // 5. Dual occurrences of different phrase lengths
    auto dualRes1 = reconciler.reconcile("original", implConstraints,
        "Keep token validation implementation unchanged then modify token validation.");
    ROCLINT_CHECK(dualRes1.outcome != ValidationOutcome::Pass);

    auto dualRes2 = reconciler.reconcile("original", implConstraints,
        "Modify token validation then keep token validation implementation unchanged.");
    ROCLINT_CHECK(dualRes2.outcome != ValidationOutcome::Pass);
}

void testContractionTokenizationAndBoundaryEnforcement() {
    ConstraintReconciler reconciler;

    ScopedConstraint prohibitToken;
    prohibitToken.id = "c_prohibit_token";
    prohibitToken.rule = "Do not modify token validation";
    prohibitToken.origin = Origin::User;
    prohibitToken.derivation = Derivation::Direct;
    prohibitToken.enforcement = Enforcement::Hard;
    prohibitToken.polarity = Polarity::Prohibit;

    const std::vector<ScopedConstraint> constraints = {prohibitToken};

    // 1. Straight and curly apostrophe contractions in preservation rules (should cleanly Pass)
    const std::vector<std::string> complyingContractions = {
        "Don't modify token validation.",
        "Don\xE2\x80\x99t modify token validation.", // curly quote ’
        "Dont modify token validation.",
        "Can't change token validation.",
        "Can\xE2\x80\x99t change token validation.",
        "Won't touch token validation.",
        "Won\xE2\x80\x99t touch token validation.",
        "Shouldn't modify token validation.",
        "Shouldn\xE2\x80\x99t modify token validation.",
        "Doesn't alter token validation.",
        "Didn't mutate token validation."
    };
    for (const auto& text : complyingContractions) {
        auto res = reconciler.reconcile("original", constraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.empty());
    }

    // 2. Prohibited mutations with non-negative contractions (must trigger hard Fail)
    const std::vector<std::string> prohibitedMutations = {
        "We'll modify token validation.",
        "We\xE2\x80\x99ll modify token validation.", // curly quote ’
        "They'll touch token validation.",
        "Let's modify token validation.",
        "Let\xE2\x80\x99s change token validation.",
        "I'll update token validation."
    };
    for (const auto& text : prohibitedMutations) {
        auto res = reconciler.reconcile("original", constraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.violations.size() == 1);
        ROCLINT_CHECK(res.violations[0].constraintId == "c_prohibit_token");
    }

    // 3. Post-target negation contractions (should cleanly Pass without false intervening noun)
    const std::vector<std::string> postTargetContractions = {
        "Token validation isn't modified.",
        "Token validation isn\xE2\x80\x99t modified.", // curly quote ’
        "Token validation wasn't changed.",
        "Token validation won't be updated.",
        "Token validation shouldn't be touched.",
        "Token validation hasn't been altered.",
        "Token validation doesn't get modified."
    };
    for (const auto& text : postTargetContractions) {
        auto res = reconciler.reconcile("original", constraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.empty());
    }

    // 4. Negative preservation predicates (negation + preservation predicate -> hard Fail)
    const std::vector<std::string> negativePreservations = {
        "Token validation isn't untouched.",
        "Token validation isn\xE2\x80\x99t untouched.", // curly quote ’
        "Token validation wasn't left intact.",
        "Token validation won't remain unchanged.",
        "Token validation isn't preserved."
    };
    for (const auto& text : negativePreservations) {
        auto res = reconciler.reconcile("original", constraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.violations.size() == 1);
        ROCLINT_CHECK(res.violations[0].constraintId == "c_prohibit_token");
    }

    std::cout << "  [PASS] Contraction tokenization and boundary enforcement tests passed.\n";
}

void testPositiveRequirementReconcilerVerification() {
    ConstraintReconciler reconciler;

    // 1. Rollback scenario with substantive words
    ScopedConstraint rollbackReq;
    rollbackReq.id = "c_rollback";
    rollbackReq.rule = "Write a migration guide explaining how to roll back safely.";
    rollbackReq.origin = Origin::User;
    rollbackReq.enforcement = Enforcement::Hard;
    rollbackReq.polarity = Polarity::Require;

    const std::vector<ScopedConstraint> rollbackConstraints = {rollbackReq};

    // Partial candidate missing substantive terms ("roll", "back", "safely") -> Unresolved
    auto unres1 = reconciler.reconcile("original", rollbackConstraints, "Write a migration guide.");
    ROCLINT_CHECK(unres1.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(unres1.violations.empty());
    ROCLINT_CHECK(!unres1.unresolved.empty());

    // Complete candidate preserving substantive terms -> Pass
    auto pass1 = reconciler.reconcile("original", rollbackConstraints, "Write a migration guide explaining how to roll back safely.");
    ROCLINT_CHECK(pass1.outcome == ValidationOutcome::Pass);
    ROCLINT_CHECK(pass1.violations.empty());
    ROCLINT_CHECK(pass1.unresolved.empty());

    // 2. Audience scenario
    ScopedConstraint audienceReq;
    audienceReq.id = "c_audience";
    audienceReq.rule = "Write documentation for ordinary users.";
    audienceReq.origin = Origin::User;
    audienceReq.enforcement = Enforcement::Hard;
    audienceReq.polarity = Polarity::Require;

    const std::vector<ScopedConstraint> audienceConstraints = {audienceReq};

    auto unres2 = reconciler.reconcile("original", audienceConstraints, "Write documentation.");
    ROCLINT_CHECK(unres2.outcome == ValidationOutcome::Unresolved);

    auto pass2 = reconciler.reconcile("original", audienceConstraints, "Write documentation for ordinary users.");
    ROCLINT_CHECK(pass2.outcome == ValidationOutcome::Pass);

    // 3. Purpose scenario
    ScopedConstraint purposeReq;
    purposeReq.id = "c_purpose";
    purposeReq.rule = "Optimize query execution to improve throughput.";
    purposeReq.origin = Origin::User;
    purposeReq.enforcement = Enforcement::Hard;
    purposeReq.polarity = Polarity::Require;

    const std::vector<ScopedConstraint> purposeConstraints = {purposeReq};

    auto unres3 = reconciler.reconcile("original", purposeConstraints, "Optimize query execution.");
    ROCLINT_CHECK(unres3.outcome == ValidationOutcome::Unresolved);

    auto pass3 = reconciler.reconcile("original", purposeConstraints, "Optimize query execution to improve throughput.");
    ROCLINT_CHECK(pass3.outcome == ValidationOutcome::Pass);

    std::cout << "  [PASS] Positive requirement reconciler verification tests passed.\n";
}

void testDisguisedProhibitedMutationsAndNegatedPreservations() {
    ConstraintReconciler reconciler;

    ScopedConstraint prohibitToken;
    prohibitToken.id = "c_prohibit_token";
    prohibitToken.rule = "Do not modify token validation";
    prohibitToken.origin = Origin::User;
    prohibitToken.derivation = Derivation::Direct;
    prohibitToken.enforcement = Enforcement::Hard;
    prohibitToken.polarity = Polarity::Prohibit;

    const std::vector<ScopedConstraint> tokenConstraints = {prohibitToken};

    // 1. Double negation lookback mutations (must trigger Fail with 1 violation)
    const std::vector<std::string> doubleNegationMutations = {
        "can't avoid modifying token validation.",
        "Can't avoid modifying token validation.",
        "won't avoid modifying token validation.",
        "cannot avoid modifying token validation.",
        "we can't prevent modifying token validation.",
        "we can't safely prevent modifying token validation.",
        "won't stop modifying token validation."
    };
    for (const auto& text : doubleNegationMutations) {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.violations.size() == 1);
        ROCLINT_CHECK(res.violations[0].constraintId == "c_prohibit_token");
    }

    // 2. Pre-target negated preservation verbs & predicates (must trigger Fail with 1 violation)
    const std::vector<std::string> negatedPreservations = {
        "won't keep untouched token validation.",
        "Won't keep token validation untouched.",
        "can't keep token validation untouched.",
        "won't leave token validation intact.",
        "won't leave token validation untouched.",
        "don't keep token validation unchanged.",
        "won't preserve token validation.",
        "can't preserve token validation.",
        "don't leave token validation unchanged."
    };
    for (const auto& text : negatedPreservations) {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.violations.size() == 1);
        ROCLINT_CHECK(res.violations[0].constraintId == "c_prohibit_token");
    }

    // 3. Post-target negated preservation directives (must trigger Fail with 1 violation)
    const std::vector<std::string> postTargetNegatedPreservations = {
        "Token validation won't remain untouched.",
        "Token validation won't be kept intact.",
        "Token validation cannot stay unmodified.",
        "Token validation won't stop changing."
    };
    for (const auto& text : postTargetNegatedPreservations) {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(res.violations.size() == 1);
        ROCLINT_CHECK(res.violations[0].constraintId == "c_prohibit_token");
    }

    // 4. Positive preservation controls with connectors and negation parity (must cleanly Pass)
    const std::vector<std::string> positiveControls = {
        "Keep token validation untouched.",
        "Leave token validation intact.",
        "Token validation will stop changing.",
        "Token validation remains strictly untouched."
    };
    for (const auto& text : positiveControls) {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.empty());
    }

    // 5. Genuine cross-clause contradictions must continue to yield Unresolved
    const std::vector<std::string> contradictoryClauses = {
        "Modify token validation while leaving token validation untouched.",
        "Keep token validation unchanged then rewrite token validation."
    };
    for (const auto& text : contradictoryClauses) {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        ROCLINT_CHECK(res.outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(res.violations.empty());
        ROCLINT_CHECK(res.unresolved.size() == 1);
        ROCLINT_CHECK(res.unresolved[0].constraintId == "c_prohibit_token");
    }

    std::cout << "  [PASS] Disguised prohibited mutations and negated preservations tests passed.\n";
}

} // namespace

int main() {
    std::cout << "[RUNNING] test_constraint_reconciler...\n";
    testOrthogonality();
    testApplicabilityGating();
    testAdvisoryZeroVetoPower();
    testNegativeBoundaryPreservation();
    testHonestUnknownsOutcome();
    testTargetBoundNegativeBoundaryScoping();
    testAdversarialBoundaryRemediations();
    testContractionTokenizationAndBoundaryEnforcement();
    testPositiveRequirementReconcilerVerification();
    testDisguisedProhibitedMutationsAndNegatedPreservations();
    std::cout << "[PASS] All constraint reconciler tests passed successfully.\n";
    return 0;
}
