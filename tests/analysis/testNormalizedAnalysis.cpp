#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string_view>

using namespace roclint::core::analysis;

void testHostMintedIds() {
    ROCLINT_CHECK(mintGroupId(0) == "grp_0");
    ROCLINT_CHECK(mintAlternativeId(1, 2) == "alt_1_2");
    ROCLINT_CHECK(mintAssumptionId(3) == "asm_3");
    ROCLINT_CHECK(mintMissingDecisionId(0) == "mdc_0");
    ROCLINT_CHECK(mintConflictId(4) == "cfl_4");
    // Index pairs cannot alias each other.
    ROCLINT_CHECK(mintAlternativeId(1, 12) != mintAlternativeId(11, 2));
}

NormalizedAnalysis sealedSample() {
    NormalizedAnalysis a;
    a.sessionId = "sess_a";
    a.baseRevisionId = "rev_sess_a_1";
    ChoiceGroup g;
    g.id = mintGroupId(0);
    g.question = "Which file?";
    g.alternatives.push_back(ChoiceAlternative{mintAlternativeId(0, 0), "A", "why", "", {SourceSpan{0, 3}}});
    g.alternatives.push_back(ChoiceAlternative{mintAlternativeId(0, 1), "B", "why", "", {SourceSpan{0, 3}}});
    a.choiceGroups.push_back(g);
    sealSnapshot(a);
    return a;
}

void testSnapshotSealing() {
    auto a = sealedSample();
    ROCLINT_CHECK(isSnapshotSealed(a));
    ROCLINT_CHECK(a.id == "snap_" + a.snapshotHash);
    ROCLINT_CHECK(a.snapshotHash.size() == 64);
    ROCLINT_CHECK(sealedSample().snapshotHash == a.snapshotHash);

    auto tamperedSpan = a;
    tamperedSpan.choiceGroups[0].alternatives[0].evidence[0].endByte = 2;
    ROCLINT_CHECK(!isSnapshotSealed(tamperedSpan));

    auto tamperedRevision = a;
    tamperedRevision.baseRevisionId = "rev_sess_a_2";
    ROCLINT_CHECK(!isSnapshotSealed(tamperedRevision));

    auto tamperedSession = a;
    tamperedSession.sessionId = "sess_b";
    ROCLINT_CHECK(!isSnapshotSealed(tamperedSession));

    auto tamperedFlag = a;
    tamperedFlag.choiceGroups[0].material = false;
    ROCLINT_CHECK(!isSnapshotSealed(tamperedFlag));
}

void testOrderedAssumptionIndices() {
    NormalizedAnalysis a;
    a.assumptions.push_back(Assumption{"asm_0", "unanchored", true, {}});
    a.assumptions.push_back(Assumption{"asm_1", "late", true, {SourceSpan{20, 25}}});
    a.assumptions.push_back(Assumption{"asm_2", "early", true, {SourceSpan{2, 4}}});
    const auto order = orderedAssumptionIndices(a);
    ROCLINT_CHECK(order.size() == 3);
    ROCLINT_CHECK(order[0] == 2);
    ROCLINT_CHECK(order[1] == 1);
    ROCLINT_CHECK(order[2] == 0);
}

void testValidAnalysisSnapshot() {
    std::string_view prompt = "Refactor SessionManager timeout handling in FloatingCard.qml";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_001";
    analysis.baseRevisionId = "rev_initial";

    ChoiceGroup group;
    group.id = "group_target";
    group.dimension = "target";
    group.question = "Which target file should receive timeout refactoring?";

    ChoiceAlternative alt1;
    alt1.id = "alt_1";
    alt1.interpretation = "FloatingCard.qml";

    alt1.evidence.push_back(SourceSpan{.startByte = 44, .endByte = 60});

    ChoiceAlternative alt2;
    alt2.id = "alt_2";
    alt2.interpretation = "SessionManager.cpp";
   
    alt2.evidence.push_back(SourceSpan{.startByte = 9, .endByte = 23});

    group.alternatives.push_back(alt1);
    group.alternatives.push_back(alt2);

    analysis.choiceGroups.push_back(group);

    auto result = validateNormalizedAnalysis(analysis, prompt);
    ROCLINT_CHECK(result.isValid());
    ROCLINT_CHECK(result.issues.empty());
}

void testCardinalityViolationRoutesToManualReview() {
    std::string_view prompt = "Refactor timeout handling";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_002";
    analysis.baseRevisionId = "rev_1";

    ChoiceGroup singleAltGroup;
    singleAltGroup.id = "group_single";
    singleAltGroup.dimension = "action";
    singleAltGroup.question = "Only one choice provided?";
    singleAltGroup.alternatives.push_back(ChoiceAlternative{
        .id = "alt_solo",
        .interpretation = "Do only option A"
    });

    analysis.choiceGroups.push_back(singleAltGroup);

    // Group with only 1 alternative violates cardinality -> RequiresManualReview
    auto result = validateNormalizedAnalysis(analysis, prompt);
    ROCLINT_CHECK(!result.isValid());
    ROCLINT_CHECK(result.requiresManualReview());
    ROCLINT_CHECK(!result.issues.empty());

    // 4 alternatives also violates cardinality (max is 3)
    singleAltGroup.alternatives.push_back(ChoiceAlternative{.id = "alt_2", .interpretation = "Option B"});
    singleAltGroup.alternatives.push_back(ChoiceAlternative{.id = "alt_3", .interpretation = "Option C"});
    singleAltGroup.alternatives.push_back(ChoiceAlternative{.id = "alt_4", .interpretation = "Option D"});

    analysis.choiceGroups.clear();
    analysis.choiceGroups.push_back(singleAltGroup);

    auto result4 = validateNormalizedAnalysis(analysis, prompt);
    ROCLINT_CHECK(!result4.isValid());
    ROCLINT_CHECK(result4.requiresManualReview());
}

void testDuplicateAlternativesRouteToManualReview() {
    std::string_view prompt = "Refactor timeout handling";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_003";
    analysis.baseRevisionId = "rev_1";

    ChoiceGroup group;
    group.id = "group_dup";
    group.dimension = "action";
    group.question = "Action to take?";
    group.alternatives.push_back(ChoiceAlternative{.id = "alt_1", .interpretation = "Refactor SessionManager"});
    group.alternatives.push_back(ChoiceAlternative{.id = "alt_2", .interpretation = "Refactor SessionManager"}); // Exact duplicate

    analysis.choiceGroups.push_back(group);

    auto result = validateNormalizedAnalysis(analysis, prompt);
    ROCLINT_CHECK(!result.isValid());
    ROCLINT_CHECK(result.requiresManualReview());
}

void testInvalidSourceSpanMarksInvalid() {
    std::string_view prompt = "Short prompt"; // length 12

    NormalizedAnalysis analysis;
    analysis.id = "analysis_004";
    analysis.baseRevisionId = "rev_1";

    ChoiceGroup group;
    group.id = "group_span_err";
    group.dimension = "target";
    group.question = "Target?";

    ChoiceAlternative alt1;
    alt1.id = "alt_1";
    alt1.interpretation = "Option A";
    // Out-of-bounds span [50, 100)
    alt1.evidence.push_back(SourceSpan{.startByte = 50, .endByte = 100});

    ChoiceAlternative alt2;
    alt2.id = "alt_2";
    alt2.interpretation = "Option B";

    group.alternatives.push_back(alt1);
    group.alternatives.push_back(alt2);

    analysis.choiceGroups.push_back(group);

    auto result = validateNormalizedAnalysis(analysis, prompt);
    ROCLINT_CHECK(!result.isValid());
    ROCLINT_CHECK(result.status == AnalysisValidationStatus::Invalid);
}

void testMissingBaseRevisionMarksInvalid() {
    std::string_view prompt = "Valid prompt";

    NormalizedAnalysis analysis;
    analysis.id = "analysis_no_base";
    analysis.baseRevisionId = ""; // Empty base revision

    auto result = validateNormalizedAnalysis(analysis, prompt);
    ROCLINT_CHECK(!result.isValid());
    ROCLINT_CHECK(result.status == AnalysisValidationStatus::Invalid);
}

int main() {
    std::cout << "[Test NormalizedAnalysis] Starting A06 test suite...\n";
    testHostMintedIds();
    testSnapshotSealing();
    testOrderedAssumptionIndices();
    testValidAnalysisSnapshot();
    testCardinalityViolationRoutesToManualReview();
    testDuplicateAlternativesRouteToManualReview();
    testInvalidSourceSpanMarksInvalid();
    testMissingBaseRevisionMarksInvalid();
    std::cout << "[Test NormalizedAnalysis] All tests PASSED successfully!\n";
    return 0;
}
