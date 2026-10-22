#include "roclint/review/ApprovalRecord.hpp"
#include "roclint/storage/Database.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::review;
using namespace roclint::storage;

void testApprovalCreationAndBindingHash() {
    std::cout << "--- 1. Approval Creation & Binding Hash Stability ---\n";
    SessionId sess("sess_appr_1");
    RevisionId rev("rev_1");

    auto appr1 = ApprovalFactory::createApproval(
        sess,
        rev,
        "snap_001",
        0x12345678ULL,
        "intent_001",
        "prop_001",
        0xABCDEF01ULL,
        "/* ROCLINT COMPILED */\nTarget: FloatingCard.qml",
        0,
        1000
    );

    ROCLINT_CHECK(!appr1.id.empty());
    ROCLINT_CHECK(appr1.sessionId == sess);
    ROCLINT_CHECK(appr1.revisionId == rev);
    ROCLINT_CHECK(appr1.snapshotId == "snap_001");
    ROCLINT_CHECK(appr1.snapshotHash == 0x12345678ULL);
    ROCLINT_CHECK(appr1.intentId == "intent_001");
    ROCLINT_CHECK(appr1.proposalId == "prop_001");
    ROCLINT_CHECK(appr1.proposalHash == 0xABCDEF01ULL);
    ROCLINT_CHECK(appr1.editGeneration == 0);
    ROCLINT_CHECK(appr1.isCurrent);
    ROCLINT_CHECK(appr1.bindingHash != 0);

    // Verify hash stability
    ROCLINT_CHECK(appr1.bindingHash == appr1.computeBindingHash());

    // Valid check
    ROCLINT_CHECK(ApprovalFactory::isApprovalValid(appr1, "rev_1", "prop_001", 0xABCDEF01ULL));

    // Stale proposal hash or ID check
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(appr1, "rev_1", "prop_modified", 0xABCDEF01ULL));
    ROCLINT_CHECK(!ApprovalFactory::isApprovalValid(appr1, "rev_2", "prop_001", 0xABCDEF01ULL));
    std::cout << "  [PASS] Approval proof fields and binding hash verified.\n";
}

void testApprovalPersistenceAndCurrentVsHistorical() {
    std::cout << "--- 2. Approval Persistence & Historical Archival ---\n";
    Database db(":memory:");
    PromptRepository repo(db);
    repo.migrate();

    SessionId sess("sess_persist_1");
    RevisionId rev1("rev_1");
    RevisionId rev2("rev_2");

    repo.createSession(sess, 100);
    repo.saveRevision(PromptRevision(sess, rev1, std::nullopt, "Initial prompt", 100));
    repo.saveRevision(PromptRevision(sess, rev2, rev1, "Second prompt revision", 200));

    // 1. Save approval for rev1
    auto appr1 = ApprovalFactory::createApproval(
        sess,
        rev1,
        "snap_1",
        1111,
        "intent_1",
        "prop_1",
        2222,
        "Approved wording 1",
        0,
        150
    );
    repo.saveApproval(appr1);

    auto curr1 = repo.getCurrentApproval(sess);
    ROCLINT_CHECK(curr1.has_value());
    ROCLINT_CHECK(curr1->id == appr1.id);
    ROCLINT_CHECK(curr1->isCurrent);
    ROCLINT_CHECK(curr1->approvedText == "Approved wording 1");

    // 2. Save new approval for rev2 (e.g. after rewrite)
    auto appr2 = ApprovalFactory::createApproval(
        sess,
        rev2,
        "snap_2",
        3333,
        "intent_2",
        "prop_2",
        4444,
        "Approved wording 2",
        1,
        250
    );
    repo.saveApproval(appr2);

    // Prior approval is automatically archived (is_current = 0)
    auto curr2 = repo.getCurrentApproval(sess);
    ROCLINT_CHECK(curr2.has_value());
    ROCLINT_CHECK(curr2->id == appr2.id);
    ROCLINT_CHECK(curr2->approvedText == "Approved wording 2");
    ROCLINT_CHECK(curr2->editGeneration == 1);

    // History contains both
    auto history = repo.getApprovalHistory(sess);
    ROCLINT_CHECK(history.size() == 2);
    ROCLINT_CHECK(history[0].id == appr2.id);
    ROCLINT_CHECK(history[0].isCurrent == true);
    ROCLINT_CHECK(history[1].id == appr1.id);
    ROCLINT_CHECK(history[1].isCurrent == false);

    // 3. User edits proposal/prompt -> invalidate current approval
    repo.invalidateApproval(sess);
    auto currAfterInvalidate = repo.getCurrentApproval(sess);
    ROCLINT_CHECK(!currAfterInvalidate.has_value());
    std::cout << "  [PASS] Approval persistence, invalidation, and history verified.\n";
}

int main() {
    std::cout << "[TEST] Running A11 Approval State & Durable Proof Tests...\n\n";

    testApprovalCreationAndBindingHash();
    testApprovalPersistenceAndCurrentVsHistorical();

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL A11 APPROVAL STATE TESTS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
