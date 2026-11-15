#pragma once

// RetrievalIndex — persistent hybrid retrieval index (R02).
//
// Stores dense embeddings (768-dim float32, L2-normalized) alongside BM25-
// compatible lexical data in a single SQLite database, using SQLite's FTS5
// for lexical search and a compact blob-serialized vector table for dense
// nearest-neighbor retrieval (approximate cosine via inner-product after
// L2 normalization).
//
// Design constraints:
// - One instance is bound to one workspace at a time.
// - Two workspaces sharing the same database file are disambiguated through
//   a workspace-namespace prefix on all chunk IDs.
// - The index is read by multiple threads (one per retrieval call) and
//   written by a dedicated indexing worker. SQLite WAL mode ensures readers
//   do not block writers and vice versa.
// - IndexPolicy records are stored in the database; a policy mismatch (model
//   revision, dimensions, or chunker version change) triggers safe invalidation
//   and forces a full re-index.

#include "roclint/retrieval/RetrievalTypes.hpp"
#include "roclint/retrieval/IEmbeddingRuntime.hpp"
#include "roclint/core/Result.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::retrieval {

// Source-class filter for retrieval queries.
struct RetrievalFilter {
    std::string workspaceRoot{};        // Required: scope to this workspace
    std::vector<SourceKind> sources{};  // Empty = all sources
    std::string activeFile{};           // Optional hint; not a hard filter
};

// Options for one retrieval call.
struct RetrievalOptions {
    std::size_t maxHits{20};
    float minFusedScore{0.0f};  // Hits below this threshold are dropped
    std::size_t contextBudgetChars{4096}; // Soft limit on total excerpt characters
    bool revalidateHashes{true};          // Verify source content hashes before returning
};

// A single indexed chunk as stored (without the embedding vector).
struct IndexedChunk {
    ChunkId chunkId{};
    SourceKind sourceKind{SourceKind::WorkspaceCode};
    ChunkLocation location{};
    std::optional<AstLink> astLink{};
    std::string text{};          // Exact indexed source bytes for this chunk.
    std::uint64_t indexedAtGen{0}; // Snapshot generation when this chunk was indexed
};

enum class RefreshType : std::uint8_t {
    Full,
    Incremental,
};

// Persistent hybrid retrieval index.
//
// Schema v3 (generation-isolated evidence and successful per-source coverage):
//   retrieval_meta         — policy, workspace, generation
//   retrieval_chunks       — chunk metadata (no embedding vector here)
//   retrieval_chunks_fts   — FTS5 virtual table over chunk text
//   retrieval_embeddings   — raw float32 blobs, one row per chunk
//   retrieval_sources      — source-file freshness tracking
//
class RetrievalIndex {
public:
    static constexpr int kSchemaVersion = 3;

    explicit RetrievalIndex(std::filesystem::path databasePath);
    ~RetrievalIndex();
    RetrievalIndex(RetrievalIndex&&) noexcept;
    RetrievalIndex& operator=(RetrievalIndex&&) noexcept;
    RetrievalIndex(const RetrievalIndex&) = delete;
    RetrievalIndex& operator=(const RetrievalIndex&) = delete;

    // Bind to a workspace root and open the database.
    // Returns an error if the database cannot be opened or the schema is
    // incompatible and the policy does not match (safe invalidation).
    [[nodiscard]] core::Status open(const std::filesystem::path& workspaceRoot,
                                    const IndexPolicy& policy);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] IndexPolicy policy() const;
    [[nodiscard]] std::uint64_t currentGeneration() const noexcept;

    // -----------------------------------------------------------------------
    // Indexing (write path, must be called off the GUI thread)
    // -----------------------------------------------------------------------

    // Begin a new generation. All chunks written after this call belong to
    // the new generation; old chunks from prior generations are not yet removed.
    [[nodiscard]] core::Status beginGeneration(RefreshType type = RefreshType::Full);

    // Write a batch of chunks with their embedding vectors in one transaction.
    // `embeddings` rows must correspond 1:1 to `chunks` in the same order.
    [[nodiscard]] core::Status insertChunks(
        const std::vector<IndexedChunk>& chunks,
        const EmbeddingBatch& embeddings);

    // Record a source file's freshness entry for the current generation.
    [[nodiscard]] core::Status recordSource(const std::string& relativePath,
                                             const std::string& contentHash,
                                             SourceKind kind,
                                             bool indexedComplete = true);

    // Mark all chunks belonging to a source file as deleted in this generation.
    [[nodiscard]] core::Status removeSourceChunks(const std::string& relativePath);

    // Reuse requires a successfully and fully indexed source plus published vectors.
    [[nodiscard]] bool isSourceUnchanged(const std::string& relativePath,
                                         const std::string& contentHash) const;

    // `complete` describes this pass. Full passes establish or revoke coverage;
    // successful incremental passes preserve established coverage. Failed or
    // incomplete passes revoke it, and incremental passes cannot establish it.
    [[nodiscard]] core::Status commitGeneration(bool complete);

    // Abort the current generation without committing. Leaves the index in the
    // last committed state.
    void rollbackGeneration() noexcept;

    // -----------------------------------------------------------------------
    // Retrieval (read path, thread-safe)
    // -----------------------------------------------------------------------

    // Hybrid retrieval: dense nearest-neighbour + FTS5 lexical, fused and ranked.
    // The query embedding must have the same dimensionality as the index policy.
    // Returns hits sorted by fusedScore descending.
    [[nodiscard]] core::Result<std::vector<RetrievedHit>> retrieve(
        const std::vector<float>& queryEmbedding,
        std::string_view queryText,
        const RetrievalFilter& filter,
        const RetrievalOptions& options = {}) const;

    // Revalidate the source hash for a hit against the workspace.
    // Returns false when the file has changed since indexing.
    [[nodiscard]] core::Result<bool> validateSourceHash(
        const std::string& relativePath,
        const std::string& expectedHash,
        const std::filesystem::path& workspaceRoot) const;

    // Current index coverage metadata.
    [[nodiscard]] core::Result<SnapshotCoverage> snapshotCoverage() const;

    // Check whether the stored policy matches the supplied policy.
    // Compares every model, normalization, preprocessing and chunking policy field.
    [[nodiscard]] bool isPolicyCompatible(const IndexPolicy& requested) const;

    // Drop all data for a workspace root (called on workspace forget).
    [[nodiscard]] core::Status forgetWorkspace(const std::string& workspaceRoot);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roclint::retrieval
