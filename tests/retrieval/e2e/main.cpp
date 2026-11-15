// =============================================================================
// Roclint Retrieval Subsystem — Unified E2E Test Runner Main
// =============================================================================

#include "E2ETestHarness.hpp"
#include "E2EFixtures.hpp"

// -----------------------------------------------------------------------------
// Baseline Harness Sanity Tests
// Verifies that TempWorkspace, TestDatabaseFixture, MockRuntimeFixture, and
// RealModelValidationFixture operate cleanly and honestly.
// -----------------------------------------------------------------------------

E2E_TEST(HarnessSanity, TempWorkspaceBasics, 1, "F0") {
    roclint::e2e::TempWorkspace ws("sanity_ws");
    E2E_CHECK(std::filesystem::exists(ws.root()));

    auto filePath = ws.writeFile("sub/test.txt", "Hello E2E Sanity\n");
    E2E_CHECK(std::filesystem::exists(filePath));

    {
        std::ifstream ifs(filePath);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        E2E_CHECK_EQ(content, "Hello E2E Sanity\n");
    }

    ws.removeFile("sub/test.txt");
    E2E_CHECK(!std::filesystem::exists(filePath));
}

E2E_TEST(HarnessSanity, TestDatabaseFixtureBasics, 1, "F6") {
    roclint::e2e::TestDatabaseFixture db("sanity_db");
    E2E_CHECK(std::filesystem::exists(db.dbPath().parent_path()));

    std::string err;
    bool createOk = db.executeSql(
        "CREATE TABLE sanity (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO sanity (id, val) VALUES (42, 'meaning_of_life');",
        &err
    );
    E2E_CHECK(createOk);
    E2E_CHECK_EQ(err, "");

    int idVal = db.queryInt("SELECT id FROM sanity WHERE val = 'meaning_of_life';");
    E2E_CHECK_EQ(idVal, 42);

    std::string strVal = db.queryString("SELECT val FROM sanity WHERE id = 42;");
    E2E_CHECK_EQ(strVal, "meaning_of_life");

    // Test legacy v1 database seeding
    auto wsRoot = std::filesystem::temp_directory_path() / "sanity_legacy_ws";
    db.seedLegacyV1Database(wsRoot, true, true);

    std::string schemaVer = db.queryString("SELECT schema_version FROM retrieval_meta;");
    E2E_CHECK_EQ(schemaVer, "1");

    int corruptCount = db.queryInt("SELECT COUNT(*) FROM retrieval_chunks WHERE chunk_text = '0';");
    E2E_CHECK_EQ(corruptCount, 1);

    int ftsCorruptCount = db.queryInt("SELECT COUNT(*) FROM retrieval_fts WHERE chunk_text = '0';");
    E2E_CHECK_EQ(ftsCorruptCount, 1);
}

E2E_TEST(HarnessSanity, MockRuntimeFixtureBasics, 1, "F1") {
    roclint::e2e::MockRuntimeFixture runtime(true);
    E2E_CHECK_EQ(runtime.callCount(), 0ULL);
    E2E_CHECK_EQ(runtime.textsEmbeddedCount(), 0ULL);

    std::vector<std::string> texts = {"First chunk text", "Second chunk text"};
    auto result = runtime.embed(texts);
    E2E_REQUIRE(result.ok());
    E2E_CHECK_EQ(result.value().rows, 2ULL);
    E2E_CHECK_EQ(result.value().dimensions, 768);
    E2E_CHECK_EQ(runtime.callCount(), 1ULL);
    E2E_CHECK_EQ(runtime.textsEmbeddedCount(), 2ULL);

    // Test fault injection
    runtime.setFailOnCall(2, roclint::core::ErrorCode::ProviderFailed);
    auto failResult = runtime.embed({"Third text"});
    E2E_CHECK(!failResult.ok());
    E2E_CHECK_EQ(failResult.error().code, roclint::core::ErrorCode::ProviderFailed);
    E2E_CHECK_EQ(runtime.callCount(), 2ULL);

    // Clear fault
    runtime.clearFail();
    auto okResult = runtime.embed({"Fourth text"});
    E2E_CHECK(okResult.ok());
    E2E_CHECK_EQ(runtime.callCount(), 3ULL);
}

E2E_TEST(HarnessSanity, RealModelAssetHonestNotRun, 4, "F18") {
    auto status = roclint::e2e::RealModelValidationFixture::probeModelAssets();
    if (!status.available) {
        E2E_NOT_RUN(status.skipReason);
    }

    // If model assets are available on the machine, verify that required files exist
    E2E_CHECK(std::filesystem::exists(status.modelDir / "model.safetensors"));
    E2E_CHECK(std::filesystem::exists(status.modelDir / "config.json"));
}

// -----------------------------------------------------------------------------
// Runner Entry Point
// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    return roclint::e2e::TestRunner::instance().run(argc, argv);
}
