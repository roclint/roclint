#include "HostController.hpp"

namespace roclint::host {

HostController::HostController(QObject* parent)
    : QObject(parent) {
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

void HostController::markResident() {
    if (m_hostState != HostState::Starting) {
        return;
    }
    m_hostState = HostState::Resident;
    emit hostStateChanged();
}

void HostController::showCard() {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    // Invariant: repeated show calls focus the single logical card instance
    setCardVisibility(CardVisibility::Focused);
    emit showCardRequested();
}

void HostController::hideCard() {
    if (!setCardVisibility(CardVisibility::Hidden)) {
        return;
    }
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
    hideCard();
    m_hostState = HostState::Quitting;
    emit hostStateChanged();
    // The application owner decides how to exit; the controller stays testable.
    emit quitRequested();
}

void HostController::syncCardVisibility(CardVisibility visibility) {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    setCardVisibility(visibility);
}

bool HostController::setCardVisibility(CardVisibility visibility) {
    if (m_cardVisibility == visibility) {
        return false;
    }
    m_cardVisibility = visibility;
    emit cardVisibilityChanged();
    return true;
}

} // namespace roclint::host
