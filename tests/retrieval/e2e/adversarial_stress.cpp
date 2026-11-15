// =============================================================================
// Roclint Retrieval Subsystem — Adversarial Stress Test Suite
// Empirical Challenger Verification Harness for Milestone E1
// =============================================================================

#include "E2EFixtures.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace roclint::e2e;

#define STRESS_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [STRESS_FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            return false; \
        } \
    } while(0)

// -----------------------------------------------------------------------------
// 1. TempWorkspace Stress Tests
// -----------------------------------------------------------------------------

bool stressTempWorkspace_RapidCreation() {
    std::cout << "  [RUN] TempWorkspace: Rapid creation and destruction (100 iterations)..." << std::endl;
    std::vector<std::filesystem::path> paths;
    paths.reserve(100);

    for (int i = 0; i < 100; ++i) {
        TempWorkspace ws("rapid_ws");
        STRESS_ASSERT(std::filesystem::exists(ws.root()), "Workspace directory should exist");
        paths.push_back(ws.root());
    }

    // Verify all 100 paths were unique
    for (size_t i = 0; i < paths.size(); ++i) {
        for (size_t j = i + 1; j < paths.size(); ++j) {
            STRESS_ASSERT(paths[i] != paths[j], "Duplicate workspace path detected");
        }
        STRESS_ASSERT(!std::filesystem::exists(paths[i]), "Workspace directory should be cleaned after destruction");
    }

    std::cout << "  [PASS] TempWorkspace: Rapid creation and destruction passed." << std::endl;
    return true;
}

bool stressTempWorkspace_ConcurrentThreads() {
    std::cout << "  [RUN] TempWorkspace: Concurrent multi-threaded creation (8 threads x 25 ws)..." << std::endl;
    constexpr int kNumThreads = 8;
    constexpr int kWsPerThread = 25;
    std::vector<std::future<std::vector<std::filesystem::path>>> futures;

    for (int t = 0; t < kNumThreads; ++t) {
        futures.push_back(std::async(std::launch::async, [t]() {
            std::vector<std::filesystem::path> localPaths;
            localPaths.reserve(kWsPerThread);
            for (int i = 0; i < kWsPerThread; ++i) {
                TempWorkspace ws("thread_" + std::to_string(t));
                auto file = ws.writeFile("nested/file.txt", "payload");
                if (std::filesystem::exists(file)) {
                    localPaths.push_back(ws.root());
                }
            }
            return localPaths;
        }));
    }

    std::vector<std::filesystem::path> allPaths;
    for (auto& f : futures) {
        auto paths = f.get();
        allPaths.insert(allPaths.end(), paths.begin(), paths.end());
    }

    STRESS_ASSERT(allPaths.size() == kNumThreads * kWsPerThread, "All workspaces should succeed");
    for (size_t i = 0; i < allPaths.size(); ++i) {
        for (size_t j = i + 1; j < allPaths.size(); ++j) {
            STRESS_ASSERT(allPaths[i] != allPaths[j], "Concurrent duplicate path detected!");
        }
        STRESS_ASSERT(!std::filesystem::exists(allPaths[i]), "Concurrent workspace should be cleaned up");
    }

    std::cout << "  [PASS] TempWorkspace: Concurrent multi-threaded creation passed." << std::endl;
    return true;
}

bool stressTempWorkspace_DeepNesting() {
    std::cout << "  [RUN] TempWorkspace: Deep nested directory creation within standard path budget (12 levels)..." << std::endl;
    {
        TempWorkspace ws("deep_ws");
        std::string relPath;
        for (int i = 0; i < 12; ++i) {
            relPath += "lvl_" + std::to_string(i) + "/";
        }
        relPath += "deep_file.txt";

        std::string testContent = "Deep nested content payload 1234567890\n";
        auto fullPath = ws.writeFile(relPath, testContent);
        STRESS_ASSERT(std::filesystem::exists(fullPath), "Deep file should exist");

        std::ifstream ifs(fullPath);
        std::string readContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();
        STRESS_ASSERT(readContent == testContent, "Deep file content must match");

        auto root = ws.root();
        ws.clean();
        STRESS_ASSERT(!std::filesystem::exists(root), "Deep nested tree must be completely deleted");
    }

#if defined(_WIN32)
    std::cout << "  [RUN] TempWorkspace: Exceeding Windows MAX_PATH boundary condition (35 levels > 260 chars)..." << std::endl;
    {
        TempWorkspace ws("overflow_ws");
        std::string hugeRelPath;
        for (int i = 0; i < 35; ++i) {
            hugeRelPath += "level_segment_" + std::to_string(i) + "/";
        }
        hugeRelPath += "overflow_file.txt";

        bool exceptionThrown = false;
        try {
            ws.writeFile(hugeRelPath, "data");
        } catch (const std::exception& e) {
            exceptionThrown = true;
            std::cout << "    Observed expected exception on path exceeding MAX_PATH: " << e.what() << std::endl;
        }
        STRESS_ASSERT(exceptionThrown, "writeFile must throw when exceeding Windows MAX_PATH without long path prefix");

        auto root = ws.root();
        ws.clean();
        STRESS_ASSERT(!std::filesystem::exists(root), "Workspace root must still clean up partial directories");
    }

#endif
    std::cout << "  [PASS] TempWorkspace: Deep nesting tests passed." << std::endl;
    return true;
}

bool stressTempWorkspace_LockedFileRetry() {
#if defined(_WIN32)
    std::cout << "  [RUN] TempWorkspace: Windows file lock retry behavior..." << std::endl;
    auto rootPath = std::filesystem::path();
    {
        TempWorkspace ws("lock_ws");
        rootPath = ws.root();
        auto filePath = ws.writeFile("locked.txt", "locked data");
        STRESS_ASSERT(std::filesystem::exists(filePath), "File should exist");

        // Open an input file stream that keeps a shared-read lock on Windows
        FILE* fp = nullptr;
        errno_t err = fopen_s(&fp, filePath.string().c_str(), "rb");
        STRESS_ASSERT(err == 0 && fp != nullptr, "File should be open");

        // Attempt removeFile while open handle exists
        auto start = std::chrono::steady_clock::now();
        ws.removeFile("locked.txt");
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        // 5 retry attempts: 20 + 40 + 60 + 80 + 100 = 300 ms
        STRESS_ASSERT(duration >= 200, "Retry backoff loop should have spent at least 200ms");
        STRESS_ASSERT(std::filesystem::exists(filePath), "File should still exist while locked");

        // Close the file handle
        fclose(fp);

        // Second attempt with lock released should succeed immediately
        ws.removeFile("locked.txt");
        STRESS_ASSERT(!std::filesystem::exists(filePath), "File should be deleted once unlocked");
    }
    STRESS_ASSERT(!std::filesystem::exists(rootPath), "Workspace should be cleaned up on destruction");

    std::cout << "  [PASS] TempWorkspace: Windows file lock retry behavior passed." << std::endl;
#else
    TempWorkspace ws("open_file");
    const auto file = ws.writeFile("open.txt", "open data");
    std::ifstream input(file);
    STRESS_ASSERT(input.is_open(), "File should be open");
    ws.removeFile("open.txt");
    STRESS_ASSERT(!std::filesystem::exists(file), "POSIX unlink should remove the path while the file is open");
    std::string text;
    std::getline(input, text);
    STRESS_ASSERT(text == "open data", "The open stream should still read the unlinked file");
#endif
    return true;
}

bool stressTempWorkspace_MoveSemantics() {
    std::cout << "  [RUN] TempWorkspace: Move semantics and RAII disarming..." << std::endl;
    std::filesystem::path movedRoot;
    {
        TempWorkspace ws1("move_source");
        auto file = ws1.writeFile("test.txt", "abc");
        movedRoot = ws1.root();
        STRESS_ASSERT(std::filesystem::exists(file), "File exists");

        // Move construct
        TempWorkspace ws2(std::move(ws1));
        STRESS_ASSERT(std::filesystem::exists(file), "File still exists in ws2");

        // Move assign
        TempWorkspace ws3("move_target");
        ws3 = std::move(ws2);
        STRESS_ASSERT(std::filesystem::exists(file), "File still exists in ws3");
    }
    STRESS_ASSERT(!std::filesystem::exists(movedRoot), "Moved root cleaned by final owner");

    std::cout << "  [PASS] TempWorkspace: Move semantics passed." << std::endl;
    return true;
}

// -----------------------------------------------------------------------------
// 2. TestDatabaseFixture Stress Tests & Schema Fidelity
// -----------------------------------------------------------------------------

bool stressTestDatabaseFixture_SchemaFidelityVsLegacyDb() {
    std::cout << "  [RUN] TestDatabaseFixture: Schema fidelity vs Bob's legacy.db..." << std::endl;
    TestDatabaseFixture fixture("schema_fidelity");
    TempWorkspace workspace("legacy_seed");
    const auto wsRoot = workspace.root();

    fixture.seedLegacyV1Database(wsRoot, true, false);

    // 1. Verify schema_version is '1'
    std::string ver = fixture.queryString("SELECT schema_version FROM retrieval_meta;");
    STRESS_ASSERT(ver == "1", "Seeded v1 database must have schema_version = '1'");

    // 2. Verify all required tables exist
    int metaCount = fixture.queryInt("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='retrieval_meta';");
    int chunksCount = fixture.queryInt("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='retrieval_chunks';");
    int sourcesCount = fixture.queryInt("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='retrieval_sources';");
    int embCount = fixture.queryInt("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='retrieval_embeddings';");
    int ftsCount = fixture.queryInt("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='retrieval_fts';");

    STRESS_ASSERT(metaCount == 1, "retrieval_meta table must exist");
    STRESS_ASSERT(chunksCount == 1, "retrieval_chunks table must exist");
    STRESS_ASSERT(sourcesCount == 1, "retrieval_sources table must exist");
    STRESS_ASSERT(embCount == 1, "retrieval_embeddings table must exist");
    STRESS_ASSERT(ftsCount == 1, "retrieval_fts virtual table must exist");

    // 3. Verify retrieval_fts lacks source_kind, snapshot_gen, published_gen
    // Attempting to insert source_kind into retrieval_fts must fail with the exact error Bob observed!
    std::string sqlError;
    bool ftsInsert = fixture.executeSql(
        "INSERT INTO retrieval_fts (chunk_id, workspace_root, source_kind, snapshot_gen, published_gen, chunk_text) "
        "VALUES ('probe', 'ws', 0, 1, 0, 'text');",
        &sqlError
    );
    STRESS_ASSERT(!ftsInsert, "Insertion of source_kind into legacy FTS must fail");
    STRESS_ASSERT(sqlError.find("has no column named source_kind") != std::string::npos,
                  "Error must state 'has no column named source_kind' matching Bob's finding");

    std::cout << "  [PASS] TestDatabaseFixture: Schema fidelity vs Bob's legacy.db passed." << std::endl;
    return true;
}

bool stressTestDatabaseFixture_CorruptedExcerptInjectionVsBob() {
    std::cout << "  [RUN] TestDatabaseFixture: Corrupted excerpt injection vs Bob's corrupt-cache.db..." << std::endl;
    TestDatabaseFixture fixture("corrupt_fidelity");
    TempWorkspace workspace("legacy_seed");
    const auto wsRoot = workspace.root();

    fixture.seedLegacyV1Database(wsRoot, true, true);

    // Verify retrieval_chunks contains chunk_text = '0'
    int corruptChunkCount = fixture.queryInt("SELECT COUNT(*) FROM retrieval_chunks WHERE chunk_text = '0';");
    STRESS_ASSERT(corruptChunkCount == 1, "Must contain exactly 1 corrupted chunk in retrieval_chunks");

    // Verify retrieval_fts contains chunk_text = '0'
    int corruptFtsCount = fixture.queryInt("SELECT COUNT(*) FROM retrieval_fts WHERE chunk_text = '0';");
    STRESS_ASSERT(corruptFtsCount == 1, "Must contain exactly 1 corrupted chunk in retrieval_fts");

    std::cout << "  [PASS] TestDatabaseFixture: Corrupted excerpt injection passed." << std::endl;
    return true;
}

bool stressTestDatabaseFixture_RawUtf8Seeding() {
    std::cout << "  [RUN] TestDatabaseFixture: Raw UTF-8 hex chunk seeding..." << std::endl;
    TestDatabaseFixture fixture("utf8_raw");
    TempWorkspace workspace("utf8_seed");
    const auto wsRoot = workspace.root();
    fixture.seedLegacyV1Database(wsRoot, true, true);

    // Seed e-acute (0xC3 0xA9)
    fixture.seedRawUtf8Chunk("corrupt_chunk_001", "C3A9");
    std::string text = fixture.queryString("SELECT chunk_text FROM retrieval_chunks WHERE chunk_id = 'corrupt_chunk_001';");
    STRESS_ASSERT(text == "\xC3\xA9", "Raw UTF-8 2-byte e-acute must roundtrip");

    // Seed 4-byte Rocket emoji (0xF0 0x9F 0x9A 0x80)
    fixture.seedRawUtf8Chunk("corrupt_chunk_001", "F09F9A80");
    std::string emoji = fixture.queryString("SELECT chunk_text FROM retrieval_chunks WHERE chunk_id = 'corrupt_chunk_001';");
    STRESS_ASSERT(emoji == "\xF0\x9F\x9A\x80", "Raw UTF-8 4-byte rocket emoji must roundtrip");

    std::cout << "  [PASS] TestDatabaseFixture: Raw UTF-8 hex chunk seeding passed." << std::endl;
    return true;
}

bool stressTestDatabaseFixture_ErrorHandling() {
    std::cout << "  [RUN] TestDatabaseFixture: Syntax error handling and safety..." << std::endl;
    TestDatabaseFixture fixture("err_db");

    std::string sqlErr;
    bool badSql = fixture.executeSql("SYNTAX INVALID STATEMENT ???", &sqlErr);
    STRESS_ASSERT(!badSql, "Invalid SQL should return false");
    STRESS_ASSERT(!sqlErr.empty(), "Error message should be populated");

    int val = fixture.queryInt("SELECT count(*) FROM nonexistent_table;", -999);
    STRESS_ASSERT(val == -999, "queryInt on missing table should return default value");

    std::string strVal = fixture.queryString("SELECT col FROM nonexistent_table;", "FALLBACK");
    STRESS_ASSERT(strVal == "FALLBACK", "queryString on missing table should return default value");

    std::cout << "  [PASS] TestDatabaseFixture: Syntax error handling passed." << std::endl;
    return true;
}

// -----------------------------------------------------------------------------
// 3. MockRuntimeFixture Stress Tests
// -----------------------------------------------------------------------------

bool stressMockRuntimeFixture_ConcurrentCallCounting() {
    std::cout << "  [RUN] MockRuntimeFixture: Multi-threaded call counting (8 threads x 50 calls)..." << std::endl;
    MockRuntimeFixture runtime(true);
    constexpr int kThreads = 8;
    constexpr int kCallsPerThread = 50;
    std::atomic<int> failedEmbeddings{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&runtime, &failedEmbeddings]() {
            for (int i = 0; i < kCallsPerThread; ++i) {
                std::vector<std::string> texts = {"item1", "item2", "item3"};
                auto res = runtime.embed(texts);
                if (!res.ok()) ++failedEmbeddings;
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    STRESS_ASSERT(failedEmbeddings.load() == 0, "Every concurrent embedding must succeed in Release too");
    STRESS_ASSERT(runtime.callCount() == kThreads * kCallsPerThread, "Total callCount must match exactly");
    STRESS_ASSERT(runtime.textsEmbeddedCount() == kThreads * kCallsPerThread * 3, "Total textsEmbeddedCount must match exactly");

    std::cout << "  [PASS] MockRuntimeFixture: Multi-threaded call counting passed." << std::endl;
    return true;
}

bool stressMockRuntimeFixture_FaultSimulation() {
    std::cout << "  [RUN] MockRuntimeFixture: Fault injection and recovery (setFailOnCall vs setFailNext)..." << std::endl;
    MockRuntimeFixture runtime(true);

    // Call 1: success
    auto r1 = runtime.embed({"first"});
    STRESS_ASSERT(r1.ok(), "Call 1 should succeed");
    STRESS_ASSERT(runtime.callCount() == 1, "Call count should be 1");

    // Simulate "setFailNext(true)": using setFailOnCall(callCount + 1)
    runtime.setFailOnCall(runtime.callCount() + 1, roclint::core::ErrorCode::ProviderUnavailable);
    auto r2 = runtime.embed({"second"});
    STRESS_ASSERT(!r2.ok(), "Call 2 should fail with injected fault");
    STRESS_ASSERT(r2.error().code == roclint::core::ErrorCode::ProviderUnavailable, "Error code must match");
    STRESS_ASSERT(runtime.callCount() == 2, "Call count should be 2");

    // Call 3: should succeed automatically because failOnCall was specifically call 2
    auto r3 = runtime.embed({"third"});
    STRESS_ASSERT(r3.ok(), "Call 3 should succeed without manual clear");
    STRESS_ASSERT(runtime.callCount() == 3, "Call count should be 3");

    // Test setReady(false)
    runtime.setReady(false);
    auto r4 = runtime.embed({"fourth"});
    STRESS_ASSERT(!r4.ok(), "Unavailable mock runtime should fail embed");
    STRESS_ASSERT(r4.error().code == roclint::core::ErrorCode::ProviderUnavailable, "Error code should be ProviderUnavailable");

    runtime.setReady(true);
    auto r5 = runtime.embed({"fifth"});
    STRESS_ASSERT(r5.ok(), "Ready mock runtime should succeed");

    std::cout << "  [PASS] MockRuntimeFixture: Fault injection and recovery passed." << std::endl;
    return true;
}

bool stressMockRuntimeFixture_DimensionAndL2Normalization() {
    std::cout << "  [RUN] MockRuntimeFixture: Vector dimension and L2-normalization constraints..." << std::endl;
    MockRuntimeFixture runtime(true);

    const std::vector<std::string> testCases = {
        "",                                          // empty string
        " ",                                         // whitespace
        "a",                                         // single char
        "Hello World",                               // ascii
        "void needle() { return 42; }",              // code snippet
        "\xC3\xA9\xC3\xA9\xC3\xA9",                 // UTF-8 accented
        "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF", // Japanese Hiragana
        "\xF0\x9F\x9A\x80\xF0\x9F\x8C\x9F",         // Emojis
        std::string(10000, 'x')                     // 10KB string
    };

    auto batchResult = runtime.embed(testCases);
    STRESS_ASSERT(batchResult.ok(), "Batch embedding should succeed");

    const auto& batch = batchResult.value();
    STRESS_ASSERT(batch.dimensions == 768, "Batch dimensions must be 768");
    STRESS_ASSERT(batch.rows == testCases.size(), "Batch rows must match input size");
    STRESS_ASSERT(batch.data.size() == testCases.size() * 768, "Data vector size must be rows * 768");

    for (size_t row = 0; row < testCases.size(); ++row) {
        const float* vec = batch.data.data() + (row * 768);
        double sumSq = 0.0;
        for (int d = 0; d < 768; ++d) {
            float v = vec[d];
            STRESS_ASSERT(!std::isnan(v) && !std::isinf(v), "Embedding vector must contain finite values");
            STRESS_ASSERT(v >= -1.0f && v <= 1.0f, "Vector components must be in [-1, 1]");
            sumSq += static_cast<double>(v) * static_cast<double>(v);
        }
        double l2norm = std::sqrt(sumSq);
        STRESS_ASSERT(std::abs(l2norm - 1.0) < 1e-5, "Vector must be L2-normalized to 1.0 (got " + std::to_string(l2norm) + ")");
    }

    std::cout << "  [PASS] MockRuntimeFixture: Dimension and L2-normalization passed." << std::endl;
    return true;
}

// -----------------------------------------------------------------------------
// 4. MockWorkerScriptFixture Tests
// -----------------------------------------------------------------------------

bool stressMockWorkerScriptFixture() {
    std::cout << "  [RUN] MockWorkerScriptFixture: Testing ephemeral script lifecycle..." << std::endl;

    const std::vector<MockWorkerBehavior> behaviors = {
        MockWorkerBehavior::NormalEcho,
        MockWorkerBehavior::WrongModelId,
        MockWorkerBehavior::WrongRevision,
        MockWorkerBehavior::WrongDimensions,
        MockWorkerBehavior::MissingPooling,
        MockWorkerBehavior::ContradictoryPooling,
        MockWorkerBehavior::HangingProcess,
        MockWorkerBehavior::CrashOnPing,
        MockWorkerBehavior::MalformedJson
    };

    for (auto b : behaviors) {
        std::filesystem::path scriptP;
        {
            MockWorkerScriptFixture fixture(b);
            scriptP = fixture.scriptPath();
            STRESS_ASSERT(std::filesystem::exists(scriptP), "Script file must exist during fixture lifetime");
            STRESS_ASSERT(std::filesystem::file_size(scriptP) > 0, "Script file must not be empty");
        }
        STRESS_ASSERT(!std::filesystem::exists(scriptP), "Script file must be cleaned on fixture destruction");
    }

    std::cout << "  [PASS] MockWorkerScriptFixture: Lifecycle tests passed." << std::endl;
    return true;
}

// -----------------------------------------------------------------------------
// 5. RealModelValidationFixture Tests
// -----------------------------------------------------------------------------

bool stressRealModelValidationFixture() {
    std::cout << "  [RUN] RealModelValidationFixture: Model asset probing and synthetic directory..." << std::endl;

    // 1. Probing with non-existent directory
    TempWorkspace missingAssets("missing_assets_probe");
    auto resNonExistent = RealModelValidationFixture::probeModelAssets((missingAssets.root() / "missing").string());
    STRESS_ASSERT(!resNonExistent.available, "Non-existent path should report available = false");
    STRESS_ASSERT(!resNonExistent.skipReason.empty(), "Skip reason should explain directory missing");

    // 2. Synthesize complete mock Granite model directory
    TempWorkspace modelWs("mock_granite_model");
    modelWs.writeFile("model.safetensors", "dummy_weights");
    modelWs.writeFile("config.json", "{}");
    modelWs.writeFile("1_Pooling/config.json", "{\"cls_token\": true, \"mean_tokens\": false}");
    modelWs.writeFile("tokenizer.json", "{}");

    auto resSynthetic = RealModelValidationFixture::probeModelAssets(modelWs.root().string());
    STRESS_ASSERT(resSynthetic.available, "Complete model directory should report available = true");
    STRESS_ASSERT(resSynthetic.skipReason.empty(), "Skip reason should be empty when available");

    // 3. Test requireOrSkip on complete directory
    auto status = RealModelValidationFixture::requireOrSkip(modelWs.root().string());
    STRESS_ASSERT(status.available, "requireOrSkip on complete model directory should return status");

    // 4. Incomplete model directory (remove tokenizer.json)
    modelWs.removeFile("tokenizer.json");
    auto resIncomplete = RealModelValidationFixture::probeModelAssets(modelWs.root().string());
    STRESS_ASSERT(!resIncomplete.available, "Incomplete model directory should report available = false");
    STRESS_ASSERT(resIncomplete.skipReason.find("tokenizer.json") != std::string::npos,
                  "Skip reason should identify missing tokenizer.json");

    bool caughtNotRun = false;
    try {
        auto ignored = RealModelValidationFixture::requireOrSkip(modelWs.root().string());
        (void)ignored;
    } catch (const NotRunException& e) {
        caughtNotRun = true;
        STRESS_ASSERT(std::string(e.what()).find("tokenizer.json") != std::string::npos,
                      "NotRunException must contain diagnostic message");
    }
    STRESS_ASSERT(caughtNotRun, "requireOrSkip must throw NotRunException when asset is missing");

    std::cout << "  [PASS] RealModelValidationFixture: Probing and synthetic tests passed." << std::endl;
    return true;
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------

int main() {
    std::cout << "======================================================================\n";
    std::cout << "Adversarial Stress Test Suite — Milestone E1\n";
    std::cout << "======================================================================\n";

    bool allPassed = true;

    try {
        allPassed &= stressTempWorkspace_RapidCreation();
        allPassed &= stressTempWorkspace_ConcurrentThreads();
        allPassed &= stressTempWorkspace_DeepNesting();
        allPassed &= stressTempWorkspace_LockedFileRetry();
        allPassed &= stressTempWorkspace_MoveSemantics();

        allPassed &= stressTestDatabaseFixture_SchemaFidelityVsLegacyDb();
        allPassed &= stressTestDatabaseFixture_CorruptedExcerptInjectionVsBob();
        allPassed &= stressTestDatabaseFixture_RawUtf8Seeding();
        allPassed &= stressTestDatabaseFixture_ErrorHandling();

        allPassed &= stressMockRuntimeFixture_ConcurrentCallCounting();
        allPassed &= stressMockRuntimeFixture_FaultSimulation();
        allPassed &= stressMockRuntimeFixture_DimensionAndL2Normalization();

        allPassed &= stressMockWorkerScriptFixture();
        allPassed &= stressRealModelValidationFixture();
    } catch (const std::exception& e) {
        std::cerr << "UNHANDLED EXCEPTION IN HARNESS: " << e.what() << std::endl;
        allPassed = false;
    }

    std::cout << "======================================================================\n";
    if (allPassed) {
        std::cout << "ALL ADVERSARIAL STRESS TESTS PASSED EMPIRICALLY!\n";
        return 0;
    } else {
        std::cerr << "ONE OR MORE ADVERSARIAL STRESS TESTS FAILED!\n";
        return 1;
    }
}
