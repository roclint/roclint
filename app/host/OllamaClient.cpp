#include "OllamaClient.hpp"

#include "roclint/core/Json.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>

namespace roclint::host {

namespace json = core::json;
using core::ErrorCode;
using core::makeError;

namespace {

core::json::ParseLimits envelopeLimits() {
    core::json::ParseLimits limits;
    limits.maxBytes = static_cast<std::size_t>(OllamaClient::kMaxEnvelopeBytes);
    limits.maxDepth = 32;
    return limits;
}

QString fromStd(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string toStd(const QString& s) {
    const QByteArray utf8 = s.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

core::Result<json::Value> parseEnvelope(const QByteArray& body) {
    auto parsed = json::parse(std::string_view(body.constData(), static_cast<std::size_t>(body.size())),
                              envelopeLimits());
    if (!parsed.ok()) {
        return makeError(ErrorCode::ProviderFailed, "Model runtime returned malformed JSON: " + parsed.error().message);
    }
    if (!parsed.value().isObject()) {
        return makeError(ErrorCode::ProviderFailed, "Model runtime returned a non-object JSON document");
    }
    if (const auto* err = parsed.value().find("error"); err != nullptr) {
        return makeError(ErrorCode::ProviderFailed,
                         "Model runtime reported an error: " + (err->isString() ? err->asString() : std::string("(unspecified)")));
    }
    return std::move(parsed).value();
}

} // namespace

QUrl OllamaClient::defaultBaseUrl() {
    return QUrl(QStringLiteral("http://127.0.0.1:11434"));
}

bool OllamaClient::isLoopback(const QUrl& url) {
    if (!url.isValid() || url.scheme() != QLatin1String("http")) {
        return false;
    }
    const QString host = url.host();
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

core::Result<OllamaClient> OllamaClient::create(const QUrl& baseUrl) {
    if (!isLoopback(baseUrl)) {
        return makeError(ErrorCode::InvalidInput,
                         "Model runtime URL must be plain http on a loopback address: " + toStd(baseUrl.toString()));
    }
    return OllamaClient(baseUrl);
}

core::Result<QByteArray> OllamaClient::request(const QString& path, const QByteArray* jsonBody, int timeoutMs) const {
    if (auto* app = QCoreApplication::instance(); app != nullptr && QThread::currentThread() == app->thread()) {
        return makeError(ErrorCode::Internal, "Model runtime I/O must not run on the GUI thread");
    }

    QUrl url = baseUrl_;
    url.setPath(path);

    QNetworkAccessManager nam;
    nam.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest req(url);
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    QNetworkReply* reply = nullptr;
    if (jsonBody != nullptr) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = nam.post(req, *jsonBody);
    } else {
        reply = nam.get(req);
    }

    bool tooLarge = false;
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [&tooLarge, reply](qint64 received, qint64) {
        if (received > kMaxEnvelopeBytes) {
            tooLarge = true;
            reply->abort();
        }
    });
    if (!reply->isFinished()) {
        loop.exec();
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString errorText = reply->errorString();
    const QByteArray body = tooLarge ? QByteArray() : reply->readAll();
    reply->deleteLater();

    if (tooLarge || body.size() > kMaxEnvelopeBytes) {
        return makeError(ErrorCode::ProviderFailed, "Model runtime response exceeded the size limit");
    }
    const std::string where = toStd(baseUrl_.host()) + ":" + std::to_string(baseUrl_.port(11434));
    if (netError == QNetworkReply::OperationCanceledError) {
        return makeError(ErrorCode::ProviderUnavailable,
                         "Ollama at " + where + " didn't respond within " + std::to_string(timeoutMs / 1000) +
                             " seconds.");
    }
    if (netError != QNetworkReply::NoError && httpStatus == 0) {
        return makeError(ErrorCode::ProviderUnavailable,
                         "Can't reach Ollama at " + where + " (" + toStd(errorText) + "). Make sure Ollama is running.");
    }
    if (httpStatus != 200) {
        std::string detail;
        if (auto env = parseEnvelope(body); !env.ok()) {
            detail = env.error().message;
        }
        return makeError(ErrorCode::ProviderFailed,
                         "Model runtime returned HTTP " + std::to_string(httpStatus) + (detail.empty() ? "" : ": " + detail));
    }
    return body;
}

core::Result<std::vector<OllamaModelEntry>> OllamaClient::listModels(int timeoutMs) const {
    auto body = request(QStringLiteral("/api/tags"), nullptr, timeoutMs);
    if (!body.ok()) return body.error();
    auto env = parseEnvelope(body.value());
    if (!env.ok()) return env.error();

    auto models = json::requireArray(env.value(), "models");
    if (!models.ok()) {
        return makeError(ErrorCode::ProviderFailed, "Model list is missing 'models': " + models.error().message);
    }

    std::vector<OllamaModelEntry> out;
    for (const auto& m : models.value()->asArray()) {
        auto name = json::requireString(m, "name");
        if (!name.ok()) {
            return makeError(ErrorCode::ProviderFailed, "Model entry is missing 'name'");
        }
        OllamaModelEntry entry;
        entry.name = fromStd(name.value());
        if (auto digest = json::requireString(m, "digest"); digest.ok()) {
            auto normalized = core::crypto::normalizeSha256Digest(digest.value());
            if (!normalized.ok()) {
                return makeError(ErrorCode::ProviderFailed,
                                 "Model '" + name.value() + "' reported a malformed digest");
            }
            entry.digest = fromStd(normalized.value());
        }
        if (const auto* size = m.find("size"); size != nullptr && size->isNumber() && size->asNumber() >= 0) {
            entry.sizeBytes = static_cast<std::int64_t>(size->asNumber());
        }
        if (const auto* details = m.find("details"); details != nullptr && details->isObject()) {
            if (auto q = json::requireString(*details, "quantization_level"); q.ok()) {
                entry.quantization = fromStd(q.value());
            }
            if (auto p = json::requireString(*details, "parameter_size"); p.ok()) {
                entry.parameterSize = fromStd(p.value());
            }
        }
        out.push_back(std::move(entry));
    }
    return out;
}

core::Result<bridge::GraniteModelInfo> OllamaClient::probeModel(const QString& model, int timeoutMs) const {
    if (model.trimmed().isEmpty()) {
        return makeError(ErrorCode::InvalidInput, "No model selected");
    }
    auto models = listModels(timeoutMs);
    if (!models.ok()) return models.error();

    for (const auto& entry : models.value()) {
        if (entry.name != model) {
            continue;
        }
        if (entry.digest.isEmpty()) {
            return makeError(ErrorCode::ProviderFailed, "Model runtime did not report a digest for " + toStd(model));
        }
        bridge::GraniteModelInfo info;
        info.modelId = toStd(entry.name);
        info.digest = "sha256:" + toStd(entry.digest);
        info.quantization = toStd(entry.quantization);
        info.transport = bridge::kLoopbackOllamaTransport;
        return info;
    }
    return makeError(ErrorCode::NotFound, "The model " + toStd(model) +
                                              " isn't installed in Ollama. Download it in Settings or choose another model.");
}

core::Status OllamaClient::warmUp(const QString& model, int timeoutMs) const {
    json::Value payload = json::Value::object();
    payload.set("model", json::Value::string(toStd(model)));
    payload.set("keep_alive", json::Value::string(kKeepAlive));
    const std::string serialized = json::serialize(payload);
    const QByteArray body(serialized.data(), static_cast<qsizetype>(serialized.size()));

    auto raw = request(QStringLiteral("/api/generate"), &body, timeoutMs);
    if (!raw.ok()) return raw.error();
    auto env = parseEnvelope(raw.value());
    if (!env.ok()) return env.error();
    auto done = json::requireBool(env.value(), "done");
    if (!done.ok() || !done.value()) {
        return makeError(ErrorCode::ProviderFailed, "Ollama didn't finish loading " + toStd(model));
    }
    return core::Status::success();
}

core::Result<std::string> OllamaClient::generate(const QString& model,
                                                 std::string_view prompt,
                                                 int timeoutMs,
                                                 std::string_view formatSchema) const {
    json::Value options = json::Value::object();
    options.set("temperature", json::Value::number(0.0));

    json::Value payload = json::Value::object();
    payload.set("model", json::Value::string(toStd(model)));
    payload.set("prompt", json::Value::string(std::string(prompt)));
    payload.set("stream", json::Value::boolean(false));
    payload.set("keep_alive", json::Value::string(kKeepAlive));
    if (formatSchema.empty()) {
        payload.set("format", json::Value::string("json"));
    } else {
        auto schema = json::parse(formatSchema);
        if (!schema.ok()) {
            return makeError(ErrorCode::Internal, "Invalid output schema: " + schema.error().message);
        }
        payload.set("format", std::move(schema).value());
    }
    payload.set("options", std::move(options));

    const std::string serialized = json::serialize(payload);
    const QByteArray body(serialized.data(), static_cast<qsizetype>(serialized.size()));

    auto raw = request(QStringLiteral("/api/generate"), &body, timeoutMs);
    if (!raw.ok()) return raw.error();
    auto env = parseEnvelope(raw.value());
    if (!env.ok()) return env.error();

    auto done = json::requireBool(env.value(), "done");
    if (!done.ok() || !done.value()) {
        return makeError(ErrorCode::ProviderFailed, "Model runtime returned an incomplete generation");
    }
    auto response = json::requireString(env.value(), "response");
    if (!response.ok()) {
        return makeError(ErrorCode::ProviderFailed, "Model runtime reply is missing 'response'");
    }
    return std::move(response).value();
}

bridge::GraniteTransport OllamaClient::transportFor(const QString& model,
                                                    int timeoutMs,
                                                    std::string formatSchema) const {
    OllamaClient client = *this;
    return [client, model, timeoutMs, schema = std::move(formatSchema)](
               std::string_view instruction) -> core::Result<std::string> {
        return client.generate(model, instruction, timeoutMs, schema);
    };
}

} // namespace roclint::host
