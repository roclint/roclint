#pragma once
#include <string>
#include <filesystem>
#include <sqlite3.h>

namespace roclint::storage
{
    class Database
    {

        private:         
            sqlite3* db_ = nullptr;
        public:

        // 1. Default constructor: starts in an uninitialized, closed state.
        Database() noexcept = default;
        // 2. Convenience constructor: immediately opens a connection.
        explicit Database(const std::filesystem::path& path);
        //destructor
        ~Database();
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        Database(Database&& other) noexcept;
        Database& operator=(Database&& other) noexcept;

        void open(const std::filesystem::path& path);
        void close() noexcept;
        [[nodiscard]] bool isOpen() const noexcept;
        void execute(const std::string& sql);
        void initializeSchema();
        [[nodiscard]] sqlite3* rawHandle() const noexcept;

    };
}
