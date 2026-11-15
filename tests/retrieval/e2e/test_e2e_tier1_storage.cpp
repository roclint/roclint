// =============================================================================
// Roclint Retrieval Subsystem — Tier 1 Storage & Snapshot Tests
// Features F1 (Generational Snapshot Isolation) and F2 (Unchanged Source Reuse)
// Conforms strictly to /W4 /w14061 /WX /utf-8 /permissive-.
// =============================================================================

#include "E2ETestHarness.hpp"
#include "E2EFixtures.hpp"

#include "roclint/retrieval/RetrievalEngine.hpp"
#include "roclint/retrieval/RetrievalIndex.hpp"
#include "roclint/retrieval/RetrievalTypes.hpp"
#include "roclint/retrieval/TextChunker.hpp"
#include "retrieval/MockEmbeddingRuntime.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

[[nodiscard]] roclint::retrieval::IndexPolicy testStoragePolicy() {
    roclint::retrieval::IndexPolicy p;
    p.modelId = "test-mock-embedding";
    p.modelRevision = "mock-rev-0000000000000000000000000000000000000000";
    p.dimensions = retrieval_test::MockEmbeddingRuntime::kMockDim;
    p.normalized = true;
    p.chunkerVersion = "text_chunker.v1.target512.overlap64";
    p.preprocessVersion = "mock.v1";
    return p;
}

[[nodiscard]] roclint::retrieval::RetrievalEngineConfig testEngineConfig(
    const std::filesystem::path& dbPath)
{
    roclint::retrieval::RetrievalEngineConfig cfg;
    cfg.indexDatabasePath = dbPath;
    cfg.policy = testStoragePolicy();
    cfg.minFusedScore = 0.0f;
    cfg.revalidateSourceHashes = true;
    return cfg;
}

[[nodiscard]] roclint::retrieval::IndexedChunk createSampleChunk(
    const std::string& chunkId,
    const std::string& path,
    const std::string& hash,
    const std::string& text)
{
    roclint::retrieval::IndexedChunk c;
    c.chunkId.value = chunkId;
    c.sourceKind = roclint::retrieval::SourceKind::WorkspaceCode;
    c.location.sourcePath = path;
    c.location.contentHash = hash;
    c.location.byteStart = 0;
    c.location.byteEnd = static_cast<std::uint32_t>(text.size());
    c.location.lineStart = 1;
    c.location.lineEnd = 1;
    c.text = text;
    return c;
}

} // namespace

// =============================================================================
// Feature F1: Generational Snapshot Isolation (5 Tests)
// =============================================================================

E2E_TEST(Tier1_SnapshotPublication, F1_01_UncommittedStagingIsolation, roclint::e2e::TestTier::Tier1, "F1") {
    roclint::e2e::TempWorkspace ws("f1_01_ws");
    roclint::e2e::TestDatabaseFixture db("f1_01_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    auto openStatus = idx.open(ws.root(), testStoragePolicy());
    E2E_REQUIRE(openStatus.ok());

    auto beginStatus = idx.beginGeneration();
    E2E_REQUIRE(beginStatus.ok());

    const std::string sampleText = "void isolatedStagingFunction() { int staged = 100; }";
    auto chunk = createSampleChunk("chunk_f1_01", "staged.cpp", "hash_01", sampleText);

    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testStoragePolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(sampleText);

    auto insStatus = idx.insertChunks({chunk}, emb);
    E2E_REQUIRE(insStatus.ok());

    auto srcStatus = idx.recordSource("staged.cpp", "hash_01", roclint::retrieval::SourceKind::WorkspaceCode);
    E2E_REQUIRE(srcStatus.ok());

    // Do NOT commit generation — reader must observe empty published snapshot
    roclint::retrieval::RetrievalFilter filter;
    filter.workspaceRoot = ws.root().generic_string();

    roclint::retrieval::RetrievalOptions opts;
    opts.minFusedScore = 0.0f;

    std::vector<float> queryEmb = emb.data;
    auto retrieveRes = idx.retrieve(queryEmb, "isolatedStagingFunction", filter, opts);
    E2E_REQUIRE(retrieveRes.ok());
    // Staged uncommitted chunks must NOT be returned
    E2E_CHECK_EQ(retrieveRes.value().size(), 0ULL);
}

E2E_TEST(Tier1_SnapshotPublication, F1_02_RollbackDeletesStagedPendingChunks, roclint::e2e::TestTier::Tier1, "F1") {
    roclint::e2e::TempWorkspace ws("f1_02_ws");
    roclint::e2e::TestDatabaseFixture db("f1_02_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testStoragePolicy()).ok());
    E2E_REQUIRE(idx.beginGeneration().ok());

    const std::string text = "int pendingFunction() { return 1; }";
    auto chunk = createSampleChunk("chunk_f1_02", "pending.cpp", "hash_02", text);

    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testStoragePolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(text);

    E2E_REQUIRE(idx.insertChunks({chunk}, emb).ok());
    E2E_REQUIRE(idx.recordSource("pending.cpp", "hash_02", roclint::retrieval::SourceKind::WorkspaceCode).ok());

    // Verify row exists in DB with published_gen = 0
    int pendingCount = db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_id = 'chunk_f1_02' AND published_gen = 0;");
    E2E_CHECK_EQ(pendingCount, 1);

    // Rollback
    idx.rollbackGeneration();

    // Verify chunk and source were purged
    int afterRollbackChunks = db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_id = 'chunk_f1_02';");
    E2E_CHECK_EQ(afterRollbackChunks, 0);

    int afterRollbackSources = db.queryInt("SELECT count(*) FROM retrieval_sources WHERE relative_path = 'pending.cpp';");
    E2E_CHECK_EQ(afterRollbackSources, 0);
}

E2E_TEST(Tier1_SnapshotPublication, F1_03_PublishedChunksImmutableAcrossRollback, roclint::e2e::TestTier::Tier1, "F1") {
    roclint::e2e::TempWorkspace ws("f1_03_ws");
    roclint::e2e::TestDatabaseFixture db("f1_03_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testStoragePolicy()).ok());

    // Commit Gen 1
    E2E_REQUIRE(idx.beginGeneration().ok());
    const std::string gen1Text = "int publishedAlpha() { return 42; }";
    auto c1 = createSampleChunk("chunk_pub_01", "alpha.cpp", "hash_alpha", gen1Text);

    roclint::retrieval::EmbeddingBatch emb1;
    emb1.dimensions = testStoragePolicy().dimensions;
    emb1.rows = 1;
    emb1.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(gen1Text);

    E2E_REQUIRE(idx.insertChunks({c1}, emb1).ok());
    E2E_REQUIRE(idx.recordSource("alpha.cpp", "hash_alpha", roclint::retrieval::SourceKind::WorkspaceCode).ok());
    E2E_REQUIRE(idx.commitGeneration(true).ok());

    auto cov1 = idx.snapshotCoverage();
    E2E_REQUIRE(cov1.ok());
    E2E_CHECK_EQ(cov1.value().indexedChunks, 1ULL);
    E2E_CHECK_EQ(cov1.value().snapshotGeneration, 1ULL);

    // Begin Gen 2, insert new chunk, then rollback
    E2E_REQUIRE(idx.beginGeneration().ok());
    const std::string gen2Text = "int stagedBeta() { return 99; }";
    auto c2 = createSampleChunk("chunk_staged_02", "beta.cpp", "hash_beta", gen2Text);
    roclint::retrieval::EmbeddingBatch emb2;
    emb2.dimensions = testStoragePolicy().dimensions;
    emb2.rows = 1;
    emb2.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(gen2Text);

    E2E_REQUIRE(idx.insertChunks({c2}, emb2).ok());
    idx.rollbackGeneration();

    // Verify published Gen 1 chunk is intact and retrievable
    roclint::retrieval::RetrievalFilter filter;
    filter.workspaceRoot = ws.root().generic_string();
    roclint::retrieval::RetrievalOptions opts;
    opts.minFusedScore = 0.0f;

    auto res = idx.retrieve(emb1.data, "publishedAlpha", filter, opts);
    E2E_REQUIRE(res.ok());
    E2E_CHECK_EQ(res.value().size(), 1ULL);
    if (!res.value().empty()) {
        E2E_CHECK_EQ(res.value()[0].chunkId.value, "chunk_pub_01");
        E2E_CHECK_EQ(res.value()[0].excerpt, gen1Text);
    }
}

E2E_TEST(Tier1_SnapshotPublication, F1_04_StagedDuplicateChunkPreservesPublishedOnRollback, roclint::e2e::TestTier::Tier1, "F1") {
    // Verifies Bob's Finding 1 reproduction:
    // Staging an identical chunk ID in Gen 2 and then rolling back must NOT delete Gen 1's published chunk.
    roclint::e2e::TempWorkspace ws("f1_04_ws");
    roclint::e2e::TestDatabaseFixture db("f1_04_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testStoragePolicy()).ok());

    // Gen 1 publication
    E2E_REQUIRE(idx.beginGeneration().ok());
    const std::string sharedText = "void preservedSharedFunction() {}";
    auto chunk = createSampleChunk("chunk_shared_01", "shared.cpp", "hash_shared", sharedText);

    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testStoragePolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(sharedText);

    E2E_REQUIRE(idx.insertChunks({chunk}, emb).ok());
    E2E_REQUIRE(idx.recordSource("shared.cpp", "hash_shared", roclint::retrieval::SourceKind::WorkspaceCode).ok());
    E2E_REQUIRE(idx.commitGeneration(true).ok());

    // Gen 2: insert identical chunk ID
    E2E_REQUIRE(idx.beginGeneration().ok());
    E2E_REQUIRE(idx.insertChunks({chunk}, emb).ok());

    // Rollback Gen 2
    idx.rollbackGeneration();

    // Verify published chunk still exists in database and has published_gen > 0
    int pubCount = db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE chunk_id = 'chunk_shared_01' AND published_gen > 0;");
    E2E_CHECK_EQ(pubCount, 1);

    // Verify retrieve still succeeds
    roclint::retrieval::RetrievalFilter filter;
    filter.workspaceRoot = ws.root().generic_string();
    roclint::retrieval::RetrievalOptions opts;
    opts.minFusedScore = 0.0f;

    auto res = idx.retrieve(emb.data, "preservedSharedFunction", filter, opts);
    E2E_REQUIRE(res.ok());
    E2E_CHECK_EQ(res.value().size(), 1ULL);
    if (!res.value().empty()) {
        E2E_CHECK_EQ(res.value()[0].chunkId.value, "chunk_shared_01");
    }
}

E2E_TEST(Tier1_SnapshotPublication, F1_05_CommitPromotesPendingToPublished, roclint::e2e::TestTier::Tier1, "F1") {
    roclint::e2e::TempWorkspace ws("f1_05_ws");
    roclint::e2e::TestDatabaseFixture db("f1_05_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testStoragePolicy()).ok());
    E2E_REQUIRE(idx.beginGeneration().ok());

    const std::string text = "int promotedFunction() { return 10; }";
    auto chunk = createSampleChunk("chunk_promoted", "promoted.cpp", "hash_p", text);

    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testStoragePolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(text);

    E2E_REQUIRE(idx.insertChunks({chunk}, emb).ok());
    E2E_REQUIRE(idx.recordSource("promoted.cpp", "hash_p", roclint::retrieval::SourceKind::WorkspaceCode).ok());

    // Commit generation
    auto commitStatus = idx.commitGeneration(true);
    E2E_REQUIRE(commitStatus.ok());

    E2E_CHECK_EQ(idx.currentGeneration(), 1ULL);

    int pubGen = db.queryInt("SELECT published_gen FROM retrieval_chunks WHERE chunk_id = 'chunk_promoted';");
    E2E_CHECK_EQ(pubGen, 1);

    auto cov = idx.snapshotCoverage();
    E2E_REQUIRE(cov.ok());
    E2E_CHECK_EQ(cov.value().coverageComplete, true);
    E2E_CHECK_EQ(cov.value().indexedChunks, 1ULL);
}

// =============================================================================
// Feature F2: Unchanged Source & Chunk Reuse (5 Tests)
// =============================================================================

E2E_TEST(Tier1_SnapshotPublication, F2_01_IdenticalContentHashReusedWithoutReembedding, roclint::e2e::TestTier::Tier1, "F2") {
    roclint::e2e::TempWorkspace ws("f2_01_ws");
    roclint::e2e::TestDatabaseFixture db("f2_01_db");

    ws.writeFile("source.cpp", "int calculateSum(int a, int b) { return a + b; }\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    auto config = testEngineConfig(db.dbPath());
    roclint::retrieval::RetrievalEngine engine(mockRuntime, config);

    // Initial indexing pass
    auto res1 = engine.indexWorkspace(ws.root(), "ws_ident");
    E2E_REQUIRE(res1.ok());
    E2E_CHECK_EQ(res1.value().filesIndexed, 1ULL);
    E2E_CHECK(res1.value().chunksAdded > 0ULL);

    const std::size_t initialCalls = mockRuntime->callCount();
    E2E_CHECK(initialCalls > 0ULL);

    // Second pass on unchanged workspace
    auto res2 = engine.indexWorkspace(ws.root(), "ws_ident");
    E2E_REQUIRE(res2.ok());

    // Call count must NOT increase — chunks and embeddings are reused
    E2E_CHECK_EQ(mockRuntime->callCount(), initialCalls);
    E2E_CHECK_EQ(res2.value().chunksAdded, 0ULL);
    E2E_CHECK_EQ(res2.value().filesIndexed, 1ULL);
}

E2E_TEST(Tier1_SnapshotPublication, F2_02_IsSourceUnchangedMatchesCommittedHash, roclint::e2e::TestTier::Tier1, "F2") {
    roclint::e2e::TempWorkspace ws("f2_02_ws");
    roclint::e2e::TestDatabaseFixture db("f2_02_db");

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testStoragePolicy()).ok());
    E2E_REQUIRE(idx.beginGeneration().ok());

    const std::string text = "int testFile() { return 0; }";
    auto chunk = createSampleChunk("chunk_f2_02", "util.cpp", "sha256_exact_hash_123", text);
    roclint::retrieval::EmbeddingBatch emb;
    emb.dimensions = testStoragePolicy().dimensions;
    emb.rows = 1;
    emb.data = retrieval_test::MockEmbeddingRuntime::expectedEmbedding(text);

    E2E_REQUIRE(idx.insertChunks({chunk}, emb).ok());
    E2E_REQUIRE(idx.recordSource("util.cpp", "sha256_exact_hash_123", roclint::retrieval::SourceKind::WorkspaceCode).ok());
    E2E_REQUIRE(idx.commitGeneration(true).ok());

    // Matches committed hash
    E2E_CHECK_EQ(idx.isSourceUnchanged("util.cpp", "sha256_exact_hash_123"), true);
    // Different hash
    E2E_CHECK_EQ(idx.isSourceUnchanged("util.cpp", "sha256_modified_hash"), false);
    // Non-existent path
    E2E_CHECK_EQ(idx.isSourceUnchanged("other.cpp", "sha256_exact_hash_123"), false);
}

E2E_TEST(Tier1_SnapshotPublication, F2_03_ChangedSourceTriggersReembedding, roclint::e2e::TestTier::Tier1, "F2") {
    roclint::e2e::TempWorkspace ws("f2_03_ws");
    roclint::e2e::TestDatabaseFixture db("f2_03_db");

    ws.writeFile("mod.cpp", "int versionOne() { return 1; }\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testEngineConfig(db.dbPath()));

    auto res1 = engine.indexWorkspace(ws.root(), "ws_mod");
    E2E_REQUIRE(res1.ok());
    const std::size_t callsAfterPass1 = mockRuntime->callCount();

    // Modify file
    ws.writeFile("mod.cpp", "int versionTwo() { return 2; }\n// additional commentary\n");

    auto res2 = engine.indexWorkspace(ws.root(), "ws_mod");
    E2E_REQUIRE(res2.ok());

    // Must trigger new embedding call
    E2E_CHECK(mockRuntime->callCount() > callsAfterPass1);
    E2E_CHECK(res2.value().chunksAdded > 0ULL);
}

E2E_TEST(Tier1_SnapshotPublication, F2_04_PartialWorkspaceRefreshPreservesUnmodifiedSourceChunks, roclint::e2e::TestTier::Tier1, "F2") {
    roclint::e2e::TempWorkspace ws("f2_04_ws");
    roclint::e2e::TestDatabaseFixture db("f2_04_db");

    ws.writeFile("file_a.cpp", "int untouchedFunctionA() { return 100; }\n");
    ws.writeFile("file_b.cpp", "int modFunctionB() { return 200; }\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testEngineConfig(db.dbPath()));

    auto fullRes = engine.indexWorkspace(ws.root(), "ws_partial");
    E2E_REQUIRE(fullRes.ok());
    E2E_CHECK_EQ(fullRes.value().filesIndexed, 2ULL);

    // Modify file_b only and run incremental indexPaths on file_b
    ws.writeFile("file_b.cpp", "int modFunctionBUpdated() { return 201; }\n");
    auto incRes = engine.indexPaths(ws.root(), "ws_partial", {"file_b.cpp"});
    E2E_REQUIRE(incRes.ok());

    // Query for untouchedFunctionA — must still be present and retrievable
    roclint::retrieval::RetrievalRequest req;
    req.requestId = "req_f2_04";
    req.promptText = "untouchedFunctionA";
    req.workspace.workspaceRoot = ws.root().generic_string();

    auto resp = engine.retrieve(req);
    E2E_CHECK_EQ(resp.status, roclint::retrieval::RetrievalStatus::Ready);
    E2E_CHECK(resp.hits.size() >= 1ULL);
    if (!resp.hits.empty()) {
        E2E_CHECK(resp.hits[0].excerpt.find("untouchedFunctionA") != std::string::npos);
    }
}

E2E_TEST(Tier1_SnapshotPublication, F2_05_ValidateSourceHashDetectsFileModification, roclint::e2e::TestTier::Tier1, "F2") {
    roclint::e2e::TempWorkspace ws("f2_05_ws");
    roclint::e2e::TestDatabaseFixture db("f2_05_db");

    auto filePath = ws.writeFile("validated.cpp", "const char* msg = \"Hello Provenance\";\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testEngineConfig(db.dbPath()));

    auto idxRes = engine.indexWorkspace(ws.root(), "ws_val");
    E2E_REQUIRE(idxRes.ok());

    roclint::retrieval::RetrievalIndex idx(db.dbPath());
    E2E_REQUIRE(idx.open(ws.root(), testStoragePolicy()).ok());

    std::string recordedHash = db.queryString("SELECT content_hash FROM retrieval_sources WHERE relative_path = 'validated.cpp';");
    E2E_CHECK_NE(recordedHash, "");

    // Exact match on disk
    auto valMatch = idx.validateSourceHash("validated.cpp", recordedHash, ws.root());
    E2E_REQUIRE(valMatch.ok());
    E2E_CHECK_EQ(valMatch.value(), true);

    // Non-matching hash
    auto valMismatch = idx.validateSourceHash("validated.cpp", "tampered_fake_hash", ws.root());
    E2E_REQUIRE(valMismatch.ok());
    E2E_CHECK_EQ(valMismatch.value(), false);

    // Modify file on disk
    ws.writeFile("validated.cpp", "const char* msg = \"Modified Content\";\n");
    auto valAfterMod = idx.validateSourceHash("validated.cpp", recordedHash, ws.root());
    E2E_REQUIRE(valAfterMod.ok());
    E2E_CHECK_EQ(valAfterMod.value(), false);
}
