#include "roclint/storage/PromptRepository.hpp"

#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace roclint::storage
{
    namespace
    {
        class Statement
        {
        public:
            Statement(sqlite3* db, const char* sql)
            {
                if (!db)
                {
                    throw std::runtime_error("Cannot prepare statement on null database handle");
                }
                const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
                if (rc != SQLITE_OK)
                {
                    std::string err = "Failed to prepare SQL statement: ";
                    err += sqlite3_errmsg(db);
                    throw std::runtime_error(err);
                }
            }

            ~Statement()
            {
                if (stmt_)
                {
                    sqlite3_finalize(stmt_);
                }
            }

            Statement(const Statement&) = delete;
            Statement& operator=(const Statement&) = delete;

            Statement(Statement&& other) noexcept : stmt_(other.stmt_)
            {
                other.stmt_ = nullptr;
            }

            Statement& operator=(Statement&& other) noexcept
            {
                if (this != &other)
                {
                    if (stmt_)
                    {
                        sqlite3_finalize(stmt_);
                    }
                    stmt_ = other.stmt_;
                    other.stmt_ = nullptr;
                }
                return *this;
            }

            [[nodiscard]] sqlite3_stmt* get() const noexcept
            {
                return stmt_;
            }

        private:
            sqlite3_stmt* stmt_{nullptr};
        };
    } // anonymous namespace

    PromptRepository::PromptRepository(Database& db)
        : db_(db)
    {
        if (!db_.isOpen())
        {
            throw std::runtime_error("PromptRepository requires an open Database connection");
        }
        // Enforce relational integrity per-connection
        db_.execute("PRAGMA foreign_keys = ON;");
    }

    int PromptRepository::getSchemaVersion() const
    {
        Statement stmt(db_.rawHandle(), "PRAGMA user_version;");
        if (sqlite3_step(stmt.get()) == SQLITE_ROW)
        {
            return sqlite3_column_int(stmt.get(), 0);
        }
        return 0;
    }

    void PromptRepository::setSchemaVersion(int version)
    {
        db_.execute("PRAGMA user_version = " + std::to_string(version) + ";");
    }

    void PromptRepository::migrateTo(int targetVersion)
    {
        int current = getSchemaVersion();
        if (current >= targetVersion)
        {
            return;
        }

        // Migration step 0 -> 1: Baseline schema (sessions and prompt_revisions tables)
        if (current < 1 && targetVersion >= 1)
        {
            db_.initializeSchema();
            setSchemaVersion(1);
            current = 1;
        }

        // Migration step 1 -> 2: Performance index on prompt_revisions(session_id, created_at_ms)
        if (current < 2 && targetVersion >= 2)
        {
            db_.execute("BEGIN IMMEDIATE TRANSACTION;");
            try
            {
                db_.execute("CREATE INDEX IF NOT EXISTS idx_prompt_revisions_session "
                            "ON prompt_revisions(session_id, created_at_ms);");
                setSchemaVersion(2);
                db_.execute("COMMIT;");
            }
            catch (...)
            {
                db_.execute("ROLLBACK;");
                throw;
            }
            current = 2;
        }
    }

    void PromptRepository::migrate()
    {
        migrateTo(CURRENT_SCHEMA_VERSION);
    }

    void PromptRepository::createSession(const core::SessionId& sessionId, std::int64_t createdAtMs)
    {
        Statement stmt(db_.rawHandle(), "INSERT INTO sessions (session_id, created_at_ms) VALUES (?1, ?2);");
        sqlite3_bind_text(stmt.get(), 1, sessionId.value().c_str(), static_cast<int>(sessionId.value().size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt.get(), 2, createdAtMs);

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE)
        {
            throw std::runtime_error("Failed to create session: " + std::string(sqlite3_errmsg(db_.rawHandle())));
        }
    }

    bool PromptRepository::sessionExists(const core::SessionId& sessionId) const
    {
        Statement stmt(db_.rawHandle(), "SELECT 1 FROM sessions WHERE session_id = ?1 LIMIT 1;");
        sqlite3_bind_text(stmt.get(), 1, sessionId.value().c_str(), static_cast<int>(sessionId.value().size()), SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt.get());
        return rc == SQLITE_ROW;
    }

    std::vector<SessionRecord> PromptRepository::listSessions() const
    {
        Statement stmt(db_.rawHandle(), "SELECT session_id, created_at_ms FROM sessions ORDER BY created_at_ms DESC;");
        std::vector<SessionRecord> sessions;

        while (sqlite3_step(stmt.get()) == SQLITE_ROW)
        {
            const auto* idText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
            const int idBytes = sqlite3_column_bytes(stmt.get(), 0);
            const std::int64_t createdAt = sqlite3_column_int64(stmt.get(), 1);

            sessions.push_back(SessionRecord{
                core::SessionId(idText ? std::string(idText, static_cast<size_t>(idBytes)) : std::string()),
                createdAt
            });
        }
        return sessions;
    }

    void PromptRepository::deleteSession(const core::SessionId& sessionId)
    {
        db_.execute("BEGIN IMMEDIATE TRANSACTION;");
        try
        {
            Statement stmt(db_.rawHandle(), "DELETE FROM sessions WHERE session_id = ?1;");
            sqlite3_bind_text(stmt.get(), 1, sessionId.value().c_str(), static_cast<int>(sessionId.value().size()), SQLITE_TRANSIENT);

            const int rc = sqlite3_step(stmt.get());
            if (rc != SQLITE_DONE)
            {
                throw std::runtime_error("Failed to delete session: " + std::string(sqlite3_errmsg(db_.rawHandle())));
            }
            db_.execute("COMMIT;");
        }
        catch (...)
        {
            db_.execute("ROLLBACK;");
            throw;
        }
    }

    void PromptRepository::saveRevision(const core::PromptRevision& revision)
    {
        Statement stmt(db_.rawHandle(),
            "INSERT INTO prompt_revisions (revision_id, parent_revision_id, prompt_text, session_id, created_at_ms) "
            "VALUES (?1, ?2, ?3, ?4, ?5);");

        const std::string& revId = revision.revisionId().value();
        sqlite3_bind_text(stmt.get(), 1, revId.c_str(), static_cast<int>(revId.size()), SQLITE_TRANSIENT);

        if (revision.parentRevisionId().has_value())
        {
            const std::string& parentId = revision.parentRevisionId()->value();
            sqlite3_bind_text(stmt.get(), 2, parentId.c_str(), static_cast<int>(parentId.size()), SQLITE_TRANSIENT);
        }
        else
        {
            sqlite3_bind_null(stmt.get(), 2);
        }

        const std::string& text = revision.text();
        sqlite3_bind_text(stmt.get(), 3, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);

        const std::string& sessId = revision.sessionId().value();
        sqlite3_bind_text(stmt.get(), 4, sessId.c_str(), static_cast<int>(sessId.size()), SQLITE_TRANSIENT);

        sqlite3_bind_int64(stmt.get(), 5, revision.createdAtMs());

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE)
        {
            throw std::runtime_error("Failed to save revision: " + std::string(sqlite3_errmsg(db_.rawHandle())));
        }
    }

    std::optional<core::PromptRevision> PromptRepository::getRevision(const core::RevisionId& revisionId) const
    {
        Statement stmt(db_.rawHandle(),
            "SELECT session_id, revision_id, parent_revision_id, prompt_text, created_at_ms "
            "FROM prompt_revisions WHERE revision_id = ?1;");

        sqlite3_bind_text(stmt.get(), 1, revisionId.value().c_str(), static_cast<int>(revisionId.value().size()), SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW)
        {
            const auto* sessText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
            const int sessBytes = sqlite3_column_bytes(stmt.get(), 0);

            const auto* revText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
            const int revBytes = sqlite3_column_bytes(stmt.get(), 1);

            const auto* parentText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
            const int parentBytes = parentText ? sqlite3_column_bytes(stmt.get(), 2) : 0;

            const auto* promptText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
            const int promptBytes = sqlite3_column_bytes(stmt.get(), 3);

            const std::int64_t createdAt = sqlite3_column_int64(stmt.get(), 4);

            std::optional<core::RevisionId> parentId;
            if (parentText != nullptr)
            {
                parentId = core::RevisionId(std::string(parentText, static_cast<size_t>(parentBytes)));
            }

            return core::PromptRevision(
                core::SessionId(sessText ? std::string(sessText, static_cast<size_t>(sessBytes)) : std::string()),
                core::RevisionId(revText ? std::string(revText, static_cast<size_t>(revBytes)) : std::string()),
                parentId,
                promptText ? std::string(promptText, static_cast<size_t>(promptBytes)) : std::string(),
                createdAt
            );
        }
        return std::nullopt;
    }

    std::vector<core::PromptRevision> PromptRepository::getHistory(const core::SessionId& sessionId) const
    {
        Statement stmt(db_.rawHandle(),
            "SELECT session_id, revision_id, parent_revision_id, prompt_text, created_at_ms "
            "FROM prompt_revisions WHERE session_id = ?1 "
            "ORDER BY created_at_ms ASC, rowid ASC;");

        sqlite3_bind_text(stmt.get(), 1, sessionId.value().c_str(), static_cast<int>(sessionId.value().size()), SQLITE_TRANSIENT);

        std::vector<core::PromptRevision> history;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW)
        {
            const auto* sessText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
            const int sessBytes = sqlite3_column_bytes(stmt.get(), 0);

            const auto* revText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
            const int revBytes = sqlite3_column_bytes(stmt.get(), 1);

            const auto* parentText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
            const int parentBytes = parentText ? sqlite3_column_bytes(stmt.get(), 2) : 0;

            const auto* promptText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
            const int promptBytes = sqlite3_column_bytes(stmt.get(), 3);

            const std::int64_t createdAt = sqlite3_column_int64(stmt.get(), 4);

            std::optional<core::RevisionId> parentId;
            if (parentText != nullptr)
            {
                parentId = core::RevisionId(std::string(parentText, static_cast<size_t>(parentBytes)));
            }

            history.emplace_back(
                core::SessionId(sessText ? std::string(sessText, static_cast<size_t>(sessBytes)) : std::string()),
                core::RevisionId(revText ? std::string(revText, static_cast<size_t>(revBytes)) : std::string()),
                parentId,
                promptText ? std::string(promptText, static_cast<size_t>(promptBytes)) : std::string(),
                createdAt
            );
        }
        return history;
    }

    std::optional<core::PromptRevision> PromptRepository::getLatestRevision(const core::SessionId& sessionId) const
    {
        Statement stmt(db_.rawHandle(),
            "SELECT session_id, revision_id, parent_revision_id, prompt_text, created_at_ms "
            "FROM prompt_revisions WHERE session_id = ?1 "
            "ORDER BY created_at_ms DESC, rowid DESC LIMIT 1;");

        sqlite3_bind_text(stmt.get(), 1, sessionId.value().c_str(), static_cast<int>(sessionId.value().size()), SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW)
        {
            const auto* sessText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
            const int sessBytes = sqlite3_column_bytes(stmt.get(), 0);

            const auto* revText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
            const int revBytes = sqlite3_column_bytes(stmt.get(), 1);

            const auto* parentText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
            const int parentBytes = parentText ? sqlite3_column_bytes(stmt.get(), 2) : 0;

            const auto* promptText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
            const int promptBytes = sqlite3_column_bytes(stmt.get(), 3);

            const std::int64_t createdAt = sqlite3_column_int64(stmt.get(), 4);

            std::optional<core::RevisionId> parentId;
            if (parentText != nullptr)
            {
                parentId = core::RevisionId(std::string(parentText, static_cast<size_t>(parentBytes)));
            }

            return core::PromptRevision(
                core::SessionId(sessText ? std::string(sessText, static_cast<size_t>(sessBytes)) : std::string()),
                core::RevisionId(revText ? std::string(revText, static_cast<size_t>(revBytes)) : std::string()),
                parentId,
                promptText ? std::string(promptText, static_cast<size_t>(promptBytes)) : std::string(),
                createdAt
            );
        }
        return std::nullopt;
    }
}
