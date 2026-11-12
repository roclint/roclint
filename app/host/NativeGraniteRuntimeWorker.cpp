#include "NativeGraniteRuntimeWorker.hpp"
#include "GraniteConfig.hpp"
#include "OwnedProcessJob.hpp"
#include "roclint/core/Json.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

namespace roclint::host {
namespace {
constexpr qsizetype kMaxFrame = 1024 * 1024;
constexpr quint64 kMaxId = 9007199254740991ULL;
constexpr int kHelloMs = 3000;
constexpr int kLoadMs = 180000;
constexpr int kShutdownMs = 2000;
std::string ss(const QString& value) { return value.toUtf8().toStdString(); }
core::Error unavailable(const char* message) { return core::makeError(core::ErrorCode::ProviderUnavailable, message); }
core::Error stale() { return core::makeError(core::ErrorCode::StaleVersion, "The native model request was cancelled because its prompt or configuration changed."); }
}

struct NativeGraniteRuntimeWorker::Call {
    std::chrono::steady_clock::time_point deadline;
    qsizetype streamedBytes{0}; int streamedFrames{0}; // I/O-thread only
    qint64 streamedTokens{0}; double streamedDecodeMs{0.0}, streamedGenerationMs{0.0};
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<core::Result<std::string>> result;
    void finish(core::Result<std::string> value) {
        const std::lock_guard lock(mutex);
        if (result) return;
        if (value.ok() && std::chrono::steady_clock::now() >= deadline)
            value = core::makeError(core::ErrorCode::ValidationFailed, "NATIVE_TIMEOUT: Native request exceeded its monotonic deadline.");
        result = std::move(value);
        changed.notify_all();
    }
    bool done() { const std::lock_guard lock(mutex); return result.has_value(); }
};

struct NativeGraniteRuntimeWorker::Mailbox {
    std::mutex mutex;
    NativeGraniteRuntimeWorker* owner;
    std::vector<std::weak_ptr<Call>> calls;
    explicit Mailbox(NativeGraniteRuntimeWorker* target) : owner(target) {}
    template<class Function> bool post(Function function, const std::shared_ptr<Call>& call = {}) {
        const std::lock_guard lock(mutex);
        if (!owner) return false;
        if (call) {
            std::erase_if(calls, [](const auto& entry) { return entry.expired(); });
            if (calls.size() >= 8) return false;
            calls.push_back(call);
        }
        return QMetaObject::invokeMethod(owner, [target = owner, function = std::move(function)]() { function(target); }, Qt::QueuedConnection);
    }
    void cancel(bool close, const core::Error& reason = stale()) {
        const std::lock_guard lock(mutex);
        if (close) owner = nullptr;
        for (const auto& entry : calls) if (auto call = entry.lock()) call->finish(reason);
        calls.clear();
    }
};

QJsonObject NativeGraniteRuntimeWorker::manifest() { return QJsonDocument::fromJson(QByteArray(runtime::kGraniteManifestJson)).object(); }
QString NativeGraniteRuntimeWorker::defaultDirectory() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/models");
    const QString s2 = base + QStringLiteral("/s2");
    if (QFileInfo(s2).isDir()) return s2;
    const QString legacy = base + QStringLiteral("/granite-4.2-3b");
    if (QFileInfo(legacy).isDir()) return legacy;
    return s2;
}
QString NativeGraniteRuntimeWorker::modelTag(const QString& quantization) { return QStringLiteral("native-granite:") + quantization; }
QString NativeGraniteRuntimeWorker::quantizationForTag(const QString& tag) {
    for (const auto& entry : manifest().value(QStringLiteral("candidates")).toArray()) {
        const auto quantization = entry.toObject().value(QStringLiteral("quantization")).toString();
        if (modelTag(quantization) == tag) return quantization;
    }
    return {};
}
bool NativeGraniteRuntimeWorker::isNativeModel(const QString& model) { return !quantizationForTag(model).isEmpty(); }

NativeGraniteRuntimeWorker::NativeGraniteRuntimeWorker(QObject* parent, QString worker)
    : QObject(parent), mailbox_(std::make_shared<Mailbox>(this)), worker_(std::move(worker)) {
    if (worker_.isEmpty()) {
        worker_ = QCoreApplication::applicationDirPath() + QStringLiteral("/runtime/roclint_granite_worker");
#ifdef Q_OS_WIN
        worker_ += QStringLiteral(".exe");
#endif
    }
    clock_.start();
}

NativeGraniteRuntimeWorker::~NativeGraniteRuntimeWorker() {
    mailbox_->cancel(true);
    stop();
    if (auto* process = process_) {
        process->disconnect(this);
        if (!process->waitForFinished(kShutdownMs)) { killWorker(); static_cast<void>(process->waitForFinished(1000)); }
        process_ = nullptr;
        delete process;
        job_.reset();
    }
}

void NativeGraniteRuntimeWorker::state(const QString& value, const QString& detail) {
    if (value == QLatin1String("READY") && !quiescing_.isEmpty()) {
        ready_ = false; state_ = QStringLiteral("CANCELLING");
        detail_ = QStringLiteral("Waiting for cancelled native work to stop.");
        emit stateChanged(); return;
    }
    state_ = value; detail_ = detail; emit stateChanged();
}

bridge::GraniteModelInfo NativeGraniteRuntimeWorker::modelInfo() const {
    if (modelMetadata_.isEmpty()) return {};
    return {ss(identity_.value(QStringLiteral("model_id")).toString() + QLatin1Char('@') + identity_.value(QStringLiteral("model_revision")).toString()),
            ss(identity_.value(QStringLiteral("weights_sha256")).toString()), ss(identity_.value(QStringLiteral("quantization")).toString()), "local_native_llama_cpp"};
}

QString NativeGraniteRuntimeWorker::configurationDetail() const {
    const auto config = manifest();
    const auto sampling = config.value(QStringLiteral("sampling")).toObject();
    return QStringLiteral("model_revision=%1; llama_revision=%2; configuration=%3; template_sha256=%4; cpu_threads=%5; gpu_layers=%6; backend=%7; mmap=true; mlock=false")
        .arg(identity_.value(QStringLiteral("model_revision")).toString(), identity_.value(QStringLiteral("runtime_revision")).toString(),
             identity_.value(QStringLiteral("configuration_id")).toString(), modelMetadata_.value(QStringLiteral("template_sha256")).toString(),
             QString::number(config.value(QStringLiteral("cpu_threads")).toInt()), QString::number(config.value(QStringLiteral("gpu_layers")).toInt()),
             config.value(QStringLiteral("backend")).toString()) +
        QStringLiteral("; temperature=%1; top_p=%2; seed=%3; sampling_reference=%4")
            .arg(QString::number(sampling.value(QStringLiteral("temperature")).toDouble()),
                 QString::number(sampling.value(QStringLiteral("top_p")).toDouble()),
                 QString::number(sampling.value(QStringLiteral("seed")).toInteger()), config.value(QStringLiteral("sampling_reference")).toString()) +
        QStringLiteral("; grammar_strategy=%1; analysis_timeout_ms=%2; rewrite_timeout_ms=%3")
            .arg(sampling.value(QStringLiteral("grammar_strategy")).toString(), QString::number(runtime::kGraniteAnalysisTimeoutMs),
                 QString::number(runtime::kGraniteRewriteTimeoutMs)) +
        QStringLiteral("; analysis_instruction_contract=%1; rewrite_instruction_contract=%2")
            .arg(config.value(QStringLiteral("analysis_instruction_contract")).toString(), config.value(QStringLiteral("rewrite_instruction_contract")).toString()) +
        QStringLiteral("; output_format=%1; grammar_transform=%2; selected_device=%3; selected_device_description=%4")
            .arg(config.value(QStringLiteral("output_format")).toString(), config.value(QStringLiteral("grammar_transform")).toString(),
                 modelMetadata_.value(QStringLiteral("selected_device")).toString(), modelMetadata_.value(QStringLiteral("selected_device_description")).toString());
}

void NativeGraniteRuntimeWorker::stop() {
    ++restartEpoch_;
    next_.reset();
    beginStop(false, QStringLiteral("Native Granite was stopped."));
}

void NativeGraniteRuntimeWorker::killWorker() { if (job_ && job_->terminate()) return; if (process_) process_->kill(); }

void NativeGraniteRuntimeWorker::beginStop(bool failure, const QString& reason) {
    ready_ = false;
    warmAfterResidency_ = false;
    failure_ = failure;
    ++requestEpoch_;
    // Clear stale speed telemetry so the UI does not show a tok/s value from a
    // previous completed request while inference is stopped or has failed.
    resetSpeedMetrics();
    mailbox_->cancel(false, failure ? unavailable("Native Granite stopped before completing this request.") : stale());
    if (stopping_) { if (failure) state(QStringLiteral("FAILED"), reason); return; }
    stopping_ = true;
    for (auto& pending : pending_) if (pending.call) pending.call->finish(unavailable("Native Granite stopped before completing this request."));
    pending_.clear(); cancelled_.clear(); quiescing_.clear();
    incoming_.clear();  // Discard any partial frame so a rapid restart does not prepend stale bytes.
    if (!process_) {
        stopping_ = false; residency_ = QStringLiteral("UNLOADED"); modelMetadata_ = {};
        state(failure ? QStringLiteral("FAILED") : QStringLiteral("UNLOADED"), reason); emit stopped(); return;
    }
    state(failure ? QStringLiteral("FAILED") : QStringLiteral("STOPPING"), reason);
    if (hello_ && process_->state() == QProcess::Running) static_cast<void>(send(QStringLiteral("shutdown"), {}, 0));
    else process_->closeWriteChannel();
    const auto generation = generation_;
    QTimer::singleShot(kShutdownMs, this, [this, generation]() { if (generation_ == generation && stopping_) killWorker(); });
}

void NativeGraniteRuntimeWorker::fail(const QString& reason) { next_.reset(); beginStop(true, reason); }

void NativeGraniteRuntimeWorker::finished(QProcess* process) {
    if (process_ != process) return;
    const bool crashed = !stopping_;
    process_ = nullptr; job_.reset(); process->deleteLater();
    incoming_.clear(); outgoing_.clear(); hello_ = false; stopping_ = false;
    residency_ = QStringLiteral("UNLOADED"); modelMetadata_ = {};
    if (crashed) {
        const auto previous = launch_;
        fail(QStringLiteral("Native Granite exited. The failed semantic request was not replayed."));
        while (!restarts_.isEmpty() && clock_.elapsed() - restarts_.first() >= 60000) restarts_.removeFirst();
        if (previous && restarts_.size() < 2) {
            restarts_.append(clock_.elapsed());
            const auto epoch = restartEpoch_;
            state(QStringLiteral("RECOVERING"), QStringLiteral("Restarting the native model without replaying semantic work."));
            QTimer::singleShot(250, this, [this, previous, epoch]() { if (restartEpoch_ == epoch && !process_) { next_ = previous; startWorker(); } });
        }
        return;
    }
    pending_.clear();
    if (next_) startWorker();
    else { if (!failure_) state(QStringLiteral("UNLOADED"), QStringLiteral("Native Granite has stopped.")); emit stopped(); }
}

void NativeGraniteRuntimeWorker::load(const QString& directory, const QString& quantization) {
    ++restartEpoch_; restarts_.clear();
    if (process_ && !stopping_ && launch_ && launch_->directory == directory && launch_->quantization == quantization) {
        if (ready_) return;
        for (const auto& item : pending_) {
            const auto op = item.envelope.value(QStringLiteral("op")).toString();
            if (op == QLatin1String("hello") || op == QLatin1String("load") || op == QLatin1String("warm")) return;
            if (op == QLatin1String("set_residency")) { warmAfterResidency_ = true; return; }
        }
    }
    if (process_ && !stopping_ && hello_ && launch_ && launch_->directory == directory && launch_->quantization == quantization &&
        (residency_ == QLatin1String("IDLE_MAPPED") || residency_ == QLatin1String("UNLOADED"))) {
        ready_ = false; state(QStringLiteral("LOADING"), QStringLiteral("Warming the selected native Granite model."));
        static_cast<void>(send(QStringLiteral("warm"), {}, kLoadMs)); return;
    }
    next_ = Launch{directory, quantization};
    if (process_) beginStop(false, QStringLiteral("Changing native Granite configuration.")); else startWorker();
}

void NativeGraniteRuntimeWorker::startWorker() {
    launch_ = std::exchange(next_, std::nullopt);
    ready_ = false; hello_ = false; stopping_ = false; failure_ = false;
    cancelled_.clear(); quiescing_.clear(); modelMetadata_ = {};
    if (!launch_ || !QFileInfo(worker_).isExecutable()) { fail(QStringLiteral("The native Granite helper is missing. Build the pinned native runtime first.")); return; }
    const auto config = manifest();
    QJsonObject candidate;
    for (const auto& entry : config.value(QStringLiteral("candidates")).toArray())
        if (entry.toObject().value(QStringLiteral("quantization")).toString() == launch_->quantization) candidate = entry.toObject();
    if (candidate.isEmpty()) { fail(QStringLiteral("This native quantization is not a pinned candidate.")); return; }
    identity_ = {{QStringLiteral("provider"), QStringLiteral("granite_local")}, {QStringLiteral("model_id"), config.value(QStringLiteral("model_family"))},
        {QStringLiteral("model_revision"), config.value(QStringLiteral("model_revision"))}, {QStringLiteral("runtime_revision"), config.value(QStringLiteral("runtime_revision"))},
        {QStringLiteral("configuration_id"), config.value(QStringLiteral("configuration_id"))}, {QStringLiteral("quantization"), launch_->quantization},
        {QStringLiteral("weights_sha256"), candidate.value(QStringLiteral("sha256"))}};
    job_ = std::make_unique<OwnedProcessJob>();
    if (!job_->valid()) { fail(QStringLiteral("Native Granite process ownership could not be established.")); return; }
    process_ = new QProcess(this);
    process_->setWorkingDirectory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    // The helper lives in runtime/, while deployed Qt/MSVC DLLs live beside the host.
    // Configure only this owned child; do not require or modify the user's PATH.
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), QCoreApplication::applicationDirPath() + QDir::listSeparator() + environment.value(QStringLiteral("PATH")));
    process_->setProcessEnvironment(environment);
#endif
    ++generation_;
    auto* process = process_;
    job_->prepare(process);
    connect(process, &QProcess::started, this, [this, process]() {
        if (process_ != process) return;
        if (!job_->resume()) { killWorker(); fail(QStringLiteral("The native process could not join its owned process group.")); return; }
        process->write(outgoing_); outgoing_.clear();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() { if (process_ == process) readOutput(); });
    connect(process, &QProcess::readyReadStandardError, this, [process]() { static_cast<void>(process->readAllStandardError()); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process_ != process || stopping_ || error == QProcess::Crashed) return;
        fail(QStringLiteral("Native Granite could not run. Check its runtime deployment."));
        if (error == QProcess::FailedToStart) finished(process);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process](int, QProcess::ExitStatus) { finished(process); });
    state(QStringLiteral("STARTING"), QStringLiteral("Starting the pinned native Granite helper."));
    process->setProgram(worker_);
    process->setArguments({QStringLiteral("--model-dir"), launch_->directory, QStringLiteral("--quantization"), launch_->quantization});
    job_->start(process);
    static_cast<void>(send(QStringLiteral("hello"), {}, kHelloMs));
}

quint64 NativeGraniteRuntimeWorker::send(const QString& operation, const QJsonObject& payload, int timeoutMs, const RequestContext* context, std::shared_ptr<Call> call) {
    const auto acceptedAt = std::chrono::steady_clock::now();
    if (!process_ || pending_.size() >= 8 || nextId_ >= kMaxId) return 0;
    const auto id = ++nextId_;
    const auto control = QStringLiteral("native_control_%1").arg(id);
    QJsonObject envelope = identity_;
    envelope.insert(QStringLiteral("protocol_version"), 1);
    envelope.insert(QStringLiteral("type"), QStringLiteral("request"));
    envelope.insert(QStringLiteral("request_id"), QString::number(id));
    envelope.insert(QStringLiteral("op"), operation);
    envelope.insert(QStringLiteral("request_group_id"), context ? context->groupId : control);
    envelope.insert(QStringLiteral("attempt_id"), context ? context->attemptId : control);
    envelope.insert(QStringLiteral("session_id"), context ? context->sessionId : QStringLiteral("runtime"));
    envelope.insert(QStringLiteral("draft_revision_id"), context ? context->revisionId : QStringLiteral("runtime"));
    envelope.insert(QStringLiteral("state_version"), static_cast<qint64>(context ? context->stateVersion : 0));
    envelope.insert(QStringLiteral("intent_id"), context ? context->intentId : QString());
    envelope.insert(QStringLiteral("rewrite_profile_id"), context ? context->profileId : QString());
    const auto profile = context ? core::rewrite::ProfileRegistry::getProfile(ss(context->profileId)) : std::nullopt;
    envelope.insert(QStringLiteral("rewrite_profile_version"), profile ? static_cast<qint64>(profile->version) : 0);
    envelope.insert(QStringLiteral("input_hash"), QString::fromLatin1(QCryptographicHash::hash(context ? context->prompt.toUtf8() : QByteArray(), QCryptographicHash::Sha256).toHex()));
    auto object = envelope; object.insert(QStringLiteral("payload"), payload);
    const auto frame = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > kMaxFrame) return 0;
    if (call && (call->done() || std::chrono::steady_clock::now() >= call->deadline)) {
        call->finish(core::makeError(core::ErrorCode::ValidationFailed, "NATIVE_TIMEOUT: Native request expired before pipe submission.")); return 0;
    }
    const auto deadline = call ? call->deadline : timeoutMs > 0 ? acceptedAt + std::chrono::milliseconds(timeoutMs) :
        std::chrono::steady_clock::time_point::max();
    pending_.insert(id, Pending{envelope, std::move(call), deadline});
    if (process_->state() == QProcess::Running) process_->write(frame); else outgoing_.append(frame);
    if (timeoutMs > 0) QTimer::singleShot(timeoutMs, this, [this, id]() {
        const auto it = pending_.constFind(id);
        if (it == pending_.cend()) return;
        if (it->call) {
            const auto call = it->call;
            call->finish(core::makeError(core::ErrorCode::ValidationFailed, "NATIVE_TIMEOUT: Native Granite exceeded its operation deadline."));
            cancelCall(call);
        } else fail(QStringLiteral("The native helper exceeded its control/loading deadline."));
    });
    return id;
}

void NativeGraniteRuntimeWorker::readOutput() {
    incoming_.append(process_->read(65536));
    if (stopping_) {
        incoming_.clear();
        if (process_ && process_->bytesAvailable() > 0) QTimer::singleShot(0, this, [this]() { if (process_) readOutput(); });
        return;
    }
    for (int frames = 0; frames < 32; ++frames) {
        const auto end = incoming_.indexOf('\n');
        if (end < 0) break;
        const auto frame = incoming_.left(end); incoming_.remove(0, end + 1);
        const auto strict = core::json::parse(std::string_view(frame.constData(), static_cast<std::size_t>(frame.size())), {static_cast<std::size_t>(kMaxFrame), 32});
        if (frame.size() + 1 > kMaxFrame || !strict.ok() || !strict.value().isObject()) { fail(QStringLiteral("Native Granite returned malformed, duplicate-key or oversized JSON.")); return; }
        receive(QJsonDocument::fromJson(frame).object());
        if (!process_ || stopping_) return;
    }
    if (incoming_.size() >= kMaxFrame && !incoming_.contains('\n')) { fail(QStringLiteral("Native Granite exceeded its frame limit.")); return; }
    if (process_ && (process_->bytesAvailable() > 0 || incoming_.contains('\n')))
        QTimer::singleShot(0, this, [this]() { if (process_) readOutput(); });
}

void NativeGraniteRuntimeWorker::resetSpeedMetrics(quint64 requestId) {
    metricsRequestId_ = requestId;
    lastTokensPerSecond_ = 0.0;
    lastDecodeTokensPerSecond_ = 0.0;
    lastGenerationTokensPerSecond_ = 0.0;
    lastOutputTokens_ = 0;
    lastDecodeMs_ = 0.0;
    lastGenerationMs_ = 0.0;
    emit speedMetricsChanged();
}

bool NativeGraniteRuntimeWorker::updateSpeedMetrics(quint64 requestId, const QJsonObject& metrics, Call& call) {
    const auto countValue = metrics.value(QStringLiteral("output_tokens"));
    const auto decodeValue = metrics.value(QStringLiteral("decode_ms"));
    const auto generationValue = metrics.value(QStringLiteral("generation_ms"));
    const double count = countValue.toDouble(-1);
    const double decode = decodeValue.toDouble(-1);
    const double elapsed = generationValue.toDouble(-1);
    if (requestId != metricsRequestId_ || !countValue.isDouble() || !decodeValue.isDouble() || !generationValue.isDouble() ||
        !std::isfinite(count) || count <= 0 || std::floor(count) != count ||
        count > manifest().value(QStringLiteral("output_tokens")).toInt() || count < call.streamedTokens ||
        !std::isfinite(decode) || !std::isfinite(elapsed) || decode <= 0 || elapsed <= 0 || decode > elapsed ||
        decode < call.streamedDecodeMs || elapsed < call.streamedGenerationMs) return false;
    const double decSpeed = count * 1000.0 / decode;
    const double genSpeed = count * 1000.0 / elapsed;
    if (!std::isfinite(decSpeed) || !std::isfinite(genSpeed)) return false;
    call.streamedTokens = lastOutputTokens_ = static_cast<qint64>(count);
    call.streamedDecodeMs = lastDecodeMs_ = decode;
    call.streamedGenerationMs = lastGenerationMs_ = elapsed;
    lastDecodeTokensPerSecond_ = decSpeed;
    lastGenerationTokensPerSecond_ = lastTokensPerSecond_ = genSpeed;
    return true;
}

void NativeGraniteRuntimeWorker::receive(const QJsonObject& response) {
    // Streaming chunk — dispatched before the final result frame.
    if (response.value(QStringLiteral("type")) == QLatin1String("partial_result")) {
        bool valid = false;
        const auto id = response.value(QStringLiteral("request_id")).toString().toULongLong(&valid);
        if (valid && pending_.contains(id)) {
            auto& pending = pending_[id];
            if (!pending.call) return;
            for (auto it = pending.envelope.begin(); it != pending.envelope.end(); ++it)
                if (it.key() != QLatin1String("type") && response.value(it.key()) != it.value()) {
                    fail(QStringLiteral("Native stream failed its request binding.")); return;
                }
            const auto chunk = response.value(QStringLiteral("chunk")).toString();
            pending.call->streamedBytes += chunk.toUtf8().size();
            if (++pending.call->streamedFrames > 2048 || pending.call->streamedBytes > 65536) {
                fail(QStringLiteral("Native stream exceeded its output budget.")); return;
            }
            // Optional telemetry is non-authoritative: malformed/backwards metrics
            // never change accepted text, request binding or model-result validation.
            if (updateSpeedMetrics(id, response, *pending.call)) {
                emit speedMetricsChanged();
            }
            if (!chunk.isEmpty()) emit partialResultReceived(id, chunk);
        }
        return;
    }
    bool valid = false;
    const auto idText = response.value(QStringLiteral("request_id"));
    const auto id = idText.toString().toULongLong(&valid);
    if (!idText.isString() || !valid || id == 0 || id > kMaxId || QString::number(id) != idText.toString()) { fail(QStringLiteral("Native reply ID was invalid.")); return; }
    const auto pending = pending_.constFind(id);
    const auto cancelled = cancelled_.constFind(id);
    if (pending != pending_.cend() && std::chrono::steady_clock::now() >= pending->deadline) {
        if (pending->call) {
            const auto call = pending->call;
            if (id == metricsRequestId_) resetSpeedMetrics();
            call->finish(core::makeError(core::ErrorCode::ValidationFailed, "NATIVE_TIMEOUT: Native reply arrived after its deadline."));
            pending_.remove(id);
            state(QStringLiteral("READY"), QStringLiteral("The late native result was discarded."));
        } else fail(QStringLiteral("The native helper exceeded its control/loading deadline."));
        return;
    }
    if (pending == pending_.cend() && cancelled == cancelled_.cend()) { fail(QStringLiteral("Native reply ID was unknown or duplicated.")); return; }
    const auto expected = pending != pending_.cend() ? pending->envelope : cancelled.value();
    for (auto it = expected.begin(); it != expected.end(); ++it)
        if (it.key() != QLatin1String("type") && response.value(it.key()) != it.value()) { fail(QStringLiteral("Native reply failed context/configuration verification.")); return; }
    if (!response.value(QStringLiteral("ok")).isBool()) { fail(QStringLiteral("Native reply status was malformed.")); return; }
    const bool ok = response.value(QStringLiteral("ok")).toBool();
    const auto op = expected.value(QStringLiteral("op")).toString();
    const auto type = !ok ? QStringLiteral("error") : op == QLatin1String("hello") ? QStringLiteral("hello_ack") :
        op == QLatin1String("runtime_status") ? QStringLiteral("runtime_status") : QStringLiteral("result");
    if (response.value(QStringLiteral("type")) != type) { fail(QStringLiteral("Native reply message type was incompatible.")); return; }
    if (cancelled != cancelled_.cend()) {
        quiescing_.remove(id);
        if (quiescing_.isEmpty() && !stopping_) { ready_ = true; state(QStringLiteral("READY"), QStringLiteral("Cancelled native work has stopped.")); }
        return;
    }
    const auto request = pending_.take(id);
    if (!ok) {
        if (request.call) {
            if (id == metricsRequestId_) resetSpeedMetrics();
            const auto code = response.value(QStringLiteral("error_code")).toString();
            const bool tooLarge = code == QLatin1String("INPUT_TOO_LARGE");
            request.call->finish(core::makeError(tooLarge ? core::ErrorCode::PreconditionFailed : core::ErrorCode::ValidationFailed,
                tooLarge ? "The exact request exceeds the native token budget. Shorten it or continue manually; nothing was truncated." :
                code == QLatin1String("TIMEOUT") ? "NATIVE_TIMEOUT: Native Granite exceeded its operation deadline; no partial result was accepted." :
                code == QLatin1String("OUTPUT_TOO_LARGE") ? "NATIVE_OUTPUT_LIMIT: Native Granite exceeded its output budget; no partial result was accepted." :
                           "Native Granite could not complete this operation within its bounded output/deadline contract."));
            state(QStringLiteral("READY"), QStringLiteral("The request failed; manual editing remains available."));
        } else {
            const auto code = response.value(QStringLiteral("error_code")).toString();
            if (code == QLatin1String("ACCELERATOR_UNAVAILABLE"))
                fail(QStringLiteral("The packaged native GPU backend is unavailable on this computer. Check the graphics driver or use a compatible runtime build; the model files were preserved."));
            else fail(QStringLiteral("Native Granite could not load or verify the pinned model. Download/repair the selected candidate or choose its correct model folder."));
        }
        return;
    }
    const auto result = response.value(QStringLiteral("result")).toObject();
    if (op == QLatin1String("hello")) {
        if (result.value(QStringLiteral("diagnostic_unbounded")).toBool(false)) {
            fail(QStringLiteral("This helper is running without semantic deadlines for a development investigation. Normal app requests require bounded mode.")); return;
        }
        const auto operations = result.value(QStringLiteral("operations")).toArray();
        bool compatible = result.value(QStringLiteral("protocol_version")) == QJsonValue(1);
        for (const auto* required : {"load", "warm", "granite_analyze", "granite_rewrite", "runtime_status", "cancel", "set_residency", "shutdown"})
            compatible = compatible && operations.contains(QJsonValue(QString::fromLatin1(required)));
        if (!compatible) {
            fail(QStringLiteral("Native Granite helper capabilities are incompatible.")); return;
        }
        hello_ = true; state(QStringLiteral("LOADING"), QStringLiteral("Verifying, mapping and warming the pinned native model."));
        static_cast<void>(send(QStringLiteral("load"), {}, kLoadMs)); return;
    }
    if (op == QLatin1String("cancel")) return;
    if (request.call) {
        const auto output = result.value(QStringLiteral("text"));
        if (id == metricsRequestId_ && !updateSpeedMetrics(id, result, *request.call)) resetSpeedMetrics(id);
        if (!output.isString() || output.toString().toUtf8().size() > 64 * 1024) request.call->finish(core::makeError(core::ErrorCode::ValidationFailed, "Native output was absent or oversized."));
        else request.call->finish(ss(output.toString()));
        state(QStringLiteral("READY"), QStringLiteral("Native Granite completed this request locally."));
        emit speedMetricsChanged();
        return;
    }
    const auto resource = result.value(QStringLiteral("residency")).toString();
    const auto readiness = result.value(QStringLiteral("state")).toString();
    if (!QStringList{QStringLiteral("HOT"), QStringLiteral("IDLE_MAPPED"), QStringLiteral("UNLOADED")}.contains(resource) ||
        (resource == QLatin1String("HOT") && (readiness != QLatin1String("READY") || result.value(QStringLiteral("template_sha256")).toString().size() != 64))) {
        fail(QStringLiteral("Native model readiness/resource metadata was invalid.")); return;
    }
    modelMetadata_ = result;
    residency_ = resource; ready_ = resource == QLatin1String("HOT");
    const bool warmRequested = std::exchange(warmAfterResidency_, false);
    state(ready_ ? QStringLiteral("READY") : QStringLiteral("UNLOADED"), ready_ ? QStringLiteral("Pinned native Granite is ready (%1).").arg(manifest().value(QStringLiteral("backend")).toString()) :
          resource == QLatin1String("IDLE_MAPPED") ? QStringLiteral("Native weights remain mapped; the context will warm on demand.") : QStringLiteral("Native model resources were released."));
    if (warmRequested && !ready_ && !stopping_) {
        state(QStringLiteral("LOADING"), QStringLiteral("Warming native Granite for the requested operation."));
        static_cast<void>(send(QStringLiteral("warm"), {}, kLoadMs));
    }
}

void NativeGraniteRuntimeWorker::cancelCall(const std::shared_ptr<Call>& call) {
    const auto ids = pending_.keys();
    for (const auto id : ids) {
        const auto it = pending_.find(id);
        if (it == pending_.end() || it->call != call) continue;
        if (id == metricsRequestId_) resetSpeedMetrics();
        cancelled_.insert(id, it->envelope); pending_.erase(it);
        quiescing_.insert(id);
        const auto generation = generation_;
        QTimer::singleShot(2000, this, [this, id, generation]() {
            if (generation == generation_ && quiescing_.contains(id)) {
                fail(QStringLiteral("Native Granite did not stop cancelled work within its deadline.")); killWorker();
            }
        });
        call->finish(stale());
        static_cast<void>(send(QStringLiteral("cancel"), {{QStringLiteral("target_request_id"), QString::number(id)}}, kHelloMs));
    }
    if (!quiescing_.isEmpty() && !stopping_) { ready_ = false; state(QStringLiteral("CANCELLING"), QStringLiteral("Waiting for cancelled native work to stop.")); }
    if (cancelled_.size() >= 256) fail(QStringLiteral("Native cancellation history is full. Reload the helper before continuing."));
}

void NativeGraniteRuntimeWorker::cancelAll() {
    ++requestEpoch_;
    mailbox_->cancel(false);
    const auto pending = pending_;
    for (const auto& item : pending) if (item.call) cancelCall(item.call);
}

void NativeGraniteRuntimeWorker::setResidency(const QString& tier) {
    if (!process_ || stopping_ || !hello_) return;
    for (const auto& item : pending_) if (item.call) return;
    if (tier == residency_) return;
    ready_ = false;
    state(QStringLiteral("LOADING"), QStringLiteral("Applying native Granite resource policy."));
    static_cast<void>(send(QStringLiteral("set_residency"), {{QStringLiteral("tier"), tier}}, kLoadMs));
}

void NativeGraniteRuntimeWorker::submit(const RequestContext& context, const QString& operation, const QString& instructions,
                                  const QString& schema, const std::shared_ptr<Call>& call) {
    if (call->done()) return;
    if (std::chrono::steady_clock::now() >= call->deadline) { call->finish(core::makeError(core::ErrorCode::ValidationFailed, "NATIVE_TIMEOUT: Native request expired before dispatch.")); return; }
    if (!ready_ || stopping_ || !hello_) { call->finish(unavailable("Native Granite is not ready.")); return; }
    if (context.groupId.isEmpty() || context.attemptId.isEmpty() || context.sessionId.isEmpty() || context.revisionId.isEmpty() || context.stateVersion > kMaxId) {
        call->finish(core::makeError(core::ErrorCode::PreconditionFailed, "Native request is missing its authoritative context.")); return;
    }
    if (operation == QLatin1String("granite_rewrite") && (context.intentId.isEmpty() || !core::rewrite::ProfileRegistry::isKnownProfile(ss(context.profileId)))) {
        call->finish(core::makeError(core::ErrorCode::PreconditionFailed, "Native rewrite is missing its confirmed intent or versioned profile.")); return;
    }
    if (context.prompt.toUtf8().size() > 64 * 1024 || context.prompt.contains(QLatin1Char('\r')) || instructions.toUtf8().size() > 256 * 1024 || schema.toUtf8().size() > 64 * 1024) {
        call->finish(core::makeError(core::ErrorCode::PreconditionFailed, "The exact native request exceeds its byte budget or is not canonical LF; nothing was truncated.")); return;
    }
    for (const auto& id : {context.groupId, context.attemptId, context.sessionId, context.revisionId, context.intentId, context.profileId})
        if (id.toUtf8().size() > 256) { call->finish(core::makeError(core::ErrorCode::PreconditionFailed, "Native request binding exceeded its limit.")); return; }
    const QJsonObject payload{{QStringLiteral("prompt"), context.prompt}, {QStringLiteral("instructions"), instructions}, {QStringLiteral("schema"), schema}};
    const auto timeout = std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(call->deadline - std::chrono::steady_clock::now()).count()));
    const auto id = send(operation, payload, timeout, &context, call);
    if (id == 0) { call->finish(unavailable("Native Granite could not accept this request.")); return; }
    resetSpeedMetrics(id);
    state(QStringLiteral("BUSY"), QStringLiteral("Native Granite is processing the frozen request locally."));
}

bridge::GraniteTransport NativeGraniteRuntimeWorker::transportFor(const RequestContext& context, const QString& operation, const std::string& schema, quint64 generation, quint64 epoch, int budgetOverride) const {
    const auto mailbox = mailbox_;
    const auto budget = budgetOverride > 0 ? budgetOverride : operation == QLatin1String("granite_analyze") ? runtime::kGraniteAnalysisTimeoutMs : runtime::kGraniteRewriteTimeoutMs;
    return [mailbox, context, operation, schema = QString::fromUtf8(schema.data(), static_cast<qsizetype>(schema.size())), generation, epoch, budget](std::string_view instructions) -> core::Result<std::string> {
        if (!QCoreApplication::instance() || QThread::currentThread() == QCoreApplication::instance()->thread())
            return core::makeError(core::ErrorCode::PreconditionFailed, "Native inference must run off the GUI thread.");
        auto call = std::make_shared<Call>();
        call->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget);
        const auto prompt = QString::fromUtf8(instructions.data(), static_cast<qsizetype>(instructions.size()));
        if (!mailbox->post([context, operation, schema, prompt, call, generation, epoch](NativeGraniteRuntimeWorker* runtime) {
            if (runtime->generation_ != generation || runtime->requestEpoch_ != epoch) { call->finish(stale()); return; }
            runtime->submit(context, operation, prompt, schema, call);
        }, call)) return unavailable("Native runtime could not accept this request.");
        std::unique_lock lock(call->mutex);
        if (!call->changed.wait_until(lock, call->deadline, [&]() { return call->result.has_value(); })) {
            lock.unlock();
            call->finish(core::makeError(core::ErrorCode::ValidationFailed, "NATIVE_TIMEOUT: Native request exceeded its monotonic deadline."));
            mailbox->post([call](NativeGraniteRuntimeWorker* runtime) { runtime->cancelCall(call); });
            lock.lock();
        }
        return *call->result;
    };
}
} // namespace roclint::host
