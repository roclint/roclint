#include "HostController.hpp"
#include <QJsonArray>
#include <utility>
#include <QKeySequence>
#include "ScreenPlacement.hpp"
#include "TerminalAdapter.hpp"

#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/prompt_state/TextNormalization.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QFileDialog>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <filesystem>

namespace roclint::host {

using core::ErrorCode;
using core::makeError;
using core::Status;
namespace analysis = core::analysis;
namespace intent = core::intent;
namespace rewrite = core::rewrite;
namespace review = core::review;

namespace {

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string ss(const QString& s) {
    const QByteArray utf8 = s.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

std::int64_t nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

std::string newOpaqueId(const char* prefix) {
    return std::string(prefix) + ss(QUuid::createUuid().toString(QUuid::Id128));
}

QString pinnedDigestKey(const QString& model) {
    return QStringLiteral("models/pinnedDigests/") + model;
}

bool isBlank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n'; });
}

QString analysisDeadlineReason() {
    return QStringLiteral("ANALYSIS_GROUP_DEADLINE: The active analysis time limit expired. Retry explicitly or keep the original prompt.");
}

Status storageUnavailable() {
    return makeError(ErrorCode::PreconditionFailed,
                     "History storage is unavailable, so this action can't be recorded. See the dashboard for details.");
}

std::string modelDetail(const bridge::GraniteModelInfo& info) {
    std::string detail = "digest=" + (info.digest.empty() ? std::string("not reported") : info.digest);
    detail += "; quantization=" + (info.quantization.empty() ? std::string("not reported") : info.quantization);
    return detail;
}

QString plainError(const core::Error& error) {
    switch (error.code) {
        case ErrorCode::StaleVersion:
            return QStringLiteral("The prompt changed while this action was pending. ") + qs(error.message);
        case ErrorCode::ProviderUnavailable:
            return QStringLiteral("The local model isn't available. ") + qs(error.message);
        case ErrorCode::ProviderFailed:
            return QStringLiteral("The local model returned an unusable reply. ") + qs(error.message);
        case ErrorCode::DbError:
            return QStringLiteral("Saving to history failed. ") + qs(error.message);
        case ErrorCode::InvalidInput:
        case ErrorCode::NotFound:
        case ErrorCode::Conflict:
        case ErrorCode::ValidationFailed:
        case ErrorCode::PreconditionFailed:
        case ErrorCode::Internal:
            break;
    }
    return qs(error.message);
}

// Model output that fails strict parsing is reported as an unusable reply, not as bad user input.
core::Error modelReplyError(const core::Error& error) {
    if (error.code != ErrorCode::InvalidInput) {
        return error;
    }
    return core::Error{ErrorCode::ProviderFailed,
                       "Nothing was shown because the reply didn't follow the required format. Try again or "
                       "rephrase the prompt. (Detail: " + error.message + ")"};
}

static QString resolveJuliaDirectory(const QString& configuredDir, const QString& baseDir) {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (!configuredDir.contains(QLatin1Char('/')) && !configuredDir.contains(QLatin1Char('\\')) && !configuredDir.isEmpty()) {
        return configuredDir;
    }
#endif
    auto hasJulia = [](const QString& dirPath) {
        if (dirPath.isEmpty()) return false;
        const QDir d(dirPath);
        return d.exists(QStringLiteral("model.safetensors")) || d.exists(QStringLiteral("roclint-julia.json"));
    };
    if (hasJulia(configuredDir)) return QDir::cleanPath(configuredDir);
    if (hasJulia(configuredDir + QStringLiteral("/s1"))) return QDir::cleanPath(configuredDir + QStringLiteral("/s1"));
    const QDir parent(configuredDir);
    if (hasJulia(parent.filePath(QStringLiteral("../s1")))) return QDir::cleanPath(parent.filePath(QStringLiteral("../s1")));
    if (hasJulia(baseDir + QStringLiteral("/s1"))) return QDir::cleanPath(baseDir + QStringLiteral("/s1"));
    if (hasJulia(baseDir)) return QDir::cleanPath(baseDir);
    const QString userHomeModels = QDir::homePath() + QStringLiteral("/models");
    if (hasJulia(userHomeModels + QStringLiteral("/s1"))) return QDir::cleanPath(userHomeModels + QStringLiteral("/s1"));
    if (hasJulia(JuliaRuntime::defaultModelDirectory())) return JuliaRuntime::defaultModelDirectory();
    return configuredDir.isEmpty() ? (baseDir + QStringLiteral("/s1")) : QDir::cleanPath(configuredDir);
}

static QString resolveNativeGraniteDirectory(const QString& configuredDir, const QString& baseDir) {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (!configuredDir.contains(QLatin1Char('/')) && !configuredDir.contains(QLatin1Char('\\')) && !configuredDir.isEmpty()) {
        return configuredDir;
    }
#endif
    auto hasGranite = [](const QString& dirPath) {
        if (dirPath.isEmpty()) return false;
        const QDir d(dirPath);
        for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
            if (d.exists(entry.toObject().value(QStringLiteral("filename")).toString())) return true;
        }
        return false;
    };
    if (hasGranite(configuredDir)) return QDir::cleanPath(configuredDir);
    if (hasGranite(configuredDir + QStringLiteral("/s2"))) return QDir::cleanPath(configuredDir + QStringLiteral("/s2"));
    const QDir parent(configuredDir);
    if (hasGranite(parent.filePath(QStringLiteral("../s2")))) return QDir::cleanPath(parent.filePath(QStringLiteral("../s2")));
    if (hasGranite(baseDir + QStringLiteral("/s2"))) return QDir::cleanPath(baseDir + QStringLiteral("/s2"));
    if (hasGranite(baseDir)) return QDir::cleanPath(baseDir);
    const QString userHomeModels = QDir::homePath() + QStringLiteral("/models");
    if (hasGranite(userHomeModels + QStringLiteral("/s2"))) return QDir::cleanPath(userHomeModels + QStringLiteral("/s2"));
    if (hasGranite(NativeGraniteRuntime::defaultDirectory())) return NativeGraniteRuntime::defaultDirectory();
    return configuredDir.isEmpty() ? (baseDir + QStringLiteral("/s2")) : QDir::cleanPath(configuredDir);
}

} // namespace

QString HostController::defaultModelsBaseDirectory() {
    const QString userModels = QDir::homePath() + QStringLiteral("/models");
    if (QFileInfo(userModels + QStringLiteral("/s1")).isDir() || QFileInfo(userModels + QStringLiteral("/s2")).isDir()) {
        return userModels;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/models");
}

// ----------------------------------------------------------------------------------------------
// Construction and storage
// ----------------------------------------------------------------------------------------------

HostController::HostController(QObject* parent)
    : HostController(QString(), createPlatformAccessibilityBridge(), parent) {
}

HostController::HostController(const QString& dbPathOverride, QObject* parent)
    : HostController(dbPathOverride, createUnavailableAccessibilityBridge(), parent) {
}

HostController::HostController(const QString& dbPathOverride,
                               std::unique_ptr<IAccessibilityBridge> accessibility,
                               QObject* parent)
    : QObject(parent),
      m_s1Adapter(bridge::ScorerEngine::JuliaLocal),
      m_julia(std::make_unique<JuliaRuntime>()),
      m_native(std::make_unique<NativeGraniteRuntime>()),
      m_nativeInstaller(std::make_unique<GraniteModelInstaller>()),
      m_modelsBaseDirectory(defaultModelsBaseDirectory()),
      m_nativeDirectory(NativeGraniteRuntime::defaultDirectory()),
      m_juliaModelDirectory(JuliaRuntime::defaultModelDirectory()),
      m_juliaPython(JuliaRuntime::defaultPython()),
      m_graniteAdapter(std::make_shared<bridge::GraniteAdapter>()),
      m_accessibility(accessibility ? std::move(accessibility) : createUnavailableAccessibilityBridge()) {
    if (auto client = OllamaClient::create(OllamaClient::defaultBaseUrl()); client.ok()) {
        m_ollama = client.value();
    }
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        const auto scorer = bridge::S1TriageAdapter::parseEngine(ss(settings.value(QStringLiteral("scoring/engine"), QStringLiteral("julia")).toString()));
        if (scorer.ok()) m_s1Adapter.setEngine(scorer.value());
        m_modelsBaseDirectory = settings.value(QStringLiteral("models/baseDirectory"), m_modelsBaseDirectory).toString();
        m_juliaModelDirectory = settings.value(QStringLiteral("scoring/juliaDirectory"), m_modelsBaseDirectory + QStringLiteral("/s1")).toString();
        m_juliaPython = settings.value(QStringLiteral("scoring/juliaPython"), m_juliaPython).toString();
        m_activeModel = settings.value(QStringLiteral("models/active"), m_activeModel).toString();
        m_modelDirectory = settings.value(QStringLiteral("models/directory")).toString();
        m_nativeDirectory = settings.value(QStringLiteral("models/nativeDirectory"), m_modelsBaseDirectory + QStringLiteral("/s2")).toString();
        m_ambientModeEnabled = settings.value(QStringLiteral("ambient/enabled"), false).toBool();
        m_hostContextEnabled = settings.value(QStringLiteral("invocation/hostContextEnabled"), true).toBool();
        m_invocationShortcut = settings.value(QStringLiteral("invocation/shortcut"), m_invocationShortcut).toString();
        const QString savedProfile = settings.value(QStringLiteral("rewrite/profile"), m_rewriteProfile).toString();
        if (const auto profile = rewrite::ProfileRegistry::getProfile(ss(savedProfile)); profile.has_value()) {
            m_rewriteProfile = qs(profile->id);
        }
    }
    if (dbPathOverride.isEmpty()) {
        QDir().mkpath(m_modelsBaseDirectory);
        QDir().mkpath(m_modelsBaseDirectory + QStringLiteral("/s1"));
        QDir().mkpath(m_modelsBaseDirectory + QStringLiteral("/s2"));
        m_juliaModelDirectory = resolveJuliaDirectory(m_juliaModelDirectory, m_modelsBaseDirectory);
        m_nativeDirectory = resolveNativeGraniteDirectory(m_nativeDirectory, m_modelsBaseDirectory);
    }
    initStorage(dbPathOverride);

    connect(m_julia.get(), &JuliaRuntime::stateChanged, this, [this]() {
        m_s1Adapter.setLocalModelReady(m_julia->isReady());
        emit scorerEngineChanged();
        continuePromptTriageLoad();
    });
    connect(m_julia.get(), &JuliaRuntime::evaluationFinished, this,
            [this](quint64 id, const bridge::SnapshotEvaluationResponse& response) { onJuliaFinished(id, &response, {}); });
    connect(m_julia.get(), &JuliaRuntime::evaluationFailed, this,
            [this](quint64 id, const QString& reason) { onJuliaFinished(id, nullptr, reason); });
    connect(m_julia.get(), &JuliaRuntime::promptClassified, this,
            [this](quint64 id, const analysis::PromptTriage& triage) { onPromptClassified(id, &triage, {}); });
    connect(m_julia.get(), &JuliaRuntime::promptClassificationFailed, this,
            [this](quint64 id, const QString& reason) { onPromptClassified(id, nullptr, reason); });

    connect(m_native.get(), &NativeGraniteRuntime::stateChanged, this, &HostController::onNativeRuntimeChanged);
    connect(m_nativeInstaller.get(), &GraniteModelInstaller::progress, this, [this](int percent, const QString& message) {
        m_downloadProgress = percent; m_downloadStatusMessage = message; emit downloadProgressChanged();
    });
    connect(m_nativeInstaller.get(), &GraniteModelInstaller::finished, this, [this](bool success, const QString& message) {
        const auto downloaded = std::exchange(m_downloadingNativeTag, {});
        m_isDownloadingModel = false; m_downloadStatusMessage = message;
        if (success && m_hostState != HostState::Quitting) {
            refreshAvailableModels();
            if ((m_discoveryQueued || m_pendingNativeRewrite) && downloaded == m_activeModel) prepareNativeModel();
        } else if (downloaded == m_activeModel) {
            if (m_discoveryQueued) { m_discoveryQueued = false; setAnalysisStatus(AnalysisStatus::Failed, message); }
            if (m_pendingNativeRewrite) { m_pendingNativeRewrite.reset(); setProposalStatus(ProposalUiStatus::Failed, message); }
        }
        emit downloadProgressChanged();
    });

    m_ambientDebounce.setSingleShot(true);
    m_ambientDebounce.setInterval(kAmbientDebounceMs);
    connect(&m_ambientDebounce, &QTimer::timeout, this, &HostController::onAmbientDebounce);
    connect(m_accessibility.get(), &IAccessibilityBridge::accessChanged, this, [this]() {
        emit accessibilityTrustedChanged();
        emit ambientModeEnabledChanged();
        applyAmbientObserving();
    });
    connect(m_accessibility.get(), &IAccessibilityBridge::fieldObserved, this, &HostController::onFieldObserved);
    connect(m_accessibility.get(), &IAccessibilityBridge::fieldLost, this, &HostController::onFieldLost);
    connect(m_accessibility.get(), &IAccessibilityBridge::injectionFinished, this,
            &HostController::onInjectionFinished);
    applyAmbientObserving();
    initializeResidencyPolicy();
}

HostController::~HostController() {
    m_residencyTimer.stop();
    if (m_nativeInstaller) { m_nativeInstaller->disconnect(this); m_nativeInstaller->cancel(); }
    if (m_native) { m_native->disconnect(this); m_native->stop(); }
    if (m_julia) {
        m_julia->stop();
        m_julia->disconnect(this);
    }
    if (m_accessibility) {
        m_accessibility->disconnect(this);
        m_accessibility->setObserving(false);
    }
}

void HostController::initStorage(const QString& dbPathOverride) {
    QString finalPath = dbPathOverride;
    if (finalPath.isEmpty()) {
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (dataDir.isEmpty() || !QDir().mkpath(dataDir)) {
            m_databaseStats = QStringLiteral("History storage unavailable: no writable application data folder.");
            emit storageChanged();
            return;
        }
        finalPath = dataDir + QStringLiteral("/history.db");
    }
    m_databasePath = finalPath;

    auto db = std::make_unique<storage::Database>();
    const std::filesystem::path path = (finalPath == QLatin1String(":memory:"))
                                           ? std::filesystem::path(":memory:")
                                           : std::filesystem::path(finalPath.toStdWString());
    if (auto st = db->open(path); !st.ok()) {
        m_databaseStats = QStringLiteral("History storage unavailable: ") + qs(st.error().message);
        emit storageChanged();
        return;
    }
    auto repo = std::make_unique<storage::PromptRepository>(*db);
    if (auto st = repo->migrate(); !st.ok()) {
        m_databaseStats = QStringLiteral("History storage unavailable: ") + qs(st.error().message);
        emit storageChanged();
        return;
    }
    if (auto interrupted = repo->interruptOrphanedAttemptsOnStartup(nowMs()); !interrupted.ok()) {
        m_databaseStats = QStringLiteral("History storage unavailable: ") + qs(interrupted.error().message);
        emit storageChanged();
        return;
    }
    m_database = std::move(db);
    m_repository = std::move(repo);
    refreshStorageStats();
}

// ----------------------------------------------------------------------------------------------
// Errors and command plumbing
// ----------------------------------------------------------------------------------------------

void HostController::setLastError(const QString& message) {
    const auto code = message.isEmpty() ? QString() : QStringLiteral("INTERNAL");
    if (m_lastError != message || m_lastErrorCode != code) {
        m_lastError = message;
        m_lastErrorCode = code;
        emit lastErrorChanged();
    }
}

void HostController::setLastError(const core::Error& error) {
    const auto message = plainError(error);
    const auto code = QString::fromUtf8(core::toString(error.code));
    if (m_lastError != message || m_lastErrorCode != code) {
        m_lastError = message;
        m_lastErrorCode = code;
        emit lastErrorChanged();
    }
}

void HostController::clearLastError() {
    setLastError(QString());
}

template <typename Fn>
bool HostController::runCommand(const char* name, Fn&& fn) {
    try {
        const Status st = fn();
        if (!st.ok()) {
            setLastError(st.error());
            return false;
        }
        clearLastError();
        return true;
    } catch (const std::exception& ex) {
        setLastError(QStringLiteral("%1 failed unexpectedly: %2").arg(QLatin1String(name), QString::fromUtf8(ex.what())));
        return false;
    } catch (...) {
        setLastError(QStringLiteral("%1 failed unexpectedly.").arg(QLatin1String(name)));
        return false;
    }
}

void HostController::setShortcutRegistrationFailed(const QString& message) {
    if (m_shortcutWarning != message) {
        m_shortcutWarning = message;
        emit shortcutWarningChanged();
    }
}

// ----------------------------------------------------------------------------------------------
// Window state
// ----------------------------------------------------------------------------------------------

QString HostController::hostStateString() const {
    switch (m_hostState) {
        case HostState::Starting: return QStringLiteral("STARTING");
        case HostState::Resident: return QStringLiteral("RESIDENT");
        case HostState::Quitting: return QStringLiteral("QUITTING");
    }
    return QStringLiteral("UNKNOWN");
}

QString HostController::cardVisibilityString() const {
    switch (m_cardVisibility) {
        case CardVisibility::Hidden: return QStringLiteral("HIDDEN");
        case CardVisibility::Visible: return QStringLiteral("VISIBLE");
        case CardVisibility::Focused: return QStringLiteral("FOCUSED");
    }
    return QStringLiteral("UNKNOWN");
}

void HostController::markResident() {
    if (m_hostState != HostState::Starting) {
        return;
    }
    m_hostState = HostState::Resident;
    emit hostStateChanged();
}

void HostController::showCard() {
    const auto frontmost = HostContextDetector::captureFrontmost();
    if (m_selectingInsertionTarget && !isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
        m_selectingInsertionTarget = false;
        showCardInternal(false);
        return;
    }
    if (isAmbientActive() && isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
        beginCaptureInvocation(frontmost);
        return;
    }
    if (!isCardVisible() && isAmbientActive() && m_ambientObservation.has_value()) {
        openFromAmbient();
        return;
    }
    setCaptureSource(CaptureSource::Widget);
    m_captureRequestedApp.clear();
    showCardInternal(false);
}

void HostController::beginCaptureInvocation(const HostContextObservation& frontmost) {
    m_captureOpenPending = true;
    m_widgetDraftEdited = false; // This explicit invocation asks to adopt the newly focused draft.
    m_captureRequestedApp = frontmost.bundleOrExecutableId;
    m_ambientObservation.reset(); // A cached field may belong to the last control, not this invocation.
    const auto generation = ++m_captureOpenGeneration;
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint invocation] refresh=" << generation << "app=" << m_captureRequestedApp;
    m_accessibility->refreshFocusedField(generation);
    QTimer::singleShot(1200, this, [this, generation, frontmost]() {
        if (!m_captureOpenPending || generation != m_captureOpenGeneration) return;
        m_captureOpenPending = false;
        const auto current = HostContextDetector::captureFrontmost();
        const bool refreshed = m_ambientObservation &&
#ifdef Q_OS_WIN
            m_ambientObservation->field.refreshRequestId == generation;
#else
            true; // Platforms without a tagged refresh keep their passive-observation fallback.
#endif
        if (refreshed && current.bundleOrExecutableId.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0 &&
            m_ambientObservation->field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0) {
            openFromAmbient(); return;
        }
        setCaptureSource(CaptureSource::Widget);
        setLastError(QStringLiteral("No editable field was captured in %1. This HUD is in Copy mode. Focus the prompt box and invoke Roclint to bind insertion.").arg(frontmost.displayName));
        showCardInternal(false);
    });
}

void HostController::showCardInternal(bool captureClipboard) {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    m_hostContext = m_hostContextEnabled ? HostContextDetector::captureFrontmost() : HostContextObservation{};
    emit hostContextChanged();
    if (captureClipboard) {
        captureClipboardDraft();
    }
    setAmbientBadge(false);

    const auto placement = ScreenPlacement::determineTargetPosition(820, 520);
    setCardVisibility(CardVisibility::Focused);
    emit showCardRequested(placement.x, placement.y);
}

void HostController::hideCard() {
    m_selectingInsertionTarget = false;
    m_captureOpenPending = false;
    m_captureRequestedApp.clear();
    ++m_captureOpenGeneration;
    if (!setCardVisibility(CardVisibility::Hidden)) {
        return;
    }
    emit hideCardRequested();
}

void HostController::toggleCard() {
    const auto frontmost = HostContextDetector::captureFrontmost();
    if (m_selectingInsertionTarget && !isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
        m_selectingInsertionTarget = false;
        showCardInternal(false);
        return;
    }
    if (isAmbientActive() && isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
        beginCaptureInvocation(frontmost);
    } else if (isAmbientActive() && m_ambientObservation.has_value() &&
        frontmost.bundleOrExecutableId.compare(m_ambientObservation->field.appIdentifier, Qt::CaseInsensitive) == 0) {
        openFromAmbient();
    } else if (isCardVisible()) {
        hideCard();
    } else if (isAmbientActive() && m_ambientObservation.has_value()) {
        openFromAmbient();
    } else {
        showCard();
    }
}

void HostController::showDashboard() {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    refreshStorageStats();
    emit sessionListChanged();
    setDashboardVisibility(true);
    emit showDashboardRequested();
}

void HostController::hideDashboard() {
    if (!setDashboardVisibility(false)) {
        return;
    }
    emit hideDashboardRequested();
}

void HostController::toggleDashboard() {
    if (isDashboardVisible()) {
        hideDashboard();
    } else {
        showDashboard();
    }
}

void HostController::requestQuit() {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    hideCard();
    hideDashboard();
    m_hostState = HostState::Quitting;
    invalidateDownstreamPipeline();
    emit hostStateChanged();
    connect(m_julia.get(), &JuliaRuntime::stopped, this, &HostController::finishQuitWhenRuntimesStopped, Qt::QueuedConnection);
    connect(m_native.get(), &NativeGraniteRuntime::stopped, this, &HostController::finishQuitWhenRuntimesStopped, Qt::QueuedConnection);
    cancelModelDownload();
    m_julia->stop();
    m_native->stop();
    finishQuitWhenRuntimesStopped();
}

void HostController::finishQuitWhenRuntimesStopped() {
    if (m_hostState == HostState::Quitting && !m_quitSignalSent && !m_julia->isRunning() && !m_native->isRunning() && m_nativeSemanticInFlight == 0) {
        m_quitSignalSent = true;
        emit quitRequested();
    }
}

void HostController::syncCardVisibility(CardVisibility visibility) {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    setCardVisibility(visibility);
}

void HostController::syncDashboardVisibility(bool visible) {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    setDashboardVisibility(visible);
}

bool HostController::setCardVisibility(CardVisibility visibility) {
    if (m_cardVisibility == visibility) {
        return false;
    }
    m_cardVisibility = visibility;
    emit cardVisibilityChanged();
    return true;
}

bool HostController::setDashboardVisibility(bool visible) {
    if (m_dashboardVisible == visible) {
        return false;
    }
    m_dashboardVisible = visible;
    emit dashboardVisibilityChanged();
    return true;
}

// ----------------------------------------------------------------------------------------------
// Revision state
// ----------------------------------------------------------------------------------------------

quint64 HostController::stateVersion() const noexcept {
    return m_revisionState ? m_revisionState->stateVersion() : 0;
}

QString HostController::currentSessionIdString() const {
    return qs(currentSessionId());
}

QString HostController::currentRevisionId() const {
    return m_revisionState ? qs(m_revisionState->currentRevision().revisionId().value()) : QString();
}

QString HostController::currentRevisionText() const {
    return m_revisionState ? qs(m_revisionState->currentRevision().text()) : QString();
}

bool HostController::isDraftDirty() const noexcept {
    return m_revisionState && m_revisionState->isDraftDirty();
}

std::string HostController::currentSessionId() const {
    return m_revisionState ? m_revisionState->currentRevision().sessionId().value() : std::string();
}

void HostController::clearHostContext() {
    ++m_configurationGeneration;
    m_ambientCheck.reset();
    setAmbientBadge(false);
    if (m_hostContext.isValid()) {
        invalidateDownstreamPipeline();
    }
    m_hostContext = HostContextObservation{};
    emit hostContextChanged();
}

void HostController::setHostContextEnabled(bool enabled) {
    if (m_hostContextEnabled == enabled) return;
    m_hostContextEnabled = enabled;
    ++m_configurationGeneration;
    m_ambientCheck.reset();
    setAmbientBadge(false);
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("invocation/hostContextEnabled"), enabled);
    }
    if (!enabled) clearHostContext();
    emit hostContextEnabledChanged();
}

QString HostController::shortcutLabel() const {
    return QKeySequence::fromString(m_invocationShortcut, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

void HostController::setInvocationShortcut(const QString& sequence) {
    emit shortcutChangeRequested(sequence);
}

void HostController::acceptInvocationShortcut(const QString& sequence) {
    m_invocationShortcut = sequence;
    m_shortcutWarning.clear();
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("invocation/shortcut"), sequence);
    }
    emit shortcutChanged();
    emit shortcutWarningChanged();
    applyAmbientObserving();
}

void HostController::captureClipboardDraft() {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        return;
    }
    if (auto* cb = QGuiApplication::clipboard()) {
        const QString clip = qs(core::normalizeLineEndings(ss(cb->text())));
        m_capturedDraftText = clip;
        emit capturedDraftTextChanged();
        emit draftCaptured(clip);
    }
}

Status HostController::copyToClipboard(const QString& text) {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr || QGuiApplication::clipboard() == nullptr) {
        return makeError(ErrorCode::PreconditionFailed, "The system clipboard is not available");
    }
    QGuiApplication::clipboard()->setText(text);
    m_lastCopiedText = text;
    emit lastCopiedTextChanged();
    emit promptCopied(text);
    return Status::success();
}


Status HostController::ensureSession(const std::string& canonicalText, std::int64_t now) {
    if (m_revisionState) {
        return Status::success();
    }
    if (!m_repository) {
        return storageUnavailable();
    }
    const core::SessionId sessionId(newOpaqueId("sess_"));
    const core::PromptRevision root(sessionId, core::RevisionState::makeRevisionId(sessionId, 1), std::nullopt,
                                    canonicalText, now);
    if (auto st = m_repository->createSessionWithRootRevision(root, now); !st.ok()) {
        return st;
    }
    m_revisionState = std::make_unique<core::RevisionState>(root);
    emit stateVersionChanged();
    emit sessionListChanged();
    refreshStorageStats();
    return Status::success();
}

Status HostController::commitRevision(const std::string& canonicalText,
                                      std::uint64_t expectedVersion,
                                      storage::RevisionOrigin origin) {
    if (!m_revisionState) {
        return makeError(ErrorCode::PreconditionFailed, "No prompt session is open");
    }
    if (!m_repository) {
        return storageUnavailable();
    }
    const std::int64_t now = nowMs();
    core::RevisionState next = *m_revisionState;
    auto newId = next.commitEdit(expectedVersion, canonicalText, now);
    if (!newId.ok()) {
        return newId.error();
    }
    const storage::RevisionWrite write{
        .revision = next.currentRevision(),
        .sequence = next.highestIssuedSequence(),
        .expectedStateVersion = expectedVersion,
        .newStateVersion = next.stateVersion(),
        .draftEditGeneration = next.draftEditGeneration(),
        .proposalEditGeneration = next.proposalEditGeneration(),
        .nowMs = now,
    };
    Status st = Status::success();
    switch (origin) {
        case storage::RevisionOrigin::Commit: st = m_repository->insertRevision(write); break;
        case storage::RevisionOrigin::DraftFreeze: st = m_repository->insertDraftRevision(write); break;
        case storage::RevisionOrigin::Restore: st = m_repository->insertRestoredRevision(write); break;
        case storage::RevisionOrigin::Root:
            return makeError(ErrorCode::Internal, "Root revisions are created with the session");
    }
    if (!st.ok()) {
        return st;
    }
    *m_revisionState = std::move(next);
    emit stateVersionChanged();
    emit sessionListChanged();
    refreshStorageStats();
    return Status::success();
}

Status HostController::freezeDraftIfNeeded(const std::string& canonicalText, std::uint64_t expectedVersion) {
    if (m_revisionState->isStale(expectedVersion)) {
        return makeError(ErrorCode::StaleVersion,
                         "Expected state version " + std::to_string(expectedVersion) + " but current is " +
                             std::to_string(m_revisionState->stateVersion()));
    }
    if (!m_revisionState->isDraftDirty() && canonicalText == m_revisionState->currentRevision().text()) {
        return Status::success();
    }
    return commitRevision(canonicalText, expectedVersion, storage::RevisionOrigin::DraftFreeze);
}

bool HostController::noteDraftEdited(quint64 expectedVersion) {
    return runCommand("noteDraftEdited", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        // A HUD edit belongs to the user; later external typing must not overwrite it.
        m_followAmbientDraft = false;
        if (m_captureSource == CaptureSource::Widget || m_captureOpenPending) m_widgetDraftEdited = true;
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
            qInfo() << "[Roclint follow] HUD edit notification; following paused";
        if (!m_revisionState) return Status::success();
        auto next = *m_revisionState;
        if (auto st = next.noteDraftEdit(expectedVersion); !st.ok()) {
            return st;
        }
        if (auto status = persistStateChange(next, expectedVersion); !status.ok()) return status;
        invalidateDownstreamPipeline();
        emit stateVersionChanged();
        return Status::success();
    });
}

bool HostController::commitPrompt(const QString& text, quint64 expectedVersion) {
    return runCommand("commitPrompt", [&]() -> Status {
        const std::string canonical = core::normalizeLineEndings(ss(text));
        if (isBlank(canonical)) {
            return makeError(ErrorCode::InvalidInput, "Nothing to save: the prompt is empty.");
        }
        if (!m_revisionState) {
            if (expectedVersion != 0) {
                return makeError(ErrorCode::StaleVersion, "The session this edit belonged to is no longer open.");
            }
            if (auto st = ensureSession(canonical, nowMs()); !st.ok()) {
                return st;
            }
            invalidateDownstreamPipeline();
            return Status::success();
        }
        if (m_revisionState->isStale(expectedVersion)) {
            return makeError(ErrorCode::StaleVersion,
                             "Expected state version " + std::to_string(expectedVersion) + " but current is " +
                                 std::to_string(m_revisionState->stateVersion()));
        }
        if (!m_revisionState->isDraftDirty() && canonical == m_revisionState->currentRevision().text()) {
            return Status::success();
        }
        if (auto st = commitRevision(canonical, expectedVersion, storage::RevisionOrigin::Commit); !st.ok()) {
            return st;
        }
        invalidateDownstreamPipeline();
        return Status::success();
    });
}

bool HostController::hardBypass(const QString& text, quint64 expectedVersion) {
    if (!commitPrompt(text, expectedVersion)) {
        return false;
    }
    return runCommand("hardBypass", [&]() -> Status {
        // The raw text goes out as typed: any analysis or rewrite in flight is dropped unused.
        if (auto status = advanceCommandVersion(stateVersion()); !status.ok()) return status;
        invalidateDownstreamPipeline();
        emit stateVersionChanged();
        return deliver(currentRevisionText());
    });
}

// ----------------------------------------------------------------------------------------------
// Pipeline invalidation and tickets
// ----------------------------------------------------------------------------------------------

void HostController::invalidateDownstreamPipeline() {
    m_selectingInsertionTarget = false;
    ++m_pipelineSerial;
    m_promptTriageLoad.reset();
    m_promptTriage.reset();
    if (m_routedAnalysisGroup) m_routedAnalysisGroup->terminal = true;
    m_routedAnalysisGroup.reset();
    m_pendingNativeRewrite.reset();
    m_julia->cancelAllEvaluations();
    m_native->cancelAll();
    m_discoveryQueued = false;
    const bool hadApproval = m_currentApproval.has_value();

    m_currentAnalysis.reset();
    m_currentSnapshot.reset();
    m_currentPolicyResult = analysis::OverallPolicyResult{};
    m_currentSnapshotEval = bridge::SnapshotEvaluationResponse{};
    m_currentDecision = intent::InterventionDecision{};
    m_workingIntent = intent::WorkingIntentState{};
    m_activeQuestion.clear();
    m_activeDimension.clear();
    m_activeAlternatives.clear();
    m_activeScore.reset();
    m_confirmedIntent.reset();
    m_currentProposal.reset();
    m_acknowledgedConstraints.clear();
    m_currentApproval.reset();
    m_proposalStatus = ProposalUiStatus::None;
    m_proposalMessage.clear();
    m_analysisStatus = AnalysisStatus::Idle;
    m_analysisMessage.clear();

    if (hadApproval && m_repository && m_revisionState) {
        if (auto st = m_repository->invalidateApprovals(core::SessionId(currentSessionId())); !st.ok()) {
            setLastError(st.error());
        }
    }

    emit analysisStatusChanged();
    emit interventionChanged();
    emit intentChanged();
    emit rewriteProposalChanged();
    emit approvalChanged();
}

Status HostController::checkExpectedVersion(quint64 expectedVersion) const {
    if (expectedVersion != stateVersion()) return makeError(ErrorCode::StaleVersion, "The prompt state changed. Review the current state and try again.");
    return Status::success();
}

Status HostController::persistStateChange(const core::RevisionState& next, quint64 expectedVersion) {
    if (!m_repository) return storageUnavailable();
    if (auto status = m_repository->advanceSessionState(next, expectedVersion, nowMs()); !status.ok()) return status;
    *m_revisionState = next;
    m_currentApproval.reset();
    return Status::success();
}

Status HostController::advanceCommandVersion(quint64 expectedVersion) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
    if (!m_revisionState) return Status::success();
    auto next = *m_revisionState;
    if (auto status = next.advanceState(expectedVersion); !status.ok()) return status;
    return persistStateChange(next, expectedVersion);
}

Status HostController::advanceProposalGeneration() {
    if (!m_revisionState) return Status::success();
    auto next = *m_revisionState;
    if (auto status = next.noteProposalEdit(stateVersion()); !status.ok()) return status;
    return persistStateChange(next, stateVersion());
}

Status HostController::invalidateProposalArtifacts() {
    if (auto status = advanceProposalGeneration(); !status.ok()) return status;
    m_pendingNativeRewrite.reset();
    m_currentProposal.reset();
    m_acknowledgedConstraints.clear();
    m_currentApproval.reset();
    m_proposalStatus = ProposalUiStatus::None;
    m_proposalMessage.clear();
    emit rewriteProposalChanged();
    emit approvalChanged();
    emit intentChanged();
    emit stateVersionChanged();
    return Status::success();
}

void HostController::resetSessionState() {
    m_followAmbientDraft = false;
    m_widgetDraftEdited = false;
    invalidateDownstreamPipeline();
    m_revisionState.reset();
    emit stateVersionChanged();
}

HostController::PipelineTicket HostController::makeTicket() const {
    PipelineTicket t;
    t.sessionId = currentSessionId();
    t.revisionId = m_revisionState->currentRevision().revisionId().value();
    t.stateVersion = m_revisionState->stateVersion();
    t.draftEditGeneration = m_revisionState->draftEditGeneration();
    t.proposalEditGeneration = m_revisionState->proposalEditGeneration();
    t.serial = m_pipelineSerial;
    return t;
}

bool HostController::isTicketCurrent(const PipelineTicket& ticket, bool checkProposalGeneration) const {
    if (!m_revisionState) {
        return false;
    }
    return ticket.serial == m_pipelineSerial && ticket.sessionId == currentSessionId() &&
           ticket.revisionId == m_revisionState->currentRevision().revisionId().value() &&
           ticket.stateVersion == m_revisionState->stateVersion() &&
           ticket.draftEditGeneration == m_revisionState->draftEditGeneration() &&
           !m_revisionState->isDraftDirty() &&
           (!checkProposalGeneration || ticket.proposalEditGeneration == m_revisionState->proposalEditGeneration());
}

std::string HostController::currentTransportName() const {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (m_transportOverride) {
        return "test_override_transport";
    }
#endif
    return nativeGraniteSelected() ? "local_native_llama_cpp" : bridge::kLoopbackOllamaTransport;
}

bridge::GraniteTransport HostController::currentTransport(const std::string& outputSchema, const PipelineTicket* ticket,
    const QString& operation, const QString& prompt, const QString& intentId, const QString& profileId) const {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (m_transportOverride) {
        return m_transportOverride;
    }
#endif
    if (nativeGraniteSelected()) {
        if (!ticket || !m_native->isReady()) return {};
        const NativeGraniteRuntime::RequestContext context{qs(ticket->analysisGroup ? ticket->analysisGroup->id : ticket->attemptId), qs(ticket->attemptId), qs(ticket->sessionId),
            qs(ticket->revisionId), ticket->stateVersion, prompt, intentId, profileId};
        return m_native->transportFor(context, operation.isEmpty() ? QStringLiteral("granite_analyze") : operation, outputSchema);
    }
    if (!m_ollama || m_activeModel.isEmpty()) {
        return {};
    }
    return m_ollama->transportFor(m_activeModel, OllamaClient::kDefaultGenerateTimeoutMs, outputSchema);
}

Status HostController::beginAttempt(PipelineTicket& ticket, const char* stage) {
    if (!m_repository) {
        return storageUnavailable();
    }
    storage::ModelAttemptRecord record;
    record.attemptId = newOpaqueId("att_");
    record.sessionId = ticket.sessionId;
    record.revisionId = ticket.revisionId;
    record.stage = stage;
    record.status = storage::AttemptStatus::InProgress;
    record.transport = currentTransportName();
    record.modelId = m_graniteAdapter->modelInfo().modelId;
    ticket.model = m_graniteAdapter->modelInfo();
    ticket.configuration = std::string(stage) == "rewrite"
                               ? "rewrite_profile=" + ss(m_rewriteProfile)
                               : "host_context_category=" + ss(m_hostContext.categoryString());
    if (std::string(stage) == "julia_scoring" || std::string(stage) == "julia_triage") {
        record.transport = "local_python_" + ss(m_julia->executionDevice());
        ticket.model.modelId = JuliaRuntime::kModelId;
        ticket.model.digest = JuliaRuntime::kWeightsSha256;
        ticket.model.quantization = ss(m_julia->executionPrecision());
        ticket.model.transport = record.transport;
        record.modelId = ticket.model.modelId;
        ticket.configuration = std::string("revision=") + JuliaRuntime::kRevision + "; choice+noul; strict_encoding=true";
        if (std::string(stage) == "julia_triage")
            ticket.configuration = std::string("revision=") + JuliaRuntime::kRevision +
                "; template=" + analysis::kPromptTriageTemplateId + "; threshold=0.70; four_independent_noul_heads";
        ticket.configuration += "; execution_configuration=" + ss(m_julia->executionConfigurationId()) +
                                "; device=" + ss(m_julia->executionDevice()) + "; precision=" + ss(m_julia->executionPrecision()) +
                                "; actual_cpu_threads=" + std::to_string(m_julia->executionCpuThreads());
    }
#if defined(ROCLINT_ENABLE_TEST_MOCKS)
    if (std::string(stage) == "test_mock_scoring") {
        record.transport = "test_mock_in_process";
        ticket.model = bridge::GraniteModelInfo{"test_mock_scorer", "", "none", record.transport};
        record.modelId = ticket.model.modelId;
        ticket.configuration = "test_only=true";
    }
#endif
    if (nativeGraniteSelected() && !usesTransportOverride() && std::string(stage) != "julia_scoring" &&
        std::string(stage) != "julia_triage" && std::string(stage) != "test_mock_scoring")
        ticket.configuration += "; " + ss(m_native->configurationDetail()) + "; residency_policy=GraniteResidencyPolicyV1; residency_preset=" +
                                ss(m_graniteResidencyPreset) + "; residency_tier=" + ss(m_native->residency());
    if (ticket.analysisGroup)
        ticket.configuration += "; request_group_id=" + ticket.analysisGroup->id + "; analysis_group_budget_ms=" + std::to_string(ticket.analysisGroup->budgetMs);
    record.modelDetail = ticket.configuration;
    record.startedAtMs = nowMs();
    if (auto st = m_repository->beginModelAttempt(record); !st.ok()) {
        return st;
    }
    ticket.attemptId = record.attemptId;
    if (ticket.analysisGroup) {
        ticket.analysisGroup->activeAttemptId = ticket.attemptId;
        ticket.analysisGroup->activeConfiguration = ticket.configuration;
        ticket.analysisGroup->activeModel = ticket.model;
    }
    return Status::success();
}

void HostController::completeAttempt(const PipelineTicket& ticket,
                                     storage::AttemptStatus status,
                                     const std::string& errorMessage,
                                     const bridge::GraniteModelInfo* model) {
    if (ticket.attemptId.empty() || !m_repository) {
        return;
    }
    const bridge::GraniteModelInfo info = model ? *model : ticket.model;
    if (auto st = m_repository->completeModelAttempt(ticket.attemptId, status, info.modelId,
                                                     modelDetail(info) + "; " + ticket.configuration,
                                                     errorMessage, nowMs());
        !st.ok()) {
        setLastError(st.error());
    }
}

void HostController::setAnalysisStatus(AnalysisStatus status, const QString& message) {
    m_analysisStatus = status;
    m_analysisMessage = message;
    emit analysisStatusChanged();
}

void HostController::setProposalStatus(ProposalUiStatus status, const QString& message) {
    m_proposalStatus = status;
    m_proposalMessage = message;
    emit rewriteProposalChanged();
    emit intentChanged();
}

// ----------------------------------------------------------------------------------------------
// Analysis
// ----------------------------------------------------------------------------------------------

bool HostController::cancelAnalysis(quint64 expectedVersion) {
    return runCommand("cancelAnalysis", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!isAnalyzing()) return Status::success();
        if (auto status = advanceCommandVersion(expectedVersion); !status.ok()) return status;
        invalidateDownstreamPipeline();
        emit stateVersionChanged();
        setAnalysisStatus(AnalysisStatus::Idle, QStringLiteral("Analysis cancelled. Your draft remains available; choose Analyze to retry."));
        return Status::success();
    });
}

bool HostController::analyzePrompt(const QString& text, quint64 expectedVersion) {
    return runCommand("analyzePrompt", [&]() -> Status {
        const std::string canonical = core::normalizeLineEndings(ss(text));
        if (isBlank(canonical)) {
            return makeError(ErrorCode::InvalidInput, "Type a prompt before analyzing it.");
        }
        if (!m_revisionState) {
            if (expectedVersion != 0) {
                return makeError(ErrorCode::StaleVersion, "The session this edit belonged to is no longer open.");
            }
            if (auto st = ensureSession(canonical, nowMs()); !st.ok()) {
                return st;
            }
        } else if (auto st = freezeDraftIfNeeded(canonical, expectedVersion); !st.ok()) {
            return st;
        }
        if (auto status = advanceCommandVersion(stateVersion()); !status.ok()) return status;
        invalidateDownstreamPipeline();
        emit stateVersionChanged();
        return startDiscovery();
    });
}

bool HostController::usesTransportOverride() const noexcept {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    return static_cast<bool>(m_transportOverride);
#else
    return false;
#endif
}

Status HostController::startDiscovery(bool allowReconnect) {
    if (!m_revisionState) {
        return makeError(ErrorCode::PreconditionFailed, "No prompt session is open");
    }
    if (promptTriageRoutingEnabled() && !m_promptTriage) return startPromptTriage();
    if (promptTriageRoutingEnabled() && (!m_routedAnalysisGroup || m_routedAnalysisGroup->terminal ||
        analysis::routePromptTriage(*m_promptTriage).value() != analysis::PromptTriageRoute::Clarify))
        return makeError(ErrorCode::PreconditionFailed, "Granite discovery requires current ambiguous S1 classification");
    if (promptTriageRoutingEnabled() && !nativeGraniteSelected() && !usesTransportOverride()) {
        m_routedAnalysisGroup->terminal = true;
        const auto reason = QStringLiteral("This flow requires pinned IBM Granite 3B. Choose Native Granite in Settings.");
        setAnalysisStatus(AnalysisStatus::Failed, reason);
        return makeError(ErrorCode::ProviderUnavailable, ss(reason));
    }
    const auto runtime = m_graniteAdapter->state();
    if (runtime == bridge::GraniteRuntimeState::Busy) {
        if (m_routedAnalysisGroup) m_routedAnalysisGroup->pause();
        m_discoveryQueued = true;
        setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Waiting for the model to finish an earlier request."));
        return Status::success();
    }
    if (allowReconnect && nativeGraniteSelected() && !usesTransportOverride() &&
        (runtime != bridge::GraniteRuntimeState::Ready || !m_native->isReady())) {
        if (m_routedAnalysisGroup) m_routedAnalysisGroup->pause();
        m_discoveryQueued = true;
        setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Warming the pinned native model. Editing and the widget remain available."));
        prepareNativeModel();
        return Status::success();
    }
    if (allowReconnect && !nativeGraniteSelected() && m_ollama && !usesTransportOverride() && !m_activeModel.isEmpty() &&
        runtime != bridge::GraniteRuntimeState::Ready) {
        if (m_routedAnalysisGroup) m_routedAnalysisGroup->pause();
        m_discoveryQueued = true;
        setAnalysisStatus(AnalysisStatus::Analyzing,
                          QStringLiteral("Loading the local model. The first load can take up to a minute."));
        if (runtime != bridge::GraniteRuntimeState::Loading || !m_healthCheckInFlight) {
            refreshAvailableModels();
        }
        return Status::success();
    }
    m_discoveryQueued = false;

    PipelineTicket ticket = makeTicket();
    ticket.analysisGroup = m_routedAnalysisGroup ? m_routedAnalysisGroup : std::make_shared<AnalysisGroup>(m_analysisGroupTimeoutMs);
    if (ticket.analysisGroup->id.empty()) ticket.analysisGroup->id = newOpaqueId("grp_");
    ticket.analysisGroup->resume();
    if (auto st = beginAttempt(ticket, "discovery"); !st.ok()) {
        ticket.analysisGroup->terminal = true;
        setAnalysisStatus(AnalysisStatus::Failed, plainError(st.error()));
        return st;
    }

    const auto transport = currentTransport(bridge::discoveryResponseSchema(), &ticket, QStringLiteral("granite_analyze"), qs(m_revisionState->currentRevision().text()));
    if (runtime != bridge::GraniteRuntimeState::Ready || !transport) {
        std::string reason;
        if (!transport) {
            reason = "No local model runtime is configured.";
        } else if (m_activeModel.isEmpty()) {
            reason = "No model is selected. Choose one in Settings.";
        } else if (!m_graniteAdapter->lastError().empty()) {
            reason = m_graniteAdapter->lastError();
        } else {
            reason = "The local model isn't connected yet.";
        }
        completeAttempt(ticket, storage::AttemptStatus::Failed, reason);
        ticket.analysisGroup->terminal = true;
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis unavailable. ") + qs(reason));
        return makeError(ErrorCode::ProviderUnavailable, reason);
    }

    bridge::GraniteOptionDiscoveryRequest request;
    request.sessionId = ticket.sessionId;
    request.revisionId = ticket.revisionId;
    request.promptString = m_revisionState->currentRevision().text();
    request.hostContextCategory = ss(m_hostContext.categoryString());
    if (promptTriageRoutingEnabled()) request.promptTriage = m_promptTriage;

    setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Analyzing your prompt."));
    armAnalysisDeadline(ticket);
    if (ticket.analysisGroup->deadline.hasExpired()) { expireAnalysisGroup(ticket); return Status::success(); }
    emit modelEngineStatusChanged();

    using DiscoveryResult = core::Result<bridge::GraniteOptionDiscoveryResponse>;
    auto adapter = m_graniteAdapter;
    auto* watcher = new QFutureWatcher<DiscoveryResult>(this);
    ++m_inFlight;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    if (nativeWork) ++m_nativeSemanticInFlight;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, ticket, nativeWork]() {
        --m_inFlight;
        if (nativeWork) --m_nativeSemanticInFlight;
        const DiscoveryResult result = watcher->result();
        watcher->deleteLater();
        onDiscoveryFinished(ticket, result);
        finishQuitWhenRuntimesStopped();
    });
    watcher->setFuture(QtConcurrent::run([adapter, request, transport]() -> DiscoveryResult {
        try {
            return adapter->discoverOptions(request, transport);
        } catch (const std::exception& ex) {
            return makeError(ErrorCode::Internal, std::string("Discovery crashed: ") + ex.what());
        }
    }));
    return Status::success();
}

void HostController::onDiscoveryFinished(const PipelineTicket& ticket,
                                         const core::Result<bridge::GraniteOptionDiscoveryResponse>& result) {
    if (m_healthRecheckRequested && !m_healthCheckInFlight) refreshAvailableModels();
    emit modelEngineStatusChanged();
    if (m_ambientCheckPending) {
        m_ambientDebounce.start();
    }
    if (ticket.analysisGroup && ticket.analysisGroup->terminal) {
        if (m_discoveryQueued) {
            if (auto status = startDiscovery(); !status.ok()) setLastError(status.error());
        }
        return;
    }
    if (!isTicketCurrent(ticket, false)) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::DiscardedStale,
                        "The prompt changed before analysis finished");
        if (m_discoveryQueued) {
            if (auto st = startDiscovery(); !st.ok()) {
                setLastError(st.error());
            }
        }
        return;
    }

    if (ticket.analysisGroup && ticket.analysisGroup->deadline.hasExpired()) { expireAnalysisGroup(ticket); return; }

    if (!result.ok()) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::Failed, result.error().message);
        const auto error = modelReplyError(result.error());
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis failed. ") + plainError(error));
        setLastError(error);
        return;
    }
    acceptDiscovery(ticket, result.value());
}

void HostController::acceptDiscovery(const PipelineTicket& ticket, const bridge::GraniteOptionDiscoveryResponse& response) {
    const std::string& frozenText = m_revisionState->currentRevision().text();
    if (response.analysis.sessionId != ticket.sessionId || response.analysis.baseRevisionId != ticket.revisionId ||
        !analysis::isSnapshotSealed(response.analysis)) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Analysis is not bound to the frozen revision",
                        &response.model);
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis failed: the result did not match this prompt."));
        return;
    }
    const auto validation = analysis::validateNormalizedAnalysis(response.analysis, frozenText);
    if (validation.status == analysis::AnalysisValidationStatus::Invalid) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        std::string issues;
        for (const auto& issue : validation.issues) {
            issues += (issues.empty() ? "" : "; ") + issue;
        }
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Invalid analysis: " + issues, &response.model);
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis failed: the model's answer was inconsistent."));
        return;
    }
    completeAttempt(ticket, storage::AttemptStatus::Succeeded, std::string(), &response.model);

    if (promptTriageRoutingEnabled()) {
        ticket.analysisGroup->terminal = true;
        if (applyAnalysis(response.analysis, ticket.attemptId, {}, std::nullopt, m_promptTriage))
            setAnalysisStatus(AnalysisStatus::Ready, QStringLiteral("S1 score %1 ≥ 0.70 (%2). Granite supplied the decisions below; confirm your choice explicitly.")
                .arg(m_promptTriage->ambiguityScore, 0, 'f', 3)
                .arg(QString::fromLatin1(analysis::toString(m_promptTriage->primaryCategory))));
        return;
    }

    QString message;
    std::string scorerAttemptId;
    switch (response.outcome) {
        case bridge::DiscoveryOutcome::NoAmbiguity:
            message = std::any_of(response.analysis.assumptions.begin(), response.analysis.assumptions.end(), [](const auto& item) { return item.material; })
                          ? QStringLiteral("The analysis includes premises to review.") : QStringLiteral("No open questions found.");
            break;
        case bridge::DiscoveryOutcome::NeedsClarification: message = QStringLiteral("The model needs one more detail from you."); break;
        case bridge::DiscoveryOutcome::OptionsGenerated: message = QStringLiteral("Some wording could be read more than one way."); break;
    }
    if (response.ungroundedEvidenceCount > 0) {
        message += QStringLiteral(" %1 suggestion(s) were set aside because they quoted text that isn't in your prompt.")
                       .arg(response.ungroundedEvidenceCount);
    }
    const auto scoringPlan = analysis::buildScoringPlan(response.analysis);
    const bool hasScoringCandidates = !scoringPlan.groupIds.empty() || !scoringPlan.assumptionIds.empty();
    if (ticket.analysisGroup && ticket.analysisGroup->deadline.hasExpired()) {
        ticket.analysisGroup->terminal = true;
        auto evaluation = bridge::S1TriageAdapter::degradedResponse(response.analysis, ss(analysisDeadlineReason()));
        if (applyAnalysis(response.analysis, ticket.attemptId, {}, std::move(evaluation)))
            setAnalysisStatus(AnalysisStatus::Ready, message + QLatin1Char(' ') + analysisDeadlineReason());
        return;
    }
    if (m_s1Adapter.engine() == bridge::ScorerEngine::JuliaLocal && m_julia->isReady() && hasScoringCandidates) {
        auto scoreTicket = ticket;
        scoreTicket.attemptId.clear();
        if (auto st = beginAttempt(scoreTicket, "julia_scoring"); st.ok()) {
            scorerAttemptId = scoreTicket.attemptId;
            const JuliaRuntime::RequestContext context{qs(ticket.analysisGroup ? ticket.analysisGroup->id : ticket.attemptId), qs(scoreTicket.attemptId),
                qs(scoreTicket.sessionId), qs(scoreTicket.revisionId), scoreTicket.stateVersion};
            const auto requestId = m_julia->evaluate(response.analysis, qs(frozenText), context);
            if (requestId != 0) {
                m_pendingJulia.insert(requestId, PendingJuliaScore{scoreTicket, response.analysis, message, ticket.attemptId});
                ++m_inFlight;
                setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Julia is scoring the supplied choices and material assumptions locally."));
                return;
            }
            completeAttempt(scoreTicket, storage::AttemptStatus::Failed, "Julia could not accept this scoring request");
            message += QStringLiteral(" Julia could not score this request; options are unranked.");
        } else {
            setLastError(st.error());
            message += QStringLiteral(" Julia scoring could not be recorded; options are unranked.");
        }
    } else if (m_s1Adapter.engine() == bridge::ScorerEngine::JuliaLocal && hasScoringCandidates) {
        message += QStringLiteral(" Julia is unavailable; options are unranked. Load it in Settings.");
    }
    if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
    if (applyAnalysis(response.analysis, ticket.attemptId, scorerAttemptId)) setAnalysisStatus(AnalysisStatus::Ready, message);
}

void HostController::armAnalysisDeadline(const PipelineTicket& ticket) {
    if (!ticket.analysisGroup || ticket.analysisGroup->budgetMs <= 0 || ticket.analysisGroup->paused) return;
    const auto remaining = static_cast<int>(ticket.analysisGroup->deadline.remainingTime());
    QTimer::singleShot(std::max(0, remaining), Qt::PreciseTimer, this, [this, ticket]() { expireAnalysisGroup(ticket); });
}

void HostController::expireAnalysisGroup(const PipelineTicket& ticket) {
    if (!ticket.analysisGroup || ticket.analysisGroup->terminal || ticket.analysisGroup->paused || !isTicketCurrent(ticket, false)) return;
    if (!ticket.analysisGroup->deadline.hasExpired()) { armAnalysisDeadline(ticket); return; }
    ticket.analysisGroup->terminal = true;
    for (auto it = m_pendingPromptTriage.cbegin(); it != m_pendingPromptTriage.cend(); ++it) {
        if (it->analysisGroup == ticket.analysisGroup) {
            m_julia->cancel(it.key(), analysisDeadlineReason()); return;
        }
    }
    // Cancel emits a local terminal result synchronously. Julia's late reply is tombstoned.
    for (auto it = m_pendingJulia.cbegin(); it != m_pendingJulia.cend(); ++it) {
        if (it->ticket.analysisGroup == ticket.analysisGroup) {
            const auto requestId = it.key();
            m_julia->cancel(requestId, analysisDeadlineReason());
            return;
        }
    }
    auto active = ticket;
    active.attemptId = ticket.analysisGroup->activeAttemptId;
    active.configuration = ticket.analysisGroup->activeConfiguration;
    active.model = ticket.analysisGroup->activeModel;
    completeAttempt(active, storage::AttemptStatus::Failed, ss(analysisDeadlineReason()));
    setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis timed out. Retry explicitly or continue with your original prompt."));
    setLastError(makeError(ErrorCode::ProviderUnavailable, ss(analysisDeadlineReason())));
    if (nativeGraniteSelected() && !usesTransportOverride()) m_native->cancelAll();
}

void HostController::onJuliaFinished(quint64 requestId, const bridge::SnapshotEvaluationResponse* response, const QString& error) {
    const auto it = m_pendingJulia.find(requestId);
    if (it == m_pendingJulia.end()) return;
    auto pending = std::move(it.value());
    m_pendingJulia.erase(it);
    --m_inFlight;
    if (pending.ticket.analysisGroup) {
        pending.ticket.analysisGroup->terminal = true;
        if (pending.ticket.analysisGroup->deadline.hasExpired()) response = nullptr;
    }
    if (!isTicketCurrent(pending.ticket, false) || m_s1Adapter.engine() != bridge::ScorerEngine::JuliaLocal) {
        completeAttempt(pending.ticket, storage::AttemptStatus::DiscardedStale, "Prompt or scorer settings changed before Julia finished");
        return;
    }
    const auto failure = pending.ticket.analysisGroup && pending.ticket.analysisGroup->deadline.hasExpired() ? analysisDeadlineReason() : error;
    auto evaluation = response ? bridge::S1TriageAdapter::validateLocalEvaluation(pending.analysis, *response)
                               : bridge::S1TriageAdapter::degradedResponse(pending.analysis, ss(failure));
    evaluation.engine = bridge::ScorerEngine::JuliaLocal;
    const bool ranked = evaluation.isRanked();
    completeAttempt(pending.ticket, ranked ? storage::AttemptStatus::Succeeded : storage::AttemptStatus::Failed,
                    ranked ? std::string() : evaluation.degradedReason);
    const auto detail = ranked ? QStringLiteral(" Julia scored the supplied decisions locally. Confirm your interpretation explicitly.")
                               : QStringLiteral(" Julia scoring was unavailable; options remain unranked. ") + qs(evaluation.degradedReason);
    if (applyAnalysis(std::move(pending.analysis), std::move(pending.discoveryAttemptId), pending.ticket.attemptId, std::move(evaluation)))
        setAnalysisStatus(AnalysisStatus::Ready, pending.message + detail);
}

bool HostController::applyAnalysis(analysis::NormalizedAnalysis normalized, std::string discoveryAttemptId,
                                    std::string scorerAttemptId,
                                    std::optional<bridge::SnapshotEvaluationResponse> evaluation,
                                    std::optional<analysis::PromptTriage> triage) {
    auto scored = triage ? bridge::SnapshotEvaluationResponse{} :
        evaluation ? std::move(*evaluation) : m_s1Adapter.evaluateSnapshot(normalized);
#if defined(ROCLINT_ENABLE_TEST_MOCKS)
    if (scored.executionStatus == bridge::ExecutionStatus::ExecutedMock && scorerAttemptId.empty()) {
        auto ticket = makeTicket();
        if (auto status = beginAttempt(ticket, "test_mock_scoring"); !status.ok()) {
            setAnalysisStatus(AnalysisStatus::Failed, plainError(status.error()));
            setLastError(status.error());
            return false;
        }
        scorerAttemptId = ticket.attemptId;
        ticket.model.modelId = scored.modelVersion;
        completeAttempt(ticket, storage::AttemptStatus::Succeeded, "");
    }
#endif
    auto result = triage ? analysis::buildRoutedAnalysisSnapshot(std::move(normalized), std::move(*triage),
        m_revisionState->currentRevision().text(), std::move(discoveryAttemptId)) :
        analysis::buildAnalysisSnapshot(std::move(normalized), std::move(scored), m_revisionState->currentRevision().text(),
            std::move(discoveryAttemptId), std::move(scorerAttemptId));
    if (!result.ok()) {
        setAnalysisStatus(AnalysisStatus::Failed, plainError(result.error()));
        setLastError(result.error());
        return false;
    }
    auto nextState = *m_revisionState;
    const auto observedVersion = stateVersion();
    if (auto status = nextState.advanceState(observedVersion); !status.ok()) { setLastError(status.error()); return false; }
    if (auto status = m_repository->insertAnalysisSnapshot(result.value(), nowMs(), observedVersion); !status.ok()) {
        setAnalysisStatus(AnalysisStatus::Failed, plainError(status.error()));
        setLastError(status.error());
        return false;
    }
    *m_revisionState = std::move(nextState);
    m_currentSnapshot = std::move(result).value();
    m_currentAnalysis = m_currentSnapshot->normalized;
    m_currentSnapshotEval = m_currentSnapshot->evaluation;
    m_currentPolicyResult = analysis::MaterialityPolicyV1::evaluatePrompt(*m_currentAnalysis, m_revisionState->currentRevision().text());
    m_workingIntent = intent::WorkingIntentState{};
    m_confirmedIntent.reset();
    reevaluateIntervention();
    emit stateVersionChanged();
    return true;
}

void HostController::reevaluateIntervention() {
    if (!m_currentAnalysis) {
        m_currentDecision = intent::InterventionDecision{};
        refreshActiveItemPresentation();
        emit interventionChanged();
        emit intentChanged();
        return;
    }
    m_currentDecision = intent::InterventionStateMachine::evaluate(*m_currentAnalysis, m_currentPolicyResult,
                                                                   m_currentSnapshotEval, m_workingIntent);
    intent::InterventionStateMachine::recordPresented(m_currentDecision, m_workingIntent);
    refreshActiveItemPresentation();
    if (auto st = finalizeIntentIfQuiet(); !st.ok()) {
        setLastError(st.error());
    }
    emit interventionChanged();
    emit intentChanged();
}

void HostController::refreshActiveItemPresentation() {
    m_activeQuestion.clear();
    m_activeDimension.clear();
    m_activeAlternatives.clear();
    m_activeScore.reset();
    if (!m_currentAnalysis || !m_currentDecision.activeItemId.has_value()) {
        return;
    }
    const std::string& id = *m_currentDecision.activeItemId;
    const auto& a = *m_currentAnalysis;

    switch (m_currentDecision.activeItemKind) {
        case intent::ActiveItemKind::ChoiceGroup: {
            const auto it = std::find_if(a.choiceGroups.begin(), a.choiceGroups.end(),
                                         [&](const analysis::ChoiceGroup& g) { return g.id == id; });
            if (it == a.choiceGroups.end()) {
                return;
            }
            const bridge::GroupEvaluationScore* score = nullptr;
            if (m_currentSnapshotEval.isRanked() && m_currentSnapshotEval.analysisSnapshotId == a.id) {
                for (const auto& s : m_currentSnapshotEval.groupScores) {
                    if (s.groupId == id && s.isScored) {
                        score = &s;
                    }
                }
            }
            if (score != nullptr && score->ambiguityScore.has_value()) {
                m_activeScore = *score->ambiguityScore;
            }
            m_activeQuestion = qs(it->question);
            m_activeDimension = qs(it->dimension);
            for (const auto& alt : it->alternatives) {
                QVariantMap item;
                item[QStringLiteral("id")] = qs(alt.id);
                item[QStringLiteral("label")] = qs(alt.interpretation);
                item[QStringLiteral("whyMaterial")] = qs(alt.whyMaterial);
                const auto violation = bridge::explicitColorConstraintViolation(m_revisionState->currentRevision().text(), alt, it->question);
                item[QStringLiteral("permitted")] = !violation.has_value();
                item[QStringLiteral("constraintViolation")] = violation ? qs(*violation) : QString();
                QStringList quotes;
                for (const auto& span : alt.evidence) {
                    if (span.endByte <= m_revisionState->currentRevision().text().size()) {
                        quotes << qs(m_revisionState->currentRevision().text().substr(span.startByte, span.length()));
                    }
                }
                item[QStringLiteral("evidence")] = quotes;
                if (score != nullptr && score->probabilities.has_value()) {
                    for (const auto& p : *score->probabilities) {
                        if (p.alternativeId == alt.id) {
                            item[QStringLiteral("probability")] = p.probability;
                        }
                    }
                }
                m_activeAlternatives.append(item);
            }
            if (m_currentSnapshot) {
                for (const auto& presentation : m_currentSnapshot->presentation) {
                    if (presentation.groupId != id) continue;
                    auto position = [&](const QVariant& value) {
                        const auto key = ss(value.toMap().value(QStringLiteral("id")).toString());
                        return std::find(presentation.alternativeIds.begin(), presentation.alternativeIds.end(), key);
                    };
                    std::sort(m_activeAlternatives.begin(), m_activeAlternatives.end(),
                              [&](const auto& lhs, const auto& rhs) { return position(lhs) < position(rhs); });
                }
            }
            return;
        }
        case intent::ActiveItemKind::Assumption:
            for (const auto& s : a.assumptions) {
                if (s.id == id) {
                    m_activeQuestion = qs(s.statement);
                    m_activeDimension = QStringLiteral("Assumption");
                    if (m_currentSnapshotEval.isRanked()) for (const auto& score : m_currentSnapshotEval.assumptionScores) {
                        if (score.assumptionId == id && score.isScored) m_activeScore = score.uncertaintyScore;
                    }
                }
            }
            return;
        case intent::ActiveItemKind::MissingDecision:
            for (const auto& m : a.missingDecisions) {
                if (m.id == id) {
                    m_activeQuestion = qs(m.question);
                    m_activeDimension = QStringLiteral("Missing detail");
                }
            }
            return;
        case intent::ActiveItemKind::Conflict:
            for (const auto& c : a.conflicts) {
                if (c.id == id) {
                    m_activeQuestion = qs(c.description);
                    m_activeDimension = QStringLiteral("Conflict");
                }
            }
            return;
        case intent::ActiveItemKind::None:
            return;
    }
}

Status HostController::finalizeIntentIfQuiet() {
    if (m_currentDecision.verdict != intent::InterventionVerdict::Quiet || !m_currentAnalysis) {
        m_confirmedIntent.reset();
        return Status::success();
    }
    if (m_confirmedIntent) {
        return Status::success();
    }
    if (!m_repository) {
        return storageUnavailable();
    }
    const std::int64_t now = nowMs();
    auto built = intent::IntentProvenanceBuilder::buildConfirmedIntent(*m_currentSnapshot, m_currentPolicyResult,
                                                                       m_workingIntent, now);
    if (!built.ok()) {
        return built.error();
    }
    if (auto st = m_repository->insertIntentVersion(built.value(), now); !st.ok()) {
        return st;
    }
    m_confirmedIntent = std::move(built).value();
    return Status::success();
}

bool HostController::applyTransition(const char* name, quint64 expectedVersion,
                                     const std::function<Status(intent::WorkingIntentState&)>& transition) {
    return runCommand(name, [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!m_currentAnalysis || !m_revisionState) {
            return makeError(ErrorCode::PreconditionFailed, "There's nothing to answer yet. Analyze the prompt first.");
        }
        if (m_revisionState->isDraftDirty()) {
            return makeError(ErrorCode::StaleVersion, "The prompt changed after it was analyzed. Analyze it again.");
        }
        intent::WorkingIntentState next = m_workingIntent;
        if (auto st = transition(next); !st.ok()) {
            return st;
        }
        auto decision = intent::InterventionStateMachine::evaluate(*m_currentAnalysis, m_currentPolicyResult, m_currentSnapshotEval, next);
        intent::InterventionStateMachine::recordPresented(decision, next);
        auto nextState = *m_revisionState;
        const bool clearProposal = m_currentProposal.has_value() || m_proposalStatus == ProposalUiStatus::Generating;
        const auto advanced = clearProposal ? nextState.noteProposalEdit(expectedVersion) : nextState.advanceState(expectedVersion);
        if (!advanced.ok()) return advanced;
        std::optional<intent::ConfirmedIntent> confirmed;
        if (decision.verdict == intent::InterventionVerdict::Quiet) {
            auto result = intent::IntentProvenanceBuilder::buildConfirmedIntent(*m_currentSnapshot, m_currentPolicyResult, next, nowMs());
            if (!result.ok()) return result.error();
            confirmed = std::move(result).value();
            if (auto status = m_repository->insertIntentVersion(*confirmed, nowMs(), expectedVersion); !status.ok()) return status;
        } else {
            if (auto status = m_repository->advanceSessionState(nextState, expectedVersion, nowMs()); !status.ok()) return status;
        }
        *m_revisionState = std::move(nextState);
        m_workingIntent = std::move(next);
        m_followAmbientDraft = false; // An explicit decision freezes the reviewed draft and destination.
        m_currentDecision = std::move(decision);
        m_confirmedIntent = std::move(confirmed);
        m_currentApproval.reset();
        if (clearProposal) {
            m_currentProposal.reset();
            m_acknowledgedConstraints.clear();
            m_proposalStatus = ProposalUiStatus::None;
            m_proposalMessage.clear();
            emit rewriteProposalChanged();
        }
        refreshActiveItemPresentation();
        emit interventionChanged();
        emit intentChanged();
        emit stateVersionChanged();
        emit approvalChanged();
        return Status::success();
    });
}

bool HostController::confirmChoice(const QString& alternativeId, quint64 expectedVersion) {
    const std::string alt = ss(alternativeId);
    return applyTransition("confirmChoice", expectedVersion, [&](intent::WorkingIntentState& state) {
        for (const auto& group : m_currentAnalysis->choiceGroups) {
            for (const auto& alternative : group.alternatives) {
                if (alternative.id != alt) continue;
                if (const auto violation = bridge::explicitColorConstraintViolation(m_revisionState->currentRevision().text(), alternative, group.question))
                    return Status(makeError(ErrorCode::ValidationFailed, *violation));
            }
        }
        return intent::InterventionStateMachine::applyChoice(*m_currentAnalysis, m_currentDecision, state, alt);
    });
}

bool HostController::submitCustomIntent(const QString& customText, quint64 expectedVersion) {
    const std::string text = core::normalizeLineEndings(ss(customText));
    return applyTransition("submitCustomIntent", expectedVersion, [&](intent::WorkingIntentState& state) {
        return intent::InterventionStateMachine::applyCustomText(*m_currentAnalysis, m_currentDecision, state, text);
    });
}

bool HostController::keepOriginalPrompt(quint64 expectedVersion) {
    return applyTransition("keepOriginalPrompt", expectedVersion, [&](intent::WorkingIntentState& state) {
        return intent::InterventionStateMachine::applyKeepOriginal(*m_currentAnalysis, m_currentDecision, state);
    });
}

bool HostController::resolveActiveAssumption(bool accepted, quint64 expectedVersion) {
    return applyTransition("resolveActiveAssumption", expectedVersion, [&](intent::WorkingIntentState& state) {
        return intent::InterventionStateMachine::applyAssumptionDecision(*m_currentAnalysis, m_currentDecision, state,
                                                                         accepted);
    });
}

bool HostController::skipCurrentItem(quint64 expectedVersion) {
    return applyTransition("skipCurrentItem", expectedVersion, [&](intent::WorkingIntentState& state) {
        return intent::InterventionStateMachine::applySkip(*m_currentAnalysis, m_currentDecision, state);
    });
}

bool HostController::switchToManual(quint64 expectedVersion) {
    return applyTransition("switchToManual", expectedVersion, [](intent::WorkingIntentState& state) {
        intent::InterventionStateMachine::applyManualRequest(state);
        return Status::success();
    });
}

// ----------------------------------------------------------------------------------------------
// Intervention presentation
// ----------------------------------------------------------------------------------------------

QVariantList HostController::assumptionReviews() const {
    QVariantList result;
    if (!m_currentAnalysis) return result;
    for (const auto index : analysis::orderedAssumptionIndices(*m_currentAnalysis, m_currentSnapshotEval)) {
        const auto& assumption = m_currentAnalysis->assumptions[index];
        if (!assumption.material) continue;
        QVariantMap item;
        item[QStringLiteral("id")] = qs(assumption.id);
        item[QStringLiteral("statement")] = qs(assumption.statement);
        QString state = QStringLiteral("Needs confirmation");
        if (const auto decision = m_workingIntent.assumptionDecisions.find(assumption.id); decision != m_workingIntent.assumptionDecisions.end())
            state = decision->second ? QStringLiteral("Confirmed") : QStringLiteral("Rejected");
        else if (m_workingIntent.customResolutions.contains(assumption.id)) state = QStringLiteral("User correction");
        QString scoring = m_currentSnapshotEval.planVersion == 1 ? QStringLiteral("Unscored: historical group-only analysis") : QStringLiteral("Unscored");
        for (const auto& score : m_currentSnapshotEval.assumptionScores) if (score.assumptionId == assumption.id) {
            if (score.isScored && score.uncertaintyScore) scoring = QStringLiteral("Estimated uncertainty %1").arg(*score.uncertaintyScore, 0, 'f', 2);
            else if (score.unscoredReason == "batch_limit") scoring = QStringLiteral("Unscored: four-assumption batch limit");
        }
        item[QStringLiteral("state")] = state;
        item[QStringLiteral("scoring")] = scoring;
        result.append(item);
    }
    return result;
}

QString HostController::analysisStatus() const {
    switch (m_analysisStatus) {
        case AnalysisStatus::Idle: return QStringLiteral("IDLE");
        case AnalysisStatus::Analyzing: return QStringLiteral("ANALYZING");
        case AnalysisStatus::Ready: return QStringLiteral("READY");
        case AnalysisStatus::Failed: return QStringLiteral("FAILED");
    }
    return QStringLiteral("IDLE");
}

QString HostController::interventionVerdict() const {
    return m_currentAnalysis ? QString::fromUtf8(intent::toString(m_currentDecision.verdict)) : QString();
}

QString HostController::interventionReason() const {
    return m_currentAnalysis ? QString::fromUtf8(intent::toString(m_currentDecision.reason)) : QString();
}

bool HostController::isClarifying() const noexcept {
    return m_currentAnalysis.has_value() && m_currentDecision.verdict == intent::InterventionVerdict::Clarify;
}

bool HostController::isManual() const noexcept {
    return m_currentAnalysis.has_value() && m_currentDecision.verdict == intent::InterventionVerdict::Manual;
}

QString HostController::activeItemId() const {
    return m_currentDecision.activeItemId.has_value() ? qs(*m_currentDecision.activeItemId) : QString();
}

QString HostController::activeItemKind() const {
    return QString::fromUtf8(intent::toString(m_currentDecision.activeItemKind));
}

int HostController::roundsCompleted() const noexcept {
    return static_cast<int>(m_workingIntent.roundsCompleted);
}

int HostController::remainingBudget() const noexcept {
    return m_currentAnalysis ? static_cast<int>(m_currentDecision.remainingBudget)
                             : static_cast<int>(intent::InterventionStateMachine::kMaxClarificationBudget);
}

QString HostController::rankingStatus() const {
    if (!m_currentAnalysis) {
        return QString();
    }
    return m_activeScore.has_value() ? QStringLiteral("RANKED") : QStringLiteral("UNRANKED");
}

QString HostController::diagnosticMessage() const {
    return qs(m_currentDecision.diagnosticMessage);
}

// ----------------------------------------------------------------------------------------------
// Intent
// ----------------------------------------------------------------------------------------------

bool HostController::isIntentStale() const {
    if (!m_confirmedIntent) {
        return false;
    }
    if (!m_revisionState || !m_currentAnalysis) {
        return true;
    }
    return !intent::IntentProvenanceBuilder::isIntentValid(*m_confirmedIntent,
                                                           m_revisionState->currentRevision().revisionId().value(),
                                                           m_revisionState->status(), *m_currentSnapshot)
                .ok();
}

QString HostController::confirmedIntentSummary() const {
    if (!m_confirmedIntent) {
        return QStringLiteral("No decisions recorded yet.");
    }
    if (isIntentStale()) {
        return QStringLiteral("Your decisions are out of date because the prompt changed.");
    }
    QString summary = QStringLiteral("%1 decision(s) recorded").arg(m_confirmedIntent->items.size());
    if (!m_confirmedIntent->unresolvedItemIds.empty()) {
        summary += QStringLiteral(", %1 left open").arg(m_confirmedIntent->unresolvedItemIds.size());
    }
    return summary + QStringLiteral(".");
}

QVariantList HostController::confirmedItems() const {
    QVariantList list;
    if (!m_confirmedIntent) {
        return list;
    }
    for (const auto& item : m_confirmedIntent->items) {
        QVariantMap map;
        map[QStringLiteral("itemId")] = qs(item.itemId);
        map[QStringLiteral("kind")] = QString::fromUtf8(intent::toString(item.kind));
        map[QStringLiteral("chosenValue")] = qs(item.chosenValue);
        map[QStringLiteral("customText")] = qs(item.customText);
        map[QStringLiteral("rationale")] = qs(item.rationale);
        list.append(map);
    }
    return list;
}

QStringList HostController::unresolvedItems() const {
    QStringList list;
    if (!m_confirmedIntent) {
        return list;
    }
    for (const auto& id : m_confirmedIntent->unresolvedItemIds) {
        list << qs(id);
    }
    return list;
}

QVariantList HostController::availableRewriteProfiles() const {
    QVariantList list;
    for (const auto& p : rewrite::ProfileRegistry::availableProfiles()) {
        QVariantMap map;
        map[QStringLiteral("id")] = qs(p.id);
        map[QStringLiteral("displayName")] = qs(p.displayName);
        list.append(map);
    }
    return list;
}

bool HostController::setRewriteProfile(const QString& profile, quint64 expectedVersion) {
    return runCommand("setRewriteProfile", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!rewrite::ProfileRegistry::isKnownProfile(ss(profile))) {
            return makeError(ErrorCode::InvalidInput, "Unknown rewrite style: " + ss(profile));
        }
        if (m_rewriteProfile == profile) {
            return Status::success();
        }
        if (auto status = invalidateProposalArtifacts(); !status.ok()) return status;
        m_rewriteProfile = qs(rewrite::ProfileRegistry::getProfile(ss(profile))->id);
        if (!QCoreApplication::organizationName().isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("rewrite/profile"), m_rewriteProfile);
        }
        emit rewriteProfileChanged(m_rewriteProfile);
        return Status::success();
    });
}

bool HostController::isRewriteEnabled() const {
    if (promptTriageRoutingEnabled() && (!m_currentSnapshot || !m_currentSnapshot->promptTriage ||
        analysis::routePromptTriage(*m_currentSnapshot->promptTriage).value() != analysis::PromptTriageRoute::Clarify ||
        (m_workingIntent.confirmedAlternatives.empty() && m_workingIntent.customResolutions.empty() &&
         m_workingIntent.assumptionDecisions.empty()))) return false;
    return m_confirmedIntent.has_value() && !isIntentStale() &&
           m_currentDecision.verdict == intent::InterventionVerdict::Quiet &&
           m_proposalStatus != ProposalUiStatus::Generating &&
           (m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready ||
            (promptTriageRoutingEnabled() && nativeGraniteSelected() && !usesTransportOverride() &&
             m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy && m_hostState != HostState::Quitting));
}

QVariantMap HostController::getConfirmedIntentPayload() const {
    QVariantMap payload;
    if (!m_confirmedIntent) {
        return payload;
    }
    payload[QStringLiteral("intentId")] = qs(m_confirmedIntent->id);
    payload[QStringLiteral("intentHash")] = qs(m_confirmedIntent->intentHash);
    payload[QStringLiteral("baseRevisionId")] = qs(m_confirmedIntent->baseRevisionId);
    payload[QStringLiteral("snapshotId")] = qs(m_confirmedIntent->snapshotId);
    payload[QStringLiteral("snapshotHash")] = qs(m_confirmedIntent->snapshotHash);
    payload[QStringLiteral("isCurrent")] = !isIntentStale();
    payload[QStringLiteral("rewriteProfile")] = m_rewriteProfile;
    payload[QStringLiteral("items")] = confirmedItems();
    payload[QStringLiteral("unresolvedItemIds")] = unresolvedItems();
    return payload;
}

// ----------------------------------------------------------------------------------------------
// Rewrite proposal and approval
// ----------------------------------------------------------------------------------------------

bool HostController::synthesizeRewrite(quint64 expectedVersion) {
    return runCommand("synthesizeRewrite", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (promptTriageRoutingEnabled() && (!m_currentSnapshot || !m_currentSnapshot->promptTriage ||
            analysis::routePromptTriage(*m_currentSnapshot->promptTriage).value() != analysis::PromptTriageRoute::Clarify ||
            (m_workingIntent.confirmedAlternatives.empty() && m_workingIntent.customResolutions.empty() &&
             m_workingIntent.assumptionDecisions.empty())))
            return makeError(ErrorCode::PreconditionFailed, "Granite rewrite requires ambiguous S1 classification and an explicit confirmed choice. Clear prompts keep their original wording.");
        if (promptTriageRoutingEnabled() && !nativeGraniteSelected() && !usesTransportOverride())
            return makeError(ErrorCode::ProviderUnavailable, "Only the pinned native IBM Granite model can generate this rewrite.");
        if (!m_revisionState) {
            return makeError(ErrorCode::PreconditionFailed, "Write and analyze a prompt first.");
        }
        if (m_revisionState->isDraftDirty()) {
            return makeError(ErrorCode::PreconditionFailed,
                             "The prompt has edits that haven't been analyzed. Analyze it again before rewriting.");
        }
        if (!m_currentAnalysis) {
            return makeError(ErrorCode::PreconditionFailed, "Analyze the prompt before asking for a rewrite.");
        }
        if (!m_confirmedIntent) {
            return makeError(ErrorCode::PreconditionFailed,
                             "Answer the open questions (or switch to manual) before asking for a rewrite.");
        }
        for (const auto& group : m_currentAnalysis->choiceGroups) {
            const auto selected = m_workingIntent.confirmedAlternatives.find(group.id);
            if (selected == m_workingIntent.confirmedAlternatives.end()) continue;
            for (const auto& alternative : group.alternatives) {
                if (alternative.id != selected->second) continue;
                if (const auto violation = bridge::explicitColorConstraintViolation(m_revisionState->currentRevision().text(), alternative, group.question))
                    return makeError(ErrorCode::ValidationFailed, *violation);
            }
        }
        if (auto st = intent::IntentProvenanceBuilder::isIntentValid(
                *m_confirmedIntent, m_revisionState->currentRevision().revisionId().value(), m_revisionState->status(),
                *m_currentSnapshot);
            !st.ok()) {
            return st;
        }
        if (m_proposalStatus == ProposalUiStatus::Generating) {
            return makeError(ErrorCode::PreconditionFailed, "A rewrite is already being written.");
        }
        const auto profile = rewrite::ProfileRegistry::getProfile(ss(m_rewriteProfile));
        if (!profile.has_value()) {
            return makeError(ErrorCode::InvalidInput, "Unknown rewrite style: " + ss(m_rewriteProfile));
        }
        const auto runtime = m_graniteAdapter->state();
        if (nativeGraniteSelected() && !usesTransportOverride() &&
            (runtime != bridge::GraniteRuntimeState::Ready || !m_native->isReady())) {
            m_pendingNativeRewrite = PendingNativeRewrite{expectedVersion, m_confirmedIntent->id, m_rewriteProfile, m_configurationGeneration};
            setProposalStatus(ProposalUiStatus::Generating, QStringLiteral("Warming the pinned native model before the requested rewrite."));
            prepareNativeModel();
            return Status::success();
        }
        if (runtime != bridge::GraniteRuntimeState::Ready || (!nativeGraniteSelected() && !currentTransport(bridge::rewriteResponseSchema()))) {
            return makeError(ErrorCode::ProviderUnavailable, ss(modelEngineStatus()));
        }

        if (auto status = invalidateProposalArtifacts(); !status.ok()) return status;
        PipelineTicket ticket = makeTicket();
        if (auto st = beginAttempt(ticket, "rewrite"); !st.ok()) {
            return st;
        }

        bridge::GraniteRewriteRequest request;
        request.sessionId = ticket.sessionId;
        request.revisionId = ticket.revisionId;
        request.originalText = m_revisionState->currentRevision().text();
        request.analysis = *m_currentSnapshot;
        request.confirmedIntent = *m_confirmedIntent;
        request.rewriteProfileId = profile->id;
        request.proposalEditGeneration = ticket.proposalEditGeneration + 1;
        const auto transport = currentTransport(bridge::rewriteResponseSchema(), &ticket, QStringLiteral("granite_rewrite"),
            qs(request.originalText), qs(request.confirmedIntent.id), m_rewriteProfile);
        if (!transport) {
            completeAttempt(ticket, storage::AttemptStatus::Failed, "Native rewrite transport is unavailable");
            return makeError(ErrorCode::ProviderUnavailable, "The local rewrite runtime is not ready.");
        }

        setProposalStatus(ProposalUiStatus::Generating, QStringLiteral("Writing a suggested rewrite."));
        emit modelEngineStatusChanged();

        using RewriteResult = core::Result<rewrite::RewriteProposal>;
        auto adapter = m_graniteAdapter;
        auto* watcher = new QFutureWatcher<RewriteResult>(this);
        ++m_inFlight;
        const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
        if (nativeWork) ++m_nativeSemanticInFlight;
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, ticket, nativeWork]() {
            --m_inFlight;
            if (nativeWork) --m_nativeSemanticInFlight;
            const RewriteResult result = watcher->result();
            watcher->deleteLater();
            onRewriteFinished(ticket, result);
            finishQuitWhenRuntimesStopped();
        });
        const std::int64_t startedAt = nowMs();
        watcher->setFuture(QtConcurrent::run([adapter, request, transport, startedAt]() -> RewriteResult {
            try {
                return bridge::GraniteRewriteAdapter::synthesizeRewrite(request, *adapter, transport, startedAt);
            } catch (const std::exception& ex) {
                return makeError(ErrorCode::Internal, std::string("Rewrite crashed: ") + ex.what());
            }
        }));
        return Status::success();
    });
}

void HostController::onRewriteFinished(const PipelineTicket& ticket,
                                       const core::Result<rewrite::RewriteProposal>& result) {
    if (m_healthRecheckRequested && !m_healthCheckInFlight) refreshAvailableModels();
    emit modelEngineStatusChanged();
    if (m_ambientCheckPending) {
        m_ambientDebounce.start();
    }
    if (!isTicketCurrent(ticket, true) || !m_confirmedIntent) {
        completeAttempt(ticket, storage::AttemptStatus::DiscardedStale, "The prompt or settings changed before the rewrite finished");
        if (m_discoveryQueued) {
            if (auto st = startDiscovery(); !st.ok()) {
                setLastError(st.error());
            }
        }
        return;
    }
    if (!result.ok()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, result.error().message);
        const auto error = modelReplyError(result.error());
        setProposalStatus(ProposalUiStatus::Failed, QStringLiteral("The rewrite couldn't be produced. ") + plainError(error));
        setLastError(error);
        return;
    }

    const rewrite::RewriteProposal& proposal = result.value();
    bridge::GraniteModelInfo model;
    model.modelId = proposal.modelId;
    model.digest = proposal.modelDigest;
    model.quantization = proposal.quantization;
    model.transport = proposal.transport;

    auto nextState = *m_revisionState;
    if (auto status = nextState.noteProposalEdit(ticket.stateVersion); !status.ok()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, status.error().message, &model);
        setProposalStatus(ProposalUiStatus::Failed, plainError(status.error()));
        return;
    }
    if (proposal.proposalEditGeneration != nextState.proposalEditGeneration()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Proposal generation mismatch", &model);
        setProposalStatus(ProposalUiStatus::Failed, QStringLiteral("The rewrite was out of date and was discarded."));
        return;
    }
    auto persistProposal = [&]() -> Status {
        if (auto status = m_repository->insertProposal(proposal, nowMs(), ticket.stateVersion); !status.ok()) return status;
        *m_revisionState = nextState;
        m_currentApproval.reset();
        return Status::success();
    };

    if (proposal.status == rewrite::ProposalStatus::NeedsClarification) {
        if (auto st = persistProposal(); !st.ok()) {
            completeAttempt(ticket, storage::AttemptStatus::Failed, st.error().message, &model);
            setProposalStatus(ProposalUiStatus::Failed, plainError(st.error()));
            return;
        }
        completeAttempt(ticket, storage::AttemptStatus::Succeeded, std::string(), &model);
        m_currentProposal = proposal;
        setProposalStatus(ProposalUiStatus::NeedsClarification, qs(proposal.clarificationQuestion));
        emit stateVersionChanged();
        return;
    }

    const auto validation = rewrite::RewriteValidator::validateProposal(
        proposal, ticket.sessionId, ticket.revisionId, *m_confirmedIntent, std::set<std::string>{});
    if (!validation.structurallyValid) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Validation failed: " + validation.rejectionReason, &model);
        setProposalStatus(ProposalUiStatus::ValidationFailed,
                          QStringLiteral("The suggested rewrite failed checks: ") + qs(validation.rejectionReason));
        return;
    }
    if (auto st = persistProposal(); !st.ok()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, st.error().message, &model);
        setProposalStatus(ProposalUiStatus::Failed, plainError(st.error()));
        setLastError(st.error());
        return;
    }
    completeAttempt(ticket, storage::AttemptStatus::Succeeded, std::string(), &model);

    m_currentProposal = proposal;
    m_acknowledgedConstraints.clear();
    emit stateVersionChanged();
    const auto pending = validation.pendingAcknowledgments.size();
    setProposalStatus(ProposalUiStatus::ReadyForReview,
                      pending == 0 ? QStringLiteral("Review the suggested wording, then choose Insert or Copy.")
                                   : QStringLiteral("Check the %1 highlighted item(s), then approve the wording.").arg(pending));
}

rewrite::ValidationResult HostController::validateCurrentProposal() const {
    if (!m_currentProposal || !m_confirmedIntent || !m_revisionState) {
        return rewrite::ValidationResult{};
    }
    return rewrite::RewriteValidator::validateProposal(*m_currentProposal, currentSessionId(),
                                                       m_revisionState->currentRevision().revisionId().value(),
                                                       *m_confirmedIntent, m_acknowledgedConstraints);
}

std::optional<review::ApprovalContext> HostController::approvalContext() const {
    if (!m_revisionState || !m_currentAnalysis || !m_confirmedIntent || !m_currentProposal) {
        return std::nullopt;
    }
    review::ApprovalContext ctx;
    ctx.sessionId = currentSessionId();
    ctx.revisionId = m_revisionState->currentRevision().revisionId().value();
    ctx.stateVersion = m_revisionState->stateVersion();
    ctx.draftEditGeneration = m_revisionState->draftEditGeneration();
    ctx.proposalEditGeneration = m_revisionState->proposalEditGeneration();
    ctx.revisionStatus = m_revisionState->status();
    ctx.analysis = &*m_currentSnapshot;
    ctx.intent = &*m_confirmedIntent;
    ctx.proposal = &*m_currentProposal;
    ctx.validatorVersion = rewrite::kValidatorVersion;
    ctx.policyVersion = analysis::MaterialityPolicyV1::kPolicyVersion;
    return ctx;
}

bool HostController::acknowledgeConstraint(const QString& constraintId, bool acknowledged, quint64 expectedVersion) {
    return runCommand("acknowledgeConstraint", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (m_proposalStatus != ProposalUiStatus::ReadyForReview || !m_currentProposal) {
            return makeError(ErrorCode::PreconditionFailed, "There is no suggested rewrite to review.");
        }
        const std::string id = ss(constraintId);
        const auto constraint = m_currentProposal->getConstraint(id);
        if (!constraint.has_value() || constraint->status != rewrite::ConstraintStatus::SemanticReviewRequired) {
            return makeError(ErrorCode::InvalidInput, "That item doesn't need your review.");
        }
        if ((m_acknowledgedConstraints.count(id) > 0) == acknowledged) return Status::success();
        if (auto status = advanceCommandVersion(expectedVersion); !status.ok()) return status;
        if (acknowledged) {
            m_acknowledgedConstraints.insert(id);
        } else {
            m_acknowledgedConstraints.erase(id);
        }
        emit rewriteProposalChanged();
        emit stateVersionChanged();
        emit approvalChanged();
        return Status::success();
    });
}

bool HostController::beginInsertionTargetSelection(quint64 expectedVersion) {
    return runCommand("beginInsertionTargetSelection", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!isAmbientActive())
            return makeError(ErrorCode::PreconditionFailed, "Enable Live prompt capture in Settings to connect an insertion field.");
        if (!m_revisionState || !m_currentProposal || m_proposalStatus != ProposalUiStatus::ReadyForReview)
            return makeError(ErrorCode::PreconditionFailed, "Finish generating the rewrite before connecting its destination.");
        if (m_pendingDelivery)
            return makeError(ErrorCode::PreconditionFailed, "An insertion is already in progress.");
        hideCard();
        m_selectingInsertionTarget = true;
        const auto message = QStringLiteral("Your rewrite and answers are kept. Click the destination prompt box, then press %1. Roclint will reopen the same review with Approve & Insert.").arg(shortcutLabel());
        setAmbientStatus(message);
        emit insertionTargetSelectionRequested(message);
        return Status::success();
    });
}

bool HostController::approveAndCopyRewrite(quint64 expectedVersion) {
    return runCommand("approveAndCopyRewrite", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (m_proposalStatus != ProposalUiStatus::ReadyForReview || !m_currentProposal) {
            return makeError(ErrorCode::PreconditionFailed, "There is no suggested rewrite to approve.");
        }
        if (!m_repository) {
            return storageUnavailable();
        }
        const auto validation = validateCurrentProposal();
        if (!validation.structurallyValid) {
            setProposalStatus(ProposalUiStatus::ValidationFailed,
                              QStringLiteral("The suggested rewrite failed checks: ") + qs(validation.rejectionReason));
            return makeError(ErrorCode::ValidationFailed, validation.rejectionReason);
        }
        if (!validation.isValid) {
            return makeError(ErrorCode::PreconditionFailed,
                             "Check the " + std::to_string(validation.pendingAcknowledgments.size()) +
                                 " highlighted item(s) before approving.");
        }
        auto ctx = approvalContext();
        if (!ctx.has_value()) {
            return makeError(ErrorCode::PreconditionFailed, "The rewrite is no longer bound to the current prompt.");
        }
        auto nextState = *m_revisionState;
        if (auto status = nextState.advanceState(expectedVersion); !status.ok()) return status;
        ctx->stateVersion = nextState.stateVersion();
        auto approval = review::ApprovalFactory::createApproval(*ctx, nowMs());
        if (!approval.ok()) {
            return approval.error();
        }
        if (!review::ApprovalFactory::isApprovalValid(approval.value(), *ctx)) {
            return makeError(ErrorCode::Internal, "The approval record failed its own validation");
        }
        if (auto st = m_repository->insertApproval(approval.value(), expectedVersion); !st.ok()) {
            return st;
        }
        *m_revisionState = nextState;
        m_currentApproval = approval.value();
        emit stateVersionChanged();
        emit approvalChanged();
        return deliver(qs(m_currentApproval->approvedText));
    });
}

Status HostController::deliver(const QString& text) {
    if (m_pendingDelivery.has_value()) {
        return makeError(ErrorCode::PreconditionFailed, "An insertion is already in progress.");
    }
    const bool inject = m_captureSource == CaptureSource::Ambient && m_captureTarget.has_value() &&
                        m_accessibility->access() == AccessibilityAccess::Granted;
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint delivery] route=" << (inject ? "insert" : "clipboard")
                << "token=" << (m_captureTarget ? m_captureTarget->field.fieldToken : 0);
    if (!inject) {
        auto st = copyToClipboard(text);
        if (st.ok()) {
            setAnalysisStatus(m_analysisStatus, QStringLiteral("Copied to clipboard. No editor is bound, so nothing was inserted. Paste it yourself, or invoke Roclint from your prompt box."));
        }
        return st;
    }
    const auto& target = *m_captureTarget;
    InjectionRequest request;
    request.requestId = ++m_nextDeliveryId;
    request.fieldToken = target.field.fieldToken;
    request.kind = target.field.kind;
    request.text = text;
    request.expectedCurrentText = target.field.kind == FieldKind::Terminal
                                      ? TerminalAdapter::extractDraft(target.field.text, target.field.caretOffset)
                                            .value_or(TerminalDraft{})
                                            .text
                                      : target.field.text;
    m_pendingDelivery = PendingDelivery{request.requestId, text, target.field.appDisplayName};
    // The native bridge restores and verifies target focus. Keep the draft visible until readback
    // confirms delivery; a clipboard fallback is not a successful insertion.
    m_accessibility->inject(request);
    return Status::success();
}

void HostController::onInjectionFinished(quint64 requestId, InjectionOutcome outcome, const QString& detail) {
    if (!m_pendingDelivery.has_value() || m_pendingDelivery->requestId != requestId) {
        return;
    }
    const PendingDelivery delivery = *m_pendingDelivery;
    m_pendingDelivery.reset();
    if (outcome == InjectionOutcome::Injected) {
        clearLastError();
        m_lastCopiedText = delivery.text;
        emit lastCopiedTextChanged();
        setAmbientStatus(QStringLiteral("Inserted into %1.").arg(delivery.appName));
        hideCard();
        emit promptInjected(delivery.text, delivery.appName);
        return;
    }
    const auto copied = copyToClipboard(delivery.text);
    const QString reason = detail.isEmpty() ? QString::fromUtf8(toString(outcome)) : detail;
    const QString message =
        copied.ok() ? QStringLiteral("Couldn't insert into %1 (%2). The text is on your clipboard: press %3 to paste it.")
                          .arg(delivery.appName, reason,
#if defined(Q_OS_MAC)
                               QStringLiteral("Cmd+V"))
#else
                               QStringLiteral("Ctrl+V"))
#endif
                    : QStringLiteral("Couldn't insert into %1 (%2), and the clipboard isn't available either.")
                          .arg(delivery.appName, reason);
    setLastError(message);
    setAmbientStatus(message);
    showCardInternal(false);
    emit deliveryFellBack(message);
}

bool HostController::discardProposal(quint64 expectedVersion) {
    return runCommand("discardProposal", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!m_currentProposal && m_proposalStatus == ProposalUiStatus::None) {
            return Status::success();
        }
        return invalidateProposalArtifacts();
    });
}

QString HostController::proposalStatus() const {
    switch (m_proposalStatus) {
        case ProposalUiStatus::None: return QStringLiteral("NONE");
        case ProposalUiStatus::Generating: return QStringLiteral("GENERATING");
        case ProposalUiStatus::ReadyForReview: return QStringLiteral("READY_FOR_REVIEW");
        case ProposalUiStatus::NeedsClarification: return QStringLiteral("NEEDS_CLARIFICATION");
        case ProposalUiStatus::ValidationFailed: return QStringLiteral("VALIDATION_FAILED");
        case ProposalUiStatus::Failed: return QStringLiteral("FAILED");
    }
    return QStringLiteral("NONE");
}

bool HostController::hasProposal() const noexcept {
    return m_currentProposal.has_value() && m_proposalStatus == ProposalUiStatus::ReadyForReview;
}

QString HostController::proposalText() const {
    return hasProposal() ? qs(m_currentProposal->rewrittenText) : QString();
}

QString HostController::lastDiffText() const {
    return hasProposal() ? qs(m_currentProposal->diffText) : QString();
}

QVariantList HostController::proposalConstraints() const {
    QVariantList list;
    if (!hasProposal()) {
        return list;
    }
    for (const auto& c : m_currentProposal->constraints) {
        QVariantMap map;
        map[QStringLiteral("id")] = qs(c.id);
        map[QStringLiteral("description")] = qs(c.description);
        map[QStringLiteral("status")] = QString::fromUtf8(rewrite::toString(c.status));
        map[QStringLiteral("evidence")] = qs(c.evidence);
        map[QStringLiteral("needsReview")] = c.status == rewrite::ConstraintStatus::SemanticReviewRequired;
        map[QStringLiteral("acknowledged")] = m_acknowledgedConstraints.count(c.id) > 0;
        list.append(map);
    }
    return list;
}

int HostController::pendingReviewCount() const {
    return hasProposal() ? static_cast<int>(validateCurrentProposal().pendingAcknowledgments.size()) : 0;
}

bool HostController::canApprove() const {
    return hasProposal() && validateCurrentProposal().isValid;
}

QString HostController::approvalLabel() const {
    if (!m_currentApproval) {
        return QString();
    }
    const auto ctx = approvalContext();
    if (!ctx.has_value() || !review::ApprovalFactory::isApprovalValid(*m_currentApproval, *ctx)) {
        return QString();
    }
    return QStringLiteral("Wording reviewed");
}

// ----------------------------------------------------------------------------------------------
// History
// ----------------------------------------------------------------------------------------------

int HostController::sessionCount() const {
    if (!m_repository) {
        return 0;
    }
    auto sessions = m_repository->listSessions();
    return sessions.ok() ? static_cast<int>(sessions.value().size()) : 0;
}

void HostController::refreshStorageStats() {
    if (!m_repository) {
        if (m_databaseStats.isEmpty()) {
            m_databaseStats = QStringLiteral("History storage is not connected.");
        }
        emit storageChanged();
        return;
    }
    auto sessions = m_repository->listSessions();
    if (!sessions.ok()) {
        m_databaseStats = QStringLiteral("History storage error: ") + qs(sessions.error().message);
        emit storageChanged();
        return;
    }
    std::size_t revisions = 0;
    for (const auto& s : sessions.value()) {
        auto history = m_repository->getHistory(s.id);
        if (!history.ok()) {
            m_databaseStats = QStringLiteral("History storage error: ") + qs(history.error().message);
            emit storageChanged();
            return;
        }
        revisions += history.value().size();
    }
    auto schema = m_repository->getSchemaVersion();
    m_databaseStats = QStringLiteral("%1 saved session(s), %2 revision(s), schema version %3")
                          .arg(sessions.value().size())
                          .arg(revisions)
                          .arg(schema.ok() ? schema.value() : 0);
    emit storageChanged();
}

QVariantList HostController::getRecentSessions() {
    QVariantList result;
    if (!m_repository) {
        return result;
    }
    auto sessions = m_repository->listSessions();
    if (!sessions.ok()) {
        setLastError(sessions.error());
        return result;
    }
    for (const auto& s : sessions.value()) {
        QVariantMap item;
        item[QStringLiteral("sessionId")] = qs(s.id.value());
        item[QStringLiteral("createdAtMs")] = static_cast<qlonglong>(s.createdAtMs);
        item[QStringLiteral("formattedTime")] =
            QDateTime::fromMSecsSinceEpoch(s.createdAtMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        auto history = m_repository->getHistory(s.id);
        item[QStringLiteral("revisionCount")] = history.ok() ? static_cast<int>(history.value().size()) : 0;
        auto latest = m_repository->getLatestRevision(s.id);
        if (latest.ok() && latest.value().has_value()) {
            item[QStringLiteral("latestPrompt")] = qs(latest.value()->revision.text());
            item[QStringLiteral("latestRevisionId")] = qs(latest.value()->revision.revisionId().value());
        } else {
            item[QStringLiteral("latestPrompt")] = QString();
            item[QStringLiteral("latestRevisionId")] = QString();
        }
        result.append(item);
    }
    return result;
}

QVariantList HostController::getSessionHistory(const QString& sessionId) {
    QVariantList result;
    if (!m_repository || sessionId.trimmed().isEmpty()) {
        return result;
    }
    auto history = m_repository->getHistory(core::SessionId(ss(sessionId)));
    if (!history.ok()) {
        setLastError(history.error());
        return result;
    }
    for (const auto& rec : history.value()) {
        const auto& rev = rec.revision;
        QVariantMap item;
        item[QStringLiteral("revisionId")] = qs(rev.revisionId().value());
        item[QStringLiteral("sessionId")] = qs(rev.sessionId().value());
        item[QStringLiteral("parentRevisionId")] =
            rev.parentRevisionId().has_value() ? qs(rev.parentRevisionId()->value()) : QString();
        item[QStringLiteral("text")] = qs(rev.text());
        item[QStringLiteral("sequence")] = static_cast<qulonglong>(rec.sequence);
        item[QStringLiteral("origin")] = QString::fromUtf8(storage::toString(rec.origin));
        item[QStringLiteral("createdAtMs")] = static_cast<qlonglong>(rev.createdAtMs());
        item[QStringLiteral("formattedTime")] =
            QDateTime::fromMSecsSinceEpoch(rev.createdAtMs()).toString(QStringLiteral("HH:mm:ss"));
        result.append(item);
    }
    return result;
}

bool HostController::restoreRevision(const QString& revisionId, quint64 expectedVersion) {
    return runCommand("restoreRevision", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!m_revisionState) {
            return makeError(ErrorCode::PreconditionFailed, "Open a session before restoring one of its revisions.");
        }
        if (!m_repository) {
            return storageUnavailable();
        }
        auto rec = m_repository->getRevision(core::SessionId(currentSessionId()), core::RevisionId(ss(revisionId)));
        if (!rec.ok()) {
            return rec.error();
        }
        if (!rec.value().has_value()) {
            return makeError(ErrorCode::NotFound, "That revision isn't part of the open session.");
        }
        const std::string text = rec.value()->revision.text();
        if (auto st = commitRevision(text, expectedVersion, storage::RevisionOrigin::Restore); !st.ok()) {
            return st;
        }
        m_followAmbientDraft = false;
        m_captureRequestedApp.clear();
        invalidateDownstreamPipeline();
        emit revisionRestored(qs(text));
        return Status::success();
    });
}

bool HostController::restoreSessionForReview(const QString& sessionId, quint64 expectedVersion) {
    if (!restoreSession(sessionId, expectedVersion)) return false;
    // Open saved work directly; a normal capture hotkey would adopt a different editor draft.
    setCaptureSource(CaptureSource::Widget);
    showCardInternal(false);
    return true;
}

bool HostController::restoreSession(const QString& sessionId, quint64 expectedVersion) {
    return runCommand("restoreSession", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!m_repository) {
            return storageUnavailable();
        }
        auto restored = m_repository->restoreSession(core::SessionId(ss(sessionId)));
        if (!restored.ok()) {
            return restored.error();
        }
        auto& r = restored.value();

        // Switching sessions does not invalidate the saved approval; the restored copy is re-validated below.
        m_followAmbientDraft = false;
        m_captureRequestedApp.clear();
        m_currentApproval.reset();
        invalidateDownstreamPipeline();
        m_revisionState = std::make_unique<core::RevisionState>(
            r.latestRevision.revision, r.session.highestRevisionSeq, r.session.stateVersion,
            r.session.draftEditGeneration, r.session.proposalEditGeneration);
        emit stateVersionChanged();
        emit revisionRestored(qs(r.latestRevision.revision.text()));

        rebuildFromRestore(r);
        return Status::success();
    });
}

void HostController::rebuildFromRestore(const storage::RestoredSession& r) {
    if (!r.analysis.has_value()) {
        if (r.legacyAnalysisRequiresReview) setAnalysisStatus(AnalysisStatus::Idle,
            QStringLiteral("This history predates saved scorer evidence. Analyze and review it again before rewriting."));
        return;
    }
    const std::string& text = m_revisionState->currentRevision().text();
    if (analysis::validateNormalizedAnalysis(r.analysis->normalized, text).status == analysis::AnalysisValidationStatus::Invalid) {
        setAnalysisStatus(AnalysisStatus::Idle, QStringLiteral("The saved analysis no longer matches this prompt. Analyze it again."));
        return;
    }
    m_currentPolicyResult = analysis::MaterialityPolicyV1::evaluatePrompt(r.analysis->normalized, text);
    m_currentSnapshot = *r.analysis;
    m_currentSnapshotEval = r.analysis->evaluation;
    m_currentAnalysis = r.analysis->normalized;

    intent::WorkingIntentState working;
    if (r.intent.has_value()) {
        const auto& in = *r.intent;
        for (const auto& item : in.items) {
            switch (item.kind) {
                case intent::IntentResolutionKind::SelectedAlternative:
                    working.confirmedAlternatives[item.itemId] = item.chosenAlternativeId;
                    break;
                case intent::IntentResolutionKind::CustomText:
                    working.customResolutions[item.itemId] = item.customText;
                    break;
                case intent::IntentResolutionKind::UseOriginal:
                    working.keptOriginalItemIds.push_back(item.itemId);
                    break;
                case intent::IntentResolutionKind::AcceptedAssumption:
                    working.assumptionDecisions[item.itemId] = true;
                    break;
                case intent::IntentResolutionKind::DeclinedAssumption:
                    working.assumptionDecisions[item.itemId] = false;
                    break;
            }
        }
        for (const auto& id : in.unresolvedItemIds) {
            if (std::find(in.visibleItemIds.begin(), in.visibleItemIds.end(), id) != in.visibleItemIds.end()) {
                working.skippedItemIds.push_back(id);
            }
        }
        working.presentedItemIds = in.visibleItemIds;
        working.roundsCompleted = std::min(in.items.size() + working.skippedItemIds.size(),
                                           intent::InterventionStateMachine::kMaxClarificationBudget);
    }

    m_workingIntent = working;
    m_currentDecision = intent::InterventionStateMachine::evaluate(*m_currentAnalysis, m_currentPolicyResult,
                                                                   m_currentSnapshotEval, m_workingIntent);
    bool intentRestored = false;
    if (r.intent.has_value() && m_currentDecision.verdict == intent::InterventionVerdict::Quiet) {
        auto rebuilt = intent::IntentProvenanceBuilder::buildConfirmedIntent(*m_currentSnapshot, m_currentPolicyResult,
                                                                             m_workingIntent, r.intent->createdAtMs);
        if (rebuilt.ok() && rebuilt.value().intentHash == r.intent->intentHash &&
            intent::IntentProvenanceBuilder::isIntentValid(*r.intent,
                                                           m_revisionState->currentRevision().revisionId().value(),
                                                           m_revisionState->status(), *m_currentSnapshot)
                .ok()) {
            m_confirmedIntent = *r.intent;
            intentRestored = true;
        }
    }
    if (!intentRestored) {
        m_workingIntent = intent::WorkingIntentState{};
        m_confirmedIntent.reset();
        m_currentDecision = intent::InterventionStateMachine::evaluate(*m_currentAnalysis, m_currentPolicyResult,
                                                                       m_currentSnapshotEval, m_workingIntent);
    }
    intent::InterventionStateMachine::recordPresented(m_currentDecision, m_workingIntent);
    refreshActiveItemPresentation();
    if (!intentRestored) {
        if (auto st = finalizeIntentIfQuiet(); !st.ok()) {
            setLastError(st.error());
        }
    }
    setAnalysisStatus(AnalysisStatus::Ready, QStringLiteral("Restored the saved analysis for this prompt."));
    emit interventionChanged();
    emit intentChanged();

    if (!intentRestored || !r.proposal.has_value() ||
        r.proposal->proposalEditGeneration != m_revisionState->proposalEditGeneration()) {
        return;
    }
    const auto& proposal = *r.proposal;
    if (proposal.status == rewrite::ProposalStatus::NeedsClarification) {
        m_currentProposal = proposal;
        setProposalStatus(ProposalUiStatus::NeedsClarification, qs(proposal.clarificationQuestion));
        return;
    }
    const auto validation = rewrite::RewriteValidator::validateProposal(
        proposal, currentSessionId(), m_revisionState->currentRevision().revisionId().value(), *m_confirmedIntent, {});
    if (!validation.structurallyValid) {
        return;
    }
    m_currentProposal = proposal;
    if (r.approval.has_value()) {
        const auto ctx = approvalContext();
        if (ctx.has_value() && review::ApprovalFactory::isApprovalValid(*r.approval, *ctx)) {
            m_currentApproval = *r.approval;
            m_acknowledgedConstraints.insert(validation.pendingAcknowledgments.begin(),
                                             validation.pendingAcknowledgments.end());
        } else if (auto st = m_repository->invalidateApprovals(core::SessionId(currentSessionId())); !st.ok()) {
            setLastError(st.error());
        }
    }
    setProposalStatus(ProposalUiStatus::ReadyForReview, QStringLiteral("Restored the saved rewrite for review."));
    emit approvalChanged();
}

bool HostController::deleteSession(const QString& sessionId) {
    return runCommand("deleteSession", [&]() -> Status {
        if (!m_repository) {
            return storageUnavailable();
        }
        const std::string id = ss(sessionId);
        if (auto st = m_repository->deleteSession(core::SessionId(id)); !st.ok()) {
            return st;
        }
        if (id == currentSessionId()) {
            resetSessionState();
        }
        refreshStorageStats();
        emit sessionListChanged();
        return Status::success();
    });
}

bool HostController::purgeHistory() {
    return runCommand("purgeHistory", [&]() -> Status {
        if (!m_repository) {
            return storageUnavailable();
        }
        if (auto st = m_repository->purgeHistory(); !st.ok()) {
            return st;
        }
        resetSessionState();
        refreshStorageStats();
        emit sessionListChanged();
        return Status::success();
    });
}

// ----------------------------------------------------------------------------------------------
// Scorer
// ----------------------------------------------------------------------------------------------

QString HostController::scorerEngine() const {
    return QString::fromUtf8(bridge::toString(m_s1Adapter.engine()).data(),
                             static_cast<qsizetype>(bridge::toString(m_s1Adapter.engine()).size()));
}

QString HostController::scorerStatus() const {
    if (promptTriageRoutingEnabled()) {
        if (m_s1Adapter.engine() != bridge::ScorerEngine::JuliaLocal)
            return QStringLiteral("S1 classification unavailable: select local Julia for the required model flow.");
        return m_julia->isReady() && m_julia->supportsPromptTriage()
            ? QStringLiteral("Julia-1 is ready for S1 classification before Granite. Below 0.70 preserves the original; otherwise review Granite choices.")
            : QStringLiteral("S1 classification unavailable until Julia is loaded. Missing scores never count as clear.");
    }
    if (m_s1Adapter.engine() == bridge::ScorerEngine::JuliaLocal) {
        return m_julia->isReady() ? QStringLiteral("Julia-1: local CPU ranking is ready. Scores are produced only when choices are analyzed.")
                                 : QStringLiteral("Julia ranking unavailable: options stay unranked until the local model is loaded.");
    }
    return m_s1Adapter.availability() == bridge::SemanticAvailability::FullRankedCloud
               ? QStringLiteral("Options are ranked by the Jev cloud scorer.")
               : QStringLiteral("Ranking unavailable: options are shown in the order they appear in your prompt.");
}

bool HostController::setScorerEngine(const QString& engine) {
    return runCommand("setScorerEngine", [&]() -> Status {
        auto parsed = bridge::S1TriageAdapter::parseEngine(ss(engine));
        if (!parsed.ok()) {
            return parsed.error();
        }
        if (m_s1Adapter.engine() != parsed.value()) {
            invalidateScorerConfiguration();
            m_s1Adapter.setEngine(parsed.value());
            if (parsed.value() != bridge::ScorerEngine::JuliaLocal) m_julia->stop();
            if (!QCoreApplication::organizationName().isEmpty()) {
                QSettings settings;
                settings.setValue(QStringLiteral("scoring/engine"), scorerEngine());
            }
            emit scorerEngineChanged();
        }
        return Status::success();
    });
}

void HostController::invalidateScorerConfiguration() {
    ++m_configurationGeneration;
    m_ambientCheck.reset();
    m_ambientDismissed.clear();
    setAmbientBadge(false);
    invalidateDownstreamPipeline();
}

void HostController::refreshJuliaModel() {
    if (m_s1Adapter.engine() != bridge::ScorerEngine::JuliaLocal) return;
    invalidateScorerConfiguration();
    m_julia->load(m_juliaModelDirectory, m_juliaPython);
}

void HostController::downloadJuliaModel() {
    invalidateScorerConfiguration();
    if (m_juliaModelDirectory.isEmpty()) {
        m_juliaModelDirectory = m_modelsBaseDirectory + QStringLiteral("/s1");
    }
    QDir().mkpath(m_juliaModelDirectory);
    m_julia->load(m_juliaModelDirectory, m_juliaPython, true);
}

void HostController::chooseJuliaModelDirectory() {
    const auto directory = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Choose Julia-1 model folder"), m_juliaModelDirectory);
    if (directory.isEmpty()) return;
    QString resolved = directory;
    const QDir d(directory);
    if (!d.exists(QStringLiteral("model.safetensors")) && !d.exists(QStringLiteral("roclint-julia.json"))) {
        if (QDir(d.filePath(QStringLiteral("s1"))).exists(QStringLiteral("model.safetensors")) ||
            QDir(d.filePath(QStringLiteral("s1"))).exists(QStringLiteral("roclint-julia.json"))) {
            resolved = d.filePath(QStringLiteral("s1"));
        }
    }
    m_juliaModelDirectory = QDir::cleanPath(resolved);
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("scoring/juliaDirectory"), m_juliaModelDirectory);
    }
    refreshJuliaModel();
    emit scorerEngineChanged();
}

void HostController::chooseJuliaPython() {
    const auto python = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Choose the Julia Python executable"), m_juliaPython);
    if (python.isEmpty()) return;
    m_juliaPython = python;
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("scoring/juliaPython"), python);
    }
    refreshJuliaModel();
    emit scorerEngineChanged();
}

// ----------------------------------------------------------------------------------------------
// Model runtime
// ----------------------------------------------------------------------------------------------

void HostController::setModelRuntimeSource(bool native) {
    if (native == nativeGraniteSelected()) return;
    QString selected;
    if (native) selected = NativeGraniteRuntime::modelTag(QStringLiteral("Q8_0"));
    else if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        selected = settings.value(QStringLiteral("models/ollamaSelection")).toString();
    }
    setActiveModel(selected);
}

void HostController::warmSelectedModel() {
    if (nativeGraniteSelected() && !usesTransportOverride()) prepareNativeModel();
    else refreshAvailableModels();
}

void HostController::chooseModelsBaseDirectory() {
    const auto directory = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Choose models base folder"), m_modelsBaseDirectory);
    if (directory.isEmpty()) return;
    m_modelsBaseDirectory = QDir::cleanPath(directory);
    QDir().mkpath(m_modelsBaseDirectory + QStringLiteral("/s1"));
    QDir().mkpath(m_modelsBaseDirectory + QStringLiteral("/s2"));
    m_juliaModelDirectory = m_modelsBaseDirectory + QStringLiteral("/s1");
    m_nativeDirectory = m_modelsBaseDirectory + QStringLiteral("/s2");
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("models/baseDirectory"), m_modelsBaseDirectory);
        settings.setValue(QStringLiteral("scoring/juliaDirectory"), m_juliaModelDirectory);
        settings.setValue(QStringLiteral("models/nativeDirectory"), m_nativeDirectory);
    }
    emit modelsBaseDirectoryChanged();
    emit scorerEngineChanged();
    invalidateDownstreamPipeline();
    m_native->stop();
    refreshJuliaModel();
    static_cast<void>(m_graniteAdapter->unload());
    refreshAvailableModels();
    emit modelEngineStatusChanged();
}

void HostController::chooseNativeModelDirectory() {
    const auto directory = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Choose pinned native Granite model folder"), m_nativeDirectory);
    if (directory.isEmpty()) return;
    QString resolved = directory;
    auto hasGraniteCandidate = [](const QString& path) {
        const QDir dir(path);
        for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
            if (dir.exists(entry.toObject().value(QStringLiteral("filename")).toString())) return true;
        }
        return false;
    };
    const QDir d(directory);
    if (!hasGraniteCandidate(directory) && hasGraniteCandidate(d.filePath(QStringLiteral("s2")))) {
        resolved = d.filePath(QStringLiteral("s2"));
    }
    if (resolved == m_nativeDirectory) return;
    ++m_configurationGeneration;
    invalidateDownstreamPipeline();
    m_native->stop();
    m_nativeDirectory = QDir::cleanPath(resolved);
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings; settings.setValue(QStringLiteral("models/nativeDirectory"), m_nativeDirectory);
    }
    static_cast<void>(m_graniteAdapter->unload());
    refreshAvailableModels();
    emit modelEngineStatusChanged();
}

void HostController::appendNativeModelFiles() {
    const auto candidates = NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray();
    for (const auto& entry : candidates) {
        const auto candidate = entry.toObject();
        const auto quantization = candidate.value(QStringLiteral("quantization")).toString();
        const auto tag = NativeGraniteRuntime::modelTag(quantization);
        bool listed = false;
        for (const auto& model : m_availableModels) if (model.toMap().value(QStringLiteral("name")).toString() == tag) listed = true;
        const QFileInfo file(QDir(m_nativeDirectory).filePath(candidate.value(QStringLiteral("filename")).toString()));
        if (listed || !file.isFile() || file.size() != candidate.value(QStringLiteral("size_bytes")).toInteger()) continue;
        QVariantMap item;
        item[QStringLiteral("name")] = tag;
        item[QStringLiteral("size")] = QString::number(static_cast<double>(file.size()) / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QStringLiteral(" GiB");
        item[QStringLiteral("quantization")] = quantization;
        item[QStringLiteral("parameterSize")] = QStringLiteral("3B");
        item[QStringLiteral("source")] = QStringLiteral("Native llama.cpp · pinned candidate; verified when loaded");
        item[QStringLiteral("isActive")] = tag == m_activeModel;
        item[QStringLiteral("requiresImport")] = false;
        item[QStringLiteral("digest")] = m_native->isReady() && m_native->modelInfo().quantization == ss(quantization)
            ? qs(m_native->modelInfo().digest) : QString();
        m_availableModels.append(item);
    }
}

void HostController::prepareNativeModel() {
    if (!nativeGraniteSelected() || usesTransportOverride() || m_hostState == HostState::Quitting) return;
    if (m_graniteAdapter->state() == bridge::GraniteRuntimeState::Busy) return;
    if (m_isDownloadingModel && m_downloadingNativeTag == m_activeModel) {
        if (m_discoveryQueued) setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Waiting for the explicitly requested model download/repair. Editing remains available."));
        return;
    }
    if (m_native->isReady() && m_native->modelInfo().quantization == ss(NativeGraniteRuntime::quantizationForTag(m_activeModel))) {
        onNativeRuntimeChanged(); return;
    }
    m_nativeWarming = true;
    if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Loading) {
        static_cast<void>(m_graniteAdapter->unload());
        static_cast<void>(m_graniteAdapter->beginLoading());
    }
    m_native->load(m_nativeDirectory, NativeGraniteRuntime::quantizationForTag(m_activeModel));
    emit modelEngineStatusChanged();
}

void HostController::onNativeRuntimeChanged() {
    emit modelEngineStatusChanged();
    if (!nativeGraniteSelected() || usesTransportOverride() || m_hostState == HostState::Quitting) {
        if (!m_native->isRunning() || m_native->state() == QLatin1String("FAILED")) m_nativeWarming = false;
        return;
    }
    const auto state = m_native->state();
    if (m_native->isReady()) {
        m_nativeLastUse.restart();
        m_nativeWarming = false;
        const auto info = m_native->modelInfo();
        if (info.quantization != ss(NativeGraniteRuntime::quantizationForTag(m_activeModel))) return;
        if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy && m_graniteAdapter->state() != bridge::GraniteRuntimeState::Ready) {
            if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Loading) {
                static_cast<void>(m_graniteAdapter->unload()); static_cast<void>(m_graniteAdapter->beginLoading());
            }
            static_cast<void>(m_graniteAdapter->completeLoading(info));
        }
        m_availableModels.clear(); appendNativeModelFiles(); appendLocalModelFiles(); emit availableModelsChanged();
        if (m_discoveryQueued && m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready) {
            if (auto status = startDiscovery(false); !status.ok()) setLastError(status.error());
        }
        if (m_pendingNativeRewrite && m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready) {
            const auto pending = std::exchange(m_pendingNativeRewrite, std::nullopt);
            setProposalStatus(ProposalUiStatus::None, {});
            if (pending->stateVersion == stateVersion() && m_confirmedIntent && pending->intentId == m_confirmedIntent->id &&
                pending->profileId == m_rewriteProfile && pending->configurationGeneration == m_configurationGeneration) {
                if (!synthesizeRewrite(pending->stateVersion)) setProposalStatus(ProposalUiStatus::Failed, lastError());
            }
        }
        if (m_ambientCheckPending) m_ambientDebounce.start();
    } else if (state == QLatin1String("LOADING")) {
        m_nativeWarming = true;
        if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy && m_graniteAdapter->state() != bridge::GraniteRuntimeState::Loading) {
            static_cast<void>(m_graniteAdapter->unload());
            static_cast<void>(m_graniteAdapter->beginLoading());
        }
    } else if (state == QLatin1String("FAILED")) {
        m_nativeWarming = false;
        if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy) m_graniteAdapter->failLoading(ss(m_native->detail()));
        if (m_discoveryQueued) { m_discoveryQueued = false; setAnalysisStatus(AnalysisStatus::Failed, m_native->detail()); }
        if (m_pendingNativeRewrite) { m_pendingNativeRewrite.reset(); setProposalStatus(ProposalUiStatus::Failed, m_native->detail()); }
    } else if (state == QLatin1String("UNLOADED")) {
        m_nativeWarming = false;
        if (m_graniteAdapter->state() == bridge::GraniteRuntimeState::Loading) static_cast<void>(m_graniteAdapter->cancelLoading());
        else if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy) static_cast<void>(m_graniteAdapter->unload());
    }
    emit modelEngineStatusChanged(); emit intentChanged();
}

QString HostController::modelRuntimeState() const {
    const auto s = bridge::toString(m_graniteAdapter->state());
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString HostController::modelEngineStatus() const {
    if (nativeGraniteSelected() && !usesTransportOverride()) return m_native->detail();
    const auto info = m_graniteAdapter->modelInfo();
    QString status;
    switch (m_graniteAdapter->state()) {
        case bridge::GraniteRuntimeState::Unloaded:
            status = m_activeModel.isEmpty() ? QStringLiteral("No model selected. Choose one in the dashboard.")
                                             : QStringLiteral("Not connected to the local model yet.");
            break;
        case bridge::GraniteRuntimeState::Loading:
            status = QStringLiteral("Loading the local model. The first load can take up to a minute.");
            break;
        case bridge::GraniteRuntimeState::Ready:
            status = QStringLiteral("Ready: %1 (%2) through the local Ollama runtime on this computer.")
                         .arg(qs(info.modelId),
                              info.quantization.empty() ? QStringLiteral("quantization not reported") : qs(info.quantization));
            break;
        case bridge::GraniteRuntimeState::Busy:
            status = QStringLiteral("Working: %1 is processing a request.").arg(qs(info.modelId));
            break;
        case bridge::GraniteRuntimeState::Failed:
            status = QStringLiteral("Unavailable: ") + qs(m_graniteAdapter->lastError());
            break;
    }
    if (!m_modelStatusDetail.isEmpty()) {
        status += QStringLiteral(" ") + m_modelStatusDetail;
    }
    return status;
}

void HostController::setActiveModel(const QString& model) {
    if (m_activeModel == model) {
        return;
    }
    if (!nativeGraniteSelected() && !m_activeModel.isEmpty() && !QCoreApplication::organizationName().isEmpty()) {
        QSettings settings; settings.setValue(QStringLiteral("models/ollamaSelection"), m_activeModel);
    }
    m_activeModel = model;
    ++m_configurationGeneration;
    m_ambientCheck.reset();
    m_ambientDismissed.clear();
    m_ambientCheckPending = m_ambientObservation.has_value();
    setAmbientBadge(false);
    // Results and approvals from the previous model/configuration must not become current.
    invalidateDownstreamPipeline();
    m_native->stop();
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("models/active"), model);
    }
    emit activeModelChanged();
    if (m_healthCheckInFlight) {
        m_healthRecheckRequested = true;
    } else {
        static_cast<void>(m_graniteAdapter->unload());
        refreshAvailableModels();
    }
    emit modelEngineStatusChanged();
}

void HostController::refreshAvailableModels() {
    if (m_hostState == HostState::Quitting) return;
    if (nativeGraniteSelected() && !usesTransportOverride()) {
        m_healthRecheckRequested = false;
        m_availableModels.clear(); appendNativeModelFiles(); appendLocalModelFiles();
        emit availableModelsChanged(); emit modelEngineStatusChanged();
        return; // Listing installed files does not start inference or download weights.
    }
    // Switching models during inference must wait for the old runtime lease to be released.
    // Otherwise the next request could use the new model with the old model's recorded identity.
    if (m_graniteAdapter->state() == bridge::GraniteRuntimeState::Busy) {
        m_healthRecheckRequested = true;
        return;
    }
    if (m_healthCheckInFlight) {
        m_healthRecheckRequested = true;
        return;
    }
    if (!m_ollama) {
        m_modelStatusDetail = QStringLiteral("No local model runtime is configured.");
        emit modelEngineStatusChanged();
        return;
    }
    m_healthRecheckRequested = false;
    auto adapter = m_graniteAdapter;
    const auto state = adapter->state();
    bool probe = !m_activeModel.isEmpty() && state != bridge::GraniteRuntimeState::Busy &&
                 state != bridge::GraniteRuntimeState::Loading;
    if (probe) {
        static_cast<void>(adapter->unload());
        probe = adapter->beginLoading();
    }

    m_healthCheckInFlight = true;
    m_modelStatusDetail.clear();
    emit modelEngineStatusChanged();

    const OllamaClient client = *m_ollama;
    const QString model = probe ? m_activeModel : QString();
    auto* watcher = new QFutureWatcher<HealthResult>(this);
    ++m_inFlight;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        --m_inFlight;
        const HealthResult result = watcher->result();
        watcher->deleteLater();
        onHealthFinished(result);
    });
    watcher->setFuture(QtConcurrent::run([client, model]() -> HealthResult {
        HealthResult r;
        r.probedModel = model;
        r.models = client.listModels(OllamaClient::kDefaultHealthTimeoutMs);
        if (!model.isEmpty()) {
            r.probe = client.probeModel(model, OllamaClient::kDefaultHealthTimeoutMs);
            if (r.probe->ok()) {
                r.warmUp = client.warmUp(model, OllamaClient::kDefaultWarmUpTimeoutMs);
            }
        }
        return r;
    }));
}

void HostController::onHealthFinished(const HealthResult& result) {
    m_healthCheckInFlight = false;
    if (nativeGraniteSelected() && !usesTransportOverride()) { refreshAvailableModels(); return; }
    m_availableModels.clear();
    if (result.models.has_value()) {
        if (result.models->ok()) {
            for (const auto& entry : result.models->value()) {
                QVariantMap item;
                item[QStringLiteral("name")] = entry.name;
                item[QStringLiteral("size")] =
                    QString::number(static_cast<double>(entry.sizeBytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1) +
                    QStringLiteral(" GB");
                item[QStringLiteral("quantization")] =
                    entry.quantization.isEmpty() ? QStringLiteral("not reported") : entry.quantization;
                item[QStringLiteral("parameterSize")] = entry.parameterSize;
                item[QStringLiteral("digest")] = entry.digest;
                item[QStringLiteral("source")] = QStringLiteral("Local Ollama runtime");
                item[QStringLiteral("isActive")] = entry.name == m_activeModel;
                m_availableModels.append(item);
            }
        } else {
            m_modelStatusDetail = plainError(result.models->error());
        }
    }

    if (result.probe.has_value()) {
        if (result.probedModel != m_activeModel) {
            m_graniteAdapter->failLoading("The selected model changed during the check");
        } else if (result.probe->ok()) {
            const auto& info = result.probe->value();
            QString pinned;
            if (!QCoreApplication::organizationName().isEmpty()) {
                QSettings settings;
                pinned = settings.value(pinnedDigestKey(m_activeModel)).toString();
            }
            auto expected = core::crypto::normalizeSha256Digest(ss(pinned));
            auto actual = core::crypto::normalizeSha256Digest(info.digest);
            if (!pinned.isEmpty() && (!expected.ok() || !actual.ok() || expected.value() != actual.value())) {
                m_graniteAdapter->failLoading("The model's SHA-256 digest doesn't match the pinned digest " + ss(pinned));
            } else if (result.warmUp.has_value() && !result.warmUp->ok()) {
                m_graniteAdapter->failLoading("Ollama couldn't load " + ss(m_activeModel) + ": " +
                                              result.warmUp->error().message);
            } else if (!m_graniteAdapter->completeLoading(info)) {
                m_graniteAdapter->failLoading("The model runtime changed state during the check");
            }
        } else {
            m_graniteAdapter->failLoading(result.probe->error().message);
        }
    }

    appendNativeModelFiles();
    appendLocalModelFiles();
    emit availableModelsChanged();
    emit modelEngineStatusChanged();
    emit intentChanged();
    if (m_healthRecheckRequested) {
        static_cast<void>(m_graniteAdapter->unload());
        refreshAvailableModels();
        return;
    }
    if (m_discoveryQueued && m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy) {
        if (auto st = startDiscovery(false); !st.ok()) {
            setLastError(st.error());
        }
    }
    if (m_ambientCheckPending && m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready) {
        m_ambientDebounce.start();
    }
}

bool HostController::pinActiveModelDigest() {
    return runCommand("pinActiveModelDigest", [&]() -> Status {
        if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Ready) {
            return makeError(ErrorCode::PreconditionFailed, "Connect to the model before pinning its digest.");
        }
        if (QCoreApplication::organizationName().isEmpty()) {
            return makeError(ErrorCode::PreconditionFailed, "Settings storage is unavailable.");
        }
        const auto info = m_graniteAdapter->modelInfo();
        auto digest = core::crypto::normalizeSha256Digest(info.digest);
        if (!digest.ok()) {
            return digest.error();
        }
        QSettings settings;
        settings.setValue(pinnedDigestKey(qs(info.modelId)), qs(digest.value()));
        return Status::success();
    });
}

void HostController::downloadModel(const QString& modelTag) {
    if (m_isDownloadingModel) {
        return;
    }
    if (NativeGraniteRuntime::isNativeModel(modelTag)) {
        QJsonObject candidate;
        for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray())
            if (entry.toObject().value(QStringLiteral("quantization")).toString() == NativeGraniteRuntime::quantizationForTag(modelTag)) candidate = entry.toObject();
        m_isDownloadingModel = true; m_downloadProgress = 0; m_downloadingNativeTag = modelTag;
        m_pendingNativeDownload = candidate;
        m_downloadStatusMessage = QStringLiteral("Preparing the pinned native model download."); emit downloadProgressChanged();
        if (m_native->activeQuantization() == NativeGraniteRuntime::quantizationForTag(modelTag) && m_native->state() != QLatin1String("STOPPING")) {
            if (modelTag == m_activeModel) invalidateDownstreamPipeline();
            m_native->stop(); // Release its mapping before atomically replacing the file.
        }
        startPendingNativeDownload();
        return;
    }
    static const QRegularExpression kTagPattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}$"));
    if (!kTagPattern.match(modelTag).hasMatch()) {
        setLastError(QStringLiteral("That isn't a valid model name."));
        return;
    }

    m_isDownloadingModel = true;
    m_downloadProgress = 0;
    m_downloadStatusMessage = QStringLiteral("Starting download of %1.").arg(modelTag);
    emit downloadProgressChanged();

    m_downloadProcess = std::make_unique<QProcess>();
    auto onOutput = [this](const QByteArray& bytes) {
        const QString text = QString::fromUtf8(bytes).trimmed();
        if (text.isEmpty()) {
            return;
        }
        m_downloadStatusMessage = text.section(QLatin1Char('\n'), -1);
        static const QRegularExpression pctRegex(QStringLiteral("(\\d+)%"));
        const auto match = pctRegex.match(text);
        if (match.hasMatch()) {
            m_downloadProgress = std::clamp(match.captured(1).toInt(), 0, 100);
        }
        emit downloadProgressChanged();
    };
    connect(m_downloadProcess.get(), &QProcess::readyReadStandardOutput, this,
            [this, onOutput]() { onOutput(m_downloadProcess->readAllStandardOutput()); });
    connect(m_downloadProcess.get(), &QProcess::readyReadStandardError, this,
            [this, onOutput]() { onOutput(m_downloadProcess->readAllStandardError()); });
    connect(m_downloadProcess.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_isDownloadingModel && m_downloadProcess->state() == QProcess::NotRunning) {
            m_isDownloadingModel = false;
            m_downloadStatusMessage = QStringLiteral("Couldn't start the Ollama command-line tool. Is Ollama installed?");
            emit downloadProgressChanged();
        }
    });
    connect(m_downloadProcess.get(), &QProcess::finished, this, [this, modelTag](int exitCode, QProcess::ExitStatus status) {
        m_isDownloadingModel = false;
        if (status == QProcess::NormalExit && exitCode == 0) {
            m_downloadProgress = 100;
            m_downloadStatusMessage = QStringLiteral("Downloaded %1.").arg(modelTag);
            refreshAvailableModels();
        } else {
            m_downloadStatusMessage = QStringLiteral("The download failed or was cancelled (exit code %1).").arg(exitCode);
        }
        emit downloadProgressChanged();
    });
    m_downloadProcess->start(QStringLiteral("ollama"), QStringList{QStringLiteral("pull"), modelTag});
}

void HostController::startPendingNativeDownload() {
    if (!m_pendingNativeDownload || !m_isDownloadingModel || m_hostState == HostState::Quitting) return;
    const auto quantization = m_pendingNativeDownload->value(QStringLiteral("quantization")).toString();
    if (m_native->isRunning() && m_native->activeQuantization() == quantization) {
        QTimer::singleShot(25, this, &HostController::startPendingNativeDownload);
        return;
    }
    if (m_nativeDirectory.isEmpty()) {
        m_nativeDirectory = m_modelsBaseDirectory + QStringLiteral("/s2");
    }
    QDir().mkpath(m_nativeDirectory);
    const auto candidate = std::exchange(m_pendingNativeDownload, std::nullopt);
    m_nativeInstaller->start(NativeGraniteRuntime::manifest(), *candidate, m_nativeDirectory);
}

void HostController::cancelModelDownload() {
    if (m_pendingNativeDownload) {
        m_pendingNativeDownload.reset(); const auto download = std::exchange(m_downloadingNativeTag, {}); m_isDownloadingModel = false;
        m_downloadStatusMessage = QStringLiteral("Native model download cancelled before starting. The previous file was preserved.");
        if (m_discoveryQueued && download == m_activeModel) { m_discoveryQueued = false; setAnalysisStatus(AnalysisStatus::Failed, m_downloadStatusMessage); }
        if (m_pendingNativeRewrite && download == m_activeModel) { m_pendingNativeRewrite.reset(); setProposalStatus(ProposalUiStatus::Failed, m_downloadStatusMessage); }
        emit downloadProgressChanged(); return;
    }
    if (m_nativeInstaller->isRunning()) { m_nativeInstaller->cancel(); return; }
    if (m_downloadProcess && m_downloadProcess->state() != QProcess::NotRunning) {
        m_downloadProcess->kill();
        m_isDownloadingModel = false;
        m_downloadStatusMessage = QStringLiteral("Download cancelled.");
        emit downloadProgressChanged();
    }
}

// ----------------------------------------------------------------------------------------------
// Test seams
// ----------------------------------------------------------------------------------------------

// ----------------------------------------------------------------------------------------------
// Ambient mode
// ----------------------------------------------------------------------------------------------

bool HostController::isAmbientEligibleApp(const QString& identifier) {
    return HostContextDetector::isAmbientEligibleApp(identifier);
}

QString HostController::globalShortcutText() {
#if defined(Q_OS_MAC)
    return QStringLiteral("Cmd+Shift+R");
#else
    return QStringLiteral("Ctrl+Shift+R");
#endif
}

bool HostController::isAccessibilityTrusted() const {
    return m_accessibility->access() == AccessibilityAccess::Granted;
}

QString HostController::accessibilityStatus() const {
    return QString::fromUtf8(toString(m_accessibility->access()));
}

bool HostController::isAmbientActive() const {
    return m_ambientModeEnabled && isAccessibilityTrusted();
}

void HostController::setAmbientModeEnabled(bool enabled) {
    if (m_ambientModeEnabled == enabled) {
        return;
    }
    m_ambientModeEnabled = enabled;
    if (!enabled) m_followAmbientDraft = false;
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("ambient/enabled"), enabled);
    }
    emit ambientModeEnabledChanged();
    applyAmbientObserving();
}

bool HostController::requestAccessibilityPermission() {
    return m_accessibility->requestAccess();
}

void HostController::applyAmbientObserving() {
    const bool active = isAmbientActive();
    m_accessibility->setObserving(active);
    if (!active) {
        m_ambientDebounce.stop();
        m_ambientObservation.reset();
        m_ambientCheckPending = false;
        setAmbientBadge(false);
    }
    if (!m_ambientModeEnabled) {
        setAmbientStatus(QStringLiteral("Off. %1 opens Roclint with its own text box.").arg(shortcutLabel()));
    } else if (m_accessibility->access() == AccessibilityAccess::NotGranted) {
        setAmbientStatus(QStringLiteral("Waiting for Accessibility permission. Until then, %1 opens Roclint with its own text box.")
                             .arg(shortcutLabel()));
    } else if (m_accessibility->access() == AccessibilityAccess::Unavailable) {
        setAmbientStatus(QStringLiteral("Accessibility isn't available on this system, so Roclint uses its own text box."));
    } else {
        setAmbientStatus(QStringLiteral("Watching for prompts in supported apps."));
    }
}

QString HostController::captureTargetName() const {
    return m_captureTarget.has_value() ? m_captureTarget->field.appDisplayName : QString();
}

QString HostController::deliveryLabel() const {
    if (m_captureSource == CaptureSource::Ambient && m_captureTarget.has_value()) {
        return QStringLiteral("Delivers into %1").arg(m_captureTarget->field.appDisplayName);
    }
    return QStringLiteral("Delivers to clipboard");
}

void HostController::setCaptureSource(CaptureSource source) {
    if (source == CaptureSource::Widget) {
        m_captureTarget.reset();
        m_followAmbientDraft = false;
    }
    if (m_captureSource != source) {
        m_captureSource = source;
    }
    emit captureSourceChanged();
}

void HostController::setAmbientBadge(bool visible, const QString& label) {
    if (m_ambientBadgeVisible == visible && (label.isEmpty() || label == m_ambientBadgeLabel)) {
        return;
    }
    m_ambientBadgeVisible = visible;
    if (!label.isEmpty()) {
        m_ambientBadgeLabel = label;
    }
    if (visible && m_ambientObservation.has_value()) {
        const auto& f = m_ambientObservation->field;
        if (!f.caretRect.isNull()) {
            m_ambientBadgePos = QPoint(f.caretRect.right() + 8, f.caretRect.bottom() + 6);
        } else if (!f.fieldRect.isNull()) {
            m_ambientBadgePos = QPoint(f.fieldRect.right() - 180, f.fieldRect.top() - 30);
        } else {
            m_ambientBadgePos = QPoint(40, 40);
        }
    }
    emit ambientBadgeChanged();
}

void HostController::setAmbientStatus(const QString& status) {
    if (m_ambientStatus != status) {
        m_ambientStatus = status;
        emit ambientStatusChanged();
    }
}

void HostController::onFieldLost() {
    m_ambientDebounce.stop();
    m_ambientObservation.reset();
    setAmbientBadge(false);
}

void HostController::followAmbientDraft() {
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS") && m_captureTarget && m_ambientObservation &&
        m_captureTarget->textHash != m_ambientObservation->textHash) {
        qInfo() << "[Roclint follow] visible=" << isCardVisible() << "ambient=" << isAmbientCapture()
                << "following=" << m_followAmbientDraft << "delivery=" << m_pendingDelivery.has_value()
                << "target_token=" << m_captureTarget->field.fieldToken << "observed_token=" << m_ambientObservation->field.fieldToken
                << "target_pid=" << m_captureTarget->field.processId << "observed_pid=" << m_ambientObservation->field.processId;
    }
    if (!isCardVisible() || !m_ambientObservation || m_pendingDelivery) return;
    const auto observed = *m_ambientObservation;
    // Chromium can expose the editor after the initial bounded invocation. Bind it without
    // another hotkey; a draft already typed into the HUD remains the user's authoritative text.
    if (!isAmbientCapture() && !m_captureOpenPending && !m_captureRequestedApp.isEmpty() &&
        observed.field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0) {
        const bool preserveHud = m_widgetDraftEdited || m_selectingInsertionTarget || m_currentProposal.has_value();
        if (!preserveHud && m_revisionState && !noteDraftEdited(stateVersion())) return;
        m_widgetDraftEdited = preserveHud;
        m_captureSource = CaptureSource::Ambient;
        m_captureTarget = observed;
        m_selectingInsertionTarget = false;
        m_followAmbientDraft = !preserveHud;
        if (!preserveHud) {
            emit ambientDraftUpdated(observed.prompt);
        }
        emit captureSourceChanged();
        clearLastError();
        setAnalysisStatus(m_analysisStatus, preserveHud
            ? QStringLiteral("Editor captured. Your HUD edit is preserved; Insert uses this destination.")
            : QStringLiteral("Live capture connected. Continue typing in your editor, then Analyze when ready."));
        return;
    }
    if (!isAmbientCapture() || !m_followAmbientDraft || !m_captureTarget) return;
    const auto& previous = *m_captureTarget;
    // Before a HUD edit or an explicit clarification decision, follow the focused draft.
    // Identity changes invalidate prior analysis/approval even when the words happen to match.
    if (previous.field.fieldToken == observed.field.fieldToken &&
        previous.field.processId == observed.field.processId &&
        previous.field.appIdentifier.compare(observed.field.appIdentifier, Qt::CaseInsensitive) == 0 &&
        previous.textHash == observed.textHash) return;
    if (!noteDraftEdited(stateVersion())) return;
    m_followAmbientDraft = true;
    m_captureTarget = observed;
    if (m_hostContextEnabled) {
        m_hostContext = HostContextObservation{HostContextDetector::mapIdentityToCategory(observed.field.appIdentifier),
                                               observed.field.appDisplayName, observed.field.appIdentifier};
        emit hostContextChanged();
    }
    emit captureSourceChanged();
    emit ambientDraftUpdated(observed.prompt);
    clearLastError();
    setAnalysisStatus(AnalysisStatus::Idle, QStringLiteral("Live draft updated. Analyze when you are ready."));
}

void HostController::onFieldObserved(const ObservedField& field) {
    if (!isAmbientActive()) {
        return;
    }
    if (!isAmbientEligibleApp(field.appIdentifier)) {
        onFieldLost();
        return;
    }
    if (field.elevationMismatch) {
        onFieldLost();
        setAmbientStatus(QStringLiteral("%1 is running as administrator, so Roclint can't read it. Use %2 instead.")
                             .arg(field.appDisplayName, globalShortcutText()));
        return;
    }
    if (field.isPassword) {
        onFieldLost();
        return;
    }

    QString prompt;
    if (field.kind == FieldKind::Terminal) {
        const auto draft = TerminalAdapter::extractDraft(field.text, field.caretOffset);
        prompt = draft.has_value() ? draft->text : QString();
    } else {
        prompt = field.text;
    }
    prompt = qs(core::normalizeLineEndings(ss(prompt)));
    if (field.kind == FieldKind::GuiEditor && prompt.trimmed().isEmpty()) {
        // An empty composer is still a valid destination for a draft authored in the HUD.
        m_ambientDebounce.stop();
        setAmbientBadge(false);
        m_ambientObservation = AmbientObservation{field, prompt, core::crypto::Sha256::hexDigest(ss(prompt))};
        followAmbientDraft();
        setAmbientStatus(QStringLiteral("Ready to capture a prompt in %1.").arg(field.appDisplayName));
        if (m_captureOpenPending && field.refreshRequestId == m_captureOpenGeneration &&
            field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0) {
            m_captureOpenPending = false;
            ++m_captureOpenGeneration;
            openFromAmbient();
        }
        return;
    }
    const auto words = prompt.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
    if (words < kAmbientMinWords || prompt.size() > kAmbientMaxChars) {
        onFieldLost();
        return;
    }

    AmbientObservation observation{field, prompt, core::crypto::Sha256::hexDigest(ss(prompt))};
    const bool textChanged = !m_ambientObservation.has_value() || m_ambientObservation->textHash != observation.textHash ||
                             m_ambientObservation->field.fieldToken != field.fieldToken ||
                             m_ambientObservation->field.processId != field.processId;
    m_ambientObservation = std::move(observation);
    followAmbientDraft();
    setAmbientStatus(QStringLiteral("Watching %1.").arg(field.appDisplayName));
    if (m_captureOpenPending && field.refreshRequestId == m_captureOpenGeneration &&
        field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0) {
        m_captureOpenPending = false;
        ++m_captureOpenGeneration;
        openFromAmbient();
        return;
    }
    if (!textChanged) {
        return; // Caret moved or the field re-reported the same text
    }
    if (m_ambientCheck.has_value() && m_ambientCheck->textHash == m_ambientObservation->textHash) {
        setAmbientBadge(m_ambientCheck->needsAttention && m_ambientDismissed.count(m_ambientCheck->textHash) == 0,
                        m_ambientBadgeLabel);
        return;
    }
    setAmbientBadge(false);
    m_ambientDebounce.start();
}

void HostController::onAmbientDebounce() {
    if (!isAmbientActive() || !m_ambientObservation.has_value() || isCardVisible()) {
        m_ambientCheckPending = false;
        return;
    }
    const std::string& hash = m_ambientObservation->textHash;
    if (promptTriageRoutingEnabled()) {
        // An ambient capture has not frozen a PromptRevision. Ask the user to open it first;
        // the normal S1-first flow then owns all model calls and their provenance.
        m_ambientCheckPending = false;
        setAmbientBadge(m_ambientDismissed.count(hash) == 0, QStringLiteral("Check prompt"));
        return;
    }
    if ((m_ambientCheck.has_value() && m_ambientCheck->textHash == hash) || m_ambientDismissed.count(hash) > 0) {
        m_ambientCheckPending = false;
        return;
    }
    const auto runtime = m_graniteAdapter->state();
    if (m_ambientCheckInFlight || runtime == bridge::GraniteRuntimeState::Busy ||
        runtime == bridge::GraniteRuntimeState::Loading) {
        m_ambientCheckPending = true;
        return;
    }
    if (runtime != bridge::GraniteRuntimeState::Ready) {
        if (m_ollama && !usesTransportOverride() && !m_ambientCheckPending) {
            m_ambientCheckPending = true;
            refreshAvailableModels();
        } else {
            m_ambientCheckPending = false;
        }
        return;
    }
    m_ambientCheckPending = false;
    startAmbientCheck();
}

void HostController::startAmbientCheck() {
    const AmbientObservation observation = *m_ambientObservation;
    PipelineTicket previewTicket;
    previewTicket.sessionId = "sess_ambient";
    previewTicket.revisionId = "rev_sess_ambient_1";
    previewTicket.stateVersion = m_configurationGeneration;
    previewTicket.attemptId = newOpaqueId("ambient_attempt_");
    const auto inner = currentTransport(bridge::discoveryResponseSchema(), &previewTicket, QStringLiteral("granite_analyze"), observation.prompt);
    if (!inner) return;
    bridge::GraniteOptionDiscoveryRequest request;
    request.sessionId = "sess_ambient";
    request.revisionId = "rev_sess_ambient_1";
    request.promptString = ss(observation.prompt);
    request.hostContextCategory = m_hostContextEnabled ? ss(HostContextObservation{HostContextDetector::mapIdentityToCategory(
                                                                observation.field.appIdentifier),
                                                            QString(), QString()}
                                         .categoryString()) : "UNKNOWN";
    const auto configurationGeneration = m_configurationGeneration;

    struct CheckResult {
        core::Result<bridge::GraniteOptionDiscoveryResponse> response{makeError(ErrorCode::Internal, "not run")};
        std::string raw;
    };
    m_ambientCheckInFlight = true;
    auto adapter = m_graniteAdapter;
    auto* watcher = new QFutureWatcher<CheckResult>(this);
    ++m_inFlight;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    if (nativeWork) ++m_nativeSemanticInFlight;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, observation, configurationGeneration, nativeWork]() {
        --m_inFlight;
        if (nativeWork) --m_nativeSemanticInFlight;
        finishQuitWhenRuntimesStopped();
        m_ambientCheckInFlight = false;
        const CheckResult result = watcher->result();
        watcher->deleteLater();
        if (m_healthRecheckRequested && !m_healthCheckInFlight) refreshAvailableModels();
        emit modelEngineStatusChanged();

        if (configurationGeneration != m_configurationGeneration) {
            if (m_ambientObservation.has_value()) m_ambientDebounce.start();
            return;
        }

        if (result.response.ok()) {
            const auto& analysisValue = result.response.value().analysis;
            const std::string text = ss(observation.prompt);
            const auto policy = analysis::MaterialityPolicyV1::evaluatePrompt(analysisValue, text);
            const auto eval = m_s1Adapter.evaluateSnapshot(analysisValue);
            const auto decision =
                intent::InterventionStateMachine::evaluate(analysisValue, policy, eval, intent::WorkingIntentState{});
            AmbientCheck check;
            check.textHash = observation.textHash;
            check.rawReply = result.raw;
            check.model = result.response.value().model;
            check.needsAttention = decision.verdict == intent::InterventionVerdict::Clarify;
            for (const auto& g : policy.groupResults) {
                check.questionCount += g.isEligible() ? 1 : 0;
            }
            for (const auto& md : analysisValue.missingDecisions) {
                check.questionCount += md.blocking ? 1 : 0;
            }
            for (const auto& c : analysisValue.conflicts) {
                check.questionCount += c.blocking ? 1 : 0;
            }
            check.questionCount = std::clamp(check.questionCount, 1, 3);
            m_ambientCheck = check;
            const bool current = m_ambientObservation.has_value() && m_ambientObservation->textHash == check.textHash;
            if (current && check.needsAttention && m_ambientDismissed.count(check.textHash) == 0 && !isCardVisible()) {
                setAmbientBadge(true, check.questionCount == 1 ? QStringLiteral("1 question")
                                                               : QStringLiteral("%1 questions").arg(check.questionCount));
            }
            setAmbientStatus(check.needsAttention ? QStringLiteral("Found something worth checking in %1.")
                                                        .arg(observation.field.appDisplayName)
                                                  : QStringLiteral("Watching %1. The last prompt looked clear.")
                                                        .arg(observation.field.appDisplayName));
        } else {
            // Failures never interrupt the user in the other app; they show only in Settings.
            setAmbientStatus(QStringLiteral("The last background check failed: ") + plainError(result.response.error()));
        }
        if (m_discoveryQueued) {
            if (auto st = startDiscovery(); !st.ok()) {
                setLastError(st.error());
            }
        }
        if (m_ambientObservation.has_value() && (!m_ambientCheck.has_value() ||
                                                 m_ambientCheck->textHash != m_ambientObservation->textHash)) {
            m_ambientDebounce.start();
        }
    });
    watcher->setFuture(QtConcurrent::run([adapter, request, inner]() -> CheckResult {
        CheckResult out;
        auto raw = std::make_shared<std::string>();
        const bridge::GraniteTransport capture = [inner, raw](std::string_view instruction) {
            auto reply = inner(instruction);
            if (reply.ok()) {
                *raw = reply.value();
            }
            return reply;
        };
        try {
            out.response = adapter->discoverOptions(request, capture);
        } catch (const std::exception& ex) {
            out.response = makeError(ErrorCode::Internal, std::string("Background check crashed: ") + ex.what());
        }
        out.raw = *raw;
        return out;
    }));
}

bool HostController::openFromAmbient() {
    return runCommand("openFromAmbient", [&]() -> Status {
        if (!isAmbientActive() || !m_ambientObservation.has_value()) {
            return makeError(ErrorCode::PreconditionFailed, "There's no prompt from another app to open.");
        }
        if (!m_repository) {
            return storageUnavailable();
        }
        const AmbientObservation observation = *m_ambientObservation;
        const std::string text = ss(observation.prompt);

        if (m_selectingInsertionTarget) {
            m_selectingInsertionTarget = false;
            if (!m_revisionState || !m_currentProposal || m_proposalStatus != ProposalUiStatus::ReadyForReview) {
                showCardInternal(false);
                return makeError(ErrorCode::StaleVersion, "The rewrite changed while choosing its destination. Review it before inserting.");
            }
            m_captureSource = CaptureSource::Ambient;
            m_captureTarget = observation;
            m_followAmbientDraft = false;
            emit captureSourceChanged();
            showCardInternal(false);
            setAnalysisStatus(m_analysisStatus, QStringLiteral("Destination connected. Your rewrite and answers are unchanged; finish review, then Approve & Insert."));
            return Status::success();
        }

        if (isBlank(text) && observation.field.kind == FieldKind::GuiEditor) {
            resetSessionState();
            m_captureSource = CaptureSource::Ambient;
            m_captureTarget = observation;
            m_followAmbientDraft = true;
            m_widgetDraftEdited = false;
            emit captureSourceChanged();
            emit ambientDraftUpdated(QString());
            showCardInternal(false);
            return Status::success();
        }

        // Each ambient capture is its own session unless it is the prompt already open.
        if (!m_revisionState || m_revisionState->isDraftDirty() || m_revisionState->currentRevision().text() != text) {
            resetSessionState();
            if (auto st = ensureSession(text, nowMs()); !st.ok()) {
                return st;
            }
            invalidateDownstreamPipeline();
        }
        m_captureSource = CaptureSource::Ambient;
        m_captureTarget = observation;
        m_followAmbientDraft = true;
        m_widgetDraftEdited = false;
        emit captureSourceChanged();
        emit ambientDraftUpdated(qs(text));

        if (!m_currentAnalysis) {
            bool rebound = false;
            if (!promptTriageRoutingEnabled() && m_ambientCheck.has_value() && m_ambientCheck->textHash == observation.textHash &&
                !m_ambientCheck->rawReply.empty()) {
                // The background check already asked the model; bind its exact reply to the frozen revision.
                auto parsed = bridge::parseDiscoveryResponse(m_ambientCheck->rawReply, currentSessionId(),
                                                             m_revisionState->currentRevision().revisionId().value(),
                                                             text);
                if (parsed.ok()) {
                    PipelineTicket ticket = makeTicket();
                    ticket.analysisGroup = std::make_shared<AnalysisGroup>(m_analysisGroupTimeoutMs);
                    ticket.analysisGroup->id = newOpaqueId("grp_");
                    if (auto st = beginAttempt(ticket, "discovery"); !st.ok()) {
                        return st;
                    }
                    auto response = std::move(parsed).value();
                    response.model = m_ambientCheck->model;
                    armAnalysisDeadline(ticket);
                    acceptDiscovery(ticket, response);
                    rebound = true;
                }
            }
            if (!rebound) {
                if (auto st = startDiscovery(); !st.ok()) {
                    setLastError(st.error());
                }
            }
        }
        showCardInternal(false);
        return Status::success();
    });
}

void HostController::dismissAmbientBadge() {
    if (m_ambientObservation.has_value()) {
        m_ambientDismissed.insert(m_ambientObservation->textHash);
    }
    setAmbientBadge(false);
}

#ifdef ROCLINT_ENABLE_TEST_MOCKS
void HostController::setTransportOverrideForTesting(bridge::GraniteTransport transport) {
    m_transportOverride = std::move(transport);
}

bool HostController::setModelRuntimeUrlForTesting(const QUrl& url) {
    auto client = OllamaClient::create(url);
    if (!client.ok()) {
        return false;
    }
    m_ollama = client.value();
    if (nativeGraniteSelected()) { m_activeModel.clear(); m_native->stop(); }
    return true;
}

bool HostController::markModelReadyForTesting(const bridge::GraniteModelInfo& info) {
    static_cast<void>(m_graniteAdapter->unload());
    const bool ok = m_graniteAdapter->beginLoading() && m_graniteAdapter->completeLoading(info);
    emit modelEngineStatusChanged();
    emit intentChanged();
    return ok;
}

bool HostController::waitForIdleForTesting(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (m_inFlight > 0 || m_nativeWarming || m_promptTriageLoad.has_value()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
    }
    QCoreApplication::processEvents();
    return true;
}
#endif

} // namespace roclint::host
