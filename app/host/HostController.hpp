#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include "roclint/core/prompt_state/RevisionState.hpp"
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

public:
    explicit HostController(QObject* parent = nullptr);
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

public slots:
    void markResident();
    void showCard();
    void hideCard();
    void toggleCard();
    void showDashboard();
    void hideDashboard();
    void toggleDashboard();
    void requestQuit();

    // Host Context actions (B02H)
    void clearHostContext();

    // Explicit one-shot clipboard capture (B02)
    void captureClipboardDraft();

    // Prompt state & clipboard actions
    void copyToClipboard(const QString& text);
    void commitPrompt(const QString& text);
    void hardBypass(const QString& text);

    // Records a visibility change the window system made on its own (taskbar
    // minimize/restore, focus loss). Updates state without emitting show/hide requests.
    void syncCardVisibility(roclint::host::CardVisibility visibility);
    void syncDashboardVisibility(bool visible);

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

private:
    bool setCardVisibility(CardVisibility visibility);
    bool setDashboardVisibility(bool visible);

    HostState m_hostState{HostState::Starting};
    CardVisibility m_cardVisibility{CardVisibility::Hidden};
    bool m_dashboardVisible{false};
    std::unique_ptr<roclint::core::RevisionState> m_revisionState;
    QString m_lastCopiedText;
    HostContextObservation m_hostContext;
    QString m_capturedDraftText;
};

} // namespace roclint::host
