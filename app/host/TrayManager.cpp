#include "TrayManager.hpp"
#include "HostController.hpp"

#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace cadroc::host {

TrayManager::TrayManager(HostController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
}

TrayManager::~TrayManager() {
    if (m_trayIcon) {
        m_trayIcon->hide();
        // m_trayMenu is destroyed before the icon (a QObject child), so detach it first.
        m_trayIcon->setContextMenu(nullptr);
    }
}

bool TrayManager::initTray() {
    if (m_trayIcon) {
        return true;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return false;
    }

    createActions();
    createTrayIcon();

    m_trayIcon->show();
    return true;
}

void TrayManager::createActions() {
    m_trayMenu = std::make_unique<QMenu>();

    m_openAction = m_trayMenu->addAction(QStringLiteral("Open Cadroc"));
    connect(m_openAction, &QAction::triggered, m_controller, &HostController::showCard);

    m_trayMenu->addSeparator();

    m_quitAction = m_trayMenu->addAction(QStringLiteral("Quit"));
    connect(m_quitAction, &QAction::triggered, m_controller, &HostController::requestQuit);
}

void TrayManager::createTrayIcon() {
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setContextMenu(m_trayMenu.get());
    m_trayIcon->setToolTip(QStringLiteral("Cadroc - Resident Host"));

    // Programmatically drawn crisp vector icon fallback
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor(80, 140, 240));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(4, 4, 24, 24);
        painter.setBrush(Qt::white);
        painter.drawEllipse(12, 12, 8, 8);
    }
    m_trayIcon->setIcon(QIcon(pixmap));

    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &TrayManager::onTrayIconActivated);
}

void TrayManager::onTrayIconActivated(int reason) {
    if (!m_controller) {
        return;
    }
    // Windows reports a double-click as Trigger followed by DoubleClick. Toggling on
    // both would show and immediately re-hide the card, so DoubleClick only shows.
    if (reason == QSystemTrayIcon::Trigger) {
        m_controller->toggleCard();
    } else if (reason == QSystemTrayIcon::DoubleClick) {
        m_controller->showCard();
    }
}

} // namespace cadroc::host
