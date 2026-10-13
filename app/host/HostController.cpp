#include "HostController.hpp"

#include <QGuiApplication>
#include <QClipboard>
#include <QDateTime>

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

quint64 HostController::stateVersion() const noexcept {
    return m_revisionState ? m_revisionState->stateVersion() : 0;
}

QString HostController::currentRevisionId() const {
    if (!m_revisionState) {
        return QStringLiteral("none");
    }
    return QString::fromStdString(m_revisionState->currentRevision().revisionId().value());
}

QString HostController::lastCopiedText() const {
    return m_lastCopiedText;
}

void HostController::copyToClipboard(const QString& text) {
    if (auto* cb = QGuiApplication::clipboard()) {
        cb->setText(text);
        m_lastCopiedText = text;
        emit lastCopiedTextChanged();
        emit promptCopied(text);
    }
}

void HostController::commitPrompt(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return;
    }
    const std::string textStd = text.toStdString();
    const auto nowMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_revisionState) {
        const std::string sessId = "sess_" + std::to_string(nowMs);
        core::PromptRevision root(
            core::SessionId(sessId),
            core::RevisionId("rev_1"),
            std::nullopt,
            textStd,
            nowMs
        );
        m_revisionState = std::make_unique<core::RevisionState>(root);
    } else {
        const auto nextNum = m_revisionState->stateVersion() + 1;
        const std::string nextId = "rev_" + std::to_string(nextNum);
        m_revisionState->commitEdit(
            m_revisionState->stateVersion(),
            core::RevisionId(nextId),
            textStd,
            static_cast<std::uint64_t>(nowMs)
        );
    }
    emit stateVersionChanged();
}

void HostController::hardBypass(const QString& text) {
    commitPrompt(text);
    copyToClipboard(text);
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
