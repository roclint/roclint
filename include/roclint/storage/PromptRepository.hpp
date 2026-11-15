#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/prompt_state/PromptRevision.hpp"
#include "roclint/core/prompt_state/RevisionId.hpp"
#include "roclint/core/prompt_state/SessionId.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/review/ApprovalRecord.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/storage/Database.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace roclint::storage {

struct SessionRecord {
    core::SessionId id;
    std::uint64_t stateVersion{1};
    std::uint64_t highestRevisionSeq{1};
    std::uint64_t draftEditGeneration{0};
    std::uint64_t proposalEditGeneration{0};
    std::int64_t createdAtMs{0};
    std::int64_t updatedAtMs{0};

    bool operator==(const SessionRecord&) const = default;
};

enum class RevisionOrigin {
    Root,
    Commit,
    DraftFreeze,
    Restore
};

[[nodiscard]] const char* toString(RevisionOrigin origin) noexcept;

struct RevisionRecord {
    core::PromptRevision revision;
    std::uint64_t sequence{1};
    std::uint64_t stateVersion{1};
    std::uint64_t draftEditGeneration{0};
    RevisionOrigin origin{RevisionOrigin::Root};
};

// One optimistic-concurrency revision write: the session must currently be at
// expectedStateVersion with highest sequence (sequence - 1).
struct RevisionWrite {
    core::PromptRevision revision;
    std::uint64_t sequence{0};
    std::uint64_t expectedStateVersion{0};
    std::uint64_t newStateVersion{0};
    std::uint64_t draftEditGeneration{0};
    std::uint64_t proposalEditGeneration{0};
    std::int64_t nowMs{0};
};

enum class AttemptStatus {
    InProgress,
    Succeeded,
    Failed,
    DiscardedStale,
    Interrupted
};

[[nodiscard]] const char* toString(AttemptStatus status) noexcept;

struct ModelAttemptRecord {
    std::string attemptId{};
    std::string sessionId{};
    std::string revisionId{};
    std::string stage{};       // "discovery" | "clarification_check" | "julia_scoring" | "rewrite" | "health"
    AttemptStatus status{AttemptStatus::InProgress};
    std::string transport{};
    std::string modelId{};
    std::string modelDetail{}; // digest / quantization as reported by the runtime
    std::string errorMessage{};
    std::int64_t startedAtMs{0};
    std::optional<std::int64_t> completedAtMs{std::nullopt};
};

// Everything needed to rebuild in-memory state after reopen. Downstream artifacts are loaded only
// when they are bound to the latest revision and to each other.
struct RestoredSession {
    SessionRecord session;
    RevisionRecord latestRevision;
    std::optional<core::analysis::AnalysisSnapshot> analysis{};
    bool legacyAnalysisRequiresReview{false};
    std::optional<core::intent::ConfirmedIntent> intent{};
    std::optional<core::rewrite::RewriteProposal> proposal{};
    std::optional<core::review::ApprovalRecord> approval{};
};

class PromptRepository {
public:
    static constexpr int CURRENT_SCHEMA_VERSION = 5;

    // Requires an open Database; throws std::invalid_argument otherwise.
    explicit PromptRepository(Database& db);

    PromptRepository(const PromptRepository&) = delete;
    PromptRepository& operator=(const PromptRepository&) = delete;

    [[nodiscard]] core::Result<int> getSchemaVersion() const;
    [[nodiscard]] core::Status migrate();
    [[nodiscard]] core::Status migrateTo(int targetVersion);

    // Sessions
    [[nodiscard]] core::Status createSessionWithRootRevision(const core::PromptRevision& root, std::int64_t nowMs);
    [[nodiscard]] core::Result<bool> sessionExists(const core::SessionId& sessionId) const;
    [[nodiscard]] core::Result<SessionRecord> getSession(const core::SessionId& sessionId) const;
    [[nodiscard]] core::Result<std::vector<SessionRecord>> listSessions() const;
    [[nodiscard]] core::Status deleteSession(const core::SessionId& sessionId);
    [[nodiscard]] core::Status purgeHistory();
    // Compare-and-swap one accepted current-session mutation; text revision remains immutable.
    [[nodiscard]] core::Status advanceSessionState(const core::RevisionState& next,
                                                   std::uint64_t expectedVersion, std::int64_t nowMs);
    [[nodiscard]] core::Status advanceDraftStateRange(const core::RevisionState& next,
                                                       std::uint64_t expectedVersion, std::int64_t nowMs);
    // Persists an advanced proposal edit generation; generations never move backwards.
    [[nodiscard]] core::Status recordProposalGeneration(const core::SessionId& sessionId,
                                                        std::uint64_t proposalEditGeneration,
                                                        std::int64_t nowMs);

    // Revisions
    [[nodiscard]] core::Status insertRevision(const RevisionWrite& write);
    [[nodiscard]] core::Status insertDraftRevision(const RevisionWrite& write);
    [[nodiscard]] core::Status insertRestoredRevision(const RevisionWrite& write);
    [[nodiscard]] core::Result<std::optional<RevisionRecord>> getRevision(const core::SessionId& sessionId,
                                                                          const core::RevisionId& revisionId) const;
    [[nodiscard]] core::Result<std::vector<RevisionRecord>> getHistory(const core::SessionId& sessionId) const;
    [[nodiscard]] core::Result<std::optional<RevisionRecord>> getLatestRevision(const core::SessionId& sessionId) const;

    // Downstream artifacts
    [[nodiscard]] core::Status insertAnalysisSnapshot(const core::analysis::AnalysisSnapshot& analysis,
                                                      std::int64_t nowMs,
                                                      std::optional<std::uint64_t> expectedVersion = std::nullopt);
    [[nodiscard]] core::Status insertIntentVersion(const core::intent::ConfirmedIntent& intent, std::int64_t nowMs,
                                                   std::optional<std::uint64_t> expectedVersion = std::nullopt);
    [[nodiscard]] core::Status insertProposal(const core::rewrite::RewriteProposal& proposal, std::int64_t nowMs,
                                              std::optional<std::uint64_t> expectedVersion = std::nullopt);
    [[nodiscard]] core::Status insertApproval(const core::review::ApprovalRecord& approval,
                                              std::optional<std::uint64_t> expectedVersion = std::nullopt);
    [[nodiscard]] core::Status invalidateApprovals(const core::SessionId& sessionId);
    [[nodiscard]] core::Result<std::optional<core::review::ApprovalRecord>> getCurrentApproval(
        const core::SessionId& sessionId) const;
    [[nodiscard]] core::Result<std::vector<core::review::ApprovalRecord>> getApprovalHistory(
        const core::SessionId& sessionId) const;

    // Model attempts
    [[nodiscard]] core::Status beginModelAttempt(const ModelAttemptRecord& attempt);
    [[nodiscard]] core::Status completeModelAttempt(const std::string& attemptId,
                                                    AttemptStatus status,
                                                    const std::string& modelId,
                                                    const std::string& modelDetail,
                                                    const std::string& errorMessage,
                                                    std::int64_t completedAtMs);
    [[nodiscard]] core::Result<std::vector<ModelAttemptRecord>> getModelAttempts(const core::SessionId& sessionId) const;
    // Marks every IN_PROGRESS attempt INTERRUPTED; returns the number of rows changed.
    [[nodiscard]] core::Result<std::size_t> interruptOrphanedAttemptsOnStartup(std::int64_t nowMs);

    [[nodiscard]] core::Result<RestoredSession> restoreSession(const core::SessionId& sessionId) const;

private:
    [[nodiscard]] core::Status insertRevisionWithOrigin(const RevisionWrite& write, RevisionOrigin origin);

    Database& db_;
};

} // namespace roclint::storage
