#include "HostController.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSettings>
#include <QTemporaryFile>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace roclint::host {
namespace {
QStringList scanProject(const QString& rootPath) {
    // Prune before descending. Read filenames only, never source text or hidden credentials.
    const QStringList excluded{QStringLiteral("node_modules"), QStringLiteral("vendor"), QStringLiteral("build"),
        QStringLiteral("dist"), QStringLiteral("out"), QStringLiteral("target"), QStringLiteral("venv"),
        QStringLiteral("__pycache__"), QStringLiteral("CMakeFiles")};
    const QStringList extensions{QStringLiteral("cpp"), QStringLiteral("hpp"), QStringLiteral("c"), QStringLiteral("h"),
        QStringLiteral("cc"), QStringLiteral("cxx"), QStringLiteral("mm"), QStringLiteral("qml"), QStringLiteral("js"),
        QStringLiteral("jsx"), QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("py"), QStringLiteral("rs"),
        QStringLiteral("go"), QStringLiteral("java"), QStringLiteral("kt"), QStringLiteral("swift"), QStringLiteral("cs"),
        QStringLiteral("md"), QStringLiteral("txt"), QStringLiteral("cmake"), QStringLiteral("json"), QStringLiteral("yaml"),
        QStringLiteral("yml"), QStringLiteral("toml"), QStringLiteral("html"), QStringLiteral("css"), QStringLiteral("sh"),
        QStringLiteral("ps1"), QStringLiteral("sql")};
    QStringList directories{rootPath}, result;
    QElapsedTimer elapsed;
    elapsed.start();
    const QDir root(rootPath);
    int visited = 0;
    while (!directories.isEmpty() && result.size() < 5000 && visited < 100000 && elapsed.elapsed() < 2000) {
        const auto folder = directories.takeLast();
        const auto entries = QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                       QDir::Name);
        for (const auto& entry : entries) {
            if (++visited > 100000 || result.size() >= 5000 || elapsed.elapsed() >= 2000) break;
            const auto name = entry.fileName();
            if (name.startsWith(QLatin1Char('.'))) continue;
            if (entry.isDir()) {
                if (!excluded.contains(name, Qt::CaseInsensitive) && !name.startsWith(QStringLiteral("build-")) &&
                    !name.startsWith(QStringLiteral("build_")) && !name.startsWith(QStringLiteral("cmake-build-")))
                    directories.append(entry.absoluteFilePath());
            } else if (extensions.contains(entry.suffix().toLower())) result.append(root.relativeFilePath(entry.filePath()));
        }
    }
    result.sort(Qt::CaseInsensitive);
    return result;
}
}

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
    }
    return canonical;
}

QStringList HostController::getProjectFiles(const QString& projectPath) {
    const auto canonical = QFileInfo(projectPath).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) return {};
    if (canonical == m_projectScanPath) return m_projectFiles;
    m_projectScanPath = canonical;
    m_projectFiles.clear();
    const auto serial = ++m_projectScanSerial;
    auto* watcher = new QFutureWatcher<QStringList>(this);
    ++m_inFlight;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, serial]() {
        --m_inFlight;
        const auto files = watcher->result();
        watcher->deleteLater();
        if (serial != m_projectScanSerial) return;
        m_projectFiles = files;
        emit projectFilesChanged();
    });
    watcher->setFuture(QtConcurrent::run([canonical]() { return scanProject(canonical); }));
    return {};
}

QString HostController::scopePreamble(const QString& projectPath, const QString& scope, const QString& file) {
    const auto root = QFileInfo(projectPath).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir() || root.contains(QLatin1Char('\n')) || root.contains(QLatin1Char('\r'))) {
        setLastError(QStringLiteral("Choose an existing workspace before applying scope.")); return {};
    }
    if (scope == QLatin1String("global")) return QStringLiteral("Workspace: %1\nScope: entire project.\n\n").arg(root);
    if (scope != QLatin1String("local") || file.isEmpty() || file.contains(QLatin1Char('\n')) || file.contains(QLatin1Char('\r'))) {
        setLastError(QStringLiteral("Select a real file before applying single-file scope.")); return {};
    }
    const QFileInfo info(QDir(root).filePath(file));
    const QString relative = QDir(root).relativeFilePath(info.canonicalFilePath());
    if (!info.isFile() || info.isSymLink() || relative == QLatin1String("..") || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
        setLastError(QStringLiteral("The selected file must be inside the workspace.")); return {};
    }
    return QStringLiteral("Workspace: %1\nScope: single file %2. Do not modify other files.\n\n").arg(root, relative);
}

QString HostController::openModelFolderDialog() {
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

} // namespace roclint::host
