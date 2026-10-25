#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>
#include <limits>

using namespace roclint::core;
using namespace roclint::bridge;

namespace {

void testPromptTriageOwnsTheExactGateBoundary() {
    analysis::PromptTriage source;
    const std::string prompt = "Review this request.";
    source.sessionId = "sess_triage"; source.revisionId = "rev_sess_triage_1";
    source.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    source.stateVersion = 2; source.requestGroupId = "grp_triage"; source.attemptId = "att_triage";
    source.modelId = "fixture_julia"; source.modelRevision = "fixture_v1";
    source.weightsSha256 = std::string(64, 'a'); source.configurationId = "test_only";
    source.execution = analysis::TriageExecution::Mock;
    source.categoryScores = {0.1, 0.2, 0.69, 0.3}; source.latencyMs = 1;
    // Three-tier routing: Clarify at threshold, UncertainBuffer in dead band, Original below dead band.
    // Default policy: RULE threshold=0.70, delta=0.08, so dead band is [0.62, 0.70).
    for (const auto score : {0.699999, 0.70, 0.700001}) {
        source.categoryScores[2] = score;
        const auto sealed = analysis::sealPromptTriage(source, prompt);
        ROCLINT_CHECK(sealed.ok() && analysis::validatePromptTriage(sealed.value()).ok());
        ROCLINT_CHECK(sealed.value().primaryCategory == analysis::PromptCategory::Rule);
        ROCLINT_CHECK(sealed.value().ambiguityScore == score);
        const auto route = analysis::routePromptTriage(sealed.value());
        // 0.699999 is within [0.62, 0.70) → UncertainBuffer; 0.70 and 0.700001 → Clarify.
        const auto expected = (score >= 0.70)
            ? analysis::PromptTriageRoute::Clarify
            : analysis::PromptTriageRoute::UncertainBuffer;
        ROCLINT_CHECK(route.ok() && route.value() == expected);
        auto tampered = sealed.value(); tampered.categoryScores[0] = 0.8;
        ROCLINT_CHECK(!analysis::routePromptTriage(tampered).ok());
    }
    // Below the dead band → Original (score 0.1 on all heads is well clear).
    source.categoryScores = {0.1, 0.2, 0.1, 0.3};
    {
        const auto sealed = analysis::sealPromptTriage(source, prompt);
        ROCLINT_CHECK(sealed.ok());
        const auto route = analysis::routePromptTriage(sealed.value());
        ROCLINT_CHECK(route.ok() && route.value() == analysis::PromptTriageRoute::Original);
    }
    source.categoryScores.fill(0.9); // Independent heads are not a normalized five-way distribution.
    const auto tied = analysis::sealPromptTriage(source, prompt);
    ROCLINT_CHECK(tied.ok() && tied.value().primaryCategory == analysis::PromptCategory::Action);
    ROCLINT_CHECK(!analysis::sealPromptTriage(source, "Different request.").ok());
    auto missing = source; missing.execution = analysis::TriageExecution::NotExecuted;
    ROCLINT_CHECK(!analysis::sealPromptTriage(missing, prompt).ok());
    missing = source; missing.attemptId.clear();
    ROCLINT_CHECK(!analysis::sealPromptTriage(missing, prompt).ok());
    missing = source; missing.stateVersion = 0;
    ROCLINT_CHECK(!analysis::sealPromptTriage(missing, prompt).ok());
    for (const auto invalidScore : {-0.01, 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        missing = source; missing.categoryScores[1] = invalidScore;
        ROCLINT_CHECK(!analysis::sealPromptTriage(missing, prompt).ok());
    }
    ROCLINT_CHECK(!analysis::routePromptTriage(analysis::PromptTriage{}).ok());
}

void testV1AndV2HashDomainsAreDistinct() {
    analysis::PromptTriage r;
    const std::string prompt = "Review this module.";
    r.sessionId = "sess_v2hash"; r.revisionId = "rev_v2hash_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 1; r.requestGroupId = "grp_v2"; r.attemptId = "att_v2";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'b'); r.configurationId = "test_only";
    r.execution = analysis::TriageExecution::Mock;
    r.categoryScores = {0.1, 0.2, 0.3, 0.4}; r.latencyMs = 2;

    r.hashVersion = analysis::TriageHashVersion::V2;
    const auto v2 = analysis::sealPromptTriage(r, prompt);
    ROCLINT_CHECK(v2.ok());
    ROCLINT_CHECK(analysis::validatePromptTriage(v2.value()).ok());

    r.hashVersion = analysis::TriageHashVersion::V1;
    const auto v1 = analysis::sealPromptTriage(r, prompt);
    ROCLINT_CHECK(v1.ok());
    ROCLINT_CHECK(analysis::validatePromptTriage(v1.value()).ok());

    // V1 domain bakes in kPromptTriageThreshold; V2 does not — hashes must differ.
    ROCLINT_CHECK(v2.value().triageHash != v1.value().triageHash);

    // Tampering the hashVersion on a sealed record must fail validation (hash mismatch).
    auto tampered = v2.value();
    tampered.hashVersion = analysis::TriageHashVersion::V1;
    ROCLINT_CHECK(!analysis::validatePromptTriage(tampered).ok());

    auto tampered2 = v1.value();
    tampered2.hashVersion = analysis::TriageHashVersion::V2;
    ROCLINT_CHECK(!analysis::validatePromptTriage(tampered2).ok());

    // New seals must default to V2.
    analysis::PromptTriage fresh;
    ROCLINT_CHECK(fresh.hashVersion == analysis::TriageHashVersion::V2);
}

void testPerHeadPolicyRouting() {
    const std::string prompt = "Apply the patch.";
    analysis::PromptTriage base;
    base.sessionId = "sess_policy"; base.revisionId = "rev_policy_1";
    base.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    base.stateVersion = 1; base.requestGroupId = "grp_policy"; base.attemptId = "att_policy";
    base.modelId = "fixture_julia"; base.modelRevision = "fixture_v1";
    base.weightsSha256 = std::string(64, 'c'); base.configurationId = "test_only";
    base.execution = analysis::TriageExecution::Mock; base.latencyMs = 1;

    analysis::TriagePolicy policy;
    policy.thresholds = {0.70, 0.70, 0.70, 0.60};
    policy.uncertaintyDelta = 0.08;

    // RULE_CONFLICT at 0.62 — below global 0.70 but above per-head 0.60 → Clarify
    base.categoryScores = {0.10, 0.15, 0.20, 0.62};
    auto sealed = analysis::sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed.ok());
    auto route = analysis::routePromptTriage(sealed.value(), policy);
    ROCLINT_CHECK(route.ok() && route.value() == analysis::PromptTriageRoute::Clarify);

    // All heads at 0.55 — RULE_CONFLICT in dead band [0.52, 0.60) → UncertainBuffer
    base.categoryScores = {0.55, 0.55, 0.55, 0.55};
    sealed = analysis::sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed.ok());
    route = analysis::routePromptTriage(sealed.value(), policy);
    ROCLINT_CHECK(route.ok() && route.value() == analysis::PromptTriageRoute::UncertainBuffer);

    // All heads below dead band → Original
    base.categoryScores = {0.20, 0.15, 0.10, 0.30};
    sealed = analysis::sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed.ok());
    route = analysis::routePromptTriage(sealed.value(), policy);
    ROCLINT_CHECK(route.ok() && route.value() == analysis::PromptTriageRoute::Original);

    // ACTION at 0.65 — in dead band for thr=0.70, delta=0.08 → UncertainBuffer
    base.categoryScores = {0.65, 0.20, 0.15, 0.10};
    sealed = analysis::sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed.ok());
    route = analysis::routePromptTriage(sealed.value(), policy);
    ROCLINT_CHECK(route.ok() && route.value() == analysis::PromptTriageRoute::UncertainBuffer);

    // Default overload must produce the same result as passing TriagePolicy::defaults() explicitly
    base.categoryScores = {0.62, 0.20, 0.15, 0.10};
    sealed = analysis::sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed.ok());
    const auto routeDefault   = analysis::routePromptTriage(sealed.value());
    const auto routeExplicit  = analysis::routePromptTriage(sealed.value(), analysis::TriagePolicy::defaults());
    ROCLINT_CHECK(routeDefault.ok() && routeExplicit.ok());
    ROCLINT_CHECK(routeDefault.value() == routeExplicit.value());
}

void testUncertainBufferDoesNotFireWithZeroDelta() {
    const std::string prompt = "Delete it.";
    analysis::PromptTriage base;
    base.sessionId = "sess_db"; base.revisionId = "rev_db_1";
    base.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    base.stateVersion = 1; base.requestGroupId = "grp_db"; base.attemptId = "att_db";
    base.modelId = "fixture_julia"; base.modelRevision = "fixture_v1";
    base.weightsSha256 = std::string(64, 'd'); base.configurationId = "test_only";
    base.execution = analysis::TriageExecution::Mock; base.latencyMs = 1;

    analysis::TriagePolicy noDeadBand;
    noDeadBand.thresholds = {0.70, 0.70, 0.70, 0.70};
    noDeadBand.uncertaintyDelta = 0.0;

    // All scores below threshold; delta=0 means no dead band → Original
    base.categoryScores = {0.65, 0.66, 0.67, 0.68};
    const auto sealed = analysis::sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed.ok());
    const auto route = analysis::routePromptTriage(sealed.value(), noDeadBand);
    ROCLINT_CHECK(route.ok() && route.value() == analysis::PromptTriageRoute::Original);
}

SnapshotEvaluationResponse scored(const analysis::NormalizedAnalysis& a) {
    SnapshotEvaluationResponse r;
    r.success = true;
    r.analysisSnapshotId = a.id;
    r.executionStatus = ExecutionStatus::ExecutedModel;
    for (const auto& g : a.choiceGroups) {
        GroupEvaluationScore s;
        s.groupId = g.id;
        s.isScored = true;
        s.ambiguityScore = 0.8;
        s.winningAlternativeId = g.alternatives[0].id;
        s.probabilities = std::vector<AlternativeProbability>{{g.alternatives[0].id, 0.75},
                                                              {g.alternatives[1].id, 0.25}};
        r.groupScores.push_back(s);
    }
    return r;
}

void testTriageValidatesInputWithoutScoring() {
    S1TriageAdapter s1;
    for (const char* text : {"Refactor this module", "Which file: a.cpp or b.cpp?"}) {
        const auto d = s1.triagePrompt(text);
        ROCLINT_CHECK(d.isExecutable());
        ROCLINT_CHECK(!d.isAmbiguityScored);
        ROCLINT_CHECK(!d.ambiguityScore.has_value());
        ROCLINT_CHECK(!d.fastPathClear);
    }
    ROCLINT_CHECK(s1.triagePrompt("").inputValidity == InputValidity::Empty);
    ROCLINT_CHECK(s1.triagePrompt(" \n\t").inputValidity == InputValidity::WhitespaceOnly);
    ROCLINT_CHECK(!s1.triagePrompt("").isExecutable());
}

void testParseEngine() {
    ROCLINT_CHECK(S1TriageAdapter::parseEngine("jev").ok());
    ROCLINT_CHECK(S1TriageAdapter::parseEngine("JEV_CLOUD").ok());
    ROCLINT_CHECK(S1TriageAdapter::parseEngine("julia").value() == ScorerEngine::JuliaLocal);
    ROCLINT_CHECK(S1TriageAdapter::parseEngine("JULIA_LOCAL").ok());
    for (const char* bad : {"native", ""}) {
        auto r = S1TriageAdapter::parseEngine(bad);
        ROCLINT_CHECK(!r.ok());
        ROCLINT_CHECK(r.error().code == ErrorCode::InvalidInput);
    }
}

void testOfflineCloudIsDegradedUnranked() {
    S1TriageAdapter s1;
    ROCLINT_CHECK(s1.availability() == SemanticAvailability::DegradedUnranked);
    const auto a = fx::threeGroupAnalysis();
    const auto r = s1.evaluateSnapshot(a);
    ROCLINT_CHECK(r.success);
    ROCLINT_CHECK(!r.isRanked());
    ROCLINT_CHECK(r.executionStatus == ExecutionStatus::DegradedUnranked);
    ROCLINT_CHECK(!r.executedEngine.has_value());
    ROCLINT_CHECK(!r.modelInferenceExecuted);
    ROCLINT_CHECK(r.modelVersion == "none");
    ROCLINT_CHECK(!r.degradedReason.empty());
    ROCLINT_CHECK(r.analysisSnapshotId == a.id);
    ROCLINT_CHECK(r.groupScores.size() == a.choiceGroups.size());
    for (const auto& s : r.groupScores) {
        ROCLINT_CHECK(!s.isScored);
        ROCLINT_CHECK(!s.ambiguityScore.has_value());
        ROCLINT_CHECK(!s.probabilities.has_value());
        ROCLINT_CHECK(!s.winningAlternativeId.has_value());
    }
}

void testFailingCloudScorerDegrades() {
    S1TriageAdapter s1;
    s1.setMockCloudScorer([](const analysis::NormalizedAnalysis&) -> Result<SnapshotEvaluationResponse> {
        return makeError(ErrorCode::ProviderUnavailable, "Jev cloud offline");
    });
    const auto a = fx::threeGroupAnalysis();
    const auto r = s1.evaluateSnapshot(a);
    ROCLINT_CHECK(r.success);
    ROCLINT_CHECK(r.executionStatus == ExecutionStatus::DegradedUnranked);
    ROCLINT_CHECK(r.degradedReason.find("offline") != std::string::npos);
    for (const auto& s : r.groupScores) {
        ROCLINT_CHECK(!s.isScored);
    }
}

void testValidCloudScoresAreKept() {
    S1TriageAdapter s1;
    s1.setMockCloudScorer([](const analysis::NormalizedAnalysis& a) -> Result<SnapshotEvaluationResponse> {
        return scored(a);
    });
    ROCLINT_CHECK(s1.availability() == SemanticAvailability::FullRankedCloud);
    const auto a = fx::threeGroupAnalysis();
    const auto r = s1.evaluateSnapshot(a);
    ROCLINT_CHECK(r.isRanked());
    ROCLINT_CHECK(r.executionStatus == ExecutionStatus::ExecutedMock);
    ROCLINT_CHECK(!r.modelInferenceExecuted);
    for (const auto& s : r.groupScores) {
        ROCLINT_CHECK(s.isScored);
        ROCLINT_CHECK(s.probabilities->size() == 2);
    }
}

void testMalformedCloudScoresAreDowngraded() {
    const auto a = fx::threeGroupAnalysis();

    S1TriageAdapter wrongSnapshot;
    wrongSnapshot.setMockCloudScorer([](const analysis::NormalizedAnalysis& x) -> Result<SnapshotEvaluationResponse> {
        auto r = scored(x);
        r.analysisSnapshotId = "snap_other";
        return r;
    });
    ROCLINT_CHECK(!wrongSnapshot.evaluateSnapshot(a).isRanked());

    S1TriageAdapter corrupt;
    corrupt.setMockCloudScorer([](const analysis::NormalizedAnalysis& x) -> Result<SnapshotEvaluationResponse> {
        auto r = scored(x);
        r.groupScores[0].ambiguityScore = std::numeric_limits<double>::quiet_NaN();
        r.groupScores[1].probabilities = std::vector<AlternativeProbability>{{"alt_1_0", 0.9}, {"alt_1_1", 0.9}};
        r.groupScores[2].winningAlternativeId = "alt_0_0"; // belongs to another group
        return r;
    });
    const auto r = corrupt.evaluateSnapshot(a);
    ROCLINT_CHECK(r.groupScores.size() == 3);
    for (const auto& s : r.groupScores) {
        ROCLINT_CHECK(!s.isScored);
    }
    ROCLINT_CHECK(!r.isRanked());

    S1TriageAdapter foreignAlt;
    foreignAlt.setMockCloudScorer([](const analysis::NormalizedAnalysis& x) -> Result<SnapshotEvaluationResponse> {
        auto r = scored(x);
        r.groupScores[0].probabilities = std::vector<AlternativeProbability>{{"alt_9_9", 1.0}};
        return r;
    });
    const auto f = foreignAlt.evaluateSnapshot(a);
    ROCLINT_CHECK(!f.groupScores[0].isScored);
    ROCLINT_CHECK(f.groupScores[1].isScored);
    ROCLINT_CHECK(f.isRanked());
}

} // namespace

int main() {
    std::cout << "[Test S1TriageAdapter] Starting...\n";
    testTriageValidatesInputWithoutScoring();
    testPromptTriageOwnsTheExactGateBoundary();
    testV1AndV2HashDomainsAreDistinct();
    testPerHeadPolicyRouting();
    testUncertainBufferDoesNotFireWithZeroDelta();
    testParseEngine();
    testOfflineCloudIsDegradedUnranked();
    testFailingCloudScorerDegrades();
    testValidCloudScoresAreKept();
    testMalformedCloudScoresAreDowngraded();
    std::cout << "[Test S1TriageAdapter] All tests PASSED.\n";
    return 0;
}
