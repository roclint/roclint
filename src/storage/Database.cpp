#include "roclint/storage/Database.hpp"

#include <cctype>
#include <stdexcept>

namespace roclint::storage {

using core::ErrorCode;
using core::makeError;

namespace {

std::string toLower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

} // namespace

Database::Database(const std::filesystem::path& path) {
    if (auto status = open(path); !status.ok()) {
        throw std::runtime_error(status.error().message);
    }
}

Database::Database(Database&& other) noexcept : db_(other.db_), inMemory_(other.inMemory_) {
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        db_ = other.db_;
        inMemory_ = other.inMemory_;
        other.db_ = nullptr;
    }
    return *this;
}

Database::~Database() {
    close();
}

core::Status Database::open(const std::filesystem::path& path) {
    close();
    const std::string target = utf8Path(path);
    inMemory_ = (target == ":memory:" || target.empty());

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    const int rc = sqlite3_open_v2(inMemory_ ? ":memory:" : target.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = "Failed to open database '" + target + "': " +
                              (db_ != nullptr ? std::string(sqlite3_errmsg(db_)) : std::string(sqlite3_errstr(rc)));
        close();
        return makeError(ErrorCode::DbError, std::move(message));
    }
    sqlite3_extended_result_codes(db_, 1);

    if (auto status = applyAndVerifyPragmas(); !status.ok()) {
        close();
        return status;
    }
    return core::Status::success();
}

core::Status Database::applyAndVerifyPragmas() {
    if (auto s = execute("PRAGMA foreign_keys = ON;"); !s.ok()) return s;
    auto fk = queryText("PRAGMA foreign_keys;");
    if (!fk.ok()) return fk.error();
    if (fk.value() != "1") {
        return makeError(ErrorCode::DbError, "PRAGMA foreign_keys could not be enabled");
    }

    auto mode = queryText("PRAGMA journal_mode = WAL;");
    if (!mode.ok()) return mode.error();
    const std::string expectedMode = inMemory_ ? "memory" : "wal";
    if (toLower(mode.value()) != expectedMode) {
        return makeError(ErrorCode::DbError,
                         "PRAGMA journal_mode returned '" + mode.value() + "', expected '" + expectedMode + "'");
    }

    if (auto s = execute("PRAGMA synchronous = NORMAL;"); !s.ok()) return s;
    auto sync = queryText("PRAGMA synchronous;");
    if (!sync.ok()) return sync.error();
    if (sync.value() != "1") {
        return makeError(ErrorCode::DbError, "PRAGMA synchronous returned " + sync.value() + ", expected 1 (NORMAL)");
    }

    auto busy = queryText("PRAGMA busy_timeout = " + std::to_string(kBusyTimeoutMs) + ";");
    if (!busy.ok()) return busy.error();
    auto busyCheck = queryText("PRAGMA busy_timeout;");
    if (!busyCheck.ok()) return busyCheck.error();
    if (busyCheck.value() != std::to_string(kBusyTimeoutMs)) {
        return makeError(ErrorCode::DbError, "PRAGMA busy_timeout returned " + busyCheck.value());
    }
    return core::Status::success();
}

void Database::close() noexcept {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

core::Status Database::execute(std::string_view sql) {
    if (!isOpen()) {
        return makeError(ErrorCode::DbError, "Database is not open");
    }
    const std::string statement(sql);
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db_, statement.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string error = "SQLite error: ";
        if (errMsg != nullptr) {
            error += errMsg;
            sqlite3_free(errMsg);
        } else {
            error += sqlite3_errmsg(db_);
        }
        return makeError(ErrorCode::DbError, std::move(error));
    }
    return core::Status::success();
}

core::Result<std::string> Database::queryText(std::string_view sql) {
    if (!isOpen()) {
        return makeError(ErrorCode::DbError, "Database is not open");
    }
    sqlite3_stmt* stmt = nullptr;
    const std::string statement(sql);
    int rc = sqlite3_prepare_v2(db_, statement.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return makeError(ErrorCode::DbError, std::string("SQLite prepare failed: ") + sqlite3_errmsg(db_));
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        const std::string message = std::string("SQLite query returned no row: ") + sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return makeError(ErrorCode::DbError, message);
    }
    const auto* text = sqlite3_column_text(stmt, 0);
    std::string value = text != nullptr ? std::string(reinterpret_cast<const char*>(text)) : std::string{};
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return makeError(ErrorCode::DbError, std::string("SQLite query failed: ") + sqlite3_errmsg(db_));
    }
    return value;
}

} // namespace roclint::storage
