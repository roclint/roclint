#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/storage/Database.hpp"
#include "roclint/storage/PromptRepository.hpp"
#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "HostContext.hpp"

namespace roclint::host {

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

class HostController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isCardVisible READ isCardVisible NOTIFY cardVisibilityChanged)
    Q_PROPERTY(bool isDashboardVisible READ isDashboardVisible NOTIFY dashboardVisibilityChanged)
    Q_PROPERTY(QString hostStateString READ hostStateString NOTIFY hostStateChanged)
    Q_PROPERTY(QString cardVisibilityString READ cardVisibilityString NOTIFY cardVisibilityChanged)
    Q_PROPERTY(quint64 stateVersion READ stateVersion NOTIFY stateVersionChanged)
    Q_PROPERTY(QString currentRevisionId READ currentRevisionId NOTIFY stateVersionChanged)
    Q_PROPERTY(QString lastCopiedText READ lastCopiedText NOTIFY lastCopiedTextChanged)
    Q_PROPERTY(bool hasHostContext READ hasHostContext NOTIFY hostContextChanged)
    Q_PROPERTY(QString hostContextCategory READ hostContextCategory NOTIFY hostContextChanged)
    Q_PROPERTY(QString hostContextDisplayName READ hostContextDisplayName NOTIFY hostContextChanged)
    Q_PROPERTY(QString capturedDraftText READ capturedDraftText NOTIFY capturedDraftTextChanged)
    Q_PROPERTY(QString databasePath READ databasePath NOTIFY storageChanged)
    Q_PROPERTY(QString databaseStats READ databaseStats NOTIFY storageChanged)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionListChanged)
    Q_PROPERTY(QString interventionVerdict READ interventionVerdict NOTIFY interventionChanged)
    Q_PROPERTY(QString interventionReason READ interventionReason NOTIFY interventionChanged)
    Q_PROPERTY(bool isClarifying READ isClarifying NOTIFY interventionChanged)
    Q_PROPERTY(bool isManual READ isManual NOTIFY interventionChanged)
    Q_PROPERTY(QString activeItemId READ activeItemId NOTIFY interventionChanged)
    Q_PROPERTY(QString activeQuestion READ activeQuestion NOTIFY interventionChanged)
    Q_PROPERTY(QString activeDimension READ activeDimension NOTIFY interventionChanged)
    Q_PROPERTY(QVariantList activeAlternatives READ activeAlternatives NOTIFY interventionChanged)
    Q_PROPERTY(int roundsCompleted READ roundsCompleted NOTIFY interventionChanged)
    Q_PROPERTY(int remainingBudget READ remainingBudget NOTIFY interventionChanged)
    Q_PROPERTY(double ambiguityScore READ ambiguityScore NOTIFY interventionChanged)
    Q_PROPERTY(QString diagnosticMessage READ diagnosticMessage NOTIFY interventionChanged)
    Q_PROPERTY(bool hasConfirmedIntent READ hasConfirmedIntent NOTIFY intentChanged)
    Q_PROPERTY(bool isIntentStale READ isIntentStale NOTIFY intentChanged)
    Q_PROPERTY(QString confirmedIntentSummary READ confirmedIntentSummary NOTIFY intentChanged)
    Q_PROPERTY(QVariantList confirmedItems READ confirmedItems NOTIFY intentChanged)
    Q_PROPERTY(QString rewriteProfile READ rewriteProfile WRITE setRewriteProfile NOTIFY rewriteProfileChanged)
    Q_PROPERTY(bool isRewriteEnabled READ isRewriteEnabled NOTIFY intentChanged)
    Q_PROPERTY(QString lastCompiledPrompt READ lastCompiledPrompt NOTIFY rewriteProposalChanged)
    Q_PROPERTY(QString lastDiffText READ lastDiffText NOTIFY rewriteProposalChanged)
    Q_PROPERTY(bool hasProposal READ hasProposal NOTIFY rewriteProposalChanged)
    Q_PROPERTY(QVariantList proposalConstraints READ proposalConstraints NOTIFY rewriteProposalChanged)

public:
    explicit HostController(QObject* parent = nullptr);
    explicit HostController(const QString& dbPathOverride, QObject* parent = nullptr);
    ~HostController() override = default;

    [[nodiscard]] HostState hostState() const noexcept;
    [[nodiscard]] QString hostStateString() const;

    [[nodiscard]] CardVisibility cardVisibility() const noexcept;
    [[nodiscard]] QString cardVisibilityString() const;
    [[nodiscard]] bool isCardVisible() const noexcept;
    [[nodiscard]] bool isDashboardVisible() const noexcept;

    [[nodiscard]] quint64 stateVersion() const noexcept;
    [[nodiscard]] QString currentRevisionId() const;
    [[nodiscard]] QString lastCopiedText() const;

    [[nodiscard]] bool hasHostContext() const noexcept;
    [[nodiscard]] QString hostContextCategory() const;
    [[nodiscard]] QString hostContextDisplayName() const;
    [[nodiscard]] QString capturedDraftText() const;

    [[nodiscard]] QString databasePath() const;
    [[nodiscard]] QString databaseStats() const;
    [[nodiscard]] int sessionCount() const;

    [[nodiscard]] QString interventionVerdict() const;
    [[nodiscard]] QString interventionReason() const;
    [[nodiscard]] bool isClarifying() const noexcept;
    [[nodiscard]] bool isManual() const noexcept;
    [[nodiscard]] QString activeItemId() const;
    [[nodiscard]] QString activeQuestion() const;
    [[nodiscard]] QString activeDimension() const;
    [[nodiscard]] QVariantList activeAlternatives() const;
    [[nodiscard]] int roundsCompleted() const noexcept;
    [[nodiscard]] int remainingBudget() const noexcept;
    [[nodiscard]] double ambiguityScore() const noexcept;
    [[nodiscard]] QString diagnosticMessage() const;

    [[nodiscard]] bool hasConfirmedIntent() const noexcept;
    [[nodiscard]] bool isIntentStale() const noexcept;
    [[nodiscard]] QString confirmedIntentSummary() const;
    [[nodiscard]] QVariantList confirmedItems() const;
    [[nodiscard]] QString rewriteProfile() const;
    [[nodiscard]] bool isRewriteEnabled() const noexcept;
    [[nodiscard]] QString lastCompiledPrompt() const;
    [[nodiscard]] QString lastDiffText() const;
    [[nodiscard]] bool hasProposal() const noexcept;
    [[nodiscard]] QVariantList proposalConstraints() const;

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

    void copyToClipboard(const QString& text);
    void commitPrompt(const QString& text);
    void hardBypass(const QString& text);

    void syncCardVisibility(roclint::host::CardVisibility visibility);
    void syncDashboardVisibility(bool visible);

    Q_INVOKABLE QVariantList getRecentSessions();
    Q_INVOKABLE QVariantList getSessionHistory(const QString& sessionId);
    Q_INVOKABLE void restoreRevision(const QString& revisionId);
    Q_INVOKABLE void restoreSession(const QString& sessionId);
    Q_INVOKABLE void deleteSession(const QString& sessionId);
    Q_INVOKABLE void purgeHistory();
    Q_INVOKABLE void refreshStorageStats();

    Q_INVOKABLE void analyzePrompt(const QString& text);
    Q_INVOKABLE void confirmChoice(const QString& alternativeId);
    Q_INVOKABLE void submitCustomIntent(const QString& customText);
    Q_INVOKABLE void skipCurrentItem();
    Q_INVOKABLE void switchToManual();
    Q_INVOKABLE void confirmLooksRight();
    Q_INVOKABLE void keepOriginalPrompt();
    Q_INVOKABLE void invalidateIntent();
    Q_INVOKABLE void setRewriteProfile(const QString& profile);
    Q_INVOKABLE QVariantMap getConfirmedIntentPayload() const;
    Q_INVOKABLE QString synthesizeRewrite(const QString& promptOverride = "");

signals:
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
    void promptCopied(const QString& text);
    void hostContextChanged();
    void capturedDraftTextChanged();
    void draftCaptured(const QString& text);
    void storageChanged();
    void sessionListChanged();
    void revisionRestored(const QString& text);
    void interventionChanged();
    void choiceConfirmed(const QString& groupId, const QString& alternativeId);
    void intentChanged();
    void rewriteProfileChanged(const QString& profile);
    void rewriteProposalChanged();
    void rewriteSynthesized(const QString& compiledPrompt, const QString& diffText);

private:
    bool setCardVisibility(CardVisibility visibility);
    bool setDashboardVisibility(bool visible);
    void initStorage(const QString& dbPathOverride);

    HostState m_hostState{HostState::Starting};
    CardVisibility m_cardVisibility{CardVisibility::Hidden};
    bool m_dashboardVisible{false};
    std::unique_ptr<roclint::core::RevisionState> m_revisionState;
    QString m_lastCopiedText;
    HostContextObservation m_hostContext;
    QString m_capturedDraftText;

    std::unique_ptr<roclint::storage::Database> m_database;
    std::unique_ptr<roclint::storage::PromptRepository> m_repository;
    QString m_databasePath;
    QString m_databaseStats;

    roclint::bridge::S1TriageAdapter m_s1Adapter;
    roclint::bridge::GraniteAdapter m_graniteAdapter;
    roclint::core::intent::WorkingIntentState m_workingIntent;
    roclint::core::analysis::NormalizedAnalysis m_currentAnalysis;
    roclint::core::analysis::OverallPolicyResult m_currentPolicyResult;
    roclint::bridge::SnapshotEvaluationResponse m_currentSnapshotEval;
    roclint::core::intent::InterventionDecision m_currentDecision;
    QString m_activeQuestion;
    QString m_activeDimension;
    QVariantList m_activeAlternatives;
    double m_ambiguityScore{0.0};

    std::optional<roclint::core::intent::ConfirmedIntent> m_confirmedIntent;
    QString m_rewriteProfile{QStringLiteral("GENERIC@1")};
    QString m_customInstructions;
    void rebuildConfirmedIntent();

    roclint::bridge::GraniteRewriteAdapter m_rewriteAdapter;
    std::optional<roclint::core::rewrite::RewriteProposal> m_currentProposal;
};

} // namespace roclint::host
