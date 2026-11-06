#pragma once

#include "roclint/core/Result.hpp"
#include "roclint/storage/AstSymbol.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::storage {

struct WorkspaceContext {
    std::filesystem::path workspaceRoot{};
    std::string activeFile{};
    bool consented{false};
};

struct CodebaseIndexStats {
    std::size_t filesVisited{0};
    std::size_t filesIndexed{0};
    std::size_t filesRecovered{0};
    std::size_t symbolsIndexed{0};
    std::uint64_t generation{0};
    double elapsedMs{0.0};
    // True when this pass omitted requested paths or when dirty rows from an
    // earlier pass remain hidden.
    bool incomplete{false};
    // Workspace-relative requested paths omitted by this pass because of the
    // file cap or scan budget. They are marked dirty and remain unqueryable
    // until a later pass revalidates them.
    std::vector<std::string> unprocessedPaths{};
};

// Bounded exact query result. `truncated` is true when at least one eligible
// candidate was omitted because of the caller's limit. `complete` is false
// whenever the current snapshot coverage is incomplete. Neither a truncated
// nor an incomplete result may be used to prove uniqueness or absence.
struct SymbolQueryResult {
    std::vector<AstSymbol> symbols{};
    bool truncated{false};
    bool complete{true};
};

// Snapshot state for the bound workspace. `fullScanComplete` reflects the last
// full scan; `coverageComplete` reflects the paths coverage recorded in the
// current generation. `complete` requires both. An incremental paths refresh
// never turns an incomplete full scan into a complete one.
struct CodebaseSnapshotInfo {
    std::uint64_t generation{0};
    bool complete{false};
    bool fullScanComplete{false};
    bool coverageComplete{false};
    bool available{false};
};

// Per-refresh bounds. The defaults match the C10 contract; tests use tighter
// values to exercise incomplete-scan behavior deterministically.
struct CodebaseIndexLimits {
    std::size_t maximumFiles{5000};
    int scanBudgetMs{2000};
    std::size_t maximumFileBytes{2 * 1024 * 1024};
};

// Read-only source index. The SQLite database lives outside the workspace and
// stores only signatures, byte spans and relative paths.
//
// An instance is bound to one canonical workspace at a time. `refresh` binds to
// the refreshed root; exact-name, qualified-name, parent/child and FTS queries
// never cross that boundary. Symbol and parent identities include a workspace
// namespace so two clones may share one persistent database safely.
//
// Schema v5 adds `workspaces.coverage_complete` on top of v4's per-file
// snapshot generations (`files.snapshot_generation`, `files.dirty`). A query
// only sees rows whose file is part of the workspace's current generation and
// is not dirty; completeness additionally requires the last full scan and the
// current paths coverage to be complete.
class CodebaseIndex {
public:
    static constexpr std::size_t kMaximumIndexedFiles = 5000;
    static constexpr std::size_t kMaximumFileBytes = 2 * 1024 * 1024;
    static constexpr int kScanBudgetMs = 2000;

    explicit CodebaseIndex(std::filesystem::path databasePath,
                           std::filesystem::path workspaceRoot = {});
    ~CodebaseIndex();
    CodebaseIndex(CodebaseIndex&&) noexcept;
    CodebaseIndex& operator=(CodebaseIndex&&) noexcept;
    CodebaseIndex(const CodebaseIndex&) = delete;
    CodebaseIndex& operator=(const CodebaseIndex&) = delete;

    // Binds this instance to a canonical workspace. Queries fail closed until a
    // workspace is bound.
    [[nodiscard]] core::Status bindWorkspace(const std::filesystem::path& root);
    [[nodiscard]] bool isWorkspaceBound() const noexcept;
    [[nodiscard]] std::string workspaceRoot() const;

    // Full scan. Allocates a new generation, revalidates visited files and, on a
    // complete pass, removes files that were not seen. An incomplete pass keeps
    // unseen rows at their older generation so they stay invisible. Both
    // snapshot completeness flags record the pass result.
    [[nodiscard]] core::Result<CodebaseIndexStats> refresh(
        const WorkspaceContext& workspace, const CodebaseIndexLimits& limits = {});

    // Incremental scan of explicit workspace-relative paths. Allocates a new
    // generation, carries forward only rows that were eligible in the previous
    // generation, then re-reads/revalidates only the requested paths. Requested
    // paths omitted by the pass are marked dirty and returned in
    // `unprocessedPaths`; coverage also remains incomplete while any dirty row
    // from an earlier pass is still hidden. The pass never claims full-scan
    // completeness.
    [[nodiscard]] core::Result<CodebaseIndexStats> refreshPaths(
        const WorkspaceContext& workspace,
        const std::vector<std::string>& relativePaths,
        const CodebaseIndexLimits& limits = {});

    [[nodiscard]] core::Result<SymbolQueryResult> findByName(
        std::string_view name, std::string_view parentSymbolId = {},
        std::string_view filePath = {}, std::size_t limit = 16,
        bool allowFtsFallback = true) const;
    // Returns every exact-qualified candidate. Overloads and declaration sites
    // are retained so the grounding resolver can choose or preserve ambiguity.
    [[nodiscard]] core::Result<SymbolQueryResult> findQualified(
        std::string_view qualifiedName, std::size_t limit = 64) const;
    [[nodiscard]] core::Result<SymbolQueryResult> children(
        std::string_view parentSymbolId, std::size_t limit = 64) const;

    // Read-only hash check for a saved workspace file. Invalid paths and
    // malformed expected digests are errors; missing, unreadable or oversized
    // files return Ok(false). The index is never mutated.
    [[nodiscard]] core::Result<bool> validateFileHash(std::string_view relativePath,
                                                      std::string_view expectedHash) const;

    [[nodiscard]] core::Result<CodebaseSnapshotInfo> snapshotInfo() const;

    [[nodiscard]] core::Status forgetWorkspace(const std::filesystem::path& root);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roclint::storage
