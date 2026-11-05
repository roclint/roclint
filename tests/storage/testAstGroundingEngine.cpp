#include "roclint/rewrite/AstGroundingEngine.hpp"
#include "roclint/storage/CodebaseIndex.hpp"
#include "roclint/storage/AstSymbolExtractor.hpp"
#include "support/check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace roclint;
namespace fs = std::filesystem;

namespace {

struct TempWorkspace {
    fs::path root = fs::temp_directory_path() /
        ("roclint-ast-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path database = root.parent_path() / (root.filename().string() + ".db");
    TempWorkspace() { fs::create_directories(root / "src"); }
    ~TempWorkspace() {
        std::error_code error;
        fs::remove_all(root, error);
        fs::remove(database, error);
        fs::remove(database.string() + "-wal", error);
        fs::remove(database.string() + "-shm", error);
    }
    void write(std::string_view path, std::string_view text) {
        std::ofstream stream(root / path, std::ios::binary);
        stream << text;
    }
};

core::rewrite::CanonicalIntent intent(std::string target, std::string condition,
                                      core::rewrite::IntentAction action = core::rewrite::IntentAction::MODIFY) {
    core::rewrite::CanonicalIntent value;
    value.action = action;
    value.target = std::move(target);
    value.condition = std::move(condition);
    value.property = value.target == "SessionManager" ? "parameter" : "name";
    value.value = "changed";
    value.nodeId = "nid_test";
    return value;
}

void testExactScopeAndHallucination() {
    TempWorkspace workspace;
    workspace.write("src/SessionManager.h",
                    "class SessionManager { public: bool readPacket(size_t offset, uint32_t flags); };\n");
    workspace.write("src/LogClient.h", "class LogClient { public: bool readPacket(size_t offset); };\n");
    storage::CodebaseIndex index(workspace.database);
    const storage::WorkspaceContext context{workspace.root, "", true};
    const auto refreshed = index.refresh(context);
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(refreshed.value().filesIndexed == 2);
    ROCLINT_CHECK(refreshed.value().symbolsIndexed >= 6);

    const std::vector<core::rewrite::CanonicalIntent> raw{
        intent("offset", "in SessionManager::readPacket"),
        intent("offset", "in LogClient::readPacket"),
        intent("offset", "in MissingClass::readPacket"),
        intent("readPacket", ""),
        intent("newSymbol", "", core::rewrite::IntentAction::ADD),
        intent("SessionManager", ""),
    };
    const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(
        raw, "src/SessionManager.h", index);
    ROCLINT_CHECK(grounded.size() == raw.size());
    ROCLINT_CHECK(grounded[0].grounding.has_value());
    ROCLINT_CHECK(grounded[0].grounding->status == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded[0].grounding->qualifiedTarget == "SessionManager::readPacket::offset");
    ROCLINT_CHECK(grounded[0].grounding->filePath == "src/SessionManager.h");
    ROCLINT_CHECK(grounded[1].grounding->qualifiedTarget == "LogClient::readPacket::offset");
    ROCLINT_CHECK(grounded[2].grounding->status == core::rewrite::GroundingStatus::NotFoundInScope);
    ROCLINT_CHECK(grounded[3].grounding->status == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded[3].grounding->qualifiedTarget == "SessionManager::readPacket");
    ROCLINT_CHECK(grounded[4].grounding->status == core::rewrite::GroundingStatus::Ungrounded);
    ROCLINT_CHECK(grounded[5].grounding->status == core::rewrite::GroundingStatus::TypeMismatch);
    const auto xml = core::rewrite::AstGroundingEngine::renderPromptXml(grounded, 300);
    ROCLINT_CHECK(xml.find("<canonical_intents>") != std::string::npos);
    ROCLINT_CHECK(xml.find("status=\"VERIFIED\"") != std::string::npos);
    ROCLINT_CHECK(xml.find("SessionManager::readPacket::offset") != std::string::npos);
    ROCLINT_CHECK(xml.find("status=\"NOT_FOUND_IN_SCOPE\"") != std::string::npos);
}

void testCrossFileAmbiguityAndActiveFileRanking() {
    TempWorkspace workspace;
    workspace.write("src/SessionManager.h",
                    "class SessionManager { public: bool readPacket(size_t offset); };\n");
    workspace.write("src/LogClient.h", "class LogClient { public: bool readPacket(size_t offset); };\n");
    storage::CodebaseIndex index(workspace.database);
    const storage::WorkspaceContext context{workspace.root, "", true};
    ROCLINT_CHECK(index.refresh(context).ok());

    const auto ambiguousRaw = std::vector<core::rewrite::CanonicalIntent>{intent("readPacket", "")};
    const auto ambiguous = core::rewrite::AstGroundingEngine::groundIntents(
        ambiguousRaw, "", index);
    ROCLINT_CHECK(ambiguous.front().grounding.has_value());
    ROCLINT_CHECK(ambiguous.front().grounding->status == core::rewrite::GroundingStatus::Ambiguous);
    ROCLINT_CHECK(ambiguous.front().grounding->candidates.size() >= 2);

    const auto active = core::rewrite::AstGroundingEngine::groundIntents(
        ambiguousRaw, "src/LogClient.h", index);
    ROCLINT_CHECK(active.front().grounding->status == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(active.front().grounding->qualifiedTarget == "LogClient::readPacket");
}

void testRecoveredHeaderStillGrounds() {
    TempWorkspace workspace;
    workspace.write("src/Widget.h",
                    "class Widget : public Base {\n"
                    "    Q_PROPERTY(bool visible READ isVisible NOTIFY visibilityChanged)\n"
                    "public:\n"
                    "    bool setVisible(bool visible);\n"
                    "    bool visible;\n"
                    "};\n");
    storage::CodebaseIndex index(workspace.database);
    const storage::WorkspaceContext context{workspace.root, "", true};
    const auto refreshed = index.refresh(context);
    ROCLINT_CHECK(refreshed.ok());
    ROCLINT_CHECK(refreshed.value().filesIndexed == 1);
    ROCLINT_CHECK(refreshed.value().filesRecovered == 1);
    const auto bogus = index.findByName("Q_PROPERTY");
    ROCLINT_CHECK(bogus.ok() && bogus.value().empty());
    const auto raw = std::vector<core::rewrite::CanonicalIntent>{intent("visible", "in Widget::setVisible")};
    const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(raw, "src/Widget.h", index);
    ROCLINT_CHECK(grounded.front().grounding.has_value());
    ROCLINT_CHECK(grounded.front().grounding->status == core::rewrite::GroundingStatus::Verified);
    ROCLINT_CHECK(grounded.front().grounding->qualifiedTarget == "Widget::setVisible::visible");
    ROCLINT_CHECK(!grounded.front().grounding->fileHash.empty());
    const auto snapshot = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(grounded, "workspace-id");
    ROCLINT_CHECK(snapshot.size() == 64);
    const auto repeated = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(grounded, "workspace-id");
    ROCLINT_CHECK(snapshot == repeated);
    const auto otherWorkspace = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(grounded, "other-id");
    ROCLINT_CHECK(snapshot != otherWorkspace);
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
    core::rewrite::CanonicalIntent intent;
    intent.action = core::rewrite::IntentAction::MODIFY;
    intent.target = "count";
    intent.property = "name";
    core::rewrite::AstGroundingEvidence evidence;
    evidence.status = core::rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget = "Widget::setCount";
    evidence.filePath = "src/Widget.h";
    evidence.line = 12;
    evidence.signature = "bool setCount(std::size_t count, const std::string& label = \"a&b<c>\")";
    intent.grounding = evidence;

    const auto bounded = core::rewrite::AstGroundingEngine::renderPromptXml({intent}, 64);
    const auto boundedBlocks = signatureBlocks(bounded);
    ROCLINT_CHECK(boundedBlocks.size() == 1);
    ROCLINT_CHECK(boundedBlocks.front().size() <= 64);
    for (std::size_t amp = boundedBlocks.front().find('&'); amp != std::string::npos;
         amp = boundedBlocks.front().find('&', amp + 1)) {
        const auto semicolon = boundedBlocks.front().find(';', amp);
        ROCLINT_CHECK(semicolon != std::string::npos && semicolon > amp);
    }

    auto second = intent;
    second.target = "label";
    second.grounding->qualifiedTarget = "Widget::setLabel";
    second.grounding->signature = "bool setLabel(const std::string& value, bool visible)";
    auto third = intent;
    third.target = "visible";
    third.grounding->qualifiedTarget = "Widget::setVisible";
    third.grounding->signature = "bool setVisible(bool visible, bool notify, bool animate)";
    const auto budgeted = core::rewrite::AstGroundingEngine::renderPromptXml({intent, second, third}, 300);
    std::size_t total = 0;
    for (const auto& block : signatureBlocks(budgeted)) total += block.size();
    ROCLINT_CHECK(total <= 300);
    ROCLINT_CHECK(budgeted.find("return") == std::string::npos);
}

void testLookupLatency() {
    TempWorkspace workspace;
    workspace.write("src/ledger.h",
                    "class Ledger { public: bool postEntry(size_t accountId, size_t amount); };\n");
    storage::CodebaseIndex index(workspace.database);
    const storage::WorkspaceContext context{workspace.root, "", true};
    ROCLINT_CHECK(index.refresh(context).ok());
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
    }
}

} // namespace

int main() {
    testExactScopeAndHallucination();
    testCrossFileAmbiguityAndActiveFileRanking();
    testRecoveredHeaderStillGrounds();
    testPromptXmlSignatureBudget();
    testLookupLatency();
    testSourceCodeHasNoBodies();
    std::cout << "AST grounding, scope hierarchy, hallucination detection and latency passed.\n";
    return 0;
}
