#include "HostController.hpp"
#include "TrayManager.hpp"
#include "cadroc/core/version.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char* argv[]) {
    bool autoShow = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--dry-run" || arg == "--smoke-test" || arg == "--version") {
            std::cout << "[Cadroc Host] Version: 0.1.0" << std::endl;
            std::cout << "[Cadroc Host] Core Version: " << cadroc::core::get_version_string() << std::endl;
            std::cout << "[Cadroc Host] Qt Version: " << qVersion() << std::endl;
            std::cout << "[Cadroc Host] Dry-run passed. Exiting cleanly." << std::endl;
            return 0;
        }
        if (arg == "--show") {
            autoShow = true;
        }
    }

    // 1. Initialize desktop application with GUI and Widget support
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Cadroc"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("Cadroc"));

    // 2. CRITICAL B00 CONTRACT: Closing windows must NOT quit the resident host
    app.setQuitOnLastWindowClosed(false);

    // 3. Create host controller
    auto controller = std::make_unique<cadroc::host::HostController>();

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
                     &app, [url, controller = controller.get(), autoShow](QObject* obj, const QUrl& objUrl) {
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
                window->setVisible(true);
                window->show();
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

            if (autoShow) {
                controller->showCard();
            }
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    std::cout << "[Cadroc Host] Resident background host started. Use System Tray -> Quit to terminate." << std::endl;
    return app.exec();
}
