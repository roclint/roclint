#include "roclint/core/constraint_reconciler.hpp"
#include "roclint/rewrite/CreativeExpansion.hpp"
#include "support/check.hpp"
#include <iostream>
#include <vector>
#include <string>

using namespace roclint::core;
using namespace roclint::core::rewrite;

int main() {
    ConstraintReconciler reconciler;

    ScopedConstraint prohibitToken;
    prohibitToken.id = "c_prohibit_token";
    prohibitToken.rule = "Do not modify token validation";
    prohibitToken.origin = Origin::User;
    prohibitToken.derivation = Derivation::Direct;
    prohibitToken.enforcement = Enforcement::Hard;
    prohibitToken.polarity = Polarity::Prohibit;

    const std::vector<ScopedConstraint> constraints = {prohibitToken};

    std::vector<std::string> testCases = {
        // Base checks from prompt item 1
        "Don't modify token validation.",
        "We'll modify token validation.",
        "Token validation isn't untouched.",
        // Curly apostrophes
        "Don\xE2\x80\x99t modify token validation.",
        "Don\xE2\x80\x98t modify token validation.",
        "We\xE2\x80\x99ll modify token validation.",
        "Token validation isn\xE2\x80\x99t untouched.",
        // Multiple contractions in single prompt
        "Don't touch token validation and won't modify tests.",
        "We'll refactor auth and won't modify token validation.",
        "We'll modify token validation and won't touch tests.",
        // Disguised prohibited mutations
        "can't avoid modifying token validation.",
        "Can't avoid modifying token validation.",
        "won't keep untouched token validation.",
        "Won't keep token validation untouched.",
        "can't keep token validation untouched.",
        "won't leave token validation intact.",
        "cannot avoid modifying token validation.",
        "we can't prevent modifying token validation."
    };

    const std::vector<bool> expectedPass = {
        true, false, false, true, true, false, false, true, true,
        false, false, false, false, false, false, false, false, false
    };
    ROCLINT_CHECK(expectedPass.size() == testCases.size());
    for (size_t i = 0; i < testCases.size(); ++i) {
        const auto& text = testCases[i];
        auto res = reconciler.reconcile("original", constraints, text);
        std::cout << "TEST|" << text << "|outcome=" << toString(res.outcome)
                  << "|violations=" << res.violations.size()
                  << "|unresolved=" << res.unresolved.size() << "\n";
        ROCLINT_CHECK(res.outcome == (expectedPass[i] ? ValidationOutcome::Pass : ValidationOutcome::Fail));
    }

    return 0;
}
