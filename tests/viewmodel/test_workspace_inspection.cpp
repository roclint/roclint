#include "HostController.hpp"
#include "WorkspaceInspection.hpp"
#include "support/check.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace roclint::host;
namespace {
template<class Predicate> void waitFor(Predicate predicate, int timeout = 5000) {
    QElapsedTimer clock; clock.start();
    while (!predicate() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    ROCLINT_CHECK(predicate());
}
void writeFile(const QString& path, const QByteArray& bytes = {}) {
    QFile file(path); ROCLINT_CHECK(file.open(QIODevice::WriteOnly)); ROCLINT_CHECK(file.write(bytes) == bytes.size());
}
QJsonObject inspect(const QJsonObject& request, int timeout = 2000) {
    QObject owner; bool done = false; QJsonObject result;
    inspectWorkspace(&owner, request, timeout, [&](QJsonObject value) { done = true; result = value; });
    waitFor([&]() { return done; });
    return result;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--stall")) { QThread::sleep(30); return 0; }
    QTemporaryDir temporary; ROCLINT_CHECK(temporary.isValid());
    const auto root = QFileInfo(temporary.path()).canonicalFilePath();
    ROCLINT_CHECK(QDir().mkpath(root + "/src"));
    writeFile(root + "/src/main.html", "<nav>Home</nav>");
    for (const auto& name : {"node_modules", ".git", "build", "dist", "venv", ".venv"}) {
        ROCLINT_CHECK(QDir().mkpath(root + '/' + name)); writeFile(root + '/' + name + "/excluded.html", "<nav>");
    }
    writeFile(root + "/.env", "SECRET"); writeFile(root + "/private.pem", "SECRET");
    ROCLINT_CHECK(QFile::link(root, root + "/loop"));
    auto scan = inspect({{"operation", "scan"}, {"root", root}});
    ROCLINT_CHECK(scan.value("files").toArray() == QJsonArray{"src/main.html"});
    auto context = inspect({{"operation", "context"}, {"root", root}, {"root_identity", scan.value("root_identity")}, {"prompt", "Change navigation in main.html"},
                            {"files", QJsonArray{"src/main.html"}}}, 500);
    ROCLINT_CHECK(context.value("matched_files").toArray() == QJsonArray{"src/main.html"});
    ROCLINT_CHECK(!context.contains("unbounded_target_files"));
    ROCLINT_CHECK(!QJsonDocument(context).toJson().contains("Home"));
    context = inspect({{"operation", "context"}, {"root", root}, {"root_identity", "replaced-directory"},
                       {"prompt", "Change navigation in main.html"}, {"files", QJsonArray{"src/main.html"}}});
    ROCLINT_CHECK(context.value("context_incomplete").toBool());
    ROCLINT_CHECK(!context.contains("matched_files"));
    QJsonArray candidates;
    for (int i = 0; i < 65; ++i) {
        const auto name = QStringLiteral("nav%1.html").arg(i);
        writeFile(root + '/' + name, "<nav>Home</nav>"); candidates.append(name);
    }
    context = inspect({{"operation", "context"}, {"root", root}, {"root_identity", scan.value("root_identity")},
                       {"prompt", "Change navigation"}, {"files", candidates}});
    ROCLINT_CHECK(context.value("context_incomplete").toBool());
    ROCLINT_CHECK(!context.contains("unbounded_target_files"));
    ROCLINT_CHECK(QJsonDocument(context).toJson(QJsonDocument::Compact).size() <= 32768);
    context = inspect({{"operation", "context"}, {"root", root}, {"root_identity", scan.value("root_identity")},
                       {"scan_incomplete", true}, {"prompt", "Wire up the form so submissions get sent"},
                       {"files", QJsonArray{"src/main.html"}}});
    ROCLINT_CHECK(context.value("context_incomplete").toBool());
    ROCLINT_CHECK(!context.contains("missing_backend_sink"));
#ifdef Q_OS_UNIX
    const auto fifo = QFile::encodeName(root + "/pipe.html");
    ROCLINT_CHECK(::mkfifo(fifo.constData(), 0600) == 0);
    context = inspect({{"operation", "context"}, {"root", root}, {"root_identity", scan.value("root_identity")}, {"prompt", "Change navigation"},
                       {"files", QJsonArray{"pipe.html", "src/main.html"}}}, 500);
    ROCLINT_CHECK(context.value("context_incomplete").toBool());
    ROCLINT_CHECK(!context.contains("unbounded_target_files"));
    QTemporaryDir outside; const auto outsideFile = outside.path() + "/nav.html";
    writeFile(outsideFile, "<nav>SECRET</nav>");
    ROCLINT_CHECK(QFile::link(outsideFile, root + "/link.html"));
    context = inspect({{"operation", "context"}, {"root", root}, {"root_identity", scan.value("root_identity")}, {"prompt", "Change navigation"},
                       {"files", QJsonArray{"link.html", "src/main.html"}}}, 500);
    ROCLINT_CHECK(context.value("context_incomplete").toBool());
    ROCLINT_CHECK(!context.contains("unbounded_target_files"));
#endif
    // A hung helper must not block event processing; completion fires once on timeout/cancel.
    QObject owner; int ticks = 0, completions = 0; QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++ticks; }); heartbeat.start(1);
    QElapsedTimer clock; clock.start();
    const auto cancel = inspectWorkspace(&owner, {}, 100, [&](QJsonObject value) { ROCLINT_CHECK(value.isEmpty()); ++completions; },
                                         app.applicationFilePath(), {"--stall"});
    waitFor([&]() { return completions > 0; });
    ROCLINT_CHECK(clock.elapsed() < 2000 && ticks > 5);
    cancel(); ROCLINT_CHECK(completions == 1);
    bool cancelled = false;
    const auto cancelEarly = inspectWorkspace(&owner, {}, 30000, [&](QJsonObject) { cancelled = true; }, app.applicationFilePath(), {"--stall"});
    cancelEarly(); ROCLINT_CHECK(cancelled);
    // Scan work is bounded even when there are more eligible files than the result limit.
    ROCLINT_CHECK(QDir().mkpath(root + "/many"));
    for (int i = 0; i < 5100; ++i) writeFile(root + "/many/" + QString::number(i) + ".cpp");
    scan = inspect({{"operation", "scan"}, {"root", root}});
    ROCLINT_CHECK(scan.value("files").toArray().size() <= 5000);
    ROCLINT_CHECK(scan.value("context_incomplete").toBool());
    QTemporaryDir deep; const auto deepRoot = QFileInfo(deep.path()).canonicalFilePath();
    QString directory = deepRoot;
    for (int i = 0; i < 40; ++i) { directory += "/d"; ROCLINT_CHECK(QDir().mkpath(directory)); }
    writeFile(directory + "/deep.cpp");
    scan = inspect({{"operation", "scan"}, {"root", deepRoot}});
    ROCLINT_CHECK(scan.value("files").toArray().isEmpty());
    ROCLINT_CHECK(scan.value("context_incomplete").toBool());
    return 0;
}
