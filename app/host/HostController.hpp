#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include "roclint/core/prompt_state/RevisionState.hpp"

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
    Q_PROPERTY(QString hostStateString READ hostStateString NOTIFY hostStateChanged)
    Q_PROPERTY(QString cardVisibilityString READ cardVisibilityString NOTIFY cardVisibilityChanged)
    Q_PROPERTY(quint64 stateVersion READ stateVersion NOTIFY stateVersionChanged)
    Q_PROPERTY(QString currentRevisionId READ currentRevisionId NOTIFY stateVersionChanged)
    Q_PROPERTY(QString lastCopiedText READ lastCopiedText NOTIFY lastCopiedTextChanged)

public:
    explicit HostController(QObject* parent = nullptr);
    ~HostController() override = default;

    [[nodiscard]] HostState hostState() const noexcept;
    [[nodiscard]] QString hostStateString() const;

    [[nodiscard]] CardVisibility cardVisibility() const noexcept;
    [[nodiscard]] QString cardVisibilityString() const;
    [[nodiscard]] bool isCardVisible() const noexcept;

    [[nodiscard]] quint64 stateVersion() const noexcept;
    [[nodiscard]] QString currentRevisionId() const;
    [[nodiscard]] QString lastCopiedText() const;

public slots:
    void markResident();
    void showCard();
    void hideCard();
    void toggleCard();
    void requestQuit();

    // Prompt state & clipboard actions
    void copyToClipboard(const QString& text);
    void commitPrompt(const QString& text);
    void hardBypass(const QString& text);

    // Records a visibility change the window system made on its own (taskbar
    // minimize/restore, focus loss). Updates state without emitting show/hide requests.
    void syncCardVisibility(roclint::host::CardVisibility visibility);

signals:
    void cardVisibilityChanged();
    void hostStateChanged();
    void showCardRequested();
    void hideCardRequested();
    void quitRequested();
    void stateVersionChanged();
    void lastCopiedTextChanged();
    void promptCopied(const QString& text);

private:
    bool setCardVisibility(CardVisibility visibility);

    HostState m_hostState{HostState::Starting};
    CardVisibility m_cardVisibility{CardVisibility::Hidden};
    std::unique_ptr<roclint::core::RevisionState> m_revisionState;
    QString m_lastCopiedText;
};

} // namespace roclint::host
