#pragma once
#include "AsyncTask.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QThread>
#include <QLockFile>
#include <QFileInfo>
#include <atomic>
#include <tuple>
#include <mutex>

namespace roclint::host {
class StorageCancelled : public std::exception {};
struct StoredHistory {
    struct Session { storage::SessionRecord record; std::vector<storage::RevisionRecord> revisions; };
    std::vector<Session> sessions;
    int schema{0};
};
// One connection, created, used, and closed exclusively on this service's thread.
class StorageService {
    struct Worker : QObject {
        std::unique_ptr<storage::Database> database;
        std::unique_ptr<storage::PromptRepository> repository;
        std::unique_ptr<QLockFile> ownership;
    };
    struct State {
        std::shared_ptr<std::atomic_bool> stopping = std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
        int pending{0}; // GUI thread only
        bool closed{false};
        std::mutex handleMutex;
        sqlite3* handle{nullptr};
    };
public:
    explicit StorageService(QObject* receiver) : receiver_(receiver), worker_(new Worker), thread_(new QThread), state_(std::make_shared<State>()) {
        worker_->moveToThread(thread_);
        QObject::connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
        QObject::connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
        thread_->start();
    }
    ~StorageService() { close(); }
    StorageService(const StorageService&) = delete;
    int pending() const { return state_->pending; }
    bool closed() const { return state_->closed; }
    void cancelOutstanding() {
        state_->cancelled->store(true);
        const std::lock_guard lock(state_->handleMutex); if (state_->handle) sqlite3_interrupt(state_->handle);
    }
    void close(std::function<void()> completed = {}) {
        if (state_->stopping->exchange(true)) return;
        cancelOutstanding();
        const auto state = state_; const QPointer<QObject> receiver = receiver_;
        auto* thread = thread_;
        QMetaObject::invokeMethod(worker_, [worker = worker_, thread, state, receiver, completed]() {
            { const std::lock_guard lock(state->handleMutex); state->handle = nullptr; }
            worker->repository.reset(); worker->database.reset(); worker->ownership.reset();
            QMetaObject::invokeMethod(QCoreApplication::instance(), [state, receiver, completed]() {
                state->closed = true;
                if (receiver && completed) completed();
            }, Qt::QueuedConnection);
            thread->quit();
        }, Qt::QueuedConnection);
    }
    template<class R> struct Awaitable {
        StorageService* service;
        std::function<R(Worker&)> work;
        std::optional<R> result;
        QPointer<QObject> receiver;
        bool await_ready() const { return false; }
        void await_suspend(std::coroutine_handle<> continuation) {
            const auto state = service->state_;
            if (state->pending >= 128 || state->stopping->load()) {
                result.emplace(core::makeError(core::ErrorCode::ProviderUnavailable, "History queue is unavailable"));
                QMetaObject::invokeMethod(QCoreApplication::instance(), [continuation]() { continuation.resume(); }, Qt::QueuedConnection);
                return;
            }
            ++state->pending;
            QMetaObject::invokeMethod(service->worker_, [this, state, continuation, worker = service->worker_]() {
                R value = core::makeError(core::ErrorCode::DbError, "History storage is unavailable");
                try {
                    if (!state->stopping->load()) value = work(*worker);
                } catch (...) { value = core::makeError(core::ErrorCode::DbError, "History operation failed"); }
                QMetaObject::invokeMethod(QCoreApplication::instance(), [this, state, continuation, value = std::move(value)]() mutable {
                    --state->pending; result.emplace(std::move(value)); continuation.resume();
                }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
        }
        R await_resume() {
            if (!receiver) throw StorageCancelled{};
            return std::move(*result);
        }
    };
    template<class R, class F> AsyncTask<R> perform(F work) {
        co_return co_await Awaitable<R>{this, std::move(work), {}, receiver_};
    }
    AsyncTask<core::Status> open(QString path, bool recoverAttempts) {
        const auto stopping = state_->cancelled;
        const auto state = state_;
        co_return co_await perform<core::Status>([path, recoverAttempts, stopping, state](Worker& worker) {
            { const std::lock_guard lock(state->handleMutex); state->handle = nullptr; }
            worker.repository.reset(); worker.database.reset(); worker.ownership.reset();
            if (path != QLatin1String(":memory:")) {
                const QFileInfo info(path);
                auto identity = info.canonicalFilePath();
                if (identity.isEmpty()) {
                    const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
                    if (parent.isEmpty()) return core::Status(core::makeError(core::ErrorCode::DbError, "History folder is unavailable"));
                    identity = parent + QLatin1Char('/') + info.fileName();
                }
                worker.ownership = std::make_unique<QLockFile>(identity + QStringLiteral(".owner.lock"));
                worker.ownership->setStaleLockTime(0);
                if (!worker.ownership->tryLock(1000)) {
                    if (!worker.ownership->removeStaleLockFile() || !worker.ownership->tryLock(1000))
                        return core::Status(core::makeError(core::ErrorCode::DbError, "Exclusive history ownership could not be established"));
                }
            }
            worker.database = std::make_unique<storage::Database>();
            worker.database->setCancellationFlag(stopping);
            const auto target = path == QLatin1String(":memory:") ? std::filesystem::path(":memory:") : std::filesystem::path(path.toStdWString());
            if (auto status = worker.database->open(target); !status.ok()) return status;
            { const std::lock_guard lock(state->handleMutex); state->handle = worker.database->rawHandle(); }
            worker.repository = std::make_unique<storage::PromptRepository>(*worker.database);
            if (auto status = worker.repository->migrate(); !status.ok()) { worker.repository.reset(); return status; }
            if (recoverAttempts) {
                auto interrupted = worker.repository->interruptOrphanedAttemptsOnStartup(QDateTime::currentMSecsSinceEpoch());
                if (!interrupted.ok()) { worker.repository.reset(); return core::Status(interrupted.error()); }
            }
            return core::Status::success();
        });
    }
    template<class Method, class... Args> auto call(Method method, Args... args) -> AsyncTask<std::invoke_result_t<Method, storage::PromptRepository&, Args...>> {
        using R = std::invoke_result_t<Method, storage::PromptRepository&, Args...>;
        co_return co_await perform<R>([method, arguments = std::make_tuple(std::move(args)...)](Worker& worker) mutable -> R {
            if (!worker.repository) return core::makeError(core::ErrorCode::DbError, "History storage is unavailable");
            return std::apply([&](auto&... values) { return std::invoke(method, *worker.repository, values...); }, arguments);
        });
    }
    AsyncTask<core::Result<StoredHistory>> history() {
        co_return co_await perform<core::Result<StoredHistory>>([](Worker& worker) -> core::Result<StoredHistory> {
            if (!worker.repository) return core::makeError(core::ErrorCode::DbError, "History storage is unavailable");
            auto sessions = worker.repository->listSessions(); if (!sessions.ok()) return sessions.error();
            StoredHistory result;
            auto schema = worker.repository->getSchemaVersion(); if (!schema.ok()) return schema.error(); result.schema = schema.value();
            for (const auto& session : sessions.value()) {
                auto revisions = worker.repository->getHistory(session.id); if (!revisions.ok()) return revisions.error();
                result.sessions.push_back({session, std::move(revisions).value()});
            }
            return result;
        });
    }
private:
    QPointer<QObject> receiver_;
    Worker* worker_;
    QThread* thread_;
    std::shared_ptr<State> state_;
};
} // namespace roclint::host
