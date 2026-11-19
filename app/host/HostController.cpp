#include "roclint/bridge/WordSurfaceCheck.hpp"
#include "HostController.hpp"
#include <iostream>
#include <regex>
#include <QJsonArray>
#include <utility>
#include <QKeySequence>
#include "ScreenPlacement.hpp"
#include "WordSpan.hpp"
#include "TerminalAdapter.hpp"

#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/prompt_state/TextNormalization.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/rewrite/StructuredEntityExtractor.hpp"

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
#include <QScopedValueRollback>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#ifdef ROCLINT_ENABLE_TEST_MOCKS
#include <QThread>
#endif

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>

namespace roclint::host {

#ifdef ROCLINT_ENABLE_TEST_MOCKS
// Defined in HostControllerWorkspace.cpp. Delays only the asynchronous
// proposal revalidation worker so tests can order superseded results.
int codebaseRevalidationDelayForTesting();
#endif

using core::ErrorCode;
using core::makeError;
using core::Status;
namespace analysis = core::analysis;
namespace intent = core::intent;
namespace rewrite = core::rewrite;
namespace review = core::review;

namespace {

void clearActiveGroundingBinding(std::vector<GroundedEvidenceView>& evidence,
                                 std::string& prompt,
                                 std::string& activeFile,
                                 std::string& proposalId,
                                 std::string& sessionId,
                                 std::string& revisionId,
                                 quint64& validationSerial) {
    evidence.clear();
    prompt.clear();
    activeFile.clear();
    proposalId.clear();
    sessionId.clear();
    revisionId.clear();
    ++validationSerial;
}

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

bool isMatchingConstraintId(std::string_view a, std::string_view b) noexcept {
    if (a == b) return true;

    if (a.starts_with("intent_grp_") && b.starts_with("intent_item_grp_")) {
        return a.substr(11) == b.substr(16);
    }
    if (b.starts_with("intent_grp_") && a.starts_with("intent_item_grp_")) {
        return b.substr(11) == a.substr(16);
    }

    if (a.starts_with("intent_") && !a.starts_with("intent_grp_") &&
        b.starts_with("intent_item_") && !b.starts_with("intent_item_grp_")) {
        return a.substr(7) == b.substr(12);
    }
    if (b.starts_with("intent_") && !b.starts_with("intent_grp_") &&
        a.starts_with("intent_item_") && !a.starts_with("intent_item_grp_")) {
        return b.substr(7) == a.substr(12);
    }

    return false;
}

// A narrow, mechanical settled-edit check. It does not interpret taste or technical choices;
// it only recognizes an exact quoted old/new replacement for a named file with an explicit
// boundary. Such a request must not acquire an invented confirmation question.
bool hasExplicitMechanicalEdit(std::string_view prompt) {
    std::string lower;
    lower.reserve(prompt.size());
    for (char c : prompt) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    const auto quoteValueAfter = [&](std::size_t at, std::string& value) -> bool {
        while (at < lower.size() && std::isspace(static_cast<unsigned char>(lower[at]))) ++at;
        if (at >= lower.size() || (lower[at] != '\'' && lower[at] != '"')) return false;
        const char quote = lower[at++];
        const auto end = lower.find(quote, at);
        if (end == std::string::npos) return false;
        value = lower.substr(at, end - at);
        return !value.empty();
    };

    std::string oldValue;
    std::string newValue;
    const auto from = lower.find(" from ");
    if (from == std::string::npos || !quoteValueAfter(from + 6, oldValue)) return false;
    const auto to = lower.find(" to ", from + 6);
    if (to == std::string::npos || !quoteValueAfter(to + 4, newValue)) return false;
    if (oldValue == newValue) return false;

    bool namedFile = false;
    std::size_t at = 0;
    while (at < lower.size()) {
        while (at < lower.size() && std::isspace(static_cast<unsigned char>(lower[at]))) ++at;
        const auto start = at;
        while (at < lower.size() && !std::isspace(static_cast<unsigned char>(lower[at]))) ++at;
        const auto token = lower.substr(start, at - start);
        const auto dot = token.rfind('.');
        if (dot != std::string::npos && dot + 1 < token.size()) {
            std::string ext = token.substr(dot + 1);
            while (!ext.empty() && !std::isalpha(static_cast<unsigned char>(ext.back()))) ext.pop_back();
            if (ext.size() >= 1 && ext.size() <= 6 &&
                std::all_of(ext.begin(), ext.end(), [](unsigned char c) { return std::isalpha(c) != 0; })) {
                namedFile = true;
                break;
            }
        }
    }
    if (!namedFile) return false;

    return lower.find("do not change") != std::string::npos ||
           lower.find("do not modify") != std::string::npos ||
           lower.find("without changing") != std::string::npos ||
           lower.find("without modifying") != std::string::npos ||
           lower.find("no other text") != std::string::npos;
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
            return QStringLiteral("The local model isn't available. Retry explicitly or keep the original prompt.");
        case ErrorCode::ProviderFailed:
            return QStringLiteral("The local model returned an unusable reply. Retry explicitly or keep the original prompt.");
        case ErrorCode::DbError:
            return QStringLiteral("History couldn't be saved. Your draft is preserved. Retry history explicitly.");
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
    if (error.code == ErrorCode::ProviderUnavailable || error.code == ErrorCode::StaleVersion) {
        return error;
    }
    return core::Error{ErrorCode::ProviderFailed,
                       "The local model returned an unusable reply. Retry explicitly or keep the original prompt."};
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
        m_juliaDevice = settings.value(QStringLiteral("scoring/juliaDevice"), QString{}).toString();
        m_tokenSpeedMode = settings.value(QStringLiteral("models/tokenSpeedMode"), QStringLiteral("BOTH")).toString();
        m_thinkingEnabled = settings.value(QStringLiteral("models/thinkingEnabled"), false).toBool();
        m_activeModel = settings.value(QStringLiteral("models/active"), m_activeModel).toString();
        m_modelDirectory = settings.value(QStringLiteral("models/directory")).toString();
        m_nativeDirectory = settings.value(QStringLiteral("models/nativeDirectory"), m_modelsBaseDirectory + QStringLiteral("/s2")).toString();
        m_ambientModeEnabled = settings.value(QStringLiteral("ambient/enabled"), true).toBool();
        m_ambientBadgeSuggestionsOnly = settings.value(QStringLiteral("ambient/badgeSuggestionsOnly"), false).toBool();
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
    connect(this, &HostController::modelEngineStatusChanged, this,
            &HostController::startPendingWordRefinement, Qt::QueuedConnection);

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
    connect(m_native.get(), &NativeGraniteRuntime::speedMetricsChanged, this, [this]() {
        emit inferenceSpeedChanged();
        emit modelEngineStatusChanged();
    });
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
        emit modelEngineStatusChanged();
        emit modelDirectoryChanged();
    });

    connect(m_native.get(), &NativeGraniteRuntime::partialResultReceived,
            this, [this](quint64, const QString& chunk) {
        if (m_proposalStatus == ProposalUiStatus::Generating) {
            m_streamingPreview += chunk;
            emit rewriteProposalChanged();
        }
    });

    m_ambientDebounce.setSingleShot(true);
    m_ambientDebounce.setInterval(kAmbientDebounceMs);
    connect(&m_ambientDebounce, &QTimer::timeout, this, &HostController::onAmbientDebounce);
    m_ambientBadgeRecovery.setSingleShot(true);
    m_ambientBadgeRecovery.setInterval(750);
    m_ambientBadgeRecovery.setTimerType(Qt::PreciseTimer);
    connect(&m_ambientBadgeRecovery, &QTimer::timeout, this, &HostController::ambientBadgeChanged);
    connect(m_accessibility.get(), &IAccessibilityBridge::accessChanged, this, [this]() {
        emit accessibilityTrustedChanged();
        emit ambientModeEnabledChanged();
        applyAmbientObserving();
    });
    connect(m_accessibility.get(), &IAccessibilityBridge::fieldObserved, this, &HostController::onFieldObserved);
    connect(m_accessibility.get(), &IAccessibilityBridge::fieldLost, this, [this]() { onFieldLost(); });
    connect(m_accessibility.get(), &IAccessibilityBridge::fieldReadInterrupted, this, [this]() { onFieldLost(true); });
    connect(m_accessibility.get(), &IAccessibilityBridge::injectionFinished, this,
            &HostController::onInjectionFinished);
    connect(m_accessibility.get(), &IAccessibilityBridge::rangeInjectionFinished, this,
            &HostController::onRangeInjectionFinished);
    connect(this, &HostController::wordRefinementChanged, this, [this]() {
        setAmbientBadge(m_ambientBadgeVisible);
        emit ambientBadgeChanged();
    });
    applyAmbientObserving();
    initializeResidencyPolicy();
}

HostController::~HostController() {
    stopCodebaseWatcher();
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
        m_lastInternalDiagnostic = QString::fromUtf8(ex.what()).left(1024);
        setLastError(QStringLiteral("%1 couldn't finish. Retry explicitly or keep the original prompt.").arg(QLatin1String(name)));
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

void HostController::activateCard() {
    showCardInternal(false);
}

void HostController::showCard() {
    if (m_resumeMinimizedCard && !m_selectingInsertionTarget) {
        showCardInternal(false);
        return;
    }
    const auto frontmost = HostContextDetector::captureFrontmost();
    if (m_selectingInsertionTarget && !isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
        m_selectingInsertionTarget = false;
        showCardInternal(false);
        return;
    }
    if (!m_selectingInsertionTarget && hasActiveWork()) {
        showCardInternal(false);
        return;
    }
    if (isAmbientActive() && m_ambientObservation.has_value() &&
        (m_ambientObservation->field.appIdentifier.compare(frontmost.bundleOrExecutableId, Qt::CaseInsensitive) == 0 ||
         frontmost.bundleOrExecutableId.isEmpty())) {
        openFromAmbient();
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
        if (refreshed &&
            (current.bundleOrExecutableId.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0 || current.bundleOrExecutableId.isEmpty()) &&
            (m_ambientObservation->field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0 || m_captureRequestedApp.isEmpty())) {
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
    clearWordRefinement();
    m_hostContext = m_hostContextEnabled ? HostContextDetector::captureFrontmost() : HostContextObservation{};
    emit hostContextChanged();
    if (captureClipboard) {
        captureClipboardDraft();
    }
    if (!promptTriageRoutingEnabled()) setAmbientBadge(false);

    const auto placement = calculateAmbientAnchor(480, 380);
    setCardVisibility(CardVisibility::Focused);
    emit showCardRequested(placement.x(), placement.y());
}

void HostController::minimizeCard() {
    hideCard();
    m_resumeMinimizedCard = true;
}

void HostController::hideCard() {
    m_resumeMinimizedCard = false;
    m_selectingInsertionTarget = false;
    m_captureOpenPending = false;
    m_captureRequestedApp.clear();
    ++m_captureOpenGeneration;
    if (!setCardVisibility(CardVisibility::Hidden)) {
        return;
    }
    emit hideCardRequested();
}

QPoint HostController::calculateAmbientAnchor(int width, int height) {
    QRect field;
    if (isAmbientActive() && m_ambientObservation) {
        field = m_ambientObservation->field.fieldRect;
    } else if (isAmbientAttached() && m_captureTarget) {
        field = m_captureTarget->field.fieldRect;
    }
    const auto placement = ScreenPlacement::determineReviewPosition(field, QSize(width, height));
    return QPoint(placement.x, placement.y);
}

void HostController::toggleCard() {
    if (m_resumeMinimizedCard && !isCardVisible() && !m_selectingInsertionTarget) {
        showCardInternal(false);
        return;
    }
    const auto frontmost = HostContextDetector::captureFrontmost();
    if (m_selectingInsertionTarget && !isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
        m_selectingInsertionTarget = false;
        showCardInternal(false);
        return;
    }
    if (!isCardVisible()) {
        if (!m_selectingInsertionTarget && hasActiveWork()) {
            showCardInternal(false);
            return;
        }
        if (isAmbientActive() && isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
            beginCaptureInvocation(frontmost);
            return;
        }
        if (isAmbientActive() && m_ambientObservation.has_value() &&
            (m_ambientObservation->field.appIdentifier.compare(frontmost.bundleOrExecutableId, Qt::CaseInsensitive) == 0 ||
             frontmost.bundleOrExecutableId.isEmpty())) {
            openFromAmbient();
            return;
        }
        showCard();
        return;
    }
    if (isAmbientAttached() && !m_selectingInsertionTarget) {
        hideCard();
        return;
    }
    if (!m_selectingInsertionTarget && hasActiveWork()) {
        hideCard();
        return;
    }
    if (isAmbientActive() && isAmbientEligibleApp(frontmost.bundleOrExecutableId)) {
        beginCaptureInvocation(frontmost);
        return;
    }
    if (isAmbientActive() && m_ambientObservation.has_value() &&
        (m_ambientObservation->field.appIdentifier.compare(frontmost.bundleOrExecutableId, Qt::CaseInsensitive) == 0 ||
         frontmost.bundleOrExecutableId.isEmpty())) {
        openFromAmbient();
        return;
    }
    hideCard();
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
    if (m_cancelWorkspaceScan) m_cancelWorkspaceScan();
    m_hostState = HostState::Quitting;
    // Pending watcher recovery belongs to this run; cancel it before anything
    // can restart the watcher during shutdown.
    stopCodebaseWatcher();
    invalidateDownstreamPipeline();
    emit hostStateChanged();
    connect(m_julia.get(), &JuliaRuntime::stopped, this, &HostController::finishQuitWhenRuntimesStopped, Qt::QueuedConnection);
    connect(m_native.get(), &NativeGraniteRuntime::stopped, this, &HostController::finishQuitWhenRuntimesStopped, Qt::QueuedConnection);
    cancelModelDownload();
    m_julia->stop();
    m_native->stop();
    QTimer::singleShot(2000, this, [this]() {
        if (m_hostState == HostState::Quitting && m_storage && !m_storage->closed()) m_storage->cancelOutstanding();
    });
    finishQuitWhenRuntimesStopped();
}

void HostController::finishQuitWhenRuntimesStopped() {
    if (m_hostState == HostState::Quitting && !m_quitSignalSent && !m_julia->isRunning() && !m_native->isRunning() && m_nativeSemanticInFlight == 0 && !m_wordRefinementGenerating) {
        if (m_pendingDraft && !m_storageFault) { flushDraftEdits(); return; }
        if (m_commandRunning || !m_storageCommands.empty() || m_draftWriting || m_draftNotifications > 0 || m_historyRefreshRunning || (m_storage && m_storage->pending())) return;
        if (m_storage && !m_storage->closed()) { m_storage->close([this]() { finishQuitWhenRuntimesStopped(); }); return; }
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
    if (m_accessibility) {
        m_accessibility->setCardVisible(visibility != CardVisibility::Hidden);
    }
    if (visibility != CardVisibility::Hidden) m_ambientBadgeRecovery.stop();
    emit cardVisibilityChanged();
    emit ambientBadgeChanged();
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


AsyncTask<core::Status> HostController::ensureSession(std::string canonicalText, std::int64_t now) {
    if (m_revisionState) {
        co_return Status::success();
    }
    if (!m_repository) {
        co_return storageUnavailable();
    }
    const core::SessionId sessionId(newOpaqueId("sess_"));
    const core::PromptRevision root(sessionId, core::RevisionState::makeRevisionId(sessionId, 1), std::nullopt,
                                    canonicalText, now);
    if (auto st = (co_await m_repository->createSessionWithRootRevision(root, now)); !st.ok()) {
        co_return st;
    }
    m_revisionState = std::make_unique<core::RevisionState>(root);
    emit stateVersionChanged();
    emit sessionListChanged();
    refreshStorageStats();
    co_return Status::success();
}

AsyncTask<core::Status> HostController::commitRevision(std::string canonicalText,
                                      std::uint64_t expectedVersion,
                                      storage::RevisionOrigin origin) {
    if (!m_revisionState) {
        co_return makeError(ErrorCode::PreconditionFailed, "No prompt session is open");
    }
    if (!m_repository) {
        co_return storageUnavailable();
    }
    const std::int64_t now = nowMs();
    core::RevisionState next = *m_revisionState;
    auto newId = next.commitEdit(expectedVersion, canonicalText, now);
    if (!newId.ok()) {
        co_return newId.error();
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
        case storage::RevisionOrigin::Commit: st = (co_await m_repository->insertRevision(write)); break;
        case storage::RevisionOrigin::DraftFreeze: st = (co_await m_repository->insertDraftRevision(write)); break;
        case storage::RevisionOrigin::Restore: st = (co_await m_repository->insertRestoredRevision(write)); break;
        case storage::RevisionOrigin::Root:
            co_return makeError(ErrorCode::Internal, "Root revisions are created with the session");
    }
    if (!st.ok()) {
        co_return st;
    }
    *m_revisionState = std::move(next);
    emit stateVersionChanged();
    emit sessionListChanged();
    refreshStorageStats();
    co_return Status::success();
}

AsyncTask<core::Status> HostController::freezeDraftIfNeeded(std::string canonicalText, std::uint64_t expectedVersion) {
    if (m_revisionState->isStale(expectedVersion)) {
        co_return makeError(ErrorCode::StaleVersion,
                         "Expected state version " + std::to_string(expectedVersion) + " but current is " +
                             std::to_string(m_revisionState->stateVersion()));
    }
    if (!m_revisionState->isDraftDirty() && canonicalText == m_revisionState->currentRevision().text()) {
        co_return Status::success();
    }
    co_return (co_await commitRevision(canonicalText, expectedVersion, storage::RevisionOrigin::DraftFreeze));
}

void HostController::pauseAmbientFollowing() {
    m_followAmbientDraft = false;
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint follow] Ambient following paused by user HUD interaction";
}



AsyncTask<core::Status> HostController::commitPromptOperation(QString text, quint64 expectedVersion) {
        const auto parsedCmd = parseCommandPrefix(text);
        if (parsedCmd.hasCommand) {
            applyAssistanceMode(parsedCmd.mode);
            text = parsedCmd.cleanText;
        }

        const std::string canonical = core::normalizeLineEndings(ss(text));
        if (isBlank(canonical)) {
            co_return makeError(ErrorCode::InvalidInput, "Nothing to save: the prompt is empty.");
        }
        if (!m_revisionState) {
            if (expectedVersion != 0) {
                co_return makeError(ErrorCode::StaleVersion, "The session this edit belonged to is no longer open.");
            }
            if (auto st = (co_await ensureSession(canonical, nowMs())); !st.ok()) {
                co_return st;
            }
            invalidateDownstreamPipeline();
            co_return Status::success();
        }
        if (m_revisionState->isStale(expectedVersion)) {
            co_return makeError(ErrorCode::StaleVersion,
                             "Expected state version " + std::to_string(expectedVersion) + " but current is " +
                                 std::to_string(m_revisionState->stateVersion()));
        }
        if (!m_revisionState->isDraftDirty() && canonical == m_revisionState->currentRevision().text()) {
            co_return Status::success();
        }
        if (auto st = (co_await commitRevision(canonical, expectedVersion, storage::RevisionOrigin::Commit)); !st.ok()) {
            co_return st;
        }
        invalidateDownstreamPipeline();
        co_return Status::success();

}

AsyncTask<core::Status> HostController::hardBypassOperation(QString text, quint64 expectedVersion) {
    if (auto status = co_await commitPromptOperation(text, expectedVersion); !status.ok()) {
        co_return status;
    }

        // The raw text goes out as typed: any analysis or rewrite in flight is dropped unused.
        if (auto status = (co_await advanceCommandVersion(stateVersion())); !status.ok()) co_return status;
        invalidateDownstreamPipeline();
        const auto deliveryTicket = makeTicket();
        const auto deliveryText = currentRevisionText();
        emit stateVersionChanged();
        if (!isTicketCurrent(deliveryTicket, false)) co_return makeError(ErrorCode::StaleVersion, "The draft changed before delivery");
        co_return deliver(deliveryText);

}

// ----------------------------------------------------------------------------------------------
// Pipeline invalidation and tickets
// ----------------------------------------------------------------------------------------------

void HostController::invalidateDownstreamPipeline() {
    m_pendingWordClarificationHandoff.reset();
    clearWordRefinement();
    cancelInterview();
    markComprehensionReviewStale();
    resetClarificationCheck();
    m_selectingInsertionTarget = false;
    ++m_pipelineSerial;
    m_lastAuditResult.reset();
    m_promptTriageLoad.reset();
    if (m_cancelWorkspaceContext) m_cancelWorkspaceContext();
    m_promptTriage.reset();
    m_reviewTrigger = (m_assistanceMode == AssistanceMode::Strict)
        ? analysis::ReviewTrigger::ExplicitReview
        : analysis::ReviewTrigger::Automatic;
    if (!m_recommendedOption.isEmpty() || m_recommendedConfidence > 0.0 ||
        !m_recommendedGroupId.isEmpty() || !m_recommendedAlternativeId.isEmpty()) {
        m_recommendedOption.clear();
        m_recommendedConfidence = 0.0;
        m_recommendedGroupId.clear();
        m_recommendedAlternativeId.clear();
        emit recommendedOptionChanged();
    }
    if (m_routedAnalysisGroup && !m_routedAnalysisGroup->terminal) {
        m_routedAnalysisGroup->superseded = true;
        m_routedAnalysisGroup->terminal = true;
    }
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
    m_creativeAlternatives.clear();
    m_creativeAlternativeAuditExecutionStatuses.clear();
    m_selectedAlternativeIndex = 0;
    emit creativeAlternativesChanged();
    emit selectedAlternativeChanged();
    m_proposalWorkspaceEpoch = 0;
    m_acknowledgedConstraints.clear();
    m_currentApproval.reset();
    clearActiveGroundingBinding(m_activeGroundedEvidence, m_activeGroundingPrompt,
                                m_activeGroundingActiveFile, m_activeGroundingProposalId,
                                m_activeGroundingSessionId, m_activeGroundingRevisionId,
                                m_groundingValidationSerial);
    m_proposalStatus = ProposalUiStatus::None;
    m_proposalMessage.clear();
    m_streamingPreview.clear();
    m_analysisStatus = AnalysisStatus::Idle;
    m_analysisMessage.clear();

    if (hadApproval && m_repository && m_revisionState) {
        auto task = m_repository->invalidateApprovals(core::SessionId(currentSessionId()));
        const QPointer<HostController> owner(this);
        task.then([task, owner]() mutable {
            if (!owner) return;
            try { auto status = task.await_resume(); if (!status.ok()) owner->failStorage(status.error()); } catch (...) {}
        });
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

AsyncTask<core::Status> HostController::persistStateChange(core::RevisionState next, quint64 expectedVersion) {
    if (!m_repository) co_return storageUnavailable();
    if (auto status = (co_await m_repository->advanceSessionState(next, expectedVersion, nowMs())); !status.ok()) co_return status;
    *m_revisionState = next;
    m_currentApproval.reset();
    co_return Status::success();
}

AsyncTask<core::Status> HostController::advanceCommandVersion(quint64 expectedVersion) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
    if (!m_revisionState) co_return Status::success();
    auto next = *m_revisionState;
    if (auto status = next.advanceState(expectedVersion); !status.ok()) co_return status;
    co_return (co_await persistStateChange(next, expectedVersion));
}

AsyncTask<core::Status> HostController::advanceProposalGeneration() {
    if (!m_revisionState) co_return Status::success();
    auto next = *m_revisionState;
    if (auto status = next.noteProposalEdit(stateVersion()); !status.ok()) co_return status;
    co_return (co_await persistStateChange(next, stateVersion()));
}

AsyncTask<core::Status> HostController::invalidateProposalArtifacts() {
    if (auto status = (co_await advanceProposalGeneration()); !status.ok()) co_return status;
    m_lastAuditResult.reset();
    m_pendingNativeRewrite.reset();
    m_currentProposal.reset();
    m_creativeAlternatives.clear();
    m_creativeAlternativeAuditExecutionStatuses.clear();
    m_selectedAlternativeIndex = 0;
    emit creativeAlternativesChanged();
    emit selectedAlternativeChanged();
    m_proposalWorkspaceEpoch = 0;
    m_acknowledgedConstraints.clear();
    m_currentApproval.reset();
    clearActiveGroundingBinding(m_activeGroundedEvidence, m_activeGroundingPrompt,
                                m_activeGroundingActiveFile, m_activeGroundingProposalId,
                                m_activeGroundingSessionId, m_activeGroundingRevisionId,
                                m_groundingValidationSerial);
    m_proposalStatus = ProposalUiStatus::None;
    m_proposalMessage.clear();
    m_streamingPreview.clear();
    markComprehensionReviewStale();
    emit rewriteProposalChanged();
    emit approvalChanged();
    emit intentChanged();
    emit stateVersionChanged();
    co_return Status::success();
}

void HostController::resetSessionState() {
    m_resumeMinimizedCard = false;
    m_followAmbientDraft = false;
    m_widgetDraftEdited = false;
    invalidateDownstreamPipeline();
    m_reviewState = ReviewState::NotRun;
    m_reviewResult = bridge::ComprehensionReviewResult{};
    m_reviewBinding = ComprehensionBinding{};
    m_findingResolutions.clear();
    m_reviewReturnedFindingId.clear();
    ++m_reviewSerial;
    emit comprehensionReviewChanged();
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
    t.workspaceEpoch = m_workspaceEpoch;
    t.reviewTrigger = m_reviewTrigger;
    return t;
}

bool HostController::isTicketCurrent(const PipelineTicket& ticket, bool checkProposalGeneration) const {
    if (!m_revisionState) {
        return false;
    }
    return ticket.serial == m_pipelineSerial && ticket.workspaceEpoch == m_workspaceEpoch &&
           ticket.sessionId == currentSessionId() &&
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

int HostController::analysisGroupBudgetMs() const {
    if (m_thinkingEnabled && m_analysisGroupTimeoutMs == kAnalysisGroupTimeoutMs && nativeGraniteSelected() && !usesTransportOverride()) {
        const auto policy = NativeGraniteRuntime::manifest().value("reasoning").toObject();
        if (policy.value("enabled").toBool())
            return std::clamp(policy.value("analysis_group_timeout_ms").toInt(kAnalysisGroupTimeoutMs),
                              kAnalysisGroupTimeoutMs, 120000);
    }
    if (m_reviewTrigger == analysis::ReviewTrigger::ExplicitReview &&
        m_analysisGroupTimeoutMs == kAnalysisGroupTimeoutMs)
        return 45000;
    return m_analysisGroupTimeoutMs;
}

bridge::GraniteTransport HostController::currentTransport(const std::string& outputSchema, const PipelineTicket* ticket,
    const QString& operation, const QString& prompt, const QString& intentId, const QString& profileId,
    std::shared_ptr<std::atomic_bool> cancellation, const QString& surface) const {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (m_transportOverride) {
        return m_transportOverride;
    }
#endif
    if (nativeGraniteSelected()) {
        if (!ticket || !m_native->isReady()) return {};
        NativeGraniteRuntime::RequestContext context{qs(ticket->analysisGroup ? ticket->analysisGroup->id : ticket->attemptId), qs(ticket->attemptId), qs(ticket->sessionId),
            qs(ticket->revisionId), ticket->stateVersion, prompt, intentId, profileId, std::move(cancellation), m_thinkingEnabled};
        context.surface = surface;
        if (ticket->analysisGroup && !ticket->analysisGroup->deadline.isForever())
            context.deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::max<qint64>(0, ticket->analysisGroup->deadline.remainingTime()));
        return m_native->transportFor(context, operation.isEmpty() ? QStringLiteral("granite_analyze") : operation, outputSchema);
    }
    if (!m_ollama || m_activeModel.isEmpty()) {
        return {};
    }
    return m_ollama->transportFor(m_activeModel, OllamaClient::kDefaultGenerateTimeoutMs, outputSchema);
}

AsyncTask<core::Status> HostController::beginAttempt(PipelineTicket& ticket, const char* stage) {
    if (!m_repository) {
        co_return storageUnavailable();
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
                "; template=" + analysis::kPromptTriageTemplateId + "; threshold=0.70; broad_heads_and_corroborated_goal_check";
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
        ticket.configuration += std::string("; thinking_enabled=") + (m_thinkingEnabled ? "true" : "false") + "; " + ss(m_native->configurationDetail()) + "; residency_policy=GraniteResidencyPolicyV1; residency_preset=" +
                                ss(m_graniteResidencyPreset) + "; residency_tier=" + ss(m_native->residency());
    if (ticket.analysisGroup)
        ticket.configuration += "; request_group_id=" + ticket.analysisGroup->id + "; analysis_group_budget_ms=" + std::to_string(ticket.analysisGroup->budgetMs);
    ticket.configuration += ticket.reviewTrigger == analysis::ReviewTrigger::ExplicitReview
        ? "; review_trigger=explicit_review" : "; review_trigger=automatic";
    record.modelDetail = ticket.configuration;
    record.startedAtMs = nowMs();
    if (auto st = (co_await m_repository->beginModelAttempt(record)); !st.ok()) {
        co_return st;
    }
    ticket.attemptId = record.attemptId;
    if (!isTicketCurrent(ticket, false)) {
        completeAttempt(ticket, storage::AttemptStatus::DiscardedStale, "Prompt changed while recording the request");
        co_return makeError(ErrorCode::StaleVersion, "The prompt changed before the request started");
    }
    if (ticket.analysisGroup) {
        ticket.analysisGroup->activeAttemptId = ticket.attemptId;
        ticket.analysisGroup->activeConfiguration = ticket.configuration;
        ticket.analysisGroup->activeModel = ticket.model;
    }
    co_return Status::success();
}

AsyncTask<void> HostController::completeAttempt(PipelineTicket ticket,
                                     storage::AttemptStatus status,
                                     std::string errorMessage,
                                     const bridge::GraniteModelInfo* model) {
    if (ticket.attemptId.empty() || !m_repository) {
        co_return;
    }
    const bridge::GraniteModelInfo info = model ? *model : ticket.model;
    if (auto st = (co_await m_repository->completeModelAttempt(ticket.attemptId, status, info.modelId,
                                                     modelDetail(info) + "; " + ticket.configuration,
                                                     errorMessage.substr(0, 1024), nowMs()));
        !st.ok()) {
        setLastError(st.error());
    }

    finishQuitWhenRuntimesStopped();
    co_return;
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

AsyncTask<core::Status> HostController::cancelAnalysisOperation(quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (!isAnalyzing()) co_return Status::success();
        if (auto status = (co_await advanceCommandVersion(expectedVersion)); !status.ok()) co_return status;
        invalidateDownstreamPipeline();
        emit stateVersionChanged();
        setAnalysisStatus(AnalysisStatus::Idle, QStringLiteral("Analysis cancelled. Your draft remains available; choose Analyze to retry."));
        co_return Status::success();

}

AsyncTask<core::Status> HostController::analyzePromptOperation(QString text, quint64 expectedVersion, analysis::ReviewTrigger trigger) {
        const auto parsedCmd = parseCommandPrefix(text);
        if (parsedCmd.hasCommand) {
            applyAssistanceMode(parsedCmd.mode);
            text = parsedCmd.cleanText;
            if (parsedCmd.mode == AssistanceMode::Strict) {
                trigger = analysis::ReviewTrigger::ExplicitReview;
            }
        }

        const std::string canonical = core::normalizeLineEndings(ss(text));
        if (isBlank(canonical)) {
            co_return makeError(ErrorCode::InvalidInput, "Type a prompt before analyzing it.");
        }
        if (!m_revisionState) {
            if (expectedVersion != 0) {
                co_return makeError(ErrorCode::StaleVersion, "The session this edit belonged to is no longer open.");
            }
            if (auto st = (co_await ensureSession(canonical, nowMs())); !st.ok()) {
                co_return st;
            }
        } else if (auto st = (co_await freezeDraftIfNeeded(canonical, expectedVersion)); !st.ok()) {
            co_return st;
        }
        if (auto status = (co_await advanceCommandVersion(stateVersion())); !status.ok()) co_return status;
        invalidateDownstreamPipeline();
        m_reviewTrigger = trigger;
        emit stateVersionChanged();
        co_return (co_await startDiscovery());

}

bool HostController::usesTransportOverride() const noexcept {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    return static_cast<bool>(m_transportOverride);
#else
    return false;
#endif
}

AsyncTask<core::Status> HostController::startDiscovery(bool allowReconnect) {
    if (!m_revisionState) {
        co_return makeError(ErrorCode::PreconditionFailed, "No prompt session is open");
    }
    if (promptTriageRoutingEnabled() && !m_promptTriage) co_return (co_await startPromptTriage());
    if (promptTriageRoutingEnabled() && (!m_routedAnalysisGroup || m_routedAnalysisGroup->terminal ||
        (m_reviewTrigger != analysis::ReviewTrigger::ExplicitReview &&
         analysis::routePromptTriage(*m_promptTriage).value() != analysis::PromptTriageRoute::Clarify)))
        co_return makeError(ErrorCode::PreconditionFailed, "Granite discovery requires current ambiguous S1 classification");
    // Exact quantities do not prove the absence of a conflict. Preserve S1's
    // Clarify route when the conservative deterministic extractor finds nothing;
    // discovery and its semantic audit must evaluate the actual requirements.
    if (promptTriageRoutingEnabled() && !nativeGraniteSelected() && !usesTransportOverride()) {
        m_routedAnalysisGroup->terminal = true;
        const auto reason = QStringLiteral("This flow requires pinned IBM Granite 3B. Choose Native Granite in Settings.");
        setAnalysisStatus(AnalysisStatus::Failed, reason);
        co_return makeError(ErrorCode::ProviderUnavailable, ss(reason));
    }
    const auto runtime = m_graniteAdapter->state();
    if (runtime == bridge::GraniteRuntimeState::Busy) {
        if (m_routedAnalysisGroup) m_routedAnalysisGroup->pause();
        m_discoveryQueued = true;
        setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Waiting for the model to finish an earlier request."));
        co_return Status::success();
    }
    if (allowReconnect && nativeGraniteSelected() && !usesTransportOverride() &&
        (runtime != bridge::GraniteRuntimeState::Ready || !m_native->isReady())) {
        if (m_routedAnalysisGroup) m_routedAnalysisGroup->pause();
        m_discoveryQueued = true;
        setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Warming the pinned native model. Editing and the widget remain available."));
        prepareNativeModel();
        co_return Status::success();
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
        co_return Status::success();
    }
    m_discoveryQueued = false;

    PipelineTicket ticket = makeTicket();
    ticket.analysisGroup = m_routedAnalysisGroup ? m_routedAnalysisGroup : std::make_shared<AnalysisGroup>(analysisGroupBudgetMs());
    if (ticket.analysisGroup->id.empty()) ticket.analysisGroup->id = newOpaqueId("grp_");
    ticket.analysisGroup->resume();
    if (auto st = (co_await beginAttempt(ticket, "discovery")); !st.ok()) {
        ticket.analysisGroup->terminal = true;
        setAnalysisStatus(AnalysisStatus::Failed, plainError(st.error()));
        co_return st;
    }

    const std::size_t maximumQuestions = m_interviewActive
        ? 1
        : (ticket.analysisGroup->deadline.isForever() ||
           ticket.analysisGroup->deadline.remainingTime() > 60000 ? 2 : 1);
    const auto transport = currentTransport(bridge::discoveryResponseSchema(m_revisionState->currentRevision().text(), maximumQuestions), &ticket,
        QStringLiteral("granite_analyze"), qs(m_revisionState->currentRevision().text()), {}, {}, {},
        m_interviewActive ? QStringLiteral("interview") : QString());
    if (runtime != bridge::GraniteRuntimeState::Ready || !transport) {
        std::string reason;
        if (!transport) {
            reason = "No local model runtime is configured.";
        } else if (m_activeModel.isEmpty()) {
            reason = "No model is selected. Choose one in Settings.";
        } else if (!m_graniteAdapter->lastError().empty()) {
            reason = "The local model isn't available. Retry explicitly or keep the original prompt.";
        } else {
            reason = "The local model isn't connected yet.";
        }
        completeAttempt(ticket, storage::AttemptStatus::Failed, reason);
        ticket.analysisGroup->terminal = true;
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis unavailable. ") + qs(reason));
        co_return makeError(ErrorCode::ProviderUnavailable, reason);
    }

    bridge::GraniteOptionDiscoveryRequest request;
    request.sessionId = ticket.sessionId;
    request.revisionId = ticket.revisionId;
    request.promptString = m_revisionState->currentRevision().text();
    request.hostContextCategory = ss(m_hostContext.categoryString());
    request.reviewTrigger = ticket.reviewTrigger;
    request.maximumQuestions = maximumQuestions;
    request.substantialInterview = m_interviewActive;
    if (promptTriageRoutingEnabled()) request.promptTriage = m_promptTriage;

    for (const auto& f : m_projectFiles) {
        request.workspaceFiles.push_back(f.toStdString());
        if (request.workspaceFiles.size() >= 25) break;
    }

    const QString pText = qs(request.promptString);
    static const QRegularExpression codeExtRe(QStringLiteral("\\.(?:html|css|js|ts|tsx|jsx|qml|cpp|hpp|c|h|py|rs|go|java|swift|json|yaml|sql)\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression codeSyntaxRe(QStringLiteral("[{}<>()\\[\\];:=#]"));
    static const QRegularExpression codeKeywordRe(QStringLiteral("\\b(?:function|class|const|let|var|import|export|co_return|struct|enum|void|public|private)\\b"));
    request.isNonTechnicalPrompt = !codeExtRe.match(pText).hasMatch() &&
                                   !codeKeywordRe.match(pText).hasMatch() &&
                                   (pText.count(codeSyntaxRe) < 3);

    // Stage-specific audit transport. It is deliberately separate from the discovery schema so
    // the audit cannot be mistaken for a second discovery generation.
    // The native worker exposes the bounded analysis operation for stage audits. The audit's
    // instruction contract and schema are distinct; only the runtime operation is shared.
    const auto auditTransport = currentTransport(bridge::goldfishDiscoveryAuditResponseSchema(), &ticket,
        QStringLiteral("granite_analyze"), qs(request.promptString));
    const QDeadlineTimer stageDeadline = ticket.analysisGroup
        ? ticket.analysisGroup->deadline : QDeadlineTimer(QDeadlineTimer::Forever);
    request.canContinue = [stageDeadline] { return !stageDeadline.hasExpired(); };

    setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Analyzing your prompt."));
    armAnalysisDeadline(ticket);
    if (ticket.analysisGroup->deadline.hasExpired()) { expireAnalysisGroup(ticket); co_return Status::success(); }
    emit modelEngineStatusChanged();

    using DiscoveryResult = core::Result<bridge::GraniteOptionDiscoveryResponse>;
    auto adapter = m_graniteAdapter;
    auto grounding = std::make_shared<GroundedPromptContext>();
    const auto groundingInput = groundingInputs(request.promptString);
    auto* watcher = new QFutureWatcher<DiscoveryResult>(this);
    ++m_inFlight;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    if (nativeWork) ++m_nativeSemanticInFlight;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, ticket, nativeWork, grounding]() {
        --m_inFlight;
        if (nativeWork) --m_nativeSemanticInFlight;
        const DiscoveryResult result = watcher->result();
        watcher->deleteLater();
        // The grounding view belongs to the ticket that produced it. A stale
        // ticket must not paint evidence into a newer workspace/revision.
        if (isTicketCurrent(ticket, false)) applyGroundingContext(*grounding);
        onDiscoveryFinished(ticket, result);
        finishQuitWhenRuntimesStopped();
    });
    watcher->setFuture(QtConcurrent::run([adapter, request, transport, auditTransport, groundingInput, grounding,
                                         stageDeadline]() mutable -> DiscoveryResult {
        try {
            *grounding = computeGroundedPromptContext(groundingInput);
            request.groundedCanonicalIntentXml = grounding->xml;
            auto discovered = adapter->discoverOptions(request, transport);
            if (!discovered.ok()) return discovered;

            auto& response = discovered.value();
            if (stageDeadline.hasExpired()) {
                response.discoveryAuditStatus = core::AuditExecutionStatus::Unavailable;
                response.discoveryAuditOutcome = core::ValidationOutcome::Unresolved;
                return discovered;
            }

            bridge::GraniteGoldfishAdapter goldfish;
            bridge::GoldfishDiscoveryAuditRequest auditRequest;
            auditRequest.sessionId = request.sessionId;
            auditRequest.originalPrompt = request.promptString;
            auditRequest.proposedAnalysis = response.analysis;

            bridge::GraniteGoldfishAdapter::DiscoveryRegeneration regenerate;
            if (auditTransport) {
                regenerate = [adapter, request, transport, stageDeadline, expectedKind = response.assistanceKind](
                    const bridge::GoldfishDiscoveryAuditResult& findings) mutable
                    -> core::Result<core::analysis::NormalizedAnalysis> {
                    if (stageDeadline.hasExpired()) {
                        return makeError(ErrorCode::ProviderUnavailable,
                                         "The shared discovery deadline expired before regeneration");
                    }
                    std::string feedback;
                    for (const auto& violation : findings.violations) {
                        feedback += "- " + violation.itemId + ": " + violation.explanation + "\n";
                        if (feedback.size() > 1536) break;
                    }
                    for (const auto& unresolved : findings.unresolved) {
                        feedback += "- " + unresolved.itemId + ": " + unresolved.explanation + "\n";
                        if (feedback.size() > 1536) break;
                    }
                    auto repairRequest = request;
                    repairRequest.repairFeedback = std::move(feedback);
                    auto regenerated = adapter->discoverOptions(repairRequest, transport);
                    if (!regenerated.ok()) return regenerated.error();
                    if (regenerated.value().assistanceKind != expectedKind)
                        return makeError(ErrorCode::ValidationFailed,
                                         "Discovery repair changed optionality; explicit new review required");
                    return std::move(regenerated).value().analysis;
                };
            }

            const auto budgetMs = stageDeadline.remainingTime() == -1
                ? std::int64_t{25000} : std::max<std::int64_t>(1, stageDeadline.remainingTime());
            auto repaired = goldfish.correctDiscoveryWithRepairLoop(auditRequest, auditTransport,
                                                                    1, budgetMs, regenerate);
            response.analysis = std::move(repaired.analysis);
            // The analysis identity must cover the repaired content, not the pre-audit snapshot.
            analysis::sealSnapshot(response.analysis);
            response.discoveryAuditStatus = repaired.auditVerdict.executionStatus;
            response.discoveryAuditOutcome = repaired.auditVerdict.outcome;
            if (response.analysis.choiceGroups.empty() && response.analysis.missingDecisions.empty() &&
                response.analysis.conflicts.empty() && response.analysis.assumptions.empty()) {
                // Pruning must never be reported as proof that the prompt is sufficient. If the
                // original answer contained a decision, keep it visible as unresolved.
                if (!auditRequest.proposedAnalysis.choiceGroups.empty()) {
                    for (const auto& group : auditRequest.proposedAnalysis.choiceGroups) {
                        core::analysis::MissingDecision missing;
                        missing.id = core::analysis::mintMissingDecisionId(response.analysis.missingDecisions.size());
                        missing.question = group.question;
                        missing.blocking = false;
                        response.analysis.missingDecisions.push_back(std::move(missing));
                    }
                }
                if (response.discoveryAuditOutcome != core::ValidationOutcome::Pass) {
                    response.outcome = bridge::DiscoveryOutcome::NeedsClarification;
                }
            }
            // Cover every audit/repair mutation in the identity before the host validates it.
            analysis::sealSnapshot(response.analysis);
            return discovered;
        } catch (const std::exception& ex) {
            return makeError(ErrorCode::Internal, std::string("Discovery crashed: ") + ex.what());
        }
    }));
    co_return Status::success();
}

void HostController::onDiscoveryFinished(PipelineTicket ticket, core::Result<bridge::GraniteOptionDiscoveryResponse> result) {
    queueModelCompletion("finishDiscovery", [this, ticket = std::move(ticket), result = std::move(result)]() mutable {
        return onDiscoveryFinishedAsync(std::move(ticket), std::move(result));
    });
}

AsyncTask<void> HostController::onDiscoveryFinishedAsync(PipelineTicket ticket,
                                         core::Result<bridge::GraniteOptionDiscoveryResponse> result) {
    if (m_healthRecheckRequested && !m_healthCheckInFlight) refreshAvailableModels();
    emit modelEngineStatusChanged();
    if (m_ambientCheckPending) {
        m_ambientDebounce.start();
    }
    if (ticket.analysisGroup && ticket.analysisGroup->terminal) {
        if (ticket.analysisGroup->superseded)
            completeAttempt(ticket, storage::AttemptStatus::DiscardedStale,
                            "The prompt changed before analysis finished");
        if (m_discoveryQueued) {
            if (auto status = (co_await startDiscovery()); !status.ok()) setLastError(status.error());
        }
        co_return;
    }
    if (!isTicketCurrent(ticket, false)) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::DiscardedStale,
                        "The prompt changed before analysis finished");
        if (m_discoveryQueued) {
            if (auto st = (co_await startDiscovery()); !st.ok()) {
                setLastError(st.error());
            }
        }
        co_return;
    }

    if (ticket.analysisGroup && ticket.analysisGroup->deadline.hasExpired()) { expireAnalysisGroup(ticket); co_return; }

    if (!result.ok()) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::Failed, result.error().message);
        const auto error = modelReplyError(result.error());
        setAnalysisStatus(AnalysisStatus::Failed, plainError(error));
        setLastError(error);
        co_return;
    }
    (co_await acceptDiscovery(ticket, result.value()));

    co_return;
}

AsyncTask<void> HostController::acceptDiscovery(PipelineTicket ticket, bridge::GraniteOptionDiscoveryResponse response) {
    const std::string frozenText = m_revisionState->currentRevision().text();
    if (response.analysis.sessionId != ticket.sessionId || response.analysis.baseRevisionId != ticket.revisionId ||
        !analysis::isSnapshotSealed(response.analysis)) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Analysis is not bound to the frozen revision",
                        &response.model);
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis failed: the result did not match this prompt."));
        co_return;
    }
    // Discovery-stage auditing and bounded repair already ran on the worker thread, inside the
    // same shared deadline as generation. Do not prune here: an unresolved decision must never
    // disappear after the audit, and a failed audit must not become NoAmbiguity.
    const auto validation = analysis::validateNormalizedAnalysis(response.analysis, frozenText);
    if (validation.status == analysis::AnalysisValidationStatus::Invalid) {
        if (ticket.analysisGroup) ticket.analysisGroup->terminal = true;
        std::string issues;
        for (const auto& issue : validation.issues) {
            issues += (issues.empty() ? "" : "; ") + issue;
        }
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Invalid analysis: " + issues, &response.model);
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Analysis failed: the model's answer was inconsistent."));
        co_return;
    }
    completeAttempt(ticket, storage::AttemptStatus::Succeeded, std::string(), &response.model);

    const bool auditVerified = response.discoveryAuditStatus == core::AuditExecutionStatus::Completed &&
                               response.discoveryAuditOutcome == core::ValidationOutcome::Pass;

    // A mechanically exact edit (quoted old/new values, named file, explicit boundary) is already
    // settled. If a verified audit still leaves only a confirmation question, keep the user's
    // exact instruction instead of manufacturing a decision.
    if (auditVerified && response.analysis.choiceGroups.empty() && response.analysis.conflicts.empty() &&
        response.analysis.assumptions.empty() && response.analysis.missingDecisions.size() == 1 &&
        hasExplicitMechanicalEdit(frozenText)) {
        response.analysis.missingDecisions.clear();
        response.outcome = bridge::DiscoveryOutcome::NoAmbiguity;
        response.assistanceKind = core::analysis::AssistanceKind::None;
        analysis::sealSnapshot(response.analysis);
    }

    const QString auditCaveat = auditVerified ? QString()
        : QStringLiteral(" Roclint couldn't independently verify that no open question remains.");
    if (promptTriageRoutingEnabled()) {
        ticket.analysisGroup->terminal = true;
        const bool noQuestions = response.outcome == bridge::DiscoveryOutcome::NoAmbiguity &&
            response.analysis.choiceGroups.empty() && response.analysis.assumptions.empty() &&
            response.analysis.missingDecisions.empty() && response.analysis.conflicts.empty() &&
            auditVerified;
        if ((co_await applyAnalysis(response.analysis, ticket.attemptId, {}, std::nullopt, m_promptTriage, response.assistanceKind)))
            setAnalysisStatus(AnalysisStatus::Ready, noQuestions
                ? QStringLiteral("No material clarification was identified. Your original wording is unchanged.")
                : isManual() ? QStringLiteral("Roclint couldn't offer a reliable suggestion. Review again or keep your original wording.") + auditCaveat
                : isOptionalHelp()
                    ? QStringLiteral("Optional help is available. You can keep your original prompt.") + auditCaveat
                    : QStringLiteral("One detail could change the result. Your original prompt is unchanged.") + auditCaveat);
        co_return;
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
    message += auditCaveat;
    if (response.ungroundedEvidenceCount > 0) {
        message += QStringLiteral(" %1 suggestion(s) were set aside because they quoted text that isn't in your prompt.")
                       .arg(response.ungroundedEvidenceCount);
    }
    const auto scoringPlan = analysis::buildScoringPlan(response.analysis);
    const bool hasScoringCandidates = !scoringPlan.groupIds.empty() || !scoringPlan.assumptionIds.empty();
    if (ticket.analysisGroup && ticket.analysisGroup->deadline.hasExpired()) {
        ticket.analysisGroup->terminal = true;
        auto evaluation = bridge::S1TriageAdapter::degradedResponse(response.analysis, ss(analysisDeadlineReason()));
        if ((co_await applyAnalysis(response.analysis, ticket.attemptId, {}, std::move(evaluation))))
            setAnalysisStatus(AnalysisStatus::Ready, message + QLatin1Char(' ') + analysisDeadlineReason());
        co_return;
    }
    if (m_s1Adapter.engine() == bridge::ScorerEngine::JuliaLocal && m_julia->isReady() && hasScoringCandidates) {
        auto scoreTicket = ticket;
        scoreTicket.attemptId.clear();
        if (auto st = (co_await beginAttempt(scoreTicket, "julia_scoring")); st.ok()) {
            scorerAttemptId = scoreTicket.attemptId;
            const JuliaRuntime::RequestContext context{qs(ticket.analysisGroup ? ticket.analysisGroup->id : ticket.attemptId), qs(scoreTicket.attemptId),
                qs(scoreTicket.sessionId), qs(scoreTicket.revisionId), scoreTicket.stateVersion};
            const auto requestId = m_julia->evaluate(response.analysis, qs(frozenText), context);
            if (requestId != 0) {
                m_pendingJulia.insert(requestId, PendingJuliaScore{scoreTicket, response.analysis, message, ticket.attemptId});
                ++m_inFlight;
                setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Julia is scoring the supplied choices and material assumptions locally."));
                co_return;
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
    if ((co_await applyAnalysis(response.analysis, ticket.attemptId, scorerAttemptId))) setAnalysisStatus(AnalysisStatus::Ready, message);

    co_return;
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
    queueModelCompletion("finishJulia", [this, pending = std::move(pending),
        response = response ? std::optional(*response) : std::nullopt, error]() mutable {
        return onJuliaFinishedAsync(std::move(pending), std::move(response), error);
    });
}
AsyncTask<void> HostController::onJuliaFinishedAsync(PendingJuliaScore pending, std::optional<bridge::SnapshotEvaluationResponse> response, QString error) {
    if (pending.ticket.analysisGroup) {
        pending.ticket.analysisGroup->terminal = true;
        if (pending.ticket.analysisGroup->deadline.hasExpired()) response = std::nullopt;
    }
    if (!isTicketCurrent(pending.ticket, false) || m_s1Adapter.engine() != bridge::ScorerEngine::JuliaLocal) {
        completeAttempt(pending.ticket, storage::AttemptStatus::DiscardedStale, "Prompt or scorer settings changed before Julia finished");
        co_return;
    }
    const auto failure = pending.ticket.analysisGroup && pending.ticket.analysisGroup->deadline.hasExpired() ? analysisDeadlineReason() : error;
    auto evaluation = response ? bridge::S1TriageAdapter::validateLocalEvaluation(pending.analysis, *response)
                               : bridge::S1TriageAdapter::degradedResponse(pending.analysis, ss(failure));
    evaluation.engine = bridge::ScorerEngine::JuliaLocal;
    const bool ranked = evaluation.isRanked();
    completeAttempt(pending.ticket, ranked ? storage::AttemptStatus::Succeeded : storage::AttemptStatus::Failed,
                    ranked ? std::string() : evaluation.degradedReason);
    // One validated recommendation policy. The worker's ambiguity-aware confidence is recomputed
    // by the same shared adapter function from the sanitized probabilities and the validated
    // ambiguity score; the host does not invent a second confidence formula.
    m_recommendedOption.clear();
    m_recommendedConfidence = 0.0;
    m_recommendedGroupId.clear();
    m_recommendedAlternativeId.clear();
    if (const auto recommendation = bridge::S1TriageAdapter::extractJustifiedRecommendation(pending.analysis, evaluation);
        recommendation.has_value() && recommendation->isJustified) {
        m_recommendedOption = qs(recommendation->alternativeLabel);
        m_recommendedConfidence = recommendation->confidence;
        m_recommendedGroupId = qs(recommendation->groupId);
        m_recommendedAlternativeId = qs(recommendation->alternativeId);
        emit recommendedOptionChanged();
    }

    const auto detail = ranked ? QStringLiteral(" Julia scored the supplied decisions locally. Confirm your interpretation explicitly.")
                               : QStringLiteral(" Julia scoring was unavailable; options remain unranked. ") + qs(evaluation.degradedReason);
    if ((co_await applyAnalysis(std::move(pending.analysis), std::move(pending.discoveryAttemptId), pending.ticket.attemptId, std::move(evaluation), m_promptTriage)))
        setAnalysisStatus(AnalysisStatus::Ready, pending.message + detail);

    co_return;
}

AsyncTask<bool> HostController::applyAnalysis(analysis::NormalizedAnalysis normalized, std::string discoveryAttemptId,
                                    std::string scorerAttemptId,
                                    std::optional<bridge::SnapshotEvaluationResponse> evaluation,
                                    std::optional<analysis::PromptTriage> triage, analysis::AssistanceKind assistance) {
    const auto operationWorkspaceEpoch = m_workspaceEpoch;
    const auto operationPipelineSerial = m_pipelineSerial;
    auto scored = evaluation ? std::move(*evaluation) :
        triage ? bridge::SnapshotEvaluationResponse{} : m_s1Adapter.evaluateSnapshot(normalized);
#if defined(ROCLINT_ENABLE_TEST_MOCKS)
    if (scored.executionStatus == bridge::ExecutionStatus::ExecutedMock && scorerAttemptId.empty()) {
        auto ticket = makeTicket();
        if (auto status = (co_await beginAttempt(ticket, "test_mock_scoring")); !status.ok()) {
            setAnalysisStatus(AnalysisStatus::Failed, plainError(status.error()));
            setLastError(status.error());
            co_return false;
        }
        scorerAttemptId = ticket.attemptId;
        ticket.model.modelId = scored.modelVersion;
        completeAttempt(ticket, storage::AttemptStatus::Succeeded, "");
    }
#endif
    auto result = triage ? analysis::buildRoutedAnalysisSnapshot(std::move(normalized), std::move(*triage),
        m_revisionState->currentRevision().text(), std::move(discoveryAttemptId), evaluation,
        analysis::ReviewPolicy{m_reviewTrigger, assistance}) :
        analysis::buildAnalysisSnapshot(std::move(normalized), std::move(scored), m_revisionState->currentRevision().text(),
            std::move(discoveryAttemptId), std::move(scorerAttemptId));
    if (!result.ok()) {
        setAnalysisStatus(AnalysisStatus::Failed, plainError(result.error()));
        setLastError(result.error());
        co_return false;
    }
    auto nextState = *m_revisionState;
    const auto observedVersion = stateVersion();
    if (auto status = nextState.advanceState(observedVersion); !status.ok()) { setLastError(status.error()); co_return false; }
    if (auto status = (co_await m_repository->insertAnalysisSnapshot(result.value(), nowMs(), observedVersion)); !status.ok()) {
        setAnalysisStatus(AnalysisStatus::Failed, plainError(status.error()));
        setLastError(status.error());
        co_return false;
    }
    const bool stillCurrent = operationWorkspaceEpoch == m_workspaceEpoch &&
                              operationPipelineSerial == m_pipelineSerial;
    // insertAnalysisSnapshot advanced the persisted session state. Keep the
    // in-memory revision version aligned even when a workspace/scope change
    // means the analysis itself must not be published.
    *m_revisionState = std::move(nextState);
    if (!stillCurrent) co_return false;
    m_currentSnapshot = std::move(result).value();
    m_currentAnalysis = m_currentSnapshot->normalized;
    m_currentSnapshotEval = m_currentSnapshot->evaluation;
    m_currentPolicyResult = analysis::MaterialityPolicyV1::evaluatePrompt(*m_currentAnalysis, m_revisionState->currentRevision().text());
    m_workingIntent = intent::WorkingIntentState{};
    m_confirmedIntent.reset();
    (co_await reevaluateIntervention());
    if (operationWorkspaceEpoch != m_workspaceEpoch || operationPipelineSerial != m_pipelineSerial) co_return false;
    emit stateVersionChanged();
    co_return true;
}

AsyncTask<void> HostController::reevaluateIntervention() {
    if (!m_currentAnalysis) {
        m_currentDecision = intent::InterventionDecision{};
        refreshActiveItemPresentation();
        emit interventionChanged();
        emit intentChanged();
        co_return;
    }
    if (auto status = co_await commitPendingWordClarification(); !status.ok()) {
        setLastError(status.error());
        co_return;
    }
    m_currentDecision = intent::InterventionStateMachine::evaluate(*m_currentAnalysis, m_currentPolicyResult,
                                                                   m_currentSnapshotEval, m_workingIntent);
    intent::InterventionStateMachine::recordPresented(m_currentDecision, m_workingIntent);
    refreshActiveItemPresentation();
    if (auto st = (co_await finalizeIntentIfQuiet()); !st.ok()) {
        setLastError(st.error());
    }
    emit interventionChanged();
    emit intentChanged();
    emit intentBriefChanged();
    if (m_interviewActive) advanceInterview();

    co_return;
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
            if (!m_recommendedOption.isEmpty()) {
                std::stable_sort(m_activeAlternatives.begin(), m_activeAlternatives.end(),
                                [&](const auto& lhs, const auto& rhs) {
                    const auto mapL = lhs.toMap();
                    const auto mapR = rhs.toMap();
                    const QString labelL = mapL.value(QStringLiteral("label")).toString();
                    const QString labelR = mapR.value(QStringLiteral("label")).toString();
                    const bool matchL = labelL.contains(QStringLiteral("Safe Rule-Compliant"), Qt::CaseInsensitive) ||
                                        (!m_recommendedOption.isEmpty() && (m_recommendedOption.contains(labelL, Qt::CaseInsensitive) || labelL.contains(m_recommendedOption, Qt::CaseInsensitive)));
                    const bool matchR = labelR.contains(QStringLiteral("Safe Rule-Compliant"), Qt::CaseInsensitive) ||
                                        (!m_recommendedOption.isEmpty() && (m_recommendedOption.contains(labelR, Qt::CaseInsensitive) || labelR.contains(m_recommendedOption, Qt::CaseInsensitive)));
                    if (matchL != matchR) return matchL > matchR;
                    return false;
                });
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
                    if (!m.suggestedAlternativeEvidence.empty()) {
                        m_activeQuestion += QStringLiteral("\n\nAlternative evidence: ") + qs(m.suggestedAlternativeEvidence);
                    }
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

AsyncTask<core::Status> HostController::finalizeIntentIfQuiet() {
    if (m_currentDecision.verdict != intent::InterventionVerdict::Quiet || !m_currentAnalysis) {
        m_confirmedIntent.reset();
        co_return Status::success();
    }
    if (m_confirmedIntent) {
        co_return Status::success();
    }
    if (!m_repository) {
        co_return storageUnavailable();
    }
    const auto operationWorkspaceEpoch = m_workspaceEpoch;
    const auto operationPipelineSerial = m_pipelineSerial;
    const std::int64_t now = nowMs();
    auto built = intent::IntentProvenanceBuilder::buildConfirmedIntent(
        *m_currentSnapshot, m_currentPolicyResult, m_workingIntent, now, supplementalInterviewDecisions());
    if (!built.ok()) {
        co_return built.error();
    }
    if (auto st = (co_await m_repository->insertIntentVersion(built.value(), now)); !st.ok()) {
        co_return st;
    }
    if (operationWorkspaceEpoch != m_workspaceEpoch || operationPipelineSerial != m_pipelineSerial ||
        !m_currentAnalysis || !m_currentSnapshot) {
        // The workspace/scope changed or the analysis was cancelled while the
        // intent row was written. The current UI must not inherit a confirmed
        // intent derived from old grounding/analysis state.
        co_return Status::success();
    }
    m_confirmedIntent = std::move(built).value();
    co_return Status::success();
}

AsyncTask<core::Status> HostController::applyTransition(const char* /*name*/, quint64 expectedVersion,
                                     std::function<Status(intent::WorkingIntentState&)> transition,
                                     bool advanceInterviewAfter) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (!m_currentAnalysis || !m_revisionState) {
            co_return makeError(ErrorCode::PreconditionFailed, "There's nothing to answer yet. Analyze the prompt first.");
        }
        if (m_revisionState->isDraftDirty()) {
            co_return makeError(ErrorCode::StaleVersion, "The prompt changed after it was analyzed. Analyze it again.");
        }
        const auto operationWorkspaceEpoch = m_workspaceEpoch;
        const auto operationPipelineSerial = m_pipelineSerial;
        const auto operationEditSerial = m_liveEditSerial;
        intent::WorkingIntentState next = m_workingIntent;
        if (auto st = transition(next); !st.ok()) {
            co_return st;
        }
        auto decision = intent::InterventionStateMachine::evaluate(*m_currentAnalysis, m_currentPolicyResult, m_currentSnapshotEval, next);
        intent::InterventionStateMachine::recordPresented(decision, next);
        auto nextState = *m_revisionState;
        const bool clearProposal = m_currentProposal.has_value() || m_proposalStatus == ProposalUiStatus::Generating;
        const auto advanced = clearProposal ? nextState.noteProposalEdit(expectedVersion) : nextState.advanceState(expectedVersion);
        if (!advanced.ok()) co_return advanced;
        std::optional<intent::ConfirmedIntent> confirmed;
        if (decision.verdict == intent::InterventionVerdict::Quiet) {
            auto result = intent::IntentProvenanceBuilder::buildConfirmedIntent(
                *m_currentSnapshot, m_currentPolicyResult, next, nowMs(), supplementalInterviewDecisions());
            if (!result.ok()) co_return result.error();
            confirmed = std::move(result).value();
            if (auto status = (co_await m_repository->insertIntentVersion(*confirmed, nowMs(), expectedVersion)); !status.ok()) co_return status;
        } else {
            if (auto status = (co_await m_repository->advanceSessionState(nextState, expectedVersion, nowMs())); !status.ok()) co_return status;
        }
        const bool stillCurrent = operationWorkspaceEpoch == m_workspaceEpoch && operationPipelineSerial == m_pipelineSerial;
        if (operationEditSerial == m_liveEditSerial) *m_revisionState = std::move(nextState);
        if (!stillCurrent) {
            // The persisted revision state advanced; do not publish the old
            // workspace's working/confirmed intent into the new epoch.
            emit stateVersionChanged();
            co_return Status::success();
        }
        m_workingIntent = std::move(next);
        m_followAmbientDraft = false; // An explicit decision freezes the reviewed draft and destination.
        m_currentDecision = std::move(decision);
        m_confirmedIntent = std::move(confirmed);
        m_currentApproval.reset();
        if (clearProposal) {
            m_currentProposal.reset();
            m_proposalWorkspaceEpoch = 0;
            m_acknowledgedConstraints.clear();
            clearActiveGroundingBinding(m_activeGroundedEvidence, m_activeGroundingPrompt,
                                        m_activeGroundingActiveFile, m_activeGroundingProposalId,
                                        m_activeGroundingSessionId, m_activeGroundingRevisionId,
                                        m_groundingValidationSerial);
            m_proposalStatus = ProposalUiStatus::None;
            m_proposalMessage.clear();
            markComprehensionReviewStale();
            emit rewriteProposalChanged();
        }
        refreshActiveItemPresentation();
        resetClarificationCheck();
        emit interventionChanged();
        emit intentChanged();
        emit intentBriefChanged();
        emit stateVersionChanged();
        emit approvalChanged();
        if (advanceInterviewAfter) advanceInterview();
        co_return Status::success();

}

AsyncTask<core::Status> HostController::confirmChoiceOperation(QString alternativeId, quint64 expectedVersion) {
    const std::string alt = ss(alternativeId);
    co_return (co_await applyTransition("confirmChoice", expectedVersion, [=, this](intent::WorkingIntentState& state) {
        for (const auto& group : m_currentAnalysis->choiceGroups) {
            for (const auto& alternative : group.alternatives) {
                if (alternative.id != alt) continue;
                if (const auto violation = bridge::explicitColorConstraintViolation(m_revisionState->currentRevision().text(), alternative, group.question))
                    return Status(makeError(ErrorCode::ValidationFailed, *violation));
            }
        }
        return intent::InterventionStateMachine::applyChoice(*m_currentAnalysis, m_currentDecision, state, alt);
    }));
}

AsyncTask<core::Status> HostController::submitCustomIntentOperation(QString customText, quint64 expectedVersion) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
    if (!m_currentAnalysis || !m_revisionState || m_revisionState->isDraftDirty())
        co_return makeError(ErrorCode::PreconditionFailed, "Review the current draft before answering a question.");
    if (m_checkingClarification)
        co_return makeError(ErrorCode::PreconditionFailed, "Your answer is already being checked.");
    const auto text = core::normalizeLineEndings(ss(customText));
    auto trial = m_workingIntent;
    if (auto status = intent::InterventionStateMachine::applyCustomText(*m_currentAnalysis, m_currentDecision, trial, text);
        !status.ok()) co_return status;
    if (text.size() > bridge::kMaxModelStringBytes)
        co_return makeError(ErrorCode::InvalidInput, "Keep the answer within 4096 UTF-8 bytes.");

    // Deterministic stage audit before the model check. This rejects blank answers and answers
    // that reverse an explicit prohibition without spending a second model call.
    bridge::GoldfishClarificationAuditRequest auditRequest;
    auditRequest.originalPrompt = m_revisionState->currentRevision().text();
    auditRequest.question = ss(m_activeQuestion);
    auditRequest.userAnswer = text;
    for (const auto& alternative : m_activeAlternatives)
        auditRequest.offeredAlternatives.push_back(ss(alternative.toMap().value(QStringLiteral("label")).toString()));
    if (m_confirmedIntent)
        auditRequest.constraints = extractSessionConstraints(auditRequest.originalPrompt, *m_confirmedIntent);
    bridge::GraniteGoldfishAdapter goldfish;
    const auto stageAudit = goldfish.auditClarification(auditRequest, nullptr);
    if (stageAudit.outcome == core::ValidationOutcome::Fail) {
        m_clarificationFeedback = stageAudit.violations.empty()
            ? QStringLiteral("This answer contradicts an explicit requirement in the prompt.")
            : qs(stageAudit.violations.front().explanation);
        emit clarificationCheckChanged();
        co_return makeError(ErrorCode::ValidationFailed, ss(m_clarificationFeedback));
    }

    const auto itemId = *m_currentDecision.activeItemId;
    if (nativeGraniteSelected() && !usesTransportOverride() && !m_native->isReady()) {
        if (m_isDownloadingModel && m_downloadingNativeTag == m_activeModel) {
            m_clarificationFeedback = QStringLiteral("The model download is still running. Your answer is kept; retry when it finishes.");
            emit clarificationCheckChanged();
            co_return makeError(ErrorCode::ProviderUnavailable, ss(m_clarificationFeedback));
        }
        m_pendingNativeClarification = PendingNativeClarification{
            expectedVersion, ++m_clarificationCheckSerial, m_configurationGeneration,
            m_workspaceEpoch, m_revisionState->currentRevision().revisionId().value(), itemId, customText};
        m_checkingClarification = true;
        m_clarificationFeedback = QStringLiteral("Preparing the local model to check your answer…");
        emit clarificationCheckChanged();
        prepareNativeModel();
        co_return Status::success();
    }
    auto ticket = makeTicket();
    const auto transport = currentTransport(bridge::clarificationCheckResponseSchema(), &ticket,
        QStringLiteral("granite_analyze"), currentRevisionText());
    if (!transport || m_graniteAdapter->state() != bridge::GraniteRuntimeState::Ready) {
        m_clarificationFeedback = QStringLiteral("The local model is not ready to check your answer. Retry when it is ready.");
        emit clarificationCheckChanged();
        co_return makeError(ErrorCode::ProviderUnavailable, ss(m_clarificationFeedback));
    }
    bridge::ClarificationCheckRequest request;
    request.prompt = m_revisionState->currentRevision().text();
    request.question = ss(m_activeQuestion);
    request.answer = text;
    for (const auto& alternative : m_activeAlternatives)
        request.alternatives.push_back(ss(alternative.toMap().value(QStringLiteral("label")).toString()));
    const auto addCustom = [&](const std::string& id, const std::string& question) {
        const auto answer = m_workingIntent.customResolutions.find(id);
        if (answer != m_workingIntent.customResolutions.end()) request.previousAnswers.emplace_back(question, answer->second);
    };
    for (const auto& group : m_currentAnalysis->choiceGroups) {
        addCustom(group.id, group.question);
        const auto chosen = m_workingIntent.confirmedAlternatives.find(group.id);
        if (chosen == m_workingIntent.confirmedAlternatives.end()) continue;
        for (const auto& alternative : group.alternatives)
            if (alternative.id == chosen->second) request.previousAnswers.emplace_back(group.question, alternative.interpretation);
    }
    for (const auto& decision : m_currentAnalysis->missingDecisions) addCustom(decision.id, decision.question);
    for (const auto& conflict : m_currentAnalysis->conflicts) addCustom(conflict.id, conflict.description);
    for (const auto& assumption : m_currentAnalysis->assumptions) {
        addCustom(assumption.id, assumption.statement);
        const auto answer = m_workingIntent.assumptionDecisions.find(assumption.id);
        if (answer != m_workingIntent.assumptionDecisions.end())
            request.previousAnswers.emplace_back(assumption.statement, answer->second ? "Yes" : "No");
    }

    const auto checkSerial = ++m_clarificationCheckSerial;
    m_checkingClarification = true;
    m_clarificationFeedback.clear();
    emit clarificationCheckChanged();
    if (auto status = co_await beginAttempt(ticket, "clarification_check"); !status.ok()) {
        if (checkSerial == m_clarificationCheckSerial) resetClarificationCheck();
        co_return status;
    }
    ticket.configuration += "; contract=granite.clarification_check.v1; item=" + itemId +
        "; answer_sha256=" + core::crypto::Sha256::hexDigest(text);
    // Construct transport with the persisted attempt ID, rather than the preflight ticket.
    const auto boundTransport = currentTransport(bridge::clarificationCheckResponseSchema(), &ticket,
        QStringLiteral("granite_analyze"), qs(request.prompt));
    if (!boundTransport) {
        co_await completeAttempt(ticket, storage::AttemptStatus::Failed, "Clarification transport became unavailable");
        if (checkSerial == m_clarificationCheckSerial) resetClarificationCheck();
        co_return makeError(ErrorCode::ProviderUnavailable, "The local model could not start checking your answer. Retry.");
    }
    using CheckResult = core::Result<bridge::ClarificationCheckResponse>;
    auto* watcher = new QFutureWatcher<CheckResult>(this);
    auto adapter = m_graniteAdapter;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    ++m_inFlight;
    if (nativeWork) ++m_nativeSemanticInFlight;
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, ticket, checkSerial, itemId, customText, nativeWork]() {
            auto result = watcher->result();
            watcher->deleteLater();
            --m_inFlight;
            if (nativeWork) --m_nativeSemanticInFlight;
            emit modelEngineStatusChanged();
            emit intentChanged();
            queueModelCompletion("finishClarificationCheck", [this, ticket, checkSerial, itemId, customText, result]() {
                return finishClarificationCheck(ticket, checkSerial, itemId, customText, result);
            });
            finishQuitWhenRuntimesStopped();
        });
    watcher->setFuture(QtConcurrent::run([adapter, request, boundTransport]() {
        return adapter->checkClarification(request, boundTransport);
    }));
    co_return Status::success();
}

void HostController::resetClarificationCheck() {
    m_pendingNativeClarification.reset();
    ++m_clarificationCheckSerial;
    m_checkingClarification = false;
    m_clarificationFeedback.clear();
    emit clarificationCheckChanged();
}

AsyncTask<void> HostController::finishClarificationCheck(PipelineTicket ticket, quint64 checkSerial,
    std::string itemId, QString answer, core::Result<bridge::ClarificationCheckResponse> result) {
    const auto current = [&]() {
        return checkSerial == m_clarificationCheckSerial && isTicketCurrent(ticket, false) &&
            m_currentDecision.activeItemId == itemId;
    };
    if (!current()) {
        co_await completeAttempt(ticket, storage::AttemptStatus::DiscardedStale, "The question or draft changed during the answer check");
        if (checkSerial == m_clarificationCheckSerial) resetClarificationCheck();
        co_return;
    }
    co_await completeAttempt(ticket, result.ok() ? storage::AttemptStatus::Succeeded : storage::AttemptStatus::Failed,
        result.ok() ? "" : result.error().message);
    if (!current()) {
        if (checkSerial == m_clarificationCheckSerial) resetClarificationCheck();
        co_return;
    }
    m_checkingClarification = false;
    if (!result.ok() || !result.value().accepted) {
        m_clarificationFeedback = result.ok() ? qs(result.value().feedback) :
            QStringLiteral("Roclint couldn't check that answer. Your text is kept. Retry, choose an option, or skip the question.");
        emit clarificationCheckChanged();
        emit clarificationAnswerChecked(false);
        co_return;
    }
    const auto status = co_await applyTransition("checkedCustomIntent", ticket.stateVersion,
        [this, answer](intent::WorkingIntentState& state) {
            auto st = intent::InterventionStateMachine::applyCustomText(*m_currentAnalysis, m_currentDecision, state,
                core::normalizeLineEndings(ss(answer)));
            if (!st.ok()) return st;
            if (m_currentAnalysis && m_currentDecision.activeItemId.has_value()) {
                const std::string normAnswer = core::normalizeLineEndings(ss(answer));
                std::string lowerAns;
                for (char c : normAnswer) lowerAns.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                for (const auto& grp : m_currentAnalysis->choiceGroups) {
                    if (grp.id == *m_currentDecision.activeItemId || state.isResolved(grp.id) || state.isSkipped(grp.id)) continue;
                    for (const auto& alt : grp.alternatives) {
                        std::string lowerAlt;
                        for (char c : alt.interpretation) lowerAlt.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                        if (!lowerAlt.empty() && lowerAns.find(lowerAlt) != std::string::npos) {
                            state.confirmedAlternatives[grp.id] = alt.id;
                            break;
                        }
                    }
                }
            }
            return st;
        });
    if (ticket.serial != m_pipelineSerial || ticket.workspaceEpoch != m_workspaceEpoch || isDraftDirty()) co_return;
    if (!status.ok()) {
        m_clarificationFeedback = plainError(status.error());
        emit clarificationCheckChanged();
    }
    emit clarificationAnswerChecked(status.ok());
}

bool HostController::isDeferralPhrase(const QString& text) {
    const QString t = text.trimmed().toLower();
    if (t.isEmpty()) {
        return true;
    }
    static const QSet<QString> kExact = {
        QStringLiteral("idk"),
        QStringLiteral("you pick"),
        QStringLiteral("u pick"),
        QStringLiteral("you decide"),
        QStringLiteral("doesn't matter"),
        QStringLiteral("doesnt matter"),
        QStringLiteral("either"),
        QStringLiteral("both"),
        QStringLiteral("?"),
        QStringLiteral("??"),
        QStringLiteral("..."),
        QStringLiteral("no preference"),
        QStringLiteral("any"),
        QStringLiteral("delegate"),
        QStringLiteral("default"),
        QStringLiteral("reasonable default"),
        QStringLiteral("standard"),
        QStringLiteral("choose for me"),
        QStringLiteral("whatever works best"),
        QStringLiteral("whatever is best"),
        QStringLiteral("up to you")
    };
    if (kExact.contains(t) || t.startsWith(QStringLiteral("idk ")) || t.startsWith(QStringLiteral("you decide")) ||
        t.startsWith(QStringLiteral("you pick")) || t.startsWith(QStringLiteral("up to you")) ||
        t.startsWith(QStringLiteral("choose for me"))) {
        return true;
    }
    return false;
}

quint64 HostController::submitClarification(const QString& text, quint64 expectedVersion) {
    return submitClarification(text, isDeferralPhrase(text), expectedVersion);
}

quint64 HostController::submitClarification(const QString& text, bool isDeferral, quint64 expectedVersion) {
    if (!isDeferral && !isDeferralPhrase(text)) {
        return submitCustomIntent(text, expectedVersion);
    }

    const auto notifyAfter = [this](quint64 requestId, const QString& message, bool automatic) {
        if (!requestId) return quint64{0};
        const auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(this, &HostController::storageCommandFinished, this,
            [this, requestId, message, automatic, connection](quint64 finishedId, const QString&, bool succeeded, const QString&, const QString&) {
                if (finishedId != requestId) return;
                disconnect(*connection);
                if (succeeded) { setAmbientStatus(message); emit clarificationDeferred(message, automatic); }
            });
        return requestId;
    };

    // If a clear recommendation exists, deferral adopts that choice. The recommendation must be
    // bound to the currently active decision group and to an alternative still offered there.
    const bool recommendationMatchesActiveGroup =
        !m_recommendedGroupId.isEmpty() && m_currentDecision.activeItemId.has_value() &&
        m_recommendedGroupId == qs(*m_currentDecision.activeItemId);
    if (!m_activeAlternatives.isEmpty() && m_recommendedConfidence >= 0.70 &&
        !m_recommendedOption.isEmpty() && recommendationMatchesActiveGroup) {
        QString chosenAltId;
        QString chosenLabel;
        for (const auto& altVar : m_activeAlternatives) {
            const auto map = altVar.toMap();
            const QString id = map.value(QStringLiteral("id")).toString();
            const bool idMatches = !m_recommendedAlternativeId.isEmpty()
                ? id == m_recommendedAlternativeId
                : id.compare(m_recommendedOption, Qt::CaseInsensitive) == 0;
            if (!idMatches) continue;
            const QString label = map.value(QStringLiteral("label")).toString();
            if (!chosenAltId.isEmpty()) { chosenAltId.clear(); chosenLabel.clear(); break; }
            chosenAltId = id;
            chosenLabel = label;
        }
        if (!chosenAltId.isEmpty()) {
            const QString message = QStringLiteral("Used your delegated choice: %1").arg(chosenLabel);
            return notifyAfter(confirmChoice(chosenAltId, expectedVersion), message, true);
        }
    }

    // Essential missing fact cannot be delegated; keep question open and provide follow-up
    if (m_currentDecision.activeItemKind == intent::ActiveItemKind::MissingDecision) {
        m_clarificationFeedback = QStringLiteral("This essential detail is required to complete the prompt. Enter a specific answer or skip.");
        emit clarificationCheckChanged();
        return 0;
    }

    // Otherwise, treat same as "Keep my original wording"
    const QString message = QStringLiteral("Kept your original wording; no option was selected.");

    if (m_currentDecision.activeItemKind == intent::ActiveItemKind::ChoiceGroup) {
        return notifyAfter(keepOriginalPrompt(expectedVersion), message, false);
    }
    return notifyAfter(skipCurrentItem(expectedVersion), message, false);
}

AsyncTask<core::Status> HostController::keepOriginalPromptOperation(quint64 expectedVersion) {
    co_return (co_await applyTransition("keepOriginalPrompt", expectedVersion, [=, this](intent::WorkingIntentState& state) {
        return intent::InterventionStateMachine::applyKeepOriginal(*m_currentAnalysis, m_currentDecision, state);
    }));
}

AsyncTask<core::Status> HostController::resolveActiveAssumptionOperation(bool accepted, quint64 expectedVersion) {
    co_return (co_await applyTransition("resolveActiveAssumption", expectedVersion, [=, this](intent::WorkingIntentState& state) {
        return intent::InterventionStateMachine::applyAssumptionDecision(*m_currentAnalysis, m_currentDecision, state,
                                                                         accepted);
    }));
}

AsyncTask<core::Status> HostController::skipCurrentItemOperation(quint64 expectedVersion) {
    co_return (co_await applyTransition("skipCurrentItem", expectedVersion, [=, this](intent::WorkingIntentState& state) {
        return intent::InterventionStateMachine::applySkip(*m_currentAnalysis, m_currentDecision, state);
    }));
}

AsyncTask<core::Status> HostController::switchToManualOperation(quint64 expectedVersion) {
    co_return (co_await applyTransition("switchToManual", expectedVersion, [](intent::WorkingIntentState& state) {
        intent::InterventionStateMachine::applyManualRequest(state);
        return Status::success();
    }));
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

bool HostController::isOptionalHelp() const {
    return m_currentSnapshot && m_currentSnapshot->payloadVersion >= 5 && !isDraftDirty() && m_currentPolicyResult.isEligible() &&
        analysis::isOptionalAssistance(m_currentSnapshot->reviewPolicy.assistance);
}

QString HostController::reviewDiagnostics() const {
    QString detail = diagnosticMessage();
    if (m_currentAnalysis) {
        detail += QStringLiteral("\nPolicy: ") + QString::fromLatin1(analysis::toString(m_currentPolicyResult.primaryReasonCode));
        for (const auto& group : m_currentPolicyResult.groupResults)
            if (!group.isEligible()) detail += QLatin1Char('\n') + qs(group.diagnosticMessage).left(512);
    }
    if (m_currentSnapshot && m_currentSnapshot->promptTriage) {
        const auto& triage = *m_currentSnapshot->promptTriage;
        detail += QStringLiteral("\nS1: action %1, target %2, rule %3, conflict %4")
            .arg(triage.categoryScores[0], 0, 'f', 3).arg(triage.categoryScores[1], 0, 'f', 3)
            .arg(triage.categoryScores[2], 0, 'f', 3).arg(triage.categoryScores[3], 0, 'f', 3);
        if (m_currentSnapshot->payloadVersion >= 5)
            detail += m_currentSnapshot->reviewPolicy.trigger == analysis::ReviewTrigger::ExplicitReview
                ? QStringLiteral("\nTrigger: explicit review") : QStringLiteral("\nTrigger: automatic check");
    }
    return detail.trimmed();
}

QString HostController::reviewOutcome() const {
    if (!m_currentAnalysis || isDraftDirty()) return {};
    if (isManual() && !m_currentDecision.activeItemId) return QStringLiteral("UNAVAILABLE");
    if (isOptionalHelp()) return QStringLiteral("OPTIONAL_HELP");
    return (isClarifying() || isManual()) ? QStringLiteral("QUESTION") : QStringLiteral("READY");
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

bool HostController::deliveryMatchesDraft() const {
    if (m_lastCopiedText.isEmpty()) return false;
    if (m_lastCopiedText == currentRevisionText()) return true;
    return !isDraftDirty() && m_currentApproval &&
        m_lastCopiedText == qs(m_currentApproval->approvedText);
}

bool HostController::hasActiveWork() const noexcept {
    if (isClarifying() || hasConfirmedIntent() || hasProposal() || isAnalyzing()) {
        return true;
    }
    if (m_currentDecision.activeItemKind != intent::ActiveItemKind::None) {
        return true;
    }
    if (m_widgetDraftEdited) {
        return true;
    }
    if (!m_capturedDraftText.trimmed().isEmpty()) {
        return true;
    }
    if (!m_revisionState) {
        return false;
    }
    const auto& text = m_revisionState->currentRevision().text();
    if (text.empty()) {
        return false;
    }
    if (deliveryMatchesDraft()) {
        return false;
    }
    return true;
}

void HostController::resetActiveDraft() {
    resetSessionState();
    m_capturedDraftText.clear();
    emit capturedDraftTextChanged();
    emit draftCaptured(QString{});
    emit stateVersionChanged();
    setAnalysisStatus(AnalysisStatus::Idle, QStringLiteral("Draft reset. Type or capture a new prompt."));
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
    if (m_confirmedIntent->items.empty() && m_confirmedIntent->unresolvedItemIds.empty())
        return QStringLiteral("Ready to use as written. Improving the wording is optional.");
    QString summary = QStringLiteral("%1 answer(s) saved").arg(m_confirmedIntent->items.size());
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

AsyncTask<core::Status> HostController::setRewriteProfileOperation(QString profile, quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (!rewrite::ProfileRegistry::isKnownProfile(ss(profile))) {
            co_return makeError(ErrorCode::InvalidInput, "Unknown rewrite style: " + ss(profile));
        }
        if (m_rewriteProfile == profile) {
            co_return Status::success();
        }
        if (auto status = (co_await invalidateProposalArtifacts()); !status.ok()) co_return status;
        m_rewriteProfile = qs(rewrite::ProfileRegistry::getProfile(ss(profile))->id);
        if (!QCoreApplication::organizationName().isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("rewrite/profile"), m_rewriteProfile);
        }
        emit rewriteProfileChanged(m_rewriteProfile);
        co_return Status::success();

}

bool HostController::isRewriteEnabled() const {
    if (promptTriageRoutingEnabled() && m_currentSnapshot && m_currentSnapshot->payloadVersion == 4 &&
        (analysis::routePromptTriage(*m_currentSnapshot->promptTriage).value() != analysis::PromptTriageRoute::Clarify ||
         (!m_confirmedIntent || std::none_of(m_confirmedIntent->items.begin(), m_confirmedIntent->items.end(), [](const auto& item) {
             return item.kind == intent::IntentResolutionKind::SelectedAlternative || item.kind == intent::IntentResolutionKind::CustomText ||
                    item.kind == intent::IntentResolutionKind::AcceptedAssumption || item.kind == intent::IntentResolutionKind::DeclinedAssumption;
         })))) return false;
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

AsyncTask<core::Status> HostController::rewriteWithoutQuestionsOperation(quint64 expectedVersion) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
    if (!m_currentAnalysis || !m_revisionState || m_revisionState->isDraftDirty())
        co_return makeError(ErrorCode::PreconditionFailed, "Review the current draft before requesting a rewrite.");
    // A wording request cannot silently settle a conflict or a blocking missing decision.
    for (const auto& item : m_currentAnalysis->conflicts)
        if (item.blocking && !m_workingIntent.isResolved(item.id))
            co_return makeError(ErrorCode::PreconditionFailed, "Conflicting requirements remain open. Edit or resolve them before rewriting.");
    for (const auto& item : m_currentAnalysis->missingDecisions)
        if (item.blocking && !m_workingIntent.isResolved(item.id))
            co_return makeError(ErrorCode::PreconditionFailed, "A required detail remains open. Edit or answer it before rewriting.");
    const auto epoch = m_workspaceEpoch;
    const auto revision = m_revisionState->currentRevision().revisionId().value();
    const auto status = co_await applyTransition("rewriteWithoutQuestions", expectedVersion,
        [this](intent::WorkingIntentState& state) {
            const auto defer = [&state](const auto& item) {
                if (!state.isResolved(item.id) && !state.isSkipped(item.id)) state.skippedItemIds.push_back(item.id);
            };
            for (const auto& item : m_currentAnalysis->choiceGroups) defer(item);
            for (const auto& item : m_currentAnalysis->assumptions) defer(item);
            // Deferral is not an answer and consumes no guided-question round.
            return Status::success();
        });
    if (!status.ok()) co_return status;
    if (epoch != m_workspaceEpoch || !m_revisionState ||
        m_revisionState->currentRevision().revisionId().value() != revision ||
        m_revisionState->stateVersion() != expectedVersion + 1)
        co_return makeError(ErrorCode::StaleVersion, "The draft or workspace changed before rewriting.");
    co_return (co_await synthesizeRewriteOperation(m_revisionState->stateVersion()));
}

AsyncTask<core::Status> HostController::synthesizeRewriteOperation(quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (m_currentSnapshot && m_currentSnapshot->payloadVersion == 4 && !isRewriteEnabled())
            co_return makeError(ErrorCode::PreconditionFailed, "Review this saved prompt again before requesting wording help.");
        if (promptTriageRoutingEnabled() && !nativeGraniteSelected() && !usesTransportOverride())
            co_return makeError(ErrorCode::ProviderUnavailable, "Only the pinned native IBM Granite model can generate this rewrite.");
        if (!m_revisionState) {
            co_return makeError(ErrorCode::PreconditionFailed, "Write and analyze a prompt first.");
        }
        if (m_revisionState->isDraftDirty()) {
            co_return makeError(ErrorCode::PreconditionFailed,
                             "The prompt has edits that haven't been analyzed. Analyze it again before rewriting.");
        }
        if (!m_currentAnalysis) {
            co_return makeError(ErrorCode::PreconditionFailed, "Analyze the prompt before asking for a rewrite.");
        }
        if (!m_confirmedIntent) {
            co_return makeError(ErrorCode::PreconditionFailed,
                             "Answer the open questions (or switch to manual) before asking for a rewrite.");
        }
        for (const auto& group : m_currentAnalysis->choiceGroups) {
            const auto selected = m_workingIntent.confirmedAlternatives.find(group.id);
            if (selected == m_workingIntent.confirmedAlternatives.end()) continue;
            for (const auto& alternative : group.alternatives) {
                if (alternative.id != selected->second) continue;
                if (const auto violation = bridge::explicitColorConstraintViolation(m_revisionState->currentRevision().text(), alternative, group.question))
                    co_return makeError(ErrorCode::ValidationFailed, *violation);
            }
        }
        if (auto st = intent::IntentProvenanceBuilder::isIntentValid(
                *m_confirmedIntent, m_revisionState->currentRevision().revisionId().value(), m_revisionState->status(),
                *m_currentSnapshot);
            !st.ok()) {
            co_return st;
        }
        if (m_proposalStatus == ProposalUiStatus::Generating) {
            co_return makeError(ErrorCode::PreconditionFailed, "A rewrite is already being written.");
        }
        const auto profile = rewrite::ProfileRegistry::getProfile(ss(m_rewriteProfile));
        if (!profile.has_value()) {
            co_return makeError(ErrorCode::InvalidInput, "Unknown rewrite style: " + ss(m_rewriteProfile));
        }
        const auto runtime = m_graniteAdapter->state();
        if (nativeGraniteSelected() && !usesTransportOverride() &&
            (runtime != bridge::GraniteRuntimeState::Ready || !m_native->isReady())) {
            m_pendingNativeRewrite = PendingNativeRewrite{expectedVersion, m_confirmedIntent->id, m_rewriteProfile, m_configurationGeneration};
            setProposalStatus(ProposalUiStatus::Generating, QStringLiteral("Warming the pinned native model before the requested rewrite."));
            prepareNativeModel();
            co_return Status::success();
        }
        if (runtime != bridge::GraniteRuntimeState::Ready || (!nativeGraniteSelected() && !currentTransport(bridge::rewriteResponseSchema()))) {
            co_return makeError(ErrorCode::ProviderUnavailable, ss(modelEngineStatus()));
        }

        const auto operationWorkspaceEpoch = m_workspaceEpoch;
        if (auto status = (co_await invalidateProposalArtifacts()); !status.ok()) co_return status;
        // The invalidation write may yield to the event loop. A workspace or
        // scope change during it must abort before the captured analysis/intent
        // state is used.
        if (operationWorkspaceEpoch != m_workspaceEpoch || !m_currentSnapshot || !m_confirmedIntent ||
            !m_revisionState) {
            co_return makeError(ErrorCode::StaleVersion,
                                "The workspace changed before the rewrite could start.");
        }
        PipelineTicket ticket = makeTicket();
        if (auto st = (co_await beginAttempt(ticket, "rewrite")); !st.ok()) {
            co_return st;
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
            qs(request.originalText), qs(request.confirmedIntent.id), m_rewriteProfile, {}, QStringLiteral("rewrite"));
        if (!transport) {
            completeAttempt(ticket, storage::AttemptStatus::Failed, "Native rewrite transport is unavailable");
            co_return makeError(ErrorCode::ProviderUnavailable, "The local rewrite runtime is not ready.");
        }

        m_streamingPreview.clear();
        setProposalStatus(ProposalUiStatus::Generating, QStringLiteral("Writing a suggested rewrite."));
        emit modelEngineStatusChanged();

        const auto auditTransport = currentTransport(bridge::goldfishAuditResponseSchema(), &ticket, QStringLiteral("granite_rewrite"),
            qs(request.originalText), qs(request.confirmedIntent.id), m_rewriteProfile, {}, QStringLiteral("rewrite"));

        struct RewriteExecutionResult {
            core::Result<rewrite::RewriteProposal> proposal;
            core::ValidationResult auditVerdict;
        };
        using RewriteResult = RewriteExecutionResult;

        auto adapter = m_graniteAdapter;
        auto grounding = std::make_shared<GroundedPromptContext>();
        auto evidenceCurrent = std::make_shared<bool>(true);
        const auto groundingInput = groundingInputs(request.originalText);
        auto* watcher = new QFutureWatcher<RewriteResult>(this);
        ++m_inFlight;
        const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
        if (nativeWork) ++m_nativeSemanticInFlight;
        connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, ticket, nativeWork, grounding, evidenceCurrent]() {
                --m_inFlight;
                if (nativeWork) --m_nativeSemanticInFlight;
                RewriteResult execResult = watcher->result();
                watcher->deleteLater();
                if (isTicketCurrent(ticket, true)) applyGroundingContext(*grounding);
                onRewriteFinished(ticket, std::move(execResult.proposal), *grounding, *evidenceCurrent, std::move(execResult.auditVerdict));
                finishQuitWhenRuntimesStopped();
            });
        const std::int64_t startedAt = nowMs();
        watcher->setFuture(QtConcurrent::run(
            [adapter, request, transport, auditTransport, startedAt, groundingInput, grounding, evidenceCurrent, ticketSessionId = ticket.sessionId]() mutable -> RewriteResult {
                try {
                    *grounding = computeGroundedPromptContext(groundingInput);
                    request.groundedCanonicalIntentXml = grounding->xml;
                    request.astSnapshotHash = grounding->astSnapshotHash;
                    request.constraints = HostController::extractSessionConstraints(request.originalText, request.confirmedIntent);
                    auto repairResult = bridge::GraniteRewriteAdapter::synthesizeRewriteWithRepairLoop(
                        request, *adapter, transport, auditTransport, startedAt);
                    auto proposalRes = std::move(repairResult.proposal);
                    core::ValidationResult auditVerdict = std::move(repairResult.auditVerdict);
                    if (grounding->available && grounding->validated) {
                        const auto post = computeGroundedPromptContext(groundingInput);
                        *evidenceCurrent = post.available && post.validated &&
                            post.astSnapshotHash == grounding->astSnapshotHash;
                    }
                    return RewriteExecutionResult{std::move(proposalRes), std::move(auditVerdict)};
                } catch (const std::exception& ex) {
                    return RewriteExecutionResult{makeError(ErrorCode::Internal, std::string("Rewrite crashed: ") + ex.what()), core::ValidationResult{}};
                }
            }));
        co_return Status::success();

}

void HostController::onRewriteFinished(PipelineTicket ticket, core::Result<rewrite::RewriteProposal> result,
                                       GroundedPromptContext grounding, bool evidenceCurrent,
                                       core::ValidationResult auditVerdict) {
    queueModelCompletion("finishRewrite",
        [this, ticket = std::move(ticket), result = std::move(result),
         grounding = std::move(grounding), evidenceCurrent, auditVerdict = std::move(auditVerdict)]() mutable {
            return onRewriteFinishedAsync(std::move(ticket), std::move(result), std::move(grounding),
                                          evidenceCurrent, std::move(auditVerdict));
        });
}

AsyncTask<void> HostController::onRewriteFinishedAsync(PipelineTicket ticket,
                                       core::Result<rewrite::RewriteProposal> result,
                                       GroundedPromptContext grounding, bool evidenceCurrent,
                                       core::ValidationResult auditVerdict) {
    if (m_healthRecheckRequested && !m_healthCheckInFlight) refreshAvailableModels();
    emit modelEngineStatusChanged();
    if (m_ambientCheckPending) {
        m_ambientDebounce.start();
    }
    if (!isTicketCurrent(ticket, true) || !m_confirmedIntent) {
        completeAttempt(ticket, storage::AttemptStatus::DiscardedStale, "The prompt or settings changed before the rewrite finished");
        if (m_discoveryQueued) {
            if (auto st = (co_await startDiscovery()); !st.ok()) {
                setLastError(st.error());
            }
        }
        co_return;
    }
    if (!result.ok()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, result.error().message);
        const auto error = result.error().code == ErrorCode::ValidationFailed
            ? makeError(ErrorCode::ProviderFailed, "That rewrite couldn't preserve your requirements. Retry explicitly or keep the original prompt.")
            : modelReplyError(result.error());
        setProposalStatus(ProposalUiStatus::Failed, QStringLiteral("The rewrite couldn't be produced. ") + plainError(error));
        setLastError(error);
        co_return;
    }

    rewrite::RewriteProposal proposal = result.value();
    if (!evidenceCurrent ||
        (!proposal.astSnapshotHash.empty() &&
         (!grounding.validated || proposal.astSnapshotHash != grounding.astSnapshotHash))) {
        completeAttempt(ticket, storage::AttemptStatus::DiscardedStale,
                        "Code evidence changed while writing the rewrite");
        m_currentProposal.reset();
        m_proposalWorkspaceEpoch = 0;
        m_acknowledgedConstraints.clear();
        m_currentApproval.reset();
        clearActiveGroundingBinding(m_activeGroundedEvidence, m_activeGroundingPrompt,
                                    m_activeGroundingActiveFile, m_activeGroundingProposalId,
                                    m_activeGroundingSessionId, m_activeGroundingRevisionId,
                                    m_groundingValidationSerial);
        setGroundingUnavailable(QStringLiteral("STALE"));
        setProposalStatus(ProposalUiStatus::Failed,
                          QStringLiteral("Code evidence changed while writing the rewrite. Review again."));
        emit approvalChanged();
        co_return;
    }
    bridge::GraniteModelInfo model;
    model.modelId = proposal.modelId;
    model.digest = proposal.modelDigest;
    model.quantization = proposal.quantization;
    model.transport = proposal.transport;

    auto nextState = *m_revisionState;
    if (auto status = nextState.noteProposalEdit(ticket.stateVersion); !status.ok()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, status.error().message, &model);
        setProposalStatus(ProposalUiStatus::Failed, plainError(status.error()));
        co_return;
    }
    if (proposal.proposalEditGeneration != nextState.proposalEditGeneration()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Proposal generation mismatch", &model);
        setProposalStatus(ProposalUiStatus::Failed, QStringLiteral("The rewrite was out of date and was discarded."));
        co_return;
    }
    auto persistProposal = [&]() -> AsyncTask<core::Status> {
        if (auto status = (co_await m_repository->insertProposal(proposal, nowMs(), ticket.stateVersion)); !status.ok()) co_return status;
        // The workspace/scope may have changed while the proposal row was
        // persisted. Keep the in-memory revision version aligned with the row
        // that was just written, but do not publish it into the new epoch.
        const bool stillCurrent = isTicketCurrent(ticket, false);
        *m_revisionState = nextState;
        m_currentApproval.reset();
        if (!stillCurrent) {
            co_return makeError(ErrorCode::StaleVersion,
                                "The workspace changed before the rewrite was saved.");
        }
        co_return Status::success();
    };

    if (proposal.status == rewrite::ProposalStatus::NeedsClarification) {
        if (auto st = (co_await persistProposal()); !st.ok()) {
            completeAttempt(ticket, storage::AttemptStatus::Failed, st.error().message, &model);
            if (ticket.workspaceEpoch != m_workspaceEpoch || ticket.serial != m_pipelineSerial) co_return;
            setProposalStatus(ProposalUiStatus::Failed, plainError(st.error()));
            co_return;
        }
        completeAttempt(ticket, storage::AttemptStatus::Succeeded, std::string(), &model);
        m_currentProposal = proposal;
        m_proposalWorkspaceEpoch = ticket.workspaceEpoch;
        if (grounding.available && grounding.validated) {
            m_activeGroundedEvidence = grounding.evidence;
        } else {
            m_activeGroundedEvidence.clear();
        }
        m_activeGroundingGeneration = grounding.validated ? grounding.snapshotGeneration : 0;
        m_activeGroundingPrompt = grounding.prompt.empty() ? proposal.originalText : grounding.prompt;
        m_activeGroundingActiveFile = grounding.activeFile;
        m_activeGroundingProposalId = proposal.id;
        m_activeGroundingSessionId = ticket.sessionId;
        m_activeGroundingRevisionId = ticket.revisionId;
        setProposalStatus(ProposalUiStatus::NeedsClarification, qs(proposal.clarificationQuestion));
        m_reviewState = ReviewState::NotRun;
        m_reviewResult = bridge::ComprehensionReviewResult{};
        m_reviewBinding = ComprehensionBinding{};
        m_findingResolutions.clear();
        m_reviewReturnedFindingId.clear();
        emit comprehensionReviewChanged();
        emit stateVersionChanged();
        co_return;
    }

    const auto validation = rewrite::RewriteValidator::validateProposal(
        proposal, ticket.sessionId, ticket.revisionId, *m_confirmedIntent, std::set<std::string>{});
    if (!validation.structurallyValid) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Validation failed: " + validation.rejectionReason, &model);
        setProposalStatus(ProposalUiStatus::ValidationFailed,
                          QStringLiteral("That rewrite couldn't be verified. Retry explicitly or keep the original prompt."));
        co_return;
    }

    // Deterministic post-generation guards. These do not claim semantic proof; they
    // prevent presenting an unchanged string, an instruction about the prompt, or a
    // rewrite that silently drops a supplied exact value/unit as an applicable rewrite.
    const auto normalizedTrimmed = [](const std::string& text) {
        std::string normalized = core::normalizeLineEndings(text);
        const auto first = normalized.find_first_not_of(" \t\n");
        if (first == std::string::npos) return std::string();
        const auto last = normalized.find_last_not_of(" \t\n");
        return normalized.substr(first, last - first + 1);
    };
    if (normalizedTrimmed(proposal.rewrittenText) == normalizedTrimmed(proposal.originalText)) {
        completeAttempt(ticket, storage::AttemptStatus::Succeeded, "No material change produced", &model);
        m_currentProposal.reset();
        m_proposalWorkspaceEpoch = 0;
        m_currentApproval.reset();
        markComprehensionReviewStale();
        setProposalStatus(ProposalUiStatus::ValidationFailed,
                          QStringLiteral("No useful change was found. Your original prompt is kept; "
                                         "skipped or unknown details remain open."));
        emit approvalChanged();
        co_return;
    }
    // Imperatives such as "identify which" can be the requested task. Do not
    // reject them by vocabulary; the required independent comprehension review
    // checks whether a rewrite expresses the task or merely asks to define it.
    if (const auto dropped = intent::droppedExactRequirements(proposal.originalText, proposal.rewrittenText);
        !dropped.empty()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed,
                        "Rewrite dropped an exact requirement: " + dropped.front(), &model);
        m_currentProposal.reset();
        m_proposalWorkspaceEpoch = 0;
        m_currentApproval.reset();
        markComprehensionReviewStale();
        setProposalStatus(ProposalUiStatus::ValidationFailed,
                          QStringLiteral("That rewrite dropped an exact requirement (%1). "
                                         "Your original prompt is kept.").arg(qs(dropped.front())));
        emit approvalChanged();
        co_return;
    }

    // Independent Goldfish Audit Pass (Zero-Memory Adversarial Audit)
    if (auditVerdict.outcome == core::ValidationOutcome::Unresolved && auditVerdict.violations.empty() && auditVerdict.unresolved.empty()) {
        const auto constraints = extractSessionConstraints(proposal.originalText, *m_confirmedIntent);
        bridge::GoldfishAuditRequest auditReq;
        auditReq.sessionId = ticket.sessionId;
        auditReq.originalPrompt = proposal.originalText;
        auditReq.candidateRewrite = proposal.rewrittenText;
        auditReq.constraints = constraints;

        bridge::GraniteGoldfishAdapter goldfishAdapter;
        bridge::GraniteTransport auditTransport = nullptr;
#ifdef ROCLINT_ENABLE_TEST_MOCKS
        if (usesTransportOverride()) {
            auditTransport = m_transportOverride;
        }
#endif
        auditVerdict = goldfishAdapter.audit(auditReq, auditTransport);
    }
    m_lastAuditResult = auditVerdict;
    proposal.auditResult = auditVerdict;

    if (auditVerdict.outcome == core::ValidationOutcome::Fail) {
        const std::string explanation = auditVerdict.violations.empty()
            ? "Proposal violated invariant constraints"
            : auditVerdict.violations.front().explanation;
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Goldfish audit failed: " + explanation, &model);
        setProposalStatus(ProposalUiStatus::ValidationFailed,
                          qs("Audit rejected rewrite: " + explanation));
        co_return;
    }

    if (auto st = (co_await persistProposal()); !st.ok()) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, st.error().message, &model);
        if (ticket.workspaceEpoch != m_workspaceEpoch || ticket.serial != m_pipelineSerial) co_return;
        setProposalStatus(ProposalUiStatus::Failed, plainError(st.error()));
        setLastError(st.error());
        co_return;
    }
    completeAttempt(ticket, storage::AttemptStatus::Succeeded, std::string(), &model);

    m_currentProposal = proposal;
    m_proposalWorkspaceEpoch = ticket.workspaceEpoch;
    m_acknowledgedConstraints.clear();
    {
        const auto constraints = extractSessionConstraints(proposal.originalText, *m_confirmedIntent);
        m_creativeAlternatives = core::rewrite::CreativeExpansionEngine::expand(
            proposal.originalText, proposal.rewrittenText, constraints);
        m_creativeAlternativeAuditExecutionStatuses.assign(m_creativeAlternatives.size(), core::AuditExecutionStatus::Unavailable);
        if (m_lastAuditResult.has_value() && !m_creativeAlternativeAuditExecutionStatuses.empty()) {
            m_creativeAlternativeAuditExecutionStatuses[0] = m_lastAuditResult->executionStatus;
        }
        if (m_lastAuditResult.has_value() && !m_creativeAlternatives.empty()) {
            m_creativeAlternatives[0].auditOutcome = m_lastAuditResult->outcome;
        }
        m_selectedAlternativeIndex = 0;
        emit creativeAlternativesChanged();
        emit selectedAlternativeChanged();
    }
    if (grounding.available && grounding.validated) {
        m_activeGroundedEvidence = grounding.evidence;
    } else {
        m_activeGroundedEvidence.clear();
    }
    m_activeGroundingGeneration = grounding.validated ? grounding.snapshotGeneration : 0;
    m_activeGroundingPrompt = grounding.prompt.empty() ? proposal.originalText : grounding.prompt;
    m_activeGroundingActiveFile = grounding.activeFile;
    m_activeGroundingProposalId = proposal.id;
    m_activeGroundingSessionId = ticket.sessionId;
    m_activeGroundingRevisionId = ticket.revisionId;
    m_reviewState = ReviewState::NotRun;
    m_reviewResult = bridge::ComprehensionReviewResult{};
    m_reviewBinding = ComprehensionBinding{};
    m_findingResolutions.clear();
    m_reviewReturnedFindingId.clear();
    emit comprehensionReviewChanged();
    emit stateVersionChanged();
    const auto currentValidation = validateCurrentProposal();
    const auto pending = currentValidation.pendingAcknowledgments.size();
    setProposalStatus(ProposalUiStatus::ReadyForReview,
                      proposal.rewrittenText == proposal.originalText
                          ? QStringLiteral("No wording changes suggested. This is your original prompt.")
                      : pending == 0 ? QStringLiteral("Review the suggested wording, then choose Insert or Copy.")
                                   : QStringLiteral("Check the %1 highlighted item(s), then approve the wording.").arg(pending));

    co_return;
}

rewrite::ValidationResult HostController::validateCurrentProposal() const {
    if (!m_currentProposal || !m_confirmedIntent || !m_revisionState ||
        m_proposalWorkspaceEpoch != m_workspaceEpoch) {
        return rewrite::ValidationResult{};
    }
    auto result = rewrite::RewriteValidator::validateProposal(*m_currentProposal, currentSessionId(),
                                                              m_revisionState->currentRevision().revisionId().value(),
                                                              *m_confirmedIntent, m_acknowledgedConstraints);
    if (!result.structurallyValid) {
        return result;
    }
    if (m_lastAuditResult.has_value()) {
        if (m_lastAuditResult->executionStatus == core::AuditExecutionStatus::DeterministicOnly) {
            const std::string fallbackId = "deterministic_fallback";
            const bool acknowledged = m_acknowledgedConstraints.count(fallbackId) > 0;
            if (!acknowledged) {
                if (std::find(result.pendingAcknowledgments.begin(), result.pendingAcknowledgments.end(), fallbackId) == result.pendingAcknowledgments.end()) {
                    result.pendingAcknowledgments.push_back(fallbackId);
                }
                result.isValid = false;
            }
        }
        if (m_lastAuditResult->outcome == core::ValidationOutcome::Fail) {
            result.isValid = false;
        } else if (m_lastAuditResult->outcome == core::ValidationOutcome::Unresolved) {
            for (const auto& u : m_lastAuditResult->unresolved) {
                std::string targetId = u.constraintId;
                for (const auto& c : m_currentProposal->constraints) {
                    if (isMatchingConstraintId(c.id, u.constraintId)) {
                        targetId = c.id;
                        break;
                    }
                }
                const bool acknowledged = m_acknowledgedConstraints.count(targetId) > 0 ||
                                          m_acknowledgedConstraints.count(u.constraintId) > 0;
                if (!acknowledged) {
                    if (std::find(result.pendingAcknowledgments.begin(), result.pendingAcknowledgments.end(), targetId) == result.pendingAcknowledgments.end()) {
                        result.pendingAcknowledgments.push_back(targetId);
                    }
                    result.isValid = false;
                }
            }
        }
    } else {
        result.isValid = false;
        if (result.pendingAcknowledgments.empty()) {
            result.pendingAcknowledgments.push_back("audit_required");
        }
    }
    return result;
}

std::optional<review::ApprovalContext> HostController::approvalContext() const {
    if (!m_revisionState || !m_currentAnalysis || !m_confirmedIntent || !m_currentProposal ||
        m_proposalWorkspaceEpoch != m_workspaceEpoch) {
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

AsyncTask<core::Status> HostController::acknowledgeConstraintOperation(QString constraintId, bool acknowledged, quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (m_proposalStatus != ProposalUiStatus::ReadyForReview || !m_currentProposal) {
            co_return makeError(ErrorCode::PreconditionFailed, "There is no suggested rewrite to review.");
        }
        if (m_proposalWorkspaceEpoch != m_workspaceEpoch) {
            co_return makeError(ErrorCode::PreconditionFailed,
                                "The workspace changed after this rewrite was prepared. Review the prompt again.");
        }
        const std::string id = ss(constraintId);
        const auto constraint = m_currentProposal->getConstraint(id);
        bool isAuditUnresolved = false;
        if (m_lastAuditResult.has_value() && m_lastAuditResult->outcome == core::ValidationOutcome::Unresolved) {
            for (const auto& u : m_lastAuditResult->unresolved) {
                if (isMatchingConstraintId(id, u.constraintId)) {
                    isAuditUnresolved = true;
                    break;
                }
            }
        }
        const bool isDeterministicFallback = (id == "deterministic_fallback" && m_lastAuditResult.has_value() &&
                                              m_lastAuditResult->executionStatus == core::AuditExecutionStatus::DeterministicOnly);
        if ((!constraint.has_value() || constraint->status != rewrite::ConstraintStatus::SemanticReviewRequired) && !isAuditUnresolved && !isDeterministicFallback) {
            co_return makeError(ErrorCode::InvalidInput, "That item doesn't need your review.");
        }
        if ((m_acknowledgedConstraints.count(id) > 0) == acknowledged) co_return Status::success();
        const auto operationWorkspaceEpoch = m_workspaceEpoch;
        const auto operationProposalId = m_currentProposal->id;
        if (auto status = (co_await advanceCommandVersion(expectedVersion)); !status.ok()) co_return status;
        if (operationWorkspaceEpoch != m_workspaceEpoch || m_proposalWorkspaceEpoch != operationWorkspaceEpoch ||
            !m_currentProposal || m_currentProposal->id != operationProposalId) {
            co_return makeError(ErrorCode::StaleVersion,
                                "The workspace changed before the review item was saved.");
        }
        if (acknowledged) {
            m_acknowledgedConstraints.insert(id);
            if (isAuditUnresolved && m_lastAuditResult.has_value()) {
                for (const auto& u : m_lastAuditResult->unresolved) {
                    if (isMatchingConstraintId(id, u.constraintId)) {
                        m_acknowledgedConstraints.insert(u.constraintId);
                    }
                }
            }
        } else {
            m_acknowledgedConstraints.erase(id);
            if (isAuditUnresolved && m_lastAuditResult.has_value()) {
                for (const auto& u : m_lastAuditResult->unresolved) {
                    if (isMatchingConstraintId(id, u.constraintId)) {
                        m_acknowledgedConstraints.erase(u.constraintId);
                    }
                }
            }
        }
        emit rewriteProposalChanged();
        emit stateVersionChanged();
        emit approvalChanged();
        co_return Status::success();

}

bool HostController::beginInsertionTargetSelection(quint64 expectedVersion) {
    return runCommand("beginInsertionTargetSelection", [&]() -> Status {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) return status;
        if (!isAmbientActive())
            return makeError(ErrorCode::PreconditionFailed, "Enable Live prompt capture in Settings to connect an insertion field.");
        if (!m_revisionState || !m_currentProposal || m_proposalStatus != ProposalUiStatus::ReadyForReview ||
            m_proposalWorkspaceEpoch != m_workspaceEpoch)
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

AsyncTask<core::Status> HostController::approveAndCopyRewriteOperation(quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (m_proposalStatus != ProposalUiStatus::ReadyForReview || !m_currentProposal) {
            co_return makeError(ErrorCode::PreconditionFailed, "There is no suggested rewrite to approve.");
        }
        if (m_proposalWorkspaceEpoch != m_workspaceEpoch) {
            co_return makeError(ErrorCode::PreconditionFailed,
                                "The workspace changed after this rewrite was prepared. Review the prompt again.");
        }
        if (!m_repository) {
            co_return storageUnavailable();
        }
        const auto operationWorkspaceEpoch = m_workspaceEpoch;
        const auto operationProposalId = m_currentProposal->id;
        const auto validation = validateCurrentProposal();
        if (!validation.structurallyValid) {
            setProposalStatus(ProposalUiStatus::ValidationFailed,
                              QStringLiteral("The suggested rewrite failed checks: ") + qs(validation.rejectionReason));
            co_return makeError(ErrorCode::ValidationFailed, validation.rejectionReason);
        }
        if (!validation.isValid) {
            co_return makeError(ErrorCode::PreconditionFailed,
                             "Check the " + std::to_string(validation.pendingAcknowledgments.size()) +
                                 " highlighted item(s) before approving.");
        }
        if (!canApplyReviewedOutput()) {
            co_return makeError(ErrorCode::PreconditionFailed,
                                "Run a fresh independent review and resolve its required findings before applying this rewrite.");
        }
        auto ctx = approvalContext();
        if (!ctx.has_value()) {
            co_return makeError(ErrorCode::PreconditionFailed, "The rewrite is no longer bound to the current prompt.");
        }
        auto nextState = *m_revisionState;
        if (auto status = nextState.advanceState(expectedVersion); !status.ok()) co_return status;
        ctx->stateVersion = nextState.stateVersion();
        auto approval = review::ApprovalFactory::createApproval(*ctx, nowMs());
        if (!approval.ok()) {
            co_return approval.error();
        }
        if (!review::ApprovalFactory::isApprovalValid(approval.value(), *ctx)) {
            co_return makeError(ErrorCode::Internal, "The approval record failed its own validation");
        }
        // F08: Ensure the proposal row exists in the repository before inserting approval
        if (m_storage) {
            (void)co_await m_storage->call(&storage::PromptRepository::insertProposal,
                                           *m_currentProposal, nowMs(), std::nullopt);
        }
        if (auto st = (co_await m_repository->insertApproval(approval.value(), expectedVersion)); !st.ok()) {
            co_return st;
        }
        // The approval write may yield to the event loop. If the workspace or
        // proposal changed, discard this approval instead of delivering it.
        const bool stillCurrent = operationWorkspaceEpoch == m_workspaceEpoch &&
            m_proposalWorkspaceEpoch == operationWorkspaceEpoch &&
            m_currentProposal && m_currentProposal->id == operationProposalId;
        // insertApproval advanced the persisted session state; align memory even
        // when the approval itself must be rejected.
        *m_revisionState = nextState;
        if (!stillCurrent) {
            if (m_repository) {
                if (auto invalidated = (co_await m_repository->invalidateApprovals(
                        core::SessionId(approval.value().sessionId)));
                    !invalidated.ok()) {
                    setLastError(invalidated.error());
                }
            }
            co_return makeError(ErrorCode::StaleVersion,
                                "The workspace changed before the approval completed.");
        }
        m_currentApproval = approval.value();
        const auto deliveryTicket = makeTicket();
        const auto deliveryText = qs(approval.value().approvedText);
        emit stateVersionChanged();
        emit approvalChanged();
        if (!m_currentApproval || !isTicketCurrent(deliveryTicket, true))
            co_return makeError(ErrorCode::StaleVersion, "The draft changed before delivery");
        co_return deliver(deliveryText);

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
    request.processId = target.field.processId;
    request.appIdentifier = target.field.appIdentifier;
    request.kind = target.field.kind;
    request.text = text;
    request.expectedCurrentText = target.field.kind == FieldKind::Terminal
                                      ? TerminalAdapter::extractDraft(target.field.text, target.field.caretOffset)
                                            .value_or(TerminalDraft{})
                                            .text
                                      : target.field.text;
    m_pendingDelivery = PendingDelivery{request.requestId, text, target.field.appDisplayName,
        target.field.fieldToken, target.field.processId, target.field.appIdentifier, m_pipelineSerial};
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
        if (m_captureSource == CaptureSource::Ambient && m_captureTarget && m_pipelineSerial == delivery.pipelineSerial &&
            m_captureTarget->field.fieldToken == delivery.fieldToken && m_captureTarget->field.processId == delivery.processId &&
            m_captureTarget->field.appIdentifier == delivery.appIdentifier) {
            // Readback confirmed this exact delivery. Ignore its observation echo,
            // then follow fresh typing in the same composer for the next prompt.
            m_captureTarget->prompt = delivery.text;
            m_captureTarget->textHash = core::crypto::Sha256::hexDigest(ss(delivery.text));
            if (m_captureTarget->field.kind == FieldKind::GuiEditor) m_captureTarget->field.text = delivery.text;
            m_widgetDraftEdited = false;
            m_followAmbientDraft = true;
        }
        clearLastError();
        m_lastCopiedText = delivery.text;
        emit lastCopiedTextChanged();
        setAmbientStatus(QStringLiteral("Inserted into %1.").arg(delivery.appName));
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
    emit deliveryFellBack(message);
}

void HostController::setWordCompanionState(const QString& state, const QString& message) {
    if (m_wordCompanionState == state && m_wordCompanionMessage == message) return;
    m_wordCompanionState = state; m_wordCompanionMessage = message;
    emit wordRefinementChanged();
}

QVariantList HostController::wordMarkRects() const {
    QVariantList result;
    for (const auto& rect : m_wordMarkRects)
        if (!rect.isEmpty()) result.append(QVariantMap{{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}});
    return result;
}

bool HostController::wordReplacementReady() const {
    return m_hasWordRefinement && !m_wordRefinementNeedsClarification &&
        !m_wordRefinementReplacement.isEmpty() && !m_wordRefinementInFlight &&
        m_wordCompanionState == QLatin1String("ready");
}

bool HostController::wordMarkVisible() const {
    return isAmbientActive() && !isCardVisible() && wordReplacementReady() &&
        !m_wordMarkRects.isEmpty() && m_ambientObservation &&
        !m_ambientDismissed.count(m_ambientObservation->textHash);
}

bool HostController::wordCanUndo() const {
    return m_wordUndo && !m_wordRefinementInFlight && m_ambientObservation &&
        m_wordUndo->workspaceEpoch == m_workspaceEpoch && m_wordUndo->pipelineSerial == m_pipelineSerial &&
        m_wordUndo->fieldToken == m_ambientObservation->field.fieldToken &&
        m_wordUndo->processId == m_ambientObservation->field.processId &&
        m_ambientObservation->field.text == m_wordUndo->after;
}

quint64 HostController::undoWordRefinement(quint64 expectedVersion) {
    if (expectedVersion != stateVersion() || !wordCanUndo()) return 0;
    auto undo = *m_wordUndo;
    std::swap(undo.before, undo.after);
    std::swap(undo.original, undo.replacement);
    undo.undo = true;
    undo.requestId = ++m_wordRefinementRequestId;
    undo.feedbackGeneration = m_wordFeedbackGeneration;
    RangeInjectionRequest request{undo.requestId, undo.fieldToken, FieldKind::GuiEditor, undo.startOffset,
        undo.startOffset + static_cast<int>(undo.original.size()), undo.original, undo.replacement, undo.before};
    m_wordUndo.reset();
    m_pendingWordApply = undo;
    m_wordRefinementInFlight = true;
    setWordCompanionState(QStringLiteral("applying"), QStringLiteral("Restoring your wording…"));
    m_accessibility->injectRange(request);
    return request.requestId;
}

bool HostController::submitWordClarification(const QString& answer, quint64 expectedVersion) {
    if (expectedVersion != stateVersion() || !m_hasWordRefinement || !m_wordRefinementNeedsClarification ||
        m_wordRefinementGenerating || m_wordRefinementInFlight || !m_ambientObservation ||
        m_wordRefinementWorkspaceEpoch != m_workspaceEpoch || m_wordRefinementPipelineSerial != m_pipelineSerial ||
        m_wordRefinementStateVersion != stateVersion() ||
        m_ambientObservation->field.fieldToken != m_wordRefinementFieldToken ||
        m_ambientObservation->field.text != m_wordRefinementFieldText) return false;
    if (answer.trimmed().isEmpty() || static_cast<std::size_t>(answer.toUtf8().size()) > bridge::kMaxModelStringBytes) return false;
    m_wordClarificationAnswer = answer.trimmed();
    requestWordRefinement(m_ambientObservation->field, m_wordRefinementStartOffset, m_wordRefinementEndOffset, m_wordRefinementOriginal);
    return true;
}

quint64 HostController::refuseWordApply(const QString& reason) {
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint inline apply] phase=refused reason=" << reason;
    if (m_hasWordRefinement && !m_wordRefinementInFlight)
        setWordCompanionState(QStringLiteral("failed"), reason);
    return 0;
}

void HostController::retryWordRefinement() {
    if (m_wordRefinementInFlight || !m_ambientObservation || isCardVisible()) return;
    const auto field = m_ambientObservation->field;
    clearWordRefinement();
    detectWordRefinements(field);
    m_wordCompanionRequested = true; // Retry is explicit, unlike passive typing.
    emit wordRefinementChanged();
    // Refresh the actual editor too; any change replaces the pending snapshot
    // before dispatch, and native insertion always verifies the frozen text again.
    m_accessibility->refreshFocusedField();
}

void HostController::onRangeInjectionFinished(quint64 requestId, InjectionOutcome outcome, const QString& detail) {
    if (!m_pendingWordApply || requestId != m_pendingWordApply->requestId) return;
    const auto pending = std::exchange(m_pendingWordApply, std::nullopt).value();
    m_wordRefinementInFlight = false;
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint inline apply] phase=completed request=" << requestId << "outcome=" << toString(outcome);
    const bool current = m_wordFeedbackGeneration == pending.feedbackGeneration &&
        m_workspaceEpoch == pending.workspaceEpoch && m_pipelineSerial == pending.pipelineSerial &&
        m_ambientObservation && m_ambientObservation->field.fieldToken == pending.fieldToken &&
        m_ambientObservation->field.processId == pending.processId &&
        (m_ambientObservation->field.text == pending.before || m_ambientObservation->field.text == pending.after);
    if (!current) { emit wordRefinementChanged(); return; }
    const bool requested = m_wordCompanionRequested;
    clearWordRefinement();
    m_wordCompanionRequested = requested;
    m_wordRefinementAnchorRect = pending.anchor;
    m_wordRefinementOriginal = pending.original;
    m_wordRefinementReplacement = pending.replacement;
    m_wordRefinementFieldToken = pending.fieldToken;
    m_wordRefinementFieldText = outcome == InjectionOutcome::Injected ? pending.after : pending.before;
    if (outcome == InjectionOutcome::Injected) {
        if (!pending.undo) m_wordUndo = pending;
        setWordCompanionState(pending.undo ? QStringLiteral("undone") : QStringLiteral("applied"),
            pending.undo ? QStringLiteral("Original wording restored.") : QStringLiteral("Applied to your draft."));
        setAmbientStatus(QStringLiteral("Applied inline refinement."));
        const auto generation = ++m_wordFeedbackGeneration;
        QTimer::singleShot(7000, this, [this, generation] {
            if (generation == m_wordFeedbackGeneration && (m_wordCompanionState == QLatin1String("applied") || m_wordCompanionState == QLatin1String("undone")))
                clearWordRefinement();
        });
    } else {
        const auto message = detail.isEmpty()
            ? QStringLiteral("The editor didn't confirm the change. Check your draft before trying again.") : detail;
        setWordCompanionState(QStringLiteral("failed"), message);
        setAmbientStatus(QStringLiteral("Inline refinement failed: %1").arg(message));
    }
}

quint64 HostController::applyWordRefinement(quint64 expectedVersion, const QString& candidateId) {
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint inline apply] phase=request expected_version=" << expectedVersion << "current_version=" << stateVersion();
    if (m_wordRefinementInFlight) return 0;
    if (m_wordRefinementNeedsClarification || !m_hasWordRefinement || !m_ambientObservation)
        return refuseWordApply(QStringLiteral("Refocus your editor and check this phrase again."));
    if (expectedVersion != 0 && expectedVersion != stateVersion())
        return refuseWordApply(QStringLiteral("This suggestion is out of date. Check your current draft again."));
    const auto& observation = *m_ambientObservation;
    if (observation.field.fieldToken != m_wordRefinementFieldToken ||
        observation.field.text != m_wordRefinementFieldText ||
        m_workspaceEpoch != m_wordRefinementWorkspaceEpoch ||
        m_pipelineSerial != m_wordRefinementPipelineSerial ||
        stateVersion() != m_wordRefinementStateVersion) {
        return refuseWordApply(QStringLiteral("Your draft or workspace changed. Check the current draft again."));
    }

    const QString chosenId = candidateId.isEmpty() ? m_wordRefinementSelectedCandidateId : candidateId;
    QVariantMap chosen;
    bool found = false;
    for (const auto& item : m_wordRefinementCandidates) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("id")).toString() == chosenId) {
            chosen = map;
            found = true;
            break;
        }
    }
    if (!found) return refuseWordApply(QStringLiteral("Choose a current suggestion before applying."));
    if (chosen.value(QStringLiteral("requiresClarification")).toBool()) return refuseWordApply(QStringLiteral("This wording needs your answer first."));

    const QString replacement = chosen.value(QStringLiteral("replacement")).toString();
    const QString originalSpan = chosen.value(QStringLiteral("originalSpan")).toString();
    if (replacement.isEmpty() || originalSpan != m_wordRefinementOriginal) return refuseWordApply(QStringLiteral("This suggestion is no longer current. Check again."));
    if (m_wordRefinementStartOffset < 0 ||
        m_wordRefinementEndOffset <= m_wordRefinementStartOffset ||
        m_wordRefinementEndOffset > m_wordRefinementFieldText.size()) return refuseWordApply(QStringLiteral("The selected phrase changed. Check your current draft again."));
    if (m_wordRefinementFieldText.mid(m_wordRefinementStartOffset,
                                      m_wordRefinementEndOffset - m_wordRefinementStartOffset) != originalSpan) {
        return refuseWordApply(QStringLiteral("The selected phrase changed. Check your current draft again."));
    }

    // Freeze the selected request before crossing the accessibility boundary. A delayed
    // completion from this request can no longer clear or overwrite a newer suggestion.
    m_wordRefinementSelectedCandidateId = chosenId;
    m_wordRefinementReplacement = replacement;
    m_wordRefinementExplanation = chosen.value(QStringLiteral("explanation")).toString();
    m_wordRefinementAssumption = chosen.value(QStringLiteral("assumption")).toString();
    m_wordRefinementInFlight = true;
    m_wordRefinementInFlightSuggestionId = m_wordRefinementSuggestionId;

    RangeInjectionRequest req;
    req.requestId = ++m_wordRefinementRequestId;
    req.fieldToken = observation.field.fieldToken;
    req.kind = observation.field.kind;
    req.startOffset = m_wordRefinementStartOffset;
    req.endOffset = m_wordRefinementEndOffset;
    req.originalRangeText = originalSpan;
    req.replacementText = replacement;
    req.expectedCurrentText = m_wordRefinementFieldText;
    m_pendingWordApply = PendingWordApply{req.requestId, observation.field.fieldToken, m_workspaceEpoch,
        m_pipelineSerial, m_wordFeedbackGeneration, observation.field.processId, m_wordRefinementFieldText,
        m_wordRefinementFieldText.left(req.startOffset) + replacement + m_wordRefinementFieldText.mid(req.endOffset),
        originalSpan, replacement, m_wordRefinementAnchorRect, req.startOffset};
    setWordCompanionState(QStringLiteral("applying"), QStringLiteral("Applying to your draft…"));
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint inline apply] phase=queued request=" << req.requestId << "field=" << req.fieldToken;
    emit wordRefinementChanged();
    m_accessibility->injectRange(req);
    return req.requestId;
}

void HostController::selectWordRefinementCandidate(const QString& candidateId) {
    for (const auto& item : m_wordRefinementCandidates) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("id")).toString() != candidateId) continue;
        if (map.value(QStringLiteral("requiresClarification")).toBool()) return;
        m_wordRefinementSelectedCandidateId = candidateId;
        m_wordRefinementReplacement = map.value(QStringLiteral("replacement")).toString();
        m_wordRefinementExplanation = map.value(QStringLiteral("explanation")).toString();
        m_wordRefinementAssumption = map.value(QStringLiteral("assumption")).toString();
        emit wordRefinementChanged();
        return;
    }
}

void HostController::dismissWordRefinement() {
    if (!m_wordRefinementKey.isEmpty()) m_wordRefinementDismissedKeys.insert(m_wordRefinementKey);
    clearWordRefinement();
}

void HostController::exploreInCompanion() {
    const quint64 version = m_revisionState ? stateVersion() : 0;
    (void)openInterviewFromCompanion(version);
}

quint64 HostController::openRewriteFromCompanion(quint64 expectedVersion) {
    if (expectedVersion != stateVersion() || stateVersion() != m_wordRefinementStateVersion || m_wordRefinementKey.isEmpty() ||
        m_wordRefinementInFlight || !m_ambientObservation ||
        m_ambientObservation->field.fieldToken != m_wordRefinementFieldToken ||
        m_ambientObservation->textHash != m_wordRefinementTextHash ||
        m_wordRefinementWorkspaceEpoch != m_workspaceEpoch ||
        m_wordRefinementPipelineSerial != m_pipelineSerial) return 0;
    const auto observation = *m_ambientObservation;
    const auto epoch = m_workspaceEpoch;
    const auto acceptedAnswer = m_acceptedWordClarification;
    if (!m_wordRefinementKey.isEmpty()) m_wordRefinementDismissedKeys.insert(m_wordRefinementKey);
    clearWordRefinement();
    return enqueueStorageCommand("openRewriteFromCompanion", [this, observation, epoch, expectedVersion, acceptedAnswer]() -> AsyncTask<core::Status> {
        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (epoch != m_workspaceEpoch || !m_ambientObservation ||
            m_ambientObservation->field.fieldToken != observation.field.fieldToken ||
            m_ambientObservation->textHash != observation.textHash)
            co_return makeError(ErrorCode::StaleVersion, "The editor draft changed before rewriting.");
        if (auto status = co_await openFromAmbientOperation(observation); !status.ok()) co_return status;
        if (epoch != m_workspaceEpoch || !m_revisionState ||
            m_revisionState->currentRevision().text() != ss(observation.prompt) ||
            !m_captureTarget || m_captureTarget->field.fieldToken != observation.field.fieldToken ||
            !m_ambientObservation || m_ambientObservation->field.fieldToken != observation.field.fieldToken ||
            m_ambientObservation->textHash != observation.textHash)
            co_return makeError(ErrorCode::StaleVersion, "The editor draft changed before rewriting.");
        if (acceptedAnswer && acceptedAnswer->workspaceEpoch == epoch &&
            acceptedAnswer->draftText == observation.field.text &&
            acceptedAnswer->fieldToken == observation.field.fieldToken &&
            acceptedAnswer->processId == observation.field.processId) {
            auto handoff = *acceptedAnswer;
            handoff.sessionId = currentSessionId();
            handoff.revisionId = m_revisionState->currentRevision().revisionId().value();
            handoff.pipelineSerial = m_pipelineSerial;
            m_pendingWordClarificationHandoff = std::move(handoff);
            if (auto status = co_await commitPendingWordClarification(); !status.ok()) co_return status;
        }
        emit companionRewriteRequested(currentRevisionText(), stateVersion());
        co_return Status::success();
    });
}

AsyncTask<core::Status> HostController::commitPendingWordClarification() {
    if (!m_pendingWordClarificationHandoff) co_return Status::success();
    const auto pending = *m_pendingWordClarificationHandoff;
    if (!m_revisionState || pending.workspaceEpoch != m_workspaceEpoch ||
        pending.pipelineSerial != m_pipelineSerial || pending.sessionId != currentSessionId() ||
        pending.revisionId != m_revisionState->currentRevision().revisionId().value() ||
        ss(pending.draftText) != m_revisionState->currentRevision().text() || m_revisionState->isDraftDirty() ||
        !m_captureTarget || pending.fieldToken != m_captureTarget->field.fieldToken ||
        pending.processId != m_captureTarget->field.processId) {
        m_pendingWordClarificationHandoff.reset();
        co_return makeError(ErrorCode::StaleVersion, "The draft changed before your phrase answer could be included.");
    }
    // Analysis may still be running. The normal reevaluation commits this exact
    // answer before confirming intent or allowing the requested full rewrite.
    if (!m_currentAnalysis || !m_currentSnapshot) co_return Status::success();
    auto records = m_interviewRecords;
    core::intent::InterviewRecord record;
    record.id = newOpaqueId("iv_word_");
    record.question = ss(pending.question);
    record.rawAnswer = ss(pending.answer);
    record.sourceExcerpt = ss(pending.original);
    record.sourceRef = pending.revisionId;
    record.origin = "word_clarification";
    record.disposition = "custom";
    record.asked = true;
    records.push_back(std::move(record));
    if (auto status = co_await commitInterviewRecordChange(stateVersion(), std::move(records)); !status.ok()) co_return status;
    m_pendingWordClarificationHandoff.reset();
    co_return Status::success();
}

void HostController::clearWordRefinement() {
    const bool changed = m_hasWordRefinement || m_wordCompanionState != QLatin1String("idle") ||
        m_wordCompanionRequested || !m_wordRefinementOriginal.isEmpty();
    ++m_wordFeedbackGeneration;
    m_wordCompanionRequested = false;
    m_wordCompanionState = QStringLiteral("idle");
    m_wordCompanionMessage.clear();
    m_wordUndo.reset();
    m_wordMarkRects.clear();
    m_wordClarificationChoices.clear();
    m_wordClarificationAnswer.clear();
    m_acceptedWordClarification.reset();
    ++m_wordRefinementSuggestionId;
    m_wordRefinementDebounce.stop();
    m_pendingWordRefinement = {};
    if (m_wordRefinementCancellation) m_wordRefinementCancellation->store(true);
    m_hasWordRefinement = false;
    m_wordRefinementOriginal.clear();
    m_wordRefinementReplacement.clear();
    m_wordRefinementExplanation.clear();
    m_wordRefinementAssumption.clear();
    m_wordRefinementAlternatives.clear();
    m_wordRefinementCandidates.clear();
    m_wordRefinementSelectedCandidateId.clear();
    m_wordRefinementClarificationQuestion.clear();
    m_wordRefinementNeedsClarification = false;
    m_wordRefinementKey.clear();
    m_wordRefinementStartOffset = -1;
    m_wordRefinementEndOffset = -1;
    if (changed) emit wordRefinementChanged();
}

void HostController::startPendingWordRefinement() {
    if (m_wordRefinementGenerating || m_wordRefinementInFlight ||
        m_wordRefinementDebounce.isActive() || !m_pendingWordRefinement ||
        m_hostState == HostState::Quitting) return;
    // Copy before invocation: successful dispatch clears the stored function.
    const auto pending = m_pendingWordRefinement;
    pending();
}

void HostController::detectWordRefinements(const ObservedField& field) {
    if (!isAmbientActive() || isCardVisible() || field.text.isEmpty() || field.kind == FieldKind::Terminal) {
        clearWordRefinement();
        return;
    }
    if (m_wordRefinementInFlight) {
        if (m_pendingWordApply && (field.fieldToken != m_pendingWordApply->fieldToken ||
            field.processId != m_pendingWordApply->processId ||
            (field.text != m_pendingWordApply->before && field.text != m_pendingWordApply->after)))
            clearWordRefinement();
        return;
    }
    // Keep the short confirmation visible through its own observation echo.
    // Actual typing or a different field replaces it immediately.
    if ((m_wordCompanionState == QLatin1String("applied") || m_wordCompanionState == QLatin1String("undone")) &&
        field.fieldToken == m_wordRefinementFieldToken && field.text == m_wordRefinementFieldText) {
        // The native completion may precede its observation echo. Publish the
        // now-current Undo eligibility without starting another model request.
        emit wordRefinementChanged();
        return;
    }

    const QString text = field.text;
    const int caret = (field.caretOffset >= 0 && field.caretOffset <= text.size()) ? field.caretOffset : text.size();

    auto span = wordSpanAtCaret(text, caret);
    if (field.wordStartOffset >= 0) {
        const auto observedSpan = wordSpanAtCaret(text, field.wordStartOffset);
        if (observedSpan && observedSpan->first == field.wordStartOffset && observedSpan->second == field.wordEndOffset)
            span = observedSpan;
    }
    if (!span) {
        clearWordRefinement();
        return;
    }
    const auto [wordStart, wordEnd] = *span;

    const QString word = text.mid(wordStart, wordEnd - wordStart);
    if (word.size() < 3) {
        clearWordRefinement();
        return;
    }

    const QString key = QStringLiteral("%1|%2|%3|%4")
        .arg(field.fieldToken)
        .arg(qs(core::crypto::Sha256::hexDigest(ss(text))))
        .arg(wordStart)
        .arg(wordEnd);
    if (m_wordRefinementDismissedKeys.contains(key)) {
        clearWordRefinement();
        return;
    }
    if (key == m_wordRefinementKey && (m_hasWordRefinement || m_pendingWordRefinement || m_wordRefinementGenerating)) {
        const auto rects = field.wordStartOffset == wordStart && field.wordEndOffset == wordEnd ? field.wordRects : QList<QRect>{};
        const auto anchor = !rects.isEmpty() ? rects.constFirst() : !field.caretRect.isNull() ? field.caretRect : field.fieldRect;
        if (anchor != m_wordRefinementAnchorRect || rects != m_wordMarkRects) {
            m_wordMarkRects = rects;
            m_wordRefinementAnchorRect = anchor;
            emit wordRefinementChanged();
        }
        return;
    }

    clearWordRefinement();
    m_wordRefinementKey = key;
    m_wordRefinementFieldText = text;
    m_wordRefinementFieldToken = field.fieldToken;
    m_wordRefinementStartOffset = wordStart;
    m_wordRefinementEndOffset = wordEnd;
    m_wordRefinementOriginal = word;
    m_wordMarkRects = field.wordStartOffset == wordStart && field.wordEndOffset == wordEnd ? field.wordRects : QList<QRect>{};
    m_wordRefinementAnchorRect = !m_wordMarkRects.isEmpty() ? m_wordMarkRects.constFirst() : !field.caretRect.isNull() ? field.caretRect : field.fieldRect;

    // Debounce so fast typing does not queue a model request for every intermediate word.
    const quint64 generation = ++m_wordRefinementSuggestionId;
    m_wordRefinementDebounce.stop();
    m_wordRefinementDebounce.setSingleShot(true);
    m_wordRefinementDebounce.setInterval(550);
    m_wordRefinementTextHash = core::crypto::Sha256::hexDigest(ss(text));
    m_wordRefinementWorkspaceEpoch = m_workspaceEpoch;
    m_wordRefinementPipelineSerial = m_pipelineSerial;
    m_wordRefinementStateVersion = stateVersion();
    disconnect(&m_wordRefinementDebounce, &QTimer::timeout, this, nullptr);
    m_pendingWordRefinement = [this, field, wordStart, wordEnd, word, generation]() {
        if (generation != m_wordRefinementSuggestionId) return;
        requestWordRefinement(field, wordStart, wordEnd, word);
    };
    connect(&m_wordRefinementDebounce, &QTimer::timeout, this, &HostController::startPendingWordRefinement);
    m_wordRefinementDebounce.start();
}

void HostController::requestWordRefinement(const ObservedField& field, int startOffset, int endOffset,
                                           const QString& selectedText) {
    if (m_wordRefinementGenerating || m_hostState == HostState::Quitting) return;
    if (!isAmbientActive() || isCardVisible() || !m_ambientObservation.has_value()) return;
    if (m_ambientObservation->field.fieldToken != field.fieldToken ||
        m_ambientObservation->field.text != field.text) return;
    setWordCompanionState(QStringLiteral("checking"),
        nativeGraniteSelected() && !usesTransportOverride() && !m_native->isReady()
            ? QStringLiteral("Loading the local model…") : QStringLiteral("Checking this phrase…"));
    // Inline assistance must work on the first draft, before any HUD analysis
    // has loaded Granite. Retain only the latest frozen request during warm-up.
    if (nativeGraniteSelected() && !usesTransportOverride() && !m_native->isReady()) {
        if (!m_nativeWarming && m_native->state() != QLatin1String("LOADING") &&
            !(m_isDownloadingModel && m_downloadingNativeTag == m_activeModel))
            prepareNativeModel();
        return;
    }
    const auto runtime = m_graniteAdapter->state();
    if (runtime != bridge::GraniteRuntimeState::Ready && !usesTransportOverride()) return;

    // isReady remains true while BUSY; wait for a READY snapshot so a scoped cancel
    // acknowledgement or another operation's completion can replay the latest request.
    if (nativeGraniteSelected() && !usesTransportOverride() &&
        (!m_native->isReady() || m_native->state() != QLatin1String("READY"))) return;

    PipelineTicket ticket;
    ticket.sessionId = m_revisionState ? currentSessionId() : std::string("sess_word_companion");
    ticket.revisionId = m_revisionState
        ? m_revisionState->currentRevision().revisionId().value()
        : std::string("rev_word_companion");
    ticket.stateVersion = stateVersion();
    ticket.serial = m_pipelineSerial;
    ticket.workspaceEpoch = m_workspaceEpoch;
    ticket.reviewTrigger = m_reviewTrigger;
    ticket.attemptId = newOpaqueId("word_attempt_");
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    ticket.analysisGroup = std::make_shared<AnalysisGroup>(m_thinkingEnabled ? analysisGroupBudgetMs() : m_analysisGroupTimeoutMs);
    ticket.analysisGroup->id = newOpaqueId("word_group_");
    const auto wordDeadline = ticket.analysisGroup->deadline;

    bridge::WordRefinementRequest request;
    request.sessionId = ticket.sessionId;
    request.revisionId = ticket.revisionId;
    request.draftText = ss(field.text);
    request.selectedText = ss(selectedText);
    request.selectionStart = startOffset;
    request.selectionEnd = endOffset;
    request.intervention = bridge::WordIntervention::LocalRefinement;

    // Approved decisions and explicit constraints are supplied only when they belong to this
    // exact draft. A decision from another prompt must never steer a local replacement.
    const bool currentAnalysisMatches = m_currentAnalysis && m_revisionState &&
        m_revisionState->currentRevision().text() == ss(field.text);
    if (currentAnalysisMatches) {
        for (const auto& group : m_currentAnalysis->choiceGroups) {
            const auto chosen = m_workingIntent.confirmedAlternatives.find(group.id);
            if (chosen == m_workingIntent.confirmedAlternatives.end()) continue;
            const auto alt = std::find_if(group.alternatives.begin(), group.alternatives.end(),
                [&](const auto& candidate) { return candidate.id == chosen->second; });
            if (alt != group.alternatives.end()) {
                request.approvedDecisions.push_back(group.question + " -> " + alt->interpretation);
            }
        }
    }
    if (currentAnalysisMatches && m_confirmedIntent) {
        for (const auto& item : m_confirmedIntent->items) {
            if (item.kind == intent::IntentResolutionKind::CustomText && !item.customText.empty()) {
                request.explicitConstraints.push_back(item.customText);
            }
        }
    }

    // Bind keep quotations to actual draft excerpts and already-approved decisions.
    auto wordSchema = core::json::parse(bridge::wordRefinementResponseSchema(ss(field.text))).value();
    auto branches = wordSchema.find("oneOf")->asArray();
    auto fields = *branches[0].find("properties");
    auto source = *fields.find("source");
    auto quotes = source.find("enum") ? *source.find("enum") : core::json::Value::array();
    for (const auto& decision : request.approvedDecisions)
        if (!decision.empty() && decision.size() <= 160) quotes.push(core::json::Value::string(decision));
    if (!quotes.asArray().empty()) source.set("enum", std::move(quotes));
    fields.set("source", std::move(source)); branches[0].set("properties", std::move(fields));
    auto schemas = core::json::Value::array(); for (auto& branch : branches) schemas.push(std::move(branch));
    wordSchema.set("oneOf", std::move(schemas));
    const auto transport = currentTransport(core::json::serialize(wordSchema), &ticket,
                                            QStringLiteral("granite_word_refinement"), field.text, {}, {}, cancellation,
                                            QStringLiteral("word"));
    const auto wordSurfaceTransport = currentTransport(bridge::wordSurfaceCheckResponseSchema(), &ticket,
                                            QStringLiteral("granite_word_refinement"), field.text, {}, {}, cancellation,
                                            QStringLiteral("word"));
    const auto wordAuditTransport = currentTransport(bridge::wordRefinementAuditResponseSchema(), &ticket,
                                            QStringLiteral("granite_word_refinement"), field.text, {}, {}, cancellation,
                                            QStringLiteral("word"));
    const auto answerCheckTransport = currentTransport(bridge::clarificationCheckResponseSchema(), &ticket,
                                            QStringLiteral("granite_analyze"), field.text, {}, {}, cancellation, QStringLiteral("word"));
    const auto answerAccepted = std::make_shared<std::atomic_bool>(false);
    const auto answer = ss(m_wordClarificationAnswer);
    const auto question = ss(m_wordRefinementClarificationQuestion);
    if (!transport) {
        setAmbientStatus(QStringLiteral("Inline suggestions need a loaded local model. Analyze in the companion instead."));
        return;
    }


    const auto groundingInput = groundingInputs(ss(field.text));
    const quint64 suggestionId = m_wordRefinementSuggestionId;
    const quint64 workspaceEpoch = m_workspaceEpoch;
    const quint64 pipelineSerial = m_pipelineSerial;
    const quint64 boundVersion = m_wordRefinementStateVersion;
    const std::string textHash = m_wordRefinementTextHash;
    const std::string sessionId = request.sessionId;
    const std::string revisionId = request.revisionId;

    m_pendingWordRefinement = {};
    m_wordRefinementGenerating = true;
    m_wordRefinementCancellation = cancellation;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    ++m_inFlight;
    if (nativeWork) ++m_nativeSemanticInFlight;
    using Result = core::Result<bridge::WordRefinementResponse>;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, suggestionId, boundVersion, sessionId, revisionId, textHash, request,
         workspaceEpoch, pipelineSerial, nativeWork, cancellation, answerAccepted, answer, question, field]() {
            const auto result = watcher->result();
            watcher->deleteLater();
            --m_inFlight;
            if (nativeWork) --m_nativeSemanticInFlight;
            m_wordRefinementGenerating = false;
            m_wordRefinementCancellation.reset();
            finishQuitWhenRuntimesStopped();
            QTimer::singleShot(0, this, &HostController::startPendingWordRefinement);
            if (cancellation->load() || suggestionId != m_wordRefinementSuggestionId || workspaceEpoch != m_workspaceEpoch ||
                pipelineSerial != m_pipelineSerial || stateVersion() != boundVersion ||
                !m_ambientObservation.has_value() ||
                m_ambientObservation->field.fieldToken != m_wordRefinementFieldToken ||
                m_ambientObservation->field.processId != field.processId ||
                m_ambientObservation->textHash != textHash || isCardVisible()) {
                return;
            }
            if (answerAccepted->load()) {
                m_acceptedWordClarification = WordClarificationDecision{
                    qs(question), qs(answer), qs(request.selectedText), field.text,
                    field.fieldToken, workspaceEpoch, pipelineSerial, field.processId, {}, {}};
            }
            finishWordRefinementRequest(suggestionId, boundVersion, sessionId, revisionId, textHash,
                                        request, result);
        });
    watcher->setFuture(QtConcurrent::run(
        [transport, wordAuditTransport, wordSurfaceTransport, answerCheckTransport, answerAccepted, answer, question, request, groundingInput, cancellation, wordDeadline]() mutable -> Result {
            try {
                request.astContextXml = computeGroundedPromptContext(groundingInput).xml;
                if (cancellation->load() || wordDeadline.hasExpired()) return makeError(ErrorCode::PreconditionFailed, "Superseded or expired word request");
                if (!answer.empty()) {
                    if (!answerCheckTransport) return makeError(ErrorCode::ProviderUnavailable, "Couldn't check your answer. Please try again.");
                    bridge::ClarificationCheckRequest check;
                    check.prompt = request.draftText; check.question = question; check.answer = answer;
                    auto checked = answerCheckTransport(bridge::buildClarificationCheckPrompt(check));
                    if (cancellation->load() || wordDeadline.hasExpired()) return makeError(ErrorCode::PreconditionFailed, "Superseded or expired answer check");
                    if (!checked.ok()) return checked.error();
                    const auto verdict = bridge::parseClarificationCheckResponse(checked.value());
                    if (!verdict.ok()) return verdict.error();
                    if (!verdict.value().accepted) return makeError(ErrorCode::ValidationFailed,
                        "word_answer_rejected: " + (verdict.value().feedback.empty()
                            ? std::string("That doesn't answer this question yet. Try describing the result you want.") : verdict.value().feedback));
                    answerAccepted->store(true);
                    request.approvedDecisions.push_back(question + " -> " + answer);
                    request.intervention = bridge::WordIntervention::Rewrite;
                }
                auto raw = transport(buildWordRefinementPrompt(request));
                if (cancellation->load() || wordDeadline.hasExpired()) return makeError(ErrorCode::PreconditionFailed, "Superseded or expired word request");
                if (!raw.ok()) return raw.error();
                auto proposed = parseWordRefinementResponse(raw.value(), request);
                if (!proposed.ok()) return proposed;
                // An accepted answer is not another unanswered question. Let the
                // user carry it into a full rewrite instead of repeating the loop.
                if (answerAccepted->load() && (proposed.value().needsClarification || proposed.value().candidates.empty()))
                    return makeError(ErrorCode::ValidationFailed, "No usable local wording for the accepted answer");
                // A question can itself be an unnecessary detour. One focused
                // surface check can repair clear prose or preserve exact text.
                // It shares the original ticket/deadline and cannot approve an edit.
                if (proposed.value().needsClarification) {
                    if (!wordSurfaceTransport) return makeError(ErrorCode::PreconditionFailed, "Word surface check unavailable");
                    auto surfaceRaw = wordSurfaceTransport(bridge::buildWordSurfaceCheckPrompt(request));
                    if (cancellation->load() || wordDeadline.hasExpired()) return makeError(ErrorCode::PreconditionFailed, "Superseded or expired word surface check");
                    if (!surfaceRaw.ok()) return surfaceRaw.error();
                    auto surface = bridge::parseWordSurfaceCheckResponse(surfaceRaw.value(), request);
                    if (!surface.ok()) return surface.error();
                    if (surface.value().handled) proposed = std::move(surface).value().response;
                }
                if (proposed.value().candidates.empty()) return proposed;
                if (!wordAuditTransport) return makeError(ErrorCode::PreconditionFailed, "Word audit unavailable");
                auto audit = wordAuditTransport(buildWordRefinementAuditPrompt(request, proposed.value()));
                if (cancellation->load() || wordDeadline.hasExpired()) return makeError(ErrorCode::PreconditionFailed, "Superseded or expired word audit");
                if (!audit.ok()) return audit.error();
                auto audited = auditWordRefinementResponse(audit.value(), request, proposed.value());
                if (answerAccepted->load() && audited.ok() && audited.value().candidates.empty())
                    return makeError(ErrorCode::ValidationFailed, "No local wording passed the sentence audit for the accepted answer");
                return audited;
            } catch (const std::exception& ex) {
                return makeError(ErrorCode::Internal,
                                 std::string("Word refinement crashed: ") + ex.what());
            }
        }));
}

void HostController::finishWordRefinementRequest(quint64 suggestionId, quint64 stateVersion,
                                                 std::string /*sessionId*/, std::string /*revisionId*/,
                                                 const std::string& textHash,
                                                 const bridge::WordRefinementRequest& request,
                                                 core::Result<bridge::WordRefinementResponse> result) {
    if (suggestionId != m_wordRefinementSuggestionId || textHash != m_wordRefinementTextHash ||
        stateVersion != m_wordRefinementStateVersion) {
        return;
    }
    if (!result.ok()) {
        m_wordClarificationAnswer.clear();
        constexpr std::string_view prefix = "word_answer_rejected: ";
        const auto& message = result.error().message;
        if (m_hasWordRefinement && m_wordRefinementNeedsClarification && !m_acceptedWordClarification) {
            setWordCompanionState(QStringLiteral("ready"), message.starts_with(prefix)
                ? qs(message.substr(prefix.size())) : QStringLiteral("Couldn't check that answer. Try again; your draft is unchanged."));
            return;
        }
        // Keep the exact draft binding so Full rewrite remains an explicit exit.
        // Failed or unaudited candidates must never remain applicable.
        m_hasWordRefinement = false;
        m_wordRefinementCandidates.clear();
        m_wordRefinementAlternatives.clear();
        m_wordRefinementReplacement.clear();
        m_wordRefinementSelectedCandidateId.clear();
        m_wordRefinementNeedsClarification = false;
        m_wordClarificationChoices.clear();
        m_wordRefinementClarificationQuestion.clear();
        setWordCompanionState(QStringLiteral("failed"), m_acceptedWordClarification
            ? QStringLiteral("Your answer is kept. Use Full rewrite to include it; your draft is unchanged.")
            : QStringLiteral("Couldn't check this phrase. Try again or use Full rewrite."));
        emit wordRefinementChanged();
        return;
    }
    const auto response = std::move(result).value();
    if (response.candidates.empty() && !response.needsClarification) {
        if (!m_wordRefinementKey.isEmpty()) m_wordRefinementDismissedKeys.insert(m_wordRefinementKey);
        clearWordRefinement();
        emit wordRefinementChanged();
        return;
    }

    m_wordRefinementCandidates.clear();
    m_wordRefinementAlternatives.clear();
    QString firstApplicableId;
    for (const auto& candidate : response.candidates) {
        QVariantMap item;
        item[QStringLiteral("id")] = qs(candidate.id);
        item[QStringLiteral("originalSpan")] = qs(candidate.originalSpan);
        item[QStringLiteral("replacement")] = qs(candidate.replacement);
        item[QStringLiteral("label")] = qs(candidate.replacement);
        item[QStringLiteral("explanation")] = qs(candidate.explanation);
        item[QStringLiteral("assumption")] = qs(candidate.assumption);
        item[QStringLiteral("requiresClarification")] = candidate.requiresClarification;
        if (!candidate.requiresClarification) {
            if (firstApplicableId.isEmpty()) firstApplicableId = qs(candidate.id);
            m_wordRefinementAlternatives.append(qs(candidate.replacement));
        }
        m_wordRefinementCandidates.append(item);
    }
    m_wordRefinementNeedsClarification = response.needsClarification;
    m_wordClarificationChoices.clear();
    for (const auto& choice : response.clarificationChoices) m_wordClarificationChoices.append(qs(choice));
    m_wordClarificationAnswer.clear();
    m_wordRefinementClarificationQuestion = qs(response.clarificationQuestion);
    if (firstApplicableId.isEmpty()) {
        m_wordRefinementNeedsClarification = true;
        if (m_wordRefinementClarificationQuestion.isEmpty()) {
            m_wordRefinementClarificationQuestion = QStringLiteral("Tell Roclint what this word should mean here.");
        }
    }
    m_wordRefinementSelectedCandidateId = firstApplicableId;
    if (!firstApplicableId.isEmpty()) {
        selectWordRefinementCandidate(firstApplicableId);
    } else {
        const auto first = m_wordRefinementCandidates.value(0).toMap();
        // Show the model's most plausible reading even when it must be clarified before use.
        m_wordRefinementSelectedCandidateId = first.value(QStringLiteral("id")).toString();
        m_wordRefinementReplacement = first.value(QStringLiteral("replacement")).toString();
        m_wordRefinementExplanation = first.value(QStringLiteral("explanation")).toString();
        m_wordRefinementAssumption = first.value(QStringLiteral("assumption")).toString();
    }
    m_wordRefinementOriginal = qs(request.selectedText);
    m_wordCompanionState = QStringLiteral("ready");
    m_wordCompanionMessage.clear();
    m_hasWordRefinement = true;
    emit wordRefinementChanged();
}

// ----------------------------------------------------------------------------------------------
// Shared intent brief and adaptive interview
// ----------------------------------------------------------------------------------------------

namespace {

[[nodiscard]] QString boundedText(const std::string& text, std::size_t maximum) {
    if (text.size() <= maximum) return qs(text);
    std::size_t end = maximum;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return qs(text.substr(0, end));
}

[[nodiscard]] QString joinedConsequence(const std::vector<core::intent::InterviewChoice>& choices) {
    QStringList parts;
    for (const auto& choice : choices) {
        if (choice.consequence.empty()) continue;
        const auto text = boundedText(choice.consequence, 120).trimmed();
        if (!text.isEmpty() && !parts.contains(text)) parts << text;
    }
    return parts.join(QStringLiteral("; ")).left(400);
}

} // namespace

core::intent::IntentBrief HostController::buildIntentBrief() const {
    core::intent::IntentBriefInputs inputs;
    inputs.sessionId = m_revisionState ? currentSessionId() : std::string();
    inputs.revisionId = m_revisionState ? m_revisionState->currentRevision().revisionId().value() : std::string();
    inputs.revisionText = m_revisionState ? m_revisionState->currentRevision().text() : std::string();
    inputs.workspaceIdentity = ss(m_workspaceRootIdentity);
    inputs.workspaceEpoch = m_workspaceEpoch;
    inputs.stateVersion = m_revisionState ? m_revisionState->stateVersion() : 0;
    if (m_currentSnapshot) {
        inputs.snapshotId = m_currentSnapshot->id;
        inputs.snapshotHash = m_currentSnapshot->snapshotHash;
        inputs.analysis = &m_currentSnapshot->normalized;
        inputs.policy = &m_currentPolicyResult;
    } else if (m_currentAnalysis) {
        inputs.analysis = &*m_currentAnalysis;
        inputs.policy = &m_currentPolicyResult;
    }
    inputs.confirmedIntent = m_confirmedIntent ? &*m_confirmedIntent : nullptr;
    inputs.workingIntent = &m_workingIntent;
    inputs.interviewRecords = m_interviewRecords;

    // Only non-user constraints are passed as source-linked constraints; user clauses
    // are extracted verbatim by the brief builder so decisions are not duplicated.
    if (m_confirmedIntent && m_revisionState) {
        for (auto& constraint : extractSessionConstraints(m_revisionState->currentRevision().text(), *m_confirmedIntent)) {
            if (constraint.origin != core::Origin::User) inputs.sourceConstraints.push_back(std::move(constraint));
        }
    }
    for (const auto& evidence : m_activeGroundedEvidence) {
        if (inputs.evidence.size() >= 8) break;
        core::intent::BriefEvidenceRef ref;
        ref.ref = evidence.filePath.empty() ? evidence.target : evidence.filePath + ":" + evidence.target;
        ref.excerpt = !evidence.signature.empty() ? evidence.signature : evidence.target;
        ref.detail = evidence.status + " " + evidence.target;
        if (!evidence.filePath.empty()) ref.detail += " (" + evidence.filePath + ")";
        inputs.evidence.push_back(std::move(ref));
    }

    // The frozen word selection is preserved in the shared brief as a proposed
    // assumption: the user's selected phrase is verbatim, the model's wording is
    // explicitly not confirmed.
    auto brief = core::intent::IntentBriefBuilder::build(inputs);
    if (m_interviewWordContext.has_value() && !m_interviewWordContext->original.isEmpty()) {
        core::intent::BriefItem item;
        item.id = "word_selection";
        item.kind = core::intent::BriefItemKind::ProposedAssumption;
        item.status = core::intent::BriefItemStatus::Proposed;
        item.text = "Selected phrase: \"" + ss(m_interviewWordContext->original) + "\"";
        if (!m_interviewWordContext->replacement.isEmpty()) {
            item.text += " - proposed wording: \"" + ss(m_interviewWordContext->replacement) + "\"";
        }
        item.sourceExcerpt = ss(m_interviewWordContext->original);
        item.sourceRef = "word:" + std::to_string(m_interviewWordContext->startOffset) + "-" +
                         std::to_string(m_interviewWordContext->endOffset);
        item.consequential = false;
        item.analysisItemId = "word_selection";
        brief.items.push_back(std::move(item));
    }
    return brief;
}

bool HostController::hasIntentBrief() const {
    return m_currentAnalysis.has_value() || m_confirmedIntent.has_value() || !m_interviewRecords.empty();
}

QVariantList HostController::intentBriefItems() const {
    QVariantList list;
    const auto brief = buildIntentBrief();
    list.reserve(static_cast<qsizetype>(brief.items.size()));
    for (const auto& item : brief.items) {
        QVariantMap map;
        map[QStringLiteral("id")] = qs(item.id);
        map[QStringLiteral("kind")] = QString::fromUtf8(core::intent::toString(item.kind));
        map[QStringLiteral("text")] = qs(item.text);
        map[QStringLiteral("sourceExcerpt")] = qs(item.sourceExcerpt);
        map[QStringLiteral("sourceRef")] = qs(item.sourceRef);
        map[QStringLiteral("rawAnswer")] = qs(item.rawAnswer);
        map[QStringLiteral("status")] = QString::fromUtf8(core::intent::toString(item.status));
        map[QStringLiteral("consequential")] = item.consequential;
        map[QStringLiteral("analysisItemId")] = qs(item.analysisItemId);
        list.append(map);
    }
    return list;
}

QStringList HostController::intentOpenChoiceIds() const {
    QStringList ids;
    for (const auto& id : buildIntentBrief().openChoiceIds()) ids << qs(id);
    return ids;
}

QString HostController::intentBriefSummary() const {
    return qs(buildIntentBrief().displaySummary);
}

bool HostController::canExploreInterview() const {
    if (m_interviewActive) return false;
    return m_hasWordRefinement || m_currentAnalysis.has_value() || m_confirmedIntent.has_value() ||
           (m_revisionState && !m_revisionState->currentRevision().text().empty()) ||
           m_ambientObservation.has_value();
}

std::vector<core::intent::SupplementalDecision> HostController::supplementalDecisionsFrom(
    const std::vector<core::intent::InterviewRecord>& records) {
    std::vector<core::intent::SupplementalDecision> decisions;
    for (const auto& record : records) {
        if (!record.analysisItemId.empty() || !record.isDecision()) continue;
        std::string text = record.rawAnswer;
        if (!record.selectedChoiceId.empty()) {
            for (const auto& choice : record.choices) {
                if (choice.id == record.selectedChoiceId) {
                    text = choice.label;
                    break;
                }
            }
        }
        if (text.empty()) continue;
        core::intent::SupplementalDecision decision;
        decision.itemId = record.id;
        decision.text = text;
        decision.rationale = record.question;
        decision.provenance = record.origin;
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

std::vector<core::intent::SupplementalDecision> HostController::supplementalInterviewDecisions() const {
    return supplementalDecisionsFrom(m_interviewRecords);
}

core::intent::InterviewRecord* HostController::findInterviewRecord(std::string_view id) {
    for (auto& record : m_interviewRecords) {
        if (record.id == id) return &record;
    }
    return nullptr;
}

const core::intent::InterviewRecord* HostController::findInterviewRecord(std::string_view id) const {
    return const_cast<HostController*>(this)->findInterviewRecord(id);
}

void HostController::captureWordContextForInterview() {
    if (!m_hasWordRefinement || m_wordRefinementStartOffset < 0) return;
    InterviewWordContext context;
    context.draftText = m_wordRefinementFieldText;
    context.startOffset = m_wordRefinementStartOffset;
    context.endOffset = m_wordRefinementEndOffset;
    context.original = m_wordRefinementOriginal;
    context.replacement = m_wordRefinementReplacement;
    context.explanation = m_wordRefinementExplanation;
    context.assumption = m_wordRefinementAssumption;
    context.selectedCandidateId = m_wordRefinementSelectedCandidateId;
    context.candidates = m_wordRefinementCandidates;
    context.fieldToken = m_wordRefinementFieldToken;
    context.workspaceEpoch = m_workspaceEpoch;
    context.textHash = m_wordRefinementTextHash;
    m_interviewWordContext = std::move(context);
}

void HostController::publishInterviewQuestion(const core::intent::InterviewRecord& record) {
    m_interviewCurrentRecordId = record.id;
    ++m_interviewQuestionSerial;
    m_interviewQuestion = qs(record.question);
    m_interviewConsequence = qs(record.consequence);
    m_interviewChoices.clear();
    for (const auto& choice : record.choices) {
        QVariantMap map;
        map[QStringLiteral("id")] = qs(choice.id);
        map[QStringLiteral("label")] = qs(choice.label);
        map[QStringLiteral("consequence")] = qs(choice.consequence);
        m_interviewChoices.append(map);
    }
    emit interviewChanged();
}

void HostController::seedInterviewFromDecision() {
    if (!m_interviewActive || !m_currentAnalysis) return;
    if (m_currentDecision.verdict != intent::InterventionVerdict::Clarify ||
        !m_currentDecision.activeItemId.has_value()) {
        m_interviewQuestion.clear();
        m_interviewConsequence.clear();
        m_interviewChoices.clear();
        m_interviewCurrentRecordId.clear();
        emit interviewChanged();
        return;
    }
    const std::string id = *m_currentDecision.activeItemId;
    auto* record = findInterviewRecord(id);
    if (record == nullptr) {
        core::intent::InterviewRecord created;
        created.id = id;
        created.analysisItemId = id;
        created.origin = "analysis";
        created.asked = true;
        const auto& a = *m_currentAnalysis;
        switch (m_currentDecision.activeItemKind) {
            case intent::ActiveItemKind::ChoiceGroup: {
                const auto group = std::find_if(a.choiceGroups.begin(), a.choiceGroups.end(),
                                                [&](const analysis::ChoiceGroup& g) { return g.id == id; });
                if (group == a.choiceGroups.end()) return;
                created.question = group->question;
                for (const auto& alt : group->alternatives) {
                    core::intent::InterviewChoice choice;
                    choice.id = alt.id;
                    choice.label = alt.interpretation;
                    choice.consequence = alt.whyMaterial.empty()
                                             ? "This changes what the result emphasizes."
                                             : alt.whyMaterial;
                    created.choices.push_back(std::move(choice));
                    if (created.sourceExcerpt.empty() && !alt.evidence.empty() && m_revisionState &&
                        analysis::isValidSpan(m_revisionState->currentRevision().text(), alt.evidence.front())) {
                        created.sourceExcerpt = m_revisionState->currentRevision().text().substr(
                            alt.evidence.front().startByte, alt.evidence.front().length());
                        created.sourceRef = "revision:" + m_revisionState->currentRevision().revisionId().value() +
                                            "#bytes:" + std::to_string(alt.evidence.front().startByte) + "-" +
                                            std::to_string(alt.evidence.front().endByte);
                    }
                }
                created.consequence = ss(joinedConsequence(created.choices));
                break;
            }
            case intent::ActiveItemKind::Assumption: {
                const auto assumption = std::find_if(a.assumptions.begin(), a.assumptions.end(),
                                                     [&](const analysis::Assumption& item) { return item.id == id; });
                if (assumption == a.assumptions.end()) return;
                created.question = assumption->statement;
                created.consequence = std::string(
                    "Accepting this adds the premise to the rewrite; rejecting it keeps the premise out.");
                created.choices.push_back(
                    core::intent::InterviewChoice{"assume_yes", "Yes, that's right",
                                                  "The rewrite will follow this assumption."});
                created.choices.push_back(
                    core::intent::InterviewChoice{"assume_no", "No, don't assume that",
                                                  "The rewrite will avoid relying on it."});
                if (!assumption->evidence.empty() && m_revisionState &&
                    analysis::isValidSpan(m_revisionState->currentRevision().text(), assumption->evidence.front())) {
                    created.sourceExcerpt = m_revisionState->currentRevision().text().substr(
                        assumption->evidence.front().startByte, assumption->evidence.front().length());
                }
                break;
            }
            case intent::ActiveItemKind::MissingDecision: {
                const auto missing = std::find_if(a.missingDecisions.begin(), a.missingDecisions.end(),
                                                  [&](const analysis::MissingDecision& item) { return item.id == id; });
                if (missing == a.missingDecisions.end()) return;
                created.question = missing->question;
                created.consequence = std::string(
                    "A faithful rewrite needs this detail; otherwise it would have to guess.");
                break;
            }
            case intent::ActiveItemKind::Conflict: {
                const auto conflict = std::find_if(a.conflicts.begin(), a.conflicts.end(),
                                                   [&](const analysis::Conflict& item) { return item.id == id; });
                if (conflict == a.conflicts.end()) return;
                created.question = conflict->description;
                created.consequence = std::string(
                    "These requirements conflict; the rewrite cannot preserve both without your decision.");
                break;
            }
            case intent::ActiveItemKind::None:
                return;
        }
        if (created.question.empty()) return;
        m_interviewRecords.push_back(std::move(created));
        record = &m_interviewRecords.back();
    }
    if (record->disposition == "rejected") record->disposition = "asked";
    publishInterviewQuestion(*record);
}

void HostController::advanceInterview() {
    if (!m_interviewActive) return;
    // 1) The sealed analysis still has a higher-priority open item.
    if (m_currentAnalysis && m_currentDecision.verdict == intent::InterventionVerdict::Clarify &&
        m_currentDecision.activeItemId.has_value()) {
        seedInterviewFromDecision();
        return;
    }
    // 2) An interview-owned question (follow-up or review finding) is still open.
    for (auto it = m_interviewRecords.rbegin(); it != m_interviewRecords.rend(); ++it) {
        if (!it->analysisItemId.empty() || !it->asked) continue;
        if (it->isOpen() && it->disposition != "rejected") {
            publishInterviewQuestion(*it);
            return;
        }
    }
    // 3) Nothing left to ask. Reassess with one bounded model request only while the
    //    interview has not already concluded.
    if (m_interviewComplete) {
        m_interviewQuestion.clear();
        m_interviewConsequence.clear();
        m_interviewChoices.clear();
        m_interviewCurrentRecordId.clear();
        emit interviewChanged();
        return;
    }
    if (!m_interviewBusy) requestInterviewFollowUp();
}

void HostController::setInterviewFeedback(const QString& feedback) {
    if (m_interviewFeedback == feedback) return;
    m_interviewFeedback = feedback;
    emit interviewChanged();
}

void HostController::cancelInterview() {
    ++m_interviewSerial;
    ++m_interviewQuestionSerial;
    if (m_interviewCancellation) m_interviewCancellation->store(true);
    m_interviewCancellation.reset();
    m_interviewBusy = false;
    m_interviewActive = false;
    m_interviewComplete = false;
    m_interviewTurnsCompleted = 0;
    if (!m_interviewOpenPending) m_interviewWordContext.reset();
    m_interviewRecords.clear();
    m_interviewQuestion.clear();
    m_interviewConsequence.clear();
    m_interviewFeedback.clear();
    m_interviewChoices.clear();
    m_interviewCurrentRecordId.clear();
    emit interviewChanged();
    emit intentBriefChanged();
}

quint64 HostController::openInterviewFromCompanion(quint64 expectedVersion) {
    if (m_revisionState) {
        if (expectedVersion != stateVersion()) {
            setLastError(core::makeError(core::ErrorCode::StaleVersion,
                                         "The prompt changed. Review the current state and try again."));
            setInterviewFeedback(QStringLiteral("The prompt changed before the interview opened. Try again."));
            return 0;
        }
    } else if (expectedVersion != 0) {
        setInterviewFeedback(QStringLiteral("The prompt changed before the interview opened. Try again."));
        return 0;
    }

    if (m_interviewActive) {
        // Idempotent re-entry: never restart the explanation or discard answers.
        if (!isCardVisible()) {
            if (m_revisionState) showCardInternal(false);
            else openFromAmbient();
        }
        return stateVersion();
    }

    captureWordContextForInterview();
    if (!m_wordRefinementKey.isEmpty()) m_wordRefinementDismissedKeys.insert(m_wordRefinementKey);
    clearWordRefinement();

    // Opening the larger interview is an explicit request for a deeper look. It
    // must not inherit the automatic route's QUITE/ORIGINAL dead end.
    m_reviewTrigger = analysis::ReviewTrigger::ExplicitReview;
    ++m_interviewSerial;
    m_interviewActive = true;
    m_interviewComplete = false;
    m_interviewBusy = false;
    m_interviewFeedback.clear();
    emit interviewChanged();
    emit intentBriefChanged();

    if (!m_revisionState) {
        if (!m_ambientObservation.has_value()) {
            setInterviewFeedback(QStringLiteral("There is no captured draft to explore yet."));
            m_interviewActive = false;
            emit interviewChanged();
            return 0;
        }
        m_interviewOpenPending = true;
        openFromAmbient();
        return 1; // The new session's root revision version.
    }

    showCardInternal(false);
    const bool emptyAnalysis = m_currentAnalysis.has_value() &&
        m_currentAnalysis->choiceGroups.empty() && m_currentAnalysis->assumptions.empty() &&
        m_currentAnalysis->missingDecisions.empty() && m_currentAnalysis->conflicts.empty();
    const bool hasAnyDecision = !m_workingIntent.confirmedAlternatives.empty() ||
        !m_workingIntent.customResolutions.empty() || !m_workingIntent.assumptionDecisions.empty() ||
        !m_workingIntent.keptOriginalItemIds.empty() || !m_interviewRecords.empty();
    const bool needsExplicitDiscovery = !m_currentAnalysis || (emptyAnalysis && !hasAnyDecision);
    if (needsExplicitDiscovery && !isAnalyzing()) {
        // The measured automatic ORIGINAL/QUIET route stores an empty snapshot and
        // never calls Granite. Explicit Explore must not inherit that dead end: start
        // a fresh bounded discovery, even though an empty snapshot already exists.
        // Completed interview answers are preserved by never restarting here.
        if (m_routedAnalysisGroup && !m_routedAnalysisGroup->terminal) {
            m_routedAnalysisGroup->superseded = true;
            m_routedAnalysisGroup->terminal = true;
        }
        m_routedAnalysisGroup = std::make_shared<AnalysisGroup>(analysisGroupBudgetMs());
        m_routedAnalysisGroup->id = newOpaqueId("grp_");
        // Publish the accepted interview version first: the discovery ticket captures
        // the state version it starts under, and advancing it afterwards would make
        // the analysis result look stale and strand the UI in ANALYZING.
        enqueueStorageCommand("openInterview", [this, expectedVersion]() { return advanceCommandVersion(expectedVersion); });
        enqueueStorageCommand("openInterviewStartAnalysis", [this]() { return startDiscovery(); });
        return expectedVersion + 1;
    }
    if (isAnalyzing()) {
        // An analysis is already in flight for this draft; let it finish rather than
        // invalidating its ticket with a new state version.
        return stateVersion();
    }
    advanceInterview();
    enqueueStorageCommand("openInterview", [this, expectedVersion]() { return advanceCommandVersion(expectedVersion); });
    return expectedVersion + 1;
}

void HostController::requestInterviewFollowUp() {
    if (!m_interviewActive || m_interviewBusy || m_interviewComplete) return;
    if (!m_currentAnalysis || !m_revisionState || m_revisionState->isDraftDirty()) return;
    if (m_hostState == HostState::Quitting) return;
    constexpr int maxOptionalFollowUps = 3;
    const auto followUps = std::count_if(m_interviewRecords.begin(), m_interviewRecords.end(),
        [](const auto& record) { return record.origin == "model_followup"; });
    if (followUps >= maxOptionalFollowUps) {
        m_interviewComplete = true;
        m_interviewFeedback = QStringLiteral("Your answers are saved. You can rewrite when ready.");
        advanceInterview();
        return;
    }
    const auto runtime = m_graniteAdapter->state();
    if (runtime != bridge::GraniteRuntimeState::Ready && !usesTransportOverride()) {
        setInterviewFeedback(QStringLiteral(
            "The local model isn't ready to explore further. You can retry, skip this, or ask for the rewrite."));
        return;
    }

    const auto brief = buildIntentBrief();
    std::vector<std::string> settled;
    for (const auto* item : brief.confirmedDecisions()) {
        if (settled.size() >= 12) break;
        if (item->text.empty()) continue;
        settled.push_back(item->text);
    }
    // Exact source values/units are settled too. Give them to the follow-up as
    // authoritative source wording so the model does not re-ask a supplied value.
    for (const auto& item : brief.items) {
        if (settled.size() >= 14) break;
        if (item.kind != core::intent::BriefItemKind::SourceRequirement &&
            item.kind != core::intent::BriefItemKind::Prohibition) {
            continue;
        }
        if (core::intent::exactRequirementTokens(item.sourceExcerpt).empty()) continue;
        settled.push_back("Source requirement: " + item.sourceExcerpt);
    }
    std::vector<std::string> open;
    for (const auto& id : brief.openChoiceIds()) {
        if (open.size() >= 8) break;
        if (const auto* item = brief.find(id)) open.push_back(item->text);
    }

    PipelineTicket ticket = makeTicket();
    ticket.analysisGroup = std::make_shared<AnalysisGroup>(analysisGroupBudgetMs());
    ticket.analysisGroup->id = newOpaqueId("interview_grp_");
    // This follow-up is a foreground interview turn, not a persisted rewrite attempt.
    // The native helper still requires a stable request identity for traceability and
    // for the frozen cancellation/deadline envelope.
    ticket.attemptId = newOpaqueId("att_interview_");
    const auto deadline = ticket.analysisGroup->deadline;
    const auto revisionText = m_revisionState->currentRevision().text();

    bridge::GraniteOptionDiscoveryRequest request;
    request.sessionId = ticket.sessionId;
    request.revisionId = ticket.revisionId;
    request.promptString = revisionText;
    request.hostContextCategory = ss(m_hostContext.categoryString());
    request.reviewTrigger = analysis::ReviewTrigger::ExplicitReview;
    request.maximumQuestions = 1;
    request.interviewFollowUp = true;
    request.substantialInterview = true;
    request.settledDecisions = std::move(settled);
    request.openChoices = std::move(open);
    const auto transport = currentTransport(bridge::discoveryResponseSchema(revisionText, 1), &ticket,
                                            QStringLiteral("granite_analyze"), qs(revisionText), {}, {}, {},
                                            QStringLiteral("interview"));
    if (!transport) {
        setInterviewFeedback(QStringLiteral("The local model is not available for a follow-up question."));
        return;
    }

    const quint64 serial = ++m_interviewSerial;
    const quint64 workspaceEpoch = m_workspaceEpoch;
    const quint64 pipelineSerial = m_pipelineSerial;
    const std::string revisionId = ticket.revisionId;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_interviewCancellation = cancellation;
    m_interviewBusy = true;
    m_interviewFeedback.clear();
    emit interviewChanged();

    auto adapter = m_graniteAdapter;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    ++m_inFlight;
    if (nativeWork) ++m_nativeSemanticInFlight;
    using Result = core::Result<bridge::GraniteOptionDiscoveryResponse>;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, serial, revisionId, workspaceEpoch, pipelineSerial, nativeWork, cancellation]() {
            auto result = watcher->result();
            watcher->deleteLater();
            --m_inFlight;
            if (nativeWork) --m_nativeSemanticInFlight;
            m_interviewCancellation.reset();
            emit modelEngineStatusChanged();
            finishQuitWhenRuntimesStopped();
            if (cancellation->load()) return;
            finishInterviewFollowUp(serial, revisionId, workspaceEpoch, pipelineSerial, std::move(result));
        });
    watcher->setFuture(QtConcurrent::run([adapter, request, transport, deadline, cancellation]() -> Result {
        try {
            if (cancellation->load() || deadline.hasExpired())
                return makeError(core::ErrorCode::PreconditionFailed, "The interview follow-up expired");
            auto result = adapter->discoverOptions(request, transport);
            if (cancellation->load() || deadline.hasExpired())
                return makeError(core::ErrorCode::PreconditionFailed, "The interview follow-up expired");
            return result;
        } catch (const std::exception& ex) {
            return makeError(core::ErrorCode::Internal, std::string("Interview follow-up crashed: ") + ex.what());
        }
    }));
}

void HostController::finishInterviewFollowUp(quint64 serial, std::string revisionId,
                                             std::uint64_t workspaceEpoch, std::uint64_t pipelineSerial,
                                             core::Result<bridge::GraniteOptionDiscoveryResponse> result) {
    if (serial != m_interviewSerial || !m_interviewActive) return;
    m_interviewBusy = false;
    if (!m_revisionState || revisionId != m_revisionState->currentRevision().revisionId().value() ||
        workspaceEpoch != m_workspaceEpoch || pipelineSerial != m_pipelineSerial) {
        emit interviewChanged();
        return;
    }
    if (!result.ok()) {
        setInterviewFeedback(QStringLiteral(
            "Couldn't prepare another question. Your answers are kept; retry, skip, or ask for the rewrite."));
        emit interviewChanged();
        return;
    }
    auto response = std::move(result).value();
    if (response.outcome == bridge::DiscoveryOutcome::NoAmbiguity) {
        m_interviewComplete = true;
        m_interviewQuestion.clear();
        m_interviewConsequence.clear();
        m_interviewChoices.clear();
        m_interviewCurrentRecordId.clear();
        m_interviewFeedback = QStringLiteral("Enough is understood for a faithful rewrite. You can continue editing or ask for the rewrite.");
        emit interviewChanged();
        emit intentBriefChanged();
        return;
    }

    const auto& a = response.analysis;
    core::intent::InterviewRecord created;
    created.origin = "model_followup";
    created.asked = true;
    created.id = "iv_q_" + std::to_string(m_interviewRecords.size() + 1);
    const auto excerptFor = [&](const analysis::SourceSpan& span) {
        if (!m_revisionState || !analysis::isValidSpan(m_revisionState->currentRevision().text(), span)) return;
        created.sourceExcerpt = m_revisionState->currentRevision().text().substr(span.startByte, span.length());
        created.sourceRef = "revision:" + revisionId + "#bytes:" + std::to_string(span.startByte) + "-" +
                            std::to_string(span.endByte);
    };
    bool grounded = false;
    if (!a.choiceGroups.empty()) {
        const auto& group = a.choiceGroups.front();
        created.question = group.question;
        for (std::size_t i = 0; i < group.alternatives.size(); ++i) {
            const auto& alt = group.alternatives[i];
            core::intent::InterviewChoice choice;
            choice.id = created.id + "_c" + std::to_string(i + 1);
            choice.label = alt.interpretation;
            choice.consequence = alt.whyMaterial.empty() ? "This changes what the result emphasizes." : alt.whyMaterial;
            created.choices.push_back(std::move(choice));
        }
        if (!group.alternatives.empty() && !group.alternatives.front().evidence.empty()) {
            excerptFor(group.alternatives.front().evidence.front());
        }
        created.consequence = ss(joinedConsequence(created.choices));
        grounded = !created.sourceExcerpt.empty();
    } else if (!a.assumptions.empty()) {
        const auto& assumption = a.assumptions.front();
        created.question = assumption.statement;
        created.choices.push_back(core::intent::InterviewChoice{
            created.id + "_yes", "Accept: " + assumption.statement,
            "The rewrite will follow this assumption."});
        created.choices.push_back(core::intent::InterviewChoice{
            created.id + "_no", "Do not assume: " + assumption.statement,
            "The rewrite will avoid relying on it."});
        created.consequence = std::string("Accepting adds this premise; rejecting keeps it out of the rewrite.");
        if (!assumption.evidence.empty()) excerptFor(assumption.evidence.front());
        grounded = !created.sourceExcerpt.empty();
    } else if (!a.missingDecisions.empty()) {
        created.question = a.missingDecisions.front().question;
        created.consequence = std::string("A faithful rewrite needs this detail; otherwise it would have to guess.");
        // A missing fact has no option evidence in the source by definition. Accept it
        // only when the model also supplied a source-grounded question elsewhere.
        grounded = true;
    } else if (!a.conflicts.empty()) {
        created.question = a.conflicts.front().description;
        created.consequence = std::string("These requirements conflict; the rewrite cannot preserve both without your decision.");
        grounded = true;
    }
    if (!grounded || created.question.empty()) {
        m_interviewComplete = true;
        setInterviewFeedback(QStringLiteral(
            "The model did not produce a grounded follow-up. Your answers are kept; ask for the rewrite when ready."));
        return;
    }
    m_interviewRecords.push_back(std::move(created));
    publishInterviewQuestion(m_interviewRecords.back());
    emit intentBriefChanged();
}

void HostController::recordInterviewTurn(core::intent::InterviewRecord& record, std::string disposition,
                                         std::string rawAnswer, std::string selectedChoiceId) {
    record.disposition = std::move(disposition);
    record.rawAnswer = std::move(rawAnswer);
    record.selectedChoiceId = std::move(selectedChoiceId);
    record.asked = true;
    ++m_interviewTurnsCompleted;
}

AsyncTask<core::Status> HostController::commitInterviewRecordChange(
    quint64 expectedVersion, std::vector<core::intent::InterviewRecord> nextRecords) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
    if (!m_currentAnalysis || !m_currentSnapshot || !m_revisionState) {
        co_return makeError(core::ErrorCode::PreconditionFailed, "There's nothing to answer yet.");
    }
    if (m_revisionState->isDraftDirty()) {
        co_return makeError(core::ErrorCode::StaleVersion, "The prompt changed after it was analyzed.");
    }
    const auto operationWorkspaceEpoch = m_workspaceEpoch;
    const auto operationPipelineSerial = m_pipelineSerial;
    const auto operationEditSerial = m_liveEditSerial;
    auto decision = intent::InterventionStateMachine::evaluate(*m_currentAnalysis, m_currentPolicyResult,
                                                               m_currentSnapshotEval, m_workingIntent);
    intent::InterventionStateMachine::recordPresented(decision, m_workingIntent);
    auto nextState = *m_revisionState;
    const bool clearProposal = m_currentProposal.has_value() || m_proposalStatus == ProposalUiStatus::Generating;
    const auto advanced = clearProposal ? nextState.noteProposalEdit(expectedVersion)
                                        : nextState.advanceState(expectedVersion);
    if (!advanced.ok()) co_return advanced;
    std::optional<intent::ConfirmedIntent> confirmed;
    if (decision.verdict == intent::InterventionVerdict::Quiet) {
        auto built = intent::IntentProvenanceBuilder::buildConfirmedIntent(
            *m_currentSnapshot, m_currentPolicyResult, m_workingIntent, nowMs(),
            supplementalDecisionsFrom(nextRecords));
        if (!built.ok()) co_return built.error();
        confirmed = std::move(built).value();
        if (auto status = (co_await m_repository->insertIntentVersion(*confirmed, nowMs(), expectedVersion));
            !status.ok()) {
            co_return status;
        }
    } else {
        if (auto status = (co_await m_repository->advanceSessionState(nextState, expectedVersion, nowMs()));
            !status.ok()) {
            co_return status;
        }
    }
    const bool stillCurrent = operationWorkspaceEpoch == m_workspaceEpoch &&
                              operationPipelineSerial == m_pipelineSerial;
    if (operationEditSerial == m_liveEditSerial) *m_revisionState = std::move(nextState);
    if (!stillCurrent) {
        emit stateVersionChanged();
        co_return makeError(core::ErrorCode::StaleVersion, "The prompt changed before the answer was saved.");
    }
    m_interviewRecords = std::move(nextRecords);
    m_currentDecision = std::move(decision);
    m_confirmedIntent = std::move(confirmed);
    m_currentApproval.reset();
    if (clearProposal) {
        m_currentProposal.reset();
        m_proposalWorkspaceEpoch = 0;
        m_acknowledgedConstraints.clear();
        clearActiveGroundingBinding(m_activeGroundedEvidence, m_activeGroundingPrompt,
                                    m_activeGroundingActiveFile, m_activeGroundingProposalId,
                                    m_activeGroundingSessionId, m_activeGroundingRevisionId,
                                    m_groundingValidationSerial);
        m_proposalStatus = ProposalUiStatus::None;
        m_proposalMessage.clear();
        markComprehensionReviewStale();
        emit rewriteProposalChanged();
    }
    refreshActiveItemPresentation();
    emit interventionChanged();
    emit intentChanged();
    emit intentBriefChanged();
    emit stateVersionChanged();
    emit approvalChanged();
    co_return Status::success();
}

quint64 HostController::submitInterviewResponse(const QString& text, const QString& disposition,
                                                quint64 expectedVersion) {
    if (!m_interviewActive) {
        setInterviewFeedback(QStringLiteral("Open the interview before answering."));
        return 0;
    }
    if (m_interviewBusy) {
        setInterviewFeedback(QStringLiteral("Your previous answer is still being checked."));
        return 0;
    }
    if (expectedVersion != stateVersion()) {
        setInterviewFeedback(QStringLiteral("The prompt changed before that answer was saved. Review the current state."));
        return 0;
    }
    if (m_interviewCurrentRecordId.empty()) {
        setInterviewFeedback(QStringLiteral("There is no open interview question."));
        return 0;
    }
    auto* record = findInterviewRecord(m_interviewCurrentRecordId);
    if (record == nullptr || !record->asked || record->isDecision()) {
        setInterviewFeedback(QStringLiteral("That question is no longer open."));
        return 0;
    }
    const auto disp = disposition.trimmed().toLower();
    if (disp != QStringLiteral("answer") && disp != QStringLiteral("custom") &&
        disp != QStringLiteral("unknown") && disp != QStringLiteral("skip")) {
        setInterviewFeedback(QStringLiteral("Unknown answer disposition."));
        return 0;
    }
    const auto trimmed = text.trimmed();
    if ((disp == QStringLiteral("answer") || disp == QStringLiteral("custom")) && trimmed.isEmpty()) {
        setInterviewFeedback(QStringLiteral("Type an answer or choose one of the options."));
        return 0;
    }
    if (disp == QStringLiteral("answer") && !record->choices.empty()) {
        bool matched = false;
        for (const auto& choice : record->choices) {
            if (choice.id == ss(trimmed)) { matched = true; break; }
        }
        if (!matched) {
            for (const auto& choice : record->choices) {
                if (QString::fromStdString(choice.label).compare(trimmed, Qt::CaseInsensitive) == 0) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            setInterviewFeedback(QStringLiteral("Choose one of the offered options, or use a custom answer."));
            return 0;
        }
    }
    if (disp == QStringLiteral("custom") && trimmed.size() > 4096) {
        setInterviewFeedback(QStringLiteral("Keep the answer within 4096 characters."));
        return 0;
    }
    if (disp == QStringLiteral("custom") || (disp == QStringLiteral("answer") && record->choices.empty())) {
        m_interviewBusy = true;
        emit interviewChanged();
    }
    const auto questionId = record->id;
    const auto questionSerial = m_interviewQuestionSerial;
    enqueueStorageCommand("submitInterviewResponse",
        [=, this]() { return submitInterviewResponseOperation(text, disposition, expectedVersion, questionId, questionSerial); });
    return expectedVersion + 1;
}

AsyncTask<core::Status> HostController::submitInterviewResponseOperation(QString text, QString disposition,
                                                                         quint64 expectedVersion,
                                                                         std::string questionId,
                                                                         quint64 questionSerial) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) {
        m_interviewBusy = false;
        emit interviewChanged();
        co_return status;
    }
    if (!m_interviewActive || questionSerial != m_interviewQuestionSerial ||
        m_interviewCurrentRecordId != questionId) {
        m_interviewBusy = false;
        emit interviewChanged();
        co_return makeError(core::ErrorCode::StaleVersion, "The interview question changed before that answer was saved.");
    }
    auto* record = findInterviewRecord(questionId);
    if (record == nullptr || !record->asked || record->isDecision()) {
        m_interviewBusy = false;
        emit interviewChanged();
        co_return makeError(core::ErrorCode::StaleVersion, "That question is no longer open.");
    }
    const auto disp = ss(disposition.trimmed().toLower());
    const bool isBase = !record->analysisItemId.empty();

    if (disp == "skip" || disp == "unknown") {
        const auto raw = ss(text);
        if (isBase) {
            const auto status = co_await applyTransition("interviewDisposition", expectedVersion,
                [&, this](intent::WorkingIntentState& state) {
                    return disp == "skip"
                        ? intent::InterventionStateMachine::applyInterviewSkip(*m_currentAnalysis, state, questionId)
                        : intent::InterventionStateMachine::applyInterviewUnknown(*m_currentAnalysis, state, questionId, raw);
                }, false);
            if (!status.ok()) {
                m_interviewBusy = false;
                setInterviewFeedback(plainError(status.error()));
                co_return status;
            }
            if (auto* updated = findInterviewRecord(questionId)) {
                recordInterviewTurn(*updated, disp, raw, {});
            }
            advanceInterview();
            co_return Status::success();
        }
        auto next = m_interviewRecords;
        for (auto& item : next) {
            if (item.id == questionId) {
                recordInterviewTurn(item, disp, raw, {});
                break;
            }
        }
        if (auto status = (co_await commitInterviewRecordChange(expectedVersion, std::move(next))); !status.ok()) {
            m_interviewBusy = false;
            setInterviewFeedback(plainError(status.error()));
            co_return status;
        }
        advanceInterview();
        co_return Status::success();
    }

    if (disp == "answer" && !record->choices.empty()) {
        const auto trimmed = text.trimmed();
        std::string selectedId;
        for (const auto& choice : record->choices) {
            if (choice.id == ss(trimmed)) { selectedId = choice.id; break; }
        }
        if (selectedId.empty()) {
            for (const auto& choice : record->choices) {
                if (QString::fromStdString(choice.label).compare(trimmed, Qt::CaseInsensitive) == 0) {
                    selectedId = choice.id;
                    break;
                }
            }
        }
        std::string selectedLabel;
        for (const auto& choice : record->choices) {
            if (choice.id == selectedId) { selectedLabel = choice.label; break; }
        }
        const auto raw = ss(trimmed);
        if (isBase) {
            const auto kind = intent::classifyItem(*m_currentAnalysis, questionId);
            const auto status = co_await applyTransition("interviewAnswer", expectedVersion,
                [&, this](intent::WorkingIntentState& state) {
                    if (kind == intent::ActiveItemKind::Assumption) {
                        const bool accepted = selectedId == "assume_yes";
                        return intent::InterventionStateMachine::applyInterviewAssumption(
                            *m_currentAnalysis, state, questionId, accepted);
                    }
                    return intent::InterventionStateMachine::applyInterviewChoice(
                        *m_currentAnalysis, state, questionId, selectedId);
                }, false);
            if (!status.ok()) {
                m_interviewBusy = false;
                setInterviewFeedback(plainError(status.error()));
                co_return status;
            }
            if (auto* updated = findInterviewRecord(questionId)) {
                recordInterviewTurn(*updated, "answer", raw, selectedId);
            }
            advanceInterview();
            co_return Status::success();
        }
        auto next = m_interviewRecords;
        for (auto& item : next) {
            if (item.id == questionId) {
                recordInterviewTurn(item, "answer", raw.empty() ? selectedLabel : raw, selectedId);
                break;
            }
        }
        if (auto status = (co_await commitInterviewRecordChange(expectedVersion, std::move(next))); !status.ok()) {
            m_interviewBusy = false;
            setInterviewFeedback(plainError(status.error()));
            co_return status;
        }
        advanceInterview();
        co_return Status::success();
    }

    // Custom answer (or an answer to a question without options): validate relevance with a
    // bounded, fresh model check before it can become a confirmed decision.
    const auto runtime = m_graniteAdapter->state();
    if (runtime != bridge::GraniteRuntimeState::Ready && !usesTransportOverride()) {
        m_interviewBusy = false;
        setInterviewFeedback(QStringLiteral("The local model isn't ready to check this answer. Your text is kept."));
        co_return makeError(core::ErrorCode::ProviderUnavailable, "Interview answer check unavailable");
    }
    PipelineTicket ticket = makeTicket();
    ticket.analysisGroup = std::make_shared<AnalysisGroup>(analysisGroupBudgetMs());
    ticket.analysisGroup->id = newOpaqueId("interview_check_");
    bridge::ClarificationCheckRequest request;
    request.prompt = m_revisionState->currentRevision().text();
    request.question = record->question;
    request.answer = ss(text.trimmed());
    for (const auto& choice : record->choices) request.alternatives.push_back(choice.label);
    for (const auto& [itemId, answer] : m_workingIntent.customResolutions) {
        if (request.previousAnswers.size() >= 8) break;
        const auto* previous = findInterviewRecord(itemId);
        request.previousAnswers.emplace_back(previous != nullptr ? previous->question : itemId, answer);
    }
    for (const auto& previous : m_interviewRecords) {
        if (request.previousAnswers.size() >= 8) break;
        if (!previous.isDecision()) continue;
        request.previousAnswers.emplace_back(previous.question, previous.rawAnswer);
    }
    if (auto status = co_await beginAttempt(ticket, "interview_clarification_check"); !status.ok()) {
        m_interviewBusy = false;
        setInterviewFeedback(plainError(status.error()));
        co_return status;
    }
    // Build the native transport only after beginAttempt assigns the authoritative
    // attempt identity. The helper rejects a context with an empty attempt ID.
    const auto transport = currentTransport(bridge::clarificationCheckResponseSchema(), &ticket,
                                            QStringLiteral("granite_analyze"), qs(m_revisionState->currentRevision().text()),
                                            {}, {}, {}, QStringLiteral("interview"));
    if (!transport) {
        co_await completeAttempt(ticket, storage::AttemptStatus::Failed, "Interview answer check transport unavailable");
        m_interviewBusy = false;
        setInterviewFeedback(QStringLiteral("The local model is not available to check this answer. Your text is kept."));
        co_return makeError(core::ErrorCode::ProviderUnavailable, "Interview answer check unavailable");
    }
    // Bound the visible wait to this turn's own deadline. The runtime transport has
    // its own timeout; this timer prevents a stuck injected transport from leaving
    // the interview busy indefinitely. A late result is discarded by serial.
    const qint64 remaining = ticket.analysisGroup->deadline.isForever()
                                 ? analysisGroupBudgetMs()
                                 : ticket.analysisGroup->deadline.remainingTime();
    if (remaining > 0) {
        QTimer::singleShot(static_cast<int>(std::min<qint64>(remaining, 120000)), this,
            [this, questionSerial]() {
                if (!m_interviewActive || questionSerial != m_interviewQuestionSerial || !m_interviewBusy) return;
                m_interviewBusy = false;
                ++m_interviewQuestionSerial;
                setInterviewFeedback(QStringLiteral(
                    "Checking that answer took too long. Your text is kept; retry, choose an option, or skip."));
                advanceInterview();
            });
    }
    auto adapter = m_graniteAdapter;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    ++m_inFlight;
    if (nativeWork) ++m_nativeSemanticInFlight;
    using CheckResult = core::Result<bridge::ClarificationCheckResponse>;
    auto* watcher = new QFutureWatcher<CheckResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, ticket, expectedVersion, questionId, questionSerial, text, nativeWork]() {
            auto result = watcher->result();
            watcher->deleteLater();
            --m_inFlight;
            if (nativeWork) --m_nativeSemanticInFlight;
            emit modelEngineStatusChanged();
            queueModelCompletion("finishInterviewCustomCheck",
                [this, ticket, expectedVersion, questionId, questionSerial, text, result]() mutable {
                    return finishInterviewCustomCheck(ticket, expectedVersion, questionId, questionSerial, text,
                                                      std::move(result));
                });
            finishQuitWhenRuntimesStopped();
        });
    watcher->setFuture(QtConcurrent::run([adapter, request, transport]() {
        return adapter->checkClarification(request, transport);
    }));
    co_return Status::success();
}

AsyncTask<void> HostController::finishInterviewCustomCheck(
    PipelineTicket ticket, quint64 expectedVersion, std::string questionId, quint64 questionSerial,
    QString answer, core::Result<bridge::ClarificationCheckResponse> result) {
    const bool current = m_interviewActive && questionSerial == m_interviewQuestionSerial &&
                         m_interviewCurrentRecordId == questionId && expectedVersion == stateVersion() &&
                         isTicketCurrent(ticket, false);
    if (!current) {
        co_await completeAttempt(ticket, storage::AttemptStatus::DiscardedStale,
                                 "The interview question or prompt changed during the answer check");
        m_interviewBusy = false;
        emit interviewChanged();
        co_return;
    }
    co_await completeAttempt(ticket, result.ok() ? storage::AttemptStatus::Succeeded : storage::AttemptStatus::Failed,
                             result.ok() ? "" : result.error().message);
    m_interviewBusy = false;
    if (!result.ok() || !result.value().accepted) {
        m_interviewFeedback = result.ok()
            ? qs(result.value().feedback)
            : QStringLiteral("Couldn't check that answer. Your text is kept; retry, choose an option, or skip.");
        if (m_interviewFeedback.isEmpty()) {
            m_interviewFeedback = QStringLiteral("That answer doesn't settle this question yet. Try wording it differently.");
        }
        emit interviewChanged();
        co_return;
    }
    auto* record = findInterviewRecord(questionId);
    if (record == nullptr) {
        emit interviewChanged();
        co_return;
    }
    const bool isBase = !record->analysisItemId.empty();
    const auto raw = ss(answer.trimmed());
    if (isBase) {
        const auto status = co_await applyTransition("interviewCustom", expectedVersion,
            [&, this](intent::WorkingIntentState& state) {
                return intent::InterventionStateMachine::applyInterviewCustomText(
                    *m_currentAnalysis, state, questionId, raw);
            }, false);
        if (!status.ok()) {
            setInterviewFeedback(plainError(status.error()));
            co_return;
        }
        if (auto* updated = findInterviewRecord(questionId)) {
            recordInterviewTurn(*updated, "custom", raw, {});
        }
    } else {
        auto next = m_interviewRecords;
        for (auto& item : next) {
            if (item.id == questionId) {
                recordInterviewTurn(item, "custom", raw, {});
                break;
            }
        }
        if (auto status = (co_await commitInterviewRecordChange(expectedVersion, std::move(next))); !status.ok()) {
            setInterviewFeedback(plainError(status.error()));
            co_return;
        }
    }
    advanceInterview();
    co_return;
}

quint64 HostController::changeIntentDecision(const QString& itemId, const QString& newText,
                                             quint64 expectedVersion) {
    if (itemId.trimmed().isEmpty()) {
        setInterviewFeedback(QStringLiteral("Choose a saved decision to change."));
        return 0;
    }
    if (newText.trimmed().isEmpty()) {
        setInterviewFeedback(QStringLiteral("The corrected decision cannot be empty."));
        return 0;
    }
    if (expectedVersion != stateVersion()) {
        setInterviewFeedback(QStringLiteral("The prompt changed before that correction was saved."));
        return 0;
    }
    const auto id = ss(itemId.trimmed());
    const auto* followUp = findInterviewRecord(id);
    const bool isFollowUpDecision = followUp != nullptr && followUp->analysisItemId.empty() && followUp->isDecision();
    const bool isAnalysisDecision = m_currentAnalysis &&
        intent::classifyItem(*m_currentAnalysis, id) != intent::ActiveItemKind::None &&
        m_workingIntent.isResolved(id);
    if (!isFollowUpDecision && !isAnalysisDecision) {
        setInterviewFeedback(QStringLiteral("Only a settled decision can be corrected. Answer or skip the open item instead."));
        return 0;
    }
    enqueueStorageCommand("changeIntentDecision",
        [=, this]() { return changeIntentDecisionOperation(itemId, newText, expectedVersion); });
    return expectedVersion + 1;
}

AsyncTask<core::Status> HostController::changeIntentDecisionOperation(QString itemId, QString newText,
                                                                      quint64 expectedVersion) {
    const auto id = ss(itemId.trimmed());
    const auto text = ss(newText.trimmed());
    const auto* followUp = findInterviewRecord(id);
    if (followUp != nullptr && followUp->analysisItemId.empty() && followUp->isDecision()) {
        auto next = m_interviewRecords;
        for (auto& record : next) {
            if (record.id != id) continue;
            std::string previous = record.rawAnswer;
            if (!record.selectedChoiceId.empty()) {
                for (const auto& choice : record.choices) {
                    if (choice.id == record.selectedChoiceId) { previous = choice.label; break; }
                }
            }
            if (!previous.empty() && previous != text) record.previousAnswers.push_back(previous);
            record.disposition = "custom";
            record.rawAnswer = text;
            record.selectedChoiceId.clear();
            break;
        }
        co_return (co_await commitInterviewRecordChange(expectedVersion, std::move(next)));
    }
    if (!m_currentAnalysis) {
        co_return makeError(core::ErrorCode::PreconditionFailed, "There's nothing to correct yet.");
    }
    const auto status = co_await applyTransition("changeIntentDecision", expectedVersion,
        [&, this](intent::WorkingIntentState& state) {
            return intent::InterventionStateMachine::applyDecisionChange(
                *m_currentAnalysis, state, id, text, nowMs());
        }, false);
    if (status.ok() && m_interviewActive) advanceInterview();
    co_return status;
}

// ----------------------------------------------------------------------------------------------
// Independent comprehension review
// ----------------------------------------------------------------------------------------------

namespace {

[[nodiscard]] std::string lowerAsciiText(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

[[nodiscard]] const char* reviewSeverityName(bridge::ReviewFindingSeverity severity) {
    switch (severity) {
        case bridge::ReviewFindingSeverity::Advisory: return "advisory";
        case bridge::ReviewFindingSeverity::Required: return "required";
        case bridge::ReviewFindingSeverity::Blocking: return "blocking";
    }
    return "advisory";
}

[[nodiscard]] const char* reviewKindName(bridge::ReviewFindingKind kind) {
    switch (kind) {
        case bridge::ReviewFindingKind::ComprehensionGap: return "comprehension";
        case bridge::ReviewFindingKind::MeaningDrift: return "meaning_drift";
        case bridge::ReviewFindingKind::Omission: return "omission";
        case bridge::ReviewFindingKind::UnsupportedAssumption: return "unsupported_assumption";
        case bridge::ReviewFindingKind::NegativeConstraint: return "negative_constraint";
        case bridge::ReviewFindingKind::Readiness: return "readiness";
    }
    return "comprehension";
}

// The task kind is review scoping only, not an authority. It is deterministic
// from the selected assistance mode and the user's own text.
[[nodiscard]] std::string reviewTaskKind(const QString& assistanceMode, std::string_view prompt) {
    const auto lower = lowerAsciiText(prompt);
    const bool performance = lower.find("faster") != std::string::npos ||
                             lower.find("latency") != std::string::npos ||
                             lower.find("throughput") != std::string::npos ||
                             lower.find("performance") != std::string::npos ||
                             lower.find("millisecond") != std::string::npos;
    if (assistanceMode == QStringLiteral("Create")) return "creative";
    if (performance) return "performance";
    if (assistanceMode == QStringLiteral("Tech")) return "code";
    return "general";
}

// A concern about candidate wording can be decided by the person who wrote
// the draft. Findings grounded in a source requirement must be repaired.
[[nodiscard]] bool canConfirmCandidateWording(const bridge::ComprehensionFinding& finding) {
    using Kind = bridge::ReviewFindingKind;
    return finding.sourceRef == "candidate" && !finding.sourceExcerpt.empty() &&
           finding.kind != Kind::Omission && finding.kind != Kind::NegativeConstraint;
}

[[nodiscard]] bool findingRequiresInterview(const bridge::ComprehensionFinding& finding) {
    return finding.requiresUserDecision || finding.severity == bridge::ReviewFindingSeverity::Blocking;
}

} // namespace

void HostController::setComprehensionReviewHandler(ComprehensionReviewHandler handler) {
    m_reviewHandler = std::move(handler);
}

std::optional<HostController::ComprehensionBinding> HostController::currentReviewBinding() const {
    if (!m_currentProposal || !m_revisionState || m_proposalWorkspaceEpoch != m_workspaceEpoch) {
        return std::nullopt;
    }
    ComprehensionBinding binding;
    binding.sessionId = currentSessionId();
    binding.revisionId = m_revisionState->currentRevision().revisionId().value();
    binding.proposalId = m_currentProposal->id;
    binding.proposalHash = m_currentProposal->proposalHash;
    binding.workspaceEpoch = m_workspaceEpoch;
    binding.stateVersion = m_revisionState->stateVersion();
    return binding;
}

bool HostController::reviewBindingMatchesCurrent() const {
    const auto current = currentReviewBinding();
    if (!current.has_value()) return false;
    return m_reviewBinding.sessionId == current->sessionId &&
           m_reviewBinding.revisionId == current->revisionId &&
           m_reviewBinding.proposalId == current->proposalId &&
           m_reviewBinding.proposalHash == current->proposalHash &&
           m_reviewBinding.workspaceEpoch == current->workspaceEpoch;
}

bool HostController::findingIsResolved(std::string_view findingId) const {
    const auto found = m_findingResolutions.find(std::string(findingId));
    return found != m_findingResolutions.end() &&
           (found->second == "acknowledge" || found->second == "confirm_intent");
}

bool HostController::hasUnresolvedRequiredFindings() const {
    for (const auto& finding : m_reviewResult.findings) {
        if (findingIsResolved(finding.id)) continue;
        if (finding.requiresUserDecision) return true;
        if (finding.severity == bridge::ReviewFindingSeverity::Blocking) return true;
        if (finding.id.empty()) return true;
        if (finding.severity == bridge::ReviewFindingSeverity::Required && !findingIsResolved(finding.id)) {
            return true;
        }
    }
    return false;
}

bool HostController::reviewIsFreshAndComplete() const {
    if (m_reviewState != ReviewState::Ready || !reviewBindingMatchesCurrent() ||
        m_reviewResult.status != bridge::ComprehensionReviewStatus::Completed ||
        m_reviewResult.interpretedOutcome.empty()) return false;
    if (m_reviewResult.readyForTask) return true;
    // A not-ready result can only be cleared by an explicit decision about
    // every reported concern, and only for findings about candidate wording.
    return !m_reviewResult.findings.empty() && std::all_of(
        m_reviewResult.findings.begin(), m_reviewResult.findings.end(),
        [this](const auto& finding) {
            const auto resolution = m_findingResolutions.find(finding.id);
            return canConfirmCandidateWording(finding) && resolution != m_findingResolutions.end() &&
                   resolution->second == "confirm_intent";
        });
}

bool HostController::canApplyReviewedOutput() const {
    return hasProposal() && reviewIsFreshAndComplete() && !hasUnresolvedRequiredFindings();
}

QString HostController::comprehensionReviewState() const {
    if (m_reviewState == ReviewState::Stale && !m_reviewReturnedFindingId.empty())
        return QStringLiteral("waiting_for_answer");
    if (m_reviewState == ReviewState::NotRun) return QStringLiteral("not_run");
    if (m_reviewState == ReviewState::Running) return QStringLiteral("running");
    if (!reviewBindingMatchesCurrent()) return QStringLiteral("stale");
    switch (m_reviewState) {
        case ReviewState::Ready: return QStringLiteral("ready");
        case ReviewState::Failed: return QStringLiteral("failed");
        case ReviewState::Stale: return QStringLiteral("stale");
        case ReviewState::NotRun: return QStringLiteral("not_run");
        case ReviewState::Running: return QStringLiteral("running");
    }
    return QStringLiteral("not_run");
}

QString HostController::comprehensionInterpretation() const {
    // Display-only: the reviewer's inference is never authoritative. A stale or
    // failed review belongs to another proposal/revision; do not show its text as
    // if it described the current rewrite.
    if (m_reviewState != ReviewState::Ready || !reviewBindingMatchesCurrent()) return QString();
    return boundedText(m_reviewResult.interpretedOutcome, 1200);
}

QVariantList HostController::comprehensionFindings() const {
    QVariantList list;
    if (m_reviewState != ReviewState::Ready || !reviewBindingMatchesCurrent()) return list;
    for (const auto& finding : m_reviewResult.findings) {
        // Acknowledged findings are resolved in this review. Keep them in the host
        // result for provenance/history, but do not keep asking the user to resolve
        // an item that no longer blocks applicability.
        if (findingIsResolved(finding.id)) continue;
        QVariantMap map;
        map[QStringLiteral("id")] = qs(finding.id);
        map[QStringLiteral("severity")] = QString::fromUtf8(reviewSeverityName(finding.severity));
        map[QStringLiteral("kind")] = QString::fromUtf8(reviewKindName(finding.kind));
        map[QStringLiteral("summary")] = boundedText(finding.summary, 600);
        map[QStringLiteral("sourceRef")] = boundedText(finding.sourceRef, 320);
        map[QStringLiteral("sourceExcerpt")] = boundedText(finding.sourceExcerpt, 320);
        map[QStringLiteral("requiresUserDecision")] = finding.requiresUserDecision;
        map[QStringLiteral("canConfirmIntent")] = canConfirmCandidateWording(finding);
        map[QStringLiteral("proposedRepair")] = boundedText(finding.proposedRepair, 600);
        list.append(map);
    }
    return list;
}

void HostController::markComprehensionReviewStale() {
    m_pendingNativeReview.reset();
    if (m_reviewState == ReviewState::NotRun) return;
    ++m_reviewSerial;
    if (m_reviewCancellation) {
        m_reviewCancellation->store(true);
        m_reviewCancellation.reset();
    }
    m_reviewState = ReviewState::Stale;
    m_findingResolutions.clear();
    m_reviewReturnedFindingId.clear();
    emit comprehensionReviewChanged();
}

AsyncTask<core::Status> HostController::requestComprehensionReviewOperation(quint64 expectedVersion) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
    if (m_proposalStatus != ProposalUiStatus::ReadyForReview || !m_currentProposal) {
        co_return makeError(core::ErrorCode::PreconditionFailed,
                            "There is no suggested rewrite to review.");
    }
    if (!m_revisionState || !m_confirmedIntent || !m_currentSnapshot || m_revisionState->isDraftDirty()) {
        co_return makeError(core::ErrorCode::PreconditionFailed,
                            "Review the current draft before requesting a review.");
    }
    if (m_reviewState == ReviewState::Running) {
        co_return makeError(core::ErrorCode::PreconditionFailed, "A review is already running.");
    }
    if (!m_reviewHandler && nativeGraniteSelected() && !usesTransportOverride() && !m_native->isReady()) {
        if (m_isDownloadingModel && m_downloadingNativeTag == m_activeModel)
            co_return makeError(core::ErrorCode::ProviderUnavailable,
                                "The model download is still running. Retry the reread when it finishes.");
        const auto binding = currentReviewBinding();
        if (!binding) co_return makeError(core::ErrorCode::PreconditionFailed,
                                          "The rewrite is no longer bound to the current prompt.");
        m_pendingNativeReview = PendingNativeReview{*binding, ++m_reviewSerial, m_configurationGeneration};
        m_reviewBinding = *binding;
        m_reviewState = ReviewState::Running;
        m_reviewResult = bridge::ComprehensionReviewResult{};
        m_findingResolutions.clear();
        emit comprehensionReviewChanged();
        emit rewriteProposalChanged();
        prepareNativeModel();
        co_return Status::success();
    }
    // Rereading is read-only. Binding the job to the proposal hash and
    // review serial keeps it stale-safe without invalidating clicks on the HUD.
    const auto binding = currentReviewBinding();
    if (!binding.has_value()) {
        co_return makeError(core::ErrorCode::PreconditionFailed,
                            "The rewrite is no longer bound to the current prompt.");
    }
    ++m_reviewSerial;
    m_reviewState = ReviewState::Running;
    m_reviewResult = bridge::ComprehensionReviewResult{};
    m_findingResolutions.clear();
    m_reviewReturnedFindingId.clear();
    m_reviewBinding = *binding;
    m_reviewBinding.stateVersion = stateVersion();
    emit comprehensionReviewChanged();
    emit rewriteProposalChanged();

    bridge::ComprehensionReviewRequest request;
    request.candidatePrompt = m_currentProposal->rewrittenText;
    request.originalPrompt = m_currentProposal->originalText;
    request.constraints = extractSessionConstraints(request.originalPrompt, *m_confirmedIntent);
    for (const auto& item : m_confirmedIntent->items) {
        if (request.confirmedDecisions.size() >= 12) break;
        const std::string wording = item.kind == intent::IntentResolutionKind::CustomText ? item.customText
                                                                                          : item.chosenValue;
        if (!wording.empty()) request.confirmedDecisions.push_back(wording.substr(0, 512));
    }
    const auto brief = buildIntentBrief();
    for (const auto& item : brief.items) {
        if (request.sourceExcerpts.size() >= 8) break;
        if (item.sourceExcerpt.empty()) continue;
        if (item.kind != core::intent::BriefItemKind::SourceRequirement &&
            item.kind != core::intent::BriefItemKind::Prohibition &&
            item.kind != core::intent::BriefItemKind::Preference &&
            item.kind != core::intent::BriefItemKind::Evidence) {
            continue;
        }
        const auto excerpt = item.sourceExcerpt.substr(0, 512);
        if (std::find(request.sourceExcerpts.begin(), request.sourceExcerpts.end(), excerpt) ==
            request.sourceExcerpts.end()) {
            request.sourceExcerpts.push_back(excerpt);
        }
    }
    request.taskKind = reviewTaskKind(assistanceMode(), request.originalPrompt);
    request.sessionId = m_reviewBinding.sessionId;
    request.revisionId = m_reviewBinding.revisionId;

    // Production reviewer. The injected seam wins when a test supplies one; the
    // production path builds exactly one fresh-context native request bound to a
    // dedicated analysis group and cancellation token. The reviewer sees only the
    // candidate/original/decisions/constraints/evidence assembled above. A missing
    // or not-ready runtime fails closed and never authorizes apply.
    ComprehensionReviewHandler handler = m_reviewHandler;
    std::shared_ptr<std::atomic_bool> reviewCancellation;
    if (!handler) {
        PipelineTicket reviewTicket = makeTicket();
        reviewTicket.analysisGroup = std::make_shared<AnalysisGroup>(45000);
        reviewTicket.analysisGroup->id = newOpaqueId("review_grp_");
        reviewTicket.attemptId = newOpaqueId("att_review_");
        reviewTicket.reviewTrigger = analysis::ReviewTrigger::ExplicitReview;
        reviewCancellation = std::make_shared<std::atomic_bool>(false);
        const auto transport = currentTransport(bridge::comprehensionReviewResponseSchema(), &reviewTicket,
            QStringLiteral("granite_analyze"), qs(request.originalPrompt),
            qs(m_confirmedIntent ? m_confirmedIntent->id : std::string()), m_rewriteProfile,
            reviewCancellation, QStringLiteral("review"));
        if (!transport) {
            m_reviewState = ReviewState::Failed;
            m_reviewResult.status = bridge::ComprehensionReviewStatus::Unavailable;
            m_reviewResult.readyForTask = false;
            m_reviewResult.rationale =
                "The independent reviewer could not start: the local model is not ready.";
            emit comprehensionReviewChanged();
            emit rewriteProposalChanged();
            co_return Status::success();
        }
        m_reviewCancellation = reviewCancellation;
        handler = [transport](const bridge::ComprehensionReviewRequest& req) {
            bridge::GraniteGoldfishAdapter reviewer;
            return reviewer.reviewComprehension(req, transport);
        };
    }
    const quint64 serial = m_reviewSerial;
    const auto bindingForWorker = m_reviewBinding;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    ++m_inFlight;
    if (nativeWork) ++m_nativeSemanticInFlight;
    auto* watcher = new QFutureWatcher<bridge::ComprehensionReviewResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, serial, bindingForWorker, nativeWork]() {
            const auto result = watcher->result();
            watcher->deleteLater();
            --m_inFlight;
            if (nativeWork) --m_nativeSemanticInFlight;
            finishQuitWhenRuntimesStopped();
            if (serial != m_reviewSerial) return;
            m_reviewCancellation.reset();
            const auto current = currentReviewBinding();
            const bool bound = current.has_value() &&
                               bindingForWorker.sessionId == current->sessionId &&
                               bindingForWorker.revisionId == current->revisionId &&
                               bindingForWorker.proposalId == current->proposalId &&
                               bindingForWorker.proposalHash == current->proposalHash &&
                               bindingForWorker.workspaceEpoch == current->workspaceEpoch;
            if (!bound) {
                m_reviewState = ReviewState::Stale;
                m_findingResolutions.clear();
                emit comprehensionReviewChanged();
                emit rewriteProposalChanged();
                return;
            }
            const bool wellFormed = result.status == bridge::ComprehensionReviewStatus::Completed &&
                                    !result.interpretedOutcome.empty();
            const bool duplicateIds = [&]() {
                std::set<std::string> ids;
                for (const auto& finding : result.findings) {
                    if (finding.id.empty()) return true;
                    if (!ids.insert(finding.id).second) return true;
                }
                return false;
            }();
            if (!wellFormed || duplicateIds) {
                m_reviewState = ReviewState::Failed;
                m_reviewResult = bridge::ComprehensionReviewResult{};
                m_reviewResult.status = bridge::ComprehensionReviewStatus::Malformed;
                m_reviewResult.readyForTask = false;
                m_findingResolutions.clear();
                emit comprehensionReviewChanged();
                emit rewriteProposalChanged();
                return;
            }
            m_reviewResult = result;
            m_reviewState = ReviewState::Ready;
            m_findingResolutions.clear();
            emit comprehensionReviewChanged();
            emit rewriteProposalChanged();
        });
    const auto deadline = QDeadlineTimer(45000, Qt::PreciseTimer);
    watcher->setFuture(QtConcurrent::run([handler, request, deadline]() -> bridge::ComprehensionReviewResult {
        bridge::ComprehensionReviewResult result;
        result.status = bridge::ComprehensionReviewStatus::Unavailable;
        result.readyForTask = false;
        try {
            result = handler(request);
        } catch (const std::exception& ex) {
            result = bridge::ComprehensionReviewResult{};
            result.status = bridge::ComprehensionReviewStatus::Unavailable;
            result.readyForTask = false;
            result.rationale = std::string("Reviewer crashed: ") + ex.what();
        } catch (...) {
            result = bridge::ComprehensionReviewResult{};
            result.status = bridge::ComprehensionReviewStatus::Unavailable;
            result.readyForTask = false;
        }
        if (deadline.hasExpired()) {
            result.status = bridge::ComprehensionReviewStatus::Unavailable;
            result.readyForTask = false;
            result.findings.clear();
            result.rationale = "The independent review exceeded its deadline.";
        }
        return result;
    }));
    co_return Status::success();
}

AsyncTask<core::Status> HostController::resolveComprehensionFindingOperation(QString findingId,
                                                                             QString resolution,
                                                                             quint64 expectedVersion) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
    if (m_reviewState != ReviewState::Ready || !reviewBindingMatchesCurrent()) {
        co_return makeError(core::ErrorCode::PreconditionFailed,
                            "There is no current review finding to resolve.");
    }
    const auto id = ss(findingId.trimmed());
    const auto res = ss(resolution.trimmed().toLower());
    if (res != "acknowledge" && res != "confirm_intent" && res != "return_to_interview" && res != "keep_original") {
        co_return makeError(core::ErrorCode::InvalidInput, "Unknown review resolution.");
    }
    const bridge::ComprehensionFinding* finding = nullptr;
    for (const auto& candidate : m_reviewResult.findings) {
        if (candidate.id == id) { finding = &candidate; break; }
    }
    if (finding == nullptr) {
        co_return makeError(core::ErrorCode::InvalidInput, "That review finding does not belong to the current review.");
    }
    if (res == "confirm_intent") {
        if (!canConfirmCandidateWording(*finding))
            co_return makeError(core::ErrorCode::ValidationFailed, "This finding requires a correction before applying.");
        if (auto status = (co_await advanceCommandVersion(expectedVersion)); !status.ok()) co_return status;
        m_findingResolutions[id] = res;
        emit comprehensionReviewChanged();
        emit rewriteProposalChanged();
        co_return Status::success();
    }
    if (res == "acknowledge") {
        if (findingRequiresInterview(*finding)) {
            co_return makeError(core::ErrorCode::ValidationFailed,
                                "This finding needs your decision. Return it to the interview or keep the original prompt.");
        }
        if (auto status = (co_await advanceCommandVersion(expectedVersion)); !status.ok()) co_return status;
        m_findingResolutions[id] = "acknowledge";
        emit comprehensionReviewChanged();
        emit rewriteProposalChanged();
        co_return Status::success();
    }
    if (res == "return_to_interview") {
        if (auto status = (co_await advanceCommandVersion(expectedVersion)); !status.ok()) co_return status;
        const auto captured = *finding;
        openReviewFindingInInterview(captured);
        m_reviewState = ReviewState::Stale;
        m_findingResolutions.clear();
        m_reviewReturnedFindingId = captured.id;
        emit comprehensionReviewChanged();
        emit rewriteProposalChanged();
        co_return Status::success();
    }
    // keep_original: discard the rewrite and retain the user's original prompt.
    if (auto status = (co_await invalidateProposalArtifacts()); !status.ok()) co_return status;
    m_reviewState = ReviewState::Stale;
    m_findingResolutions.clear();
    m_reviewReturnedFindingId.clear();
    m_interviewFeedback = QStringLiteral("Kept your original prompt. No rewrite was applied.");
    emit comprehensionReviewChanged();
    emit rewriteProposalChanged();
    emit interviewChanged();
    co_return Status::success();
}

void HostController::openReviewFindingInInterview(const bridge::ComprehensionFinding& finding) {
    core::intent::InterviewRecord record;
    record.id = "iv_review_" + std::to_string(m_interviewRecords.size() + 1);
    record.origin = "review_finding";
    record.asked = true;
    record.question = "What should the rewrite say here?";
    if (!finding.sourceExcerpt.empty()) {
        record.sourceExcerpt = boundedText(finding.sourceExcerpt, 320).toStdString();
        record.sourceRef = finding.sourceRef.empty() ? finding.id : finding.sourceRef;
    }
    m_interviewRecords.push_back(std::move(record));
    m_interviewActive = true;
    m_interviewComplete = false;
    m_interviewFeedback.clear();
    m_interviewWordContext.reset();
    publishInterviewQuestion(m_interviewRecords.back());
    emit intentBriefChanged();
    emit interviewChanged();
    if (!isCardVisible()) showCardInternal(false);
}

AsyncTask<core::Status> HostController::discardProposalOperation(quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (!m_currentProposal && m_proposalStatus == ProposalUiStatus::None) {
            co_return Status::success();
        }
        co_return (co_await invalidateProposalArtifacts());

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
    return m_currentProposal.has_value() && m_proposalStatus == ProposalUiStatus::ReadyForReview &&
           m_proposalWorkspaceEpoch == m_workspaceEpoch;
}

QString HostController::proposalText() const {
    return hasProposal() ? qs(m_currentProposal->rewrittenText) : QString();
}

QString HostController::lastDiffText() const {
    return hasProposal() ? qs(m_currentProposal->diffText) : QString();
}

QVariantList HostController::creativeAlternatives() const {
    QVariantList list;
    list.reserve(static_cast<qsizetype>(m_creativeAlternatives.size()));
    for (size_t i = 0; i < m_creativeAlternatives.size(); ++i) {
        const auto& alt = m_creativeAlternatives[i];
        QVariantMap map;
        map.insert(QStringLiteral("id"), qs(alt.id));
        map.insert(QStringLiteral("title"), qs(alt.title));
        map.insert(QStringLiteral("styleDescriptor"), qs(alt.styleDescriptor));
        map.insert(QStringLiteral("rewrittenText"), qs(alt.rewrittenText));
        map.insert(QStringLiteral("diffText"), qs(alt.diffText));
        map.insert(QStringLiteral("auditOutcome"), QString::fromUtf8(toString(alt.auditOutcome)));

        core::AuditExecutionStatus execStatus = core::AuditExecutionStatus::Completed;
        if (static_cast<int>(i) == m_selectedAlternativeIndex && m_lastAuditResult.has_value()) {
            execStatus = m_lastAuditResult->executionStatus;
        } else if (i < m_creativeAlternativeAuditExecutionStatuses.size()) {
            execStatus = m_creativeAlternativeAuditExecutionStatuses[i];
        }
        map.insert(QStringLiteral("auditExecutionStatus"), QString::fromUtf8(core::toString(execStatus)));
        list.append(map);
    }
    return list;
}

void HostController::selectAlternative(int index) {
    if (m_candidateWriteInFlight || !m_revisionState || !m_currentProposal ||
        index < 0 || index >= static_cast<int>(m_creativeAlternatives.size())) {
        return;
    }
    if (m_selectedAlternativeIndex == index) {
        return;
    }
    m_selectedAlternativeIndex = index;
    const auto& alt = m_creativeAlternatives[static_cast<size_t>(index)];
    auto proposal = *m_currentProposal;
    proposal.rewrittenText = alt.rewrittenText;
    proposal.diffText = alt.diffText;

    // F06: Recompute text-derived constraints for the selected text
    if (m_confirmedIntent) {
        if (const auto profile = rewrite::ProfileRegistry::getProfile(ss(m_rewriteProfile)); profile.has_value()) {
            proposal.constraints = rewrite::RewriteValidator::deriveConstraints(
                *m_confirmedIntent, *profile, alt.rewrittenText);
        }
    }

    // Assign a unique proposal ID on each candidate selection
    proposal.id = newOpaqueId("prop_");
    proposal.auditResult.reset();
    core::rewrite::ProposalHasher::seal(proposal);
    m_currentProposal = proposal;

    // Clear stale audit presentation immediately so stale audit results
    // are not displayed while background audit is in progress for the new candidate
    m_lastAuditResult.reset();

    // Hide prior approval while the new selection is pending. The durable
    // version and approval change together when the audited proposal is saved.
    m_acknowledgedConstraints.clear();
    m_currentApproval.reset();
    if (!m_activeGroundingProposalId.empty()) {
        m_activeGroundingProposalId = proposal.id;
    }

    // Prepare ticket for asynchronous selection audit
    PipelineTicket ticket = makeTicket();
    ticket.attemptId = newOpaqueId("att_sel_");
    if (m_confirmedIntent) {
        ticket.intentId = m_confirmedIntent->id;
    }
    ticket.model = {proposal.modelId, proposal.modelDigest, proposal.quantization, proposal.transport};

    bridge::GraniteTransport auditTransport = nullptr;
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (usesTransportOverride()) {
        auditTransport = m_transportOverride;
    }
#endif
    if (!auditTransport) {
        auditTransport = currentTransport(bridge::goldfishAuditResponseSchema(), &ticket, QStringLiteral("granite_rewrite"),
            qs(proposal.originalText), qs(ticket.intentId), m_rewriteProfile);
    }

    bridge::GoldfishAuditRequest auditReq;
    auditReq.originalPrompt = proposal.originalText;
    auditReq.candidateRewrite = proposal.rewrittenText;
    if (m_confirmedIntent) {
        auditReq.constraints = extractSessionConstraints(proposal.originalText, *m_confirmedIntent);
    }
    auditReq.sessionId = currentSessionId();

    setProposalStatus(ProposalUiStatus::Generating,
                      QStringLiteral("Auditing selected candidate rewrite..."));
    emit selectedAlternativeChanged();
    emit approvalChanged();
    if (!isAlternativeSelectionCurrent(ticket, proposal.id)) return;

    auto* watcher = new QFutureWatcher<core::ValidationResult>(this);
    ++m_inFlight;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    if (nativeWork) ++m_nativeSemanticInFlight;

    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, ticket, index, proposal = std::move(proposal), nativeWork]() mutable {
            --m_inFlight;
            if (nativeWork) --m_nativeSemanticInFlight;
            core::ValidationResult result = watcher->result();
            watcher->deleteLater();
            onAlternativeAuditFinished(ticket, index, std::move(proposal), std::move(result));
            finishQuitWhenRuntimesStopped();
        });

    watcher->setFuture(QtConcurrent::run(
        [auditReq, auditTransport]() -> core::ValidationResult {
            bridge::GraniteGoldfishAdapter goldfishAdapter;
            return goldfishAdapter.audit(auditReq, auditTransport);
        }));
}

bool HostController::isAlternativeSelectionCurrent(const PipelineTicket& ticket, const std::string& proposalId) const {
    return isTicketCurrent(ticket, true) && m_currentProposal && m_currentProposal->id == proposalId &&
           m_proposalWorkspaceEpoch == ticket.workspaceEpoch;
}

void HostController::onAlternativeAuditFinished(
    PipelineTicket ticket, int index, rewrite::RewriteProposal proposal, core::ValidationResult auditVerdict) {
    if (!isAlternativeSelectionCurrent(ticket, proposal.id)) return;
    proposal.auditResult = std::move(auditVerdict);
    const auto cmdId = enqueueStorageCommand("persistSelectedAlternative",
        [this, ticket = std::move(ticket), index, proposal = std::move(proposal)]() mutable {
            return persistSelectedAlternativeOperation(std::move(ticket), index, std::move(proposal));
        }, true);
    if (cmdId == 0) {
        setProposalStatus(ProposalUiStatus::Failed, QStringLiteral("Storage command queue rejected save."));
        emit rewriteProposalChanged();
        emit approvalChanged();
    }
}

AsyncTask<core::Status> HostController::persistSelectedAlternativeOperation(
    PipelineTicket ticket, int index, rewrite::RewriteProposal proposal) {
    if (!isAlternativeSelectionCurrent(ticket, proposal.id)) {
        co_return makeError(ErrorCode::StaleVersion, "Candidate selection is stale");
    }

    if (!m_storage || !m_repository) {
        setProposalStatus(ProposalUiStatus::Failed, QStringLiteral("Storage is unavailable."));
        emit rewriteProposalChanged();
        emit approvalChanged();
        co_return storageUnavailable();
    }

    Status status = Status::success();
    {
        // Draft edits can still update the editor while SQLite is writing.
        // Hold their flush until the outcome determines their expected version.
        QScopedValueRollback writing(m_candidateWriteInFlight, true);
        status = co_await m_storage->call(&storage::PromptRepository::insertProposal,
                                          proposal, nowMs(), std::optional{ticket.stateVersion});
    }

    const bool selectionCurrent = isAlternativeSelectionCurrent(ticket, proposal.id);
    const bool sameRevision = m_revisionState && currentSessionId() == ticket.sessionId &&
        m_revisionState->currentRevision().revisionId().value() == ticket.revisionId;
    if (status.ok() && sameRevision) {
        // A successful insert advanced SQLite by one. Preserve every edit
        // made during the write, including its dirty state and generation.
        (void)m_revisionState->advanceState(stateVersion());
        if (m_pendingDraft) {
            ++m_pendingDraft->expectedVersion;
            m_pendingDraft->next = *m_revisionState;
        }
    }
    if (m_pendingDraft && !m_storageFault) {
        QTimer::singleShot(0, this, [this]() { flushDraftEdits(); });
    }

    if (!selectionCurrent) {
        if (status.ok() && sameRevision) emit stateVersionChanged();
        co_return makeError(ErrorCode::StaleVersion, "Candidate selection became stale during persistence");
    }

    if (!status.ok()) {
        setProposalStatus(ProposalUiStatus::Failed, plainError(status.error()));
        emit approvalChanged();
        co_return status;
    }

    m_lastAuditResult = proposal.auditResult;
    m_currentProposal = std::move(proposal);
    if (index >= 0 && static_cast<size_t>(index) < m_creativeAlternatives.size()) {
        m_creativeAlternatives[static_cast<size_t>(index)].auditOutcome = m_lastAuditResult->outcome;
    }
    if (index >= 0 && static_cast<size_t>(index) < m_creativeAlternativeAuditExecutionStatuses.size()) {
        m_creativeAlternativeAuditExecutionStatuses[static_cast<size_t>(index)] = m_lastAuditResult->executionStatus;
    }

    const auto currentValidation = validateCurrentProposal();
    const auto pending = currentValidation.pendingAcknowledgments.size();
    setProposalStatus(ProposalUiStatus::ReadyForReview,
                      m_currentProposal->rewrittenText == m_currentProposal->originalText
                          ? QStringLiteral("No wording changes suggested. This is your original prompt.")
                      : pending == 0 ? QStringLiteral("Review the suggested wording, then choose Insert or Copy.")
                                   : QStringLiteral("Check the %1 highlighted item(s), then approve the wording.").arg(pending));

    emit selectedAlternativeChanged();
    emit rewriteProposalChanged();
    emit stateVersionChanged();
    emit approvalChanged();

    co_return Status::success();
}

QVariantList HostController::proposalConstraints() const {
    QVariantList list;
    if (!hasProposal()) {
        return list;
    }
    std::set<std::string> handledAuditIds;
    for (const auto& c : m_currentProposal->constraints) {
        QVariantMap map;
        map[QStringLiteral("id")] = qs(c.id);
        map[QStringLiteral("description")] = qs(c.description);
        map[QStringLiteral("status")] = QString::fromUtf8(rewrite::toString(c.status));
        map[QStringLiteral("evidence")] = qs(c.evidence);
        bool needsReview = c.status == rewrite::ConstraintStatus::SemanticReviewRequired;
        bool acknowledged = m_acknowledgedConstraints.count(c.id) > 0;

        if (m_lastAuditResult.has_value() && m_lastAuditResult->outcome == core::ValidationOutcome::Unresolved) {
            for (const auto& u : m_lastAuditResult->unresolved) {
                if (isMatchingConstraintId(c.id, u.constraintId)) {
                    needsReview = true;
                    if (!u.explanation.empty()) {
                        map[QStringLiteral("auditExplanation")] = qs(u.explanation);
                    }
                    map[QStringLiteral("status")] = QStringLiteral("SemanticReviewRequired");
                    acknowledged = m_acknowledgedConstraints.count(c.id) > 0 || m_acknowledgedConstraints.count(u.constraintId) > 0;
                    handledAuditIds.insert(u.constraintId);
                    break;
                }
            }
        }

        map[QStringLiteral("needsReview")] = needsReview;
        map[QStringLiteral("acknowledged")] = acknowledged;
        list.append(map);
    }

    if (m_lastAuditResult.has_value() && m_lastAuditResult->outcome == core::ValidationOutcome::Unresolved) {
        for (const auto& u : m_lastAuditResult->unresolved) {
            if (handledAuditIds.count(u.constraintId) == 0) {
                QVariantMap map;
                map[QStringLiteral("id")] = qs(u.constraintId);
                map[QStringLiteral("description")] = QStringLiteral("Check that the rewrite keeps your original requirements.");
                map[QStringLiteral("status")] = QStringLiteral("SemanticReviewRequired");
                map[QStringLiteral("evidence")] = QString(); // An audit ID is not a user answer.
                map[QStringLiteral("auditExplanation")] = qs(u.explanation);
                map[QStringLiteral("needsReview")] = true;
                map[QStringLiteral("acknowledged")] = m_acknowledgedConstraints.count(u.constraintId) > 0;
                list.append(map);
            }
        }
    }
    if (m_lastAuditResult.has_value() && m_lastAuditResult->executionStatus == core::AuditExecutionStatus::DeterministicOnly) {
        const std::string fallbackId = "deterministic_fallback";
        QVariantMap map;
        map[QStringLiteral("id")] = qs(fallbackId);
        map[QStringLiteral("description")] = QStringLiteral("The model check is unavailable. Review your requirements before applying.");
        map[QStringLiteral("status")] = QStringLiteral("SemanticReviewRequired");
        map[QStringLiteral("evidence")] = QString();
        map[QStringLiteral("auditExplanation")] = QStringLiteral("DeterministicOnly: model transport offline.");
        map[QStringLiteral("needsReview")] = true;
        map[QStringLiteral("acknowledged")] = m_acknowledgedConstraints.count(fallbackId) > 0;
        list.append(map);
    }
    return list;
}

int HostController::pendingReviewCount() const {
    return hasProposal() ? static_cast<int>(validateCurrentProposal().pendingAcknowledgments.size()) : 0;
}

bool HostController::canApprove() const {
    return isStorageAvailable() && hasProposal() && validateCurrentProposal().isValid &&
           canApplyReviewedOutput();
}

QVariantList HostController::groundedEvidence() const {
    return m_groundedEvidenceViews;
}

void HostController::applyGroundingContext(const GroundedPromptContext& context) {
    // The context must belong to the workspace/scope epoch that is current on
    // the UI thread before any view is published. There is no epoch-0 bypass:
    // every production and test helper context carries the captured epoch.
    if (context.workspaceEpoch != m_workspaceEpoch) return;
    QElapsedTimer applyTimer;
    applyTimer.start();
    m_groundedEvidenceViews.clear();
    constexpr std::size_t kMaximumViews = 16;
    for (std::size_t index = 0; index < context.evidence.size() && index < kMaximumViews; ++index) {
        const auto& evidence = context.evidence[index];
        QVariantMap item;
        item[QStringLiteral("target")] = qs(evidence.target);
        item[QStringLiteral("qualifiedTarget")] = qs(evidence.qualifiedTarget);
        item[QStringLiteral("signature")] = qs(evidence.signature);
        item[QStringLiteral("filePath")] = qs(evidence.filePath);
        item[QStringLiteral("line")] = static_cast<int>(evidence.line);
        item[QStringLiteral("status")] = qs(evidence.status);
        item[QStringLiteral("matchReason")] = qs(evidence.matchReason);
        item[QStringLiteral("complete")] = evidence.complete;
        item[QStringLiteral("hashValidated")] = evidence.hashValidated;
        QStringList candidates;
        for (const auto& candidate : evidence.candidates) candidates.append(qs(candidate));
        item[QStringLiteral("candidates")] = candidates;
        m_groundedEvidenceViews.append(item);
    }
    m_groundingStatus = !context.available ? QStringLiteral("UNAVAILABLE")
        : !context.validated ? QStringLiteral("STALE")
        : !context.complete ? QStringLiteral("INCOMPLETE") : QStringLiteral("CURRENT");
    if (context.validated) m_activeGroundingGeneration = context.snapshotGeneration;
    if (context.validated) {
        // A validated snapshot is the only proof that the withheld paths have
        // recovered; it clears the bounded per-path retry state.
        resetStalePathRecovery();
    } else {
        scheduleStalePathRecovery(context.stalePaths);
    }
    m_lastGroundingApplyMs = static_cast<double>(applyTimer.nsecsElapsed()) / 1e6;
    emit groundingChanged();
}

void HostController::setGroundingUnavailable(const QString& status) {
    m_groundedEvidenceViews.clear();
    m_groundingStatus = status;
    emit groundingChanged();
}

void HostController::scheduleGroundingRevalidation() {
    if (!m_codebaseContextConsented || m_projectScanPath.isEmpty() || !m_currentProposal) return;
    if (m_proposalWorkspaceEpoch != m_workspaceEpoch) return;
    const auto workspace = m_projectScanPath;
    const auto workspaceEpoch = m_workspaceEpoch;
    const auto proposalId = m_currentProposal->id;
    const auto proposalAstHash = m_currentProposal->astSnapshotHash;
    // A proposal accepted in this session has an exact evidence baseline. A
    // restored proposal has only its persisted AST hash; its current evidence
    // becomes the baseline once it has been re-resolved successfully.
    const bool bindingKnown = !proposalId.empty() && m_activeGroundingProposalId == proposalId;
    const auto sessionId = bindingKnown && !m_activeGroundingSessionId.empty()
        ? m_activeGroundingSessionId : currentSessionId();
    const auto revisionId = bindingKnown && !m_activeGroundingRevisionId.empty()
        ? m_activeGroundingRevisionId
        : (m_revisionState ? m_revisionState->currentRevision().revisionId().value() : std::string{});
    if (revisionId.empty()) return;
    // The snapshot generation is part of the binding. A readable result from a
    // different generation is superseded; an unreadable result is a current
    // validation failure even though it has no generation to compare.
    const auto snapshotGeneration = m_codebaseSnapshotGeneration;

    auto evidence = m_activeGroundedEvidence;
    if (!bindingKnown) evidence.clear();

    std::string prompt = bindingKnown ? m_activeGroundingPrompt : std::string{};
    if (prompt.empty() && m_currentProposal) prompt = m_currentProposal->originalText;
    if (prompt.empty() && m_revisionState) prompt = m_revisionState->currentRevision().text();
    auto inputs = groundingInputs(std::move(prompt));
    if (bindingKnown) inputs.activeFile = m_activeGroundingActiveFile;
    if (inputs.indexPath.empty()) return;

    const auto serial = ++m_groundingValidationSerial;
    auto* watcher = new QFutureWatcher<GroundedPromptContext>(this);
    ++m_inFlight;
    connect(watcher, &QFutureWatcher<GroundedPromptContext>::finished, this,
        [this, watcher, serial, workspace, workspaceEpoch, proposalId, sessionId, revisionId,
         snapshotGeneration, evidence, proposalAstHash, bindingKnown]() {
            --m_inFlight;
            const auto current = watcher->result();
            watcher->deleteLater();
            const auto stillCurrent = [&]() {
                if (serial != m_groundingValidationSerial) return false;
                if (workspace != m_projectScanPath) return false;
                if (workspaceEpoch != m_workspaceEpoch) return false;
                if (m_hostState == HostState::Quitting) return false;
                if (!m_currentProposal || m_currentProposal->id != proposalId) return false;
                if (m_proposalWorkspaceEpoch != workspaceEpoch) return false;
                if (currentSessionId() != sessionId) return false;
                if (!m_revisionState ||
                    m_revisionState->currentRevision().revisionId().value() != revisionId)
                    return false;
                return true;
            };
            if (!stillCurrent()) {
                finishQuitWhenRuntimesStopped();
                return;
            }

            // A readable snapshot from another generation belongs to a newer
            // refresh. Discard it without touching the bound proposal. A
            // missing/unreadable/zero-generation result is not supersession:
            // the current validation itself failed and must invalidate.
            const bool readable = current.available && current.snapshotGeneration != 0;
            // A zero captured generation means this host has no baseline yet
            // (for example a restored proposal before the first refresh). The
            // readable result becomes the baseline instead of being called a
            // different generation.
            const bool superseded = readable && snapshotGeneration != 0 &&
                (current.snapshotGeneration != snapshotGeneration ||
                 snapshotGeneration != m_codebaseSnapshotGeneration);
            if (superseded) {
                finishQuitWhenRuntimesStopped();
                return;
            }

            bool equivalent = readable && current.validated && current.complete &&
                              current.astSnapshotHash == proposalAstHash;
            if (equivalent && bindingKnown && current.evidence != evidence) equivalent = false;
            if (equivalent && !bindingKnown && proposalAstHash.empty()) {
                // There is no stored evidence vector to compare against. A
                // restored proposal must not stay reviewable when the current
                // resolution is already ambiguous or locally incomplete.
                for (const auto& item : current.evidence) {
                    if (!item.complete || item.status == "AMBIGUOUS") {
                        equivalent = false;
                        break;
                    }
                }
            }

            if (!equivalent) {
                m_activeGroundedEvidence.clear();
                setGroundingUnavailable(QStringLiteral("STALE"));
                if (m_currentProposal || m_currentApproval ||
                    m_proposalStatus == ProposalUiStatus::Generating) {
                    queueModelCompletion("invalidateStaleCodeEvidence",
                        [this, serial, workspace, workspaceEpoch, proposalId, sessionId, revisionId]() -> AsyncTask<void> {
                            // Recheck the whole binding after queuing: a newer
                            // proposal, workspace, revision or epoch must
                            // survive an older validation failure.
                            if (serial != m_groundingValidationSerial) co_return;
                            if (workspace != m_projectScanPath) co_return;
                            if (workspaceEpoch != m_workspaceEpoch) co_return;
                            if (m_hostState == HostState::Quitting) co_return;
                            if (!m_currentProposal || m_currentProposal->id != proposalId) co_return;
                            if (m_proposalWorkspaceEpoch != workspaceEpoch) co_return;
                            if (currentSessionId() != sessionId) co_return;
                            if (!m_revisionState ||
                                m_revisionState->currentRevision().revisionId().value() != revisionId)
                                co_return;
                            if (auto status = (co_await invalidateProposalArtifacts()); !status.ok())
                                setLastError(status.error());
                        });
                }
                finishQuitWhenRuntimesStopped();
                return;
            }

            // Equivalent evidence: unrelated changes are harmless. Adopt the
            // baseline for a restored proposal, then only generation advances.
            if (!bindingKnown) {
                m_activeGroundedEvidence = current.evidence;
                m_activeGroundingPrompt = current.prompt;
                m_activeGroundingActiveFile = current.activeFile;
                m_activeGroundingProposalId = proposalId;
                m_activeGroundingSessionId = sessionId;
                m_activeGroundingRevisionId = revisionId;
            }
            m_activeGroundingGeneration = current.snapshotGeneration;
            finishQuitWhenRuntimesStopped();
        });
    watcher->setFuture(QtConcurrent::run([inputs]() -> GroundedPromptContext {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
        if (const int delayMs = codebaseRevalidationDelayForTesting(); delayMs > 0)
            QThread::msleep(static_cast<unsigned long>(delayMs));
#endif
        try {
            return computeGroundedPromptContext(inputs);
        } catch (...) {
            return GroundedPromptContext{};
        }
    }));
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









AsyncTask<core::Status> HostController::restoreRevisionOperation(QString revisionId, quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (!m_revisionState) {
            co_return makeError(ErrorCode::PreconditionFailed, "Open a session before restoring one of its revisions.");
        }
        if (!m_repository) {
            co_return storageUnavailable();
        }
        auto rec = (co_await m_repository->getRevision(core::SessionId(currentSessionId()), core::RevisionId(ss(revisionId))));
        if (!rec.ok()) {
            co_return rec.error();
        }
        if (!rec.value().has_value()) {
            co_return makeError(ErrorCode::NotFound, "That revision isn't part of the open session.");
        }
        const std::string text = rec.value()->revision.text();
        if (auto st = (co_await commitRevision(text, expectedVersion, storage::RevisionOrigin::Restore)); !st.ok()) {
            co_return st;
        }
        m_followAmbientDraft = false;
        m_widgetDraftEdited = true; // Restored text, not the live editor, owns this draft.
        m_captureRequestedApp.clear();
        invalidateDownstreamPipeline();
        emit revisionRestored(qs(text));
        co_return Status::success();

}

AsyncTask<core::Status> HostController::restoreSessionForReviewOperation(QString sessionId, quint64 expectedVersion) {
    if (!(co_await restoreSessionOperation(sessionId, expectedVersion))) co_return Status(makeError(ErrorCode::StaleVersion, "Session restore failed"));
    // Open saved work directly; a normal capture hotkey would adopt a different editor draft.
    setCaptureSource(CaptureSource::Widget);
    showCardInternal(false);
    co_return Status::success();
}

AsyncTask<core::Status> HostController::restoreSessionOperation(QString sessionId, quint64 expectedVersion) {

        if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
        if (!m_repository) {
            co_return storageUnavailable();
        }
        auto restored = (co_await m_repository->restoreSession(core::SessionId(ss(sessionId))));
        if (!restored.ok()) {
            co_return restored.error();
        }
        auto& r = restored.value();

        // Switching sessions does not invalidate the saved approval; the restored copy is re-validated below.
        m_followAmbientDraft = false;
        m_widgetDraftEdited = true;
        m_captureRequestedApp.clear();
        m_currentApproval.reset();
        invalidateDownstreamPipeline();
        m_revisionState = std::make_unique<core::RevisionState>(
            r.latestRevision.revision, r.session.highestRevisionSeq, r.session.stateVersion,
            r.session.draftEditGeneration, r.session.proposalEditGeneration);
        emit stateVersionChanged();
        emit revisionRestored(qs(r.latestRevision.revision.text()));

        (co_await rebuildFromRestore(r));
        co_return Status::success();

}

AsyncTask<void> HostController::rebuildFromRestore(storage::RestoredSession r) {
    const auto operationWorkspaceEpoch = m_workspaceEpoch;
    const auto operationPipelineSerial = m_pipelineSerial;
    if (!r.analysis.has_value()) {
        if (r.legacyAnalysisRequiresReview) setAnalysisStatus(AnalysisStatus::Idle,
            QStringLiteral("This history predates saved scorer evidence. Analyze and review it again before rewriting."));
        co_return;
    }
    const std::string& text = m_revisionState->currentRevision().text();
    if (analysis::validateNormalizedAnalysis(r.analysis->normalized, text).status == analysis::AnalysisValidationStatus::Invalid) {
        setAnalysisStatus(AnalysisStatus::Idle, QStringLiteral("The saved analysis no longer matches this prompt. Analyze it again."));
        co_return;
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
                                                                             m_workingIntent, r.intent->createdAtMs,
                                                                             supplementalInterviewDecisions());
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
        if (auto st = (co_await finalizeIntentIfQuiet()); !st.ok()) {
            setLastError(st.error());
        }
    }
    if (operationWorkspaceEpoch != m_workspaceEpoch || operationPipelineSerial != m_pipelineSerial) co_return;
    setAnalysisStatus(AnalysisStatus::Ready, QStringLiteral("Restored the saved analysis for this prompt."));
    emit interventionChanged();
    emit intentChanged();

    if (!intentRestored || !r.proposal.has_value() ||
        r.proposal->proposalEditGeneration != m_revisionState->proposalEditGeneration()) {
        co_return;
    }
    const auto& proposal = *r.proposal;
    if (proposal.status == rewrite::ProposalStatus::NeedsClarification) {
        m_currentProposal = proposal;
        m_proposalWorkspaceEpoch = m_workspaceEpoch;
        setProposalStatus(ProposalUiStatus::NeedsClarification, qs(proposal.clarificationQuestion));
        co_return;
    }
    const auto validation = rewrite::RewriteValidator::validateProposal(
        proposal, currentSessionId(), m_revisionState->currentRevision().revisionId().value(), *m_confirmedIntent, {});
    if (!validation.structurallyValid) {
        co_return;
    }
    m_currentProposal = proposal;
    m_lastAuditResult = proposal.auditResult;
    if (m_confirmedIntent) {
        const auto constraints = extractSessionConstraints(proposal.originalText, *m_confirmedIntent);
        m_creativeAlternatives = core::rewrite::CreativeExpansionEngine::expand(
            proposal.originalText, proposal.rewrittenText, constraints);
        m_creativeAlternativeAuditExecutionStatuses.assign(m_creativeAlternatives.size(), core::AuditExecutionStatus::Unavailable);
        if (m_lastAuditResult.has_value() && !m_creativeAlternativeAuditExecutionStatuses.empty()) {
            m_creativeAlternativeAuditExecutionStatuses[0] = m_lastAuditResult->executionStatus;
        }
        if (m_lastAuditResult.has_value() && !m_creativeAlternatives.empty()) {
            m_creativeAlternatives[0].auditOutcome = m_lastAuditResult->outcome;
        }
        m_selectedAlternativeIndex = 0;
        emit creativeAlternativesChanged();
        emit selectedAlternativeChanged();
    }
    // A restored proposal is admitted only into the restore path's current
    // workspace epoch. AST-bound proposals are still revalidated below before
    // approval can remain enabled.
    m_proposalWorkspaceEpoch = m_workspaceEpoch;
    if (r.approval.has_value()) {
        const auto ctx = approvalContext();
        if (ctx.has_value() && review::ApprovalFactory::isApprovalValid(*r.approval, *ctx)) {
            m_currentApproval = *r.approval;
            m_acknowledgedConstraints.insert(validation.pendingAcknowledgments.begin(),
                                             validation.pendingAcknowledgments.end());
        } else if (auto st = (co_await m_repository->invalidateApprovals(core::SessionId(currentSessionId()))); !st.ok()) {
            setLastError(st.error());
        }
    }
    if (operationWorkspaceEpoch != m_workspaceEpoch || operationPipelineSerial != m_pipelineSerial) co_return;
    setProposalStatus(ProposalUiStatus::ReadyForReview, QStringLiteral("Restored the saved rewrite for review."));
    emit approvalChanged();
    scheduleGroundingRevalidation();

    co_return;
}

AsyncTask<core::Status> HostController::deleteSessionOperation(QString sessionId) {

        if (!m_repository) {
            co_return storageUnavailable();
        }
        const std::string id = ss(sessionId);
        if (auto st = (co_await m_repository->deleteSession(core::SessionId(id))); !st.ok()) {
            co_return st;
        }
        if (id == currentSessionId()) {
            resetSessionState();
        }
        refreshStorageStats();
        emit sessionListChanged();
        co_return Status::success();

}

AsyncTask<core::Status> HostController::purgeHistoryOperation() {

        if (!m_repository) {
            co_return storageUnavailable();
        }
        if (auto st = (co_await m_repository->purgeHistory()); !st.ok()) {
            co_return st;
        }
        resetSessionState();
        refreshStorageStats();
        emit sessionListChanged();
        co_return Status::success();

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
    m_julia->setJuliaDevice(m_juliaDevice);
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

void HostController::setJuliaDevice(const QString& device) {
    static const QStringList kAllowed{
        QStringLiteral(""), QStringLiteral("auto"), QStringLiteral("cpu"),
        QStringLiteral("cuda"), QStringLiteral("mps"), QStringLiteral("directml")};
    if (!kAllowed.contains(device)) return;
    if (device == m_juliaDevice) return;
    m_juliaDevice = device;
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        if (device.isEmpty()) settings.remove(QStringLiteral("scoring/juliaDevice"));
        else settings.setValue(QStringLiteral("scoring/juliaDevice"), device);
    }
    refreshJuliaModel();
    emit scorerEngineChanged();
}

bool HostController::isJuliaModelInstalled() const {
    if (m_juliaModelDirectory.isEmpty()) return false;
    const QDir d(m_juliaModelDirectory);
    if (d.exists(QStringLiteral("model.safetensors"))) {
        const QFileInfo fi(d.filePath(QStringLiteral("model.safetensors")));
        if (fi.isFile() && fi.size() > 1024 * 1024) return true;
    }
    if (d.exists(QStringLiteral("roclint-julia.json"))) {
        const QFileInfo fi(d.filePath(QStringLiteral("roclint-julia.json")));
        if (fi.isFile() && fi.size() > 0) return true;
    }
    return false;
}

QString HostController::juliaDiskStatus() const {
    if (m_juliaModelDirectory.isEmpty()) return QStringLiteral("No model folder configured");
    const QDir d(m_juliaModelDirectory);
    if (d.exists(QStringLiteral("model.safetensors"))) {
        const QFileInfo fi(d.filePath(QStringLiteral("model.safetensors")));
        const double mb = static_cast<double>(fi.size()) / (1024.0 * 1024.0);
        return QStringLiteral("✓ Installed on disk (%1 MB · Verified)").arg(QString::number(mb, 'f', 0));
    }
    if (d.exists(QStringLiteral("roclint-julia.json"))) {
        return QStringLiteral("✓ Configured on disk (roclint-julia.json)");
    }
    return QStringLiteral("⚠️ Not downloaded on disk (click 'Download / repair' or choose your model folder)");
}

QString HostController::juliaMemoryStatus() const {
    if (m_julia->isReady()) {
        return QStringLiteral("Loaded in GPU memory (Apple Metal MPS float16)");
    }
    const auto state = m_julia->state();
    if (state == QLatin1String("LOADING") || state == QLatin1String("STARTING")) {
        return QStringLiteral("Starting Julia worker process...");
    }
    if (state == QLatin1String("DOWNLOADING")) {
        return QStringLiteral("Downloading Julia model...");
    }
    if (isJuliaModelInstalled()) {
        return QStringLiteral("Unloaded (Standby on disk — warms automatically on demand or click 'Load Julia')");
    }
    return QStringLiteral("Unloaded (Model missing from disk)");
}

QString HostController::juliaStatus() const {
    if (m_julia->isReady()) return m_julia->detail();
    if (isJuliaModelInstalled()) {
        return QStringLiteral("Julia-1 weights verified on disk (Standby). Warms on demand or click 'Load Julia'.");
    }
    return m_julia->detail();
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
    emit modelDirectoryChanged();
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
    emit modelDirectoryChanged();
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
        item[QStringLiteral("isInstalled")] = true;
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

AsyncTask<void> HostController::onNativeRuntimeChanged() {
    emit modelEngineStatusChanged();
    if (!nativeGraniteSelected() || usesTransportOverride() || m_hostState == HostState::Quitting) {
        if (!m_native->isRunning() || m_native->state() == QLatin1String("FAILED")) m_nativeWarming = false;
        co_return;
    }
    const auto state = m_native->state();
    if (m_native->isReady()) {
        m_nativeLastUse.restart();
        m_nativeWarming = false;
        const auto info = m_native->modelInfo();
        if (info.quantization != ss(NativeGraniteRuntime::quantizationForTag(m_activeModel))) co_return;
        if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy && m_graniteAdapter->state() != bridge::GraniteRuntimeState::Ready) {
            if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Loading) {
                static_cast<void>(m_graniteAdapter->unload()); static_cast<void>(m_graniteAdapter->beginLoading());
            }
            static_cast<void>(m_graniteAdapter->completeLoading(info));
        }
        m_availableModels.clear(); appendNativeModelFiles(); appendLocalModelFiles(); emit availableModelsChanged();
        if (m_discoveryQueued && m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready) {
            if (auto status = (co_await startDiscovery(false)); !status.ok()) setLastError(status.error());
        }
        if (m_pendingNativeRewrite && m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready) {
            const auto pending = std::exchange(m_pendingNativeRewrite, std::nullopt);
            queueModelCompletion("resumeNativeRewrite", [this, pending]() -> AsyncTask<void> {
                // Loading can finish while another command is persisting. Revalidate the
                // captured request after taking its turn, before advancing proposal state.
                if (pending->stateVersion == stateVersion() && m_confirmedIntent && pending->intentId == m_confirmedIntent->id &&
                    pending->profileId == m_rewriteProfile && pending->configurationGeneration == m_configurationGeneration) {
                    setProposalStatus(ProposalUiStatus::None, {});
                    if (auto status = co_await synthesizeRewriteOperation(pending->stateVersion); !status.ok())
                        setProposalStatus(ProposalUiStatus::Failed, plainError(status.error()));
                }
                co_return;
            });
        }
        if (m_pendingNativeReview) {
            const auto pending = std::exchange(m_pendingNativeReview, std::nullopt);
            queueModelCompletion("resumeNativeReview", [this, pending]() -> AsyncTask<void> {
                if (pending->serial != m_reviewSerial) co_return;
                if (!reviewBindingMatchesCurrent() ||
                    pending->configurationGeneration != m_configurationGeneration) {
                    markComprehensionReviewStale();
                    co_return;
                }
                m_reviewState = ReviewState::NotRun;
                if (auto status = co_await requestComprehensionReviewOperation(stateVersion()); !status.ok()) {
                    m_reviewState = ReviewState::Failed;
                    emit comprehensionReviewChanged();
                    setLastError(status.error());
                }
                co_return;
            });
        }
        if (m_pendingNativeClarification) {
            const auto pending = std::exchange(m_pendingNativeClarification, std::nullopt);
            queueModelCompletion("resumeNativeClarification", [this, pending]() -> AsyncTask<void> {
                if (pending->serial != m_clarificationCheckSerial) co_return;
                const bool current = pending->stateVersion == stateVersion() &&
                    pending->configurationGeneration == m_configurationGeneration &&
                    pending->workspaceEpoch == m_workspaceEpoch && m_revisionState &&
                    pending->revisionId == m_revisionState->currentRevision().revisionId().value() &&
                    m_currentDecision.activeItemId && *m_currentDecision.activeItemId == pending->itemId;
                resetClarificationCheck();
                if (current) {
                    if (auto status = co_await submitCustomIntentOperation(pending->answer, pending->stateVersion); !status.ok())
                        setLastError(status.error());
                }
                co_return;
            });
        }
        if (m_ambientCheckPending) m_ambientDebounce.start();
        startPendingWordRefinement();
    } else if (state == QLatin1String("LOADING")) {
        m_nativeWarming = true;
        if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy && m_graniteAdapter->state() != bridge::GraniteRuntimeState::Loading) {
            static_cast<void>(m_graniteAdapter->unload());
            static_cast<void>(m_graniteAdapter->beginLoading());
        }
    } else if (state == QLatin1String("FAILED")) {
        m_nativeWarming = false;
        if (m_pendingWordRefinement && m_wordCompanionState == QLatin1String("checking")) {
            m_pendingWordRefinement = {};
            setWordCompanionState(QStringLiteral("failed"),
                QStringLiteral("The local model couldn't load. Check its setup or try again."));
        }
        if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy) m_graniteAdapter->failLoading(ss(m_native->detail()));
        if (m_discoveryQueued) { m_discoveryQueued = false; setAnalysisStatus(AnalysisStatus::Failed, m_native->detail()); }
        if (m_pendingNativeRewrite) { m_pendingNativeRewrite.reset(); setProposalStatus(ProposalUiStatus::Failed, m_native->detail()); }
        if (m_pendingNativeReview) {
            m_pendingNativeReview.reset();
            m_reviewState = ReviewState::Failed;
            emit comprehensionReviewChanged();
            emit rewriteProposalChanged();
        }
        if (m_pendingNativeClarification) {
            resetClarificationCheck();
            m_clarificationFeedback = QStringLiteral("The local model could not load. Your answer has not been accepted. Retry to check it.");
            emit clarificationCheckChanged();
        }
    } else if (state == QLatin1String("UNLOADED")) {
        m_nativeWarming = false;
        if (m_graniteAdapter->state() == bridge::GraniteRuntimeState::Loading) static_cast<void>(m_graniteAdapter->cancelLoading());
        else if (m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy) static_cast<void>(m_graniteAdapter->unload());
    }
    emit modelEngineStatusChanged(); emit intentChanged();

    co_return;
}

bool HostController::isNativeGraniteInstalled() const {
    if (m_nativeDirectory.isEmpty()) return false;
    const QDir d(m_nativeDirectory);
    const QString activeQuant = NativeGraniteRuntime::quantizationForTag(m_activeModel);
    for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
        const auto cand = entry.toObject();
        if (!activeQuant.isEmpty() && cand.value(QStringLiteral("quantization")).toString() != activeQuant) {
            continue;
        }
        const QFileInfo fi(d.filePath(cand.value(QStringLiteral("filename")).toString()));
        if (fi.isFile() && fi.size() == cand.value(QStringLiteral("size_bytes")).toInteger()) {
            return true;
        }
    }
    if (activeQuant.isEmpty()) {
        for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
            const auto cand = entry.toObject();
            const QFileInfo fi(d.filePath(cand.value(QStringLiteral("filename")).toString()));
            if (fi.isFile() && fi.size() == cand.value(QStringLiteral("size_bytes")).toInteger()) {
                return true;
            }
        }
    }
    return false;
}

bool HostController::isNativeGraniteQ8Installed() const {
    if (m_nativeDirectory.isEmpty()) return false;
    const QDir d(m_nativeDirectory);
    for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
        const auto cand = entry.toObject();
        if (cand.value(QStringLiteral("quantization")).toString() == QLatin1String("Q8_0")) {
            const QFileInfo fi(d.filePath(cand.value(QStringLiteral("filename")).toString()));
            return fi.isFile() && fi.size() == cand.value(QStringLiteral("size_bytes")).toInteger();
        }
    }
    return false;
}

bool HostController::isNativeGraniteQ5Installed() const {
    if (m_nativeDirectory.isEmpty()) return false;
    const QDir d(m_nativeDirectory);
    for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
        const auto cand = entry.toObject();
        if (cand.value(QStringLiteral("quantization")).toString() == QLatin1String("Q5_K_M")) {
            const QFileInfo fi(d.filePath(cand.value(QStringLiteral("filename")).toString()));
            return fi.isFile() && fi.size() == cand.value(QStringLiteral("size_bytes")).toInteger();
        }
    }
    return false;
}

bool HostController::isNativeGraniteQ4Installed() const {
    if (m_nativeDirectory.isEmpty()) return false;
    const QDir d(m_nativeDirectory);
    for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
        const auto cand = entry.toObject();
        if (cand.value(QStringLiteral("quantization")).toString() == QLatin1String("Q4_K_M")) {
            const QFileInfo fi(d.filePath(cand.value(QStringLiteral("filename")).toString()));
            return fi.isFile() && fi.size() == cand.value(QStringLiteral("size_bytes")).toInteger();
        }
    }
    return false;
}

qint64 HostController::generationTokens() const {
    return nativeGraniteSelected() && m_native ? m_native->lastOutputTokens() : 0;
}

qint64 HostController::reasoningTokens() const {
    return nativeGraniteSelected() && m_native ? m_native->lastReasoningTokens() : 0;
}
qint64 HostController::answerTokens() const {
    return nativeGraniteSelected() && m_native ? m_native->lastAnswerTokens() : 0;
}
QString HostController::generationPhase() const {
    return nativeGraniteSelected() && m_native ? m_native->generationPhase() : QStringLiteral("idle");
}

double HostController::generationElapsedMs() const {
    return nativeGraniteSelected() && m_native ? m_native->lastGenerationMs() : 0;
}

double HostController::decodeElapsedMs() const {
    return nativeGraniteSelected() && m_native ? m_native->lastDecodeMs() : 0;
}

void HostController::setThinkingEnabled(bool enabled) {
    if (m_thinkingEnabled == enabled || m_hostState == HostState::Quitting) return;
    m_thinkingEnabled = enabled;
    ++m_configurationGeneration;
    m_ambientCheck.reset(); m_ambientDismissed.clear(); m_ambientCheckPending = m_ambientObservation.has_value();
    invalidateDownstreamPipeline();
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings; settings.setValue(QStringLiteral("models/thinkingEnabled"), enabled);
    }
    emit modelEngineStatusChanged();
}

void HostController::setTokenSpeedMode(const QString& mode) {
    if (m_tokenSpeedMode == mode) return;
    m_tokenSpeedMode = mode;
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("models/tokenSpeedMode"), mode);
    }
    emit tokenSpeedModeChanged();
    emit inferenceSpeedChanged();
}

double HostController::generationTokensPerSecond() const {
    if (nativeGraniteSelected() && m_native) {
        return m_native->lastGenerationTokensPerSecond();
    }
    return 0.0;
}

double HostController::decodeTokensPerSecond() const {
    if (nativeGraniteSelected() && m_native) {
        return m_native->lastDecodeTokensPerSecond();
    }
    return 0.0;
}

double HostController::tokensPerSecond() const {
    if (m_tokenSpeedMode == QLatin1String("DECODE")) {
        return decodeTokensPerSecond();
    }
    return generationTokensPerSecond();
}

QString HostController::generationSpeedLabel() const {
    const double s = generationTokensPerSecond();
    if (s > 0.0) {
        return QStringLiteral("%1 tok/s").arg(s, 0, 'f', 1);
    }
    return {};
}

QString HostController::decodeSpeedLabel() const {
    const double s = decodeTokensPerSecond();
    if (s > 0.0) {
        return QStringLiteral("%1 tok/s").arg(s, 0, 'f', 1);
    }
    return {};
}

QString HostController::inferenceSpeed() const {
    const double gen = generationTokensPerSecond();
    const double dec = decodeTokensPerSecond();
    if (m_tokenSpeedMode == QLatin1String("BOTH")) {
        if (gen > 0.0 && dec > 0.0) {
            return QStringLiteral("%1 tok/s (gen) · %2 tok/s (dec)")
                .arg(gen, 0, 'f', 1)
                .arg(dec, 0, 'f', 1);
        }
        if (gen > 0.0) return QStringLiteral("%1 tok/s (gen)").arg(gen, 0, 'f', 1);
        if (dec > 0.0) return QStringLiteral("%1 tok/s (dec)").arg(dec, 0, 'f', 1);
    } else if (m_tokenSpeedMode == QLatin1String("DECODE")) {
        if (dec > 0.0) return QStringLiteral("%1 tok/s (dec)").arg(dec, 0, 'f', 1);
    } else {
        if (gen > 0.0) return QStringLiteral("%1 tok/s (gen)").arg(gen, 0, 'f', 1);
    }
    return {};
}

QString HostController::nativeGraniteDiskStatus() const {
    if (m_nativeDirectory.isEmpty()) return QStringLiteral("No model folder configured");
    const QDir d(m_nativeDirectory);
    const QString activeQuant = NativeGraniteRuntime::quantizationForTag(m_activeModel);
    for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
        const auto cand = entry.toObject();
        const auto quant = cand.value(QStringLiteral("quantization")).toString();
        if (!activeQuant.isEmpty() && quant != activeQuant) continue;
        const QFileInfo fi(d.filePath(cand.value(QStringLiteral("filename")).toString()));
        if (fi.isFile() && fi.size() == cand.value(QStringLiteral("size_bytes")).toInteger()) {
            const double gb = static_cast<double>(fi.size()) / (1024.0 * 1024.0 * 1024.0);
            return QStringLiteral("✓ Installed on disk (%1 GiB · %2 · Ready)").arg(QString::number(gb, 'f', 2), quant);
        }
    }
    for (const auto& entry : NativeGraniteRuntime::manifest().value(QStringLiteral("candidates")).toArray()) {
        const auto cand = entry.toObject();
        const auto quant = cand.value(QStringLiteral("quantization")).toString();
        const QFileInfo fi(d.filePath(cand.value(QStringLiteral("filename")).toString()));
        if (fi.isFile() && fi.size() == cand.value(QStringLiteral("size_bytes")).toInteger()) {
            const double gb = static_cast<double>(fi.size()) / (1024.0 * 1024.0 * 1024.0);
            return QStringLiteral("✓ %1 installed on disk (%2 GiB · select candidate)").arg(quant, QString::number(gb, 'f', 2));
        }
    }
    return QStringLiteral("⚠️ Not downloaded on disk (download below or connect your model folder)");
}

QString HostController::nativeGraniteMemoryStatus() const {
    if (m_native->isReady() || usesLiveModel()) {
        const double tps = tokensPerSecond();
        if (tps > 0.0) {
            return QStringLiteral("Loaded in GPU memory (%1 · %2 tok/s)")
                .arg(acceleratorName(), QString::number(tps, 'f', 1));
        }
        return QStringLiteral("Loaded in GPU memory (%1 Unified Memory · Ready for inference)")
            .arg(acceleratorName());
    }
    if (m_nativeWarming || isCheckingModel()) {
        return QStringLiteral("Warming into GPU Unified Memory (%1)...").arg(acceleratorName());
    }
    if (isNativeGraniteInstalled()) {
        return QStringLiteral("Unloaded (Standby on disk — warms automatically on first prompt or click 'Load model')");
    }
    return QStringLiteral("Unloaded (Model missing from disk)");
}

QString HostController::modelRuntimeState() const {
    const auto s = bridge::toString(m_graniteAdapter->state());
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString HostController::modelEngineStatus() const {
    if (nativeGraniteSelected() && !usesTransportOverride()) {
        if (m_native->isReady()) return m_native->detail();
        if (m_nativeWarming || isCheckingModel()) return QStringLiteral("Warming Granite into %1 GPU memory...").arg(acceleratorName());
        if (isNativeGraniteInstalled()) {
            return QStringLiteral("Model verified on disk (Standby). Warms automatically on prompt or click 'Warm Model'.");
        }
        return QStringLiteral("Model file not found. Download Granite below or connect your model folder.");
    }
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
            status = QStringLiteral("Unavailable. Retry explicitly or keep the original prompt.");
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

AsyncTask<void> HostController::onHealthFinished(HealthResult result) {
    m_healthCheckInFlight = false;
    if (nativeGraniteSelected() && !usesTransportOverride()) { refreshAvailableModels(); co_return; }
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
        co_return;
    }
    if (m_discoveryQueued && m_graniteAdapter->state() != bridge::GraniteRuntimeState::Busy) {
        if (auto st = (co_await startDiscovery(false)); !st.ok()) {
            setLastError(st.error());
        }
    }
    if (m_ambientCheckPending && m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready) {
        m_ambientDebounce.start();
    }

    co_return;
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
    if (!enabled) setCaptureSource(CaptureSource::Widget);
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("ambient/enabled"), enabled);
        settings.sync();
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
    if (!active) onFieldLost();
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

void HostController::setAmbientBadgeSuggestionsOnly(bool enabled) {
    if (m_ambientBadgeSuggestionsOnly == enabled) return;
    m_ambientBadgeSuggestionsOnly = enabled;
    QSettings().setValue(QStringLiteral("ambient/badgeSuggestionsOnly"), enabled);
    emit ambientBadgeChanged();
}

bool HostController::isAmbientBadgeVisible() const {
    if (!isAmbientActive() || isCardVisible() || wordCompanionVisible()) return false;
    if (!m_ambientObservation) return !m_ambientBadgeSuggestionsOnly && m_ambientBadgeRecovery.isActive();
    if (m_ambientDismissed.count(m_ambientObservation->textHash)) return false;
    return wordReplacementReady() || (!m_ambientBadgeSuggestionsOnly && m_ambientBadgeVisible);
}

void HostController::activateAmbientIndicator() {
    if (!isAmbientActive() || isCardVisible() || !m_ambientObservation) return;
    if (!m_hasWordRefinement && m_wordCompanionState == QLatin1String("idle")) return;
    m_wordCompanionRequested = true;
    emit wordRefinementChanged();
}

void HostController::setAmbientBadge(bool visible, const QString& label) {
    QPoint newPos = m_ambientBadgePos;
    if ((visible || m_hasWordRefinement || m_wordCompanionState != QLatin1String("idle")) && m_ambientObservation.has_value()) {
        const auto& f = m_ambientObservation->field;
        if (!f.fieldRect.isNull()) {
            const auto placement = ScreenPlacement::determineIndicatorPosition(f.fieldRect);
            newPos = QPoint(placement.x, placement.y);
        } else if (!f.caretRect.isNull()) {
            newPos = QPoint(f.caretRect.right() + 8, f.caretRect.bottom() + 6);
        } else {
            newPos = QPoint(40, 40);
        }
    }

    const QString newLabel = !label.isEmpty() ? label : m_ambientBadgeLabel;
    if (m_ambientBadgeVisible == visible && m_ambientBadgePos == newPos && m_ambientBadgeLabel == newLabel) {
        return;
    }

    m_ambientBadgeVisible = visible;
    m_ambientBadgeLabel = newLabel;
    m_ambientBadgePos = newPos;
    emit ambientBadgeChanged();
}

void HostController::setAmbientStatus(const QString& status) {
    if (m_ambientStatus != status) {
        m_ambientStatus = status;
        emit ambientStatusChanged();
    }
}

void HostController::onFieldLost(bool interrupted) {
    if (interrupted) {
        if (!m_ambientObservation) return;
        // Only presentation survives one missed read. Invalidate the draft and
        // suggestion now so the indicator cannot open or edit stale text.
        if (isAmbientBadgeVisible()) m_ambientBadgeRecovery.start();
    } else {
        m_ambientBadgeRecovery.stop();
    }
    m_ambientDebounce.stop();
    m_ambientCheckPending = false;
    m_ambientObservation.reset();
    setAmbientBadge(false);
    clearWordRefinement();
    emit ambientBadgeChanged();
}

void HostController::followAmbientDraft() {
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS") && m_captureTarget && m_ambientObservation &&
        m_captureTarget->textHash != m_ambientObservation->textHash) {
        qInfo() << "[Roclint follow] visible=" << isCardVisible() << "ambient=" << isAmbientCapture()
                << "following=" << m_followAmbientDraft << "delivery=" << m_pendingDelivery.has_value()
                << "target_token=" << m_captureTarget->field.fieldToken << "observed_token=" << m_ambientObservation->field.fieldToken
                << "target_pid=" << m_captureTarget->field.processId << "observed_pid=" << m_ambientObservation->field.processId;
    }
    if (!isCardVisible() || m_resumeMinimizedCard || !m_ambientObservation || m_pendingDelivery) return;
    const auto observed = *m_ambientObservation;
    const bool inReview = isClarifying() || hasConfirmedIntent() || m_currentProposal.has_value();
    const bool preserveHud = m_widgetDraftEdited || m_selectingInsertionTarget || inReview;
    // Chromium can expose the editor after the initial bounded invocation. Bind it without
    // another hotkey; a draft already typed into the HUD remains the user's authoritative text.
    if (!isAmbientCapture() && !m_captureOpenPending && !m_captureRequestedApp.isEmpty() &&
        observed.field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0) {
        if (!preserveHud && m_revisionState && !noteDraftEditedInternal(stateVersion(), false)) return;
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
    if (!isAmbientCapture() || !m_captureTarget) return;
    if (m_widgetDraftEdited || m_selectingInsertionTarget) {
        m_followAmbientDraft = false;
        return;
    }
    const auto& previous = *m_captureTarget;
    const bool sameField = previous.field.fieldToken == observed.field.fieldToken &&
        previous.field.processId == observed.field.processId &&
        previous.field.appIdentifier.compare(observed.field.appIdentifier, Qt::CaseInsensitive) == 0;
    // Answering a question freezes that review, but a real edit in its bound
    // composer invalidates it. An unrelated field must not replace that review.
    if ((!m_followAmbientDraft || (inReview && !m_currentApproval)) && !sameField) return;
    if (sameField && previous.textHash == observed.textHash) {
        m_captureTarget->field = observed.field;
        return;
    }
    if (!noteDraftEditedInternal(stateVersion(), false)) return;
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

void HostController::onFieldObserved(const ObservedField& rawField) {
    auto field = rawField;
    if (field.geometryInNativePixels) {
        field.fieldRect = ScreenPlacement::fromNativePixels(field.fieldRect);
        field.caretRect = ScreenPlacement::fromNativePixels(field.caretRect);
        for (auto& rect : field.wordRects) rect = ScreenPlacement::fromNativePixels(rect);
        field.geometryInNativePixels = false;
    }
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
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
        qInfo() << "[HostController] onFieldObserved: app=" << field.appDisplayName << "characters=" << prompt.size();
    }
    prompt = qs(core::normalizeLineEndings(ss(prompt)));
    if (field.kind == FieldKind::GuiEditor && prompt.trimmed().isEmpty()) {
        const bool recoveringBadge = m_ambientBadgeRecovery.isActive();
        m_ambientBadgeRecovery.stop();
        clearWordRefinement();
        // An empty composer is still a valid destination for a draft authored in the HUD.
        m_ambientDebounce.stop();
        if (!m_ambientObservation.has_value() || m_ambientObservation->prompt.trimmed().isEmpty() ||
            m_ambientObservation->field.fieldToken == field.fieldToken) {
            m_ambientObservation = AmbientObservation{field, prompt, core::crypto::Sha256::hexDigest(ss(prompt))};
        } else {
            // A different destination field (box Y) was focused in the same app session.
            // Keep the non-empty draft prompt from the previous field in this session,
            // but update the target field to this newly focused destination box.
            auto updated = *m_ambientObservation;
            updated.field = field;
            m_ambientObservation = std::move(updated);
        }
        setAmbientBadge(false);
        if (recoveringBadge) emit ambientBadgeChanged();
        followAmbientDraft();
        setAmbientStatus(QStringLiteral("Ready to capture a prompt in %1.").arg(field.appDisplayName));
        if (m_captureOpenPending &&
            (field.refreshRequestId == m_captureOpenGeneration || field.refreshRequestId == 0) &&
            (field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0 || m_captureRequestedApp.isEmpty())) {
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
    const bool fieldTokenChanged = !m_ambientObservation.has_value() ||
                                   m_ambientObservation->field.fieldToken != field.fieldToken ||
                                   m_ambientObservation->field.processId != field.processId;
    const bool rectChanged = m_ambientObservation.has_value() &&
                             m_ambientObservation->field.fieldRect != field.fieldRect;
    const bool textChanged = fieldTokenChanged || m_ambientObservation->textHash != observation.textHash;
    m_ambientObservation = std::move(observation);
    const bool recoveringBadge = m_ambientBadgeRecovery.isActive();
    m_ambientBadgeRecovery.stop();
    if (promptTriageRoutingEnabled()) {
        // The quiet entry point belongs to the composer. Current suggestions
        // affect the badge styling, while draft edits keep the window in place.
        setAmbientBadge(true, QStringLiteral("Check prompt"));
    } else if (fieldTokenChanged || rectChanged) {
        setAmbientBadge(!fieldTokenChanged && m_ambientBadgeVisible);
    }
    if (recoveringBadge || textChanged) emit ambientBadgeChanged();
    followAmbientDraft();
    detectWordRefinements(field);
    setAmbientStatus(QStringLiteral("Watching %1.").arg(field.appDisplayName));
    if (m_captureOpenPending &&
        (field.refreshRequestId == m_captureOpenGeneration || field.refreshRequestId == 0) &&
        (field.appIdentifier.compare(m_captureRequestedApp, Qt::CaseInsensitive) == 0 || m_captureRequestedApp.isEmpty())) {
        m_captureOpenPending = false;
        ++m_captureOpenGeneration;
        openFromAmbient();
        return;
    }
    if (promptTriageRoutingEnabled() || !textChanged) {
        return; // Production indicator admission is synchronous with the observed composer.
    }
    if (m_ambientCheck.has_value() && m_ambientCheck->textHash == m_ambientObservation->textHash) {
        setAmbientBadge(m_ambientCheck->needsAttention && m_ambientDismissed.count(m_ambientCheck->textHash) == 0,
                        m_ambientBadgeLabel);
        return;
    }
    m_ambientDebounce.start();
}

void HostController::onAmbientDebounce() {
    if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
        qInfo() << "[Roclint badge] active=" << isAmbientActive() << "field=" << m_ambientObservation.has_value()
                << "hudVisible=" << isCardVisible();
    if (!isAmbientActive() || !m_ambientObservation.has_value() || isCardVisible()) {
        m_ambientCheckPending = false;
        return;
    }
    const std::string& hash = m_ambientObservation->textHash;
    if (promptTriageRoutingEnabled()) {
        m_ambientCheckPending = false;
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
    const auto workspaceEpoch = m_workspaceEpoch;

    struct CheckResult {
        core::Result<bridge::GraniteOptionDiscoveryResponse> response{makeError(ErrorCode::Internal, "not run")};
        std::string raw;
    };
    m_ambientCheckInFlight = true;
    auto adapter = m_graniteAdapter;
    auto grounding = std::make_shared<GroundedPromptContext>();
    const auto groundingInput = groundingInputs(request.promptString);
    auto* watcher = new QFutureWatcher<CheckResult>(this);
    ++m_inFlight;
    const bool nativeWork = nativeGraniteSelected() && !usesTransportOverride();
    if (nativeWork) ++m_nativeSemanticInFlight;
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, observation, configurationGeneration, workspaceEpoch, nativeWork, grounding]() {
        --m_inFlight;
        if (nativeWork) --m_nativeSemanticInFlight;
        finishQuitWhenRuntimesStopped();
        m_ambientCheckInFlight = false;
        const CheckResult result = watcher->result();
        watcher->deleteLater();
        // An ambient check started in an older workspace epoch must not publish
        // its evidence view or its preview into the new workspace.
        if (workspaceEpoch != m_workspaceEpoch) {
            if (m_ambientObservation.has_value()) m_ambientDebounce.start();
            return;
        }
        applyGroundingContext(*grounding);
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
            resumeDiscovery();
        }
        if (m_ambientObservation.has_value() && (!m_ambientCheck.has_value() ||
                                                 m_ambientCheck->textHash != m_ambientObservation->textHash)) {
            m_ambientDebounce.start();
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [adapter, request, inner, groundingInput, grounding]() mutable -> CheckResult {
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
            *grounding = computeGroundedPromptContext(groundingInput);
            request.groundedCanonicalIntentXml = grounding->xml;
            out.response = adapter->discoverOptions(request, capture);
        } catch (const std::exception& ex) {
            out.response = makeError(ErrorCode::Internal, std::string("Background check crashed: ") + ex.what());
        }
        out.raw = *raw;
        return out;
    }));
}

AsyncTask<core::Status> HostController::openFromAmbientOperation(std::optional<AmbientObservation> observationSnapshot) {

        if (!isAmbientActive() || !observationSnapshot.has_value()) {
            co_return makeError(ErrorCode::PreconditionFailed, "There's no prompt from another app to open.");
        }
        if (!m_repository) {
            co_return storageUnavailable();
        }
        const bool resumeInterview = m_interviewOpenPending;
        m_interviewOpenPending = false;
        const auto pendingWordContext = m_interviewWordContext;
        const AmbientObservation observation = *observationSnapshot;
        const std::string text = ss(observation.prompt);

        if (m_selectingInsertionTarget) {
            m_selectingInsertionTarget = false;
            if (!m_revisionState || !m_currentProposal || m_proposalStatus != ProposalUiStatus::ReadyForReview) {
                showCardInternal(false);
                co_return makeError(ErrorCode::StaleVersion, "The rewrite changed while choosing its destination. Review it before inserting.");
            }
            m_captureSource = CaptureSource::Ambient;
            m_captureTarget = observation;
            m_followAmbientDraft = false;
            emit captureSourceChanged();
            showCardInternal(false);
            setAnalysisStatus(m_analysisStatus, QStringLiteral("Destination connected. Your rewrite and answers are unchanged; finish review, then Approve & Insert."));
            co_return Status::success();
        }

        if (isBlank(text) && observation.field.kind == FieldKind::GuiEditor) {
            const bool sameField = m_captureTarget &&
                m_captureTarget->field.fieldToken == observation.field.fieldToken &&
                m_captureTarget->field.processId == observation.field.processId &&
                m_captureTarget->field.appIdentifier.compare(observation.field.appIdentifier, Qt::CaseInsensitive) == 0;
            if (!sameField && hasActiveWork()) {
                showCardInternal(false);
                co_return Status::success();
            }
            resetSessionState();
            m_captureSource = CaptureSource::Ambient;
            m_captureTarget = observation;
            m_followAmbientDraft = true;
            m_widgetDraftEdited = false;
            emit captureSourceChanged();
            emit ambientDraftUpdated(QString());
            showCardInternal(false);
            co_return Status::success();
        }

        // Each ambient capture is its own session unless it is the prompt already open.
        const bool sameDraft = m_revisionState && (
            m_revisionState->currentRevision().text() == text ||
            m_revisionState->currentRevision().text().ends_with(text)
        );

        if (!sameDraft && hasActiveWork()) {
            showCardInternal(false);
            co_return Status::success();
        }

        if (!m_revisionState || m_revisionState->isDraftDirty() || !sameDraft) {
            resetSessionState();
            if (auto st = (co_await ensureSession(text, nowMs())); !st.ok()) {
                co_return st;
            }
            invalidateDownstreamPipeline();
        }
        if (resumeInterview) {
            ++m_interviewSerial;
            m_interviewActive = true;
            m_interviewComplete = false;
            m_interviewFeedback.clear();
            m_interviewWordContext = pendingWordContext;
            emit interviewChanged();
        }
        const bool activeWork = hasActiveWork();
        m_captureSource = CaptureSource::Ambient;
        m_captureTarget = observation;
        m_followAmbientDraft = !activeWork;
        if (!activeWork) {
            m_widgetDraftEdited = false;
        }
        emit captureSourceChanged();
        emit ambientDraftUpdated(qs(text));
        showCardInternal(false);

        if (!m_currentAnalysis) {
            bool rebound = false;
            if (m_ambientCheck.has_value() && m_ambientCheck->textHash == observation.textHash &&
                !m_ambientCheck->rawReply.empty()) {
                // The background check already asked the model; bind its exact reply to the frozen revision.
                auto parsed = bridge::parseDiscoveryResponse(m_ambientCheck->rawReply, currentSessionId(),
                                                             m_revisionState->currentRevision().revisionId().value(),
                                                             text);
                if (parsed.ok()) {
                    PipelineTicket ticket = makeTicket();
                    ticket.analysisGroup = std::make_shared<AnalysisGroup>(analysisGroupBudgetMs());
                    ticket.analysisGroup->id = newOpaqueId("grp_");
                    if (auto st = (co_await beginAttempt(ticket, "discovery")); !st.ok()) {
                        co_return st;
                    }
                    auto response = std::move(parsed).value();
                    response.model = m_ambientCheck->model;
                    armAnalysisDeadline(ticket);
                    (co_await acceptDiscovery(ticket, response));
                    rebound = true;
                }
            }
            if (!rebound) {
                if (auto st = (co_await startDiscovery()); !st.ok()) {
                    setLastError(st.error());
                }
            }
        }
        co_return Status::success();

}

void HostController::dismissAmbientBadge() {
    m_ambientBadgeRecovery.stop();
    if (m_ambientObservation.has_value()) {
        m_ambientDismissed.insert(m_ambientObservation->textHash);
    }
    setAmbientBadge(false);
    emit ambientBadgeChanged();
}

#ifdef ROCLINT_ENABLE_TEST_MOCKS
void HostController::setTransportOverrideForTesting(bridge::GraniteTransport transport) {
    m_transportOverride = std::move(transport);
}

quint64 HostController::startDiscoveryWithTriageForTesting(core::analysis::PromptTriage triage) {
    if (!m_revisionState) return 0;
    m_promptTriageRouting = true;
    m_promptTriage = triage;
    m_reviewTrigger = analysis::ReviewTrigger::Automatic;
    m_promptTriageLoad.reset();
    m_routedAnalysisGroup = std::make_shared<AnalysisGroup>(analysisGroupBudgetMs());
    m_routedAnalysisGroup->id = newOpaqueId("grp_");
    const auto sessionId = currentSessionId();
    const auto revisionId = m_revisionState->currentRevision().revisionId().value();
    return enqueueStorageCommand("startDiscoveryWithTriage",
        [this, triage = std::move(triage), sessionId, revisionId]() -> AsyncTask<core::Status> {
            // Production records the Julia triage attempt before the routed analysis.
            // The test seam seals the same evidence, so persist the honest row too.
            storage::ModelAttemptRecord record;
            record.attemptId = triage.attemptId;
            record.sessionId = sessionId;
            record.revisionId = revisionId;
            record.stage = "julia_triage";
            record.status = storage::AttemptStatus::InProgress;
            record.transport = "test_mock_triage";
            record.modelId = triage.modelId;
            record.modelDetail = "test classifier attempt";
            record.startedAtMs = nowMs();
            if (auto st = (co_await m_repository->beginModelAttempt(record)); !st.ok()) co_return st;
            const auto detail = "test classifier attempt; prompt_triage_payload=" + storage::encodePromptTriage(triage);
            if (auto st = (co_await m_repository->completeModelAttempt(
                    triage.attemptId, storage::AttemptStatus::Succeeded, triage.modelId, detail, "", nowMs()));
                !st.ok()) {
                co_return st;
            }
            co_return (co_await startDiscovery());
        });
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
    int idleTurns = 0;
    while (timer.elapsed() <= timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const bool busy = m_inFlight > 0 || m_nativeWarming || m_promptTriageLoad.has_value() ||
            storageBusy() || m_historyRefreshRunning || (m_storage && m_storage->pending() > 0);
        // A QML callLater or queued completion can start work during the pump.
        // Observe quiescence after dispatch, across two turns, before returning.
        idleTurns = busy ? 0 : idleTurns + 1;
        if (idleTurns >= 2) return true;
        QThread::msleep(1);
    }
    return false;
}
#endif

quint64 HostController::openFromAmbient() {
    const auto observation = m_ambientObservation;
    return enqueueStorageCommand("openFromAmbient", [=, this]() { return openFromAmbientOperation(observation); });
}

quint64 HostController::purgeHistory() {
    return enqueueStorageCommand("purgeHistory", [=, this]() { return purgeHistoryOperation(); });
}

quint64 HostController::deleteSession(QString sessionId) {
    return enqueueStorageCommand("deleteSession", [=, this]() { return deleteSessionOperation(sessionId); });
}

quint64 HostController::restoreSession(QString sessionId, quint64 expectedVersion) {
    return enqueueStorageCommand("restoreSession", [=, this]() { return restoreSessionOperation(sessionId, expectedVersion); });
}

quint64 HostController::restoreSessionForReview(QString sessionId, quint64 expectedVersion) {
    return enqueueStorageCommand("restoreSessionForReview", [=, this]() { return restoreSessionForReviewOperation(sessionId, expectedVersion); });
}

quint64 HostController::restoreRevision(QString revisionId, quint64 expectedVersion) {
    return enqueueStorageCommand("restoreRevision", [=, this]() { return restoreRevisionOperation(revisionId, expectedVersion); });
}

quint64 HostController::discardProposal(quint64 expectedVersion) {
    return enqueueStorageCommand("discardProposal", [=, this]() { return discardProposalOperation(expectedVersion); });
}

quint64 HostController::approveAndCopyRewrite(quint64 expectedVersion) {
    return enqueueStorageCommand("approveAndCopyRewrite", [=, this]() { return approveAndCopyRewriteOperation(expectedVersion); });
}

quint64 HostController::acknowledgeConstraint(QString constraintId, bool acknowledged, quint64 expectedVersion) {
    return enqueueStorageCommand("acknowledgeConstraint", [=, this]() { return acknowledgeConstraintOperation(constraintId, acknowledged, expectedVersion); });
}

quint64 HostController::synthesizeRewrite(quint64 expectedVersion) {
    return enqueueStorageCommand("synthesizeRewrite", [=, this]() { return synthesizeRewriteOperation(expectedVersion); });
}

quint64 HostController::setRewriteProfile(QString profile, quint64 expectedVersion) {
    return enqueueStorageCommand("setRewriteProfile", [=, this]() { return setRewriteProfileOperation(profile, expectedVersion); });
}

quint64 HostController::switchToManual(quint64 expectedVersion) {
    return enqueueStorageCommand("switchToManual", [=, this]() { return switchToManualOperation(expectedVersion); });
}

quint64 HostController::skipCurrentItem(quint64 expectedVersion) {
    return enqueueStorageCommand("skipCurrentItem", [=, this]() { return skipCurrentItemOperation(expectedVersion); });
}

quint64 HostController::rewriteWithoutQuestions(quint64 expectedVersion) {
    return enqueueStorageCommand("rewriteWithoutQuestions", [=, this]() { return rewriteWithoutQuestionsOperation(expectedVersion); });
}

quint64 HostController::resolveActiveAssumption(bool accepted, quint64 expectedVersion) {
    return enqueueStorageCommand("resolveActiveAssumption", [=, this]() { return resolveActiveAssumptionOperation(accepted, expectedVersion); });
}

quint64 HostController::keepOriginalPrompt(quint64 expectedVersion) {
    return enqueueStorageCommand("keepOriginalPrompt", [=, this]() { return keepOriginalPromptOperation(expectedVersion); });
}

quint64 HostController::submitCustomIntent(QString customText, quint64 expectedVersion) {
    return enqueueStorageCommand("submitCustomIntent", [=, this]() { return submitCustomIntentOperation(customText, expectedVersion); });
}

quint64 HostController::confirmChoice(QString alternativeId, quint64 expectedVersion) {
    return enqueueStorageCommand("confirmChoice", [=, this]() { return confirmChoiceOperation(alternativeId, expectedVersion); });
}

quint64 HostController::requestComprehensionReview(quint64 expectedVersion) {
    if (expectedVersion != stateVersion()) {
        setLastError(core::makeError(core::ErrorCode::StaleVersion,
                                     "The prompt changed. Review the current state and try again."));
        return 0;
    }
    if (m_proposalStatus != ProposalUiStatus::ReadyForReview || !m_currentProposal) {
        setLastError(core::makeError(core::ErrorCode::PreconditionFailed,
                                     "There is no suggested rewrite to review."));
        return 0;
    }
    if (m_reviewState == ReviewState::Running || !m_reviewReturnedFindingId.empty()) return 0;
    return enqueueStorageCommand("requestComprehensionReview",
        [=, this]() { return requestComprehensionReviewOperation(expectedVersion); });
}

quint64 HostController::resolveComprehensionFinding(const QString& findingId, const QString& resolution,
                                                    quint64 expectedVersion) {
    if (expectedVersion != stateVersion()) {
        setLastError(core::makeError(core::ErrorCode::StaleVersion,
                                     "The prompt changed. Review the current state and try again."));
        return 0;
    }
    if (m_reviewState != ReviewState::Ready || !reviewBindingMatchesCurrent()) {
        setLastError(core::makeError(core::ErrorCode::PreconditionFailed,
                                     "There is no current review finding to resolve."));
        return 0;
    }
    const auto res = resolution.trimmed().toLower();
    if (res != QStringLiteral("acknowledge") && res != QStringLiteral("confirm_intent") &&
        res != QStringLiteral("return_to_interview") && res != QStringLiteral("keep_original")) {
        setLastError(core::makeError(core::ErrorCode::InvalidInput, "Unknown review resolution."));
        return 0;
    }
    const bridge::ComprehensionFinding* finding = nullptr;
    for (const auto& candidate : m_reviewResult.findings) {
        if (candidate.id == ss(findingId)) { finding = &candidate; break; }
    }
    if (finding == nullptr) {
        setLastError(core::makeError(core::ErrorCode::InvalidInput,
                                     "That review finding does not belong to the current review."));
        return 0;
    }
    if (res == QStringLiteral("confirm_intent") && !canConfirmCandidateWording(*finding)) return 0;
    if (res == QStringLiteral("acknowledge") && findingRequiresInterview(*finding)) {
        setLastError(core::makeError(core::ErrorCode::ValidationFailed,
                                     "This finding needs your decision. Return it to the interview or keep the original prompt."));
        return 0;
    }
    enqueueStorageCommand("resolveComprehensionFinding",
        [=, this]() { return resolveComprehensionFindingOperation(findingId, resolution, expectedVersion); });
    return expectedVersion + 1;
}

HostController::ParsedCommand HostController::parseCommandPrefix(QString text) {
    const QString trimmed = text.trimmed();
    if (!trimmed.startsWith(QLatin1Char('/'))) {
        return {AssistanceMode::Auto, text, false};
    }

    int firstSpace = -1;
    for (int i = 0; i < trimmed.size(); ++i) {
        if (trimmed.at(i).isSpace()) {
            firstSpace = i;
            break;
        }
    }

    const QString cmd = (firstSpace == -1) ? trimmed.toLower() : trimmed.left(firstSpace).toLower();
    const QString rest = (firstSpace == -1) ? QString{} : trimmed.mid(firstSpace).trimmed();

    if (cmd == QStringLiteral("/auto")) {
        return {AssistanceMode::Auto, rest, true};
    } else if (cmd == QStringLiteral("/tech")) {
        return {AssistanceMode::Tech, rest, true};
    } else if (cmd == QStringLiteral("/create")) {
        return {AssistanceMode::Create, rest, true};
    } else if (cmd == QStringLiteral("/strict")) {
        return {AssistanceMode::Strict, rest, true};
    } else if (cmd == QStringLiteral("/inspect")) {
        return {AssistanceMode::Inspect, rest, true};
    } else if (cmd == QStringLiteral("/quiet")) {
        return {AssistanceMode::Quiet, rest, true};
    }

    return {AssistanceMode::Auto, text, false};
}

void HostController::applyAssistanceMode(AssistanceMode mode) {
    if (m_assistanceMode == mode) return;
    m_assistanceMode = mode;
    switch (mode) {
        case AssistanceMode::Auto:
        case AssistanceMode::Tech:
        case AssistanceMode::Create:
        case AssistanceMode::Inspect:
        case AssistanceMode::Quiet:
            m_reviewTrigger = analysis::ReviewTrigger::Automatic;
            break;
        case AssistanceMode::Strict:
            m_reviewTrigger = analysis::ReviewTrigger::ExplicitReview;
            break;
    }
    emit assistanceModeChanged();
}

QString HostController::assistanceMode() const {
    switch (m_assistanceMode) {
        case AssistanceMode::Auto: return QStringLiteral("auto");
        case AssistanceMode::Tech: return QStringLiteral("tech");
        case AssistanceMode::Create: return QStringLiteral("create");
        case AssistanceMode::Strict: return QStringLiteral("strict");
        case AssistanceMode::Inspect: return QStringLiteral("inspect");
        case AssistanceMode::Quiet: return QStringLiteral("quiet");
    }
    return QStringLiteral("auto");
}

bool HostController::isInspectMode() const noexcept {
    return m_assistanceMode == AssistanceMode::Inspect;
}

void HostController::setAssistanceMode(const QString& mode) {
    const QString lower = mode.trimmed().toLower();
    if (lower == QStringLiteral("auto") || lower == QStringLiteral("/auto")) {
        applyAssistanceMode(AssistanceMode::Auto);
    } else if (lower == QStringLiteral("tech") || lower == QStringLiteral("/tech")) {
        applyAssistanceMode(AssistanceMode::Tech);
    } else if (lower == QStringLiteral("create") || lower == QStringLiteral("/create")) {
        applyAssistanceMode(AssistanceMode::Create);
    } else if (lower == QStringLiteral("strict") || lower == QStringLiteral("/strict")) {
        applyAssistanceMode(AssistanceMode::Strict);
    } else if (lower == QStringLiteral("inspect") || lower == QStringLiteral("/inspect")) {
        applyAssistanceMode(AssistanceMode::Inspect);
    } else if (lower == QStringLiteral("quiet") || lower == QStringLiteral("/quiet")) {
        applyAssistanceMode(AssistanceMode::Quiet);
    }
}

QString HostController::auditOutcome() const {
    if (!m_lastAuditResult.has_value()) return QString{};
    switch (m_lastAuditResult->outcome) {
        case core::ValidationOutcome::Pass: return QStringLiteral("Pass");
        case core::ValidationOutcome::Fail: return QStringLiteral("Fail");
        case core::ValidationOutcome::Unresolved: return QStringLiteral("Unresolved");
    }
    return QString{};
}

QString HostController::auditExecutionStatus() const {
    if (!m_lastAuditResult.has_value()) return QString{};
    return QString::fromUtf8(core::toString(m_lastAuditResult->executionStatus));
}

QVariantList HostController::auditViolations() const {
    QVariantList list;
    if (!m_lastAuditResult.has_value()) return list;
    for (const auto& v : m_lastAuditResult->violations) {
        QVariantMap map;
        map.insert(QStringLiteral("constraintId"), qs(v.constraintId));
        map.insert(QStringLiteral("explanation"), qs(v.explanation));
        list.append(map);
    }
    return list;
}

QVariantList HostController::auditUnresolved() const {
    QVariantList list;
    if (!m_lastAuditResult.has_value()) return list;
    for (const auto& u : m_lastAuditResult->unresolved) {
        QVariantMap map;
        map.insert(QStringLiteral("constraintId"), qs(u.constraintId));
        map.insert(QStringLiteral("explanation"), qs(u.explanation));
        list.append(map);
    }
    return list;
}

std::vector<core::ScopedConstraint> HostController::extractSessionConstraints(
    std::string_view originalPrompt,
    const core::intent::ConfirmedIntent& intent) {

    std::vector<core::ScopedConstraint> constraints;

    for (const auto& item : intent.items) {
        core::ScopedConstraint c;
        c.id = "intent_" + item.itemId;
        c.origin = core::Origin::User;
        c.enforcement = core::Enforcement::Hard;

        switch (item.kind) {
            case core::intent::IntentResolutionKind::SelectedAlternative:
            case core::intent::IntentResolutionKind::CustomText: {
                c.derivation = core::Derivation::Direct;
                c.polarity = core::Polarity::Require;
                c.aspect = "user_choice";
                c.rule = (item.kind == core::intent::IntentResolutionKind::CustomText)
                             ? item.customText : item.chosenValue;
                constraints.push_back(std::move(c));
                break;
            }
            case core::intent::IntentResolutionKind::AcceptedAssumption: {
                c.derivation = core::Derivation::Entailed;
                c.polarity = core::Polarity::Require;
                c.aspect = "assumption";
                c.rule = "Accepted assumption: " + item.itemId;
                constraints.push_back(std::move(c));
                break;
            }
            case core::intent::IntentResolutionKind::DeclinedAssumption: {
                c.derivation = core::Derivation::Direct;
                c.polarity = core::Polarity::Prohibit;
                c.aspect = "assumption";
                c.rule = "Do not assume: " + item.itemId;
                constraints.push_back(std::move(c));
                break;
            }
            case core::intent::IntentResolutionKind::UseOriginal: {
                c.derivation = core::Derivation::Direct;
                c.polarity = core::Polarity::Require;
                c.aspect = "original_wording";
                c.rule = "Keep original wording";
                constraints.push_back(std::move(c));
                break;
            }
        }
    }

    const std::string promptStr(originalPrompt);
    std::vector<std::string> clauses;
    std::string current;
    for (std::size_t i = 0; i < promptStr.size(); ++i) {
        const char ch = promptStr[i];
        if (ch == '.' || ch == ';' || ch == '\n' || ch == '\r') {
            if (!current.empty()) {
                clauses.push_back(std::move(current));
                current.clear();
            }
        } else if (ch == ',' && i + 4 < promptStr.size() &&
                   promptStr.substr(i + 1, 4) == " but") {
            if (!current.empty()) {
                clauses.push_back(std::move(current));
                current.clear();
            }
            i += 4;
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        clauses.push_back(std::move(current));
    }

    int prohibitIdx = 0;
    for (auto& clause : clauses) {
        const auto start = clause.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        const auto end = clause.find_last_not_of(" \t");
        std::string trimmed = clause.substr(start, end - start + 1);

        if (trimmed.starts_with("but ")) trimmed = trimmed.substr(4);
        else if (trimmed.starts_with("and ")) trimmed = trimmed.substr(4);

        std::string lower;
        lower.reserve(trimmed.size());
        for (char c : trimmed) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        static const std::string_view kNegPrefixes[] = {
            "do not ", "don't ", "dont ", "without ", "never ", "leave untouched", "leaving unchanged", "keep intact"
        };
        bool isNegative = false;
        for (const auto& marker : kNegPrefixes) {
            std::size_t position = 0;
            while ((position = lower.find(marker, position)) != std::string::npos) {
                const auto after = position + marker.size();
                // Negative opinions describe the problem or preference; they are
                // not a prohibition on mutating every noun in that sentence.
                // Keep scanning so a later "do not change ..." is still protected.
                static const std::regex opinionSubject(R"(\b(?:i|we)\s+(?:(?:really|especially)\s+)?$)");
                const bool negativeOpinion = std::regex_search(lower.substr(0, position), opinionSubject) &&
                    (marker == "do not " || marker == "don't " || marker == "dont ") &&
                    (std::string_view(lower).substr(after).starts_with("like ") ||
                     std::string_view(lower).substr(after).starts_with("love ") ||
                     std::string_view(lower).substr(after).starts_with("enjoy "));
                if (negativeOpinion) { position = after; continue; }
                isNegative = true;
                break;
            }
            if (isNegative) break;
        }

        if (isNegative) {
            core::ScopedConstraint sc;
            sc.id = "prohibit_boundary_" + std::to_string(++prohibitIdx);
            sc.rule = trimmed;
            sc.origin = core::Origin::User;
            sc.derivation = core::Derivation::Direct;
            sc.enforcement = core::Enforcement::Hard;
            sc.polarity = core::Polarity::Prohibit;
            sc.aspect = "boundary";
            constraints.push_back(std::move(sc));
        }
    }

    std::string lowerPrompt;
    lowerPrompt.reserve(promptStr.size());
    for (char c : promptStr) lowerPrompt.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lowerPrompt.find("layer 3") != std::string::npos || lowerPrompt.find("packet forwarding") != std::string::npos) {
        core::ScopedConstraint sc;
        sc.id = "domain_l3_forwarding";
        sc.rule = "Destination forwarding follows longest prefix match";
        sc.origin = core::Origin::Domain;
        sc.derivation = core::Derivation::Entailed;
        sc.enforcement = core::Enforcement::Conditional;
        sc.polarity = core::Polarity::Require;
        sc.aspect = "networking";
        sc.appliesWhen = "depicts_ipv4_forwarding";
        sc.authorityRef = "RFC 1812";
        constraints.push_back(std::move(sc));
    }

    return constraints;
}

quint64 HostController::analyzePrompt(QString text, quint64 expectedVersion) {
    return enqueueStorageCommand("analyzePrompt", [=, this]() { return analyzePromptOperation(text, expectedVersion); });
}

quint64 HostController::reviewPrompt(QString text, quint64 expectedVersion) {
    return enqueueStorageCommand("reviewPrompt", [=, this]() {
        return analyzePromptOperation(text, expectedVersion, analysis::ReviewTrigger::ExplicitReview);
    });
}

quint64 HostController::cancelAnalysis(quint64 expectedVersion) {
    return enqueueStorageCommand("cancelAnalysis", [=, this]() { return cancelAnalysisOperation(expectedVersion); });
}

quint64 HostController::hardBypass(QString text, quint64 expectedVersion) {
    return enqueueStorageCommand("hardBypass", [=, this]() { return hardBypassOperation(text, expectedVersion); });
}

quint64 HostController::commitPrompt(QString text, quint64 expectedVersion) {
    return enqueueStorageCommand("commitPrompt", [=, this]() { return commitPromptOperation(text, expectedVersion); });
}

AsyncTask<void> HostController::initStorage(QString dbPathOverride) {
    m_databasePath = dbPathOverride;
    if (m_databasePath.isEmpty()) m_databasePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/history.db");
    ++m_storageEpoch;
    m_repository.reset();
    m_storage = std::make_unique<StorageService>(this);
    auto status = co_await m_storage->open(m_databasePath, true);
    if (!status.ok()) { failStorage(status.error()); co_return; }
    m_repository = std::make_unique<AsyncRepository>(*m_storage);
    m_repository->guardFactory = [this](bool durableMutation) -> AsyncRepository::Guard {
        const QPointer<HostController> owner(this);
        const auto version = stateVersion(); const auto serial = m_liveEditSerial; const auto session = currentSessionId();
        const auto pipeline = m_pipelineSerial; const auto epoch = m_storageEpoch;
        return [owner, version, serial, session, pipeline, epoch, durableMutation](bool success, const core::Error* error) -> Status {
            if (!owner) throw StorageCancelled{};
            if (owner->m_storageEpoch != epoch) return makeError(ErrorCode::StaleVersion, "History recovery superseded this result");
            if (error && (error->code == ErrorCode::DbError || (durableMutation && error->code == ErrorCode::StaleVersion))) owner->failStorage(*error);
            if (owner->m_storageFault) return storageUnavailable();
            if (owner->stateVersion() != version || owner->m_liveEditSerial != serial || owner->currentSessionId() != session || owner->m_pipelineSerial != pipeline) {
                auto stale = makeError(ErrorCode::StaleVersion, "The prompt changed while history was saving. Your current draft is preserved.");
                if (success && durableMutation) owner->failStorage(stale);
                return stale;
            }
            return Status::success();
        };
    };
    const QPointer<HostController> storageOwner(this);
    const auto storageEpoch = m_storageEpoch;
    m_repository->errorHandler = [storageOwner, storageEpoch](const core::Error& error) {
        if (storageOwner && storageOwner->m_storageEpoch == storageEpoch) storageOwner->failStorage(error);
    };
    m_draftDebounce.disconnect(this); m_draftMaxWait.disconnect(this);
    m_draftDebounce.setSingleShot(true); m_draftDebounce.setInterval(150);
    m_draftMaxWait.setSingleShot(true); m_draftMaxWait.setInterval(1000);
    connect(&m_draftDebounce, &QTimer::timeout, this, [this]() { flushDraftEdits(); });
    connect(&m_draftMaxWait, &QTimer::timeout, this, [this]() { flushDraftEdits(); });
    emit storageChanged();
    refreshStorageStats();
}

void HostController::failStorage(const core::Error& error) {
    m_lastInternalDiagnostic = qs(error.message).left(1024);
    const bool firstFailure = !m_storageFault;
    m_storageFault = true;
    if (firstFailure && m_repository && m_revisionState)
        m_repository->invalidateApprovals(core::SessionId(currentSessionId()));
    if (m_pendingDraft) {
        auto requests = std::move(m_pendingDraft->requests); m_pendingDraft->requests.clear();
        finishDraftRequests(std::move(requests), storageUnavailable());
    }
    m_databaseStats = QStringLiteral("History couldn't be saved. Your draft is preserved. Retry history before analyzing or approving.");
    m_draftDebounce.stop(); m_draftMaxWait.stop();
    invalidateDownstreamPipeline();
    emit storageChanged(); emit storageBusyChanged();
    finishQuitWhenRuntimesStopped();
}

quint64 HostController::enqueueStorageCommand(const char* name, std::function<AsyncTask<Status>()> operation, bool modelCompletion) {
    if ((!isStorageAvailable() && std::string_view(name) != "retryStorage") || m_hostState == HostState::Quitting || m_storageCommands.size() + static_cast<size_t>(m_commandRunning) >= 128 ||
        m_nextStorageRequest == core::RevisionState::kMaxStateVersion) {
        setLastError(QStringLiteral("History is unavailable or busy. Your draft is preserved; retry explicitly.")); return 0;
    }
    const auto id = ++m_nextStorageRequest;
    m_storageCommands.push_back(std::make_shared<StorageCommand>(StorageCommand{id, m_liveEditSerial, QLatin1String(name), std::move(operation), modelCompletion}));
    emit storageBusyChanged(); QTimer::singleShot(0, this, [this]() { pumpStorageCommands(); }); return id;
}
void HostController::queueModelCompletion(const char* name, std::function<AsyncTask<void>()> completion) {
    // SQL is serialized by StorageService; publishing its corresponding controller version
    // must also finish before a user command or another model continuation starts a write.
    const auto accepted = enqueueStorageCommand(name, [completion = std::move(completion)]() -> AsyncTask<Status> {
        co_await completion();
        co_return Status::success();
    }, true);
    if (!accepted && m_hostState != HostState::Quitting) {
        invalidateDownstreamPipeline();
        setAnalysisStatus(AnalysisStatus::Failed,
            QStringLiteral("Roclint couldn't save this result. Your draft is preserved; retry explicitly."));
    }
}

void HostController::pumpStorageCommands() {
    if (m_commandRunning || m_storageCommands.empty()) return;
    auto command = m_storageCommands.front(); m_storageCommands.pop_front(); m_commandRunning = true;
    runStorageCommand(std::move(command));
}
AsyncTask<void> HostController::runStorageCommand(std::shared_ptr<StorageCommand> command) {
    Status status = Status::success();
    try {
        const bool recovery = command->name == QLatin1String("retryStorage");
        if (!recovery) status = co_await flushDraftEdits();
        if (status.ok() && !command->modelCompletion && command->editSerial != m_liveEditSerial)
            status = makeError(ErrorCode::StaleVersion, "The draft changed while this action was queued. Your current text is preserved.");
        if (status.ok() && (!m_storageFault || recovery) && m_hostState != HostState::Quitting) status = co_await command->operation();
        else if (status.ok()) status = storageUnavailable();
    } catch (const StorageCancelled&) { co_return; }
    catch (...) { status = makeError(ErrorCode::Internal, "The action couldn't finish. Your draft is preserved; retry explicitly."); }
    // Internal model continuations validate their own immutable pipeline tickets. They must
    // not clear a user's command error or publish a completion for an unrequested UI command.
    if (!command->modelCompletion) finishStorageCommand(command->id, command->name, status);
    else if (!status.ok() && status.error().code != ErrorCode::StaleVersion) setLastError(status.error());
    m_commandRunning = false; emit storageBusyChanged();
    QTimer::singleShot(0, this, [this]() { pumpStorageCommands(); finishQuitWhenRuntimesStopped(); });
}
void HostController::finishStorageCommand(quint64 id, const QString& name, const Status& status) {
    if (id >= m_latestStorageFeedback) {
        m_latestStorageFeedback = id;
        if (!status.ok()) setLastError(status.error()); else clearLastError();
    }
    m_commandResults.insert(id, status.ok());
    if (m_commandResults.size() > 512) {
        auto smallest = m_commandResults.keyBegin(); quint64 oldest = *smallest;
        for (auto it = m_commandResults.keyBegin(); it != m_commandResults.keyEnd(); ++it) oldest = std::min(oldest, *it);
        m_commandResults.remove(oldest);
    }
    emit storageCommandFinished(id, name, status.ok(), status.ok() ? QString{} : QString::fromUtf8(core::toString(status.error().code)),
        status.ok() ? QString{} : plainError(status.error()));
}
quint64 HostController::noteDraftEdited(quint64 expectedVersion) {
    return noteDraftEditedInternal(expectedVersion, true);
}
quint64 HostController::noteDraftEditedInternal(quint64 expectedVersion, bool editedInHud) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) { setLastError(status.error()); return 0; }
    if (m_hostState == HostState::Quitting || m_nextStorageRequest == core::RevisionState::kMaxStateVersion) return 0;
    // Leave room for the candidate transaction's version if it commits.
    if (m_candidateWriteInFlight && expectedVersion >= core::RevisionState::kMaxStateVersion - 1) return 0;
    const auto id = ++m_nextStorageRequest; ++m_liveEditSerial;
    m_followAmbientDraft = false;
    if (editedInHud) m_widgetDraftEdited = true;
    if (!m_revisionState) { invalidateDownstreamPipeline(); finishStorageCommand(id, "noteDraftEdited", Status::success()); return id; }
    auto next = *m_revisionState;
    if (auto status = next.noteDraftEdit(expectedVersion); !status.ok()) { finishStorageCommand(id, "noteDraftEdited", status); return 0; }
    *m_revisionState = next;
    invalidateDownstreamPipeline(); emit stateVersionChanged();
    if (!m_pendingDraft) m_pendingDraft = DraftBatch{next, expectedVersion, {}};
    m_pendingDraft->next = next;
    auto& requests = m_pendingDraft->requests;
    if (!requests.empty() && requests.back().second + 1 == id) requests.back().second = id;
    else requests.emplace_back(id, id);
    if (m_storageFault) {
        finishStorageCommand(id, "noteDraftEdited", storageUnavailable()); m_pendingDraft->requests.clear();
    } else {
        m_draftDebounce.start(); if (!m_draftMaxWait.isActive()) m_draftMaxWait.start();
    }
    emit storageBusyChanged(); return id;
}
void HostController::finishDraftRequests(std::vector<std::pair<quint64, quint64>> requests, Status status) {
    if (requests.empty()) return;
    auto completion = std::make_shared<DraftCompletion>(DraftCompletion{std::move(requests), 0, 0, std::move(status)});
    completion->next = completion->ranges.front().first;
    ++m_draftNotifications; emit storageBusyChanged();
    publishDraftCompletions(std::move(completion));
}
void HostController::publishDraftCompletions(std::shared_ptr<DraftCompletion> completion) {
    // Large AX bursts use bounded memory and yield between batches of completion signals.
    for (int budget = 0; budget < 64 && completion->range < completion->ranges.size(); ++budget) {
        const auto id = completion->next++;
        if (id == completion->ranges[completion->range].second) {
            ++completion->range;
            if (completion->range < completion->ranges.size()) completion->next = completion->ranges[completion->range].first;
        }
        finishStorageCommand(id, "noteDraftEdited", completion->status);
    }
    if (completion->range == completion->ranges.size()) {
        --m_draftNotifications; emit storageBusyChanged(); finishQuitWhenRuntimesStopped();
    } else QTimer::singleShot(0, this, [this, completion]() { publishDraftCompletions(completion); });
}

AsyncTask<Status> HostController::flushDraftEdits() {
    if (m_candidateWriteInFlight) co_return Status::success();
    m_draftDebounce.stop(); m_draftMaxWait.stop();
    while (!m_storageFault) {
        if (m_draftFlushTask && !m_draftFlushTask->done()) {
            auto active = *m_draftFlushTask;
            auto status = co_await active; if (!status.ok()) co_return status;
        } else if (m_pendingDraft) {
            auto batch = std::move(*m_pendingDraft); m_pendingDraft.reset();
            m_draftFlushTask = writeDraftBatch(std::move(batch));
        } else co_return Status::success();
    }
    co_return storageUnavailable();
}
AsyncTask<Status> HostController::writeDraftBatch(DraftBatch batch) {
    m_draftWriting = true;
    auto status = co_await m_storage->call(&storage::PromptRepository::advanceDraftStateRange, batch.next, batch.expectedVersion, nowMs());
    m_draftWriting = false;
    if (!status.ok()) failStorage(status.error());
    finishDraftRequests(std::move(batch.requests), status);
    emit storageBusyChanged(); finishQuitWhenRuntimesStopped();
    co_return status;
}

int HostController::sessionCount() const { return static_cast<int>(m_recentSessions.size()); }
QVariantList HostController::getRecentSessions() { return m_recentSessions; }
QVariantList HostController::getSessionHistory(const QString& sessionId) { return m_historyCache.value(sessionId); }
void HostController::refreshStorageStats() {
    if (!m_repository || m_storageFault) { emit storageChanged(); return; }
    if (m_historyRefreshRunning) { m_historyRefreshAgain = true; return; }
    loadHistoryCache();
}
AsyncTask<void> HostController::loadHistoryCache() {
    m_historyRefreshRunning = true;
    do {
        m_historyRefreshAgain = false;
        const auto epoch = m_storageEpoch;
        auto snapshot = co_await m_storage->history();
        if (epoch != m_storageEpoch) { m_historyRefreshRunning = false; refreshStorageStats(); co_return; }
        if (!snapshot.ok()) { m_historyRefreshRunning = false; failStorage(snapshot.error()); co_return; }
        m_recentSessions.clear(); m_historyCache.clear(); size_t count = 0;
        for (const auto& entry : snapshot.value().sessions) {
            const auto& session = entry.record; QVariantList history;
            for (const auto& rec : entry.revisions) {
                const auto& rev = rec.revision;
                history.append(QVariantMap{{"revisionId", qs(rev.revisionId().value())}, {"sessionId", qs(rev.sessionId().value())},
                    {"parentRevisionId", rev.parentRevisionId() ? qs(rev.parentRevisionId()->value()) : QString{}},
                    {"text", qs(rev.text())}, {"sequence", static_cast<qulonglong>(rec.sequence)},
                    {"origin", QString::fromUtf8(storage::toString(rec.origin))}, {"createdAtMs", static_cast<qlonglong>(rev.createdAtMs())},
                    {"formattedTime", QDateTime::fromMSecsSinceEpoch(rev.createdAtMs()).toString("HH:mm:ss")}});
            }
            count += entry.revisions.size(); m_historyCache.insert(qs(session.id.value()), history);
            const auto* latest = entry.revisions.empty() ? nullptr : &entry.revisions.back().revision;
            m_recentSessions.append(QVariantMap{{"sessionId", qs(session.id.value())}, {"createdAtMs", static_cast<qlonglong>(session.createdAtMs)},
                {"formattedTime", QDateTime::fromMSecsSinceEpoch(session.createdAtMs).toString("yyyy-MM-dd HH:mm:ss")},
                {"revisionCount", static_cast<int>(entry.revisions.size())}, {"latestPrompt", latest ? qs(latest->text()) : QString{}},
                {"latestRevisionId", latest ? qs(latest->revisionId().value()) : QString{}}});
        }
        m_databaseStats = QStringLiteral("%1 saved session(s), %2 revision(s), schema version %3").arg(m_recentSessions.size()).arg(count).arg(snapshot.value().schema);
        emit storageChanged(); emit sessionListChanged();
    } while (m_historyRefreshAgain && !m_storageFault);
    m_historyRefreshRunning = false;
    finishQuitWhenRuntimesStopped();
}
quint64 HostController::retryStorage(const QString& draftText, quint64 expectedVersion) {
    if (!m_storageFault || m_commandRunning || m_draftWriting || !m_storageCommands.empty()) return 0;
    return enqueueStorageCommand("retryStorage", [=, this]() { return retryStorageOperation(draftText, expectedVersion); });
}
AsyncTask<Status> HostController::retryStorageOperation(QString draftText, quint64 expectedVersion) {
    if (auto status = checkExpectedVersion(expectedVersion); !status.ok()) co_return status;
    m_pendingDraft.reset();
    const auto session = currentSessionId();
    const auto editSerial = m_liveEditSerial;
    const auto previousVersion = stateVersion();
    // Recovery remains unavailable to other commands and draft flushes until reconciliation commits.
    co_await initStorage(m_databasePath);
    if (!m_repository) co_return storageUnavailable();
    if (editSerial != m_liveEditSerial) co_return makeError(ErrorCode::StaleVersion, "Draft changed during recovery; retry with the current text");
    if (!session.empty()) {
        auto restored = co_await m_storage->call(&storage::PromptRepository::restoreSession, core::SessionId(session));
        if (!restored.ok()) { failStorage(restored.error()); co_return restored.error(); }
        const auto& r = restored.value();
        const auto largestVersion = std::max<std::uint64_t>(previousVersion, r.session.stateVersion);
        if (largestVersion >= core::RevisionState::kMaxStateVersion)
            co_return makeError(ErrorCode::InvalidInput, "The session version is exhausted");
        const auto target = largestVersion + 1;
        const auto delta = target - r.session.stateVersion;
        if (r.session.draftEditGeneration > core::RevisionState::kMaxStateVersion - delta)
            co_return makeError(ErrorCode::InvalidInput, "The draft generation is exhausted");
        core::RevisionState reconciled(r.latestRevision.revision, r.session.highestRevisionSeq, target - 1,
            r.session.draftEditGeneration + delta - 1, r.session.proposalEditGeneration);
        if (auto status = reconciled.noteDraftEdit(target - 1); !status.ok()) co_return status;
        auto saved = co_await m_storage->call(&storage::PromptRepository::advanceDraftStateRange,
            reconciled, r.session.stateVersion, nowMs());
        if (!saved.ok()) { failStorage(saved.error()); co_return saved; }
        if (editSerial != m_liveEditSerial) co_return makeError(ErrorCode::StaleVersion, "Draft changed during recovery; retry with the current text");
        m_revisionState = std::make_unique<core::RevisionState>(std::move(reconciled));
    }
    m_pendingDraft.reset();
    m_storageFault = false;
    m_capturedDraftText = draftText; emit capturedDraftTextChanged(); emit draftCaptured(draftText); emit stateVersionChanged();
    refreshStorageStats(); emit storageChanged();
    co_return Status::success();
}

AsyncTask<void> HostController::resumeDiscovery(bool allowReconnect) {
    auto status = co_await startDiscovery(allowReconnect);
    if (!status.ok()) setLastError(status.error());
}
#ifdef ROCLINT_ENABLE_TEST_MOCKS
bool HostController::waitForCommandForTesting(quint64 id, int timeoutMs) {
    if (!id) return false;
    QElapsedTimer clock; clock.start();
    while ((!m_commandResults.contains(id) || m_historyRefreshRunning) && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return m_commandResults.value(id, false);
}
bool HostController::waitForStorageForTesting(int timeoutMs) {
    QElapsedTimer clock; clock.start();
    while ((!m_repository && !m_storageFault) || m_historyRefreshRunning) {
        if (clock.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return true;
}
#endif
} // namespace roclint::host
