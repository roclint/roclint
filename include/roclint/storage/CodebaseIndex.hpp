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
    bool incomplete{false};
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

    [[nodiscard]] core::Result<CodebaseIndexStats> refresh(
        const WorkspaceContext& workspace, const CodebaseIndexLimits& limits = {});
    [[nodiscard]] core::Result<std::vector<AstSymbol>> findByName(std::string_view name,
                                                                 std::string_view parentSymbolId = {},
                                                                 std::string_view filePath = {},
                                                                 std::size_t limit = 16,
                                                                 bool allowFtsFallback = true) const;
    // Returns every exact-qualified candidate. Overloads and declaration sites
    // are retained so the grounding resolver can choose or preserve ambiguity.
    [[nodiscard]] core::Result<std::vector<AstSymbol>> findQualified(std::string_view qualifiedName,
                                                                     std::size_t limit = 64) const;
    [[nodiscard]] core::Result<std::vector<AstSymbol>> children(std::string_view parentSymbolId,
                                                                std::size_t limit = 64) const;
    [[nodiscard]] core::Status forgetWorkspace(const std::filesystem::path& root);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roclint::storage
