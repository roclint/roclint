#include "roclint/rewrite/AstGroundingEngine.hpp"
#include "roclint/storage/CodebaseIndex.hpp"
#include "support/check.hpp"

#include <llama.h>

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
namespace rewrite = core::rewrite;

namespace {

struct TempArea {
    fs::path root = [&]() {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() /
            ("roclint-token-budget-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
    }();
    TempArea() { fs::create_directories(root); }
    ~TempArea() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void writeFile(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

rewrite::CanonicalIntent resolve(storage::CodebaseIndex& index, std::string target) {
    rewrite::CanonicalIntent input;
    input.action = rewrite::IntentAction::MODIFY;
    input.target = std::move(target);
    input.property = "name";
    input.value = "changed";
    input.nodeId = "nid_token_budget";
    auto grounded = rewrite::AstGroundingEngine::groundIntents({input}, {}, index);
    ROCLINT_CHECK(!grounded.empty());
    return grounded.front();
}

std::size_t tokenCount(llama_model* model, std::string_view text) {
    const auto* vocab = llama_model_get_vocab(model);
    const int count = llama_tokenize(vocab, text.data(), static_cast<int>(text.size()),
                                     nullptr, 0, false, false);
    ROCLINT_CHECK(count <= 0);
    return static_cast<std::size_t>(-count);
}

void checkRendered(llama_model* model, const rewrite::PromptRenderResult& result,
                   std::string_view label) {
    ROCLINT_CHECK(result.xml.size() <= rewrite::AstGroundingEngine::kMaximumPromptBytes);
    ROCLINT_CHECK(result.xml.starts_with("<canonical_intents>"));
    ROCLINT_CHECK(result.xml.ends_with("</canonical_intents>"));
    const auto tokens = tokenCount(model, result.xml);
    std::cout << "token_budget|case=" << label << "|bytes=" << result.xml.size()
              << "|granite_tokens=" << tokens << "|rendered=" << result.rendered.size() << "\n";
    ROCLINT_CHECK(tokens <= rewrite::AstGroundingEngine::kMaximumPromptTokens);
}

void testRenderedReviewFixture(llama_model* model) {
    TempArea area;
    const auto workspace = area.root / "workspace";
    std::string name = "n";
    unsigned int state = 82371;
    const char* alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int index = 1; index < 650; ++index) {
        state = state * 1664525U + 1013904223U;
        name += alphabet[(state >> 16) % 62];
    }
    std::string file = "f";
    for (int index = 1; index < 110; ++index) {
        state = state * 1664525U + 1013904223U;
        file += alphabet[(state >> 16) % 62];
    }
    file += ".hpp";
    writeFile(workspace / file, "int " + name + ";\n");

    storage::CodebaseIndex index(area.root / "review.db", workspace);
    const auto refreshed = index.refresh({workspace, {}, true});
    ROCLINT_CHECK(refreshed.ok());
    const auto grounded = resolve(index, name);
    ROCLINT_CHECK(grounded.grounding.has_value());
    ROCLINT_CHECK(grounded.grounding->status == rewrite::GroundingStatus::Verified);

    const auto rendered = rewrite::AstGroundingEngine::renderPromptContext({grounded});
    ROCLINT_CHECK(!rendered.rendered.empty());
    ROCLINT_CHECK(rendered.xml.find("VERIFIED") != std::string::npos);
    checkRendered(model, rendered, "long_identifier_and_path");
}

void testUnicodeAndEscaping(llama_model* model) {
    rewrite::CanonicalIntent intent;
    intent.action = rewrite::IntentAction::MODIFY;
    intent.target = "démarrer_référence";
    intent.property = "name";
    intent.value = "changed";
    intent.nodeId = "nid_unicode";

    rewrite::AstGroundingEvidence evidence;
    evidence.status = rewrite::GroundingStatus::Verified;
    evidence.qualifiedTarget = "namespace_démo::démarrer_référence";
    evidence.targetKind = storage::AstNodeKind::Function;
    evidence.filePath = "src/éxample<&>\"path.hpp";
    evidence.fileHash = std::string(64, 'a');
    evidence.fileSpan = {10, 42};
    evidence.line = 7;
    evidence.signature = "bool démarrer_référence(const std::string &valeur, int nombre) noexcept";
    intent.grounding = evidence;

    const auto rendered = rewrite::AstGroundingEngine::renderPromptContext({intent});
    ROCLINT_CHECK(!rendered.rendered.empty());
    checkRendered(model, rendered, "unicode_and_escaped_xml");
}

void testAmbiguityCandidates(llama_model* model) {
    rewrite::CanonicalIntent intent;
    intent.action = rewrite::IntentAction::MODIFY;
    intent.target = "run";
    intent.property = "name";
    intent.value = "changed";
    intent.nodeId = "nid_ambiguous";

    rewrite::AstGroundingEvidence evidence;
    evidence.status = rewrite::GroundingStatus::Ambiguous;
    evidence.qualifiedTarget = "run";
    evidence.complete = false;
    for (int index = 0; index < 8; ++index) {
        evidence.candidates.push_back(
            "candidate_" + std::to_string(index) + " @ src/" + std::string(40, 'p') + ".hpp:1:1");
    }
    intent.grounding = evidence;

    const auto rendered = rewrite::AstGroundingEngine::renderPromptContext({intent});
    ROCLINT_CHECK(!rendered.rendered.empty());
    checkRendered(model, rendered, "multiple_ambiguity_candidates");
}

} // namespace

int main() {
#ifdef ROCLINT_GRANITE_TOKENIZER_MODEL
    const fs::path modelPath = fs::path(ROCLINT_GRANITE_TOKENIZER_MODEL);
#else
    const fs::path modelPath{};
#endif
    if (modelPath.empty() || !fs::is_regular_file(modelPath)) {
        std::cout << "token_budget|skipped=model_unavailable\n";
        return 0;
    }

    auto params = llama_model_default_params();
    params.vocab_only = true;
    llama_model* model = llama_model_load_from_file(modelPath.string().c_str(), params);
    ROCLINT_CHECK(model != nullptr);
    if (model == nullptr) return 1;

    testRenderedReviewFixture(model);
    testUnicodeAndEscaping(model);
    testAmbiguityCandidates(model);
    llama_model_free(model);
    std::cout << "AST context token budget, UTF-8/XML boundaries and rendered-subset checks passed.\n";
    return 0;
}
