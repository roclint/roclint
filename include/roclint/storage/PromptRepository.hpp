#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "roclint/core/prompt_state/PromptRevision.hpp"
#include "roclint/core/prompt_state/RevisionId.hpp"
#include "roclint/core/prompt_state/SessionId.hpp"
#include "roclint/storage/Database.hpp"

namespace roclint::storage
{
    struct SessionRecord
    {
        core::SessionId id;
        std::int64_t createdAtMs{0};

        bool operator==(const SessionRecord& other) const noexcept = default;
    };

    class PromptRepository
    {
    public:
        static constexpr int CURRENT_SCHEMA_VERSION = 2;

        explicit PromptRepository(Database& db);
        ~PromptRepository() = default;

        PromptRepository(const PromptRepository&) = delete;
        PromptRepository& operator=(const PromptRepository&) = delete;

        PromptRepository(PromptRepository&&) noexcept = default;
        PromptRepository& operator=(PromptRepository&&) = delete;

        // Schema versioning & migrations
        [[nodiscard]] int getSchemaVersion() const;
        void setSchemaVersion(int version);
        void migrate();
        void migrateTo(int targetVersion);

        // Session operations
        void createSession(const core::SessionId& sessionId, std::int64_t createdAtMs);
        [[nodiscard]] bool sessionExists(const core::SessionId& sessionId) const;
        [[nodiscard]] std::vector<SessionRecord> listSessions() const;
        void deleteSession(const core::SessionId& sessionId);

        // Revision operations 
        void saveRevision(const core::PromptRevision& revision);
        [[nodiscard]] std::optional<core::PromptRevision> getRevision(const core::RevisionId& revisionId) const;
        [[nodiscard]] std::vector<core::PromptRevision> getHistory(const core::SessionId& sessionId) const;
        [[nodiscard]] std::optional<core::PromptRevision> getLatestRevision(const core::SessionId& sessionId) const;

    private:
        Database& db_;
    };
}
