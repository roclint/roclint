#include "roclint/core/Json.hpp"
#include "roclint/rewrite/AstGroundingEngine.hpp"
#include "roclint/storage/CodebaseIndex.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
namespace json = roclint::core::json;
namespace rewrite = roclint::core::rewrite;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string text(const json::Value& value, std::string_view key, std::string fallback = {}) {
    const auto* found = value.find(key);
    if (!found) return fallback;
    if (!found->isString()) throw std::runtime_error("Expected string field");
    return found->asString();
}

struct TemporaryWorkspace {
    fs::path root;
    TemporaryWorkspace() {
        const auto stamp = Clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            root = fs::temp_directory_path() /
                ("roclint-eval-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (fs::create_directory(root)) return;
        }
        throw std::runtime_error("Cannot create isolated evaluation workspace");
    }
    ~TemporaryWorkspace() { std::error_code error; fs::remove_all(root, error); }
};

void writeFiles(const json::Value* files, const fs::path& root) {
    if (!files) return;
    if (!files->isObject()) throw std::runtime_error("files must be an object");
    for (const auto& [name, value] : files->asObject()) {
        const auto relative = fs::path(std::u8string(name.begin(), name.end()));
        if (relative.empty() || relative.has_root_path() || !value.isString())
            throw std::runtime_error("Invalid synthetic fixture path/content");
        for (const auto& part : relative)
            if (part == "..") throw std::runtime_error("Fixture path escapes workspace");
        const auto destination = root / relative;
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << value.asString();
        if (!output) throw std::runtime_error("Cannot write synthetic fixture");
    }
}

const char* statusName(rewrite::GroundingStatus status) {
    switch (status) {
        case rewrite::GroundingStatus::Verified: return "Verified";
        case rewrite::GroundingStatus::Ambiguous: return "Ambiguous";
        case rewrite::GroundingStatus::NotFoundInScope: return "NotFoundInScope";
        case rewrite::GroundingStatus::TypeMismatch: return "TypeMismatch";
        case rewrite::GroundingStatus::Ungrounded: return "Ungrounded";
    }
    return "Ungrounded";
}

double elapsed(Clock::time_point start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start).count()) / 1e6;
}

json::Value evaluate(const json::Value& input) {
    TemporaryWorkspace temporary;
    const auto workspace = temporary.root / "workspace";
    fs::create_directory(workspace);
    writeFiles(input.find("files"), workspace);
    roclint::storage::CodebaseIndex index(temporary.root / "index.sqlite", workspace);
    const auto scanStart = Clock::now();
    const auto refreshed = index.refresh({workspace, text(input, "activeFile"), true});
    if (!refreshed.ok()) throw std::runtime_error(refreshed.error().message);
    const auto indexMs = elapsed(scanStart);
    writeFiles(input.find("mutate"), workspace);

    std::vector<rewrite::CanonicalIntent> intents;
    if (input.find("prompt")) {
        intents = rewrite::CanonicalIntentExtractor::extract(text(input, "prompt"));
    } else {
        const auto* value = input.find("intent");
        if (!value || !value->isObject()) throw std::runtime_error("Missing intent");
        rewrite::CanonicalIntent intent;
        const auto action = text(*value, "action", "MODIFY");
        if (action == "ADD") intent.action = rewrite::IntentAction::ADD;
        else if (action == "REMOVE") intent.action = rewrite::IntentAction::REMOVE;
        else if (action == "PRESERVE") intent.action = rewrite::IntentAction::PRESERVE;
        else if (action == "CONSTRAIN") intent.action = rewrite::IntentAction::CONSTRAIN;
        else if (action != "MODIFY") throw std::runtime_error("Unknown intent action");
        intent.target = text(*value, "target");
        intent.property = text(*value, "property", "name");
        intent.value = text(*value, "value", "changed");
        intent.condition = text(*value, "condition");
        intents.push_back(std::move(intent));
    }
    const auto groundStart = Clock::now();
    const auto grounded = rewrite::AstGroundingEngine::groundIntents(intents, text(input, "activeFile"), index);
    const auto groundingMs = elapsed(groundStart);
    const auto renderStart = Clock::now();
    const auto rendered = rewrite::AstGroundingEngine::renderPromptContext(grounded);
    const auto renderingMs = elapsed(renderStart);
    auto evidence = json::Value::array();
    for (const auto& intent : grounded) {
        if (!intent.grounding) continue;
        const auto& item = *intent.grounding;
        auto row = json::Value::object();
        row.set("status", json::Value::string(statusName(item.status)));
        row.set("target", json::Value::string(item.qualifiedTarget));
        row.set("file", json::Value::string(item.filePath));
        row.set("signature", json::Value::string(item.signature));
        row.set("matchReason", json::Value::string(item.matchReason));
        row.set("complete", json::Value::boolean(item.complete));
        row.set("candidates", json::Value::number(static_cast<double>(item.candidates.size())));
        if (!item.filePath.empty() && !item.fileHash.empty()) {
            const auto valid = index.validateFileHash(item.filePath, item.fileHash);
            row.set("validateFileHash", json::Value::boolean(valid.ok() && valid.value()));
        }
        evidence.push(std::move(row));
    }
    auto result = json::Value::object();
    result.set("evidence", std::move(evidence));
    result.set("xml", json::Value::string(rendered.xml));
    result.set("rendered_count", json::Value::number(static_cast<double>(rendered.rendered.size())));
    result.set("ast_hash", json::Value::string(rewrite::AstGroundingEngine::computeAstSnapshotHash(
        rendered.rendered, "synthetic-evaluation-workspace")));
    result.set("index_ms", json::Value::number(indexMs));
    result.set("grounding_ms", json::Value::number(groundingMs));
    result.set("render_ms", json::Value::number(renderingMs));
    return result;
}
}

int main() {
    try {
        const std::string input(std::istreambuf_iterator<char>{std::cin}, {});
        const auto parsed = json::parse(input, {4 * 1024 * 1024, 32});
        if (!parsed.ok()) throw std::runtime_error(parsed.error().message);
        std::cout << json::serialize(evaluate(parsed.value())) << '\n';
        return 0;
    } catch (const std::exception& error) {
        auto output = json::Value::object();
        output.set("error", json::Value::string(error.what()));
        std::cout << json::serialize(output) << '\n';
        return 1;
    }
}
