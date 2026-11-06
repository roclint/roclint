#include "HostController.hpp"
#include "CodebaseWatcher.hpp"
#include "WorkspaceInspection.hpp"
#include "roclint/rewrite/AstGroundingEngine.hpp"
#include "roclint/rewrite/CanonicalIntent.hpp"
#include <QJsonArray>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <utility>

namespace roclint::host {
namespace {
struct CodebaseRefreshOutcome {
    bool ok{false};
    bool incomplete{false};
    bool fullScan{true};
    storage::CodebaseSnapshotInfo snapshot{};
    QStringList unprocessedPaths{};
    QString error{};
};

constexpr int kCodebasePathsCoalesceMs = 200;
constexpr int kCodebaseMaximumPendingPaths = 4096;
constexpr int kCodebaseMaximumPathsRetries = 2;
constexpr int kCodebaseMaximumStaleRecoveryAttempts = 3;
constexpr int kCodebaseMaximumWatcherRecoveryAttempts = 3;
constexpr int kCodebaseWatcherRecoveryDelaysMs[kCodebaseMaximumWatcherRecoveryAttempts] = {200, 600, 1200};

QString normalizedPendingPath(const QString& path) {
    const auto normalized = QDir::fromNativeSeparators(QDir::cleanPath(path));
    if (normalized.isEmpty() || normalized == QLatin1String(".") || QDir::isAbsolutePath(normalized) ||
        normalized == QLatin1String("..") || normalized.startsWith(QStringLiteral("../")))
        return {};
    return normalized;
}
} // namespace

#ifdef ROCLINT_ENABLE_TEST_MOCKS
namespace {
std::atomic<int> g_codebaseRevalidationDelayMs{0};
} // namespace

// Test-only delay applied inside the asynchronous proposal revalidation worker
// so tests can order a superseded result against a newer proposal.
void setCodebaseRevalidationDelayForTesting(int milliseconds) {
    g_codebaseRevalidationDelayMs.store(std::max(0, milliseconds));
}

int codebaseRevalidationDelayForTesting() {
    return g_codebaseRevalidationDelayMs.load();
}
#endif

QVariantList HostController::recentProjects() const {
    QVariantList result;
    if (QCoreApplication::organizationName().isEmpty()) return result;
    const auto paths = QSettings().value(QStringLiteral("workspace/recent")).toStringList();
    for (const auto& path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) result.append(QVariantMap{{QStringLiteral("name"), info.fileName()}, {QStringLiteral("path"), info.canonicalFilePath()}});
    }
    return result;
}

QString HostController::openFolderDialog() {
    const auto directory = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Select Project Folder"));
    if (directory.isEmpty()) return {};
    const auto canonical = QFileInfo(directory).canonicalFilePath();
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        auto paths = settings.value(QStringLiteral("workspace/recent")).toStringList();
        paths.removeAll(canonical);
        paths.prepend(canonical);
        while (paths.size() > 12) paths.removeLast();
        settings.setValue(QStringLiteral("workspace/recent"), paths);
        auto consentedRoots = settings.value(QStringLiteral("workspace/codebaseContextConsentedRoots")).toStringList();
        consentedRoots.removeAll(canonical);
        consentedRoots.prepend(canonical);
        settings.setValue(QStringLiteral("workspace/codebaseContextConsentedRoots"), consentedRoots);
    }
    m_codebaseContextConsented = true;
    return canonical;
}

QStringList HostController::getProjectFiles(const QString& projectPath) {
    const auto canonical = QDir::cleanPath(projectPath);
    if (projectPath.isEmpty() || !QDir::isAbsolutePath(canonical)) return {};
    if (canonical == m_projectScanPath) {
        // Re-selecting the same folder is an explicit refresh request. Keep the
        // cached file list, but schedule a hash-incremental full pass.
        if (m_codebaseContextConsented) {
            m_codebaseIncompleteRetries = 0;
            m_codebasePathsRetries = 0;
            resetCodebaseWatcherRecovery();
            startCodebaseWatcher(canonical);
            startCodebaseRefresh(canonical);
        }
        return m_projectFiles;
    }
    // A different canonical workspace starts a new epoch immediately. Every
    // ticket, proposal and revalidation captured under the previous epoch is
    // now stale, including work already running on a worker thread.
    ++m_workspaceEpoch;
    // Synchronously drop all derived state (analysis, intent, proposal,
    // acknowledgments, in-memory approval and active grounding). Persisted
    // approvals are invalidated through the repository contract; the in-memory
    // approval is already gone, so approval cannot be used while that write is
    // pending.
    invalidateDownstreamPipeline();
    resetStalePathRecovery();
    // Invalidate every in-flight codebase callback before the old watcher or
    // index can publish into the new workspace.
    stopCodebaseWatcher();
    resetCodebaseWatcherRecovery();
    ++m_codebaseRefreshSerial;
    ++m_codebasePathsCoalesceSerial;
    m_codebaseRefreshInFlight = false;
    m_codebaseFullRefreshQueued = false;
    m_pendingCodebasePaths.clear();
    m_projectScanPath = canonical;
    m_projectFiles.clear();
    m_codebaseActiveFile.clear();
    m_codebaseIndexReady = false;
    m_codebaseIncompleteRetries = 0;
    m_codebasePathsRetries = 0;
    m_codebaseSnapshotGeneration = 0;
    setGroundingUnavailable(QStringLiteral("UNAVAILABLE"));
    m_workspaceRootIdentity.clear();
    m_workspaceScanIncomplete = true;
    if (!QCoreApplication::organizationName().isEmpty()) {
        const auto roots = QSettings().value(QStringLiteral("workspace/codebaseContextConsentedRoots")).toStringList();
        m_codebaseContextConsented = roots.contains(canonical);
    }
    if (m_codebaseContextConsented) {
        // Watch immediately so changes during the initial scan are queued for
        // the incremental pass that follows it.
        startCodebaseWatcher(canonical);
        startCodebaseRefresh(canonical);
    }
    const auto serial = ++m_projectScanSerial;
    if (m_cancelWorkspaceScan) m_cancelWorkspaceScan();
    ++m_inFlight;
    m_cancelWorkspaceScan = inspectWorkspace(this, {{"operation", "scan"}, {"root", canonical}}, 2000,
        [this, serial](QJsonObject result) {
            --m_inFlight;
            if (serial != m_projectScanSerial) return;
            m_workspaceRootIdentity = result.value("root_identity").toString();
            m_workspaceScanIncomplete = result.value("context_incomplete").toBool(true);
            m_projectFiles.clear();
            for (const auto& value : result.value("files").toArray()) m_projectFiles.append(value.toString());
            emit projectFilesChanged();
            finishQuitWhenRuntimesStopped();
        });
    return {};
}

bool HostController::refreshCodebaseIndex() {
    if (m_projectScanPath.isEmpty() || !m_codebaseContextConsented) {
        setLastError(QStringLiteral("Select a workspace before refreshing the code index."));
        return false;
    }
    m_codebaseIncompleteRetries = 0;
    m_codebasePathsRetries = 0;
    // An explicit refresh is the deliberate recovery path: reset the bounded
    // automatic recovery state and restart the watcher exactly once.
    resetCodebaseWatcherRecovery();
    startCodebaseWatcher(m_projectScanPath);
    startCodebaseRefresh(m_projectScanPath);
    return true;
}

void HostController::startCodebaseWatcher(const QString& projectPath) {
    const bool deliberate = m_codebaseWatcherDeliberateRecovery;
    m_codebaseWatcherDeliberateRecovery = false;
    if (m_hostState == HostState::Quitting || !m_codebaseContextConsented || projectPath.isEmpty()) return;

    // Retain a running partial watcher (and its fallback timer). Only a
    // deliberate explicit restart or a stopped watcher may replace it.
    if (m_codebaseWatcher && m_codebaseWatcher->isWatching() && !deliberate) return;

    stopCodebaseWatcher();
    const auto workspace = QFileInfo(projectPath).canonicalFilePath();
    if (workspace.isEmpty()) return;

    const auto serial = ++m_codebaseWatcherSerial;
    m_codebaseWatcher = std::make_unique<CodebaseWatcher>(this);
    auto* watcher = m_codebaseWatcher.get();

    connect(watcher, &CodebaseWatcher::pathsChanged, this,
        [this, serial](const QStringList& paths) {
            if (serial != m_codebaseWatcherSerial) return;
            scheduleCodebasePathsRefresh(paths);
        });
    connect(watcher, &CodebaseWatcher::fullRefreshRequired, this,
        [this, serial](const QString& reason) {
            if (serial != m_codebaseWatcherSerial) return;
            m_lastInternalDiagnostic = QStringLiteral("Codebase watcher full refresh: %1").arg(reason).left(200);
            startCodebaseRefresh(m_projectScanPath);
        });
    connect(watcher, &CodebaseWatcher::watchOverflowed, this,
        [this, serial](const QString& reason) {
            if (serial != m_codebaseWatcherSerial) return;
            m_lastInternalDiagnostic = QStringLiteral("Codebase watcher overflow: %1").arg(reason).left(200);
        });
    connect(watcher, &CodebaseWatcher::watchFailed, this,
        [this, serial](const QString& reason) {
            if (serial != m_codebaseWatcherSerial) return;
            m_lastInternalDiagnostic = QStringLiteral("Codebase watcher failed: %1").arg(reason).left(200);
        });
    const auto limits = m_codebaseWatcherLimitsForTesting.value_or(CodebaseWatcher::Limits{});
    static_cast<void>(watcher->start(workspace, limits));
}

void HostController::stopCodebaseWatcher() {
    ++m_codebaseWatcherSerial;
    // A pending automatic recovery belongs to the watcher/workspace being
    // stopped and must never restart it afterwards.
    ++m_codebaseWatcherRecoverySerial;
    m_codebaseWatcherDeliberateRecovery = false;
    if (m_codebaseWatcher) {
        m_codebaseWatcher->stop();
        m_codebaseWatcher.reset();
    }
}

void HostController::resetCodebaseWatcherRecovery() {
    ++m_codebaseWatcherRecoverySerial;
    m_codebaseWatcherRecoveryAttempts = 0;
    m_codebaseWatcherDegraded = false;
    m_codebaseWatcherDeliberateRecovery = true;
}

void HostController::maybeRecoverCodebaseWatcher() {
    if (m_hostState == HostState::Quitting || !m_codebaseContextConsented || m_projectScanPath.isEmpty()) return;
    // A running watcher is retained even when it is unhealthy: its fallback
    // timer provides the refresh cadence without repeating startup traversal.
    if (m_codebaseWatcher && m_codebaseWatcher->isWatching()) return;
    if (m_codebaseWatcherDegraded) return;
    if (m_codebaseWatcherRecoveryAttempts >= kCodebaseMaximumWatcherRecoveryAttempts) {
        m_codebaseWatcherDegraded = true;
        m_lastInternalDiagnostic = QStringLiteral("Codebase watcher degraded after %1 recovery attempts; explicit refresh required.")
                                       .arg(kCodebaseMaximumWatcherRecoveryAttempts).left(200);
        return;
    }
    ++m_codebaseWatcherRecoveryAttempts;
    scheduleCodebaseWatcherRecovery();
}

void HostController::scheduleCodebaseWatcherRecovery() {
    if (m_codebaseWatcherRecoveryAttempts <= 0 || m_codebaseWatcherRecoveryAttempts > kCodebaseMaximumWatcherRecoveryAttempts)
        return;
    const int delayMs = kCodebaseWatcherRecoveryDelaysMs[m_codebaseWatcherRecoveryAttempts - 1];
    const auto serial = ++m_codebaseWatcherRecoverySerial;
    const auto workspace = m_projectScanPath;
    QTimer::singleShot(delayMs, this, [this, serial, workspace]() {
        if (serial != m_codebaseWatcherRecoverySerial) return;
        if (m_hostState == HostState::Quitting || !m_codebaseContextConsented) return;
        if (workspace != m_projectScanPath) return;
        if (m_codebaseWatcher && m_codebaseWatcher->isWatching()) return;
        if (m_codebaseWatcherDegraded) return;
        startCodebaseWatcher(workspace);
        if (m_hostState == HostState::Quitting || workspace != m_projectScanPath) return;
        // A failed start must chain to the next bounded attempt; do not rely on
        // a later scan completion (the failed watcher may not produce one).
        // A running partial watcher is retained and never restarted here.
        if (m_codebaseWatcher && m_codebaseWatcher->isWatching()) return;
        maybeRecoverCodebaseWatcher();
    });
}

void HostController::startCodebaseRefresh(const QString& projectPath) {
    if (m_hostState == HostState::Quitting || !m_codebaseContextConsented || projectPath.isEmpty()) {
        if (m_hostState != HostState::Quitting) stopCodebaseWatcher();
        return;
    }
    if (m_codebaseRefreshInFlight) {
        m_codebaseFullRefreshQueued = true;
        return;
    }
    const auto indexPathText = codebaseIndexPathForHost();
    if (indexPathText.isEmpty()) return;

    // A full scan supersedes pending paths; every path is visited by the scan.
    ++m_codebasePathsCoalesceSerial;
    m_pendingCodebasePaths.clear();
    m_codebaseFullRefreshQueued = false;
    m_codebasePathsRetries = 0;
    ++m_codebaseRefreshStartsForTesting;

    const auto indexPath = std::filesystem::path(indexPathText.toStdWString());
    const auto rootPath = std::filesystem::path(projectPath.toStdWString());
    const auto limits = m_codebaseRefreshLimitsForTesting.value_or(storage::CodebaseIndexLimits{});
    const auto serial = ++m_codebaseRefreshSerial;
    const auto workspace = projectPath;
    m_codebaseRefreshInFlight = true;
    // A full pass does not expose stale evidence while it runs. Incremental
    // path refreshes below deliberately keep the previous snapshot visible.
    m_codebaseIndexReady = false;
    setGroundingUnavailable(QStringLiteral("INCOMPLETE"));
    auto* watcher = new QFutureWatcher<CodebaseRefreshOutcome>(this);
    ++m_inFlight;
    connect(watcher, &QFutureWatcher<CodebaseRefreshOutcome>::finished, this,
        [this, watcher, serial, workspace]() {
            --m_inFlight;
            if (serial == m_codebaseRefreshSerial && workspace == m_projectScanPath) {
                m_codebaseRefreshInFlight = false;
                const auto outcome = watcher->result();
                if (outcome.ok) {
                    if (outcome.snapshot.available)
                        m_codebaseSnapshotGeneration = outcome.snapshot.generation;
                    // Readiness is strictly the snapshot's own completeness.
                    // An earlier ready flag never authorizes a partial pass.
                    const bool complete = outcome.snapshot.available && outcome.snapshot.complete &&
                                          !outcome.incomplete;
                    m_codebaseIndexReady = complete;
                    if (!complete) {
                        setGroundingUnavailable(QStringLiteral("INCOMPLETE"));
                        scheduleGroundingRevalidation();
                        // A bounded first pass can stop on the 2 s scan budget.
                        // The next pass hashes unchanged files instead of
                        // re-parsing them, so a small, bounded retry set makes
                        // the snapshot complete without hiding the work.
                        if (m_codebaseIncompleteRetries < 2) {
                            ++m_codebaseIncompleteRetries;
                            m_codebaseFullRefreshQueued = true;
                        } else {
                            setLastError(QStringLiteral("Codebase index refresh incomplete; grounding withheld."));
                        }
                    } else {
                        m_codebaseIncompleteRetries = 0;
                        m_codebasePathsRetries = 0;
                        scheduleGroundingRevalidation();
                    }
                } else {
                    m_codebaseIndexReady = false;
                    m_codebaseIncompleteRetries = 0;
                    m_pendingCodebasePaths.clear();
                    setGroundingUnavailable(QStringLiteral("UNAVAILABLE"));
                    scheduleGroundingRevalidation();
                    setLastError(QStringLiteral("Codebase index refresh failed: %1")
                                     .arg(outcome.error.isEmpty() ? QStringLiteral("unknown error") : outcome.error));
                    maybeRecoverCodebaseWatcher();
                }
                if (m_codebaseFullRefreshQueued) {
                    m_codebaseFullRefreshQueued = false;
                    startCodebaseRefresh(m_projectScanPath);
                } else if (outcome.ok && m_codebaseContextConsented && workspace == m_projectScanPath) {
                    maybeRecoverCodebaseWatcher();
                    if (!m_pendingCodebasePaths.isEmpty()) {
                        auto paths = std::move(m_pendingCodebasePaths);
                        m_pendingCodebasePaths.clear();
                        startCodebasePathsRefresh(workspace, paths);
                    }
                }
            }
            watcher->deleteLater();
            finishQuitWhenRuntimesStopped();
        });
    watcher->setFuture(QtConcurrent::run([indexPath, rootPath, limits]() -> CodebaseRefreshOutcome {
        CodebaseRefreshOutcome outcome;
        outcome.fullScan = true;
        storage::CodebaseIndex index(indexPath);
        const storage::WorkspaceContext workspace{rootPath, {}, true};
        const auto refreshed = index.refresh(workspace, limits);
        if (!refreshed.ok()) {
            outcome.error = QString::fromStdString(refreshed.error().message);
            return outcome;
        }
        outcome.ok = true;
        outcome.incomplete = refreshed.value().incomplete;
        const auto info = index.snapshotInfo();
        if (info.ok()) outcome.snapshot = info.value();
        return outcome;
    }));
}

void HostController::scheduleCodebasePathsRefresh(const QStringList& relativePaths) {
    if (m_hostState == HostState::Quitting || !m_codebaseContextConsented || m_projectScanPath.isEmpty() ||
        relativePaths.isEmpty())
        return;
    for (const auto& path : relativePaths) {
        const auto normalized = normalizedPendingPath(path);
        if (normalized.isEmpty()) continue;
        if (!m_pendingCodebasePaths.contains(normalized)) m_pendingCodebasePaths.append(normalized);
    }
    if (m_pendingCodebasePaths.size() > kCodebaseMaximumPendingPaths) {
        m_pendingCodebasePaths.clear();
        startCodebaseRefresh(m_projectScanPath);
        return;
    }
    if (m_pendingCodebasePaths.isEmpty()) return;
    const auto serial = ++m_codebasePathsCoalesceSerial;
    const auto workspace = m_projectScanPath;
    QTimer::singleShot(kCodebasePathsCoalesceMs, this, [this, serial, workspace]() {
        if (serial != m_codebasePathsCoalesceSerial || workspace != m_projectScanPath) return;
        if (m_pendingCodebasePaths.isEmpty()) return;
        auto paths = std::move(m_pendingCodebasePaths);
        m_pendingCodebasePaths.clear();
        startCodebasePathsRefresh(workspace, paths);
    });
}

void HostController::scheduleStalePathRecovery(const std::vector<std::string>& relativePaths) {
    if (m_hostState == HostState::Quitting || !m_codebaseContextConsented || m_projectScanPath.isEmpty() ||
        relativePaths.empty())
        return;
    if (codebaseIndexPathForHost().isEmpty()) return;

    // Keep recovery bound to paths the index can safely re-read. Already queued
    // paths (watcher or an earlier stale detection) are about to be refreshed,
    // so they consume no additional attempt.
    QStringList eligible;
    for (const auto& raw : relativePaths) {
        const auto path = normalizedPendingPath(
            QString::fromLocal8Bit(raw.data(), static_cast<qsizetype>(raw.size())));
        if (path.isEmpty()) continue;
        if (m_pendingCodebasePaths.contains(path)) continue;
        const int attempts = m_staleRecoveryAttempts.value(path, 0);
        if (attempts >= kCodebaseMaximumStaleRecoveryAttempts) continue;
        if (!eligible.contains(path)) eligible.append(path);
    }
    if (eligible.isEmpty()) return;

    for (const auto& path : eligible)
        m_staleRecoveryAttempts.insert(path, m_staleRecoveryAttempts.value(path) + 1);
    // Reuse the existing 200 ms coalescing and in-flight queueing; hashing and
    // parsing remain on the CodebaseIndex worker.
    scheduleCodebasePathsRefresh(eligible);
}

void HostController::resetStalePathRecovery() {
    // Watcher-pending paths are intentionally untouched: only the bounded
    // automatic retry state for hash-detected stale evidence is cleared.
    m_staleRecoveryAttempts.clear();
}

void HostController::startCodebasePathsRefresh(const QString& projectPath, const QStringList& relativePaths) {
    if (m_hostState == HostState::Quitting || !m_codebaseContextConsented || projectPath.isEmpty() ||
        relativePaths.isEmpty())
        return;
    if (m_codebaseRefreshInFlight) {
        for (const auto& path : relativePaths)
            if (!m_pendingCodebasePaths.contains(path)) m_pendingCodebasePaths.append(path);
        return;
    }
    const auto indexPathText = codebaseIndexPathForHost();
    if (indexPathText.isEmpty()) return;

    const auto indexPath = std::filesystem::path(indexPathText.toStdWString());
    const auto rootPath = std::filesystem::path(projectPath.toStdWString());
    const auto limits = m_codebaseRefreshLimitsForTesting.value_or(storage::CodebaseIndexLimits{});
    const auto serial = ++m_codebaseRefreshSerial;
    const auto workspace = projectPath;
    m_codebaseRefreshInFlight = true;
    ++m_codebaseRefreshStartsForTesting;
    // Keep the previous index and readiness while the incremental scan runs.
    auto* watcher = new QFutureWatcher<CodebaseRefreshOutcome>(this);
    ++m_inFlight;
    connect(watcher, &QFutureWatcher<CodebaseRefreshOutcome>::finished, this,
        [this, watcher, serial, workspace]() {
            --m_inFlight;
            if (serial == m_codebaseRefreshSerial && workspace == m_projectScanPath) {
                m_codebaseRefreshInFlight = false;
                const auto outcome = watcher->result();
                if (outcome.ok) {
                    if (outcome.snapshot.available)
                        m_codebaseSnapshotGeneration = outcome.snapshot.generation;
                    // The incremental pass is authoritative only when the
                    // snapshot itself is complete. A previously ready flag
                    // must never mask newly dirty or omitted candidates.
                    const bool complete = outcome.snapshot.available && outcome.snapshot.complete &&
                                          !outcome.incomplete;
                    m_codebaseIndexReady = complete;
                    if (!complete) {
                        setGroundingUnavailable(QStringLiteral("INCOMPLETE"));
                        scheduleGroundingRevalidation();
                        if (outcome.incomplete && m_codebasePathsRetries < kCodebaseMaximumPathsRetries &&
                            !outcome.unprocessedPaths.isEmpty()) {
                            ++m_codebasePathsRetries;
                            // Retry only the paths this pass explicitly left
                            // unprocessed. Queued watcher paths are retained.
                            for (const auto& path : outcome.unprocessedPaths) {
                                const auto normalized = normalizedPendingPath(path);
                                if (normalized.isEmpty()) continue;
                                if (!m_pendingCodebasePaths.contains(normalized))
                                    m_pendingCodebasePaths.append(normalized);
                            }
                            if (m_pendingCodebasePaths.size() > kCodebaseMaximumPendingPaths) {
                                m_pendingCodebasePaths.clear();
                                m_codebaseFullRefreshQueued = true;
                            }
                        } else {
                            m_codebasePathsRetries = 0;
                            setLastError(QStringLiteral("Codebase index refresh incomplete; grounding withheld."));
                        }
                    } else {
                        m_codebasePathsRetries = 0;
                        m_codebaseIncompleteRetries = 0;
                        scheduleGroundingRevalidation();
                    }
                } else {
                    // A paths pass that cannot open/read the index is a failed
                    // current validation, not a superseded snapshot. Keep any
                    // previous snapshot visible for ordinary requests, but let
                    // revalidation invalidate the exact bound proposal.
                    setLastError(QStringLiteral("Codebase index refresh failed: %1")
                                     .arg(outcome.error.isEmpty() ? QStringLiteral("unknown error") : outcome.error));
                    scheduleGroundingRevalidation();
                    maybeRecoverCodebaseWatcher();
                }
                if (m_codebaseFullRefreshQueued) {
                    m_codebaseFullRefreshQueued = false;
                    startCodebaseRefresh(m_projectScanPath);
                } else if (outcome.ok && m_codebaseContextConsented && workspace == m_projectScanPath) {
                    maybeRecoverCodebaseWatcher();
                    if (!m_pendingCodebasePaths.isEmpty()) {
                        auto paths = std::move(m_pendingCodebasePaths);
                        m_pendingCodebasePaths.clear();
                        startCodebasePathsRefresh(workspace, paths);
                    }
                }
            }
            watcher->deleteLater();
            finishQuitWhenRuntimesStopped();
        });
    watcher->setFuture(QtConcurrent::run([indexPath, rootPath, limits, relativePaths]() -> CodebaseRefreshOutcome {
        CodebaseRefreshOutcome outcome;
        outcome.fullScan = false;
        std::vector<std::string> paths;
        paths.reserve(static_cast<std::size_t>(relativePaths.size()));
        // std::filesystem::path::generic_string() uses the native narrow
        // encoding on Windows; match it rather than assuming UTF-8.
        for (const auto& path : relativePaths) paths.push_back(path.toLocal8Bit().toStdString());
        storage::CodebaseIndex index(indexPath);
        const storage::WorkspaceContext workspace{rootPath, {}, true};
        const auto refreshed = index.refreshPaths(workspace, paths, limits);
        if (!refreshed.ok()) {
            outcome.error = QString::fromStdString(refreshed.error().message);
            return outcome;
        }
        outcome.ok = true;
        outcome.incomplete = refreshed.value().incomplete;
        outcome.unprocessedPaths.reserve(static_cast<qsizetype>(refreshed.value().unprocessedPaths.size()));
        for (const auto& path : refreshed.value().unprocessedPaths)
            outcome.unprocessedPaths.append(QString::fromLocal8Bit(path.data(), static_cast<qsizetype>(path.size())));
        const auto info = index.snapshotInfo();
        if (info.ok()) outcome.snapshot = info.value();
        return outcome;
    }));
}

QString HostController::scopePreamble(const QString& projectPath, const QString& scope, const QString& file) {
    const auto root = QFileInfo(projectPath).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir() || root.contains(QLatin1Char('\n')) || root.contains(QLatin1Char('\r'))) {
        setLastError(QStringLiteral("Choose an existing workspace before applying scope.")); return {};
    }
    QString nextActiveFile;
    QString preamble;
    if (scope == QLatin1String("global")) {
        preamble = QStringLiteral("Workspace: %1\nScope: entire project.\n\n").arg(root);
    } else {
        if (scope != QLatin1String("local") || file.isEmpty() || file.contains(QLatin1Char('\n')) || file.contains(QLatin1Char('\r'))) {
            setLastError(QStringLiteral("Select a real file before applying single-file scope.")); return {};
        }
        const QFileInfo info(QDir(root).filePath(file));
        const QString relative = QDir(root).relativeFilePath(info.canonicalFilePath());
        if (!info.isFile() || info.isSymLink() || relative == QLatin1String("..") ||
            relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
            setLastError(QStringLiteral("The selected file must be inside the workspace.")); return {};
        }
        nextActiveFile = relative;
        preamble = QStringLiteral("Workspace: %1\nScope: single file %2. Do not modify other files.\n\n").arg(root, relative);
    }
    if (nextActiveFile == m_codebaseActiveFile) return preamble;

    // A deliberate active-file/scope change is a new grounding epoch. Clear the
    // workspace-bound proposal/evidence synchronously instead of silently
    // re-binding the old proposal to a different file.
    m_codebaseActiveFile = nextActiveFile;
    ++m_workspaceEpoch;
    // Deliveries and tickets predating the scope change must not complete into
    // it; the epoch check covers tickets, the serial covers pending delivery.
    ++m_pipelineSerial;
    m_selectingInsertionTarget = false;
    m_pendingNativeRewrite.reset();
    m_currentProposal.reset();
    m_proposalWorkspaceEpoch = 0;
    m_acknowledgedConstraints.clear();
    const bool hadApproval = m_currentApproval.has_value();
    m_currentApproval.reset();
    m_activeGroundedEvidence.clear();
    m_activeGroundingPrompt.clear();
    m_activeGroundingActiveFile.clear();
    m_activeGroundingProposalId.clear();
    m_activeGroundingSessionId.clear();
    m_activeGroundingRevisionId.clear();
    ++m_groundingValidationSerial;
    m_proposalStatus = ProposalUiStatus::None;
    m_proposalMessage.clear();
    m_streamingPreview.clear();
    if (hadApproval && m_repository && m_revisionState) {
        auto task = m_repository->invalidateApprovals(core::SessionId(currentSessionId()));
        const QPointer<HostController> owner(this);
        task.then([task, owner]() mutable {
            if (!owner) return;
            try {
                auto status = task.await_resume();
                if (!status.ok()) owner->failStorage(status.error());
            } catch (...) {}
        });
    }
    resetStalePathRecovery();
    setGroundingUnavailable(QStringLiteral("UNAVAILABLE"));
    emit rewriteProposalChanged();
    emit approvalChanged();
    emit intentChanged();
    return preamble;
}

QString HostController::openModelFolderDialog() {
    if (nativeGraniteSelected()) {
        chooseNativeModelDirectory();
        return m_nativeDirectory;
    }
    const auto directory = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Select local GGUF model folder"), m_modelDirectory);
    if (directory.isEmpty()) return {};
    m_modelDirectory = QFileInfo(directory).canonicalFilePath();
    if (!QCoreApplication::organizationName().isEmpty()) QSettings().setValue(QStringLiteral("models/directory"), m_modelDirectory);
    emit modelDirectoryChanged();
    refreshAvailableModels();
    return m_modelDirectory;
}

void HostController::appendLocalModelFiles() {
    if (m_modelDirectory.isEmpty()) return;
    const auto files = QDir(m_modelDirectory).entryInfoList({QStringLiteral("*.gguf")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const auto& file : files.mid(0, 100)) {
        m_availableModels.append(QVariantMap{{QStringLiteral("name"), file.fileName()}, {QStringLiteral("path"), file.canonicalFilePath()},
            {QStringLiteral("size"), QStringLiteral("%1 GB").arg(static_cast<double>(file.size()) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1)},
            {QStringLiteral("source"), QStringLiteral("GGUF file; import required")}, {QStringLiteral("quantization"), QStringLiteral("not loaded")},
            {QStringLiteral("requiresImport"), true}, {QStringLiteral("isActive"), false}});
    }
}

void HostController::importLocalModel(const QString& path) {
    if (m_isDownloadingModel) { setLastError(QStringLiteral("Wait for the current model operation to finish.")); return; }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!info.isFile() || info.isSymLink() || info.suffix().compare(QLatin1String("gguf"), Qt::CaseInsensitive) != 0 ||
        canonical.contains(QLatin1Char('"')) || canonical.contains(QLatin1Char('\n')) || canonical.contains(QLatin1Char('\r'))) {
        setLastError(QStringLiteral("Choose a valid GGUF model file.")); return;
    }
    // Ollama owns imported model storage. Never write a Modelfile into the selected model folder.
    auto modelfile = std::make_shared<QTemporaryFile>();
    if (!modelfile->open()) { setLastError(QStringLiteral("Couldn't create the temporary model import file.")); return; }
    const auto contents = QStringLiteral("FROM \"%1\"\n").arg(QDir::fromNativeSeparators(canonical)).toUtf8();
    if (modelfile->write(contents) != contents.size() || !modelfile->flush()) {
        setLastError(QStringLiteral("Couldn't write the temporary model import file.")); return;
    }
    const QString modelFilePath = modelfile->fileName();
    modelfile->close();
    const QString tag = QStringLiteral("roclint-local-") + QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    m_isDownloadingModel = true;
    m_downloadProgress = 0;
    m_downloadStatusMessage = QStringLiteral("Importing %1 into the local Ollama runtime.").arg(info.fileName());
    emit downloadProgressChanged();
    m_downloadProcess = std::make_unique<QProcess>();
    auto* process = m_downloadProcess.get();
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        m_downloadStatusMessage = QString::fromUtf8(process->readAllStandardOutput()).right(2000).trimmed(); emit downloadProgressChanged();
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        m_downloadStatusMessage = QString::fromUtf8(process->readAllStandardError()).right(2000).trimmed(); emit downloadProgressChanged();
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_isDownloadingModel = false;
        m_downloadStatusMessage = QStringLiteral("Couldn't run the local Ollama model importer."); emit downloadProgressChanged();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, modelfile, tag](int code, QProcess::ExitStatus status) {
            Q_UNUSED(modelfile);
            m_isDownloadingModel = false;
            if (code == 0 && status == QProcess::NormalExit) {
                m_downloadProgress = 100;
                m_downloadStatusMessage = QStringLiteral("Imported %1. Checking the selected model.").arg(tag);
                if (m_activeModel == tag) refreshAvailableModels();
                else setActiveModel(tag);
            } else m_downloadStatusMessage = QStringLiteral("Model import failed or was cancelled.");
            emit downloadProgressChanged();
        });
    QTimer::singleShot(600000, process, [process]() { if (process->state() != QProcess::NotRunning) process->kill(); });
    process->start(QStringLiteral("ollama"), {QStringLiteral("create"), tag, QStringLiteral("-f"), modelFilePath});
}

namespace {

const char* groundingStatusName(core::rewrite::GroundingStatus status) noexcept {
    switch (status) {
        case core::rewrite::GroundingStatus::Ungrounded: return "UNGROUNDED";
        case core::rewrite::GroundingStatus::Verified: return "VERIFIED";
        case core::rewrite::GroundingStatus::Ambiguous: return "AMBIGUOUS";
        case core::rewrite::GroundingStatus::NotFoundInScope: return "NOT_FOUND_IN_SCOPE";
        case core::rewrite::GroundingStatus::TypeMismatch: return "TYPE_MISMATCH";
    }
    return "UNGROUNDED";
}

GroundedEvidenceView evidenceViewFor(const core::rewrite::CanonicalIntent& intent,
                                     const core::rewrite::AstGroundingEvidence& evidence) {
    GroundedEvidenceView view;
    view.target = intent.target;
    view.qualifiedTarget = evidence.qualifiedTarget.empty() ? intent.target : evidence.qualifiedTarget;
    view.signature = evidence.signature;
    view.filePath = evidence.filePath;
    view.fileHash = evidence.fileHash;
    view.line = evidence.line;
    view.status = groundingStatusName(evidence.status);
    view.matchReason = evidence.matchReason;
    view.complete = evidence.complete;
    view.hashValidated = evidence.hashValidated;
    view.candidates = evidence.candidates;
    if (view.candidates.size() > core::rewrite::AstGroundingEngine::kMaximumCandidates)
        view.candidates.resize(core::rewrite::AstGroundingEngine::kMaximumCandidates);
    return view;
}

} // namespace

GroundedPromptContext computeGroundedPromptContext(const GroundingInputs& inputs) {
    GroundedPromptContext context;
    context.prompt = inputs.prompt;
    context.activeFile = inputs.activeFile;
    context.workspaceEpoch = inputs.workspaceEpoch;
    if (!inputs.consented || inputs.indexPath.empty() || inputs.workspaceRoot.empty())
        return context;

    // A worker-owned connection keeps SQLite and file hashing off the UI thread.
    // Even when the host withholds readiness, report the database generation so
    // revalidation can distinguish a current incomplete snapshot from a
    // superseded one.
    storage::CodebaseIndex index(inputs.indexPath, inputs.workspaceRoot);
    const auto generationInfo = index.snapshotInfo();
    if (generationInfo.ok() && generationInfo.value().available)
        context.snapshotGeneration = generationInfo.value().generation;
    if (!inputs.ready) return context;
    context.available = true;

    auto raw = core::rewrite::CanonicalIntentExtractor::extract(inputs.prompt);
    if (raw.empty()) {
        if (!generationInfo.ok() || !generationInfo.value().available) {
            context.available = false;
            return context;
        }
        context.validated = true;
        context.complete = generationInfo.value().complete;
        return context;
    }

    std::vector<std::string> stalePaths;
    const auto recordStalePath = [&stalePaths](const std::string& path) {
        if (path.empty()) return;
        if (std::find(stalePaths.begin(), stalePaths.end(), path) == stalePaths.end())
            stalePaths.push_back(path);
    };

    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto before = index.snapshotInfo();
        if (!before.ok() || !before.value().available) {
            context.available = false;
            // A hash failure from an earlier attempt is already a known reason
            // the current result is withheld; keep it for the owning host.
            context.stalePaths = stalePaths;
            return context;
        }
        auto grounded = core::rewrite::AstGroundingEngine::groundIntents(
            raw, inputs.activeFile, index);
        // The rendered subset is the exact evidence that entered the prompt and
        // must therefore be the only evidence that is hashed and displayed.
        auto rendered = core::rewrite::AstGroundingEngine::renderPromptContext(grounded);
        bool complete = before.value().complete;
        bool hashesValid = true;
        std::vector<std::string> attemptStalePaths;
        for (auto& intent : rendered.rendered) {
            if (!intent.grounding.has_value()) continue;
            auto& evidence = *intent.grounding;
            if (!evidence.complete) complete = false;
            // Validate every bound file that contributed to the rendered
            // evidence, including ambiguity candidates, not only Verified rows.
            if (evidence.filePath.empty() || evidence.fileHash.empty()) continue;
            const auto valid = index.validateFileHash(evidence.filePath, evidence.fileHash);
            if (!valid.ok() || !valid.value()) {
                evidence.hashValidated = false;
                hashesValid = false;
                // Only the rendered evidence files may request recovery; the
                // path is already workspace-relative and bounded by validation.
                if (std::find(attemptStalePaths.begin(), attemptStalePaths.end(),
                              evidence.filePath) == attemptStalePaths.end())
                    attemptStalePaths.push_back(evidence.filePath);
            }
        }
        const auto after = index.snapshotInfo();
        const bool generationStable = after.ok() && after.value().available &&
            after.value().generation == before.value().generation;
        if (generationStable && hashesValid) {
            context.snapshotGeneration = before.value().generation;
            context.complete = complete;
            context.validated = true;
            context.xml = rendered.xml;
            context.astSnapshotHash = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
                rendered.rendered, inputs.workspaceIdentity);
            context.evidence.reserve(rendered.rendered.size());
            for (const auto& intent : rendered.rendered) {
                if (intent.grounding.has_value())
                    context.evidence.push_back(evidenceViewFor(intent, *intent.grounding));
            }
            return context;
        }
        for (const auto& path : attemptStalePaths) recordStalePath(path);
        if (attempt == 1) {
            context.snapshotGeneration = after.ok() ? after.value().generation : before.value().generation;
            context.complete = complete;
            context.validated = false;
            context.stalePaths = stalePaths;
            context.evidence.reserve(rendered.rendered.size());
            for (const auto& intent : rendered.rendered) {
                if (intent.grounding.has_value())
                    context.evidence.push_back(evidenceViewFor(intent, *intent.grounding));
            }
            return context;
        }
        // The snapshot moved while we enriched (or a selected file changed).
        // Re-resolve once against the newer coherent snapshot rather than
        // promoting mixed-generation evidence.
    }
    context.stalePaths = stalePaths;
    return context;
}

GroundedPromptContext HostController::groundedCanonicalIntentContext(std::string_view prompt) {
    return computeGroundedPromptContext(groundingInputs(std::string(prompt)));
}

GroundingInputs HostController::groundingInputs(std::string prompt) const {
    GroundingInputs inputs;
    inputs.prompt = std::move(prompt);
    inputs.consented = m_codebaseContextConsented;
    inputs.ready = m_codebaseIndexReady;
    inputs.workspaceRoot = std::filesystem::path(m_projectScanPath.toStdWString());
    inputs.activeFile = m_codebaseActiveFile.toLocal8Bit().toStdString();
    inputs.workspaceIdentity = m_workspaceRootIdentity.isEmpty()
        ? m_projectScanPath.toStdString() : m_workspaceRootIdentity.toStdString();
    inputs.workspaceEpoch = m_workspaceEpoch;
    const auto indexPath = codebaseIndexPathForHost();
    if (!indexPath.isEmpty()) inputs.indexPath = std::filesystem::path(indexPath.toStdWString());
    return inputs;
}

QString HostController::codebaseIndexPathForHost() const {
    if (!m_codebaseIndexPathOverride.isEmpty()) return m_codebaseIndexPathOverride;
    const auto dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dataDirectory.isEmpty() || !QDir().mkpath(dataDirectory)) return {};
    return dataDirectory + QStringLiteral("/codebase-index.db");
}

#ifdef ROCLINT_ENABLE_TEST_MOCKS
void HostController::setCodebaseIndexPathForTesting(QString path) {
    ++m_codebaseRefreshSerial;
    ++m_codebasePathsCoalesceSerial;
    stopCodebaseWatcher();
    m_codebaseRefreshInFlight = false;
    m_codebaseFullRefreshQueued = false;
    m_pendingCodebasePaths.clear();
    m_codebaseIndexPathOverride = std::move(path);
    m_codebaseIndexReady = false;
    m_codebaseIncompleteRetries = 0;
    m_codebasePathsRetries = 0;
    m_codebaseSnapshotGeneration = 0;
    m_activeGroundedEvidence.clear();
    m_activeGroundingPrompt.clear();
    m_activeGroundingActiveFile.clear();
    m_activeGroundingProposalId.clear();
    m_activeGroundingSessionId.clear();
    m_activeGroundingRevisionId.clear();
    ++m_groundingValidationSerial;
    setGroundingUnavailable(QStringLiteral("UNAVAILABLE"));
}

void HostController::setCodebaseRefreshLimitsForTesting(std::optional<storage::CodebaseIndexLimits> limits) {
    m_codebaseRefreshLimitsForTesting = std::move(limits);
}

void HostController::setCodebaseWatcherLimitsForTesting(std::optional<CodebaseWatcher::Limits> limits) {
    m_codebaseWatcherLimitsForTesting = std::move(limits);
}

void HostController::startCodebaseRefreshForTesting(const QString& projectPath, bool consented) {
    const auto canonical = QFileInfo(projectPath).canonicalFilePath();
    if (canonical.isEmpty()) return;
    if (canonical != m_projectScanPath) {
        // The test seam models a real workspace selection when the path moves.
        ++m_workspaceEpoch;
        invalidateDownstreamPipeline();
        resetStalePathRecovery();
    }
    stopCodebaseWatcher();
    resetCodebaseWatcherRecovery();
    ++m_codebaseRefreshSerial;
    ++m_codebasePathsCoalesceSerial;
    m_codebaseRefreshInFlight = false;
    m_codebaseFullRefreshQueued = false;
    m_pendingCodebasePaths.clear();
    m_projectScanPath = canonical;
    m_workspaceRootIdentity.clear();
    m_codebaseContextConsented = consented;
    m_codebaseActiveFile.clear();
    m_codebaseIndexReady = false;
    m_codebaseIncompleteRetries = 0;
    m_codebasePathsRetries = 0;
    m_codebaseSnapshotGeneration = 0;
    setGroundingUnavailable(QStringLiteral("UNAVAILABLE"));
    if (consented) {
        startCodebaseWatcher(canonical);
        startCodebaseRefresh(canonical);
    }
}

QString HostController::groundedIntentXmlForTesting(const QString& prompt) {
    const auto context = groundedCanonicalIntentContext(prompt.toStdString());
    applyGroundingContext(context);
    return QString::fromStdString(context.xml);
}
#endif

} // namespace roclint::host
