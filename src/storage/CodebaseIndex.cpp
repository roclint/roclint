#include "roclint/storage/CodebaseIndex.hpp"

#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/storage/AstSymbolExtractor.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace roclint::storage {
namespace {

constexpr int kSchemaVersion = 5;
constexpr std::size_t kMaximumQueryLimit = 1024;

void markIncomplete(CodebaseIndexStats& stats, CodebaseIndexIncompleteReason reason) {
    stats.incomplete = true;
    if (stats.incompleteReason == CodebaseIndexIncompleteReason::None)
        stats.incompleteReason = reason;
}

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

bool readFileBounded(const std::filesystem::path& path, std::size_t maximumBytes, std::string& data) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    data.clear();
    constexpr std::size_t kChunkBytes = 64 * 1024;
    char buffer[kChunkBytes];
    while (stream) {
        stream.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
        const auto read = stream.gcount();
        if (read <= 0) break;
        const auto count = static_cast<std::size_t>(read);
        if (data.size() > maximumBytes || count > maximumBytes - data.size()) return false;
        data.append(buffer, count);
    }
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

std::string normalizeRelativeText(std::string_view value) {
    std::string result(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    while (result.size() >= 2 && result[0] == '.' && result[1] == '/') result.erase(0, 2);
    return result;
}

std::string lexicallyNormalized(std::string_view value) {
    auto result = std::filesystem::path(std::string(value)).lexically_normal().generic_string();
    while (result.size() >= 2 && result[0] == '.' && result[1] == '/') result.erase(0, 2);
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    if (result == ".") result.clear();
    return result;
}

bool hasParentEscape(std::string_view normalized) {
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const auto separator = normalized.find('/', start);
        const auto end = separator == std::string_view::npos ? normalized.size() : separator;
        if (normalized.substr(start, end - start) == "..") return true;
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return false;
}

bool isSafeRelativeText(std::string_view relative) {
    if (relative.empty() || relative.find('\0') != std::string_view::npos) return false;
    const std::filesystem::path candidate{std::string(relative)};
    if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory()) return false;
    return !hasParentEscape(relative);
}

bool isWithinRoot(std::string_view root, std::string_view candidate) {
    if (root.empty() || candidate.empty()) return false;
    std::string normalizedRoot(root);
    std::string normalizedCandidate(candidate);
#ifdef _WIN32
    const auto fold = [](std::string& value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    };
    fold(normalizedRoot);
    fold(normalizedCandidate);
#endif
    if (normalizedRoot.back() != '/') normalizedRoot.push_back('/');
    return normalizedCandidate.size() > normalizedRoot.size() &&
           normalizedCandidate.compare(0, normalizedRoot.size(), normalizedRoot) == 0;
}

// Resolves a workspace-relative path without allowing absolute paths, drive
// qualifiers, `..` escapes, or symlink/reparse-point escapes from the canonical
// workspace root. The returned absolute path is canonical for existing files
// and canonicalized up to the deepest existing parent for new files.
bool resolveWorkspaceRelative(const std::filesystem::path& root, std::string_view rawRelative,
                              std::string& normalized, std::filesystem::path& absolute) {
    const auto raw = normalizeRelativeText(rawRelative);
    if (!isSafeRelativeText(raw)) return false;
    normalized = lexicallyNormalized(raw);
    if (!isSafeRelativeText(normalized)) return false;
    const auto lexical = root / std::filesystem::path(normalized);
    std::filesystem::path probe = root;
    for (const auto& component : std::filesystem::path(normalized)) {
        probe /= component;
        std::error_code symlinkError;
        if (std::filesystem::is_symlink(probe, symlinkError) && !symlinkError) return false;
        if (symlinkError) break;
    }
    std::error_code error;
    error.clear();
    const auto canonical = std::filesystem::weakly_canonical(lexical, error);
    if (error || canonical.empty()) return false;
    if (!isWithinRoot(root.generic_string(), canonical.generic_string())) return false;
    std::error_code relativeError;
    const auto relative = std::filesystem::relative(canonical, root, relativeError);
    if (relativeError || relative.empty() || relative == "." || !isSafeRelativeText(relative.generic_string()))
        return false;
    normalized = relative.generic_string();
    absolute = canonical;
    return true;
}

bool isHexDigest(std::string_view value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

std::string normalizeExpectedDigest(std::string_view value) {
    std::string result(value);
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.front())) != 0) result.erase(result.begin());
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())) != 0) result.pop_back();
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (result.size() > 7 && result.compare(0, 7, "sha256:") == 0) result.erase(0, 7);
    return result;
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
    symbol.role = static_cast<AstSymbolRole>(sqlite3_column_int(statement, 16));
    symbol.isDeclaration = sqlite3_column_int(statement, 17) != 0;
    symbol.isDefinition = sqlite3_column_int(statement, 18) != 0;
    symbol.isTemplate = sqlite3_column_int(statement, 19) != 0;
    symbol.isStatic = sqlite3_column_int(statement, 20) != 0;
    symbol.isConstexpr = sqlite3_column_int(statement, 21) != 0;
    symbol.isInline = sqlite3_column_int(statement, 22) != 0;
    symbol.isVariadic = sqlite3_column_int(statement, 23) != 0;
    symbol.isReference = sqlite3_column_int(statement, 24) != 0;
    symbol.parameterIndex = static_cast<std::uint16_t>(sqlite3_column_int(statement, 25));
    symbol.signatureKey = columnText(statement, 26);
    symbol.qualifiers = columnText(statement, 27);
    return symbol;
}

constexpr const char* kSymbolColumns =
    "a.symbol_id,a.file_path,a.file_hash,a.name,a.qualified_name,a.kind,a.parent_id,a.signature,"
    "a.start_line,a.end_line,a.start_column,a.end_column,a.byte_offset,a.byte_length,a.type_name,a.is_const,"
    "a.role,a.is_declaration,a.is_definition,a.is_template,a.is_static,a.is_constexpr,a.is_inline,"
    "a.is_variadic,a.is_reference,a.parameter_index,a.signature_key,a.qualifiers";

constexpr const char* kSymbolInsertSql =
    "INSERT INTO ast_symbols(symbol_id,workspace_root,validated_generation,file_id,"
    "file_path,file_hash,name,qualified_name,kind,parent_id,signature,start_line,end_line,"
    "start_column,end_column,byte_offset,byte_length,type_name,is_const,"
    "role,is_declaration,is_definition,is_template,is_static,is_constexpr,is_inline,"
    "is_variadic,is_reference,parameter_index,signature_key,qualifiers)"
    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,"
    "?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31);";

constexpr const char* kJoinFilter =
    " FROM ast_symbols a"
    " JOIN files f ON f.id=a.file_id"
    " JOIN workspaces w ON w.root=a.workspace_root"
    " WHERE a.workspace_root=?1 AND f.root=?1 AND f.snapshot_generation=w.generation AND f.dirty=0";

std::string scopedSymbolId(std::string_view workspaceKey, std::string_view symbolId) {
    return "ws_" + std::string(workspaceKey) + "_" + std::string(symbolId);
}

std::size_t boundedQueryLimit(std::size_t limit) {
    return std::min<std::size_t>(std::max<std::size_t>(limit, 1), kMaximumQueryLimit);
}

SymbolQueryResult finalizedResult(std::vector<AstSymbol> symbols, std::size_t limit, bool complete) {
    SymbolQueryResult result;
    if (symbols.size() > limit) {
        symbols.resize(limit);
        result.truncated = true;
    }
    result.symbols = std::move(symbols);
    result.complete = complete;
    return result;
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

        // The index is a cache. Version 3 can be migrated in place so that a
        // database shared with unrelated history tables keeps those tables,
        // but its evidence is invalidated because v4 adds structural columns
        // that require a fresh parse. Version 4 gains the coverage flag; its
        // value is reconstructed from the last full-scan claim and dirty rows.
        // Older or malformed schemas are rebuilt.
        bool rebuild = false;
        if (userVersion == 3) {
            if (!migrateV3ToV4()) rebuild = true;
            else if (!migrateV4ToV5()) rebuild = true;
        } else if (userVersion == 4) {
            if (!migrateV4ToV5()) rebuild = true;
        } else if (userVersion != kSchemaVersion) {
            rebuild = true;
        }
        if (rebuild && !dropIndexTables()) {
            initError = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
            return;
        }
        if (!createSchema()) {
            initError = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
            return;
        }
        if (!exec(db, "PRAGMA user_version=5;")) {
            initError = sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
        }
    }

    ~Impl() {
        if (db != nullptr) sqlite3_close(db);
    }

    bool migrateV3ToV4() {
        if (!exec(db, "BEGIN IMMEDIATE;")) return false;
        bool ok = exec(db,
                       "ALTER TABLE files ADD COLUMN snapshot_generation INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE files ADD COLUMN dirty INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN role INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_declaration INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_definition INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_template INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_static INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_constexpr INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_inline INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_variadic INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN is_reference INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN parameter_index INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE ast_symbols ADD COLUMN signature_key TEXT NOT NULL DEFAULT '';"
                       "ALTER TABLE ast_symbols ADD COLUMN qualifiers TEXT NOT NULL DEFAULT '';");
        if (ok) {
            // v3 rows have no structural metadata. Keep them as diagnostics but
            // hide them, and force the next scan to re-read the bytes.
            ok = exec(db,
                      "UPDATE files SET snapshot_generation=0,dirty=1,hash='';"
                      "UPDATE ast_symbols SET validated_generation=0;"
                      "UPDATE workspaces SET last_refresh_complete=0;");
        }
        if (ok) ok = exec(db, "PRAGMA user_version=4;");
        if (ok) ok = exec(db, "COMMIT;");
        if (!ok) static_cast<void>(exec(db, "ROLLBACK;"));
        return ok;
    }

    bool migrateV4ToV5() {
        if (!exec(db, "BEGIN IMMEDIATE;")) return false;
        bool ok = exec(db,
                       "ALTER TABLE workspaces ADD COLUMN coverage_complete INTEGER NOT NULL DEFAULT 0;");
        if (ok) {
            // v4 had no separate coverage flag. A workspace whose last full scan
            // completed and that has no dirty rows was fully covered; anything
            // else must stay incomplete.
            ok = exec(db,
                      "UPDATE workspaces SET coverage_complete=CASE"
                      " WHEN last_refresh_complete<>0 AND NOT EXISTS("
                      "  SELECT 1 FROM files f WHERE f.root=workspaces.root AND f.dirty<>0)"
                      " THEN 1 ELSE 0 END;");
        }
        if (ok) ok = exec(db, "PRAGMA user_version=5;");
        if (ok) ok = exec(db, "COMMIT;");
        if (!ok) static_cast<void>(exec(db, "ROLLBACK;"));
        return ok;
    }

    bool dropIndexTables() {
        return exec(db,
                    "DROP TABLE IF EXISTS ast_symbols_fts;"
                    "DROP TABLE IF EXISTS ast_symbols;"
                    "DROP TABLE IF EXISTS files;"
                    "DROP TABLE IF EXISTS workspaces;");
    }

    bool createSchema() {
        return exec(db,
                    "CREATE TABLE IF NOT EXISTS workspaces("
                    "  root TEXT PRIMARY KEY, generation INTEGER NOT NULL DEFAULT 0,"
                    "  last_refresh_complete INTEGER NOT NULL DEFAULT 0,"
                    "  coverage_complete INTEGER NOT NULL DEFAULT 0);"
                    "CREATE TABLE IF NOT EXISTS files("
                    "  id INTEGER PRIMARY KEY, root TEXT NOT NULL, path TEXT NOT NULL, hash TEXT NOT NULL,"
                    "  validated_generation INTEGER NOT NULL DEFAULT 0,"
                    "  snapshot_generation INTEGER NOT NULL DEFAULT 0,"
                    "  dirty INTEGER NOT NULL DEFAULT 0, UNIQUE(root,path));"
                    "CREATE TABLE IF NOT EXISTS ast_symbols("
                    "  symbol_id TEXT PRIMARY KEY, workspace_root TEXT NOT NULL,"
                    "  validated_generation INTEGER NOT NULL,"
                    "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
                    "  file_path TEXT NOT NULL, file_hash TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL,"
                    "  kind INTEGER NOT NULL, parent_id TEXT, signature TEXT NOT NULL, start_line INTEGER NOT NULL,"
                    "  end_line INTEGER NOT NULL, start_column INTEGER NOT NULL, end_column INTEGER NOT NULL,"
                    "  byte_offset INTEGER NOT NULL, byte_length INTEGER NOT NULL, type_name TEXT NOT NULL,"
                    "  is_const INTEGER NOT NULL,"
                    "  role INTEGER NOT NULL DEFAULT 0, is_declaration INTEGER NOT NULL DEFAULT 0,"
                    "  is_definition INTEGER NOT NULL DEFAULT 0, is_template INTEGER NOT NULL DEFAULT 0,"
                    "  is_static INTEGER NOT NULL DEFAULT 0, is_constexpr INTEGER NOT NULL DEFAULT 0,"
                    "  is_inline INTEGER NOT NULL DEFAULT 0, is_variadic INTEGER NOT NULL DEFAULT 0,"
                    "  is_reference INTEGER NOT NULL DEFAULT 0, parameter_index INTEGER NOT NULL DEFAULT 0,"
                    "  signature_key TEXT NOT NULL DEFAULT '', qualifiers TEXT NOT NULL DEFAULT '');"
                    "CREATE INDEX IF NOT EXISTS files_ws_snapshot_idx"
                    "  ON files(root,snapshot_generation,dirty);"
                    "CREATE INDEX IF NOT EXISTS ast_symbols_file_idx ON ast_symbols(file_id);"
                    "CREATE INDEX IF NOT EXISTS ast_symbols_ws_name_v4_idx"
                    "  ON ast_symbols(workspace_root,name);"
                    "CREATE INDEX IF NOT EXISTS ast_symbols_ws_parent_v4_idx"
                    "  ON ast_symbols(workspace_root,parent_id);"
                    "CREATE INDEX IF NOT EXISTS ast_symbols_ws_qualified_v4_idx"
                    "  ON ast_symbols(workspace_root,qualified_name);"
                    "CREATE VIRTUAL TABLE IF NOT EXISTS ast_symbols_fts USING fts5("
                    "  name, qualified_name, signature, content='ast_symbols', content_rowid='rowid', tokenize='trigram');");
    }

    bool bind(std::string root) {
        workspaceRoot = std::move(root);
        workspaceKey = core::crypto::Sha256::hexDigest(workspaceRoot).substr(0, 16);
        bound = true;
        return true;
    }

    bool ensureWorkspace(std::string_view root) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO workspaces(root) VALUES(?1);", -1, &statement, nullptr) != SQLITE_OK)
            return false;
        bindText(statement, 1, root);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return ok;
    }

    bool readGeneration(std::string_view root, std::int64_t& generation, bool& fullScanComplete,
                        bool& coverageComplete, bool& exists) {
        generation = 0;
        fullScanComplete = false;
        coverageComplete = false;
        exists = false;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT generation,last_refresh_complete,coverage_complete"
                               " FROM workspaces WHERE root=?1;",
                               -1, &statement, nullptr) != SQLITE_OK)
            return false;
        bindText(statement, 1, root);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            generation = sqlite3_column_int64(statement, 0);
            fullScanComplete = sqlite3_column_int(statement, 1) != 0;
            coverageComplete = sqlite3_column_int(statement, 2) != 0;
            exists = true;
        }
        sqlite3_finalize(statement);
        return true;
    }

    bool findFile(std::string_view root, std::string_view relative, std::int64_t& fileId, std::string& existingHash) {
        fileId = 0;
        existingHash.clear();
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id,hash FROM files WHERE root=?1 AND path=?2;", -1, &statement, nullptr) != SQLITE_OK)
            return false;
        bindText(statement, 1, root);
        bindText(statement, 2, relative);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            fileId = sqlite3_column_int64(statement, 0);
            existingHash = columnText(statement, 1);
        }
        sqlite3_finalize(statement);
        return true;
    }

    bool upsertFile(std::string_view root, std::string_view relative, std::string_view hash,
                    std::int64_t generation, std::int64_t& fileId) {
        std::string existingHash;
        if (!findFile(root, relative, fileId, existingHash)) return false;
        if (fileId == 0) {
            sqlite3_stmt* insert = nullptr;
            if (sqlite3_prepare_v2(db,
                                   "INSERT INTO files(root,path,hash,validated_generation,snapshot_generation,dirty)"
                                   " VALUES(?1,?2,?3,?4,?4,0);",
                                   -1, &insert, nullptr) != SQLITE_OK)
                return false;
            bindText(insert, 1, root);
            bindText(insert, 2, relative);
            bindText(insert, 3, hash);
            sqlite3_bind_int64(insert, 4, generation);
            const bool ok = sqlite3_step(insert) == SQLITE_DONE;
            sqlite3_finalize(insert);
            if (!ok) return false;
            fileId = sqlite3_last_insert_rowid(db);
            return true;
        }
        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(db,
                               "UPDATE files SET hash=?1,validated_generation=?2,snapshot_generation=?2,dirty=0"
                               " WHERE id=?3;",
                               -1, &update, nullptr) != SQLITE_OK)
            return false;
        bindText(update, 1, hash);
        sqlite3_bind_int64(update, 2, generation);
        sqlite3_bind_int64(update, 3, fileId);
        const bool ok = sqlite3_step(update) == SQLITE_DONE;
        sqlite3_finalize(update);
        return ok;
    }

    bool touchFile(std::int64_t fileId, std::int64_t generation) {
        sqlite3_stmt* file = nullptr;
        if (sqlite3_prepare_v2(db,
                               "UPDATE files SET validated_generation=?1,snapshot_generation=?1,dirty=0 WHERE id=?2;",
                               -1, &file, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(file, 1, generation);
        sqlite3_bind_int64(file, 2, fileId);
        const bool fileTouched = sqlite3_step(file) == SQLITE_DONE;
        sqlite3_finalize(file);
        if (!fileTouched) return false;
        sqlite3_stmt* symbols = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE ast_symbols SET validated_generation=?1 WHERE file_id=?2;", -1,
                               &symbols, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(symbols, 1, generation);
        sqlite3_bind_int64(symbols, 2, fileId);
        const bool symbolsTouched = sqlite3_step(symbols) == SQLITE_DONE;
        sqlite3_finalize(symbols);
        return symbolsTouched;
    }

    bool deleteSymbols(std::int64_t fileId) {
        sqlite3_stmt* remove = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM ast_symbols WHERE file_id=?1;", -1, &remove, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(remove, 1, fileId);
        const bool ok = sqlite3_step(remove) == SQLITE_DONE;
        sqlite3_finalize(remove);
        return ok;
    }

    bool invalidateFile(std::string_view root, std::string_view relative) {
        sqlite3_stmt* symbols = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM ast_symbols WHERE workspace_root=?1 AND file_path=?2;", -1,
                               &symbols, nullptr) != SQLITE_OK)
            return false;
        bindText(symbols, 1, root);
        bindText(symbols, 2, relative);
        const bool symbolsDeleted = sqlite3_step(symbols) == SQLITE_DONE;
        sqlite3_finalize(symbols);
        if (!symbolsDeleted) return false;
        sqlite3_stmt* remove = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM files WHERE root=?1 AND path=?2;", -1, &remove, nullptr) != SQLITE_OK)
            return false;
        bindText(remove, 1, root);
        bindText(remove, 2, relative);
        const bool fileDeleted = sqlite3_step(remove) == SQLITE_DONE;
        sqlite3_finalize(remove);
        return fileDeleted;
    }

    bool markDirty(std::string_view root, std::string_view relative, std::int64_t generation) {
        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE files SET dirty=1 WHERE root=?1 AND path=?2;", -1,
                               &update, nullptr) != SQLITE_OK)
            return false;
        bindText(update, 1, root);
        bindText(update, 2, relative);
        const bool updated = sqlite3_step(update) == SQLITE_DONE;
        const int changed = sqlite3_changes(db);
        sqlite3_finalize(update);
        if (!updated) return false;
        if (changed > 0) return true;
        // A requested path that has no row yet is recorded as dirty so it cannot
        // be mistaken for clean or absent evidence before it is processed.
        sqlite3_stmt* insert = nullptr;
        if (sqlite3_prepare_v2(db,
                               "INSERT INTO files(root,path,hash,validated_generation,snapshot_generation,dirty)"
                               " VALUES(?1,?2,'',0,?3,1);",
                               -1, &insert, nullptr) != SQLITE_OK)
            return false;
        bindText(insert, 1, root);
        bindText(insert, 2, relative);
        sqlite3_bind_int64(insert, 3, generation);
        const bool inserted = sqlite3_step(insert) == SQLITE_DONE;
        sqlite3_finalize(insert);
        return inserted;
    }

    bool hasDirtyFiles(std::string_view root, bool& dirty) {
        dirty = false;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM files WHERE root=?1 AND dirty<>0 LIMIT 1;", -1,
                               &statement, nullptr) != SQLITE_OK)
            return false;
        bindText(statement, 1, root);
        const int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) dirty = true;
        sqlite3_finalize(statement);
        return step == SQLITE_ROW || step == SQLITE_DONE;
    }

    bool carryForward(std::string_view root, std::int64_t previousGeneration, std::int64_t generation) {
        if (previousGeneration <= 0) return true;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db,
                               "UPDATE files SET snapshot_generation=?1 WHERE root=?2"
                               " AND snapshot_generation=?3 AND dirty=0;",
                               -1, &statement, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(statement, 1, generation);
        bindText(statement, 2, root);
        sqlite3_bind_int64(statement, 3, previousGeneration);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return ok;
    }

    bool deleteUnseen(std::string_view root, std::int64_t generation) {
        sqlite3_stmt* files = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT id FROM files WHERE root=?1 AND (snapshot_generation<>?2 OR dirty<>0);",
                               -1, &files, nullptr) != SQLITE_OK)
            return false;
        bindText(files, 1, root);
        sqlite3_bind_int64(files, 2, generation);
        std::vector<std::int64_t> stale;
        while (sqlite3_step(files) == SQLITE_ROW) stale.push_back(sqlite3_column_int64(files, 0));
        sqlite3_finalize(files);
        for (const auto id : stale) {
            if (!deleteSymbols(id)) return false;
            sqlite3_stmt* remove = nullptr;
            if (sqlite3_prepare_v2(db, "DELETE FROM files WHERE id=?1;", -1, &remove, nullptr) != SQLITE_OK)
                return false;
            sqlite3_bind_int64(remove, 1, id);
            const bool ok = sqlite3_step(remove) == SQLITE_DONE;
            sqlite3_finalize(remove);
            if (!ok) return false;
        }
        return true;
    }

    bool insertSymbols(std::string_view root, std::string_view workspaceNamespace, std::int64_t generation,
                       std::int64_t fileId, std::string_view relative, std::string_view fileHash,
                       const std::vector<AstSymbol>& symbols, CodebaseIndexStats& stats) {
        std::unordered_map<std::string, std::string> scopedIds;
        scopedIds.reserve(symbols.size());
        for (const auto& symbol : symbols)
            scopedIds.emplace(symbol.symbolId, scopedSymbolId(workspaceNamespace, symbol.symbolId));

        sqlite3_stmt* insert = nullptr;
        if (sqlite3_prepare_v2(db, kSymbolInsertSql, -1, &insert, nullptr) != SQLITE_OK) return false;
        for (const auto& symbol : symbols) {
            const auto scoped = scopedIds.find(symbol.symbolId);
            if (scoped == scopedIds.end()) continue;
            const auto parent = symbol.parentSymbolId.empty()
                ? std::string{}
                : scopedIds.find(symbol.parentSymbolId) != scopedIds.end()
                      ? scopedIds.at(symbol.parentSymbolId) : std::string{};
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
            bindText(insert, 1, scoped->second);
            bindText(insert, 2, root);
            sqlite3_bind_int64(insert, 3, generation);
            sqlite3_bind_int64(insert, 4, fileId);
            bindText(insert, 5, relative);
            bindText(insert, 6, fileHash);
            bindText(insert, 7, symbol.name);
            bindText(insert, 8, symbol.qualifiedName);
            sqlite3_bind_int(insert, 9, static_cast<int>(symbol.kind));
            bindText(insert, 10, parent);
            bindText(insert, 11, symbol.signature);
            sqlite3_bind_int64(insert, 12, symbol.startLine);
            sqlite3_bind_int64(insert, 13, symbol.endLine);
            sqlite3_bind_int64(insert, 14, symbol.startColumn);
            sqlite3_bind_int64(insert, 15, symbol.endColumn);
            sqlite3_bind_int64(insert, 16, static_cast<sqlite3_int64>(symbol.span.startByte));
            sqlite3_bind_int64(insert, 17, static_cast<sqlite3_int64>(symbol.span.length()));
            bindText(insert, 18, symbol.typeName);
            sqlite3_bind_int(insert, 19, symbol.isConst ? 1 : 0);
            sqlite3_bind_int(insert, 20, static_cast<int>(symbol.role));
            sqlite3_bind_int(insert, 21, symbol.isDeclaration ? 1 : 0);
            sqlite3_bind_int(insert, 22, symbol.isDefinition ? 1 : 0);
            sqlite3_bind_int(insert, 23, symbol.isTemplate ? 1 : 0);
            sqlite3_bind_int(insert, 24, symbol.isStatic ? 1 : 0);
            sqlite3_bind_int(insert, 25, symbol.isConstexpr ? 1 : 0);
            sqlite3_bind_int(insert, 26, symbol.isInline ? 1 : 0);
            sqlite3_bind_int(insert, 27, symbol.isVariadic ? 1 : 0);
            sqlite3_bind_int(insert, 28, symbol.isReference ? 1 : 0);
            sqlite3_bind_int(insert, 29, static_cast<int>(symbol.parameterIndex));
            bindText(insert, 30, symbol.signatureKey);
            bindText(insert, 31, symbol.qualifiers);
            if (sqlite3_step(insert) != SQLITE_DONE) {
                sqlite3_finalize(insert);
                return false;
            }
            ++stats.symbolsIndexed;
        }
        sqlite3_finalize(insert);
        return true;
    }

    // Re-reads one workspace-relative source file. Missing, unreadable,
    // symlinked, oversized and non-regular paths are removed from the index so
    // they can never remain current evidence. Returns false only for a database
    // failure.
    bool processFile(std::string_view rootText, std::string_view workspaceNamespace,
                     const std::string& relative, const std::filesystem::path& absolute,
                     std::int64_t generation, const CodebaseIndexLimits& limits, CodebaseIndexStats& stats) {
        if (!isSourceFile(absolute)) return invalidateFile(rootText, relative);
        std::error_code error;
        if (std::filesystem::is_symlink(absolute, error)) {
            error.clear();
            return invalidateFile(rootText, relative);
        }
        if (error) {
            error.clear();
            return invalidateFile(rootText, relative);
        }
        if (!std::filesystem::is_regular_file(absolute, error) || error) {
            error.clear();
            return invalidateFile(rootText, relative);
        }
        const auto size = std::filesystem::file_size(absolute, error);
        if (error || size > limits.maximumFileBytes) {
            error.clear();
            return invalidateFile(rootText, relative);
        }
        std::string source;
        if (!readFileBounded(absolute, limits.maximumFileBytes, source))
            return invalidateFile(rootText, relative);
        const auto fileHash = core::crypto::Sha256::hexDigest(source);

        std::int64_t fileId = 0;
        std::string existingHash;
        if (!findFile(rootText, relative, fileId, existingHash)) return false;
        if (fileId != 0 && existingHash == fileHash) return touchFile(fileId, generation);

        const auto extraction = AstSymbolExtractor::extractSymbols(source, relative);
        if (!upsertFile(rootText, relative, fileHash, generation, fileId)) return false;
        if (!deleteSymbols(fileId)) return false;
        ++stats.filesIndexed;
        if (extraction.recovered) ++stats.filesRecovered;
        if (!extraction.parseOk && !extraction.recovered) {
            // The current bytes are known, but unusable. Keep the new hash with
            // an empty symbol snapshot so stale evidence cannot survive.
            return true;
        }
        return insertSymbols(rootText, workspaceNamespace, generation, fileId, relative, fileHash,
                             extraction.symbols, stats);
    }

    bool rebuildFts() {
        return exec(db, "INSERT INTO ast_symbols_fts(ast_symbols_fts) VALUES('rebuild');");
    }

    bool writeGeneration(std::string_view root, std::int64_t generation,
                         std::optional<bool> fullScanComplete,
                         std::optional<bool> coverageComplete) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db,
                               "UPDATE workspaces SET generation=?2,"
                               " last_refresh_complete=CASE WHEN ?3 IS NULL"
                               "  THEN last_refresh_complete ELSE ?3 END,"
                               " coverage_complete=CASE WHEN ?4 IS NULL"
                               "  THEN coverage_complete ELSE ?4 END"
                               " WHERE root=?1;",
                               -1, &statement, nullptr) != SQLITE_OK)
            return false;
        bindText(statement, 1, root);
        sqlite3_bind_int64(statement, 2, generation);
        if (fullScanComplete.has_value())
            sqlite3_bind_int(statement, 3, *fullScanComplete ? 1 : 0);
        else
            sqlite3_bind_null(statement, 3);
        if (coverageComplete.has_value())
            sqlite3_bind_int(statement, 4, *coverageComplete ? 1 : 0);
        else
            sqlite3_bind_null(statement, 4);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return ok;
    }

    bool workspaceInfo(CodebaseSnapshotInfo& info) const {
        info = {};
        if (!bound) return true;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT generation,last_refresh_complete,coverage_complete"
                               " FROM workspaces WHERE root=?1;",
                               -1, &statement, nullptr) != SQLITE_OK)
            return false;
        bindText(statement, 1, workspaceRoot);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            info.generation = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
            info.fullScanComplete = sqlite3_column_int(statement, 1) != 0;
            info.coverageComplete = sqlite3_column_int(statement, 2) != 0;
            info.complete = info.fullScanComplete && info.coverageComplete;
            info.available = true;
        }
        sqlite3_finalize(statement);
        return true;
    }

    bool executeExact(std::string_view sql, std::string_view workspace, std::string_view value,
                      std::string_view parent, std::string_view filePath, std::size_t fetch,
                      std::vector<AstSymbol>& symbols) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, std::string(sql).c_str(), -1, &statement, nullptr) != SQLITE_OK) return false;
        int index = 1;
        bindText(statement, index++, workspace);
        bindText(statement, index++, value);
        if (!parent.empty()) bindText(statement, index++, parent);
        if (!filePath.empty()) bindText(statement, index++, filePath);
        sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(fetch));
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement)) == SQLITE_ROW) symbols.push_back(rowSymbol(statement));
        sqlite3_finalize(statement);
        return step == SQLITE_DONE;
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
    const auto started = std::chrono::steady_clock::now();
    if (workspace.workspaceRoot.empty()) {
        if (impl_->bound) {
            impl_->workspaceRoot.clear();
            impl_->workspaceKey.clear();
            impl_->bound = false;
        }
    } else {
        std::error_code identityError;
        const auto requested = std::filesystem::weakly_canonical(workspace.workspaceRoot, identityError);
        if (impl_->bound && (identityError || requested.generic_string() != impl_->workspaceRoot)) {
            // A refresh attempt for another (or unidentifiable) workspace must
            // never leave the previous workspace's evidence queryable.
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
    const auto rootText = root.generic_string();
    // Bind before the transaction starts so a failed refresh can never leave the
    // instance answering from the previously selected workspace.
    impl_->bind(rootText);
    const auto workspaceKey = impl_->workspaceKey;
    CodebaseIndexStats stats;
    if (!exec(impl_->db, "BEGIN IMMEDIATE;")) {
        return core::makeError(core::ErrorCode::DbError, "Unable to start code index transaction");
    }
    auto rollback = [&]() { static_cast<void>(exec(impl_->db, "ROLLBACK;")); };
    if (!impl_->ensureWorkspace(rootText)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to register workspace");
    }
    std::int64_t previousGeneration = 0;
    bool previousFullScanComplete = false;
    bool previousCoverageComplete = false;
    bool workspaceExists = false;
    if (!impl_->readGeneration(rootText, previousGeneration, previousFullScanComplete,
                               previousCoverageComplete, workspaceExists)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to read the code index generation");
    }
    static_cast<void>(previousFullScanComplete);
    static_cast<void>(previousCoverageComplete);
    static_cast<void>(workspaceExists);
    const std::int64_t generation = previousGeneration <= 0 ? 1 : previousGeneration + 1;
    stats.generation = static_cast<std::uint64_t>(generation);

    // A denied subtree can hide competing symbols. Do not silently skip it or
    // throw out of the Result API; retain an explicitly incomplete snapshot.
    const auto options = std::filesystem::directory_options::none;
    std::filesystem::recursive_directory_iterator iterator(root, options, error), end;
    if (error) markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
    auto invalidateObservedSource = [&](const std::filesystem::path& path) -> bool {
        const auto relative = std::filesystem::relative(path, root, error);
        if (error || relative.empty() || !isSafeRelativeText(relative.generic_string())) {
            error.clear();
            markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
            return true;
        }
        return impl_->invalidateFile(rootText, relative.generic_string());
    };
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            // An unreadable subtree could hide files; never treat unvisited paths
            // as confirmed deletions for this pass.
            markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
            error.clear();
            continue;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count() >= limits.scanBudgetMs) {
            markIncomplete(stats, CodebaseIndexIncompleteReason::TimeBudget);
            break;
        }
        std::error_code statusError;
        const bool symlink = iterator->is_symlink(statusError);
        if (statusError) {
            statusError.clear();
            markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
            if (isSourceFile(iterator->path()) && !invalidateObservedSource(iterator->path())) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unavailable file");
            }
            continue;
        }
        if (symlink) {
            std::error_code directoryError;
            if (iterator->is_directory(directoryError)) iterator.disable_recursion_pending();
            directoryError.clear();
            if (isSourceFile(iterator->path()) && !invalidateObservedSource(iterator->path())) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate symlinked file");
            }
            continue;
        }
        const bool directory = iterator->is_directory(statusError);
        if (statusError) {
            statusError.clear();
            markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
            if (isSourceFile(iterator->path()) && !invalidateObservedSource(iterator->path())) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unavailable file");
            }
            continue;
        }
        if (directory) {
            if (isSourceFile(iterator->path()) && !invalidateObservedSource(iterator->path())) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate directory-shaped file");
            }
            if (kExcludedDirectories.contains(iterator->path().filename().string())) {
                iterator.disable_recursion_pending();
                continue;
            }
            // A junction or reparse point can resolve outside the workspace.
            std::error_code canonicalError;
            const auto canonicalDirectory = std::filesystem::weakly_canonical(iterator->path(), canonicalError);
            if (canonicalError || canonicalDirectory.empty()) {
                // An unreadable subtree could hide files.
                canonicalError.clear();
                markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
                iterator.disable_recursion_pending();
            } else if (!isWithinRoot(rootText, canonicalDirectory.generic_string())) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        const bool regular = iterator->is_regular_file(statusError);
        if (statusError) {
            statusError.clear();
            markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
            if (isSourceFile(iterator->path()) && !invalidateObservedSource(iterator->path())) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unreadable file");
            }
            continue;
        }
        if (!regular) {
            if (isSourceFile(iterator->path()) && !invalidateObservedSource(iterator->path())) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate special file");
            }
            continue;
        }
        if (!isSourceFile(iterator->path())) continue;
        if (stats.filesVisited >= limits.maximumFiles) {
            markIncomplete(stats, CodebaseIndexIncompleteReason::FileLimit);
            break;
        }
        ++stats.filesVisited;
        const auto relative = std::filesystem::relative(iterator->path(), root, error);
        if (error || relative.empty() || !isSafeRelativeText(relative.generic_string())) {
            markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
            error.clear();
            continue;
        }
        const auto relativeText = relative.generic_string();
        if (!impl_->processFile(rootText, workspaceKey, relativeText, iterator->path(),
                                generation, limits, stats)) {
            rollback();
            return core::makeError(core::ErrorCode::DbError, "Unable to index source file");
        }
    }

    // increment(error) can also move the iterator to end, bypassing the body.
    if (error) markIncomplete(stats, CodebaseIndexIncompleteReason::Filesystem);
    if (!stats.incomplete && !impl_->deleteUnseen(rootText, generation)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to remove stale indexed files");
    }
    if (!impl_->rebuildFts()) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to rebuild the symbol FTS index");
    }
    if (!impl_->writeGeneration(rootText, generation, !stats.incomplete, !stats.incomplete)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to record code index completeness");
    }
    if (!exec(impl_->db, "COMMIT;")) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to commit the code index");
    }
    stats.elapsedMs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count()) / 1000.0;
    return stats;
}

core::Result<CodebaseIndexStats> CodebaseIndex::refreshPaths(
    const WorkspaceContext& workspace, const std::vector<std::string>& relativePaths,
    const CodebaseIndexLimits& limits) {
    if (!impl_ || impl_->db == nullptr) {
        return core::makeError(core::ErrorCode::DbError,
                               impl_ && !impl_->initError.empty() ? impl_->initError : "Code index database is unavailable");
    }
    const auto started = std::chrono::steady_clock::now();
    if (workspace.workspaceRoot.empty()) {
        if (impl_->bound) {
            impl_->workspaceRoot.clear();
            impl_->workspaceKey.clear();
            impl_->bound = false;
        }
    } else {
        std::error_code identityError;
        const auto requested = std::filesystem::weakly_canonical(workspace.workspaceRoot, identityError);
        if (impl_->bound && (identityError || requested.generic_string() != impl_->workspaceRoot)) {
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

    struct RequestedFile {
        std::string relative{};
        std::filesystem::path absolute{};
    };
    std::vector<RequestedFile> requested;
    requested.reserve(relativePaths.size());
    std::unordered_set<std::string> seen;
    for (const auto& raw : relativePaths) {
        std::string normalized;
        std::filesystem::path absolute;
        if (!resolveWorkspaceRelative(root, raw, normalized, absolute)) {
            return core::makeError(core::ErrorCode::InvalidInput,
                                   "Invalid workspace-relative path: " + std::string(raw));
        }
        if (!seen.insert(normalized).second) continue;
        requested.push_back({std::move(normalized), std::move(absolute)});
    }

    const auto rootText = root.generic_string();
    impl_->bind(rootText);
    const auto workspaceKey = impl_->workspaceKey;
    CodebaseIndexStats stats;
    if (!exec(impl_->db, "BEGIN IMMEDIATE;")) {
        return core::makeError(core::ErrorCode::DbError, "Unable to start code index transaction");
    }
    auto rollback = [&]() { static_cast<void>(exec(impl_->db, "ROLLBACK;")); };
    if (!impl_->ensureWorkspace(rootText)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to register workspace");
    }
    std::int64_t previousGeneration = 0;
    bool previousFullScanComplete = false;
    bool previousCoverageComplete = false;
    bool workspaceExists = false;
    if (!impl_->readGeneration(rootText, previousGeneration, previousFullScanComplete,
                               previousCoverageComplete, workspaceExists)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to read the code index generation");
    }
    static_cast<void>(previousFullScanComplete);
    static_cast<void>(previousCoverageComplete);
    static_cast<void>(workspaceExists);
    const std::int64_t generation = previousGeneration <= 0 ? 1 : previousGeneration + 1;
    stats.generation = static_cast<std::uint64_t>(generation);
    if (!impl_->carryForward(rootText, previousGeneration, generation)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to carry forward the code index snapshot");
    }

    std::size_t processed = 0;
    std::size_t index = 0;
    for (; index < requested.size(); ++index) {
        const bool overBudget = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started).count() >= limits.scanBudgetMs;
        if (processed >= limits.maximumFiles || overBudget) {
            markIncomplete(stats, processed >= limits.maximumFiles
                ? CodebaseIndexIncompleteReason::FileLimit : CodebaseIndexIncompleteReason::TimeBudget);
            break;
        }
        const auto& file = requested[index];
        ++processed;
        ++stats.filesVisited;
        if (!impl_->processFile(rootText, workspaceKey, file.relative, file.absolute,
                                generation, limits, stats)) {
            rollback();
            return core::makeError(core::ErrorCode::DbError, "Unable to index requested source file");
        }
    }
    if (index < requested.size()) {
        stats.unprocessedPaths.reserve(requested.size() - index);
        for (; index < requested.size(); ++index) {
            stats.unprocessedPaths.push_back(requested[index].relative);
            if (!impl_->markDirty(rootText, requested[index].relative, generation)) {
                rollback();
                return core::makeError(core::ErrorCode::DbError, "Unable to invalidate unprocessed source file");
            }
        }
    }
    // Coverage also stays incomplete while any dirty row from an earlier pass
    // remains hidden. A later unrelated paths pass must not clear that
    // protection before those rows are revalidated.
    bool dirtyFiles = false;
    if (!impl_->hasDirtyFiles(rootText, dirtyFiles)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to read code index coverage state");
    }
    if (dirtyFiles) markIncomplete(stats, CodebaseIndexIncompleteReason::DirtyFiles);
    if (!impl_->rebuildFts()) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to rebuild the symbol FTS index");
    }
    // A paths pass can restore paths coverage, but it never repairs an
    // incomplete full scan.
    if (!impl_->writeGeneration(rootText, generation, std::nullopt, !stats.incomplete)) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to record the code index generation");
    }
    if (!exec(impl_->db, "COMMIT;")) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to commit the code index");
    }
    stats.elapsedMs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count()) / 1000.0;
    return stats;
}

core::Result<SymbolQueryResult> CodebaseIndex::findByName(std::string_view name,
                                                          std::string_view parentSymbolId,
                                                          std::string_view filePath,
                                                          std::size_t limit,
                                                          bool allowFtsFallback) const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    if (!impl_->bound) return core::makeError(core::ErrorCode::PreconditionFailed, "Code index has no bound workspace");
    CodebaseSnapshotInfo snapshot;
    if (!impl_->workspaceInfo(snapshot))
        return core::makeError(core::ErrorCode::DbError, "Unable to read code index snapshot state");
    const auto bounded = boundedQueryLimit(limit);
    const auto fetch = bounded + 1;
    const auto normalizedPath = lexicallyNormalized(normalizeRelativeText(filePath));
    std::string sql = std::string("SELECT ") + kSymbolColumns + kJoinFilter + " AND a.name=?2";
    int index = 3;
    if (!parentSymbolId.empty()) sql += " AND a.parent_id=?" + std::to_string(index++);
    if (!normalizedPath.empty()) sql += " AND a.file_path=?" + std::to_string(index++);
    sql += " ORDER BY a.file_path,a.byte_offset,a.symbol_id LIMIT ?" + std::to_string(index) + ";";

    std::vector<AstSymbol> symbols;
    if (!impl_->executeExact(sql, impl_->workspaceRoot, name, parentSymbolId, normalizedPath,
                             fetch, symbols)) {
        return core::makeError(core::ErrorCode::DbError, "Unable to query AST symbols");
    }
    if (!symbols.empty() || !allowFtsFallback)
        return finalizedResult(std::move(symbols), bounded, snapshot.complete);

    const auto query = normalizedFtsQuery(name);
    if (query.empty()) return finalizedResult(std::move(symbols), bounded, snapshot.complete);
    std::string ftsSql = std::string("SELECT ") + kSymbolColumns +
        " FROM ast_symbols_fts fts JOIN ast_symbols a ON a.rowid=fts.rowid"
        " JOIN files f ON f.id=a.file_id"
        " JOIN workspaces w ON w.root=a.workspace_root"
        " WHERE ast_symbols_fts MATCH ?1 AND a.workspace_root=?2"
        " AND f.root=?2 AND f.snapshot_generation=w.generation AND f.dirty=0";
    int ftsIndex = 3;
    if (!parentSymbolId.empty()) ftsSql += " AND a.parent_id=?" + std::to_string(ftsIndex++);
    if (!normalizedPath.empty()) ftsSql += " AND a.file_path=?" + std::to_string(ftsIndex++);
    ftsSql += " ORDER BY bm25(ast_symbols_fts),a.file_path,a.byte_offset LIMIT ?" +
        std::to_string(ftsIndex) + ";";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, ftsSql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return finalizedResult(std::move(symbols), bounded, snapshot.complete);
    bindText(statement, 1, query);
    bindText(statement, 2, impl_->workspaceRoot);
    int bindIndex = 3;
    if (!parentSymbolId.empty()) bindText(statement, bindIndex++, parentSymbolId);
    if (!normalizedPath.empty()) bindText(statement, bindIndex++, normalizedPath);
    sqlite3_bind_int64(statement, bindIndex, static_cast<sqlite3_int64>(fetch));
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) symbols.push_back(rowSymbol(statement));
    sqlite3_finalize(statement);
    return finalizedResult(std::move(symbols), bounded, snapshot.complete);
}

core::Result<SymbolQueryResult> CodebaseIndex::findQualified(std::string_view qualifiedName,
                                                             std::size_t limit) const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    if (!impl_->bound) return core::makeError(core::ErrorCode::PreconditionFailed, "Code index has no bound workspace");
    CodebaseSnapshotInfo snapshot;
    if (!impl_->workspaceInfo(snapshot))
        return core::makeError(core::ErrorCode::DbError, "Unable to read code index snapshot state");
    const auto bounded = boundedQueryLimit(limit);
    const std::string sql = std::string("SELECT ") + kSymbolColumns + kJoinFilter +
        " AND a.qualified_name=?2 ORDER BY a.file_path,a.byte_offset,a.symbol_id LIMIT ?3;";
    std::vector<AstSymbol> symbols;
    if (!impl_->executeExact(sql, impl_->workspaceRoot, qualifiedName, {}, {}, bounded + 1, symbols))
        return core::makeError(core::ErrorCode::DbError, "Unable to query AST symbol");
    return finalizedResult(std::move(symbols), bounded, snapshot.complete);
}

core::Result<SymbolQueryResult> CodebaseIndex::children(std::string_view parentSymbolId,
                                                        std::size_t limit) const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    if (!impl_->bound) return core::makeError(core::ErrorCode::PreconditionFailed, "Code index has no bound workspace");
    CodebaseSnapshotInfo snapshot;
    if (!impl_->workspaceInfo(snapshot))
        return core::makeError(core::ErrorCode::DbError, "Unable to read code index snapshot state");
    const auto bounded = boundedQueryLimit(limit);
    const std::string sql = std::string("SELECT ") + kSymbolColumns + kJoinFilter +
        " AND a.parent_id=?2 ORDER BY a.file_path,a.byte_offset,a.symbol_id LIMIT ?3;";
    std::vector<AstSymbol> symbols;
    if (!impl_->executeExact(sql, impl_->workspaceRoot, parentSymbolId, {}, {}, bounded + 1, symbols))
        return core::makeError(core::ErrorCode::DbError, "Unable to query AST children");
    return finalizedResult(std::move(symbols), bounded, snapshot.complete);
}

core::Result<bool> CodebaseIndex::validateFileHash(std::string_view relativePath,
                                                   std::string_view expectedHash) const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    if (!impl_->bound) return core::makeError(core::ErrorCode::PreconditionFailed, "Code index has no bound workspace");
    const auto expected = normalizeExpectedDigest(expectedHash);
    if (!isHexDigest(expected)) {
        return core::makeError(core::ErrorCode::InvalidInput, "Expected file hash is not a SHA-256 digest");
    }
    std::string normalized;
    std::filesystem::path absolute;
    if (!resolveWorkspaceRelative(std::filesystem::path(impl_->workspaceRoot), relativePath, normalized, absolute)) {
        return core::makeError(core::ErrorCode::InvalidInput, "Invalid workspace-relative path");
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(absolute, error) || error) return false;
    const auto size = std::filesystem::file_size(absolute, error);
    if (error || size > kMaximumFileBytes) return false;
    std::string source;
    if (!readFileBounded(absolute, kMaximumFileBytes, source)) return false;
    return core::crypto::Sha256::hexDigest(source) == expected;
}

core::Result<CodebaseSnapshotInfo> CodebaseIndex::snapshotInfo() const {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    CodebaseSnapshotInfo info;
    if (!impl_->workspaceInfo(info))
        return core::makeError(core::ErrorCode::DbError, "Unable to read code index snapshot state");
    return info;
}

core::Status CodebaseIndex::forgetWorkspace(const std::filesystem::path& root) {
    if (!impl_ || impl_->db == nullptr) return core::makeError(core::ErrorCode::DbError, "Code index is unavailable");
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(root, error).generic_string();
    if (error) return core::makeError(core::ErrorCode::InvalidInput, "Workspace path is unavailable");
    if (!exec(impl_->db, "BEGIN IMMEDIATE;"))
        return core::makeError(core::ErrorCode::DbError, "Unable to start workspace deletion");
    auto rollback = [&]() { static_cast<void>(exec(impl_->db, "ROLLBACK;")); };
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM ast_symbols WHERE workspace_root=?1;", -1,
                           &statement, nullptr) != SQLITE_OK) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to forget workspace symbols");
    }
    bindText(statement, 1, normalized);
    static_cast<void>(sqlite3_step(statement));
    sqlite3_finalize(statement);
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM files WHERE root=?1;", -1, &statement, nullptr) != SQLITE_OK) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to forget workspace files");
    }
    bindText(statement, 1, normalized);
    static_cast<void>(sqlite3_step(statement));
    sqlite3_finalize(statement);
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM workspaces WHERE root=?1;", -1, &statement, nullptr) != SQLITE_OK) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to forget workspace");
    }
    bindText(statement, 1, normalized);
    static_cast<void>(sqlite3_step(statement));
    sqlite3_finalize(statement);
    if (!impl_->rebuildFts()) {
        rollback();
        return core::makeError(core::ErrorCode::DbError, "Unable to rebuild the symbol FTS index");
    }
    if (!exec(impl_->db, "COMMIT;"))
        return core::makeError(core::ErrorCode::DbError, "Unable to commit workspace deletion");
    if (impl_->bound && impl_->workspaceRoot == normalized) {
        impl_->workspaceRoot.clear();
        impl_->workspaceKey.clear();
        impl_->bound = false;
    }
    return core::Status::success();
}

} // namespace roclint::storage
