#pragma once

// EmbeddingWorkerRuntime — drives the embedding_worker.py subprocess (R05).
//
// Manages the Python process lifetime, sends JSON-newline requests, and parses
// responses. All public methods are thread-safe. Blocking embed() calls are
// intended to run on a dedicated worker thread, never the GUI thread.
//
// This class is part of the retrieval subsystem and must only be linked into
// the retrieval library, not into roclint_core.

#include "roclint/retrieval/IEmbeddingRuntime.hpp"
#include "roclint/retrieval/RetrievalTypes.hpp"
#include "roclint/core/Result.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace roclint::retrieval {

struct EmbeddingWorkerConfig {
    // Path to the Python interpreter.
    std::string pythonExecutable{"python"};
    // Path to embedding_worker.py (must exist).
    std::filesystem::path workerScriptPath{};
    // Path to the local model directory (optional; passes --model-dir).
    std::filesystem::path modelDirectory{};
    // Startup timeout.
    int startupTimeoutMs{30000};
};

// Production IEmbeddingRuntime backed by the embedding_worker.py subprocess.
class EmbeddingWorkerRuntime final : public IEmbeddingRuntime {
public:
    explicit EmbeddingWorkerRuntime(EmbeddingWorkerConfig config);
    ~EmbeddingWorkerRuntime() override;

    // Start the Python subprocess and wait for a ping response.
    // Must be called before embed(). Thread-safe but non-reentrant.
    [[nodiscard]] core::Status start();

    // Stop the subprocess gracefully.
    void stop();

    // IEmbeddingRuntime interface.
    [[nodiscard]] core::Result<EmbeddingBatch> embed(
        const std::vector<std::string>& texts,
        const EmbedOptions& options = {}) override;

    [[nodiscard]] EmbeddingRuntimeState state() const noexcept override;
    [[nodiscard]] IndexPolicy policy() const override;
    [[nodiscard]] std::string statusMessage() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roclint::retrieval
