#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/analysis/SourceSpan.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/core/prompt_state/PromptRevision.hpp"
#include "roclint/core/prompt_state/RevisionId.hpp"
#include "roclint/core/prompt_state/SessionId.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::bridge;

void testEndToEndJevFastTriageAndEvaluationWalkthrough() {
    SessionId sessionId("sess_i05_001");
    RevisionId revId("rev_i05_root");
    std::string ambiguousPrompt = "Which component should handle connection timeout: FloatingCard.qml or HostController.cpp?";
    PromptRevision promptRev(sessionId, revId, std::nullopt, ambiguousPrompt);

    S1TriageAdapter s1Adapter(ScorerEngine::JuliaLocal);

    auto triage = s1Adapter.triagePrompt(promptRev.text());
    ROCLINT_CHECK(triage.isAmbiguous);
    ROCLINT_CHECK(triage.ambiguityScore >= 0.70);
    ROCLINT_CHECK(triage.dimension == "target");
    ROCLINT_CHECK(!triage.fastPathClear);

    GraniteAdapter graniteAdapter;
    GraniteOptionDiscoveryRequest graniteReq{
        .sessionId = promptRev.sessionId().value(),
        .revisionId = promptRev.revisionId().value(),
        .promptString = promptRev.text(),
        .s1Category = triage.dimension,
        .hostContextCategory = "CODE_EDITOR"
    };

    auto discoveryResp = graniteAdapter.discoverOptions(graniteReq);
    ROCLINT_CHECK(discoveryResp.success);
    ROCLINT_CHECK(discoveryResp.choiceGroup.has_value());
    ROCLINT_CHECK(discoveryResp.quantization == "Q8_0");

    auto group = *discoveryResp.choiceGroup;
    ROCLINT_CHECK(group.alternatives.size() == 2);

    auto spanMatch1 = findUniqueSpan(promptRev.text(), "FloatingCard.qml");
    ROCLINT_CHECK(spanMatch1.status == SpanMatchStatus::ExactMatch);
    group.alternatives[0].evidence.push_back(*spanMatch1.span);

    auto spanMatch2 = findUniqueSpan(promptRev.text(), "HostController.cpp");
    ROCLINT_CHECK(spanMatch2.status == SpanMatchStatus::ExactMatch);
    group.alternatives[1].evidence.push_back(*spanMatch2.span);

    NormalizedAnalysis analysis;
    analysis.id = generateDeterministicId("snapshot", 0, promptRev.revisionId().value());
    analysis.baseRevisionId = promptRev.revisionId().value();
    analysis.choiceGroups.push_back(group);

    auto policyResult = MaterialityPolicyV1::evaluatePrompt(analysis, promptRev.text());
    ROCLINT_CHECK(policyResult.isEligible());
    ROCLINT_CHECK(policyResult.overallVerdict == PolicyVerdict::EligibleForChoice);
    ROCLINT_CHECK(policyResult.primaryReasonCode == ReasonCode::Ok);
    ROCLINT_CHECK(policyResult.groupResults.size() == 1);
    ROCLINT_CHECK(policyResult.groupResults[0].isEligible());

    auto snapshotEval = s1Adapter.evaluateSnapshot(analysis);
    ROCLINT_CHECK(snapshotEval.success);
    ROCLINT_CHECK(snapshotEval.engine == ScorerEngine::JuliaLocal);
    ROCLINT_CHECK(snapshotEval.groupScores.size() == 1);

    const auto& score = snapshotEval.groupScores.front();
    ROCLINT_CHECK(score.groupId == group.id);
    ROCLINT_CHECK(score.probabilities.size() == 2);
    ROCLINT_CHECK(score.probabilities[0].alternativeId == group.alternatives[0].id);
    ROCLINT_CHECK(score.probabilities[1].alternativeId == group.alternatives[1].id);

    std::cout << "[I05 Walkthrough] Prompt -> S1 Triage -> Granite Q8_0 -> MaterialityPolicy -> Julia Evaluation OK!\n";
}

void testClearPromptFastPathBypass() {
    SessionId sessionId("sess_i05_002");
    RevisionId revId("rev_i05_clear");
    std::string clearPrompt = "Set background color of FloatingCard to #1E1E2E";
    PromptRevision promptRev(sessionId, revId, std::nullopt, clearPrompt);

    S1TriageAdapter s1Adapter(ScorerEngine::JuliaLocal);
    auto triage = s1Adapter.triagePrompt(promptRev.text());

    ROCLINT_CHECK(!triage.isAmbiguous);
    ROCLINT_CHECK(triage.ambiguityScore < 0.70);
    ROCLINT_CHECK(triage.fastPathClear);

    std::cout << "[I05 FastPath] Clear prompt successfully bypassed Granite in <" << triage.latencyMs << "ms!\n";
}

void testAblationCloudFailureGracefulDegradation() {
    S1TriageAdapter cloudAdapter(ScorerEngine::JevCloud);
    cloudAdapter.setCloudAvailable(false);

    ROCLINT_CHECK(cloudAdapter.availability() == SemanticAvailability::DegradedUnranked);

    NormalizedAnalysis analysis;
    analysis.id = "ablation_snap";
    analysis.baseRevisionId = "ablation_rev";

    auto resp = cloudAdapter.evaluateSnapshot(analysis);
    ROCLINT_CHECK(!resp.success);
    ROCLINT_CHECK(resp.errorMessage.find("unavailable") != std::string::npos);

    std::cout << "[I05 Ablation] Cloud unavailable returns typed failure without fabricated confidence.\n";
}

int main() {
    std::cout << "[TEST] Running I05 Jev Checkpoint Integration Tests..." << std::endl;

    testEndToEndJevFastTriageAndEvaluationWalkthrough();
    testClearPromptFastPathBypass();
    testAblationCloudFailureGracefulDegradation();

    std::cout << "[PASS] All I05 Jev Checkpoint tests passed successfully!" << std::endl;
    return 0;
}
