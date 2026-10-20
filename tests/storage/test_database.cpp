#include "support/check.hpp"
#include "roclint/storage/Database.hpp"
#include <iostream>
#include <filesystem>
#include <stdexcept>

using roclint::storage::Database;

// Test 1: Open, initialize, close, and reopen from disk (Durability)
void testOpenReopen()
{
    std::cout << "  -> Running testOpenReopen...\n";
    const std::filesystem::path testDbPath = "./test_durability.db";
    std::filesystem::remove(testDbPath); // Ensure clean start

    // Scope 1: Create DB, initialize schema, and insert a session
    {
        Database db(testDbPath);
        ROCLINT_CHECK(db.isOpen());
        db.initializeSchema();

        db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_durability_1', 1700000000);");
        // Destructor closes the connection here
    }

    // Scope 2: Reopen from disk, verify connection is active and data persisted
    {
        Database db(testDbPath);
        ROCLINT_CHECK(db.isOpen());

        // Prove 'sess_durability_1' exists by attempting to insert a duplicate:
        // SQLite MUST reject this with a PRIMARY KEY constraint violation.
        bool caughtDuplicate = false;
        try {
            db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_durability_1', 1700000001);");
        } catch (const std::runtime_error& e) {
            caughtDuplicate = true;
        }
        ROCLINT_CHECK(caughtDuplicate);
    }

    // Clean up temporary test file
    std::filesystem::remove(testDbPath);
}

// Test 2: Verify that foreign keys are actually enforced (PRAGMA foreign_keys = ON)
void testForeignKeysEnforced()
{
    std::cout << "  -> Running testForeignKeysEnforced...\n";
    Database db(":memory:");
    ROCLINT_CHECK(db.isOpen());
    db.initializeSchema();

    // Attempt to insert a prompt_revision pointing to a non-existent session_id:
    // With foreign keys ON, SQLite MUST throw a foreign key constraint violation.
    bool caughtForeignKeyViolation = false;
    try {
        db.execute("INSERT INTO prompt_revisions (revision_id, prompt_text, session_id, created_at_ms) "
                   "VALUES ('rev_orphan', 'Add authentication flow', 'non_existent_session', 1700000002);");
    } catch (const std::runtime_error& e) {
        caughtForeignKeyViolation = true;
    }
    ROCLINT_CHECK(caughtForeignKeyViolation);
}

// Test 3: Transaction Rollback on Failure
void testTransactionRollback()
{
    std::cout << "  -> Running testTransactionRollback...\n";
    Database db(":memory:");
    ROCLINT_CHECK(db.isOpen());
    db.initializeSchema();

    // Start a transaction, insert a session, then roll back
    db.execute("BEGIN TRANSACTION;");
    db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_rollback_1', 1700000003);");
    db.execute("ROLLBACK;");

    // Because the transaction was rolled back, 'sess_rollback_1' should NOT exist.
    // Inserting 'sess_rollback_1' now must SUCCEED cleanly (not throw a duplicate key error).
    bool insertSucceeded = true;
    try {
        db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_rollback_1', 1700000004);");
    } catch (const std::runtime_error& e) {
        insertSucceeded = false;
    }
    ROCLINT_CHECK(insertSucceeded);
}

// Test 4: Cross-session illegal relation & cascade deletion
void testCascadeDeletion()
{
    std::cout << "  -> Running testCascadeDeletion...\n";
    Database db(":memory:");
    ROCLINT_CHECK(db.isOpen());
    db.initializeSchema();

    // 1. Insert parent session
    db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_parent', 1700000005);");

    // 2. Insert valid child revision
    db.execute("INSERT INTO prompt_revisions (revision_id, prompt_text, session_id, created_at_ms) "
               "VALUES ('rev_child', 'Fix check constraint', 'sess_parent', 1700000006);");

    // 3. Delete parent session: ON DELETE CASCADE should remove child revisions
    db.execute("DELETE FROM sessions WHERE session_id = 'sess_parent';");

    // 4. Verify parent is gone by trying to insert a child referencing 'sess_parent'
    // This MUST fail with a foreign key violation.
    bool caughtForeignKeyViolation = false;
    try {
        db.execute("INSERT INTO prompt_revisions (revision_id, prompt_text, session_id, created_at_ms) "
                   "VALUES ('rev_child_orphan', 'Another prompt', 'sess_parent', 1700000007);");
    } catch (const std::runtime_error& e) {
        caughtForeignKeyViolation = true;
    }
    ROCLINT_CHECK(caughtForeignKeyViolation);
}

// Test 5: Move Constructor and Move Assignment Operator (Rule of 5 Safety)
void testMoveSemantics()
{
    std::cout << "  -> Running testMoveSemantics...\n";
    
    // Move construction
    {
        Database db1(":memory:");
        ROCLINT_CHECK(db1.isOpen());
        db1.initializeSchema();

        Database db2 = std::move(db1);
        ROCLINT_CHECK(!db1.isOpen());
        ROCLINT_CHECK(db2.isOpen());
        ROCLINT_CHECK(db1.rawHandle() == nullptr);
        ROCLINT_CHECK(db2.rawHandle() != nullptr);
    }

    // Move assignment
    {
        Database dbA(":memory:");
        Database dbB(":memory:");
        ROCLINT_CHECK(dbA.isOpen());
        ROCLINT_CHECK(dbB.isOpen());

        dbA = std::move(dbB);
        ROCLINT_CHECK(!dbB.isOpen());
        ROCLINT_CHECK(dbA.isOpen());
        ROCLINT_CHECK(dbB.rawHandle() == nullptr);
    }
}

int main()
{
    std::cout << "[test_database] Starting storage foundation tests...\n";

    testOpenReopen();
    testForeignKeysEnforced();
    testTransactionRollback();
    testCascadeDeletion();
    testMoveSemantics();

    std::cout << "[test_database] All 5 storage foundation tests passed successfully!\n";
    return 0;
}
