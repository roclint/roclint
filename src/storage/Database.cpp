#include "roclint/storage/Database.hpp"
#include <string_view>
#include <stdexcept>
#include <iostream>

namespace roclint::storage
{

    constexpr std::string_view SCHEMA_V1 = R"(
        CREATE TABLE IF NOT EXISTS sessions (
            session_id TEXT NOT NULL PRIMARY KEY,
            created_at_ms INTEGER NOT NULL 
        );

        CREATE TABLE IF NOT EXISTS prompt_revisions (
            revision_id TEXT NOT NULL Primary Key,
            parent_revision_id TEXT ,
            prompt_text Text NOT NULL,
            session_id TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            FOREIGN KEY(session_id) REFERENCES sessions(session_id)
            
            ON DELETE CASCADE
        );
    
    )";
   
    Database::Database(const std::filesystem::path& path) 
    {
        open(path);
    }

    Database::Database (Database && other) noexcept
    {
        db_ = other.db_;
        other.db_ = nullptr;
    }
    Database::~Database() 
    {
        close();
    }

    void Database::open(const std::filesystem::path& path)
    {
        if(isOpen())
        {
            close();
        }
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        int rc = sqlite3_open_v2(
            path.string().c_str(),
            &db_,
            flags,
            nullptr
        );

        if(rc != SQLITE_OK)
        {
            const std::string errorMessage = "Failed to open database: " + 
                (db_ ? std::string(sqlite3_errmsg(db_)) : std::string("unknown error (database handle was null)"));
            close();
            throw std::runtime_error(errorMessage);
        }

        sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    }
    void Database::close() noexcept
    {
        if (isOpen())
        {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    }
    [[nodiscard]] bool Database::isOpen() const noexcept
    {
        return db_ != nullptr;
    }
    [[nodiscard]] sqlite3* Database::rawHandle() const noexcept
    {
        return db_;    
    }

    void Database::execute(const std::string& sql)
    {
        if(!isOpen())
        {
           throw std::runtime_error("Database is not open");
        }
        char* errMsg = nullptr;

        const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string error = "SQLite execution error: ";
            if (errMsg) {
                error += errMsg;
                sqlite3_free(errMsg);
            } else {
                error += sqlite3_errmsg(db_);
            }
            throw std::runtime_error(error);
        }
    }

void Database::initializeSchema()
{
    execute("PRAGMA foreign_keys = ON;");
    execute("PRAGMA journal_mode = WAL;");
    execute(std::string(SCHEMA_V1));
}

Database& Database::operator=(Database&& other) noexcept
{
    if (this != &other)
    {
        close();
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}


}

