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
    QString screenshotPath;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--dry-run" || arg == "--smoke-test") {
            dryRun = true;
        }
        if (arg == "--show") {
            autoShow = true;
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

    // 4b. Initialize global system hotkey (Cmd+Shift+R)
    auto shortcutManager = std::make_unique<roclint::host::GlobalShortcutManager>(controller.get());
    if (!shortcutManager->registerShortcut()) {
        std::cerr << "[Roclint Host] Warning: Global shortcut could not be registered.\n";
    }

    // 5. Initialize QML engine
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), controller.get());

    const QUrl url(QStringLiteral("qrc:/shell/FloatingCard.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url, controller = controller.get(), autoShow, dryRun, screenshotPath](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl) {
            std::cerr << "[Roclint Host] Error: Failed to load QML root object from " << objUrl.toString().toStdString() << std::endl;
            QCoreApplication::exit(-1);
            return;
        }

        auto* window = qobject_cast<QQuickWindow*>(obj);
        if (window) {
            std::cout << "[Roclint Host] QQuickWindow initialized successfully." << std::endl;
            QObject::connect(controller, &roclint::host::HostController::showCardRequested,
                             window, [window]() {
                std::cout << "[Roclint Host] Showing floating card window..." << std::endl;
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

            if (autoShow && !dryRun) {
                controller->showCard();
            }

            if (!screenshotPath.isEmpty()) {
                QTimer::singleShot(600, window, [window, screenshotPath]() {
                    const QImage img = window->grabWindow();
                    if (img.save(screenshotPath)) {
                        std::cout << "[Roclint Host] Screenshot saved to " << screenshotPath.toStdString() << std::endl;
                    } else {
                        std::cerr << "[Roclint Host] Error saving screenshot to " << screenshotPath.toStdString() << std::endl;
                    }
                    QCoreApplication::quit();
                });
            }
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        std::cerr << "[Roclint Host] Error: Failed to load QML root object from " << url.toString().toStdString() << std::endl;
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
