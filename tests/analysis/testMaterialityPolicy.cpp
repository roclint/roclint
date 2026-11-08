#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/analysis/SourceSpan.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#define ROCLINT_CHECK(cond)                                                          \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__        \
                      << " -> " << #cond << std::endl;                              \
            std::exit(1);                                                            \
        }                                                                            \
    } while (false)

using namespace roclint::core::analysis;

void testCardinalityBounds() {
    const std::string prompt = "Refactor the authentication module";

    ChoiceGroup g1;
    g1.id = "group_1";
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "Use OAuth2",
        .whyMaterial = "Industry standard token auth",
        .evidence = {SourceSpan{0, 8}}
    });

    auto res1 = MaterialityPolicyV1::evaluateGroup(g1, prompt);
    ROCLINT_CHECK(res1.verdict == PolicyVerdict::Rejected);
    ROCLINT_CHECK(res1.reasonCode == ReasonCode::InsufficientAlternatives);

    ChoiceGroup g2 = g1;
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "Use SAML",
        .whyMaterial = "Enterprise SSO",
        .evidence = {SourceSpan{0, 8}}
    });
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_3",
        .interpretation = "Use JWT",
        .whyMaterial = "Stateless token auth",
        .evidence = {SourceSpan{0, 8}}
    });
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_4",
        .interpretation = "Use mTLS",
        .whyMaterial = "Zero-trust client certs",
        .evidence = {SourceSpan{0, 8}}
    });

    auto res2 = MaterialityPolicyV1::evaluateGroup(g2, prompt);
    ROCLINT_CHECK(res2.verdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(res2.reasonCode == ReasonCode::ExcessiveAlternatives);

    ChoiceGroup g3;
    g3.id = "group_3";
    g3.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "Use OAuth2",
        .whyMaterial = "Industry standard token auth",
        .evidence = {SourceSpan{0, 8}}
    });
    g3.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "Use SAML",
        .whyMaterial = "Enterprise SSO",
        .evidence = {SourceSpan{0, 8}}
    });

    auto res3 = MaterialityPolicyV1::evaluateGroup(g3, prompt);
    ROCLINT_CHECK(res3.verdict == PolicyVerdict::EligibleForChoice);
    ROCLINT_CHECK(res3.reasonCode == ReasonCode::Ok);
    ROCLINT_CHECK(res3.isEligible());
}

void testLabelDistinctness() {
    const std::string prompt = "Optimize database queries";

    ChoiceGroup g;
    g.id = "group_distinct";
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "Add B-Tree Index",
        .whyMaterial = "Speeds up lookups",
        .evidence = {SourceSpan{0, 8}}
    });
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "  add b-tree index  ",
        .whyMaterial = "Reduces scan time",
        .evidence = {SourceSpan{0, 8}}
    });

    auto res = MaterialityPolicyV1::evaluateGroup(g, prompt);
    ROCLINT_CHECK(res.verdict == PolicyVerdict::Rejected);
    ROCLINT_CHECK(res.reasonCode == ReasonCode::DuplicateAlternativeLabels);
}

void testMaterialityGrounding() {
    const std::string prompt = "Update system logger configuration";

    ChoiceGroup g;
    g.id = "group_grounding";
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "Log to stdout",
        .whyMaterial = "",
        .evidence = {SourceSpan{0, 6}}
    });
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "Log to file",
        .whyMaterial = "Persistent disk logs",
        .evidence = {SourceSpan{0, 6}}
    });

    auto res = MaterialityPolicyV1::evaluateGroup(g, prompt);
    ROCLINT_CHECK(res.verdict == PolicyVerdict::Rejected);
    ROCLINT_CHECK(res.reasonCode == ReasonCode::UngroundedMateriality);
}

void testUtf8SpanBoundaryValidation() {
    const std::string prompt = "🚀 launch rocket now";

    ChoiceGroup g;
    g.id = "group_utf8";
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "Suborbital launch",
        .whyMaterial = "Altitude below Karman line",
        .evidence = {SourceSpan{1, 4}}
    });
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "Orbital insertion",
        .whyMaterial = "Complete orbit around Earth",
        .evidence = {SourceSpan{0, 4}}
    });

    auto res = MaterialityPolicyV1::evaluateGroup(g, prompt);
    ROCLINT_CHECK(res.verdict == PolicyVerdict::Rejected);
    ROCLINT_CHECK(res.reasonCode == ReasonCode::InvalidUtf8SpanBoundary);

    g.alternatives[0].evidence = {SourceSpan{0, 100}};
    auto resOob = MaterialityPolicyV1::evaluateGroup(g, prompt);
    ROCLINT_CHECK(resOob.verdict == PolicyVerdict::Rejected);
    ROCLINT_CHECK(resOob.reasonCode == ReasonCode::InvalidUtf8SpanBoundary);
}

void testCrossGroupSpanOverlap() {
    const std::string prompt = "Configure network socket timeout buffer";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_001";
    analysis.baseRevisionId = "rev_001";

    ChoiceGroup g1;
    g1.id = "group_network";
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1a",
        .interpretation = "TCP Socket",
        .whyMaterial = "Reliable stream",
        .evidence = {SourceSpan{10, 24}}
    });
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1b",
        .interpretation = "UDP Socket",
        .whyMaterial = "Low latency datagram",
        .evidence = {SourceSpan{10, 24}}
    });

    ChoiceGroup g2;
    g2.id = "group_timeout";
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2a",
        .interpretation = "5000ms Timeout",
        .whyMaterial = "Fast failover",
        .evidence = {SourceSpan{18, 32}}
    });
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2b",
        .interpretation = "30000ms Timeout",
        .whyMaterial = "Patient retry",
        .evidence = {SourceSpan{18, 32}}
    });

    analysis.choiceGroups = {g1, g2};

    auto overall = MaterialityPolicyV1::evaluatePrompt(analysis, prompt);
    ROCLINT_CHECK(overall.hasCrossGroupOverlap);
    ROCLINT_CHECK(overall.overallVerdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(overall.primaryReasonCode == ReasonCode::UnsafeSpanOverlap);

    ROCLINT_CHECK(overall.groupResults.size() == 2);
    ROCLINT_CHECK(overall.groupResults[0].verdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(overall.groupResults[0].reasonCode == ReasonCode::UnsafeSpanOverlap);
    ROCLINT_CHECK(overall.groupResults[1].verdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(overall.groupResults[1].reasonCode == ReasonCode::UnsafeSpanOverlap);
}

void testDirectConflictPresence() {
    const std::string prompt = "Implement stateless authentication with sticky server sessions";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_conflict";
    analysis.baseRevisionId = "rev_conflict";

    ChoiceGroup g;
    g.id = "group_auth";
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "Stateless JWT",
        .whyMaterial = "No server session table",
        .evidence = {SourceSpan{10, 34}}
    });
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "Stateful Redis Sessions",
        .whyMaterial = "Server-side revocation",
        .evidence = {SourceSpan{10, 34}}
    });
    analysis.choiceGroups.push_back(g);

    Conflict c;
    c.id = "conflict_1";
    c.description = "Contradiction between 'stateless' and 'sticky server sessions'";
    c.blocking = true;
    analysis.conflicts.push_back(c);

    auto overall = MaterialityPolicyV1::evaluatePrompt(analysis, prompt);
    ROCLINT_CHECK(overall.overallVerdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(overall.primaryReasonCode == ReasonCode::DirectConflictPresent);
    ROCLINT_CHECK(overall.conflictCount == 1);
}

void testAssumptionBudgetCap() {
    const std::string prompt = "Build payment gateway";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_assumptions";
    analysis.baseRevisionId = "rev_assumptions";

    ChoiceGroup g;
    g.id = "group_payment";
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1",
        .interpretation = "Stripe integration",
        .whyMaterial = "Global card processing",
        .evidence = {SourceSpan{6, 21}}
    });
    g.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2",
        .interpretation = "PayPal integration",
        .whyMaterial = "Wallet support",
        .evidence = {SourceSpan{6, 21}}
    });
    analysis.choiceGroups.push_back(g);

    analysis.assumptions = {
        Assumption{.id = "a1", .statement = "Target currency is USD"},
        Assumption{.id = "a2", .statement = "Webhook endpoint is HTTPS"},
        Assumption{.id = "a3", .statement = "PCI DSS Level 1 compliant"}
    };

    auto resUnder = MaterialityPolicyV1::evaluatePrompt(analysis, prompt);
    ROCLINT_CHECK(resUnder.overallVerdict == PolicyVerdict::EligibleForChoice);
    ROCLINT_CHECK(resUnder.primaryReasonCode == ReasonCode::Ok);

    analysis.assumptions.push_back(Assumption{.id = "a4", .statement = "Idempotency keys are enabled"});
    auto resOver = MaterialityPolicyV1::evaluatePrompt(analysis, prompt);
    ROCLINT_CHECK(resOver.overallVerdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(resOver.primaryReasonCode == ReasonCode::ExceedsAssumptionCap);
}

void testLocalIsolation() {
    const std::string prompt = "Sort list and format report outputs";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_iso";
    analysis.baseRevisionId = "rev_iso";

    ChoiceGroup g1;
    g1.id = "group_sort";
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_sort_asc",
        .interpretation = "Ascending Order",
        .whyMaterial = "Smallest to largest",
        .evidence = {SourceSpan{0, 9}}
    });
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_sort_desc",
        .interpretation = "Descending Order",
        .whyMaterial = "Largest to smallest",
        .evidence = {SourceSpan{0, 9}}
    });

    ChoiceGroup g2;
    g2.id = "group_format";
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_f1",
        .interpretation = "JSON format",
        .whyMaterial = "Machine readable",
        .evidence = {SourceSpan{14, 27}}
    });
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_f2",
        .interpretation = "CSV format",
        .whyMaterial = "Spreadsheet readable",
        .evidence = {SourceSpan{14, 27}}
    });
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_f3",
        .interpretation = "Markdown table",
        .whyMaterial = "Human readable",
        .evidence = {SourceSpan{14, 27}}
    });
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_f4",
        .interpretation = "Plain text",
        .whyMaterial = "Raw dump",
        .evidence = {SourceSpan{14, 27}}
    });

    analysis.choiceGroups = {g1, g2};

    auto overall = MaterialityPolicyV1::evaluatePrompt(analysis, prompt);
    ROCLINT_CHECK(overall.groupResults[0].verdict == PolicyVerdict::EligibleForChoice);
    ROCLINT_CHECK(overall.groupResults[0].isEligible());

    ROCLINT_CHECK(overall.groupResults[1].verdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(overall.groupResults[1].reasonCode == ReasonCode::ExcessiveAlternatives);

    ROCLINT_CHECK(overall.overallVerdict == PolicyVerdict::EligibleForChoice);
}

void testOrthogonalDimensionsOnSameSpan() {
    const std::string prompt = "make my website more faster";

    NormalizedAnalysis analysis;
    analysis.sessionId = "sess_ortho";
    analysis.baseRevisionId = "rev_ortho";

    ChoiceGroup g1;
    g1.id = "group_speed_strategy";
    g1.dimension = "speed_strategy";
    g1.question = "Which optimization strategy should be prioritized?";
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1a",
        .interpretation = "Optimize images and static assets",
        .whyMaterial = "Reduces asset payload",
        .evidence = {SourceSpan{0, 27}}
    });
    g1.alternatives.push_back(ChoiceAlternative{
        .id = "alt_1b",
        .interpretation = "Implement aggressive caching",
        .whyMaterial = "Avoids redundant server round-trips",
        .evidence = {SourceSpan{0, 27}}
    });

    ChoiceGroup g2;
    g2.id = "group_frontend_arch";
    g2.dimension = "frontend_architecture";
    g2.question = "What architectural approach best supports faster loading?";
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2a",
        .interpretation = "Modular component design",
        .whyMaterial = "Improves code reuse and loading granularity",
        .evidence = {SourceSpan{0, 27}}
    });
    g2.alternatives.push_back(ChoiceAlternative{
        .id = "alt_2b",
        .interpretation = "Let agent follow workspace conventions",
        .whyMaterial = "Adapts to existing UI structure",
        .evidence = {SourceSpan{0, 27}}
    });

    analysis.choiceGroups = {g1, g2};

    auto overall = MaterialityPolicyV1::evaluatePrompt(analysis, prompt);
    ROCLINT_CHECK(!overall.hasCrossGroupOverlap);
    ROCLINT_CHECK(overall.overallVerdict == PolicyVerdict::EligibleForChoice);
    ROCLINT_CHECK(overall.primaryReasonCode == ReasonCode::Ok);

    ROCLINT_CHECK(overall.groupResults.size() == 2);
    ROCLINT_CHECK(overall.groupResults[0].verdict == PolicyVerdict::EligibleForChoice);
    ROCLINT_CHECK(overall.groupResults[0].isEligible());
    ROCLINT_CHECK(overall.groupResults[1].verdict == PolicyVerdict::EligibleForChoice);
    ROCLINT_CHECK(overall.groupResults[1].isEligible());
}

int main() {
    std::cout << "[TEST] Running MaterialityPolicyV1 Unit Tests..." << std::endl;

    testCardinalityBounds();
    testLabelDistinctness();
    testMaterialityGrounding();
    testUtf8SpanBoundaryValidation();
    testCrossGroupSpanOverlap();
    testOrthogonalDimensionsOnSameSpan();
    testDirectConflictPresence();
    testAssumptionBudgetCap();
    testLocalIsolation();

    std::cout << "[PASS] All MaterialityPolicyV1 tests passed successfully!" << std::endl;
    return 0;
}
