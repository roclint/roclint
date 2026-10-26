#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <optional>
#include <cstdio>
#include <vector>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
bool safeRelative(const QString& path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(QLatin1Char('\\')) || path.contains(QChar(0))) return false;
    for (const auto& part : path.split(QLatin1Char('/'))) if (part.isEmpty() || part == QLatin1String("..") || part == QLatin1String(".")) return false;
    return true;
}
#ifdef Q_OS_UNIX
int openSafeDescriptor(const QString& root, const QString& relative, bool directory) {
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const auto components = (root + (relative.isEmpty() ? QString{} : QLatin1Char('/') + relative)).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; fd >= 0 && i < components.size(); ++i) {
        if (components[i] == QLatin1String("..") || components[i] == QLatin1String(".")) { ::close(fd); return -1; }
        const auto name = QFile::encodeName(components[i]);
        const int next = ::openat(fd, name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                                  (i + 1 < components.size() ? O_DIRECTORY : (directory ? O_DIRECTORY : 0)));
        ::close(fd); fd = next;
    }
    return fd;
}
#endif
#ifdef Q_OS_WIN
// Deny write/delete sharing while traversing so an ancestor cannot become a junction.
class WindowsPathLease {
public:
    explicit WindowsPathLease(const QString& path) {
        const auto absolute = QDir::toNativeSeparators(path);
        if (absolute.size() < 3 || absolute[1] != QLatin1Char(':') || absolute[2] != QLatin1Char('\\')) return;
        QString prefix = absolute.left(3);
        const auto parts = absolute.mid(3).split(QLatin1Char('\\'), Qt::SkipEmptyParts);
        for (const auto& part : parts) {
            if (part == QLatin1String("..") || part == QLatin1String(".") || part.contains(QLatin1Char(':'))) return;
            prefix += (prefix.endsWith(QLatin1Char('\\')) ? QString{} : QStringLiteral("\\")) + part;
            const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(prefix.utf16()), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return;
            handles_.push_back(handle);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        }
        valid_ = !handles_.empty();
    }
    ~WindowsPathLease() { for (const auto handle : handles_) CloseHandle(handle); }
    WindowsPathLease(const WindowsPathLease&) = delete;
    WindowsPathLease& operator=(const WindowsPathLease&) = delete;
    bool valid() const { return valid_; }
    HANDLE handle() const { return handles_.back(); }
private:
    std::vector<HANDLE> handles_;
    bool valid_{false};
};
#endif
QString rootIdentity(const QString& root) {
#ifdef Q_OS_UNIX
    const int fd = openSafeDescriptor(root, {}, true);
    if (fd < 0) return {};
    struct stat info{}; const bool ok = ::fstat(fd, &info) == 0; ::close(fd);
    return ok ? QString::number(static_cast<qulonglong>(info.st_dev)) + QLatin1Char(':') + QString::number(static_cast<qulonglong>(info.st_ino)) : QString{};
#else
    WindowsPathLease lease(root);
    if (!lease.valid()) return {};
    BY_HANDLE_FILE_INFORMATION info{}; const bool ok = GetFileInformationByHandle(lease.handle(), &info);
    return ok && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        ? QString::number(info.dwVolumeSerialNumber) + QLatin1Char(':') + QString::number(info.nFileIndexHigh) + QLatin1Char(':') + QString::number(info.nFileIndexLow) : QString{};
#endif
}
std::optional<QByteArray> readSafeFile(const QString& root, const QString& relative) {
    if (!safeRelative(relative)) return std::nullopt;
#ifdef Q_OS_UNIX
    int fd = openSafeDescriptor(root, relative, false);
    if (fd < 0) return std::nullopt;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { ::close(fd); return std::nullopt; }
    QByteArray bytes(8192, Qt::Uninitialized);
    const auto count = ::read(fd, bytes.data(), static_cast<size_t>(bytes.size()));
    ::close(fd);
    if (count < 0) return std::nullopt;
    bytes.resize(static_cast<qsizetype>(count));
    return bytes;
#else
    // Pin each ancestor without delete sharing so a junction cannot replace it during the open.
    QVector<HANDLE> ancestors;
    auto cleanup = [&]() { for (auto handle : ancestors) CloseHandle(handle); };
    const auto absolute = QDir::toNativeSeparators(root + QLatin1Char('/') + relative);
    QString prefix = absolute.left(3);
    const auto parts = absolute.mid(3).split(QLatin1Char('\\'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
        prefix += (prefix.endsWith(QLatin1Char('\\')) ? QString{} : QStringLiteral("\\")) + parts[i];
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(prefix.utf16()), i + 1 == parts.size() ? GENERIC_READ : FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { cleanup(); return std::nullopt; }
        ancestors.append(handle);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            (i + 1 == parts.size() && ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || GetFileType(handle) != FILE_TYPE_DISK))) {
            cleanup(); return std::nullopt;
        }
    }
    QByteArray bytes(8192, Qt::Uninitialized); DWORD count = 0;
    const bool ok = !ancestors.isEmpty() && ReadFile(ancestors.last(), bytes.data(), 8192, &count, nullptr);
    cleanup(); if (!ok) return std::nullopt; bytes.resize(count); return bytes;
#endif
}
QJsonObject extractDynamicAstContext(const QString& promptText, const QString& projectRoot, const QStringList& projectFiles) {
    QElapsedTimer budget; budget.start();
    QJsonObject meta;
    meta[QStringLiteral("prompt")] = promptText;
    const QString pLower = promptText.toLower();

    bool incomplete = false;
    // 1. Matched files
    QStringList matchedFiles;
    for (const auto& file : projectFiles) {
        const QFileInfo fi(file);
        const QString baseName = fi.fileName().toLower();
        if (pLower.contains(baseName)) {
            if (matchedFiles.size() < 20 && file.toUtf8().size() <= 512) matchedFiles.append(file);
        }
    }
    if (!matchedFiles.isEmpty()) {
        meta[QStringLiteral("matched_files")] = QJsonArray::fromStringList(matchedFiles);
        meta[QStringLiteral("workspace_matches")] = QJsonArray::fromStringList(matchedFiles);
    }

    // 2. Unbounded Target Files (Multi-file collisions)
    QStringList targetCollisionFiles;
    const bool mentionsNav = pLower.contains(QStringLiteral("navigation")) || pLower.contains(QStringLiteral("nav link")) ||
                             pLower.contains(QStringLiteral("site navigation")) || pLower.contains(QStringLiteral("header nav"));
    if (mentionsNav && !projectRoot.isEmpty()) {
        int reads = 0;
        for (const auto& relFile : projectFiles) {
            if (relFile.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive) ||
                relFile.endsWith(QStringLiteral(".qml"), Qt::CaseInsensitive)) {
                if (++reads > 64 || budget.elapsed() >= 400) { incomplete = true; break; }
                const auto bytes = readSafeFile(projectRoot, relFile);
                if (bytes.has_value()) {
                    const QString content = QString::fromUtf8(*bytes);
                    if (content.contains(QStringLiteral("<nav"), Qt::CaseInsensitive) ||
                        content.contains(QStringLiteral("class=\"nav"), Qt::CaseInsensitive)) {
                        if (targetCollisionFiles.size() < 20 && relFile.toUtf8().size() <= 512) targetCollisionFiles.append(relFile);
                    }
                } else incomplete = true;
            }
        }
    }
    if (!incomplete && targetCollisionFiles.size() > 1) {
        meta[QStringLiteral("unbounded_target_files")] = QJsonArray::fromStringList(targetCollisionFiles);
    }

    // 3. Subjective / Unbounded Action
    static const QRegularExpression subjectiveRe(QStringLiteral("\\b(much nicer|nicer|more modern|modern|look better|cleaner look|beautify)\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression numericCssRe(QStringLiteral("\\b\\d+(?:\\.\\d+)?(?:rem|em|px|pt|%|vh|vw|s|ms)\\b"), QRegularExpression::CaseInsensitiveOption);
    if (subjectiveRe.match(pLower).hasMatch() && !numericCssRe.match(pLower).hasMatch()) {
        meta[QStringLiteral("unbounded_action")] = true;
    }

    // 4. Constraint Violations / Rule Conflict
    static const QRegularExpression negativeRe(QStringLiteral("(?:without modifying|without changing|do not modify|don't change|must not modify)\\s+([^.,;]+)"), QRegularExpression::CaseInsensitiveOption);
    const auto negMatch = negativeRe.match(pLower);
    if (negMatch.hasMatch()) {
        const QString forbidden = negMatch.captured(1).trimmed();
        QJsonArray violations;
        violations.append(forbidden);
        meta[QStringLiteral("constraint_violations")] = violations;
    }

    // 5. Missing Backend Sink in Static Workspace
    static const QRegularExpression sinkRe(QStringLiteral("\\b(send(?:s)? and save(?:d)?|sent and saved|wire up .*form|submissions? get sent|save submissions?)\\b"), QRegularExpression::CaseInsensitiveOption);
    if (sinkRe.match(pLower).hasMatch()) {
        bool hasBackend = false;
        bool hasForm = false;
        for (const auto& file : projectFiles) {
            const QString ext = QFileInfo(file).suffix().toLower();
            if (ext == QStringLiteral("py") || ext == QStringLiteral("go") || ext == QStringLiteral("rs") ||
                ext == QStringLiteral("php") || ext == QStringLiteral("rb") || file.contains(QStringLiteral("server"))) {
                hasBackend = true;
            }
            if (ext == QStringLiteral("html")) {
                hasForm = true;
            }
        }
        if (hasForm && !hasBackend) {
            meta[QStringLiteral("missing_backend_sink")] = true;
        }
    }

    meta.remove(QStringLiteral("prompt"));
    meta[QStringLiteral("context_incomplete")] = incomplete;
    if (QJsonDocument(meta).toJson(QJsonDocument::Compact).size() > 32768)
        meta = {{QStringLiteral("context_incomplete"), true}};
    return meta;
}
QJsonObject scanProject(const QString& rootPath) {
    const QStringList excluded{"node_modules", "vendor", "build", "dist", "out", "target", "venv", "__pycache__", "CMakeFiles"};
    const QStringList extensions{"cpp", "hpp", "c", "h", "cc", "cxx", "mm", "qml", "js", "jsx", "ts", "tsx", "py", "rs", "go", "java", "kt", "swift", "cs", "md", "txt", "cmake", "json", "yaml", "yml", "toml", "html", "css", "sh", "ps1", "sql"};
    struct Directory { QString path; int depth; };
    QList<Directory> pending{{QString{}, 0}};
    QStringList files;
    QElapsedTimer clock; clock.start(); int visited = 0; bool incomplete = false;
    [[maybe_unused]] const QDir root(rootPath);
    while (!pending.isEmpty() && !incomplete) {
        const auto directory = pending.takeLast();
#ifdef Q_OS_UNIX
        const int fd = openSafeDescriptor(rootPath, directory.path, true);
        if (fd < 0) { incomplete = true; break; }
        DIR* entries = ::fdopendir(fd);
        if (!entries) { ::close(fd); incomplete = true; break; }
        while (auto* entry = ::readdir(entries)) {
            if (++visited > 100000 || files.size() >= 5000 || clock.elapsed() >= 1800) { incomplete = true; break; }
            const auto name = QFile::decodeName(entry->d_name);
            if (name.startsWith(QLatin1Char('.'))) continue;
            const auto relative = directory.path.isEmpty() ? name : directory.path + QLatin1Char('/') + name;
            struct stat info{};
            if (::fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) { incomplete = true; break; }
            if (S_ISDIR(info.st_mode)) {
                if (excluded.contains(name, Qt::CaseInsensitive) || name.startsWith("build-") || name.startsWith("build_") || name.startsWith("cmake-build-")) continue;
                if (directory.depth >= 32 || pending.size() >= 4096) { incomplete = true; break; }
                pending.append({relative, directory.depth + 1});
            } else if (S_ISREG(info.st_mode) && extensions.contains(QFileInfo(name).suffix().toLower())) files.append(relative);
        }
        ::closedir(entries);
#else
        WindowsPathLease lease(root.filePath(directory.path));
        if (!lease.valid()) { incomplete = true; break; }
        QDirIterator entries(root.filePath(directory.path), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        while (entries.hasNext()) {
            if (++visited > 100000 || files.size() >= 5000 || clock.elapsed() >= 1800) { incomplete = true; break; }
            entries.next(); const auto entry = entries.fileInfo(); const auto name = entry.fileName();
            if (name.startsWith(QLatin1Char('.')) || entry.isSymLink()) continue;
            if (entry.isDir()) {
                if (excluded.contains(name, Qt::CaseInsensitive) || name.startsWith("build-") || name.startsWith("build_") || name.startsWith("cmake-build-")) continue;
                if (directory.depth >= 32 || pending.size() >= 4096) { incomplete = true; break; }
                pending.append({root.relativeFilePath(entry.filePath()), directory.depth + 1});
            } else if (entry.isFile() && extensions.contains(entry.suffix().toLower())) files.append(root.relativeFilePath(entry.filePath()));
        }
#endif
    }
    files.sort(Qt::CaseInsensitive);
    return {{"files", QJsonArray::fromStringList(files)}, {"context_incomplete", incomplete}};
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QFile input; if (!input.open(stdin, QIODevice::ReadOnly)) return 1;
    QByteArray bytes;
    while (!input.atEnd() && bytes.size() <= 1024 * 1024) bytes += input.read(16384);
    if (bytes.size() > 1024 * 1024) return 1;
    const auto request = QJsonDocument::fromJson(bytes).object();
    const auto requestedRoot = request.value("root").toString();
    if (!requestedRoot.isEmpty() && (!QDir::isAbsolutePath(requestedRoot) || QFileInfo(requestedRoot).isSymLink())) return 1;
    const auto root = requestedRoot.isEmpty() ? QString{} : QFileInfo(requestedRoot).canonicalFilePath();
    if (!requestedRoot.isEmpty() && (root.isEmpty() || !QFileInfo(root).isDir())) return 1;
    const auto identity = root.isEmpty() ? QString{} : rootIdentity(root);
    if (request.value("operation") == QLatin1String("scan") && root.isEmpty()) return 1;
    QJsonObject result;
    if (request.value("operation") == QLatin1String("scan")) {
        if (identity.isEmpty()) return 1;
        result = scanProject(root);
        result["root_identity"] = identity;
    }
    else {
        QStringList files;
        for (const auto& value : request.value("files").toArray()) if (files.size() < 5000 && safeRelative(value.toString())) files.append(value.toString());
        const bool replaced = !files.isEmpty() && (identity.isEmpty() || request.value("root_identity").toString() != identity);
        if (replaced) files.clear();
        const bool scanIncomplete = request.value("scan_incomplete").toBool();
        result = extractDynamicAstContext(request.value("prompt").toString(), scanIncomplete ? QString{} : root, files);
        if (replaced || scanIncomplete) {
            result.remove("unbounded_target_files");
            result.remove("missing_backend_sink");
            result["context_incomplete"] = true;
        }
    }
    const auto output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (output.size() > 1024 * 1024) return 1;
    return std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout) == static_cast<size_t>(output.size()) ? 0 : 1;
}
