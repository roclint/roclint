#include "JuliaRuntime.hpp"
#include "NativeGraniteRuntime.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QtConcurrent>
#include <iostream>
#include <utility>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif
using namespace roclint::host;
namespace {
template<class F> void until(F condition, int timeout = 6000) {
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    ROCLINT_CHECK(condition());
}
void pidFile(const QString& path) {
    QFile file(path); ROCLINT_CHECK(file.open(QIODevice::WriteOnly));
#ifdef Q_OS_UNIX
    file.write(QByteArray::number(::getpid()));
#endif
}
int fixture(QString mode, bool native) {
    QProcess descendant;
    if (mode.startsWith("orphan|")) {
#ifdef Q_OS_UNIX
        std::signal(SIGTERM, SIG_IGN);
#endif
        const auto directory = mode.mid(7); pidFile(directory + "/worker.pid");
        descendant.start(QCoreApplication::applicationFilePath(), {"--descendant", directory});
        if (!descendant.waitForStarted(1000)) return 2;
    }
    bool firstSemantic = true;
    std::string line;
    while (std::getline(std::cin, line)) {
        auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto op = request.value("op").toString(); const auto payload = request.value("payload").toObject();
        const bool semantic = op == "evaluate" || op == "triage" || op == "granite_analyze" || op == "granite_rewrite";
        if (semantic && mode.startsWith("trace|")) {
            QFile trace(mode.mid(6)); if (trace.open(QIODevice::Append)) trace.write("semantic\n");
        }
        if (semantic && mode == "cancel_stuck") continue;
        if (semantic && mode == "cancel_idle" && std::exchange(firstSemantic, false)) continue;
        QJsonObject result;
        if (op == "hello") result = {{"protocol_version", 1}, {"operations", native
            ? QJsonArray{"load", "warm", "granite_analyze", "granite_rewrite", "runtime_status", "cancel", "set_residency", "shutdown"}
            : QJsonArray{"load", "evaluate", "cancel", "runtime_status", "shutdown"}}, {"plan_versions", QJsonArray{2}}};
        else if (op == "shutdown") return 0;
        else if (op == "cancel") result = {{"cancelled", true}};
        else if (op == "runtime_status") result = {{"state", mode == "cancel_stuck" ? "busy" : "ready"}};
        else if (op == "evaluate") {
            QJsonArray groups;
            for (const auto& value : payload.value("groups").toArray()) {
                const auto group = value.toObject(); const auto choices = group.value("alternatives").toArray(); QJsonArray probabilities;
                for (qsizetype i = 0; i < choices.size(); ++i)
                    probabilities.append(QJsonObject{{"id", choices[i].toObject().value("id")}, {"probability", i == 0 ? .8 : .2 / static_cast<double>(choices.size() - 1)}});
                groups.append(QJsonObject{{"group_id", group.value("id")}, {"ambiguity", .8}, {"noul_certainty", false},
                    {"winner", choices.first().toObject().value("id")}, {"probabilities", probabilities}});
            }
            result = {{"snapshot_id", payload.value("snapshot_id")}, {"plan_version", 2}, {"groups", groups}, {"assumptions", QJsonArray{}}};
        } else if (semantic) result = {{"text", "{\"status\":\"no_ambiguity\"}"}};
        else result = native ? QJsonObject{{"state", "READY"}, {"residency", "HOT"}, {"template_sha256", QString(64, 'a')}}
            : QJsonObject{{"state", "ready"}, {"runtime", QJsonObject{{"configuration_id", "julia.cpu.fp32.v2"}, {"device", "cpu"}, {"compute_dtype", "float32"}, {"cpu_threads", 1}}}};
        request.remove("payload"); request["type"] = op == "hello" ? "hello_ack" : op == "runtime_status" ? "runtime_status" : "result";
        request["ok"] = true; request["latency_ms"] = 1; request["result"] = result;
        if (mode == "flood") { request["padding"] = QString(131072, 'x'); std::cerr << std::string(262144, 'e') << std::flush; }
        std::cout << QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString() << '\n' << std::flush;
    }
    if (mode.startsWith("orphan|")) QThread::sleep(60); // Deliberately ignores host EOF and SIGTERM.
    return 0;
}
NativeGraniteRuntime::RequestContext nativeContext() { return {"group", "attempt", "session", "revision", 1, "prompt", {}, {}}; }
void nativeTests() {
    NativeGraniteRuntime runtime(nullptr, QCoreApplication::applicationFilePath());
    runtime.load("flood", "Q4_K_M"); until([&]() { return runtime.isReady(); });
    auto transport = runtime.transportFor(nativeContext(), "granite_analyze", "{}");
    auto future = QtConcurrent::run([transport]() { return transport(std::string(204800, 'i')); });
    QThread::msleep(800); // Main-loop stalls must not prevent pipe draining or completion.
    ROCLINT_CHECK(future.isFinished() && future.result().ok());
    runtime.stop(); until([&]() { return !runtime.isRunning(); });
    QTemporaryDir directory; const auto trace = directory.path() + "/semantic.log";
    runtime.load("trace|" + trace, "Q4_K_M"); until([&]() { return runtime.isReady(); });
    runtime.setTimeoutForTesting(80); transport = runtime.transportFor(nativeContext(), "granite_analyze", "{}");
    runtime.stallIoForTesting(450);
    future = QtConcurrent::run([transport]() { return transport("expired request"); });
    QThread::msleep(250);
    ROCLINT_CHECK(future.isFinished() && !future.result().ok());
    QThread::msleep(300); QCoreApplication::processEvents();
    ROCLINT_CHECK(!QFile::exists(trace)); // Timed-out queued work never reaches the helper.
    runtime.stop(); until([&]() { return !runtime.isRunning(); });
    runtime.setTimeoutForTesting(0);
    runtime.load("cancel_stuck", "Q4_K_M"); until([&]() { return runtime.isReady(); });
    transport = runtime.transportFor(nativeContext(), "granite_analyze", "{}");
    future = QtConcurrent::run([transport]() { return transport("cancel request"); });
    until([&]() { return runtime.state() == "BUSY"; }); runtime.cancelAll();
    until([&]() { return future.isFinished(); }); ROCLINT_CHECK(!future.result().ok() && !runtime.isReady());
    until([&]() { return !runtime.isRunning(); }, 4000); ROCLINT_CHECK(runtime.state() == "FAILED");
}
void juliaTests() {
    JuliaRuntime runtime(nullptr, QCoreApplication::applicationFilePath());
    const auto analysis = fx::chainAnalysis("session", "revision");
    JuliaRuntime::RequestContext context{"group", "attempt", "session", "revision", 1};
    int completed = 0, failures = 0;
    QObject::connect(&runtime, &JuliaRuntime::evaluationFinished, [&](quint64, const auto&) { ++completed; });
    QObject::connect(&runtime, &JuliaRuntime::evaluationFailed, [&](quint64, const auto&) { ++failures; });
    QTemporaryDir directory; const auto trace = directory.path() + "/julia-semantic.log";
    runtime.load("trace|" + trace, QCoreApplication::applicationFilePath()); until([&]() { return runtime.isReady(); });
    runtime.setTimeoutForTesting(80); runtime.stallIoForTesting(450);
    QElapsedTimer deadline; deadline.start();
    ROCLINT_CHECK(runtime.evaluate(analysis, "expired before dispatch", context));
    until([&]() { return failures == 1; }, 300);
    ROCLINT_CHECK(deadline.elapsed() < 300);
    until([&]() { return runtime.isReady(); });
    ROCLINT_CHECK(!QFile::exists(trace) && completed == 0);
    runtime.stop(); until([&]() { return !runtime.isRunning(); });
    runtime.setTimeoutForTesting(0); failures = 0;
    runtime.load("flood", QCoreApplication::applicationFilePath()); until([&]() { return runtime.isReady(); });
    ROCLINT_CHECK(runtime.evaluate(analysis, QString(65536, 'p'), context));
    until([&]() { return completed == 1; }); ROCLINT_CHECK(failures == 0);
    runtime.stop(); until([&]() { return !runtime.isRunning(); });
    runtime.load("cancel_idle", QCoreApplication::applicationFilePath()); until([&]() { return runtime.isReady(); });
    const auto idle = runtime.evaluate(analysis, "prompt", context); ROCLINT_CHECK(idle); runtime.cancel(idle);
    ROCLINT_CHECK(!runtime.isReady() && failures == 1);
    until([&]() { return runtime.isReady(); });
    ROCLINT_CHECK(runtime.automaticRestartCount() == 0 && runtime.isRunning());
    ROCLINT_CHECK(runtime.evaluate(analysis, "after verified idle", context));
    until([&]() { return completed == 2; }); ROCLINT_CHECK(failures == 1);
    runtime.stop(); until([&]() { return !runtime.isRunning(); });
    runtime.load("cancel_stuck", QCoreApplication::applicationFilePath()); until([&]() { return runtime.isReady(); });
    const auto id = runtime.evaluate(analysis, "prompt", context); ROCLINT_CHECK(id); runtime.cancel(id);
    ROCLINT_CHECK(failures == 2 && !runtime.isReady());
    until([&]() { return !runtime.isRunning(); }, 4000); ROCLINT_CHECK(runtime.state() == "FAILED" && failures == 2);
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 5 && QString::fromLocal8Bit(argv[1]) == "-u") return fixture(QString::fromLocal8Bit(argv[4]), false);
    if (argc == 5 && QString::fromLocal8Bit(argv[1]) == "--model-dir") return fixture(QString::fromLocal8Bit(argv[2]), true);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--descendant") {
#ifdef Q_OS_UNIX
        std::signal(SIGTERM, SIG_IGN);
#endif
        pidFile(QString::fromLocal8Bit(argv[2]) + "/descendant.pid"); QThread::sleep(60); return 0;
    }
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--owned-parent") {
        JuliaRuntime runtime(nullptr, app.applicationFilePath()); runtime.load("orphan|" + QString::fromLocal8Bit(argv[2]), app.applicationFilePath());
        until([&]() { return runtime.isReady(); }); std::cout << "READY\n" << std::flush; return app.exec();
    }
    nativeTests(); juliaTests(); std::cout << "IPC floods, independent deadlines and cancellation quiescence passed.\n";
}
