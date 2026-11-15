#include "roclint/retrieval/RetrievalIndex.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::retrieval {
namespace {

// ---------------------------------------------------------------------------
// SQLite RAII wrappers
// ---------------------------------------------------------------------------

struct Stmt {
    sqlite3_stmt* s{nullptr};
    ~Stmt() { sqlite3_finalize(s); }
    bool ok() const noexcept { return s != nullptr; }
    int step() { return sqlite3_step(s); }
    void reset() { sqlite3_reset(s); sqlite3_clear_bindings(s); }
};

static core::Status sqlErr(sqlite3* db, int rc, std::string_view context) {
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW)
        return core::Status::success();
    return core::makeError(core::ErrorCode::DbError,
        std::string(context) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)));
}

static core::Status exec(sqlite3* db, const char* sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (errmsg) {
        std::string msg = "exec(" + std::string(sql, std::min(std::strlen(sql), std::size_t{60})) + "): " + errmsg;
        sqlite3_free(errmsg);
        return core::makeError(core::ErrorCode::DbError, std::move(msg));
    }
    return sqlErr(db, rc, "exec");
}

static core::Status prepareStmt(sqlite3* db, const char* sql, Stmt& out) {
    int rc = sqlite3_prepare_v2(db, sql, -1, &out.s, nullptr);
    return sqlErr(db, rc, sql);
}

// ---------------------------------------------------------------------------
// File hashing
// ---------------------------------------------------------------------------

static std::string hashFileContents(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 64 * 1024 * 1024) return {};
    std::FILE* f = nullptr;
#if defined(_WIN32)
    _wfopen_s(&f, path.c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return {};
    std::vector<char> buf(static_cast<std::size_t>(size));
    std::size_t rd = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (rd == 0) return {};
    return core::crypto::sha256Hex(std::string_view(buf.data(), rd));
}

// ---------------------------------------------------------------------------
// Vector math
// ---------------------------------------------------------------------------

static float innerProduct(const float* a, const float* b, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

// v3 distinguishes reusable, fully indexed sources from partial/failed work.
static const std::string kSchemaVersionText = std::to_string(RetrievalIndex::kSchemaVersion);

// RRF maximum score for a single-branch query over a two-branch index:
//   rank 1 in one branch only → 1/(60+1) ≈ 0.01639
//   rank 1 in both branches   → 1/(60+1) + 1/(60+1) ≈ 0.03279
//
// The default minFusedScore must be < this maximum. 0.0 is the fail-safe
// (all candidates pass); callers should set a meaningful threshold after
// evaluating retrieval quality.
//
// The old default of 0.05 was larger than the two-branch RRF maximum and
// therefore silently suppressed every result. The fix is to use 0.0 as the
// default in RetrievalOptions (defined in the header) and let RetrievalEngine
// apply its own (lower) threshold after quality evaluation.

static constexpr const char* kCreateMeta = R"sql(
CREATE TABLE IF NOT EXISTS retrieval_meta (
    workspace_root    TEXT NOT NULL PRIMARY KEY,
    schema_version    TEXT NOT NULL,
    model_id          TEXT NOT NULL,
    model_revision    TEXT NOT NULL,
    dimensions        INTEGER NOT NULL,
    normalized        INTEGER NOT NULL,
    chunker_version   TEXT NOT NULL,
    preprocess_version TEXT NOT NULL,
    snapshot_gen      INTEGER NOT NULL DEFAULT 0,
    published_gen     INTEGER NOT NULL DEFAULT 0,
    coverage_complete INTEGER NOT NULL DEFAULT 0,
    indexed_chunks    INTEGER NOT NULL DEFAULT 0,
    indexed_files     INTEGER NOT NULL DEFAULT 0,
    last_refresh_ms   REAL NOT NULL DEFAULT 0
);
)sql";

// retrieval_sources uses a compound primary key that includes snapshot_gen so
// that a pending (unpublished) row for a path coexists with the previous
// published row.  rollbackGeneration() deletes only the pending rows.
// commitGeneration() promotes pending rows and deletes superseded published rows.
static constexpr const char* kCreateSources = R"sql(
CREATE TABLE IF NOT EXISTS retrieval_sources (
    workspace_root   TEXT NOT NULL,
    relative_path    TEXT NOT NULL,
    content_hash     TEXT NOT NULL,
    source_kind      INTEGER NOT NULL,
    snapshot_gen     INTEGER NOT NULL,
    published_gen    INTEGER NOT NULL DEFAULT 0,
    indexed_complete INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (workspace_root, relative_path, snapshot_gen)
);
CREATE INDEX IF NOT EXISTS idx_sources_ws_pubgen
    ON retrieval_sources(workspace_root, published_gen);
)sql";

static constexpr const char* kCreateChunks = R"sql(
CREATE TABLE IF NOT EXISTS retrieval_chunks (
    chunk_id         TEXT NOT NULL,
    workspace_root   TEXT NOT NULL,
    relative_path    TEXT NOT NULL,
    content_hash     TEXT NOT NULL,
    source_kind      INTEGER NOT NULL,
    byte_start       INTEGER NOT NULL,
    byte_end         INTEGER NOT NULL,
    line_start       INTEGER NOT NULL,
    line_end         INTEGER NOT NULL,
    ast_symbol_id    TEXT,
    ast_qualified    TEXT,
    ast_link_status  INTEGER NOT NULL DEFAULT 0,
    snapshot_gen     INTEGER NOT NULL,
    published_gen    INTEGER NOT NULL DEFAULT 0,
    heading_context  TEXT NOT NULL DEFAULT '',
    has_ast_hint     INTEGER NOT NULL DEFAULT 0,
    chunk_text       TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (chunk_id, snapshot_gen)
);
CREATE INDEX IF NOT EXISTS idx_chunks_ws_gen
    ON retrieval_chunks(workspace_root, snapshot_gen);
CREATE INDEX IF NOT EXISTS idx_chunks_ws_pubgen
    ON retrieval_chunks(workspace_root, published_gen);
CREATE INDEX IF NOT EXISTS idx_chunks_ws_path_pub
    ON retrieval_chunks(workspace_root, relative_path, published_gen);
)sql";

static constexpr const char* kCreateEmbeddings = R"sql(
CREATE TABLE IF NOT EXISTS retrieval_embeddings (
    chunk_id     TEXT NOT NULL,
    snapshot_gen INTEGER NOT NULL,
    embedding    BLOB NOT NULL,
    PRIMARY KEY (chunk_id, snapshot_gen),
    FOREIGN KEY (chunk_id, snapshot_gen)
        REFERENCES retrieval_chunks(chunk_id, snapshot_gen)
        ON UPDATE CASCADE ON DELETE CASCADE
);
)sql";

static constexpr const char* kCreateFts = R"sql(
CREATE VIRTUAL TABLE IF NOT EXISTS retrieval_fts USING fts5(
    chunk_id UNINDEXED,
    workspace_root UNINDEXED,
    source_kind UNINDEXED,
    snapshot_gen UNINDEXED,
    published_gen UNINDEXED,
    chunk_text,
    tokenize='porter ascii'
);
)sql";

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct RetrievalIndex::Impl {
    sqlite3* db{nullptr};
    std::string workspaceRoot{};
    IndexPolicy policy_{};
    std::uint64_t currentGen_{0};    // Last successfully committed generation.
    std::uint64_t publishedGen_{0};  // Last published (reader-visible) generation.
    std::uint64_t pendingGen_{0};
    bool generationOpen_{false};
    RefreshType refreshType_{RefreshType::Full};
    bool schemaReady_{false};
    std::filesystem::path dbPath_{};

    ~Impl() { if (db) sqlite3_close(db); }

    // Migrate an existing schema-v1 database to schema-v2.
    // Safe to call on a v2 database (ADD COLUMN IF NOT EXISTS is not available
    // on older SQLite; we guard by checking the column list first).
    // Inspect a table's columns via PRAGMA table_info.
    // Returns true if the table exists, populates hasCol for each column name.
    // Returns false if the table does not exist.
    struct ColDetails {
        bool exists{false};
        int pk{0};
    };

    bool getColumnDetails(const char* tableName, const char* colName, ColDetails& details) {
        details.exists = false;
        details.pk = 0;
        bool tableExists = false;
        std::string sql = std::string("PRAGMA table_info(") + tableName + ")";
        Stmt pragma;
        if (!prepareStmt(db, sql.c_str(), pragma).ok()) return false;
        while (pragma.step() == SQLITE_ROW) {
            tableExists = true;
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(pragma.s, 1));
            if (!name) continue;
            if (std::string_view(name) == colName) {
                details.exists = true;
                details.pk = sqlite3_column_int(pragma.s, 5);
            }
        }
        return tableExists;
    }

    core::Status migrateSchemaIfNeeded() {
        ColDetails chunksPubGen, chunksText, chunksSnapGen;
        bool chunksExists = getColumnDetails("retrieval_chunks", "published_gen", chunksPubGen);
        getColumnDetails("retrieval_chunks", "chunk_text", chunksText);
        getColumnDetails("retrieval_chunks", "snapshot_gen", chunksSnapGen);

        ColDetails sourcesPubGen, sourcesSnapGen;
        bool sourcesExists = getColumnDetails("retrieval_sources", "published_gen", sourcesPubGen);
        getColumnDetails("retrieval_sources", "snapshot_gen", sourcesSnapGen);

        ColDetails metaPubGen;
        bool metaExists = getColumnDetails("retrieval_meta", "published_gen", metaPubGen);

        ColDetails embSnapGen;
        bool embExists = getColumnDetails("retrieval_embeddings", "snapshot_gen", embSnapGen);

        if (!chunksExists && !sourcesExists && !metaExists)
            return core::Status::success();

        // 1. If retrieval_chunks has old single primary key (snapshot_gen is not in PK)
        if (chunksExists && chunksSnapGen.pk == 0) {
            if (auto s = exec(db,
                "CREATE TABLE IF NOT EXISTS retrieval_chunks_migrated ("
                "    chunk_id         TEXT NOT NULL,"
                "    workspace_root   TEXT NOT NULL,"
                "    relative_path    TEXT NOT NULL,"
                "    content_hash     TEXT NOT NULL,"
                "    source_kind      INTEGER NOT NULL,"
                "    byte_start       INTEGER NOT NULL,"
                "    byte_end         INTEGER NOT NULL,"
                "    line_start       INTEGER NOT NULL,"
                "    line_end         INTEGER NOT NULL,"
                "    ast_symbol_id    TEXT,"
                "    ast_qualified    TEXT,"
                "    ast_link_status  INTEGER NOT NULL DEFAULT 0,"
                "    snapshot_gen     INTEGER NOT NULL,"
                "    published_gen    INTEGER NOT NULL DEFAULT 0,"
                "    heading_context  TEXT NOT NULL DEFAULT '',"
                "    has_ast_hint     INTEGER NOT NULL DEFAULT 0,"
                "    chunk_text       TEXT NOT NULL DEFAULT '',"
                "    PRIMARY KEY (chunk_id, snapshot_gen)"
                ");"); !s.ok()) return s;

            std::string copySql = chunksPubGen.exists ?
                "INSERT OR IGNORE INTO retrieval_chunks_migrated "
                "SELECT chunk_id, workspace_root, relative_path, content_hash, source_kind, "
                "       byte_start, byte_end, line_start, line_end, "
                "       ast_symbol_id, ast_qualified, ast_link_status, "
                "       snapshot_gen, CASE WHEN published_gen > 0 THEN published_gen ELSE snapshot_gen END, "
                "       heading_context, has_ast_hint, chunk_text "
                "FROM retrieval_chunks;" :
                "INSERT OR IGNORE INTO retrieval_chunks_migrated "
                "SELECT chunk_id, workspace_root, relative_path, content_hash, source_kind, "
                "       byte_start, byte_end, line_start, line_end, "
                "       ast_symbol_id, ast_qualified, ast_link_status, "
                "       snapshot_gen, snapshot_gen, "
                "       heading_context, has_ast_hint, " + std::string(chunksText.exists ? "chunk_text" : "''") + " "
                "FROM retrieval_chunks;";
            if (auto s = exec(db, copySql.c_str()); !s.ok()) return s;

            // Recreate embeddings if snapshot_gen is not in PK
            if (embExists && embSnapGen.pk == 0) {
                if (auto s = exec(db,
                    "CREATE TABLE IF NOT EXISTS retrieval_embeddings_migrated ("
                    "    chunk_id     TEXT NOT NULL,"
                    "    snapshot_gen INTEGER NOT NULL,"
                    "    embedding    BLOB NOT NULL,"
                    "    PRIMARY KEY (chunk_id, snapshot_gen),"
                    "    FOREIGN KEY (chunk_id, snapshot_gen)"
                    "        REFERENCES retrieval_chunks(chunk_id, snapshot_gen)"
                    "        ON UPDATE CASCADE ON DELETE CASCADE"
                    ");"); !s.ok()) return s;

                if (auto s = exec(db,
                    "INSERT OR IGNORE INTO retrieval_embeddings_migrated (chunk_id, snapshot_gen, embedding) "
                    "SELECT e.chunk_id, COALESCE(c.snapshot_gen, 1), e.embedding "
                    "FROM retrieval_embeddings e "
                    "LEFT JOIN retrieval_chunks c ON c.chunk_id = e.chunk_id;"); !s.ok()) return s;

                if (auto s = exec(db, "DROP TABLE retrieval_embeddings;"); !s.ok()) return s;
                if (auto s = exec(db, "ALTER TABLE retrieval_embeddings_migrated RENAME TO retrieval_embeddings;"); !s.ok()) return s;
            }

            if (auto s = exec(db, "DROP TABLE retrieval_chunks;"); !s.ok()) return s;
            if (auto s = exec(db, "ALTER TABLE retrieval_chunks_migrated RENAME TO retrieval_chunks;"); !s.ok()) return s;
            if (auto s = exec(db, "CREATE INDEX IF NOT EXISTS idx_chunks_ws_gen ON retrieval_chunks(workspace_root, snapshot_gen);"); !s.ok()) return s;
            if (auto s = exec(db, "CREATE INDEX IF NOT EXISTS idx_chunks_ws_pubgen ON retrieval_chunks(workspace_root, published_gen);"); !s.ok()) return s;
            if (auto s = exec(db, "CREATE INDEX IF NOT EXISTS idx_chunks_ws_path_pub ON retrieval_chunks(workspace_root, relative_path, published_gen);"); !s.ok()) return s;
        } else {
            // Already compound PK or chunks not existing yet
            if (chunksExists && !chunksPubGen.exists) {
                if (auto s = exec(db, "ALTER TABLE retrieval_chunks ADD COLUMN published_gen INTEGER NOT NULL DEFAULT 0"); !s.ok()) return s;
                if (auto s = exec(db, "UPDATE retrieval_chunks SET published_gen = snapshot_gen WHERE published_gen = 0"); !s.ok()) return s;
            }
            if (chunksExists && !chunksText.exists) {
                if (auto s = exec(db, "ALTER TABLE retrieval_chunks ADD COLUMN chunk_text TEXT NOT NULL DEFAULT ''"); !s.ok()) return s;
            }
        }

        // 2. Migrate retrieval_sources
        if (sourcesExists && sourcesSnapGen.pk == 0) {
            if (auto s = exec(db,
                "CREATE TABLE IF NOT EXISTS retrieval_sources_migrated ("
                "    workspace_root   TEXT NOT NULL,"
                "    relative_path    TEXT NOT NULL,"
                "    content_hash     TEXT NOT NULL,"
                "    source_kind      INTEGER NOT NULL,"
                "    snapshot_gen     INTEGER NOT NULL,"
                "    published_gen    INTEGER NOT NULL DEFAULT 0,"
                "    PRIMARY KEY (workspace_root, relative_path, snapshot_gen)"
                ");"); !s.ok()) return s;

            std::string copySrcSql = sourcesPubGen.exists ?
                "INSERT OR IGNORE INTO retrieval_sources_migrated "
                "SELECT workspace_root, relative_path, content_hash, source_kind, snapshot_gen, "
                "       CASE WHEN published_gen > 0 THEN published_gen ELSE snapshot_gen END "
                "FROM retrieval_sources;" :
                "INSERT OR IGNORE INTO retrieval_sources_migrated "
                "SELECT workspace_root, relative_path, content_hash, source_kind, snapshot_gen, snapshot_gen "
                "FROM retrieval_sources;";
            if (auto s = exec(db, copySrcSql.c_str()); !s.ok()) return s;

            if (auto s = exec(db, "DROP TABLE retrieval_sources;"); !s.ok()) return s;
            if (auto s = exec(db, "ALTER TABLE retrieval_sources_migrated RENAME TO retrieval_sources;"); !s.ok()) return s;
            if (auto s = exec(db, "CREATE INDEX IF NOT EXISTS idx_sources_ws_pubgen ON retrieval_sources(workspace_root, published_gen);"); !s.ok()) return s;
        } else if (sourcesExists && !sourcesPubGen.exists) {
            if (auto s = exec(db, "ALTER TABLE retrieval_sources ADD COLUMN published_gen INTEGER NOT NULL DEFAULT 0"); !s.ok()) return s;
            if (auto s = exec(db, "UPDATE retrieval_sources SET published_gen = snapshot_gen WHERE published_gen = 0"); !s.ok()) return s;
        }

        // 3. Migrate retrieval_meta
        if (metaExists && !metaPubGen.exists) {
            if (auto s = exec(db, "ALTER TABLE retrieval_meta ADD COLUMN published_gen INTEGER NOT NULL DEFAULT 0"); !s.ok()) return s;
            if (auto s = exec(db, "UPDATE retrieval_meta SET published_gen = snapshot_gen WHERE published_gen = 0"); !s.ok()) return s;
        }

        // 4. Migrate retrieval_fts from external content to standalone virtual table
        bool ftsNeedsRecreate = false;
        {
            Stmt ftsCheck;
            if (prepareStmt(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='retrieval_fts'", ftsCheck).ok()) {
                if (ftsCheck.step() == SQLITE_ROW) {
                    const char* sql = reinterpret_cast<const char*>(sqlite3_column_text(ftsCheck.s, 0));
                    if (sql && (std::string_view(sql).find("content=") != std::string_view::npos ||
                                std::string_view(sql).find("content_rowid=") != std::string_view::npos)) {
                        ftsNeedsRecreate = true;
                    }
                }
            }
        }
        if (ftsNeedsRecreate) {
            static_cast<void>(exec(db, "DROP TABLE IF EXISTS retrieval_fts;"));
            static_cast<void>(exec(db, kCreateFts));
            static_cast<void>(exec(db,
                "INSERT INTO retrieval_fts (chunk_id, workspace_root, source_kind, snapshot_gen, published_gen, chunk_text) "
                "SELECT chunk_id, workspace_root, source_kind, snapshot_gen, published_gen, chunk_text "
                "FROM retrieval_chunks WHERE chunk_text != '';"));
        }

        // Existing source rows have no trustworthy successful-indexing marker.
        // Revalidate them on the next refresh instead of reusing failed work.
        ColDetails sourceComplete;
        if (getColumnDetails("retrieval_sources", "indexed_complete", sourceComplete) && !sourceComplete.exists) {
            if (auto s = exec(db,
                "ALTER TABLE retrieval_sources ADD COLUMN indexed_complete INTEGER NOT NULL DEFAULT 0");
                !s.ok()) return s;
        }
        if (metaExists) {
            const auto sql = "UPDATE retrieval_meta SET schema_version='" + kSchemaVersionText +
                "' WHERE schema_version IN ('1','2')";
            if (auto s = exec(db, sql.c_str()); !s.ok()) return s;
        }

        return core::Status::success();
    }

    core::Status openDb(const std::filesystem::path& path) {
        // Refreshes reuse the connection rather than leaking a new handle each pass.
        if (db && dbPath_ == path && schemaReady_) return core::Status::success();
        if (db) {
            if (sqlite3_close(db) != SQLITE_OK)
                return core::makeError(core::ErrorCode::DbError, "Retrieval connection is still in use");
            db = nullptr;
        }
        schemaReady_ = false;
        dbPath_ = path;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        int rc = sqlite3_open(path.string().c_str(), &db);
        if (rc != SQLITE_OK) {
            return core::makeError(core::ErrorCode::DbError,
                std::string("Cannot open retrieval index: ") + sqlite3_errmsg(db));
        }
        // WAL mode and pragmas.
        if (auto s = exec(db, "PRAGMA journal_mode=WAL;"); !s.ok()) return s;
        if (auto s = exec(db, "PRAGMA synchronous=NORMAL;"); !s.ok()) return s;
        if (auto s = exec(db, "PRAGMA busy_timeout=5000;"); !s.ok()) return s;
        // Run schema migration BEFORE enabling foreign keys and before CREATE TABLE IF NOT EXISTS
        if (auto s = exec(db, "BEGIN IMMEDIATE"); !s.ok()) return s;
        const auto migrate = [&]() -> core::Status {
            if (auto s = migrateSchemaIfNeeded(); !s.ok()) return s;
            if (auto s = exec(db, kCreateMeta); !s.ok()) return s;
            if (auto s = exec(db, kCreateSources); !s.ok()) return s;
            if (auto s = exec(db, kCreateChunks); !s.ok()) return s;
            if (auto s = exec(db, kCreateEmbeddings); !s.ok()) return s;
            if (auto s = exec(db, kCreateFts); !s.ok()) return s;
            return exec(db, "COMMIT");
        };
        if (auto s = migrate(); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        if (auto s = exec(db, "PRAGMA foreign_keys=ON;"); !s.ok()) return s;
        if (auto s = repairCorruptEvidence(); !s.ok()) return s;
        schemaReady_ = true;
        return core::Status::success();
    }

    core::Status repairCorruptEvidence() {
        // The historical binding bug stored one byte ('0') for longer spans.
        // Preserve genuine one-byte zero content and all unaffected evidence.
        if (auto s = exec(db, "BEGIN IMMEDIATE"); !s.ok()) return s;
        static constexpr const char* repairs[] = {
            "UPDATE retrieval_meta SET coverage_complete=0 WHERE EXISTS ("
            "SELECT 1 FROM retrieval_chunks c WHERE c.workspace_root=retrieval_meta.workspace_root "
            "AND c.chunk_text='0' AND c.byte_end-c.byte_start<>1)",
            "DELETE FROM retrieval_sources WHERE EXISTS (SELECT 1 FROM retrieval_chunks c "
            "WHERE c.workspace_root=retrieval_sources.workspace_root "
            "AND c.relative_path=retrieval_sources.relative_path "
            "AND c.chunk_text='0' AND c.byte_end-c.byte_start<>1)",
            "DELETE FROM retrieval_fts WHERE EXISTS (SELECT 1 FROM retrieval_chunks c "
            "WHERE c.workspace_root=retrieval_fts.workspace_root AND c.chunk_id=retrieval_fts.chunk_id "
            "AND c.snapshot_gen=retrieval_fts.snapshot_gen "
            "AND c.chunk_text='0' AND c.byte_end-c.byte_start<>1)",
            "DELETE FROM retrieval_chunks WHERE chunk_text='0' AND byte_end-byte_start<>1",
            "UPDATE retrieval_meta SET indexed_chunks=(SELECT count(*) FROM retrieval_chunks c "
            "WHERE c.workspace_root=retrieval_meta.workspace_root AND c.published_gen>0), "
            "indexed_files=(SELECT count(DISTINCT relative_path) FROM retrieval_chunks c "
            "WHERE c.workspace_root=retrieval_meta.workspace_root AND c.published_gen>0)"
        };
        for (const auto* sql : repairs) {
            if (auto s = exec(db, sql); !s.ok()) {
                static_cast<void>(exec(db, "ROLLBACK"));
                return s;
            }
        }
        return exec(db, "COMMIT");
    }

    core::Status ensureMeta(const IndexPolicy& policy) {
        // Insert or verify existing meta row.
        Stmt sel;
        if (auto s = prepareStmt(db,
            "SELECT schema_version, model_revision, dimensions, chunker_version, "
            "snapshot_gen, published_gen, model_id, normalized, preprocess_version "
            "FROM retrieval_meta WHERE workspace_root=?", sel); !s.ok()) return s;
        sqlite3_bind_text(sel.s, 1, workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sel.step();
        if (rc == SQLITE_ROW) {
            // Existing row: check compatibility.
            std::string ver = reinterpret_cast<const char*>(sqlite3_column_text(sel.s, 0));
            std::string rev = reinterpret_cast<const char*>(sqlite3_column_text(sel.s, 1));
            int dims = sqlite3_column_int(sel.s, 2);
            std::string chunkVer = reinterpret_cast<const char*>(sqlite3_column_text(sel.s, 3));
            if (ver != kSchemaVersionText)
                return core::makeError(core::ErrorCode::PreconditionFailed, "Unsupported retrieval cache schema: " + ver);
            const bool compatible =
                                    (rev == policy.modelRevision) &&
                                    (dims == policy.dimensions) &&
                                    (chunkVer == policy.chunkerVersion) &&
                                    (std::string(reinterpret_cast<const char*>(sqlite3_column_text(sel.s, 6))) == policy.modelId) &&
                                    ((sqlite3_column_int(sel.s, 7) != 0) == policy.normalized) &&
                                    (std::string(reinterpret_cast<const char*>(sqlite3_column_text(sel.s, 8))) == policy.preprocessVersion);
            if (!compatible) {
                // Policy mismatch: wipe and re-create for this workspace.
                if (auto s = purgeWorkspace(); !s.ok()) return s;
            } else {
                currentGen_   = static_cast<std::uint64_t>(sqlite3_column_int64(sel.s, 4));
                publishedGen_ = static_cast<std::uint64_t>(sqlite3_column_int64(sel.s, 5));
                return core::Status::success();
            }
        }
        // Insert meta row.
        Stmt ins;
        if (auto s = prepareStmt(db,
            "INSERT OR REPLACE INTO retrieval_meta "
            "(workspace_root, schema_version, model_id, model_revision, dimensions, normalized, "
            "chunker_version, preprocess_version, snapshot_gen, published_gen, coverage_complete) "
            "VALUES (?,?,?,?,?,?,?,?,0,0,0)", ins); !s.ok()) return s;
        sqlite3_bind_text(ins.s, 1, workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.s, 2, kSchemaVersionText.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.s, 3, policy.modelId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.s, 4, policy.modelRevision.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins.s, 5, policy.dimensions);
        sqlite3_bind_int(ins.s, 6, policy.normalized ? 1 : 0);
        sqlite3_bind_text(ins.s, 7, policy.chunkerVersion.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.s, 8, policy.preprocessVersion.c_str(), -1, SQLITE_TRANSIENT);
        return sqlErr(db, ins.step(), "insert meta");
    }

    core::Status purgeWorkspace() {
        // Delete all chunks, sources, embeddings, FTS, and meta for this workspace.
        Stmt s1, s2, s3, s4;
        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_fts WHERE workspace_root=?", s1); !s.ok()) return s;
        sqlite3_bind_text(s1.s, 1, workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        if (s1.step() != SQLITE_DONE) return sqlErr(db, SQLITE_ERROR, "purge fts");

        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_chunks WHERE workspace_root=?", s2); !s.ok()) return s;
        sqlite3_bind_text(s2.s, 1, workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        if (s2.step() != SQLITE_DONE) return sqlErr(db, SQLITE_ERROR, "purge chunks");

        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_sources WHERE workspace_root=?", s3); !s.ok()) return s;
        sqlite3_bind_text(s3.s, 1, workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        if (s3.step() != SQLITE_DONE) return sqlErr(db, SQLITE_ERROR, "purge sources");

        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_meta WHERE workspace_root=?", s4); !s.ok()) return s;
        sqlite3_bind_text(s4.s, 1, workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        if (s4.step() != SQLITE_DONE) return sqlErr(db, SQLITE_ERROR, "purge meta");

        currentGen_   = 0;
        publishedGen_ = 0;
        return core::Status::success();
    }

    bool isOpen() const noexcept { return db != nullptr; }
};

// ---------------------------------------------------------------------------
// RetrievalIndex public interface
// ---------------------------------------------------------------------------

RetrievalIndex::RetrievalIndex(std::filesystem::path databasePath)
    : impl_(std::make_unique<Impl>()) {
    impl_->dbPath_ = std::move(databasePath);
}

RetrievalIndex::~RetrievalIndex() = default;
RetrievalIndex::RetrievalIndex(RetrievalIndex&&) noexcept = default;
RetrievalIndex& RetrievalIndex::operator=(RetrievalIndex&&) noexcept = default;

core::Status RetrievalIndex::open(const std::filesystem::path& workspaceRoot,
                                   const IndexPolicy& policy) {
    if (impl_->generationOpen_)
        return core::makeError(core::ErrorCode::PreconditionFailed, "Cannot rebind an active retrieval refresh");
    impl_->workspaceRoot = workspaceRoot.generic_string();
    impl_->policy_ = policy;
    if (auto s = impl_->openDb(impl_->dbPath_); !s.ok()) return s;
    return impl_->ensureMeta(policy);
}

bool RetrievalIndex::isOpen() const noexcept {
    return impl_ && impl_->isOpen();
}

IndexPolicy RetrievalIndex::policy() const {
    return impl_ ? impl_->policy_ : IndexPolicy{};
}

std::uint64_t RetrievalIndex::currentGeneration() const noexcept {
    return impl_ ? impl_->currentGen_ : 0;
}

bool RetrievalIndex::isPolicyCompatible(const IndexPolicy& requested) const {
    if (!impl_ || !impl_->isOpen()) return false;
    return impl_->policy_ == requested;
}

core::Status RetrievalIndex::beginGeneration(RefreshType type) {
    if (!impl_ || !impl_->isOpen())
        return core::makeError(core::ErrorCode::PreconditionFailed, "Index not open");
    if (impl_->generationOpen_)
        return core::makeError(core::ErrorCode::PreconditionFailed, "Generation already open");
    impl_->pendingGen_ = impl_->currentGen_ + 1;
    impl_->refreshType_ = type;
    impl_->generationOpen_ = true;
    return core::Status::success();
}

core::Status RetrievalIndex::insertChunks(const std::vector<IndexedChunk>& chunks,
                                            const EmbeddingBatch& embeddings) {
    if (!impl_ || !impl_->isOpen())
        return core::makeError(core::ErrorCode::PreconditionFailed, "Index not open");
    if (!impl_->generationOpen_)
        return core::makeError(core::ErrorCode::PreconditionFailed, "No open generation");
    if (chunks.size() != embeddings.rows)
        return core::makeError(core::ErrorCode::InvalidInput, "chunks/embeddings count mismatch");
    if (chunks.empty()) return core::Status::success();

    const int dims = embeddings.dimensions;

    // --- Vector buffer shape validation ---
    if (dims <= 0 || dims > 65536)
        return core::makeError(core::ErrorCode::InvalidInput,
            "Invalid embedding dimensions: " + std::to_string(dims));
    const std::size_t expectedSize = embeddings.rows * static_cast<std::size_t>(dims);
    if (embeddings.data.size() != expectedSize)
        return core::makeError(core::ErrorCode::InvalidInput,
            "Embedding buffer size " + std::to_string(embeddings.data.size()) +
            " != expected " + std::to_string(expectedSize));

    // Verify per-row that there are no non-finite values.
    for (std::size_t row = 0; row < embeddings.rows; ++row) {
        const float* vec = embeddings.data.data() + row * static_cast<std::size_t>(dims);
        for (int col = 0; col < dims; ++col) {
            if (!std::isfinite(vec[col]))
                return core::makeError(core::ErrorCode::InvalidInput,
                    "Non-finite value in embedding row " + std::to_string(row) +
                    " col " + std::to_string(col));
        }
    }

    auto* db = impl_->db;

    // Write chunks into the PENDING generation (pendingGen_). These chunks are
    // not yet reader-visible because published_gen stays at the previous value.
    // commitGeneration() atomically advances published_gen.
    if (auto s = exec(db, "BEGIN IMMEDIATE"); !s.ok()) return s;

    // -----------------------------------------------------------------------
    // PARAMETER MAPPING NOTE (Blocker 1 fix):
    //
    //   17 columns → 17 ? parameters.  published_gen is now a ? (not literal 0)
    //   so the parameter numbers are contiguous:
    //     1  chunk_id
    //     2  workspace_root
    //     3  relative_path
    //     4  content_hash
    //     5  source_kind
    //     6  byte_start
    //     7  byte_end
    //     8  line_start
    //     9  line_end
    //    10  ast_symbol_id
    //    11  ast_qualified
    //    12  ast_link_status
    //    13  snapshot_gen
    //    14  published_gen      ← was literal 0, now explicitly bound to 0
    //    15  heading_context    ← was bound to param 15 (skipping 14), now 15
    //    16  has_ast_hint       ← was bound to param 16, now 16
    //    17  chunk_text         ← was bound to param 17 (out of range!), now 17
    // -----------------------------------------------------------------------
    Stmt insChunk, insEmb, insFts;
    if (auto s = prepareStmt(db,
        "INSERT OR REPLACE INTO retrieval_chunks "
        "(chunk_id, workspace_root, relative_path, content_hash, source_kind, "
        " byte_start, byte_end, line_start, line_end, "
        " ast_symbol_id, ast_qualified, ast_link_status, "
        " snapshot_gen, published_gen, heading_context, has_ast_hint, chunk_text) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", insChunk); !s.ok()) {
        static_cast<void>(exec(db, "ROLLBACK"));
        return s;
    }
    if (auto s = prepareStmt(db,
        "INSERT OR REPLACE INTO retrieval_embeddings (chunk_id, snapshot_gen, embedding) VALUES (?,?,?)",
        insEmb); !s.ok()) {
        static_cast<void>(exec(db, "ROLLBACK"));
        return s;
    }
    // FTS mirrors snapshot_gen/published_gen; the WHERE clause in retrieve()
    // filters on those values in retrieval_fts directly (standalone virtual table).
    if (auto s = prepareStmt(db,
        "INSERT INTO retrieval_fts "
        "(chunk_id, workspace_root, source_kind, snapshot_gen, published_gen, chunk_text) "
        "VALUES (?,?,?,?,0,?)",
        insFts); !s.ok()) {
        static_cast<void>(exec(db, "ROLLBACK"));
        return s;
    }

    auto checkBind = [&](int rc, const char* name) -> core::Status {
        if (rc != SQLITE_OK) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return core::makeError(core::ErrorCode::DbError,
                std::string("Bind ") + name + " failed: " + sqlite3_errmsg(db));
        }
        return core::Status::success();
    };

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto& c = chunks[i];
        const std::string& cid = c.chunkId.value;

        if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 1, cid.c_str(), -1, SQLITE_TRANSIENT), "chunk_id"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 2, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT), "workspace_root"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 3, c.location.sourcePath.c_str(), -1, SQLITE_TRANSIENT), "relative_path"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 4, c.location.contentHash.c_str(), -1, SQLITE_TRANSIENT), "content_hash"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int(insChunk.s, 5, static_cast<int>(c.sourceKind)), "source_kind"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int64(insChunk.s, 6, c.location.byteStart), "byte_start"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int64(insChunk.s, 7, c.location.byteEnd), "byte_end"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int64(insChunk.s, 8, c.location.lineStart), "line_start"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int64(insChunk.s, 9, c.location.lineEnd), "line_end"); !s.ok()) return s;
        if (c.astLink && !c.astLink->symbolId.empty()) {
            if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 10, c.astLink->symbolId.c_str(), -1, SQLITE_TRANSIENT), "ast_symbol_id"); !s.ok()) return s;
        } else {
            if (auto s = checkBind(sqlite3_bind_null(insChunk.s, 10), "ast_symbol_id_null"); !s.ok()) return s;
        }
        if (c.astLink && !c.astLink->qualifiedName.empty()) {
            if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 11, c.astLink->qualifiedName.c_str(), -1, SQLITE_TRANSIENT), "ast_qualified"); !s.ok()) return s;
        } else {
            if (auto s = checkBind(sqlite3_bind_null(insChunk.s, 11), "ast_qualified_null"); !s.ok()) return s;
        }
        if (auto s = checkBind(sqlite3_bind_int(insChunk.s, 12, c.astLink ? static_cast<int>(c.astLink->status) : 0), "ast_link_status"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int64(insChunk.s, 13, static_cast<sqlite3_int64>(impl_->pendingGen_)), "snapshot_gen"); !s.ok()) return s;
        // Param 14 = published_gen: 0 (staged, not yet reader-visible).
        if (auto s = checkBind(sqlite3_bind_int(insChunk.s, 14, 0), "published_gen"); !s.ok()) return s;
        // Param 15 = heading_context.
        if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 15, "", -1, SQLITE_STATIC), "heading_context"); !s.ok()) return s;
        // Param 16 = has_ast_hint.
        if (auto s = checkBind(sqlite3_bind_int(insChunk.s, 16, 0), "has_ast_hint"); !s.ok()) return s;
        // Param 17 = chunk_text.
        if (auto s = checkBind(sqlite3_bind_text(insChunk.s, 17, c.text.c_str(), -1, SQLITE_TRANSIENT), "chunk_text"); !s.ok()) return s;

        int rc = insChunk.step();
        insChunk.reset();
        if (rc != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, rc, "insert chunk");
        }

        // Insert embedding blob.
        const float* vec = embeddings.data.data() + i * static_cast<std::size_t>(dims);
        if (auto s = checkBind(sqlite3_bind_text(insEmb.s, 1, cid.c_str(), -1, SQLITE_TRANSIENT), "emb_chunk_id"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int64(insEmb.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_)), "emb_snapshot_gen"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_blob(insEmb.s, 3, vec, dims * static_cast<int>(sizeof(float)), SQLITE_TRANSIENT), "emb_blob"); !s.ok()) return s;
        rc = insEmb.step();
        insEmb.reset();
        if (rc != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, rc, "insert embedding");
        }

        // Insert into FTS (staged with published_gen=0).
        if (auto s = checkBind(sqlite3_bind_text(insFts.s, 1, cid.c_str(), -1, SQLITE_TRANSIENT), "fts_chunk_id"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_text(insFts.s, 2, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT), "fts_workspace_root"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int(insFts.s, 3, static_cast<int>(c.sourceKind)), "fts_source_kind"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_int64(insFts.s, 4, static_cast<sqlite3_int64>(impl_->pendingGen_)), "fts_snapshot_gen"); !s.ok()) return s;
        if (auto s = checkBind(sqlite3_bind_text(insFts.s, 5, c.text.c_str(), -1, SQLITE_TRANSIENT), "fts_chunk_text"); !s.ok()) return s;
        rc = insFts.step();
        insFts.reset();
        if (rc != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, rc, "insert fts");
        }
    }

    return exec(db, "COMMIT");
}

core::Status RetrievalIndex::recordSource(const std::string& relativePath,
                                           const std::string& contentHash,
                                           SourceKind kind,
                                           bool indexedComplete) {
    if (!impl_ || !impl_->isOpen())
        return core::makeError(core::ErrorCode::PreconditionFailed, "Index not open");
    if (!impl_->generationOpen_)
        return core::makeError(core::ErrorCode::PreconditionFailed, "No open generation");
    auto* db = impl_->db;
    // Insert a PENDING source row (published_gen=0) with its own snapshot_gen key.
    // The previous published row (if any) for this path is not touched here;
    // it coexists until commitGeneration() promotes this row and prunes old ones,
    // or rollbackGeneration() deletes this row.  This prevents the INSERT OR REPLACE
    // pattern from destroying the last valid published snapshot on rollback.
    Stmt ins;
    if (auto s = prepareStmt(db,
        "INSERT OR REPLACE INTO retrieval_sources "
        "(workspace_root, relative_path, content_hash, source_kind, snapshot_gen, published_gen, indexed_complete) "
        "VALUES (?,?,?,?,?,0,?)", ins); !s.ok()) return s;
    sqlite3_bind_text(ins.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins.s, 2, relativePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins.s, 3, contentHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins.s, 4, static_cast<int>(kind));
    sqlite3_bind_int64(ins.s, 5, static_cast<sqlite3_int64>(impl_->pendingGen_));
    sqlite3_bind_int(ins.s, 6, indexedComplete ? 1 : 0);
    return sqlErr(db, ins.step(), "record source");
}

core::Status RetrievalIndex::removeSourceChunks(const std::string& relativePath) {
    if (!impl_ || !impl_->isOpen())
        return core::makeError(core::ErrorCode::PreconditionFailed, "Index not open");
    auto* db = impl_->db;
    // Delete from FTS first (the virtual table does not CASCADE).
    Stmt fts;
    if (auto s = prepareStmt(db,
        "DELETE FROM retrieval_fts WHERE chunk_id IN "
        "(SELECT chunk_id FROM retrieval_chunks WHERE workspace_root=? AND relative_path=?)",
        fts); !s.ok()) return s;
    sqlite3_bind_text(fts.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(fts.s, 2, relativePath.c_str(), -1, SQLITE_TRANSIENT);
    if (fts.step() != SQLITE_DONE) return sqlErr(db, SQLITE_ERROR, "remove fts");

    Stmt del;
    if (auto s = prepareStmt(db,
        "DELETE FROM retrieval_chunks WHERE workspace_root=? AND relative_path=?",
        del); !s.ok()) return s;
    sqlite3_bind_text(del.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(del.s, 2, relativePath.c_str(), -1, SQLITE_TRANSIENT);
    return sqlErr(db, del.step(), "remove chunks");
}

core::Status RetrievalIndex::commitGeneration(bool complete) {
    if (!impl_ || !impl_->isOpen())
        return core::makeError(core::ErrorCode::PreconditionFailed, "Index not open");
    if (!impl_->generationOpen_)
        return core::makeError(core::ErrorCode::PreconditionFailed, "No open generation");

    auto* db = impl_->db;
    if (auto s = exec(db, "BEGIN IMMEDIATE"); !s.ok()) return s;

    // ---- 1. Promote staged chunks ----
    // Atomically promote staged chunks: published_gen = snapshot_gen
    {
        Stmt pub;
        if (auto s = prepareStmt(db,
            "UPDATE retrieval_chunks SET published_gen = snapshot_gen "
            "WHERE workspace_root = ? AND snapshot_gen = ? AND published_gen = 0",
            pub); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(pub.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(pub.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        if (pub.step() != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, SQLITE_ERROR, "publish chunks");
        }
    }

    // ---- 2. Promote staged sources ----
    // Atomically promote staged sources: published_gen = snapshot_gen
    {
        Stmt pubSrc;
        if (auto s = prepareStmt(db,
            "UPDATE retrieval_sources SET published_gen = snapshot_gen "
            "WHERE workspace_root = ? AND snapshot_gen = ? AND published_gen = 0",
            pubSrc); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(pubSrc.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(pubSrc.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        if (pubSrc.step() != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, SQLITE_ERROR, "publish sources");
        }
    }

    // ---- 3. Promote staged FTS ----
    // Atomically promote staged FTS: published_gen = snapshot_gen
    {
        Stmt pubFts;
        if (auto s = prepareStmt(db,
            "UPDATE retrieval_fts SET published_gen = snapshot_gen "
            "WHERE workspace_root = ? AND snapshot_gen = ? AND published_gen = 0",
            pubFts); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(pubFts.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(pubFts.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        if (pubFts.step() != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, SQLITE_ERROR, "publish fts");
        }
    }

    // ---- 4. Carry forward active unchanged chunks if complete == true ----
    if (complete) {
        Stmt carryChunks;
        if (auto s = prepareStmt(db,
            "UPDATE retrieval_chunks "
            "SET snapshot_gen = ?, published_gen = ? "
            "WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ? "
            "  AND EXISTS ("
            "    SELECT 1 FROM retrieval_sources s "
            "    WHERE s.workspace_root = retrieval_chunks.workspace_root "
            "      AND s.relative_path = retrieval_chunks.relative_path "
            "      AND s.content_hash = retrieval_chunks.content_hash "
            "      AND s.snapshot_gen = ?"
            "  ) AND NOT EXISTS (SELECT 1 FROM retrieval_chunks pending "
            "WHERE pending.chunk_id=retrieval_chunks.chunk_id AND pending.snapshot_gen=?)",
            carryChunks); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_int64(carryChunks.s, 1, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_int64(carryChunks.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_text(carryChunks.s, 3, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(carryChunks.s, 4, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_int64(carryChunks.s, 5, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_int64(carryChunks.s, 6, static_cast<sqlite3_int64>(impl_->pendingGen_));
        if (carryChunks.step() != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, SQLITE_ERROR, "carry forward chunks");
        }

        Stmt carryFts;
        if (auto s = prepareStmt(db,
            "UPDATE retrieval_fts "
            "SET snapshot_gen = ?, published_gen = ? "
            "WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ? "
            "  AND chunk_id IN ("
            "    SELECT chunk_id FROM retrieval_chunks "
            "    WHERE workspace_root = ? AND snapshot_gen = ?"
            "  ) AND NOT EXISTS (SELECT 1 FROM retrieval_chunks old "
            "WHERE old.chunk_id=retrieval_fts.chunk_id AND old.snapshot_gen=retrieval_fts.snapshot_gen)",
            carryFts); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_int64(carryFts.s, 1, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_int64(carryFts.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_text(carryFts.s, 3, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(carryFts.s, 4, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_text(carryFts.s, 5, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(carryFts.s, 6, static_cast<sqlite3_int64>(impl_->pendingGen_));
        if (carryFts.step() != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, SQLITE_ERROR, "carry forward fts");
        }
    }

    // ---- 5. Delete obsolete chunks/sources/fts (published_gen < pendingGen_) ----
    if (complete && impl_->refreshType_ == RefreshType::Full) {
        Stmt delChunks;
        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_chunks WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ?",
            delChunks); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(delChunks.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delChunks.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        if (delChunks.step() != SQLITE_DONE) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return sqlErr(db, SQLITE_ERROR, "purge old chunks");
        }

        Stmt delSources;
        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_sources WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ?",
            delSources); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(delSources.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delSources.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        delSources.step();

        Stmt delFts;
        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_fts WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ?",
            delFts); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(delFts.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delFts.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        delFts.step();
    } else {
        // Incremental: only delete obsolete chunks/sources/fts for relative_paths updated in pendingGen_
        Stmt delFtsInc;
        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_fts "
            "WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ? "
            "  AND chunk_id IN ("
            "    SELECT chunk_id FROM retrieval_chunks "
            "    WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ? "
            "      AND relative_path IN ("
            "        SELECT relative_path FROM retrieval_chunks WHERE workspace_root = ? AND snapshot_gen = ?"
            "      )"
            "  )",
            delFtsInc); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(delFtsInc.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delFtsInc.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_text(delFtsInc.s, 3, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delFtsInc.s, 4, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_text(delFtsInc.s, 5, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delFtsInc.s, 6, static_cast<sqlite3_int64>(impl_->pendingGen_));
        delFtsInc.step();

        Stmt delChunksInc;
        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_chunks "
            "WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ? "
            "  AND relative_path IN ("
            "    SELECT relative_path FROM retrieval_chunks WHERE workspace_root = ? AND snapshot_gen = ?"
            "  )",
            delChunksInc); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(delChunksInc.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delChunksInc.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_text(delChunksInc.s, 3, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delChunksInc.s, 4, static_cast<sqlite3_int64>(impl_->pendingGen_));
        delChunksInc.step();

        Stmt delSourcesInc;
        if (auto s = prepareStmt(db,
            "DELETE FROM retrieval_sources "
            "WHERE workspace_root = ? AND published_gen > 0 AND published_gen < ? "
            "  AND relative_path IN ("
            "    SELECT relative_path FROM retrieval_sources WHERE workspace_root = ? AND snapshot_gen = ?"
            "  )",
            delSourcesInc); !s.ok()) {
            static_cast<void>(exec(db, "ROLLBACK"));
            return s;
        }
        sqlite3_bind_text(delSourcesInc.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delSourcesInc.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
        sqlite3_bind_text(delSourcesInc.s, 3, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(delSourcesInc.s, 4, static_cast<sqlite3_int64>(impl_->pendingGen_));
        delSourcesInc.step();
    }

    // ---- 6. Remove any staged-but-unpublished chunks/sources/fts from aborted prior attempts ----
    {
        Stmt delStaged;
        if (prepareStmt(db,
            "DELETE FROM retrieval_chunks WHERE workspace_root = ? AND published_gen = 0 AND snapshot_gen < ?",
            delStaged).ok()) {
            sqlite3_bind_text(delStaged.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(delStaged.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
            delStaged.step();
        }
        Stmt delStagedSrc;
        if (prepareStmt(db,
            "DELETE FROM retrieval_sources WHERE workspace_root = ? AND published_gen = 0 AND snapshot_gen < ?",
            delStagedSrc).ok()) {
            sqlite3_bind_text(delStagedSrc.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(delStagedSrc.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
            delStagedSrc.step();
        }
        Stmt delStagedFts;
        if (prepareStmt(db,
            "DELETE FROM retrieval_fts WHERE workspace_root = ? AND published_gen = 0 AND snapshot_gen < ?",
            delStagedFts).ok()) {
            sqlite3_bind_text(delStagedFts.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(delStagedFts.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
            delStagedFts.step();
        }
    }

    // Count indexed chunks and files (published only).
    Stmt cnt;
    if (auto s = prepareStmt(db,
        "SELECT COUNT(*), COUNT(DISTINCT relative_path) FROM retrieval_chunks "
        "WHERE workspace_root=? AND published_gen>0",
        cnt); !s.ok()) {
        static_cast<void>(exec(db, "ROLLBACK"));
        return s;
    }
    sqlite3_bind_text(cnt.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
    std::int64_t nChunks = 0, nFiles = 0;
    if (cnt.step() == SQLITE_ROW) {
        nChunks = sqlite3_column_int64(cnt.s, 0);
        nFiles = sqlite3_column_int64(cnt.s, 1);
    }

    // Full success establishes coverage. Incremental success can preserve it;
    // no incomplete pass, of either type, can preserve or establish completeness.
    std::int64_t existingCoverageComplete = 0;
    {
        Stmt selCov;
        if (prepareStmt(db,
            "SELECT coverage_complete FROM retrieval_meta WHERE workspace_root=?",
            selCov).ok()) {
            sqlite3_bind_text(selCov.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
            if (selCov.step() == SQLITE_ROW)
                existingCoverageComplete = sqlite3_column_int64(selCov.s, 0);
        }
    }
    const int newCoverageComplete = complete &&
        (impl_->refreshType_ == RefreshType::Full || existingCoverageComplete != 0) ? 1 : 0;

    // Update meta row: advance published_gen and snapshot_gen together.
    Stmt upd;
    if (auto s = prepareStmt(db,
        "UPDATE retrieval_meta SET snapshot_gen=?, published_gen=?, coverage_complete=?, "
        "indexed_chunks=?, indexed_files=? WHERE workspace_root=?", upd); !s.ok()) {
        static_cast<void>(exec(db, "ROLLBACK"));
        return s;
    }
    sqlite3_bind_int64(upd.s, 1, static_cast<sqlite3_int64>(impl_->pendingGen_));
    sqlite3_bind_int64(upd.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
    sqlite3_bind_int(upd.s, 3, newCoverageComplete);
    sqlite3_bind_int64(upd.s, 4, nChunks);
    sqlite3_bind_int64(upd.s, 5, nFiles);
    sqlite3_bind_text(upd.s, 6, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
    if (upd.step() != SQLITE_DONE) {
        static_cast<void>(exec(db, "ROLLBACK"));
        return sqlErr(db, SQLITE_ERROR, "update meta");
    }

    if (auto s = exec(db, "COMMIT"); !s.ok()) return s;
    impl_->currentGen_   = impl_->pendingGen_;
    impl_->publishedGen_ = impl_->pendingGen_;
    impl_->generationOpen_ = false;
    return core::Status::success();
}

void RetrievalIndex::rollbackGeneration() noexcept {
    if (!impl_) return;
    impl_->generationOpen_ = false;
    if (!impl_->db) return;

    // Remove any staged-but-unpublished chunks and sources written in this generation.
    // This is the real rollback that the original code lacked.
    // The previous published records coexist in the database (different snapshot_gen),
    // so they are unaffected by this deletion.
    if (impl_->pendingGen_ > 0) {
        // Use prepared statements to avoid SQL injection via workspaceRoot.
        Stmt delChunks;
        if (prepareStmt(impl_->db,
            "DELETE FROM retrieval_chunks WHERE workspace_root=? AND published_gen=0 AND snapshot_gen=?",
            delChunks).ok()) {
            sqlite3_bind_text(delChunks.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(delChunks.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
            delChunks.step();
        }
        Stmt delSources;
        if (prepareStmt(impl_->db,
            "DELETE FROM retrieval_sources WHERE workspace_root=? AND published_gen=0 AND snapshot_gen=?",
            delSources).ok()) {
            sqlite3_bind_text(delSources.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(delSources.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
            delSources.step();
        }
        Stmt delFts;
        if (prepareStmt(impl_->db,
            "DELETE FROM retrieval_fts WHERE workspace_root=? AND published_gen=0 AND snapshot_gen=?",
            delFts).ok()) {
            sqlite3_bind_text(delFts.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(delFts.s, 2, static_cast<sqlite3_int64>(impl_->pendingGen_));
            delFts.step();
        }
    }
    // No transaction to roll back (insertChunks uses its own transactions).
}

// ---------------------------------------------------------------------------
// Retrieve
// ---------------------------------------------------------------------------

core::Result<std::vector<RetrievedHit>> RetrievalIndex::retrieve(
    const std::vector<float>& queryEmbedding,
    std::string_view queryText,
    const RetrievalFilter& filter,
    const RetrievalOptions& options) const {

    if (!impl_ || !impl_->isOpen())
        return core::makeError(core::ErrorCode::PreconditionFailed, "Index not open");

    // --- Workspace root validation ---
    // The filter's workspaceRoot must match the index's bound workspaceRoot.
    if (!filter.workspaceRoot.empty()) {
        // Normalize both to generic paths for comparison.
        std::string filterRoot = std::filesystem::path(filter.workspaceRoot).generic_string();
        std::string indexRoot  = impl_->workspaceRoot;
        if (filterRoot != indexRoot) {
            return core::makeError(core::ErrorCode::InvalidInput,
                "Request workspace root '" + filterRoot +
                "' does not match index workspace root '" + indexRoot + "'");
        }
    }

    auto* db = impl_->db;
    const int dims = impl_->policy_.dimensions;

    if (static_cast<int>(queryEmbedding.size()) != dims)
        return core::makeError(core::ErrorCode::InvalidInput, "query embedding dimension mismatch");

    // Build source kind filter predicate.
    auto kindAllowed = [&](int kind) -> bool {
        if (filter.sources.empty()) return true;
        for (auto k : filter.sources)
            if (static_cast<int>(k) == kind) return true;
        return false;
    };

    struct Candidate {
        std::string chunkId;
        float denseScore{0.0f};
        float lexicalScore{0.0f};
        float fusedScore{0.0f};
        bool inDense{false};   // Participated in the dense branch.
        bool inLexical{false}; // Participated in the lexical branch.
    };

    // ---- Dense retrieval: scan only PUBLISHED chunks ----
    // Readers only see chunks with published_gen > 0 (committed generation).
    const std::size_t kDenseCandidates = std::max(options.maxHits * 4, std::size_t{40});
    std::vector<Candidate> denseCandidates;
    denseCandidates.reserve(kDenseCandidates);

    {
        Stmt sel;
        // Filter by workspace, published_gen (committed evidence only), and source kind.
        const char* sql = R"sql(
            SELECT e.chunk_id, e.embedding, c.source_kind
            FROM retrieval_embeddings e
            JOIN retrieval_chunks c ON c.chunk_id = e.chunk_id AND c.snapshot_gen = e.snapshot_gen
            WHERE c.workspace_root = ? AND c.published_gen > 0
        )sql";
        if (auto s = prepareStmt(db, sql, sel); !s.ok()) return s.error();
        sqlite3_bind_text(sel.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);

        while (sel.step() == SQLITE_ROW) {
            int kind = sqlite3_column_int(sel.s, 2);
            if (!kindAllowed(kind)) continue;

            const void* blobData = sqlite3_column_blob(sel.s, 1);
            int blobBytes = sqlite3_column_bytes(sel.s, 1);
            if (blobBytes != dims * static_cast<int>(sizeof(float))) continue;
            const float* vec = static_cast<const float*>(blobData);

            // Validate row for non-finite values before scoring.
            bool rowValid = true;
            for (int ci = 0; ci < dims; ++ci) {
                if (!std::isfinite(vec[ci])) { rowValid = false; break; }
            }
            if (!rowValid) continue;

            float score = innerProduct(queryEmbedding.data(), vec, dims);

            const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(sel.s, 0));
            if (!cid) continue;

            Candidate c;
            c.chunkId = cid;
            c.denseScore = score;
            c.inDense = true;
            denseCandidates.push_back(std::move(c));
        }
    }

    // Keep top-k dense candidates.
    if (denseCandidates.size() > kDenseCandidates) {
        std::partial_sort(denseCandidates.begin(),
                          denseCandidates.begin() + static_cast<std::ptrdiff_t>(kDenseCandidates),
                          denseCandidates.end(),
                          [](const Candidate& a, const Candidate& b){ return a.denseScore > b.denseScore; });
        denseCandidates.resize(kDenseCandidates);
    }

    // ---- Lexical retrieval via FTS5 ----
    // FTS must apply the same workspace, published_gen, and source-kind filters.
    if (!queryText.empty()) {
        // Build a simple FTS5 query: take first N words, strip punctuation.
        std::string ftsQuery;
        {
            std::string qcopy(queryText);
            for (char& ch : qcopy)
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = ' ';
            std::istringstream ss(qcopy);
            std::string word;
            int wordCount = 0;
            while (ss >> word && wordCount < 16) {
                if (word.size() < 2) continue;
                if (!ftsQuery.empty()) ftsQuery += " OR ";
                ftsQuery += '"' + word + '"';
                ++wordCount;
            }
        }

        if (!ftsQuery.empty()) {
            Stmt fts;
            // Filter on workspace_root and published_gen in the FTS shadow columns.
            // source_kind filter applied in the loop below.
            const char* sql = R"sql(
                SELECT f.chunk_id, bm25(retrieval_fts) AS score, f.source_kind
                FROM retrieval_fts f
                WHERE retrieval_fts MATCH ? AND f.workspace_root = ? AND f.published_gen > 0
                ORDER BY score
                LIMIT 40
            )sql";
            if (prepareStmt(db, sql, fts).ok()) {
                sqlite3_bind_text(fts.s, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(fts.s, 2, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
                while (fts.step() == SQLITE_ROW) {
                    const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(fts.s, 0));
                    if (!cid) continue;

                    // Apply source kind filter to lexical results too.
                    int kind = sqlite3_column_int(fts.s, 2);
                    if (!kindAllowed(kind)) continue;

                    // bm25 returns negative values (more negative = more relevant).
                    float lexScore = -static_cast<float>(sqlite3_column_double(fts.s, 1));

                    // Add or update candidate.
                    bool found = false;
                    for (auto& cand : denseCandidates) {
                        if (cand.chunkId == cid) {
                            cand.lexicalScore = lexScore;
                            cand.inLexical = true;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        Candidate c;
                        c.chunkId = cid;
                        c.lexicalScore = lexScore;
                        c.inLexical = true;
                        denseCandidates.push_back(std::move(c));
                    }
                }
            }
        }
    }

    // Deduplicate candidates (same chunk_id from multiple sources).
    {
        std::vector<Candidate> deduped;
        deduped.reserve(denseCandidates.size());
        for (auto& cand : denseCandidates) {
            bool seen = false;
            for (const auto& d : deduped)
                if (d.chunkId == cand.chunkId) { seen = true; break; }
            if (!seen) deduped.push_back(std::move(cand));
        }
        denseCandidates = std::move(deduped);
    }

    // ---- Fused ranking: Reciprocal Rank Fusion (RRF) ----
    // RRF score = 1/(k + rank_in_dense_branch) [only if inDense] +
    //             1/(k + rank_in_lexical_branch) [only if inLexical]
    //
    // Rank contributions reflect actual branch membership: a chunk that only
    // appeared in the lexical branch gets no dense rank contribution and
    // vice versa. This preserves the semantic meaning of each branch.
    constexpr float kRRF = 60.0f;

    // Rank among dense candidates (only those with inDense=true).
    std::vector<std::size_t> denseOnlyIndices;
    denseOnlyIndices.reserve(denseCandidates.size());
    for (std::size_t i = 0; i < denseCandidates.size(); ++i)
        if (denseCandidates[i].inDense) denseOnlyIndices.push_back(i);
    std::sort(denseOnlyIndices.begin(), denseOnlyIndices.end(),
        [&](std::size_t a, std::size_t b){
            return denseCandidates[a].denseScore > denseCandidates[b].denseScore;
        });

    // Rank among lexical candidates (only those with inLexical=true).
    std::vector<std::size_t> lexOnlyIndices;
    lexOnlyIndices.reserve(denseCandidates.size());
    for (std::size_t i = 0; i < denseCandidates.size(); ++i)
        if (denseCandidates[i].inLexical) lexOnlyIndices.push_back(i);
    std::sort(lexOnlyIndices.begin(), lexOnlyIndices.end(),
        [&](std::size_t a, std::size_t b){
            return denseCandidates[a].lexicalScore > denseCandidates[b].lexicalScore;
        });

    std::vector<float> fusedScores(denseCandidates.size(), 0.0f);
    for (std::size_t rank = 0; rank < denseOnlyIndices.size(); ++rank)
        fusedScores[denseOnlyIndices[rank]] += 1.0f / (kRRF + static_cast<float>(rank + 1));
    for (std::size_t rank = 0; rank < lexOnlyIndices.size(); ++rank)
        fusedScores[lexOnlyIndices[rank]] += 1.0f / (kRRF + static_cast<float>(rank + 1));

    for (std::size_t i = 0; i < denseCandidates.size(); ++i)
        denseCandidates[i].fusedScore = fusedScores[i];

    // Sort by fused score descending (deterministic: break ties by chunkId).
    std::sort(denseCandidates.begin(), denseCandidates.end(),
        [](const Candidate& a, const Candidate& b){
            if (a.fusedScore != b.fusedScore) return a.fusedScore > b.fusedScore;
            return a.chunkId < b.chunkId;
        });
    if (denseCandidates.size() > options.maxHits)
        denseCandidates.resize(options.maxHits);

    // ---- Context budget enforcement ----
    // Accumulate excerpt bytes and enforce the character budget before returning.
    std::size_t budgetUsed = 0;

    // ---- Fetch chunk metadata for final hits ----
    std::vector<RetrievedHit> hits;
    hits.reserve(denseCandidates.size());

    for (const auto& cand : denseCandidates) {
        // Apply minimum score filter.
        if (cand.fusedScore < options.minFusedScore) continue;

        // Apply context budget (soft limit: exclude when budget already exhausted).
        if (options.contextBudgetChars > 0 && budgetUsed >= options.contextBudgetChars) break;

        Stmt meta;
        const char* sql = R"sql(
            SELECT relative_path, content_hash, source_kind,
                   byte_start, byte_end, line_start, line_end,
                   ast_symbol_id, ast_qualified, ast_link_status,
                   chunk_text, heading_context, has_ast_hint
            FROM retrieval_chunks
            WHERE chunk_id=? AND workspace_root=? AND published_gen>0
            ORDER BY published_gen DESC LIMIT 1
        )sql";
        if (!prepareStmt(db, sql, meta).ok()) continue;
        sqlite3_bind_text(meta.s, 1, cand.chunkId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(meta.s, 2, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
        if (meta.step() != SQLITE_ROW) continue;

        RetrievedHit hit;
        hit.chunkId.value = cand.chunkId;
        hit.denseScore = cand.denseScore;
        hit.lexicalScore = cand.lexicalScore;
        hit.fusedScore = cand.fusedScore;

        auto col_text = [&](int col) -> std::string {
            const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(meta.s, col));
            return t ? std::string(t) : std::string{};
        };

        hit.location.sourcePath = col_text(0);
        hit.location.contentHash = col_text(1);
        hit.sourceKind = static_cast<SourceKind>(sqlite3_column_int(meta.s, 2));

        // Validate that the source path is under the workspace root (safe containment).
        if (!hit.location.sourcePath.empty() && !filter.workspaceRoot.empty()) {
            // Relative paths should not escape the workspace (no ".." traversal).
            const auto& rp = hit.location.sourcePath;
            if (rp.find("..") != std::string::npos ||
                (!rp.empty() && rp[0] == '/') ||
                (rp.size() >= 2 && rp[1] == ':')) {
                // Suspicious path: skip this hit.
                continue;
            }
        }

        hit.location.byteStart = static_cast<std::uint32_t>(sqlite3_column_int64(meta.s, 3));
        hit.location.byteEnd   = static_cast<std::uint32_t>(sqlite3_column_int64(meta.s, 4));
        hit.location.lineStart = static_cast<std::uint32_t>(sqlite3_column_int64(meta.s, 5));
        hit.location.lineEnd   = static_cast<std::uint32_t>(sqlite3_column_int64(meta.s, 6));

        const std::string symId = col_text(7);
        const std::string symQual = col_text(8);
        int astStatus = sqlite3_column_int(meta.s, 9);
        if (!symId.empty() || astStatus != 0) {
            AstLink link;
            link.symbolId = symId;
            link.qualifiedName = symQual;
            link.status = static_cast<AstLinkStatus>(astStatus);
            hit.astLink = link;
        }

        hit.excerpt = col_text(10);
        hit.rankingEvidence = "dense=" + std::to_string(cand.denseScore) +
                               " lex=" + std::to_string(cand.lexicalScore) +
                               " fused=" + std::to_string(cand.fusedScore) +
                               " branches=" +
                               (cand.inDense ? "D" : "") +
                               (cand.inLexical ? "L" : "");
        hit.sourceHashValid = true; // Revalidated by caller if options.revalidateHashes.

        // Check if adding this excerpt would exceed the budget.
        const std::size_t excerptBytes = hit.excerpt.size();
        if (options.contextBudgetChars > 0 &&
            budgetUsed + excerptBytes > options.contextBudgetChars) {
            std::size_t remaining = options.contextBudgetChars - budgetUsed;
            if (remaining == 0) break;
            hit.excerpt.resize(remaining);
            // ---- Blocker 5: correct UTF-8 boundary truncation ----
            // Ensure we do not cut in the middle of a multi-byte sequence.
            // If the truncated string ends with an incomplete multi-byte character,
            // drop the incomplete sequence so only valid UTF-8 remains.
            if (!hit.excerpt.empty()) {
                std::size_t i = hit.excerpt.size();
                std::size_t continuationCount = 0;
                while (i > 0) {
                    unsigned char b = static_cast<unsigned char>(hit.excerpt[i - 1]);
                    if ((b & 0xC0) == 0x80) {
                        continuationCount++;
                        i--;
                        if (continuationCount > 3) {
                            hit.excerpt.resize(i);
                            break;
                        }
                    } else {
                        std::size_t expectedContinuation = 0;
                        if ((b & 0x80) == 0) {
                            expectedContinuation = 0;
                        } else if ((b & 0xE0) == 0xC0) {
                            expectedContinuation = 1;
                        } else if ((b & 0xF0) == 0xE0) {
                            expectedContinuation = 2;
                        } else if ((b & 0xF8) == 0xF0) {
                            expectedContinuation = 3;
                        } else {
                            // Invalid leading byte
                            hit.excerpt.resize(i - 1);
                            break;
                        }

                        if (continuationCount < expectedContinuation) {
                            // Cut inside this multi-byte sequence: drop the incomplete character
                            hit.excerpt.resize(i - 1);
                        }
                        break;
                    }
                }
                if (i == 0 && continuationCount > 0) {
                    // String consisted only of continuation bytes without a leading byte
                    hit.excerpt.clear();
                }
            }
        }

        budgetUsed += hit.excerpt.size();
        hits.push_back(std::move(hit));

        // Stop once we have exactly maxHits after score + budget filtering.
        if (hits.size() >= options.maxHits) break;
    }

    return hits;
}

core::Result<bool> RetrievalIndex::validateSourceHash(
    const std::string& relativePath,
    const std::string& expectedHash,
    const std::filesystem::path& workspaceRoot) const {
    std::filesystem::path absPath = workspaceRoot / relativePath;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(absPath, ec) || ec) return false;
    const auto actual = hashFileContents(absPath);
    if (actual.empty()) return false;
    return actual == expectedHash;
}

core::Result<SnapshotCoverage> RetrievalIndex::snapshotCoverage() const {
    if (!impl_ || !impl_->isOpen())
        return core::makeError(core::ErrorCode::PreconditionFailed, "Index not open");

    auto* db = impl_->db;
    Stmt sel;
    if (auto s = prepareStmt(db,
        "SELECT coverage_complete, indexed_chunks, indexed_files, published_gen "
        "FROM retrieval_meta WHERE workspace_root=?", sel); !s.ok()) return s.error();
    sqlite3_bind_text(sel.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);

    SnapshotCoverage cov{};
    cov.snapshotGeneration = impl_->publishedGen_;
    if (sel.step() == SQLITE_ROW) {
        cov.coverageComplete = sqlite3_column_int(sel.s, 0) != 0;
        cov.indexedChunks = static_cast<std::size_t>(sqlite3_column_int64(sel.s, 1));
        cov.indexedFiles  = static_cast<std::size_t>(sqlite3_column_int64(sel.s, 2));
        // Prefer the persisted published_gen over the in-memory value.
        cov.snapshotGeneration = static_cast<std::uint64_t>(sqlite3_column_int64(sel.s, 3));
    }
    return cov;
}

bool RetrievalIndex::isSourceUnchanged(const std::string& relativePath,
                                       const std::string& contentHash) const {
    if (!impl_ || !impl_->isOpen()) return false;
    Stmt sel;
    if (!prepareStmt(impl_->db,
        "SELECT 1 FROM retrieval_sources s "
        "WHERE s.workspace_root = ? AND s.relative_path = ? AND s.content_hash = ? "
        "AND s.published_gen > 0 AND s.indexed_complete=1 "
        "AND EXISTS (SELECT 1 FROM retrieval_chunks c JOIN retrieval_embeddings e "
        "ON e.chunk_id=c.chunk_id AND e.snapshot_gen=c.snapshot_gen "
        "WHERE c.workspace_root=s.workspace_root AND c.relative_path=s.relative_path "
        "AND c.content_hash=s.content_hash AND c.published_gen>0) LIMIT 1;",
        sel).ok()) {
        return false;
    }
    sqlite3_bind_text(sel.s, 1, impl_->workspaceRoot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sel.s, 2, relativePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sel.s, 3, contentHash.c_str(), -1, SQLITE_TRANSIENT);
    return sel.step() == SQLITE_ROW;
}

core::Status RetrievalIndex::forgetWorkspace(const std::string& workspaceRoot) {
    if (!impl_ || !impl_->isOpen()) return core::Status::success();
    const std::string saved = impl_->workspaceRoot;
    impl_->workspaceRoot = workspaceRoot;
    auto s = impl_->purgeWorkspace();
    impl_->workspaceRoot = saved;
    return s;
}

} // namespace roclint::retrieval
