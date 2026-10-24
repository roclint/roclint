#pragma once

#include <QCryptographicHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <memory>

namespace roclint::host {
// Explicit managed download only. It never receives prompt text or runs a model.
class GraniteModelInstaller : public QObject {
    Q_OBJECT
public:
    explicit GraniteModelInstaller(QObject* parent = nullptr);
    ~GraniteModelInstaller() override;
    void start(const QJsonObject& manifest, const QJsonObject& candidate, const QString& directory);
    void cancel();
    [[nodiscard]] bool isRunning() const noexcept { return reply_ != nullptr; }
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    [[nodiscard]] bool setOriginForTesting(const QUrl& origin);
#endif
signals:
    void progress(int percent, const QString& message);
    void finished(bool success, const QString& message);
private:
    void read();
    void complete(bool success, const QString& message);
    QNetworkAccessManager manager_;
    QNetworkReply* reply_{nullptr};
    std::unique_ptr<QSaveFile> file_;
    std::unique_ptr<QCryptographicHash> hash_;
    qint64 expectedSize_{0};
    qint64 written_{0};
    QByteArray expectedHash_;
    QString quantization_;
    QUrl origin_{QStringLiteral("https://huggingface.co")};
};
} // namespace roclint::host
