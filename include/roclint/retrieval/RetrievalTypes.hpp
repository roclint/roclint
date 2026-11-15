#pragma once

// Shared value types for the retrieval subsystem (R00).
//
// These types carry no Qt or model dependencies and are safe to include from
// any translation unit including pure C++ library code.

#include "roclint/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::retrieval {

// ---------------------------------------------------------------------------
// Source and provenance identifiers
// ---------------------------------------------------------------------------

// Discriminator for where a retrieved chunk came from.
enum class SourceKind : std::uint8_t {
    WorkspaceCode,     // Implementation, test or configuration file inside the workspace
    WorkspaceDoc,      // Documentation file (Markdown, RST, plain text) inside the workspace
    ImportedReference, // Explicitly imported text reference material
    UserPreference,    // Explicitly saved user-level preference or approved decision
    WorkspaceDecision, // Workspace-scoped approved decision
};

[[nodiscard]] const char* toString(SourceKind kind) noexcept;

// Policy governing what the embedding index covers.
struct IndexPolicy {
    std::string modelId{};          // HuggingFace model ID, e.g. "ibm-granite/granite-embedding-311m-multilingual-r2"
    std::string modelRevision{};    // Immutable HF commit hash
    int dimensions{768};            // Embedding dimensionality in use (768 = full)
    bool normalized{true};          // Embeddings are L2-normalized before storage
    std::string chunkerVersion{};   // Identifies the chunking algorithm and parameters
    std::string preprocessVersion{}; // Identifies tokenization/preprocessing version
};

bool operator==(const IndexPolicy&, const IndexPolicy&) noexcept;

// Stable opaque identifier for a single chunk in the index.
// Format: "chunk_<hex-sha256-of(source-path:start:end:policy-hash)>"
struct ChunkId {
    std::string value{};
    bool operator==(const ChunkId&) const = default;
};

// Source location bound to a chunk.
struct ChunkLocation {
    std::string sourcePath{};    // Workspace-relative path or reference ID
    std::string contentHash{};   // SHA-256 of the indexed source bytes at index time
    std::uint32_t byteStart{0};  // Byte offset of first included byte
    std::uint32_t byteEnd{0};    // Byte offset one past last included byte
    std::uint32_t lineStart{1};  // 1-based line of first byte
    std::uint32_t lineEnd{1};    // 1-based line of last byte
};

// Optional AST link attached to a chunk. Status is advisory; a semantic match
// must never become AstVerified through retrieval alone.
enum class AstLinkStatus : std::uint8_t {
    None,       // No AST link attempted
    Linked,     // Symbol was found in the index at chunk time (not re-verified at retrieval time)
    Missing,    // Symbol extraction was attempted but produced no symbol for this span
};

struct AstLink {
    std::string symbolId{};         // sym_<prefix> from storage::AstSymbol, may be empty
    std::string qualifiedName{};
    AstLinkStatus status{AstLinkStatus::None};
};

// ---------------------------------------------------------------------------
// Retrieval request / response
// ---------------------------------------------------------------------------

// Caller-supplied purpose hint.
enum class RetrievalPurpose : std::uint8_t {
    WordRefinement,     // Small targeted retrieval for a selected word or phrase
    ClarificationAid,   // Broader retrieval for clarification question generation
    FullPromptContext,  // Full-prompt rewriting or generation augmentation
};

// Workspace identity, epoch and selection carried by the caller.
struct WorkspaceBinding {
    std::string workspaceRoot{};     // Canonical absolute path
    std::string workspaceIdentity{}; // Canonical hash from workspace_helper
    std::uint64_t epoch{0};          // Monotonically increasing workspace-change counter
    std::string activeFile{};        // Workspace-relative hint; empty = none
};

// Optional text selection carried in the request.
struct TextSelection {
    std::string text{};         // The selected word or phrase
    std::uint32_t byteStart{0}; // Byte offset within the prompt string
    std::uint32_t byteEnd{0};
};

// Versioned retrieval request.
struct RetrievalRequest {
    // Identifiers for stale-response rejection by the caller.
    std::string requestId{};          // Caller-minted, unique per request
    std::string sessionId{};
    std::string revisionId{};
    std::string promptHash{};         // SHA-256 of the frozen prompt text

    WorkspaceBinding workspace{};
    std::optional<TextSelection> selection{};

    std::string promptText{};         // Full frozen prompt text
    std::string currentGoal{};        // Caller-supplied goal summary (may be empty)
    std::vector<std::string> activeDecisions{}; // Caller-supplied confirmed decisions

    RetrievalPurpose purpose{RetrievalPurpose::ClarificationAid};

    // Budget limits (tokens for the downstream generation context).
    std::size_t maxHits{20};                // Upper bound on returned chunks
    std::size_t contextBudgetTokens{512};   // Approximate token budget for all excerpts

    // Source filter: if empty, all source kinds are eligible.
    std::vector<SourceKind> allowedSources{};
};

// Evidence and provenance for a single retrieved chunk.
struct RetrievedHit {
    ChunkId chunkId{};
    SourceKind sourceKind{SourceKind::WorkspaceCode};
    ChunkLocation location{};
    std::optional<AstLink> astLink{};

    std::string excerpt{};         // The exact bytes emitted into the context
    float denseScore{0.0f};        // Cosine similarity to query embedding
    float lexicalScore{0.0f};      // BM25 / FTS5 score component (0 when not ranked)
    float fusedScore{0.0f};        // Final fused rank score (higher = more relevant)
    std::string rankingEvidence{}; // Human-readable ranking notes (for diagnostics)

    // Freshness: true if the source hash matched the index-time hash on this call.
    bool sourceHashValid{false};

    // A chunk sourced from an injected instruction should not be used as evidence.
    bool promptInjectionFlagged{false};
};

// Indexing and retrieval availability status.
enum class RetrievalStatus : std::uint8_t {
    Ready,           // Index is current and results are authoritative
    Indexing,        // A refresh is in progress; results are from the last complete snapshot
    Partial,         // Index exists but coverage is incomplete (some files not yet indexed)
    NoRelevantContext, // Index is ready but no hits exceeded the relevance threshold
    Unavailable,     // Embedding runtime or model assets are missing/failed
};

[[nodiscard]] const char* toString(RetrievalStatus status) noexcept;

// Coverage and freshness of the index snapshot used by this response.
struct SnapshotCoverage {
    std::uint64_t snapshotGeneration{0};
    bool coverageComplete{false}; // All eligible workspace files are indexed
    std::size_t indexedChunks{0};
    std::size_t indexedFiles{0};
    double indexAgeSec{0.0};      // Seconds since the last completed refresh
};

// Versioned retrieval response.
struct RetrievalResponse {
    // Mirrors request identifiers for stale-response rejection.
    std::string requestId{};
    std::string revisionId{};
    std::string promptHash{};
    std::uint64_t workspaceEpoch{0};

    RetrievalStatus status{RetrievalStatus::Unavailable};
    SnapshotCoverage coverage{};
    IndexPolicy policy{};        // Policy that produced this response

    std::vector<RetrievedHit> hits{};

    // SHA-256 over requestId + sorted(chunkId.value + excerpt) for the
    // exact emitted excerpts. Only emitted excerpts enter this fingerprint.
    std::string contextFingerprint{};

    // Timing and diagnostics.
    double retrievalMs{0.0};
    double embeddingMs{0.0};
    std::string failureReason{};  // Non-empty when status == Unavailable

    // True when the caller should reject this response due to workspace/epoch mismatch.
    [[nodiscard]] bool isStale(std::string_view expectedRevisionId,
                                std::uint64_t expectedEpoch) const noexcept {
        return revisionId != expectedRevisionId || workspaceEpoch != expectedEpoch;
    }
};

// ---------------------------------------------------------------------------
// Index operation types
// ---------------------------------------------------------------------------

struct IndexStats {
    std::size_t chunksAdded{0};
    std::size_t chunksRemoved{0};
    std::size_t chunksUnchanged{0};
    std::size_t filesVisited{0};
    std::size_t filesIndexed{0};
    bool incomplete{false};
    std::uint64_t snapshotGeneration{0};
    double elapsedMs{0.0};
    std::vector<std::string> skippedPaths{};
};

// Caller-supplied limits for one indexing pass.
struct IndexLimits {
    std::size_t maximumFiles{10000};
    std::size_t maximumChunksPerFile{512};
    std::size_t maximumFileBytes{512 * 1024}; // 512 KB; oversized files are skipped
    int scanBudgetMs{10000};
};

// Runtime availability state for the embedding model.
enum class EmbeddingRuntimeState : std::uint8_t {
    Unloaded,
    Loading,
    Ready,
    Busy,
    Failed,
    Unavailable, // Model assets missing
};

[[nodiscard]] const char* toString(EmbeddingRuntimeState state) noexcept;

} // namespace roclint::retrieval
