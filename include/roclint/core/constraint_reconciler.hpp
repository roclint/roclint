#pragma once

#include "roclint/core/scoped_constraint.hpp"
#include "roclint/core/validation_result.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace roclint::core {

// Evaluator for registered applicability predicates (e.g. "depicts_ipv4_forwarding").
using PredicateFn = std::function<ApplicabilityStatus(std::string_view contextText)>;

class ConstraintReconciler {
public:
    ConstraintReconciler();

    // Register a known applicability predicate identifier.
    void registerPredicate(std::string name, PredicateFn predicate);

    // Determines the applicability status of a constraint against candidate text.
    [[nodiscard]] ApplicabilityStatus evaluateApplicability(
        const ScopedConstraint& constraint,
        std::string_view contextText) const;

    // Checks whether two constraints govern orthogonal aspect coordinate spaces.
    [[nodiscard]] static bool areOrthogonal(
        const ScopedConstraint& a,
        const ScopedConstraint& b) noexcept;

    // Reconciles and validates a collection of constraints against a candidate rewrite.
    [[nodiscard]] ValidationResult reconcile(
        std::string_view originalPrompt,
        const std::vector<ScopedConstraint>& constraints,
        std::string_view candidateRewrite) const;

private:
    std::unordered_map<std::string, PredicateFn> predicates_;
};

} // namespace roclint::core
