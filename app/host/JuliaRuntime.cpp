#include "JuliaRuntime.hpp"
#include "OwnedProcessJob.hpp"
#include "roclint/core/Json.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>


namespace roclint::host {

namespace {
constexpr quint64 kMaxId = 9007199254740991ULL;
std::string ss(const QString& text) { return text.toUtf8().toStdString(); }
QString qs(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
bool verifiedIdentity(const QJsonObject& object) {
    return object.value(QStringLiteral("model_id")).toString() == QLatin1String(JuliaRuntime::kModelId) &&
           object.value(QStringLiteral("revision")).toString() == QLatin1String(JuliaRuntime::kRevision) &&
           object.value(QStringLiteral("weights_sha256")).toString() == QLatin1String(JuliaRuntime::kWeightsSha256);
}
} // namespace

JuliaRuntime::JuliaRuntime(QObject* parent, QString helperPath)
    : QObject(parent), helperPath_(helperPath.isEmpty()
          ? QCoreApplication::applicationDirPath() + QStringLiteral("/runtime/julia_worker.py") : std::move(helperPath)) { clock_.start(); }

JuliaRuntime::~JuliaRuntime() {
    stop();
    // Normal explicit Quit waits asynchronously via stopped(). This bounded
    // fallback also owns cleanup when an embedding destroys the controller.
    if (auto* process = process_) {
        process->disconnect(this);
        if (!process->waitForFinished(kShutdownTimeoutMs)) {
            killWorker();
            static_cast<void>(process->waitForFinished(1000));
        }
        process_ = nullptr;
        delete process;
        job_.reset();
    }
}

QString JuliaRuntime::defaultModelDirectory() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/models");
    const QString s1 = base + QStringLiteral("/s1");
    if (QFileInfo(s1).isDir()) return s1;
    const QString legacy = base + QStringLiteral("/julia-1");
    if (QFileInfo(legacy).isDir()) return legacy;
    return s1;
}

QString JuliaRuntime::defaultPython() {
    const QDir app(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QStringList candidates = {app.filePath(QStringLiteral("python/python.exe")),
        app.filePath(QStringLiteral("runtime/julia-python/Scripts/python.exe")),
        app.filePath(QStringLiteral("../runtime/julia-python/Scripts/python.exe"))};
#else
    const QStringList candidates = {app.filePath(QStringLiteral("python/bin/python3")),
        app.filePath(QStringLiteral("runtime/julia-python/bin/python3")),
        app.filePath(QStringLiteral("../runtime/julia-python/bin/python3")),
        QStringLiteral("/opt/homebrew/bin/python3.13"),
        QStringLiteral("/opt/homebrew/bin/python3.12"),
        QStringLiteral("/opt/homebrew/bin/python3.11"),
        QStringLiteral("/usr/local/bin/python3.13"),
        QStringLiteral("/usr/local/bin/python3.12")};
#endif
    for (const auto& candidate : candidates) {
        if (QFileInfo(candidate).isExecutable()) return QFileInfo(candidate).absoluteFilePath();
    }
    const QString python3 = QStandardPaths::findExecutable(QStringLiteral("python3"));
    return python3.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("python")) : python3;
}

void JuliaRuntime::setState(const QString& state, const QString& detail) {
    state_ = state;
    detail_ = detail;
    emit stateChanged();
}

void JuliaRuntime::stop() {
    ++restartEpoch_;
    nextLoad_.reset();
    beginStop(false, QStringLiteral("Julia stopped before scoring completed."));
}

void JuliaRuntime::setJuliaDevice(const QString& device) {
    static const QStringList kAllowed{
        QStringLiteral(""), QStringLiteral("auto"), QStringLiteral("cpu"),
        QStringLiteral("cuda"), QStringLiteral("mps"), QStringLiteral("directml")};
    if (!kAllowed.contains(device)) {
        fail(QStringLiteral("JULIA_DEVICE value '%1' is not supported. Allowed values: auto, cpu, cuda, mps, directml.").arg(device));
        return;
    }
    juliaDevice_ = device;
}

void JuliaRuntime::beginStop(bool failed, const QString& reason) {
    ready_ = false;
    promptTriageSupported_ = false;
    activePlanVersion_ = 0;
    executionConfigurationId_.clear(); executionDevice_.clear(); executionPrecision_.clear();
    executionCpuThreads_ = 0;
    failed_ = failed;
    if (stopping_) {
        if (failed) setState(QStringLiteral("FAILED"), reason);
        return;
    }
    stopping_ = true;
    const auto pending = std::exchange(pending_, {});
    cancelled_.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (it->operation == QLatin1String("evaluate")) emit evaluationFailed(it.key(), reason);
        if (it->operation == QLatin1String("triage")) emit promptClassificationFailed(it.key(), reason);
    }
    if (!process_) {
        stopping_ = false;
        setState(failed ? QStringLiteral("FAILED") : QStringLiteral("NOT_LOADED"),
                 failed ? reason : QStringLiteral("Julia is not loaded. Load its local model to enable scoring."));
        emit stopped();
        return;
    }
    setState(failed ? QStringLiteral("FAILED") : QStringLiteral("STOPPING"),
             failed ? reason : QStringLiteral("Shutting down Julia's owned worker."));
    // Control messages remain readable during inference. No result is accepted
    // once stopping begins; a non-cooperating process is killed at the deadline.
    if (handshaken_ && process_->state() == QProcess::Running)
        static_cast<void>(send(QStringLiteral("shutdown"), {}, {}, 0));
    else process_->closeWriteChannel();
    auto* process = process_;
    const auto generation = processGeneration_;
    QTimer::singleShot(kShutdownTimeoutMs, this, [this, process, generation]() {
        if (processGeneration_ == generation && process_ == process && stopping_) killWorker();
    });
}

void JuliaRuntime::fail(const QString& reason) {
    nextLoad_.reset();
    beginStop(true, reason);
}

void JuliaRuntime::killWorker() {
    if (job_) job_->terminate();
    if (process_) process_->kill();
}

void JuliaRuntime::workerFinished(QProcess* process) {
    if (process_ != process) return;
    const bool unexpected = !stopping_;
    process_ = nullptr;
    job_.reset(); // Also terminates any descendant left by an exited launcher.
    process->deleteLater();
    incoming_.clear();
    outgoing_.clear();
    handshaken_ = false;
    promptTriageSupported_ = false;
    stopping_ = false;
    if (unexpected) {
        const auto previous = launch_;
        fail(QStringLiteral("Julia's worker exited. Retry explicitly; the failed scoring request was not replayed."));
        const auto now = clock_.elapsed();
        while (!restartTimes_.isEmpty() && now - restartTimes_.first() >= kRestartWindowMs) restartTimes_.removeFirst();
        if (previous && !previous->install && restartTimes_.size() < kMaxAutomaticRestarts) {
            restartTimes_.append(now);
            const auto epoch = restartEpoch_;
            setState(QStringLiteral("RECOVERING"), QStringLiteral("Restarting Julia's worker without replaying the failed scoring request."));
            QTimer::singleShot(250, this, [this, previous, epoch]() {
                if (epoch != restartEpoch_ || process_) return;
                nextLoad_ = previous;
                startWorker();
            });
        }
        return;
    }
    pending_.clear();
    if (nextLoad_) startWorker();
    else {
        if (!failed_) setState(QStringLiteral("NOT_LOADED"), QStringLiteral("Julia's owned worker has stopped."));
        emit stopped();
    }
}

void JuliaRuntime::load(const QString& directory, const QString& python, bool install) {
    ++restartEpoch_;
    restartTimes_.clear(); // Explicit user retry starts a new recovery budget.
    nextLoad_ = Launch{directory, python, install};
    if (process_) beginStop(false, QStringLiteral("Julia configuration changed before scoring completed."));
    else startWorker();
}

void JuliaRuntime::startWorker() {
    launch_ = std::exchange(nextLoad_, std::nullopt);
    ready_ = false;
    stopping_ = false;
    failed_ = false;
    handshaken_ = false;
    cancelled_.clear();
    if (!launch_ || !QFileInfo(helperPath_).isFile() || launch_->python.isEmpty()) {
        fail(QStringLiteral("Julia's worker or Python runtime is missing. Rebuild with the runtime files and select a Julia Python environment."));
        return;
    }
    job_ = std::make_unique<OwnedProcessJob>();
    if (!job_->valid()) {
        fail(QStringLiteral("Julia's owned process group could not be created. No helper was started.")); return;
    }
    process_ = new QProcess(this);
    ++processGeneration_;
    auto* process = process_;
    job_->prepare(process);
    connect(process, &QProcess::started, this, [this, process]() {
        if (process_ == process) {
            if (!job_ || !job_->resume()) {
                killWorker();
                fail(QStringLiteral("Julia's worker could not join its owned process group. It was not allowed to run."));
                return;
            }
            process->write(outgoing_); outgoing_.clear();
        }
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &JuliaRuntime::readOutput);
    connect(process, &QProcess::readyReadStandardError, this, [process]() { static_cast<void>(process->readAllStandardError()); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process_ != process || stopping_) return;
        if (error == QProcess::Crashed) return; // finished() owns bounded recovery.
        fail(QStringLiteral("Julia's Python worker couldn't run. Check the configured Python runtime and its dependencies."));
        if (error == QProcess::FailedToStart) workerFinished(process);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int, QProcess::ExitStatus) { workerFinished(process); });
    setState(QStringLiteral("STARTING"), QStringLiteral("Starting Julia's versioned local helper."));
    process->setProgram(launch_->python);
    process->setArguments({QStringLiteral("-u"), helperPath_, QStringLiteral("--model-dir"), launch_->directory});
    if (!juliaDevice_.isEmpty()) {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("JULIA_DEVICE"), juliaDevice_);
        process->setProcessEnvironment(env);
    }
    process->start();
    static_cast<void>(send(QStringLiteral("hello"), {}, {}, kHandshakeTimeoutMs));
}

void JuliaRuntime::startModelLoad() {
    const bool install = launch_->install;
    setState(install ? QStringLiteral("DOWNLOADING") : QStringLiteral("LOADING"),
             install ? QStringLiteral("Downloading and verifying Julia-1 (about 600 MB).")
                     : QStringLiteral("Loading and warming Julia-1 locally (automatic CUDA BF16 / CPU FP32)."));
    static_cast<void>(send(install ? QStringLiteral("install") : QStringLiteral("load"), {}, {},
                           install ? kInstallTimeoutMs : kLoadTimeoutMs));
}

quint64 JuliaRuntime::send(const QString& operation, QJsonObject payload, QString snapshot, int timeoutMs,
                           const RequestContext* context) {
    if (!process_ || nextId_ >= kMaxId) return 0;
    if (pending_.size() >= 8) {
        emit requestQueueFull();
        return 0;
    }
    const quint64 id = ++nextId_;
    const QString control = QStringLiteral("runtime_%1").arg(id);
    const QByteArray input = payload.value(QStringLiteral("prompt")).toString().toUtf8();
    QJsonObject object{{QStringLiteral("protocol_version"), kProtocolVersion},
        {QStringLiteral("type"), QStringLiteral("request")}, {QStringLiteral("op"), operation},
        {QStringLiteral("request_id"), QString::number(id)},
        {QStringLiteral("request_group_id"), context ? context->requestGroupId : control},
        {QStringLiteral("attempt_id"), context ? context->attemptId : control},
        {QStringLiteral("session_id"), context ? context->sessionId : QStringLiteral("runtime")},
        {QStringLiteral("draft_revision_id"), context ? context->revisionId : QStringLiteral("runtime")},
        {QStringLiteral("state_version"), static_cast<qint64>(context ? context->stateVersion : 0)},
        {QStringLiteral("input_hash"), QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("provider"), QStringLiteral("julia_local")},
        {QStringLiteral("model_id"), QString::fromLatin1(kModelId)}, {QStringLiteral("revision"), QString::fromLatin1(kRevision)},
        {QStringLiteral("weights_sha256"), QString::fromLatin1(kWeightsSha256)},
        {QStringLiteral("configuration_id"), QString::fromLatin1(kConfigurationId)}};
    const QJsonObject envelope = object;
    object.insert(QStringLiteral("payload"), payload);
    const QByteArray frame = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > kMaxFrame) return 0;
    pending_.insert(id, Pending{operation, std::move(snapshot), envelope, payload.value(QStringLiteral("prompt")).toString()});
    if (process_->state() == QProcess::Running) process_->write(frame);
    else outgoing_.append(frame);
    if (timeoutMs > 0) QTimer::singleShot(timeoutMs, this, [this, id]() {
        if (pending_.contains(id)) fail(QStringLiteral("Julia timed out. Retry explicitly; the original prompt remains available."));
    });
    return id;
}

void JuliaRuntime::cancel(quint64 requestId, const QString& reason) {
    auto it = pending_.find(requestId);
    if (it == pending_.end() || (it->operation != QLatin1String("evaluate") && it->operation != QLatin1String("triage"))) return;
    const bool triage = it->operation == QLatin1String("triage");
    cancelled_.insert(requestId, it->envelope);
    pending_.erase(it);
    static_cast<void>(send(QStringLiteral("cancel"), {{QStringLiteral("target_request_id"), QString::number(requestId)}}, {}, kHandshakeTimeoutMs));
    if (ready_) setState(QStringLiteral("READY"), QStringLiteral("The scoring request was cancelled; late results will be discarded."));
    if (triage) emit promptClassificationFailed(requestId, reason);
    else emit evaluationFailed(requestId, reason);
    // Bound cancellation history without ever accepting an evicted late result.
    if (cancelled_.size() >= 256) fail(QStringLiteral("Julia's cancellation history is full. Reload the worker before scoring again."));
}

void JuliaRuntime::cancelAllEvaluations() {
    const auto ids = pending_.keys();
    for (const auto id : ids) cancel(id);
}

quint64 JuliaRuntime::evaluate(const core::analysis::NormalizedAnalysis& analysis, const QString& prompt, const RequestContext& context) {
    if (!ready_ || stopping_ || !handshaken_ || context.requestGroupId.isEmpty() || context.attemptId.isEmpty() ||
        context.sessionId.isEmpty() || context.revisionId.isEmpty() || context.stateVersion > kMaxId ||
        context.sessionId != qs(analysis.sessionId) || context.revisionId != qs(analysis.baseRevisionId)) return 0;
    if (prompt.contains(QLatin1Char('\r')) || prompt.toUtf8().size() > 64 * 1024) return 0;
    for (const auto& id : {context.requestGroupId, context.attemptId, context.sessionId, context.revisionId})
        if (id.toUtf8().size() > 256) return 0;
    QJsonArray groups, assumptions;
    const auto plan = core::analysis::buildScoringPlan(analysis);
    if (plan.groupIds.empty() && plan.assumptionIds.empty()) return 0;
    for (const auto& group : analysis.choiceGroups) {
        if (std::find(plan.groupIds.begin(), plan.groupIds.end(), group.id) == plan.groupIds.end()) continue;
        QJsonArray alternatives;
        for (const auto& alt : group.alternatives) {
            alternatives.append(QJsonObject{{QStringLiteral("id"), qs(alt.id)}, {QStringLiteral("label"), qs(alt.interpretation)}});
        }
        groups.append(QJsonObject{{QStringLiteral("id"), qs(group.id)}, {QStringLiteral("question"), qs(group.question)},
                                   {QStringLiteral("alternatives"), alternatives}});
    }
    for (const auto& id : plan.assumptionIds) {
        const auto item = std::find_if(analysis.assumptions.begin(), analysis.assumptions.end(), [&](const auto& value) { return value.id == id; });
        assumptions.append(QJsonObject{{QStringLiteral("id"), qs(id)}, {QStringLiteral("statement"), qs(item->statement)}});
    }
    QJsonObject request{{QStringLiteral("prompt"), prompt},
                        {QStringLiteral("snapshot_id"), qs(analysis.id)}, {QStringLiteral("groups"), groups},
                        {QStringLiteral("plan_version"), 2}, {QStringLiteral("assumptions"), assumptions}};
    const auto id = send(QStringLiteral("evaluate"), request, qs(analysis.id), kEvaluationTimeoutMs, &context);
    if (id != 0) setState(QStringLiteral("SCORING"), QStringLiteral("Julia is scoring the supplied choices and material assumptions locally."));
    return id;
}

quint64 JuliaRuntime::classifyPrompt(const QString& prompt, const RequestContext& context) {
    if (!ready_ || !promptTriageSupported_ || stopping_ || !handshaken_ || context.stateVersion == 0 ||
        context.stateVersion > kMaxId || prompt.trimmed().isEmpty() || prompt.contains(QLatin1Char('\r')) ||
        prompt.toUtf8().size() > 64 * 1024) return 0;
    for (const auto& value : {context.requestGroupId, context.attemptId, context.sessionId, context.revisionId}) {
        const auto bytes = value.toUtf8();
        if (bytes.isEmpty() || bytes.size() > 256) return 0;
        for (const auto byte : bytes)
            if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                  (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.')) return 0;
    }
    const auto id = send(QStringLiteral("triage"), {{QStringLiteral("prompt"), prompt},
        {QStringLiteral("triage_plan_version"), 2}}, {}, kTriageTimeoutMs, &context);
    if (id) setState(QStringLiteral("TRIAGING"), QStringLiteral("Julia is classifying the frozen prompt before Granite."));
    return id;
}

void JuliaRuntime::readOutput() {
    if (!process_) return;
    incoming_.append(process_->readAllStandardOutput());
    if (stopping_) { incoming_.clear(); return; }
    while (true) {
        const auto end = incoming_.indexOf('\n');
        if (end < 0) break;
        const QByteArray frame = incoming_.left(end);
        incoming_.remove(0, end + 1);
        const auto strict = core::json::parse(std::string_view(frame.constData(), static_cast<std::size_t>(frame.size())),
                                               {static_cast<std::size_t>(kMaxFrame), 32});
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(frame, &error);
        if (frame.size() + 1 > kMaxFrame || !strict.ok() || error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("Julia returned a malformed protocol response."));
            return;
        }
        receive(document.object());
        if (!process_) return;
    }
    if (incoming_.size() > kMaxFrame) fail(QStringLiteral("Julia's response exceeded the frame limit."));
}

void JuliaRuntime::receive(const QJsonObject& object) {
    if (stopping_) return;
    const auto idValue = object.value(QStringLiteral("request_id"));
    bool validId = false;
    const quint64 id = idValue.toString().toULongLong(&validId);
    if (!idValue.isString() || !validId || id == 0 || id > kMaxId || QString::number(id) != idValue.toString()) {
        fail(QStringLiteral("Julia returned an invalid request ID.")); return;
    }
    const auto pending = pending_.constFind(id);
    const auto cancelled = cancelled_.constFind(id);
    if (pending == pending_.cend() && cancelled == cancelled_.cend()) {
        fail(QStringLiteral("Julia returned an unknown or duplicate request ID.")); return;
    }
    const QJsonObject expected = pending != pending_.cend() ? pending->envelope : cancelled.value();
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        if (it.key() == QLatin1String("type")) continue;
        if (object.value(it.key()) != it.value()) {
            fail(QStringLiteral("Julia's reply did not match its request context or pinned configuration.")); return;
        }
    }
    if (!object.value(QStringLiteral("ok")).isBool()) { fail(QStringLiteral("Julia returned a malformed result status.")); return; }
    const auto operation = expected.value(QStringLiteral("op")).toString();
    const bool ok = object.value(QStringLiteral("ok")).toBool();
    const auto type = object.value(QStringLiteral("type")).toString();
    const auto expectedType = !ok ? QStringLiteral("error") : operation == QLatin1String("hello") ? QStringLiteral("hello_ack") : QStringLiteral("result");
    if (type != expectedType || (ok && !verifiedIdentity(object))) {
        fail(QStringLiteral("Julia returned an incompatible protocol message.")); return;
    }
    if (cancelled != cancelled_.cend()) return; // Terminal locally even if the helper could not interrupt a kernel.
    const Pending request = pending_.take(id);
    if (!ok) {
        const QString error = QStringLiteral("Julia could not complete the local operation; this snapshot remains unranked.");
        if (request.operation == QLatin1String("evaluate")) {
            setState(QStringLiteral("READY"), error);
            emit evaluationFailed(id, error);
        } else if (request.operation == QLatin1String("triage")) {
            setState(QStringLiteral("READY"), QStringLiteral("Classification failed. The original prompt remains available."));
            emit promptClassificationFailed(id, QStringLiteral("Julia could not classify this prompt."));
        } else fail(error);
        return;
    }
    const QJsonObject result = object.value(QStringLiteral("result")).toObject();
    if (request.operation == QLatin1String("hello")) {
        const auto operations = result.value(QStringLiteral("operations")).toArray();
        if (result.value(QStringLiteral("protocol_version")).toInt(-1) != kProtocolVersion ||
            !operations.contains(QJsonValue(QStringLiteral("evaluate"))) || !operations.contains(QJsonValue(QStringLiteral("cancel"))) ||
            !operations.contains(QJsonValue(QStringLiteral("shutdown")))) {
            fail(QStringLiteral("Julia's helper is incompatible. Rebuild the bundled runtime.")); return;
        }
        handshaken_ = true;
        promptTriageSupported_ = operations.contains(QJsonValue(QStringLiteral("triage")));
        // Negotiate the triage plan version. The worker reports which versions it supports;
        // the host picks the highest version both sides know. Old workers that omit the field
        // default to [2] for backward compatibility.
        const auto planVersionsArray = result.value(QStringLiteral("triage_plan_versions")).toArray();
        QList<int> workerVersions;
        if (planVersionsArray.isEmpty()) {
            workerVersions = {2}; // Backward-compatible default for old workers.
        } else {
            for (const auto& v : planVersionsArray)
                if (v.isDouble() && v.toInt() > 0) workerVersions.append(v.toInt());
        }
        static const QList<int> kHostVersions{2};
        activePlanVersion_ = 0;
        for (int v : kHostVersions)
            if (workerVersions.contains(v) && v > activePlanVersion_) activePlanVersion_ = v;
        if (activePlanVersion_ == 0) {
            fail(QStringLiteral("Julia's triage plan version is incompatible with this host. "
                                "Update the Julia worker or reinstall Roclint.")); return;
        }
        startModelLoad();
        return;
    }
    if (request.operation == QLatin1String("cancel")) return;
    if (request.operation == QLatin1String("triage")) {
        auto reject = [this, id]() {
            setState(QStringLiteral("READY"), QStringLiteral("Julia returned invalid classification evidence; no route was selected."));
            emit promptClassificationFailed(id, QStringLiteral("Julia returned invalid prompt classification evidence."));
        };
        const auto scores = result.value(QStringLiteral("category_scores")).toArray();
        if (result.value(QStringLiteral("triage_plan_version")) != QJsonValue(2) ||
            result.value(QStringLiteral("template_id")) != QJsonValue(QString::fromLatin1(core::analysis::kPromptTriageTemplateId)) ||
            scores.size() != 4 || !object.value(QStringLiteral("latency_ms")).isDouble()) { reject(); return; }
        core::analysis::PromptTriage triage;
        triage.sessionId = ss(request.envelope.value(QStringLiteral("session_id")).toString());
        triage.revisionId = ss(request.envelope.value(QStringLiteral("draft_revision_id")).toString());
        triage.revisionTextHash = ss(request.envelope.value(QStringLiteral("input_hash")).toString());
        triage.stateVersion = static_cast<quint64>(request.envelope.value(QStringLiteral("state_version")).toDouble());
        triage.requestGroupId = ss(request.envelope.value(QStringLiteral("request_group_id")).toString());
        triage.attemptId = ss(request.envelope.value(QStringLiteral("attempt_id")).toString());
        triage.modelId = kModelId; triage.modelRevision = kRevision; triage.weightsSha256 = kWeightsSha256;
        if (result.value(QStringLiteral("execution_configuration_id")) != QJsonValue(executionConfigurationId_)) { reject(); return; }
        triage.configurationId = ss(executionConfigurationId_ + QStringLiteral(".threads%1").arg(executionCpuThreads_));
        triage.execution = core::analysis::TriageExecution::Model;
        triage.latencyMs = object.value(QStringLiteral("latency_ms")).toDouble(-1);
        std::array<bool, 4> seen{};
        for (const auto& value : scores) {
            const auto score = value.toObject();
            bool found = false;
            for (std::size_t index = 0; index < 4; ++index) {
                if (score.value(QStringLiteral("category")) != QJsonValue(QString::fromLatin1(
                        core::analysis::toString(static_cast<core::analysis::PromptCategory>(index))))) continue;
                if (seen[index] || !score.value(QStringLiteral("score")).isDouble()) { reject(); return; }
                seen[index] = true; found = true;
                triage.categoryScores[index] = score.value(QStringLiteral("score")).toDouble(-1);
            }
            if (!found) { reject(); return; }
        }
        const auto sealed = core::analysis::sealPromptTriage(std::move(triage), ss(request.prompt));
        if (!sealed.ok()) { reject(); return; }
        setState(QStringLiteral("READY"), QStringLiteral("Julia classified the prompt locally. Scores do not confirm user intent."));
        emit promptClassified(id, sealed.value());
        return;
    }
    if (request.operation != QLatin1String("evaluate")) {
        if (result.value(QStringLiteral("state")).toString() != QLatin1String("ready")) {
            fail(QStringLiteral("Julia did not finish loading.")); return;
        }
        const auto profile = result.value(QStringLiteral("runtime")).toObject();
        const auto device = profile.value(QStringLiteral("device")).toString();
        const auto precision = profile.value(QStringLiteral("compute_dtype")).toString();
        const auto configuration = profile.value(QStringLiteral("configuration_id")).toString();
        const auto threads = profile.value(QStringLiteral("cpu_threads"));
        const bool cpu = device == QLatin1String("cpu") && precision == QLatin1String("float32") &&
            configuration == QLatin1String("julia.cpu.fp32.v2");
        const bool cuda = device == QLatin1String("cuda") && precision == QLatin1String("bfloat16") &&
            configuration == QLatin1String("julia.cuda.bf16.v2");
        const bool mps = device == QLatin1String("mps") &&
            (precision == QLatin1String("float16") || precision == QLatin1String("bfloat16")) &&
            configuration == QLatin1String("julia.mps.fp16.v2");
        const bool directml = device == QLatin1String("directml") &&
            precision == QLatin1String("float16") &&
            configuration == QLatin1String("julia.directml.fp16.v2");
        if ((!cpu && !cuda && !mps && !directml) || !threads.isDouble() || threads.toDouble() != threads.toInt(-1) || threads.toInt() < 1 || threads.toInt() > 256) {
            fail(QStringLiteral("Julia did not report a supported execution device, precision and thread count.")); return;
        }
        executionConfigurationId_ = configuration; executionDevice_ = device; executionPrecision_ = precision;
        executionCpuThreads_ = threads.toInt();
        ready_ = true;
#ifdef Q_OS_WIN
        if (directml) {
            setState(QStringLiteral("READY"),
                     QStringLiteral("Julia-1 is ready on DirectML (%1). GPU acceleration active via DirectML.").arg(precision));
        } else if (cpu) {
            // On Windows, CPU FP32 means no supported GPU accelerator was available.
            setState(QStringLiteral("READY"),
                     QStringLiteral("Julia-1 is running on CPU (FP32) — GPU acceleration unavailable. "
                                    "Install CUDA drivers or torch-directml and restart."));
        } else {
            setState(QStringLiteral("READY"), QStringLiteral("Julia-1 is ready on %1 (%2). Scores come from local model inference.").arg(device, precision));
        }
#else
        setState(QStringLiteral("READY"), QStringLiteral("Julia-1 is ready on %1 (%2). Scores come from local model inference.").arg(device, precision));
#endif
        return;
    }
    if (result.value(QStringLiteral("snapshot_id")).toString() != request.snapshot) {
        setState(QStringLiteral("READY"), QStringLiteral("Julia rejected a result for another prompt snapshot."));
        emit evaluationFailed(id, QStringLiteral("Julia's result belongs to another prompt snapshot."));
        return;
    }
    if (result.value(QStringLiteral("plan_version")).toInt(-1) != 2 ||
        !result.value(QStringLiteral("groups")).isArray() || !result.value(QStringLiteral("assumptions")).isArray()) {
        emit evaluationFailed(id, QStringLiteral("Julia returned an incompatible scoring plan. Update the bundled worker."));
        setState(QStringLiteral("READY"), QStringLiteral("This result remains unranked because its scoring plan was incompatible."));
        return;
    }
    bridge::SnapshotEvaluationResponse response;
    response.success = true;
    response.engine = bridge::ScorerEngine::JuliaLocal;
    response.executedEngine = bridge::ScorerEngine::JuliaLocal;
    response.executionStatus = bridge::ExecutionStatus::ExecutedModel;
    response.modelInferenceExecuted = true;
    response.analysisSnapshotId = ss(request.snapshot);
    response.modelVersion = std::string(kModelId) + "@" + kRevision + "/" +
        ss(executionConfigurationId_ + QStringLiteral(".threads%1").arg(executionCpuThreads_));
    response.totalLatencyMs = object.value(QStringLiteral("latency_ms")).toDouble();
    for (const auto& value : result.value(QStringLiteral("groups")).toArray()) {
        const auto group = value.toObject();
        bridge::GroupEvaluationScore score;
        score.groupId = ss(group.value(QStringLiteral("group_id")).toString());
        score.isScored = true;
        score.ambiguityScore = group.value(QStringLiteral("ambiguity")).toDouble(-1);
        score.noulCertainty = group.value(QStringLiteral("noul_certainty")).toBool();
        score.winningAlternativeId = ss(group.value(QStringLiteral("winner")).toString());
        score.probabilities = std::vector<bridge::AlternativeProbability>{};
        for (const auto& entry : group.value(QStringLiteral("probabilities")).toArray()) {
            const auto probability = entry.toObject();
            score.probabilities->push_back({ss(probability.value(QStringLiteral("id")).toString()),
                                          probability.value(QStringLiteral("probability")).toDouble(-1)});
        }
        response.groupScores.push_back(std::move(score));
    }
    for (const auto& value : result.value(QStringLiteral("assumptions")).toArray()) {
        const auto assumption = value.toObject();
        bridge::AssumptionEvaluationScore score;
        score.assumptionId = ss(assumption.value(QStringLiteral("assumption_id")).toString());
        score.isScored = true;
        score.uncertaintyScore = assumption.value(QStringLiteral("uncertainty")).toDouble(-1);
        score.noulCertainty = assumption.value(QStringLiteral("noul_certainty")).toBool();
        response.assumptionScores.push_back(std::move(score));
    }
    setState(QStringLiteral("READY"), QStringLiteral("Julia-1 is ready. The last local scoring request completed."));
    emit evaluationFinished(id, response);
}

} // namespace roclint::host
