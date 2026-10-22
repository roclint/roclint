#include "HostController.hpp"
#include "TrayManager.hpp"
#include "GlobalShortcutManager.hpp"
#include "roclint/core/version.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char* argv[]) {
    bool dryRun = false;
    bool autoShow = false;
    int customWidth = 0;
    int customHeight = 0;
    int resolveChoice = 0;
    bool openFileDropdown = false;
    QString targetScope;
    QString screenshotPath;
    bool autoShowDashboard = false;
    int dashboardTab = 0;
    QString dashboardScreenshotPath;
    bool dashboardMaximized = false;
    bool pinCard = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--dry-run" || arg == "--smoke-test") {
            dryRun = true;
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
        if (arg == "--dashboard") {
            autoShowDashboard = true;
        }
        if (arg == "--dashboard-tab" && i + 1 < argc) {
            dashboardTab = std::atoi(argv[++i]);
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
        if (arg == "--resolve" && i + 1 < argc) {
            resolveChoice = std::atoi(argv[++i]);
        }
        if (arg == "--open-file-dropdown") {
            openFileDropdown = true;
        }
        if (arg == "--scope" && i + 1 < argc) {
            targetScope = QString::fromLocal8Bit(argv[++i]);
        }
        if (arg == "--screenshot" && i + 1 < argc) {
            screenshotPath = QString::fromLocal8Bit(argv[++i]);
            autoShow = true;
        }
    }

    // 1. Initialize desktop application with GUI and Widget support
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Roclint"));
    // Single source of truth: the version CMake injects into roclint_core
    const std::string_view coreVersion = roclint::core::get_version_string();
    app.setApplicationVersion(QString::fromUtf8(coreVersion.data(), static_cast<qsizetype>(coreVersion.size())));
    app.setOrganizationName(QStringLiteral("roclint"));

    // 2. CRITICAL B00 CONTRACT: Closing windows must NOT quit the resident host
    app.setQuitOnLastWindowClosed(false);

    // 3. Create host controller
    auto controller = std::make_unique<roclint::host::HostController>();
    // Explicit Quit is the only path that ends the resident host (C08)
    QObject::connect(controller.get(), &roclint::host::HostController::quitRequested,
                     &app, &QCoreApplication::quit, Qt::QueuedConnection);

    // 4. Initialize system tray
    auto tray = std::make_unique<roclint::host::TrayManager>(controller.get());
    if (!tray->initTray()) {
        std::cerr << "[Roclint Host] Warning: System tray is not available on this environment.\n";
    }

    // 4b. Initialize global system hotkey (Cmd+Shift+L)
    auto shortcutManager = std::make_unique<roclint::host::GlobalShortcutManager>(controller.get());
    if (!shortcutManager->registerShortcut()) {
        std::cerr << "[Roclint Host] Warning: Global shortcut could not be registered.\n";
    }

    // 5. Initialize QML engine
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), controller.get());

    const QUrl cardUrl(QStringLiteral("qrc:/shell/FloatingCard.qml"));
    const QUrl dashboardUrl(QStringLiteral("qrc:/shell/DashboardWindow.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [cardUrl, dashboardUrl, controller = controller.get(), autoShow, autoShowDashboard, dryRun, screenshotPath, dashboardScreenshotPath, customWidth, customHeight, resolveChoice, openFileDropdown, targetScope, dashboardTab, pinCard, dashboardMaximized](QObject* obj, const QUrl& objUrl) {
        if (!obj && (cardUrl == objUrl || dashboardUrl == objUrl)) {
            std::cerr << "[Roclint Host] Error: Failed to load QML root object from " << objUrl.toString().toStdString() << std::endl;
            QCoreApplication::exit(-1);
            return;
        }

        auto* window = qobject_cast<QQuickWindow*>(obj);
        if (!window) return;

        if (objUrl == cardUrl) {
            std::cout << "[Roclint Host] FloatingCard window initialized successfully." << std::endl;
            if (customWidth > 0) {
                window->setWidth(customWidth);
            }
            if (customHeight > 0) {
                window->setHeight(customHeight);
            }
            if (!targetScope.isEmpty()) {
                window->setProperty("activeScope", targetScope);
            }
            QObject::connect(controller, &roclint::host::HostController::showCardRequested,
                             window, [window](int x, int y) {
                std::cout << "[Roclint Host] Showing floating card window at (" << x << ", " << y << ")..." << std::endl;
                if (x > 0 || y > 0) {
                    window->setX(x);
                    window->setY(y);
                }
                // showNormal() also restores a minimized card; show() keeps the minimized state
                window->showNormal();
                window->raise();
                window->requestActivate();
            });

            QObject::connect(controller, &roclint::host::HostController::hideCardRequested,
                             window, [window]() {
                std::cout << "[Roclint Host] Hiding floating card window..." << std::endl;
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
                controller->showCard();
            }

            if (!screenshotPath.isEmpty()) {
                if (resolveChoice > 0) {
                    QTimer::singleShot(250, window, [window, resolveChoice]() {
                        std::cout << "[Roclint Host] Resolving ambiguity with option " << resolveChoice << "..." << std::endl;
                        QMetaObject::invokeMethod(window, "commitChoice", Q_ARG(QVariant, resolveChoice));
                    });
                }
                if (openFileDropdown) {
                    QTimer::singleShot(300, window, [window]() {
                        std::cout << "[Roclint Host] Opening single-file target dropdown..." << std::endl;
                        QMetaObject::invokeMethod(window, "openFileDropdown");
                    });
                }

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
            std::cout << "[Roclint Host] Dashboard window initialized successfully." << std::endl;
            if (dashboardTab > 0) {
                window->setProperty("currentTab", dashboardTab);
            }
            QObject::connect(controller, &roclint::host::HostController::showDashboardRequested,
                             window, [window]() {
                std::cout << "[Roclint Host] Showing Dashboard window..." << std::endl;
                window->showNormal();
                window->raise();
                window->requestActivate();
            });

            QObject::connect(controller, &roclint::host::HostController::hideDashboardRequested,
                             window, [window]() {
                std::cout << "[Roclint Host] Hiding Dashboard window..." << std::endl;
                window->hide();
            });

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
        }
    }, Qt::QueuedConnection);

    engine.load(cardUrl);
    engine.load(dashboardUrl);

    if (engine.rootObjects().isEmpty()) {
        std::cerr << "[Roclint Host] Error: Failed to load QML root objects" << std::endl;
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

    std::cout << "[Roclint Host] Resident background host started. Use System Tray -> Quit to terminate." << std::endl;
    return app.exec();
}
