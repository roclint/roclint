#include "HostController.hpp"
#include "TrayManager.hpp"
#include "cadroc/core/version.hpp"

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
    app.setApplicationName(QStringLiteral("Cadroc"));
    // Single source of truth: the version CMake injects into cadroc_core
    const std::string_view coreVersion = cadroc::core::get_version_string();
    app.setApplicationVersion(QString::fromUtf8(coreVersion.data(), static_cast<qsizetype>(coreVersion.size())));
    app.setOrganizationName(QStringLiteral("Cadroc"));

    // 2. CRITICAL B00 CONTRACT: Closing windows must NOT quit the resident host
    app.setQuitOnLastWindowClosed(false);

    // 3. Create host controller
    auto controller = std::make_unique<cadroc::host::HostController>();
    // Explicit Quit is the only path that ends the resident host (C08)
    QObject::connect(controller.get(), &cadroc::host::HostController::quitRequested,
                     &app, &QCoreApplication::quit, Qt::QueuedConnection);

    // 4. Initialize system tray
    auto tray = std::make_unique<cadroc::host::TrayManager>(controller.get());
    if (!tray->initTray()) {
        std::cerr << "[Cadroc Host] Warning: System tray is not available on this environment.\n";
    }

    // 5. Initialize QML engine
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hostController"), controller.get());

    const QUrl url(QStringLiteral("qrc:/shell/FloatingCard.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url, controller = controller.get(), autoShow, dryRun, screenshotPath](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl) {
            std::cerr << "[Cadroc Host] Error: Failed to load QML root object from " << objUrl.toString().toStdString() << std::endl;
            QCoreApplication::exit(-1);
            return;
        }

        auto* window = qobject_cast<QQuickWindow*>(obj);
        if (window) {
            std::cout << "[Cadroc Host] QQuickWindow initialized successfully." << std::endl;
            QObject::connect(controller, &cadroc::host::HostController::showCardRequested,
                             window, [window]() {
                std::cout << "[Cadroc Host] Showing floating card window..." << std::endl;
                // showNormal() also restores a minimized card; show() keeps the minimized state
                window->showNormal();
                window->raise();
                window->requestActivate();
            });

            QObject::connect(controller, &cadroc::host::HostController::hideCardRequested,
                             window, [window]() {
                std::cout << "[Cadroc Host] Hiding floating card window..." << std::endl;
                window->hide();
            });

            // If closed via window controls or system event, hide instead of quitting
            QObject::connect(window, &QQuickWindow::closing,
                             controller, &cadroc::host::HostController::hideCard);

            // Keep the controller's typed visibility in sync with changes the window
            // system makes on its own (taskbar minimize/restore, focus loss)
            const auto syncVisibility = [window, controller]() {
                using cadroc::host::CardVisibility;
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
                        std::cout << "[Cadroc Host] Screenshot saved to " << screenshotPath.toStdString() << std::endl;
                    } else {
                        std::cerr << "[Cadroc Host] Error saving screenshot to " << screenshotPath.toStdString() << std::endl;
                    }
                    QCoreApplication::quit();
                });
            }
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        std::cerr << "[Cadroc Host] Error: Failed to load QML root object from " << url.toString().toStdString() << std::endl;
        return 1;
    }

    controller->markResident();

    if (dryRun) {
        std::cout << "[Cadroc Host] Version: " << app.applicationVersion().toStdString() << std::endl;
        std::cout << "[Cadroc Host] Core Version: " << coreVersion << std::endl;
        std::cout << "[Cadroc Host] Host State: " << controller->hostStateString().toStdString() << std::endl;
        std::cout << "[Cadroc Host] Qt Version: " << qVersion() << std::endl;
        std::cout << "[Cadroc Host] Dry-run passed. QML root object verified. Exiting cleanly." << std::endl;
        return 0;
    }

    std::cout << "[Cadroc Host] Resident background host started. Use System Tray -> Quit to terminate." << std::endl;
    return app.exec();
}
