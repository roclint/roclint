#include "roclint/storage/Database.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "support/check.hpp"

#include <filesystem>
#include <iostream>

using namespace roclint::core;
using namespace roclint::storage;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const char8_t* name) {
    const fs::path dir = fs::temp_directory_path() / fs::path(name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void testInMemoryPragmas() {
    Database db;
    ROCLINT_CHECK(db.open(":memory:").ok());
    ROCLINT_CHECK(db.isOpen());
    ROCLINT_CHECK(db.isInMemory());
    ROCLINT_CHECK(db.queryText("PRAGMA foreign_keys;").value() == "1");
    ROCLINT_CHECK(db.queryText("PRAGMA journal_mode;").value() == "memory");
}

void testFilePragmasOnNonAsciiPath() {
    const auto dir = freshDir(u8"roclint_db_\u00e9t\u00e9_\u65e5\u672c");
    const auto path = dir / fs::path(u8"hist\u00f6ry.db");
    {
        Database db;
        ROCLINT_CHECK(db.open(path).ok());
        ROCLINT_CHECK(!db.isInMemory());
        ROCLINT_CHECK(db.queryText("PRAGMA foreign_keys;").value() == "1");
        ROCLINT_CHECK(db.queryText("PRAGMA journal_mode;").value() == "wal");
        ROCLINT_CHECK(db.queryText("PRAGMA synchronous;").value() == "1");
        ROCLINT_CHECK(db.queryText("PRAGMA busy_timeout;").value() == std::to_string(Database::kBusyTimeoutMs));
        ROCLINT_CHECK(db.execute("CREATE TABLE t (x INTEGER);").ok());
    }
    // The file was created at the exact Unicode path, not a mangled ANSI one.
    ROCLINT_CHECK(fs::exists(path));
    {
        Database reopened;
        ROCLINT_CHECK(reopened.open(path).ok());
        ROCLINT_CHECK(reopened.queryText("SELECT COUNT(*) FROM sqlite_master WHERE name = 't';").value() == "1");
        ROCLINT_CHECK(reopened.queryText("PRAGMA foreign_keys;").value() == "1");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void testOpenFailureIsReported() {
    Database db;
    const auto dir = freshDir(u8"roclint_db_missing_parent");
    auto st = db.open(dir / "no" / "such" / "dir" / "x.db");
    ROCLINT_CHECK(!st.ok());
    ROCLINT_CHECK(!db.isOpen());
    ROCLINT_CHECK(!db.execute("SELECT 1;").ok());
}

void testMoveTransfersHandle() {
    Database a;
    ROCLINT_CHECK(a.open(":memory:").ok());
    Database b(std::move(a));
    ROCLINT_CHECK(b.isOpen());
    ROCLINT_CHECK(!a.isOpen());
}

void testLegacyMigrationToCompositeSchema() {
    Database db;
    ROCLINT_CHECK(db.open(":memory:").ok());
    PromptRepository repo(db);
    ROCLINT_CHECK(repo.migrateTo(3).ok());
    ROCLINT_CHECK(repo.getSchemaVersion().value() == 3);

    // Legacy data: two sessions, one revision with a cross-session parent, one self-parented.
    ROCLINT_CHECK(db.execute(R"SQL(
        INSERT INTO sessions (session_id, created_at_ms) VALUES ('s1', 10), ('s2', 20), ('empty', 30);
        INSERT INTO prompt_revisions (revision_id, parent_revision_id, prompt_text, session_id, created_at_ms) VALUES
            ('r1', NULL, 'first', 's1', 100),
            ('r2', 'r1', 'second', 's1', 200),
            ('x1', 'r2', 'cross parent', 's2', 150),
            ('x2', 'x2', 'self parent', 's2', 160);
    )SQL").ok());

    ROCLINT_CHECK(repo.migrate().ok());
    ROCLINT_CHECK(repo.getSchemaVersion().value() == PromptRepository::CURRENT_SCHEMA_VERSION);
    auto s1 = repo.getHistory(SessionId("s1")).value();
    ROCLINT_CHECK(s1.size() == 2);
    ROCLINT_CHECK(s1[0].sequence == 1 && s1[0].origin == RevisionOrigin::Root);
    ROCLINT_CHECK(s1[1].sequence == 2 && s1[1].revision.parentRevisionId()->value() == "r1");
    ROCLINT_CHECK(repo.getSession(SessionId("s1")).value().highestRevisionSeq == 2);

    auto s2 = repo.getHistory(SessionId("s2")).value();
    ROCLINT_CHECK(s2.size() == 2);
    ROCLINT_CHECK(!s2[0].revision.parentRevisionId().has_value()); // cross-session parent dropped
    ROCLINT_CHECK(!s2[1].revision.parentRevisionId().has_value()); // self parent dropped

    // Sessions without revisions are not carried over.
    ROCLINT_CHECK(!repo.sessionExists(SessionId("empty")).value());

    // Migrating again is a no-op.
    ROCLINT_CHECK(repo.migrate().ok());
    ROCLINT_CHECK(!repo.migrateTo(PromptRepository::CURRENT_SCHEMA_VERSION + 1).ok());
}

void testNewerSchemaIsRefused() {
    Database db;
    ROCLINT_CHECK(db.open(":memory:").ok());
    ROCLINT_CHECK(db.execute("PRAGMA user_version = 99;").ok());
    PromptRepository repo(db);
    ROCLINT_CHECK(!repo.migrate().ok());
}

} // namespace

int main() {
    std::cout << "[Test Database] Starting...\n";
    testInMemoryPragmas();
    testFilePragmasOnNonAsciiPath();
    testOpenFailureIsReported();
    testMoveTransfersHandle();
    testLegacyMigrationToCompositeSchema();
    testNewerSchemaIsRefused();
    std::cout << "[Test Database] All tests PASSED.\n";
    return 0;
}
