#pragma once

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>

namespace roclint::runtime {

// Opt-in, best-effort JSONL timing trace shared by the native helper and the host
// runtime worker. It is enabled only when ROCLINT_TRACE_TIMING names a file; the
// variable is re-read for each event so tests and long-lived processes can turn it
// on or off without a restart.
//
// Contract (D00): one JSON object per line, UTF-8, with
// ts_ms, trace_id, request_id, revision_id, surface, stage, phase, elapsed_ms and
// detail. Prompt text, instruction text, schema text and source excerpts are never
// recorded. A failed open/write disables tracing for that path; it never throws and
// never changes request behaviour.
class GraniteTimingTrace {
public:
    static GraniteTimingTrace& instance() {
        static GraniteTimingTrace trace;
        return trace;
    }

    GraniteTimingTrace(const GraniteTimingTrace&) = delete;
    GraniteTimingTrace& operator=(const GraniteTimingTrace&) = delete;

    // "granite_helper" or "native_runtime"; recorded in detail.source.
    void identify(QString source) {
        const std::lock_guard lock(mutex_);
        source_ = std::move(source);
    }

    [[nodiscard]] bool enabled() const { return !pathFromEnvironment().isEmpty(); }

    void event(const QString& stage, const QString& phase, const QString& traceId,
               const QString& requestId, const QString& revisionId, const QString& surface,
               double elapsedMs, const QJsonObject& detail = {}) {
        QString path = pathFromEnvironment();
        if (path.isEmpty()) return;
        const std::lock_guard lock(mutex_);
        if (path_ != path || !file_.isOpen()) {
            file_.close();
            file_.setFileName(path);
            // Append per event; concurrent helper/host writers keep one line per write.
            if (!file_.open(QIODevice::WriteOnly | QIODevice::Append)) {
                path_.clear();
                return;
            }
            path_ = path;
        }
        if (!std::isfinite(elapsedMs) || elapsedMs < 0.0) elapsedMs = 0.0;
        // Monotonic milliseconds since boot. The host and helper share the same
        // steady-clock epoch on this machine, so events remain orderable across
        // both processes without depending on wall-clock adjustments.
        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        QJsonObject record{{QStringLiteral("ts_ms"), ts},
            {QStringLiteral("trace_id"), traceId},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("revision_id"), revisionId},
            {QStringLiteral("surface"), surface},
            {QStringLiteral("stage"), stage},
            {QStringLiteral("phase"), phase},
            {QStringLiteral("elapsed_ms"), std::round(elapsedMs * 1000.0) / 1000.0},
            {QStringLiteral("detail"), detail}};
        if (!source_.isEmpty()) {
            auto inner = detail;
            inner.insert(QStringLiteral("source"), source_);
            record.insert(QStringLiteral("detail"), inner);
        }
        const auto line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (file_.write(line) != line.size() || !file_.flush()) {
            file_.close();
            path_.clear();
        }
    }

private:
    GraniteTimingTrace() = default;
    ~GraniteTimingTrace() { file_.close(); }

    [[nodiscard]] static QString pathFromEnvironment() {
        return qEnvironmentVariable("ROCLINT_TRACE_TIMING");
    }

    mutable std::mutex mutex_;
    QFile file_;
    QString path_;
    QString source_;
};

} // namespace roclint::runtime
