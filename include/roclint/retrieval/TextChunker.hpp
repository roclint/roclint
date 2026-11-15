#pragma once

// TextChunker — structure-aware, tokenizer-aware chunking for the retrieval
// subsystem (R03).
//
// Splits source files into bounded, overlapping chunks suitable for embedding.
// C/C++ files receive AST-aware boundaries (function/class/namespace level);
// other text files receive paragraph and line-boundary chunking.
//
// Design constraints:
// - Chunk sizes are chosen for useful retrieval and downstream generation budget,
//   not to maximize embedding input utilisation.
// - Chunks preserve enough surrounding context (a heading or symbol signature)
//   to be interpretable in isolation.
// - Binary, generated, secret and dependency files must be excluded before
//   passing to the chunker; the chunker does not perform those checks itself.

#include "roclint/retrieval/RetrievalTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::retrieval {

// Chunking policy parameters.
struct ChunkerPolicy {
    // Approximate character target for one chunk (before overlap).
    std::size_t targetChunkChars{512};
    // Overlap in characters between consecutive chunks.
    std::size_t overlapChars{64};
    // Maximum chunk characters (hard limit; chunks exceeding this are split).
    std::size_t maxChunkChars{1024};
    // Minimum non-trivial chunk size (chunks smaller than this are merged or dropped).
    std::size_t minChunkChars{32};

    // Version string embedded in IndexPolicy::chunkerVersion.
    // Increment this when any parameter that affects chunking changes.
    static constexpr const char* kVersion = "text_chunker.v1.target512.overlap64";
};

// A single candidate chunk produced by the chunker.
struct Chunk {
    // Byte offsets within the source file (half-open: [byteStart, byteEnd)).
    std::uint32_t byteStart{0};
    std::uint32_t byteEnd{0};
    std::uint32_t lineStart{1};
    std::uint32_t lineEnd{1};

    std::string text{};          // The chunk text (a copy)
    std::string headingContext{}; // Leading heading, symbol signature, or scope (may be empty)
    bool hasAstHint{false};      // True when the boundary was derived from AST structure
    std::string astSymbolId{};   // sym_<prefix> when known, else empty
};

// Exclusion policy for the filesystem walker.
// Returns true when the file or directory should be excluded.
using ExclusionPredicate = std::function<bool(const std::filesystem::path& absPath, bool isDir)>;

// TextChunker: stateless, produces chunks from a file's bytes.
class TextChunker {
public:
    explicit TextChunker(ChunkerPolicy policy = {});

    // Chunk the supplied text. `filePath` is used only to pick the chunking
    // strategy (by extension); it does not perform I/O.
    [[nodiscard]] std::vector<Chunk> chunk(std::string_view text,
                                            const std::filesystem::path& filePath) const;

    [[nodiscard]] const ChunkerPolicy& policy() const noexcept { return policy_; }

    // Returns the chunker version string embedded in IndexPolicy.
    [[nodiscard]] static std::string_view version() noexcept;

private:
    // Generic text chunking (Markdown, RST, plain text, Python, etc.).
    [[nodiscard]] std::vector<Chunk> chunkText(std::string_view text) const;

    // C/C++ aware chunking: attempts top-level declaration boundaries.
    [[nodiscard]] std::vector<Chunk> chunkCpp(std::string_view text) const;

    ChunkerPolicy policy_;
};

// ---------------------------------------------------------------------------
// Workspace filesystem walker
// ---------------------------------------------------------------------------

// A single eligible source file discovered by the walker.
struct SourceFile {
    std::filesystem::path absolutePath{};
    std::string relativePath{};   // Relative to the workspace root (generic '/' separators)
    SourceKind kind{SourceKind::WorkspaceCode};
    std::uintmax_t sizeBytes{0};
};

// Result of a workspace walk.
struct WalkResult {
    std::vector<SourceFile> files{};
    // True when the iterator was exhausted before the file limit was reached.
    // False means the returned list was capped by maxFiles (scan incomplete).
    bool exhausted{false};
};

// Walk the workspace and return eligible source files.
// Respects exclusion rules: .gitignore patterns are honoured via a simple
// prefix-based filter. Binary files, generated directories, dependency trees
// (node_modules, .git, build, __pycache__, etc.) and files exceeding
// maxFileBytes are excluded.
//
// This function is intentionally synchronous and cheap: it does not hash
// or read file contents.
//
// When result.exhausted is false the scan hit the maxFiles cap: coverage
// cannot be declared complete for a full-workspace pass.
[[nodiscard]] WalkResult walkWorkspace(
    const std::filesystem::path& workspaceRoot,
    std::size_t maxFiles = 10000,
    std::size_t maxFileBytes = 512 * 1024,
    const ExclusionPredicate& extra = {});

// Backward-compatible overload: returns only the file list (use walkWorkspace
// with WalkResult to check exhaustion).
[[nodiscard]] std::vector<SourceFile> walkWorkspaceFiles(
    const std::filesystem::path& workspaceRoot,
    std::size_t maxFiles = 10000,
    std::size_t maxFileBytes = 512 * 1024,
    const ExclusionPredicate& extra = {});

} // namespace roclint::retrieval
