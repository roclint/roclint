#include "HostController.hpp"
#include <QCoreApplication>

namespace cadroc::host {

HostController::HostController(QObject* parent)
    : QObject(parent),
      m_hostState(HostState::Resident),
      m_cardVisibility(CardVisibility::Hidden) {
}

HostState HostController::hostState() const noexcept {
    return m_hostState;
}

QString HostController::hostStateString() const {
    switch (m_hostState) {
        case HostState::Starting: return QStringLiteral("STARTING");
        case HostState::Resident: return QStringLiteral("RESIDENT");
        case HostState::Quitting: return QStringLiteral("QUITTING");
    }
    return QStringLiteral("UNKNOWN");
}

CardVisibility HostController::cardVisibility() const noexcept {
    return m_cardVisibility;
}

QString HostController::cardVisibilityString() const {
    switch (m_cardVisibility) {
        case CardVisibility::Hidden: return QStringLiteral("HIDDEN");
        case CardVisibility::Visible: return QStringLiteral("VISIBLE");
        case CardVisibility::Focused: return QStringLiteral("FOCUSED");
    }
    return QStringLiteral("UNKNOWN");
}

bool HostController::isCardVisible() const noexcept {
    return m_cardVisibility != CardVisibility::Hidden;
}

void HostController::showCard() {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    // Invariant: repeated show calls focus the single logical card instance
    m_cardVisibility = CardVisibility::Focused;
    emit cardVisibilityChanged();
    emit showCardRequested();
}

void HostController::hideCard() {
    if (m_cardVisibility == CardVisibility::Hidden) {
        return;
    }
    m_cardVisibility = CardVisibility::Hidden;
    emit cardVisibilityChanged();
    emit hideCardRequested();
    // Invariant: hiding/closing the card keeps m_hostState at Resident
}

void HostController::toggleCard() {
    if (isCardVisible()) {
        hideCard();
    } else {
        showCard();
    }
}

void HostController::requestQuit() {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    m_hostState = HostState::Quitting;
    m_cardVisibility = CardVisibility::Hidden;
    emit cardVisibilityChanged();
    emit hostStateChanged();
    emit quitRequested();
    QCoreApplication::quit();
}

} // namespace cadroc::host
