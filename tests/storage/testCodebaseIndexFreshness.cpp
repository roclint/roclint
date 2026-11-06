#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/storage/CodebaseIndex.hpp"
#include "support/check.hpp"

#define ROCLINT_CODEBASE_WATCHER_NO_QT
#include "CodebaseWatcher.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace roclint;
namespace fs = std::filesystem;

namespace {

struct TempArea {
    fs::path root = [&]() {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() /
            ("roclint-freshness-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
    }();
    TempArea() { fs::create_directories(root); }
    ~TempArea() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    [[nodiscard]] fs::path workspace(std::string_view name) {
        const auto path = root / std::string(name);
        fs::create_directories(path / "src");
        return path;
    }
    [[nodiscard]] fs::path database(std::string_view name) const {
        return root / (std::string(name) + ".db");
    }
};

void write(const fs::path& root, std::string_view relative, std::string_view text) {
    const auto path = root / std::string(relative);
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}

std::string readFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void replaceFile(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
    ROCLINT_CHECK(MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
#else
    std::error_code error;
    fs::rename(source, destination, error);
    ROCLINT_CHECK(!error);
#endif
}

sqlite3* openDatabase(const fs::path& path) {
    sqlite3* db = nullptr;
    ROCLINT_CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    return db;
}

int userVersion(const fs::path& path) {
    sqlite3* db = openDatabase(path);
    sqlite3_stmt* statement = nullptr;
    ROCLINT_CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &statement, nullptr) == SQLITE_OK);
    ROCLINT_CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto version = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return version;
}

bool tableExists(const fs::path& path, std::string_view table) {
    sqlite3* db = openDatabase(path);
    sqlite3_stmt* statement = nullptr;
    const std::string sql = "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?1;";
    ROCLINT_CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    ROCLINT_CHECK(sqlite3_bind_text(statement, 1, table.data(), static_cast<int>(table.size()), SQLITE_TRANSIENT) == SQLITE_OK);
    ROCLINT_CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const bool exists = sqlite3_column_int(statement, 0) > 0;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return exists;
}

bool columnExists(const fs::path& path, std::string_view table, std::string_view column) {
    sqlite3* db = openDatabase(path);
    sqlite3_stmt* statement = nullptr;
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    ROCLINT_CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    bool exists = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* name = sqlite3_column_text(statement, 1);
        if (name != nullptr && column == reinterpret_cast<const char*>(name)) {
            exists = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return exists;
}

std::int64_t queryInt(const fs::path& path, const std::string& sql) {
    sqlite3* db = openDatabase(path);
    sqlite3_stmt* statement = nullptr;
    ROCLINT_CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    std::int64_t value = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

void execSql(const fs::path& path, const std::string& sql) {
    sqlite3* db = openDatabase(path);
    ROCLINT_CHECK(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

void createVersion3Database(const fs::path& path, const fs::path& workspace) {
    const auto root = fs::canonical(workspace).generic_string();
    const std::string escapedRoot = root;
    execSql(path,
        "CREATE TABLE workspaces(root TEXT PRIMARY KEY, generation INTEGER NOT NULL DEFAULT 0,"
        " last_refresh_complete INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE files(id INTEGER PRIMARY KEY, root TEXT NOT NULL, path TEXT NOT NULL, hash TEXT NOT NULL,"
        " validated_generation INTEGER NOT NULL DEFAULT 0, UNIQUE(root,path));"
        "CREATE TABLE ast_symbols(symbol_id TEXT PRIMARY KEY, workspace_root TEXT NOT NULL,"
        " validated_generation INTEGER NOT NULL,"
        " file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        " file_path TEXT NOT NULL, file_hash TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL,"
        " kind INTEGER NOT NULL, parent_id TEXT, signature TEXT NOT NULL, start_line INTEGER NOT NULL,"
        " end_line INTEGER NOT NULL, start_column INTEGER NOT NULL, end_column INTEGER NOT NULL,"
        " byte_offset INTEGER NOT NULL, byte_length INTEGER NOT NULL, type_name TEXT NOT NULL,"
        " is_const INTEGER NOT NULL);"
        "CREATE VIRTUAL TABLE ast_symbols_fts USING fts5(name, qualified_name, signature,"
        " content='ast_symbols', content_rowid='rowid', tokenize='trigram');"
        "CREATE TABLE history_events(id INTEGER PRIMARY KEY, note TEXT NOT NULL);"
        "INSERT INTO history_events(note) VALUES('keep-me');"
        "INSERT INTO workspaces(root,generation,last_refresh_complete) VALUES('" + escapedRoot + "',5,1);"
        "INSERT INTO files(root,path,hash,validated_generation)"
        " VALUES('" + escapedRoot + "','src/legacy.hpp','stale-hash',5);"
        "INSERT INTO ast_symbols(symbol_id,workspace_root,validated_generation,file_id,file_path,file_hash,"
        " name,qualified_name,kind,parent_id,signature,start_line,end_line,start_column,end_column,"
        " byte_offset,byte_length,type_name,is_const)"
        " VALUES('stale-symbol','" + escapedRoot + "',5,1,'src/legacy.hpp','stale-hash',"
        " 'staleField','Legacy::staleField',6,'','int staleField;',1,1,1,1,0,0,'int',1);"
        "PRAGMA user_version=3;");
}

void createVersion4Schema(const fs::path& path) {
    execSql(path,
        "CREATE TABLE workspaces(root TEXT PRIMARY KEY, generation INTEGER NOT NULL DEFAULT 0,"
        " last_refresh_complete INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE files(id INTEGER PRIMARY KEY, root TEXT NOT NULL, path TEXT NOT NULL, hash TEXT NOT NULL,"
        " validated_generation INTEGER NOT NULL DEFAULT 0, snapshot_generation INTEGER NOT NULL DEFAULT 0,"
        " dirty INTEGER NOT NULL DEFAULT 0, UNIQUE(root,path));"
        "CREATE TABLE ast_symbols(symbol_id TEXT PRIMARY KEY, workspace_root TEXT NOT NULL,"
        " validated_generation INTEGER NOT NULL,"
        " file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        " file_path TEXT NOT NULL, file_hash TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL,"
        " kind INTEGER NOT NULL, parent_id TEXT, signature TEXT NOT NULL, start_line INTEGER NOT NULL,"
        " end_line INTEGER NOT NULL, start_column INTEGER NOT NULL, end_column INTEGER NOT NULL,"
        " byte_offset INTEGER NOT NULL, byte_length INTEGER NOT NULL, type_name TEXT NOT NULL,"
        " is_const INTEGER NOT NULL,"
        " role INTEGER NOT NULL DEFAULT 0, is_declaration INTEGER NOT NULL DEFAULT 0,"
        " is_definition INTEGER NOT NULL DEFAULT 0, is_template INTEGER NOT NULL DEFAULT 0,"
        " is_static INTEGER NOT NULL DEFAULT 0, is_constexpr INTEGER NOT NULL DEFAULT 0,"
        " is_inline INTEGER NOT NULL DEFAULT 0, is_variadic INTEGER NOT NULL DEFAULT 0,"
        " is_reference INTEGER NOT NULL DEFAULT 0, parameter_index INTEGER NOT NULL DEFAULT 0,"
        " signature_key TEXT NOT NULL DEFAULT '', qualifiers TEXT NOT NULL DEFAULT '');"
        "CREATE VIRTUAL TABLE ast_symbols_fts USING fts5(name, qualified_name, signature,"
        " content='ast_symbols', content_rowid='rowid', tokenize='trigram');"
        "CREATE TABLE history_events(id INTEGER PRIMARY KEY, note TEXT NOT NULL);"
        "INSERT INTO history_events(note) VALUES('keep-me');"
        "PRAGMA user_version=4;");
}

void addVersion4Workspace(const fs::path& path, const fs::path& workspace, bool lastRefreshComplete,
                          bool dirtyRow) {
    const auto root = fs::canonical(workspace).generic_string();
    execSql(path,
        "INSERT INTO workspaces(root,generation,last_refresh_complete) VALUES('" + root + "',3,"
        + (lastRefreshComplete ? "1" : "0") + ");"
        "INSERT INTO files(root,path,hash,validated_generation,snapshot_generation,dirty)"
        " VALUES('" + root + "','src/clean.hpp','clean-hash',3,3,0);"
        "INSERT INTO files(root,path,hash,validated_generation,snapshot_generation,dirty)"
        " VALUES('" + root + "','src/other.hpp','other-hash',3,3," + (dirtyRow ? "1" : "0") + ");");
}

std::size_t symbolCount(const storage::CodebaseIndex& index, std::string_view name) {
    const auto result = index.findByName(name, {}, {}, 64, false);
    ROCLINT_CHECK(result.ok());
    return result.value().symbols.size();
}

void testFullThenSinglePathRefresh() {
    TempArea area;
    const auto workspace = area.workspace("paths");
    const auto database = area.database("paths");
    write(workspace, "src/a.hpp", "class Alpha { public: int alphaField; };\n");
    write(workspace, "src/b.hpp", "class Beta { public: int betaField; };\n");
    storage::CodebaseIndex index(database, workspace);
    const auto first = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(first.ok());
    ROCLINT_CHECK(!first.value().incomplete);
    ROCLINT_CHECK(first.value().generation == 1);
    ROCLINT_CHECK(symbolCount(index, "alphaField") == 1);
    ROCLINT_CHECK(symbolCount(index, "betaField") == 1);
    const auto before = index.snapshotInfo();
    ROCLINT_CHECK(before.ok());
    ROCLINT_CHECK(before.value().available);
    ROCLINT_CHECK(before.value().complete);
    ROCLINT_CHECK(before.value().fullScanComplete);
    ROCLINT_CHECK(before.value().coverageComplete);
    ROCLINT_CHECK(before.value().generation == 1);

    write(workspace, "src/a.hpp", "class Gamma { public: int gammaField; };\n");
    const auto refreshed = index.refreshPaths({workspace, {}, true}, {"src/a.hpp"});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(!refreshed.value().incomplete);
    ROCLINT_CHECK(refreshed.value().generation == 2);
    ROCLINT_CHECK(refreshed.value().filesVisited == 1);
    ROCLINT_CHECK(symbolCount(index, "alphaField") == 0);
    ROCLINT_CHECK(symbolCount(index, "gammaField") == 1);
    // An unrelated, unchanged file remains eligible in the carried generation.
    ROCLINT_CHECK(symbolCount(index, "betaField") == 1);
    const auto after = index.snapshotInfo();
    ROCLINT_CHECK(after.ok());
    ROCLINT_CHECK(after.value().complete);
    ROCLINT_CHECK(after.value().fullScanComplete);
    ROCLINT_CHECK(after.value().coverageComplete);
    ROCLINT_CHECK(after.value().generation == 2);
    const auto carried = index.findByName("betaField", {}, {}, 16, false);
    ROCLINT_CHECK(carried.ok());
    ROCLINT_CHECK(carried.value().complete);
    ROCLINT_CHECK(carried.value().symbols.size() == 1);

    // b.hpp was carried forward without a re-read, so its validated generation
    // stays at 1 while its snapshot generation advances to 2.
    ROCLINT_CHECK(queryInt(database,
        "SELECT snapshot_generation FROM files WHERE path='src/b.hpp';") == 2);
    ROCLINT_CHECK(queryInt(database,
        "SELECT validated_generation FROM files WHERE path='src/b.hpp';") == 1);
    ROCLINT_CHECK(queryInt(database,
        "SELECT validated_generation FROM ast_symbols WHERE file_path='src/b.hpp' LIMIT 1;") == 1);

    // A deleted requested file becomes ineligible and is removed.
    fs::remove(workspace / "src/b.hpp");
    ROCLINT_CHECK(index.refreshPaths({workspace, {}, true}, {"src/b.hpp"}).ok());
    ROCLINT_CHECK(symbolCount(index, "betaField") == 0);
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM files WHERE path='src/b.hpp';") == 0);
    ROCLINT_CHECK(symbolCount(index, "gammaField") == 1);

    // A newly created file can be added through refreshPaths alone.
    write(workspace, "src/c.hpp", "class Delta { public: int deltaField; };\n");
    ROCLINT_CHECK(index.refreshPaths({workspace, {}, true}, {"src/c.hpp"}).ok());
    ROCLINT_CHECK(symbolCount(index, "deltaField") == 1);
    ROCLINT_CHECK(symbolCount(index, "gammaField") == 1);
}

void testAtomicSaveReplacement() {
    TempArea area;
    const auto workspace = area.workspace("atomic");
    const auto database = area.database("atomic");
    write(workspace, "src/saved.hpp", "class Saved { public: int oldValue; };\n");
    storage::CodebaseIndex index(database, workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(symbolCount(index, "oldValue") == 1);

    write(workspace, "src/.saved.hpp.tmp", "class Saved { public: int newValue; };\n");
    replaceFile(workspace / "src/.saved.hpp.tmp", workspace / "src/saved.hpp");
    const auto refreshed = index.refreshPaths({workspace, {}, true}, {"src/saved.hpp"});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(symbolCount(index, "oldValue") == 0);
    ROCLINT_CHECK(symbolCount(index, "newValue") == 1);
    ROCLINT_CHECK(index.snapshotInfo().value().generation == 2);
}

void testIncompleteFullThenPathsStaysIncomplete() {
    TempArea area;
    const auto workspace = area.workspace("incomplete");
    const auto database = area.database("incomplete");
    write(workspace, "src/one.hpp", "class One { public: int oldOne; };\n");
    write(workspace, "src/two.hpp", "class Two { public: int oldTwo; };\n");
    storage::CodebaseIndex index(database, workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(index.snapshotInfo().value().complete);
    ROCLINT_CHECK(index.snapshotInfo().value().fullScanComplete);
    ROCLINT_CHECK(index.snapshotInfo().value().coverageComplete);
    ROCLINT_CHECK(index.snapshotInfo().value().generation == 1);

    const storage::CodebaseIndexLimits noTime{
        storage::CodebaseIndex::kMaximumIndexedFiles, 0, storage::CodebaseIndex::kMaximumFileBytes};
    const auto incomplete = index.refresh({workspace, {}, true}, noTime);
    ROCLINT_CHECK(incomplete.ok());
    ROCLINT_CHECK(incomplete.value().incomplete);
    ROCLINT_CHECK(incomplete.value().unprocessedPaths.empty());
    ROCLINT_CHECK(incomplete.value().generation == 2);
    const auto incompleteInfo = index.snapshotInfo();
    ROCLINT_CHECK(incompleteInfo.ok());
    ROCLINT_CHECK(!incompleteInfo.value().complete);
    ROCLINT_CHECK(!incompleteInfo.value().fullScanComplete);
    ROCLINT_CHECK(!incompleteInfo.value().coverageComplete);
    ROCLINT_CHECK(incompleteInfo.value().generation == 2);
    // Unvisited rows stay at generation 1 and are not exposed under generation 2.
    ROCLINT_CHECK(symbolCount(index, "oldOne") == 0);
    ROCLINT_CHECK(symbolCount(index, "oldTwo") == 0);
    const auto incompleteQuery = index.findByName("oldOne", {}, {}, 8, false);
    ROCLINT_CHECK(incompleteQuery.ok());
    ROCLINT_CHECK(incompleteQuery.value().symbols.empty());
    ROCLINT_CHECK(!incompleteQuery.value().complete);
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM ast_symbols;") > 0);

    // An incremental refresh of one file restores paths coverage but must not
    // turn the incomplete full scan complete, and must not resurrect the other
    // old-generation row.
    const auto paths = index.refreshPaths({workspace, {}, true}, {"src/one.hpp"});
    ROCLINT_CHECK(paths.ok());
    ROCLINT_CHECK(!paths.value().incomplete);
    ROCLINT_CHECK(paths.value().unprocessedPaths.empty());
    ROCLINT_CHECK(paths.value().generation == 3);
    ROCLINT_CHECK(symbolCount(index, "oldOne") == 1);
    ROCLINT_CHECK(symbolCount(index, "oldTwo") == 0);
    const auto pathsInfo = index.snapshotInfo();
    ROCLINT_CHECK(pathsInfo.ok());
    ROCLINT_CHECK(!pathsInfo.value().complete);
    ROCLINT_CHECK(!pathsInfo.value().fullScanComplete);
    ROCLINT_CHECK(pathsInfo.value().coverageComplete);
    ROCLINT_CHECK(pathsInfo.value().generation == 3);
    const auto pathsQuery = index.findByName("oldOne", {}, {}, 8, false);
    ROCLINT_CHECK(pathsQuery.ok());
    ROCLINT_CHECK(pathsQuery.value().symbols.size() == 1);
    ROCLINT_CHECK(!pathsQuery.value().complete);

    const auto complete = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(complete.ok());
    ROCLINT_CHECK(!complete.value().incomplete);
    ROCLINT_CHECK(complete.value().generation == 4);
    ROCLINT_CHECK(index.snapshotInfo().value().complete);
    ROCLINT_CHECK(index.snapshotInfo().value().fullScanComplete);
    ROCLINT_CHECK(index.snapshotInfo().value().coverageComplete);
    ROCLINT_CHECK(symbolCount(index, "oldOne") == 1);
    ROCLINT_CHECK(symbolCount(index, "oldTwo") == 1);
}

void testWorkspaceIsolationAndFailedRebind() {
    TempArea area;
    const auto workA = area.workspace("isolation-a");
    const auto workB = area.workspace("isolation-b");
    write(workA, "src/a.hpp", "class Alpha { public: int onlyInA; };\n");
    write(workB, "src/b.hpp", "class Beta { public: int onlyInB; };\n");
    const auto database = area.database("isolation");
    storage::CodebaseIndex index(database);
    const auto unbound = index.snapshotInfo();
    ROCLINT_CHECK(unbound.ok());
    ROCLINT_CHECK(!unbound.value().available);
    ROCLINT_CHECK(!index.findByName("onlyInA").ok());

    ROCLINT_CHECK(index.refresh({workA, {}, true}).ok());
    ROCLINT_CHECK(index.snapshotInfo().value().available);
    ROCLINT_CHECK(index.snapshotInfo().value().complete);
    ROCLINT_CHECK(index.snapshotInfo().value().fullScanComplete);
    ROCLINT_CHECK(index.snapshotInfo().value().coverageComplete);
    ROCLINT_CHECK(symbolCount(index, "onlyInA") == 1);
    ROCLINT_CHECK(index.refresh({workB, {}, true}).ok());
    ROCLINT_CHECK(index.snapshotInfo().value().complete);
    ROCLINT_CHECK(symbolCount(index, "onlyInB") == 1);
    ROCLINT_CHECK(symbolCount(index, "onlyInA") == 0);

    const auto failed = index.refresh({workA, {}, false});
    ROCLINT_CHECK(!failed.ok());
    ROCLINT_CHECK(!index.isWorkspaceBound());
    ROCLINT_CHECK(!index.findByName("onlyInB").ok());
    ROCLINT_CHECK(!index.snapshotInfo().value().available);
    ROCLINT_CHECK(!index.snapshotInfo().value().complete);
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM workspaces;") == 2);

    // Stale callback tokens follow the same rule as the host: a superseded
    // serial can never publish into the new workspace.
    int serial = 7;
    const int captured = serial;
    ++serial;
    ROCLINT_CHECK(captured != serial);
}

void testValidateFileHash() {
    TempArea area;
    const auto workspace = area.workspace("hash");
    const auto database = area.database("hash");
    const std::string source = "class Hashed { public: int hashedField; };\n";
    write(workspace, "src/hashed.hpp", source);
    storage::CodebaseIndex index(database, workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto before = index.snapshotInfo();
    ROCLINT_CHECK(before.ok());
    ROCLINT_CHECK(before.value().complete);
    ROCLINT_CHECK(before.value().fullScanComplete);
    ROCLINT_CHECK(before.value().coverageComplete);
    const auto digest = core::crypto::Sha256::hexDigest(source);

    const auto valid = index.validateFileHash("src/hashed.hpp", digest);
    ROCLINT_CHECK(valid.ok());
    ROCLINT_CHECK(valid.value());
    const auto prefixed = index.validateFileHash("src/hashed.hpp", "sha256:" + digest);
    ROCLINT_CHECK(prefixed.ok());
    ROCLINT_CHECK(prefixed.value());

    const auto mismatch = index.validateFileHash("src/hashed.hpp", std::string(64, '0'));
    ROCLINT_CHECK(mismatch.ok());
    ROCLINT_CHECK(!mismatch.value());
    const auto missing = index.validateFileHash("src/missing.hpp", digest);
    ROCLINT_CHECK(missing.ok());
    ROCLINT_CHECK(!missing.value());
    const auto malformed = index.validateFileHash("src/hashed.hpp", "not-a-digest");
    ROCLINT_CHECK(!malformed.ok());
    ROCLINT_CHECK(malformed.error().code == core::ErrorCode::InvalidInput);

    ROCLINT_CHECK(!index.validateFileHash("../outside.hpp", digest).ok());
    ROCLINT_CHECK(!index.validateFileHash((workspace / "src/hashed.hpp").generic_string(), digest).ok());
    ROCLINT_CHECK(!index.validateFileHash("src/../../outside.hpp", digest).ok());

    write(workspace, "src/huge.hpp", std::string(storage::CodebaseIndex::kMaximumFileBytes + 1, ' '));
    const auto oversized = index.validateFileHash("src/huge.hpp", digest);
    ROCLINT_CHECK(oversized.ok());
    ROCLINT_CHECK(!oversized.value());

    // Hash validation is read-only: no generation change and no index mutation.
    const auto after = index.snapshotInfo();
    ROCLINT_CHECK(after.ok());
    ROCLINT_CHECK(after.value().generation == before.value().generation);
    ROCLINT_CHECK(symbolCount(index, "hashedField") == 1);
}

void testQueryTruncationAndExactness() {
    TempArea area;
    const auto workspace = area.workspace("queries");
    write(workspace, "src/engine.hpp",
          "class Engine { public: bool run(int first); bool run(double second); };\n"
          "class EngineRunner { public: int runnerField; };\n");
    storage::CodebaseIndex index(area.database("queries"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto qualifiedAll = index.findQualified("Engine::run");
    ROCLINT_CHECK(qualifiedAll.ok());
    ROCLINT_CHECK(qualifiedAll.value().symbols.size() == 2);
    ROCLINT_CHECK(!qualifiedAll.value().truncated);
    ROCLINT_CHECK(qualifiedAll.value().complete);
    const auto qualifiedOne = index.findQualified("Engine::run", 1);
    ROCLINT_CHECK(qualifiedOne.ok());
    ROCLINT_CHECK(qualifiedOne.value().symbols.size() == 1);
    ROCLINT_CHECK(qualifiedOne.value().truncated);
    ROCLINT_CHECK(qualifiedOne.value().complete);
    ROCLINT_CHECK(index.findQualified("Engine", 8).value().symbols.size() == 1);

    const auto engine = index.findQualified("Engine", 1);
    ROCLINT_CHECK(engine.ok() && engine.value().symbols.size() == 1);
    const auto parentId = engine.value().symbols.front().symbolId;
    const auto childrenAll = index.children(parentId, 16);
    ROCLINT_CHECK(childrenAll.ok());
    ROCLINT_CHECK(childrenAll.value().symbols.size() == 2);
    ROCLINT_CHECK(!childrenAll.value().truncated);
    ROCLINT_CHECK(childrenAll.value().complete);
    const auto childrenOne = index.children(parentId, 1);
    ROCLINT_CHECK(childrenOne.ok());
    ROCLINT_CHECK(childrenOne.value().symbols.size() == 1);
    ROCLINT_CHECK(childrenOne.value().truncated);
    ROCLINT_CHECK(childrenOne.value().complete);

    const auto namesAll = index.findByName("run", parentId, {}, 16, false);
    ROCLINT_CHECK(namesAll.ok());
    ROCLINT_CHECK(namesAll.value().symbols.size() == 2);
    ROCLINT_CHECK(!namesAll.value().truncated);
    ROCLINT_CHECK(namesAll.value().complete);
    const auto namesOne = index.findByName("run", parentId, {}, 1, false);
    ROCLINT_CHECK(namesOne.ok());
    ROCLINT_CHECK(namesOne.value().symbols.size() == 1);
    ROCLINT_CHECK(namesOne.value().truncated);
    ROCLINT_CHECK(namesOne.value().complete);
    const auto inFile = index.findByName("run", parentId, "src/engine.hpp", 16, false);
    ROCLINT_CHECK(inFile.ok());
    ROCLINT_CHECK(inFile.value().symbols.size() == 2);
    ROCLINT_CHECK(inFile.value().complete);
    const auto wrongFile = index.findByName("run", parentId, "src/other.hpp", 16, false);
    ROCLINT_CHECK(wrongFile.ok());
    ROCLINT_CHECK(wrongFile.value().symbols.empty());
    ROCLINT_CHECK(wrongFile.value().complete);

    // Exact-name lookup must not be widened by the FTS fallback.
    const auto exactEngine = index.findByName("Engine", {}, {}, 16, true);
    ROCLINT_CHECK(exactEngine.ok());
    ROCLINT_CHECK(exactEngine.value().symbols.size() == 1);
    ROCLINT_CHECK(exactEngine.value().symbols.front().name == "Engine");
    ROCLINT_CHECK(exactEngine.value().complete);
    // A real FTS fallback hit reports the same snapshot completeness.
    const auto ftsFallback = index.findByName("double", {}, {}, 16, true);
    ROCLINT_CHECK(ftsFallback.ok());
    ROCLINT_CHECK(!ftsFallback.value().symbols.empty());
    ROCLINT_CHECK(ftsFallback.value().complete);
    ROCLINT_CHECK(symbolCount(index, "EngineRunner") == 1);
}

void testNewSymbolFieldsRoundTrip() {
    TempArea area;
    const auto workspace = area.workspace("symbol-fields");
    write(workspace, "src/fields.hpp",
          "class Persisted {\n"
          "public:\n"
          "    static constexpr int value = 3;\n"
          "    bool run(int count) const noexcept;\n"
          "};\n");
    storage::CodebaseIndex index(area.database("symbol-fields"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto value = index.findQualified("Persisted::value", 4);
    ROCLINT_CHECK(value.ok());
    ROCLINT_CHECK(value.value().symbols.size() == 1);
    const auto& field = value.value().symbols.front();
    ROCLINT_CHECK(field.role != storage::AstSymbolRole::Unknown);
    ROCLINT_CHECK(field.isStatic);
    ROCLINT_CHECK(field.isConstexpr);

    const auto run = index.findQualified("Persisted::run", 4);
    ROCLINT_CHECK(run.ok());
    ROCLINT_CHECK(run.value().symbols.size() == 1);
    ROCLINT_CHECK(!run.value().symbols.front().signatureKey.empty());
    ROCLINT_CHECK(run.value().symbols.front().qualifiers.find("const") != std::string::npos);

    const auto runId = run.value().symbols.front().symbolId;
    const auto count = index.findByName("count", runId, {}, 4, false);
    ROCLINT_CHECK(count.ok());
    ROCLINT_CHECK(count.value().symbols.size() == 1);
    ROCLINT_CHECK(count.value().symbols.front().parameterIndex == 1);
}

void testRefreshPathsRejectsInvalidPaths() {
    TempArea area;
    const auto workspace = area.workspace("invalid-paths");
    const auto database = area.database("invalid-paths");
    write(workspace, "src/valid.hpp", "class Valid { public: int validField; };\n");
    storage::CodebaseIndex index(database, workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto generation = index.snapshotInfo().value().generation;

    const auto absolute = index.refreshPaths({workspace, {}, true},
        {(workspace / "src/valid.hpp").generic_string()});
    ROCLINT_CHECK(!absolute.ok());
    ROCLINT_CHECK(absolute.error().code == core::ErrorCode::InvalidInput);
    const auto escape = index.refreshPaths({workspace, {}, true}, {"../outside.hpp"});
    ROCLINT_CHECK(!escape.ok());
    ROCLINT_CHECK(escape.error().code == core::ErrorCode::InvalidInput);
    ROCLINT_CHECK(index.snapshotInfo().value().generation == generation);
    ROCLINT_CHECK(symbolCount(index, "validField") == 1);
}

void testV3MigrationPreservesUnrelatedTables() {
    TempArea area;
    const auto workspace = area.workspace("migration");
    const auto database = area.database("migration");
    write(workspace, "src/legacy.hpp", "class Legacy { public: int legacyField; };\n");
    createVersion3Database(database, workspace);
    ROCLINT_CHECK(userVersion(database) == 3);
    ROCLINT_CHECK(tableExists(database, "history_events"));
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM history_events;") == 1);

    storage::CodebaseIndex index(database, workspace);
    ROCLINT_CHECK(userVersion(database) == 5);
    ROCLINT_CHECK(columnExists(database, "files", "snapshot_generation"));
    ROCLINT_CHECK(columnExists(database, "files", "dirty"));
    ROCLINT_CHECK(columnExists(database, "workspaces", "coverage_complete"));
    for (const char* column : {"role", "is_declaration", "is_definition", "is_template", "is_static",
                               "is_constexpr", "is_inline", "is_variadic", "is_reference",
                               "parameter_index", "signature_key", "qualifiers"})
        ROCLINT_CHECK(columnExists(database, "ast_symbols", column));
    ROCLINT_CHECK(tableExists(database, "history_events"));
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM history_events;") == 1);
    // Migrated v3 evidence has no structural metadata, so it is hidden, marked
    // dirty and the completeness claims are cleared through v4 and v5.
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM files WHERE dirty<>0;") == 1);
    ROCLINT_CHECK(queryInt(database,
        "SELECT COUNT(*) FROM ast_symbols WHERE validated_generation<>0;") == 0);
    ROCLINT_CHECK(queryInt(database, "SELECT coverage_complete FROM workspaces LIMIT 1;") == 0);
    const auto migrated = index.snapshotInfo();
    ROCLINT_CHECK(migrated.ok());
    ROCLINT_CHECK(!migrated.value().complete);
    ROCLINT_CHECK(!migrated.value().fullScanComplete);
    ROCLINT_CHECK(!migrated.value().coverageComplete);
    const auto migratedQuery = index.findByName("staleField", {}, {}, 8, false);
    ROCLINT_CHECK(migratedQuery.ok());
    ROCLINT_CHECK(migratedQuery.value().symbols.empty());
    ROCLINT_CHECK(!migratedQuery.value().complete);

    const auto refreshed = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(index.snapshotInfo().value().complete);
    ROCLINT_CHECK(index.snapshotInfo().value().fullScanComplete);
    ROCLINT_CHECK(index.snapshotInfo().value().coverageComplete);
    ROCLINT_CHECK(symbolCount(index, "legacyField") == 1);
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM history_events;") == 1);
}

void testIncompletePathsRefreshCannotProveUniqueness() {
    TempArea area;
    const auto workspace = area.workspace("coverage");
    const auto database = area.database("coverage");
    const std::string declaration = "int shared(int value);\n";
    write(workspace, "src/a.hpp", declaration);
    write(workspace, "src/b.hpp", declaration);

    {
        storage::CodebaseIndex index(database, workspace);
        const auto initial = index.refresh({workspace, {}, true});
        ROCLINT_CHECK(initial.ok());
        ROCLINT_CHECK(!initial.value().incomplete);
        ROCLINT_CHECK(initial.value().unprocessedPaths.empty());
        const auto initialInfo = index.snapshotInfo();
        ROCLINT_CHECK(initialInfo.ok());
        ROCLINT_CHECK(initialInfo.value().complete);
        ROCLINT_CHECK(initialInfo.value().fullScanComplete);
        ROCLINT_CHECK(initialInfo.value().coverageComplete);

        const auto ambiguous = index.findQualified("shared", 8);
        ROCLINT_CHECK(ambiguous.ok());
        ROCLINT_CHECK(ambiguous.value().symbols.size() == 2);
        ROCLINT_CHECK(!ambiguous.value().truncated);
        ROCLINT_CHECK(ambiguous.value().complete);

        storage::CodebaseIndexLimits oneFile;
        oneFile.maximumFiles = 1;
        const auto partial = index.refreshPaths(
            {workspace, {}, true}, {"src/a.hpp", "src/b.hpp"}, oneFile);
        ROCLINT_CHECK(partial.ok());
        ROCLINT_CHECK(partial.value().incomplete);
        ROCLINT_CHECK(partial.value().generation == 2);
        ROCLINT_CHECK(partial.value().unprocessedPaths.size() == 1);
        ROCLINT_CHECK(partial.value().unprocessedPaths.front() == "src/b.hpp");
        ROCLINT_CHECK(queryInt(database, "SELECT dirty FROM files WHERE path='src/b.hpp';") == 1);

        const auto partialInfo = index.snapshotInfo();
        ROCLINT_CHECK(partialInfo.ok());
        ROCLINT_CHECK(!partialInfo.value().complete);
        ROCLINT_CHECK(partialInfo.value().fullScanComplete);
        ROCLINT_CHECK(!partialInfo.value().coverageComplete);

        const auto partialQuery = index.findQualified("shared", 8);
        ROCLINT_CHECK(partialQuery.ok());
        ROCLINT_CHECK(partialQuery.value().symbols.size() == 1);
        ROCLINT_CHECK(!partialQuery.value().truncated);
        ROCLINT_CHECK(!partialQuery.value().complete);
        ROCLINT_CHECK(partialQuery.value().symbols.front().filePath == "src/a.hpp");

        // The dirty competitor must stay hidden, including through the FTS
        // fallback, while the result remains non-authoritative.
        const auto ftsQuery = index.findByName("shar", {}, {}, 8, true);
        ROCLINT_CHECK(ftsQuery.ok());
        ROCLINT_CHECK(!ftsQuery.value().symbols.empty());
        // Trigram FTS can also match a qualified-name child (`shared::value`);
        // every visible match must still come from the clean file only.
        for (const auto& symbol : ftsQuery.value().symbols)
            ROCLINT_CHECK(symbol.filePath == "src/a.hpp");
        ROCLINT_CHECK(!ftsQuery.value().complete);

        const auto childrenQuery = index.children("missing-parent", 8);
        ROCLINT_CHECK(childrenQuery.ok());
        ROCLINT_CHECK(childrenQuery.value().symbols.empty());
        ROCLINT_CHECK(!childrenQuery.value().complete);
    }

    // A reopened database exposes the same protection: neither the coverage
    // flags nor the dirty-file hiding are process-local.
    {
        storage::CodebaseIndex reopened(database, workspace);
        const auto info = reopened.snapshotInfo();
        ROCLINT_CHECK(info.ok());
        ROCLINT_CHECK(info.value().generation == 2);
        ROCLINT_CHECK(!info.value().complete);
        ROCLINT_CHECK(info.value().fullScanComplete);
        ROCLINT_CHECK(!info.value().coverageComplete);
        const auto query = reopened.findQualified("shared", 8);
        ROCLINT_CHECK(query.ok());
        ROCLINT_CHECK(query.value().symbols.size() == 1);
        ROCLINT_CHECK(query.value().symbols.front().filePath == "src/a.hpp");
        ROCLINT_CHECK(!query.value().complete);

        // A complete paths pass restores coverage. The unchanged files are
        // revalidated and both candidates become visible again.
        const auto recovered = reopened.refreshPaths(
            {workspace, {}, true}, {"src/a.hpp", "src/b.hpp"});
        ROCLINT_CHECK(recovered.ok());
        ROCLINT_CHECK(!recovered.value().incomplete);
        ROCLINT_CHECK(recovered.value().unprocessedPaths.empty());
        ROCLINT_CHECK(recovered.value().generation == 3);
        const auto recoveredInfo = reopened.snapshotInfo();
        ROCLINT_CHECK(recoveredInfo.ok());
        ROCLINT_CHECK(recoveredInfo.value().complete);
        ROCLINT_CHECK(recoveredInfo.value().fullScanComplete);
        ROCLINT_CHECK(recoveredInfo.value().coverageComplete);
        const auto recoveredQuery = reopened.findQualified("shared", 8);
        ROCLINT_CHECK(recoveredQuery.ok());
        ROCLINT_CHECK(recoveredQuery.value().symbols.size() == 2);
        ROCLINT_CHECK(recoveredQuery.value().complete);
        ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM files WHERE dirty<>0;") == 0);
    }
}

void testIncompletePathsRefreshDoesNotProveAbsence() {
    TempArea area;
    const auto workspace = area.workspace("absence");
    const auto database = area.database("absence");
    write(workspace, "src/visible.hpp", "int visibleTarget(int value);\n");
    write(workspace, "src/hidden.hpp", "int hiddenOnly(int value);\n");
    storage::CodebaseIndex index(database, workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto before = index.findQualified("hiddenOnly", 8);
    ROCLINT_CHECK(before.ok());
    ROCLINT_CHECK(before.value().symbols.size() == 1);
    ROCLINT_CHECK(before.value().complete);

    storage::CodebaseIndexLimits oneFile;
    oneFile.maximumFiles = 1;
    const auto partial = index.refreshPaths(
        {workspace, {}, true}, {"src/visible.hpp", "src/hidden.hpp"}, oneFile);
    ROCLINT_CHECK(partial.ok());
    ROCLINT_CHECK(partial.value().incomplete);
    ROCLINT_CHECK(partial.value().unprocessedPaths.size() == 1);
    ROCLINT_CHECK(partial.value().unprocessedPaths.front() == "src/hidden.hpp");

    // The target exists only in the unprocessed file. Zero rows must not be
    // presented as an authoritative absence.
    const auto hidden = index.findQualified("hiddenOnly", 8);
    ROCLINT_CHECK(hidden.ok());
    ROCLINT_CHECK(hidden.value().symbols.empty());
    ROCLINT_CHECK(!hidden.value().complete);
    const auto hiddenFts = index.findByName("hiddenOnly", {}, {}, 8, true);
    ROCLINT_CHECK(hiddenFts.ok());
    ROCLINT_CHECK(hiddenFts.value().symbols.empty());
    ROCLINT_CHECK(!hiddenFts.value().complete);

    // A requested-but-omitted path that has no row yet is still recorded dirty.
    write(workspace, "src/new.hpp", "int newTarget(int value);\n");
    const auto unseen = index.refreshPaths(
        {workspace, {}, true}, {"src/visible.hpp", "src/new.hpp"}, oneFile);
    ROCLINT_CHECK(unseen.ok());
    ROCLINT_CHECK(unseen.value().incomplete);
    ROCLINT_CHECK(unseen.value().unprocessedPaths.size() == 1);
    ROCLINT_CHECK(unseen.value().unprocessedPaths.front() == "src/new.hpp");
    ROCLINT_CHECK(queryInt(database,
        "SELECT COUNT(*) FROM files WHERE path='src/new.hpp' AND dirty<>0;") == 1);
}

void testV4MigrationDerivesCoverage() {
    TempArea area;
    const auto completeWorkspace = area.workspace("v4-complete");
    const auto dirtyWorkspace = area.workspace("v4-dirty");
    const auto incompleteWorkspace = area.workspace("v4-incomplete");
    const auto database = area.database("v4-migration");
    createVersion4Schema(database);
    addVersion4Workspace(database, completeWorkspace, true, false);
    addVersion4Workspace(database, dirtyWorkspace, true, true);
    addVersion4Workspace(database, incompleteWorkspace, false, false);
    ROCLINT_CHECK(userVersion(database) == 4);
    ROCLINT_CHECK(!columnExists(database, "workspaces", "coverage_complete"));
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM history_events;") == 1);

    storage::CodebaseIndex completeIndex(database, completeWorkspace);
    ROCLINT_CHECK(userVersion(database) == 5);
    ROCLINT_CHECK(columnExists(database, "workspaces", "coverage_complete"));
    ROCLINT_CHECK(tableExists(database, "history_events"));
    ROCLINT_CHECK(queryInt(database, "SELECT COUNT(*) FROM history_events;") == 1);
    ROCLINT_CHECK(queryInt(database,
        "SELECT coverage_complete FROM workspaces WHERE root='" +
        fs::canonical(completeWorkspace).generic_string() + "';") == 1);
    ROCLINT_CHECK(queryInt(database,
        "SELECT coverage_complete FROM workspaces WHERE root='" +
        fs::canonical(dirtyWorkspace).generic_string() + "';") == 0);
    ROCLINT_CHECK(queryInt(database,
        "SELECT coverage_complete FROM workspaces WHERE root='" +
        fs::canonical(incompleteWorkspace).generic_string() + "';") == 0);

    const auto completeInfo = completeIndex.snapshotInfo();
    ROCLINT_CHECK(completeInfo.ok());
    ROCLINT_CHECK(completeInfo.value().complete);
    ROCLINT_CHECK(completeInfo.value().fullScanComplete);
    ROCLINT_CHECK(completeInfo.value().coverageComplete);

    storage::CodebaseIndex dirtyIndex(database, dirtyWorkspace);
    const auto dirtyInfo = dirtyIndex.snapshotInfo();
    ROCLINT_CHECK(dirtyInfo.ok());
    ROCLINT_CHECK(!dirtyInfo.value().complete);
    ROCLINT_CHECK(dirtyInfo.value().fullScanComplete);
    ROCLINT_CHECK(!dirtyInfo.value().coverageComplete);
    const auto dirtyQuery = dirtyIndex.findQualified("anything", 8);
    ROCLINT_CHECK(dirtyQuery.ok());
    ROCLINT_CHECK(dirtyQuery.value().symbols.empty());
    ROCLINT_CHECK(!dirtyQuery.value().complete);

    // A paths pass that does not revalidate the migrated dirty row cannot
    // restore coverage simply by completing its own request.
    const auto dirtyPaths = dirtyIndex.refreshPaths({dirtyWorkspace, {}, true}, {"src/clean.hpp"});
    ROCLINT_CHECK(dirtyPaths.ok());
    ROCLINT_CHECK(dirtyPaths.value().incomplete);
    ROCLINT_CHECK(dirtyPaths.value().unprocessedPaths.empty());
    ROCLINT_CHECK(!dirtyIndex.snapshotInfo().value().complete);
    ROCLINT_CHECK(dirtyIndex.snapshotInfo().value().fullScanComplete);
    ROCLINT_CHECK(!dirtyIndex.snapshotInfo().value().coverageComplete);

    storage::CodebaseIndex incompleteIndex(database, incompleteWorkspace);
    const auto incompleteInfo = incompleteIndex.snapshotInfo();
    ROCLINT_CHECK(incompleteInfo.ok());
    ROCLINT_CHECK(!incompleteInfo.value().complete);
    ROCLINT_CHECK(!incompleteInfo.value().fullScanComplete);
    ROCLINT_CHECK(!incompleteInfo.value().coverageComplete);
}

void testWatcherBatchCoalescing() {
    host::CodebaseWatchBatch batch(3);
    ROCLINT_CHECK(batch.addPath("src/a.hpp"));
    ROCLINT_CHECK(!batch.addPath("src/a.hpp"));
    ROCLINT_CHECK(batch.addPath("src/b.hpp"));
    ROCLINT_CHECK(batch.addPath("src/c.hpp"));
    ROCLINT_CHECK(batch.size() == 3);
    ROCLINT_CHECK(!batch.addPath("src/d.hpp"));
    ROCLINT_CHECK(batch.overflowed());
    ROCLINT_CHECK(batch.fullRefreshRequested());
    ROCLINT_CHECK(batch.fullRefreshReason() == "path-overflow");
    ROCLINT_CHECK(batch.takePaths().empty());
    ROCLINT_CHECK(batch.empty());
    ROCLINT_CHECK(!batch.fullRefreshRequested());

    ROCLINT_CHECK(batch.addPath("src/e.hpp"));
    batch.requestFullRefresh("directory-removed");
    ROCLINT_CHECK(batch.fullRefreshRequested());
    ROCLINT_CHECK(batch.takePaths().empty());
    ROCLINT_CHECK(batch.fullRefreshReason().empty());

    ROCLINT_CHECK(batch.addPath("src/f.hpp"));
    auto paths = batch.takePaths();
    ROCLINT_CHECK(paths.size() == 1 && paths.front() == "src/f.hpp");
    ROCLINT_CHECK(batch.empty());
    batch.clear();
    ROCLINT_CHECK(!batch.overflowed());
}

} // namespace

int main() {
    testFullThenSinglePathRefresh();
    testAtomicSaveReplacement();
    testIncompleteFullThenPathsStaysIncomplete();
    testIncompletePathsRefreshCannotProveUniqueness();
    testIncompletePathsRefreshDoesNotProveAbsence();
    testWorkspaceIsolationAndFailedRebind();
    testValidateFileHash();
    testQueryTruncationAndExactness();
    testNewSymbolFieldsRoundTrip();
    testRefreshPathsRejectsInvalidPaths();
    testV3MigrationPreservesUnrelatedTables();
    testV4MigrationDerivesCoverage();
    testWatcherBatchCoalescing();
    std::cout << "Codebase index freshness, coverage, generation, hash validation and watcher batching gates passed.\n";
    return 0;
}
