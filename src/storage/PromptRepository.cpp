#include "roclint/storage/PromptRepository.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/Json.hpp"

#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/storage/PayloadCodec.hpp"

#include <exception>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace roclint::storage {

using core::Error;
using core::ErrorCode;
using core::makeError;
using core::Result;
using core::Status;

const char* toString(RevisionOrigin origin) noexcept {
    switch (origin) {
        case RevisionOrigin::Root: return "ROOT";
        case RevisionOrigin::Commit: return "COMMIT";
        case RevisionOrigin::DraftFreeze: return "DRAFT_FREEZE";
        case RevisionOrigin::Restore: return "RESTORE";
    }
    return "UNKNOWN";
}

const char* toString(AttemptStatus status) noexcept {
    switch (status) {
        case AttemptStatus::InProgress: return "IN_PROGRESS";
        case AttemptStatus::Succeeded: return "SUCCEEDED";
        case AttemptStatus::Failed: return "FAILED";
        case AttemptStatus::DiscardedStale: return "DISCARDED_STALE";
        case AttemptStatus::Interrupted: return "INTERRUPTED";
    }
    return "UNKNOWN";
}

namespace {

// Internal failure carrier; every public method converts it into a typed Error.
struct RepoFailure {
    Error error;
};

[[noreturn]] void fail(ErrorCode code, std::string message) {
    throw RepoFailure{makeError(code, std::move(message))};
}

[[noreturn]] void failSqlite(sqlite3* db, int rc, const std::string& what) {
    const std::string detail = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    const ErrorCode code = (rc & 0xFF) == SQLITE_CONSTRAINT ? ErrorCode::Conflict : ErrorCode::DbError;
    fail(code, what + ": " + detail);
}

template <typename F>
auto guarded(F&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const RepoFailure& f) {
        return f.error;
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::DbError, ex.what());
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (db_ == nullptr) {
            fail(ErrorCode::DbError, "Cannot prepare statement on a closed database");
        }
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            failSqlite(db_, rc, "Failed to prepare SQL statement");
        }
    }
    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& text(int idx, const std::string& value) {
        check(sqlite3_bind_text(stmt_, idx, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
        return *this;
    }
    Statement& integer(int idx, std::int64_t value) {
        check(sqlite3_bind_int64(stmt_, idx, value));
        return *this;
    }
    Statement& unsignedInt(int idx, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(INT64_MAX)) {
            fail(ErrorCode::InvalidInput, "Integer value exceeds SQLite range");
        }
        return integer(idx, static_cast<std::int64_t>(value));
    }
    Statement& null(int idx) {
        check(sqlite3_bind_null(stmt_, idx));
        return *this;
    }

    // Returns true for SQLITE_ROW, false for SQLITE_DONE; throws otherwise.
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        failSqlite(db_, rc, "SQLite step failed");
    }
    void run() {
        if (step()) {
            fail(ErrorCode::DbError, "Statement unexpectedly returned rows");
        }
    }

    [[nodiscard]] std::string columnText(int col) const {
        const auto* p = sqlite3_column_text(stmt_, col);
        return p != nullptr ? std::string(reinterpret_cast<const char*>(p),
                                          static_cast<std::size_t>(sqlite3_column_bytes(stmt_, col)))
                            : std::string{};
    }
    [[nodiscard]] bool columnIsNull(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
    [[nodiscard]] std::int64_t columnInt(int col) const { return sqlite3_column_int64(stmt_, col); }
    [[nodiscard]] std::uint64_t columnUInt(int col) const {
        const auto v = sqlite3_column_int64(stmt_, col);
        if (v < 0) {
            fail(ErrorCode::ValidationFailed, "Negative value in unsigned column");
        }
        return static_cast<std::uint64_t>(v);
    }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) {
            failSqlite(db_, rc, "SQLite bind failed");
        }
    }

    sqlite3* db_{nullptr};
    sqlite3_stmt* stmt_{nullptr};
};

class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) {
        if (auto s = db_.execute("BEGIN IMMEDIATE TRANSACTION;"); !s.ok()) {
            throw RepoFailure{s.error()};
        }
    }
    ~Transaction() {
        if (!committed_) {
            (void)db_.execute("ROLLBACK;");
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        if (auto s = db_.execute("COMMIT;"); !s.ok()) {
            throw RepoFailure{s.error()};
        }
        committed_ = true;
    }

private:
    Database& db_;
    bool committed_{false};
};

void exec(Database& db, const char* sql) {
    if (auto s = db.execute(sql); !s.ok()) {
        throw RepoFailure{s.error()};
    }
}

constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE IF NOT EXISTS sessions (
    session_id TEXT NOT NULL PRIMARY KEY,
    created_at_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS prompt_revisions (
    revision_id TEXT NOT NULL PRIMARY KEY,
    parent_revision_id TEXT,
    prompt_text TEXT NOT NULL,
    session_id TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
)SQL";

constexpr const char* kSchemaV2 =
    "CREATE INDEX IF NOT EXISTS idx_prompt_revisions_session ON prompt_revisions(session_id, created_at_ms);";

constexpr const char* kSchemaV3 = R"SQL(
CREATE TABLE IF NOT EXISTS approvals (
    id TEXT PRIMARY KEY NOT NULL,
    session_id TEXT NOT NULL,
    revision_id TEXT NOT NULL,
    snapshot_id TEXT NOT NULL,
    snapshot_hash INTEGER NOT NULL,
    intent_id TEXT NOT NULL,
    proposal_id TEXT NOT NULL,
    proposal_hash INTEGER NOT NULL,
    approved_text TEXT NOT NULL,
    edit_generation INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL,
    is_current INTEGER NOT NULL DEFAULT 1,
    binding_hash INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE,
    FOREIGN KEY(revision_id) REFERENCES prompt_revisions(revision_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_approvals_session ON approvals(session_id, is_current);
CREATE INDEX IF NOT EXISTS idx_approvals_revision ON approvals(revision_id, is_current);
)SQL";

// v3 -> v4: full C02 schema with composite (session_id, revision_id) foreign keys. Legacy rows are
// carried over with per-session sequences; the legacy approvals table was never written by the app.
constexpr const char* kSchemaV4 = R"SQL(
DROP INDEX IF EXISTS idx_approvals_session;
DROP INDEX IF EXISTS idx_approvals_revision;
DROP INDEX IF EXISTS idx_prompt_revisions_session;
ALTER TABLE approvals RENAME TO legacy_approvals;
ALTER TABLE prompt_revisions RENAME TO legacy_prompt_revisions;
ALTER TABLE sessions RENAME TO legacy_sessions;

CREATE TABLE sessions (
    session_id TEXT NOT NULL PRIMARY KEY,
    state_version INTEGER NOT NULL CHECK (state_version >= 1),
    highest_revision_seq INTEGER NOT NULL CHECK (highest_revision_seq >= 1),
    draft_edit_generation INTEGER NOT NULL DEFAULT 0,
    proposal_edit_generation INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE revisions (
    session_id TEXT NOT NULL,
    revision_id TEXT NOT NULL,
    revision_seq INTEGER NOT NULL CHECK (revision_seq >= 1),
    parent_revision_id TEXT,
    content TEXT NOT NULL,
    state_version INTEGER NOT NULL,
    draft_edit_generation INTEGER NOT NULL,
    origin TEXT NOT NULL CHECK (origin IN ('ROOT', 'COMMIT', 'DRAFT_FREEZE', 'RESTORE')),
    created_at INTEGER NOT NULL,
    PRIMARY KEY (session_id, revision_id),
    UNIQUE (revision_id),
    UNIQUE (session_id, revision_seq),
    CHECK (parent_revision_id IS NULL OR parent_revision_id <> revision_id),
    FOREIGN KEY (session_id) REFERENCES sessions(session_id) ON DELETE CASCADE,
    FOREIGN KEY (session_id, parent_revision_id) REFERENCES revisions(session_id, revision_id) ON DELETE CASCADE
);

CREATE TABLE analysis_snapshots (
    snapshot_id TEXT NOT NULL PRIMARY KEY,
    session_id TEXT NOT NULL,
    base_revision_id TEXT NOT NULL,
    snapshot_hash TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (session_id, base_revision_id, snapshot_id),
    FOREIGN KEY (session_id, base_revision_id) REFERENCES revisions(session_id, revision_id) ON DELETE CASCADE
);

CREATE TABLE intent_versions (
    intent_id TEXT NOT NULL PRIMARY KEY,
    session_id TEXT NOT NULL,
    base_revision_id TEXT NOT NULL,
    snapshot_id TEXT NOT NULL,
    intent_hash TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (session_id, base_revision_id, intent_id),
    FOREIGN KEY (session_id, base_revision_id) REFERENCES revisions(session_id, revision_id) ON DELETE CASCADE,
    FOREIGN KEY (snapshot_id) REFERENCES analysis_snapshots(snapshot_id) ON DELETE CASCADE,
    FOREIGN KEY (session_id, base_revision_id, snapshot_id)
        REFERENCES analysis_snapshots(session_id, base_revision_id, snapshot_id) ON DELETE CASCADE
);

CREATE TABLE proposals (
    proposal_id TEXT NOT NULL PRIMARY KEY,
    session_id TEXT NOT NULL,
    base_revision_id TEXT NOT NULL,
    intent_id TEXT NOT NULL,
    proposal_hash TEXT NOT NULL,
    proposal_edit_generation INTEGER NOT NULL,
    payload_json TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (session_id, base_revision_id, proposal_id),
    FOREIGN KEY (session_id, base_revision_id) REFERENCES revisions(session_id, revision_id) ON DELETE CASCADE,
    FOREIGN KEY (intent_id) REFERENCES intent_versions(intent_id) ON DELETE CASCADE,
    FOREIGN KEY (session_id, base_revision_id, intent_id)
        REFERENCES intent_versions(session_id, base_revision_id, intent_id) ON DELETE CASCADE
);

CREATE TABLE approvals (
    approval_id TEXT NOT NULL PRIMARY KEY,
    session_id TEXT NOT NULL,
    revision_id TEXT NOT NULL,
    snapshot_id TEXT NOT NULL,
    snapshot_hash TEXT NOT NULL,
    intent_id TEXT NOT NULL,
    intent_hash TEXT NOT NULL,
    proposal_id TEXT NOT NULL,
    proposal_hash TEXT NOT NULL,
    approved_text TEXT NOT NULL,
    state_version INTEGER NOT NULL,
    draft_edit_generation INTEGER NOT NULL,
    proposal_edit_generation INTEGER NOT NULL,
    validator_version TEXT NOT NULL,
    policy_version TEXT NOT NULL,
    rewrite_profile TEXT NOT NULL,
    binding_hash TEXT NOT NULL,
    is_current INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (session_id, revision_id) REFERENCES revisions(session_id, revision_id) ON DELETE CASCADE,
    FOREIGN KEY (snapshot_id) REFERENCES analysis_snapshots(snapshot_id) ON DELETE CASCADE,
    FOREIGN KEY (intent_id) REFERENCES intent_versions(intent_id) ON DELETE CASCADE,
    FOREIGN KEY (proposal_id) REFERENCES proposals(proposal_id) ON DELETE CASCADE,
    FOREIGN KEY (session_id, revision_id, snapshot_id)
        REFERENCES analysis_snapshots(session_id, base_revision_id, snapshot_id) ON DELETE CASCADE,
    FOREIGN KEY (session_id, revision_id, intent_id)
        REFERENCES intent_versions(session_id, base_revision_id, intent_id) ON DELETE CASCADE,
    FOREIGN KEY (session_id, revision_id, proposal_id)
        REFERENCES proposals(session_id, base_revision_id, proposal_id) ON DELETE CASCADE
);

CREATE TABLE model_attempts (
    attempt_id TEXT NOT NULL PRIMARY KEY,
    session_id TEXT NOT NULL,
    revision_id TEXT NOT NULL,
    stage TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('IN_PROGRESS', 'SUCCEEDED', 'FAILED', 'DISCARDED_STALE', 'INTERRUPTED')),
    transport TEXT NOT NULL,
    model_id TEXT,
    model_detail TEXT,
    error_message TEXT,
    started_at INTEGER,
    completed_at INTEGER,
    FOREIGN KEY (session_id, revision_id) REFERENCES revisions(session_id, revision_id) ON DELETE CASCADE
);

CREATE INDEX idx_revisions_session_seq ON revisions(session_id, revision_seq);
CREATE INDEX idx_snapshots_revision ON analysis_snapshots(session_id, base_revision_id);
CREATE INDEX idx_intents_snapshot ON intent_versions(snapshot_id);
CREATE INDEX idx_proposals_intent ON proposals(intent_id);
CREATE INDEX idx_approvals_session ON approvals(session_id, is_current);
CREATE INDEX idx_approvals_proposal ON approvals(proposal_id);
CREATE INDEX idx_attempts_session ON model_attempts(session_id, revision_id);
CREATE INDEX idx_attempts_status ON model_attempts(status);

INSERT INTO sessions (session_id, state_version, highest_revision_seq, draft_edit_generation,
                      proposal_edit_generation, created_at, updated_at)
SELECT s.session_id,
       (SELECT COUNT(*) FROM legacy_prompt_revisions r WHERE r.session_id = s.session_id),
       (SELECT COUNT(*) FROM legacy_prompt_revisions r WHERE r.session_id = s.session_id),
       0, 0, s.created_at_ms, s.created_at_ms
FROM legacy_sessions s
WHERE EXISTS (SELECT 1 FROM legacy_prompt_revisions r WHERE r.session_id = s.session_id);

INSERT INTO revisions (session_id, revision_id, revision_seq, parent_revision_id, content, state_version,
                       draft_edit_generation, origin, created_at)
SELECT r.session_id, r.revision_id, r.seq,
       CASE WHEN r.parent_revision_id IS NOT NULL
             AND r.parent_revision_id <> r.revision_id
             AND EXISTS (SELECT 1 FROM legacy_prompt_revisions p
                         WHERE p.revision_id = r.parent_revision_id AND p.session_id = r.session_id)
            THEN r.parent_revision_id ELSE NULL END,
       r.prompt_text, r.seq, 0,
       CASE WHEN r.seq = 1 THEN 'ROOT' ELSE 'COMMIT' END,
       r.created_at_ms
FROM (SELECT lpr.*, ROW_NUMBER() OVER (PARTITION BY lpr.session_id ORDER BY lpr.created_at_ms, lpr.rowid) AS seq
      FROM legacy_prompt_revisions lpr) r
WHERE r.session_id IN (SELECT session_id FROM sessions);

DROP TABLE legacy_approvals;
DROP TABLE legacy_prompt_revisions;
DROP TABLE legacy_sessions;
)SQL";

// Keep legacy pre-score rows as history. Their approval cannot be reused as final scored evidence.
constexpr const char* kSchemaV5 = R"SQL(
ALTER TABLE analysis_snapshots ADD COLUMN format_version INTEGER NOT NULL DEFAULT 1 CHECK (format_version IN (1, 2));
UPDATE approvals SET is_current = 0 WHERE snapshot_id IN
    (SELECT snapshot_id FROM analysis_snapshots WHERE format_version = 1);
)SQL";

constexpr const char* kSchemaV6 =
    "CREATE INDEX IF NOT EXISTS idx_sessions_updated_order ON sessions(updated_at DESC, session_id ASC);";

constexpr const char* kSchemaV7 = R"SQL(
CREATE TABLE delivery_events (
    event_id TEXT PRIMARY KEY NOT NULL,
    session_id TEXT NOT NULL,
    revision_id TEXT NOT NULL,
    outcome TEXT NOT NULL CHECK(outcome IN ('INSERTED', 'COPIED')),
    text_hash TEXT NOT NULL CHECK(length(text_hash) = 64),
    app_identifier TEXT NOT NULL,
    destination TEXT NOT NULL,
    scope_summary TEXT NOT NULL,
    field_token TEXT NOT NULL,
    process_id INTEGER NOT NULL,
    completed_at INTEGER NOT NULL CHECK(completed_at > 0),
    FOREIGN KEY(session_id, revision_id) REFERENCES revisions(session_id, revision_id) ON DELETE CASCADE
);
CREATE INDEX idx_delivery_events_session ON delivery_events(session_id, completed_at DESC, event_id DESC);
)SQL";

// Called within the same write transaction as an artifact insert where applicable.
void advanceStoredState(Database& db, const std::string& sessionId, const std::string& revisionId,
                        std::uint64_t expectedVersion, std::int64_t nowMs,
                        std::optional<std::uint64_t> draftGeneration = std::nullopt,
                        std::optional<std::uint64_t> proposalGeneration = std::nullopt,
                        std::optional<std::uint64_t> highestSequence = std::nullopt) {
    if (expectedVersion >= core::RevisionState::kMaxStateVersion || nowMs < 0) fail(ErrorCode::InvalidInput, "Invalid state version or mutation timestamp");
    Statement current(db.rawHandle(),
        "SELECT highest_revision_seq, draft_edit_generation, proposal_edit_generation FROM sessions "
        "WHERE session_id = ?1 AND state_version = ?2 AND EXISTS "
        "(SELECT 1 FROM revisions WHERE revisions.session_id = sessions.session_id "
        "AND revisions.revision_seq = sessions.highest_revision_seq AND revisions.revision_id = ?3);");
    current.text(1, sessionId).unsignedInt(2, expectedVersion).text(3, revisionId);
    if (!current.step()) fail(ErrorCode::StaleVersion, "The stored session changed before this command was applied");
    if (highestSequence && *highestSequence != current.columnUInt(0)) fail(ErrorCode::StaleVersion, "The revision sequence changed");
    const auto draft = draftGeneration.value_or(current.columnUInt(1));
    const auto proposal = proposalGeneration.value_or(current.columnUInt(2));
    if (draft < current.columnUInt(1) || proposal < current.columnUInt(2)) fail(ErrorCode::InvalidInput, "Edit generations cannot move backwards");
    Statement update(db.rawHandle(),
        "UPDATE sessions SET state_version = ?1, draft_edit_generation = ?2, proposal_edit_generation = ?3, updated_at = ?4 "
        "WHERE session_id = ?5 AND state_version = ?6;");
    update.unsignedInt(1, expectedVersion + 1).unsignedInt(2, draft).unsignedInt(3, proposal)
        .integer(4, nowMs).text(5, sessionId).unsignedInt(6, expectedVersion);
    update.run();
    if (sqlite3_changes(db.rawHandle()) != 1) fail(ErrorCode::StaleVersion, "The stored session changed before this command was applied");
    Statement approvals(db.rawHandle(), "UPDATE approvals SET is_current = 0 WHERE session_id = ?1;");
    approvals.text(1, sessionId);
    approvals.run();
}

void validateSnapshotAttempts(Database& db, const core::analysis::AnalysisSnapshot& snapshot) {
    auto requireAttempt = [&](const std::string& id, bool discovery) {
        Statement statement(db.rawHandle(), "SELECT session_id, revision_id, stage, status, model_detail FROM model_attempts WHERE attempt_id = ?1;");
        statement.text(1, id);
        if (!statement.step() || statement.columnText(0) != snapshot.normalized.sessionId ||
            statement.columnText(1) != snapshot.normalized.baseRevisionId) {
            fail(ErrorCode::ValidationFailed, "Final analysis refers to a missing or foreign model attempt");
        }
        const auto stage = statement.columnText(2), status = statement.columnText(3);
        const bool ranked = snapshot.evaluation.isRanked();
        if (discovery) {
            if (stage != "discovery" || status != "SUCCEEDED") fail(ErrorCode::ValidationFailed, "Final analysis requires successful discovery");
            if (snapshot.payloadVersion >= 5) {
                const auto marker = snapshot.reviewPolicy.trigger == core::analysis::ReviewTrigger::ExplicitReview
                    ? "; review_trigger=explicit_review" : "; review_trigger=automatic";
                const auto detail = statement.columnText(4) + ";";
                const bool automatic = detail.find("; review_trigger=automatic;") != std::string::npos;
                const bool explicitReview = detail.find("; review_trigger=explicit_review;") != std::string::npos;
                if (automatic == explicitReview || detail.find(std::string(marker) + ";") == std::string::npos)
                    fail(ErrorCode::ValidationFailed, "Review trigger differs from its recorded discovery request");
            }
        } else {
            std::string expected = snapshot.evaluation.engine == core::analysis::ScorerEngine::JuliaLocal ? "julia_scoring" : "jev_scoring";
#if defined(ROCLINT_ENABLE_TEST_MOCKS)
            if (snapshot.evaluation.executionStatus == core::analysis::ExecutionStatus::ExecutedMock) expected = "test_mock_scoring";
#endif
            if (stage != expected || (ranked ? status != "SUCCEEDED" : (status != "FAILED" && status != "SUCCEEDED"))) {
                fail(ErrorCode::ValidationFailed, "Scoring attempt stage/status does not match the final artifact");
            }
        }
    };
    if (snapshot.payloadVersion >= 4 && snapshot.promptTriage) {
        const auto& triage = *snapshot.promptTriage;
        Statement statement(db.rawHandle(), "SELECT session_id, revision_id, stage, status, model_id, model_detail FROM model_attempts WHERE attempt_id = ?1;");
        statement.text(1, triage.attemptId);
        const auto suffix = "; prompt_triage_payload=" + encodePromptTriage(triage);
        if (!statement.step() || statement.columnText(0) != triage.sessionId || statement.columnText(1) != triage.revisionId ||
            statement.columnText(2) != "julia_triage" || statement.columnText(3) != "SUCCEEDED" ||
            statement.columnText(4) != triage.modelId || !statement.columnText(5).ends_with(suffix))
            fail(ErrorCode::ValidationFailed, "Final analysis requires its exact successful classifier attempt");
    }
    if (!snapshot.discoveryAttemptId.empty()) requireAttempt(snapshot.discoveryAttemptId, true);
    if (!snapshot.scorerAttemptId.empty()) requireAttempt(snapshot.scorerAttemptId, false);
}

SessionRecord readSession(const Statement& s) {
    SessionRecord rec{core::SessionId(s.columnText(0))};
    rec.stateVersion = s.columnUInt(1);
    rec.highestRevisionSeq = s.columnUInt(2);
    rec.draftEditGeneration = s.columnUInt(3);
    rec.proposalEditGeneration = s.columnUInt(4);
    rec.createdAtMs = s.columnInt(5);
    rec.updatedAtMs = s.columnInt(6);
    return rec;
}

constexpr const char* kSessionColumns =
    "session_id, state_version, highest_revision_seq, draft_edit_generation, proposal_edit_generation, "
    "created_at, updated_at";

RevisionOrigin originFromString(const std::string& s) {
    for (const auto o : {RevisionOrigin::Root, RevisionOrigin::Commit, RevisionOrigin::DraftFreeze,
                         RevisionOrigin::Restore}) {
        if (s == toString(o)) return o;
    }
    fail(ErrorCode::ValidationFailed, "Unknown revision origin '" + s + "'");
}

AttemptStatus attemptStatusFromString(const std::string& s) {
    for (const auto a : {AttemptStatus::InProgress, AttemptStatus::Succeeded, AttemptStatus::Failed,
                         AttemptStatus::DiscardedStale, AttemptStatus::Interrupted}) {
        if (s == toString(a)) return a;
    }
    fail(ErrorCode::ValidationFailed, "Unknown attempt status '" + s + "'");
}

constexpr const char* kRevisionColumns =
    "session_id, revision_id, parent_revision_id, content, created_at, revision_seq, state_version, "
    "draft_edit_generation, origin";

RevisionRecord readRevision(const Statement& s) {
    std::optional<core::RevisionId> parent;
    if (!s.columnIsNull(2)) {
        parent = core::RevisionId(s.columnText(2));
    }
    core::PromptRevision rev(core::SessionId(s.columnText(0)), core::RevisionId(s.columnText(1)), parent,
                             s.columnText(3), s.columnInt(4));
    RevisionRecord rec{std::move(rev)};
    rec.sequence = s.columnUInt(5);
    rec.stateVersion = s.columnUInt(6);
    rec.draftEditGeneration = s.columnUInt(7);
    rec.origin = originFromString(s.columnText(8));
    return rec;
}

constexpr const char* kApprovalColumns =
    "approval_id, session_id, revision_id, state_version, snapshot_id, snapshot_hash, intent_id, intent_hash, "
    "proposal_id, proposal_hash, approved_text, draft_edit_generation, proposal_edit_generation, "
    "validator_version, policy_version, rewrite_profile, created_at, is_current, binding_hash";

core::review::ApprovalRecord readApproval(const Statement& s) {
    core::review::ApprovalRecord a;
    a.id = s.columnText(0);
    a.sessionId = s.columnText(1);
    a.revisionId = s.columnText(2);
    a.stateVersion = s.columnUInt(3);
    a.snapshotId = s.columnText(4);
    a.snapshotHash = s.columnText(5);
    a.intentId = s.columnText(6);
    a.intentHash = s.columnText(7);
    a.proposalId = s.columnText(8);
    a.proposalHash = s.columnText(9);
    a.approvedText = s.columnText(10);
    a.draftEditGeneration = s.columnUInt(11);
    a.proposalEditGeneration = s.columnUInt(12);
    a.validatorVersion = s.columnText(13);
    a.policyVersion = s.columnText(14);
    a.rewriteProfile = s.columnText(15);
    a.createdAtMs = s.columnInt(16);
    a.isCurrent = s.columnInt(17) != 0;
    a.bindingHash = s.columnText(18);
    return a;
}

bool sessionRowExists(sqlite3* db, const std::string& sessionId) {
    Statement s(db, "SELECT 1 FROM sessions WHERE session_id = ?1;");
    s.text(1, sessionId);
    return s.step();
}

} // namespace

PromptRepository::PromptRepository(Database& db) : db_(db) {
    if (!db_.isOpen()) {
        throw std::invalid_argument("PromptRepository requires an open Database connection");
    }
}

Result<int> PromptRepository::getSchemaVersion() const {
    return guarded([&]() -> Result<int> {
        Statement stmt(db_.rawHandle(), "PRAGMA user_version;");
        if (!stmt.step()) {
            fail(ErrorCode::DbError, "PRAGMA user_version returned no row");
        }
        return static_cast<int>(stmt.columnInt(0));
    });
}

Status PromptRepository::migrate() {
    return migrateTo(CURRENT_SCHEMA_VERSION);
}

Status PromptRepository::migrateTo(int targetVersion) {
    return guarded([&]() -> Status {
        if (targetVersion < 0 || targetVersion > CURRENT_SCHEMA_VERSION) {
            fail(ErrorCode::InvalidInput, "Unknown schema version " + std::to_string(targetVersion));
        }
        const char* steps[] = {kSchemaV1, kSchemaV2, kSchemaV3, kSchemaV4, kSchemaV5, kSchemaV6, kSchemaV7};
        for (;;) {
            // Serialize the version read with the migration. Another connection may
            // have completed any number of steps while BEGIN IMMEDIATE waited.
            Transaction tx(db_);
            auto current = getSchemaVersion();
            if (!current.ok()) return current.error();
            const int version = current.value();
            if (version < 0 || version > CURRENT_SCHEMA_VERSION)
                fail(ErrorCode::DbError, "Database schema v" + std::to_string(version) + " is unsupported by this build");
            if (version >= targetVersion) { tx.commit(); break; }
            exec(db_, steps[version]);
            const std::string setVersion = "PRAGMA user_version = " + std::to_string(version + 1) + ";";
            exec(db_, setVersion.c_str());
            {
                Statement fkCheck(db_.rawHandle(), "PRAGMA foreign_key_check;");
                if (fkCheck.step()) {
                    fail(ErrorCode::DbError, "Migration to v" + std::to_string(version + 1) +
                                                 " left foreign key violations in table " + fkCheck.columnText(0));
                }
            }
            tx.commit();
        }
        return Status::success();
    });
}

Status PromptRepository::createSessionWithRootRevision(const core::PromptRevision& root, std::int64_t nowMs) {
    return guarded([&]() -> Status {
        const auto expectedId = core::RevisionState::makeRevisionId(root.sessionId(), 1);
        if (root.revisionId() != expectedId) {
            fail(ErrorCode::InvalidInput, "Root revision ID must be '" + expectedId.value() + "'");
        }
        if (root.parentRevisionId().has_value()) {
            fail(ErrorCode::InvalidInput, "Root revision cannot have a parent");
        }
        Transaction tx(db_);
        {
            Statement s(db_.rawHandle(),
                        "INSERT INTO sessions (session_id, state_version, highest_revision_seq, "
                        "draft_edit_generation, proposal_edit_generation, created_at, updated_at) "
                        "VALUES (?1, 1, 1, 0, 0, ?2, ?2);");
            s.text(1, root.sessionId().value()).integer(2, nowMs);
            s.run();
        }
        {
            Statement s(db_.rawHandle(),
                        "INSERT INTO revisions (session_id, revision_id, revision_seq, parent_revision_id, content, "
                        "state_version, draft_edit_generation, origin, created_at) "
                        "VALUES (?1, ?2, 1, NULL, ?3, 1, 0, 'ROOT', ?4);");
            s.text(1, root.sessionId().value())
                .text(2, root.revisionId().value())
                .text(3, root.text())
                .integer(4, root.createdAtMs());
            s.run();
        }
        tx.commit();
        return Status::success();
    });
}

Result<bool> PromptRepository::sessionExists(const core::SessionId& sessionId) const {
    return guarded([&]() -> Result<bool> { return sessionRowExists(db_.rawHandle(), sessionId.value()); });
}

Result<SessionRecord> PromptRepository::getSession(const core::SessionId& sessionId) const {
    return guarded([&]() -> Result<SessionRecord> {
        const std::string sql = std::string("SELECT ") + kSessionColumns + " FROM sessions WHERE session_id = ?1;";
        Statement s(db_.rawHandle(), sql.c_str());
        s.text(1, sessionId.value());
        if (!s.step()) {
            fail(ErrorCode::NotFound, "Session '" + sessionId.value() + "' does not exist");
        }
        return readSession(s);
    });
}

Result<std::vector<SessionRecord>> PromptRepository::listSessions() const {
    return guarded([&]() -> Result<std::vector<SessionRecord>> {
        const std::string sql =
            std::string("SELECT ") + kSessionColumns + " FROM sessions ORDER BY updated_at DESC, session_id;";
        Statement s(db_.rawHandle(), sql.c_str());
        std::vector<SessionRecord> out;
        while (s.step()) {
            out.push_back(readSession(s));
        }
        return out;
    });
}

Result<SessionSummaryPage> PromptRepository::listSessionSummariesPaged(
    const std::optional<std::int64_t>& afterUpdatedAt,
    const std::optional<std::string>& afterSessionId,
    int limit) const {
    return guarded([&]() -> Result<SessionSummaryPage> {
        if (limit <= 0 || limit > 50) limit = 50;
        std::string sql =
            "SELECT s.session_id, s.created_at, s.updated_at, s.highest_revision_seq, "
            "       r.revision_id, SUBSTR(r.content, 1, 512) "
            "FROM sessions s "
            "LEFT JOIN revisions r ON r.session_id = s.session_id AND r.revision_seq = s.highest_revision_seq ";
        const bool hasCursor = afterUpdatedAt.has_value() && afterSessionId.has_value();
        if (hasCursor) {
            sql += "WHERE (s.updated_at < ?1) OR (s.updated_at = ?1 AND s.session_id > ?2) ";
        }
        sql += "ORDER BY s.updated_at DESC, s.session_id ASC LIMIT ?" + std::to_string(hasCursor ? 3 : 1) + ";";

        Statement s(db_.rawHandle(), sql.c_str());
        if (hasCursor) {
            s.integer(1, *afterUpdatedAt).text(2, *afterSessionId).integer(3, limit + 1);
        } else {
            s.integer(1, limit + 1);
        }

        SessionSummaryPage page;
        while (s.step()) {
            if (static_cast<int>(page.items.size()) == limit) {
                page.hasMore = true;
                break;
            }
            SessionSummary summary{core::SessionId(s.columnText(0))};
            summary.createdAtMs = s.columnInt(1);
            summary.updatedAtMs = s.columnInt(2);
            summary.revisionCount = static_cast<std::size_t>(s.columnUInt(3));
            if (!s.columnIsNull(4)) {
                summary.latestRevisionId = s.columnText(4);
            }
            if (!s.columnIsNull(5)) {
                summary.promptPreview = s.columnText(5);
            }
            page.items.push_back(std::move(summary));
        }
        return page;
    });
}

Result<std::size_t> PromptRepository::getSessionCount() const {
    return guarded([&]() -> Result<std::size_t> {
        Statement s(db_.rawHandle(), "SELECT COUNT(*) FROM sessions;");
        if (!s.step()) return 0;
        return static_cast<std::size_t>(s.columnUInt(0));
    });
}

Result<std::size_t> PromptRepository::getTotalRevisionCount() const {
    return guarded([&]() -> Result<std::size_t> {
        Statement s(db_.rawHandle(), "SELECT COUNT(*) FROM revisions;");
        if (!s.step()) return 0;
        return static_cast<std::size_t>(s.columnUInt(0));
    });
}

Status PromptRepository::deleteSession(const core::SessionId& sessionId) {
    return guarded([&]() -> Status {
        Transaction tx(db_);
        Statement s(db_.rawHandle(), "DELETE FROM sessions WHERE session_id = ?1;");
        s.text(1, sessionId.value());
        s.run();
        if (sqlite3_changes(db_.rawHandle()) == 0) {
            fail(ErrorCode::NotFound, "Session '" + sessionId.value() + "' does not exist");
        }
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::purgeHistory() {
    return guarded([&]() -> Status {
        Transaction tx(db_);
        exec(db_, "DELETE FROM model_attempts;");
        exec(db_, "DELETE FROM approvals;");
        exec(db_, "DELETE FROM proposals;");
        exec(db_, "DELETE FROM intent_versions;");
        exec(db_, "DELETE FROM analysis_snapshots;");
        exec(db_, "DELETE FROM revisions;");
        exec(db_, "DELETE FROM sessions;");
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::advanceSessionState(const core::RevisionState& next, std::uint64_t expectedVersion, std::int64_t nowMs) {
    return guarded([&]() -> Status {
        if (expectedVersion >= core::RevisionState::kMaxStateVersion || next.stateVersion() != expectedVersion + 1) fail(ErrorCode::InvalidInput, "A mutation must advance state by exactly one");
        Transaction transaction(db_);
        advanceStoredState(db_, next.currentRevision().sessionId().value(), next.currentRevision().revisionId().value(),
                           expectedVersion, nowMs, next.draftEditGeneration(), next.proposalEditGeneration(), next.highestIssuedSequence());
        transaction.commit();
        return Status::success();
    });
}

Status PromptRepository::advanceDraftStateRange(const core::RevisionState& next, std::uint64_t expectedVersion, std::int64_t nowMs) {
    return guarded([&]() -> Status {
        if (nowMs < 0 || !next.isDraftDirty() || next.stateVersion() <= expectedVersion || next.stateVersion() > core::RevisionState::kMaxStateVersion)
            fail(ErrorCode::InvalidInput, "Invalid draft version range");
        Transaction tx(db_);
        const auto& revision = next.currentRevision();
        auto current = getSession(revision.sessionId());
        if (!current.ok()) return current.error();
        const auto delta = next.stateVersion() - expectedVersion;
        if (current.value().stateVersion != expectedVersion || current.value().highestRevisionSeq != next.highestIssuedSequence() ||
            next.draftEditGeneration() < current.value().draftEditGeneration ||
            next.draftEditGeneration() - current.value().draftEditGeneration != delta ||
            next.proposalEditGeneration() != current.value().proposalEditGeneration)
            fail(ErrorCode::StaleVersion, "Draft state changed before it could be saved");
        auto latest = getLatestRevision(revision.sessionId());
        if (!latest.ok()) return latest.error();
        if (!latest.value() || latest.value()->revision.revisionId() != revision.revisionId())
            fail(ErrorCode::StaleVersion, "Draft revision changed before it could be saved");
        Statement update(db_.rawHandle(), "UPDATE sessions SET state_version=?1, draft_edit_generation=?2, updated_at=?3 WHERE session_id=?4 AND state_version=?5;");
        update.unsignedInt(1, next.stateVersion()).unsignedInt(2, next.draftEditGeneration()).integer(3, nowMs)
            .text(4, revision.sessionId().value()).unsignedInt(5, expectedVersion); update.run();
        if (sqlite3_changes(db_.rawHandle()) != 1) fail(ErrorCode::StaleVersion, "Draft write lost its version check");
        Statement approvals(db_.rawHandle(), "UPDATE approvals SET is_current=0 WHERE session_id=?1;");
        approvals.text(1, revision.sessionId().value()); approvals.run();
        tx.commit(); return Status::success();
    });
}

Status PromptRepository::recordProposalGeneration(const core::SessionId& sessionId,
                                                  std::uint64_t proposalEditGeneration,
                                                  std::int64_t nowMs) {
    return guarded([&]() -> Status {
        Transaction tx(db_);
        {
            Statement s(db_.rawHandle(),
                        "UPDATE sessions SET proposal_edit_generation = MAX(proposal_edit_generation, ?1), "
                        "updated_at = ?2 WHERE session_id = ?3;");
            s.unsignedInt(1, proposalEditGeneration).integer(2, nowMs).text(3, sessionId.value());
            s.run();
        }
        if (sqlite3_changes(db_.rawHandle()) == 0) {
            fail(ErrorCode::NotFound, "Session '" + sessionId.value() + "' does not exist");
        }
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::insertRevisionWithOrigin(const RevisionWrite& w, RevisionOrigin origin) {
    return guarded([&]() -> Status {
        const auto& rev = w.revision;
        if (w.sequence < 2) {
            fail(ErrorCode::InvalidInput, "Non-root revisions must have sequence >= 2");
        }
        const auto expectedId = core::RevisionState::makeRevisionId(rev.sessionId(), w.sequence);
        if (rev.revisionId() != expectedId) {
            fail(ErrorCode::InvalidInput, "Revision ID must be '" + expectedId.value() + "'");
        }
        if (!rev.parentRevisionId().has_value()) {
            fail(ErrorCode::InvalidInput, "Non-root revisions require a parent");
        }
        if (w.newStateVersion != w.expectedStateVersion + 1) {
            fail(ErrorCode::InvalidInput, "New state version must follow the expected version");
        }

        Transaction tx(db_);
        {
            Statement s(db_.rawHandle(),
                        "UPDATE sessions SET state_version = ?1, highest_revision_seq = ?2, "
                        "draft_edit_generation = ?3, proposal_edit_generation = ?4, updated_at = ?5 "
                        "WHERE session_id = ?6 AND state_version = ?7 AND highest_revision_seq = ?8;");
            s.unsignedInt(1, w.newStateVersion)
                .unsignedInt(2, w.sequence)
                .unsignedInt(3, w.draftEditGeneration)
                .unsignedInt(4, w.proposalEditGeneration)
                .integer(5, w.nowMs)
                .text(6, rev.sessionId().value())
                .unsignedInt(7, w.expectedStateVersion)
                .unsignedInt(8, w.sequence - 1);
            s.run();
        }
        if (sqlite3_changes(db_.rawHandle()) == 0) {
            if (!sessionRowExists(db_.rawHandle(), rev.sessionId().value())) {
                fail(ErrorCode::NotFound, "Session '" + rev.sessionId().value() + "' does not exist");
            }
            fail(ErrorCode::StaleVersion, "Session state version or revision sequence changed");
        }
        {
            Statement s(db_.rawHandle(),
                        "INSERT INTO revisions (session_id, revision_id, revision_seq, parent_revision_id, content, "
                        "state_version, draft_edit_generation, origin, created_at) "
                        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
            s.text(1, rev.sessionId().value())
                .text(2, rev.revisionId().value())
                .unsignedInt(3, w.sequence)
                .text(4, rev.parentRevisionId()->value())
                .text(5, rev.text())
                .unsignedInt(6, w.newStateVersion)
                .unsignedInt(7, w.draftEditGeneration)
                .text(8, toString(origin))
                .integer(9, rev.createdAtMs());
            s.run();
        }
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::insertRevision(const RevisionWrite& write) {
    return insertRevisionWithOrigin(write, RevisionOrigin::Commit);
}

Status PromptRepository::insertDraftRevision(const RevisionWrite& write) {
    return insertRevisionWithOrigin(write, RevisionOrigin::DraftFreeze);
}

Status PromptRepository::insertRestoredRevision(const RevisionWrite& write) {
    return insertRevisionWithOrigin(write, RevisionOrigin::Restore);
}

Result<std::optional<RevisionRecord>> PromptRepository::getRevision(const core::SessionId& sessionId,
                                                                    const core::RevisionId& revisionId) const {
    return guarded([&]() -> Result<std::optional<RevisionRecord>> {
        const std::string sql = std::string("SELECT ") + kRevisionColumns +
                                " FROM revisions WHERE session_id = ?1 AND revision_id = ?2;";
        Statement s(db_.rawHandle(), sql.c_str());
        s.text(1, sessionId.value()).text(2, revisionId.value());
        if (!s.step()) {
            return std::optional<RevisionRecord>{};
        }
        return std::optional<RevisionRecord>(readRevision(s));
    });
}

Result<std::vector<RevisionRecord>> PromptRepository::getHistory(const core::SessionId& sessionId) const {
    return guarded([&]() -> Result<std::vector<RevisionRecord>> {
        const std::string sql = std::string("SELECT ") + kRevisionColumns +
                                " FROM revisions WHERE session_id = ?1 ORDER BY revision_seq ASC;";
        Statement s(db_.rawHandle(), sql.c_str());
        s.text(1, sessionId.value());
        std::vector<RevisionRecord> out;
        while (s.step()) {
            out.push_back(readRevision(s));
        }
        return out;
    });
}

Result<RevisionSummaryPage> PromptRepository::listRevisionSummariesPaged(
    const core::SessionId& sessionId, std::uint64_t afterSequence) const {
    return guarded([&]() -> Result<RevisionSummaryPage> {
        Statement s(db_.rawHandle(),
            "SELECT revision_id, parent_revision_id, SUBSTR(content,1,512), origin, revision_seq, created_at "
            "FROM revisions WHERE session_id=?1 AND revision_seq>?2 ORDER BY revision_seq ASC LIMIT 51;");
        s.text(1, sessionId.value()).unsignedInt(2, afterSequence);
        RevisionSummaryPage page;
        while (s.step()) {
            if (page.items.size() == 50) { page.hasMore = true; break; }
            page.items.push_back({s.columnText(0), s.columnIsNull(1) ? "" : s.columnText(1),
                s.columnText(2), s.columnText(3), s.columnUInt(4), s.columnInt(5)});
        }
        Statement count(db_.rawHandle(), "SELECT COUNT(*) FROM revisions WHERE session_id=?1;");
        count.text(1, sessionId.value());
        if (count.step()) page.total = static_cast<std::size_t>(count.columnUInt(0));
        return page;
    });
}

Result<std::vector<RevisionRecord>> PromptRepository::getHistoryPaged(
    const core::SessionId& sessionId,
    std::uint64_t fromSeq,
    int limit) const {
    return guarded([&]() -> Result<std::vector<RevisionRecord>> {
        if (limit <= 0 || limit > 50) limit = 50;
        const std::string sql = std::string("SELECT ") + kRevisionColumns +
                                " FROM revisions WHERE session_id = ?1 AND revision_seq >= ?2 ORDER BY revision_seq ASC LIMIT ?3;";
        Statement s(db_.rawHandle(), sql.c_str());
        s.text(1, sessionId.value()).unsignedInt(2, fromSeq).integer(3, limit);
        std::vector<RevisionRecord> out;
        while (s.step()) {
            out.push_back(readRevision(s));
        }
        return out;
    });
}

Result<std::optional<RevisionRecord>> PromptRepository::getLatestRevision(const core::SessionId& sessionId) const {
    return guarded([&]() -> Result<std::optional<RevisionRecord>> {
        const std::string sql = std::string("SELECT ") + kRevisionColumns +
                                " FROM revisions WHERE session_id = ?1 ORDER BY revision_seq DESC LIMIT 1;";
        Statement s(db_.rawHandle(), sql.c_str());
        s.text(1, sessionId.value());
        if (!s.step()) {
            return std::optional<RevisionRecord>{};
        }
        return std::optional<RevisionRecord>(readRevision(s));
    });
}

Status PromptRepository::insertAnalysisSnapshot(const core::analysis::AnalysisSnapshot& analysis, std::int64_t nowMs, std::optional<std::uint64_t> expectedVersion) {
    return guarded([&]() -> Status {
        if (auto status = core::analysis::validateAnalysisSnapshot(analysis); !status.ok()) return status;
        const auto& normalized = analysis.normalized;
        auto revision = getRevision(core::SessionId(normalized.sessionId), core::RevisionId(normalized.baseRevisionId));
        if (!revision.ok()) return revision.error();
        if (!revision.value() || analysis.revisionTextHash != core::crypto::Sha256::hexDigest(revision.value()->revision.text()) ||
            core::analysis::validateNormalizedAnalysis(normalized, revision.value()->revision.text()).status == core::analysis::AnalysisValidationStatus::Invalid) {
            fail(ErrorCode::ValidationFailed, "Final analysis does not match a stored revision");
        }
        validateSnapshotAttempts(db_, analysis);
        Transaction tx(db_);
        if (expectedVersion) advanceStoredState(db_, normalized.sessionId, normalized.baseRevisionId, *expectedVersion, nowMs);
        Statement s(db_.rawHandle(),
                    "INSERT INTO analysis_snapshots (snapshot_id, session_id, base_revision_id, snapshot_hash, "
                    "payload_json, created_at, format_version) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 2) "
                    "ON CONFLICT(snapshot_id) DO NOTHING;");
        s.text(1, analysis.id)
            .text(2, normalized.sessionId)
            .text(3, normalized.baseRevisionId)
            .text(4, analysis.snapshotHash)
            .text(5, encodeAnalysisSnapshot(analysis))
            .integer(6, nowMs);
        s.run();
        Statement invalidate(db_.rawHandle(), "UPDATE approvals SET is_current = 0 WHERE session_id = ?1 AND snapshot_id <> ?2;");
        invalidate.text(1, normalized.sessionId).text(2, analysis.id);
        invalidate.run();
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::insertIntentVersion(const core::intent::ConfirmedIntent& intent, std::int64_t nowMs, std::optional<std::uint64_t> expectedVersion) {
    return guarded([&]() -> Status {
        const auto recomputed = core::intent::IntentProvenanceBuilder::computeIntentHash(intent);
        if (recomputed != intent.intentHash || intent.id != "intent_" + recomputed) {
            fail(ErrorCode::InvalidInput, "Intent hash does not match its contents");
        }
        Statement source(db_.rawHandle(), "SELECT payload_json, format_version FROM analysis_snapshots WHERE snapshot_id = ?1 AND session_id = ?2 AND base_revision_id = ?3;");
        source.text(1, intent.snapshotId).text(2, intent.sessionId).text(3, intent.baseRevisionId);
        if (!source.step() || source.columnInt(1) != 2) fail(ErrorCode::ValidationFailed, "Intent requires a persisted final analysis artifact");
        auto snapshot = decodeAnalysisSnapshot(source.columnText(0));
        if (!snapshot.ok()) return snapshot.error();
        if (auto status = core::intent::IntentProvenanceBuilder::isIntentValid(intent, intent.baseRevisionId, core::RevisionStatus::Clean, snapshot.value()); !status.ok()) return status;
        Transaction tx(db_);
        if (expectedVersion) advanceStoredState(db_, intent.sessionId, intent.baseRevisionId, *expectedVersion, nowMs);
        Statement s(db_.rawHandle(),
                    "INSERT INTO intent_versions (intent_id, session_id, base_revision_id, snapshot_id, intent_hash, "
                    "payload_json, created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
                    "ON CONFLICT(intent_id) DO NOTHING;");
        s.text(1, intent.id)
            .text(2, intent.sessionId)
            .text(3, intent.baseRevisionId)
            .text(4, intent.snapshotId)
            .text(5, intent.intentHash)
            .text(6, encodeIntent(intent))
            .integer(7, nowMs);
        s.run();
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::insertProposal(const core::rewrite::RewriteProposal& proposal, std::int64_t nowMs, std::optional<std::uint64_t> expectedVersion) {
    return guarded([&]() -> Status {
        const auto recomputed = core::rewrite::ProposalHasher::computeProposalHash(proposal);
        if (recomputed != proposal.proposalHash || proposal.id != "prop_" + recomputed) {
            fail(ErrorCode::InvalidInput, "Proposal hash does not match its contents");
        }
        Transaction tx(db_);
        if (expectedVersion) advanceStoredState(db_, proposal.sessionId, proposal.baseRevisionId, *expectedVersion, nowMs, std::nullopt, proposal.proposalEditGeneration);
        Statement s(db_.rawHandle(),
                    "INSERT INTO proposals (proposal_id, session_id, base_revision_id, intent_id, proposal_hash, "
                    "proposal_edit_generation, payload_json, created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                    "ON CONFLICT(proposal_id) DO NOTHING;");
        s.text(1, proposal.id)
            .text(2, proposal.sessionId)
            .text(3, proposal.baseRevisionId)
            .text(4, proposal.intentId)
            .text(5, proposal.proposalHash)
            .unsignedInt(6, proposal.proposalEditGeneration)
            .text(7, encodeProposal(proposal))
            .integer(8, nowMs);
        s.run();
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::insertApproval(const core::review::ApprovalRecord& a, std::optional<std::uint64_t> expectedVersion) {
    return guarded([&]() -> Status {
        if (a.bindingHash != a.computeBindingHash() || a.id != "appr_" + a.bindingHash) {
            fail(ErrorCode::InvalidInput, "Approval binding hash does not match its fields");
        }
        Transaction tx(db_);
        if (expectedVersion) {
            if (*expectedVersion >= core::RevisionState::kMaxStateVersion || a.stateVersion != *expectedVersion + 1) fail(ErrorCode::InvalidInput, "Approval has the wrong post-command version");
            advanceStoredState(db_, a.sessionId, a.revisionId, *expectedVersion, a.createdAtMs, a.draftEditGeneration, a.proposalEditGeneration);
        }
        {
            Statement s(db_.rawHandle(),
                        "UPDATE approvals SET is_current = 0 WHERE session_id = ?1 AND approval_id <> ?2;");
            s.text(1, a.sessionId).text(2, a.id);
            s.run();
        }
        {
            const std::string sql =
                std::string("INSERT INTO approvals (") + kApprovalColumns +
                ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, 1, ?18) "
                "ON CONFLICT(approval_id) DO UPDATE SET is_current = 1;";
            Statement s(db_.rawHandle(), sql.c_str());
            s.text(1, a.id)
                .text(2, a.sessionId)
                .text(3, a.revisionId)
                .unsignedInt(4, a.stateVersion)
                .text(5, a.snapshotId)
                .text(6, a.snapshotHash)
                .text(7, a.intentId)
                .text(8, a.intentHash)
                .text(9, a.proposalId)
                .text(10, a.proposalHash)
                .text(11, a.approvedText)
                .unsignedInt(12, a.draftEditGeneration)
                .unsignedInt(13, a.proposalEditGeneration)
                .text(14, a.validatorVersion)
                .text(15, a.policyVersion)
                .text(16, a.rewriteProfile)
                .integer(17, a.createdAtMs)
                .text(18, a.bindingHash);
            s.run();
        }
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::invalidateApprovals(const core::SessionId& sessionId) {
    return guarded([&]() -> Status {
        Transaction tx(db_);
        Statement s(db_.rawHandle(), "UPDATE approvals SET is_current = 0 WHERE session_id = ?1;");
        s.text(1, sessionId.value());
        s.run();
        tx.commit();
        return Status::success();
    });
}

Result<std::optional<core::review::ApprovalRecord>> PromptRepository::getCurrentApproval(
    const core::SessionId& sessionId) const {
    return guarded([&]() -> Result<std::optional<core::review::ApprovalRecord>> {
        const std::string sql = std::string("SELECT ") + kApprovalColumns +
                                " FROM approvals WHERE session_id = ?1 AND is_current = 1 "
                                "ORDER BY created_at DESC, rowid DESC LIMIT 1;";
        Statement s(db_.rawHandle(), sql.c_str());
        s.text(1, sessionId.value());
        if (!s.step()) {
            return std::optional<core::review::ApprovalRecord>{};
        }
        return std::optional<core::review::ApprovalRecord>(readApproval(s));
    });
}

Result<std::vector<core::review::ApprovalRecord>> PromptRepository::getApprovalHistory(
    const core::SessionId& sessionId) const {
    return guarded([&]() -> Result<std::vector<core::review::ApprovalRecord>> {
        const std::string sql = std::string("SELECT ") + kApprovalColumns +
                                " FROM approvals WHERE session_id = ?1 ORDER BY created_at ASC, rowid ASC;";
        Statement s(db_.rawHandle(), sql.c_str());
        s.text(1, sessionId.value());
        std::vector<core::review::ApprovalRecord> out;
        while (s.step()) {
            out.push_back(readApproval(s));
        }
        return out;
    });
}

Status PromptRepository::recordDeliveryEvent(const DeliveryEventRecord& e) {
    return guarded([&]() -> Status {
        if (e.eventId.empty() || e.sessionId.empty() || e.revisionId.empty() ||
            (e.outcome != "INSERTED" && e.outcome != "COPIED") || e.completedAtMs <= 0 ||
            e.textHash.size() != 64 || e.textHash.find_first_not_of("0123456789abcdef") != std::string::npos)
            fail(ErrorCode::InvalidInput, "Invalid completed delivery event");
        Transaction tx(db_);
        Statement s(db_.rawHandle(), "INSERT INTO delivery_events (event_id,session_id,revision_id,outcome,text_hash,app_identifier,destination,scope_summary,field_token,process_id,completed_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) ON CONFLICT(event_id) DO NOTHING;");
        s.text(1,e.eventId).text(2,e.sessionId).text(3,e.revisionId).text(4,e.outcome).text(5,e.textHash)
            .text(6,e.appIdentifier).text(7,e.destination).text(8,e.scopeSummary).text(9,std::to_string(e.fieldToken))
            .integer(10,e.processId).integer(11,e.completedAtMs); s.run();
        Statement check(db_.rawHandle(), "SELECT session_id,revision_id,outcome,text_hash,app_identifier,destination,scope_summary,field_token,process_id,completed_at FROM delivery_events WHERE event_id=?1;");
        check.text(1,e.eventId);
        if (!check.step() || check.columnText(0)!=e.sessionId || check.columnText(1)!=e.revisionId ||
            check.columnText(2)!=e.outcome || check.columnText(3)!=e.textHash || check.columnText(4)!=e.appIdentifier ||
            check.columnText(5)!=e.destination || check.columnText(6)!=e.scopeSummary ||
            check.columnText(7)!=std::to_string(e.fieldToken) || check.columnInt(8)!=e.processId || check.columnInt(9)!=e.completedAtMs)
            fail(ErrorCode::Conflict, "A delivery event ID cannot be reused for a different completion");
        tx.commit(); return Status::success();
    });
}

Result<std::vector<DeliveryEventRecord>> PromptRepository::getDeliveryEvents(const core::SessionId& session) const {
    return guarded([&]() -> Result<std::vector<DeliveryEventRecord>> {
        Statement s(db_.rawHandle(), "SELECT event_id,session_id,revision_id,outcome,text_hash,app_identifier,destination,scope_summary,field_token,process_id,completed_at FROM delivery_events WHERE session_id=?1 ORDER BY completed_at DESC,rowid DESC LIMIT 100;");
        s.text(1,session.value()); std::vector<DeliveryEventRecord> events;
        while(s.step()) {
            DeliveryEventRecord e; e.eventId=s.columnText(0);e.sessionId=s.columnText(1);e.revisionId=s.columnText(2);
            e.outcome=s.columnText(3);e.textHash=s.columnText(4);e.appIdentifier=s.columnText(5);e.destination=s.columnText(6);
            e.scopeSummary=s.columnText(7);e.fieldToken=std::stoull(s.columnText(8));e.processId=s.columnInt(9);e.completedAtMs=s.columnInt(10);
            events.push_back(std::move(e));
        }
        return events;
    });
}

Status PromptRepository::beginModelAttempt(const ModelAttemptRecord& a) {
    return guarded([&]() -> Status {
        if (a.status != AttemptStatus::InProgress) {
            fail(ErrorCode::InvalidInput, "New model attempts must start IN_PROGRESS");
        }
        if (a.transport.empty()) {
            fail(ErrorCode::InvalidInput, "Model attempts must record their transport");
        }
        Transaction tx(db_);
        Statement s(db_.rawHandle(),
                    "INSERT INTO model_attempts (attempt_id, session_id, revision_id, stage, status, transport, "
                    "model_id, model_detail, error_message, started_at, completed_at) "
                    "VALUES (?1, ?2, ?3, ?4, 'IN_PROGRESS', ?5, ?6, ?7, NULL, ?8, NULL);");
        s.text(1, a.attemptId)
            .text(2, a.sessionId)
            .text(3, a.revisionId)
            .text(4, a.stage)
            .text(5, a.transport)
            .text(6, a.modelId)
            .text(7, a.modelDetail)
            .integer(8, a.startedAtMs);
        s.run();
        tx.commit();
        return Status::success();
    });
}

Status PromptRepository::completeModelAttempt(const std::string& attemptId,
                                              AttemptStatus status,
                                              const std::string& modelId,
                                              const std::string& modelDetail,
                                              const std::string& errorMessage,
                                              std::int64_t completedAtMs) {
    return guarded([&]() -> Status {
        if (status == AttemptStatus::InProgress) {
            fail(ErrorCode::InvalidInput, "Completion status cannot be IN_PROGRESS");
        }
        Transaction tx(db_);
        Statement s(db_.rawHandle(),
                    "UPDATE model_attempts SET status = ?1, model_id = ?2, model_detail = ?3, error_message = ?4, "
                    "completed_at = ?5 WHERE attempt_id = ?6 AND status = 'IN_PROGRESS';");
        s.text(1, toString(status)).text(2, modelId).text(3, modelDetail);
        if (errorMessage.empty()) {
            s.null(4);
        } else {
            s.text(4, errorMessage);
        }
        s.integer(5, completedAtMs).text(6, attemptId);
        s.run();
        if (sqlite3_changes(db_.rawHandle()) == 0) {
            fail(ErrorCode::NotFound, "No in-progress model attempt '" + attemptId + "'");
        }
        tx.commit();
        return Status::success();
    });
}

Result<std::vector<ModelAttemptRecord>> PromptRepository::getModelAttempts(const core::SessionId& sessionId) const {
    return guarded([&]() -> Result<std::vector<ModelAttemptRecord>> {
        Statement s(db_.rawHandle(),
                    "SELECT attempt_id, session_id, revision_id, stage, status, transport, model_id, model_detail, "
                    "error_message, started_at, completed_at FROM model_attempts WHERE session_id = ?1 "
                    "ORDER BY started_at ASC, rowid ASC;");
        s.text(1, sessionId.value());
        std::vector<ModelAttemptRecord> out;
        while (s.step()) {
            ModelAttemptRecord a;
            a.attemptId = s.columnText(0);
            a.sessionId = s.columnText(1);
            a.revisionId = s.columnText(2);
            a.stage = s.columnText(3);
            a.status = attemptStatusFromString(s.columnText(4));
            a.transport = s.columnText(5);
            a.modelId = s.columnText(6);
            a.modelDetail = s.columnText(7);
            a.errorMessage = s.columnText(8);
            a.startedAtMs = s.columnInt(9);
            if (!s.columnIsNull(10)) {
                a.completedAtMs = s.columnInt(10);
            }
            out.push_back(std::move(a));
        }
        return out;
    });
}

Result<std::size_t> PromptRepository::interruptOrphanedAttemptsOnStartup(std::int64_t nowMs) {
    return guarded([&]() -> Result<std::size_t> {
        Transaction tx(db_);
        Statement s(db_.rawHandle(),
                    "UPDATE model_attempts SET status = 'INTERRUPTED', completed_at = ?1, "
                    "error_message = 'Application exited before the attempt completed' "
                    "WHERE status = 'IN_PROGRESS';");
        s.integer(1, nowMs);
        s.run();
        const auto changed = static_cast<std::size_t>(sqlite3_changes(db_.rawHandle()));
        tx.commit();
        return changed;
    });
}

Result<RestoredSession> PromptRepository::restoreSession(const core::SessionId& sessionId) const {
    auto session = getSession(sessionId);
    if (!session.ok()) {
        return session.error();
    }
    auto latest = getLatestRevision(sessionId);
    if (!latest.ok()) {
        return latest.error();
    }
    if (!latest.value().has_value()) {
        return makeError(ErrorCode::ValidationFailed, "Session '" + sessionId.value() + "' has no revisions");
    }

    return guarded([&]() -> Result<RestoredSession> {
        RestoredSession restored{session.value(), *latest.value()};
        auto deliveries = getDeliveryEvents(sessionId);
        if (!deliveries.ok()) return deliveries.error();
        if (!deliveries.value().empty()) restored.lastDelivery = deliveries.value().front();
        const std::string revisionId = restored.latestRevision.revision.revisionId().value();

        {
            Statement s(db_.rawHandle(),
                        "SELECT payload_json, format_version, snapshot_id, snapshot_hash FROM analysis_snapshots WHERE session_id = ?1 AND base_revision_id = ?2 "
                        "ORDER BY created_at DESC, rowid DESC LIMIT 1;");
            s.text(1, sessionId.value()).text(2, revisionId);
            if (!s.step()) {
                return restored;
            }
            if (s.columnInt(1) == 1) {
                restored.legacyAnalysisRequiresReview = true;
                return restored;
            }
            auto analysis = decodeAnalysisSnapshot(s.columnText(0));
            if (!analysis.ok()) {
                return analysis.error();
            }
            if (analysis.value().normalized.sessionId != sessionId.value() || analysis.value().normalized.baseRevisionId != revisionId) {
                fail(ErrorCode::ValidationFailed, "Stored analysis payload is bound to a different revision");
            }
            if (analysis.value().id != s.columnText(2) || analysis.value().snapshotHash != s.columnText(3)) {
                fail(ErrorCode::ValidationFailed, "Stored analysis key/hash differs from its payload");
            }
            validateSnapshotAttempts(db_, analysis.value());
            if (analysis.value().revisionTextHash != core::crypto::Sha256::hexDigest(restored.latestRevision.revision.text())) {
                fail(ErrorCode::ValidationFailed, "Saved analysis belongs to different frozen prompt bytes");
            }
            restored.analysis = std::move(analysis).value();
        }
        {
            Statement s(db_.rawHandle(),
                        "SELECT payload_json FROM intent_versions WHERE session_id = ?1 AND base_revision_id = ?2 "
                        "AND snapshot_id = ?3 ORDER BY created_at DESC, rowid DESC LIMIT 1;");
            s.text(1, sessionId.value()).text(2, revisionId).text(3, restored.analysis->id);
            if (!s.step()) {
                return restored;
            }
            auto intent = decodeIntent(s.columnText(0));
            if (!intent.ok()) {
                return intent.error();
            }
            if (intent.value().snapshotHash != restored.analysis->snapshotHash) {
                fail(ErrorCode::ValidationFailed, "Stored intent is bound to a different snapshot hash");
            }
            restored.intent = std::move(intent).value();
        }
        {
            Statement s(db_.rawHandle(),
                        "SELECT payload_json FROM proposals WHERE session_id = ?1 AND base_revision_id = ?2 "
                        "AND intent_id = ?3 ORDER BY created_at DESC, rowid DESC LIMIT 1;");
            s.text(1, sessionId.value()).text(2, revisionId).text(3, restored.intent->id);
            if (!s.step()) {
                return restored;
            }
            auto proposal = decodeProposal(s.columnText(0));
            if (!proposal.ok()) {
                return proposal.error();
            }
            if (proposal.value().intentHash != restored.intent->intentHash) {
                fail(ErrorCode::ValidationFailed, "Stored proposal is bound to a different intent hash");
            }
            restored.proposal = std::move(proposal).value();
        }
        {
            const std::string sql = std::string("SELECT ") + kApprovalColumns +
                                    " FROM approvals WHERE session_id = ?1 AND proposal_id = ?2 AND is_current = 1 "
                                    "ORDER BY created_at DESC, rowid DESC LIMIT 1;";
            Statement s(db_.rawHandle(), sql.c_str());
            s.text(1, sessionId.value()).text(2, restored.proposal->id);
            if (s.step()) {
                restored.approval = readApproval(s);
            }
        }
        return restored;
    });
}

} // namespace roclint::storage
