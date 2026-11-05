#include "roclint/rewrite/AstGroundingEngine.hpp"
#include "roclint/storage/AstSymbolExtractor.hpp"
#include "roclint/storage/CodebaseIndex.hpp"
#include "support/check.hpp"

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
            ("roclint-ast-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
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

core::rewrite::CanonicalIntent intent(std::string target, std::string condition = {},
                                      core::rewrite::IntentAction action = core::rewrite::IntentAction::MODIFY,
                                      std::string property = "name") {
    core::rewrite::CanonicalIntent value;
    value.action = action;
    value.target = std::move(target);
    value.condition = std::move(condition);
    value.property = std::move(property);
    value.value = "changed";
    value.nodeId = "nid_test";
    return value;
}

core::rewrite::CanonicalIntent ground(const storage::CodebaseIndex& index, std::string target,
                                      std::string condition = {}, std::string active = {},
                                      std::string property = "name",
                                      core::rewrite::IntentAction action = core::rewrite::IntentAction::MODIFY) {
    return core::rewrite::AstGroundingEngine::groundIntents(
        {intent(std::move(target), std::move(condition), action, std::move(property))}, active, index).front();
}

core::rewrite::GroundingStatus statusOf(const core::rewrite::CanonicalIntent& value) {
    ROCLINT_CHECK(value.grounding.has_value());
    return value.grounding->status;
}

const core::rewrite::CanonicalIntent* findIntent(const std::vector<core::rewrite::CanonicalIntent>& intents,
                                                 std::string_view needle) {
    for (const auto& value : intents) {
        if (value.target.find(needle) != std::string::npos) return &value;
    }
    return nullptr;
}

std::size_t exactSymbolCount(const storage::CodebaseIndex& index, std::string_view name) {
    const auto found = index.findByName(name, {}, {}, 64, false);
    ROCLINT_CHECK(found.ok());
    return found.value().size();
}

void createLegacyDatabase(const fs::path& path) {
    sqlite3* db = nullptr;
    ROCLINT_CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    ROCLINT_CHECK(sqlite3_exec(db,
                               "CREATE TABLE workspaces(root TEXT PRIMARY KEY);"
                               "CREATE TABLE files(id INTEGER PRIMARY KEY);"
                               "CREATE TABLE ast_symbols(symbol_id TEXT PRIMARY KEY);",
                               nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

int userVersion(const fs::path& path) {
    sqlite3* db = nullptr;
    ROCLINT_CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ROCLINT_CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &statement, nullptr) == SQLITE_OK);
    ROCLINT_CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto version = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return version;
}

void testExactScopeAndHallucination() {
    TempArea area;
    const auto workspace = area.workspace("scope");
    write(workspace, "src/SessionManager.h",
          "class SessionManager { public: bool readPacket(size_t offset, uint32_t flags); };\n");
    write(workspace, "src/LogClient.h", "class LogClient { public: bool readPacket(size_t offset); };\n");
    storage::CodebaseIndex index(area.database("scope"), workspace);
    const auto refreshed = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(refreshed.value().filesIndexed == 2);
    ROCLINT_CHECK(refreshed.value().symbolsIndexed >= 6);

    const std::vector<core::rewrite::CanonicalIntent> raw{
        intent("offset", "in SessionManager::readPacket"),
        intent("offset", "in LogClient::readPacket"),
        intent("offset", "in MissingClass::readPacket"),
        intent("readPacket", ""),
        intent("newSymbol", "", core::rewrite::IntentAction::ADD),
        intent("readPacket", "in SessionManager", core::rewrite::IntentAction::MODIFY, "parameter"),
    };
    const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(
        raw, "src/SessionManager.h", index);
    ROCLINT_CHECK(grounded.size() == raw.size());
    ROCLINT_CHECK(statusOf(grounded[0]) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded[0].grounding->qualifiedTarget == "SessionManager::readPacket::offset");
    ROCLINT_CHECK(grounded[0].grounding->filePath == "src/SessionManager.h");
    ROCLINT_CHECK(grounded[1].grounding->qualifiedTarget == "LogClient::readPacket::offset");
    ROCLINT_CHECK(statusOf(grounded[2]) == core::rewrite::GroundingStatus::NotFoundInScope);
    ROCLINT_CHECK(statusOf(grounded[3]) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded[3].grounding->qualifiedTarget == "SessionManager::readPacket");
    ROCLINT_CHECK(statusOf(grounded[4]) == core::rewrite::GroundingStatus::Ungrounded);
    ROCLINT_CHECK(statusOf(grounded[5]) == core::rewrite::GroundingStatus::TypeMismatch);
    const auto xml = core::rewrite::AstGroundingEngine::renderPromptXml(grounded, 300);
    ROCLINT_CHECK(xml.find("<canonical_intents>") != std::string::npos);
    ROCLINT_CHECK(xml.find("status=\"VERIFIED\"") != std::string::npos);
    ROCLINT_CHECK(xml.find("SessionManager::readPacket::offset") != std::string::npos);
    ROCLINT_CHECK(xml.find("status=\"NOT_FOUND_IN_SCOPE\"") != std::string::npos);
}

void testWorkspaceIsolationAndCloneIdentity() {
    TempArea area;
    const auto workA = area.workspace("work-a");
    const auto workB = area.workspace("work-b");
    write(workA, "src/a.hpp", "class Alpha { public: int onlyInA; };\n");
    write(workB, "src/b.hpp", "class Beta { public: int onlyInB; };\n");
    const auto database = area.database("shared");
    storage::CodebaseIndex index(database);

    ROCLINT_CHECK(!index.isWorkspaceBound());
    ROCLINT_CHECK(!index.findByName("onlyInA").ok());
    ROCLINT_CHECK(index.refresh({workA, {}, true}).ok());
    ROCLINT_CHECK(index.isWorkspaceBound());
    const auto aHit = index.findByName("onlyInA");
    ROCLINT_CHECK(aHit.ok() && aHit.value().size() == 1);
    ROCLINT_CHECK(index.findByName("onlyInB").ok() && index.findByName("onlyInB").value().empty());

    ROCLINT_CHECK(index.refresh({workB, {}, true}).ok());
    const auto bHit = index.findByName("onlyInB");
    ROCLINT_CHECK(bHit.ok() && bHit.value().size() == 1);
    ROCLINT_CHECK(index.findByName("onlyInA").ok() && index.findByName("onlyInA").value().empty());
    const auto crossWorkspace = ground(index, "onlyInA", {}, "src/b.hpp");
    ROCLINT_CHECK(statusOf(crossWorkspace) == core::rewrite::GroundingStatus::NotFoundInScope);

    const auto cloneA = area.workspace("clone-a");
    const auto cloneB = area.workspace("clone-b");
    const std::string cloneSource = "class Widget { public: int value; };\n";
    write(cloneA, "src/shared.hpp", cloneSource);
    write(cloneB, "src/shared.hpp", cloneSource);
    storage::CodebaseIndex first(area.database("clones"), cloneA);
    storage::CodebaseIndex second(area.database("clones"), cloneB);
    ROCLINT_CHECK(first.refresh({cloneA, {}, true}).ok());
    ROCLINT_CHECK(second.refresh({cloneB, {}, true}).ok());
    const auto widgetA = first.findByName("Widget");
    const auto widgetB = second.findByName("Widget");
    ROCLINT_CHECK(widgetA.ok() && widgetA.value().size() == 1);
    ROCLINT_CHECK(widgetB.ok() && widgetB.value().size() == 1);
    ROCLINT_CHECK(widgetA.value().front().symbolId != widgetB.value().front().symbolId);

    storage::CodebaseIndex rebound(area.database("clones"));
    ROCLINT_CHECK(rebound.refresh({cloneA, {}, true}).ok());
    ROCLINT_CHECK(rebound.refresh({cloneB, {}, true}).ok());
    ROCLINT_CHECK(rebound.forgetWorkspace(cloneB).ok());
    ROCLINT_CHECK(!rebound.isWorkspaceBound());
}

void testFailedWorkspaceRefreshDoesNotFallBack() {
    TempArea area;
    const auto workA = area.workspace("fallback-a");
    const auto workB = area.workspace("fallback-b");
    write(workA, "src/a.hpp", "class Alpha { public: int onlyInA; };\n");
    write(workB, "src/b.hpp", "class Beta { public: int onlyInB; };\n");
    storage::CodebaseIndex index(area.database("fallback"), workA);
    ROCLINT_CHECK(index.refresh({workA, {}, true}).ok());
    ROCLINT_CHECK(index.findByName("onlyInA").value().size() == 1);

    const auto failed = index.refresh({workB, {}, false});
    ROCLINT_CHECK(!failed.ok());
    ROCLINT_CHECK(!index.isWorkspaceBound());
    ROCLINT_CHECK(!index.findByName("onlyInA").ok());

    const auto invalid = index.refresh({area.root / "missing-project", {}, true});
    ROCLINT_CHECK(!invalid.ok());
    ROCLINT_CHECK(!index.isWorkspaceBound());
}

void testLegacyIndexSchemaRebuilds() {
    TempArea area;
    const auto workspace = area.workspace("legacy");
    write(workspace, "src/legacy.hpp", "class Legacy { public: int legacyField; };\n");
    const auto database = area.database("legacy");
    createLegacyDatabase(database);
    storage::CodebaseIndex index(database, workspace);
    const auto refreshed = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(userVersion(database) == 2);
    const auto hit = index.findByName("legacyField");
    ROCLINT_CHECK(hit.ok() && hit.value().size() == 1);
}

void testQualifiedOverloadsRemainAmbiguous() {
    TempArea area;
    const auto workspace = area.workspace("overloads");
    write(workspace, "src/engine.hpp",
          "class Engine { public: bool run(int first); bool run(double second); };\n");
    storage::CodebaseIndex index(area.database("overloads"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto declarations = index.findQualified("Engine::run");
    ROCLINT_CHECK(declarations.ok() && declarations.value().size() == 2);
    const auto qualified = ground(index, "Engine::run");
    ROCLINT_CHECK(statusOf(qualified) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(qualified.grounding->candidates.size() >= 2);

    const auto first = ground(index, "first", "in Engine::run");
    ROCLINT_CHECK(statusOf(first) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(first.grounding->qualifiedTarget == "Engine::run::first");
    const auto second = ground(index, "second", "in Engine::run");
    ROCLINT_CHECK(statusOf(second) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(second.grounding->qualifiedTarget == "Engine::run::second");
    const auto both = ground(index, "run", "in Engine");
    ROCLINT_CHECK(statusOf(both) == core::rewrite::GroundingStatus::Ambiguous);
}

void testAmbiguousScopeCandidates() {
    TempArea area;
    const auto workspace = area.workspace("ambiguous-scope");
    write(workspace, "src/a.hpp", "class Engine { public: bool run(int first); bool shared(int value); };\n");
    write(workspace, "src/b.hpp", "class Engine { public: bool stop(int second); bool shared(int value); };\n");
    storage::CodebaseIndex index(area.database("ambiguous-scope"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto engines = index.findQualified("Engine");
    ROCLINT_CHECK(engines.ok() && engines.value().size() == 2);

    const auto uniqueChild = ground(index, "run", "in Engine");
    ROCLINT_CHECK(statusOf(uniqueChild) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(uniqueChild.grounding->qualifiedTarget == "Engine::run");
    const auto shared = ground(index, "shared", "in Engine");
    ROCLINT_CHECK(statusOf(shared) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(shared.grounding->candidates.size() >= 2);
    const auto missing = ground(index, "absent", "in Engine");
    ROCLINT_CHECK(statusOf(missing) == core::rewrite::GroundingStatus::Ambiguous);
}

void testDeclarationAndDefinitionSites() {
    TempArea area;
    const auto workspace = area.workspace("declarations");
    write(workspace, "src/engine.hpp", "class Engine { public: bool run(int first); };\n");
    write(workspace, "src/engine.cpp", "bool Engine::run(int first) { return first > 0; }\n");
    storage::CodebaseIndex index(area.database("declarations"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto declarations = index.findQualified("Engine::run");
    ROCLINT_CHECK(declarations.ok() && declarations.value().size() == 2);
    const auto active = ground(index, "Engine::run", {}, "src/engine.cpp");
    ROCLINT_CHECK(statusOf(active) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(active.grounding->filePath == "src/engine.cpp");
    const auto unscoped = ground(index, "Engine::run");
    ROCLINT_CHECK(statusOf(unscoped) == core::rewrite::GroundingStatus::Ambiguous);

    const auto parameter = ground(index, "first", "in Engine::run", "src/engine.cpp");
    ROCLINT_CHECK(statusOf(parameter) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(parameter.grounding->filePath == "src/engine.cpp");
    ROCLINT_CHECK(statusOf(ground(index, "first", "in Engine::run")) ==
                  core::rewrite::GroundingStatus::Ambiguous);
}

void testExplicitScopeIsFailClosedAndNormalized() {
    TempArea area;
    const auto workspace = area.workspace("explicit-scope");
    write(workspace, "src/engine.hpp", "class Engine { public: bool run(int first); };\n");
    storage::CodebaseIndex index(area.database("explicit-scope"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    for (const auto* condition : {"in MissingClass", "in `MissingClass`", "in **MissingClass**",
                                  "in __MissingClass__", "in 'MissingClass'"}) {
        const auto missing = ground(index, "Engine::run", condition);
        ROCLINT_CHECK(statusOf(missing) == core::rewrite::GroundingStatus::NotFoundInScope);
    }
    ROCLINT_CHECK(statusOf(ground(index, "MissingClass::run")) ==
                  core::rewrite::GroundingStatus::NotFoundInScope);
    const auto formattedTarget = ground(index, "`Engine::run`");
    ROCLINT_CHECK(statusOf(formattedTarget) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(formattedTarget.grounding->qualifiedTarget == "Engine::run");
    const auto formattedScope = ground(index, "run", "in `Engine`");
    ROCLINT_CHECK(statusOf(formattedScope) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(formattedScope.grounding->qualifiedTarget == "Engine::run");

    const auto raw = core::rewrite::CanonicalIntentExtractor::extract("Rename `Engine::run` to `step`.");
    const auto extracted = core::rewrite::AstGroundingEngine::groundIntents(raw, {}, index);
    const auto* target = findIntent(extracted, "Engine");
    ROCLINT_CHECK(target != nullptr);
    ROCLINT_CHECK(target->target.find('`') != std::string::npos);
    ROCLINT_CHECK(statusOf(*target) == core::rewrite::GroundingStatus::Verified);

    const auto missingRaw =
        core::rewrite::CanonicalIntentExtractor::extract("Rename Engine::run to step in `MissingClass`.");
    const auto missingGrounded = core::rewrite::AstGroundingEngine::groundIntents(missingRaw, {}, index);
    const auto* missingTarget = findIntent(missingGrounded, "Engine");
    ROCLINT_CHECK(missingTarget != nullptr);
    ROCLINT_CHECK(statusOf(*missingTarget) == core::rewrite::GroundingStatus::NotFoundInScope);
}

void testRealExtractorOverloadAndMissingScope() {
    TempArea area;
    const auto workspace = area.workspace("extractor");
    write(workspace, "src/engine.hpp",
          "class Engine { public: bool run(int first); bool run(double second); };\n");
    storage::CodebaseIndex index(area.database("extractor"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto raw = core::rewrite::CanonicalIntentExtractor::extract("Rename Engine::run to step.");
    const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(raw, {}, index);
    const auto* target = findIntent(grounded, "Engine::run");
    ROCLINT_CHECK(target != nullptr);
    ROCLINT_CHECK(statusOf(*target) == core::rewrite::GroundingStatus::Ambiguous);

    const auto missingRaw =
        core::rewrite::CanonicalIntentExtractor::extract("Rename Engine::run to step in MissingClass.");
    const auto missing = core::rewrite::AstGroundingEngine::groundIntents(missingRaw, {}, index);
    const auto* missingTarget = findIntent(missing, "Engine::run");
    ROCLINT_CHECK(missingTarget != nullptr);
    ROCLINT_CHECK(statusOf(*missingTarget) == core::rewrite::GroundingStatus::NotFoundInScope);
}

void testRefreshInvalidatesTruncatedEvidence() {
    TempArea area;
    const auto workspace = area.workspace("truncate");
    const auto file = workspace / "src/gone.hpp";
    write(workspace, "src/gone.hpp", "class Gone { public: int oldField; };\n");
    storage::CodebaseIndex index(area.database("truncate"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(statusOf(ground(index, "oldField", "in Gone")) ==
                  core::rewrite::GroundingStatus::Verified);

    write(workspace, "src/gone.hpp", "");
    ROCLINT_CHECK(fs::file_size(file) == 0);
    const auto refreshed = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(index.findByName("oldField").ok() && index.findByName("oldField").value().empty());
    ROCLINT_CHECK(statusOf(ground(index, "oldField", "in Gone")) ==
                  core::rewrite::GroundingStatus::NotFoundInScope);

    write(workspace, "src/gone.hpp", "class Back { public: int newField; };\n");
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(statusOf(ground(index, "newField", "in Back")) ==
                  core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(index.findByName("oldField").value().empty());
}

void testRefreshDeletionOversizedAndUnreadable() {
    TempArea area;
    {
        const auto workspace = area.workspace("deleted");
        write(workspace, "src/deleted.hpp", "class Deleted { public: int deletedField; };\n");
        storage::CodebaseIndex index(area.database("deleted"), workspace);
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        fs::remove(workspace / "src/deleted.hpp");
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        ROCLINT_CHECK(index.findByName("deletedField").value().empty());
    }
    {
        const auto workspace = area.workspace("oversized");
        write(workspace, "src/huge.hpp", "class Huge { public: int hugeField; };\n");
        storage::CodebaseIndex index(area.database("oversized"), workspace);
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        std::string oversized = "class Huge { public: int replacementField; };\n";
        oversized.append(storage::CodebaseIndex::kMaximumFileBytes + 1, ' ');
        write(workspace, "src/huge.hpp", oversized);
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        ROCLINT_CHECK(index.findByName("hugeField").value().empty());
        ROCLINT_CHECK(index.findByName("replacementField").value().empty());
        write(workspace, "src/huge.hpp", "class Huge { public: int freshField; };\n");
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        ROCLINT_CHECK(index.findByName("freshField").value().size() == 1);
        ROCLINT_CHECK(index.findByName("hugeField").value().empty());
    }
    {
        const auto workspace = area.workspace("unreadable");
        const auto file = workspace / "src/blocked.hpp";
        write(workspace, "src/blocked.hpp", "class Blocked { public: int blockedField; };\n");
        storage::CodebaseIndex index(area.database("unreadable"), workspace);
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        fs::remove(file);
        fs::create_directory(file);
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        ROCLINT_CHECK(index.findByName("blockedField").value().empty());
        fs::remove(file);
        write(workspace, "src/blocked.hpp", "class Restored { public: int restoredField; };\n");
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        ROCLINT_CHECK(index.findByName("restoredField").value().size() == 1);
        ROCLINT_CHECK(index.findByName("blockedField").value().empty());
    }
}

void testRefreshFailedReadInvalidatesEvidence() {
#ifdef _WIN32
    TempArea area;
    const auto workspace = area.workspace("read-failure");
    const auto file = workspace / "src/locked.hpp";
    write(workspace, "src/locked.hpp", "class Locked { public: int lockedField; };\n");
    storage::CodebaseIndex index(area.database("read-failure"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(exactSymbolCount(index, "lockedField") == 1);

    // A share-mode-zero handle makes the source unreadable while metadata remains
    // visible, exercising the actual read-failure path rather than a directory.
    HANDLE handle = CreateFileW(file.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    ROCLINT_CHECK(handle != INVALID_HANDLE_VALUE);
    const auto lockedRefresh = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(lockedRefresh.ok());
    ROCLINT_CHECK(exactSymbolCount(index, "lockedField") == 0);
    CloseHandle(handle);

    write(workspace, "src/locked.hpp", "class Locked { public: int unlockedField; };\n");
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(exactSymbolCount(index, "unlockedField") == 1);
    ROCLINT_CHECK(exactSymbolCount(index, "lockedField") == 0);
    ROCLINT_CHECK(statusOf(ground(index, "lockedField", "in Locked")) ==
                  core::rewrite::GroundingStatus::NotFoundInScope);
#else
    std::cout << "  [SKIP] exclusive read failure is Windows-specific\n";
#endif
}

void testIncompleteScanPreservesUnvisitedEvidence() {
    TempArea area;
    const auto workspace = area.workspace("incomplete");
    write(workspace, "src/one.hpp", "class One { public: int oldOne; };\n");
    write(workspace, "src/two.hpp", "class Two { public: int oldTwo; };\n");
    storage::CodebaseIndex index(area.database("incomplete"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(index.findByName("oldOne").value().size() == 1);
    ROCLINT_CHECK(index.findByName("oldTwo").value().size() == 1);

    write(workspace, "src/one.hpp", "");
    write(workspace, "src/two.hpp", "");
    const storage::CodebaseIndexLimits emptyScan{
        storage::CodebaseIndex::kMaximumIndexedFiles, 0, storage::CodebaseIndex::kMaximumFileBytes};
    const auto incomplete = index.refresh({workspace, {}, true}, emptyScan);
    ROCLINT_CHECK(incomplete.ok());
    ROCLINT_CHECK(incomplete.value().incomplete);
    ROCLINT_CHECK(index.findByName("oldOne").value().size() == 1);
    ROCLINT_CHECK(index.findByName("oldTwo").value().size() == 1);

    const auto complete = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(complete.ok());
    ROCLINT_CHECK(!complete.value().incomplete);
    ROCLINT_CHECK(index.findByName("oldOne").value().empty());
    ROCLINT_CHECK(index.findByName("oldTwo").value().empty());
}

void testCrossFileAmbiguityAndActiveFileRanking() {
    TempArea area;
    const auto workspace = area.workspace("ambiguity");
    write(workspace, "src/SessionManager.h",
          "class SessionManager { public: bool readPacket(size_t offset); };\n");
    write(workspace, "src/LogClient.h", "class LogClient { public: bool readPacket(size_t offset); };\n");
    storage::CodebaseIndex index(area.database("ambiguity"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto ambiguous = ground(index, "readPacket");
    ROCLINT_CHECK(statusOf(ambiguous) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(ambiguous.grounding->candidates.size() >= 2);
    const auto active = ground(index, "readPacket", {}, "src/LogClient.h");
    ROCLINT_CHECK(statusOf(active) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(active.grounding->qualifiedTarget == "LogClient::readPacket");
}

void testRecoveredHeaderStillGroundsAndHashesSource() {
    TempArea area;
    const auto workspace = area.workspace("recovered");
    write(workspace, "src/Widget.h",
          "class Widget : public Base {\n"
          "    Q_PROPERTY(bool visible READ isVisible NOTIFY visibilityChanged)\n"
          "public:\n"
          "    bool setVisible(bool visible);\n"
          "    bool visible;\n"
          "};\n");
    storage::CodebaseIndex index(area.database("recovered"), workspace);
    const auto refreshed = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(refreshed.value().filesIndexed == 1);
    ROCLINT_CHECK(refreshed.value().filesRecovered == 1);
    const auto bogus = index.findByName("Q_PROPERTY");
    ROCLINT_CHECK(bogus.ok() && bogus.value().empty());

    const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(
        {intent("visible", "in Widget::setVisible")}, "src/Widget.h", index);
    ROCLINT_CHECK(statusOf(grounded.front()) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded.front().grounding->qualifiedTarget == "Widget::setVisible::visible");
    ROCLINT_CHECK(!grounded.front().grounding->fileHash.empty());
    const auto firstSnapshot = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(grounded, "workspace-id");
    ROCLINT_CHECK(firstSnapshot.size() == 64);
    ROCLINT_CHECK(firstSnapshot ==
                  core::rewrite::AstGroundingEngine::computeAstSnapshotHash(grounded, "workspace-id"));
    ROCLINT_CHECK(firstSnapshot !=
                  core::rewrite::AstGroundingEngine::computeAstSnapshotHash(grounded, "other-workspace"));

    write(workspace, "src/Widget.h",
          "class Widget : public Base {\n"
          "    Q_PROPERTY(bool visible READ isVisible NOTIFY visibilityChanged)\n"
          "public:\n"
          "    bool setVisible(bool visible, bool animate);\n"
          "    bool visible;\n"
          "};\n");
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto edited = core::rewrite::AstGroundingEngine::groundIntents(
        {intent("visible", "in Widget::setVisible")}, "src/Widget.h", index);
    ROCLINT_CHECK(statusOf(edited.front()) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(edited, "workspace-id") !=
                  firstSnapshot);
}

std::vector<std::string> signatureBlocks(const std::string& xml) {
    std::vector<std::string> blocks;
    std::size_t position = 0;
    while ((position = xml.find("<signature>", position)) != std::string::npos) {
        const auto begin = position + std::string("<signature>").size();
        const auto end = xml.find("</signature>", begin);
        if (end == std::string::npos) break;
        blocks.push_back(xml.substr(begin, end - begin));
        position = end + std::string("</signature>").size();
    }
    return blocks;
}

void testPromptXmlSignatureBudget() {
    core::rewrite::CanonicalIntent value;
    value.action = core::rewrite::IntentAction::MODIFY;
    value.target = "count";
    value.property = "name";
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget = "Widget::setCount";
    evidence.filePath = "src/Widget.h";
    evidence.line = 12;
    evidence.signature = "bool setCount(std::size_t count, const std::string& label = \"a&b<c>\")";
    value.grounding = evidence;

    const auto bounded = core::rewrite::AstGroundingEngine::renderPromptXml({value}, 64);
    const auto boundedBlocks = signatureBlocks(bounded);
    ROCLINT_CHECK(boundedBlocks.size() == 1);
    ROCLINT_CHECK(boundedBlocks.front().size() <= 64);
    for (std::size_t amp = boundedBlocks.front().find('&'); amp != std::string::npos;
         amp = boundedBlocks.front().find('&', amp + 1)) {
        const auto semicolon = boundedBlocks.front().find(';', amp);
        ROCLINT_CHECK(semicolon != std::string::npos && semicolon > amp);
    }

    auto second = value;
    second.target = "label";
    second.grounding->qualifiedTarget = "Widget::setLabel";
    second.grounding->signature = "bool setLabel(const std::string& value, bool visible)";
    auto third = value;
    third.target = "visible";
    third.grounding->qualifiedTarget = "Widget::setVisible";
    third.grounding->signature = "bool setVisible(bool visible, bool notify, bool animate)";
    const auto budgeted = core::rewrite::AstGroundingEngine::renderPromptXml({value, second, third}, 300);
    std::size_t total = 0;
    for (const auto& block : signatureBlocks(budgeted)) total += block.size();
    ROCLINT_CHECK(total <= 300);
    ROCLINT_CHECK(budgeted.find("return") == std::string::npos);
}

void testPromptXmlExcludesMemberInitializerBodies() {
    TempArea area;
    const auto workspace = area.workspace("bodies");
    write(workspace, "src/handler.hpp",
          "class Handler { public: int value = [] { return 12345; }(); };\n");
    storage::CodebaseIndex index(area.database("bodies"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto grounded = ground(index, "value", "in Handler");
    ROCLINT_CHECK(statusOf(grounded) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded.grounding->signature.find("return") == std::string::npos);
    const auto xml = core::rewrite::AstGroundingEngine::renderPromptXml({grounded}, 300);
    ROCLINT_CHECK(xml.find("return 12345") == std::string::npos);
    for (const auto& block : signatureBlocks(xml)) ROCLINT_CHECK(block.size() <= 300);
}

void testLookupLatency() {
    TempArea area;
    const auto workspace = area.workspace("latency");
    write(workspace, "src/ledger.h",
          "class Ledger { public: bool postEntry(size_t accountId, size_t amount); };\n");
    storage::CodebaseIndex index(area.database("latency"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto raw = std::vector<core::rewrite::CanonicalIntent>{
        intent("accountId", "in Ledger::postEntry"),
    };
    constexpr int kSamples = 200;
    const auto started = std::chrono::steady_clock::now();
    for (int sample = 0; sample < kSamples; ++sample) {
        const auto result = core::rewrite::AstGroundingEngine::groundIntents(raw, "src/ledger.h", index);
        ROCLINT_CHECK(result.front().grounding->status == core::rewrite::GroundingStatus::Verified);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count() / kSamples;
    std::cout << "  [INFO] AST grounding average=" << elapsed << " ms\n";
    ROCLINT_CHECK(elapsed < 2.0);
}

void testSourceCodeHasNoBodies() {
    const std::string source = "int hidden() { return 7; }\n";
    const auto result = storage::AstSymbolExtractor::extractSymbols(source, "hidden.cpp");
    ROCLINT_CHECK(result.parseOk);
    ROCLINT_CHECK(!result.symbols.empty());
    for (const auto& symbol : result.symbols) {
        ROCLINT_CHECK(symbol.signature.find("return 7") == std::string::npos);
        ROCLINT_CHECK(symbol.signature.size() <= 1200);
    }
}

} // namespace

int main() {
    testExactScopeAndHallucination();
    testWorkspaceIsolationAndCloneIdentity();
    testFailedWorkspaceRefreshDoesNotFallBack();
    testLegacyIndexSchemaRebuilds();
    testQualifiedOverloadsRemainAmbiguous();
    testAmbiguousScopeCandidates();
    testDeclarationAndDefinitionSites();
    testExplicitScopeIsFailClosedAndNormalized();
    testRealExtractorOverloadAndMissingScope();
    testRefreshInvalidatesTruncatedEvidence();
    testRefreshDeletionOversizedAndUnreadable();
    testRefreshFailedReadInvalidatesEvidence();
    testIncompleteScanPreservesUnvisitedEvidence();
    testCrossFileAmbiguityAndActiveFileRanking();
    testRecoveredHeaderStillGroundsAndHashesSource();
    testPromptXmlSignatureBudget();
    testPromptXmlExcludesMemberInitializerBodies();
    testLookupLatency();
    testSourceCodeHasNoBodies();
    std::cout << "AST grounding, workspace isolation, ambiguity, scope, refresh and signature gates passed.\n";
    return 0;
}
