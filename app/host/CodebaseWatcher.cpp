#include "CodebaseWatcher.hpp"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace roclint::host {
namespace {

const QSet<QString>& excludedDirectoryNames() {
    static const QSet<QString> names{
        QStringLiteral(".git"), QStringLiteral(".hg"), QStringLiteral(".svn"), QStringLiteral(".cache"),
        QStringLiteral("build"), QStringLiteral("dist"), QStringLiteral("out"), QStringLiteral("target"),
        QStringLiteral("node_modules"), QStringLiteral("vendor"), QStringLiteral("deps"),
        QStringLiteral("CMakeFiles"), QStringLiteral("__pycache__"), QStringLiteral("venv")};
    return names;
}

const QSet<QString>& supportedExtensions() {
    static const QSet<QString> extensions{
        QStringLiteral("cpp"), QStringLiteral("hpp"), QStringLiteral("cc"), QStringLiteral("hh"),
        QStringLiteral("cxx"), QStringLiteral("hxx"), QStringLiteral("c"), QStringLiteral("h")};
    return extensions;
}

QString cleanAbsolute(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

#ifdef ROCLINT_ENABLE_TEST_MOCKS
bool g_forceAddPathFailure = false;
bool g_forceStartFailure = false;
#endif

} // namespace

#ifdef ROCLINT_ENABLE_TEST_MOCKS
void CodebaseWatcher::setForceAddPathFailureForTesting(bool fail) {
    g_forceAddPathFailure = fail;
}

void CodebaseWatcher::setForceStartFailureForTesting(bool fail) {
    g_forceStartFailure = fail;
}
#endif

CodebaseWatcher::CodebaseWatcher(QObject* parent)
    : QObject(parent), watcher_(this) {
    debounceTimer_.setSingleShot(true);
    connect(&debounceTimer_, &QTimer::timeout, this, &CodebaseWatcher::flushPending);

    fallbackTimer_.setSingleShot(false);
    connect(&fallbackTimer_, &QTimer::timeout, this, [this]() {
        if (running_) emit fullRefreshRequired(QStringLiteral("periodic-fallback"));
    });

    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &CodebaseWatcher::onDirectoryChanged);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &CodebaseWatcher::onFileChanged);
}

CodebaseWatcher::~CodebaseWatcher() {
    stop();
}

bool CodebaseWatcher::isExcludedDirectoryName(const QString& name) {
    return excludedDirectoryNames().contains(name);
}

bool CodebaseWatcher::isSupportedSourcePath(const QString& path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    return supportedExtensions().contains(suffix);
}

bool CodebaseWatcher::start(const QString& workspaceRoot, const Limits& limits) {
    stop();
    limits_ = limits;
    debounceTimer_.setInterval(std::max(1, limits.debounceMs));
    if (limits.fallbackRefreshIntervalMs > 0) fallbackTimer_.setInterval(limits.fallbackRefreshIntervalMs);
    if (workspaceRoot.trimmed().isEmpty()) return false;

    const QFileInfo rootInfo(workspaceRoot);
    const auto canonical = rootInfo.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
        root_ = workspaceRoot;
        noteFailure(QStringLiteral("workspace-unavailable"));
        return false;
    }

    root_ = canonical;
    batch_ = CodebaseWatchBatch(limits.maximumPendingPaths);
    running_ = true;
    failed_ = false;
    overflowed_ = false;

#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (g_forceStartFailure) {
        running_ = false;
        noteFailure(QStringLiteral("watch-start-failed"));
        return false;
    }
#endif
    if (!addDirectory(root_, false)) {
        if (!failed_ && !overflowed_) noteFailure(QStringLiteral("watch-add-failed"));
        // Nothing below the root is observed when the root registration fails,
        // so recovery must treat this as a stopped watcher rather than partial.
        if (!overflowed_) running_ = false;
    }
    // A healthy watcher still reconciles periodically. This is the only path
    // that can discover a missed creation/deletion notification without a
    // per-keystroke rescan; failure/overflow arm the same bounded timer.
    if (running_ && !failed_ && !overflowed_) armFallback();
    return running_;
}

void CodebaseWatcher::stop() {
    debounceTimer_.stop();
    fallbackTimer_.stop();
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    directoryFiles_.clear();
    directorySubdirectories_.clear();
    watchedDirectories_.clear();
    watchedFiles_.clear();
    batch_.clear();
    root_.clear();
    running_ = false;
    failed_ = false;
    overflowed_ = false;
}

void CodebaseWatcher::flushPending() {
    debounceTimer_.stop();
    if (batch_.fullRefreshRequested()) {
        const auto reason = QString::fromStdString(batch_.fullRefreshReason());
        batch_.clear();
        emit fullRefreshRequired(reason.isEmpty() ? QStringLiteral("watcher") : reason);
        return;
    }
    const auto paths = batch_.takePaths();
    if (paths.empty()) return;
    QStringList relativePaths;
    relativePaths.reserve(static_cast<qsizetype>(paths.size()));
    for (const auto& path : paths) relativePaths.append(QString::fromStdString(path));
    emit pathsChanged(relativePaths);
}

bool CodebaseWatcher::addWatchPath(const QString& absolutePath) {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (g_forceAddPathFailure) return false;
#endif
    return watcher_.addPath(absolutePath);
}

void CodebaseWatcher::onFileChanged(const QString& path) {
    if (!running_) return;
    const auto absolute = cleanAbsolute(path);
    if (!isSupportedSourcePath(absolute)) return;
    queueFile(absolute);
    if (QFileInfo::exists(absolute)) {
        // Some editors replace a file instead of writing it in place, which
        // drops the file watch on Windows. The parent directory watch catches
        // the replacement; re-arm the file watch when we can.
        if (!watcher_.files().contains(absolute) && !addWatchPath(absolute))
            noteFailure(QStringLiteral("watch-add-failed"));
    } else {
        watcher_.removePath(absolute);
        watchedFiles_.remove(absolute);
        const auto parent = QFileInfo(absolute).absolutePath();
        auto files = directoryFiles_.find(parent);
        if (files != directoryFiles_.end()) files->remove(absolute);
    }
}

void CodebaseWatcher::onDirectoryChanged(const QString& path) {
    if (!running_) return;
    const auto absolute = cleanAbsolute(path);
    if (watchedDirectories_.contains(absolute)) {
        rescanDirectory(absolute);
    } else if (QFileInfo(absolute).isDir()) {
        // A newly created directory can appear before its parent's event.
        addDirectory(absolute, true);
    }
}

void CodebaseWatcher::rescanDirectory(const QString& absolutePath) {
    const QFileInfo info(absolutePath);
    if (!info.isDir()) {
        removeDirectory(absolutePath);
        if (absolutePath == root_) {
            // The watched root is gone, so nothing is observed any more.
            running_ = false;
            noteFailure(QStringLiteral("workspace-removed"));
        } else {
            requestFullRefresh(QStringLiteral("directory-removed"));
        }
        return;
    }
    if (!watchedDirectories_.contains(absolutePath)) {
        addDirectory(absolutePath, true);
        return;
    }
    if (!watcher_.directories().contains(absolutePath) && !addWatchPath(absolutePath))
        noteFailure(QStringLiteral("watch-add-failed"));

    const auto previousFiles = directoryFiles_.value(absolutePath);
    QSet<QString> activelyWatched;
    for (const auto& watched : watcher_.files()) activelyWatched.insert(watched);
    QHash<QString, FileStamp> currentFiles;
    const auto entries = QDir(absolutePath).entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entries) {
        if (!isSupportedSourcePath(entry.fileName())) continue;
        const auto filePath = entry.canonicalFilePath();
        if (filePath.isEmpty() || !isSameOrWithinRoot(root_, filePath)) continue;
        const auto stamp = stampFor(entry);
        currentFiles.insert(filePath, stamp);
        if (previousFiles.value(filePath, FileStamp{-1, -1}) != stamp) queueFile(filePath);
        if (watchedFiles_.contains(filePath)) {
            if (!activelyWatched.contains(filePath)) {
                if (addWatchPath(filePath)) watchedFiles_.insert(filePath);
                else noteFailure(QStringLiteral("watch-add-failed"));
            }
            continue;
        }
        if (static_cast<std::size_t>(watchedFiles_.size()) >= limits_.maximumWatchedFiles) {
            noteOverflow(QStringLiteral("watch-file-limit"));
            continue;
        }
        if (!addWatchPath(filePath)) {
            noteFailure(QStringLiteral("watch-add-failed"));
            continue;
        }
        watchedFiles_.insert(filePath);
    }
    for (auto it = previousFiles.cbegin(); it != previousFiles.cend(); ++it) {
        if (currentFiles.contains(it.key())) continue;
        watcher_.removePath(it.key());
        watchedFiles_.remove(it.key());
        queueFile(it.key());
    }
    directoryFiles_.insert(absolutePath, currentFiles);

    QStringList currentSubdirectories;
    const auto subdirectories = QDir(absolutePath).entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : subdirectories) {
        if (isExcludedDirectoryName(entry.fileName())) continue;
        const auto subdirectory = entry.canonicalFilePath();
        if (subdirectory.isEmpty() || !isSameOrWithinRoot(root_, subdirectory)) continue;
        currentSubdirectories.append(subdirectory);
        if (!watchedDirectories_.contains(subdirectory)) addDirectory(subdirectory, true);
    }
    const auto previousSubdirectories = directorySubdirectories_.value(absolutePath);
    for (const auto& previous : previousSubdirectories) {
        if (currentSubdirectories.contains(previous)) continue;
        removeDirectory(previous);
        requestFullRefresh(QStringLiteral("directory-removed"));
    }
    directorySubdirectories_.insert(absolutePath, currentSubdirectories);
}

bool CodebaseWatcher::addDirectory(const QString& absolutePath, bool emitFiles) {
    const QFileInfo info(absolutePath);
    if (!info.isDir()) return false;
    const auto canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !isSameOrWithinRoot(root_, canonical)) return false;
    if (canonical != root_ && isExcludedDirectoryName(info.fileName())) return true;
    if (watchedDirectories_.contains(canonical)) return true;
    if (static_cast<std::size_t>(watchedDirectories_.size()) >= limits_.maximumWatchedDirectories) {
        noteOverflow(QStringLiteral("watch-directory-limit"));
        return false;
    }
    if (!addWatchPath(canonical)) {
        noteFailure(QStringLiteral("watch-add-failed"));
        return false;
    }
    watchedDirectories_.insert(canonical);
    directoryFiles_.insert(canonical, {});
    directorySubdirectories_.insert(canonical, {});

    QHash<QString, FileStamp> files;
    const auto entries = QDir(canonical).entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entries) {
        if (!isSupportedSourcePath(entry.fileName())) continue;
        const auto filePath = entry.canonicalFilePath();
        if (filePath.isEmpty() || !isSameOrWithinRoot(root_, filePath)) continue;
        files.insert(filePath, stampFor(entry));
        if (emitFiles) queueFile(filePath);
        if (watchedFiles_.contains(filePath)) continue;
        if (static_cast<std::size_t>(watchedFiles_.size()) >= limits_.maximumWatchedFiles) {
            noteOverflow(QStringLiteral("watch-file-limit"));
            continue;
        }
        if (!addWatchPath(filePath)) {
            noteFailure(QStringLiteral("watch-add-failed"));
            continue;
        }
        watchedFiles_.insert(filePath);
    }
    directoryFiles_.insert(canonical, files);

    QStringList subdirectories;
    const auto entriesForDirectories = QDir(canonical).entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entriesForDirectories) {
        if (isExcludedDirectoryName(entry.fileName())) continue;
        const auto subdirectory = entry.canonicalFilePath();
        if (subdirectory.isEmpty() || !isSameOrWithinRoot(root_, subdirectory)) continue;
        subdirectories.append(subdirectory);
        if (!watchedDirectories_.contains(subdirectory)) addDirectory(subdirectory, emitFiles);
    }
    directorySubdirectories_.insert(canonical, subdirectories);
    return true;
}

void CodebaseWatcher::removeDirectory(const QString& absolutePath) {
    const auto subdirectories = directorySubdirectories_.take(absolutePath);
    for (const auto& subdirectory : subdirectories) removeDirectory(subdirectory);
    const auto files = directoryFiles_.take(absolutePath);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        watcher_.removePath(it.key());
        watchedFiles_.remove(it.key());
    }
    watcher_.removePath(absolutePath);
    watchedDirectories_.remove(absolutePath);
}

void CodebaseWatcher::queueFile(const QString& absolutePath) {
    if (!running_ || batch_.fullRefreshRequested()) return;
    const auto relative = relativePathFor(absolutePath);
    if (relative.isEmpty()) return;
    if (!batch_.addPath(relative.toStdString()) && batch_.overflowed())
        noteOverflow(QStringLiteral("path-overflow"));
    debounceTimer_.start();
}

void CodebaseWatcher::requestFullRefresh(const QString& reason) {
    batch_.requestFullRefresh(reason.toStdString());
    debounceTimer_.start();
}

void CodebaseWatcher::noteFailure(const QString& reason) {
    if (failed_) return;
    failed_ = true;
    emit watchFailed(reason);
    requestFullRefresh(reason);
    armFallback();
}

void CodebaseWatcher::noteOverflow(const QString& reason) {
    if (overflowed_) return;
    overflowed_ = true;
    emit watchOverflowed(reason);
    requestFullRefresh(reason);
    armFallback();
}

void CodebaseWatcher::armFallback() {
    if (limits_.fallbackRefreshIntervalMs > 0 && !fallbackTimer_.isActive())
        fallbackTimer_.start();
}

QString CodebaseWatcher::relativePathFor(const QString& absolutePath) const {
    if (root_.isEmpty()) return {};
    const auto absolute = cleanAbsolute(absolutePath);
    auto relative = QDir::fromNativeSeparators(QDir(root_).relativeFilePath(absolute));
    if (relative == QLatin1String(".") || relative == QLatin1String("..") ||
        relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
        return {};
    return relative;
}

bool CodebaseWatcher::isSameOrWithinRoot(const QString& root, const QString& candidate) {
    auto normalizedRoot = cleanAbsolute(root);
    auto normalizedCandidate = cleanAbsolute(candidate);
#ifdef Q_OS_WIN
    normalizedRoot = normalizedRoot.toLower();
    normalizedCandidate = normalizedCandidate.toLower();
#endif
    if (normalizedRoot == normalizedCandidate) return true;
    if (!normalizedRoot.endsWith(QLatin1Char('/'))) normalizedRoot.append(QLatin1Char('/'));
    return normalizedCandidate.startsWith(normalizedRoot);
}

CodebaseWatcher::FileStamp CodebaseWatcher::stampFor(const QFileInfo& info) {
    return FileStamp{info.lastModified().toMSecsSinceEpoch(), info.size()};
}

} // namespace roclint::host
