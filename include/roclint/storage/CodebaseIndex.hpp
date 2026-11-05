#pragma once

#include "roclint/core/Result.hpp"
#include "roclint/storage/AstSymbol.hpp"

#include <cstddef>
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
    double elapsedMs{0.0};
    bool incomplete{false};
};

// Read-only source index. The SQLite database lives outside the workspace and
// stores only signatures, byte spans and relative paths.
class CodebaseIndex {
public:
    static constexpr std::size_t kMaximumIndexedFiles = 5000;
    static constexpr std::size_t kMaximumFileBytes = 2 * 1024 * 1024;
    static constexpr int kScanBudgetMs = 2000;

    explicit CodebaseIndex(std::filesystem::path databasePath);
    ~CodebaseIndex();
    CodebaseIndex(CodebaseIndex&&) noexcept;
    CodebaseIndex& operator=(CodebaseIndex&&) noexcept;
    CodebaseIndex(const CodebaseIndex&) = delete;
    CodebaseIndex& operator=(const CodebaseIndex&) = delete;

    [[nodiscard]] core::Result<CodebaseIndexStats> refresh(const WorkspaceContext& workspace);
    [[nodiscard]] core::Result<std::vector<AstSymbol>> findByName(std::string_view name,
                                                                 std::string_view parentSymbolId = {},
                                                                 std::string_view filePath = {},
                                                                 std::size_t limit = 16,
                                                                 bool allowFtsFallback = true) const;
    [[nodiscard]] core::Result<std::optional<AstSymbol>> findQualified(std::string_view qualifiedName) const;
    [[nodiscard]] core::Result<std::vector<AstSymbol>> children(std::string_view parentSymbolId,
                                                                std::size_t limit = 64) const;
    [[nodiscard]] core::Status forgetWorkspace(const std::filesystem::path& root);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roclint::storage
