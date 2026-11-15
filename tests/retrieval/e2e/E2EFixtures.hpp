#pragma once

// =============================================================================
// Roclint Retrieval Subsystem — E2E Test Fixtures
// RAII Temp Workspace, SQLite Database Fixture, Mock Runtime Spy,
// Ephemeral Python Worker Generator, and Real Model Asset Prober.
// Conforms strictly to MSVC /W4 /w14061 /WX /utf-8 /permissive-.
// =============================================================================

#include "E2ETestHarness.hpp"

#include <sqlite3.h>

#include "roclint/core/Result.hpp"
#include "roclint/retrieval/IEmbeddingRuntime.hpp"
#include "roclint/retrieval/RetrievalTypes.hpp"
#include "retrieval/MockEmbeddingRuntime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace roclint::e2e {

namespace detail {

inline std::uint64_t uniqueId() noexcept {
    static std::atomic<std::uint64_t> s_counter{1};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (static_cast<std::uint64_t>(now) ^ (s_counter.fetch_add(1, std::memory_order_relaxed) << 32));
}

inline int currentPid() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

inline std::string getEnvVar(const char* name) {
#if defined(_WIN32)
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, name) == 0 && buf != nullptr) {
        std::string val(buf);
        free(buf);
        return val;
    }
    return "";
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
#endif
}

} // namespace detail

// =============================================================================
// 1. TempWorkspace — RAII Sandboxed Directory with Windows Retry Logic
// =============================================================================

class TempWorkspace {
public:
    explicit TempWorkspace(std::string_view prefix = "roclint_ws") {
        auto tmpBase = std::filesystem::temp_directory_path();
        std::string dirName = std::string(prefix) + "_" +
                              std::to_string(detail::currentPid()) + "_" +
                              std::to_string(detail::uniqueId());
        root_ = tmpBase / dirName;

        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
    }

    ~TempWorkspace() {
        clean();
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    TempWorkspace(TempWorkspace&& other) noexcept
        : root_(std::move(other.root_))
        , cleaned_(other.cleaned_)
    {
        other.cleaned_ = true;
    }

    TempWorkspace& operator=(TempWorkspace&& other) noexcept {
        if (this != &other) {
            clean();
            root_ = std::move(other.root_);
            cleaned_ = other.cleaned_;
            other.cleaned_ = true;
        }
        return *this;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    std::filesystem::path writeFile(const std::filesystem::path& relPath, std::string_view content) {
        auto fullPath = root_ / relPath;
        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);

        std::ofstream ofs(fullPath, std::ios::binary);
        if (!ofs.is_open()) {
            throw std::runtime_error("TempWorkspace: failed to open file for writing: " + fullPath.string());
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.close();
        return fullPath;
    }

    void removeFile(const std::filesystem::path& relPath) {
        auto fullPath = root_ / relPath;
        std::error_code ec;
        for (int attempt = 0; attempt < 5; ++attempt) {
            std::filesystem::remove(fullPath, ec);
            if (!std::filesystem::exists(fullPath, ec)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));
        }
    }

    [[nodiscard]] std::filesystem::path path(const std::filesystem::path& relPath) const {
        return root_ / relPath;
    }

    void clean() {
        if (cleaned_ || root_.empty()) return;
        std::error_code ec;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (!std::filesystem::exists(root_, ec)) {
                cleaned_ = true;
                return;
            }
            std::filesystem::remove_all(root_, ec);
            if (!ec) {
                cleaned_ = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));
        }
        cleaned_ = true;
    }

private:
    std::filesystem::path root_;
    bool cleaned_{false};
};

// =============================================================================
// 2. TestDatabaseFixture — RAII SQLite Database Manager & Legacy Seeder
// =============================================================================

class TestDatabaseFixture {
public:
    explicit TestDatabaseFixture(std::string_view name = "roclint_test") {
        auto tmpBase = std::filesystem::temp_directory_path();
        std::string fileName = std::string(name) + "_" +
                               std::to_string(detail::currentPid()) + "_" +
                               std::to_string(detail::uniqueId()) + ".db";
        dbPath_ = tmpBase / fileName;
        clean();
    }

    ~TestDatabaseFixture() {
        clean();
    }

    TestDatabaseFixture(const TestDatabaseFixture&) = delete;
    TestDatabaseFixture& operator=(const TestDatabaseFixture&) = delete;

    TestDatabaseFixture(TestDatabaseFixture&& other) noexcept
        : dbPath_(std::move(other.dbPath_))
        , cleaned_(other.cleaned_)
    {
        other.cleaned_ = true;
    }

    TestDatabaseFixture& operator=(TestDatabaseFixture&& other) noexcept {
        if (this != &other) {
            clean();
            dbPath_ = std::move(other.dbPath_);
            cleaned_ = other.cleaned_;
            other.cleaned_ = true;
        }
        return *this;
    }

    [[nodiscard]] const std::filesystem::path& dbPath() const noexcept {
        return dbPath_;
    }

    void clean() {
        if (dbPath_.empty()) return;
        std::error_code ec;
        auto walPath = std::filesystem::path(dbPath_.string() + "-wal");
        auto shmPath = std::filesystem::path(dbPath_.string() + "-shm");

        for (int attempt = 0; attempt < 5; ++attempt) {
            std::filesystem::remove(dbPath_, ec);
            std::filesystem::remove(walPath, ec);
            std::filesystem::remove(shmPath, ec);

            if (!std::filesystem::exists(dbPath_, ec)) {
                cleaned_ = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));
        }
        cleaned_ = true;
    }

    bool executeSql(std::string_view sql, std::string* outError = nullptr) {
        sqlite3* db = nullptr;
        int rc = sqlite3_open(dbPath_.string().c_str(), &db);
        if (rc != SQLITE_OK) {
            if (outError) *outError = sqlite3_errmsg(db);
            if (db) sqlite3_close(db);
            return false;
        }

        char* errmsg = nullptr;
        rc = sqlite3_exec(db, sql.data(), nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            if (outError && errmsg) *outError = errmsg;
            sqlite3_free(errmsg);
            sqlite3_close(db);
            return false;
        }

        sqlite3_close(db);
        return true;
    }

    [[nodiscard]] int queryInt(std::string_view sql, int defaultValue = -1) {
        sqlite3* db = nullptr;
        if (sqlite3_open(dbPath_.string().c_str(), &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return defaultValue;
        }

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_close(db);
            return defaultValue;
        }

        int result = defaultValue;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = sqlite3_column_int(stmt, 0);
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    [[nodiscard]] std::string queryString(std::string_view sql, std::string defaultValue = "") {
        sqlite3* db = nullptr;
        if (sqlite3_open(dbPath_.string().c_str(), &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return defaultValue;
        }

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_close(db);
            return defaultValue;
        }

        std::string result = std::move(defaultValue);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) {
                result = reinterpret_cast<const char*>(text);
            }
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    void seedLegacyV1Database(
        const std::filesystem::path& workspaceRoot,
        bool includeLegacyFts = true,
        bool includeCorruptExcerpts = false)
    {
        clean();
        sqlite3* db = nullptr;
        if (sqlite3_open(dbPath_.string().c_str(), &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            throw std::runtime_error("seedLegacyV1Database: failed to open SQLite database: " + dbPath_.string());
        }

        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        // Schema v1 tables (WITHOUT published_gen columns — simulating legacy database)
        const char* const kSchemaV1Sql =
            "CREATE TABLE IF NOT EXISTS retrieval_meta ("
            "    workspace_root TEXT NOT NULL PRIMARY KEY,"
            "    schema_version TEXT NOT NULL,"
            "    model_id TEXT NOT NULL,"
            "    model_revision TEXT NOT NULL,"
            "    dimensions INTEGER NOT NULL,"
            "    normalized INTEGER NOT NULL,"
            "    chunker_version TEXT NOT NULL,"
            "    preprocess_version TEXT NOT NULL,"
            "    snapshot_gen INTEGER NOT NULL DEFAULT 0,"
            "    coverage_complete INTEGER NOT NULL DEFAULT 0,"
            "    indexed_chunks INTEGER NOT NULL DEFAULT 0,"
            "    indexed_files INTEGER NOT NULL DEFAULT 0,"
            "    last_refresh_ms REAL NOT NULL DEFAULT 0"
            ");"
            "CREATE TABLE IF NOT EXISTS retrieval_chunks ("
            "    chunk_id TEXT NOT NULL PRIMARY KEY,"
            "    workspace_root TEXT NOT NULL,"
            "    relative_path TEXT NOT NULL,"
            "    content_hash TEXT NOT NULL,"
            "    source_kind INTEGER NOT NULL,"
            "    byte_start INTEGER NOT NULL,"
            "    byte_end INTEGER NOT NULL,"
            "    line_start INTEGER NOT NULL,"
            "    line_end INTEGER NOT NULL,"
            "    ast_symbol_id TEXT,"
            "    ast_qualified TEXT,"
            "    ast_link_status INTEGER NOT NULL DEFAULT 0,"
            "    snapshot_gen INTEGER NOT NULL,"
            "    heading_context TEXT NOT NULL DEFAULT '',"
            "    has_ast_hint INTEGER NOT NULL DEFAULT 0,"
            "    chunk_text TEXT"
            ");"
            "CREATE TABLE IF NOT EXISTS retrieval_sources ("
            "    workspace_root TEXT NOT NULL,"
            "    relative_path TEXT NOT NULL,"
            "    content_hash TEXT NOT NULL,"
            "    source_kind INTEGER NOT NULL,"
            "    snapshot_gen INTEGER NOT NULL,"
            "    PRIMARY KEY (workspace_root, relative_path)"
            ");"
            "CREATE TABLE IF NOT EXISTS retrieval_embeddings ("
            "    chunk_id TEXT NOT NULL PRIMARY KEY,"
            "    embedding BLOB NOT NULL"
            ");";

        sqlite3_exec(db, kSchemaV1Sql, nullptr, nullptr, nullptr);

        if (includeLegacyFts) {
            // Authentic legacy FTS table lacking source_kind, snapshot_gen, published_gen
            const char* const kLegacyFtsSql =
                "CREATE VIRTUAL TABLE IF NOT EXISTS retrieval_fts USING fts5("
                "    chunk_id UNINDEXED, workspace_root UNINDEXED, chunk_text,"
                "    content='retrieval_chunks', content_rowid='rowid'"
                ");";
            sqlite3_exec(db, kLegacyFtsSql, nullptr, nullptr, nullptr);
        }

        // Insert legacy metadata row with schema_version = '1'
        std::string metaInsert =
            "INSERT INTO retrieval_meta "
            "(workspace_root, schema_version, model_id, model_revision, dimensions, "
            " normalized, chunker_version, preprocess_version, snapshot_gen, coverage_complete) "
            "VALUES ('" + workspaceRoot.generic_string() + "', '1', 'test-mock-embedding', "
            "'mock-rev-0000000000000000000000000000000000000000', 768, 1, "
            "'text_chunker.v1', 'mock.v1', 1, 1);";
        sqlite3_exec(db, metaInsert.c_str(), nullptr, nullptr, nullptr);

        if (includeCorruptExcerpts) {
            // Seed corrupted "0" excerpt reproducing Finding 1 & Bob's corrupt-cache.db
            std::string corruptChunk =
                "INSERT INTO retrieval_chunks "
                "(chunk_id, workspace_root, relative_path, content_hash, source_kind, "
                " byte_start, byte_end, line_start, line_end, snapshot_gen, chunk_text) "
                "VALUES ('corrupt_chunk_001', '" + workspaceRoot.generic_string() + "', "
                "'a.cpp', 'h_corrupt', 0, 0, 10, 1, 1, 1, '0');";
            sqlite3_exec(db, corruptChunk.c_str(), nullptr, nullptr, nullptr);

            if (includeLegacyFts) {
                std::string corruptFts =
                    "INSERT INTO retrieval_fts (rowid, chunk_id, workspace_root, chunk_text) "
                    "VALUES (1, 'corrupt_chunk_001', '" + workspaceRoot.generic_string() + "', '0');";
                sqlite3_exec(db, corruptFts.c_str(), nullptr, nullptr, nullptr);
            }
        }

        sqlite3_close(db);
    }

    void seedRawUtf8Chunk(std::string_view chunkId, std::string_view hexBytes) {
        sqlite3* db = nullptr;
        if (sqlite3_open(dbPath_.string().c_str(), &db) == SQLITE_OK) {
            std::string sql = "UPDATE retrieval_chunks SET chunk_text = CAST(X'" +
                              std::string(hexBytes) + "' AS TEXT) WHERE chunk_id = '" +
                              std::string(chunkId) + "';";
            sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
            sqlite3_close(db);
        }
    }

private:
    std::filesystem::path dbPath_;
    bool cleaned_{false};
};

// =============================================================================
// 3. MockRuntimeFixture — Observable Spy Wrapper & Fault Injector
// =============================================================================

class MockRuntimeFixture final : public roclint::retrieval::IEmbeddingRuntime {
public:
    explicit MockRuntimeFixture(bool ready = true)
        : mock_(ready)
    {}

    [[nodiscard]] std::size_t callCount() const noexcept { return callCount_.load(); }
    [[nodiscard]] std::size_t textsEmbeddedCount() const noexcept { return textsEmbeddedCount_.load(); }

    void resetCounts() noexcept {
        callCount_.store(0);
        textsEmbeddedCount_.store(0);
    }

    void setFailOnCall(std::size_t callIndex, roclint::core::ErrorCode code = roclint::core::ErrorCode::ProviderFailed) {
        failOnCall_ = callIndex;
        failErrorCode_ = code;
    }

    void clearFail() noexcept {
        failOnCall_.reset();
    }

    void setLatencyMs(int ms) noexcept {
        latencyMs_.store(ms);
    }

    void setReady(bool ready) {
        mock_.setReady(ready);
    }

    roclint::core::Result<roclint::retrieval::EmbeddingBatch> embed(
        const std::vector<std::string>& texts,
        const roclint::retrieval::EmbedOptions& opts = {}) override
    {
        const std::size_t currentCall = ++callCount_;
        textsEmbeddedCount_ += texts.size();

        if (failOnCall_ && currentCall == *failOnCall_) {
            return roclint::core::makeError(failErrorCode_, "Injected mock runtime failure");
        }

        const int latency = latencyMs_.load();
        if (latency > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(latency));
        }

        return mock_.embed(texts, opts);
    }

    [[nodiscard]] roclint::retrieval::EmbeddingRuntimeState state() const noexcept override {
        return mock_.state();
    }

    [[nodiscard]] roclint::retrieval::IndexPolicy policy() const override {
        return mock_.policy();
    }

    [[nodiscard]] std::string statusMessage() const override {
        return mock_.statusMessage();
    }

private:
    retrieval_test::MockEmbeddingRuntime mock_;
    std::atomic<std::size_t> callCount_{0};
    std::atomic<std::size_t> textsEmbeddedCount_{0};
    std::atomic<int> latencyMs_{0};
    std::optional<std::size_t> failOnCall_{std::nullopt};
    roclint::core::ErrorCode failErrorCode_{roclint::core::ErrorCode::ProviderFailed};
};

// =============================================================================
// 4. MockWorkerScriptFixture — Ephemeral Python IPC Fault Injector
// =============================================================================

enum class MockWorkerBehavior : int {
    NormalEcho = 0,
    WrongModelId = 1,
    WrongRevision = 2,
    WrongDimensions = 3,
    MissingPooling = 4,
    ContradictoryPooling = 5,
    HangingProcess = 6,
    CrashOnPing = 7,
    MalformedJson = 8
};

class MockWorkerScriptFixture {
public:
    explicit MockWorkerScriptFixture(MockWorkerBehavior behavior) {
        auto tmpBase = std::filesystem::temp_directory_path();
        std::string fileName = "mock_worker_" +
                               std::to_string(detail::currentPid()) + "_" +
                               std::to_string(detail::uniqueId()) + ".py";
        scriptPath_ = tmpBase / fileName;

        std::string pythonCode;
        switch (behavior) {
            case MockWorkerBehavior::NormalEcho:
                pythonCode =
                    "import sys, json\n"
                    "for line in sys.stdin:\n"
                    "    line = line.strip()\n"
                    "    if not line: continue\n"
                    "    try:\n"
                    "        req = json.loads(line)\n"
                    "        op = req.get('op', '')\n"
                    "        req_id = req.get('id', '0')\n"
                    "        if op == 'ping':\n"
                    "            print(json.dumps({'id': req_id, 'ok': True, "
                    "'model_id': 'ibm-granite/granite-embedding-311m-multilingual-r2', "
                    "'model_revision': '44399559930365213510b1ee2eb15ded83374f0e', "
                    "'dim': 768, 'pooling': 'cls'}), flush=True)\n"
                    "        elif op == 'embed':\n"
                    "            texts = req.get('texts', [])\n"
                    "            data = [0.0] * (len(texts) * 768)\n"
                    "            print(json.dumps({'id': req_id, 'ok': True, 'dim': 768, 'rows': len(texts), 'embeddings': data}), flush=True)\n"
                    "        elif op == 'quit':\n"
                    "            break\n"
                    "    except Exception as e:\n"
                    "        print(json.dumps({'id': '0', 'ok': False, 'error': str(e)}), flush=True)\n";
                break;

            case MockWorkerBehavior::WrongModelId:
                pythonCode =
                    "import sys, json\n"
                    "for line in sys.stdin:\n"
                    "    line = line.strip()\n"
                    "    if not line: continue\n"
                    "    req = json.loads(line)\n"
                    "    if req.get('op') == 'quit': break\n"
                    "    print(json.dumps({'id': req.get('id', '0'), 'ok': True, "
                    "'model_id': 'not-the-selected-model', "
                    "'model_revision': 'unverified-revision', 'dim': 768, 'pooling': 'cls'}), flush=True)\n";
                break;

            case MockWorkerBehavior::WrongRevision:
                pythonCode =
                    "import sys, json\n"
                    "for line in sys.stdin:\n"
                    "    line = line.strip()\n"
                    "    if not line: continue\n"
                    "    req = json.loads(line)\n"
                    "    if req.get('op') == 'quit': break\n"
                    "    print(json.dumps({'id': req.get('id', '0'), 'ok': True, "
                    "'model_id': 'ibm-granite/granite-embedding-311m-multilingual-r2', "
                    "'model_revision': 'unverified-revision', 'dim': 768, 'pooling': 'cls'}), flush=True)\n";
                break;

            case MockWorkerBehavior::WrongDimensions:
                pythonCode =
                    "import sys, json\n"
                    "for line in sys.stdin:\n"
                    "    line = line.strip()\n"
                    "    if not line: continue\n"
                    "    req = json.loads(line)\n"
                    "    if req.get('op') == 'quit': break\n"
                    "    print(json.dumps({'id': req.get('id', '0'), 'ok': True, "
                    "'model_id': 'ibm-granite/granite-embedding-311m-multilingual-r2', "
                    "'model_revision': '44399559930365213510b1ee2eb15ded83374f0e', 'dim': 512, 'pooling': 'cls'}), flush=True)\n";
                break;

            case MockWorkerBehavior::MissingPooling:
                pythonCode =
                    "import sys, json\n"
                    "for line in sys.stdin:\n"
                    "    line = line.strip()\n"
                    "    if not line: continue\n"
                    "    req = json.loads(line)\n"
                    "    if req.get('op') == 'quit': break\n"
                    "    print(json.dumps({'id': req.get('id', '0'), 'ok': True, "
                    "'model_id': 'ibm-granite/granite-embedding-311m-multilingual-r2', "
                    "'model_revision': '44399559930365213510b1ee2eb15ded83374f0e', 'dim': 768, 'pooling': 'unknown'}), flush=True)\n";
                break;

            case MockWorkerBehavior::ContradictoryPooling:
                pythonCode =
                    "import sys, json\n"
                    "for line in sys.stdin:\n"
                    "    line = line.strip()\n"
                    "    if not line: continue\n"
                    "    req = json.loads(line)\n"
                    "    if req.get('op') == 'quit': break\n"
                    "    print(json.dumps({'id': req.get('id', '0'), 'ok': True, "
                    "'model_id': 'ibm-granite/granite-embedding-311m-multilingual-r2', "
                    "'model_revision': '44399559930365213510b1ee2eb15ded83374f0e', 'dim': 768, 'pooling': 'both'}), flush=True)\n";
                break;

            case MockWorkerBehavior::HangingProcess:
                pythonCode =
                    "import time\n"
                    "time.sleep(3600)\n";
                break;

            case MockWorkerBehavior::CrashOnPing:
                pythonCode =
                    "import sys\n"
                    "sys.exit(1)\n";
                break;

            case MockWorkerBehavior::MalformedJson:
                pythonCode =
                    "import sys\n"
                    "print('NOT_VALID_JSON_STRING{{{', flush=True)\n";
                break;
        }

        std::ofstream ofs(scriptPath_, std::ios::binary);
        if (!ofs.is_open()) {
            throw std::runtime_error("MockWorkerScriptFixture: failed to write script: " + scriptPath_.string());
        }
        ofs.write(pythonCode.data(), static_cast<std::streamsize>(pythonCode.size()));
        ofs.close();
    }

    ~MockWorkerScriptFixture() {
        std::error_code ec;
        std::filesystem::remove(scriptPath_, ec);
    }

    MockWorkerScriptFixture(const MockWorkerScriptFixture&) = delete;
    MockWorkerScriptFixture& operator=(const MockWorkerScriptFixture&) = delete;

    MockWorkerScriptFixture(MockWorkerScriptFixture&& other) noexcept
        : scriptPath_(std::move(other.scriptPath_))
    {}

    MockWorkerScriptFixture& operator=(MockWorkerScriptFixture&& other) noexcept {
        if (this != &other) {
            std::error_code ec;
            std::filesystem::remove(scriptPath_, ec);
            scriptPath_ = std::move(other.scriptPath_);
        }
        return *this;
    }

    [[nodiscard]] const std::filesystem::path& scriptPath() const noexcept {
        return scriptPath_;
    }

private:
    std::filesystem::path scriptPath_;
};

// =============================================================================
// 5. RealModelValidationFixture — Pinned Granite Asset Prober
// =============================================================================

struct ModelAssetStatus {
    bool available{false};
    std::string skipReason;
    std::filesystem::path modelDir;
    std::string pythonExecutable{"python"};
};

class RealModelValidationFixture {
public:
    [[nodiscard]] static ModelAssetStatus probeModelAssets(const std::string& customModelDir = "") {
        ModelAssetStatus s;
        std::filesystem::path candidateDir;

        if (!customModelDir.empty()) {
            candidateDir = customModelDir;
        } else if (!TestRunner::instance().options().modelDir.empty()) {
            candidateDir = TestRunner::instance().options().modelDir;
        } else {
            std::string envDir = detail::getEnvVar("ROCLINT_MODEL_DIR");
            if (!envDir.empty()) {
                candidateDir = envDir;
            } else {
#if defined(_WIN32)
                std::string localApp = detail::getEnvVar("LOCALAPPDATA");
                if (!localApp.empty()) {
                    candidateDir = std::filesystem::path(localApp) / "roclint" / "Roclint" / "models" / "embedding";
                } else {
                    candidateDir = std::filesystem::path("C:/Users/Default/AppData/Local/roclint/Roclint/models/embedding");
                }
#else
                std::string home = detail::getEnvVar("HOME");
                if (!home.empty()) {
                    candidateDir = std::filesystem::path(home) / ".local" / "share" / "roclint" / "models" / "embedding";
                } else {
                    candidateDir = "/var/lib/roclint/models/embedding";
                }
#endif
            }
        }

        s.modelDir = candidateDir;

        std::error_code ec;
        if (!std::filesystem::is_directory(candidateDir, ec)) {
            s.available = false;
            s.skipReason = "Granite model directory does not exist: " + candidateDir.string();
            return s;
        }

        const std::vector<std::string> requiredFiles = {
            "model.safetensors",
            "config.json",
            "1_Pooling/config.json",
            "tokenizer.json"
        };

        for (const auto& relFile : requiredFiles) {
            auto fullPath = candidateDir / relFile;
            if (!std::filesystem::exists(fullPath, ec)) {
                s.available = false;
                s.skipReason = "Missing required Granite model asset: " + relFile + " in " + candidateDir.string();
                return s;
            }
        }

        s.available = true;
        return s;
    }

    [[nodiscard]] static ModelAssetStatus requireOrSkip(const std::string& customModelDir = "") {
        auto status = probeModelAssets(customModelDir);
        if (!status.available) {
            throw NotRunException(status.skipReason);
        }
        return status;
    }
};

} // namespace roclint::e2e
