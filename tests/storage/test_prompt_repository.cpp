#include "support/check.hpp"
#include "roclint/storage/Database.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using roclint::core::PromptRevision;
using roclint::core::RevisionId;
using roclint::core::SessionId;
using roclint::storage::Database;
using roclint::storage::PromptRepository;

void testSessionLifecycle()
{
    std::cout << "  -> Running testSessionLifecycle...\n";
    Database db(":memory:");
    PromptRepository repo(db);
    repo.migrate();

    const SessionId sess1("sess_lifecycle_01");
    const SessionId sess2("sess_lifecycle_02");

    ROCLINT_CHECK(!repo.sessionExists(sess1));
    ROCLINT_CHECK(!repo.sessionExists(sess2));

    repo.createSession(sess1, 1000);
    repo.createSession(sess2, 2000);

    ROCLINT_CHECK(repo.sessionExists(sess1));
    ROCLINT_CHECK(repo.sessionExists(sess2));
    ROCLINT_CHECK(!repo.sessionExists(SessionId("sess_lifecycle_missing")));

    auto sessions = repo.listSessions();
    ROCLINT_CHECK(sessions.size() == 2);
    // Ordered by created_at_ms DESC
    ROCLINT_CHECK(sessions[0].id == sess2);
    ROCLINT_CHECK(sessions[0].createdAtMs == 2000);
    ROCLINT_CHECK(sessions[1].id == sess1);
    ROCLINT_CHECK(sessions[1].createdAtMs == 1000);
}

void testRevisionPersistenceAndRetrieval()
{
    std::cout << "  -> Running testRevisionPersistenceAndRetrieval...\n";
    Database db(":memory:");
    PromptRepository repo(db);
    repo.migrate();

    const SessionId sessId("sess_persist_01");
    repo.createSession(sessId, 1700000000);

    // 1. Save root revision (no parent)
    const RevisionId rev1Id("rev_root_01");
    const PromptRevision rev1(sessId, rev1Id, std::nullopt, "Initial prompt instruction", 1700000010);
    repo.saveRevision(rev1);

    auto fetched1 = repo.getRevision(rev1Id);
    ROCLINT_CHECK(fetched1.has_value());
    ROCLINT_CHECK(fetched1->sessionId() == sessId);
    ROCLINT_CHECK(fetched1->revisionId() == rev1Id);
    ROCLINT_CHECK(!fetched1->parentRevisionId().has_value());
    ROCLINT_CHECK(fetched1->text() == "Initial prompt instruction");
    ROCLINT_CHECK(fetched1->createdAtMs() == 1700000010);

    // 2. Save child revision
    const RevisionId rev2Id("rev_child_02");
    const PromptRevision rev2(sessId, rev2Id, rev1Id, "Refined prompt instruction with constraint", 1700000020);
    repo.saveRevision(rev2);

    auto fetched2 = repo.getRevision(rev2Id);
    ROCLINT_CHECK(fetched2.has_value());
    ROCLINT_CHECK(fetched2->sessionId() == sessId);
    ROCLINT_CHECK(fetched2->revisionId() == rev2Id);
    ROCLINT_CHECK(fetched2->parentRevisionId().has_value());
    ROCLINT_CHECK(fetched2->parentRevisionId().value() == rev1Id);
    ROCLINT_CHECK(fetched2->text() == "Refined prompt instruction with constraint");
    ROCLINT_CHECK(fetched2->createdAtMs() == 1700000020);

    // 3. Query non-existent revision
    auto missing = repo.getRevision(RevisionId("non_existent"));
    ROCLINT_CHECK(!missing.has_value());

    // 4. Foreign key enforcement: attempt saving revision for nonexistent session must throw
    bool caughtForeignKeyViolation = false;
    try
    {
        const PromptRevision orphan(SessionId("non_existent_sess"), RevisionId("rev_orphan"),
                                   std::nullopt, "Should fail", 1700000030);
        repo.saveRevision(orphan);
    }
    catch (const std::runtime_error&)
    {
        caughtForeignKeyViolation = true;
    }
    ROCLINT_CHECK(caughtForeignKeyViolation);
}

void testLineageAndHistoryOrder()
{
    std::cout << "  -> Running testLineageAndHistoryOrder...\n";
    Database db(":memory:");
    PromptRepository repo(db);
    repo.migrate();

    const SessionId sessId("sess_lineage_01");
    repo.createSession(sessId, 1700000000);

    const RevisionId rev1("rev_1");
    const RevisionId rev2("rev_2");
    const RevisionId rev3("rev_3");

    repo.saveRevision(PromptRevision(sessId, rev1, std::nullopt, "Step 1", 100));
    repo.saveRevision(PromptRevision(sessId, rev2, rev1, "Step 2", 200));
    repo.saveRevision(PromptRevision(sessId, rev3, rev2, "Step 3", 300));

    auto history = repo.getHistory(sessId);
    ROCLINT_CHECK(history.size() == 3);
    ROCLINT_CHECK(history[0].revisionId() == rev1);
    ROCLINT_CHECK(history[1].revisionId() == rev2);
    ROCLINT_CHECK(history[2].revisionId() == rev3);

    auto latest = repo.getLatestRevision(sessId);
    ROCLINT_CHECK(latest.has_value());
    ROCLINT_CHECK(latest->revisionId() == rev3);
    ROCLINT_CHECK(latest->text() == "Step 3");

    // Empty session history
    const SessionId emptySess("sess_empty");
    repo.createSession(emptySess, 1700000100);
    ROCLINT_CHECK(repo.getHistory(emptySess).empty());
    ROCLINT_CHECK(!repo.getLatestRevision(emptySess).has_value());
}

void testReopenDurability()
{
    std::cout << "  -> Running testReopenDurability...\n";
    const std::filesystem::path dbPath = "./test_prompt_repo_durability.db";
    std::filesystem::remove(dbPath);

    const SessionId sessId("sess_durable_01");
    const RevisionId revId("rev_durable_01");
    const std::string promptText = "Survives process restart across SQLite connections";

    // Scope 1: Write data and close connection
    {
        Database db(dbPath);
        PromptRepository repo(db);
        repo.migrate();

        repo.createSession(sessId, 1700000000);
        repo.saveRevision(PromptRevision(sessId, revId, std::nullopt, promptText, 1700000010));
    }

    // Scope 2: Reopen in a new Database and verify durability
    {
        Database db(dbPath);
        PromptRepository repo(db);

        ROCLINT_CHECK(repo.sessionExists(sessId));
        auto fetched = repo.getRevision(revId);
        ROCLINT_CHECK(fetched.has_value());
        ROCLINT_CHECK(fetched->text() == promptText);
        ROCLINT_CHECK(fetched->createdAtMs() == 1700000010);

        auto history = repo.getHistory(sessId);
        ROCLINT_CHECK(history.size() == 1);
        ROCLINT_CHECK(history[0].revisionId() == revId);
    }

    std::filesystem::remove(dbPath);
}

void testUnicodeAndSpecialCharacters()
{
    std::cout << "  -> Running testUnicodeAndSpecialCharacters...\n";
    Database db(":memory:");
    PromptRepository repo(db);
    repo.migrate();

    const SessionId sessId("sess_unicode_01");
    repo.createSession(sessId, 1700000000);

    // Text containing single quotes, double quotes, LF newlines, emojis, and non-ASCII scripts
    const std::string complexText =
        "Review prompt: \"Don't drop tables!\" — O'Connor's request.\n"
        "Multi-line 🚀 with UTF-8: 漢字, 한국어, Ελληνικά, and symbols: \t \n"
        "SQL injection attempt: '); DROP TABLE prompt_revisions; --";

    const RevisionId revId("rev_unicode_01");
    repo.saveRevision(PromptRevision(sessId, revId, std::nullopt, complexText, 1700000050));

    auto fetched = repo.getRevision(revId);
    ROCLINT_CHECK(fetched.has_value());
    ROCLINT_CHECK(fetched->text() == complexText);

    // Verify the tables were NOT dropped by the injection attempt
    ROCLINT_CHECK(repo.sessionExists(sessId));
}

void testLogicalCascadeDelete()
{
    std::cout << "  -> Running testLogicalCascadeDelete...\n";
    Database db(":memory:");
    PromptRepository repo(db);
    repo.migrate();

    const SessionId sessId("sess_to_delete");
    repo.createSession(sessId, 1700000000);

    const RevisionId rev1("rev_del_1");
    const RevisionId rev2("rev_del_2");

    repo.saveRevision(PromptRevision(sessId, rev1, std::nullopt, "Rev 1 to be deleted", 100));
    repo.saveRevision(PromptRevision(sessId, rev2, rev1, "Rev 2 to be deleted", 200));

    ROCLINT_CHECK(repo.sessionExists(sessId));
    ROCLINT_CHECK(repo.getHistory(sessId).size() == 2);

    // Perform logical delete
    repo.deleteSession(sessId);

    // Verify session and cascade-deleted revisions are gone
    ROCLINT_CHECK(!repo.sessionExists(sessId));
    ROCLINT_CHECK(repo.getHistory(sessId).empty());
    ROCLINT_CHECK(!repo.getRevision(rev1).has_value());
    ROCLINT_CHECK(!repo.getRevision(rev2).has_value());
}

void testSchemaVersionAndMigration()
{
    std::cout << "  -> Running testSchemaVersionAndMigration...\n";
    Database db(":memory:");
    PromptRepository repo(db);

    // Fresh DB before migration
    ROCLINT_CHECK(repo.getSchemaVersion() == 0);

    // Migrate to V1
    repo.migrateTo(1);
    ROCLINT_CHECK(repo.getSchemaVersion() == 1);

    // Insert data into V1
    const SessionId sess("sess_mig_01");
    repo.createSession(sess, 1700000000);
    const RevisionId rev("rev_mig_01");
    repo.saveRevision(PromptRevision(sess, rev, std::nullopt, "V1 prompt", 1700000001));

    // Migrate from V1 to V2
    repo.migrateTo(2);
    ROCLINT_CHECK(repo.getSchemaVersion() == 2);

    // Verify previously inserted data is preserved
    auto fetched = repo.getRevision(rev);
    ROCLINT_CHECK(fetched.has_value());
    ROCLINT_CHECK(fetched->text() == "V1 prompt");

    // Idempotent: migrating again advances to latest schema version without corrupting data
    repo.migrate();
    ROCLINT_CHECK(repo.getSchemaVersion() == PromptRepository::CURRENT_SCHEMA_VERSION);
}

void testTransactionRollback()
{
    std::cout << "  -> Running testTransactionRollback...\n";
    Database db(":memory:");
    PromptRepository repo(db);
    repo.migrate();

    const SessionId sessId("sess_txn_01");
    repo.createSession(sessId, 1700000000);

    const RevisionId rev1("rev_txn_valid");
    repo.saveRevision(PromptRevision(sessId, rev1, std::nullopt, "Authoritative state before failure", 100));

    // Transaction that attempts an invalid operation and rolls back
    db.execute("BEGIN IMMEDIATE TRANSACTION;");
    bool transactionFailed = false;
    try
    {
        repo.saveRevision(PromptRevision(sessId, RevisionId("rev_txn_2"), std::nullopt, "Valid step in txn", 200));
        // Force foreign key failure in same transaction
        repo.saveRevision(PromptRevision(SessionId("non_existent_sess"), RevisionId("rev_fail"), std::nullopt, "Fails", 300));
    }
    catch (const std::runtime_error&)
    {
        transactionFailed = true;
        db.execute("ROLLBACK;");
    }
    ROCLINT_CHECK(transactionFailed);

    // Authoritative state must be intact: rev1 is present, rev_txn_2 was rolled back
    auto history = repo.getHistory(sessId);
    ROCLINT_CHECK(history.size() == 1);
    ROCLINT_CHECK(history[0].revisionId() == rev1);
    ROCLINT_CHECK(!repo.getRevision(RevisionId("rev_txn_2")).has_value());
}

int main()
{
    std::cout << "[test_prompt_repository] Starting PromptRepository tests (A04)...\n";

    testSessionLifecycle();
    testRevisionPersistenceAndRetrieval();
    testLineageAndHistoryOrder();
    testReopenDurability();
    testUnicodeAndSpecialCharacters();
    testLogicalCascadeDelete();
    testSchemaVersionAndMigration();
    testTransactionRollback();

    std::cout << "[test_prompt_repository] All 8 PromptRepository tests passed successfully!\n";
    return 0;
}
