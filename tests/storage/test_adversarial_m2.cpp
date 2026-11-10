#include "roclint/storage/Database.hpp"
#include "roclint/storage/PayloadCodec.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "roclint/core/validation_result.hpp"
#include "support/check.hpp"
#include "support/storage_fixtures.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

// ----------------------------------------------------------------------------
// Suite 1: Repeated Selection Sequence A -> B -> A and Return to Base
// ----------------------------------------------------------------------------
void testRepeatedSelectionCycleAndReturnToBase() {
    std::cout << "  [Subtest] Repeated Selection Cycle A -> B -> A -> Base...\n";
    Store store;
    const std::string sessId = "sess_adv_cycle";
    fx::Chain c(sessId);
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root(sessId), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    // Selection 1: Candidate A
    auto propA1 = c.proposal;
    propA1.id = "prop_candA_sel1";
    propA1.rewrittenText = "Candidate A: optimize memory layout and cache lines";
    ValidationResult auditA1;
    auditA1.outcome = ValidationOutcome::Pass;
    auditA1.executionStatus = AuditExecutionStatus::Completed;
    propA1.auditResult = auditA1;
    rewrite::ProposalHasher::seal(propA1);
    ROCLINT_CHECK(store.repo->insertProposal(propA1, 400).ok());

    // Selection 2: Candidate B
    auto propB = c.proposal;
    propB.id = "prop_candB_sel2";
    propB.rewrittenText = "Candidate B: parallelize execution across worker threads";
    ValidationResult auditB;
    auditB.outcome = ValidationOutcome::Pass;
    auditB.executionStatus = AuditExecutionStatus::Completed;
    propB.auditResult = auditB;
    rewrite::ProposalHasher::seal(propB);
    ROCLINT_CHECK(store.repo->insertProposal(propB, 500).ok());

    // Selection 3: Reselect Candidate A with fresh proposal ID and fresh audit verdict (Unresolved)
    auto propA2 = c.proposal;
    propA2.id = "prop_candA_sel3";
    propA2.rewrittenText = propA1.rewrittenText; // identical text!
    ValidationResult auditA2;
    auditA2.outcome = ValidationOutcome::Unresolved;
    auditA2.executionStatus = AuditExecutionStatus::Completed;
    auditA2.unresolved.push_back({"cache_coherency", "Fresh uncertainty: check hardware cache line boundary"});
    propA2.auditResult = auditA2;
    rewrite::ProposalHasher::seal(propA2);

    // Hashes must be identical because content is identical!
    ROCLINT_CHECK(propA1.proposalHash == propA2.proposalHash);
    ROCLINT_CHECK(propA1.id != propA2.id);
    ROCLINT_CHECK(store.repo->insertProposal(propA2, 600).ok());

    // Verify restore right now returns Selection 3 (propA2) with Unresolved audit
    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal.has_value());
        const auto& rp = *restored.value().proposal;
        ROCLINT_CHECK(rp.id == "prop_candA_sel3");
        ROCLINT_CHECK(rp.proposalHash == propA1.proposalHash);
        ROCLINT_CHECK(rp.auditResult.has_value());
        ROCLINT_CHECK(rp.auditResult->outcome == ValidationOutcome::Unresolved);
        ROCLINT_CHECK(rp.auditResult->unresolved.size() == 1);
        ROCLINT_CHECK(rp.auditResult->unresolved[0].explanation == "Fresh uncertainty: check hardware cache line boundary");
    }

    // Selection 4: Return to Base revision/prompt
    auto propBase = c.proposal;
    propBase.id = "prop_base_sel4";
    propBase.rewrittenText = fx::kChainPrompt;
    ValidationResult auditBase;
    auditBase.outcome = ValidationOutcome::Pass;
    auditBase.executionStatus = AuditExecutionStatus::DeterministicOnly;
    propBase.auditResult = auditBase;
    rewrite::ProposalHasher::seal(propBase);
    ROCLINT_CHECK(store.repo->insertProposal(propBase, 700).ok());

    // Verify total rows in proposals table is exactly 4
    const auto count4 = store.db.queryText("SELECT COUNT(*) FROM proposals WHERE session_id = 'sess_adv_cycle';");
    ROCLINT_CHECK(count4.ok() && count4.value() == "4");

    // Verify restore now returns Selection 4 (Base)
    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal.has_value());
        const auto& rp = *restored.value().proposal;
        ROCLINT_CHECK(rp.id == "prop_base_sel4");
        ROCLINT_CHECK(rp.rewrittenText == fx::kChainPrompt);
        ROCLINT_CHECK(rp.auditResult.has_value());
        ROCLINT_CHECK(rp.auditResult->outcome == ValidationOutcome::Pass);
        ROCLINT_CHECK(rp.auditResult->executionStatus == AuditExecutionStatus::DeterministicOnly);
    }

    // Selection 5: Reselect Candidate A yet again with fresh audit outcome (Fail)
    auto propA3 = c.proposal;
    propA3.id = "prop_candA_sel5";
    propA3.rewrittenText = propA1.rewrittenText; // identical text!
    ValidationResult auditA3;
    auditA3.outcome = ValidationOutcome::Fail;
    auditA3.executionStatus = AuditExecutionStatus::Completed;
    auditA3.violations.push_back({"prohibit_mutation", "Violation: cache line padding alters external ABI"});
    propA3.auditResult = auditA3;
    rewrite::ProposalHasher::seal(propA3);
    ROCLINT_CHECK(propA3.proposalHash == propA1.proposalHash);
    ROCLINT_CHECK(store.repo->insertProposal(propA3, 800).ok());

    // Total rows is now 5
    const auto count5 = store.db.queryText("SELECT COUNT(*) FROM proposals WHERE session_id = 'sess_adv_cycle';");
    ROCLINT_CHECK(count5.ok() && count5.value() == "5");

    // Verify restore returns Selection 5 with Fail outcome
    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal.has_value());
        const auto& rp = *restored.value().proposal;
        ROCLINT_CHECK(rp.id == "prop_candA_sel5");
        ROCLINT_CHECK(rp.auditResult.has_value());
        ROCLINT_CHECK(rp.auditResult->outcome == ValidationOutcome::Fail);
        ROCLINT_CHECK(!rp.auditResult->violations.empty());
        ROCLINT_CHECK(rp.auditResult->violations[0].explanation == "Violation: cache line padding alters external ABI");
    }

    // Direct SQL check: verify historical selections 1 and 3 are intact with their respective audits
    {
        auto row1 = store.db.queryText("SELECT payload_json FROM proposals WHERE proposal_id = 'prop_candA_sel1';");
        ROCLINT_CHECK(row1.ok());
        auto decoded1 = decodeProposal(row1.value());
        ROCLINT_CHECK(decoded1.ok() && decoded1.value().auditResult.has_value());
        ROCLINT_CHECK(decoded1.value().auditResult->outcome == ValidationOutcome::Pass);

        auto row3 = store.db.queryText("SELECT payload_json FROM proposals WHERE proposal_id = 'prop_candA_sel3';");
        ROCLINT_CHECK(row3.ok());
        auto decoded3 = decodeProposal(row3.value());
        ROCLINT_CHECK(decoded3.ok() && decoded3.value().auditResult.has_value());
        ROCLINT_CHECK(decoded3.value().auditResult->outcome == ValidationOutcome::Unresolved);
    }
}

// ----------------------------------------------------------------------------
// Suite 2: Transactional Restore Ordering & Tie-Breaking
// ----------------------------------------------------------------------------
void testTransactionalRestoreOrderingAndTieBreaking() {
    std::cout << "  [Subtest] Transactional Restore Ordering & Tie-Breaking...\n";
    Store store;
    const std::string sessId = "sess_adv_tiebreak";
    fx::Chain c(sessId);
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root(sessId), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    // Scenario A: Identical timestamps (nowMs = 1500) inserted consecutively
    const std::int64_t sameTime = 1500;

    auto p1 = c.proposal;
    p1.id = "prop_tie_1";
    ValidationResult v1;
    v1.outcome = ValidationOutcome::Pass;
    v1.executionStatus = AuditExecutionStatus::Completed;
    p1.auditResult = v1;
    rewrite::ProposalHasher::seal(p1);
    ROCLINT_CHECK(store.repo->insertProposal(p1, sameTime).ok());

    auto p2 = c.proposal;
    p2.id = "prop_tie_2";
    ValidationResult v2;
    v2.outcome = ValidationOutcome::Unresolved;
    v2.executionStatus = AuditExecutionStatus::Completed;
    v2.unresolved.push_back({"r1", "Unresolved pending"});
    p2.auditResult = v2;
    rewrite::ProposalHasher::seal(p2);
    ROCLINT_CHECK(store.repo->insertProposal(p2, sameTime).ok());

    auto p3 = c.proposal;
    p3.id = "prop_tie_3";
    ValidationResult v3;
    v3.outcome = ValidationOutcome::Fail;
    v3.executionStatus = AuditExecutionStatus::Completed;
    v3.violations.push_back({"v1", "Hard violation"});
    p3.auditResult = v3;
    rewrite::ProposalHasher::seal(p3);
    ROCLINT_CHECK(store.repo->insertProposal(p3, sameTime).ok());

    // restoreSession must use rowid DESC to tie-break identical created_at timestamps!
    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal.has_value());
        ROCLINT_CHECK(restored.value().proposal->id == "prop_tie_3");
        ROCLINT_CHECK(restored.value().proposal->auditResult->outcome == ValidationOutcome::Fail);
    }

    // Insert p4 at same timestamp: now p4 must win by rowid
    auto p4 = c.proposal;
    p4.id = "prop_tie_4";
    ValidationResult v4;
    v4.outcome = ValidationOutcome::Pass;
    v4.executionStatus = AuditExecutionStatus::Completed;
    p4.auditResult = v4;
    rewrite::ProposalHasher::seal(p4);
    ROCLINT_CHECK(store.repo->insertProposal(p4, sameTime).ok());

    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal->id == "prop_tie_4");
        ROCLINT_CHECK(restored.value().proposal->auditResult->outcome == ValidationOutcome::Pass);
    }

    // Scenario B: Historical Approval Preservation
    // Approve p4
    auto ctx4 = c.context();
    ctx4.proposal = &p4;
    auto appr4 = review::ApprovalFactory::createApproval(ctx4, 1600);
    ROCLINT_CHECK(appr4.ok());
    ROCLINT_CHECK(store.repo->insertApproval(appr4.value()).ok());

    // Restore right now should have proposal p4 AND approval
    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal->id == "prop_tie_4");
        ROCLINT_CHECK(restored.value().approval.has_value());
        ROCLINT_CHECK(restored.value().approval->proposalId == "prop_tie_4");
    }

    // Reselect p1 (with fresh id p5) without approving it
    auto p5 = c.proposal;
    p5.id = "prop_tie_5_unapproved";
    ValidationResult v5;
    v5.outcome = ValidationOutcome::Pass;
    v5.executionStatus = AuditExecutionStatus::Completed;
    p5.auditResult = v5;
    rewrite::ProposalHasher::seal(p5);
    ROCLINT_CHECK(store.repo->insertProposal(p5, 2000).ok());

    // Restore returns p5, but p5 is NOT approved -> restored.approval MUST be std::nullopt!
    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal->id == "prop_tie_5_unapproved");
        ROCLINT_CHECK(!restored.value().approval.has_value());
    }

    // History still holds p4's approval
    {
        auto history = store.repo->getApprovalHistory(SessionId(sessId));
        ROCLINT_CHECK(history.ok());
        ROCLINT_CHECK(history.value().size() == 1);
        ROCLINT_CHECK(history.value()[0].proposalId == "prop_tie_4");
    }

    // Now approve p5:
    auto ctx5 = c.context(3);
    ctx5.proposal = &p5;
    auto appr5 = review::ApprovalFactory::createApproval(ctx5, 2100);
    ROCLINT_CHECK(appr5.ok());
    ROCLINT_CHECK(store.repo->insertApproval(appr5.value()).ok());

    // Restore returns p5 AND its approval
    {
        auto restored = store.repo->restoreSession(SessionId(sessId));
        ROCLINT_CHECK(restored.ok());
        ROCLINT_CHECK(restored.value().proposal->id == "prop_tie_5_unapproved");
        ROCLINT_CHECK(restored.value().approval.has_value());
        ROCLINT_CHECK(restored.value().approval->proposalId == "prop_tie_5_unapproved");
    }

    // History now holds 2 approval records (unmutated audit trail)
    {
        auto history = store.repo->getApprovalHistory(SessionId(sessId));
        ROCLINT_CHECK(history.ok());
        ROCLINT_CHECK(history.value().size() == 2);
    }
}

// ----------------------------------------------------------------------------
// Suite 3: Duplicate Proposal ID Transaction Failure & Rejection
// ----------------------------------------------------------------------------
void testDuplicateProposalIdFailsTransactionally() {
    std::cout << "  [Subtest] Duplicate Proposal ID Transaction Failure...\n";
    Store store;
    const std::string sessId = "sess_adv_dup";
    fx::Chain c(sessId);
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root(sessId), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    auto prop1 = c.proposal;
    prop1.id = "prop_unique_deterministic_key";
    prop1.rewrittenText = "First original text for this ID";
    rewrite::ProposalHasher::seal(prop1);
    ROCLINT_CHECK(store.repo->insertProposal(prop1, 400).ok());

    // Re-inserting with the same proposal ID must fail with an error!
    auto propDup = prop1;
    propDup.rewrittenText = "Attempted overwrite with altered text";
    rewrite::ProposalHasher::seal(propDup);
    auto dupResult = store.repo->insertProposal(propDup, 500);
    ROCLINT_CHECK(!dupResult.ok());
    // Must NOT be silently ignored like ON CONFLICT DO NOTHING

    // Verify row in database was NOT changed or overwritten
    auto row = store.db.queryText("SELECT payload_json FROM proposals WHERE proposal_id = 'prop_unique_deterministic_key';");
    ROCLINT_CHECK(row.ok());
    auto decoded = decodeProposal(row.value());
    ROCLINT_CHECK(decoded.ok());
    ROCLINT_CHECK(decoded.value().rewrittenText == "First original text for this ID");

    // Test transaction rollback when expectedVersion is supplied
    auto sessionRecBefore = store.repo->getSession(SessionId(sessId));
    ROCLINT_CHECK(sessionRecBefore.ok());
    const auto verBefore = sessionRecBefore.value().stateVersion;

    auto dupWithExpectedVer = prop1;
    auto failWithTx = store.repo->insertProposal(dupWithExpectedVer, 600, verBefore);
    ROCLINT_CHECK(!failWithTx.ok());

    auto sessionRecAfter = store.repo->getSession(SessionId(sessId));
    ROCLINT_CHECK(sessionRecAfter.ok());
    ROCLINT_CHECK(sessionRecAfter.value().stateVersion == verBefore); // State version was NOT advanced!

    // Verify database is completely healthy and accepts subsequent valid insertions
    auto propNext = c.proposal;
    propNext.id = "prop_subsequent_healthy_insert";
    rewrite::ProposalHasher::seal(propNext);
    ROCLINT_CHECK(store.repo->insertProposal(propNext, 700).ok());
}

// ----------------------------------------------------------------------------
// Suite 4: Invalid Proposal IDs and Seal Behavior
// ----------------------------------------------------------------------------
void testInvalidProposalIdsAndSealContract() {
    std::cout << "  [Subtest] Invalid Proposal IDs and Seal Contract...\n";
    Store store;
    const std::string sessId = "sess_adv_inval";
    fx::Chain c(sessId);
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root(sessId), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    // 1. Missing "prop_" prefix
    auto pBadPrefix = c.proposal;
    pBadPrefix.id = "noprefix_123";
    ROCLINT_CHECK(!store.repo->insertProposal(pBadPrefix, 400).ok());

    // 2. Empty ID
    auto pEmptyId = c.proposal;
    pEmptyId.id = "";
    ROCLINT_CHECK(!store.repo->insertProposal(pEmptyId, 400).ok());

    // 3. Leading whitespace prefix
    auto pSpacePrefix = c.proposal;
    pSpacePrefix.id = " prop_leading_space";
    ROCLINT_CHECK(!store.repo->insertProposal(pSpacePrefix, 400).ok());

    // 4. Tampered hash
    auto pBadHash = c.proposal;
    pBadHash.id = "prop_valid_prefix_bad_hash";
    pBadHash.proposalHash = "0000000000000000000000000000000000000000000000000000000000000000";
    ROCLINT_CHECK(!store.repo->insertProposal(pBadHash, 400).ok());

    // 5. Seal contract semantics
    {
        auto p = c.proposal;
        p.id = "";
        rewrite::ProposalHasher::seal(p);
        ROCLINT_CHECK(!p.id.empty());
        ROCLINT_CHECK(p.id.starts_with("prop_"));
        ROCLINT_CHECK(p.id == "prop_" + p.proposalHash);

        // Pre-existing prop_ ID is preserved
        p.id = "prop_custom_unique_id";
        rewrite::ProposalHasher::seal(p);
        ROCLINT_CHECK(p.id == "prop_custom_unique_id");

        // Non-prop prefix gets "prop_" prepended
        p.id = "unprefixed_id";
        rewrite::ProposalHasher::seal(p);
        ROCLINT_CHECK(p.id == "prop_unprefixed_id");

        // Idempotency: repeated seal() changes nothing
        const std::string finalId = p.id;
        const std::string finalHash = p.proposalHash;
        rewrite::ProposalHasher::seal(p);
        ROCLINT_CHECK(p.id == finalId);
        ROCLINT_CHECK(p.proposalHash == finalHash);
    }
}

// ----------------------------------------------------------------------------
// Suite 5: Rapid Alternation Stress Test
// ----------------------------------------------------------------------------
void testRapidAlternationStress() {
    std::cout << "  [Subtest] Rapid Alternation Stress Test (100 iterations)...\n";
    Store store;
    const std::string sessId = "sess_adv_stress";
    fx::Chain c(sessId);
    ROCLINT_CHECK(store.repo->createSessionWithRootRevision(root(sessId), 100).ok());
    fx::recordSnapshotAttempts(*store.repo, c.snapshot);
    ROCLINT_CHECK(store.repo->insertAnalysisSnapshot(c.snapshot, 200).ok());
    ROCLINT_CHECK(store.repo->insertIntentVersion(c.intent, 300).ok());

    const int iterations = 100;
    for (int i = 1; i <= iterations; ++i) {
        auto p = c.proposal;
        p.id = "prop_pingpong_" + std::to_string(i);
        p.rewrittenText = (i % 2 == 1) ? "Candidate A variant" : "Candidate B variant";

        ValidationResult audit;
        if (i % 3 == 0) {
            audit.outcome = ValidationOutcome::Fail;
            audit.violations.push_back({"rule_fail", "Violation in iter " + std::to_string(i)});
        } else if (i % 3 == 1) {
            audit.outcome = ValidationOutcome::Unresolved;
            audit.unresolved.push_back({"rule_unres", "Uncertainty in iter " + std::to_string(i)});
        } else {
            audit.outcome = ValidationOutcome::Pass;
        }
        audit.executionStatus = AuditExecutionStatus::Completed;
        p.auditResult = audit;
        rewrite::ProposalHasher::seal(p);

        ROCLINT_CHECK(store.repo->insertProposal(p, 1000 + i).ok());
    }

    // Verify row count
    const auto count = store.db.queryText("SELECT COUNT(*) FROM proposals WHERE session_id = 'sess_adv_stress';");
    ROCLINT_CHECK(count.ok() && count.value() == "100");

    // Verify restore returns the 100th iteration
    auto restored = store.repo->restoreSession(SessionId(sessId));
    ROCLINT_CHECK(restored.ok());
    ROCLINT_CHECK(restored.value().proposal.has_value());
    const auto& rp = *restored.value().proposal;
    ROCLINT_CHECK(rp.id == "prop_pingpong_100");
    ROCLINT_CHECK(rp.rewrittenText == "Candidate B variant");
    ROCLINT_CHECK(rp.auditResult.has_value());
    // 100 % 3 == 1 -> Unresolved
    ROCLINT_CHECK(rp.auditResult->outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(!rp.auditResult->unresolved.empty());
    ROCLINT_CHECK(rp.auditResult->unresolved[0].explanation == "Uncertainty in iter 100");
}

// ----------------------------------------------------------------------------
// Suite 6: Audit Evidence Serialization and Payload Roundtrip Fidelity
// ----------------------------------------------------------------------------
void testAuditEvidencePersistenceFidelity() {
    std::cout << "  [Subtest] Audit Evidence PayloadCodec Fidelity...\n";
    fx::Chain c("sess_adv_codec");
    auto p = c.proposal;
    p.id = "prop_codec_rich_audit";

    ValidationResult richAudit;
    richAudit.outcome = ValidationOutcome::Unresolved;
    richAudit.executionStatus = AuditExecutionStatus::DeterministicOnly;
    richAudit.violations.push_back({"v1", "First violation"});
    richAudit.violations.push_back({"v2", "Second violation"});
    richAudit.unresolved.push_back({"u1", "First uncertainty"});
    richAudit.unresolved.push_back({"u2", "Second uncertainty"});
    richAudit.unresolved.push_back({"u3", "Third uncertainty"});
    p.auditResult = richAudit;
    rewrite::ProposalHasher::seal(p);

    const std::string json = encodeProposal(p);
    auto decoded = decodeProposal(json);
    ROCLINT_CHECK(decoded.ok());
    const auto& dp = decoded.value();
    ROCLINT_CHECK(dp.id == p.id);
    ROCLINT_CHECK(dp.proposalHash == p.proposalHash);
    ROCLINT_CHECK(dp.auditResult.has_value());
    ROCLINT_CHECK(dp.auditResult->outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(dp.auditResult->executionStatus == AuditExecutionStatus::DeterministicOnly);
    ROCLINT_CHECK(dp.auditResult->violations.size() == 2);
    ROCLINT_CHECK(dp.auditResult->violations[0].explanation == "First violation");
    ROCLINT_CHECK(dp.auditResult->violations[1].explanation == "Second violation");
    ROCLINT_CHECK(dp.auditResult->unresolved.size() == 3);
    ROCLINT_CHECK(dp.auditResult->unresolved[0].explanation == "First uncertainty");
    ROCLINT_CHECK(dp.auditResult->unresolved[1].explanation == "Second uncertainty");
    ROCLINT_CHECK(dp.auditResult->unresolved[2].explanation == "Third uncertainty");
}

} // namespace

int main() {
    std::cout << "=======================================================\n";
    std::cout << "[EMPIRICAL CHALLENGER] Milestone 2 Adversarial Harness\n";
    std::cout << "=======================================================\n";

    testRepeatedSelectionCycleAndReturnToBase();
    testTransactionalRestoreOrderingAndTieBreaking();
    testDuplicateProposalIdFailsTransactionally();
    testInvalidProposalIdsAndSealContract();
    testRapidAlternationStress();
    testAuditEvidencePersistenceFidelity();

    std::cout << "=======================================================\n";
    std::cout << "[ALL EMPIRICAL CHALLENGE TESTS PASSED SUCCESSFULLY]\n";
    std::cout << "=======================================================\n";
    return 0;
}
