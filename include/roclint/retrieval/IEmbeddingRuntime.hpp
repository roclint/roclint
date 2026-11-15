#pragma once

// IEmbeddingRuntime — interface to the embedding model subprocess (R01).
//
// This interface decouples the retrieval index from the specific Python worker
// process management. The production implementation drives the
// embedding_worker.py subprocess; a mock implementation serves unit tests.
//
// All methods are off-GUI-thread safe. The implementation must never block
// the caller for longer than the supplied timeout.

#include "roclint/retrieval/RetrievalTypes.hpp"
#include "roclint/core/Result.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace roclint::retrieval {

// A batch of raw embedding vectors. Each row is `dimensions` floats and is
// L2-normalized when IndexPolicy::normalized is true. Rows correspond 1:1 to
// the input texts in the same order.
struct EmbeddingBatch {
    std::vector<float> data{};  // flat: rows × dimensions
    std::size_t rows{0};
    int dimensions{768};
};

// Options for a single embedding call.
struct EmbedOptions {
    // Maximum wall-clock time to wait for the subprocess.
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    // Cancellation flag: set to true to abort in-progress work.
    std::shared_ptr<std::atomic_bool> cancellation{};
};

// Abstract embedding runtime. Implementations must be thread-safe.
class IEmbeddingRuntime {
public:
    virtual ~IEmbeddingRuntime() = default;

    // Synchronous batch embedding. Returns L2-normalized 768-dim vectors.
    // May fail with ProviderUnavailable when the runtime is not ready.
    // May fail with ProviderFailed on subprocess error or timeout.
    [[nodiscard]] virtual core::Result<EmbeddingBatch> embed(
        const std::vector<std::string>& texts,
        const EmbedOptions& options = {}) = 0;

    // Current runtime state.
    [[nodiscard]] virtual EmbeddingRuntimeState state() const noexcept = 0;

    // Model policy this runtime was initialized with. Empty when unavailable.
    [[nodiscard]] virtual IndexPolicy policy() const = 0;

    // Human-readable status description for diagnostics.
    [[nodiscard]] virtual std::string statusMessage() const = 0;
};

} // namespace roclint::retrieval
