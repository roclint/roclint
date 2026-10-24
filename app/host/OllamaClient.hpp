#pragma once

#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/Result.hpp"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::host {

struct OllamaModelEntry {
    QString name;
    QString digest;        // Lowercase SHA-256 hex as reported by the runtime
    std::int64_t sizeBytes{0};
    QString quantization;  // Empty when the runtime does not report it
    QString parameterSize;
};

// Blocking client for a loopback Ollama runtime. Every call performs network I/O and refuses to
// run on the GUI thread; callers dispatch it through QtConcurrent.
class OllamaClient {
public:
    static constexpr std::int64_t kMaxEnvelopeBytes = 4 * 1024 * 1024;
    static constexpr int kDefaultHealthTimeoutMs = 10000;
    static constexpr int kDefaultWarmUpTimeoutMs = 180000;
    static constexpr int kDefaultGenerateTimeoutMs = 120000;
    // How long the runtime keeps the model in memory after the last request.
    static constexpr const char* kKeepAlive = "30m";

    // Fails with InvalidInput unless baseUrl is http on 127.0.0.1, ::1 or localhost.
    [[nodiscard]] static core::Result<OllamaClient> create(const QUrl& baseUrl);
    [[nodiscard]] static QUrl defaultBaseUrl();
    [[nodiscard]] static bool isLoopback(const QUrl& url);

    [[nodiscard]] const QUrl& baseUrl() const noexcept { return baseUrl_; }

    [[nodiscard]] core::Result<std::vector<OllamaModelEntry>> listModels(int timeoutMs) const;

    // Resolves the runtime-reported identity (digest, quantization) of an installed model.
    [[nodiscard]] core::Result<bridge::GraniteModelInfo> probeModel(const QString& model, int timeoutMs) const;

    // Loads the model into memory without generating, so the first real request isn't a cold start.
    [[nodiscard]] core::Status warmUp(const QString& model, int timeoutMs) const;

    // /api/generate with stream=false and temperature=0. The output is constrained to formatSchema
    // (a JSON Schema document) when given, otherwise to any JSON. Returns the model's raw text.
    [[nodiscard]] core::Result<std::string> generate(const QString& model,
                                                     std::string_view prompt,
                                                     int timeoutMs,
                                                     std::string_view formatSchema = {}) const;

    [[nodiscard]] bridge::GraniteTransport transportFor(const QString& model,
                                                        int timeoutMs,
                                                        std::string formatSchema = {}) const;

private:
    explicit OllamaClient(QUrl baseUrl) : baseUrl_(std::move(baseUrl)) {}

    [[nodiscard]] core::Result<QByteArray> request(const QString& path,
                                                   const QByteArray* jsonBody,
                                                   int timeoutMs) const;

    QUrl baseUrl_;
};

} // namespace roclint::host
