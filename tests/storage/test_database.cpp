#include "support/check.hpp"
#include "roclint/storage/Database.hpp"
#include <iostream>
#include <filesystem>
#include <stdexcept>

using roclint::storage::Database;


void testOpenReopen()
{
    std::cout << "  -> Running testOpenReopen...\n";
    const std::filesystem::path testDbPath = "./test_durability.db";
    std::filesystem::remove(testDbPath); 
    
    {
        Database db(testDbPath);
        ROCLINT_CHECK(db.isOpen());
        db.initializeSchema();

        db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_durability_1', 1700000000);");
       
    }

    
    {
        Database db(testDbPath);
        ROCLINT_CHECK(db.isOpen());

        bool caughtDuplicate = false;
        try {
            db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_durability_1', 1700000001);");
        } catch (const std::runtime_error& e) {
            caughtDuplicate = true;
        }
        ROCLINT_CHECK(caughtDuplicate);
    }

    
    std::filesystem::remove(testDbPath);
}

void testForeignKeysEnforced()
{
    std::cout << "  -> Running testForeignKeysEnforced...\n";
    Database db(":memory:");
    ROCLINT_CHECK(db.isOpen());
    db.initializeSchema();

    bool caughtForeignKeyViolation = false;
    try {
        db.execute("INSERT INTO prompt_revisions (revision_id, prompt_text, session_id, created_at_ms) "
                   "VALUES ('rev_orphan', 'Add authentication flow', 'non_existent_session', 1700000002);");
    } catch (const std::runtime_error& e) {
        caughtForeignKeyViolation = true;
    }
    ROCLINT_CHECK(caughtForeignKeyViolation);
}

void testTransactionRollback()
{
    std::cout << "  -> Running testTransactionRollback...\n";
    Database db(":memory:");
    ROCLINT_CHECK(db.isOpen());
    db.initializeSchema();

   
    db.execute("BEGIN TRANSACTION;");
    db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_rollback_1', 1700000003);");
    db.execute("ROLLBACK;");


    bool insertSucceeded = true;
    try {
        db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_rollback_1', 1700000004);");
    } catch (const std::runtime_error& e) {
        insertSucceeded = false;
    }
    ROCLINT_CHECK(insertSucceeded);
}

void testCascadeDeletion()
{
    std::cout << "  -> Running testCascadeDeletion...\n";
    Database db(":memory:");
    ROCLINT_CHECK(db.isOpen());
    db.initializeSchema();

   
    db.execute("INSERT INTO sessions (session_id, created_at_ms) VALUES ('sess_parent', 1700000005);");

    db.execute("INSERT INTO prompt_revisions (revision_id, prompt_text, session_id, created_at_ms) "
               "VALUES ('rev_child', 'Fix check constraint', 'sess_parent', 1700000006);");

    db.execute("DELETE FROM sessions WHERE session_id = 'sess_parent';");

    bool caughtForeignKeyViolation = false;
    try {
        db.execute("INSERT INTO prompt_revisions (revision_id, prompt_text, session_id, created_at_ms) "
                   "VALUES ('rev_child_orphan', 'Another prompt', 'sess_parent', 1700000007);");
    } catch (const std::runtime_error& e) {
        caughtForeignKeyViolation = true;
    }
    ROCLINT_CHECK(caughtForeignKeyViolation);
}

void testMoveSemantics()
{
    std::cout << "  -> Running testMoveSemantics...\n";
    
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
