#pragma once

#include <QObject>
#include <QString>

namespace cadroc::host {

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

public:
    explicit HostController(QObject* parent = nullptr);
    ~HostController() override = default;

    [[nodiscard]] HostState hostState() const noexcept;
    [[nodiscard]] QString hostStateString() const;

    [[nodiscard]] CardVisibility cardVisibility() const noexcept;
    [[nodiscard]] QString cardVisibilityString() const;
    [[nodiscard]] bool isCardVisible() const noexcept;

public slots:
    void markResident();
    void showCard();
    void hideCard();
    void toggleCard();
    void requestQuit();

    // Records a visibility change the window system made on its own (taskbar
    // minimize/restore, focus loss). Updates state without emitting show/hide requests.
    void syncCardVisibility(cadroc::host::CardVisibility visibility);

signals:
    void cardVisibilityChanged();
    void hostStateChanged();
    void showCardRequested();
    void hideCardRequested();
    void quitRequested();

private:
    bool setCardVisibility(CardVisibility visibility);

    HostState m_hostState{HostState::Starting};
    CardVisibility m_cardVisibility{CardVisibility::Hidden};
};

} // namespace cadroc::host
