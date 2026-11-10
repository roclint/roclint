#include "roclint/storage/Database.hpp"
#include "roclint/storage/PayloadCodec.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "support/check.hpp"
#include "support/storage_fixtures.hpp"

#include <filesystem>
#include <iostream>
#include <memory>

using namespace roclint::core;
using namespace roclint::storage;
namespace fs = std::filesystem;

namespace {

struct Store {
    Database db;
    std::unique_ptr<PromptRepository> repo;

    explicit Store(const fs::path& path = ":memory:") {
        ROCLINT_CHECK(db.open(path).ok());
        repo = std::make_unique<PromptRepository>(db);
        ROCLINT_CHECK(repo->migrate().ok());
    }
};

PromptRevision root(const std::string& session, const std::string& text = fx::kChainPrompt) {
    const SessionId sid(session);
    return PromptRevision(sid, RevisionState::makeRevisionId(sid, 1), std::nullopt, text, 100);
}

// Persists session root, analysis, intent, proposal, and approval for the chain.
review::ApprovalRecord seed(PromptRepository& repo, const fx::Chain& c) {
    ROCLINT_CHECK(repo.createSessionWithRootRevision(root(c.session), 100).ok());
    fx::recordSnapshotAttempts(repo, c.snapshot);
    ROCLINT_CHECK(repo.insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(repo.insertIntentVersion(c.intent, 300).ok());
    ROCLINT_CHECK(repo.insertProposal(c.proposal, 400).ok());
    auto approval = c.approve(1);
    ROCLINT_CHECK(repo.insertApproval(approval).ok());
    return approval;
}

RevisionWrite nextWrite(const std::string& session, std::uint64_t seq, std::uint64_t expectedVersion,
                        const std::string& parent, const std::string& text = "next") {
    const SessionId sid(session);
    RevisionWrite w{PromptRevision(sid, RevisionState::makeRevisionId(sid, seq), RevisionId(parent), text, 500)};
    w.sequence = seq;
    w.expectedStateVersion = expectedVersion;
    w.newStateVersion = expectedVersion + 1;
    w.nowMs = 500;
    return w;
}

void testRootRevisionsInTwoSessionsDoNotCollide() {
    Store s;
    ROCLINT_CHECK(s.repo->createSessionWithRootRevision(root("sess_a"), 1).ok());
    ROCLINT_CHECK(s.repo->createSessionWithRootRevision(root("sess_b"), 2).ok());
    ROCLINT_CHECK(s.repo->getLatestRevision(SessionId("sess_a")).value()->revision.revisionId().value() ==
                  "rev_sess_a_1");
    ROCLINT_CHECK(s.repo->getLatestRevision(SessionId("sess_b")).value()->revision.revisionId().value() ==
                  "rev_sess_b_1");
    ROCLINT_CHECK(s.repo->listSessions().value().size() == 2);

    // A second root for an existing session, or a misnamed root, is refused.
    ROCLINT_CHECK(!s.repo->createSessionWithRootRevision(root("sess_a"), 3).ok());
    const SessionId sid("sess_c");
    ROCLINT_CHECK(!s.repo->createSessionWithRootRevision(PromptRevision(sid, RevisionId("rev_1"), std::nullopt, "x"), 1)
                       .ok());
}

void testRevisionWritesUseOptimisticConcurrency() {
    Store s;
    ROCLINT_CHECK(s.repo->createSessionWithRootRevision(root("sess_a"), 1).ok());
    ROCLINT_CHECK(s.repo->insertRevision(nextWrite("sess_a", 2, 1, "rev_sess_a_1")).ok());

    auto stale = s.repo->insertRevision(nextWrite("sess_a", 3, 1, "rev_sess_a_2"));
    ROCLINT_CHECK(!stale.ok() && stale.error().code == ErrorCode::StaleVersion);

    auto skipped = s.repo->insertRevision(nextWrite("sess_a", 4, 2, "rev_sess_a_2"));
    ROCLINT_CHECK(!skipped.ok());

    ROCLINT_CHECK(s.repo->insertDraftRevision(nextWrite("sess_a", 3, 2, "rev_sess_a_2", "frozen")).ok());
    ROCLINT_CHECK(s.repo->insertRestoredRevision(nextWrite("sess_a", 4, 3, "rev_sess_a_1", "restored")).ok());

    const auto history = s.repo->getHistory(SessionId("sess_a")).value();
    ROCLINT_CHECK(history.size() == 4);
    ROCLINT_CHECK(history[1].origin == RevisionOrigin::Commit);
    ROCLINT_CHECK(history[2].origin == RevisionOrigin::DraftFreeze);
    ROCLINT_CHECK(history[3].origin == RevisionOrigin::Restore);
    ROCLINT_CHECK(history[3].revision.parentRevisionId()->value() == "rev_sess_a_1");
    const auto session = s.repo->getSession(SessionId("sess_a")).value();
    ROCLINT_CHECK(session.stateVersion == 4);
    ROCLINT_CHECK(session.highestRevisionSeq == 4);

    auto missing = s.repo->insertRevision(nextWrite("sess_zzz", 2, 1, "rev_sess_zzz_1"));
    ROCLINT_CHECK(!missing.ok() && missing.error().code == ErrorCode::NotFound);
}

void testCrossSessionReferencesAreRejected(PromptRepository& repo) {
    fx::Chain a("sess_a");
    fx::Chain b("sess_b");
    const auto approvalA = seed(repo, a);
    seed(repo, b);

    // Parent revision from another session.
    auto parent = repo.insertRevision(nextWrite("sess_b", 2, 1, "rev_sess_a_1"));
    ROCLINT_CHECK(!parent.ok());
    ROCLINT_CHECK(repo.getSession(SessionId("sess_b")).value().stateVersion == 1); // rolled back

    // Analysis claiming session B but bound to session A's revision.
    auto analysis = a.pipe.analysis;
    analysis.sessionId = "sess_b";
    analysis::sealSnapshot(analysis);
    ROCLINT_CHECK(!repo.insertAnalysisSnapshot(fx::finalSnapshot(analysis, fx::kChainPrompt), 1).ok());

    // Intent for session B pointing at session A's revision and snapshot.
    auto intent = a.intent;
    intent.sessionId = "sess_b";
    intent.intentHash = intent::IntentProvenanceBuilder::computeIntentHash(intent);
    intent.id = "intent_" + intent.intentHash;
    ROCLINT_CHECK(!repo.insertIntentVersion(intent, 1).ok());

    // Intent in session B whose snapshot belongs to session A.
    auto intentB = b.intent;
    intentB.snapshotId = a.snapshot.id;
    intentB.intentHash = intent::IntentProvenanceBuilder::computeIntentHash(intentB);
    intentB.id = "intent_" + intentB.intentHash;
    ROCLINT_CHECK(!repo.insertIntentVersion(intentB, 1).ok());

    // Proposal in session B bound to session A's intent.
    auto proposal = b.proposal;
    proposal.intentId = a.intent.id;
    rewrite::ProposalHasher::seal(proposal);
    ROCLINT_CHECK(!repo.insertProposal(proposal, 1).ok());

    // Approval in session B for session A's proposal.
    auto approval = approvalA;
    approval.sessionId = "sess_b";
    approval.revisionId = b.revision;
    approval.bindingHash = approval.computeBindingHash();
    approval.id = "appr_" + approval.bindingHash;
    ROCLINT_CHECK(!repo.insertApproval(approval).ok());

    // Model attempt in session B against session A's revision.
    ModelAttemptRecord attempt{"att_x", "sess_b", "rev_sess_a_1", "discovery"};
    attempt.transport = roclint::bridge::kLoopbackOllamaTransport;
    ROCLINT_CHECK(!repo.beginModelAttempt(attempt).ok());

    // Tampered payloads are refused before SQL.
    auto unsealedIntent = a.intent;
    unsealedIntent.items.clear();
    ROCLINT_CHECK(!repo.insertIntentVersion(unsealedIntent, 1).ok());
    auto unsealedProposal = a.proposal;
    unsealedProposal.rewrittenText = "changed";
    ROCLINT_CHECK(!repo.insertProposal(unsealedProposal, 1).ok());
    auto unsealedApproval = approvalA;
    unsealedApproval.approvedText = "changed";
    ROCLINT_CHECK(!repo.insertApproval(unsealedApproval).ok());
}

void testCrossSessionRejectionInMemory() {
    Store s;
    testCrossSessionReferencesAreRejected(*s.repo);
}

void testForeignKeysHoldAfterReopenOnNonAsciiPath() {
    const fs::path dir = fs::temp_directory_path() / fs::path(u8"roclint_repo_\u00fc\u00df_\u4e2d\u6587");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path path = dir / "history.db";
    {
        Store s(path);
        ROCLINT_CHECK(s.repo->createSessionWithRootRevision(root("sess_a"), 1).ok());
    }
    {
        Store reopened(path);
        ROCLINT_CHECK(reopened.db.queryText("PRAGMA foreign_keys;").value() == "1");
        ROCLINT_CHECK(reopened.repo->sessionExists(SessionId("sess_a")).value());
        ROCLINT_CHECK(reopened.repo->createSessionWithRootRevision(root("sess_b"), 2).ok());
        ROCLINT_CHECK(!reopened.repo->insertRevision(nextWrite("sess_b", 2, 1, "rev_sess_a_1")).ok());
    }
    {
        Store again(path);
        ROCLINT_CHECK(again.repo->purgeHistory().ok());
        testCrossSessionReferencesAreRejected(*again.repo);
    }
    fs::remove_all(dir, ec);
}

void testFailedApprovalKeepsPreviousCurrent() {
    Store s;
    fx::Chain c("sess_a");
    const auto first = seed(*s.repo, c);
    ROCLINT_CHECK(s.repo->getCurrentApproval(SessionId("sess_a")).value()->id == first.id);

    // A new approval whose proposal doesn't exist violates an FK; the transaction rolls back.
    auto broken = first;
    broken.proposalId = "prop_missing";
    broken.stateVersion = 7;
    broken.bindingHash = broken.computeBindingHash();
    broken.id = "appr_" + broken.bindingHash;
    auto st = s.repo->insertApproval(broken);
    ROCLINT_CHECK(!st.ok());

    const auto current = s.repo->getCurrentApproval(SessionId("sess_a")).value();
    ROCLINT_CHECK(current.has_value());
    ROCLINT_CHECK(current->id == first.id);
    ROCLINT_CHECK(current->isCurrent);
    ROCLINT_CHECK(s.repo->getApprovalHistory(SessionId("sess_a")).value().size() == 1);

    // A valid second approval supersedes the first.
    const auto second = c.approve(2);
    ROCLINT_CHECK(s.repo->insertApproval(second).ok());
    ROCLINT_CHECK(s.repo->getCurrentApproval(SessionId("sess_a")).value()->id == second.id);
    const auto history = s.repo->getApprovalHistory(SessionId("sess_a")).value();
    ROCLINT_CHECK(history.size() == 2);
    ROCLINT_CHECK(!history[0].isCurrent);

    ROCLINT_CHECK(s.repo->invalidateApprovals(SessionId("sess_a")).ok());
    ROCLINT_CHECK(!s.repo->getCurrentApproval(SessionId("sess_a")).value().has_value());
}

void testRestoreReturnsBoundArtifactsOnly() {
    Store s;
    fx::Chain c("sess_a");
    const auto approval = seed(*s.repo, c);

    auto restored = s.repo->restoreSession(SessionId("sess_a"));
    ROCLINT_CHECK(restored.ok());
    const auto& r = restored.value();
    ROCLINT_CHECK(r.latestRevision.revision.revisionId().value() == c.revision);
    ROCLINT_CHECK(r.analysis.has_value() && *r.analysis == c.snapshot);
    ROCLINT_CHECK(r.intent.has_value() && r.intent->intentHash == c.intent.intentHash);
    ROCLINT_CHECK(r.intent->items == c.intent.items);
    ROCLINT_CHECK(r.proposal.has_value() && r.proposal->proposalHash == c.proposal.proposalHash);
    ROCLINT_CHECK(r.proposal->constraints == c.proposal.constraints);
    ROCLINT_CHECK(r.approval.has_value() && *r.approval == approval);

    // After a new revision, nothing downstream is restored for it.
    ROCLINT_CHECK(s.repo->insertRevision(nextWrite("sess_a", 2, 1, c.revision, "edited")).ok());
    auto after = s.repo->restoreSession(SessionId("sess_a")).value();
    ROCLINT_CHECK(after.latestRevision.revision.text() == "edited");
    ROCLINT_CHECK(!after.analysis.has_value());
    ROCLINT_CHECK(!after.intent.has_value());
    ROCLINT_CHECK(!after.proposal.has_value());
    ROCLINT_CHECK(!after.approval.has_value());

    ROCLINT_CHECK(!s.repo->restoreSession(SessionId("sess_missing")).ok());
}

void testModelAttemptsAndOrphanRecovery() {
    Store s;
    ROCLINT_CHECK(s.repo->createSessionWithRootRevision(root("sess_a"), 1).ok());
    auto attempt = [](const char* id, const char* stage) {
        ModelAttemptRecord a{id, "sess_a", "rev_sess_a_1", stage};
        a.transport = roclint::bridge::kLoopbackOllamaTransport;
        a.startedAtMs = 10;
        return a;
    };
    ModelAttemptRecord noTransport{"att_0", "sess_a", "rev_sess_a_1", "discovery"};
    ROCLINT_CHECK(!s.repo->beginModelAttempt(noTransport).ok());

    ROCLINT_CHECK(s.repo->beginModelAttempt(attempt("att_1", "discovery")).ok());
    ROCLINT_CHECK(s.repo->beginModelAttempt(attempt("att_2", "rewrite")).ok());
    ROCLINT_CHECK(s.repo->beginModelAttempt(attempt("att_3", "discovery")).ok());
    ROCLINT_CHECK(s.repo
                      ->completeModelAttempt("att_1", AttemptStatus::Succeeded, "granite",
                                             "digest=sha256:aa; quantization=Q8_0", "", 20)
                      .ok());
    ROCLINT_CHECK(s.repo->completeModelAttempt("att_3", AttemptStatus::DiscardedStale, "granite", "", "", 21).ok());
    ROCLINT_CHECK(!s.repo->completeModelAttempt("att_1", AttemptStatus::Failed, "", "", "again", 22).ok());
    ROCLINT_CHECK(!s.repo->completeModelAttempt("att_2", AttemptStatus::InProgress, "", "", "", 22).ok());

    auto interrupted = s.repo->interruptOrphanedAttemptsOnStartup(99);
    ROCLINT_CHECK(interrupted.ok() && interrupted.value() == 1);
    ROCLINT_CHECK(s.repo->interruptOrphanedAttemptsOnStartup(100).value() == 0);

    const auto all = s.repo->getModelAttempts(SessionId("sess_a")).value();
    ROCLINT_CHECK(all.size() == 3);
    ROCLINT_CHECK(all[0].status == AttemptStatus::Succeeded);
    ROCLINT_CHECK(all[0].modelDetail.find("quantization=Q8_0") != std::string::npos);
    ROCLINT_CHECK(all[0].transport == std::string(roclint::bridge::kLoopbackOllamaTransport));
    ROCLINT_CHECK(all[1].status == AttemptStatus::Interrupted);
    ROCLINT_CHECK(all[1].completedAtMs.has_value() && *all[1].completedAtMs == 99);
    ROCLINT_CHECK(all[2].status == AttemptStatus::DiscardedStale);
}

void testProposalGenerationIsPersistedMonotonically() {
    Store s;
    ROCLINT_CHECK(s.repo->createSessionWithRootRevision(root("sess_a"), 1).ok());
    ROCLINT_CHECK(s.repo->recordProposalGeneration(SessionId("sess_a"), 3, 10).ok());
    ROCLINT_CHECK(s.repo->getSession(SessionId("sess_a")).value().proposalEditGeneration == 3);
    ROCLINT_CHECK(s.repo->recordProposalGeneration(SessionId("sess_a"), 2, 11).ok());
    ROCLINT_CHECK(s.repo->getSession(SessionId("sess_a")).value().proposalEditGeneration == 3);
    auto missing = s.repo->recordProposalGeneration(SessionId("sess_none"), 1, 12);
    ROCLINT_CHECK(!missing.ok() && missing.error().code == ErrorCode::NotFound);
}

void testDeleteAndPurge() {
    Store s;
    fx::Chain a("sess_a");
    fx::Chain b("sess_b");
    seed(*s.repo, a);
    seed(*s.repo, b);
    ROCLINT_CHECK(s.repo->deleteSession(SessionId("sess_a")).ok());
    ROCLINT_CHECK(!s.repo->sessionExists(SessionId("sess_a")).value());
    ROCLINT_CHECK(s.repo->getHistory(SessionId("sess_a")).value().empty());
    ROCLINT_CHECK(s.repo->getApprovalHistory(SessionId("sess_a")).value().empty());
    ROCLINT_CHECK(s.repo->getCurrentApproval(SessionId("sess_b")).value().has_value());
    auto missing = s.repo->deleteSession(SessionId("sess_a"));
    ROCLINT_CHECK(!missing.ok() && missing.error().code == ErrorCode::NotFound);

    ROCLINT_CHECK(s.repo->purgeHistory().ok());
    ROCLINT_CHECK(s.repo->listSessions().value().empty());
    ROCLINT_CHECK(s.db.queryText("SELECT COUNT(*) FROM approvals;").value() == "0");
    ROCLINT_CHECK(s.db.queryText("SELECT COUNT(*) FROM analysis_snapshots;").value() == "0");
}

void testStateCompareAndSwapAndArtifactFailuresRollBackTogether() {
    Store store;
    fx::Chain chain;
    const auto approval = seed(*store.repo, chain);
    const SessionId session(chain.session);
    auto next = RevisionState(root(chain.session));
    ROCLINT_CHECK(next.noteDraftEdit(1).ok());
    ROCLINT_CHECK(store.repo->advanceSessionState(next, 1, 500).ok());
    ROCLINT_CHECK(!store.repo->getCurrentApproval(session).value());
    const auto before = store.repo->getSession(session).value();
    ROCLINT_CHECK(!store.repo->advanceSessionState(next, 1, 501).ok());
    ROCLINT_CHECK(store.repo->getSession(session).value() == before);

    Store rollback;
    seed(*rollback.repo, chain);
    const auto original = rollback.repo->getSession(session).value();
    auto bad = chain.proposal;
    bad.intentId = "not_a_stored_intent";
    bad.proposalEditGeneration = 1;
    rewrite::ProposalHasher::seal(bad);
    ROCLINT_CHECK(!rollback.repo->insertProposal(bad, 600, 1).ok());
    ROCLINT_CHECK(rollback.repo->getSession(session).value() == original);
    ROCLINT_CHECK(rollback.repo->getCurrentApproval(session).value()->id == approval.id);
    auto badApproval = chain.approve(2);
    badApproval.proposalId = "not_a_stored_proposal";
    badApproval.bindingHash = badApproval.computeBindingHash();
    badApproval.id = "appr_" + badApproval.bindingHash;
    ROCLINT_CHECK(!rollback.repo->insertApproval(badApproval, 1).ok());
    ROCLINT_CHECK(rollback.repo->getSession(session).value() == original);
    ROCLINT_CHECK(rollback.repo->getCurrentApproval(session).value()->id == approval.id);
    ROCLINT_CHECK(rollback.db.execute("CREATE TEMP TRIGGER reject_intent BEFORE INSERT ON intent_versions BEGIN SELECT RAISE(ABORT, 'test failure'); END;").ok());
    ROCLINT_CHECK(!rollback.repo->insertIntentVersion(chain.intent, 650, 1).ok());
    ROCLINT_CHECK(rollback.repo->getSession(session).value() == original);
    ROCLINT_CHECK(rollback.repo->getCurrentApproval(session).value()->id == approval.id);
    ROCLINT_CHECK(rollback.db.execute("CREATE TEMP TRIGGER reject_snapshot BEFORE INSERT ON analysis_snapshots BEGIN SELECT RAISE(ABORT, 'test failure'); END;").ok());
    ROCLINT_CHECK(!rollback.repo->insertAnalysisSnapshot(chain.snapshot, 700, 1).ok());
    ROCLINT_CHECK(rollback.repo->getSession(session).value() == original);
    ROCLINT_CHECK(rollback.repo->getCurrentApproval(session).value()->id == approval.id);
}

void testCoalescedDraftRangeIsAtomic() {
    Store store; fx::Chain chain; seed(*store.repo, chain);
    const SessionId session(chain.session);
    auto draft = RevisionState(root(chain.session));
    for (int i = 0; i < 8; ++i) ROCLINT_CHECK(draft.noteDraftEdit(draft.stateVersion()).ok());
    ROCLINT_CHECK(store.repo->advanceDraftStateRange(draft, 1, 500).ok());
    const auto stored = store.repo->getSession(session).value();
    ROCLINT_CHECK(stored.stateVersion == 9 && stored.draftEditGeneration == 8);
    ROCLINT_CHECK(store.repo->getHistory(session).value().size() == 1);
    ROCLINT_CHECK(!store.repo->getCurrentApproval(session).value());
    ROCLINT_CHECK(!store.repo->advanceDraftStateRange(draft, 1, 501).ok());
    ROCLINT_CHECK(store.repo->getSession(session).value() == stored);
    auto wrongGeneration = RevisionState(root(chain.session), 1, 9, 8, 1);
    ROCLINT_CHECK(wrongGeneration.noteDraftEdit(9).ok());
    ROCLINT_CHECK(!store.repo->advanceDraftStateRange(wrongGeneration, 9, 502).ok());
    ROCLINT_CHECK(store.repo->getSession(session).value() == stored);
    auto wrongRevision = RevisionState(PromptRevision(session, RevisionId("rev_other"), std::nullopt, "other", 100), 1, 9, 8, 0);
    ROCLINT_CHECK(wrongRevision.noteDraftEdit(9).ok());
    ROCLINT_CHECK(!store.repo->advanceDraftStateRange(wrongRevision, 9, 503).ok());
    ROCLINT_CHECK(store.repo->getSession(session).value() == stored);
}

void testAstSnapshotHashPayloadRoundTrip() {
    fx::Chain chain;
    auto proposal = chain.proposal;
    proposal.astSnapshotHash = std::string(64, 'd');
    rewrite::ProposalHasher::seal(proposal);
    const auto decoded = decodeProposal(encodeProposal(proposal));
    ROCLINT_CHECK(decoded.ok());
    ROCLINT_CHECK(decoded.value().astSnapshotHash == proposal.astSnapshotHash);
    ROCLINT_CHECK(decoded.value().proposalHash == proposal.proposalHash);
    ROCLINT_CHECK(decoded.value().id == proposal.id);

    auto legacy = chain.proposal;
    ROCLINT_CHECK(legacy.astSnapshotHash.empty());
    const auto legacyDecoded = decodeProposal(encodeProposal(legacy));
    ROCLINT_CHECK(legacyDecoded.ok());
    ROCLINT_CHECK(legacyDecoded.value().astSnapshotHash.empty());
    ROCLINT_CHECK(legacyDecoded.value().proposalHash == legacy.proposalHash);
}

void testPayloadCodecCustomProposalIdAndValidation() {
    fx::Chain chain;
    auto proposal = chain.proposal;
    proposal.id = "prop_custom_payload_id_42";
    rewrite::ProposalHasher::seal(proposal);
    const auto encoded = encodeProposal(proposal);
    const auto decoded = decodeProposal(encoded);
    ROCLINT_CHECK(decoded.ok());
    ROCLINT_CHECK(decoded.value().id == "prop_custom_payload_id_42");
    ROCLINT_CHECK(decoded.value().proposalHash == proposal.proposalHash);

    // Tamper with proposal ID in JSON: missing prop_ prefix
    auto badPrefixJson = encoded;
    const std::string findId = "\"id\":\"prop_custom_payload_id_42\"";
    const std::string badId = "\"id\":\"unprefixed_payload_id_42\"";
    const auto pos = badPrefixJson.find(findId);
    ROCLINT_CHECK(pos != std::string::npos);
    badPrefixJson.replace(pos, findId.size(), badId);
    ROCLINT_CHECK(!decodeProposal(badPrefixJson).ok());

    // Tamper with proposal ID in JSON: empty id
    auto emptyIdJson = encoded;
    const std::string emptyId = "\"id\":\"\"";
    emptyIdJson.replace(pos, findId.size(), emptyId);
    ROCLINT_CHECK(!decodeProposal(emptyIdJson).ok());

    // Tamper with proposal hash in JSON
    auto badHashJson = encoded;
    const std::string findHash = "\"proposal_hash\":\"" + proposal.proposalHash + "\"";
    const std::string badHash = "\"proposal_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"";
    const auto hashPos = badHashJson.find(findHash);
    ROCLINT_CHECK(hashPos != std::string::npos);
    badHashJson.replace(hashPos, findHash.size(), badHash);
    ROCLINT_CHECK(!decodeProposal(badHashJson).ok());
}

void testReselectionWithDistinctIdsInsertsSeparateRowsAndRestoresLatest() {
    Store store;
    fx::Chain c("sess_reselect");
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root("sess_reselect"), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    // Selection 1: Candidate A
    auto propA1 = c.proposal;
    propA1.id = "prop_candA_sel1";
    rewrite::ProposalHasher::seal(propA1);
    ROCLINT_CHECK(store.repo->insertProposal(propA1, 400).ok());

    // Selection 2: Candidate B (different text)
    auto propB = c.proposal;
    propB.rewrittenText = "Candidate B distinct text.\n";
    propB.diffText = rewrite::RewriteValidator::computeAccessibleDiff(propB.originalText, propB.rewrittenText);
    const auto profile = rewrite::ProfileRegistry::getProfile(propB.rewriteProfileId);
    ROCLINT_CHECK(profile.has_value());
    propB.constraints = rewrite::RewriteValidator::deriveConstraints(c.intent, *profile, propB.rewrittenText);
    propB.id = "prop_candB_sel2";
    rewrite::ProposalHasher::seal(propB);
    ROCLINT_CHECK(store.repo->insertProposal(propB, 500).ok());

    // Selection 3: Reselect Candidate A with fresh proposal ID and fresh audit verdict
    auto propA2 = c.proposal; // identical text/hash to propA1
    propA2.id = "prop_candA_sel3";
    ValidationResult freshAudit;
    freshAudit.outcome = ValidationOutcome::Unresolved;
    freshAudit.executionStatus = AuditExecutionStatus::Completed;
    freshAudit.unresolved.push_back({"constraint_1", "Fresh uncertainty review pending"});
    propA2.auditResult = freshAudit;
    rewrite::ProposalHasher::seal(propA2);
    ROCLINT_CHECK(propA2.proposalHash == propA1.proposalHash);
    ROCLINT_CHECK(store.repo->insertProposal(propA2, 600).ok());

    // Verify all 3 selection rows exist in storage without conflict suppression
    const auto count = store.db.queryText("SELECT COUNT(*) FROM proposals WHERE session_id = 'sess_reselect';");
    ROCLINT_CHECK(count.ok() && count.value() == "3");

    // Session restore retrieves the latest selected candidate (propA2) and its exact fresh audit
    auto restored = store.repo->restoreSession(SessionId("sess_reselect"));
    ROCLINT_CHECK(restored.ok());
    ROCLINT_CHECK(restored.value().proposal.has_value());
    const auto& rp = *restored.value().proposal;
    ROCLINT_CHECK(rp.id == "prop_candA_sel3");
    ROCLINT_CHECK(rp.proposalHash == propA1.proposalHash);
    ROCLINT_CHECK(rp.rewrittenText == propA1.rewrittenText);
    ROCLINT_CHECK(rp.auditResult.has_value());
    ROCLINT_CHECK(rp.auditResult->outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(!rp.auditResult->unresolved.empty());
    ROCLINT_CHECK(rp.auditResult->unresolved[0].explanation == "Fresh uncertainty review pending");
}

void testHistoricalApprovalPreservedOnReselection() {
    Store store;
    fx::Chain c("sess_hist_appr");
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root("sess_hist_appr"), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    // Selection 1: Candidate A
    auto propA1 = c.proposal;
    propA1.id = "prop_candA_sel1";
    rewrite::ProposalHasher::seal(propA1);
    ROCLINT_CHECK(store.repo->insertProposal(propA1, 400).ok());

    // Approve Selection 1
    auto ctx = c.context();
    ctx.proposal = &propA1;
    auto appr = review::ApprovalFactory::createApproval(ctx, 450);
    ROCLINT_CHECK(appr.ok());
    ROCLINT_CHECK(store.repo->insertApproval(appr.value()).ok());

    // Selection 2: Reselect Candidate A with fresh proposal ID (not yet approved)
    auto propA2 = c.proposal;
    propA2.id = "prop_candA_sel2";
    rewrite::ProposalHasher::seal(propA2);
    ROCLINT_CHECK(store.repo->insertProposal(propA2, 500).ok());

    // Restore returns latest selection (propA2). Since propA2 has no approval, restored.approval is nullopt!
    auto restored = store.repo->restoreSession(SessionId("sess_hist_appr"));
    ROCLINT_CHECK(restored.ok());
    ROCLINT_CHECK(restored.value().proposal.has_value());
    ROCLINT_CHECK(restored.value().proposal->id == "prop_candA_sel2");
    ROCLINT_CHECK(!restored.value().approval.has_value());

    // Historical approval record bound to propA1 remains intact in database
    auto history = store.repo->getApprovalHistory(SessionId("sess_hist_appr"));
    ROCLINT_CHECK(history.ok());
    ROCLINT_CHECK(history.value().size() == 1);
    ROCLINT_CHECK(history.value()[0].proposalId == "prop_candA_sel1");
    ROCLINT_CHECK(history.value()[0].proposalHash == propA1.proposalHash);
    ROCLINT_CHECK(history.value()[0].id == appr.value().id);
}

void testInsertProposalRejectionAndDuplicateConflict() {
    Store store;
    fx::Chain c("sess_conflict");
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root("sess_conflict"), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    // Missing prop_ prefix is rejected before SQL
    auto badPrefix = c.proposal;
    badPrefix.id = "no_prefix_here";
    ROCLINT_CHECK(!store.repo->insertProposal(badPrefix, 400).ok());

    // Empty id is rejected before SQL
    auto emptyId = c.proposal;
    emptyId.id = "";
    ROCLINT_CHECK(!store.repo->insertProposal(emptyId, 400).ok());

    // Mismatched proposalHash is rejected before SQL
    auto badHash = c.proposal;
    badHash.id = "prop_valid_prefix";
    badHash.proposalHash = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
    ROCLINT_CHECK(!store.repo->insertProposal(badHash, 400).ok());

    // Valid proposal inserts cleanly
    auto prop1 = c.proposal;
    prop1.id = "prop_unique_1";
    rewrite::ProposalHasher::seal(prop1);
    ROCLINT_CHECK(store.repo->insertProposal(prop1, 400).ok());

    // Re-inserting the exact same proposal_id fails without conflict suppression
    ROCLINT_CHECK(!store.repo->insertProposal(prop1, 401).ok());
}

} // namespace

int main() {
    std::cout << "[Test PromptRepository] Starting...\n";
    testCoalescedDraftRangeIsAtomic();
    testRootRevisionsInTwoSessionsDoNotCollide();
    testRevisionWritesUseOptimisticConcurrency();
    testCrossSessionRejectionInMemory();
    testForeignKeysHoldAfterReopenOnNonAsciiPath();
    testFailedApprovalKeepsPreviousCurrent();
    testRestoreReturnsBoundArtifactsOnly();
    testModelAttemptsAndOrphanRecovery();
    testProposalGenerationIsPersistedMonotonically();
    testDeleteAndPurge();
    testStateCompareAndSwapAndArtifactFailuresRollBackTogether();
    testAstSnapshotHashPayloadRoundTrip();
    testPayloadCodecCustomProposalIdAndValidation();
    testReselectionWithDistinctIdsInsertsSeparateRowsAndRestoresLatest();
    testHistoricalApprovalPreservedOnReselection();
    testInsertProposalRejectionAndDuplicateConflict();
    std::cout << "[Test PromptRepository] All tests PASSED.\n";
    return 0;
}
