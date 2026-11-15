#pragma once

// RetrievalEngine — top-level retrieval subsystem interface (R04).
//
// This is the primary integration point for Gemini's generation layer.
//
// Ownership:
//   - RetrievalEngine owns the EmbeddingRuntime (subprocess lifetime).
//   - RetrievalEngine owns the RetrievalIndex (SQLite database connection pool).
//   - All indexing and retrieval operations run off the GUI thread.
//
// Thread safety:
//   - retrieve() is safe to call from multiple threads concurrently (each call
//     opens a read-only SQLite connection).
//   - indexWorkspace() must only be called from one indexing thread at a time
//     (enforced by the engine's internal queue).
//   - state() is atomic.
//
// Stale-response rejection:
//   - Every RetrievalResponse carries the caller's requestId, revisionId, and
//     workspaceEpoch. The caller must call response.isStale() before consuming
//     any hits.
//
// Unavailable state:
//   - When the embedding runtime or model assets are missing, state() returns
//     Unavailable and retrieve() returns a response with status Unavailable.
//   - The engine never fabricates embeddings or returns synthetic hits.

#include "roclint/retrieval/RetrievalTypes.hpp"
#include "roclint/retrieval/RetrievalIndex.hpp"
#include "roclint/retrieval/TextChunker.hpp"
#include "roclint/core/Result.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace roclint::retrieval {

class IEmbeddingRuntime;

// Configuration for the retrieval engine.
struct RetrievalEngineConfig {
    // Path to the persistent retrieval index database.
    // Must be outside the workspace (like the AST codebase-index.db).
    std::filesystem::path indexDatabasePath{};

    // Policy identifying the model, dimensions, and chunker version.
    IndexPolicy policy{};

    // Chunking policy.
    ChunkerPolicy chunkerPolicy{};

    // Default retrieval limits (can be overridden per-request).
    std::size_t defaultMaxHits{20};
    std::size_t defaultContextBudgetTokens{512};

    // Minimum fused score threshold (hits below this are dropped).
    //
    // With RRF (k=60) and two branches, the maximum achievable score is:
    //   rank 1 in both branches: 1/(60+1) + 1/(60+1) ≈ 0.03279
    //   rank 1 in one branch only: 1/(60+1) ≈ 0.01639
    //
    // Therefore the old default of 0.05 was larger than any possible RRF
    // score and silently suppressed every result. The corrected default is
    // 0.0 (no threshold); a meaningful threshold should be established from
    // empirical retrieval quality evaluation on the specific corpus.
    float minFusedScore{0.0f};

    // Revalidate source hashes on each retrieval call.
    bool revalidateSourceHashes{true};
};

// Indexing progress callback.
struct IndexingProgress {
    std::size_t filesProcessed{0};
    std::size_t filesTotal{0};
    bool complete{false};
    std::string currentFile{};
};
using IndexingProgressCallback = std::function<void(const IndexingProgress&)>;

// The retrieval engine. Callers own one instance per workspace/database path.
class RetrievalEngine {
public:
    explicit RetrievalEngine(std::shared_ptr<IEmbeddingRuntime> runtime,
                              RetrievalEngineConfig config);
    ~RetrievalEngine();
    RetrievalEngine(const RetrievalEngine&) = delete;
    RetrievalEngine& operator=(const RetrievalEngine&) = delete;
    RetrievalEngine(RetrievalEngine&&) noexcept;
    RetrievalEngine& operator=(RetrievalEngine&&) noexcept;

    // -----------------------------------------------------------------------
    // Runtime state
    // -----------------------------------------------------------------------

    // Combined state: Ready when both the index and the runtime are available.
    [[nodiscard]] RetrievalStatus status() const noexcept;

    // Human-readable status description.
    [[nodiscard]] std::string statusMessage() const;

    // The index policy in use (empty policy when not yet opened).
    [[nodiscard]] IndexPolicy activePolicy() const;

    // -----------------------------------------------------------------------
    // Indexing (must be called off the GUI thread)
    // -----------------------------------------------------------------------

    // Full workspace index refresh. Opens/creates the index for the workspace,
    // walks eligible files, chunks and embeds new/changed content, and removes
    // deleted files. Returns index statistics.
    //
    // Set `cancellation` to abort an in-progress pass.
    [[nodiscard]] core::Result<IndexStats> indexWorkspace(
        const std::filesystem::path& workspaceRoot,
        const std::string& workspaceIdentity,
        const IndexLimits& limits = {},
        std::shared_ptr<std::atomic_bool> cancellation = {},
        IndexingProgressCallback progressCallback = {});

    // Incremental refresh of explicit workspace-relative paths.
    [[nodiscard]] core::Result<IndexStats> indexPaths(
        const std::filesystem::path& workspaceRoot,
        const std::string& workspaceIdentity,
        const std::vector<std::string>& relativePaths,
        const IndexLimits& limits = {},
        std::shared_ptr<std::atomic_bool> cancellation = {});

    // -----------------------------------------------------------------------
    // Retrieval (thread-safe; each call is independent)
    // -----------------------------------------------------------------------

    // Execute a retrieval request. Embeds the query text (or uses the selection
    // text as the query), searches the index, fuses dense and lexical scores,
    // and returns bounded hits.
    //
    // This method is synchronous and must be called off the GUI thread.
    // Superseded requests must be detected by the caller via isStale().
    [[nodiscard]] RetrievalResponse retrieve(const RetrievalRequest& request);

    // -----------------------------------------------------------------------
    // Evidence revalidation
    // -----------------------------------------------------------------------

    // Revalidate the source hashes for a set of previously retrieved hits.
    // Returns the subset of hits whose source content has changed since indexing.
    [[nodiscard]] std::vector<ChunkId> revalidateHits(
        const std::vector<RetrievedHit>& hits,
        const std::filesystem::path& workspaceRoot) const;

    // Compute the context fingerprint for a subset of hits (the emitted subset).
    // This fingerprint must be stored alongside any approval that relied on
    // retrieved context.
    [[nodiscard]] static std::string computeContextFingerprint(
        std::string_view requestId,
        const std::vector<RetrievedHit>& emittedHits);

    // -----------------------------------------------------------------------
    // Workspace lifecycle
    // -----------------------------------------------------------------------

    // Drop all index data for the given workspace root.
    [[nodiscard]] core::Status forgetWorkspace(const std::filesystem::path& workspaceRoot);

    // -----------------------------------------------------------------------
    // Test seams (compiled only when ROCLINT_ENABLE_TEST_MOCKS is defined)
    // -----------------------------------------------------------------------
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    [[nodiscard]] RetrievalIndex* indexForTesting() noexcept;
    [[nodiscard]] IEmbeddingRuntime* runtimeForTesting() noexcept;
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roclint::retrieval
