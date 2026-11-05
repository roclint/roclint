#include "HostController.hpp"
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
#include <filesystem>

namespace roclint::host {
namespace {
std::filesystem::path codebaseIndexPath() {
    const auto dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dataDirectory.isEmpty() || !QDir().mkpath(dataDirectory)) return {};
    return std::filesystem::path((dataDirectory + QStringLiteral("/codebase-index.db")).toStdWString());
}
} // namespace

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
        // cached file list, but schedule the incremental AST pass.
        if (m_codebaseContextConsented && !m_codebaseRefreshInFlight) startCodebaseRefresh(canonical);
        return m_projectFiles;
    }
    m_projectScanPath = canonical;
    m_projectFiles.clear();
    m_codebaseActiveFile.clear();
    m_codebaseIndex.reset();
    m_codebaseIndexReady = false;
    m_workspaceRootIdentity.clear();
    m_workspaceScanIncomplete = true;
    if (!QCoreApplication::organizationName().isEmpty()) {
        const auto roots = QSettings().value(QStringLiteral("workspace/codebaseContextConsentedRoots")).toStringList();
        m_codebaseContextConsented = roots.contains(canonical);
    }
    if (m_codebaseContextConsented) startCodebaseRefresh(canonical);
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
    startCodebaseRefresh(m_projectScanPath);
    return true;
}

void HostController::startCodebaseRefresh(const QString& projectPath) {
    if (!m_codebaseContextConsented || projectPath.isEmpty()) return;
    const auto indexPath = codebaseIndexPath();
    if (indexPath.empty()) return;
    const auto rootPath = std::filesystem::path(projectPath.toStdWString());
    const auto serial = ++m_codebaseRefreshSerial;
    m_codebaseRefreshInFlight = true;
    m_codebaseIndexReady = false;
    m_codebaseIndex.reset();
    auto* watcher = new QFutureWatcher<QString>(this);
    ++m_inFlight;
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, serial]() {
        --m_inFlight;
        if (serial == m_codebaseRefreshSerial) {
            m_codebaseRefreshInFlight = false;
            const auto error = watcher->result();
            m_codebaseIndexReady = error.isEmpty();
            if (!m_codebaseIndexReady) {
                m_codebaseIndex.reset();
                setLastError(QStringLiteral("Codebase index refresh failed: %1")
                                 .arg(error.isEmpty() ? QStringLiteral("unknown error") : error));
            }
        }
        watcher->deleteLater();
        finishQuitWhenRuntimesStopped();
    });
    watcher->setFuture(QtConcurrent::run([rootPath, indexPath]() -> QString {
        storage::CodebaseIndex index(indexPath, rootPath);
        const storage::WorkspaceContext workspace{rootPath, {}, true};
        const auto refreshed = index.refresh(workspace);
        if (!refreshed.ok()) return QString::fromStdString(refreshed.error().message);
        return {};
    }));
}

QString HostController::scopePreamble(const QString& projectPath, const QString& scope, const QString& file) {
    const auto root = QFileInfo(projectPath).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir() || root.contains(QLatin1Char('\n')) || root.contains(QLatin1Char('\r'))) {
        setLastError(QStringLiteral("Choose an existing workspace before applying scope.")); return {};
    }
    if (scope == QLatin1String("global")) {
        m_codebaseActiveFile.clear();
        return QStringLiteral("Workspace: %1\nScope: entire project.\n\n").arg(root);
    }
    if (scope != QLatin1String("local") || file.isEmpty() || file.contains(QLatin1Char('\n')) || file.contains(QLatin1Char('\r'))) {
        setLastError(QStringLiteral("Select a real file before applying single-file scope.")); return {};
    }
    const QFileInfo info(QDir(root).filePath(file));
    const QString relative = QDir(root).relativeFilePath(info.canonicalFilePath());
    if (!info.isFile() || info.isSymLink() || relative == QLatin1String("..") || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
        setLastError(QStringLiteral("The selected file must be inside the workspace.")); return {};
    }
    m_codebaseActiveFile = relative;
    return QStringLiteral("Workspace: %1\nScope: single file %2. Do not modify other files.\n\n").arg(root, relative);
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

GroundedPromptContext HostController::groundedCanonicalIntentContext(std::string_view prompt) {
    GroundedPromptContext context;
    if (!m_codebaseContextConsented || m_projectScanPath.isEmpty() || !m_codebaseIndexReady) return context;
    auto raw = core::rewrite::CanonicalIntentExtractor::extract(prompt);
    if (raw.empty()) return context;
    if (!m_codebaseIndex) {
        const auto path = codebaseIndexPath();
        if (path.empty()) return context;
        std::error_code error;
        const auto root = std::filesystem::canonical(
            std::filesystem::path(m_projectScanPath.toStdWString()), error);
        if (error) return context;
        m_codebaseIndex = std::make_unique<storage::CodebaseIndex>(path, root);
    }
    const auto grounded = core::rewrite::AstGroundingEngine::groundIntents(
        raw, m_codebaseActiveFile.toStdString(), *m_codebaseIndex);
    context.xml = core::rewrite::AstGroundingEngine::renderPromptXml(grounded);
    const auto workspaceIdentity = m_workspaceRootIdentity.isEmpty()
        ? m_projectScanPath.toStdString() : m_workspaceRootIdentity.toStdString();
    context.astSnapshotHash = core::rewrite::AstGroundingEngine::computeAstSnapshotHash(
        grounded, workspaceIdentity);
    return context;
}

} // namespace roclint::host
