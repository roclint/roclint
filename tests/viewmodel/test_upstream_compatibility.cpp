#include "HostController.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>

#include <iostream>
#include <cstdlib>
#include <QTimer>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using roclint::host::HostController;

namespace {
void writeFile(const QString& path) {
    ROCLINT_CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ROCLINT_CHECK(file.open(QIODevice::WriteOnly));
    ROCLINT_CHECK(file.write("// fixture\n") > 0);
}

// Test-only child-process fixture: exercises the real Qt pipe/protocol without a model download.
int protocolWorker(const QString& rawMode) {
    const auto mode = QFileInfo(rawMode).isDir() ? QFileInfo(rawMode).fileName() : rawMode;
    QProcess descendant;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto envelope = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto request = envelope.value(QStringLiteral("payload")).toObject();
        const auto operation = envelope.value(QStringLiteral("op")).toString();
        if (operation == QLatin1String("hello") && mode.startsWith(QLatin1String("spawn_child|"))) {
            descendant.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--owned-child")});
            if (!descendant.waitForStarted(1000)) return 2;
            QFile pidFile(mode.mid(QStringLiteral("spawn_child|").size()));
            if (!pidFile.open(QIODevice::WriteOnly)) return 2;
            pidFile.write(QByteArray::number(descendant.processId()));
            pidFile.close();
        }
        if (operation == QLatin1String("hello") && mode == QLatin1String("no_hello")) continue;
        if (operation == QLatin1String("shutdown") && (mode == QLatin1String("hang_shutdown") || mode.startsWith(QLatin1String("spawn_child|")))) QThread::msleep(5000);
        if ((operation == QLatin1String("evaluate") && mode == QLatin1String("crash_evaluate")) ||
            (operation == QLatin1String("triage") && mode == QLatin1String("crash_triage")) ||
            (operation == QLatin1String("load") && mode == QLatin1String("crash_load"))) std::_Exit(2);
        QJsonObject result{{QStringLiteral("state"), QStringLiteral("ready")}, {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("configuration_id"), QStringLiteral("julia.cpu.fp32.v2")}, {QStringLiteral("device"), QStringLiteral("cpu")},
            {QStringLiteral("compute_dtype"), QStringLiteral("float32")}, {QStringLiteral("cpu_threads"), 4}}}};
        if (operation == QLatin1String("hello")) {
            result = {{QStringLiteral("protocol_version"), 1}, {QStringLiteral("operations"),
                QJsonArray{QStringLiteral("evaluate"), QStringLiteral("triage"), QStringLiteral("cancel"), QStringLiteral("shutdown")}}};
        }
        if (operation == QLatin1String("triage")) {
            QJsonArray scores;
            for (const auto* category : {"ACTION", "TARGET", "RULE", "RULE_CONFLICT"})
                scores.append(QJsonObject{{QStringLiteral("category"), QString::fromLatin1(category)},
                    {QStringLiteral("score"), mode == QLatin1String("clear") ? .20 : .70}});
            if (mode == QLatin1String("bad_triage")) scores[3] = scores[0];
            if (mode == QLatin1String("bad_triage_score")) scores[0] = QJsonObject{
                {QStringLiteral("category"), QStringLiteral("ACTION")}, {QStringLiteral("score"), QStringLiteral("0.1")}};
            result = {{QStringLiteral("triage_plan_version"), 2},
                {QStringLiteral("execution_configuration_id"), QStringLiteral("julia.cpu.fp32.v2")},
                {QStringLiteral("template_id"), QString::fromLatin1(roclint::core::analysis::kPromptTriageTemplateId)},
                {QStringLiteral("category_scores"), scores}};
        }
        if (operation == QLatin1String("evaluate")) {
            QJsonArray scores, assumptionScores;
            for (const auto& value : request.value(QStringLiteral("groups")).toArray()) {
                const auto group = value.toObject();
                const auto alternatives = group.value(QStringLiteral("alternatives")).toArray();
                QJsonArray probabilities;
                for (const auto& alt : alternatives)
                    probabilities.append(QJsonObject{{QStringLiteral("id"), alt.toObject().value(QStringLiteral("id"))},
                        {QStringLiteral("probability"), 1.0 / static_cast<double>(alternatives.size())}});
                scores.append(QJsonObject{{QStringLiteral("group_id"), group.value(QStringLiteral("id"))},
                    {QStringLiteral("winner"), alternatives.first().toObject().value(QStringLiteral("id"))},
                    {QStringLiteral("probabilities"), probabilities}, {QStringLiteral("ambiguity"), 0.8},
                    {QStringLiteral("noul_certainty"), true}});
            }
            for (const auto& value : request.value(QStringLiteral("assumptions")).toArray()) {
                assumptionScores.append(QJsonObject{{QStringLiteral("assumption_id"), value.toObject().value(QStringLiteral("id"))},
                    {QStringLiteral("uncertainty"), 0.75}, {QStringLiteral("noul_certainty"), true}});
            }
            result = {{QStringLiteral("snapshot_id"), mode == QLatin1String("bad_snapshot")
                ? QJsonValue(QStringLiteral("another-snapshot")) : request.value(QStringLiteral("snapshot_id"))},
                {QStringLiteral("groups"), scores}, {QStringLiteral("assumptions"), assumptionScores},
                {QStringLiteral("plan_version"), mode == QLatin1String("bad_plan") ? 1 : 2}};
        }
        auto response = envelope;
        response.remove(QStringLiteral("payload"));
        response.insert(QStringLiteral("type"), operation == QLatin1String("hello") ? QStringLiteral("hello_ack") : QStringLiteral("result"));
        response.insert(QStringLiteral("ok"), true);
        response.insert(QStringLiteral("result"), result);
        response.insert(QStringLiteral("latency_ms"), 1.0);
        if (mode == QLatin1String("bad_identity")) response.insert(QStringLiteral("weights_sha256"), QStringLiteral("wrong"));
        if (operation == QLatin1String("evaluate") || operation == QLatin1String("triage")) {
            if (mode == QLatin1String("late_cancel")) QThread::msleep(300);
            if (mode == QLatin1String("slow_triage") && operation == QLatin1String("triage")) QThread::msleep(150);
            if (mode == QLatin1String("s1_timeout") && operation == QLatin1String("triage")) QThread::msleep(300);
            if (mode == QLatin1String("bad_context")) response.insert(QStringLiteral("session_id"), QStringLiteral("other"));
            if (mode == QLatin1String("bad_hash")) response.insert(QStringLiteral("input_hash"), QString(64, QLatin1Char('0')));
            if (mode == QLatin1String("bad_version")) response.insert(QStringLiteral("protocol_version"), 2);
            if (mode == QLatin1String("bad_type")) response.insert(QStringLiteral("type"), QStringLiteral("surprise"));
            if (mode == QLatin1String("unknown_id")) response.insert(QStringLiteral("request_id"), QStringLiteral("999999"));
            if (mode == QLatin1String("malformed")) { std::cout << "{bad json" << std::endl; continue; }
            if (mode == QLatin1String("oversize")) { std::cout << std::string(256 * 1024 + 1, 'x') << std::endl; continue; }
            if (mode == QLatin1String("invalid_utf8")) { std::cout << "\xff" << std::endl; continue; }
        }
        const auto frame = QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString() + "\n";
        if (mode == QLatin1String("partial")) {
            std::cerr << "dependency diagnostics" << std::endl;
            std::cout << frame.substr(0, frame.size() / 2) << std::flush;
            QThread::msleep(10);
            std::cout << frame.substr(frame.size() / 2) << std::flush;
        } else std::cout << frame << std::flush;
        if (operation == QLatin1String("evaluate") && mode == QLatin1String("duplicate")) std::cout << frame << std::flush;
        if (operation == QLatin1String("shutdown")) return 0;
    }
    return 0;
}

template<class Predicate> void waitUntil(Predicate predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    ROCLINT_CHECK(predicate());
}

void testJuliaProtocol() {
    QTemporaryFile helper;
    ROCLINT_CHECK(helper.open());
    roclint::host::JuliaRuntime runtime(nullptr, helper.fileName());
    int completed = 0, failures = 0;
    roclint::bridge::SnapshotEvaluationResponse received;
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::evaluationFinished,
                     [&](quint64, const auto& response) { received = response; ++completed; });
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::evaluationFailed,
                     [&](quint64, const QString&) { ++failures; });
    const auto analysis = fx::threeGroupAnalysis();
    const roclint::host::JuliaRuntime::RequestContext context{QStringLiteral("group"), QStringLiteral("attempt"),
        QString::fromStdString(analysis.sessionId), QString::fromStdString(analysis.baseRevisionId), 3};
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) == 0);
    runtime.load("valid", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
    waitUntil([&]() { return completed == 1; });
    const auto clean = roclint::bridge::S1TriageAdapter::validateLocalEvaluation(analysis, received);
    ROCLINT_CHECK(clean.isRanked() && clean.modelInferenceExecuted);
    ROCLINT_CHECK(clean.groupScores.size() == analysis.choiceGroups.size());
    for (auto& score : received.groupScores) score.probabilities->push_back(score.probabilities->front());
    ROCLINT_CHECK(!roclint::bridge::S1TriageAdapter::validateLocalEvaluation(analysis, received).isRanked());

    auto assumptionBuilder = fx::AnalysisBuilder("sess_proto", "rev_sess_proto_1", "The service needs tests.");
    for (int index = 0; index < 5; ++index) assumptionBuilder.assumption("Premise " + std::to_string(index), "service", true);
    const auto assumptions = assumptionBuilder.build();
    ROCLINT_CHECK(runtime.evaluate(assumptions, "The service needs tests.", {QStringLiteral("group_assumptions"), QStringLiteral("attempt_assumptions"),
        QString::fromStdString(assumptions.sessionId), QString::fromStdString(assumptions.baseRevisionId), 4}) != 0);
    waitUntil([&]() { return completed == 2; });
    const auto assumptionResult = roclint::bridge::S1TriageAdapter::validateLocalEvaluation(assumptions, received);
    ROCLINT_CHECK(assumptionResult.isRanked() && assumptionResult.groupScores.empty());
    ROCLINT_CHECK(assumptionResult.assumptionScores.size() == 5);
    ROCLINT_CHECK(assumptionResult.assumptionScores[3].isScored && !assumptionResult.assumptionScores[4].isScored);
    ROCLINT_CHECK(assumptionResult.assumptionScores[4].unscoredReason == "batch_limit");

    runtime.load("bad_snapshot", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
    waitUntil([&]() { return failures == 1; });
    ROCLINT_CHECK(completed == 2);
    runtime.load("bad_plan", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
    waitUntil([&]() { return failures == 2; });
    ROCLINT_CHECK(completed == 2);
    runtime.load("bad_identity", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.state() == QLatin1String("FAILED"); });
    ROCLINT_CHECK(!runtime.isReady());
    runtime.stop();
    waitUntil([&]() { return !runtime.isRunning(); });

    for (const auto& mode : {"bad_context", "bad_hash", "bad_version", "bad_type", "unknown_id", "malformed", "oversize", "invalid_utf8"}) {
        runtime.load(QString::fromLatin1(mode), QCoreApplication::applicationFilePath());
        waitUntil([&]() { return runtime.isReady(); });
        const auto before = failures;
        ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
        waitUntil([&]() { return runtime.state() == QLatin1String("FAILED"); });
        ROCLINT_CHECK(failures == before + 1);
        ROCLINT_CHECK(runtime.automaticRestartCount() == 0); // Protocol faults never loop/replay.
    }
    runtime.load("partial", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    auto before = completed;
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
    waitUntil([&]() { return completed == before + 1; });
    auto wrongContext = context;
    wrongContext.revisionId = QStringLiteral("wrong_revision");
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", wrongContext) == 0);

    runtime.load("duplicate", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    before = completed;
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
    waitUntil([&]() { return runtime.state() == QLatin1String("FAILED"); });
    ROCLINT_CHECK(completed == before + 1);

    runtime.load("late_cancel", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    before = completed;
    const auto failuresBeforeCancel = failures;
    const auto cancelledId = runtime.evaluate(analysis, "prompt", context);
    ROCLINT_CHECK(cancelledId != 0);
    runtime.cancel(cancelledId);
    ROCLINT_CHECK(failures == failuresBeforeCancel + 1);
    bool delayDone = false;
    QTimer::singleShot(250, [&]() { delayDone = true; });
    waitUntil([&]() { return delayDone; });
    ROCLINT_CHECK(completed == before && runtime.isReady());
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
    waitUntil([&]() { return completed == before + 1; });

    runtime.load("crash_evaluate", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    before = failures;
    ROCLINT_CHECK(runtime.evaluate(analysis, "prompt", context) != 0);
    waitUntil([&]() { return failures == before + 1 && runtime.isReady(); });
    ROCLINT_CHECK(runtime.automaticRestartCount() == 1); // Recovery loads only; it never resubmits scoring.

    runtime.load("crash_load", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.automaticRestartCount() == 2 && runtime.state() == QLatin1String("FAILED"); });
    runtime.load("no_hello", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.state() == QLatin1String("FAILED"); });
    ROCLINT_CHECK(!runtime.isReady() && runtime.automaticRestartCount() == 0);

    runtime.load("hang_shutdown", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    runtime.stop();
    ROCLINT_CHECK(runtime.state() == QLatin1String("STOPPING"));
    waitUntil([&]() { return !runtime.isRunning(); });
    ROCLINT_CHECK(shutdownTimer.elapsed() >= 1800 && shutdownTimer.elapsed() < 4000);
#ifdef Q_OS_WIN
    QTemporaryDir childFiles;
    const auto pidPath = childFiles.filePath(QStringLiteral("owned-child.pid"));
    runtime.load(QStringLiteral("spawn_child|") + pidPath, QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    QFile pidFile(pidPath);
    ROCLINT_CHECK(pidFile.open(QIODevice::ReadOnly));
    const auto childPid = pidFile.readAll().toULong();
    const auto child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, childPid);
    ROCLINT_CHECK(child && WaitForSingleObject(child, 0) == WAIT_TIMEOUT);
    runtime.stop();
    waitUntil([&]() { return !runtime.isRunning(); });
    ROCLINT_CHECK(WaitForSingleObject(child, 1000) == WAIT_OBJECT_0);
    CloseHandle(child);
#endif
}

void testPromptClassificationProtocol() {
    using namespace roclint::core::analysis;
    QTemporaryFile helper;
    ROCLINT_CHECK(helper.open());
    roclint::host::JuliaRuntime runtime(nullptr, helper.fileName());
    int completed = 0, failed = 0;
    PromptTriage result;
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::promptClassified,
        [&](quint64, const PromptTriage& value) { result = value; ++completed; });
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::promptClassificationFailed,
        [&](quint64, const QString&) { ++failed; });
    const roclint::host::JuliaRuntime::RequestContext context{QStringLiteral("group"), QStringLiteral("attempt"),
        QStringLiteral("session"), QStringLiteral("revision"), 3};
    ROCLINT_CHECK(runtime.classifyPrompt("Rename offset to cursor.", context) == 0);
    for (const auto* mode : {"clear", "valid"}) {
        runtime.load(QString::fromLatin1(mode), QCoreApplication::applicationFilePath());
        waitUntil([&]() { return runtime.isReady(); });
        ROCLINT_CHECK(runtime.supportsPromptTriage());
        const auto before = completed;
        ROCLINT_CHECK(runtime.classifyPrompt("Rename offset to cursor.", context) != 0);
        waitUntil([&]() { return completed == before + 1; });
        ROCLINT_CHECK(validatePromptTriage(result).ok() && result.stateVersion == 3);
        ROCLINT_CHECK(result.execution == TriageExecution::Model);
        ROCLINT_CHECK(routePromptTriage(result).value() ==
            (std::string_view(mode) == "clear" ? PromptTriageRoute::Original : PromptTriageRoute::Clarify));
    }
    for (const auto* mode : {"bad_triage", "bad_triage_score", "bad_context", "bad_hash", "bad_version"}) {
        runtime.load(QString::fromLatin1(mode), QCoreApplication::applicationFilePath());
        waitUntil([&]() { return runtime.isReady(); });
        const auto before = failed, successes = completed;
        ROCLINT_CHECK(runtime.classifyPrompt("Frozen prompt", context) != 0);
        waitUntil([&]() { return failed == before + 1; });
        ROCLINT_CHECK(completed == successes);
    }
    runtime.load("late_cancel", QCoreApplication::applicationFilePath());
    waitUntil([&]() { return runtime.isReady(); });
    const auto before = failed, successes = completed;
    const auto id = runtime.classifyPrompt("Frozen prompt", context);
    ROCLINT_CHECK(id != 0);
    runtime.cancel(id);
    ROCLINT_CHECK(failed == before + 1);
    bool elapsed = false;
    QTimer::singleShot(400, [&]() { elapsed = true; });
    waitUntil([&]() { return elapsed; });
    ROCLINT_CHECK(completed == successes && runtime.isReady());
    auto invalid = context; invalid.stateVersion = 0;
    ROCLINT_CHECK(runtime.classifyPrompt("Frozen prompt", invalid) == 0);
    ROCLINT_CHECK(runtime.classifyPrompt("\r\n", context) == 0);
    runtime.stop();
    waitUntil([&]() { return !runtime.isRunning(); });
}

void testJuliaControllerLifecycle() {
    QSettings settings;
    settings.setValue(QStringLiteral("scoring/juliaPython"), QCoreApplication::applicationFilePath());
    settings.setValue(QStringLiteral("scoring/juliaDirectory"), QStringLiteral("late_cancel"));
    settings.setValue(QStringLiteral("scoring/engine"), QStringLiteral("julia"));
    {
        HostController controller(QStringLiteral(":memory:"));
        controller.setLegacyDiscoveryForTesting(true);
        controller.setAnalysisGroupTimeoutForTesting(100);
        ROCLINT_CHECK(controller.juliaState() == QLatin1String("NOT_LOADED"));
        controller.refreshJuliaModel();
        waitUntil([&]() { return controller.juliaState() == QLatin1String("READY"); });
        controller.setTransportOverrideForTesting([](std::string_view) -> roclint::core::Result<std::string> {
            return std::string(R"({"status":"options_generated","choice_groups":[{"dimension":"performance",
                "question":"Which performance goal?","material":true,"alternatives":[
                {"label":"Lower latency","why_material":"Changes the optimization","evidence":["runs faster"]},
                {"label":"Higher throughput","why_material":"Changes the optimization","evidence":["runs faster"]}]}]})");
        });
        ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
        const auto prompt = QString::fromStdString(fx::kChainPrompt);
        ROCLINT_CHECK(controller.analyzePrompt(prompt, controller.stateVersion()));
        waitUntil([&]() { return controller.juliaState() == QLatin1String("SCORING"); });
        ROCLINT_CHECK(controller.noteDraftEdited(controller.stateVersion()));
        ROCLINT_CHECK(controller.waitForIdleForTesting(1000));
        bool lateWindowElapsed = false;
        QTimer::singleShot(700, [&]() { lateWindowElapsed = true; });
        waitUntil([&]() { return lateWindowElapsed; });
        ROCLINT_CHECK(!controller.analysisForTesting().has_value());
        ROCLINT_CHECK(controller.isDraftDirty() && controller.currentRevisionText() == prompt);
        const auto attempts = controller.repositoryForTesting()->getModelAttempts(
            roclint::core::SessionId(controller.currentSessionIdString().toStdString()));
        ROCLINT_CHECK(attempts.ok());
        bool discardedScore = false;
        for (const auto& attempt : attempts.value())
            if (attempt.stage == "julia_scoring") discardedScore = attempt.status == roclint::storage::AttemptStatus::DiscardedStale;
        ROCLINT_CHECK(discardedScore);
    }
    settings.setValue(QStringLiteral("scoring/juliaDirectory"), QStringLiteral("hang_shutdown"));
    {
        HostController controller(QStringLiteral(":memory:"));
        controller.setLegacyDiscoveryForTesting(true);
        controller.refreshJuliaModel();
        waitUntil([&]() { return controller.juliaState() == QLatin1String("READY"); });
        bool quit = false;
        QObject::connect(&controller, &HostController::quitRequested, [&]() { quit = true; });
        controller.requestQuit();
        ROCLINT_CHECK(!quit && controller.juliaState() == QLatin1String("STOPPING"));
        waitUntil([&]() { return quit; });
        ROCLINT_CHECK(controller.juliaState() == QLatin1String("NOT_LOADED"));
    }
    settings.remove(QStringLiteral("scoring/juliaPython"));
    settings.remove(QStringLiteral("scoring/juliaDirectory"));
}

void testSharedDiscoveryJuliaDeadline() {
    QSettings settings;
    settings.setValue(QStringLiteral("scoring/juliaPython"), QCoreApplication::applicationFilePath());
    settings.setValue(QStringLiteral("scoring/juliaDirectory"), QStringLiteral("late_cancel"));
    settings.setValue(QStringLiteral("scoring/engine"), QStringLiteral("julia"));
    for (const int budget : {250, 2000}) {
        HostController controller(QStringLiteral(":memory:"));
        controller.setLegacyDiscoveryForTesting(true);
        controller.setAnalysisGroupTimeoutForTesting(budget);
        controller.refreshJuliaModel();
        waitUntil([&]() { return controller.juliaState() == QLatin1String("READY"); });
        controller.setTransportOverrideForTesting([](std::string_view) -> roclint::core::Result<std::string> {
            QThread::msleep(100);
            return std::string(R"({"status":"options_generated","choice_groups":[{"question":"Which speed?","material":true,
                "alternatives":[{"label":"Lower latency","why_material":"Single calls","evidence":["runs faster"]},
                {"label":"Higher throughput","why_material":"Batches","evidence":["runs faster"]}]}]})");
        });
        ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
        QElapsedTimer elapsed; elapsed.start();
        ROCLINT_CHECK(controller.analyzePrompt(QString::fromStdString(fx::kChainPrompt), 0));
        waitUntil([&]() { return controller.juliaState() == QLatin1String("SCORING"); });
        ROCLINT_CHECK(!controller.finalSnapshotForTesting());
        ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
        ROCLINT_CHECK(controller.analysisStatus() == "READY" && controller.finalSnapshotForTesting());
        const auto snapshot = *controller.finalSnapshotForTesting();
        ROCLINT_CHECK(snapshot.evaluation.isRanked() == (budget == 2000));
        ROCLINT_CHECK(!controller.hasConfirmedIntent() && !controller.hasApproval());
        if (budget == 250) {
            ROCLINT_CHECK(elapsed.elapsed() < 400); // Completion does not wait for the late 300 ms worker result.
            ROCLINT_CHECK(snapshot.evaluation.degradedReason.find("ANALYSIS_GROUP_DEADLINE") != std::string::npos);
            for (const auto& score : snapshot.evaluation.groupScores) ROCLINT_CHECK(!score.isScored && !score.probabilities);
        }
        const auto attempts = controller.repositoryForTesting()->getModelAttempts(
            roclint::core::SessionId(controller.currentSessionIdString().toStdString()));
        ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 2);
        std::string group;
        for (const auto& attempt : attempts.value()) {
            const auto position = attempt.modelDetail.find("request_group_id=");
            ROCLINT_CHECK(position != std::string::npos && attempt.completedAtMs);
            const auto end = attempt.modelDetail.find(';', position);
            const auto id = attempt.modelDetail.substr(position, end - position);
            if (group.empty()) group = id;
            ROCLINT_CHECK(id == group);
            if (attempt.stage == "julia_scoring")
                ROCLINT_CHECK(attempt.status == (budget == 2000 ? roclint::storage::AttemptStatus::Succeeded : roclint::storage::AttemptStatus::Failed));
        }
        bool lateWindowElapsed = false;
        QTimer::singleShot(budget + 100, [&]() { lateWindowElapsed = true; });
        waitUntil([&]() { return lateWindowElapsed; });
        ROCLINT_CHECK(controller.finalSnapshotForTesting() == snapshot && controller.analysisStatus() == "READY");
        ROCLINT_CHECK(controller.juliaState() == QLatin1String("READY"));
        controller.requestQuit();
        waitUntil([&]() { return controller.juliaState() == QLatin1String("NOT_LOADED"); });
    }
    settings.remove(QStringLiteral("scoring/juliaPython"));
    settings.remove(QStringLiteral("scoring/juliaDirectory"));
}

void testS1FirstControllerRouting() {
    namespace an = roclint::core::analysis;
    QSettings settings;
    settings.setValue(QStringLiteral("scoring/juliaPython"), QCoreApplication::applicationFilePath());
    settings.setValue(QStringLiteral("scoring/engine"), QStringLiteral("julia"));
    QTemporaryDir history;
    QString clearSession;
    for (const auto* mode : {"clear", "valid", "assumption_only", "late_cancel", "bad_triage", "slow_triage", "s1_timeout", "no_options", "crash_triage"}) {
        settings.setValue(QStringLiteral("scoring/juliaDirectory"), QString::fromLatin1(mode));
        HostController controller(history.filePath(QString::fromLatin1(mode) + ".db"));
        if (std::string_view(mode) == "slow_triage") controller.setAnalysisGroupTimeoutForTesting(200);
        if (std::string_view(mode) == "s1_timeout") controller.setAnalysisGroupTimeoutForTesting(25);
        // Uses the production routing default. Only the out-of-process model data is scripted.
        controller.refreshJuliaModel();
        waitUntil([&]() { return controller.juliaState() == QLatin1String("READY"); });
        int graniteCalls = 0, rewriteCalls = 0;
        controller.setTransportOverrideForTesting([&](std::string_view instruction) -> roclint::core::Result<std::string> {
            ++graniteCalls;
            if (std::string_view(mode) == "slow_triage") QThread::msleep(100);
            if (instruction.find("<instructions>") != std::string_view::npos) {
                ++rewriteCalls;
                return fx::rewriteReply("Refactor the parser to reduce single-call latency.");
            }
            ROCLINT_CHECK(instruction.find("s1_classification") != std::string_view::npos);
            ROCLINT_CHECK(instruction.find("RULE_CONFLICT") == std::string_view::npos); // Equal head scores choose ACTION first.
            ROCLINT_CHECK(instruction.find("ACTION") != std::string_view::npos);
            if (std::string_view(mode) == "no_options")
                return std::string(R"({"status":"no_ambiguity","choice_groups":[],"assumptions":[],"missing_decisions":[],"conflicts":[]})");
            if (std::string_view(mode) == "assumption_only")
                return std::string(R"({"status":"no_ambiguity","choice_groups":[],"assumptions":[{"statement":"Faster means lower single-call latency.","material":true,"evidence":["runs faster"]}],"missing_decisions":[],"conflicts":[]})");
            return std::string(R"({"status":"options_generated","choice_groups":[{"dimension":"performance",
                "question":"Which performance goal?","material":true,"alternatives":[
                {"label":"Lower latency","why_material":"Changes the optimization","evidence":["runs faster"]},
                {"label":"Higher throughput","why_material":"Changes the optimization","evidence":["runs faster"]}]}]})");
        });
        ROCLINT_CHECK(controller.markModelReadyForTesting(fx::testModel()));
        const auto prompt = QString::fromStdString(fx::kChainPrompt);
        ROCLINT_CHECK(controller.analyzePrompt(prompt, 0));
        ROCLINT_CHECK(graniteCalls == 0); // S1 must complete first.
        if (std::string_view(mode) == "late_cancel") {
            waitUntil([&]() { return controller.juliaState() == QLatin1String("TRIAGING"); });
            ROCLINT_CHECK(controller.noteDraftEdited(controller.stateVersion()));
        }
        ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
        if (std::string_view(mode) == "clear") {
            ROCLINT_CHECK(graniteCalls == 0 && controller.isOriginalRoute());
            ROCLINT_CHECK(controller.currentRevisionText() == prompt && controller.analysisStatus() == "READY");
            ROCLINT_CHECK(controller.finalSnapshotForTesting()->payloadVersion == 4);
            ROCLINT_CHECK(!controller.isRewriteEnabled());
            ROCLINT_CHECK(!controller.synthesizeRewrite(controller.stateVersion()));
            clearSession = controller.currentSessionIdString();
        } else if (std::string_view(mode) == "valid" || std::string_view(mode) == "assumption_only") {
            ROCLINT_CHECK(graniteCalls == 1 && !controller.isOriginalRoute());
            ROCLINT_CHECK(controller.finalSnapshotForTesting()->promptTriage->ambiguityScore == .70);
            ROCLINT_CHECK(controller.finalSnapshotForTesting()->evaluation.executionStatus == an::ExecutionStatus::NotRequested);
            ROCLINT_CHECK(controller.isClarifying() && !controller.isRewriteEnabled() && rewriteCalls == 0);
            ROCLINT_CHECK(!controller.synthesizeRewrite(controller.stateVersion()));
            if (std::string_view(mode) == "assumption_only")
                ROCLINT_CHECK(controller.resolveActiveAssumption(true, controller.stateVersion()));
            else ROCLINT_CHECK(controller.confirmChoice("alt_0_0", controller.stateVersion()));
            ROCLINT_CHECK(controller.isRewriteEnabled() && rewriteCalls == 0);
            ROCLINT_CHECK(controller.synthesizeRewrite(controller.stateVersion()));
            ROCLINT_CHECK(controller.waitForIdleForTesting(5000));
            ROCLINT_CHECK(rewriteCalls == 1 && controller.hasProposal() && !controller.hasApproval());
        } else if (std::string_view(mode) == "slow_triage" || std::string_view(mode) == "no_options") {
            ROCLINT_CHECK(graniteCalls == 1 && !controller.finalSnapshotForTesting() && !controller.isOriginalRoute());
            ROCLINT_CHECK(controller.analysisStatus() == "FAILED");
        } else {
            ROCLINT_CHECK(graniteCalls == 0 && !controller.finalSnapshotForTesting());
        }
        const auto attempts = controller.repositoryForTesting()->getModelAttempts(
            roclint::core::SessionId(controller.currentSessionIdString().toStdString())).value();
        ROCLINT_CHECK(attempts.front().stage == "julia_triage");
        for (const auto& attempt : attempts) ROCLINT_CHECK(attempt.stage != "julia_scoring");
        if (std::string_view(mode) == "clear" || std::string_view(mode) == "valid")
            for (const auto& attempt : attempts)
                if (attempt.stage == "julia_triage" || attempt.stage == "discovery")
                    ROCLINT_CHECK(attempt.modelDetail.find("analysis_group_budget_ms=20000") != std::string::npos);
        if (std::string_view(mode) == "no_options" || std::string_view(mode) == "slow_triage") {
            ROCLINT_CHECK(attempts.front().status == roclint::storage::AttemptStatus::Succeeded);
            ROCLINT_CHECK(attempts.front().modelDetail.find("prompt_triage_payload=") != std::string::npos);
        }
        for (const auto& attempt : attempts) ROCLINT_CHECK(attempt.status != roclint::storage::AttemptStatus::InProgress);
        if (std::string_view(mode) == "slow_triage") {
            ROCLINT_CHECK(attempts.size() == 2 && attempts.back().stage == "discovery");
            ROCLINT_CHECK(attempts.back().status == roclint::storage::AttemptStatus::Failed);
            ROCLINT_CHECK(attempts.back().errorMessage.find("ANALYSIS_GROUP_DEADLINE") != std::string::npos);
            ROCLINT_CHECK(attempts.back().modelId == fx::testModel().modelId);
        }
        controller.requestQuit();
        waitUntil([&]() { return controller.juliaState() == QLatin1String("NOT_LOADED"); });
    }
    {
        const auto localDirectory = history.filePath("clear");
        ROCLINT_CHECK(QDir().mkpath(localDirectory));
        settings.setValue(QStringLiteral("scoring/juliaDirectory"), localDirectory);
        HostController automatic(":memory:");
        ROCLINT_CHECK(automatic.analyzePrompt(QString::fromStdString(fx::kChainPrompt), 0));
        ROCLINT_CHECK(automatic.waitForIdleForTesting(5000));
        ROCLINT_CHECK(automatic.isOriginalRoute() && automatic.finalSnapshotForTesting());
        // No Granite transport was configured or marked ready.
        automatic.requestQuit();
        waitUntil([&]() { return automatic.juliaState() == QLatin1String("NOT_LOADED"); });
    }
    {
        settings.setValue(QStringLiteral("scoring/juliaDirectory"), history.filePath("missing"));
        HostController missing(":memory:");
        ROCLINT_CHECK(!missing.analyzePrompt(QString::fromStdString(fx::kChainPrompt), 0));
        ROCLINT_CHECK(missing.analysisStatus() == "FAILED" && !missing.isOriginalRoute() && !missing.finalSnapshotForTesting());
    }
    {
        HostController restored(history.filePath("clear.db"));
        ROCLINT_CHECK(restored.restoreSession(clearSession, 0));
        ROCLINT_CHECK(restored.isOriginalRoute() && !restored.isRewriteEnabled());
        ROCLINT_CHECK(restored.currentRevisionText() == QString::fromStdString(fx::kChainPrompt));
    }
    settings.remove(QStringLiteral("scoring/juliaPython"));
    settings.remove(QStringLiteral("scoring/juliaDirectory"));
}


// Opt-in developer probe; never registered with CTest or run against user history.
int checkLiveJulia(const QString& python, const QString& directory, const QString& helper) {
    roclint::host::JuliaRuntime runtime(nullptr, helper);
    int completed = 0, failures = 0;
    roclint::bridge::SnapshotEvaluationResponse response;
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::evaluationFinished, [&](quint64, const auto& value) { response = value; ++completed; });
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::evaluationFailed, [&](quint64, const QString&) { ++failures; });
    QElapsedTimer loadTimer;
    loadTimer.start();
    runtime.load(directory, python);
    waitUntil([&]() { return runtime.isReady() || runtime.state() == QLatin1String("FAILED"); }, 185000);
    ROCLINT_CHECK(runtime.isReady());
    const auto loadMs = loadTimer.elapsed();
    int classified = 0, classificationFailures = 0;
    roclint::core::analysis::PromptTriage classification;
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::promptClassified,
        [&](quint64, const auto& value) { classification = value; ++classified; });
    QObject::connect(&runtime, &roclint::host::JuliaRuntime::promptClassificationFailed,
        [&](quint64, const QString&) { ++classificationFailures; });
    QJsonArray triageCases;
    for (const auto* prompt : {"Rename the local variable count to itemCount in parseItems. Do not change behavior or public APIs.",
                              "Refactor the parser so it runs faster.", "Return 7 plus 5."}) {
        const auto before = classified;
        const roclint::host::JuliaRuntime::RequestContext triageContext{QStringLiteral("live_triage_group"),
            QStringLiteral("live_triage_attempt_%1").arg(before), QStringLiteral("live_triage_session"),
            QStringLiteral("live_triage_revision"), 1};
        ROCLINT_CHECK(runtime.classifyPrompt(QString::fromLatin1(prompt), triageContext) != 0);
        waitUntil([&]() { return classified > before || classificationFailures; }, 10000);
        ROCLINT_CHECK(classified == before + 1 && classificationFailures == 0);
        ROCLINT_CHECK(roclint::core::analysis::validatePromptTriage(classification).ok());
        QJsonArray heads;
        for (const auto score : classification.categoryScores) heads.append(score);
        triageCases.append(QJsonObject{{QStringLiteral("prompt"), QString::fromLatin1(prompt)},
            {QStringLiteral("category_scores"), heads}, {QStringLiteral("score"), classification.ambiguityScore},
            {QStringLiteral("category"), QString::fromLatin1(roclint::core::analysis::toString(classification.primaryCategory))},
            {QStringLiteral("latency_ms"), classification.latencyMs},
            {QStringLiteral("route"), classification.ambiguityScore < .70 ? QStringLiteral("ORIGINAL") : QStringLiteral("CLARIFY")},
            {QStringLiteral("artifact_hash"), QString::fromStdString(classification.triageHash)}});
        auto recorded = triageCases.last().toObject();
        recorded.insert(QStringLiteral("execution_configuration_id"), QString::fromStdString(classification.configurationId));
        triageCases[triageCases.size() - 1] = recorded;
    }
    const auto analysis = fx::threeGroupAnalysis();
    const roclint::host::JuliaRuntime::RequestContext context{QStringLiteral("live_qt_group"), QStringLiteral("live_qt_attempt"),
        QString::fromStdString(analysis.sessionId), QString::fromStdString(analysis.baseRevisionId), 3};
    ROCLINT_CHECK(runtime.evaluate(analysis, QString::fromStdString(fx::kThreeGroupPrompt), context) != 0);
    waitUntil([&]() { return completed == 1 || failures != 0; }, 65000);
    ROCLINT_CHECK(completed == 1 && failures == 0);
    const auto validated = roclint::bridge::S1TriageAdapter::validateLocalEvaluation(analysis, response);
    ROCLINT_CHECK(validated.isRanked() && validated.modelInferenceExecuted && validated.groupScores.size() == 3);
    for (const auto& score : validated.groupScores) ROCLINT_CHECK(score.isScored);
    auto cancelledContext = context;
    cancelledContext.attemptId = QStringLiteral("live_qt_cancelled_attempt");
    const auto cancelled = runtime.evaluate(analysis, QString::fromStdString(fx::kThreeGroupPrompt), cancelledContext);
    ROCLINT_CHECK(cancelled != 0);
    runtime.cancel(cancelled);
    ROCLINT_CHECK(failures == 1);
    auto retryContext = context;
    retryContext.attemptId = QStringLiteral("live_qt_explicit_retry_attempt");
    ROCLINT_CHECK(runtime.evaluate(analysis, QString::fromStdString(fx::kThreeGroupPrompt), retryContext) != 0);
    waitUntil([&]() { return completed == 2 || failures > 1; }, 65000);
    ROCLINT_CHECK(completed == 2 && failures == 1);
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    runtime.stop();
    waitUntil([&]() { return !runtime.isRunning(); }, 5000);
    std::cout << QJsonDocument(QJsonObject{{QStringLiteral("verified"), true},
        {QStringLiteral("model"), QString::fromLatin1(roclint::host::JuliaRuntime::kModelId)},
        {QStringLiteral("load_ms"), static_cast<qint64>(loadMs)},
        {QStringLiteral("triage_cases"), triageCases},
        {QStringLiteral("scope"), QStringLiteral("Actual local Qt/helper inference; development cases, not model quality or calibration acceptance")},
        {QStringLiteral("scored_groups"), 3}, {QStringLiteral("cancelled_requests"), 1},
        {QStringLiteral("accepted_completions"), completed},
        {QStringLiteral("shutdown_ms"), static_cast<qint64>(shutdownTimer.elapsed())}}).toJson().toStdString();
    return 0;
}

}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 5 && std::string_view(argv[1]) == "--live-julia")
        return checkLiveJulia(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]));
    if (argc == 2 && std::string_view(argv[1]) == "--owned-child") {
        QThread::msleep(60000);
        return 0;
    }
    if (argc == 5 && std::string_view(argv[1]) == "-u") return protocolWorker(QString::fromLocal8Bit(argv[4]));
    QTemporaryDir settings, workspace;
    ROCLINT_CHECK(settings.isValid() && workspace.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("RoclintCompatibilityTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UpstreamCompatibility"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

    const QString root = workspace.path() + QStringLiteral("/project");
    writeFile(root + QStringLiteral("/src/main.cpp"));
    writeFile(root + QStringLiteral("/README.md"));
    writeFile(root + QStringLiteral("/.env"));
    writeFile(root + QStringLiteral("/node_modules/dependency.js"));
    writeFile(root + QStringLiteral("/build-release/generated.cpp"));
    writeFile(workspace.path() + QStringLiteral("/outside.cpp"));

    HostController c(QStringLiteral(":memory:"));

    c.setLegacyDiscoveryForTesting(true);
    ROCLINT_CHECK(c.invocationShortcut() == "Ctrl+Shift+L");
    ROCLINT_CHECK(c.recentProjects().isEmpty());
    ROCLINT_CHECK(c.getProjectFiles(root).isEmpty()); // the scan is asynchronous
    ROCLINT_CHECK(c.waitForIdleForTesting(5000));
    const auto files = c.getProjectFiles(root);
    ROCLINT_CHECK(files.size() == 2);
    ROCLINT_CHECK(files.contains(QStringLiteral("src/main.cpp")));
    ROCLINT_CHECK(files.contains(QStringLiteral("README.md")));
    ROCLINT_CHECK(c.scopePreamble(root, "global", {}).contains(QStringLiteral("Scope: entire project.")));
    ROCLINT_CHECK(c.scopePreamble(root, "local", "src/main.cpp").contains(QStringLiteral("Do not modify other files.")));
    ROCLINT_CHECK(c.scopePreamble(root, "local", "../outside.cpp").isEmpty());
    ROCLINT_CHECK(c.scopePreamble(root, "local", "missing.cpp").isEmpty());
    ROCLINT_CHECK(c.scopePreamble(root, "unknown", "src/main.cpp").isEmpty());
    ROCLINT_CHECK(c.stateVersion() == 0 && c.sessionCount() == 0); // scope helpers do not create authoritative intent

    for (const char* property : {"hasJuliaModel", "useLiveWeights", "isInferring", "directInjectionEnabled", "modelDirectory"}) {
        ROCLINT_CHECK(c.metaObject()->indexOfProperty(property) >= 0);
    }
    ROCLINT_CHECK(!c.hasJuliaModel() && !c.usesLiveModel() && !c.isInferring());
    ROCLINT_CHECK(c.setScorerEngine("jev"));
    ROCLINT_CHECK(c.scorerStatus().contains(QStringLiteral("unavailable")));
    ROCLINT_CHECK(c.setScorerEngine("julia"));
    c.importLocalModel(root + QStringLiteral("/missing.gguf"));
    ROCLINT_CHECK(!c.isDownloadingModel());
    ROCLINT_CHECK(!c.lastError().isEmpty());
    testJuliaProtocol();
    testPromptClassificationProtocol();
    testJuliaControllerLifecycle();
    testSharedDiscoveryJuliaDeadline();
    testS1FirstControllerRouting();
    std::cout << "[Upstream Compatibility] Real asynchronous file lists, explicit confined scope, original shortcut and runtime bindings passed.\n";
    return 0;
}
