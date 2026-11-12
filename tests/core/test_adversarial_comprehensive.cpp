#include "roclint/core/constraint_reconciler.hpp"
#include "roclint/rewrite/CreativeExpansion.hpp"
#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

using namespace roclint::core;
using namespace roclint::core::rewrite;
using namespace roclint::bridge;

struct TestCaseResult {
    std::string category;
    std::string text;
    ValidationOutcome outcome;
    size_t violations;
    size_t unresolved;
    bool passExpected;
    bool actualMatchesExpected;
    std::string notes;
};

int main() {
    ConstraintReconciler reconciler;
    std::vector<TestCaseResult> results;

    ScopedConstraint prohibitToken;
    prohibitToken.id = "c_prohibit_token";
    prohibitToken.rule = "Do not modify token validation";
    prohibitToken.origin = Origin::User;
    prohibitToken.derivation = Derivation::Direct;
    prohibitToken.enforcement = Enforcement::Hard;
    prohibitToken.polarity = Polarity::Prohibit;

    const std::vector<ScopedConstraint> tokenConstraints = {prohibitToken};

    auto testReconciler = [&](const std::string& category, const std::string& text, bool expectPass, const std::string& notes = "") {
        auto res = reconciler.reconcile("original", tokenConstraints, text);
        bool match = expectPass ? (res.outcome == ValidationOutcome::Pass && res.violations.empty())
                                : (res.outcome == ValidationOutcome::Fail && res.violations.size() >= 1);
        results.push_back({
            category,
            text,
            res.outcome,
            res.violations.size(),
            res.unresolved.size(),
            expectPass,
            match,
            notes
        });
    };

    std::cout << "=================================================================\n";
    std::cout << "SECTION 1: Core Probe Invariants\n";
    std::cout << "=================================================================\n";
    testReconciler("Probe Invariant", "Don't modify token validation.", true, "Preservation contraction should Pass");
    testReconciler("Probe Invariant", "We'll modify token validation.", false, "Affirmative mutation should Fail");
    testReconciler("Probe Invariant", "Token validation isn't untouched.", false, "Negated preservation predicate should Fail");

    std::cout << "=================================================================\n";
    std::cout << "SECTION 2: Straight & Curly Apostrophes\n";
    std::cout << "=================================================================\n";
    // Straight apostrophe
    testReconciler("Apostrophe", "Don't modify token validation.", true, "Straight quote negation");
    testReconciler("Apostrophe", "We'll modify token validation.", false, "Straight quote mutation");
    testReconciler("Apostrophe", "Token validation isn't untouched.", false, "Straight quote post-target negation predicate");

    // Right curly apostrophe U+2019 (\xE2\x80\x99)
    testReconciler("Apostrophe", "Don\xE2\x80\x99t modify token validation.", true, "Curly quote U+2019 negation");
    testReconciler("Apostrophe", "We\xE2\x80\x99ll modify token validation.", false, "Curly quote U+2019 mutation");
    testReconciler("Apostrophe", "Token validation isn\xE2\x80\x99t untouched.", false, "Curly quote U+2019 post-target negation predicate");

    // Left curly apostrophe U+2018 (\xE2\x80\x98)
    testReconciler("Apostrophe", "Don\xE2\x80\x98t modify token validation.", true, "Curly quote U+2018 negation");
    testReconciler("Apostrophe", "We\xE2\x80\x98ll modify token validation.", false, "Curly quote U+2018 mutation");
    testReconciler("Apostrophe", "Token validation isn\xE2\x80\x98t untouched.", false, "Curly quote U+2018 post-target negation predicate");

    // Backtick
    testReconciler("Apostrophe", "Don`t modify token validation.", true, "Backtick negation");
    testReconciler("Apostrophe", "We`ll modify token validation.", false, "Backtick mutation");

    std::cout << "=================================================================\n";
    std::cout << "SECTION 3: Multiple Contractions in Single Prompt\n";
    std::cout << "=================================================================\n";
    testReconciler("Multi-Contraction", "Don't touch token validation and won't modify tests.", true, "Both clauses preserved");
    testReconciler("Multi-Contraction", "We'll refactor auth and won't modify token validation.", true, "Auth mutated, token validation preserved");
    testReconciler("Multi-Contraction", "We'll modify token validation and won't touch tests.", false, "Token validation mutated");
    testReconciler("Multi-Contraction", "We'll modify token validation, but won't touch tests.", false, "Token validation mutated with clause separator");
    testReconciler("Multi-Contraction", "Don't touch token validation, won't modify tests, and aren't changing config.", true, "3 negative contractions");
    testReconciler("Multi-Contraction", "They'll update token validation and we won't stop them.", false, "Mutating token validation with multiple contractions");

    std::cout << "=================================================================\n";
    std::cout << "SECTION 4: Disguised Prohibited Mutations (Adversarial)\n";
    std::cout << "=================================================================\n";
    // Double negations preceding mutation verb
    testReconciler("Disguised Mutation", "can't avoid modifying token validation.", false, "Double negation: can't avoid modifying = modifying");
    testReconciler("Disguised Mutation", "Can't avoid modifying token validation.", false, "Capitalized double negation");
    testReconciler("Disguised Mutation", "won't avoid modifying token validation.", false, "Double negation: won't avoid modifying");
    testReconciler("Disguised Mutation", "cannot avoid modifying token validation.", false, "Double negation: cannot avoid modifying");
    testReconciler("Disguised Mutation", "we can't prevent modifying token validation.", false, "Double negation: can't prevent modifying");
    testReconciler("Disguised Mutation", "won't stop modifying token validation.", false, "Double negation: won't stop modifying");

    // Negated preservation verbs / predicates
    testReconciler("Disguised Mutation", "won't keep untouched token validation.", false, "Negated preservation: won't keep untouched");
    testReconciler("Disguised Mutation", "Won't keep token validation untouched.", false, "Negated preservation: Won't keep ... untouched");
    testReconciler("Disguised Mutation", "can't keep token validation untouched.", false, "Negated preservation: can't keep ... untouched");
    testReconciler("Disguised Mutation", "won't leave token validation intact.", false, "Negated preservation: won't leave ... intact");
    testReconciler("Disguised Mutation", "won't leave token validation untouched.", false, "Negated preservation: won't leave ... untouched");
    testReconciler("Disguised Mutation", "don't keep token validation unchanged.", false, "Negated preservation: don't keep ... unchanged");
    testReconciler("Disguised Mutation", "won't preserve token validation.", false, "Negated preservation verb: won't preserve");
    testReconciler("Disguised Mutation", "can't preserve token validation.", false, "Negated preservation verb: can't preserve");
    testReconciler("Disguised Mutation", "don't leave token validation unchanged.", false, "Negated preservation: don't leave ... unchanged");

    // Post-target negated preservation
    testReconciler("Disguised Mutation", "Token validation won't remain untouched.", false, "Post-target: won't remain untouched");
    testReconciler("Disguised Mutation", "Token validation won't be kept intact.", false, "Post-target: won't be kept intact");
    testReconciler("Disguised Mutation", "Token validation cannot stay unmodified.", false, "Post-target: cannot stay unmodified");

    // Print summary table of reconcile tests
    std::cout << "\n-----------------------------------------------------------------\n";
    std::cout << "SUMMARY TABLE OF CONSTRAINT RECONCILER STRESS TESTS\n";
    std::cout << "-----------------------------------------------------------------\n";
    int passCount = 0;
    int failCount = 0;
    for (const auto& r : results) {
        bool ok = r.actualMatchesExpected;
        if (ok) passCount++; else failCount++;
        std::cout << "[" << (ok ? "PASS" : "FAIL") << "] "
                  << std::setw(20) << std::left << r.category
                  << " | Expected: " << (r.passExpected ? "Pass" : "Fail")
                  << " | Actual: " << std::setw(10) << toString(r.outcome)
                  << " | Violations: " << r.violations
                  << " | Text: \"" << r.text << "\""
                  << (ok ? "" : " <-- INVARIANT VIOLATION") << "\n";
    }
    std::cout << "\nReconciler Tests Total: " << results.size()
              << " | Passed: " << passCount << " | Failed: " << failCount << "\n\n";

    std::cout << "=================================================================\n";
    std::cout << "SECTION 5: Creative Expansion Truncation Stress Tests\n";
    std::cout << "=================================================================\n";

    int expPassCount = 0;
    int expFailCount = 0;

    auto testExpansion = [&](const std::string& testName,
                             const std::string& prompt,
                             const std::vector<ScopedConstraint>& reqConstraints,
                             const std::vector<std::string>& mandatoryTerms,
                             const std::vector<std::string>& prohibitedOmissions) {
        auto alts = CreativeExpansionEngine::expand(prompt, prompt, reqConstraints);
        bool testPassed = true;
        std::string failureReason;

        if (alts.empty()) {
            testPassed = false;
            failureReason = "No alternatives emitted!";
        } else {
            for (const auto& a : alts) {
                // Check if any mandatory substantive terms are missing
                for (const auto& term : mandatoryTerms) {
                    if (a.rewrittenText.find(term) == std::string::npos) {
                        testPassed = false;
                        failureReason = "Alternative '" + a.id + "' is missing mandatory term '" + term + "': \"" + a.rewrittenText + "\"";
                        break;
                    }
                }
                if (!testPassed) break;

                // Check if any explicitly prohibited omission text was emitted
                for (const auto& badText : prohibitedOmissions) {
                    if (a.rewrittenText == badText) {
                        testPassed = false;
                        failureReason = "Alternative '" + a.id + "' emitted prohibited truncated text: \"" + badText + "\"";
                        break;
                    }
                }
                if (!testPassed) break;
            }
        }

        if (testPassed) {
            expPassCount++;
            std::cout << "[PASS] " << testName << " (" << alts.size() << " alternatives emitted)\n";
        } else {
            expFailCount++;
            std::cout << "[FAIL] " << testName << " --> " << failureReason << "\n";
        }
    };

    // 1. Rollback clause stress tests
    {
        ScopedConstraint req;
        req.id = "req_rb1";
        req.rule = "Write a migration guide explaining how to roll back safely.";
        req.origin = Origin::User;
        req.enforcement = Enforcement::Hard;
        req.polarity = Polarity::Require;

        testExpansion(
            "Rollback 1 (Canonical)",
            req.rule,
            {req},
            {"roll back"},
            {"Write a migration guide."}
        );
    }
    {
        ScopedConstraint req;
        req.id = "req_rb2";
        req.rule = "Create a deployment playbook explaining how to rollback in emergency situations.";
        req.origin = Origin::User;
        req.enforcement = Enforcement::Hard;
        req.polarity = Polarity::Require;

        testExpansion(
            "Rollback 2 (Emergency rollback)",
            req.rule,
            {req},
            {"rollback"},
            {"Create a deployment playbook."}
        );
    }

    // 2. Audience clause stress tests
    {
        ScopedConstraint req;
        req.id = "req_aud1";
        req.rule = "Write documentation for ordinary users.";
        req.origin = Origin::User;
        req.enforcement = Enforcement::Hard;
        req.polarity = Polarity::Require;

        testExpansion(
            "Audience 1 (Ordinary users)",
            req.rule,
            {req},
            {"ordinary users"},
            {"Write documentation."}
        );
    }
    {
        ScopedConstraint req;
        req.id = "req_aud2";
        req.rule = "Provide release notes for upgrading users.";
        req.origin = Origin::User;
        req.enforcement = Enforcement::Hard;
        req.polarity = Polarity::Require;

        testExpansion(
            "Audience 2 (Upgrading users)",
            req.rule,
            {req},
            {"upgrading users"},
            {"Provide release notes."}
        );
    }

    // 3. Purpose clause stress tests
    {
        ScopedConstraint req;
        req.id = "req_purp1";
        req.rule = "Optimize query execution to improve throughput.";
        req.origin = Origin::User;
        req.enforcement = Enforcement::Hard;
        req.polarity = Polarity::Require;

        testExpansion(
            "Purpose 1 (Throughput)",
            req.rule,
            {req},
            {"throughput"},
            {"Optimize query execution."}
        );
    }
    {
        ScopedConstraint req;
        req.id = "req_purp2";
        req.rule = "Refactor connection pooling to improve latency.";
        req.origin = Origin::User;
        req.enforcement = Enforcement::Hard;
        req.polarity = Polarity::Require;

        testExpansion(
            "Purpose 2 (Latency)",
            req.rule,
            {req},
            {"latency"},
            {"Refactor connection pooling."}
        );
    }

    // 4. Exact-target clause stress tests
    {
        ScopedConstraint req;
        req.id = "req_target1";
        req.rule = "Refactor Reader::open for readability.";
        req.origin = Origin::User;
        req.enforcement = Enforcement::Hard;
        req.polarity = Polarity::Require;

        testExpansion(
            "Exact-Target 1 (Reader::open)",
            req.rule,
            {req},
            {"Reader::open", "readability"},
            {"Refactor Reader::open."}
        );
    }

    // 5. Multi-requirement clause stress tests
    {
        ScopedConstraint r1{"r1", "Write a release note", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};
        ScopedConstraint r2{"r2", "Add migration instructions", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};
        ScopedConstraint p1{"p1", "Do not disclose internal codenames", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Prohibit};

        testExpansion(
            "Multi-Requirement 1 (Release note + migration + codenames prohibition)",
            "Write a concise release note explaining the change for ordinary users. Add migration instructions for upgrading users. Do not disclose internal codenames.",
            {r1, r2, p1},
            {"migration", "codenames"},
            {"Write a release note."}
        );
    }

    // 6. Generic verb exclusion check
    {
        ConstraintReconciler r;
        ScopedConstraint genericReq{"gen_req", "Write a guide explaining rollback.", Origin::User, Derivation::Direct, Enforcement::Hard, Polarity::Require};
        auto unresRes = r.reconcile("orig", {genericReq}, "Write a guide.");
        bool genericExcluded = (unresRes.outcome == ValidationOutcome::Unresolved);
        if (genericExcluded) {
            expPassCount++;
            std::cout << "[PASS] Generic Verb Exclusion: \"Write a guide.\" cannot satisfy requirement \"rollback\"\n";
        } else {
            expFailCount++;
            std::cout << "[FAIL] Generic Verb Exclusion: \"Write a guide.\" satisfied requirement without substantive term!\n";
        }
    }

    std::cout << "\nExpansion Stress Tests Total: " << (expPassCount + expFailCount)
              << " | Passed: " << expPassCount << " | Failed: " << expFailCount << "\n";

    return (failCount > 0 || expFailCount > 0) ? 1 : 0;
}
