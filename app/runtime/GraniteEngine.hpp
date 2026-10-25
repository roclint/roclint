#pragma once

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace roclint::runtime {

struct GraniteFailure : std::runtime_error {
    QString code;
    QJsonObject metrics; // Counts/timings only; failed calls never return partial generated text.
    GraniteFailure(QString category, const char* message) : std::runtime_error(message), code(std::move(category)) {}
};

struct GraniteWorkControl {
    std::atomic_bool& cancelled;
    std::atomic_bool& stopping;
    std::chrono::steady_clock::time_point deadline;
    [[nodiscard]] bool aborted() const;
    void check() const;
};

// Model state belongs only to the inference thread. status() reads atomic state;
// cancellation/control I/O never waits on model loading or a decode operation.
class GraniteEngine {
public:
    GraniteEngine(QString directory, const QString& quantization);
    ~GraniteEngine();
    GraniteEngine(const GraniteEngine&) = delete;
    GraniteEngine& operator=(const GraniteEngine&) = delete;
    [[nodiscard]] QJsonObject identity() const;
    [[nodiscard]] QJsonObject status() const;
    // progressCallback, if set, is invoked with each decoded text piece during generation.
    [[nodiscard]] QJsonObject execute(const QString& operation, const QJsonObject& payload,
                                      GraniteWorkControl& control,
                                      std::function<void(std::string_view)> progressCallback = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roclint::runtime
