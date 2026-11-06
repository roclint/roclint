#pragma once

#include <QObject>
#include <QDeadlineTimer>
#include <QProcess>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "HostContext.hpp"
#include "CodebaseWatcher.hpp"
#include "AsyncRepository.hpp"
#include "roclint/storage/CodebaseIndex.hpp"
#include <deque>
#include <QHash>
#include <QPointer>
#include "IAccessibilityBridge.hpp"
#include "OllamaClient.hpp"
#include "JuliaRuntime.hpp"
#include "NativeGraniteRuntime.hpp"
#include "GraniteModelInstaller.hpp"
#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"
#include "roclint/review/ApprovalRecord.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "roclint/storage/Database.hpp"
#include "roclint/storage/PromptRepository.hpp"

namespace roclint::host {

class MemoryPressureMonitor;

enum class HostState {
    Starting,
    Resident,
    Quitting
};

enum class CardVisibility {
    Hidden,
    Visible,
    Focused
};

enum class AnalysisStatus {
    Idle,
    Analyzing,
    Ready,
    Failed
};

enum class ProposalUiStatus {
    None,
    Generating,
    ReadyForReview,
    NeedsClarification,
    ValidationFailed,
    Failed
};

// Where the card's prompt came from, which decides how approved text is delivered.
enum class CaptureSource {
    Widget, // Typed or pasted into the card; delivered to the clipboard
    Ambient // Read from another app's focused field; written back into that field
};

// One grounded target as presented to the host/HUD. Fields are display metadata;
// the proposal hash still binds only file hashes, byte spans and signatures.
struct GroundedEvidenceView {
    std::string target{};
    std::string qualifiedTarget{};
    std::string signature{};
    std::string filePath{};
    std::string fileHash{}; // internal freshness binding, not rendered by the HUD
    std::uint32_t line{0};
    std::string status{};      // VERIFIED, AMBIGUOUS, NOT_FOUND_IN_SCOPE, ...
    std::string matchReason{};
    bool complete{true};
    bool hashValidated{true};
    std::vector<std::string> candidates{};

    bool operator==(const GroundedEvidenceView&) const = default;
};

// Thread-safe inputs for grounding. A worker opens its own read-only index
// connection; the host must not touch SQLite or source files on the UI thread.
struct GroundingInputs {
    std::string prompt{};
    std::filesystem::path indexPath{};
    std::filesystem::path workspaceRoot{};
    std::string activeFile{};
    std::string workspaceIdentity{};
    std::uint64_t workspaceEpoch{0};
    bool consented{false};
    bool ready{false};
};

// Grounded intents plus the hash of the exact verified AST evidence used to
// render them. The hash is empty when no intent was verified or when the
// selected files changed during enrichment.
struct GroundedPromptContext {
    std::string xml{};
    std::string astSnapshotHash{};
    std::vector<GroundedEvidenceView> evidence{};
    std::string prompt{};      // exact prompt used for revalidation
    std::string activeFile{};  // exact active-file hint used for revalidation
    std::uint64_t workspaceEpoch{0};
    std::vector<std::string> stalePaths{}; // rendered files whose saved bytes changed
    bool available{false};   // consented index was ready and queried
    bool complete{false};    // every candidate set used for the decision was complete
    bool validated{false};   // file hashes and snapshot generation stayed coherent
    std::uint64_t snapshotGeneration{0};
};

[[nodiscard]] GroundedPromptContext computeGroundedPromptContext(const GroundingInputs& inputs);

// The single owner of authoritative prompt state for the desktop host. QML only forwards user
// gestures; every mutation goes through a version-checked command below.
class HostController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isCardVisible READ isCardVisible NOTIFY cardVisibilityChanged)
    Q_PROPERTY(bool isDashboardVisible READ isDashboardVisible NOTIFY dashboardVisibilityChanged)
    Q_PROPERTY(QString hostStateString READ hostStateString NOTIFY hostStateChanged)
    Q_PROPERTY(QString cardVisibilityString READ cardVisibilityString NOTIFY cardVisibilityChanged)
    Q_PROPERTY(quint64 stateVersion READ stateVersion NOTIFY stateVersionChanged)
    Q_PROPERTY(QString currentSessionId READ currentSessionIdString NOTIFY stateVersionChanged)
    Q_PROPERTY(QString currentRevisionId READ currentRevisionId NOTIFY stateVersionChanged)
    Q_PROPERTY(QString currentRevisionText READ currentRevisionText NOTIFY stateVersionChanged)
    Q_PROPERTY(bool isDraftDirty READ isDraftDirty NOTIFY stateVersionChanged)
    Q_PROPERTY(QString lastCopiedText READ lastCopiedText NOTIFY lastCopiedTextChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString lastErrorCode READ lastErrorCode NOTIFY lastErrorChanged)
    Q_PROPERTY(QString shortcutWarning READ shortcutWarning NOTIFY shortcutWarningChanged)

    Q_PROPERTY(bool hasHostContext READ hasHostContext NOTIFY hostContextChanged)
    Q_PROPERTY(QString hostContextCategory READ hostContextCategory NOTIFY hostContextChanged)
    Q_PROPERTY(QString hostContextDisplayName READ hostContextDisplayName NOTIFY hostContextChanged)
    Q_PROPERTY(QString capturedDraftText READ capturedDraftText NOTIFY capturedDraftTextChanged)

    Q_PROPERTY(QString databasePath READ databasePath NOTIFY storageChanged)
    Q_PROPERTY(QString databaseStats READ databaseStats NOTIFY storageChanged)
    Q_PROPERTY(bool storageBusy READ storageBusy NOTIFY storageBusyChanged)
    Q_PROPERTY(bool storageNeedsRecovery READ storageNeedsRecovery NOTIFY storageChanged)
    Q_PROPERTY(bool isStorageAvailable READ isStorageAvailable NOTIFY storageChanged)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionListChanged)

    Q_PROPERTY(QString analysisStatus READ analysisStatus NOTIFY analysisStatusChanged)
    Q_PROPERTY(QString analysisMessage READ analysisMessage NOTIFY analysisStatusChanged)
    Q_PROPERTY(bool isAnalyzing READ isAnalyzing NOTIFY analysisStatusChanged)
    Q_PROPERTY(QString reviewOutcome READ reviewOutcome NOTIFY interventionChanged)
    Q_PROPERTY(QString reviewDiagnostics READ reviewDiagnostics NOTIFY interventionChanged)
    Q_PROPERTY(bool isOptionalHelp READ isOptionalHelp NOTIFY interventionChanged)

    Q_PROPERTY(QString interventionVerdict READ interventionVerdict NOTIFY interventionChanged)
    Q_PROPERTY(QString interventionReason READ interventionReason NOTIFY interventionChanged)
    Q_PROPERTY(bool isClarifying READ isClarifying NOTIFY interventionChanged)
    Q_PROPERTY(bool isManual READ isManual NOTIFY interventionChanged)
    Q_PROPERTY(QString activeItemId READ activeItemId NOTIFY interventionChanged)
    Q_PROPERTY(QString activeItemKind READ activeItemKind NOTIFY interventionChanged)
    Q_PROPERTY(QString activeQuestion READ activeQuestion NOTIFY interventionChanged)
    Q_PROPERTY(QString activeDimension READ activeDimension NOTIFY interventionChanged)
    Q_PROPERTY(QVariantList activeAlternatives READ activeAlternatives NOTIFY interventionChanged)
    Q_PROPERTY(int roundsCompleted READ roundsCompleted NOTIFY interventionChanged)
    Q_PROPERTY(int remainingBudget READ remainingBudget NOTIFY interventionChanged)
    Q_PROPERTY(bool isAmbiguityScored READ isAmbiguityScored NOTIFY interventionChanged)
    Q_PROPERTY(double ambiguityScore READ ambiguityScore NOTIFY interventionChanged)
    Q_PROPERTY(QVariantList assumptionReviews READ assumptionReviews NOTIFY interventionChanged)
    Q_PROPERTY(QString rankingStatus READ rankingStatus NOTIFY interventionChanged)
    Q_PROPERTY(QString diagnosticMessage READ diagnosticMessage NOTIFY interventionChanged)
    Q_PROPERTY(QString recommendedOption READ recommendedOption NOTIFY recommendedOptionChanged)
    Q_PROPERTY(double recommendedConfidence READ recommendedConfidence NOTIFY recommendedOptionChanged)

    Q_PROPERTY(bool hasConfirmedIntent READ hasConfirmedIntent NOTIFY intentChanged)
    Q_PROPERTY(bool isIntentStale READ isIntentStale NOTIFY intentChanged)
    Q_PROPERTY(QString confirmedIntentSummary READ confirmedIntentSummary NOTIFY intentChanged)
    Q_PROPERTY(QVariantList confirmedItems READ confirmedItems NOTIFY intentChanged)
    Q_PROPERTY(QStringList unresolvedItems READ unresolvedItems NOTIFY intentChanged)
    Q_PROPERTY(QString rewriteProfile READ rewriteProfile NOTIFY rewriteProfileChanged)
    Q_PROPERTY(QVariantList availableRewriteProfiles READ availableRewriteProfiles CONSTANT)
    Q_PROPERTY(bool isRewriteEnabled READ isRewriteEnabled NOTIFY intentChanged)

    Q_PROPERTY(QString proposalStatus READ proposalStatus NOTIFY rewriteProposalChanged)
    Q_PROPERTY(QString proposalMessage READ proposalMessage NOTIFY rewriteProposalChanged)
    Q_PROPERTY(bool hasProposal READ hasProposal NOTIFY rewriteProposalChanged)
    Q_PROPERTY(QString proposalText READ proposalText NOTIFY rewriteProposalChanged)
    Q_PROPERTY(QString streamingPreview READ streamingPreview NOTIFY rewriteProposalChanged)
    Q_PROPERTY(QString lastDiffText READ lastDiffText NOTIFY rewriteProposalChanged)
    Q_PROPERTY(QVariantList proposalConstraints READ proposalConstraints NOTIFY rewriteProposalChanged)
    Q_PROPERTY(int pendingReviewCount READ pendingReviewCount NOTIFY rewriteProposalChanged)
    Q_PROPERTY(bool canApprove READ canApprove NOTIFY rewriteProposalChanged)
    Q_PROPERTY(bool hasApproval READ hasApproval NOTIFY approvalChanged)
    Q_PROPERTY(QString approvalLabel READ approvalLabel NOTIFY approvalChanged)
    Q_PROPERTY(QVariantList groundedEvidence READ groundedEvidence NOTIFY groundingChanged)
    Q_PROPERTY(QString groundingStatus READ groundingStatus NOTIFY groundingChanged)

    Q_PROPERTY(bool accessibilityTrusted READ isAccessibilityTrusted NOTIFY accessibilityTrustedChanged)
    Q_PROPERTY(bool hasAmbientPermissions READ isAccessibilityTrusted NOTIFY accessibilityTrustedChanged)
    Q_PROPERTY(QString accessibilityStatus READ accessibilityStatus NOTIFY accessibilityTrustedChanged)
    Q_PROPERTY(bool ambientModeEnabled READ isAmbientModeEnabled WRITE setAmbientModeEnabled NOTIFY ambientModeEnabledChanged)
    Q_PROPERTY(bool isAmbientActive READ isAmbientActive NOTIFY ambientModeEnabledChanged)
    Q_PROPERTY(bool isAmbientCapture READ isAmbientCapture NOTIFY captureSourceChanged)
    Q_PROPERTY(bool isAmbientAttached READ isAmbientAttached NOTIFY captureSourceChanged)
    Q_PROPERTY(QString captureTargetName READ captureTargetName NOTIFY captureSourceChanged)
    Q_PROPERTY(QString deliveryLabel READ deliveryLabel NOTIFY captureSourceChanged)
    Q_PROPERTY(bool ambientBadgeVisible READ isAmbientBadgeVisible NOTIFY ambientBadgeChanged)
    Q_PROPERTY(int ambientBadgeX READ ambientBadgeX NOTIFY ambientBadgeChanged)
    Q_PROPERTY(int ambientBadgeY READ ambientBadgeY NOTIFY ambientBadgeChanged)
    Q_PROPERTY(QString ambientBadgeLabel READ ambientBadgeLabel NOTIFY ambientBadgeChanged)
    Q_PROPERTY(QString ambientStatus READ ambientStatus NOTIFY ambientStatusChanged)
    Q_PROPERTY(QString shortcutLabel READ shortcutLabel NOTIFY shortcutChanged)
    Q_PROPERTY(QString invocationShortcut READ invocationShortcut NOTIFY shortcutChanged)
    Q_PROPERTY(bool hostContextEnabled READ hostContextEnabled WRITE setHostContextEnabled NOTIFY hostContextEnabledChanged)

    Q_PROPERTY(QString scorerEngine READ scorerEngine NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString scorerStatus READ scorerStatus NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString juliaState READ juliaState NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString juliaStatus READ juliaStatus NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString juliaModelDirectory READ juliaModelDirectory NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString juliaPython READ juliaPython NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString juliaDevice READ juliaDevice NOTIFY scorerEngineChanged)
    Q_PROPERTY(bool nativeGraniteSelected READ nativeGraniteSelected NOTIFY activeModelChanged)
    Q_PROPERTY(QString nativeGraniteResidency READ nativeGraniteResidency NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(QString graniteResidencyPreset READ graniteResidencyPreset NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(QString graniteResidencyStatus READ graniteResidencyStatus NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(QString nativeGraniteDirectory READ nativeGraniteDirectory NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(bool isNativeGraniteInstalled READ isNativeGraniteInstalled NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(bool isNativeGraniteQ8Installed READ isNativeGraniteQ8Installed NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(bool isNativeGraniteQ5Installed READ isNativeGraniteQ5Installed NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(bool isNativeGraniteQ4Installed READ isNativeGraniteQ4Installed NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(QString inferenceSpeed READ inferenceSpeed NOTIFY inferenceSpeedChanged)
    Q_PROPERTY(double tokensPerSecond READ tokensPerSecond NOTIFY inferenceSpeedChanged)
    Q_PROPERTY(QString acceleratorName READ acceleratorName CONSTANT)
    Q_PROPERTY(QString nativeGraniteDiskStatus READ nativeGraniteDiskStatus NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(QString nativeGraniteMemoryStatus READ nativeGraniteMemoryStatus NOTIFY modelEngineStatusChanged)
    // Upstream HUD/dashboard bindings retained over the corrected runtime services.
    Q_PROPERTY(bool hasJuliaModel READ hasJuliaModel NOTIFY scorerEngineChanged)
    Q_PROPERTY(bool isJuliaModelInstalled READ isJuliaModelInstalled NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString juliaDiskStatus READ juliaDiskStatus NOTIFY scorerEngineChanged)
    Q_PROPERTY(QString juliaMemoryStatus READ juliaMemoryStatus NOTIFY scorerEngineChanged)
    Q_PROPERTY(bool isOriginalRoute READ isOriginalRoute NOTIFY analysisStatusChanged)
    Q_PROPERTY(bool isUncertainRoute READ isUncertainRoute NOTIFY analysisStatusChanged)
    Q_PROPERTY(bool useLiveWeights READ usesLiveModel NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(bool isInferring READ isInferring NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(bool directInjectionEnabled READ isAmbientModeEnabled WRITE setAmbientModeEnabled NOTIFY ambientModeEnabledChanged)
    Q_PROPERTY(QString modelsBaseDirectory READ modelsBaseDirectory NOTIFY modelsBaseDirectoryChanged)
    Q_PROPERTY(QString modelDirectory READ modelDirectory NOTIFY modelDirectoryChanged)
    Q_PROPERTY(QString effectiveModelDirectory READ effectiveModelDirectory NOTIFY modelDirectoryChanged)

    Q_PROPERTY(QString modelRuntimeState READ modelRuntimeState NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(QString modelEngineStatus READ modelEngineStatus NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(bool isCheckingModel READ isCheckingModel NOTIFY modelEngineStatusChanged)
    Q_PROPERTY(QVariantList availableModels READ availableModels NOTIFY availableModelsChanged)
    Q_PROPERTY(QString activeModel READ activeModel WRITE setActiveModel NOTIFY activeModelChanged)
    Q_PROPERTY(bool isDownloadingModel READ isDownloadingModel NOTIFY downloadProgressChanged)
    Q_PROPERTY(int downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)
    Q_PROPERTY(QString downloadStatusMessage READ downloadStatusMessage NOTIFY downloadProgressChanged)

public:
    // Debounce between the last observed keystroke and an ambient check.
    static constexpr int kAmbientDebounceMs = 150;
    static constexpr int kAmbientMinWords = 1;
    static constexpr int kAmbientMaxChars = 4000;

    // Production host: the user's database and this platform's accessibility bridge.
    explicit HostController(QObject* parent = nullptr);
    // Headless host on an explicit database with accessibility unavailable.
    explicit HostController(const QString& dbPathOverride, QObject* parent = nullptr);
    HostController(const QString& dbPathOverride,
                   std::unique_ptr<IAccessibilityBridge> accessibility,
                   QObject* parent = nullptr);
    ~HostController() override;

    // Apps whose focused field ambient mode may read: GUI agent IDEs and the terminals that host
    // CLI agents. Exact, case-insensitive executable names or bundle identifiers.
    [[nodiscard]] static bool isAmbientEligibleApp(const QString& identifier);
    [[nodiscard]] static QString globalShortcutText();
    [[nodiscard]] QString shortcutLabel() const;
    [[nodiscard]] QString invocationShortcut() const { return m_invocationShortcut; }
    [[nodiscard]] bool hostContextEnabled() const noexcept { return m_hostContextEnabled; }
    void setHostContextEnabled(bool enabled);
    Q_INVOKABLE void setInvocationShortcut(const QString& sequence);
    // Called by the platform registrar only after the OS accepts the shortcut.
    void acceptInvocationShortcut(const QString& sequence);

    [[nodiscard]] HostState hostState() const noexcept { return m_hostState; }
    [[nodiscard]] QString hostStateString() const;
    [[nodiscard]] CardVisibility cardVisibility() const noexcept { return m_cardVisibility; }
    [[nodiscard]] QString cardVisibilityString() const;
    [[nodiscard]] bool isCardVisible() const noexcept { return m_cardVisibility != CardVisibility::Hidden; }
    [[nodiscard]] bool isDashboardVisible() const noexcept { return m_dashboardVisible; }

    [[nodiscard]] quint64 stateVersion() const noexcept;
    [[nodiscard]] QString currentSessionIdString() const;
    [[nodiscard]] QString currentRevisionId() const;
    [[nodiscard]] QString currentRevisionText() const;
    [[nodiscard]] bool isDraftDirty() const noexcept;
    [[nodiscard]] QString lastCopiedText() const { return m_lastCopiedText; }
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] QString lastErrorCode() const { return m_lastErrorCode; }
    [[nodiscard]] QString shortcutWarning() const { return m_shortcutWarning; }

    [[nodiscard]] bool hasHostContext() const noexcept { return m_hostContext.isValid(); }
    [[nodiscard]] QString hostContextCategory() const { return m_hostContext.categoryString(); }
    [[nodiscard]] QString hostContextDisplayName() const { return m_hostContext.displayName; }
    [[nodiscard]] QString capturedDraftText() const { return m_capturedDraftText; }

    bool storageBusy() const { return m_commandRunning || !m_storageCommands.empty() || (m_pendingDraft.has_value() && !m_storageFault) || m_draftWriting || m_draftNotifications > 0; }
    bool storageNeedsRecovery() const { return m_storageFault; }
    Q_INVOKABLE quint64 retryStorage(const QString& draftText, quint64 expectedVersion);
    [[nodiscard]] QString databasePath() const { return m_databasePath; }
    [[nodiscard]] QString databaseStats() const { return m_databaseStats; }
    [[nodiscard]] bool isStorageAvailable() const noexcept { return m_repository != nullptr && !m_storageFault; }
    [[nodiscard]] int sessionCount() const;

    [[nodiscard]] QString analysisStatus() const;
    [[nodiscard]] QString analysisMessage() const { return m_analysisMessage; }
    [[nodiscard]] bool isAnalyzing() const noexcept { return m_analysisStatus == AnalysisStatus::Analyzing; }

    [[nodiscard]] QString reviewOutcome() const;
    [[nodiscard]] QString reviewDiagnostics() const;
    [[nodiscard]] bool isOptionalHelp() const;
    [[nodiscard]] QString interventionVerdict() const;
    [[nodiscard]] QString interventionReason() const;
    [[nodiscard]] bool isClarifying() const noexcept;
    [[nodiscard]] bool isManual() const noexcept;
    [[nodiscard]] QString activeItemId() const;
    [[nodiscard]] QString activeItemKind() const;
    [[nodiscard]] QString activeQuestion() const { return m_activeQuestion; }
    [[nodiscard]] QString activeDimension() const { return m_activeDimension; }
    [[nodiscard]] QVariantList activeAlternatives() const { return m_activeAlternatives; }
    [[nodiscard]] int roundsCompleted() const noexcept;
    [[nodiscard]] int remainingBudget() const noexcept;
    [[nodiscard]] bool isAmbiguityScored() const noexcept { return m_activeScore.has_value(); }
    [[nodiscard]] double ambiguityScore() const noexcept { return m_activeScore.value_or(0.0); }
    [[nodiscard]] QVariantList assumptionReviews() const;
    [[nodiscard]] QString rankingStatus() const;
    [[nodiscard]] QString diagnosticMessage() const;

    [[nodiscard]] bool hasConfirmedIntent() const noexcept { return m_confirmedIntent.has_value(); }
    [[nodiscard]] bool isIntentStale() const;
    [[nodiscard]] QString confirmedIntentSummary() const;
    [[nodiscard]] QVariantList confirmedItems() const;
    [[nodiscard]] QStringList unresolvedItems() const;
    [[nodiscard]] QString rewriteProfile() const { return m_rewriteProfile; }
    [[nodiscard]] QVariantList availableRewriteProfiles() const;
    [[nodiscard]] bool isRewriteEnabled() const;

    [[nodiscard]] QString proposalStatus() const;
    [[nodiscard]] QString proposalMessage() const { return m_proposalMessage; }
    [[nodiscard]] bool hasProposal() const noexcept;
    [[nodiscard]] QString proposalText() const;
    [[nodiscard]] QString streamingPreview() const { return m_streamingPreview; }
    [[nodiscard]] QString lastDiffText() const;
    [[nodiscard]] QVariantList proposalConstraints() const;
    [[nodiscard]] int pendingReviewCount() const;
    [[nodiscard]] bool canApprove() const;
    [[nodiscard]] bool hasApproval() const noexcept { return m_currentApproval.has_value(); }
    [[nodiscard]] QString approvalLabel() const;
    [[nodiscard]] QVariantList groundedEvidence() const;
    [[nodiscard]] QString groundingStatus() const { return m_groundingStatus; }

    [[nodiscard]] bool isAccessibilityTrusted() const;
    [[nodiscard]] QString accessibilityStatus() const;
    [[nodiscard]] bool isAmbientModeEnabled() const noexcept { return m_ambientModeEnabled; }
    void setAmbientModeEnabled(bool enabled);
    [[nodiscard]] bool isAmbientActive() const;
    [[nodiscard]] bool isAmbientCapture() const noexcept { return m_captureSource == CaptureSource::Ambient; }
    [[nodiscard]] bool isAmbientAttached() const noexcept {
        return isAmbientActive() && m_captureSource == CaptureSource::Ambient && m_captureTarget.has_value();
    }
    [[nodiscard]] QString captureTargetName() const;
    [[nodiscard]] QString deliveryLabel() const;
    [[nodiscard]] bool isAmbientBadgeVisible() const noexcept { return m_ambientBadgeVisible; }
    [[nodiscard]] int ambientBadgeX() const noexcept { return m_ambientBadgePos.x(); }
    [[nodiscard]] int ambientBadgeY() const noexcept { return m_ambientBadgePos.y(); }
    [[nodiscard]] QString ambientBadgeLabel() const { return m_ambientBadgeLabel; }
    [[nodiscard]] QString ambientStatus() const { return m_ambientStatus; }
    [[nodiscard]] QString recommendedOption() const { return m_recommendedOption; }
    [[nodiscard]] double recommendedConfidence() const noexcept { return m_recommendedConfidence; }

    [[nodiscard]] QString scorerEngine() const;
    [[nodiscard]] QString scorerStatus() const;
    [[nodiscard]] QString juliaState() const { return m_julia->state(); }
    [[nodiscard]] QString juliaStatus() const;
    [[nodiscard]] QString juliaModelDirectory() const { return m_juliaModelDirectory; }
    [[nodiscard]] QString juliaPython() const { return m_juliaPython; }
    [[nodiscard]] QString juliaDevice() const { return m_juliaDevice; }
    [[nodiscard]] bool hasJuliaModel() const { return m_julia->isReady(); }
    [[nodiscard]] bool isJuliaModelInstalled() const;
    [[nodiscard]] QString juliaDiskStatus() const;
    [[nodiscard]] QString juliaMemoryStatus() const;
    [[nodiscard]] bool isOriginalRoute() const {
        if (!m_currentSnapshot || !m_currentSnapshot->promptTriage || !m_revisionState || m_revisionState->isDraftDirty()) return false;
        const auto& a = m_currentSnapshot->normalized;
        return a.choiceGroups.empty() && a.assumptions.empty() && a.missingDecisions.empty() && a.conflicts.empty();
    }
    [[nodiscard]] bool isUncertainRoute() const {
        return m_currentSnapshot && m_currentSnapshot->promptTriage && m_revisionState && !m_revisionState->isDraftDirty() &&
            m_currentSnapshot->discoveryAttemptId.empty() &&
            core::analysis::routePromptTriage(*m_currentSnapshot->promptTriage).value() == core::analysis::PromptTriageRoute::UncertainBuffer;
    }
    [[nodiscard]] bool usesLiveModel() const { return m_graniteAdapter->state() == bridge::GraniteRuntimeState::Ready || isInferring(); }
    [[nodiscard]] bool isInferring() const { return m_graniteAdapter->state() == bridge::GraniteRuntimeState::Busy; }
    [[nodiscard]] static QString defaultModelsBaseDirectory();
    [[nodiscard]] QString modelsBaseDirectory() const { return m_modelsBaseDirectory; }
    [[nodiscard]] QString modelDirectory() const {
        if (nativeGraniteSelected() && !m_nativeDirectory.isEmpty()) return m_nativeDirectory;
        return m_modelDirectory;
    }
    [[nodiscard]] QString effectiveModelDirectory() const { return modelDirectory(); }
    [[nodiscard]] bool nativeGraniteSelected() const { return NativeGraniteRuntime::isNativeModel(m_activeModel); }
    [[nodiscard]] bool isNativeGraniteInstalled() const;
    [[nodiscard]] bool isNativeGraniteQ8Installed() const;
    [[nodiscard]] bool isNativeGraniteQ5Installed() const;
    [[nodiscard]] bool isNativeGraniteQ4Installed() const;
    [[nodiscard]] QString inferenceSpeed() const;
    [[nodiscard]] double tokensPerSecond() const;
    [[nodiscard]] QString acceleratorName() const {
        // ROCLINT_ACCELERATOR_NAME is injected by CMakeLists.txt (PUBLIC) from the resolved
        // _roclint_backend variable so it always matches the actual compiled backend
        // (METAL / VULKAN / CPU / GPU). The #ifndef fallback below covers any TU that is
        // compiled outside the normal roclint_host_lib target graph (e.g. standalone headers).
        // QLatin1StringView is used instead of QStringLiteral because QStringLiteral requires
        // a raw string-literal token and cannot accept a macro expansion.
#ifndef ROCLINT_ACCELERATOR_NAME
#  if defined(__APPLE__)
#    define ROCLINT_ACCELERATOR_NAME "METAL"
#  elif defined(_WIN32)
#    define ROCLINT_ACCELERATOR_NAME "VULKAN"
#  else
#    define ROCLINT_ACCELERATOR_NAME "GPU"
#  endif
#endif
        return QString(QLatin1StringView(ROCLINT_ACCELERATOR_NAME));
    }
    [[nodiscard]] QString nativeGraniteDiskStatus() const;
    [[nodiscard]] QString nativeGraniteMemoryStatus() const;
    [[nodiscard]] QString graniteResidencyPreset() const { return m_graniteResidencyPreset; }
    [[nodiscard]] QString graniteResidencyStatus() const { return m_graniteResidencyStatus; }
    [[nodiscard]] QString nativeGraniteResidency() const { return m_native->residency(); }
    [[nodiscard]] QString nativeGraniteDirectory() const { return m_nativeDirectory; }

    [[nodiscard]] QString modelRuntimeState() const;
    [[nodiscard]] QString modelEngineStatus() const;
    [[nodiscard]] bool isCheckingModel() const noexcept { return m_healthCheckInFlight || m_nativeWarming; }
    [[nodiscard]] QVariantList availableModels() const { return m_availableModels; }
    [[nodiscard]] QString activeModel() const { return m_activeModel; }
    void setActiveModel(const QString& model);
    [[nodiscard]] bool isDownloadingModel() const noexcept { return m_isDownloadingModel; }
    [[nodiscard]] int downloadProgress() const noexcept { return m_downloadProgress; }
    [[nodiscard]] QString downloadStatusMessage() const { return m_downloadStatusMessage; }

    void setShortcutRegistrationFailed(const QString& message);

    // Test seams. Compiled only into the *_testing libraries.
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    // Retains coverage of historical scoring contracts; production always uses S1 first.
    void setLegacyDiscoveryForTesting(bool legacy) { m_promptTriageRouting = !legacy; }
    void setTransportOverrideForTesting(bridge::GraniteTransport transport);
    [[nodiscard]] bool setModelRuntimeUrlForTesting(const QUrl& url);
    [[nodiscard]] bool markModelReadyForTesting(const bridge::GraniteModelInfo& info);
    [[nodiscard]] bridge::S1TriageAdapter& s1AdapterForTesting() noexcept { return m_s1Adapter; }
    bool waitForCommandForTesting(quint64 id, int timeoutMs = 15000);
    bool waitForStorageForTesting(int timeoutMs = 15000);
    [[nodiscard]] bool waitForIdleForTesting(int timeoutMs);
    void setAnalysisGroupTimeoutForTesting(int timeoutMs) { m_analysisGroupTimeoutMs = timeoutMs > 0 ? timeoutMs : 1; }
    void setRecommendedOptionForTesting(const QString& option, double confidence) {
        m_recommendedOption = option;
        m_recommendedConfidence = confidence;
        emit recommendedOptionChanged();
    }
    bool setNativeWorkerForTesting(const QString& worker) { return m_native->setWorkerForTesting(worker); }
    void setCodebaseContextConsentedForTesting(bool consented) { m_codebaseContextConsented = consented; }
    void setCodebaseIndexPathForTesting(QString path);
    void setCodebaseRefreshLimitsForTesting(std::optional<storage::CodebaseIndexLimits> limits);
    void setCodebaseWatcherLimitsForTesting(std::optional<CodebaseWatcher::Limits> limits);
    void startCodebaseRefreshForTesting(const QString& projectPath, bool consented = true);
    [[nodiscard]] bool codebaseIndexReadyForTesting() const noexcept { return m_codebaseIndexReady; }
    [[nodiscard]] std::uint64_t workspaceEpochForTesting() const noexcept { return m_workspaceEpoch; }
    [[nodiscard]] int codebaseRefreshStartsForTesting() const noexcept { return m_codebaseRefreshStartsForTesting; }
    [[nodiscard]] bool codebaseWatcherDegradedForTesting() const noexcept { return m_codebaseWatcherDegraded; }
    [[nodiscard]] double lastGroundingApplyMsForTesting() const noexcept { return m_lastGroundingApplyMs; }
    [[nodiscard]] QString groundedIntentXmlForTesting(const QString& prompt);
    void setResidencyInputsForTesting(std::optional<qint64> idleMs, std::optional<bool> lowMemory) {
        m_residencyIdleOverride = idleMs; m_residencyPressureOverride = lowMemory; applyResidencyPolicy();
    }
    [[nodiscard]] const std::optional<core::intent::ConfirmedIntent>& confirmedIntentForTesting() const noexcept {
        return m_confirmedIntent;
    }
    [[nodiscard]] const std::optional<core::rewrite::RewriteProposal>& proposalForTesting() const noexcept {
        return m_currentProposal;
    }
    [[nodiscard]] const std::optional<core::analysis::NormalizedAnalysis>& analysisForTesting() const noexcept {
        return m_currentAnalysis;
    }
    [[nodiscard]] const std::optional<core::analysis::AnalysisSnapshot>& finalSnapshotForTesting() const noexcept {
        return m_currentSnapshot;
    }
    [[nodiscard]] const core::intent::WorkingIntentState& workingIntentForTesting() const noexcept {
        return m_workingIntent;
    }
    [[nodiscard]] AsyncRepository* repositoryForTesting() noexcept { return m_repository.get(); }
#endif

public slots:
    void markResident();
    void showCard();
    void hideCard();
    void toggleCard();
    void showDashboard();
    void hideDashboard();
    void toggleDashboard();
    void requestQuit();

    void clearHostContext();
    void captureClipboardDraft();
    void clearLastError();

    void syncCardVisibility(roclint::host::CardVisibility visibility);
    void syncDashboardVisibility(bool visible);

    // Draft lifecycle. expectedVersion is the stateVersion the caller last observed.
    Q_INVOKABLE void pauseAmbientFollowing();
    Q_INVOKABLE quint64 noteDraftEdited(quint64 expectedVersion);
    Q_INVOKABLE quint64 commitPrompt(QString text, quint64 expectedVersion);
    Q_INVOKABLE quint64 hardBypass(QString text, quint64 expectedVersion);

    // Analysis and clarification.
    Q_INVOKABLE quint64 analyzePrompt(QString text, quint64 expectedVersion);
    Q_INVOKABLE quint64 reviewPrompt(QString text, quint64 expectedVersion);
    Q_INVOKABLE quint64 cancelAnalysis(quint64 expectedVersion);
    Q_INVOKABLE quint64 confirmChoice(QString alternativeId, quint64 expectedVersion);
    Q_INVOKABLE quint64 submitCustomIntent(QString customText, quint64 expectedVersion);
    Q_INVOKABLE quint64 submitClarification(const QString& text, bool isDeferral, quint64 expectedVersion);
    Q_INVOKABLE quint64 submitClarification(const QString& text, quint64 expectedVersion);
    [[nodiscard]] static bool isDeferralPhrase(const QString& text);
    Q_INVOKABLE quint64 keepOriginalPrompt(quint64 expectedVersion);
    Q_INVOKABLE quint64 resolveActiveAssumption(bool accepted, quint64 expectedVersion);
    Q_INVOKABLE quint64 skipCurrentItem(quint64 expectedVersion);
    Q_INVOKABLE quint64 switchToManual(quint64 expectedVersion);
    Q_INVOKABLE quint64 setRewriteProfile(QString profile, quint64 expectedVersion);
    Q_INVOKABLE QVariantMap getConfirmedIntentPayload() const;

    // Rewrite review. synthesizeRewrite only produces and validates a proposal.
    Q_INVOKABLE quint64 synthesizeRewrite(quint64 expectedVersion);
    // Explicit wording-only request: defer unanswered choices without selecting them.
    Q_INVOKABLE quint64 rewriteWithoutQuestions(quint64 expectedVersion);
    Q_INVOKABLE quint64 acknowledgeConstraint(QString constraintId, bool acknowledged, quint64 expectedVersion);
    // Persists the approval, then delivers the approved text: into the captured field for an
    // ambient capture (falling back to the clipboard if that fails), otherwise to the clipboard.
    Q_INVOKABLE quint64 approveAndCopyRewrite(quint64 expectedVersion);
    Q_INVOKABLE bool beginInsertionTargetSelection(quint64 expectedVersion);
    Q_INVOKABLE quint64 discardProposal(quint64 expectedVersion);

    // Ambient mode.
    Q_INVOKABLE bool requestAccessibilityPermission();
    // Opens the card on the prompt last observed in an eligible app.
    Q_INVOKABLE quint64 openFromAmbient();
    Q_INVOKABLE void dismissAmbientBadge();

    // History.
    Q_INVOKABLE QVariantList getRecentSessions();
    Q_INVOKABLE QVariantList getSessionHistory(const QString& sessionId);
    Q_INVOKABLE quint64 restoreRevision(QString revisionId, quint64 expectedVersion);
    Q_INVOKABLE quint64 restoreSession(QString sessionId, quint64 expectedVersion);
    Q_INVOKABLE quint64 restoreSessionForReview(QString sessionId, quint64 expectedVersion);
    Q_INVOKABLE quint64 deleteSession(QString sessionId);
    Q_INVOKABLE quint64 purgeHistory();
    Q_INVOKABLE void refreshStorageStats();

    // Scorer and model runtime.
    Q_INVOKABLE bool setScorerEngine(const QString& engine);
    Q_INVOKABLE QString openFolderDialog();
    Q_INVOKABLE QVariantList recentProjects() const;
    Q_INVOKABLE QStringList getProjectFiles(const QString& projectPath);
    // Explicitly schedules an incremental refresh of the selected workspace.
    Q_INVOKABLE bool refreshCodebaseIndex();
    Q_INVOKABLE QString scopePreamble(const QString& projectPath, const QString& scope, const QString& file);
    Q_INVOKABLE QString openModelFolderDialog();
    Q_INVOKABLE void importLocalModel(const QString& path);
    Q_INVOKABLE void refreshJuliaModel();
    Q_INVOKABLE void setModelRuntimeSource(bool native);
    Q_INVOKABLE bool setGraniteResidencyPreset(const QString& preset);
    Q_INVOKABLE void warmSelectedModel();
    Q_INVOKABLE void chooseModelsBaseDirectory();
    Q_INVOKABLE void chooseNativeModelDirectory();
    Q_INVOKABLE void downloadJuliaModel();
    Q_INVOKABLE void chooseJuliaModelDirectory();
    Q_INVOKABLE void chooseJuliaPython();
    Q_INVOKABLE void setJuliaDevice(const QString& device);
    Q_INVOKABLE void refreshAvailableModels();
    Q_INVOKABLE bool pinActiveModelDigest();
    Q_INVOKABLE void downloadModel(const QString& modelTag);
    Q_INVOKABLE void cancelModelDownload();
    Q_INVOKABLE QPoint calculateAmbientAnchor(int width, int height);

signals:
    void storageBusyChanged();
    void storageCommandFinished(quint64 requestId, const QString& name, bool succeeded, const QString& errorCode, const QString& message);
    void cardVisibilityChanged();
    void dashboardVisibilityChanged();
    void hostStateChanged();
    void showCardRequested(int x, int y);
    void hideCardRequested();
    void showDashboardRequested();
    void hideDashboardRequested();
    void quitRequested();
    void stateVersionChanged();
    void lastCopiedTextChanged();
    void lastErrorChanged();
    void shortcutWarningChanged();
    void shortcutChanged();
    void shortcutChangeRequested(const QString& sequence);
    void hostContextEnabledChanged();
    void promptCopied(const QString& text);
    void hostContextChanged();
    void capturedDraftTextChanged();
    void draftCaptured(const QString& text);
    void storageChanged();
    void sessionListChanged();
    void revisionRestored(const QString& text);
    void analysisStatusChanged();
    void interventionChanged();
    void intentChanged();
    void rewriteProfileChanged(const QString& profile);
    void rewriteProposalChanged();
    void inferenceSpeedChanged();
    void approvalChanged();
    void groundingChanged();
    void accessibilityTrustedChanged();
    void ambientModeEnabledChanged();
    void captureSourceChanged();
    void ambientDraftUpdated(const QString& text);
    void ambientBadgeChanged();
    void ambientStatusChanged();
    void recommendedOptionChanged();
    void clarificationDeferred(const QString& message, bool autoSelected);
    void promptInjected(const QString& text, const QString& appName);
    // Ambient delivery failed and the text went to the clipboard instead.
    void deliveryFellBack(const QString& message);
    void insertionTargetSelectionRequested(const QString& message);
    void scorerEngineChanged();
    void projectFilesChanged();
    void modelsBaseDirectoryChanged();
    void modelDirectoryChanged();
    void availableModelsChanged();
    void activeModelChanged();
    void modelEngineStatusChanged();
    void downloadProgressChanged();

private:
    // A positive budget spans discovery and Julia. Zero waits until the request finishes. Warm-up has its own deadline.
    struct AnalysisGroup {
        explicit AnalysisGroup(int timeoutMs)
            : deadline(timeoutMs > 0 ? QDeadlineTimer(timeoutMs, Qt::PreciseTimer) : QDeadlineTimer(QDeadlineTimer::Forever)),
              budgetMs(timeoutMs) {}
        QDeadlineTimer deadline;
        int budgetMs;
        std::string id;
        bool terminal{false};
        bool superseded{false};
        bool paused{false};
        qint64 remainingMs{0};
        // A timer armed by S1 may fire during Granite. Complete the active phase's attempt.
        std::string activeAttemptId;
        std::string activeConfiguration;
        bridge::GraniteModelInfo activeModel;
        void pause() {
            if (paused) return;
            remainingMs = deadline.remainingTime();
            deadline = QDeadlineTimer(QDeadlineTimer::Forever); paused = true;
        }
        void resume() {
            if (!paused) return;
            deadline = budgetMs > 0 ? QDeadlineTimer(std::max<qint64>(0, remainingMs), Qt::PreciseTimer)
                                    : QDeadlineTimer(QDeadlineTimer::Forever);
            paused = false;
        }
    };
    struct PipelineTicket {
        std::string sessionId;
        std::string revisionId;
        std::uint64_t stateVersion{0};
        std::uint64_t draftEditGeneration{0};
        std::uint64_t proposalEditGeneration{0};
        std::uint64_t serial{0};
        std::uint64_t workspaceEpoch{0};
        core::analysis::ReviewTrigger reviewTrigger{core::analysis::ReviewTrigger::Automatic};
        std::string attemptId;
        std::string configuration;
        bridge::GraniteModelInfo model;
        std::shared_ptr<AnalysisGroup> analysisGroup;
    };

    struct HealthResult {
        std::optional<core::Result<std::vector<OllamaModelEntry>>> models;
        std::optional<core::Result<bridge::GraniteModelInfo>> probe;
        std::optional<core::Status> warmUp;
        QString probedModel;
    };

    struct PendingJuliaScore {
        PipelineTicket ticket;
        core::analysis::NormalizedAnalysis analysis;
        QString message;
        std::string discoveryAttemptId;
    };

    // The prompt read from an eligible app's focused field. Held in memory only.
    struct AmbientObservation {
        ObservedField field;
        QString prompt;        // Canonical LF prompt: the whole field, or the terminal draft
        std::string textHash;  // SHA-256 of prompt
    };

    // A completed background check. Nothing here is persisted until the card is opened.
    struct AmbientCheck {
        std::string textHash;
        std::string rawReply;
        bridge::GraniteModelInfo model;
        bool needsAttention{false};
        int questionCount{0};
    };

    struct PendingDelivery {
        quint64 requestId{0};
        QString text;
        QString appName;
        quint64 fieldToken{0};
        qint64 processId{0};
        QString appIdentifier;
        quint64 pipelineSerial{0};
    };

    bool setCardVisibility(CardVisibility visibility);
    quint64 noteDraftEditedInternal(quint64 expectedVersion, bool editedInHud);
    bool setDashboardVisibility(bool visible);
    AsyncTask<void> initStorage(QString dbPathOverride);
    AsyncTask<core::Status> openFromAmbientOperation(std::optional<AmbientObservation> observationSnapshot);
    AsyncTask<core::Status> purgeHistoryOperation();
    AsyncTask<core::Status> deleteSessionOperation(QString sessionId);
    AsyncTask<core::Status> restoreSessionOperation(QString sessionId, quint64 expectedVersion);
    AsyncTask<core::Status> restoreSessionForReviewOperation(QString sessionId, quint64 expectedVersion);
    AsyncTask<core::Status> restoreRevisionOperation(QString revisionId, quint64 expectedVersion);
    AsyncTask<core::Status> discardProposalOperation(quint64 expectedVersion);
    AsyncTask<core::Status> approveAndCopyRewriteOperation(quint64 expectedVersion);
    AsyncTask<core::Status> acknowledgeConstraintOperation(QString constraintId, bool acknowledged, quint64 expectedVersion);
    AsyncTask<core::Status> synthesizeRewriteOperation(quint64 expectedVersion);
    AsyncTask<core::Status> rewriteWithoutQuestionsOperation(quint64 expectedVersion);
    AsyncTask<core::Status> setRewriteProfileOperation(QString profile, quint64 expectedVersion);
    AsyncTask<core::Status> switchToManualOperation(quint64 expectedVersion);
    AsyncTask<core::Status> skipCurrentItemOperation(quint64 expectedVersion);
    AsyncTask<core::Status> resolveActiveAssumptionOperation(bool accepted, quint64 expectedVersion);
    AsyncTask<core::Status> keepOriginalPromptOperation(quint64 expectedVersion);
    AsyncTask<core::Status> submitCustomIntentOperation(QString customText, quint64 expectedVersion);
    AsyncTask<core::Status> confirmChoiceOperation(QString alternativeId, quint64 expectedVersion);
    AsyncTask<core::Status> analyzePromptOperation(QString text, quint64 expectedVersion,
        core::analysis::ReviewTrigger trigger = core::analysis::ReviewTrigger::Automatic);
    AsyncTask<core::Status> cancelAnalysisOperation(quint64 expectedVersion);
    AsyncTask<core::Status> hardBypassOperation(QString text, quint64 expectedVersion);
    AsyncTask<core::Status> commitPromptOperation(QString text, quint64 expectedVersion);

    void setLastError(const QString& message);
    void setLastError(const core::Error& error);
    template <typename Fn>
    bool runCommand(const char* name, Fn&& fn);

    [[nodiscard]] std::string currentSessionId() const;
    [[nodiscard]] bool isTicketCurrent(const PipelineTicket& ticket, bool checkProposalGeneration) const;
    [[nodiscard]] PipelineTicket makeTicket() const;

    AsyncTask<core::Status> ensureSession(std::string canonicalText, std::int64_t nowMs);
    AsyncTask<core::Status> commitRevision(std::string canonicalText,
                                std::uint64_t expectedVersion,
                                storage::RevisionOrigin origin);
    AsyncTask<core::Status> freezeDraftIfNeeded(std::string canonicalText, std::uint64_t expectedVersion);

    // Clears every artifact derived from the current revision: analysis, scores, decision, working
    // intent, confirmed intent, proposal, acknowledgments, approval; cancels in-flight results.
    void invalidateDownstreamPipeline();
    // Clears the proposal, acknowledgments and approval and advances the proposal generation.
    AsyncTask<core::Status> invalidateProposalArtifacts();
    AsyncTask<core::Status> advanceProposalGeneration();
    core::Status checkExpectedVersion(quint64 expectedVersion) const;
    AsyncTask<core::Status> persistStateChange(core::RevisionState next, quint64 expectedVersion);
    AsyncTask<core::Status> advanceCommandVersion(quint64 expectedVersion);
    void resetSessionState();

    // With allowReconnect, an unconnected or failed runtime is re-checked first and discovery
    // resumes from onHealthFinished; without it, discovery fails immediately.
    AsyncTask<void> resumeDiscovery(bool allowReconnect = true);
    AsyncTask<core::Status> startDiscovery(bool allowReconnect = true);
    AsyncTask<core::Status> startPromptTriage();
    AsyncTask<void> continuePromptTriageLoad();
    void onPromptClassified(quint64 requestId, const core::analysis::PromptTriage* triage, const QString& error);
    AsyncTask<void> onPromptClassifiedAsync(PipelineTicket ticket, std::optional<core::analysis::PromptTriage> triage, QString error);
    [[nodiscard]] bool promptTriageRoutingEnabled() const noexcept;
    [[nodiscard]] bool usesTransportOverride() const noexcept;
    void onDiscoveryFinished(PipelineTicket ticket, core::Result<bridge::GraniteOptionDiscoveryResponse> result);
    AsyncTask<void> onDiscoveryFinishedAsync(PipelineTicket ticket,
                             core::Result<bridge::GraniteOptionDiscoveryResponse> result);
    void onRewriteFinished(PipelineTicket ticket, core::Result<core::rewrite::RewriteProposal> result,
                           GroundedPromptContext grounding, bool evidenceCurrent);
    AsyncTask<void> onRewriteFinishedAsync(PipelineTicket ticket,
                           core::Result<core::rewrite::RewriteProposal> result,
                           GroundedPromptContext grounding, bool evidenceCurrent);
    AsyncTask<void> onHealthFinished(HealthResult result);
    AsyncTask<void> rebuildFromRestore(storage::RestoredSession restored);

    AsyncTask<bool> applyAnalysis(core::analysis::NormalizedAnalysis analysis, std::string discoveryAttemptId,
                       std::string scorerAttemptId = {},
                       std::optional<bridge::SnapshotEvaluationResponse> evaluation = std::nullopt,
                       std::optional<core::analysis::PromptTriage> triage = std::nullopt,
                       core::analysis::AssistanceKind assistance = core::analysis::AssistanceKind::None);
    void onJuliaFinished(quint64 requestId, const bridge::SnapshotEvaluationResponse* response, const QString& error);
    AsyncTask<void> onJuliaFinishedAsync(PendingJuliaScore pending, std::optional<bridge::SnapshotEvaluationResponse> response, QString error);
    void armAnalysisDeadline(const PipelineTicket& ticket);
    void expireAnalysisGroup(const PipelineTicket& ticket);
    void invalidateScorerConfiguration();
    void appendLocalModelFiles();
    AsyncTask<void> reevaluateIntervention();
    void refreshActiveItemPresentation();
    AsyncTask<core::Status> finalizeIntentIfQuiet();
    AsyncTask<core::Status> applyTransition(const char* name, quint64 expectedVersion,
                         std::function<core::Status(core::intent::WorkingIntentState&)> transition);

    [[nodiscard]] core::rewrite::ValidationResult validateCurrentProposal() const;
    [[nodiscard]] std::optional<core::review::ApprovalContext> approvalContext() const;

    AsyncTask<core::Status> beginAttempt(PipelineTicket& ticket, const char* stage);
    AsyncTask<void> completeAttempt(PipelineTicket ticket,
                         storage::AttemptStatus status,
                         std::string errorMessage,
                         const bridge::GraniteModelInfo* model = nullptr);

    [[nodiscard]] bridge::GraniteTransport currentTransport(const std::string& outputSchema, const PipelineTicket* ticket = nullptr,
        const QString& operation = {}, const QString& prompt = {}, const QString& intentId = {}, const QString& profileId = {}) const;
    void prepareNativeModel();
    void initializeResidencyPolicy();
    void applyResidencyPolicy();
    AsyncTask<void> onNativeRuntimeChanged();
    void appendNativeModelFiles();
    void finishQuitWhenRuntimesStopped();
    // Resolves lexical intents against the local AST index and renders the bounded prompt block.
    [[nodiscard]] GroundedPromptContext groundedCanonicalIntentContext(std::string_view prompt);
    [[nodiscard]] GroundingInputs groundingInputs(std::string prompt) const;
    void applyGroundingContext(const GroundedPromptContext& context);
    void setGroundingUnavailable(const QString& status);
    void scheduleGroundingRevalidation();
    [[nodiscard]] QString codebaseIndexPathForHost() const;
    void startCodebaseRefresh(const QString& projectPath);
    void startCodebaseWatcher(const QString& projectPath);
    void stopCodebaseWatcher();
    void maybeRecoverCodebaseWatcher();
    void resetCodebaseWatcherRecovery();
    void scheduleCodebaseWatcherRecovery();
    void scheduleCodebasePathsRefresh(const QStringList& relativePaths);
    void startCodebasePathsRefresh(const QString& projectPath, const QStringList& relativePaths);
    void scheduleStalePathRecovery(const std::vector<std::string>& relativePaths);
    void resetStalePathRecovery();
    void startPendingNativeDownload();
    [[nodiscard]] std::string currentTransportName() const;
    core::Status copyToClipboard(const QString& text);

    // Persists a discovery response for the current revision and applies it. Shared by live
    // discovery and by rebinding an ambient check's reply to the frozen revision.
    AsyncTask<void> acceptDiscovery(PipelineTicket ticket, bridge::GraniteOptionDiscoveryResponse response);
    void showCardInternal(bool captureClipboard);
    void beginCaptureInvocation(const HostContextObservation& frontmost);
    // Delivers text for the current capture: injection for ambient captures, clipboard otherwise.
    core::Status deliver(const QString& text);
    void onInjectionFinished(quint64 requestId, InjectionOutcome outcome, const QString& detail);
    void onFieldObserved(const ObservedField& field);
    void followAmbientDraft();
    void onFieldLost();
    void onAmbientDebounce();
    void startAmbientCheck();
    void setAmbientBadge(bool visible, const QString& label = QString());
    void setAmbientStatus(const QString& status);
    void setCaptureSource(CaptureSource source);
    void applyAmbientObserving();
    void setAnalysisStatus(AnalysisStatus status, const QString& message);
    void setProposalStatus(ProposalUiStatus status, const QString& message);

    HostState m_hostState{HostState::Starting};
    CardVisibility m_cardVisibility{CardVisibility::Hidden};
    bool m_dashboardVisible{false};
    QString m_lastCopiedText;
    QString m_lastError;
    QString m_lastErrorCode;
    QString m_shortcutWarning;
    QString m_invocationShortcut{QStringLiteral("Ctrl+Shift+L")};
    bool m_hostContextEnabled{true};
    HostContextObservation m_hostContext;
    QString m_capturedDraftText;

    struct StorageCommand { quint64 id; quint64 editSerial; QString name; std::function<AsyncTask<core::Status>()> operation; bool modelCompletion{false}; };
    struct DraftBatch { core::RevisionState next; quint64 expectedVersion; std::vector<std::pair<quint64, quint64>> requests; };
    quint64 enqueueStorageCommand(const char* name, std::function<AsyncTask<core::Status>()> operation,
        bool modelCompletion = false);
    void queueModelCompletion(const char* name, std::function<AsyncTask<void>()> completion);
    void pumpStorageCommands();
    AsyncTask<void> runStorageCommand(std::shared_ptr<StorageCommand> command);
    AsyncTask<core::Status> flushDraftEdits();
    AsyncTask<core::Status> writeDraftBatch(DraftBatch batch);
    AsyncTask<void> loadHistoryCache();
    AsyncTask<core::Status> retryStorageOperation(QString draftText, quint64 expectedVersion);
    struct DraftCompletion { std::vector<std::pair<quint64, quint64>> ranges; size_t range{0}; quint64 next{0}; core::Status status; };
    void finishDraftRequests(std::vector<std::pair<quint64, quint64>> requests, core::Status status);
    void publishDraftCompletions(std::shared_ptr<DraftCompletion> completion);
    size_t m_draftNotifications{0};
    void finishStorageCommand(quint64 id, const QString& name, const core::Status& status);
    void failStorage(const core::Error& error);
    bool m_storageFault{false};
    bool m_commandRunning{false};
    bool m_historyRefreshRunning{false};
    bool m_historyRefreshAgain{false};
    bool m_draftWriting{false};
    quint64 m_nextStorageRequest{0};
    quint64 m_latestStorageFeedback{0};
    quint64 m_liveEditSerial{0};
    quint64 m_storageEpoch{0};
    std::deque<std::shared_ptr<StorageCommand>> m_storageCommands;
    std::optional<DraftBatch> m_pendingDraft;
    std::optional<AsyncTask<core::Status>> m_draftFlushTask;
    QTimer m_draftDebounce;
    QTimer m_draftMaxWait;
    QVariantList m_recentSessions;
    QHash<QString, QVariantList> m_historyCache;
    QHash<quint64, bool> m_commandResults;
    std::unique_ptr<StorageService> m_storage;
    std::unique_ptr<AsyncRepository> m_repository;
    QString m_workspaceRootIdentity;
    bool m_workspaceScanIncomplete{true};
    QString m_lastInternalDiagnostic; // Bounded diagnostic; never exposed as a QML property or logged by default.
    std::function<void()> m_cancelWorkspaceScan;
    std::function<void()> m_cancelWorkspaceContext;
    QString m_databasePath;
    QString m_databaseStats;

    std::unique_ptr<core::RevisionState> m_revisionState;

    bridge::S1TriageAdapter m_s1Adapter;
    std::unique_ptr<JuliaRuntime> m_julia;
    std::unique_ptr<NativeGraniteRuntime> m_native;
    std::unique_ptr<GraniteModelInstaller> m_nativeInstaller;
    QString m_modelsBaseDirectory;
    QString m_nativeDirectory;
    QTimer m_residencyTimer;
    QElapsedTimer m_nativeLastUse;
    std::shared_ptr<MemoryPressureMonitor> m_memoryPressure;
    QString m_graniteResidencyPreset{QStringLiteral("BALANCED")};
    QString m_graniteResidencyStatus;
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    std::optional<qint64> m_residencyIdleOverride;
    std::optional<bool> m_residencyPressureOverride;
#endif
    bool m_nativeWarming{false};
    int m_nativeSemanticInFlight{0};
    bool m_quitSignalSent{false};
    std::optional<QJsonObject> m_pendingNativeDownload;
    QString m_downloadingNativeTag;
    struct PendingNativeRewrite {
        quint64 stateVersion;
        std::string intentId;
        QString profileId;
        quint64 configurationGeneration;
    };
    std::optional<PendingNativeRewrite> m_pendingNativeRewrite;
    QString m_juliaModelDirectory;
    QString m_juliaPython;
    QString m_juliaDevice;
    QHash<quint64, PendingJuliaScore> m_pendingJulia;
    QHash<quint64, PipelineTicket> m_pendingPromptTriage;
    std::optional<PipelineTicket> m_promptTriageLoad;
    std::optional<core::analysis::PromptTriage> m_promptTriage;
    core::analysis::ReviewTrigger m_reviewTrigger{core::analysis::ReviewTrigger::Automatic};
    core::analysis::PromptTriageRoute m_promptTriageLastRoute{core::analysis::PromptTriageRoute::Original};
    std::shared_ptr<AnalysisGroup> m_routedAnalysisGroup;
    QString m_recommendedOption;
    double m_recommendedConfidence{0.0};
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    bool m_promptTriageRouting{true};
#endif
    static constexpr int kAnalysisGroupTimeoutMs = 20000;
    int m_analysisGroupTimeoutMs{kAnalysisGroupTimeoutMs};
    QString m_projectScanPath;
    QStringList m_projectFiles;
    quint64 m_projectScanSerial{0};
    bool m_codebaseContextConsented{false};
    QString m_codebaseActiveFile;
    QString m_codebaseIndexPathOverride;
    std::optional<storage::CodebaseIndexLimits> m_codebaseRefreshLimitsForTesting;
    int m_codebaseRefreshSerial{0};
    bool m_codebaseRefreshInFlight{false};
    bool m_codebaseIndexReady{false};
    int m_codebaseIncompleteRetries{0};
    int m_codebasePathsRetries{0};
    std::unique_ptr<CodebaseWatcher> m_codebaseWatcher;
    quint64 m_codebaseWatcherSerial{0};
    QStringList m_pendingCodebasePaths;
    quint64 m_codebasePathsCoalesceSerial{0};
    bool m_codebaseFullRefreshQueued{false};
    int m_codebaseWatcherRecoveryAttempts{0};
    bool m_codebaseWatcherDeliberateRecovery{false};
    bool m_codebaseWatcherDegraded{false};
    quint64 m_codebaseWatcherRecoverySerial{0};
    int m_codebaseRefreshStartsForTesting{0};
    std::uint64_t m_codebaseSnapshotGeneration{0};
    std::uint64_t m_workspaceEpoch{0};
    std::uint64_t m_proposalWorkspaceEpoch{0};
    QHash<QString, int> m_staleRecoveryAttempts;
    std::optional<CodebaseWatcher::Limits> m_codebaseWatcherLimitsForTesting;
    QString m_modelDirectory;
    std::shared_ptr<bridge::GraniteAdapter> m_graniteAdapter;
    std::optional<OllamaClient> m_ollama;

    std::uint64_t m_pipelineSerial{0};
    std::uint64_t m_configurationGeneration{0};
    int m_inFlight{0};
    bool m_discoveryQueued{false};

    AnalysisStatus m_analysisStatus{AnalysisStatus::Idle};
    QString m_analysisMessage;
    std::optional<core::analysis::NormalizedAnalysis> m_currentAnalysis;
    std::optional<core::analysis::AnalysisSnapshot> m_currentSnapshot;
    core::analysis::OverallPolicyResult m_currentPolicyResult;
    bridge::SnapshotEvaluationResponse m_currentSnapshotEval;
    core::intent::InterventionDecision m_currentDecision;
    core::intent::WorkingIntentState m_workingIntent;
    QString m_activeQuestion;
    QString m_activeDimension;
    QVariantList m_activeAlternatives;
    std::optional<double> m_activeScore;

    std::optional<core::intent::ConfirmedIntent> m_confirmedIntent;
    QString m_rewriteProfile{QStringLiteral("GENERIC@1")};

    ProposalUiStatus m_proposalStatus{ProposalUiStatus::None};
    QString m_proposalMessage;
    QString m_streamingPreview;
    std::optional<core::rewrite::RewriteProposal> m_currentProposal;
    std::set<std::string> m_acknowledgedConstraints;
    std::optional<core::review::ApprovalRecord> m_currentApproval;
    std::vector<GroundedEvidenceView> m_activeGroundedEvidence;
    std::uint64_t m_activeGroundingGeneration{0};
    std::string m_activeGroundingPrompt;
    std::string m_activeGroundingActiveFile;
    std::string m_activeGroundingProposalId;
    std::string m_activeGroundingSessionId;
    std::string m_activeGroundingRevisionId;
    quint64 m_groundingValidationSerial{0};
    QVariantList m_groundedEvidenceViews;
    QString m_groundingStatus{QStringLiteral("UNAVAILABLE")};
    double m_lastGroundingApplyMs{0.0};

    std::unique_ptr<IAccessibilityBridge> m_accessibility;
    bool m_ambientModeEnabled{false};
    CaptureSource m_captureSource{CaptureSource::Widget};
    std::optional<AmbientObservation> m_ambientObservation;
    std::optional<AmbientObservation> m_captureTarget;
    bool m_followAmbientDraft{false};
    bool m_widgetDraftEdited{false};
    quint64 m_captureOpenGeneration{0};
    bool m_captureOpenPending{false};
    bool m_selectingInsertionTarget{false};
    QString m_captureRequestedApp;
    std::optional<AmbientCheck> m_ambientCheck;
    std::set<std::string> m_ambientDismissed;
    bool m_ambientCheckInFlight{false};
    bool m_ambientCheckPending{false};
    QTimer m_ambientDebounce;
    bool m_ambientBadgeVisible{false};
    QPoint m_ambientBadgePos;
    QString m_ambientBadgeLabel;
    QString m_ambientStatus;
    std::optional<PendingDelivery> m_pendingDelivery;
    quint64 m_nextDeliveryId{0};

    bool m_healthCheckInFlight{false};
    bool m_healthRecheckRequested{false};
    QString m_modelStatusDetail;
    QVariantList m_availableModels;
    QString m_activeModel{NativeGraniteRuntime::modelTag(QStringLiteral("Q8_0"))};
    bool m_isDownloadingModel{false};
    int m_downloadProgress{0};
    QString m_downloadStatusMessage;
    std::unique_ptr<QProcess> m_downloadProcess;

#ifdef ROCLINT_ENABLE_TEST_MOCKS
    bridge::GraniteTransport m_transportOverride;
#endif
};

} // namespace roclint::host
