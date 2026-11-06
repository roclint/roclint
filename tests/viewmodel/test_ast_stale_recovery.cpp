#include "HostController.hpp"
#include "CodebaseWatcher.hpp"
#include "support/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

using roclint::host::HostController;
using roclint::host::CodebaseWatcher;

namespace {

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(5);
    }
    return predicate();
}

void processFor(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

void writeSource(const QString& path, const QByteArray& text) {
    QFile file(path);
    ROCLINT_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ROCLINT_CHECK(file.write(text) == text.size());
    ROCLINT_CHECK(file.flush());
}

QString indexDatabase(QTemporaryDir& data) {
    return QDir(data.path()).filePath(QStringLiteral("index.db"));
}

bool hasVerifiedEvidence(const HostController& controller) {
    const auto views = controller.groundedEvidence();
    for (const auto& value : views) {
        if (value.toMap().value(QStringLiteral("status")).toString() == QStringLiteral("VERIFIED"))
            return true;
    }
    return false;
}

// A workspace with one exact target (`demo::run`).
struct IndexedWorkspace {
    QTemporaryDir workspace{};
    QTemporaryDir data{};
    QString source{};
    QString index{};

    IndexedWorkspace() {
        ROCLINT_CHECK(workspace.isValid());
        ROCLINT_CHECK(data.isValid());
        ROCLINT_CHECK(QDir(workspace.path()).mkpath(QStringLiteral("src")));
        source = QDir(workspace.path()).filePath(QStringLiteral("src/engine.cpp"));
        index = indexDatabase(data);
        writeSource(source, "namespace demo { int run(int count); }\n");
    }
};

// The watcher must not deliver a hint during these tests; recovery is only the
// hash-detected path, not a queued filesystem notification.
void suppressWatcherDelivery(HostController& controller) {
    CodebaseWatcher::Limits limits;
    limits.debounceMs = 60000;
    limits.fallbackRefreshIntervalMs = 0;
    controller.setCodebaseWatcherLimitsForTesting(limits);
}

void startIndexed(HostController& controller, IndexedWorkspace& fixture) {
    ROCLINT_CHECK(controller.waitForStorageForTesting());
    controller.setCodebaseIndexPathForTesting(fixture.index);
    suppressWatcherDelivery(controller);
    controller.startCodebaseRefreshForTesting(fixture.workspace.path(), true);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
}

void testHashStalenessSchedulesAutomaticRecovery() {
    IndexedWorkspace fixture;
    HostController controller(QStringLiteral(":memory:"));
    startIndexed(controller, fixture);

    const QString prompt = QStringLiteral("Rename demo::run to step in demo.");
    const QString initial = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(initial.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(initial.contains(QStringLiteral("demo::run")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    // Saved bytes change with no watcher delivery. The hash check itself must
    // identify the file and schedule a coalesced incremental refresh.
    writeSource(fixture.source, "namespace demo { int walk(int count); }\n");
    const int before = controller.codebaseRefreshStartsForTesting();
    const QString first = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(first.isEmpty());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));
    const QString second = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(second.isEmpty());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));

    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > before;
    }, 10000));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    // Two back-to-back detections coalesce into one automatic scan.
    ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() - before <= 2);

    const QString recovered = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename demo::walk to step in demo."));
    ROCLINT_CHECK(recovered.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(recovered.contains(QStringLiteral("demo::walk")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] Hash-detected stale evidence starts a coalesced automatic refresh.\n";
}

void testRepeatedStaleRequestsCoalesce() {
    IndexedWorkspace fixture;
    HostController controller(QStringLiteral(":memory:"));
    startIndexed(controller, fixture);

    const QString prompt = QStringLiteral("Rename demo::run to step in demo.");
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(prompt).contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    writeSource(fixture.source, "namespace demo { int walk(int count); }\n");
    const int before = controller.codebaseRefreshStartsForTesting();
    for (int attempt = 0; attempt < 4; ++attempt) {
        ROCLINT_CHECK(controller.groundedIntentXmlForTesting(prompt).isEmpty());
        ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));
    }
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > before;
    }, 10000));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    const int settled = controller.codebaseRefreshStartsForTesting();
    ROCLINT_CHECK(settled - before == 1);
    processFor(500);
    ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() == settled);

    const QString recovered = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename demo::walk to step in demo."));
    ROCLINT_CHECK(recovered.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    std::cout << "  [PASS] Repeated stale requests for one file coalesce into one scan.\n";
}

void testStaleRecoveryAttemptsAreBoundedAndReset() {
    IndexedWorkspace fixture;
    HostController controller(QStringLiteral(":memory:"));
    startIndexed(controller, fixture);

    const QString prompt = QStringLiteral("Rename demo::run to step in demo.");
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(prompt).contains(QStringLiteral("VERIFIED")));
    const int baseline = controller.codebaseRefreshStartsForTesting();

    // Each cycle changes the saved bytes again only after the previous
    // automatic scan finished. No validated grounding occurs between cycles, so
    // the per-path retry budget is consumed one attempt at a time.
    for (int cycle = 0; cycle < 6; ++cycle) {
        writeSource(fixture.source,
                    QByteArray("namespace demo { int run(int count); }\n// ") +
                        QByteArray::number(cycle) + "\n");
        const int before = controller.codebaseRefreshStartsForTesting();
        const QString xml = controller.groundedIntentXmlForTesting(prompt);
        ROCLINT_CHECK(xml.isEmpty());
        ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));
        if (cycle < 3) {
            ROCLINT_CHECK(waitFor([&]() {
                return controller.codebaseRefreshStartsForTesting() > before;
            }, 10000));
        } else {
            processFor(500);
            ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() == before);
        }
        ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    }

    // Exactly three automatic attempts, then the path is exhausted.
    ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() - baseline == 3);

    // Saving bytes that match the indexed snapshot proves recovery and clears
    // the per-path retry budget.
    writeSource(fixture.source, "namespace demo { int run(int count); }\n// 2\n");
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(prompt).contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    const int afterReset = controller.codebaseRefreshStartsForTesting();
    writeSource(fixture.source, "namespace demo { int run(int count); }\n// reset\n");
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(prompt).isEmpty());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > afterReset;
    }, 10000));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));

    std::cout << "  [PASS] Stale recovery is bounded to three attempts and a validated context resets it.\n";
}

void testMissingFileStaysFailClosedAndRemovalSettles() {
    IndexedWorkspace fixture;
    HostController controller(QStringLiteral(":memory:"));
    startIndexed(controller, fixture);

    const QString prompt = QStringLiteral("Rename demo::run to step in demo.");
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(prompt).contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    ROCLINT_CHECK(QFile::remove(fixture.source));
    const int before = controller.codebaseRefreshStartsForTesting();
    const QString stale = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(stale.isEmpty());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));
    ROCLINT_CHECK(waitFor([&]() {
        return controller.codebaseRefreshStartsForTesting() > before;
    }, 10000));
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    // The automatic pass invalidates the missing file instead of retaining its
    // old evidence. The deleted target may then settle as not-found.
    const QString removed = controller.groundedIntentXmlForTesting(prompt);
    ROCLINT_CHECK(!removed.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    ROCLINT_CHECK(!hasVerifiedEvidence(controller));

    std::cout << "  [PASS] Missing saved evidence stays fail-closed and removal settles after recovery.\n";
}

void testWorkspaceSwitchCancelsStaleRecovery() {
    IndexedWorkspace workspaceA;
    QTemporaryDir workspaceB;
    ROCLINT_CHECK(workspaceB.isValid());
    ROCLINT_CHECK(QDir(workspaceB.path()).mkpath(QStringLiteral("src")));
    writeSource(QDir(workspaceB.path()).filePath(QStringLiteral("src/other.cpp")),
                "namespace other { int helper(int value); }\n");

    HostController controller(QStringLiteral(":memory:"));
    startIndexed(controller, workspaceA);
    const auto epochInA = controller.workspaceEpochForTesting();

    const QString promptA = QStringLiteral("Rename demo::run to step in demo.");
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(promptA).contains(QStringLiteral("VERIFIED")));
    writeSource(workspaceA.source, "namespace demo { int walk(int count); }\n");
    const int beforeSwitch = controller.codebaseRefreshStartsForTesting();
    ROCLINT_CHECK(controller.groundedIntentXmlForTesting(promptA).isEmpty());
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("STALE"));
    // The 200 ms stale refresh is still pending when the workspace changes.
    ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() == beforeSwitch);

    controller.startCodebaseRefreshForTesting(workspaceB.path(), true);
    ROCLINT_CHECK(controller.workspaceEpochForTesting() > epochInA);
    ROCLINT_CHECK(controller.waitForIdleForTesting(15000));
    ROCLINT_CHECK(controller.codebaseIndexReadyForTesting());
    const int afterSwitch = controller.codebaseRefreshStartsForTesting();

    // Let the superseded timer fire; it must not start a paths pass in B.
    processFor(800);
    ROCLINT_CHECK(controller.codebaseRefreshStartsForTesting() == afterSwitch);

    const QString currentB = controller.groundedIntentXmlForTesting(
        QStringLiteral("Rename other::helper to assist."));
    ROCLINT_CHECK(currentB.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(currentB.contains(QStringLiteral("other::helper")));
    ROCLINT_CHECK(controller.groundingStatus() == QStringLiteral("CURRENT"));

    // A's old target must not resurface through B's index.
    const QString oldA = controller.groundedIntentXmlForTesting(promptA);
    ROCLINT_CHECK(!oldA.contains(QStringLiteral("VERIFIED")));
    ROCLINT_CHECK(!hasVerifiedEvidence(controller));

    std::cout << "  [PASS] A workspace switch supersedes a pending stale-path recovery.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir settingsDirectory;
    ROCLINT_CHECK(settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("RoclintAstStaleRecoveryTests"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();
    std::cout << "[Test AST Stale Recovery] Starting...\n";
    testHashStalenessSchedulesAutomaticRecovery();
    testRepeatedStaleRequestsCoalesce();
    testStaleRecoveryAttemptsAreBoundedAndReset();
    testMissingFileStaysFailClosedAndRemovalSettles();
    testWorkspaceSwitchCancelsStaleRecovery();
    std::cout << "[Test AST Stale Recovery] All tests PASSED.\n";
    return 0;
}
