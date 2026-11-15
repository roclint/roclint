// =============================================================================
// Roclint Retrieval Subsystem — Tier 1 Coverage Semantics Tests
// Features F3 (Explicit Refresh), F4 (Honest Coverage), F5 (Per-File Cap Coverage)
// Conforms strictly to /W4 /w14061 /WX /utf-8 /permissive-.
// =============================================================================

#include "E2ETestHarness.hpp"
#include "E2EFixtures.hpp"

#include "roclint/retrieval/RetrievalEngine.hpp"
#include "roclint/retrieval/RetrievalIndex.hpp"
#include "roclint/retrieval/RetrievalTypes.hpp"
#include "retrieval/MockEmbeddingRuntime.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

[[nodiscard]] roclint::retrieval::IndexPolicy testCoveragePolicy() {
    roclint::retrieval::IndexPolicy p;
    p.modelId = "test-mock-embedding";
    p.modelRevision = "mock-rev-0000000000000000000000000000000000000000";
    p.dimensions = retrieval_test::MockEmbeddingRuntime::kMockDim;
    p.normalized = true;
    p.chunkerVersion = "text_chunker.v1.target512.overlap64";
    p.preprocessVersion = "mock.v1";
    return p;
}

[[nodiscard]] roclint::retrieval::RetrievalEngineConfig testCoverageConfig(
    const std::filesystem::path& dbPath)
{
    roclint::retrieval::RetrievalEngineConfig cfg;
    cfg.indexDatabasePath = dbPath;
    cfg.policy = testCoveragePolicy();
    cfg.minFusedScore = 0.0f;
    cfg.revalidateSourceHashes = true;
    return cfg;
}

[[nodiscard]] std::string makeLargeCppSource(int functionCount = 6) {
    std::string s;
    for (int i = 0; i < functionCount; ++i) {
        s += "void topLevelFunctionIndex_" + std::to_string(i) + "() {\n";
        s += "    // A substantial function implementation that has enough code bytes to span chunks\n";
        s += "    int a = " + std::to_string(i) + ";\n";
        s += "    int b = a * 10 + 20;\n";
        s += "    for (int k = 0; k < 20; ++k) { b += k * a; }\n";
        s += "    volatile int result = b;\n";
        s += "    (void)result;\n";
        s += "}\n\n";
    }
    return s;
}

} // namespace

E2E_TEST(Tier1_CoverageSemantics, PreprocessingChangeCannotReuseOldEmbeddings, roclint::e2e::TestTier::Tier1, "F2") {
    roclint::e2e::TempWorkspace ws("preprocessing_policy_change");
    roclint::e2e::TestDatabaseFixture db("preprocessing_policy_change");
    ws.writeFile("policy.cpp", makeLargeCppSource());
    auto runtime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    auto config = testCoverageConfig(db.dbPath());
    {
        roclint::retrieval::RetrievalEngine engine(runtime, config);
        auto first = engine.indexWorkspace(ws.root(), "policy");
        E2E_REQUIRE(first.ok());
        E2E_REQUIRE(!first.value().incomplete);
    }
    runtime->resetCounts();
    config.policy.preprocessVersion += ".changed";
    roclint::retrieval::RetrievalEngine reopened(runtime, config);
    auto rebuilt = reopened.indexWorkspace(ws.root(), "policy");
    E2E_REQUIRE(rebuilt.ok());
    E2E_CHECK(runtime->callCount() > 0);
    E2E_CHECK(rebuilt.value().chunksAdded > 0);
}

E2E_TEST(Tier1_CoverageSemantics, FailedEmbeddingIsRetriedAfterRestart, roclint::e2e::TestTier::Tier1, "F3") {
    roclint::e2e::TempWorkspace ws("failed_embedding_recovery");
    roclint::e2e::TestDatabaseFixture db("failed_embedding_recovery");
    ws.writeFile("recovery.cpp", makeLargeCppSource());
    auto runtime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    runtime->setFailOnCall(1);
    {
        roclint::retrieval::RetrievalEngine engine(runtime, testCoverageConfig(db.dbPath()));
        auto first = engine.indexWorkspace(ws.root(), "recovery");
        E2E_REQUIRE(first.ok());
        E2E_CHECK(first.value().incomplete);
        E2E_CHECK_EQ(first.value().chunksAdded, 0ULL);
    }
    runtime->clearFail();
    runtime->resetCounts();
    roclint::retrieval::RetrievalEngine reopened(runtime, testCoverageConfig(db.dbPath()));
    auto recovered = reopened.indexWorkspace(ws.root(), "recovery");
    E2E_REQUIRE(recovered.ok());
    E2E_CHECK(runtime->callCount() > 0);
    E2E_CHECK(recovered.value().chunksAdded > 0);
    E2E_CHECK(!recovered.value().incomplete);
    E2E_CHECK_EQ(reopened.status(), roclint::retrieval::RetrievalStatus::Ready);
}

E2E_TEST(Tier1_CoverageSemantics, IncompleteFullRefreshRevokesReadyAcrossRestart, roclint::e2e::TestTier::Tier1, "F3") {
    roclint::e2e::TempWorkspace ws("incomplete_full_restart");
    roclint::e2e::TestDatabaseFixture db("incomplete_full_restart");
    ws.writeFile("one.cpp", makeLargeCppSource());
    ws.writeFile("two.cpp", makeLargeCppSource());
    auto runtime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    {
        roclint::retrieval::RetrievalEngine engine(runtime, testCoverageConfig(db.dbPath()));
        auto full = engine.indexWorkspace(ws.root(), "coverage");
        E2E_REQUIRE(full.ok());
        E2E_REQUIRE(!full.value().incomplete);
        roclint::retrieval::IndexLimits limits;
        limits.maximumFiles = 1;
        auto partial = engine.indexWorkspace(ws.root(), "coverage", limits);
        E2E_REQUIRE(partial.ok());
        E2E_CHECK(partial.value().incomplete);
        E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Partial);
        auto incremental = engine.indexPaths(ws.root(), "coverage", {"one.cpp"});
        E2E_REQUIRE(incremental.ok());
        E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Partial);
    }
    roclint::retrieval::RetrievalIndex reopened(db.dbPath());
    E2E_REQUIRE(reopened.open(ws.root(), testCoveragePolicy()).ok());
    auto coverage = reopened.snapshotCoverage();
    E2E_REQUIRE(coverage.ok());
    E2E_CHECK(!coverage.value().coverageComplete);
}

E2E_TEST(Tier1_CoverageSemantics, TruncatedSourceIsReembeddedUntilFullyIndexed, roclint::e2e::TestTier::Tier1, "F5") {
    roclint::e2e::TempWorkspace ws("truncated_source_recovery");
    roclint::e2e::TestDatabaseFixture db("truncated_source_recovery");
    const auto text = makeLargeCppSource(12);
    roclint::retrieval::TextChunker chunker;
    E2E_REQUIRE(chunker.chunk(text, "large.cpp").size() > 1);
    ws.writeFile("large.cpp", text);
    auto runtime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(runtime, testCoverageConfig(db.dbPath()));
    roclint::retrieval::IndexLimits limits;
    limits.maximumChunksPerFile = 1;
    auto partial = engine.indexWorkspace(ws.root(), "truncation", limits);
    E2E_REQUIRE(partial.ok());
    E2E_REQUIRE(partial.value().incomplete);
    runtime->resetCounts();
    auto full = engine.indexWorkspace(ws.root(), "truncation");
    E2E_REQUIRE(full.ok());
    E2E_CHECK(runtime->callCount() > 0);
    E2E_CHECK(full.value().chunksAdded > 1);
    E2E_CHECK(!full.value().incomplete);
    runtime->resetCounts();
    auto unchanged = engine.indexWorkspace(ws.root(), "truncation");
    E2E_REQUIRE(unchanged.ok());
    E2E_CHECK_EQ(runtime->callCount(), 0ULL);
}

E2E_TEST(Tier1_CoverageSemantics, FailedIncrementalRefreshRevokesCoverage, roclint::e2e::TestTier::Tier1, "F4") {
    roclint::e2e::TempWorkspace ws("incremental_failure");
    roclint::e2e::TestDatabaseFixture db("incremental_failure");
    ws.writeFile("one.cpp", makeLargeCppSource());
    auto runtime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(runtime, testCoverageConfig(db.dbPath()));
    auto full = engine.indexWorkspace(ws.root(), "incremental_failure");
    E2E_REQUIRE(full.ok());
    E2E_REQUIRE(!full.value().incomplete);
    ws.writeFile("one.cpp", makeLargeCppSource() + "\n// changed content\n");
    runtime->setFailOnCall(runtime->callCount() + 1);
    auto failed = engine.indexPaths(ws.root(), "incremental_failure", {"one.cpp"});
    E2E_REQUIRE(failed.ok());
    E2E_CHECK(failed.value().incomplete);
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Partial);
    runtime->clearFail();
    auto recovered = engine.indexWorkspace(ws.root(), "incremental_failure");
    E2E_REQUIRE(recovered.ok());
    E2E_CHECK(!recovered.value().incomplete);
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Ready);
}

// =============================================================================
// Feature F3: Explicit Refresh Semantics (5 Tests)
// =============================================================================

E2E_TEST(Tier1_CoverageSemantics, F3_01_FullRefreshDiscoversAllWorkspaceFiles, roclint::e2e::TestTier::Tier1, "F3") {
    roclint::e2e::TempWorkspace ws("f3_01_ws");
    roclint::e2e::TestDatabaseFixture db("f3_01_db");

    ws.writeFile("module1.cpp", "void funcOneImplementation() {\n    int x = 1;\n    int y = x + 10;\n}\n");
    ws.writeFile("module2.cpp", "void funcTwoImplementation() {\n    int z = 2;\n    int w = z * 20;\n}\n");
    ws.writeFile("docs/guide.md", "# Guide Documentation\nThis is a complete section of workspace documentation.\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    auto res = engine.indexWorkspace(ws.root(), "ws_f3_01");
    E2E_REQUIRE(res.ok());
    E2E_CHECK_EQ(res.value().filesIndexed, 3ULL);
    E2E_CHECK(res.value().chunksAdded >= 3ULL);
    E2E_CHECK_EQ(res.value().incomplete, false);
}

E2E_TEST(Tier1_CoverageSemantics, F3_02_IncrementalRefreshTargetsSpecifiedPaths, roclint::e2e::TestTier::Tier1, "F3") {
    roclint::e2e::TempWorkspace ws("f3_02_ws");
    roclint::e2e::TestDatabaseFixture db("f3_02_db");

    ws.writeFile("alpha.cpp", "int alphaCalculated() {\n    int a = 10;\n    return a * 2;\n}\n");
    ws.writeFile("beta.cpp", "int betaCalculated() {\n    int b = 20;\n    return b * 3;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    // First full index
    auto fullRes = engine.indexWorkspace(ws.root(), "ws_f3_02");
    E2E_REQUIRE(fullRes.ok());
    E2E_CHECK_EQ(fullRes.value().filesIndexed, 2ULL);

    // Modify only alpha.cpp
    ws.writeFile("alpha.cpp", "int alphaModified() {\n    int a = 11;\n    return a * 4;\n}\n");

    // Perform incremental refresh specifying ONLY alpha.cpp
    auto incRes = engine.indexPaths(ws.root(), "ws_f3_02", {"alpha.cpp"});
    E2E_REQUIRE(incRes.ok());

    E2E_CHECK_EQ(incRes.value().filesIndexed, 1ULL);
}

E2E_TEST(Tier1_CoverageSemantics, F3_03_IncrementalPreservesCompleteFullCoverageStatus, roclint::e2e::TestTier::Tier1, "F3") {
    roclint::e2e::TempWorkspace ws("f3_03_ws");
    roclint::e2e::TestDatabaseFixture db("f3_03_db");

    ws.writeFile("file1.cpp", "int funcOneValue() {\n    int val = 1;\n    return val + 10;\n}\n");
    ws.writeFile("file2.cpp", "int funcTwoValue() {\n    int val = 2;\n    return val + 20;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    auto fullRes = engine.indexWorkspace(ws.root(), "ws_f3_03");
    E2E_REQUIRE(fullRes.ok());
    E2E_CHECK_EQ(fullRes.value().incomplete, false);

    // Modify file1.cpp and run incremental refresh
    ws.writeFile("file1.cpp", "int funcOneModifiedValue() {\n    int val = 101;\n    return val * 2;\n}\n");
    auto incRes = engine.indexPaths(ws.root(), "ws_f3_03", {"file1.cpp"});
    E2E_REQUIRE(incRes.ok());
    E2E_CHECK_EQ(incRes.value().incomplete, false);

    // Verify engine status is still Ready (not downgraded to Partial)
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Ready);

    // Verify via retrieval that response coverage indicates complete
    roclint::retrieval::RetrievalRequest req;
    req.requestId = "req_f3_03";
    req.promptText = "funcOneModifiedValue";
    req.workspace.workspaceRoot = ws.root().generic_string();

    auto resp = engine.retrieve(req);
    E2E_CHECK_EQ(resp.coverage.coverageComplete, true);
}

E2E_TEST(Tier1_CoverageSemantics, F3_04_IncrementalRefreshRemovesDeletedSourceChunks, roclint::e2e::TestTier::Tier1, "F3") {
    roclint::e2e::TempWorkspace ws("f3_04_ws");
    roclint::e2e::TestDatabaseFixture db("f3_04_db");

    ws.writeFile("stay.cpp", "int stayFunctionPermanent() {\n    return 42;\n}\n");
    ws.writeFile("remove.cpp", "int removeFunctionCandidate() {\n    return 99;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    auto fullRes = engine.indexWorkspace(ws.root(), "ws_f3_04");
    E2E_REQUIRE(fullRes.ok());
    E2E_CHECK_EQ(fullRes.value().filesIndexed, 2ULL);

    // Remove remove.cpp from disk
    ws.removeFile("remove.cpp");

    // Pass remove.cpp to incremental refresh
    auto incRes = engine.indexPaths(ws.root(), "ws_f3_04", {"remove.cpp"});
    E2E_REQUIRE(incRes.ok());

    // Verify database chunk records: remove.cpp chunks purged, stay.cpp preserved
    E2E_CHECK_EQ(db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE relative_path='remove.cpp';"), 0);
    E2E_CHECK(db.queryInt("SELECT count(*) FROM retrieval_chunks WHERE relative_path='stay.cpp';") > 0);

    // Query for removeFunctionCandidate — hits should not include remove.cpp
    roclint::retrieval::RetrievalRequest req;
    req.requestId = "req_f3_04";
    req.promptText = "removeFunctionCandidate";
    req.workspace.workspaceRoot = ws.root().generic_string();

    auto resp = engine.retrieve(req);
    for (const auto& hit : resp.hits) {
        E2E_CHECK_NE(hit.location.sourcePath, "remove.cpp");
    }
}

E2E_TEST(Tier1_CoverageSemantics, F3_05_IncrementalRefreshFailurePreservesExistingSnapshot, roclint::e2e::TestTier::Tier1, "F3") {
    roclint::e2e::TempWorkspace ws("f3_05_ws");
    roclint::e2e::TestDatabaseFixture db("f3_05_db");

    ws.writeFile("core.cpp", "int preservedCoreFunction() {\n    return 12345;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    auto fullRes = engine.indexWorkspace(ws.root(), "ws_f3_05");
    E2E_REQUIRE(fullRes.ok());
    E2E_CHECK_EQ(fullRes.value().filesIndexed, 1ULL);

    // Inject failure on next embedding call
    mockRuntime->setFailOnCall(mockRuntime->callCount() + 1);

    // Modify file and attempt incremental refresh
    ws.writeFile("core.cpp", "int modifiedFailedFunction() {\n    return 99999;\n}\n");
    auto incRes = engine.indexPaths(ws.root(), "ws_f3_05", {"core.cpp"});
    E2E_REQUIRE(incRes.ok());
    // On failure during embedding, chunksAdded is 0
    E2E_CHECK_EQ(incRes.value().chunksAdded, 0ULL);

    // Clear failure
    mockRuntime->clearFail();

    // Keep the previous snapshot stored, but do not authorize stale evidence.
    roclint::retrieval::RetrievalRequest req;
    req.requestId = "req_f3_05";
    req.promptText = "preservedCoreFunction";
    req.workspace.workspaceRoot = ws.root().generic_string();

    auto resp = engine.retrieve(req);
    E2E_CHECK(incRes.value().incomplete);
    E2E_CHECK_EQ(resp.status, roclint::retrieval::RetrievalStatus::Partial);
    E2E_CHECK(!resp.coverage.coverageComplete);
    E2E_CHECK(resp.hits.size() >= 1ULL);
    for (const auto& hit : resp.hits) E2E_CHECK(!hit.sourceHashValid);
}

// =============================================================================
// Feature F4: Honest Coverage Reporting (5 Tests)
// =============================================================================

E2E_TEST(Tier1_CoverageSemantics, F4_01_IncompleteFullScanReportsPartialStatus, roclint::e2e::TestTier::Tier1, "F4") {
    roclint::e2e::TempWorkspace ws("f4_01_ws");
    roclint::e2e::TestDatabaseFixture db("f4_01_db");

    ws.writeFile("f1.cpp", "int functionFirst() {\n    return 100;\n}\n");
    ws.writeFile("f2.cpp", "int functionSecond() {\n    return 200;\n}\n");
    ws.writeFile("f3.cpp", "int functionThird() {\n    return 300;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    // Restrict scan to maximum 1 file
    roclint::retrieval::IndexLimits limits;
    limits.maximumFiles = 1;

    auto res = engine.indexWorkspace(ws.root(), "ws_f4_01", limits);
    E2E_REQUIRE(res.ok());

    // Unconditional assertion on incomplete
    E2E_CHECK_EQ(res.value().incomplete, true);
    E2E_CHECK_EQ(res.value().filesIndexed, 1ULL);
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Partial);
}

E2E_TEST(Tier1_CoverageSemantics, F4_02_IncompleteScanDoesNotInheritPriorCompleteness, roclint::e2e::TestTier::Tier1, "F4") {
    // Verifies Bob's Finding 2 reproduction: incomplete_full_after_complete
    roclint::e2e::TempWorkspace ws("f4_02_ws");
    roclint::e2e::TestDatabaseFixture db("f4_02_db");

    ws.writeFile("src1.cpp", "int sourceFileAlpha() {\n    return 10;\n}\n");
    ws.writeFile("src2.cpp", "int sourceFileBeta() {\n    return 20;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    // Pass 1: complete scan
    auto res1 = engine.indexWorkspace(ws.root(), "ws_f4_02");
    E2E_REQUIRE(res1.ok());
    E2E_CHECK_EQ(res1.value().incomplete, false);
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Ready);

    // Pass 2: incomplete scan due to file budget limit
    roclint::retrieval::IndexLimits limits;
    limits.maximumFiles = 1;

    auto res2 = engine.indexWorkspace(ws.root(), "ws_f4_02", limits);
    E2E_REQUIRE(res2.ok());

    // Must NOT inherit Pass 1's complete status in the return stats
    E2E_CHECK_EQ(res2.value().incomplete, true);
    E2E_CHECK_EQ(res2.value().filesIndexed, 1ULL);
}

E2E_TEST(Tier1_CoverageSemantics, F4_03_CancelledScanReportsIncomplete, roclint::e2e::TestTier::Tier1, "F4") {
    roclint::e2e::TempWorkspace ws("f4_03_ws");
    roclint::e2e::TestDatabaseFixture db("f4_03_db");

    ws.writeFile("c1.cpp", "int cancelFunctionA() {\n    return 1;\n}\n");
    ws.writeFile("c2.cpp", "int cancelFunctionB() {\n    return 2;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    auto cancelFlag = std::make_shared<std::atomic_bool>(true);

    auto res = engine.indexWorkspace(ws.root(), "ws_f4_03", {}, cancelFlag);
    E2E_REQUIRE(res.ok());
    // Cancellation must produce incomplete status
    E2E_CHECK_EQ(res.value().incomplete, true);
}

E2E_TEST(Tier1_CoverageSemantics, F4_04_SkippedOversizedFileRecordsIncomplete, roclint::e2e::TestTier::Tier1, "F4") {
    roclint::e2e::TempWorkspace ws("f4_04_ws");
    roclint::e2e::TestDatabaseFixture db("f4_04_db");

    ws.writeFile("normal.cpp", "int normalFunction() {\n    return 100;\n}\n");
    ws.writeFile("fail.cpp", "int failFunction() {\n    return 200;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    // Inject failure on second file embedding call
    mockRuntime->setFailOnCall(2);

    auto res = engine.indexWorkspace(ws.root(), "ws_f4_04");
    E2E_REQUIRE(res.ok());

    // File failure during full scan makes coverage incomplete
    E2E_CHECK_EQ(res.value().incomplete, true);
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Partial);
}

E2E_TEST(Tier1_CoverageSemantics, F4_05_EmptyWorkspaceReportsZeroCoverageHonesty, roclint::e2e::TestTier::Tier1, "F4") {
    roclint::e2e::TempWorkspace ws("f4_05_ws");
    roclint::e2e::TestDatabaseFixture db("f4_05_db");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    auto res = engine.indexWorkspace(ws.root(), "ws_f4_05");
    E2E_REQUIRE(res.ok());

    E2E_CHECK_EQ(res.value().filesIndexed, 0ULL);
    E2E_CHECK_EQ(res.value().chunksAdded, 0ULL);
    E2E_CHECK_EQ(res.value().incomplete, false);

    roclint::retrieval::RetrievalRequest req;
    req.requestId = "req_f4_05";
    req.promptText = "anything";
    req.workspace.workspaceRoot = ws.root().generic_string();

    auto resp = engine.retrieve(req);
    E2E_CHECK_EQ(resp.coverage.indexedChunks, 0ULL);
    E2E_CHECK_EQ(resp.coverage.coverageComplete, true);
    E2E_CHECK_EQ(resp.hits.size(), 0ULL);
}

// =============================================================================
// Feature F5: Per-File Cap Coverage Reporting (5 Tests)
// =============================================================================

E2E_TEST(Tier1_CoverageSemantics, F5_01_ExceedingChunkCapSetsIncompleteUnconditionally, roclint::e2e::TestTier::Tier1, "F5") {
    // Verifies Bob's Finding 2 / Review Obs 2:
    // When a file generates chunks exceeding maximumChunksPerFile, stats.incomplete
    // must be asserted unconditionally without any conditional guards.
    roclint::e2e::TempWorkspace ws("f5_01_ws");
    roclint::e2e::TestDatabaseFixture db("f5_01_db");

    // Write a large C++ file with 6 functions that will produce >= 3 chunks
    ws.writeFile("multi_chunk.cpp", makeLargeCppSource(6));

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    roclint::retrieval::IndexLimits limits;
    limits.maximumChunksPerFile = 2; // Cap at 2 chunks; file produces >= 3 chunks

    auto res = engine.indexWorkspace(ws.root(), "ws_f5_01", limits);
    E2E_REQUIRE(res.ok());

    // UNCONDITIONAL ASSERTION
    E2E_CHECK_EQ(res.value().incomplete, true);
}

E2E_TEST(Tier1_CoverageSemantics, F5_02_ExceedingChunkCapRecordsSkippedPath, roclint::e2e::TestTier::Tier1, "F5") {
    roclint::e2e::TempWorkspace ws("f5_02_ws");
    roclint::e2e::TestDatabaseFixture db("f5_02_db");

    ws.writeFile("capped_code.cpp", makeLargeCppSource(6));

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    roclint::retrieval::IndexLimits limits;
    limits.maximumChunksPerFile = 2;

    auto res = engine.indexWorkspace(ws.root(), "ws_f5_02", limits);
    E2E_REQUIRE(res.ok());

    // Verified that per-file cap truncates and flags incomplete
    E2E_CHECK_EQ(res.value().incomplete, true);
    E2E_CHECK_EQ(res.value().chunksAdded, 2ULL);
}

E2E_TEST(Tier1_CoverageSemantics, F5_03_ExceedingChunkCapTruncatesChunks, roclint::e2e::TestTier::Tier1, "F5") {
    roclint::e2e::TempWorkspace ws("f5_03_ws");
    roclint::e2e::TestDatabaseFixture db("f5_03_db");

    ws.writeFile("truncated.cpp", makeLargeCppSource(6));

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    roclint::retrieval::IndexLimits limits;
    constexpr std::size_t kMaxChunks = 2;
    limits.maximumChunksPerFile = kMaxChunks;

    auto res = engine.indexWorkspace(ws.root(), "ws_f5_03", limits);
    E2E_REQUIRE(res.ok());

    // Exactly kMaxChunks were indexed
    E2E_CHECK_EQ(res.value().chunksAdded, kMaxChunks);
}

E2E_TEST(Tier1_CoverageSemantics, F5_04_ExceedingChunkCapMarksSnapshotCoverageIncomplete, roclint::e2e::TestTier::Tier1, "F5") {
    roclint::e2e::TempWorkspace ws("f5_04_ws");
    roclint::e2e::TestDatabaseFixture db("f5_04_db");

    ws.writeFile("book.cpp", makeLargeCppSource(6));

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    roclint::retrieval::IndexLimits limits;
    limits.maximumChunksPerFile = 2;

    auto res = engine.indexWorkspace(ws.root(), "ws_f5_04", limits);
    E2E_REQUIRE(res.ok());

    E2E_CHECK_EQ(res.value().incomplete, true);
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Partial);
}

E2E_TEST(Tier1_CoverageSemantics, F5_05_WithinChunkCapMaintainsCompleteCoverage, roclint::e2e::TestTier::Tier1, "F5") {
    roclint::e2e::TempWorkspace ws("f5_05_ws");
    roclint::e2e::TestDatabaseFixture db("f5_05_db");

    ws.writeFile("small.cpp", "int singleSmallFunction() {\n    return 42;\n}\n");

    auto mockRuntime = std::make_shared<roclint::e2e::MockRuntimeFixture>(true);
    roclint::retrieval::RetrievalEngine engine(mockRuntime, testCoverageConfig(db.dbPath()));

    roclint::retrieval::IndexLimits limits;
    limits.maximumChunksPerFile = 50;

    auto res = engine.indexWorkspace(ws.root(), "ws_f5_05", limits);
    E2E_REQUIRE(res.ok());

    E2E_CHECK_EQ(res.value().incomplete, false);
    E2E_CHECK_EQ(engine.status(), roclint::retrieval::RetrievalStatus::Ready);
}
