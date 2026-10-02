#pragma once

#include <QObject>

#include <memory>

class QSystemTrayIcon;
class QMenu;
class QAction;

namespace cadroc::host {

class HostController;

class TrayManager : public QObject {
    Q_OBJECT

public:
    explicit TrayManager(HostController* controller, QObject* parent = nullptr);
    ~TrayManager() override;

    bool initTray();

private slots:
    void onTrayIconActivated(int reason);

private:
    void createActions();
    void createTrayIcon();

    HostController* m_controller{nullptr};
    QSystemTrayIcon* m_trayIcon{nullptr};
    std::unique_ptr<QMenu> m_trayMenu; // QSystemTrayIcon does not take ownership
    QAction* m_openAction{nullptr};
    QAction* m_quitAction{nullptr};
};

} // namespace cadroc::host
