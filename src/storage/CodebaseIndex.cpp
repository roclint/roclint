#include "roclint/storage/CodebaseIndex.hpp"

#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/storage/AstSymbolExtractor.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace roclint::storage {
namespace {

constexpr int kSchemaVersion = 2;

const std::set<std::string> kExcludedDirectories{
    ".git", ".hg", ".svn", ".cache", "build", "dist", "out", "target", "node_modules", "vendor", "deps",
    "CMakeFiles", "__pycache__", "venv"};

bool isSourceFile(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".cpp" || extension == ".hpp" || extension == ".cc" || extension == ".hh" ||
           extension == ".cxx" || extension == ".hxx" || extension == ".c" || extension == ".h";
}

bool exec(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value != nullptr ? reinterpret_cast<const char*>(value) : std::string{};
}

int bindText(sqlite3_stmt* statement, int index, std::string_view value) {
    return sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

bool readFileText(const std::filesystem::path& path, std::string& data) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    data.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return !stream.bad();
}

std::string normalizedFtsQuery(std::string_view value) {
    std::string result;
    for (const char character : value) {
        const auto code = static_cast<unsigned char>(character);
        if (std::isalnum(code) != 0 || character == '_' || character == ':') result.push_back(character);
        else if (!result.empty() && result.back() != ' ') result.push_back(' ');
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    if (result.empty()) return result;
    return result + "*";
}

AstSymbol rowSymbol(sqlite3_stmt* statement) {
    AstSymbol symbol;
    symbol.symbolId = columnText(statement, 0);
    symbol.filePath = columnText(statement, 1);
    symbol.fileHash = columnText(statement, 2);
    symbol.name = columnText(statement, 3);
    symbol.qualifiedName = columnText(statement, 4);
    symbol.kind = static_cast<AstNodeKind>(sqlite3_column_int(statement, 5));
    symbol.parentSymbolId = columnText(statement, 6);
    symbol.signature = columnText(statement, 7);
    symbol.startLine = static_cast<std::uint32_t>(sqlite3_column_int(statement, 8));
    symbol.endLine = static_cast<std::uint32_t>(sqlite3_column_int(statement, 9));
    symbol.startColumn = static_cast<std::uint32_t>(sqlite3_column_int(statement, 10));
    symbol.endColumn = static_cast<std::uint32_t>(sqlite3_column_int(statement, 11));
    const auto offset = static_cast<std::size_t>(sqlite3_column_int64(statement, 12));
    const auto length = static_cast<std::size_t>(sqlite3_column_int64(statement, 13));
    symbol.span = {offset, offset + length};
    symbol.typeName = columnText(statement, 14);
    symbol.isConst = sqlite3_column_int(statement, 15) != 0;
    return symbol;
}

constexpr const char* kSymbolColumns =
    "symbol_id,file_path,file_hash,name,qualified_name,kind,parent_id,signature,start_line,end_line,"
    "start_column,end_column,byte_offset,byte_length,type_name,is_const";
constexpr const char* kSymbolColumnsAliased =
    "a.symbol_id,a.file_path,a.file_hash,a.name,a.qualified_name,a.kind,a.parent_id,a.signature,"
    "a.start_line,a.end_line,a.start_column,a.end_column,a.byte_offset,a.byte_length,a.type_name,a.is_const";

std::string scopedSymbolId(std::string_view workspaceKey, std::string_view symbolId) {
    return "ws_" + std::string(workspaceKey) + "_" + std::string(symbolId);
}

} // namespace

struct CodebaseIndex::Impl {
    explicit Impl(const std::filesystem::path& path) {
        if (sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            if (db != nullptr) sqlite3_close(db);
            db = nullptr;
            return;
        }
        sqlite3_busy_timeout(db, 5000);
        if (!exec(db,
                  "PRAGMA journal_mode=WAL;"
                  "PRAGMA synchronous=NORMAL;"
                  "PRAGMA foreign_keys=ON;")) {
            initError = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
            return;
        }

        int userVersion = 0;
        sqlite3_stmt* version = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &version, nullptr) == SQLITE_OK) {
            if (sqlite3_step(version) == SQLITE_ROW) userVersion = sqlite3_column_int(version, 0);
            sqlite3_finalize(version);
        }

        // The index is a cache. A schema change rebuilds it from source instead
        // of carrying old unscoped identity or columns forward.
        if (userVersion != kSchemaVersion &&
            !exec(db, "DROP TABLE IF EXISTS ast_symbols_fts;"
                      "DROP TABLE IF EXISTS ast_symbols;"
                      "DROP TABLE IF EXISTS files;"
                      "DROP TABLE IF EXISTS workspaces;")) {
            initError = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
            return;
        }

        if (!exec(db,
                  "CREATE TABLE IF NOT EXISTS workspaces(root TEXT PRIMARY KEY);"
                  "CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY, root TEXT NOT NULL, path TEXT NOT NULL,"
                  "  hash TEXT NOT NULL, UNIQUE(root,path));"
                  "CREATE TABLE IF NOT EXISTS ast_symbols("
                  "  symbol_id TEXT PRIMARY KEY, workspace_root TEXT NOT NULL,"
                  "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
                  "  file_path TEXT NOT NULL, file_hash TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL,"
                  "  kind INTEGER NOT NULL, parent_id TEXT, signature TEXT NOT NULL, start_line INTEGER NOT NULL,"
                  "  end_line INTEGER NOT NULL, start_column INTEGER NOT NULL, end_column INTEGER NOT NULL,"
                  "  byte_offset INTEGER NOT NULL, byte_length INTEGER NOT NULL, type_name TEXT NOT NULL,"
                  "  is_const INTEGER NOT NULL);"
                  "CREATE INDEX IF NOT EXISTS ast_symbols_ws_name_idx ON ast_symbols(workspace_root,name);"
                  "CREATE INDEX IF NOT EXISTS ast_symbols_ws_parent_idx ON ast_symbols(workspace_root,parent_id);"
                  "CREATE INDEX IF NOT EXISTS ast_symbols_ws_qualified_idx ON ast_symbols(workspace_root,qualified_name);"
                  "CREATE VIRTUAL TABLE IF NOT EXISTS ast_symbols_fts USING fts5("
                  "  name, qualified_name, signature, content='ast_symbols', content_rowid='rowid', tokenize='trigram');")) {
            initError = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
            return;
        }
        if (userVersion != kSchemaVersion &&
            !exec(db, "PRAGMA user_version=2;")) {
            initError = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
        }
    }

    ~Impl() {
        if (db != nullptr) sqlite3_close(db);
    }

    bool bind(std::string root) {
        workspaceRoot = std::move(root);
        workspaceKey = core::crypto::Sha256::hexDigest(workspaceRoot).substr(0, 16);
        bound = true;
        return true;
    }

    sqlite3* db{nullptr};
    std::string initError;
    std::string workspaceRoot;
    std::string workspaceKey;
    bool bound{false};
};

CodebaseIndex::CodebaseIndex(std::filesystem::path databasePath, std::filesystem::path workspaceRoot)
    : impl_(std::make_unique<Impl>(databasePath)) {
    if (impl_ && impl_->db != nullptr && !workspaceRoot.empty())
        static_cast<void>(bindWorkspace(workspaceRoot));
}
CodebaseIndex::~CodebaseIndex() = default;
CodebaseIndex::CodebaseIndex(CodebaseIndex&&) noexcept = default;
CodebaseIndex& CodebaseIndex::operator=(CodebaseIndex&&) noexcept = default;

core::Status CodebaseIndex::bindWorkspace(const std::filesystem::path& root) {
    if (!impl_ || impl_->db == nullptr)
        return core::makeError(core::ErrorCode::DbError,
                               impl_ && !impl_->initError.empty() ? impl_->initError : "Code index database is unavailable");
    if (root.empty()) return core::makeError(core::ErrorCode::InvalidInput, "Workspace root is empty");
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error || canonical.empty())
        return core::makeError(core::ErrorCode::InvalidInput, "Workspace path is unavailable");
    impl_->bind(canonical.generic_string());
    return core::Status::success();
}

bool CodebaseIndex::isWorkspaceBound() const noexcept {
    return impl_ && impl_->db != nullptr && impl_->bound;
}

std::string CodebaseIndex::workspaceRoot() const {
    return impl_ ? impl_->workspaceRoot : std::string{};
}

core::Result<CodebaseIndexStats> CodebaseIndex::refresh(const WorkspaceContext& workspace,
                                                        const CodebaseIndexLimits& limits) {
    if (!impl_ || impl_->db == nullptr) {
        return core::makeError(core::ErrorCode::DbError,
                               impl_ && !impl_->initError.empty() ? impl_->initError : "Code index database is unavailable");
    }
    if (!workspace.workspaceRoot.empty()) {
        std::error_code identityError;
        const auto requested = std::filesystem::weakly_canonical(workspace.workspaceRoot, identityError);
        if (!identityError && impl_->bound && requested.generic_string() != impl_->workspaceRoot) {
            // A refresh attempt for another workspace must never leave the
            // previous workspace's evidence queryable.
            impl_->workspaceRoot.clear();
            impl_->workspaceKey.clear();
            impl_->bound = false;
        }
    }
    if (!workspace.consented) {
        return core::makeError(core::ErrorCode::PreconditionFailed,
                               "Code indexing requires explicit workspace consent");
    }
    std::error_code error;
    const auto root = std::filesystem::canonical(workspace.workspaceRoot, error);
    if (error || !std::filesystem::is_directory(root, error)) {
        return core::makeError(core::ErrorCode::InvalidInput, "Selected workspace is unavailable");
    }
    const auto started = std::chrono::steady_clock::now();
    const auto rootText = root.generic_string();
    // Bind before the transaction starts so a failed refresh can never leave the
    // instance answering from the previously selected workspace.
    impl_->bind(rootText);
    const auto& workspaceKey = impl_->workspaceKey;
    CodebaseIndexStats stats;
    if (!exec(impl_->db, "BEGIN IMMEDIATE;")) {
        return core::makeError(core::ErrorCode::DbError, "Unable to start code index transaction");
    }
    auto rollback = [&]() { static_cast<void>(exec(impl_->db, "ROLLBACK;")); };
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "INSERT OR IGNORE INTO workspaces(root) VALUES(?1);", -1, &statement, nullptr) != SQLITE_OK) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to register workspace");
    }
    bindText(statement, 1, rootText);
    static_cast<void>(sqlite3_step(statement));
    sqlite3_finalize(statement);

    auto findFile = [&](std::string_view relative, std::int64_t& fileId, std::string& existingHash) -> bool {
        fileId = 0;
        existingHash.clear();
        sqlite3_stmt* find = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "SELECT id,hash FROM files WHERE root=?1 AND path=?2;", -1, &find, nullptr) != SQLITE_OK)
            return false;
        bindText(find, 1, rootText);
        bindText(find, 2, relative);
        const bool found = sqlite3_step(find) == SQLITE_ROW;
        if (found) {
            fileId = sqlite3_column_int64(find, 0);
            existingHash = columnText(find, 1);
        }
        sqlite3_finalize(find);
        return true;
    };
    auto upsertFile = [&](std::string_view relative, std::string_view hash, std::int64_t& fileId) -> bool {
        std::string existingHash;
        if (!findFile(relative, fileId, existingHash)) return false;
        if (fileId == 0) {
            sqlite3_stmt* insert = nullptr;
            if (sqlite3_prepare_v2(impl_->db, "INSERT INTO files(root,path,hash) VALUES(?1,?2,?3);", -1, &insert, nullptr) != SQLITE_OK)
                return false;
            bindText(insert, 1, rootText);
            bindText(insert, 2, relative);
            bindText(insert, 3, hash);
            const bool ok = sqlite3_step(insert) == SQLITE_DONE;
            sqlite3_finalize(insert);
            if (!ok) return false;
            fileId = sqlite3_last_insert_rowid(impl_->db);
            return true;
        }
        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "UPDATE files SET hash=?1 WHERE id=?2;", -1, &update, nullptr) != SQLITE_OK)
            return false;
        bindText(update, 1, hash);
        sqlite3_bind_int64(update, 2, fileId);
        const bool ok = sqlite3_step(update) == SQLITE_DONE;
        sqlite3_finalize(update);
        return ok;
    };
    auto deleteSymbols = [&](std::int64_t fileId) -> bool {
        sqlite3_stmt* remove = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "DELETE FROM ast_symbols WHERE file_id=?1;", -1, &remove, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(remove, 1, fileId);
        const bool ok = sqlite3_step(remove) == SQLITE_DONE;
        sqlite3_finalize(remove);
        return ok;
    };
    auto invalidateFile = [&](std::string_view relative) -> bool {
        sqlite3_stmt* symbols = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                               "DELETE FROM ast_symbols WHERE workspace_root=?1 AND file_path=?2;",
                               -1, &symbols, nullptr) != SQLITE_OK)
            return false;
        bindText(symbols, 1, rootText);
        bindText(symbols, 2, relative);
        const bool symbolsDeleted = sqlite3_step(symbols) == SQLITE_DONE;
        sqlite3_finalize(symbols);
        if (!symbolsDeleted) return false;
        sqlite3_stmt* remove = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "DELETE FROM files WHERE root=?1 AND path=?2;", -1, &remove, nullptr) != SQLITE_OK)
            return false;
        bindText(remove, 1, rootText);
        bindText(remove, 2, relative);
        const bool fileDeleted = sqlite3_step(remove) == SQLITE_DONE;
        sqlite3_finalize(remove);
        return fileDeleted;
    };

    std::unordered_set<std::string> seen;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(root, options), end;
    auto invalidateObservedSource = [&]() -> bool {
        const auto relative = std::filesystem::relative(iterator->path(), root, error);
        if (error || relative.empty() || relative.generic_string().starts_with("..")) {
            error.clear();
            stats.incomplete = true;
            return true;
        }
        const auto relativeText = relative.generic_string();
        seen.insert(relativeText);
        return invalidateFile(relativeText);
    };
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            // An unreadable subtree could hide files; never treat unvisited paths
            // as confirmed deletions for this pass.
            stats.incomplete = true;
            error.clear();
            continue;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count() >= limits.scanBudgetMs) {
            stats.incomplete = true;
            break;
        }
        std::error_code statusError;
        const bool symlink = iterator->is_symlink(statusError);
        if (statusError) {
            statusError.clear();
            stats.incomplete = true;
            if (isSourceFile(iterator->path()) && !invalidateObservedSource()) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unavailable file");
            }
            continue;
        }
        if (symlink) {
            std::error_code directoryError;
            if (iterator->is_directory(directoryError)) iterator.disable_recursion_pending();
            directoryError.clear();
            if (isSourceFile(iterator->path()) && !invalidateObservedSource()) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate symlinked file");
            }
            continue;
        }
        const bool directory = iterator->is_directory(statusError);
        if (statusError) {
            statusError.clear();
            stats.incomplete = true;
            if (isSourceFile(iterator->path()) && !invalidateObservedSource()) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unavailable file");
            }
            continue;
        }
        if (directory) {
            if (isSourceFile(iterator->path()) && !invalidateObservedSource()) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate directory-shaped file");
            }
            if (kExcludedDirectories.contains(iterator->path().filename().string())) iterator.disable_recursion_pending();
            continue;
        }
        const bool regular = iterator->is_regular_file(statusError);
        if (statusError) {
            statusError.clear();
            stats.incomplete = true;
            if (isSourceFile(iterator->path()) && !invalidateObservedSource()) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unreadable file");
            }
            continue;
        }
        if (!regular) {
            if (isSourceFile(iterator->path()) && !invalidateObservedSource()) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate special file");
            }
            continue;
        }
        if (!isSourceFile(iterator->path())) continue;
        ++stats.filesVisited;
        if (stats.filesVisited > limits.maximumFiles) {
            stats.incomplete = true;
            break;
        }
        const auto relative = std::filesystem::relative(iterator->path(), root, error);
        if (error || relative.empty() || relative.generic_string().starts_with("..")) {
            stats.incomplete = true;
            error.clear();
            continue;
        }
        const auto relativeText = relative.generic_string();
        seen.insert(relativeText);

        const auto size = std::filesystem::file_size(iterator->path(), error);
        if (error || size > limits.maximumFileBytes) {
            error.clear();
            if (!invalidateFile(relativeText)) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate indexed file");
            }
            continue;
        }
        std::string source;
        if (!readFileText(iterator->path(), source)) {
            if (!invalidateFile(relativeText)) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unreadable file");
            }
            continue;
        }
        const auto fileHash = core::crypto::Sha256::hexDigest(source);

        std::int64_t fileId = 0;
        std::string existingHash;
        if (!findFile(relativeText, fileId, existingHash)) {
            rollback();
            return core::makeError(core::ErrorCode::DbError, "Unable to inspect indexed file");
        }
        if (fileId != 0 && existingHash == fileHash) continue;

        const auto extraction = AstSymbolExtractor::extractSymbols(source, relativeText);
        if (!upsertFile(relativeText, fileHash, fileId)) {
            rollback();
            return core::makeError(core::ErrorCode::DbError, "Unable to persist indexed file");
        }
        if (!deleteSymbols(fileId)) {
            rollback();
            return core::makeError(core::ErrorCode::DbError, "Unable to replace indexed symbols");
        }
        ++stats.filesIndexed;
        if (extraction.recovered) ++stats.filesRecovered;
        if (!extraction.parseOk && !extraction.recovered) {
            // The current bytes are known, but unusable. Keep the new hash with
            // an empty symbol snapshot so stale evidence cannot survive.
            continue;
        }

        std::unordered_map<std::string, std::string> scopedIds;
        scopedIds.reserve(extraction.symbols.size());
        for (const auto& symbol : extraction.symbols)
            scopedIds.emplace(symbol.symbolId, scopedSymbolId(workspaceKey, symbol.symbolId));

        sqlite3_stmt* insert = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                               "INSERT INTO ast_symbols(symbol_id,workspace_root,file_id,file_path,file_hash,name,"
                               "qualified_name,kind,parent_id,signature,start_line,end_line,start_column,end_column,"
                               "byte_offset,byte_length,type_name,is_const)"
                               " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);",
                               -1, &insert, nullptr) != SQLITE_OK) {
            rollback();
            return core::makeError(core::ErrorCode::DbError, "Unable to prepare symbol insert");
        }
        for (const auto& symbol : extraction.symbols) {
            const auto scoped = scopedIds.find(symbol.symbolId);
            const auto parent = symbol.parentSymbolId.empty()
                ? std::string{} : scopedIds.find(symbol.parentSymbolId) != scopedIds.end()
                    ? scopedIds.at(symbol.parentSymbolId) : std::string{};
            if (scoped == scopedIds.end()) continue;
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
            bindText(insert, 1, scoped->second);
            bindText(insert, 2, rootText);
            sqlite3_bind_int64(insert, 3, fileId);
            bindText(insert, 4, symbol.filePath);
            bindText(insert, 5, fileHash);
            bindText(insert, 6, symbol.name);
            bindText(insert, 7, symbol.qualifiedName);
            sqlite3_bind_int(insert, 8, static_cast<int>(symbol.kind));
            bindText(insert, 9, parent);
            bindText(insert, 10, symbol.signature);
            sqlite3_bind_int64(insert, 11, symbol.startLine);
            sqlite3_bind_int64(insert, 12, symbol.endLine);
            sqlite3_bind_int64(insert, 13, symbol.startColumn);
            sqlite3_bind_int64(insert, 14, symbol.endColumn);
            sqlite3_bind_int64(insert, 15, static_cast<sqlite3_int64>(symbol.span.startByte));
            sqlite3_bind_int64(insert, 16, static_cast<sqlite3_int64>(symbol.span.length()));
            bindText(insert, 17, symbol.typeName);
            sqlite3_bind_int(insert, 18, symbol.isConst ? 1 : 0);
            if (sqlite3_step(insert) != SQLITE_DONE) {
                sqlite3_finalize(insert);
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to insert AST symbol");
            }
            ++stats.symbolsIndexed;
        }
        sqlite3_finalize(insert);
    }

    if (!stats.incomplete) {
        sqlite3_stmt* files = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "SELECT id,path FROM files WHERE root=?1;", -1, &files, nullptr) != SQLITE_OK) {
            rollback();
            return core::makeError(core::ErrorCode::DbError, "Unable to enumerate indexed files");
        }
        bindText(files, 1, rootText);
        std::vector<std::int64_t> stale;
        while (sqlite3_step(files) == SQLITE_ROW) {
            if (!seen.contains(columnText(files, 1))) stale.push_back(sqlite3_column_int64(files, 0));
        }
        sqlite3_finalize(files);
        for (const auto id : stale) {
            sqlite3_stmt* symbols = nullptr;
            if (sqlite3_prepare_v2(impl_->db, "DELETE FROM ast_symbols WHERE file_id=?1;", -1, &symbols, nullptr) != SQLITE_OK) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to remove stale indexed symbols");
            }
            sqlite3_bind_int64(symbols, 1, id);
            const bool symbolsDeleted = sqlite3_step(symbols) == SQLITE_DONE;
            sqlite3_finalize(symbols);
            if (!symbolsDeleted) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to remove stale indexed symbols");
            }
            sqlite3_stmt* remove = nullptr;
            if (sqlite3_prepare_v2(impl_->db, "DELETE FROM files WHERE id=?1;", -1, &remove, nullptr) != SQLITE_OK) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to remove stale indexed file");
            }
            sqlite3_bind_int64(remove, 1, id);
            const bool ok = sqlite3_step(remove) == SQLITE_DONE;
            sqlite3_finalize(remove);
            if (!ok) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to remove stale indexed file");
            }
        }
    }

    if (!exec(impl_->db, "INSERT INTO ast_symbols_fts(ast_symbols_fts) VALUES('rebuild');")) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to rebuild the symbol FTS index");
    }
    if (!exec(impl_->db, "COMMIT;")) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to commit the code index");
    }
    stats.elapsedMs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count()) / 1000.0;
    return stats;
}

core::Result<std::vector<AstSymbol>> CodebaseIndex::findByName(std::string_view name,
                                                               std::string_view parentSymbolId,
                                                               std::string_view filePath,
                                                               std::size_t limit,
                                                               bool allowFtsFallback) const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    if (!impl_->bound) return core::makeError(core::ErrorCode::PreconditionFailed, "Code index has no bound workspace");
    const auto bounded = std::min<std::size_t>(std::max<std::size_t>(limit, 1), 64);
    std::string sql = std::string("SELECT ") + kSymbolColumns +
        " FROM ast_symbols WHERE workspace_root=?1 AND name=?2";
    int bindIndex = 3;
    if (!parentSymbolId.empty()) sql += " AND parent_id=?" + std::to_string(bindIndex++);
    if (!filePath.empty()) sql += " AND file_path=?" + std::to_string(bindIndex++);
    sql += " LIMIT ?" + std::to_string(bindIndex);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return core::makeError(core::ErrorCode::DbError, "Unable to query AST symbols");
    bindText(statement, 1, impl_->workspaceRoot);
    bindText(statement, 2, name);
    int index = 3;
    if (!parentSymbolId.empty()) bindText(statement, index++, parentSymbolId);
    if (!filePath.empty()) bindText(statement, index++, filePath);
    sqlite3_bind_int(statement, index, static_cast<int>(bounded));
    std::vector<AstSymbol> symbols;
    while (sqlite3_step(statement) == SQLITE_ROW) symbols.push_back(rowSymbol(statement));
    sqlite3_finalize(statement);
    if (!symbols.empty() || !allowFtsFallback) return symbols;

    const auto query = normalizedFtsQuery(name);
    if (query.empty()) return symbols;
    std::string ftsSql = "SELECT " + std::string(kSymbolColumnsAliased) +
        " FROM ast_symbols_fts f JOIN ast_symbols a ON a.rowid=f.rowid"
        " WHERE ast_symbols_fts MATCH ?1 AND a.workspace_root=?2"
        " ORDER BY bm25(ast_symbols_fts) LIMIT ?3";
    if (sqlite3_prepare_v2(impl_->db, ftsSql.c_str(), -1, &statement, nullptr) != SQLITE_OK) return symbols;
    bindText(statement, 1, query);
    bindText(statement, 2, impl_->workspaceRoot);
    sqlite3_bind_int(statement, 3, static_cast<int>(bounded));
    while (sqlite3_step(statement) == SQLITE_ROW) symbols.push_back(rowSymbol(statement));
    sqlite3_finalize(statement);
    return symbols;
}

core::Result<std::vector<AstSymbol>> CodebaseIndex::findQualified(std::string_view qualifiedName,
                                                                  std::size_t limit) const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    if (!impl_->bound) return core::makeError(core::ErrorCode::PreconditionFailed, "Code index has no bound workspace");
    const auto bounded = std::min<std::size_t>(std::max<std::size_t>(limit, 1), 64);
    const std::string sql = std::string("SELECT ") + kSymbolColumns +
        " FROM ast_symbols WHERE workspace_root=?1 AND qualified_name=?2"
        " ORDER BY file_path,byte_offset LIMIT ?3;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return core::makeError(core::ErrorCode::DbError, "Unable to query AST symbol");
    bindText(statement, 1, impl_->workspaceRoot);
    bindText(statement, 2, qualifiedName);
    sqlite3_bind_int(statement, 3, static_cast<int>(bounded));
    std::vector<AstSymbol> symbols;
    while (sqlite3_step(statement) == SQLITE_ROW) symbols.push_back(rowSymbol(statement));
    sqlite3_finalize(statement);
    return symbols;
}

core::Result<std::vector<AstSymbol>> CodebaseIndex::children(std::string_view parentSymbolId,
                                                             std::size_t limit) const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    if (!impl_->bound) return core::makeError(core::ErrorCode::PreconditionFailed, "Code index has no bound workspace");
    const auto bounded = std::min<std::size_t>(std::max<std::size_t>(limit, 1), 128);
    const std::string sql = std::string("SELECT ") + kSymbolColumns +
        " FROM ast_symbols WHERE workspace_root=?1 AND parent_id=?2 ORDER BY byte_offset LIMIT ?3;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return core::makeError(core::ErrorCode::DbError, "Unable to query AST children");
    bindText(statement, 1, impl_->workspaceRoot);
    bindText(statement, 2, parentSymbolId);
    sqlite3_bind_int(statement, 3, static_cast<int>(bounded));
    std::vector<AstSymbol> symbols;
    while (sqlite3_step(statement) == SQLITE_ROW) symbols.push_back(rowSymbol(statement));
    sqlite3_finalize(statement);
    return symbols;
}

core::Status CodebaseIndex::forgetWorkspace(const std::filesystem::path& root) {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(root, error).generic_string();
    if (error) return core::makeError(core::ErrorCode::InvalidInput, "Workspace path is unavailable");
    sqlite3_stmt* workspace = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM workspaces WHERE root=?1;", -1, &workspace, nullptr) != SQLITE_OK)
        return core::makeError(core::ErrorCode::DbError, "Unable to forget workspace");
    bindText(workspace, 1, normalized);
    static_cast<void>(sqlite3_step(workspace));
    sqlite3_finalize(workspace);
    sqlite3_stmt* symbols = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM ast_symbols WHERE workspace_root=?1;", -1,
                           &symbols, nullptr) != SQLITE_OK)
        return core::makeError(core::ErrorCode::DbError, "Unable to forget workspace symbols");
    bindText(symbols, 1, normalized);
    static_cast<void>(sqlite3_step(symbols));
    sqlite3_finalize(symbols);
    sqlite3_stmt* files = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM files WHERE root=?1;", -1, &files, nullptr) != SQLITE_OK)
        return core::makeError(core::ErrorCode::DbError, "Unable to forget workspace files");
    bindText(files, 1, normalized);
    static_cast<void>(sqlite3_step(files));
    sqlite3_finalize(files);
    if (!exec(impl_->db, "INSERT INTO ast_symbols_fts(ast_symbols_fts) VALUES('rebuild');"))
        return core::makeError(core::ErrorCode::DbError, "Unable to rebuild the symbol FTS index");
    if (impl_->bound && impl_->workspaceRoot == normalized) {
        impl_->workspaceRoot.clear();
        impl_->workspaceKey.clear();
        impl_->bound = false;
    }
    return core::Status::success();
}

} // namespace roclint::storage
