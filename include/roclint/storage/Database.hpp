#pragma once

#include "roclint/core/Result.hpp"

#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <atomic>
#include <memory>

namespace roclint::storage {

class Database {
public:
    static constexpr int kBusyTimeoutMs = 5000;

    Database() noexcept = default;
    // Convenience constructor for tools/tests; throws std::runtime_error when open() fails.
    explicit Database(const std::filesystem::path& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Opens the database (UTF-8 path) and sets and verifies foreign_keys=ON, journal_mode=WAL
    // ("memory" for in-memory databases), synchronous=NORMAL, and busy_timeout=5000.
    // With a cancellation flag, a cancellable handler replaces the native busy timeout after initialization.
    [[nodiscard]] core::Status open(const std::filesystem::path& path);
    void close() noexcept;
    // Set before open; storage workers cancel busy waits and long-running statements cooperatively.
    void setCancellationFlag(std::shared_ptr<std::atomic_bool> flag) { cancellation_ = std::move(flag); }

    [[nodiscard]] bool isOpen() const noexcept { return db_ != nullptr; }
    [[nodiscard]] bool isInMemory() const noexcept { return inMemory_; }
    [[nodiscard]] sqlite3* rawHandle() const noexcept { return db_; }

    [[nodiscard]] core::Status execute(std::string_view sql);
    // Runs a single-row query and returns column 0 as text.
    [[nodiscard]] core::Result<std::string> queryText(std::string_view sql);

private:
    [[nodiscard]] core::Status applyAndVerifyPragmas();

    sqlite3* db_{nullptr};
    bool inMemory_{false};
    std::shared_ptr<std::atomic_bool> cancellation_;
};

} // namespace roclint::storage
