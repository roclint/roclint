#include "GraniteEngine.hpp"
#include "GraniteConfig.hpp"
#include "GraniteQueuePolicy.hpp"
#include "GraniteTimingTrace.hpp"
#include "roclint/core/Json.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"

#include <llama.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>

#include <QStringList>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using roclint::runtime::GraniteEngine;
using roclint::runtime::GraniteStreamProgress;
using roclint::runtime::GraniteFailure;
constexpr qsizetype kMaxFrame = 1024 * 1024;
constexpr quint64 kMaxId = 9007199254740991ULL;
constexpr int kLoadBudgetMs = 180000;
std::mutex outputMutex;

void emitFrame(const QJsonObject& object) {
    const auto frame = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > kMaxFrame) throw GraniteFailure(QStringLiteral("FRAME_LIMIT"), "Native response exceeded the frame limit.");
    const std::lock_guard lock(outputMutex);
    static_cast<void>(std::fwrite(frame.constData(), 1, static_cast<std::size_t>(frame.size()), stdout));
    static_cast<void>(std::fflush(stdout));
}

QJsonObject reply(const QJsonObject& request, const QJsonObject& result, const QString& error = {}, const QString& code = {}, double latency = 0,
                  const QJsonObject& metrics = {}) {
    auto response = request;
    response.remove(QStringLiteral("payload"));
    const auto op = request.value(QStringLiteral("op")).toString();
    response.insert(QStringLiteral("type"), !error.isEmpty() ? QStringLiteral("error") : op == QLatin1String("hello") ? QStringLiteral("hello_ack") :
                    op == QLatin1String("runtime_status") ? QStringLiteral("runtime_status") : QStringLiteral("result"));
    response.insert(QStringLiteral("ok"), error.isEmpty());
    response.insert(QStringLiteral("latency_ms"), latency);
    if (error.isEmpty()) response.insert(QStringLiteral("result"), result);
    else {
        response.insert(QStringLiteral("error"), error);
        response.insert(QStringLiteral("error_code"), code);
        if (!metrics.isEmpty()) response.insert(QStringLiteral("metrics"), metrics);
    }
    return response;
}

// Windows only reads available bytes: an idle input thread holds no CRT stdin
// lock during native-library initialization. POSIX uses an ordinary pipe read.
bool readFrame(QByteArray& buffer, QByteArray& frame) {
    while (true) {
        const auto end = buffer.indexOf('\n');
        if (end >= 0) {
            frame = buffer.left(end + 1);
            buffer.remove(0, end + 1);
            return true;
        }
        if (buffer.size() >= kMaxFrame) throw GraniteFailure(QStringLiteral("FRAME_LIMIT"), "Native request exceeded the frame limit.");
        char bytes[16384];
#ifdef Q_OS_WIN
        const auto handle = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) {
                if (!buffer.isEmpty()) throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native input ended inside a frame.");
                return false;
            }
            throw GraniteFailure(QStringLiteral("PIPE"), "Native input pipe failed.");
        }
        if (available == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        DWORD received = 0;
        if (!ReadFile(handle, bytes, std::min<DWORD>(available, static_cast<DWORD>(sizeof(bytes))), &received, nullptr))
            throw GraniteFailure(QStringLiteral("PIPE"), "Native input pipe failed.");
        if (received == 0) return false;
        buffer.append(bytes, static_cast<qsizetype>(received));
#else
        const auto received = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (received == 0) {
            if (!buffer.isEmpty()) throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native input ended inside a frame.");
            return false;
        }
        if (received < 0) throw GraniteFailure(QStringLiteral("PIPE"), "Native input pipe failed.");
        buffer.append(bytes, static_cast<qsizetype>(received));
#endif
    }
}

quint64 requestId(const QJsonObject& object) {
    bool valid = false;
    const auto value = object.value(QStringLiteral("request_id"));
    const auto id = value.toString().toULongLong(&valid);
    if (!value.isString() || !valid || id == 0 || id > kMaxId || QString::number(id) != value.toString())
        throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native request identity is invalid.");
    return id;
}

QJsonObject decode(const QByteArray& frame, const QJsonObject& identity) {
    const auto strict = roclint::core::json::parse(std::string_view(frame.constData(), static_cast<std::size_t>(frame.size())),
                                                 {static_cast<std::size_t>(kMaxFrame), 32});
    if (frame.size() > kMaxFrame || !frame.endsWith('\n') || !strict.ok() || !strict.value().isObject())
        throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native request is malformed or oversized.");
    const auto request = QJsonDocument::fromJson(frame).object();
    if (request.value(QStringLiteral("protocol_version")) != QJsonValue(1) || request.value(QStringLiteral("type")) != QLatin1String("request"))
        throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native protocol version or message type is incompatible.");
    static_cast<void>(requestId(request));
    for (const auto* key : {"request_group_id", "attempt_id", "session_id", "draft_revision_id"}) {
        const auto value = request.value(QLatin1String(key));
        if (!value.isString() || value.toString().isEmpty() || value.toString().toUtf8().size() > 256)
            throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native request binding is invalid.");
    }
    const auto version = roclint::core::json::requireUInt(strict.value(), "state_version");
    if (!version.ok() || version.value() > kMaxId || !request.value(QStringLiteral("payload")).isObject())
        throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native state version or payload is invalid.");
    for (auto it = identity.begin(); it != identity.end(); ++it)
        if (request.value(it.key()) != it.value()) throw GraniteFailure(QStringLiteral("IDENTITY"), "Native model/runtime configuration did not match the pinned build.");
    const auto hash = request.value(QStringLiteral("input_hash")).toString();
    if (hash.size() != 64 || std::any_of(hash.begin(), hash.end(), [](QChar value) {
            return !(value >= QLatin1Char('0') && value <= QLatin1Char('9')) && !(value >= QLatin1Char('a') && value <= QLatin1Char('f'));
        })) throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native input hash is invalid.");
    if (request.contains(QStringLiteral("surface"))) {
        const auto surface = request.value(QStringLiteral("surface"));
        if (!surface.isString() || surface.toString().size() > 32 ||
            (!surface.toString().isEmpty() && !QStringList{QStringLiteral("word"), QStringLiteral("interview"),
                QStringLiteral("rewrite"), QStringLiteral("review"), QStringLiteral("apply")}.contains(surface.toString())))
            throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native surface binding is invalid.");
    }
    if (request.contains(QStringLiteral("remaining_budget_ms"))) {
        const auto budget = request.value(QStringLiteral("remaining_budget_ms"));
        const auto value = budget.toDouble(-1);
        if (!budget.isDouble() || value < 0.0 || value > 3600000.0 || std::floor(value) != value)
            throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native remaining-budget binding is invalid.");
    }
    const auto op = request.value(QStringLiteral("op")).toString();
    if (op == QLatin1String("granite_analyze") || op == QLatin1String("granite_rewrite") ||
        op == QLatin1String("granite_word_refinement")) {
        const auto payload = request.value(QStringLiteral("payload")).toObject();
        if (request.contains("thinking_enabled") &&
            (!request.value("thinking_enabled").isBool() || payload.value("thinking_enabled") != request.value("thinking_enabled")))
            throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native reasoning mode failed its request binding.");
        const auto prompt = payload.value(QStringLiteral("prompt"));
        const auto bytes = prompt.toString().toUtf8();
        if (!prompt.isString() || bytes.size() > 64 * 1024 || bytes.contains('\r') ||
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != hash.toLatin1())
            throw GraniteFailure(QStringLiteral("INPUT_TOO_LARGE"), "Native input hash, text encoding or prompt budget did not match.");
        if (op == QLatin1String("granite_rewrite")) {
            const auto profile = roclint::core::rewrite::ProfileRegistry::getProfile(request.value(QStringLiteral("rewrite_profile_id")).toString().toUtf8().toStdString());
            if (request.value(QStringLiteral("intent_id")).toString().isEmpty() || !profile ||
                request.value(QStringLiteral("rewrite_profile_version")).toInt(-1) != static_cast<int>(profile->version))
                throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native rewrite is missing confirmed-intent/profile binding.");
        }
    }
    return request;
}

struct Work {
    QJsonObject request;
    std::shared_ptr<std::atomic_bool> cancelled;
    Clock::time_point deadline;
    Clock::time_point receivedAt;
};

QString traceSurface(const QJsonObject& request, const QString& operation) {
    const auto declared = request.value(QStringLiteral("surface")).toString();
    if (!declared.isEmpty()) return declared;
    if (operation == QLatin1String("granite_word_refinement")) return QStringLiteral("word");
    if (operation == QLatin1String("granite_rewrite")) return QStringLiteral("rewrite");
    return {};
}

QString traceIdOf(const QJsonObject& request) { return request.value(QStringLiteral("request_group_id")).toString(); }
QString traceRequestOf(const QJsonObject& request) { return request.value(QStringLiteral("attempt_id")).toString(); }
QString traceRevisionOf(const QJsonObject& request) { return request.value(QStringLiteral("draft_revision_id")).toString(); }

// The instruction contract id is a bounded identifier (for example
// granite.discovery.selective.v11), never instruction text. Emitted only when
// the first line matches a conservative identifier shape.
QString traceContract(const QJsonObject& request) {
    const auto instructions = request.value(QStringLiteral("payload")).toObject()
        .value(QStringLiteral("instructions")).toString();
    auto line = instructions.section(QLatin1Char('\n'), 0, 0).trimmed();
    const QString prefix = QStringLiteral("Instruction contract: ");
    if (line.startsWith(prefix)) line = line.mid(prefix.size());
    if (line.isEmpty() || line.size() > 64) return {};
    for (const auto character : line)
        if (!character.isLetterOrNumber() && character != QLatin1Char('_') && character != QLatin1Char('.') &&
            character != QLatin1Char('-')) return {};
    return line;
}

void trace(const QJsonObject& request, const QString& stage, const QString& phase, double elapsedMs,
           QJsonObject detail = {}) {
    detail.insert(QStringLiteral("operation"), request.value(QStringLiteral("op")).toString());
    detail.insert(QStringLiteral("wire_request_id"), request.value(QStringLiteral("request_id")).toString());
    detail.insert(QStringLiteral("attempt_id"), traceRequestOf(request));
    detail.insert(QStringLiteral("group_id"), traceIdOf(request));
    const auto contract = traceContract(request);
    if (!contract.isEmpty()) detail.insert(QStringLiteral("contract"), contract);
    const auto payload = request.value(QStringLiteral("payload")).toObject();
    const auto inputBytes = payload.value(QStringLiteral("prompt")).toString().toUtf8().size() +
        payload.value(QStringLiteral("instructions")).toString().toUtf8().size() +
        payload.value(QStringLiteral("schema")).toString().toUtf8().size();
    if (inputBytes > 0) detail.insert(QStringLiteral("input_bytes"), static_cast<qint64>(inputBytes));
    roclint::runtime::GraniteTimingTrace::instance().event(stage, phase, traceIdOf(request),
        traceRequestOf(request), traceRevisionOf(request),
        traceSurface(request, request.value(QStringLiteral("op")).toString()), elapsedMs, detail);
}

// Only counts, identifiers, durations and outcomes. Never prompt, instruction,
// schema, token or source text.
QJsonObject measuredDetail(const QJsonObject& source) {
    QJsonObject detail;
    for (const auto* key : {"prefill_ms", "decode_ms", "first_token_ms", "first_answer_token_ms",
             "sampler_ms", "generation_ms", "latency_ms", "setup_ms", "finish_ms", "prepare_ms",
             "verify_ms", "model_load_ms", "context_init_ms", "warmup_ms", "total_ms"}) {
        const auto value = source.value(QLatin1String(key));
        if (value.isDouble()) detail.insert(QLatin1String(key), value);
    }
    for (const auto* key : {"input_tokens", "output_tokens", "reasoning_tokens", "answer_tokens",
             "control_tokens", "kv_cache_reused_tokens", "kv_cache_candidate_tokens",
             "reasoning_budget", "answer_budget"}) {
        const auto value = source.value(QLatin1String(key));
        if (value.isDouble()) detail.insert(QLatin1String(key), value);
    }
    for (const auto* key : {"phase", "reasoning_stop", "reasoning_profile", "state", "residency"}) {
        const auto value = source.value(QLatin1String(key));
        if (value.isString()) detail.insert(QLatin1String(key), value);
    }
    const auto output = source.value(QStringLiteral("text"));
    if (output.isString()) detail.insert(QStringLiteral("output_bytes"), static_cast<qint64>(output.toString().toUtf8().size()));
    return detail;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("model-dir"), QStringLiteral("Directory containing pinned Granite GGUF files."), QStringLiteral("directory")});
    parser.addOption({QStringLiteral("quantization"), QStringLiteral("Pinned benchmark candidate."), QStringLiteral("quantization")});
    parser.addOption({QStringLiteral("sampling-temperature"),
        QStringLiteral("Development-only sampling experiment; overrides the pinned temperature. Not a release configuration."),
        QStringLiteral("value")});
    parser.addOption({QStringLiteral("no-thinking-reference"), QStringLiteral("Development-only A/B reference: disable the bounded reasoning phase.")});
    parser.addOption({QStringLiteral("cpu-reference"),
        QStringLiteral("Development-only benchmark reference: force gpu_layers=0 with the same weights.")});
    parser.addOption({QStringLiteral("diagnostic-no-deadlines"), QStringLiteral("Development investigation only: disable semantic operation deadlines; cancellation and shutdown remain active.")});
    parser.process(app);
    roclint::runtime::GraniteTimingTrace::instance().identify(QStringLiteral("granite_helper"));
    if (!parser.isSet(QStringLiteral("model-dir")) || !parser.isSet(QStringLiteral("quantization"))) return 2;
    std::optional<double> temperatureOverride;
    if (parser.isSet(QStringLiteral("sampling-temperature"))) {
        bool valid = false;
        const auto value = parser.value(QStringLiteral("sampling-temperature")).toDouble(&valid);
        if (!valid || value <= 0.0 || value > 4.0) return 2;
        temperatureOverride = value;
    }
    const std::optional<int> gpuLayersOverride = parser.isSet(QStringLiteral("cpu-reference")) ? std::optional<int>(0) : std::nullopt;
    llama_log_set([](ggml_log_level level, const char* message, void*) {
        // Do not enable dependency DEBUG/payload tracing in the resident helper.
        if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR) static_cast<void>(std::fputs(message, stderr));
    }, nullptr);
    try {
        const bool diagnosticUnbounded = parser.isSet(QStringLiteral("diagnostic-no-deadlines"));
        GraniteEngine engine(parser.value(QStringLiteral("model-dir")), parser.value(QStringLiteral("quantization")),
                             temperatureOverride, gpuLayersOverride, parser.isSet(QStringLiteral("no-thinking-reference")));
        const auto identity = engine.identity();
        const auto initialStatus = engine.status();
        std::mutex mutex;
        std::condition_variable available;
        std::deque<Work> queue;
        std::map<quint64, std::shared_ptr<std::atomic_bool>> pending;
        std::atomic_bool stopping{false};
        std::atomic_int exitCode{0};
        std::thread reader([&]() {
            QByteArray buffer, frame;
            quint64 lastId = 0;
            bool hello = false;
            try {
                while (readFrame(buffer, frame)) {
                    const auto request = decode(frame, identity);
                    const auto id = requestId(request);
                    if (id <= lastId) throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native request IDs must be unique and increasing.");
                    lastId = id;
                    const auto op = request.value(QStringLiteral("op")).toString();
                    const auto payload = request.value(QStringLiteral("payload")).toObject();
                    if (op == QLatin1String("hello")) {
                        if (hello) throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native handshake was repeated.");
                        hello = true;
                        emitFrame(reply(request, {{QStringLiteral("protocol_version"), 1},
                            {QStringLiteral("diagnostic_unbounded"), diagnosticUnbounded},
                            {QStringLiteral("sampling_temperature"), initialStatus.value(QStringLiteral("sampling_temperature"))},
                            {QStringLiteral("kv_prefix_cache"), initialStatus.value(QStringLiteral("kv_prefix_cache"))},
                            {QStringLiteral("cpu_reference"), gpuLayersOverride.has_value()},
                            {QStringLiteral("thinking_reference_disabled"), initialStatus.value(QStringLiteral("thinking_reference_disabled"))},
                            {QStringLiteral("bounded_reasoning"), initialStatus.value(QStringLiteral("bounded_reasoning"))},
                            {QStringLiteral("gpu_layers"), initialStatus.value(QStringLiteral("gpu_layers"))},
                            {QStringLiteral("analysis_timeout_ms"), diagnosticUnbounded ? 0 : roclint::runtime::kGraniteAnalysisTimeoutMs},
                            {QStringLiteral("rewrite_timeout_ms"), diagnosticUnbounded ? 0 : roclint::runtime::kGraniteRewriteTimeoutMs},
                            {QStringLiteral("word_refinement_timeout_ms"), roclint::runtime::kGraniteWordRefinementTimeoutMs},
                            {QStringLiteral("operations"), QJsonArray{
                            QStringLiteral("load"), QStringLiteral("warm"), QStringLiteral("granite_analyze"), QStringLiteral("granite_rewrite"),
                            QStringLiteral("granite_word_refinement"),
                            QStringLiteral("runtime_status"), QStringLiteral("set_residency"), QStringLiteral("reset_cache"), QStringLiteral("cancel"), QStringLiteral("shutdown")}}}));
                    } else if (!hello) throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native handshake is required before work.");
                    else if (op == QLatin1String("runtime_status")) emitFrame(reply(request, engine.status()));
                    else if (op == QLatin1String("shutdown")) {
                        stopping.store(true);
                        emitFrame(reply(request, {{QStringLiteral("state"), QStringLiteral("STOPPING")}}));
                        break;
                    } else if (op == QLatin1String("cancel")) {
                        bool valid = false;
                        const auto target = payload.value(QStringLiteral("target_request_id")).toString().toULongLong(&valid);
                        if (!valid || target == 0 || target >= id) throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native cancellation target is invalid.");
                        bool found = false;
                        {
                            const std::lock_guard lock(mutex);
                            const auto it = pending.find(target);
                            found = it != pending.end();
                            if (found) it->second->store(true);
                        }
                        emitFrame(reply(request, {{QStringLiteral("cancelled"), found}}));
                    } else if (op == QLatin1String("load") || op == QLatin1String("warm") || op == QLatin1String("reset_cache") ||
                               op == QLatin1String("set_residency") || op == QLatin1String("granite_analyze") ||
                               op == QLatin1String("granite_rewrite") || op == QLatin1String("granite_word_refinement")) {
                        const bool semantic = op == QLatin1String("granite_analyze") || op == QLatin1String("granite_rewrite");
                        const auto budget = op == QLatin1String("granite_analyze") ? roclint::runtime::kGraniteAnalysisTimeoutMs :
                            op == QLatin1String("granite_rewrite") ? roclint::runtime::kGraniteRewriteTimeoutMs :
                            op == QLatin1String("granite_word_refinement") ? roclint::runtime::kGraniteWordRefinementTimeoutMs : kLoadBudgetMs;
                        const auto receivedAt = Clock::now();
                        std::optional<std::chrono::milliseconds> callerRemaining;
                        if (request.contains(QStringLiteral("remaining_budget_ms")))
                            callerRemaining = std::chrono::milliseconds(request.value(QStringLiteral("remaining_budget_ms")).toInteger());
                        const auto deadline = semantic && diagnosticUnbounded ? Clock::time_point::max()
                            : roclint::runtime::effectiveNativeDeadline(receivedAt, std::chrono::milliseconds(budget), callerRemaining);
                        bool accepted = false;
                        qsizetype queueDepth = 0;
                        {
                            const std::lock_guard lock(mutex);
                            if (pending.size() < 8) {
                                auto cancelled = std::make_shared<std::atomic_bool>(false);
                                pending.emplace(id, cancelled);
                                queue.push_back({request, std::move(cancelled), deadline, receivedAt});
                                queueDepth = queue.size();
                                accepted = true;
                                // Trace start before waking the worker so queue spans
                                // can never appear end-before-start in the file.
                                trace(request, QStringLiteral("queue"), QStringLiteral("start"), 0.0,
                                    {{QStringLiteral("depth"), static_cast<qint64>(queueDepth)},
                                     {QStringLiteral("queue_length"), static_cast<qint64>(queueDepth)},
                                     {QStringLiteral("reason"), queueDepth > 1 ? QStringLiteral("busy") : QStringLiteral("admitted")},
                                     {QStringLiteral("remaining_budget_ms"), callerRemaining ? QJsonValue(static_cast<qint64>(callerRemaining->count())) : QJsonValue(QJsonValue::Null)}});
                                available.notify_one();
                            }
                        }
                        if (!accepted) emitFrame(reply(request, {}, QStringLiteral("Native request queue is full."), QStringLiteral("QUEUE_FULL")));
                    } else throw GraniteFailure(QStringLiteral("PROTOCOL"), "Native operation is unsupported.");
                }
            } catch (...) {
                emitFrame({{QStringLiteral("protocol_version"), 1}, {QStringLiteral("type"), QStringLiteral("error")},
                    {QStringLiteral("request_id"), QStringLiteral("0")}, {QStringLiteral("ok"), false},
                    {QStringLiteral("error"), QStringLiteral("Native helper rejected its input protocol.")}});
                exitCode.store(2);
            }
            stopping.store(true);
            available.notify_one();
        });
        while (!stopping.load()) {
            Work work;
            qsizetype queuedBehind = 0;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&]() { return stopping.load() || !queue.empty(); });
                if (stopping.load()) break;
                work = std::move(queue.front()); queue.pop_front();
                queuedBehind = queue.size();
            }
            const auto op = work.request.value(QStringLiteral("op")).toString();
            const bool semanticWork = op == QLatin1String("granite_analyze") || op == QLatin1String("granite_rewrite") ||
                op == QLatin1String("granite_word_refinement");
            const bool readiness = op == QLatin1String("load") || op == QLatin1String("warm") || op == QLatin1String("set_residency");
            const auto started = Clock::now();
            const auto queueWaitMs = std::chrono::duration<double, std::milli>(started - work.receivedAt).count();
            trace(work.request, QStringLiteral("queue"), QStringLiteral("end"), queueWaitMs,
                {{QStringLiteral("depth"), static_cast<qint64>(queuedBehind)},
                 {QStringLiteral("queue_length"), static_cast<qint64>(queuedBehind)},
                 {QStringLiteral("reason"), queueWaitMs >= 1.0 ? QStringLiteral("busy") : QStringLiteral("admitted")}});
            // Queued work can be obsolete before it starts. The caller's remaining
            // budget was captured at submission; never start work that outlived it.
            if (started >= work.deadline || work.cancelled->load()) {
                const auto cancelled = work.cancelled->load();
                const auto remainingMs = std::chrono::duration<double, std::milli>(work.deadline - started).count();
                trace(work.request, QStringLiteral("queue"), QStringLiteral("mark"), queueWaitMs,
                    {{QStringLiteral("event"), cancelled ? QStringLiteral("cancelled_before_start") : QStringLiteral("expired_before_start")},
                     {QStringLiteral("remaining_ms"), remainingMs}});
                const std::lock_guard lock(mutex);
                if (!stopping.load()) emitFrame(cancelled
                    ? reply(work.request, {}, QStringLiteral("Native operation was cancelled."), QStringLiteral("CANCELLED"))
                    : reply(work.request, {}, QStringLiteral("Native request expired while queued behind other local work."),
                            QStringLiteral("TIMEOUT"), queueWaitMs));
                pending.erase(requestId(work.request));
                continue;
            }
            const auto readinessReason = op == QLatin1String("warm") ? QStringLiteral("warm")
                : op == QLatin1String("load") ? QStringLiteral("cold") : QStringLiteral("policy");
            if (semanticWork) trace(work.request, QStringLiteral("generate"), QStringLiteral("start"), 0.0,
                {{QStringLiteral("queue_wait_ms"), queueWaitMs}});
            else if (readiness) trace(work.request, QStringLiteral("model_ready"), QStringLiteral("start"), 0.0,
                {{QStringLiteral("runtime"), QStringLiteral("granite")},
                 {QStringLiteral("reason"), readinessReason},
                 {QStringLiteral("quantization"), engine.identity().value(QStringLiteral("quantization"))},
                 {QStringLiteral("backend"), engine.status().value(QStringLiteral("backend"))},
                 {QStringLiteral("residency_before"), engine.status().value(QStringLiteral("residency"))}});
            roclint::runtime::GraniteWorkControl control{*work.cancelled, stopping, work.deadline};
            QJsonObject response;
            QJsonObject outcomeDetail{{QStringLiteral("queue_wait_ms"), queueWaitMs}};
            QString outcome = QStringLiteral("error");
            QString errorCode;
            try {
                control.check();
                // The decoder preserves Unicode split across token/batch boundaries.
                QStringDecoder streamDecoder(QStringDecoder::Utf8);
                const auto streamToken = [&](const GraniteStreamProgress& progress) {
                    if (work.cancelled->load() || stopping.load()) return;
                    auto partial = work.request;
                    partial.remove(QStringLiteral("payload"));
                    partial.insert(QStringLiteral("type"), QStringLiteral("partial_result"));
                    partial.insert(QStringLiteral("ok"), true);
                    partial.insert(QStringLiteral("chunk"), QString(streamDecoder(QByteArrayView(progress.text.data(), static_cast<qsizetype>(progress.text.size())))));
                    partial.insert(QStringLiteral("output_tokens"), progress.outputTokens);
                    partial.insert(QStringLiteral("reasoning_tokens"), progress.reasoningTokens);
                    partial.insert(QStringLiteral("answer_tokens"), progress.answerTokens);
                    partial.insert(QStringLiteral("phase"), progress.thinking ? QStringLiteral("thinking") : QStringLiteral("answer"));
                    partial.insert(QStringLiteral("decode_ms"), progress.decodeMs);
                    partial.insert(QStringLiteral("generation_ms"), progress.generationMs);
                    emitFrame(partial);
                };
                const auto result = engine.execute(op, work.request.value(QStringLiteral("payload")).toObject(), control, streamToken);
                outcome = QStringLiteral("ok");
                outcomeDetail = measuredDetail(result);
                outcomeDetail.insert(QStringLiteral("queue_wait_ms"), queueWaitMs);
                response = reply(work.request, result, {}, {}, std::chrono::duration<double, std::milli>(Clock::now() - started).count());
            } catch (const GraniteFailure& error) {
                errorCode = error.code;
                outcome = error.code == QLatin1String("CANCELLED") ? QStringLiteral("cancelled")
                    : error.code == QLatin1String("TIMEOUT") ? QStringLiteral("timeout") : QStringLiteral("error");
                outcomeDetail = measuredDetail(error.metrics);
                outcomeDetail.insert(QStringLiteral("queue_wait_ms"), queueWaitMs);
                response = reply(work.request, {}, QString::fromLatin1(error.what()), error.code,
                    std::chrono::duration<double, std::milli>(Clock::now() - started).count(), error.metrics);
            } catch (...) {
                outcome = QStringLiteral("error");
                response = reply(work.request, {}, QStringLiteral("Native Granite could not complete this local operation."), QStringLiteral("RUNTIME"));
            }
            const auto serviceMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            outcomeDetail.insert(QStringLiteral("outcome"), outcome);
            if (!errorCode.isEmpty()) outcomeDetail.insert(QStringLiteral("error_code"), errorCode);
            if (stopping.load()) outcomeDetail.insert(QStringLiteral("stopping"), true);
            if (semanticWork) trace(work.request, QStringLiteral("generate"), QStringLiteral("end"), serviceMs, outcomeDetail);
            else if (readiness) {
                outcomeDetail.insert(QStringLiteral("runtime"), QStringLiteral("granite"));
                outcomeDetail.insert(QStringLiteral("reason"), readinessReason);
                outcomeDetail.insert(QStringLiteral("load_ms"),
                    outcomeDetail.value(QStringLiteral("prepare_ms")).isDouble()
                        ? outcomeDetail.value(QStringLiteral("prepare_ms")).toDouble() : serviceMs);
                outcomeDetail.insert(QStringLiteral("residency_after"), engine.status().value(QStringLiteral("residency")));
                trace(work.request, QStringLiteral("model_ready"), QStringLiteral("end"), serviceMs, outcomeDetail);
            }
            const std::lock_guard lock(mutex);
            if (!stopping.load()) emitFrame(work.cancelled->load()
                ? reply(work.request, {}, QStringLiteral("Native operation was cancelled."), QStringLiteral("CANCELLED")) : response);
            pending.erase(requestId(work.request));
        }
        reader.join();
        return exitCode.load();
    } catch (...) {
        static_cast<void>(std::fputs("Native Granite configuration could not be initialized.\n", stderr));
        return 2;
    }
}
