#include "GraniteEngine.hpp"
#include "GraniteConfig.hpp"
#include "roclint/core/Json.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"

#include <llama.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
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
    const auto op = request.value(QStringLiteral("op")).toString();
    if (op == QLatin1String("granite_analyze") || op == QLatin1String("granite_rewrite")) {
        const auto payload = request.value(QStringLiteral("payload")).toObject();
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
};
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("model-dir"), QStringLiteral("Directory containing pinned Granite GGUF files."), QStringLiteral("directory")});
    parser.addOption({QStringLiteral("quantization"), QStringLiteral("Pinned benchmark candidate."), QStringLiteral("quantization")});
    parser.addOption({QStringLiteral("diagnostic-no-deadlines"), QStringLiteral("Development investigation only: disable semantic operation deadlines; cancellation and shutdown remain active.")});
    parser.process(app);
    if (!parser.isSet(QStringLiteral("model-dir")) || !parser.isSet(QStringLiteral("quantization"))) return 2;
    llama_log_set([](ggml_log_level level, const char* message, void*) {
        // Do not enable dependency DEBUG/payload tracing in the resident helper.
        if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR) static_cast<void>(std::fputs(message, stderr));
    }, nullptr);
    try {
        const bool diagnosticUnbounded = parser.isSet(QStringLiteral("diagnostic-no-deadlines"));
        GraniteEngine engine(parser.value(QStringLiteral("model-dir")), parser.value(QStringLiteral("quantization")));
        const auto identity = engine.identity();
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
                            {QStringLiteral("analysis_timeout_ms"), diagnosticUnbounded ? 0 : roclint::runtime::kGraniteAnalysisTimeoutMs},
                            {QStringLiteral("rewrite_timeout_ms"), diagnosticUnbounded ? 0 : roclint::runtime::kGraniteRewriteTimeoutMs},
                            {QStringLiteral("operations"), QJsonArray{
                            QStringLiteral("load"), QStringLiteral("warm"), QStringLiteral("granite_analyze"), QStringLiteral("granite_rewrite"),
                            QStringLiteral("runtime_status"), QStringLiteral("set_residency"), QStringLiteral("cancel"), QStringLiteral("shutdown")}}}));
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
                    } else if (op == QLatin1String("load") || op == QLatin1String("warm") || op == QLatin1String("set_residency") ||
                               op == QLatin1String("granite_analyze") || op == QLatin1String("granite_rewrite")) {
                        const bool semantic = op == QLatin1String("granite_analyze") || op == QLatin1String("granite_rewrite");
                        const auto budget = op == QLatin1String("granite_analyze") ? roclint::runtime::kGraniteAnalysisTimeoutMs :
                            op == QLatin1String("granite_rewrite") ? roclint::runtime::kGraniteRewriteTimeoutMs : kLoadBudgetMs;
                        const auto deadline = semantic && diagnosticUnbounded ? Clock::time_point::max() : Clock::now() + std::chrono::milliseconds(budget);
                        const std::lock_guard lock(mutex);
                        if (pending.size() >= 8) emitFrame(reply(request, {}, QStringLiteral("Native request queue is full."), QStringLiteral("QUEUE_FULL")));
                        else {
                            auto cancelled = std::make_shared<std::atomic_bool>(false);
                            pending.emplace(id, cancelled);
                            queue.push_back({request, std::move(cancelled), deadline});
                            available.notify_one();
                        }
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
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&]() { return stopping.load() || !queue.empty(); });
                if (stopping.load()) break;
                work = std::move(queue.front()); queue.pop_front();
            }
            roclint::runtime::GraniteWorkControl control{*work.cancelled, stopping, work.deadline};
            const auto started = Clock::now();
            QJsonObject response;
            try {
                control.check();
                // Stream each decoded text piece to the host as a partial_result frame.
                const auto streamToken = [&](std::string_view piece) {
                    if (work.cancelled->load() || stopping.load()) return;
                    auto partial = work.request;
                    partial.remove(QStringLiteral("payload"));
                    partial.insert(QStringLiteral("type"), QStringLiteral("partial_result"));
                    partial.insert(QStringLiteral("ok"), true);
                    partial.insert(QStringLiteral("chunk"), QString::fromUtf8(piece.data(), static_cast<qsizetype>(piece.size())));
                    emitFrame(partial);
                };
                const auto result = engine.execute(work.request.value(QStringLiteral("op")).toString(),
                                                   work.request.value(QStringLiteral("payload")).toObject(), control, streamToken);
                response = reply(work.request, result, {}, {}, std::chrono::duration<double, std::milli>(Clock::now() - started).count());
            } catch (const GraniteFailure& error) {
                response = reply(work.request, {}, QString::fromLatin1(error.what()), error.code,
                    std::chrono::duration<double, std::milli>(Clock::now() - started).count(), error.metrics);
            } catch (...) {
                response = reply(work.request, {}, QStringLiteral("Native Granite could not complete this local operation."), QStringLiteral("RUNTIME"));
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
