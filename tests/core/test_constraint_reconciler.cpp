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

} // namespace

int main() {
    std::cout << "[RUNNING] test_constraint_reconciler...\n";
    testOrthogonality();
    testApplicabilityGating();
    testAdvisoryZeroVetoPower();
    testNegativeBoundaryPreservation();
    testHonestUnknownsOutcome();
    std::cout << "[PASS] All constraint reconciler tests passed successfully.\n";
    return 0;
}
