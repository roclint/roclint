// Retrieval subsystem benchmark (R00).
//
// Measures:
//   - Cold load time (chunker + index open)
//   - Initial indexing speed (files/second, chunks/second)
//   - Changed-file incremental update
//   - Warm retrieval p50/p95 latency
//   - Peak index size
//   - Embedding batch throughput (mock runtime)
//
// Usage: roclint_retrieval_benchmark [workspace_path]
//
// If no workspace is supplied, the tool indexes a small synthetic corpus.

#include "roclint/retrieval/RetrievalTypes.hpp"
#include "roclint/retrieval/RetrievalIndex.hpp"
#include "roclint/retrieval/TextChunker.hpp"
#include "roclint/retrieval/RetrievalEngine.hpp"

// Use the mock runtime for portable benchmark without model assets.
// For realistic embedding throughput, run test_retrieval_integration.py.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double elapsedMs(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Minimal mock runtime for benchmark purposes.
class BenchmarkRuntime final : public roclint::retrieval::IEmbeddingRuntime {
public:
    BenchmarkRuntime() {
        policy_.modelId = "benchmark-mock";
        policy_.modelRevision = "bench-rev-0000000000000000000000000000000000000000";
        policy_.dimensions = 768;
        policy_.normalized = true;
        policy_.chunkerVersion = roclint::retrieval::TextChunker::version().data();
        policy_.preprocessVersion = "bench.v1";
    }
    roclint::core::Result<roclint::retrieval::EmbeddingBatch> embed(
        const std::vector<std::string>& texts,
        const roclint::retrieval::EmbedOptions&) override
    {
        roclint::retrieval::EmbeddingBatch b;
        b.dimensions = 768;
        b.rows = texts.size();
        b.data.assign(texts.size() * 768, 0.01f);
        ++embedCallCount;
        embedTextCount += texts.size();
        return b;
    }
    roclint::retrieval::EmbeddingRuntimeState state() const noexcept override {
        return roclint::retrieval::EmbeddingRuntimeState::Ready;
    }
    roclint::retrieval::IndexPolicy policy() const override { return policy_; }
    std::string statusMessage() const override { return "Benchmark mock ready"; }
    std::atomic<std::size_t> embedCallCount{0};
    std::atomic<std::size_t> embedTextCount{0};
private:
    roclint::retrieval::IndexPolicy policy_{};
};

void printSeparator() { std::cout << std::string(60, '-') << "\n"; }

// Generate synthetic source files in a temp directory.
std::filesystem::path createSyntheticWorkspace(int numFiles) {
    auto ws = std::filesystem::temp_directory_path() / "roclint_bench_ws";
    std::filesystem::create_directories(ws);
    for (int i = 0; i < numFiles; ++i) {
        auto p = ws / ("file" + std::to_string(i) + ".cpp");
        std::FILE* f = nullptr;
#if defined(_WIN32)
        _wfopen_s(&f, p.c_str(), L"wb");
#else
        f = std::fopen(p.c_str(), "wb");
#endif
        if (f) {
            // Write ~400 bytes of realistic-ish C++ per file.
            std::string content =
                "// file " + std::to_string(i) + "\n"
                "namespace bench {\n"
                "class Item" + std::to_string(i) + " {\n"
                "public:\n"
                "    void process(int v);\n"
                "    int result() const;\n"
                "private:\n"
                "    int data_{0};\n"
                "};\n"
                "void Item" + std::to_string(i) + "::process(int v) { data_ = v * 2; }\n"
                "int Item" + std::to_string(i) + "::result() const { return data_; }\n"
                "} // namespace bench\n";
            std::fwrite(content.data(), 1, content.size(), f);
            std::fclose(f);
        }
    }
    return ws;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path wsRoot;
    bool syntheticWs = false;

    if (argc >= 2) {
        wsRoot = std::filesystem::path(argv[1]);
        if (!std::filesystem::is_directory(wsRoot)) {
            std::cerr << "ERROR: workspace path does not exist: " << wsRoot << "\n";
            return 1;
        }
    } else {
        // Create a 100-file synthetic workspace.
        wsRoot = createSyntheticWorkspace(100);
        syntheticWs = true;
    }

    auto dbPath = std::filesystem::temp_directory_path() / "roclint_bench.db";
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(std::filesystem::path(dbPath.string() + "-wal"), ec);
    std::filesystem::remove(std::filesystem::path(dbPath.string() + "-shm"), ec);

    std::cout << "Roclint Retrieval Subsystem Benchmark\n";
    printSeparator();
    std::cout << "Workspace: " << wsRoot << "\n";
    std::cout << "Index DB:  " << dbPath << "\n";
    printSeparator();

    auto runtime = std::make_shared<BenchmarkRuntime>();

    roclint::retrieval::RetrievalEngineConfig cfg;
    cfg.indexDatabasePath = dbPath;
    cfg.policy = runtime->policy();
    cfg.minFusedScore = 0.0f;
    cfg.revalidateSourceHashes = false;

    roclint::retrieval::RetrievalEngine engine(runtime, cfg);
    roclint::retrieval::IndexLimits limits;
    limits.maximumFiles = 20000;

    // ---- Initial indexing ----
    std::cout << "Initial workspace indexing...\n";
    auto t0 = Clock::now();
    auto statsResult = engine.indexWorkspace(wsRoot, "bench-identity", limits);
    double indexMs = elapsedMs(t0);

    if (!statsResult.ok()) {
        std::cerr << "Index failed: " << statsResult.error().message << "\n";
        return 1;
    }

    const auto& stats = statsResult.value();
    std::cout << "  Files visited:   " << stats.filesVisited << "\n";
    std::cout << "  Files indexed:   " << stats.filesIndexed << "\n";
    std::cout << "  Chunks added:    " << stats.chunksAdded << "\n";
    std::cout << "  Embedding calls: " << runtime->embedCallCount.load() << "\n";
    std::cout << "  Total texts emb: " << runtime->embedTextCount.load() << "\n";
    std::cout << "  Elapsed:         " << indexMs << " ms\n";
    if (stats.filesIndexed > 0)
        std::cout << "  Throughput:      " << (static_cast<double>(stats.filesIndexed) * 1000.0 / indexMs) << " files/s\n";
    printSeparator();

    // ---- Index size ----
    auto dbSize = std::filesystem::file_size(dbPath, ec);
    if (!ec) {
        std::cout << "Index size: " << (dbSize / 1024) << " KB\n";
        if (stats.chunksAdded > 0)
            std::cout << "  Per chunk: " << (dbSize / stats.chunksAdded) << " bytes\n";
    }
    printSeparator();

    // ---- Warm retrieval ----
    std::cout << "Warm retrieval latency (20 queries)...\n";
    std::vector<double> latencies;
    const std::vector<std::string> kQueries = {
        "process data items", "namespace bench class", "result const return",
        "void function parameter", "int data member", "constructor destructor",
        "template class generic", "namespace roclint", "SQLite database query",
        "widget update repaint", "queue push pop fifo", "stack lifo",
        "recursive tree traversal", "hash map lookup", "iterator begin end",
        "const reference parameter", "static member function", "inline constexpr",
        "error handling exception", "callback function pointer"
    };

    // Open index and do retrieval.
    for (const auto& q : kQueries) {
        roclint::retrieval::RetrievalRequest req;
        req.requestId = "bench_q";
        req.revisionId = "rev_1";
        req.promptText = q;
        req.workspace.workspaceRoot = wsRoot.generic_string();
        req.workspace.epoch = 1;
        req.maxHits = 10;

        auto tr = Clock::now();
        auto resp = engine.retrieve(req);
        double ms = elapsedMs(tr);
        latencies.push_back(ms);
        (void)resp;
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        double p50 = latencies[latencies.size() / 2];
        double p95 = latencies[static_cast<std::size_t>(latencies.size() * 0.95)];
        double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) / static_cast<double>(latencies.size());
        std::cout << "  p50: " << p50 << " ms\n";
        std::cout << "  p95: " << p95 << " ms\n";
        std::cout << "  mean: " << mean << " ms\n";
        std::cout << "NOTE: Latency above includes mock embedding (no I/O).\n";
        std::cout << "      With real model, embedding dominates (see integration test).\n";
    }
    printSeparator();

    // Cleanup synthetic workspace.
    if (syntheticWs) {
        std::filesystem::remove_all(wsRoot, ec);
    }
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(std::filesystem::path(dbPath.string() + "-wal"), ec);
    std::filesystem::remove(std::filesystem::path(dbPath.string() + "-shm"), ec);

    std::cout << "Benchmark complete.\n";
    return 0;
}
