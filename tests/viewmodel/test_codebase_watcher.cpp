#include "CodebaseWatcher.hpp"
#include "support/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

using roclint::host::CodebaseWatcher;

namespace {

struct WatchEvents {
    QStringList paths{};
    int fullRefresh{0};
    int failures{0};
    int overflows{0};
};

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(10);
    }
    return predicate();
}

void writeFile(const QString& path, const QByteArray& text) {
    QFile file(path);
    ROCLINT_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ROCLINT_CHECK(file.write(text) == text.size());
    ROCLINT_CHECK(file.flush());
}

void reset(WatchEvents& events) {
    events = WatchEvents{};
}

void testWatcherLifecycleAndChanges() {
    QTemporaryDir workspace;
    ROCLINT_CHECK(workspace.isValid());
    const QString root = workspace.path();
    ROCLINT_CHECK(QDir(root).mkpath(QStringLiteral("src")));
    const QString first = QDir(root).filePath(QStringLiteral("src/a.cpp"));
    writeFile(first, "int alpha;\n");

    CodebaseWatcher watcher;
    WatchEvents events;
    QObject::connect(&watcher, &CodebaseWatcher::pathsChanged,
                     [&events](const QStringList& paths) { events.paths += paths; });
    QObject::connect(&watcher, &CodebaseWatcher::fullRefreshRequired,
                     [&events](const QString&) { ++events.fullRefresh; });
    QObject::connect(&watcher, &CodebaseWatcher::watchFailed,
                     [&events](const QString&) { ++events.failures; });
    QObject::connect(&watcher, &CodebaseWatcher::watchOverflowed,
                     [&events](const QString&) { ++events.overflows; });

    CodebaseWatcher::Limits limits;
    limits.debounceMs = 40;
    limits.fallbackRefreshIntervalMs = 0;
    ROCLINT_CHECK(watcher.start(root, limits));
    ROCLINT_CHECK(watcher.isWatching());

    // In-place modification.
    reset(events);
    writeFile(first, "int beta;\n");
    ROCLINT_CHECK(waitFor([&]() {
        return events.fullRefresh > 0 || events.paths.contains(QStringLiteral("src/a.cpp")) ||
               events.paths.contains(first);
    }));
    ROCLINT_CHECK(events.failures == 0);
    ROCLINT_CHECK(events.overflows == 0);

    // Atomic save (write temp + rename over the destination).
    reset(events);
    {
        QSaveFile save(first);
        ROCLINT_CHECK(save.open(QIODevice::WriteOnly));
        const QByteArray text = "int gamma;\n";
        ROCLINT_CHECK(save.write(text) == text.size());
        ROCLINT_CHECK(save.commit());
    }
    ROCLINT_CHECK(waitFor([&]() {
        return events.fullRefresh > 0 || events.paths.contains(QStringLiteral("src/a.cpp")) ||
               events.paths.contains(first);
    }));

    // Rename.
    reset(events);
    const QString renamed = QDir(root).filePath(QStringLiteral("src/b.cpp"));
    ROCLINT_CHECK(QFile::rename(first, renamed));
    ROCLINT_CHECK(waitFor([&]() {
        return events.fullRefresh > 0 || events.paths.contains(QStringLiteral("src/a.cpp")) ||
               events.paths.contains(QStringLiteral("src/b.cpp")) ||
               events.paths.contains(first) || events.paths.contains(renamed);
    }));

    // Deletion.
    reset(events);
    ROCLINT_CHECK(QFile::remove(renamed));
    ROCLINT_CHECK(waitFor([&]() {
        return events.fullRefresh > 0 || events.paths.contains(QStringLiteral("src/b.cpp")) ||
               events.paths.contains(renamed);
    }));

    // Event burst.
    reset(events);
    for (int index = 0; index < 24; ++index) {
        writeFile(QDir(root).filePath(QStringLiteral("src/burst_%1.cpp").arg(index)),
                  QByteArray("int burst_") + QByteArray::number(index) + ";\n");
    }
    ROCLINT_CHECK(waitFor([&]() {
        return events.fullRefresh > 0 || events.paths.size() >= 4;
    }, 12000));
    ROCLINT_CHECK(events.failures == 0);
    ROCLINT_CHECK(events.overflows == 0);

    // New directory with a supported file.
    reset(events);
    ROCLINT_CHECK(QDir(root).mkpath(QStringLiteral("src/nested")));
    writeFile(QDir(root).filePath(QStringLiteral("src/nested/added.hpp")), "struct Added {};\n");
    ROCLINT_CHECK(waitFor([&]() {
        return events.fullRefresh > 0 || events.paths.contains(QStringLiteral("src/nested/added.hpp")) ||
               events.paths.contains(QDir(root).filePath(QStringLiteral("src/nested/added.hpp")));
    }));

    watcher.stop();
    ROCLINT_CHECK(!watcher.isWatching());

    std::cout << "  [PASS] Watcher reports modification, atomic save, rename, deletion, bursts and directories.\n";
}

void testPartialOverflowRetainsRunningWatcher() {
    QTemporaryDir workspace;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("a")));
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("b")));

    CodebaseWatcher watcher;
    WatchEvents events;
    QObject::connect(&watcher, &CodebaseWatcher::watchOverflowed,
                     [&events](const QString&) { ++events.overflows; });
    QObject::connect(&watcher, &CodebaseWatcher::fullRefreshRequired,
                     [&events](const QString&) { ++events.fullRefresh; });

    CodebaseWatcher::Limits limits;
    limits.maximumWatchedDirectories = 1;
    limits.debounceMs = 30;
    limits.fallbackRefreshIntervalMs = 60000;
    ROCLINT_CHECK(watcher.start(workspace.path(), limits));
    ROCLINT_CHECK(watcher.isWatching());
    ROCLINT_CHECK(!watcher.isHealthy());
    ROCLINT_CHECK(waitFor([&]() { return events.overflows == 1 && events.fullRefresh == 1; }));
    // The watcher itself never restarts; a persistent overflow is one
    // notification plus stable partial watching.
    ROCLINT_CHECK(waitFor([&]() { return events.overflows == 1; }, 400));
    ROCLINT_CHECK(watcher.isWatching());
    watcher.stop();

    std::cout << "  [PASS] A partial watcher stays running after one overflow notification.\n";
}

void testForcedRegistrationFailureCanRecover() {
    QTemporaryDir workspace;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));

    CodebaseWatcher::Limits limits;
    limits.debounceMs = 30;
    limits.fallbackRefreshIntervalMs = 0;
    CodebaseWatcher watcher;
    WatchEvents events;
    QObject::connect(&watcher, &CodebaseWatcher::watchFailed,
                     [&events](const QString&) { ++events.failures; });
    QObject::connect(&watcher, &CodebaseWatcher::fullRefreshRequired,
                     [&events](const QString&) { ++events.fullRefresh; });

    CodebaseWatcher::setForceAddPathFailureForTesting(true);
    ROCLINT_CHECK(!watcher.start(workspace.path(), limits));
    ROCLINT_CHECK(!watcher.isWatching());
    ROCLINT_CHECK(events.failures == 1);
    ROCLINT_CHECK(waitFor([&]() { return events.fullRefresh > 0; }));

    CodebaseWatcher::setForceAddPathFailureForTesting(false);
    watcher.stop();
    ROCLINT_CHECK(watcher.start(workspace.path(), limits));
    ROCLINT_CHECK(watcher.isWatching());
    watcher.stop();

    std::cout << "  [PASS] Forced registration failure stops the watcher and a later start recovers.\n";
}

void testHealthyWatcherFallbackCadence() {
    QTemporaryDir workspace;
    ROCLINT_CHECK(workspace.isValid());
    ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
    writeFile(QDir(workspace.path()).filePath(QStringLiteral("src/a.cpp")), "int alpha;\n");

    CodebaseWatcher watcher;
    WatchEvents events;
    QString lastReason;
    QObject::connect(&watcher, &CodebaseWatcher::fullRefreshRequired,
                     [&events, &lastReason](const QString& reason) {
                         ++events.fullRefresh;
                         lastReason = reason;
                     });
    QObject::connect(&watcher, &CodebaseWatcher::watchFailed,
                     [&events](const QString&) { ++events.failures; });
    QObject::connect(&watcher, &CodebaseWatcher::watchOverflowed,
                     [&events](const QString&) { ++events.overflows; });

    CodebaseWatcher::Limits limits;
    limits.debounceMs = 60000; // notifications are not what this test observes
    limits.fallbackRefreshIntervalMs = 1000;
    ROCLINT_CHECK(watcher.start(workspace.path(), limits));
    ROCLINT_CHECK(watcher.isWatching());
    ROCLINT_CHECK(watcher.isHealthy());

    // No reconciliation before the configured interval.
    QElapsedTimer quiet;
    quiet.start();
    while (quiet.elapsed() < 400) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    ROCLINT_CHECK(events.fullRefresh == 0);

    ROCLINT_CHECK(waitFor([&]() { return events.fullRefresh >= 1; }, 5000));
    ROCLINT_CHECK(lastReason == QStringLiteral("periodic-fallback"));
    const int first = events.fullRefresh;
    ROCLINT_CHECK(waitFor([&]() { return events.fullRefresh >= first + 1; }, 5000));

    QElapsedTimer window;
    window.start();
    const int windowStart = events.fullRefresh;
    while (window.elapsed() < 1200) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    // 1200 ms at a 500 ms cadence is a small, bounded number of refreshes.
    ROCLINT_CHECK(events.fullRefresh - windowStart <= 5);
    ROCLINT_CHECK(watcher.isWatching());
    ROCLINT_CHECK(watcher.isHealthy());
    ROCLINT_CHECK(events.failures == 0);
    ROCLINT_CHECK(events.overflows == 0);

    watcher.stop();
    const int stopped = events.fullRefresh;
    quiet.restart();
    while (quiet.elapsed() < 800) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    ROCLINT_CHECK(events.fullRefresh == stopped);

    std::cout << "  [PASS] A healthy watcher reconciles only on the configured fallback cadence.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);
    std::cout << "[Test Codebase Watcher] Starting...\n";
    testWatcherLifecycleAndChanges();
    testPartialOverflowRetainsRunningWatcher();
    testForcedRegistrationFailureCanRecover();
    testHealthyWatcherFallbackCadence();
    std::cout << "[Test Codebase Watcher] All tests PASSED.\n";
    return 0;
}
