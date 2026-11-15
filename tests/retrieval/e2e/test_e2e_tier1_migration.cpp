// =============================================================================
// Roclint Retrieval Subsystem — Tier 1 Schema Migration & Invalidation Tests
// Features F6 (Schema Constant), F7 (FTS Migration), F8 (Corrupt Cache Invalidation),
// and F9 (Unrelated App/AST Data Preservation).
// Conforms strictly to /W4 /w14061 /WX /utf-8 /permissive-.
// =============================================================================

#include "E2ETestHarness.hpp"
#include "E2EFixtures.hpp"

#include "roclint/retrieval/RetrievalEngine.hpp"
#include "roclint/retrieval/RetrievalIndex.hpp"
#include "roclint/retrieval/RetrievalTypes.hpp"
#include "retrieval/MockEmbeddingRuntime.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

[[nodiscard]] roclint::retrieval::IndexPolicy testMigrationPolicy() {
    roclint::retrieval::IndexPolicy p;
    p.modelId = "test-mock-embedding";
    p.modelRevision = "mock-rev-0000000000000000000000000000000000000000";
    p.dimensions = retrieval_test::MockEmbeddingRuntime::kMockDim;
    p.normalized = true;
    p.chunkerVersion = "text_chunker.v1";
    p.preprocessVersion = "mock.v1";
    return p;
}

[[nodiscard]] roclint::retrieval::IndexedChunk createMigrationChunk(
    const std::string& chunkId,
    const std::string& path,
    const std::string& text)
{
    roclint::retrieval::IndexedChunk c;
    c.chunkId.value = chunkId;
    c.sourceKind = roclint::retrieval::SourceKind::WorkspaceCode;
    c.location.sourcePath = path;
    c.location.contentHash = "hash_" + chunkId;
    c.location.byteStart = 0;
    c.location.byteEnd = static_cast<std::uint32_t>(text.size());
    c.location.lineStart = 1;
    c.location.lineEnd = 1;
    c.text = text;
    return c;
}

} // namespace

// =============================================================================
// Feature F6: Authoritative Schema Constant (5 Tests)
// =============================================================================

E2E_TEST(Tier1_SchemaMigration, F6_01_SchemaVersionConstantIsSynchronized, roclint::e2e::TestTier::Tier1, "F6") {
    // v3 includes successful per-source indexing coverage.
    int schemaVer = roclint::retrieval::RetrievalIndex::kSchemaVersion;
    E2E_CHECK_EQ(schemaVer, 3);
}

E2E_TEST(Tier1_SchemaMigration, F6_02_FreshDatabaseOpensAtCurrentVersion, roclint::e2e::TestTier::Tier1, "F6") {
    roclint::e2e::TempWorkspace ws("f6_02_ws");
    roclint::e2e::TestDatabaseFixture db("f6_02_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    std::string schemaVer = db.queryString("SELECT schema_version FROM retrieval_meta;");
    E2E_CHECK_EQ(schemaVer, std::to_string(roclint::retrieval::RetrievalIndex::kSchemaVersion));
}

E2E_TEST(Tier1_SchemaMigration, F6_03_MetaTableSchemaMatchesSpecification, roclint::e2e::TestTier::Tier1, "F6") {
    roclint::e2e::TempWorkspace ws("f6_03_ws");
    roclint::e2e::TestDatabaseFixture db("f6_03_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Verify all specified columns exist in retrieval_meta
    const std::vector<std::string> expectedCols = {
        "workspace_root", "schema_version", "model_id", "model_revision",
        "dimensions", "normalized", "chunker_version", "preprocess_version",
        "snapshot_gen", "coverage_complete", "indexed_chunks", "indexed_files",
        "last_refresh_ms"
    };

    for (const auto& col : expectedCols) {
        std::string sql = "SELECT count(*) FROM pragma_table_info('retrieval_meta') WHERE name = '" + col + "';";
        int count = db.queryInt(sql);
        E2E_CHECK_EQ(count, 1);
    }
}

E2E_TEST(Tier1_SchemaMigration, F6_04_PolicyMismatchTriggersRebuild, roclint::e2e::TestTier::Tier1, "F6") {
    roclint::e2e::TempWorkspace ws("f6_04_ws");
    roclint::e2e::TestDatabaseFixture db("f6_04_db");

    // Open with original policy
    auto policyA = testMigrationPolicy();
    {
        roclint::retrieval::RetrievalIndex idx(db.dbPath());
        E2E_REQUIRE(idx.open(ws.root(), policyA).ok());
        E2E_REQUIRE(idx.beginGeneration().ok());

        auto c = createMigrationChunk("c_mismatch", "file.cpp", "int oldPolicy() {}");
        roclint::retrieval::EmbeddingBatch emb;
        emb.dimensions = policyA.dimensions;
        emb.rows = 1;
        emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(c.text);

        E2E_REQUIRE(idx.insertChunks({c}, emb).ok());
        E2E_REQUIRE(idx.commitGeneration(true).ok());
        E2E_CHECK_EQ(idx.snapshotCoverage().value().indexedChunks, 1ULL);
    }

    // Open with different policy (e.g. different model revision)
    auto policyB = testMigrationPolicy();
    policyB.modelRevision = "new-rev-1111111111111111111111111111111111111111";

    {
        roclint::retrieval::RetrievalIndex idx(db.dbPath());
        // Safe invalidation should succeed and reset index
        auto openRes = idx.open(ws.root(), policyB);
        E2E_REQUIRE(openRes.ok());

        auto cov = idx.snapshotCoverage();
        E2E_REQUIRE(cov.ok());
        // Chunks from mismatched policy were safely wiped
        E2E_CHECK_EQ(cov.value().indexedChunks, 0ULL);

        std::string storedRev = db.queryString("SELECT model_revision FROM retrieval_meta;");
        E2E_CHECK_EQ(storedRev, policyB.modelRevision);
    }
}

E2E_TEST(Tier1_SchemaMigration, F6_05_CompatiblePolicyReopensWithoutWipe, roclint::e2e::TestTier::Tier1, "F6") {
    roclint::e2e::TempWorkspace ws("f6_05_ws");
    roclint::e2e::TestDatabaseFixture db("f6_05_db");

    auto policy = testMigrationPolicy();
    {
        roclint::retrieval::RetrievalIndex idx(db.dbPath());
        E2E_REQUIRE(idx.open(ws.root(), policy).ok());
        E2E_REQUIRE(idx.beginGeneration().ok());

        auto c = createMigrationChunk("c_compat", "file.cpp", "int compat() { return 1; }");
        roclint::retrieval::EmbeddingBatch emb;
        emb.dimensions = policy.dimensions;
        emb.rows = 1;
        emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(c.text);

        E2E_REQUIRE(idx.insertChunks({c}, emb).ok());
        E2E_REQUIRE(idx.recordSource("file.cpp", "hash_c_compat", roclint::retrieval::SourceKind::WorkspaceCode).ok());
        E2E_REQUIRE(idx.commitGeneration(true).ok());
    }

    // Reopen with identical compatible policy
    {
        roclint::retrieval::RetrievalIndex idx(db.dbPath());
        E2E_REQUIRE(idx.open(ws.root(), policy).ok());
        auto cov = idx.snapshotCoverage();
        E2E_REQUIRE(cov.ok());
        E2E_CHECK_EQ(cov.value().indexedChunks, 1ULL);
    }
}

// =============================================================================
// Feature F7: Virtual Table FTS Migration (5 Tests)
// =============================================================================

E2E_TEST(Tier1_SchemaMigration, F7_01_LegacyFtsTableWithoutSourceKindMigrates, roclint::e2e::TestTier::Tier1, "F7") {
    roclint::e2e::TempWorkspace ws("f7_01_ws");
    roclint::e2e::TestDatabaseFixture db("f7_01_db");

    // Seed authentic legacy database lacking source_kind in retrieval_fts
    db.seedLegacyV1Database(ws.root(), true, false);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    auto openStatus = idx.open(ws.root(), testMigrationPolicy());
    E2E_REQUIRE(openStatus.ok());

    std::string schemaVer = db.queryString("SELECT schema_version FROM retrieval_meta;");
    E2E_CHECK_EQ(schemaVer, std::to_string(roclint::retrieval::RetrievalIndex::kSchemaVersion));
}

E2E_TEST(Tier1_SchemaMigration, F7_02_MigratedFtsColumnsContainSourceKindAndGenerations, roclint::e2e::TestTier::Tier1, "F7") {
    roclint::e2e::TempWorkspace ws("f7_02_ws");
    roclint::e2e::TestDatabaseFixture db("f7_02_db");

    db.seedLegacyV1Database(ws.root(), true, false);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Verify FTS table structure contains source_kind and published_gen
    int count = db.queryInt("SELECT count(*) FROM pragma_table_info('retrieval_fts') WHERE name = 'source_kind';");
    E2E_CHECK_EQ(count, 1);

    int pubGenCount = db.queryInt("SELECT count(*) FROM pragma_table_info('retrieval_fts') WHERE name = 'published_gen';");
    E2E_CHECK_EQ(pubGenCount, 1);
}

E2E_TEST(Tier1_SchemaMigration, F7_03_MigratedFtsSupportsChunkInsertion, roclint::e2e::TestTier::Tier1, "F7") {
    roclint::e2e::TempWorkspace ws("f7_03_ws");
    roclint::e2e::TestDatabaseFixture db("f7_03_db");

    db.seedLegacyV1Database(ws.root(), true, false);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    E2E_REQUIRE(idx.beginGeneration().ok());
    const std::string text = "void postMigrationInsert() { int valid = 1; }";
    auto c = createMigrationChunk("c_post_mig", "mig.cpp", text);

    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testMigrationPolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(text);

    auto insStatus = idx.insertChunks({c}, emb);
    // Must succeed without SQLite schema mismatch error
    E2E_REQUIRE(insStatus.ok());

    E2E_REQUIRE(idx.recordSource("mig.cpp", "hash_c_post_mig", roclint::retrieval::SourceKind::WorkspaceCode).ok());
    E2E_REQUIRE(idx.commitGeneration(true).ok());

    int ftsRows = db.queryInt("SELECT count(*) FROM retrieval_fts WHERE chunk_id = 'c_post_mig';");
    E2E_CHECK_EQ(ftsRows, 1);
}

E2E_TEST(Tier1_SchemaMigration, F7_04_MigratedFtsSupportsLexicalSearch, roclint::e2e::TestTier::Tier1, "F7") {
    roclint::e2e::TempWorkspace ws("f7_04_ws");
    roclint::e2e::TestDatabaseFixture db("f7_04_db");

    db.seedLegacyV1Database(ws.root(), true, false);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    E2E_REQUIRE(idx.beginGeneration().ok());
    const std::string text = "int calculateLexicalTargetScore() { return 777; }";
    auto c = createMigrationChunk("c_lexical", "lexical.cpp", text);

    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testMigrationPolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(text);

    E2E_REQUIRE(idx.insertChunks({c}, emb).ok());
    E2E_REQUIRE(idx.recordSource("lexical.cpp", "hash_c_lexical", roclint::retrieval::SourceKind::WorkspaceCode).ok());
    E2E_REQUIRE(idx.commitGeneration(true).ok());

    roclint::retrieval::RetrievalFilter filter;
    filter.workspaceRoot = ws.root().generic_string();
    roclint::retrieval::RetrievalOptions opts;
    opts.minFusedScore = 0.0f;

    auto res = idx.retrieve(emb.data, "calculateLexicalTargetScore", filter, opts);
    E2E_REQUIRE(res.ok());
    E2E_CHECK(res.value().size() >= 1ULL);
    if (!res.value().empty()) {
        E2E_CHECK(res.value()[0].fusedScore > 0.0f);
    }
}

E2E_TEST(Tier1_SchemaMigration, F7_05_MigratedFtsRespectsPublishedGenFilter, roclint::e2e::TestTier::Tier1, "F7") {
    roclint::e2e::TempWorkspace ws("f7_05_ws");
    roclint::e2e::TestDatabaseFixture db("f7_05_db");

    db.seedLegacyV1Database(ws.root(), true, false);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Begin generation and insert chunk without committing
    E2E_REQUIRE(idx.beginGeneration().ok());
    const std::string text = "void secretStagedTokenFunction() {}";
    auto c = createMigrationChunk("c_secret", "secret.cpp", text);

    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testMigrationPolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(text);

    E2E_REQUIRE(idx.insertChunks({c}, emb).ok());

    // Retrieve via FTS text query
    roclint::retrieval::RetrievalFilter filter;
    filter.workspaceRoot = ws.root().generic_string();
    roclint::retrieval::RetrievalOptions opts;
    opts.minFusedScore = 0.0f;

    auto res = idx.retrieve(emb.data, "secretStagedTokenFunction", filter, opts);
    E2E_REQUIRE(res.ok());
    // Must NOT return uncommitted staged chunk from FTS
    E2E_CHECK_EQ(res.value().size(), 0ULL);
}

// =============================================================================
// Feature F8: Corrupted Cache Invalidation (5 Tests)
// =============================================================================

E2E_TEST(Tier1_SchemaMigration, F8_01_CorruptZeroExcerptPurgedOnUpgrade, roclint::e2e::TestTier::Tier1, "F8") {
    // Verifies Bob's Finding 5 reproduction: corrupt_cache_upgrade
    roclint::e2e::TempWorkspace ws("f8_01_ws");
    roclint::e2e::TestDatabaseFixture db("f8_01_db");

    // Seed corrupt "0" excerpt
    db.seedLegacyV1Database(ws.root(), true, true);
    int beforeCorrupt = db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_text = '0';");
    E2E_CHECK_EQ(beforeCorrupt, 1);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Corrupted chunk must be purged
    int afterCorrupt = db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_text = '0';");
    E2E_CHECK_EQ(afterCorrupt, 0);
}

E2E_TEST(Tier1_SchemaMigration, F8_02_CorruptZeroExcerptPurgedFromFts, roclint::e2e::TestTier::Tier1, "F8") {
    roclint::e2e::TempWorkspace ws("f8_02_ws");
    roclint::e2e::TestDatabaseFixture db("f8_02_db");

    db.seedLegacyV1Database(ws.root(), true, true);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    int ftsCorrupt = db.queryInt("SELECT count(*) FROM retrieval_fts WHERE chunk_text = '0';");
    E2E_CHECK_EQ(ftsCorrupt, 0);
}

E2E_TEST(Tier1_SchemaMigration, F8_03_CorruptCacheUpgradeResetsCoverageFlags, roclint::e2e::TestTier::Tier1, "F8") {
    roclint::e2e::TempWorkspace ws("f8_03_ws");
    roclint::e2e::TestDatabaseFixture db("f8_03_db");

    db.seedLegacyV1Database(ws.root(), true, true);
    // Legacy database was seeded with coverage_complete = 1
    int beforeCov = db.queryInt("SELECT coverage_complete FROM retrieval_meta;");
    E2E_CHECK_EQ(beforeCov, 1);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Purging corrupt excerpts must reset coverage to incomplete
    int afterCov = db.queryInt("SELECT coverage_complete FROM retrieval_meta;");
    E2E_CHECK_EQ(afterCov, 0);
}

E2E_TEST(Tier1_SchemaMigration, F8_04_ValidChunksPreservedDuringCorruptPurge, roclint::e2e::TestTier::Tier1, "F8") {
    roclint::e2e::TempWorkspace ws("f8_04_ws");
    roclint::e2e::TestDatabaseFixture db("f8_04_db");

    db.seedLegacyV1Database(ws.root(), true, true);

    // Insert a valid non-corrupt chunk into the legacy DB before upgrading
    db.executeSql(
        "INSERT INTO retrieval_chunks "
        "(chunk_id, workspace_root, relative_path, content_hash, source_kind, "
        " byte_start, byte_end, line_start, line_end, snapshot_gen, chunk_text) "
        "VALUES ('valid_chunk_001', '" + ws.root().generic_string() + "', "
        "'valid.cpp', 'h_valid', 0, 0, 20, 1, 1, 1, 'int legitimateCode() {}');"
    );

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Corrupted chunk purged
    E2E_CHECK_EQ(db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_text = '0';"), 0);

    // Valid chunk preserved
    int validCount = db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_id = 'valid_chunk_001';");
    E2E_CHECK_EQ(validCount, 1);
}

E2E_TEST(Tier1_SchemaMigration, LegitimateSingleByteZeroSurvivesUpgrade, roclint::e2e::TestTier::Tier1, "F8") {
    roclint::e2e::TempWorkspace ws("valid_zero_cache");
    roclint::e2e::TestDatabaseFixture db("valid_zero_cache");
    ws.writeFile("zero.cpp", "0");
    db.seedLegacyV1Database(ws.root(), true, false);
    E2E_REQUIRE(db.executeSql(
        "INSERT INTO retrieval_chunks "
        "(chunk_id, workspace_root, relative_path, content_hash, source_kind, "
        "byte_start, byte_end, line_start, line_end, snapshot_gen, chunk_text) "
        "VALUES ('valid_zero', '" + ws.root().generic_string() + "', "
        "'zero.cpp', 'zero_hash', 0, 0, 1, 1, 1, 1, '0');"));
    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());
    E2E_CHECK_EQ(db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_id='valid_zero' AND chunk_text='0';"), 1);
    std::ifstream original(ws.root() / "zero.cpp");
    std::string content((std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>());
    E2E_CHECK_EQ(content, "0");
}

E2E_TEST(Tier1_SchemaMigration, F8_05_CorruptCacheDoesNotCrashEngine, roclint::e2e::TestTier::Tier1, "F8") {
    roclint::e2e::TempWorkspace ws("f8_05_ws");
    roclint::e2e::TestDatabaseFixture db("f8_05_db");

    db.seedLegacyV1Database(ws.root(), true, true);

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngineConfig cfg;
    cfg.indexDatabasePath = db.dbPath();
    cfg.policy = testMigrationPolicy();

    roclint::retrieval::RetrievalEngine engine(mockRuntime, cfg);

    // Engine initialization and indexing must purge corruption without crash or exception
    auto idxRes = engine.indexWorkspace(ws.root(), "ws_f8_05");
    E2E_REQUIRE(idxRes.ok());

    auto st = engine.status();
    E2E_CHECK(st == roclint::retrieval::RetrievalStatus::Ready ||
              st == roclint::retrieval::RetrievalStatus::Partial);
}

// =============================================================================
// Feature F9: Unrelated AST/App Data Preservation (5 Tests)
// =============================================================================

E2E_TEST(Tier1_SchemaMigration, F9_01_NonRetrievalTablesPreservedAcrossOpen, roclint::e2e::TestTier::Tier1, "F9") {
    roclint::e2e::TempWorkspace ws("f9_01_ws");
    roclint::e2e::TestDatabaseFixture db("f9_01_db");

    // Pre-create unrelated AST symbols table
    db.executeSql(
        "CREATE TABLE ast_symbols (sym_id TEXT PRIMARY KEY, name TEXT, kind INTEGER);"
        "INSERT INTO ast_symbols VALUES ('sym_widget', 'roclint::Widget', 42);"
    );

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    int symCount = db.queryInt("SELECT count(*) FROM ast_symbols WHERE sym_id = 'sym_widget';");
    E2E_CHECK_EQ(symCount, 1);

    std::string symName = db.queryString("SELECT name FROM ast_symbols WHERE sym_id = 'sym_widget';");
    E2E_CHECK_EQ(symName, "roclint::Widget");
}

E2E_TEST(Tier1_SchemaMigration, F9_02_NonRetrievalTablesPreservedAcrossMigration, roclint::e2e::TestTier::Tier1, "F9") {
    roclint::e2e::TempWorkspace ws("f9_02_ws");
    roclint::e2e::TestDatabaseFixture db("f9_02_db");

    db.seedLegacyV1Database(ws.root(), true, false);

    // Insert user preferences and AST nodes
    db.executeSql(
        "CREATE TABLE user_preferences (pref_key TEXT PRIMARY KEY, pref_val TEXT);"
        "INSERT INTO user_preferences VALUES ('theme', 'nord_dark');"
        "CREATE TABLE ast_nodes (node_id INTEGER PRIMARY KEY, syntax_type TEXT);"
        "INSERT INTO ast_nodes VALUES (101, 'translation_unit');"
    );

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Verify application tables were untouched
    std::string theme = db.queryString("SELECT pref_val FROM user_preferences WHERE pref_key = 'theme';");
    E2E_CHECK_EQ(theme, "nord_dark");

    std::string nodeType = db.queryString("SELECT syntax_type FROM ast_nodes WHERE node_id = 101;");
    E2E_CHECK_EQ(nodeType, "translation_unit");
}

E2E_TEST(Tier1_SchemaMigration, F9_03_NonRetrievalTablesPreservedAcrossForgetWorkspace, roclint::e2e::TestTier::Tier1, "F9") {
    roclint::e2e::TempWorkspace ws("f9_03_ws");
    roclint::e2e::TestDatabaseFixture db("f9_03_db");

    db.executeSql(
        "CREATE TABLE app_meta (key TEXT PRIMARY KEY, value TEXT);"
        "INSERT INTO app_meta VALUES ('host_version', '1.2.3');"
    );

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());
    E2E_REQUIRE(idx.beginGeneration().ok());

    auto c = createMigrationChunk("c_forget", "f.cpp", "int f() {}");
    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testMigrationPolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(c.text);

    E2E_REQUIRE(idx.insertChunks({c}, emb).ok());
    E2E_REQUIRE(idx.recordSource("f.cpp", "hash_c_forget", roclint::retrieval::SourceKind::WorkspaceCode).ok());
    E2E_REQUIRE(idx.commitGeneration(true).ok());

    // Forget workspace
    auto forgetStatus = idx.forgetWorkspace(ws.root().generic_string());
    E2E_REQUIRE(forgetStatus.ok());

    // Retrieval chunks deleted for this workspace
    E2E_CHECK_EQ(db.queryInt("SELECT count(*) FROM retrieval_chunks;"), 0);

    // App meta table remains untouched
    std::string val = db.queryString("SELECT value FROM app_meta WHERE key = 'host_version';");
    E2E_CHECK_EQ(val, "1.2.3");
}

E2E_TEST(Tier1_SchemaMigration, F9_04_WorkspaceFilesUntouchedByMigration, roclint::e2e::TestTier::Tier1, "F9") {
    roclint::e2e::TempWorkspace ws("f9_04_ws");
    roclint::e2e::TestDatabaseFixture db("f9_04_db");

    const std::string originalSource = "/* Original User Source Code */\nint main() { return 0; }\n";
    auto srcPath = ws.writeFile("main.cpp", originalSource);

    db.seedLegacyV1Database(ws.root(), true, true);

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());

    // Read file on disk
    std::ifstream ifs(srcPath, std::ios::binary);
    std::string currentContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    E2E_CHECK_EQ(currentContent, originalSource);
}

E2E_TEST(Tier1_SchemaMigration, F9_05_NonRetrievalTablesPreservedAcrossRollback, roclint::e2e::TestTier::Tier1, "F9") {
    roclint::e2e::TempWorkspace ws("f9_05_ws");
    roclint::e2e::TestDatabaseFixture db("f9_05_db");

    db.executeSql(
        "CREATE TABLE diagnostic_logs (id INTEGER PRIMARY KEY, msg TEXT);"
        "INSERT INTO diagnostic_logs VALUES (1, 'Log entry before rollback');"
    );

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testMigrationPolicy()).ok());
    E2E_REQUIRE(idx.beginGeneration().ok());

    auto c = createMigrationChunk("c_rb_ast", "rb.cpp", "int toRollback() {}");
    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testMigrationPolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(c.text);

    E2E_REQUIRE(idx.insertChunks({c}, emb).ok());
    idx.rollbackGeneration();

    int logCount = db.queryInt("SELECT count(*) FROM diagnostic_logs;");
    E2E_CHECK_EQ(logCount, 1);
    std::string logMsg = db.queryString("SELECT msg FROM diagnostic_logs WHERE id = 1;");
    E2E_CHECK_EQ(logMsg, "Log entry before rollback");
}
