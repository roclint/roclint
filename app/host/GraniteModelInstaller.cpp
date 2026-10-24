#include "GraniteModelInstaller.hpp"

#include <QDir>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStorageInfo>

#include <algorithm>
#include <utility>

namespace roclint::host {
GraniteModelInstaller::GraniteModelInstaller(QObject* parent) : QObject(parent) {}
GraniteModelInstaller::~GraniteModelInstaller() { cancel(); }

void GraniteModelInstaller::start(const QJsonObject& manifest, const QJsonObject& candidate, const QString& directory) {
    if (isRunning()) return;
    const auto name = candidate.value(QStringLiteral("filename")).toString();
    const auto family = manifest.value(QStringLiteral("model_family")).toString();
    const auto revision = manifest.value(QStringLiteral("model_revision")).toString();
    const auto sha = candidate.value(QStringLiteral("sha256")).toString();
    expectedSize_ = candidate.value(QStringLiteral("size_bytes")).toInteger(-1);
    static const QRegularExpression filename(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\\.gguf$"));
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression repository(QStringLiteral("^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$"));
    static const QRegularExpression commit(QStringLiteral("^[0-9a-f]{40}$"));
    if (!filename.match(name).hasMatch() || !digest.match(sha).hasMatch() || !repository.match(family).hasMatch() ||
        !commit.match(revision).hasMatch() || expectedSize_ <= 0 || expectedSize_ > 8LL * 1024 * 1024 * 1024) {
        emit finished(false, QStringLiteral("Pinned model download metadata is invalid.")); return;
    }
    if (!QDir().mkpath(directory)) { emit finished(false, QStringLiteral("The managed model directory could not be created.")); return; }
    const QStorageInfo storage(directory);
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < expectedSize_ + 256LL * 1024 * 1024) {
        emit finished(false, QStringLiteral("There is not enough free space to download and atomically verify this model.")); return;
    }
    file_ = std::make_unique<QSaveFile>(QDir(directory).filePath(name));
    file_->setDirectWriteFallback(false);
    if (!file_->open(QIODevice::WriteOnly)) { file_.reset(); emit finished(false, QStringLiteral("The managed model file could not be opened for an atomic download.")); return; }
    expectedHash_ = sha.toLatin1(); written_ = 0;
    quantization_ = candidate.value(QStringLiteral("quantization")).toString();
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    auto url = origin_;
    url.setPath(QLatin1Char('/') + family + QStringLiteral("/resolve/") + revision + QLatin1Char('/') + name);
    QNetworkRequest request(url);
    request.setTransferTimeout(60000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    reply_ = manager_.get(request);
    reply_->setReadBufferSize(256 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, &GraniteModelInstaller::read);
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        if (!reply_) return;
        read();
        if (!reply_) return;
        if (reply_->error() != QNetworkReply::NoError || written_ != expectedSize_) {
            complete(false, QStringLiteral("The pinned model download failed or ended before its exact size was received.")); return;
        }
        if (hash_->result().toHex() != expectedHash_) {
            complete(false, QStringLiteral("The downloaded model failed its pinned SHA-256 check. The previous model was preserved.")); return;
        }
        if (!file_->commit()) { complete(false, QStringLiteral("The verified model could not be committed to its managed location.")); return; }
        complete(true, QStringLiteral("Downloaded and verified native Granite %1. Select it and load it to begin inference.").arg(quantization_));
    });
    emit progress(0, QStringLiteral("Downloading native Granite %1 (%2 GiB).")
        .arg(quantization_, QString::number(static_cast<double>(expectedSize_) / (1024.0 * 1024.0 * 1024.0), 'f', 2)));
}

void GraniteModelInstaller::read() {
    while (reply_ && reply_->bytesAvailable() > 0) {
        const auto bytes = reply_->read(64 * 1024);
        if (bytes.isEmpty()) break;
        if (written_ + bytes.size() > expectedSize_) {
            complete(false, QStringLiteral("The model response exceeded its pinned size. No file was installed.")); return;
        }
        if (file_->write(bytes) != bytes.size()) { complete(false, QStringLiteral("Writing the model download failed. The previous model was preserved.")); return; }
        written_ += bytes.size(); hash_->addData(bytes);
        const auto percent = static_cast<int>(std::min<qint64>(99, written_ * 100 / expectedSize_));
        emit progress(percent, QStringLiteral("Downloading native Granite %1: %2% (verification required before installation).")
            .arg(quantization_).arg(percent));
    }
}

void GraniteModelInstaller::complete(bool success, const QString& message) {
    auto* reply = std::exchange(reply_, nullptr);
    if (reply) { reply->disconnect(this); if (reply->isRunning()) reply->abort(); reply->deleteLater(); }
    if (file_ && !success) file_->cancelWriting();
    file_.reset(); hash_.reset();
    if (success) emit progress(100, message);
    emit finished(success, message);
}

void GraniteModelInstaller::cancel() { if (reply_) complete(false, QStringLiteral("Native model download cancelled. The previous model was preserved.")); }

#ifdef ROCLINT_ENABLE_TEST_MOCKS
bool GraniteModelInstaller::setOriginForTesting(const QUrl& origin) {
    if (isRunning() || origin.scheme() != QLatin1String("http") || origin.host() != QLatin1String("127.0.0.1")) return false;
    origin_ = origin; return true;
}
#endif
} // namespace roclint::host
