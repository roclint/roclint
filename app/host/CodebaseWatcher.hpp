#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace roclint::host {

// Pure, dependency-free batching policy used by CodebaseWatcher. It coalesces
// duplicate path notifications, bounds the pending set and escalates overflow
// or structural changes to a single full-refresh request.
class CodebaseWatchBatch {
public:
    explicit CodebaseWatchBatch(std::size_t maximumPaths = 4096)
        : maximumPaths_(maximumPaths == 0 ? 1 : maximumPaths) {}

    // Returns true when the path was newly queued. When the pending set would
    // exceed its bound the batch escalates to a full refresh instead.
    bool addPath(std::string relativePath) {
        if (relativePath.empty() || fullRefresh_) return false;
        if (seen_.find(relativePath) != seen_.end()) return false;
        if (paths_.size() >= maximumPaths_) {
            overflow_ = true;
            requestFullRefresh("path-overflow");
            return false;
        }
        seen_.insert(relativePath);
        paths_.push_back(std::move(relativePath));
        return true;
    }

    void requestFullRefresh(std::string reason) {
        fullRefresh_ = true;
        if (!reason.empty()) fullRefreshReason_ = std::move(reason);
        paths_.clear();
        seen_.clear();
    }

    [[nodiscard]] bool fullRefreshRequested() const noexcept { return fullRefresh_; }
    [[nodiscard]] const std::string& fullRefreshReason() const noexcept { return fullRefreshReason_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }
    [[nodiscard]] std::size_t size() const noexcept { return paths_.size(); }
    [[nodiscard]] bool empty() const noexcept { return paths_.empty() && !fullRefresh_; }

    std::vector<std::string> takePaths() {
        auto result = std::move(paths_);
        paths_.clear();
        seen_.clear();
        fullRefresh_ = false;
        fullRefreshReason_.clear();
        overflow_ = false;
        return result;
    }

    void clear() {
        paths_.clear();
        seen_.clear();
        fullRefresh_ = false;
        fullRefreshReason_.clear();
        overflow_ = false;
    }

private:
    std::size_t maximumPaths_;
    std::vector<std::string> paths_;
    std::unordered_set<std::string> seen_;
    bool fullRefresh_{false};
    bool overflow_{false};
    std::string fullRefreshReason_{};
};

} // namespace roclint::host

// The Qt watcher is excluded when a storage-only test wants to exercise the
// batching policy without Qt headers or an event loop.
#if !defined(ROCLINT_CODEBASE_WATCHER_NO_QT)

#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace roclint::host {

// Recursive filesystem watcher for a consented workspace. Notifications are
// hints only: they are debounced and batched, and the host must revalidate the
// named paths through CodebaseIndex before treating anything as evidence.
class CodebaseWatcher : public QObject {
    Q_OBJECT
public:
    struct Limits {
        std::size_t maximumWatchedDirectories{2048};
        std::size_t maximumWatchedFiles{8192};
        std::size_t maximumPendingPaths{4096};
        int debounceMs{250};
        // Periodic full-refresh reconciliation. Armed on a healthy start as
        // well as on failure/overflow so missed creation/deletion events are
        // eventually noticed without a per-keystroke rescan.
        int fallbackRefreshIntervalMs{5 * 60 * 1000};

        Limits()
            : maximumWatchedDirectories(2048),
              maximumWatchedFiles(8192),
              maximumPendingPaths(4096),
              debounceMs(250),
              fallbackRefreshIntervalMs(5 * 60 * 1000) {}
    };

    explicit CodebaseWatcher(QObject* parent = nullptr);
    ~CodebaseWatcher() override;

    // Starts watching `workspaceRoot`. Returns false when the root itself
    // cannot be registered; partial subdirectory/file watching still returns
    // true and reports through signals.
    bool start(const QString& workspaceRoot, const Limits& limits = {});
    void stop();
    [[nodiscard]] bool isWatching() const noexcept { return running_; }
    [[nodiscard]] bool isHealthy() const noexcept { return running_ && !failed_ && !overflowed_; }
    [[nodiscard]] QString workspaceRoot() const { return root_; }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

    // Exclusions and extensions mirror CodebaseIndex's scan policy.
    [[nodiscard]] static bool isExcludedDirectoryName(const QString& name);
    [[nodiscard]] static bool isSupportedSourcePath(const QString& path);

#ifdef ROCLINT_ENABLE_TEST_MOCKS
    // Test seams for permanent registration failure. The flags are process-wide
    // so every watcher instance created during a recovery cycle observes them.
    static void setForceAddPathFailureForTesting(bool fail);
    static void setForceStartFailureForTesting(bool fail);
#endif

signals:
    // Coalesced workspace-relative paths that need refreshPaths validation.
    void pathsChanged(const QStringList& relativePaths);
    // A safe full refresh is required (directory structure, watch overflow or
    // watcher failure).
    void fullRefreshRequired(const QString& reason);
    void watchOverflowed(const QString& reason);
    void watchFailed(const QString& reason);

public slots:
    // Forces the pending batch out. Normally invoked by the debounce timer.
    void flushPending();

private slots:
    void onDirectoryChanged(const QString& path);
    void onFileChanged(const QString& path);

private:
    struct FileStamp {
        qint64 modifiedMs{0};
        qint64 size{-1};
        bool operator==(const FileStamp&) const = default;
    };

    bool addDirectory(const QString& absolutePath, bool emitFiles);
    bool addWatchPath(const QString& absolutePath);
    void removeDirectory(const QString& absolutePath);
    void rescanDirectory(const QString& absolutePath);
    void queueFile(const QString& absolutePath);
    void requestFullRefresh(const QString& reason);
    void noteFailure(const QString& reason);
    void noteOverflow(const QString& reason);
    void armFallback();
    [[nodiscard]] QString relativePathFor(const QString& absolutePath) const;
    [[nodiscard]] static bool isSameOrWithinRoot(const QString& root, const QString& candidate);
    [[nodiscard]] static FileStamp stampFor(const QFileInfo& info);

    QFileSystemWatcher watcher_;
    QTimer debounceTimer_;
    QTimer fallbackTimer_;
    QHash<QString, QHash<QString, FileStamp>> directoryFiles_;
    QHash<QString, QStringList> directorySubdirectories_;
    QSet<QString> watchedDirectories_;
    QSet<QString> watchedFiles_;
    CodebaseWatchBatch batch_;
    QString root_;
    Limits limits_{};
    bool running_{false};
    bool failed_{false};
    bool overflowed_{false};
};

} // namespace roclint::host

#endif // ROCLINT_CODEBASE_WATCHER_NO_QT
