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
            path.string().c_str(),    // Filename as a null-terminated C-string
            &db_,                     // Address of the pointer to receive the handle
            flags,                   // Behavior configuration flags
            nullptr                  // Default VFS (Virtual File System)
            );

        if(rc != SQLITE_OK)
        {
            std::string errorMessage = "Failed to open database: ";
            if (isOpen()) 
            {
                errorMessage += sqlite3_errmsg(db_);
            } 
            else 
            {
                errorMessage += "Unknown error (database handle was null)";
            }
            close();
            throw std::runtime_error(errorMessage);
            
        }

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
            // SQLite allocates zErrMsg internally; you must free it using sqlite3_free
            sqlite3_free(errMsg);
        } else {
            error += sqlite3_errmsg(db_);
        }
        throw std::runtime_error(error);
    }

        
    }

    void Database::initializeSchema()
{
    // 1. Enforce relational integrity per-connection
    execute("PRAGMA foreign_keys = ON;");

    // 2. Enable Write-Ahead Logging for concurrency and speed
    execute("PRAGMA journal_mode = WAL;");

    // 3. Create the tables defined in SCHEMA_V1
    execute(std::string(SCHEMA_V1));
}

Database& Database::operator=(Database&& other) noexcept
{
    if (this != &other) // Guard against db1 = std::move(db1)
    {
        close();             // 1. Close current connection if open
        db_ = other.db_;     // 2. Take ownership of other's connection
        other.db_ = nullptr; // 3. Reset other so it won't close it
    }
    return *this;
}


}

