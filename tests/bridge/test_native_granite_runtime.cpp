#include "NativeGraniteRuntime.hpp"
#include "GraniteModelInstaller.hpp"
#include "GraniteJsonGrammar.hpp"
#include "GraniteResidencyPolicy.hpp"
#include "GraniteConfig.hpp"
#include "HostController.hpp"
#include "support/check.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSettings>
#include <QThread>
#include <QtConcurrent>

#include <cstdlib>
#include <iostream>
#include <source_location>

using roclint::host::NativeGraniteRuntime;
using roclint::host::GraniteModelInstaller;
namespace {
template<class Predicate> void waitUntil(Predicate predicate, int timeout = 6000,
    const std::source_location& source = std::source_location::current()) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    if (!predicate()) std::cerr << "Timed out at " << source.file_name() << ':' << source.line() << '\n';
    ROCLINT_CHECK(predicate());
}

// Test-only executable mode. Production helpers contain only actual llama.cpp inference.
int fixture(const QString& mode) {
    std::string line;
    while (std::getline(std::cin, line)) {
        auto response = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto payload = response.value(QStringLiteral("payload")).toObject();
        const auto op = response.value(QStringLiteral("op")).toString();
        response.remove(QStringLiteral("payload"));
        response.insert(QStringLiteral("ok"), true);
        response.insert(QStringLiteral("latency_ms"), 1);
        response.insert(QStringLiteral("type"), op == QLatin1String("hello") ? QStringLiteral("hello_ack") : QStringLiteral("result"));
        QJsonObject result;
        if (op == QLatin1String("hello")) {
            if (mode == QLatin1String("require_app_cwd")) {
                if (QDir::currentPath() != QCoreApplication::applicationDirPath()) return 2;
#ifdef Q_OS_WIN
                if (qEnvironmentVariable("PATH").section(QDir::listSeparator(), 0, 0) != QCoreApplication::applicationDirPath()) return 2;
#endif
            }
            if (mode == QLatin1String("no_hello")) continue;
            result = {{QStringLiteral("protocol_version"), 1}, {QStringLiteral("operations"), QJsonArray{QStringLiteral("load"),
                QStringLiteral("warm"), QStringLiteral("granite_analyze"), QStringLiteral("granite_rewrite"), QStringLiteral("runtime_status"),
                QStringLiteral("cancel"), QStringLiteral("set_residency"), QStringLiteral("shutdown")}}};
            result.insert(QStringLiteral("diagnostic_unbounded"), mode == QLatin1String("unbounded"));
        } else if (op == QLatin1String("load") || op == QLatin1String("warm") || op == QLatin1String("set_residency")) {
            if (mode == QLatin1String("crash_load")) std::_Exit(2);
            const auto requested = op == QLatin1String("set_residency") ? payload.value(QStringLiteral("tier")).toString() : QStringLiteral("HOT");
            const auto resource = requested == QLatin1String("IDLE_MAPPED") && NativeGraniteRuntime::manifest().value(QStringLiteral("gpu_layers")).toInt() > 0
                ? QStringLiteral("UNLOADED") : requested;
            result = {{QStringLiteral("state"), resource == QLatin1String("HOT") ? QStringLiteral("READY") : QStringLiteral("UNLOADED")},
                {QStringLiteral("residency"), resource}, {QStringLiteral("template_sha256"), QString(64, QLatin1Char('a'))}};
            if (mode == QLatin1String("bad_identity")) response.insert(QStringLiteral("model_revision"), QStringLiteral("other"));
        } else if (op == QLatin1String("granite_analyze") || op == QLatin1String("granite_rewrite")) {
            if (mode == QLatin1String("crash_semantic")) std::_Exit(2);
            if (mode == QLatin1String("late")) QThread::msleep(300);
            if (mode == QLatin1String("malformed")) { std::cout << "{bad json\n" << std::flush; continue; }
            if (mode == QLatin1String("oversize")) { std::cout << std::string(1024 * 1024 + 1, 'x') << '\n' << std::flush; continue; }
            if (mode == QLatin1String("bad_utf8")) { std::cout << "\xff\n" << std::flush; continue; }
            if (mode == QLatin1String("bad_context")) response.insert(QStringLiteral("state_version"), 99);
            if (mode == QLatin1String("bad_hash")) response.insert(QStringLiteral("input_hash"), QString(64, QLatin1Char('0')));
            if (mode == QLatin1String("bad_version")) response.insert(QStringLiteral("protocol_version"), 2);
            result = {{QStringLiteral("text"), QStringLiteral("{\"status\":\"no_ambiguity\"}")}};
        } else if (op == QLatin1String("cancel")) result = {{QStringLiteral("cancelled"), true}};
        else if (op == QLatin1String("shutdown")) {
            if (mode == QLatin1String("hang_shutdown")) QThread::msleep(5000);
            return 0;
        }
        response.insert(QStringLiteral("result"), result);
        const auto frame = QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString() + "\n";
        if (mode == QLatin1String("partial")) {
            std::cerr << "dependency diagnostics\n";
            std::cout << frame.substr(0, frame.size() / 2) << std::flush;
            QThread::msleep(10);
            std::cout << frame.substr(frame.size() / 2) << std::flush;
        } else std::cout << frame << std::flush;
        if (mode == QLatin1String("duplicate") && op == QLatin1String("granite_analyze")) std::cout << frame << std::flush;
    }
    return 0;
}

NativeGraniteRuntime::RequestContext context() {
    return {QStringLiteral("native_test_group"), QStringLiteral("native_test_attempt"), QStringLiteral("sess_native_test"),
        QStringLiteral("rev_sess_native_test_1"), 7, QStringLiteral("Bound prompt."), {}, {}};
}

QFuture<roclint::core::Result<std::string>> generate(NativeGraniteRuntime& runtime) {
    auto transport = runtime.transportFor(context(), QStringLiteral("granite_analyze"), "{\"type\":\"object\"}");
    return QtConcurrent::run([transport]() { return transport("Bound instructions."); });
}

void grammarTests() {
    using roclint::runtime::compactGraniteJsonGrammar;
    const std::string space = R"(space ::= | " " | "\n"{1,2} [ \t]{0,20})";
    const std::string prefix = R"(root ::= object space
string ::= "space ::=" char*
char ::= [^"\\] | "\\" ["\\/bfnrt]
)";
    const auto compact = compactGraniteJsonGrammar(prefix + space + "\n");
    ROCLINT_CHECK(compact && *compact == prefix + "space ::= \"\"\n");
    ROCLINT_CHECK(compactGraniteJsonGrammar(space).value() == "space ::= \"\"");
    ROCLINT_CHECK(!compactGraniteJsonGrammar(prefix));
    ROCLINT_CHECK(!compactGraniteJsonGrammar(space + "\n" + space));
    ROCLINT_CHECK(!compactGraniteJsonGrammar("space ::= [ \\t]*\n"));
    ROCLINT_CHECK(!compactGraniteJsonGrammar(space + " \n"));
    ROCLINT_CHECK(!compactGraniteJsonGrammar(space + "\r\n"));
}

void residencyPolicyTests() {
    using P = roclint::host::GraniteResidencyPolicyV1;
    for (const auto* preset : {"KEEP_WARM", "BALANCED", "LOW_MEMORY"}) {
        ROCLINT_CHECK(P::validPreset(preset));
        ROCLINT_CHECK(!P::desiredTier(preset, "HOT", 0, false, false));
        ROCLINT_CHECK(!P::desiredTier(preset, "HOT", P::kBalancedUnloadMs, true, true));
        ROCLINT_CHECK(!P::desiredTier(preset, "IDLE_MAPPED", P::kBalancedUnloadMs, true, false));
        ROCLINT_CHECK(P::desiredTier(preset, "HOT", 0, false, true) == "UNLOADED");
        ROCLINT_CHECK(P::desiredTier(preset, "IDLE_MAPPED", 0, false, true) == "UNLOADED");
        ROCLINT_CHECK(!P::desiredTier(preset, "UNLOADED", P::kBalancedUnloadMs, false, true));
    }
    ROCLINT_CHECK(!P::validPreset("balanced"));
    ROCLINT_CHECK(!P::desiredTier("INVALID", "HOT", P::kBalancedUnloadMs, false, true));
    ROCLINT_CHECK(!P::desiredTier("BALANCED", "UNKNOWN", P::kBalancedUnloadMs, false, true));
    ROCLINT_CHECK(!P::desiredTier("BALANCED", "HOT", -1, false, true));
    ROCLINT_CHECK(!P::desiredTier("KEEP_WARM", "HOT", P::kBalancedUnloadMs, false, false));
    ROCLINT_CHECK(!P::desiredTier("BALANCED", "HOT", P::kBalancedMappedMs - 1, false, false));
    ROCLINT_CHECK(P::desiredTier("BALANCED", "HOT", P::kBalancedMappedMs, false, false) == "IDLE_MAPPED");
    ROCLINT_CHECK(!P::desiredTier("BALANCED", "IDLE_MAPPED", P::kBalancedUnloadMs - 1, false, false));
    ROCLINT_CHECK(P::desiredTier("BALANCED", "IDLE_MAPPED", P::kBalancedUnloadMs, false, false) == "UNLOADED");
    ROCLINT_CHECK(!P::desiredTier("LOW_MEMORY", "HOT", P::kLowMemoryUnloadMs - 1, false, false));
    ROCLINT_CHECK(P::desiredTier("LOW_MEMORY", "HOT", P::kLowMemoryUnloadMs, false, false) == "UNLOADED");
}

void controllerResidencyTests() {
    using roclint::host::HostController;
    using P = roclint::host::GraniteResidencyPolicyV1;
    QTemporaryDir settingsDirectory;
    ROCLINT_CHECK(settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("RoclintNativeResidencyTests"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings settings;
    settings.setValue(QStringLiteral("models/nativeDirectory"), QStringLiteral("late"));
    {
        HostController host(QStringLiteral(":memory:"));
        host.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(host.graniteResidencyPreset() == "BALANCED");
        ROCLINT_CHECK(host.modelRuntimeState() == "UNLOADED" && host.stateVersion() == 0);
        ROCLINT_CHECK(host.setNativeWorkerForTesting(QCoreApplication::applicationFilePath()));
        const auto warm = [&]() {
            host.setResidencyInputsForTesting(0, false);
            host.warmSelectedModel();
            waitUntil([&]() { return host.modelRuntimeState() == "READY"; });
            ROCLINT_CHECK(host.nativeGraniteResidency() == "HOT");
        };
        warm();
        ROCLINT_CHECK(host.setGraniteResidencyPreset("KEEP_WARM"));
        host.setResidencyInputsForTesting(P::kBalancedUnloadMs, false);
        ROCLINT_CHECK(host.nativeGraniteResidency() == "HOT");
        host.setResidencyInputsForTesting(0, true);
        waitUntil([&]() { return host.modelRuntimeState() == "UNLOADED"; });
        ROCLINT_CHECK(host.nativeGraniteResidency() == "UNLOADED");
        warm();
        ROCLINT_CHECK(host.setGraniteResidencyPreset("LOW_MEMORY"));
        host.setResidencyInputsForTesting(P::kLowMemoryUnloadMs - 1, false);
        ROCLINT_CHECK(host.nativeGraniteResidency() == "HOT");
        host.setResidencyInputsForTesting(P::kLowMemoryUnloadMs, false);
        waitUntil([&]() { return host.modelRuntimeState() == "UNLOADED"; });
        warm();
        ROCLINT_CHECK(host.setGraniteResidencyPreset("BALANCED"));
        host.setResidencyInputsForTesting(P::kBalancedMappedMs, false);
        waitUntil([&]() { return host.modelRuntimeState() == "UNLOADED"; });
        const auto expected = NativeGraniteRuntime::manifest().value(QStringLiteral("gpu_layers")).toInt() > 0 ? "UNLOADED" : "IDLE_MAPPED";
        ROCLINT_CHECK(host.nativeGraniteResidency() == QLatin1String(expected));
        host.setResidencyInputsForTesting(P::kBalancedUnloadMs, false);
        waitUntil([&]() { return host.nativeGraniteResidency() == "UNLOADED"; });
        ROCLINT_CHECK(host.stateVersion() == 0 && host.sessionCount() == 0);

        // An active semantic future protects the model even under pressure and long idle input.
        warm();
        ROCLINT_CHECK(host.analyzePrompt(QStringLiteral("Review this prompt."), 0));
        host.setResidencyInputsForTesting(P::kBalancedUnloadMs, true);
        ROCLINT_CHECK(host.nativeGraniteResidency() == "HOT");
        ROCLINT_CHECK(host.waitForIdleForTesting(5000));
        ROCLINT_CHECK(host.nativeGraniteResidency() == "HOT");
        host.setResidencyInputsForTesting(P::kBalancedUnloadMs, true);
        waitUntil([&]() { return host.nativeGraniteResidency() == "UNLOADED"; });
        ROCLINT_CHECK(!host.setGraniteResidencyPreset("INVALID"));
        ROCLINT_CHECK(host.graniteResidencyPreset() == "BALANCED");
        ROCLINT_CHECK(host.setGraniteResidencyPreset("LOW_MEMORY"));
        bool quit = false;
        QObject::connect(&host, &HostController::quitRequested, [&]() { quit = true; });
        host.requestQuit(); waitUntil([&]() { return quit; });
    }
    {
        HostController restored(QStringLiteral(":memory:"));
        restored.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(restored.graniteResidencyPreset() == "LOW_MEMORY");
        ROCLINT_CHECK(restored.modelRuntimeState() == "UNLOADED" && restored.stateVersion() == 0);
    }
}

void protocolTests() {
    QFile sourceManifest(QString::fromUtf8(ROCLINT_GRANITE_SOURCE_MANIFEST));
    ROCLINT_CHECK(sourceManifest.open(QIODevice::ReadOnly));
    const auto source = QJsonDocument::fromJson(sourceManifest.readAll()).object();
    const auto compiled = NativeGraniteRuntime::manifest();
    const auto profile = source.value(QStringLiteral("profiles")).toObject()
        .value(compiled.value(QStringLiteral("backend")).toString().toUpper()).toObject();
    for (const auto* field : {"configuration_id", "backend", "gpu_layers"})
        ROCLINT_CHECK(compiled.value(QLatin1String(field)) == profile.value(QLatin1String(field)));
    for (const auto* field : {"model_family", "model_revision", "runtime_revision", "default_quantization", "candidates", "sampling", "sampling_reference", "output_format", "grammar_transform", "analysis_instruction_contract", "rewrite_instruction_contract"})
        ROCLINT_CHECK(compiled.value(QLatin1String(field)) == source.value(QLatin1String(field)));
    ROCLINT_CHECK(compiled.value(QStringLiteral("default_quantization")).toString() == QLatin1String("Q8_0"));
    NativeGraniteRuntime runtime(nullptr, QCoreApplication::applicationFilePath());
    const auto load = [&](const QString& mode) {
        runtime.load(mode, QStringLiteral("Q4_K_M")); waitUntil([&]() { return runtime.isReady(); });
    };
    load(QStringLiteral("require_app_cwd"));
    load(QStringLiteral("partial"));
    const auto direct = runtime.transportFor(context(), QStringLiteral("granite_analyze"), "{}");
    ROCLINT_CHECK(!direct("Bound instructions.").ok()); // No GUI-thread inference/deadlock.
    auto future = generate(runtime);
    waitUntil([&]() { return future.isFinished(); });
    ROCLINT_CHECK(future.result().ok());
    ROCLINT_CHECK(runtime.modelInfo().transport == "local_native_llama_cpp");
    ROCLINT_CHECK(runtime.configurationDetail().contains(QStringLiteral("template_sha256=")));
    ROCLINT_CHECK(runtime.configurationDetail().contains(QStringLiteral("temperature=1; top_p=0.95; seed=42")));
    ROCLINT_CHECK(runtime.configurationDetail().contains(QStringLiteral("sampling_reference=") + compiled.value(QStringLiteral("sampling_reference")).toString()));
    ROCLINT_CHECK(runtime.configurationDetail().contains(QStringLiteral("output_format=compact_json; grammar_transform=json_gbnf_space_empty_v1")));
    ROCLINT_CHECK(runtime.configurationDetail().contains(
        QStringLiteral("grammar_strategy=llama_common_rejection_v1; analysis_timeout_ms=%1; rewrite_timeout_ms=60000")
            .arg(roclint::runtime::kGraniteAnalysisTimeoutMs)));

    runtime.setResidency(QStringLiteral("IDLE_MAPPED"));
    waitUntil([&]() { return runtime.state() == QLatin1String("UNLOADED"); });
    ROCLINT_CHECK(!runtime.isReady());
    const auto expectedTier = NativeGraniteRuntime::manifest().value(QStringLiteral("gpu_layers")).toInt() > 0 ? QStringLiteral("UNLOADED") : QStringLiteral("IDLE_MAPPED");
    ROCLINT_CHECK(runtime.residency() == expectedTier);
    load(QStringLiteral("partial"));
    const auto generation = runtime.generationForTesting();
    runtime.setResidency(QStringLiteral("IDLE_MAPPED"));
    runtime.load(QStringLiteral("partial"), QStringLiteral("Q4_K_M")); // Warm requested before the resource transition acknowledges.
    waitUntil([&]() { return runtime.isReady() && runtime.residency() == QLatin1String("HOT"); });
    ROCLINT_CHECK(runtime.generationForTesting() == generation); // Reuse the owned helper/mapping; no unnecessary restart.

    for (const auto& mode : {"bad_context", "bad_hash", "bad_version", "malformed", "oversize", "bad_utf8"}) {
        load(QString::fromLatin1(mode));
        future = generate(runtime);
        waitUntil([&]() { return future.isFinished() && runtime.state() == QLatin1String("FAILED"); });
        ROCLINT_CHECK(!future.result().ok());
    }
    auto failedGeneration = runtime.generationForTesting();
    runtime.load(QStringLiteral("bad_identity"), QStringLiteral("Q4_K_M"));
    waitUntil([&]() { return runtime.generationForTesting() > failedGeneration && runtime.state() == QLatin1String("FAILED"); });
    ROCLINT_CHECK(!runtime.isReady());
    failedGeneration = runtime.generationForTesting();
    runtime.load(QStringLiteral("unbounded"), QStringLiteral("Q4_K_M"));
    waitUntil([&]() { return runtime.generationForTesting() > failedGeneration && runtime.state() == QLatin1String("FAILED"); });
    ROCLINT_CHECK(!runtime.isReady() && runtime.detail().contains(QStringLiteral("development investigation")));
    load(QStringLiteral("duplicate")); future = generate(runtime);
    waitUntil([&]() { return runtime.state() == QLatin1String("FAILED") && future.isFinished(); });

    load(QStringLiteral("late"));
    auto unusedTransport = runtime.transportFor(context(), QStringLiteral("granite_analyze"), "{}");
    future = generate(runtime);
    waitUntil([&]() { return runtime.state() == QLatin1String("BUSY"); });
    runtime.cancelAll();
    waitUntil([&]() { return future.isFinished(); });
    ROCLINT_CHECK(!future.result().ok() && future.result().error().code == roclint::core::ErrorCode::StaleVersion);
    auto staleFuture = QtConcurrent::run([unusedTransport]() { return unusedTransport("late instructions"); });
    waitUntil([&]() { return staleFuture.isFinished(); });
    ROCLINT_CHECK(!staleFuture.result().ok() && staleFuture.result().error().code == roclint::core::ErrorCode::StaleVersion);
    future = generate(runtime);
    waitUntil([&]() { return future.isFinished(); });
    ROCLINT_CHECK(future.result().ok() && runtime.isReady()); // The late old result was discarded.

    load(QStringLiteral("crash_semantic")); future = generate(runtime);
    waitUntil([&]() { return future.isFinished() && runtime.isReady(); });
    ROCLINT_CHECK(!future.result().ok()); // Recovery loaded only; no semantic replay.
    runtime.load(QStringLiteral("crash_load"), QStringLiteral("Q4_K_M"));
    waitUntil([&]() { return runtime.state() == QLatin1String("FAILED") && !runtime.isRunning(); });
    runtime.load(QStringLiteral("no_hello"), QStringLiteral("Q4_K_M"));
    waitUntil([&]() { return runtime.state() == QLatin1String("FAILED"); });
    load(QStringLiteral("hang_shutdown"));
    QElapsedTimer shutdown; shutdown.start(); runtime.stop();
    waitUntil([&]() { return !runtime.isRunning(); });
    ROCLINT_CHECK(shutdown.elapsed() >= 1800 && shutdown.elapsed() < 4000);

    roclint::bridge::GraniteTransport expired;
    {
        auto owned = std::make_unique<NativeGraniteRuntime>(nullptr, QCoreApplication::applicationFilePath());
        expired = owned->transportFor(context(), QStringLiteral("granite_analyze"), "{}");
    }
    auto expiredFuture = QtConcurrent::run([expired]() { return expired("not sent"); });
    waitUntil([&]() { return expiredFuture.isFinished(); });
    ROCLINT_CHECK(!expiredFuture.result().ok());
}

struct DownloadServer {
    QTcpServer server;
    QByteArray body;
    bool hold{false};
    int requests{0};
    DownloadServer() {
        ROCLINT_CHECK(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QTcpServer::newConnection, [&]() {
            auto* socket = server.nextPendingConnection();
            auto request = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket, request]() {
                *request += socket->readAll();
                if (!request->contains("\r\n\r\n") || request->startsWith("handled")) return;
                *request = "handled"; ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
                socket->write(hold ? body.left(2) : body);
                if (!hold) socket->disconnectFromHost();
            });
        });
    }
    QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }
};

void installerTests() {
    QTemporaryDir directory;
    ROCLINT_CHECK(directory.isValid());
    DownloadServer server;
    server.body = QByteArray(64, 'w');
    const QJsonObject manifest{{QStringLiteral("model_family"), QStringLiteral("fixtures/native")}, {QStringLiteral("model_revision"), QString(40, QLatin1Char('b'))}};
    QJsonObject candidate{{QStringLiteral("filename"), QStringLiteral("native-fixture.gguf")}, {QStringLiteral("quantization"), QStringLiteral("Q4_K_M")},
        {QStringLiteral("size_bytes"), static_cast<qint64>(server.body.size())},
        {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(server.body, QCryptographicHash::Sha256).toHex())}};
    GraniteModelInstaller installer;
    ROCLINT_CHECK(installer.setOriginForTesting(server.origin()));
    bool done = false, success = false;
    int percent = 0;
    QObject::connect(&installer, &GraniteModelInstaller::finished, [&](bool valid, const QString&) { success = valid; done = true; });
    QObject::connect(&installer, &GraniteModelInstaller::progress, [&](int value, const QString&) { percent = value; });
    installer.start(manifest, candidate, directory.path()); waitUntil([&]() { return done; });
    ROCLINT_CHECK(success && percent == 100);
    QFile file(directory.filePath(QStringLiteral("native-fixture.gguf")));
    ROCLINT_CHECK(file.open(QIODevice::ReadOnly)); ROCLINT_CHECK(file.readAll() == server.body); file.close();
    candidate.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
    done = false; installer.start(manifest, candidate, directory.path()); waitUntil([&]() { return done; });
    ROCLINT_CHECK(!success);
    ROCLINT_CHECK(file.open(QIODevice::ReadOnly)); ROCLINT_CHECK(file.readAll() == server.body); file.close();
    candidate.insert(QStringLiteral("size_bytes"), 1);
    done = false; installer.start(manifest, candidate, directory.path()); waitUntil([&]() { return done; }); ROCLINT_CHECK(!success);
    candidate.insert(QStringLiteral("size_bytes"), static_cast<qint64>(server.body.size()));
    server.hold = true; percent = 0; done = false;
    installer.start(manifest, candidate, directory.path()); waitUntil([&]() { return percent > 0; });
    installer.cancel(); ROCLINT_CHECK(done && !success && !installer.isRunning());
    ROCLINT_CHECK(file.open(QIODevice::ReadOnly)); ROCLINT_CHECK(file.readAll() == server.body); file.close();
    const auto before = server.requests;
    candidate.insert(QStringLiteral("filename"), QStringLiteral("../outside.gguf"));
    done = false; installer.start(manifest, candidate, directory.path()); ROCLINT_CHECK(done && !success && server.requests == before);
}
}

// Opt-in installed-model probe. It never observes desktop fields, uses isolated settings/history,
// performs no downloads, and does not approve, insert or send generated content.
int liveHostPipeline(int argc, char** argv) {
    QTemporaryDir settingsDirectory;
    ROCLINT_CHECK(settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("RoclintLivePipelineProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    {
        QSettings settings;
        settings.setValue(QStringLiteral("scoring/engine"), QStringLiteral("julia"));
        settings.setValue(QStringLiteral("scoring/juliaPython"), QString::fromLocal8Bit(argv[2]));
        settings.setValue(QStringLiteral("scoring/juliaDirectory"), QString::fromLocal8Bit(argv[3]));
        settings.setValue(QStringLiteral("models/nativeDirectory"), QString::fromLocal8Bit(argv[4]));
        settings.setValue(QStringLiteral("models/active"), QStringLiteral("native-granite:Q8_0"));
    }
    roclint::host::HostController host(QStringLiteral(":memory:"));
    ROCLINT_CHECK(host.setNativeWorkerForTesting(QString::fromLocal8Bit(argv[5])));
    QObject::connect(&host, &roclint::host::HostController::analysisStatusChanged, [&]() {
        std::cout << host.analysisStatus().toStdString() << ": " << host.analysisMessage().toStdString() << std::endl;
    });
    auto prompt = QStringLiteral("Delete the old files.");
    if (argc == 7) {
        QFile input(QString::fromLocal8Bit(argv[6]));
        ROCLINT_CHECK(input.open(QIODevice::ReadOnly));
        prompt = QString::fromUtf8(input.readAll());
    }
    QElapsedTimer timer; timer.start();
    ROCLINT_CHECK(host.analyzePrompt(prompt, 0));
    waitUntil([&]() { return !host.isAnalyzing(); }, 400000);
    std::cout << "elapsed_ms=" << timer.elapsed() << "; granite=" << host.modelRuntimeState().toStdString()
              << "; error=" << host.lastError().toStdString() << std::endl;
    const auto snapshot = host.finalSnapshotForTesting();
    ROCLINT_CHECK(host.analysisStatus() == QLatin1String("READY") && snapshot && snapshot->promptTriage);
    ROCLINT_CHECK(snapshot->promptTriage->ambiguityScore >= roclint::core::analysis::kPromptTriageThreshold);
    ROCLINT_CHECK(!snapshot->normalized.choiceGroups.empty());
    ROCLINT_CHECK(!host.hasProposal());
    ROCLINT_CHECK(host.waitForIdleForTesting(5000));
    std::cout << "Installed Julia -> native Granite pipeline completed; review remains explicit.\n";
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if ((argc == 6 || argc == 7) && std::string_view(argv[1]) == "--live-host-pipeline") return liveHostPipeline(argc, argv);
    if (argc == 5 && std::string_view(argv[1]) == "--model-dir") return fixture(QString::fromLocal8Bit(argv[2]));
    grammarTests(); residencyPolicyTests(); protocolTests(); installerTests(); controllerResidencyTests();
    std::cout << "Native protocol, cancellation, recovery, residency and atomic model installation passed.\n";
    return 0;
}
