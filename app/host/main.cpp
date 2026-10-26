#include "HostController.hpp"
#include "TrayManager.hpp"
#include "GlobalShortcutManager.hpp"
#include "ScreenPlacement.hpp"
#include "roclint/core/version.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char* argv[]) {
    bool dryRun = false;
    bool autoShow = false;
    int customWidth = 0;
    int customHeight = 0;
    QString screenshotPath;
    bool autoShowDashboard = false;
    int dashboardTab = 0;
    QString dashboardScreenshotPath;
    double dashboardScroll = 0.0;
    bool dashboardMaximized = false;
    bool pinCard = false;
    QString databaseOverride;
    QString targetScope;
    QString workspacePath;
    QString targetFile;
    QString initialPrompt;
    int resolveChoice = 0;
    bool openFileDropdown = false;
    bool autoEnhance = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--dry-run" || arg == "--smoke-test") {
            dryRun = true;
        }
        if (arg == "--db" && i + 1 < argc) {
            databaseOverride = QString::fromLocal8Bit(argv[++i]);
        }
        if (arg == "--dashboard-maximized") {
            dashboardMaximized = true;
            autoShowDashboard = true;
        }
        if (arg == "--pin") {
            pinCard = true;
        }
        if (arg == "--show") {
            autoShow = true;
        }
        if (arg == "--scope" && i + 1 < argc) targetScope = QString::fromLocal8Bit(argv[++i]);
        if (arg == "--workspace" && i + 1 < argc) workspacePath = QString::fromLocal8Bit(argv[++i]);
        if (arg == "--file" && i + 1 < argc) targetFile = QString::fromLocal8Bit(argv[++i]);
        if (arg == "--prompt" && i + 1 < argc) { initialPrompt = QString::fromLocal8Bit(argv[++i]); autoShow = true; }
        if (arg == "--resolve" && i + 1 < argc) { resolveChoice = std::atoi(argv[++i]); autoShow = true; }
        if (arg == "--open-file-dropdown") { openFileDropdown = true; autoShow = true; }
        if (arg == "--enhance") { autoEnhance = true; autoShow = true; }
        if (arg == "--dashboard" || arg == "--show-dashboard") {
            autoShowDashboard = true;
        }
        if (arg == "--dashboard-tab" && i + 1 < argc) {
            dashboardTab = std::atoi(argv[++i]);
        }
        if (arg == "--dashboard-scroll" && i + 1 < argc) {
            dashboardScroll = std::atof(argv[++i]);
        }
        if (arg == "--dashboard-screenshot" && i + 1 < argc) {
            dashboardScreenshotPath = QString::fromLocal8Bit(argv[++i]);
            autoShowDashboard = true;
        }
        if (arg == "--width" && i + 1 < argc) {
            customWidth = std::atoi(argv[++i]);
        }
        if (arg == "--height" && i + 1 < argc) {
            customHeight = std::atoi(argv[++i]);
        }
        if (arg == "--screenshot" && i + 1 < argc) {
            screenshotPath = QString::fromLocal8Bit(argv[++i]);
            autoShow = true;
        }
    }

    if (!targetScope.isEmpty() && targetScope != QLatin1String("global") && targetScope != QLatin1String("local")) {
        std::cerr << "[Roclint Host] --scope must be global or local.\n"; return 2;
    }
    if (!workspacePath.isEmpty() && !QFileInfo(workspacePath).isDir()) {
        std::cerr << "[Roclint Host] --workspace must name an existing directory.\n"; return 2;
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Roclint"));
    const std::string_view coreVersion = roclint::core::get_version_string();
    app.setApplicationVersion(QString::fromUtf8(coreVersion.data(), static_cast<qsizetype>(coreVersion.size())));
    app.setOrganizationName(QStringLiteral("roclint"));
    app.setQuitOnLastWindowClosed(false);
    roclint::host::configureHostProcess();

    // One resident host per user: a second instance would fight over the hotkey and the database.
    std::unique_ptr<QLockFile> instanceLock;
    if (!dryRun) {
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(dataDir);
        instanceLock = std::make_unique<QLockFile>(dataDir + QStringLiteral("/roclint-host.lock"));
        if (!instanceLock->tryLock(100)) {
            if (instanceLock->removeStaleLockFile() && instanceLock->tryLock(100)) {
                std::cout << "[Roclint Host] Recovered stale lock file.\n";
            } else {
                std::cerr << "[Roclint Host] Another Roclint instance is already running. Exiting.\n";
                return 0;
            }
        }
    }

    if (dryRun && databaseOverride.isEmpty()) databaseOverride = QStringLiteral(":memory:");
    auto controller = std::make_unique<roclint::host::HostController>(
        databaseOverride, dryRun ? roclint::host::createUnavailableAccessibilityBridge()
                                : roclint::host::createPlatformAccessibilityBridge());
    QObject::connect(controller.get(), &roclint::host::HostController::quitRequested,
                     &app, &QCoreApplication::quit, Qt::QueuedConnection);

    auto tray = std::make_unique<roclint::host::TrayManager>(controller.get());
    if (!dryRun && !tray->initTray()) {
        std::cerr << "[Roclint Host] Warning: System tray is not available on this environment.\n";
    }
    QObject::connect(controller.get(), &roclint::host::HostController::deliveryFellBack, tray.get(),
                     [trayPtr = tray.get()](const QString& message) { trayPtr->notify(message); });
    QObject::connect(controller.get(), &roclint::host::HostController::insertionTargetSelectionRequested, tray.get(),
                     [trayPtr = tray.get()](const QString& message) { trayPtr->notify(message); });

    auto shortcutManager = std::make_unique<roclint::host::GlobalShortcutManager>(controller.get());
    if (!dryRun && !shortcutManager->registerShortcut()) {
        controller->setShortcutRegistrationFailed(
            QStringLiteral("The %1 shortcut couldn't be registered (another app may be using it). "
                           "Open Roclint from the tray icon instead.")
                .arg(controller->shortcutLabel()));
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), controller.get());

    const QUrl cardUrl(QStringLiteral("qrc:/shell/FloatingCard.qml"));
    const QUrl dashboardUrl(QStringLiteral("qrc:/shell/DashboardWindow.qml"));
    const QUrl badgeUrl(QStringLiteral("qrc:/shell/AmbientBadge.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [cardUrl, dashboardUrl, badgeUrl, controller = controller.get(), autoShow, autoShowDashboard, dryRun, screenshotPath, dashboardScreenshotPath, dashboardScroll, customWidth, customHeight, dashboardTab, pinCard, dashboardMaximized, targetScope, workspacePath, targetFile, resolveChoice, openFileDropdown, autoEnhance, initialPrompt](QObject* obj, const QUrl& objUrl) {
        if (!obj && (cardUrl == objUrl || dashboardUrl == objUrl || badgeUrl == objUrl)) {
            std::cerr << "[Roclint Host] Error: Failed to load QML root object from " << objUrl.toString().toStdString() << std::endl;
            QCoreApplication::exit(-1);
            return;
        }

        auto* window = qobject_cast<QQuickWindow*>(obj);
        if (!window) return;

        if (objUrl == cardUrl) {
            roclint::host::configureNonActivatingPanel(window);
            if (customWidth > 0) {
                window->setWidth(customWidth);
            }
            if (customHeight > 0) {
                window->setHeight(customHeight);
            }
            QObject::connect(controller, &roclint::host::HostController::showCardRequested,
                             window, [window, controller](int x, int y) {
                if (x >= 0 && y >= 0) {
                    window->setPosition(x, y);
                } else {
                    const auto placement = roclint::host::ScreenPlacement::determineTargetPosition(window->width(), window->height());
                    window->setPosition(placement.x, placement.y);
                }
                // showNormal() also restores a minimized card; show() keeps the minimized state
                window->showNormal();
                window->raise();
                if (!controller->isAmbientActive() && !controller->isAmbientAttached()) {
                    window->requestActivate();
                } else {
                    roclint::host::installOutsideClickMonitor(window, [controller]() {
                        if (controller->isAmbientAttached()) {
                            controller->hideCard();
                        }
                    });
                }
            });

            QObject::connect(controller, &roclint::host::HostController::hideCardRequested,
                             window, [window]() {
                roclint::host::uninstallOutsideClickMonitor();
                window->hide();
            });

            // If closed via window controls or system event, hide instead of quitting
            QObject::connect(window, &QQuickWindow::closing,
                             controller, &roclint::host::HostController::hideCard);

            // Keep the controller's typed visibility in sync with changes the window
            // system makes on its own (taskbar minimize/restore, focus loss)
            const auto syncVisibility = [window, controller]() {
                using roclint::host::CardVisibility;
                if (!window->isVisible() || window->visibility() == QWindow::Minimized) {
                    controller->syncCardVisibility(CardVisibility::Hidden);
                } else {
                    controller->syncCardVisibility(window->isActive() ? CardVisibility::Focused
                                                                      : CardVisibility::Visible);
                }
            };
            QObject::connect(window, &QWindow::visibilityChanged, controller, syncVisibility);
            QObject::connect(window, &QWindow::activeChanged, controller, syncVisibility);

            if (pinCard) {
                window->setProperty("pinToTop", true);
            }

            if (autoShow && !dryRun) {
                controller->showCard(); window->showNormal(); window->raise(); window->requestActivate();
            }

            if (!workspacePath.isEmpty()) {
                const QFileInfo workspace(workspacePath);
                window->setProperty("projectList", QVariantList{QVariantMap{{QStringLiteral("name"), workspace.fileName()},
                    {QStringLiteral("path"), workspace.canonicalFilePath()}}});
                window->setProperty("currentProjectIndex", 0);
                QMetaObject::invokeMethod(window, "refreshProjectFiles");
            }
            if (!targetScope.isEmpty()) window->setProperty("activeScope", targetScope);
            if (!targetFile.isEmpty()) window->setProperty("targetLocalFile", targetFile);
            if (openFileDropdown) QTimer::singleShot(0, window, [window]() { QMetaObject::invokeMethod(window, "openFileDropdown"); });
            if (!targetScope.isEmpty() && !dryRun)
                QTimer::singleShot(0, window, [window]() { QMetaObject::invokeMethod(window, "applyScope"); });

            // Retain upstream automation flags with asynchronous, explicit-review semantics.
            // A CLI choice is applied only after analysis completes, never on a guessed delay.
            if (!dryRun && (resolveChoice > 0 || autoEnhance)) {
                const auto resolved = std::make_shared<bool>(false);
                QObject::connect(controller, &roclint::host::HostController::analysisStatusChanged, window,
                    [controller, window, resolved, resolveChoice, autoEnhance]() {
                        if (controller->analysisStatus() != QLatin1String("READY") || *resolved) return;
                        *resolved = true;
                        if (resolveChoice > 0 && controller->isClarifying()) {
                            QMetaObject::invokeMethod(window, "commitChoice", Q_ARG(QVariant, resolveChoice));
                        }
                        if (autoEnhance && controller->isRewriteEnabled() && controller->proposalStatus() == QLatin1String("NONE"))
                            controller->synthesizeRewrite(controller->stateVersion());
                    });
                QTimer::singleShot(0, window, [window]() { QMetaObject::invokeMethod(window, "analyzeNow"); });
            }

            if (!initialPrompt.isEmpty() && !dryRun) {
                QTimer::singleShot(150, window, [window, initialPrompt]() {
                    QMetaObject::invokeMethod(window, "setEditorText", Q_ARG(QVariant, initialPrompt));
                    QMetaObject::invokeMethod(window, "analyzeNow");
                });
            }

            if (!screenshotPath.isEmpty()) {
                QTimer::singleShot(800, window, [window, screenshotPath]() {
                    const QImage img = window->grabWindow();
                    if (img.save(screenshotPath)) {
                        std::cout << "[Roclint Host] Screenshot saved to " << screenshotPath.toStdString() << std::endl;
                    } else {
                        std::cerr << "[Roclint Host] Error saving screenshot to " << screenshotPath.toStdString() << std::endl;
                    }
                    QCoreApplication::quit();
                });
            }
        } else if (objUrl == dashboardUrl) {
            if (dashboardTab > 0) {
                window->setProperty("currentTab", dashboardTab);
            }
            QObject::connect(controller, &roclint::host::HostController::showDashboardRequested,
                             window, [window]() {
                if (window->x() <= 0 && window->y() <= 0) {
                    if (const auto* s = window->screen()) {
                        const QRect geom = s->availableGeometry();
                        const int cx = geom.x() + (geom.width() - window->width()) / 2;
                        const int cy = geom.y() + (geom.height() - window->height()) / 2;
                        window->setX(std::max(geom.x(), cx));
                        window->setY(std::max(geom.y(), cy));
                    }
                }
                window->showNormal();
                window->raise();
                window->requestActivate();
            });

            QObject::connect(controller, &roclint::host::HostController::hideDashboardRequested,
                             window, [window]() { window->hide(); });

            QObject::connect(window, &QQuickWindow::closing,
                             controller, &roclint::host::HostController::hideDashboard);

            QObject::connect(window, &QWindow::visibilityChanged, controller, [window, controller]() {
                controller->syncDashboardVisibility(window->isVisible() && window->visibility() != QWindow::Minimized);
            });

            if (autoShowDashboard && !dryRun) {
                if (dashboardMaximized) {
                    window->showMaximized();
                } else {
                    controller->showDashboard();
                }
            }

            if (!dashboardScreenshotPath.isEmpty()) {
                if (dashboardScroll > 0.0) {
                    QTimer::singleShot(200, window, [window, dashboardScroll]() {
                        QMetaObject::invokeMethod(window, "scrollToPosition", Q_ARG(QVariant, dashboardScroll));
                    });
                }
                QTimer::singleShot(800, window, [window, dashboardScreenshotPath]() {
                    const QImage img = window->grabWindow();
                    if (img.save(dashboardScreenshotPath)) {
                        std::cout << "[Roclint Host] Dashboard screenshot saved to " << dashboardScreenshotPath.toStdString() << std::endl;
                    } else {
                        std::cerr << "[Roclint Host] Error saving dashboard screenshot to " << dashboardScreenshotPath.toStdString() << std::endl;
                    }
                    QCoreApplication::quit();
                });
            }
        } else if (objUrl == badgeUrl) {
            roclint::host::configureNonActivatingPanel(window);
            QObject::connect(controller, &roclint::host::HostController::ambientBadgeChanged,
                             window, [window, controller]() {
                if (controller->isAmbientBadgeVisible()) {
                    window->setPosition(controller->ambientBadgeX(), controller->ambientBadgeY());
                    if (!window->isVisible()) {
                        window->show();
                    }
                } else {
                    if (window->isVisible()) {
                        window->hide();
                    }
                }
            });
        }
    });

    engine.load(cardUrl);
    engine.load(dashboardUrl);
    engine.load(badgeUrl);

    if (engine.rootObjects().size() != 3) {
        std::cerr << "[Roclint Host] Error: One or more required QML screens failed to load" << std::endl;
        return 1;
    }

    controller->markResident();

    if (dryRun) {
        std::cout << "[Roclint Host] Version: " << app.applicationVersion().toStdString() << std::endl;
        std::cout << "[Roclint Host] Core Version: " << coreVersion << std::endl;
        std::cout << "[Roclint Host] Host State: " << controller->hostStateString().toStdString() << std::endl;
        std::cout << "[Roclint Host] Qt Version: " << qVersion() << std::endl;
        std::cout << "[Roclint Host] Dry-run passed. QML root object verified. Exiting cleanly." << std::endl;
        return 0;
    }

    // Model discovery runs on a worker thread; the UI is usable while it completes.
    if (!dryRun) {
        controller->refreshAvailableModels();
        controller->refreshJuliaModel();
    }

    std::cout << "[Roclint Host] Resident background host started. Use System Tray -> Quit to terminate." << std::endl;
    return app.exec();
}
