#include "roclint/analysis/AnalysisSnapshot.hpp"
#include "roclint/storage/PayloadCodec.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "support/storage_fixtures.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>

using namespace roclint;
using namespace roclint::core;
namespace an = roclint::core::analysis;

namespace {
void testS1FirstArtifact() {
    an::PromptTriage triage;
    triage.sessionId = "sess_triage"; triage.revisionId = "rev_sess_triage_1";
    triage.revisionTextHash = crypto::Sha256::hexDigest(fx::kChainPrompt);
    triage.stateVersion = 1; triage.requestGroupId = "grp_triage"; triage.attemptId = "att_triage";
    triage.modelId = "fixture_classifier"; triage.modelRevision = "fixture_revision";
    triage.weightsSha256 = std::string(64, 'a'); triage.configurationId = "fixture_config";
    triage.execution = an::TriageExecution::Mock;
    triage.categoryScores = {.699999, .1, .2, .3};
    triage = an::sealPromptTriage(triage, fx::kChainPrompt).value();
    an::NormalizedAnalysis empty;
    empty.sessionId = triage.sessionId; empty.baseRevisionId = triage.revisionId; an::sealSnapshot(empty);
    auto clear = an::buildRoutedAnalysisSnapshot(empty, triage, fx::kChainPrompt);
    ROCLINT_CHECK(clear.ok() && clear.value().payloadVersion == 4);
    ROCLINT_CHECK(clear.value().evaluation.executionStatus == an::ExecutionStatus::NotRequested);
    ROCLINT_CHECK(!clear.value().evaluation.isRanked());
    ROCLINT_CHECK(storage::decodeAnalysisSnapshot(storage::encodeAnalysisSnapshot(clear.value())).value() == clear.value());
    ROCLINT_CHECK(storage::decodePromptTriage(storage::encodePromptTriage(triage)).value() == triage);
    ROCLINT_CHECK(!an::buildRoutedAnalysisSnapshot(empty, triage, fx::kChainPrompt, "invented_discovery").ok());
    const auto choices = fx::chainAnalysis(triage.sessionId, triage.revisionId);
    ROCLINT_CHECK(!an::buildRoutedAnalysisSnapshot(choices, triage, fx::kChainPrompt, "discovery").ok());
    triage.categoryScores[0] = .70;
    triage = an::sealPromptTriage(triage, fx::kChainPrompt).value();
    ROCLINT_CHECK(!an::buildRoutedAnalysisSnapshot(empty, triage, fx::kChainPrompt).ok());
    ROCLINT_CHECK(!an::buildRoutedAnalysisSnapshot(choices, triage, fx::kChainPrompt).ok());
    auto ambiguous = an::buildRoutedAnalysisSnapshot(choices, triage, fx::kChainPrompt, "discovery");
    ROCLINT_CHECK(ambiguous.ok());
    ROCLINT_CHECK(storage::decodeAnalysisSnapshot(storage::encodeAnalysisSnapshot(ambiguous.value())).value() == ambiguous.value());
    auto forged = ambiguous.value();
    forged.evaluation.groupScores.push_back({"grp_0"});
    forged.snapshotHash = an::computeSnapshotHash(forged); forged.id = "analysis_" + forged.snapshotHash;
    ROCLINT_CHECK(!an::validateAnalysisSnapshot(forged).ok());
    forged = ambiguous.value(); forged.promptTriage->categoryScores[0] = .99;
    forged.snapshotHash = an::computeSnapshotHash(forged); forged.id = "analysis_" + forged.snapshotHash;
    ROCLINT_CHECK(!an::validateAnalysisSnapshot(forged).ok());

    storage::Database db(":memory:"); storage::PromptRepository repository(db);
    ROCLINT_CHECK(repository.migrate().ok());
    PromptRevision root(SessionId(triage.sessionId), RevisionId(triage.revisionId), std::nullopt, fx::kChainPrompt, 1);
    ROCLINT_CHECK(repository.createSessionWithRootRevision(root, 1).ok());
    ROCLINT_CHECK(!repository.insertAnalysisSnapshot(clear.value(), 2).ok());
    storage::ModelAttemptRecord attempt{clear.value().promptTriage->attemptId, triage.sessionId, triage.revisionId, "julia_triage"};
    attempt.transport = "test_fixture"; attempt.modelId = triage.modelId;
    ROCLINT_CHECK(repository.beginModelAttempt(attempt).ok());
    const auto detail = std::string("fixture; prompt_triage_payload=") + storage::encodePromptTriage(*clear.value().promptTriage);
    ROCLINT_CHECK(repository.completeModelAttempt(attempt.attemptId, storage::AttemptStatus::Succeeded, triage.modelId, detail, "", 2).ok());
    ROCLINT_CHECK(repository.insertAnalysisSnapshot(clear.value(), 3).ok());
    ROCLINT_CHECK(repository.restoreSession(SessionId(triage.sessionId)).value().analysis == clear.value());
    ROCLINT_CHECK(db.execute("UPDATE model_attempts SET model_detail = 'tampered';").ok());
    ROCLINT_CHECK(!repository.restoreSession(SessionId(triage.sessionId)).ok());
}
an::SnapshotEvaluationResponse scored(const an::NormalizedAnalysis& normalized, double second = 0.8) {
    an::SnapshotEvaluationResponse result;
    result.success = true;
    result.analysisSnapshotId = normalized.id;
    result.engine = an::ScorerEngine::JuliaLocal;
    result.executedEngine = result.engine;
    result.executionStatus = an::ExecutionStatus::ExecutedModel;
    result.modelInferenceExecuted = true;
    result.modelVersion = "SupersonicLabs/Julia-1@a85b127321d580d65176c89ced8273f305745d85";
    result.totalLatencyMs = 0.123456789012345;
    for (const auto& group : normalized.choiceGroups) {
        an::GroupEvaluationScore score;
        score.groupId = group.id;
        score.isScored = true;
        score.ambiguityScore = 0.75;
        score.winningAlternativeId = group.alternatives[1].id;
        score.probabilities = std::vector<an::AlternativeProbability>{{group.alternatives[0].id, 1 - second},
                                                                    {group.alternatives[1].id, second}};
        result.groupScores.push_back(score);
    }
    return result;
}

an::AnalysisSnapshot snapshot(const an::NormalizedAnalysis& normalized, double second = 0.8) {
    auto result = an::buildAnalysisSnapshot(normalized, scored(normalized, second), fx::kChainPrompt, "discovery_1", "julia_" + std::to_string(second));
    ROCLINT_CHECK(result.ok());
    return std::move(result).value();
}

void testFinalIdentityCoversScoringAndOrdering() {
    const auto normalized = fx::chainAnalysis("sess_a", "rev_sess_a_1");
    const auto final = snapshot(normalized);
    ROCLINT_CHECK(final.id != normalized.id && final.snapshotHash != normalized.snapshotHash);
    ROCLINT_CHECK(an::isSnapshotSealed(final));
    ROCLINT_CHECK(final.presentation.at(0).alternativeIds.at(0) == "alt_0_1");
    ROCLINT_CHECK(snapshot(normalized, 0.7).id != final.id);
    const auto differentPrompt = an::buildAnalysisSnapshot(normalized, scored(normalized), fx::kChainPrompt + " Additional instruction.", "discovery_1", "julia_0.800000");
    ROCLINT_CHECK(differentPrompt.ok() && differentPrompt.value().id != final.id);
    const auto roundtrip = storage::decodeAnalysisSnapshot(storage::encodeAnalysisSnapshot(final));
    ROCLINT_CHECK(roundtrip.ok() && roundtrip.value() == final);
    auto changed = final;
    changed.evaluation.groupScores[0].probabilities->at(0).probability += 0.1;
    ROCLINT_CHECK(!an::validateAnalysisSnapshot(changed).ok());
    changed = final;
    changed.scorerAttemptId = "different_attempt";
    ROCLINT_CHECK(!an::isSnapshotSealed(changed));
    changed = final;
    std::reverse(changed.presentation[0].alternativeIds.begin(), changed.presentation[0].alternativeIds.end());
    changed.snapshotHash = an::computeSnapshotHash(changed);
    changed.id = "analysis_" + changed.snapshotHash;
    ROCLINT_CHECK(!an::validateAnalysisSnapshot(changed).ok());
}

void testRejectsInvalidEvidenceRatherThanSanitizingHistory() {
    const auto normalized = fx::chainAnalysis("sess_a", "rev_sess_a_1");
    auto evaluation = scored(normalized);
    evaluation.groupScores[0].probabilities->at(0).probability = std::numeric_limits<double>::quiet_NaN();
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(normalized, evaluation, fx::kChainPrompt, "d", "s").ok());
    evaluation = scored(normalized);
    evaluation.analysisSnapshotId = "foreign";
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(normalized, evaluation, fx::kChainPrompt, "d", "s").ok());
    evaluation = scored(normalized);
    evaluation.groupScores.push_back(evaluation.groupScores[0]);
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(normalized, evaluation, fx::kChainPrompt, "d", "s").ok());
    evaluation = scored(normalized);
    evaluation.groupScores[0].winningAlternativeId = "alt_0_0";
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(normalized, evaluation, fx::kChainPrompt, "d", "s").ok());
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(normalized, scored(normalized), fx::kChainPrompt, "d", "").ok());
    auto degraded = bridge::S1TriageAdapter::degradedResponse(normalized, "Julia unavailable");
    auto unranked = an::buildAnalysisSnapshot(normalized, degraded, fx::kChainPrompt, "d");
    ROCLINT_CHECK(unranked.ok());
    ROCLINT_CHECK(unranked.value().presentation[0].alternativeIds[0] == "alt_0_0");
    ROCLINT_CHECK(storage::decodeAnalysisSnapshot(storage::encodeAnalysisSnapshot(unranked.value())).value() == unranked.value());
    degraded.groupScores[0].ambiguityScore = 0.9;
    ROCLINT_CHECK(!an::buildAnalysisSnapshot(normalized, degraded, fx::kChainPrompt, "d").ok());
    auto unknownSchema = storage::encodeAnalysisSnapshot(unranked.value());
    const auto pos = unknownSchema.find("analysis_snapshot.v3");
    ROCLINT_CHECK(pos != std::string::npos);
    unknownSchema.replace(pos, 20, "analysis_snapshot.v9");
    ROCLINT_CHECK(!storage::decodeAnalysisSnapshot(unknownSchema).ok());
}

void testChangedScoresInvalidateIntentAndApproval() {
    fx::Chain chain;
    const auto previousIntent = chain.intent;
    chain.snapshot = snapshot(chain.pipe.analysis);
    ROCLINT_CHECK(!intent::IntentProvenanceBuilder::isIntentValid(previousIntent, chain.revision, RevisionStatus::Clean, chain.snapshot).ok());
    chain.intent = intent::IntentProvenanceBuilder::buildConfirmedIntent(chain.snapshot, chain.pipe.policy, chain.pipe.state, 1).value();
    bridge::GraniteRewriteRequest request;
    request.sessionId = chain.session;
    request.revisionId = chain.revision;
    request.originalText = fx::kChainPrompt;
    request.analysis = chain.snapshot;
    request.confirmedIntent = chain.intent;
    const auto profile = rewrite::ProfileRegistry::getProfile("GENERIC@1");
    chain.proposal = bridge::GraniteRewriteAdapter::parseRewriteResponse(fx::rewriteReply(chain.proposal.rewrittenText), request, *profile, fx::testModel(), 2).value();
    auto alteredPrompt = request;
    alteredPrompt.originalText += " Additional instruction.";
    ROCLINT_CHECK(!bridge::GraniteRewriteAdapter::parseRewriteResponse(fx::rewriteReply(chain.proposal.rewrittenText), alteredPrompt, *profile, fx::testModel(), 2).ok());
    const auto approval = chain.approve(1);
    ROCLINT_CHECK(review::ApprovalFactory::isApprovalValid(approval, chain.context(1)));
    const auto next = snapshot(chain.pipe.analysis, 0.7);
    auto context = chain.context(1);
    context.analysis = &next;
    ROCLINT_CHECK(!review::ApprovalFactory::isApprovalValid(approval, context));

    const auto path = std::filesystem::temp_directory_path() /
        ("roclint-final-analysis-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
    {
        storage::Database db(path);
        storage::PromptRepository repository(db);
        ROCLINT_CHECK(repository.migrate().ok());
        PromptRevision root(SessionId(chain.session), RevisionId(chain.revision), std::nullopt, fx::kChainPrompt, 1);
        ROCLINT_CHECK(repository.createSessionWithRootRevision(root, 1).ok());
        ROCLINT_CHECK(!repository.insertAnalysisSnapshot(chain.snapshot, 2).ok());
        fx::recordSnapshotAttempts(repository, chain.snapshot);
        ROCLINT_CHECK(repository.insertAnalysisSnapshot(chain.snapshot, 2).ok());
        ROCLINT_CHECK(repository.insertIntentVersion(chain.intent, 3).ok());
        ROCLINT_CHECK(repository.insertProposal(chain.proposal, 4).ok());
        ROCLINT_CHECK(repository.insertApproval(approval).ok());
    }
    {
        storage::Database db(path);
        storage::PromptRepository repository(db);
        ROCLINT_CHECK(repository.migrate().ok());
        const auto restored = repository.restoreSession(SessionId(chain.session)).value();
        ROCLINT_CHECK(restored.analysis && *restored.analysis == chain.snapshot);
        ROCLINT_CHECK(restored.intent && *restored.intent == chain.intent);
        ROCLINT_CHECK(restored.proposal && *restored.proposal == chain.proposal);
        ROCLINT_CHECK(restored.approval && *restored.approval == approval);
        ROCLINT_CHECK(!restored.legacyAnalysisRequiresReview);
        auto restoredContext = chain.context(1);
        restoredContext.analysis = &*restored.analysis;
        restoredContext.intent = &*restored.intent;
        restoredContext.proposal = &*restored.proposal;
        ROCLINT_CHECK(review::ApprovalFactory::isApprovalValid(*restored.approval, restoredContext));
        fx::recordSnapshotAttempts(repository, next);
        ROCLINT_CHECK(repository.insertAnalysisSnapshot(next, 5).ok());
        ROCLINT_CHECK(!repository.getCurrentApproval(SessionId(chain.session)).value());
        ROCLINT_CHECK(repository.insertIntentVersion(chain.intent, 6).ok()); // Old intent remains valid historical evidence.
        ROCLINT_CHECK(db.execute("UPDATE analysis_snapshots SET snapshot_hash = 'tampered' WHERE format_version = 2;").ok());
        ROCLINT_CHECK(!repository.restoreSession(SessionId(chain.session)).ok());
    }
    std::filesystem::remove(path);
}

void testLegacyHistoryIsRetainedWithoutReusingApproval() {
    storage::Database db(":memory:");
    storage::PromptRepository repository(db);
    ROCLINT_CHECK(repository.migrateTo(4).ok());
    fx::Chain chain;
    PromptRevision root(SessionId(chain.session), RevisionId(chain.revision), std::nullopt, fx::kChainPrompt, 1);
    ROCLINT_CHECK(repository.createSessionWithRootRevision(root, 1).ok());
    const auto normalizedPayload = storage::encodeAnalysis(chain.pipe.analysis);
    auto legacyInsert = [&](const char* sql, const std::vector<std::string>& fields) {
        sqlite3_stmt* statement = nullptr;
        ROCLINT_CHECK(sqlite3_prepare_v2(db.rawHandle(), sql, -1, &statement, nullptr) == SQLITE_OK);
        for (std::size_t index = 0; index < fields.size(); ++index) ROCLINT_CHECK(sqlite3_bind_text(statement, static_cast<int>(index + 1), fields[index].c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        ROCLINT_CHECK(sqlite3_step(statement) == SQLITE_DONE);
        ROCLINT_CHECK(sqlite3_finalize(statement) == SQLITE_OK);
    };
    legacyInsert("INSERT INTO analysis_snapshots VALUES (?1, ?2, ?3, ?4, ?5, 2);",
                 {chain.pipe.analysis.id, chain.session, chain.revision, chain.pipe.analysis.snapshotHash, normalizedPayload});
    auto legacyIntent = chain.intent;
    legacyIntent.snapshotId = chain.pipe.analysis.id;
    legacyIntent.snapshotHash = chain.pipe.analysis.snapshotHash;
    legacyIntent.intentHash = intent::IntentProvenanceBuilder::computeIntentHash(legacyIntent);
    legacyIntent.id = "intent_" + legacyIntent.intentHash;
    legacyInsert("INSERT INTO intent_versions VALUES (?1, ?2, ?3, ?4, ?5, ?6, 3);",
                 {legacyIntent.id, chain.session, chain.revision, legacyIntent.snapshotId, legacyIntent.intentHash, storage::encodeIntent(legacyIntent)});
    auto legacyProposal = chain.proposal;
    legacyProposal.intentId = legacyIntent.id;
    legacyProposal.intentHash = legacyIntent.intentHash;
    legacyProposal.snapshotId = legacyIntent.snapshotId;
    legacyProposal.snapshotHash = legacyIntent.snapshotHash;
    rewrite::ProposalHasher::seal(legacyProposal);
    legacyInsert("INSERT INTO proposals VALUES (?1, ?2, ?3, ?4, ?5, 0, ?6, 4);",
                 {legacyProposal.id, chain.session, chain.revision, legacyIntent.id, legacyProposal.proposalHash, storage::encodeProposal(legacyProposal)});
    auto approval = chain.approve(1);
    approval.snapshotId = chain.pipe.analysis.id;
    approval.snapshotHash = chain.pipe.analysis.snapshotHash;
    approval.intentId = legacyIntent.id;
    approval.intentHash = legacyIntent.intentHash;
    approval.proposalId = legacyProposal.id;
    approval.proposalHash = legacyProposal.proposalHash;
    approval.bindingHash = approval.computeBindingHash();
    approval.id = "appr_" + approval.bindingHash;
    // Preserve the complete historical chain with its original pre-score binding.
    ROCLINT_CHECK(repository.insertApproval(approval).ok());
    ROCLINT_CHECK(repository.migrate().ok());
    const auto restored = repository.restoreSession(SessionId(chain.session)).value();
    ROCLINT_CHECK(restored.legacyAnalysisRequiresReview && !restored.analysis && !restored.intent && !restored.proposal && !restored.approval);
    ROCLINT_CHECK(repository.getHistory(SessionId(chain.session)).value().size() == 1);
    ROCLINT_CHECK(db.queryText("SELECT COUNT(*) FROM analysis_snapshots;").value() == "1");
    const auto history = repository.getApprovalHistory(SessionId(chain.session)).value();
    ROCLINT_CHECK(history.size() == 1 && !history[0].isCurrent && history[0].approvedText == approval.approvedText);
}
} // namespace

int main() {
    testS1FirstArtifact();
    testFinalIdentityCoversScoringAndOrdering();
    testRejectsInvalidEvidenceRatherThanSanitizingHistory();
    testChangedScoresInvalidateIntentAndApproval();
    testLegacyHistoryIsRetainedWithoutReusingApproval();
    std::cout << "Final scored analysis, bindings, restore and legacy migration passed.\n";
}
