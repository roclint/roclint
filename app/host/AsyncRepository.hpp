#pragma once
#include "StorageService.hpp"
namespace roclint::host {
using storage::SessionRecord; using storage::RevisionRecord; using storage::RevisionWrite; using storage::RestoredSession; using storage::ModelAttemptRecord; using storage::AttemptStatus;
// GUI-side value proxy. No SQLite handles or repository instances cross the thread boundary.
class AsyncRepository {
public:
    using Guard = std::function<core::Status(bool, const core::Error*)>;
    explicit AsyncRepository(StorageService& target) : service(target) {}
    std::function<Guard(bool)> guardFactory;
    std::function<void(const core::Error&)> errorHandler;
    AsyncTask<core::Result<int>> getSchemaVersion() {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::getSchemaVersion);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> migrate() {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::migrate);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> migrateTo(int targetVersion) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::migrateTo, targetVersion);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> createSessionWithRootRevision(core::PromptRevision root, std::int64_t nowMs) {
        auto guard = guardFactory ? guardFactory(true) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::createSessionWithRootRevision, root, nowMs);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Result<bool>> sessionExists(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::sessionExists, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<SessionRecord>> getSession(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::getSession, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<std::vector<SessionRecord>>> listSessions() {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::listSessions);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> deleteSession(core::SessionId sessionId) {
        auto guard = guardFactory ? guardFactory(true) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::deleteSession, sessionId);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> purgeHistory() {
        auto guard = guardFactory ? guardFactory(true) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::purgeHistory);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> advanceSessionState(core::RevisionState next, std::uint64_t expectedVersion, std::int64_t nowMs) {
        auto guard = guardFactory ? guardFactory(true) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::advanceSessionState, next, expectedVersion, nowMs);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> recordProposalGeneration(core::SessionId sessionId, std::uint64_t proposalEditGeneration, std::int64_t nowMs) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::recordProposalGeneration, sessionId, proposalEditGeneration, nowMs);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> insertRevision(RevisionWrite write) {
        auto guard = guardFactory ? guardFactory(true) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::insertRevision, write);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> insertDraftRevision(RevisionWrite write) {
        auto guard = guardFactory ? guardFactory(true) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::insertDraftRevision, write);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> insertRestoredRevision(RevisionWrite write) {
        auto guard = guardFactory ? guardFactory(true) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::insertRestoredRevision, write);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Result<std::optional<RevisionRecord>>> getRevision(core::SessionId sessionId, core::RevisionId revisionId) {
        auto guard = guardFactory ? guardFactory(false) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::getRevision, sessionId, revisionId);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Result<std::vector<RevisionRecord>>> getHistory(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::getHistory, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<std::optional<RevisionRecord>>> getLatestRevision(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::getLatestRevision, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> insertAnalysisSnapshot(core::analysis::AnalysisSnapshot analysis, std::int64_t nowMs, std::optional<std::uint64_t> expectedVersion = std::nullopt) {
        auto guard = guardFactory ? guardFactory(expectedVersion.has_value()) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::insertAnalysisSnapshot, analysis, nowMs, expectedVersion);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> insertIntentVersion(core::intent::ConfirmedIntent intent, std::int64_t nowMs, std::optional<std::uint64_t> expectedVersion = std::nullopt) {
        auto guard = guardFactory ? guardFactory(expectedVersion.has_value()) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::insertIntentVersion, intent, nowMs, expectedVersion);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> insertProposal(core::rewrite::RewriteProposal proposal, std::int64_t nowMs, std::optional<std::uint64_t> expectedVersion = std::nullopt) {
        auto guard = guardFactory ? guardFactory(expectedVersion.has_value()) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::insertProposal, proposal, nowMs, expectedVersion);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> insertApproval(core::review::ApprovalRecord approval, std::optional<std::uint64_t> expectedVersion = std::nullopt) {
        auto guard = guardFactory ? guardFactory(expectedVersion.has_value()) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::insertApproval, approval, expectedVersion);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
    AsyncTask<core::Status> invalidateApprovals(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::invalidateApprovals, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<std::optional<core::review::ApprovalRecord>>> getCurrentApproval(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::getCurrentApproval, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<std::vector<core::review::ApprovalRecord>>> getApprovalHistory(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::getApprovalHistory, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> beginModelAttempt(ModelAttemptRecord attempt) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::beginModelAttempt, attempt);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Status> completeModelAttempt(std::string attemptId, AttemptStatus status, std::string modelId, std::string modelDetail, std::string errorMessage, std::int64_t completedAtMs) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::completeModelAttempt, attemptId, status, modelId, modelDetail, errorMessage, completedAtMs);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<std::vector<ModelAttemptRecord>>> getModelAttempts(core::SessionId sessionId) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::getModelAttempts, sessionId);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<std::size_t>> interruptOrphanedAttemptsOnStartup(std::int64_t nowMs) {
        const auto reportError = errorHandler;
        auto result = co_await service.call(&storage::PromptRepository::interruptOrphanedAttemptsOnStartup, nowMs);
        if (!result.ok() && result.error().code == core::ErrorCode::DbError && reportError) reportError(result.error());
        co_return result;
    }
    AsyncTask<core::Result<RestoredSession>> restoreSession(core::SessionId sessionId) {
        auto guard = guardFactory ? guardFactory(false) : Guard{};
        auto result = co_await service.call(&storage::PromptRepository::restoreSession, sessionId);
        if (guard) { auto status = guard(result.ok(), result.ok() ? nullptr : &result.error()); if (!status.ok()) co_return status.error(); }
        co_return result;
    }
private:
    StorageService& service;
};
} // namespace roclint::host
