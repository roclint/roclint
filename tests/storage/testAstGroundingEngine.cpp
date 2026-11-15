#include "roclint/core/Json.hpp"
#include "roclint/rewrite/AstGroundingEngine.hpp"
#include "roclint/storage/AstSymbolExtractor.hpp"
#include "roclint/storage/CodebaseIndex.hpp"
#include "support/check.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
                                      std::string property = "name", std::string value = "changed") {
    core::rewrite::CanonicalIntent canonical;
    canonical.action = action;
    canonical.target = std::move(target);
    canonical.condition = std::move(condition);
    canonical.property = std::move(property);
    canonical.value = std::move(value);
    canonical.nodeId = "nid_test";
    return canonical;
}

core::rewrite::CanonicalIntent ground(const storage::CodebaseIndex& index, std::string target,
                                      std::string condition = {}, std::string active = {},
                                      std::string property = "name",
                                      core::rewrite::IntentAction action = core::rewrite::IntentAction::MODIFY,
                                      std::string value = "changed") {
    return core::rewrite::AstGroundingEngine::groundIntents(
        {intent(std::move(target), std::move(condition), action, std::move(property), std::move(value))},
        active, index).front();
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

core::rewrite::CanonicalIntent verifiedIntent(std::string target,
                                              std::string qualifiedTarget = {},
                                              std::string file = {},
                                              std::string signature = {},
                                              std::uint32_t line = 0) {
    auto value = intent(std::move(target));
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget =
        qualifiedTarget.empty() ? value.target : std::move(qualifiedTarget);
    evidence.filePath = std::move(file);
    evidence.line = line;
    evidence.signature = std::move(signature);
    evidence.fileHash = std::string(64, 'a');
    evidence.fileSpan = {0, 1};
    value.grounding = std::move(evidence);
    return value;
}

std::vector<std::string> emittedTargets(const std::string& xml) {
    std::vector<std::string> targets;
    std::size_t position = 0;
    while ((position = xml.find("<intent ", position)) != std::string::npos) {
        const auto attribute = xml.find(" target=\"", position);
        if (attribute == std::string::npos) break;
        const auto begin = attribute + std::string(" target=\"").size();
        const auto end = xml.find('"', begin);
        if (end == std::string::npos) break;
        targets.push_back(xml.substr(begin, end - begin));
        position = end + 1;
    }
    return targets;
}

std::size_t exactSymbolCount(const storage::CodebaseIndex& index, std::string_view name) {
    const auto found = index.findByName(name, {}, {}, 64, false);
    ROCLINT_CHECK(found.ok());
    return found.value().symbols.size();
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

std::int64_t cachedSymbolCount(const fs::path& path) {
    sqlite3* db = nullptr;
    ROCLINT_CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ROCLINT_CHECK(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM ast_symbols;", -1, &statement, nullptr) == SQLITE_OK);
    ROCLINT_CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto count = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return count;
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
    ROCLINT_CHECK(xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(xml.find("<canonical_intents>") != std::string::npos);
    ROCLINT_CHECK(xml.find("status=\"VERIFIED\"") != std::string::npos);
    ROCLINT_CHECK(xml.find("SessionManager::readPacket::offset") != std::string::npos);
    // A crowded batch cannot contain every intent inside 350 bytes; the
    // renderer still represents an absence result when it is the evidence
    // selected for the block.
    const auto notFoundXml =
        core::rewrite::AstGroundingEngine::renderPromptXml({grounded[2]}, 300);
    ROCLINT_CHECK(notFoundXml.find("status=\"NOT_FOUND_IN_SCOPE\"") != std::string::npos);
    ROCLINT_CHECK(notFoundXml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
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
    ROCLINT_CHECK(aHit.ok() && aHit.value().symbols.size() == 1);
    ROCLINT_CHECK(index.findByName("onlyInB").ok() && index.findByName("onlyInB").value().symbols.empty());

    ROCLINT_CHECK(index.refresh({workB, {}, true}).ok());
    const auto bHit = index.findByName("onlyInB");
    ROCLINT_CHECK(bHit.ok() && bHit.value().symbols.size() == 1);
    ROCLINT_CHECK(index.findByName("onlyInA").ok() && index.findByName("onlyInA").value().symbols.empty());
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
    ROCLINT_CHECK(widgetA.ok() && widgetA.value().symbols.size() == 1);
    ROCLINT_CHECK(widgetB.ok() && widgetB.value().symbols.size() == 1);
    ROCLINT_CHECK(widgetA.value().symbols.front().symbolId != widgetB.value().symbols.front().symbolId);

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
    ROCLINT_CHECK(index.findByName("onlyInA").value().symbols.size() == 1);

    const auto failed = index.refresh({workB, {}, false});
    ROCLINT_CHECK(!failed.ok());
    ROCLINT_CHECK(!index.isWorkspaceBound());
    ROCLINT_CHECK(!index.findByName("onlyInA").ok());
    const auto unbound = ground(index, "onlyInA");
    ROCLINT_CHECK(statusOf(unbound) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!unbound.grounding->complete);

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
    ROCLINT_CHECK(userVersion(database) == 5);
    const auto hit = index.findByName("legacyField");
    ROCLINT_CHECK(hit.ok() && hit.value().symbols.size() == 1);
}

void testQualifiedOverloadsRemainAmbiguous() {
    TempArea area;
    const auto workspace = area.workspace("overloads");
    write(workspace, "src/engine.hpp",
          "class Engine { public: bool run(int first); bool run(double second); };\n");
    storage::CodebaseIndex index(area.database("overloads"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto declarations = index.findQualified("Engine::run");
    ROCLINT_CHECK(declarations.ok() && declarations.value().symbols.size() == 2);
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
    const auto scopedSignature = ground(index, "Engine::run(double)", "in Engine");
    ROCLINT_CHECK(statusOf(scopedSignature) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(scopedSignature.grounding->qualifiedTarget == "Engine::run");
    ROCLINT_CHECK(scopedSignature.grounding->signature.find("double second") != std::string::npos);
    ROCLINT_CHECK(scopedSignature.grounding->matchReason == "signature");
}

void testAmbiguousScopeCandidates() {
    TempArea area;
    const auto workspace = area.workspace("ambiguous-scope");
    write(workspace, "src/a.hpp", "class Engine { public: bool run(int first); bool shared(int value); };\n");
    write(workspace, "src/b.hpp", "class Engine { public: bool stop(int second); bool shared(int value); };\n");
    storage::CodebaseIndex index(area.database("ambiguous-scope"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto engines = index.findQualified("Engine");
    ROCLINT_CHECK(engines.ok() && engines.value().symbols.size() == 2);

    const auto uniqueChild = ground(index, "run", "in Engine");
    ROCLINT_CHECK(statusOf(uniqueChild) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(uniqueChild.grounding->qualifiedTarget == "Engine::run");
    const auto shared = ground(index, "shared", "in Engine");
    ROCLINT_CHECK(statusOf(shared) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(shared.grounding->candidates.size() >= 2);
    const auto missing = ground(index, "absent", "in Engine");
    ROCLINT_CHECK(statusOf(missing) == core::rewrite::GroundingStatus::Ambiguous);
}

std::string largeClassSource(std::string_view extraMember = {}) {
    std::string code = "class Large { public:\n";
    for (int index = 0; index < 140; ++index)
        code += "int member" + std::to_string(index) + ";\n";
    if (!extraMember.empty()) code += "int " + std::string(extraMember) + ";\n";
    code += "};\n";
    return code;
}

void testLargeScopeMatchesLateMembers() {
    TempArea area;
    const auto workspace = area.workspace("large-class");
    write(workspace, "src/large.hpp", largeClassSource());
    storage::CodebaseIndex index(area.database("large-class"), workspace);
    const auto refreshed = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(refreshed.value().symbolsIndexed == 141);
    ROCLINT_CHECK(exactSymbolCount(index, "member139") == 1);

    const auto early = ground(index, "member0", "in Large");
    ROCLINT_CHECK(statusOf(early) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(early.grounding->qualifiedTarget == "Large::member0");
    const auto late = ground(index, "member139", "in Large");
    ROCLINT_CHECK(statusOf(late) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(late.grounding->qualifiedTarget == "Large::member139");
    ROCLINT_CHECK(statusOf(ground(index, "member140", "in Large")) ==
                  core::rewrite::GroundingStatus::NotFoundInScope);

    const auto raw = core::rewrite::CanonicalIntentExtractor::extract(
        "Rename member139 to renamed in Large.");
    const auto extracted = core::rewrite::AstGroundingEngine::groundIntents(raw, {}, index);
    const auto* target = findIntent(extracted, "member139");
    ROCLINT_CHECK(target != nullptr);
    ROCLINT_CHECK(statusOf(*target) == core::rewrite::GroundingStatus::Verified);
}

void testLargeScopeAmbiguityAcrossCandidates() {
    TempArea area;
    const auto workspace = area.workspace("large-ambiguous");
    write(workspace, "src/a.hpp", largeClassSource("onlyA139"));
    write(workspace, "src/b.hpp", largeClassSource());
    storage::CodebaseIndex index(area.database("large-ambiguous"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    const auto scopes = index.findQualified("Large");
    ROCLINT_CHECK(scopes.ok() && scopes.value().symbols.size() == 2);

    ROCLINT_CHECK(statusOf(ground(index, "member0", "in Large")) ==
                  core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(statusOf(ground(index, "member139", "in Large")) ==
                  core::rewrite::GroundingStatus::Ambiguous);
    const auto unique = ground(index, "onlyA139", "in Large");
    ROCLINT_CHECK(statusOf(unique) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(unique.grounding->qualifiedTarget == "Large::onlyA139");
    ROCLINT_CHECK(statusOf(ground(index, "absent", "in Large")) ==
                  core::rewrite::GroundingStatus::Ambiguous);
}

void testDeclarationAndDefinitionSites() {
    TempArea area;
    const auto workspace = area.workspace("declarations");
    write(workspace, "src/engine.hpp", "class Engine { public: bool run(int first); };\n");
    write(workspace, "src/engine.cpp", "bool Engine::run(int first) { return first > 0; }\n");
    storage::CodebaseIndex index(area.database("declarations"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto declarations = index.findQualified("Engine::run");
    ROCLINT_CHECK(declarations.ok() && declarations.value().symbols.size() == 2);
    const auto active = ground(index, "Engine::run", {}, "src/engine.cpp");
    ROCLINT_CHECK(statusOf(active) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(active.grounding->filePath == "src/engine.cpp");
    const auto unscoped = ground(index, "Engine::run");
    ROCLINT_CHECK(statusOf(unscoped) == core::rewrite::GroundingStatus::Ambiguous);

    // An explicit signatureKey is evidence of a declaration/definition
    // relationship, not permission to merge their file/span bindings.
    const auto signedUnscoped = ground(index, "Engine::run(int)");
    ROCLINT_CHECK(statusOf(signedUnscoped) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(signedUnscoped.grounding->candidates.size() >= 2);
    const auto signedActive = ground(index, "Engine::run(int)", {}, "src/engine.cpp");
    ROCLINT_CHECK(statusOf(signedActive) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(signedActive.grounding->filePath == "src/engine.cpp");
    ROCLINT_CHECK(!signedActive.grounding->fileHash.empty());
    ROCLINT_CHECK(signedActive.grounding->fileSpan.endByte > signedActive.grounding->fileSpan.startByte);

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
    const auto unsupportedQualifier = ground(index, "Engine::run(int) const");
    ROCLINT_CHECK(statusOf(unsupportedQualifier) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!unsupportedQualifier.grounding->complete);
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
    ROCLINT_CHECK(index.findByName("oldField").ok() && index.findByName("oldField").value().symbols.empty());
    ROCLINT_CHECK(statusOf(ground(index, "oldField", "in Gone")) ==
                  core::rewrite::GroundingStatus::NotFoundInScope);

    write(workspace, "src/gone.hpp", "class Back { public: int newField; };\n");
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
    ROCLINT_CHECK(statusOf(ground(index, "newField", "in Back")) ==
                  core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(index.findByName("oldField").value().symbols.empty());
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
        ROCLINT_CHECK(index.findByName("deletedField").value().symbols.empty());
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
        ROCLINT_CHECK(index.findByName("hugeField").value().symbols.empty());
        ROCLINT_CHECK(index.findByName("replacementField").value().symbols.empty());
        write(workspace, "src/huge.hpp", "class Huge { public: int freshField; };\n");
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        ROCLINT_CHECK(index.findByName("freshField").value().symbols.size() == 1);
        ROCLINT_CHECK(index.findByName("hugeField").value().symbols.empty());
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
        ROCLINT_CHECK(index.findByName("blockedField").value().symbols.empty());
        fs::remove(file);
        write(workspace, "src/blocked.hpp", "class Restored { public: int restoredField; };\n");
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());
        ROCLINT_CHECK(index.findByName("restoredField").value().symbols.size() == 1);
        ROCLINT_CHECK(index.findByName("blockedField").value().symbols.empty());
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

void testIncompleteRefreshWithholdsStaleEvidence() {
    TempArea area;
    const auto workspace = area.workspace("incomplete");
    const auto database = area.database("incomplete");
    write(workspace, "src/one.hpp", "class One { public: int oldOne; };\n");
    write(workspace, "src/two.hpp", "class Two { public: int oldTwo; };\n");
    storage::CodebaseIndex index(database, workspace);
    const auto first = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(first.ok());
    ROCLINT_CHECK(!first.value().incomplete);
    const auto firstGeneration = first.value().generation;
    ROCLINT_CHECK(firstGeneration > 0);
    ROCLINT_CHECK(exactSymbolCount(index, "oldOne") == 1);
    ROCLINT_CHECK(exactSymbolCount(index, "oldTwo") == 1);
    const auto firstGrounding = ground(index, "oldOne", "in One");
    ROCLINT_CHECK(statusOf(firstGrounding) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(firstGrounding.grounding->complete);
    ROCLINT_CHECK(!core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                       {firstGrounding}, "workspace").empty());
    const auto firstInfo = index.snapshotInfo();
    ROCLINT_CHECK(firstInfo.ok() && firstInfo.value().available);
    ROCLINT_CHECK(firstInfo.value().complete);

    write(workspace, "src/one.hpp", "");
    write(workspace, "src/two.hpp", "");
    const storage::CodebaseIndexLimits emptyScan{
        storage::CodebaseIndex::kMaximumIndexedFiles, 0, storage::CodebaseIndex::kMaximumFileBytes};
    const auto incomplete = index.refresh({workspace, {}, true}, emptyScan);
    ROCLINT_CHECK(incomplete.ok());
    ROCLINT_CHECK(incomplete.value().incomplete);
    ROCLINT_CHECK(incomplete.value().generation == firstGeneration + 1);
    const auto incompleteInfo = index.snapshotInfo();
    ROCLINT_CHECK(incompleteInfo.ok() && incompleteInfo.value().available);
    ROCLINT_CHECK(!incompleteInfo.value().complete);
    ROCLINT_CHECK(!incompleteInfo.value().fullScanComplete);

    // The old cache rows are retained, but they belong to the prior generation
    // and must not become authoritative evidence. Absence on an incomplete
    // snapshot is not a proof, so the result is bounded ambiguity, not
    // NotFoundInScope.
    ROCLINT_CHECK(cachedSymbolCount(database) > 0);
    ROCLINT_CHECK(exactSymbolCount(index, "oldOne") == 0);
    ROCLINT_CHECK(exactSymbolCount(index, "oldTwo") == 0);
    const auto stale = ground(index, "oldOne", "in One");
    ROCLINT_CHECK(statusOf(stale) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(stale.grounding->complete == false);
    ROCLINT_CHECK(statusOf(stale) != core::rewrite::GroundingStatus::NotFoundInScope);
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash({stale}, "workspace").empty());

    // A newly opened instance reads the same current generation and cannot
    // bypass the freshness gate.
    storage::CodebaseIndex reopened(database, workspace);
    ROCLINT_CHECK(exactSymbolCount(reopened, "oldOne") == 0);
    const auto reopenedStale = ground(reopened, "oldOne", "in One");
    ROCLINT_CHECK(statusOf(reopenedStale) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!reopenedStale.grounding->complete);

    write(workspace, "src/one.hpp", "class One { public: int freshOne; };\n");
    write(workspace, "src/two.hpp", "class Two { public: int freshTwo; };\n");
    const auto complete = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(complete.ok());
    ROCLINT_CHECK(!complete.value().incomplete);
    ROCLINT_CHECK(complete.value().generation == firstGeneration + 2);
    const auto completeInfo = index.snapshotInfo();
    ROCLINT_CHECK(completeInfo.ok() && completeInfo.value().complete);
    const auto fresh = ground(index, "freshOne", "in One");
    ROCLINT_CHECK(statusOf(fresh) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(fresh.grounding->complete);
    ROCLINT_CHECK(!core::rewrite::AstGroundingEngine::computeAstSnapshotHash({fresh}, "workspace").empty());
    ROCLINT_CHECK(exactSymbolCount(index, "oldOne") == 0);
    ROCLINT_CHECK(exactSymbolCount(index, "freshTwo") == 1);
}

void testPathsRefreshCompletenessGatesAuthority() {
    TempArea area;
    const auto workspace = area.workspace("paths-completeness");
    const auto database = area.database("paths-completeness");
    write(workspace, "src/a.hpp", "int shared(int value);\n");
    write(workspace, "src/b.hpp", "int shared(int value);\nint onlyInB(int value);\n");
    write(workspace, "src/c.hpp", "int carried(int value);\n");
    storage::CodebaseIndex index(database, workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto hashOf = [](const core::rewrite::CanonicalIntent& value) {
        return core::rewrite::AstGroundingEngine::computeAstSnapshotHash({value}, "workspace");
    };

    const auto initial = ground(index, "shared");
    ROCLINT_CHECK(statusOf(initial) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(initial.grounding->complete);
    ROCLINT_CHECK(initial.grounding->candidates.size() >= 2);
    ROCLINT_CHECK(hashOf(initial).empty());

    // Maximum one processed path over two requested paths: the second path is
    // marked dirty and coverage becomes incomplete.
    const storage::CodebaseIndexLimits oneFile{
        1, storage::CodebaseIndex::kScanBudgetMs, storage::CodebaseIndex::kMaximumFileBytes};
    const auto partial = index.refreshPaths(
        {workspace, {}, true}, {"src/a.hpp", "src/b.hpp"}, oneFile);
    ROCLINT_CHECK(partial.ok());
    ROCLINT_CHECK(partial.value().incomplete);
    ROCLINT_CHECK(partial.value().unprocessedPaths.size() == 1);
    ROCLINT_CHECK(partial.value().unprocessedPaths.front() == "src/b.hpp");
    const auto partialInfo = index.snapshotInfo();
    ROCLINT_CHECK(partialInfo.ok() && partialInfo.value().available);
    ROCLINT_CHECK(!partialInfo.value().complete);
    ROCLINT_CHECK(!partialInfo.value().coverageComplete);

    // Queries stay bounded but explicitly report the incomplete universe.
    const auto visible = index.findByName("shared", {}, {}, 64, false);
    ROCLINT_CHECK(visible.ok());
    ROCLINT_CHECK(!visible.value().complete);
    ROCLINT_CHECK(visible.value().symbols.size() == 1);
    ROCLINT_CHECK(visible.value().symbols.front().filePath == "src/a.hpp");

    const auto partialShared = ground(index, "shared");
    ROCLINT_CHECK(statusOf(partialShared) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!partialShared.grounding->complete);
    ROCLINT_CHECK(!partialShared.grounding->candidates.empty());
    ROCLINT_CHECK(statusOf(partialShared) != core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(hashOf(partialShared).empty());

    // Already-grounded evidence cannot survive an incomplete snapshot either.
    const auto replayed = core::rewrite::AstGroundingEngine::groundIntents(
        {initial}, {}, index);
    ROCLINT_CHECK(statusOf(replayed.front()) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!replayed.front().grounding->complete);
    ROCLINT_CHECK(hashOf(replayed.front()).empty());

    // The target that lives only in the unprocessed file is a false absence;
    // incomplete coverage cannot promote it to NotFoundInScope or Verified.
    const auto falseAbsence = ground(index, "onlyInB");
    ROCLINT_CHECK(statusOf(falseAbsence) != core::rewrite::GroundingStatus::NotFoundInScope);
    ROCLINT_CHECK(statusOf(falseAbsence) != core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(!falseAbsence.grounding->complete);
    ROCLINT_CHECK(hashOf(falseAbsence).empty());

    // Unchanged rows carried into the new generation remain queryable, but the
    // incomplete universe still prevents authoritative promotion.
    ROCLINT_CHECK(exactSymbolCount(index, "carried") == 1);
    const auto carried = ground(index, "carried");
    ROCLINT_CHECK(statusOf(carried) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!carried.grounding->complete);
    ROCLINT_CHECK(statusOf(carried) != core::rewrite::GroundingStatus::Verified);

    // Reopening the database preserves the same incomplete coverage.
    storage::CodebaseIndex reopened(database, workspace);
    const auto reopenedInfo = reopened.snapshotInfo();
    ROCLINT_CHECK(reopenedInfo.ok() && !reopenedInfo.value().complete);
    const auto reopenedShared = ground(reopened, "shared");
    ROCLINT_CHECK(statusOf(reopenedShared) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!reopenedShared.grounding->complete);
    ROCLINT_CHECK(hashOf(reopenedShared).empty());
    const auto reopenedAbsence = ground(reopened, "onlyInB");
    ROCLINT_CHECK(statusOf(reopenedAbsence) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!reopenedAbsence.grounding->complete);

    // Re-processing both requested paths restores coverage without a full scan.
    const auto recovered =
        index.refreshPaths({workspace, {}, true}, {"src/a.hpp", "src/b.hpp"});
    ROCLINT_CHECK(recovered.ok());
    ROCLINT_CHECK(!recovered.value().incomplete);
    ROCLINT_CHECK(recovered.value().unprocessedPaths.empty());
    const auto recoveredInfo = index.snapshotInfo();
    ROCLINT_CHECK(recoveredInfo.ok() && recoveredInfo.value().complete);
    ROCLINT_CHECK(recoveredInfo.value().coverageComplete);
    const auto recoveredShared = ground(index, "shared");
    ROCLINT_CHECK(statusOf(recoveredShared) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(recoveredShared.grounding->complete);
    ROCLINT_CHECK(recoveredShared.grounding->candidates.size() >= 2);
    ROCLINT_CHECK(hashOf(recoveredShared).empty());
    const auto recoveredOnly = ground(index, "onlyInB");
    ROCLINT_CHECK(statusOf(recoveredOnly) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(recoveredOnly.grounding->complete);
    ROCLINT_CHECK(!hashOf(recoveredOnly).empty());

    // Binding another workspace cannot see this workspace's carried rows.
    const auto other = area.workspace("paths-other");
    write(other, "src/other.hpp", "int shared(int value);\n");
    storage::CodebaseIndex otherIndex(area.database("paths-other"), other);
    ROCLINT_CHECK(otherIndex.refresh({other, {}, true}).ok());
    const auto otherShared = ground(otherIndex, "shared");
    ROCLINT_CHECK(statusOf(otherShared) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(otherShared.grounding->filePath == "src/other.hpp");
    ROCLINT_CHECK(statusOf(ground(otherIndex, "carried")) ==
                  core::rewrite::GroundingStatus::NotFoundInScope);
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
    ROCLINT_CHECK(bogus.ok() && bogus.value().symbols.empty());

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

void testAstSnapshotHashLegacyDomain() {
    core::rewrite::CanonicalIntent value;
    value.action = core::rewrite::IntentAction::MODIFY;
    value.target = "count";
    value.property = "name";
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget = "Widget::setCount";
    evidence.filePath = "src/Widget.h";
    evidence.fileHash = std::string(64, 'a');
    evidence.fileSpan.startByte = 10;
    evidence.fileSpan.endByte = 42;
    evidence.line = 12;
    evidence.signature = "bool setCount(int count)";
    value.grounding = evidence;

    // Fixed vector for the proposal-v2 domain: workspace identity, count,
    // qualified target, file path, file hash, byte span and signature.
    const auto hash =
        core::rewrite::AstGroundingEngine::computeAstSnapshotHash({value}, "workspace");
    ROCLINT_CHECK(hash ==
                  "5b4176bdb31c97b71df985417de7323816faf9e6f630285abb0b3df948142d8a");

    // Diagnostic metadata, including completeness, must not enter the hash.
    auto metadata = value;
    metadata.grounding->complete = false;
    metadata.grounding->hashValidated = false;
    metadata.grounding->matchReason = "metadata-only";
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                      {metadata}, "workspace") == hash);

    // Incomplete evidence is never Verified, so it cannot produce a hash.
    auto ambiguous = value;
    ambiguous.grounding->status = core::rewrite::GroundingStatus::Ambiguous;
    ambiguous.grounding->complete = false;
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                      {ambiguous}, "workspace").empty());
}

bool isWellFormedCanonicalIntentXml(const std::string& xml);

void testPromptRenderContextHashesOnlyRenderedSubset() {
    std::vector<core::rewrite::CanonicalIntent> all;
    for (int index = 0; index < 3; ++index) {
        core::rewrite::CanonicalIntent value;
        value.action = core::rewrite::IntentAction::MODIFY;
        value.target = "target" + std::to_string(index);
        value.property = "name";
        core::rewrite::AstGroundingEvidence evidence;
        evidence.status = core::rewrite::GroundingStatus::Verified;
        evidence.qualifiedTarget = "Widget::target" + std::to_string(index);
        evidence.filePath = "src/file" + std::to_string(index) + ".hpp";
        evidence.fileHash = std::string(64, static_cast<char>('a' + index));
        evidence.fileSpan.startByte = static_cast<std::uint32_t>(10 + index * 100);
        evidence.fileSpan.endByte = static_cast<std::uint32_t>(40 + index * 100);
        evidence.line = static_cast<std::uint32_t>(index + 1);
        evidence.signature = std::string(180, static_cast<char>('a' + index));
        value.grounding = evidence;
        all.push_back(std::move(value));
    }

    const auto context = core::rewrite::AstGroundingEngine::renderPromptContext(all, 300);
    ROCLINT_CHECK(context.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(context.xml));
    ROCLINT_CHECK(context.rendered.size() < all.size());
    ROCLINT_CHECK(context.rendered.size() >= 1);
    ROCLINT_CHECK(context.rendered.front().target == "target0");
    for (std::size_t index = 0; index < context.rendered.size(); ++index)
        ROCLINT_CHECK(context.rendered[index].grounding.has_value());
    std::size_t emittedIntents = 0;
    for (std::size_t position = 0;
         (position = context.xml.find("<intent ", position)) != std::string::npos;
         position += std::string("<intent ").size()) {
        ++emittedIntents;
    }
    ROCLINT_CHECK(emittedIntents == context.rendered.size());
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::renderPromptXml(all, 300) == context.xml);

    const auto renderedHash =
        core::rewrite::AstGroundingEngine::computeAstSnapshotHash(context.rendered, "workspace");
    ROCLINT_CHECK(!renderedHash.empty());
    // Evidence omitted to satisfy the byte cap must not enter the proposal hash.
    ROCLINT_CHECK(renderedHash !=
                  core::rewrite::AstGroundingEngine::computeAstSnapshotHash(all, "workspace"));

    // Any change to a rendered file hash, span or signature changes the hash.
    auto changedHash = context.rendered;
    changedHash.front().grounding->fileHash = std::string(64, 'z');
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                      changedHash, "workspace") != renderedHash);
    auto changedSpan = context.rendered;
    changedSpan.front().grounding->fileSpan.endByte += 1;
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                      changedSpan, "workspace") != renderedHash);
    auto changedSignature = context.rendered;
    changedSignature.front().grounding->signature.front() = 'z';
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                      changedSignature, "workspace") != renderedHash);

    // Mutating an omitted intent with same-sized fields cannot change the
    // rendered subset or its hash.
    auto changedOmitted = all;
    changedOmitted.back().grounding->fileHash = std::string(64, 'z');
    changedOmitted.back().grounding->signature = std::string(180, 'z');
    const auto omittedContext =
        core::rewrite::AstGroundingEngine::renderPromptContext(changedOmitted, 300);
    ROCLINT_CHECK(omittedContext.rendered.size() == context.rendered.size());
    ROCLINT_CHECK(omittedContext.rendered == context.rendered);
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                      omittedContext.rendered, "workspace") == renderedHash);

    // When every intent fits, the legacy all-intents hash is preserved.
    std::vector<core::rewrite::CanonicalIntent> shortSet;
    for (const auto* name : {"a", "b"}) {
        core::rewrite::CanonicalIntent value;
        value.action = core::rewrite::IntentAction::MODIFY;
        value.target = name;
        value.property = "name";
        core::rewrite::AstGroundingEvidence evidence;
        evidence.status = core::rewrite::GroundingStatus::Verified;
        evidence.qualifiedTarget = name;
        evidence.filePath = std::string(name) + ".h";
        evidence.fileHash = std::string(64, 'c');
        evidence.fileSpan.startByte = 1;
        evidence.fileSpan.endByte = 2;
        evidence.line = 1;
        evidence.signature = "int value";
        value.grounding = evidence;
        shortSet.push_back(std::move(value));
    }
    const auto shortContext = core::rewrite::AstGroundingEngine::renderPromptContext(shortSet, 300);
    ROCLINT_CHECK(shortContext.rendered.size() == shortSet.size());
    ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                      shortContext.rendered, "workspace") ==
                  core::rewrite::AstGroundingEngine::computeAstSnapshotHash(shortSet, "workspace"));
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

bool isValidUtf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first < 0x80U) {
            ++offset;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if ((first & 0xE0U) == 0xC0U) {
            length = 2;
            codepoint = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            length = 3;
            codepoint = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (offset + length > text.size()) return false;
        for (std::size_t index = 1; index < length; ++index) {
            const auto next = static_cast<unsigned char>(text[offset + index]);
            if ((next & 0xC0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if ((length == 2 && codepoint < 0x80U) || (length == 3 && codepoint < 0x800U) ||
            (length == 4 && codepoint < 0x10000U) || codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        offset += length;
    }
    return true;
}

bool isKnownXmlEntity(std::string_view entity) {
    return entity == "amp" || entity == "lt" || entity == "gt" || entity == "quot" ||
           entity == "apos";
}

// Minimal well-formedness check for the subset emitted by the grounding
// renderer: balanced tags, quoted attributes and only the five predefined XML
// entities.
bool isWellFormedCanonicalIntentXml(const std::string& xml) {
    if (!xml.starts_with("<canonical_intents>") || !xml.ends_with("</canonical_intents>"))
        return false;

    std::vector<std::string> stack;
    std::size_t position = 0;
    while (position < xml.size()) {
        if (xml[position] == '<') {
            const auto end = xml.find('>', position + 1);
            if (end == std::string::npos) return false;
            const std::string token = xml.substr(position + 1, end - position - 1);
            if (token.empty()) return false;

            const bool closing = token.front() == '/';
            const std::size_t nameStart = closing ? 1 : 0;
            std::size_t nameEnd = nameStart;
            while (nameEnd < token.size()) {
                const auto character = static_cast<unsigned char>(token[nameEnd]);
                if (!(std::isalnum(character) || token[nameEnd] == '_' ||
                      token[nameEnd] == '-' || token[nameEnd] == ':' || token[nameEnd] == '.'))
                    break;
                ++nameEnd;
            }
            if (nameEnd == nameStart) return false;
            const std::string name = token.substr(nameStart, nameEnd - nameStart);
            const bool selfClosing = !closing && token.back() == '/';
            const auto attributeEnd = selfClosing ? token.size() - 1 : token.size();

            bool inQuote = false;
            char quote = 0;
            for (std::size_t index = nameEnd; index < attributeEnd; ++index) {
                const char character = token[index];
                if (inQuote) {
                    if (character == quote) {
                        inQuote = false;
                        quote = 0;
                    } else if (character == '<') {
                        return false;
                    } else if (character == '&') {
                        const auto semicolon = token.find(';', index + 1);
                        if (semicolon == std::string::npos || semicolon >= attributeEnd)
                            return false;
                        if (!isKnownXmlEntity(token.substr(index + 1, semicolon - index - 1)))
                            return false;
                        index = semicolon;
                    }
                } else if (character == '"' || character == '\'') {
                    inQuote = true;
                    quote = character;
                } else if (character == '<') {
                    return false;
                } else if (character == '&') {
                    const auto semicolon = token.find(';', index + 1);
                    if (semicolon == std::string::npos || semicolon >= attributeEnd)
                        return false;
                    if (!isKnownXmlEntity(token.substr(index + 1, semicolon - index - 1)))
                        return false;
                    index = semicolon;
                }
            }
            if (inQuote) return false;

            if (closing) {
                if (selfClosing || stack.empty() || stack.back() != name) return false;
                stack.pop_back();
            } else if (!selfClosing) {
                stack.push_back(name);
            }
            position = end + 1;
        } else if (xml[position] == '&') {
            const auto semicolon = xml.find(';', position + 1);
            if (semicolon == std::string::npos) return false;
            if (!isKnownXmlEntity(xml.substr(position + 1, semicolon - position - 1)))
                return false;
            position = semicolon + 1;
        } else {
            const auto character = static_cast<unsigned char>(xml[position]);
            if (character < 0x20U && character != '\n' && character != '\t' &&
                character != '\r')
                return false;
            ++position;
        }
    }
    return stack.empty();
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

void testPromptXmlReviewReproduction() {
    std::string identifier = "veryLongIdentifier_";
    while (identifier.size() < 160) identifier += "segment0123456789";
    identifier.resize(160, 'x');

    std::string filename = "veryLongFileName_";
    while (filename.size() < 160) filename += "component9876543210";
    filename.resize(160, 'y');

    core::rewrite::CanonicalIntent value;
    value.action = core::rewrite::IntentAction::MODIFY;
    value.target = identifier;
    value.property = "name";
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget = identifier;
    evidence.filePath = "src/" + filename + ".hpp";
    evidence.line = 1;
    evidence.signature = "int " + identifier + "(int value)";
    value.grounding = evidence;

    const auto xml = core::rewrite::AstGroundingEngine::renderPromptXml({value});
    ROCLINT_CHECK(xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptTokens);
    ROCLINT_CHECK(isValidUtf8(xml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(xml));
    ROCLINT_CHECK(xml.find("<intent ") != std::string::npos);
    ROCLINT_CHECK(xml.find("status=\"VERIFIED\"") != std::string::npos);
    ROCLINT_CHECK(xml.find("<location ") != std::string::npos);
    ROCLINT_CHECK(xml.find("<signature>") != std::string::npos);
    ROCLINT_CHECK(xml.find("...") != std::string::npos);
    // The compact form keeps both ends of long evidence instead of dropping it.
    ROCLINT_CHECK(xml.find(identifier.substr(0, 24)) != std::string::npos);
    ROCLINT_CHECK(xml.find(identifier.substr(identifier.size() - 20)) != std::string::npos);
    ROCLINT_CHECK(xml.find(filename.substr(filename.size() - 16)) != std::string::npos);
    ROCLINT_CHECK(xml.find(".hpp") != std::string::npos);
}

void testPromptXmlPaddedIdentityCannotBecomeWhitespaceOnly() {
    const std::string target = std::string(50, ' ') + "ActualIdentity" + std::string(50, ' ');
    const auto padded = verifiedIntent(target);
    for (std::size_t length = 1; length < 230; ++length) {
        const auto filler = verifiedIntent(std::string(length, 'f'));
        const auto result = core::rewrite::AstGroundingEngine::renderPromptContext({filler, padded});
        ROCLINT_CHECK(result.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
        ROCLINT_CHECK(isValidUtf8(result.xml));
        ROCLINT_CHECK(isWellFormedCanonicalIntentXml(result.xml));
        const auto targets = emittedTargets(result.xml);
        ROCLINT_CHECK(targets.size() == result.rendered.size());
        for (const auto& emitted : targets) {
            // The synthetic identities contain letters only apart from padding
            // and the explicit truncation marker. At least one letter must
            // survive; whitespace plus an ellipsis cannot identify the target.
            ROCLINT_CHECK(std::any_of(emitted.begin(), emitted.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            }));
        }
        if (result.rendered.size() == 1) {
            ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                              result.rendered, "padded-workspace") ==
                          core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                              {filler}, "padded-workspace"));
        }
    }
}

void testPromptXmlVerifiedIdentityReviewReproduction() {
    const std::string firstTarget(57, 'a');
    const std::vector<core::rewrite::CanonicalIntent> all{
        verifiedIntent(firstTarget, firstTarget, "src/test.hpp",
                       "int " + firstTarget + ";", 1),
        verifiedIntent("SecondImportantTarget", "SecondImportantTarget", "src/test.hpp",
                       "int SecondImportantTarget;", 1),
        verifiedIntent("ThirdImportantTarget", "ThirdImportantTarget", "src/test.hpp",
                       "int ThirdImportantTarget;", 1),
    };

    const auto context = core::rewrite::AstGroundingEngine::renderPromptContext(all);
    ROCLINT_CHECK(context.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(context.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptTokens);
    ROCLINT_CHECK(isValidUtf8(context.xml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(context.xml));
    ROCLINT_CHECK(context.xml.find("target=\"\"") == std::string::npos);

    const auto targets = emittedTargets(context.xml);
    ROCLINT_CHECK(!targets.empty());
    ROCLINT_CHECK(targets.size() == context.rendered.size());
    ROCLINT_CHECK(context.rendered.front().target == firstTarget);
    for (const auto& target : targets) ROCLINT_CHECK(!target.empty());
    for (const auto& rendered : context.rendered) {
        ROCLINT_CHECK(!rendered.target.empty());
        ROCLINT_CHECK(rendered.grounding.has_value());
        ROCLINT_CHECK(rendered.grounding->status == core::rewrite::GroundingStatus::Verified);
    }
    // Every emitted target is either complete or explicitly marked. The review
    // reproduction previously emitted a second Verified record with target="".
    for (const auto& target : targets) {
        const bool complete = target == firstTarget || target == "SecondImportantTarget" ||
                              target == "ThirdImportantTarget";
        if (!complete) ROCLINT_CHECK(target.find("...") != std::string::npos);
    }

    const auto renderedHash = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
        context.rendered, "workspace");
    ROCLINT_CHECK(!renderedHash.empty());
    if (context.rendered.size() < all.size()) {
        ROCLINT_CHECK(renderedHash != core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                                          all, "workspace"));
        // Omitted evidence cannot influence the rendered subset or its hash.
        auto mutated = all;
        for (auto& value : mutated) {
            const bool emitted = std::any_of(
                context.rendered.begin(), context.rendered.end(),
                [&](const auto& rendered) { return rendered.target == value.target; });
            if (!emitted) value.grounding->fileHash = std::string(64, 'z');
        }
        const auto mutatedContext =
            core::rewrite::AstGroundingEngine::renderPromptContext(mutated);
        ROCLINT_CHECK(mutatedContext.rendered == context.rendered);
        ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                          mutatedContext.rendered, "workspace") == renderedHash);
    }
}

void testPromptXmlVerifiedIdentityRequiresTarget() {
    // The target attribute is required identity. A grounded symbol is optional
    // metadata and cannot replace a missing requested target, so no Verified
    // record is emitted.
    {
        const auto missingTarget = verifiedIntent("", "Widget::fallback");
        const auto context =
            core::rewrite::AstGroundingEngine::renderPromptContext({missingTarget});
        ROCLINT_CHECK(context.rendered.empty());
        ROCLINT_CHECK(context.xml.find("<intent ") == std::string::npos);
        ROCLINT_CHECK(context.xml == "<canonical_intents>\n</canonical_intents>");
        ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                          context.rendered, "workspace").empty());
    }
    // Whitespace-only identity is no identity either.
    {
        const auto whitespace = verifiedIntent("   ", "Widget::fallback");
        const auto context =
            core::rewrite::AstGroundingEngine::renderPromptContext({whitespace});
        ROCLINT_CHECK(context.rendered.empty());
        ROCLINT_CHECK(context.xml.find("<intent ") == std::string::npos);
    }
}

void testPromptXmlVerifiedIdentityBudgetBoundaries() {
    const std::string opening = "<canonical_intents>\n";
    const std::string closing = "</canonical_intents>";
    const auto contentBudget =
        core::rewrite::AstGroundingEngine::kMaximumPromptBytes - opening.size() - closing.size();

    const auto shortIntent = verifiedIntent("b");
    const auto single = core::rewrite::AstGroundingEngine::renderPromptContext({shortIntent});
    ROCLINT_CHECK(single.rendered.size() == 1);
    ROCLINT_CHECK(single.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    const auto minimumBlock = single.xml.size() - opening.size() - closing.size();
    ROCLINT_CHECK(minimumBlock > 1);
    const auto fixed = minimumBlock - 1; // The complete target is one byte.

    const auto renderAtRemaining = [&](std::size_t remaining,
                                       const core::rewrite::CanonicalIntent& value) {
        ROCLINT_CHECK(remaining < contentBudget - fixed);
        const auto fillerLength = contentBudget - fixed - remaining;
        const auto filler = verifiedIntent(std::string(fillerLength, 'f'));
        return core::rewrite::AstGroundingEngine::renderPromptContext({filler, value});
    };
    const auto checkContext = [](const core::rewrite::PromptRenderResult& context) {
        ROCLINT_CHECK(context.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
        ROCLINT_CHECK(isValidUtf8(context.xml));
        ROCLINT_CHECK(isWellFormedCanonicalIntentXml(context.xml));
        ROCLINT_CHECK(context.xml.find("target=\"\"") == std::string::npos);
        const auto targets = emittedTargets(context.xml);
        ROCLINT_CHECK(targets.size() == context.rendered.size());
        for (const auto& target : targets) ROCLINT_CHECK(!target.empty());
    };

    // One byte below the minimal action/target/status/tail record, the Verified
    // record is omitted rather than emitted with an empty target.
    {
        const auto context = renderAtRemaining(minimumBlock - 1, shortIntent);
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 1);
        ROCLINT_CHECK(context.xml.find("target=\"b\"") == std::string::npos);
        const auto renderedHash = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
            context.rendered, "workspace");
        ROCLINT_CHECK(renderedHash ==
                      core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                          {context.rendered.front()}, "workspace"));
        // The omitted Verified intent does not enter the rendered hash.
        ROCLINT_CHECK(renderedHash !=
                      core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                          {context.rendered.front(), shortIntent}, "workspace"));
    }
    // At the minimum, the complete one-byte target is valid identity.
    {
        const auto context = renderAtRemaining(minimumBlock, shortIntent);
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 2);
        const auto targets = emittedTargets(context.xml);
        ROCLINT_CHECK(targets.size() == 2);
        ROCLINT_CHECK(targets.back() == "b");
        ROCLINT_CHECK(context.xml.find("...") == std::string::npos);
    }
    // One byte above the minimum keeps the complete target.
    {
        const auto context = renderAtRemaining(minimumBlock + 1, shortIntent);
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 2);
        ROCLINT_CHECK(emittedTargets(context.xml).back() == "b");
    }

    const std::string longTarget = "SecondImportantTarget";
    const auto longIntent = verifiedIntent(longTarget);
    const auto markedMinimum = fixed + 4; // "..." plus one content byte
    const auto fullTargetMinimum = fixed + longTarget.size();

    // Three content bytes cannot carry the marker plus identity: omit.
    {
        const auto context = renderAtRemaining(markedMinimum - 1, longIntent);
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 1);
        const auto targets = emittedTargets(context.xml);
        ROCLINT_CHECK(targets.size() == 1);
        ROCLINT_CHECK(targets.front() != longTarget);
        ROCLINT_CHECK(context.xml.find("...") == std::string::npos);
    }
    // Four content bytes are enough for an explicit marked truncation.
    {
        const auto context = renderAtRemaining(markedMinimum, longIntent);
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 2);
        const auto targets = emittedTargets(context.xml);
        ROCLINT_CHECK(targets.size() == 2);
        ROCLINT_CHECK(targets.back() != longTarget);
        ROCLINT_CHECK(targets.back().find("...") != std::string::npos);
        ROCLINT_CHECK(targets.back().size() > 3);
    }
    {
        const auto context = renderAtRemaining(markedMinimum + 1, longIntent);
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 2);
        const auto emitted = emittedTargets(context.xml).back();
        ROCLINT_CHECK(emitted != longTarget);
        ROCLINT_CHECK(emitted.find("...") != std::string::npos);
    }
    // At the complete-target minimum, no truncation marker is emitted.
    {
        const auto context = renderAtRemaining(fullTargetMinimum, longIntent);
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 2);
        ROCLINT_CHECK(emittedTargets(context.xml).back() == longTarget);
        ROCLINT_CHECK(context.xml.find("...") == std::string::npos);
    }

    // An omitted long-target record consumes no budget, so a later short
    // Verified record can still use the remaining bytes.
    {
        const auto fillerLength = contentBudget - fixed - (fixed + 3);
        const auto filler = verifiedIntent(std::string(fillerLength, 'f'));
        const auto context = core::rewrite::AstGroundingEngine::renderPromptContext(
            {filler, longIntent, shortIntent});
        checkContext(context);
        ROCLINT_CHECK(context.rendered.size() == 2);
        const auto targets = emittedTargets(context.xml);
        ROCLINT_CHECK(targets.size() == 2);
        ROCLINT_CHECK(targets.back() == "b");
    }

    // Sweep the whole boundary region: a Verified target is either complete or
    // explicitly marked, never an empty or unmarked partial identifier.
    for (std::size_t remaining = fixed; remaining <= fullTargetMinimum + 1; ++remaining) {
        const auto context = renderAtRemaining(remaining, longIntent);
        checkContext(context);
        const auto targets = emittedTargets(context.xml);
        if (context.rendered.size() == 1) {
            ROCLINT_CHECK(targets.size() == 1);
            ROCLINT_CHECK(targets.front() != longTarget);
        } else {
            ROCLINT_CHECK(context.rendered.size() == 2);
            ROCLINT_CHECK(targets.size() == 2);
            ROCLINT_CHECK(!targets.back().empty());
            if (targets.back() != longTarget)
                ROCLINT_CHECK(targets.back().find("...") != std::string::npos);
        }
    }
}

void testPromptXmlVerifiedAndAmbiguousIdentity() {
    std::vector<core::rewrite::CanonicalIntent> all;
    all.push_back(verifiedIntent("verifiedOne", "Widget::verifiedOne", "src/one.hpp",
                                 std::string(120, 'a'), 1));
    all.push_back(verifiedIntent("verifiedTwo", "Widget::verifiedTwo", "src/two.hpp",
                                 std::string(120, 'b'), 2));

    for (const auto* name : {"ambiguousOne", "ambiguousTwo"}) {
        auto value = intent(name);
        core::rewrite::AstGroundingEvidence evidence;
        evidence.status = core::rewrite::GroundingStatus::Ambiguous;
        evidence.complete = false;
        evidence.qualifiedTarget = std::string("Engine::") + name;
        evidence.filePath = "src/ambiguous.hpp";
        evidence.line = 3;
        for (int index = 0; index < 8; ++index) {
            evidence.candidates.push_back(
                std::string("Engine::") + name + " @ src/very/long/path/" +
                std::string(40, static_cast<char>('a' + index)) + ".hpp:10:3");
        }
        value.grounding = std::move(evidence);
        all.push_back(std::move(value));
    }

    const auto context = core::rewrite::AstGroundingEngine::renderPromptContext(all);
    ROCLINT_CHECK(context.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(isValidUtf8(context.xml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(context.xml));
    ROCLINT_CHECK(context.xml.find("target=\"\"") == std::string::npos);

    const auto targets = emittedTargets(context.xml);
    ROCLINT_CHECK(targets.size() == context.rendered.size());
    ROCLINT_CHECK(!context.rendered.empty());
    for (const auto& target : targets) ROCLINT_CHECK(!target.empty());

    bool sawNonVerified = false;
    for (const auto& rendered : context.rendered) {
        ROCLINT_CHECK(rendered.grounding.has_value());
        const bool verified =
            rendered.grounding->status == core::rewrite::GroundingStatus::Verified;
        if (verified) {
            ROCLINT_CHECK(!sawNonVerified);
        } else {
            sawNonVerified = true;
        }
    }

    const auto renderedHash = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
        context.rendered, "workspace");
    ROCLINT_CHECK(!renderedHash.empty());
    if (context.rendered.size() < all.size()) {
        auto mutated = all;
        bool omittedVerified = false;
        for (auto& value : mutated) {
            const bool emitted = std::any_of(
                context.rendered.begin(), context.rendered.end(),
                [&](const auto& rendered) { return rendered.target == value.target; });
            if (emitted) continue;
            if (value.grounding->status == core::rewrite::GroundingStatus::Verified)
                omittedVerified = true;
            value.grounding->fileHash = std::string(64, 'z');
        }
        // The AST hash covers Verified evidence only, so its rendered subset can
        // differ from the full set only when a Verified record was omitted.
        if (omittedVerified) {
            ROCLINT_CHECK(renderedHash !=
                          core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                              all, "workspace"));
        }
        const auto mutatedContext =
            core::rewrite::AstGroundingEngine::renderPromptContext(mutated);
        ROCLINT_CHECK(mutatedContext.rendered == context.rendered);
        ROCLINT_CHECK(core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                          mutatedContext.rendered, "workspace") == renderedHash);
    }
}

void testPromptXmlVerifiedLongUnicodeIdentity() {
    std::string identifier;
    for (int index = 0; index < 40; ++index) identifier += "\xE5\x87\xBD\xE6\x95\xB0";
    const std::string qualified = "Namespace::caf\xC3\xA9::" + identifier;
    const std::string path =
        "src/\xE8\xB7\xAF\xE5\xBE\x84/" + std::string(60, 'p') + "&<>\"'.hpp";
    const std::string signature =
        "bool \xE5\x87\xBD\xE6\x95\xB0(const std::string& value) const; // <tag> & \"quoted\" 'apos'";
    const auto value = verifiedIntent(identifier, qualified, path, signature, 9);

    const auto context = core::rewrite::AstGroundingEngine::renderPromptContext({value});
    ROCLINT_CHECK(context.rendered.size() == 1);
    ROCLINT_CHECK(context.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(context.xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptTokens);
    ROCLINT_CHECK(isValidUtf8(context.xml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(context.xml));
    ROCLINT_CHECK(context.xml.find("target=\"\"") == std::string::npos);
    const auto targets = emittedTargets(context.xml);
    ROCLINT_CHECK(targets.size() == 1);
    ROCLINT_CHECK(!targets.front().empty());
    if (targets.front() != identifier)
        ROCLINT_CHECK(targets.front().find("...") != std::string::npos);
    ROCLINT_CHECK(context.xml.find("...") != std::string::npos);
}

void testPromptXmlUnicodeEscapingAndBoundaries() {
    const std::string unicodeTarget = "caf\xC3\xA9_" "\xE5\x87\xBD\xE6\x95\xB0";
    const std::string unicodePath = "src/na\xC3\xAFve_" "\xE8\xB7\xAF\xE5\xBE\x84.hpp";

    core::rewrite::CanonicalIntent value;
    value.action = core::rewrite::IntentAction::MODIFY;
    value.target = unicodeTarget;
    value.property = "name";
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget = unicodeTarget;
    evidence.filePath = unicodePath;
    evidence.line = 7;
    evidence.signature =
        "bool operator()(const std::string& value, int count) const; // <tag> & \"quoted\" 'apos'";
    value.grounding = evidence;

    const auto xml = core::rewrite::AstGroundingEngine::renderPromptXml({value});
    ROCLINT_CHECK(xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(isValidUtf8(xml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(xml));
    ROCLINT_CHECK(xml.find("caf\xC3\xA9") != std::string::npos);
    ROCLINT_CHECK(xml.find("&amp;") != std::string::npos);
    ROCLINT_CHECK(xml.find("&lt;") != std::string::npos);
    ROCLINT_CHECK(xml.find("&gt;") != std::string::npos);
    ROCLINT_CHECK(xml.find("&quot;") != std::string::npos);
    ROCLINT_CHECK(xml.find("&apos;") != std::string::npos);

    // Force middle truncation through multi-byte code points. The validator
    // rejects a split sequence, a broken entity or unbalanced XML.
    std::string longUnicode;
    for (int index = 0; index < 120; ++index)
        longUnicode += "\xE5\x87\xBD\xE6\x95\xB0";
    auto wide = value;
    wide.target = longUnicode;
    wide.grounding->qualifiedTarget = longUnicode;
    wide.grounding->filePath = "src/" + longUnicode + ".hpp";
    wide.grounding->signature = "int " + longUnicode + "();";
    const auto wideXml = core::rewrite::AstGroundingEngine::renderPromptXml({wide});
    ROCLINT_CHECK(wideXml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(isValidUtf8(wideXml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(wideXml));
    ROCLINT_CHECK(wideXml.find("...") != std::string::npos);

    // Force truncation through a dense run of XML entities so no `&amp;`,
    // `&lt;`, `&gt;`, `&quot;` or `&apos;` can be split.
    std::string heavyPunctuation;
    for (int index = 0; index < 80; ++index) heavyPunctuation += "&<>\"'";
    auto heavy = value;
    heavy.grounding->signature = heavyPunctuation;
    const auto heavyXml = core::rewrite::AstGroundingEngine::renderPromptXml({heavy});
    ROCLINT_CHECK(heavyXml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(isValidUtf8(heavyXml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(heavyXml));
    ROCLINT_CHECK(heavyXml.find("...") != std::string::npos);
}

void testPromptXmlSignatureBudgetZeroAndTiny() {
    core::rewrite::CanonicalIntent value;
    value.action = core::rewrite::IntentAction::MODIFY;
    value.target = "caf\xC3\xA9";
    value.property = "name";
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget = "Widget::caf\xC3\xA9";
    evidence.filePath = "src/Widget.h";
    evidence.line = 3;
    evidence.signature = std::string(200, 's');
    value.grounding = evidence;

    for (const std::size_t budget : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                                     std::size_t{3}, std::size_t{4}, std::size_t{8},
                                     std::size_t{16}}) {
        const auto xml = core::rewrite::AstGroundingEngine::renderPromptXml({value}, budget);
        ROCLINT_CHECK(xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
        ROCLINT_CHECK(isValidUtf8(xml));
        ROCLINT_CHECK(isWellFormedCanonicalIntentXml(xml));
        std::size_t signatureBytes = 0;
        for (const auto& block : signatureBlocks(xml)) signatureBytes += block.size();
        ROCLINT_CHECK(signatureBytes <= budget);
        if (budget == 0) ROCLINT_CHECK(signatureBlocks(xml).empty());
    }

    // A caller-supplied oversized signature allowance cannot widen the total.
    const auto oversized =
        core::rewrite::AstGroundingEngine::renderPromptXml({value}, 1000000);
    ROCLINT_CHECK(oversized.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(oversized));
}

void testPromptXmlMultipleAmbiguityCandidates() {
    core::rewrite::CanonicalIntent value;
    value.action = core::rewrite::IntentAction::MODIFY;
    value.target = "lookup";
    value.property = "name";
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Ambiguous;
    evidence.complete = false;
    evidence.qualifiedTarget = "Engine::lookup";
    for (int index = 0; index < 10; ++index) {
        evidence.candidates.push_back("Engine::lookup @ src/very/long/path/directory_" +
                                      std::to_string(index) +
                                      "/engine_implementation_file_" + std::to_string(index) +
                                      ".hpp:10:3");
    }
    value.grounding = evidence;

    const auto xml = core::rewrite::AstGroundingEngine::renderPromptXml({value});
    ROCLINT_CHECK(xml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(isValidUtf8(xml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(xml));
    ROCLINT_CHECK(xml.find("complete=\"false\"") != std::string::npos);

    std::size_t candidateCount = 0;
    for (std::size_t position = 0;
         (position = xml.find("<candidate>", position)) != std::string::npos;
         position += std::string("<candidate>").size()) {
        ++candidateCount;
    }
    ROCLINT_CHECK(candidateCount >= 2);
    ROCLINT_CHECK(xml.find("<candidate>Engine") != std::string::npos);
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

// ---------------------------------------------------------------------------
// Corpus-driven acceptance gate.
// ---------------------------------------------------------------------------

void failCorpus(const char* message) {
    std::cerr << "[CHECK FAILED] corpus: " << message << "\n";
    std::exit(EXIT_FAILURE);
}

const char* corpusStatusName(core::rewrite::GroundingStatus status) {
    switch (status) {
        case core::rewrite::GroundingStatus::Verified: return "Verified";
        case core::rewrite::GroundingStatus::Ambiguous: return "Ambiguous";
        case core::rewrite::GroundingStatus::NotFoundInScope: return "NotFoundInScope";
        case core::rewrite::GroundingStatus::TypeMismatch: return "TypeMismatch";
        case core::rewrite::GroundingStatus::Ungrounded: return "Ungrounded";
    }
    return "Ungrounded";
}

core::rewrite::GroundingStatus parseCorpusStatus(std::string_view text) {
    if (text == "Verified") return core::rewrite::GroundingStatus::Verified;
    if (text == "Ambiguous") return core::rewrite::GroundingStatus::Ambiguous;
    if (text == "NotFoundInScope") return core::rewrite::GroundingStatus::NotFoundInScope;
    if (text == "TypeMismatch") return core::rewrite::GroundingStatus::TypeMismatch;
    if (text == "Ungrounded") return core::rewrite::GroundingStatus::Ungrounded;
    failCorpus("unknown grounding status");
    return core::rewrite::GroundingStatus::Ungrounded;
}

core::rewrite::IntentAction parseCorpusAction(std::string_view text) {
    if (text == "ADD") return core::rewrite::IntentAction::ADD;
    if (text == "REMOVE") return core::rewrite::IntentAction::REMOVE;
    if (text == "MODIFY") return core::rewrite::IntentAction::MODIFY;
    if (text == "CONSTRAIN") return core::rewrite::IntentAction::CONSTRAIN;
    if (text == "PRESERVE") return core::rewrite::IntentAction::PRESERVE;
    failCorpus("unknown intent action");
    return core::rewrite::IntentAction::MODIFY;
}

std::string requiredString(const core::json::Value& object, std::string_view key) {
    const auto* value = object.find(key);
    ROCLINT_CHECK(value != nullptr && value->isString());
    return value->asString();
}

std::string optionalString(const core::json::Value& object, std::string_view key,
                           std::string fallback = {}) {
    const auto* value = object.find(key);
    if (value == nullptr) return fallback;
    ROCLINT_CHECK(value->isString());
    return value->asString();
}

bool optionalBool(const core::json::Value& object, std::string_view key, bool fallback) {
    const auto* value = object.find(key);
    if (value == nullptr) return fallback;
    ROCLINT_CHECK(value->isBool());
    return value->asBool();
}

double optionalNumber(const core::json::Value& object, std::string_view key, double fallback) {
    const auto* value = object.find(key);
    if (value == nullptr) return fallback;
    ROCLINT_CHECK(value->isNumber());
    return value->asNumber();
}

std::map<std::string, std::string> stringMap(const core::json::Value& object, std::string_view key,
                                             bool required) {
    std::map<std::string, std::string> result;
    const auto* value = object.find(key);
    if (value == nullptr) {
        ROCLINT_CHECK(!required);
        return result;
    }
    ROCLINT_CHECK(value->isObject());
    for (const auto& member : value->asObject()) {
        ROCLINT_CHECK(member.second.isString());
        result.emplace(member.first, member.second.asString());
    }
    return result;
}

struct CorpusExpected {
    core::rewrite::GroundingStatus status{core::rewrite::GroundingStatus::Ungrounded};
    std::string target{};
    std::string file{};
    std::string signatureContains{};
    std::string signatureKey{};
    std::string matchReason{};
    bool complete{true};
    bool hashValidated{true};
    bool hasValidateFileHash{false};
    bool validateFileHash{false};
    bool hasHostPromotes{false};
    bool hostPromotes{true};
    std::size_t minCandidates{0};
    bool candidatesDistinct{false};
};

struct CorpusCase {
    std::string id{};
    std::string category{};
    std::string note{};
    std::string activeFile{};
    std::map<std::string, std::string> files{};
    std::map<std::string, std::string> mutate{};
    core::rewrite::IntentAction action{core::rewrite::IntentAction::MODIFY};
    std::string target{};
    std::string property{};
    std::string value{};
    std::string condition{};
    CorpusExpected expected{};
};

CorpusCase parseCorpusCase(const core::json::Value& value) {
    CorpusCase result;
    result.id = requiredString(value, "id");
    result.category = requiredString(value, "category");
    result.note = optionalString(value, "note");
    result.activeFile = optionalString(value, "activeFile");
    result.files = stringMap(value, "files", true);
    result.mutate = stringMap(value, "mutate", false);

    const auto* intentValue = value.find("intent");
    ROCLINT_CHECK(intentValue != nullptr && intentValue->isObject());
    result.action = parseCorpusAction(requiredString(*intentValue, "action"));
    result.target = requiredString(*intentValue, "target");
    result.property = optionalString(*intentValue, "property", "name");
    result.value = optionalString(*intentValue, "value", "changed");
    result.condition = optionalString(*intentValue, "condition");

    const auto* expectedValue = value.find("expected");
    ROCLINT_CHECK(expectedValue != nullptr && expectedValue->isObject());
    result.expected.status = parseCorpusStatus(requiredString(*expectedValue, "status"));
    result.expected.target = optionalString(*expectedValue, "target");
    result.expected.file = optionalString(*expectedValue, "file");
    result.expected.signatureContains = optionalString(*expectedValue, "signatureContains");
    result.expected.signatureKey = optionalString(*expectedValue, "signatureKey");
    result.expected.matchReason = optionalString(*expectedValue, "matchReason");
    result.expected.complete = optionalBool(*expectedValue, "complete", true);
    result.expected.hashValidated = optionalBool(*expectedValue, "hashValidatedDefault", true);
    result.expected.hasValidateFileHash = expectedValue->find("validateFileHash") != nullptr;
    result.expected.validateFileHash = optionalBool(*expectedValue, "validateFileHash", false);
    result.expected.hasHostPromotes = expectedValue->find("hostPromotes") != nullptr;
    result.expected.hostPromotes = optionalBool(*expectedValue, "hostPromotes", true);
    result.expected.minCandidates =
        static_cast<std::size_t>(optionalNumber(*expectedValue, "minCandidates", 0.0));
    result.expected.candidatesDistinct = optionalBool(*expectedValue, "candidatesDistinct", false);
    return result;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    ROCLINT_CHECK(stream.is_open());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

fs::path corpusPath() {
#ifdef ROCLINT_AST_GROUNDING_CORPUS
    const fs::path compiled{ROCLINT_AST_GROUNDING_CORPUS};
    if (fs::exists(compiled)) return compiled;
#endif
    const auto sourceRelative = fs::path(__FILE__).parent_path() / "ast_grounding_corpus.json";
    if (fs::exists(sourceRelative)) return sourceRelative;

    const auto workingDirectory = fs::current_path() / "ast_grounding_corpus.json";
    if (fs::exists(workingDirectory)) return workingDirectory;

    fs::path cursor = fs::current_path();
    for (int depth = 0; depth < 5 && !cursor.empty(); ++depth) {
        const auto candidate = cursor / "tests" / "storage" / "ast_grounding_corpus.json";
        if (fs::exists(candidate)) return candidate;
        cursor = cursor.parent_path();
    }
    failCorpus("ast_grounding_corpus.json not found");
    return {};
}

std::string compactTestSyntax(std::string_view text) {
    std::string compact;
    compact.reserve(text.size());
    for (const char character : text) {
        if (!std::isspace(static_cast<unsigned char>(character))) compact.push_back(character);
    }
    return compact;
}

void assertEvidenceBinding(const storage::CodebaseIndex& index,
                           const core::rewrite::AstGroundingEvidence& evidence,
                           const CorpusExpected& expected) {
    if (evidence.fileHash.empty()) return;
    const auto symbols = index.findQualified(evidence.qualifiedTarget);
    ROCLINT_CHECK(symbols.ok());
    bool bound = false;
    for (const auto& symbol : symbols.value().symbols) {
        if (symbol.filePath == evidence.filePath && symbol.fileHash == evidence.fileHash &&
            symbol.span.startByte == evidence.fileSpan.startByte &&
            symbol.span.endByte == evidence.fileSpan.endByte) {
            bound = true;
            ROCLINT_CHECK(evidence.signature == symbol.signature);
            ROCLINT_CHECK(evidence.line == symbol.startLine);
            if (!expected.signatureKey.empty()) {
                ROCLINT_CHECK(compactTestSyntax(symbol.signatureKey) ==
                              compactTestSyntax(expected.signatureKey));
            }
            break;
        }
    }
    ROCLINT_CHECK(bound);
}

void testAstGroundingCorpus() {
    const auto parsed =
        core::json::parse(readTextFile(corpusPath()), core::json::ParseLimits{256 * 1024, 32});
    ROCLINT_CHECK(parsed.ok());
    const auto& root = parsed.value();

    const auto* casesValue = root.find("cases");
    ROCLINT_CHECK(casesValue != nullptr && casesValue->isArray());

    std::size_t total = 0;
    std::size_t correct = 0;
    std::size_t statusMismatches = 0;
    std::size_t wrongVerified = 0;
    std::size_t ambiguousExpected = 0;
    std::size_t ambiguousPreserved = 0;
    std::map<std::string, std::size_t> categories;
    std::map<std::string, std::size_t> actualStatuses;
    double groundingMs = 0.0;

    const auto started = std::chrono::steady_clock::now();
    for (const auto& caseValue : casesValue->asArray()) {
        ROCLINT_CHECK(caseValue.isObject());
        const auto corpusCase = parseCorpusCase(caseValue);

        TempArea area;
        const auto workspace = area.workspace(corpusCase.id);
        for (const auto& file : corpusCase.files) write(workspace, file.first, file.second);

        storage::CodebaseIndex index(area.database(corpusCase.id), workspace);
        ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

        // Mutate after indexing to model a host file change that has not reached
        // the AST index yet.
        for (const auto& file : corpusCase.mutate) write(workspace, file.first, file.second);

        core::rewrite::CanonicalIntent canonical;
        canonical.action = corpusCase.action;
        canonical.target = corpusCase.target;
        canonical.property = corpusCase.property;
        canonical.value = corpusCase.value;
        canonical.condition = corpusCase.condition;
        canonical.disposition = "corpus";
        canonical.nodeId = "nid_corpus_" + corpusCase.id;

        const auto groundingStarted = std::chrono::steady_clock::now();
        const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(
            {canonical}, corpusCase.activeFile, index);
        groundingMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - groundingStarted).count();
        ROCLINT_CHECK(grounded.size() == 1);
        ROCLINT_CHECK(grounded.front().grounding.has_value());
        const auto& evidence = *grounded.front().grounding;

        ++total;
        ++categories[corpusCase.category];
        ++actualStatuses[corpusStatusName(evidence.status)];
        const bool ambiguityCase =
            corpusCase.expected.status == core::rewrite::GroundingStatus::Ambiguous &&
            corpusCase.category != "unsupported";
        if (ambiguityCase) ++ambiguousExpected;

        if (evidence.status != corpusCase.expected.status) {
            ++statusMismatches;
            if (evidence.status == core::rewrite::GroundingStatus::Verified &&
                corpusCase.expected.status != core::rewrite::GroundingStatus::Verified) {
                ++wrongVerified;
            }
            std::cerr << "  [CORPUS] " << corpusCase.id
                      << " expected=" << corpusStatusName(corpusCase.expected.status)
                      << " actual=" << corpusStatusName(evidence.status) << "\n";
            continue;
        }

        ROCLINT_CHECK(evidence.complete == corpusCase.expected.complete);
        ROCLINT_CHECK(evidence.hashValidated == corpusCase.expected.hashValidated);
        if (!corpusCase.expected.target.empty())
            ROCLINT_CHECK(evidence.qualifiedTarget == corpusCase.expected.target);
        if (!corpusCase.expected.file.empty())
            ROCLINT_CHECK(evidence.filePath == corpusCase.expected.file);
        if (!corpusCase.expected.signatureContains.empty()) {
            ROCLINT_CHECK(evidence.signature.find(corpusCase.expected.signatureContains) !=
                          std::string::npos);
        }
        ROCLINT_CHECK(evidence.matchReason == corpusCase.expected.matchReason);

        if (corpusCase.expected.minCandidates > 0)
            ROCLINT_CHECK(evidence.candidates.size() >= corpusCase.expected.minCandidates);
        if (corpusCase.expected.candidatesDistinct) {
            const std::set<std::string> distinct(evidence.candidates.begin(),
                                                 evidence.candidates.end());
            ROCLINT_CHECK(distinct.size() == evidence.candidates.size());
            ROCLINT_CHECK(distinct.size() >= 2);
        }

        if (evidence.status == core::rewrite::GroundingStatus::Verified) {
            ROCLINT_CHECK(!evidence.fileHash.empty());
            ROCLINT_CHECK(evidence.fileSpan.endByte > evidence.fileSpan.startByte);
        }
        if (!evidence.filePath.empty()) ROCLINT_CHECK(!evidence.fileHash.empty());
        assertEvidenceBinding(index, evidence, corpusCase.expected);

        if (evidence.status == core::rewrite::GroundingStatus::Verified &&
            !corpusCase.expected.hasValidateFileHash) {
            const auto hash = index.validateFileHash(evidence.filePath, evidence.fileHash);
            ROCLINT_CHECK(hash.ok());
            ROCLINT_CHECK(hash.value());
        }
        if (corpusCase.expected.hasValidateFileHash) {
            const auto hash = index.validateFileHash(evidence.filePath, evidence.fileHash);
            ROCLINT_CHECK(hash.ok());
            ROCLINT_CHECK(hash.value() == corpusCase.expected.validateFileHash);
            const bool hostPromotes = evidence.status == core::rewrite::GroundingStatus::Verified &&
                                      evidence.complete && evidence.hashValidated && hash.value();
            if (corpusCase.expected.hasHostPromotes)
                ROCLINT_CHECK(hostPromotes == corpusCase.expected.hostPromotes);
            if (!hash.value()) {
                // The coordinator's host applies the same rule: a failed byte-hash
                // check sets hashValidated=false and withholds current evidence.
                auto hostView = evidence;
                hostView.hashValidated = false;
                ROCLINT_CHECK(!hostView.hashValidated);
                ROCLINT_CHECK(!(hostView == evidence));
            }
        }

        // New evidence metadata is diagnostic only and must never enter the
        // proposal v2 AST snapshot hash.
        const auto hashBefore = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
            grounded, "corpus-" + corpusCase.id);
        auto metadataCopy = grounded;
        metadataCopy.front().grounding->complete = !metadataCopy.front().grounding->complete;
        metadataCopy.front().grounding->hashValidated = false;
        metadataCopy.front().grounding->matchReason = "metadata-only";
        const auto hashAfter = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
            metadataCopy, "corpus-" + corpusCase.id);
        ROCLINT_CHECK(hashBefore == hashAfter);

        if (ambiguityCase && evidence.status == core::rewrite::GroundingStatus::Ambiguous)
            ++ambiguousPreserved;
        ++correct;
    }
    const auto elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    const auto coverage = total == 0 ? 0.0 : static_cast<double>(correct) / static_cast<double>(total);

    const auto unsupported = categories["unsupported"];
    const auto missing = categories["missing"];
    std::cout << "  [INFO] ast corpus categories";
    for (const auto& category : categories)
        std::cout << " " << category.first << "=" << category.second;
    std::cout << "\n";
    std::cout << "  [INFO] ast corpus total=" << total << " correct=" << correct
              << " coverage=" << coverage << " wrong_verified=" << wrongVerified
              << " status_mismatches=" << statusMismatches
              << " ambiguous=" << ambiguousPreserved << "/" << ambiguousExpected
              << " unsupported=" << unsupported << " missing=" << missing
              << " grounding_ms=" << groundingMs << " elapsed_ms=" << elapsedMs << "\n";

    const auto* metricsValue = root.find("expectedMetrics");
    ROCLINT_CHECK(metricsValue != nullptr && metricsValue->isObject());
    const auto* categoryCounts = metricsValue->find("categoryCounts");
    ROCLINT_CHECK(categoryCounts != nullptr && categoryCounts->isObject());
    for (const auto& member : categoryCounts->asObject()) {
        ROCLINT_CHECK(member.second.isNumber());
        ROCLINT_CHECK(categories[member.first] ==
                      static_cast<std::size_t>(member.second.asNumber()));
    }
    ROCLINT_CHECK(categories.size() == categoryCounts->asObject().size());

    const auto* statusCounts = metricsValue->find("expectedStatusCounts");
    ROCLINT_CHECK(statusCounts != nullptr && statusCounts->isObject());
    for (const auto& member : statusCounts->asObject()) {
        ROCLINT_CHECK(member.second.isNumber());
        ROCLINT_CHECK(actualStatuses[member.first] ==
                      static_cast<std::size_t>(member.second.asNumber()));
    }
    ROCLINT_CHECK(actualStatuses.size() == statusCounts->asObject().size());

    const auto expectedTotal =
        static_cast<std::size_t>(optionalNumber(*metricsValue, "totalCases", 0.0));
    const auto expectedCorrect =
        static_cast<std::size_t>(optionalNumber(*metricsValue, "expectedCorrectResolutions", 0.0));
    const auto expectedWrongVerified =
        static_cast<std::size_t>(optionalNumber(*metricsValue, "expectedWrongVerified", 0.0));
    const auto expectedCoverage = optionalNumber(*metricsValue, "coverage", 0.0);

    ROCLINT_CHECK(total == expectedTotal);
    ROCLINT_CHECK(correct == expectedCorrect);
    ROCLINT_CHECK(wrongVerified == expectedWrongVerified);
    ROCLINT_CHECK(std::abs(coverage - expectedCoverage) < 1e-9);
    ROCLINT_CHECK(ambiguousPreserved == ambiguousExpected);
    ROCLINT_CHECK(statusMismatches == 0);
    ROCLINT_CHECK(wrongVerified == 0);
    ROCLINT_CHECK(correct == total);
    ROCLINT_CHECK(elapsedMs < 30000.0);
}

void testEvidenceMetadataDefaultsAndEquality() {
    core::rewrite::AstGroundingEvidence evidence;
    ROCLINT_CHECK(evidence.complete);
    ROCLINT_CHECK(evidence.hashValidated);
    ROCLINT_CHECK(evidence.matchReason.empty());

    auto other = evidence;
    other.matchReason = "file";
    ROCLINT_CHECK(!(other == evidence));
    other = evidence;
    other.complete = false;
    ROCLINT_CHECK(!(other == evidence));
    other = evidence;
    other.hashValidated = false;
    ROCLINT_CHECK(!(other == evidence));
}

void testTruncatedQueriesCannotProve() {
    TempArea area;
    const auto workspace = area.workspace("truncated");
    for (int index = 0; index < 65; ++index) {
        std::string source = "class Engine { public: int member" + std::to_string(index) + ";";
        if (index == 0) source += " int onlyTarget;";
        source += " };\n";
        source += "class Dup" + std::to_string(index) + " { public: int duplicate; };\n";
        write(workspace, "src/fixture_" + std::to_string(index) + ".hpp", source);
    }
    storage::CodebaseIndex index(area.database("truncated"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto engines = index.findQualified("Engine");
    ROCLINT_CHECK(engines.ok());
    ROCLINT_CHECK(engines.value().truncated);
    ROCLINT_CHECK(engines.value().symbols.size() == 64);

    const auto duplicates = index.findByName("duplicate", {}, {}, 64, false);
    ROCLINT_CHECK(duplicates.ok());
    ROCLINT_CHECK(duplicates.value().truncated);
    ROCLINT_CHECK(duplicates.value().symbols.size() == 64);

    // A truncated scope query may hide the only matching scope. One candidate is
    // not proof of uniqueness, and no candidate is not proof of absence.
    const auto scoped = ground(index, "onlyTarget", "in Engine");
    ROCLINT_CHECK(statusOf(scoped) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!scoped.grounding->complete);
    ROCLINT_CHECK(scoped.grounding->candidates.size() == 1);
    ROCLINT_CHECK(statusOf(scoped) != core::rewrite::GroundingStatus::Verified);

    const auto absent = ground(index, "absent", "in Engine");
    ROCLINT_CHECK(statusOf(absent) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!absent.grounding->complete);
    ROCLINT_CHECK(statusOf(absent) != core::rewrite::GroundingStatus::NotFoundInScope);

    // Focus is a tie-breaker only: a truncated name query can never promote the
    // active-file candidate to Verified.
    const auto focused = ground(index, "duplicate", {}, "src/fixture_0.hpp");
    ROCLINT_CHECK(statusOf(focused) == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(!focused.grounding->complete);
    ROCLINT_CHECK(statusOf(focused) != core::rewrite::GroundingStatus::Verified);
}

void testExplicitConstraintsBeatFocus() {
    TempArea area;
    const auto workspace = area.workspace("constraints");
    write(workspace, "src/session.hpp", "class SessionManager { public: bool send(int packet); };\n");
    write(workspace, "src/log.hpp", "class LogClient { public: bool send(int packet); };\n");
    write(workspace, "src/widget_a.hpp", "class Widget { public: int value; };\n");
    write(workspace, "src/widget_b.hpp", "class Widget { public: int value; };\n");
    write(workspace, "src/engine_a.hpp", "class Engine { public: bool run(double second); };\n");
    write(workspace, "src/engine_b.hpp", "class Engine { public: bool run(int first); };\n");
    write(workspace, "src/service_a.hpp", "class Service { public: bool submit(int count); };\n");
    write(workspace, "src/service_b.hpp",
          "class Service { public: bool submit(int count, double limit); };\n");
    write(workspace, "src/pair.hpp",
          "class Pair { public: bool run(int first); bool run(int first, double second); };\n");
    storage::CodebaseIndex index(area.database("constraints"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto focused = ground(index, "send", {}, "src/log.hpp");
    ROCLINT_CHECK(statusOf(focused) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(focused.grounding->qualifiedTarget == "LogClient::send");
    ROCLINT_CHECK(focused.grounding->matchReason == "active-file");

    const auto scoped = ground(index, "send", "in SessionManager", "src/log.hpp");
    ROCLINT_CHECK(statusOf(scoped) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(scoped.grounding->qualifiedTarget == "SessionManager::send");
    ROCLINT_CHECK(scoped.grounding->matchReason == "scope");

    const auto within = ground(index, "send", "within SessionManager", "src/log.hpp");
    ROCLINT_CHECK(statusOf(within) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(within.grounding->qualifiedTarget == "SessionManager::send");
    const auto inside = ground(index, "send", "inside SessionManager", "src/log.hpp");
    ROCLINT_CHECK(statusOf(inside) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(inside.grounding->qualifiedTarget == "SessionManager::send");
    const auto scopedFile =
        ground(index, "send", "in SessionManager file src/session.hpp", "src/log.hpp");
    ROCLINT_CHECK(statusOf(scopedFile) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(scopedFile.grounding->qualifiedTarget == "SessionManager::send");
    ROCLINT_CHECK(scopedFile.grounding->matchReason == "file");

    const auto filed = ground(index, "value", "file src/widget_a.hpp", "src/widget_b.hpp");
    ROCLINT_CHECK(statusOf(filed) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(filed.grounding->qualifiedTarget == "Widget::value");
    ROCLINT_CHECK(filed.grounding->filePath == "src/widget_a.hpp");
    ROCLINT_CHECK(filed.grounding->matchReason == "file");

    const auto signedOverload = ground(index, "Engine::run(int)", {}, "src/engine_a.hpp");
    ROCLINT_CHECK(statusOf(signedOverload) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(signedOverload.grounding->qualifiedTarget == "Engine::run");
    ROCLINT_CHECK(signedOverload.grounding->filePath == "src/engine_b.hpp");
    ROCLINT_CHECK(signedOverload.grounding->matchReason == "signature");

    const auto commaSignature = ground(index, "Pair::run(int, double)");
    ROCLINT_CHECK(statusOf(commaSignature) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(commaSignature.grounding->qualifiedTarget == "Pair::run");
    ROCLINT_CHECK(commaSignature.grounding->signature.find("double second") !=
                  std::string::npos);
    ROCLINT_CHECK(commaSignature.grounding->matchReason == "signature");

    const auto parameter = ground(index, "submit", "in Service", "src/service_a.hpp", "parameter",
                                  core::rewrite::IntentAction::MODIFY, "#2");
    ROCLINT_CHECK(statusOf(parameter) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(parameter.grounding->qualifiedTarget == "Service::submit::limit");
    ROCLINT_CHECK(parameter.grounding->filePath == "src/service_b.hpp");
    ROCLINT_CHECK(parameter.grounding->matchReason == "parameter");
}

void testBatchGroundingCoherence() {
    TempArea area;
    const auto workspace = area.workspace("batch");
    write(workspace, "src/a.hpp", "class Alpha { public: int value; bool send(int packet); };\n");
    write(workspace, "src/b.hpp", "class Beta { public: int value; bool send(int packet); };\n");
    write(workspace, "src/c.hpp", "class Gamma { public: int late; };\n");
    storage::CodebaseIndex index(area.database("batch"), workspace);
    ROCLINT_CHECK(index.refresh({workspace, {}, true}).ok());

    const auto beforeInfo = index.snapshotInfo();
    ROCLINT_CHECK(beforeInfo.ok());
    ROCLINT_CHECK(beforeInfo.value().available);
    ROCLINT_CHECK(beforeInfo.value().complete);

    const std::vector<core::rewrite::CanonicalIntent> raw{
        intent("value", "in Alpha"),
        intent("send", "in Beta"),
        intent("absent", "in Alpha"),
        intent("brandNew", "", core::rewrite::IntentAction::ADD),
    };
    const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(raw, "src/a.hpp", index);
    ROCLINT_CHECK(grounded.size() == raw.size());
    ROCLINT_CHECK(statusOf(grounded[0]) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded[0].grounding->qualifiedTarget == "Alpha::value");
    ROCLINT_CHECK(statusOf(grounded[1]) == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded[1].grounding->qualifiedTarget == "Beta::send");
    ROCLINT_CHECK(statusOf(grounded[2]) == core::rewrite::GroundingStatus::NotFoundInScope);
    ROCLINT_CHECK(statusOf(grounded[3]) == core::rewrite::GroundingStatus::Ungrounded);
    for (const auto& groundedIntent : grounded) {
        ROCLINT_CHECK(groundedIntent.grounding.has_value());
        ROCLINT_CHECK(groundedIntent.grounding->complete);
        ROCLINT_CHECK(groundedIntent.grounding->hashValidated);
    }
    ROCLINT_CHECK(!core::rewrite::AstGroundingEngine::computeAstSnapshotHash(grounded, "batch").empty());

    const auto afterInfo = index.snapshotInfo();
    ROCLINT_CHECK(afterInfo.ok());
    ROCLINT_CHECK(afterInfo.value().available);
    ROCLINT_CHECK(afterInfo.value().generation == beforeInfo.value().generation);

    const auto repeated = core::rewrite::AstGroundingEngine::groundIntents(grounded, "src/b.hpp", index);
    ROCLINT_CHECK(repeated == grounded);
    const auto afterRepeat = index.snapshotInfo();
    ROCLINT_CHECK(afterRepeat.ok());
    ROCLINT_CHECK(afterRepeat.value().generation == beforeInfo.value().generation);
}

void testPromptXmlTotalBudget() {
    core::rewrite::CanonicalIntent ambiguous;
    ambiguous.action = core::rewrite::IntentAction::MODIFY;
    ambiguous.target = "lookup";
    ambiguous.property = "name";
    core::rewrite::AstGroundingEvidence ambiguousEvidence;
    ambiguousEvidence.status = core::rewrite::GroundingStatus::Ambiguous;
    ambiguousEvidence.complete = false;
    ambiguousEvidence.qualifiedTarget = "Engine::lookup";
    for (int index = 0; index < 12; ++index) {
        ambiguousEvidence.candidates.push_back(
            "Engine::lookup @ src/very/long/path/engine_" + std::to_string(index) + ".hpp:10:3");
    }
    ambiguous.grounding = ambiguousEvidence;
    const auto ambiguousXml = core::rewrite::AstGroundingEngine::renderPromptXml({ambiguous}, 300);
    ROCLINT_CHECK(ambiguousXml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(ambiguousXml.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptTokens);
    ROCLINT_CHECK(isValidUtf8(ambiguousXml));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(ambiguousXml));
    ROCLINT_CHECK(ambiguousXml.starts_with("<canonical_intents>"));
    ROCLINT_CHECK(ambiguousXml.ends_with("</canonical_intents>"));
    ROCLINT_CHECK(ambiguousXml.find("complete=\"false\"") != std::string::npos);
    std::size_t candidateCount = 0;
    for (std::size_t position = 0;
         (position = ambiguousXml.find("<candidate>", position)) != std::string::npos;
         position += std::string("<candidate>").size()) {
        ++candidateCount;
    }
    ROCLINT_CHECK(candidateCount >= 2);

    std::vector<core::rewrite::CanonicalIntent> many;
    for (int index = 0; index < 20; ++index) {
        core::rewrite::CanonicalIntent value;
        value.action = core::rewrite::IntentAction::MODIFY;
        value.target = "count";
        core::rewrite::AstGroundingEvidence evidence;
        evidence.status = core::rewrite::GroundingStatus::Verified;
        evidence.qualifiedTarget = "Widget::setCount";
        evidence.filePath = "src/Widget.h";
        evidence.line = static_cast<std::uint32_t>(index + 1);
        evidence.signature = std::string(500, 's');
        value.grounding = evidence;
        many.push_back(std::move(value));
    }
    const auto bounded = core::rewrite::AstGroundingEngine::renderPromptXml(many, 300);
    ROCLINT_CHECK(bounded.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(bounded.size() <= core::rewrite::AstGroundingEngine::kMaximumPromptTokens);
    ROCLINT_CHECK(isValidUtf8(bounded));
    ROCLINT_CHECK(isWellFormedCanonicalIntentXml(bounded));
    ROCLINT_CHECK(bounded.starts_with("<canonical_intents>"));
    ROCLINT_CHECK(bounded.ends_with("</canonical_intents>"));
    ROCLINT_CHECK(bounded.find("<intent ") != std::string::npos);
    std::size_t signatureBytes = 0;
    for (const auto& block : signatureBlocks(bounded)) signatureBytes += block.size();
    ROCLINT_CHECK(signatureBytes <= 300);
}

void testGeneralConstraintsAreNotSymbols() {
    TempArea area; const auto workspace=area.workspace("constraints");
    write(workspace,"src/queue.hpp","namespace demo { struct Queue { bool push(int item); }; }");
    storage::CodebaseIndex index(area.database("constraints"));
    ROCLINT_CHECK(index.refresh({workspace,{},true}).ok());
    const auto raw=core::rewrite::CanonicalIntentExtractor::extract("Make demo::Queue::push return false when full. Keep the public signature unchanged. Do not add logging.");
    const auto grounded=core::rewrite::AstGroundingEngine::groundIntents(raw,{},index);
    ROCLINT_CHECK(grounded.size()==3 && statusOf(grounded[0])==core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(!grounded[1].grounding && !grounded[2].grounding);
    ROCLINT_CHECK(grounded[2].sourceText=="Do not add logging" && grounded[2].value=="prohibited");
    const auto rendered=core::rewrite::AstGroundingEngine::renderPromptContext(grounded);
    ROCLINT_CHECK(rendered.rendered.size()==1 && rendered.xml.find("signature unchanged")==std::string::npos);
    ROCLINT_CHECK(rendered.xml.find("NOT_FOUND")==std::string::npos);
    write(workspace,"src/collision.hpp","void behavior(); void signature();\n");
    ROCLINT_CHECK(index.refresh({workspace,{},true}).ok());
    const auto requirements=core::rewrite::CanonicalIntentExtractor::extract("Keep function behavior unchanged. Preserve function signature.");
    const auto collisions=core::rewrite::AstGroundingEngine::groundIntents(requirements,{},index);
    ROCLINT_CHECK(collisions.size()==2 && !collisions[0].grounding && !collisions[1].grounding);
}

} // namespace

int main() {
    testGeneralConstraintsAreNotSymbols();
    testExactScopeAndHallucination();
    testWorkspaceIsolationAndCloneIdentity();
    testFailedWorkspaceRefreshDoesNotFallBack();
    testLegacyIndexSchemaRebuilds();
    testQualifiedOverloadsRemainAmbiguous();
    testAmbiguousScopeCandidates();
    testLargeScopeMatchesLateMembers();
    testLargeScopeAmbiguityAcrossCandidates();
    testDeclarationAndDefinitionSites();
    testExplicitScopeIsFailClosedAndNormalized();
    testRealExtractorOverloadAndMissingScope();
    testRefreshInvalidatesTruncatedEvidence();
    testRefreshDeletionOversizedAndUnreadable();
    testRefreshFailedReadInvalidatesEvidence();
    testIncompleteRefreshWithholdsStaleEvidence();
    testPathsRefreshCompletenessGatesAuthority();
    testCrossFileAmbiguityAndActiveFileRanking();
    testRecoveredHeaderStillGroundsAndHashesSource();
    testAstSnapshotHashLegacyDomain();
    testPromptRenderContextHashesOnlyRenderedSubset();
    testPromptXmlSignatureBudget();
    testPromptXmlReviewReproduction();
    testPromptXmlPaddedIdentityCannotBecomeWhitespaceOnly();
    testPromptXmlVerifiedIdentityReviewReproduction();
    testPromptXmlVerifiedIdentityRequiresTarget();
    testPromptXmlVerifiedIdentityBudgetBoundaries();
    testPromptXmlVerifiedAndAmbiguousIdentity();
    testPromptXmlVerifiedLongUnicodeIdentity();
    testPromptXmlUnicodeEscapingAndBoundaries();
    testPromptXmlSignatureBudgetZeroAndTiny();
    testPromptXmlMultipleAmbiguityCandidates();
    testPromptXmlExcludesMemberInitializerBodies();
    testPromptXmlTotalBudget();
    testEvidenceMetadataDefaultsAndEquality();
    testTruncatedQueriesCannotProve();
    testExplicitConstraintsBeatFocus();
    testBatchGroundingCoherence();
    testAstGroundingCorpus();
    testLookupLatency();
    testSourceCodeHasNoBodies();
    std::cout << "AST grounding, workspace isolation, ambiguity, scope, refresh, corpus and signature gates passed.\n";
    return 0;
}
