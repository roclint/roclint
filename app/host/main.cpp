#include "HostController.hpp"
#include "TrayManager.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <iostream>
#include <memory>

int main(int argc, char* argv[]) {
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
                     &app, [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl) {
            QCoreApplication::exit(-1);
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    // 6. Connect controller signals to the single QQuickWindow
    if (!engine.rootObjects().isEmpty()) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (window) {
            QObject::connect(controller.get(), &cadroc::host::HostController::showCardRequested,
                             window, [window]() {
                window->show();
                window->raise();
                window->requestActivate();
            });

            QObject::connect(controller.get(), &cadroc::host::HostController::hideCardRequested,
                             window, [window]() {
                window->hide();
            });

            // If closed via window controls or system event, hide instead of quitting
            QObject::connect(window, &QQuickWindow::closing,
                             controller.get(), [controller = controller.get()](QCloseEvent* /*event*/) {
                controller->hideCard();
            });
        }
    }

    std::cout << "[Cadroc Host] Resident background host started. Use System Tray -> Quit to terminate.\n";
    return app.exec();
}
