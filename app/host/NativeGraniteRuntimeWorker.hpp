#pragma once

#include "roclint/bridge/GraniteAdapter.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>

#include <memory>
#include <chrono>
#include <optional>
#include <QSet>

namespace roclint::host {
class OwnedProcessJob;

// Qt owns the process and all pipe I/O. Existing blocking bridge transports wait
// only on their QtConcurrent thread; the GUI thread never waits for inference.
class NativeGraniteRuntimeWorker : public QObject {
    Q_OBJECT
public:
    struct RequestContext {
        QString groupId;
        QString attemptId;
        QString sessionId;
        QString revisionId;
        quint64 stateVersion{0};
        QString prompt;
        QString intentId;
        QString profileId;
    };
    explicit NativeGraniteRuntimeWorker(QObject* parent = nullptr, QString worker = {});
    ~NativeGraniteRuntimeWorker() override;
    [[nodiscard]] static QJsonObject manifest();
    [[nodiscard]] static QString defaultDirectory();
    [[nodiscard]] static bool isNativeModel(const QString& model);
    [[nodiscard]] static QString modelTag(const QString& quantization);
    [[nodiscard]] static QString quantizationForTag(const QString& tag);
    [[nodiscard]] bool isReady() const noexcept { return ready_; }
    [[nodiscard]] bool isRunning() const noexcept { return process_ != nullptr; }
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] QString detail() const { return detail_; }
    [[nodiscard]] QString residency() const { return residency_; }
    [[nodiscard]] QString activeQuantization() const { return identity_.value(QStringLiteral("quantization")).toString(); }
    [[nodiscard]] bridge::GraniteModelInfo modelInfo() const;
    [[nodiscard]] QString configurationDetail() const;
    [[nodiscard]] double lastTokensPerSecond() const noexcept { return lastTokensPerSecond_; }
    [[nodiscard]] double lastDecodeTokensPerSecond() const noexcept { return lastDecodeTokensPerSecond_; }
    [[nodiscard]] double lastGenerationTokensPerSecond() const noexcept { return lastGenerationTokensPerSecond_; }
    [[nodiscard]] qint64 lastOutputTokens() const noexcept { return lastOutputTokens_; }
    [[nodiscard]] double lastDecodeMs() const noexcept { return lastDecodeMs_; }
    [[nodiscard]] double lastGenerationMs() const noexcept { return lastGenerationMs_; }
    void load(const QString& directory, const QString& quantization);
    void setResidency(const QString& tier);
    void stop();
    void cancelAll();
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    bool setWorkerForTesting(QString worker) { if (process_) return false; worker_ = std::move(worker); return true; }
    quint64 generationForTesting() const { return generation_; }
#endif
    [[nodiscard]] bridge::GraniteTransport transportFor(const RequestContext& context, const QString& operation, const std::string& schema, quint64 generation, quint64 epoch, int budgetOverride = 0) const;
    quint64 requestEpochForFacade() const { return requestEpoch_; }
    quint64 generationForFacade() const { return generation_; }

signals:
    void stateChanged();
    void stopped();
    void speedMetricsChanged();
    // Emitted for each streaming chunk during an active generation.
    void partialResultReceived(quint64 requestId, const QString& chunk);

private:
    struct Call;
    struct Mailbox;
    struct Pending {
        QJsonObject envelope;
        std::shared_ptr<Call> call;
        std::chrono::steady_clock::time_point deadline;
    };
    struct Launch { QString directory; QString quantization; };
    void startWorker();
    void beginStop(bool failure, const QString& reason);
    void finished(QProcess* process);
    void fail(const QString& reason);
    void killWorker();
    void state(const QString& value, const QString& detail);
    [[nodiscard]] quint64 send(const QString& operation, const QJsonObject& payload, int timeoutMs,
                               const RequestContext* context = nullptr, std::shared_ptr<Call> call = {});
    void readOutput();
    void receive(const QJsonObject& response);
    void submit(const RequestContext& context, const QString& operation, const QString& instructions,
                const QString& schema, const std::shared_ptr<Call>& call);
    void cancelCall(const std::shared_ptr<Call>& call);
    void resetSpeedMetrics(quint64 requestId = 0);

    QProcess* process_{nullptr};
    std::unique_ptr<OwnedProcessJob> job_;
    std::shared_ptr<Mailbox> mailbox_;
    QString worker_;
    std::optional<Launch> launch_;
    std::optional<Launch> next_;
    QJsonObject identity_;
    QJsonObject modelMetadata_;
    QHash<quint64, Pending> pending_;
    QHash<quint64, QJsonObject> cancelled_;
    QSet<quint64> quiescing_;
    QByteArray incoming_;
    QByteArray outgoing_;
    quint64 nextId_{0};
    quint64 generation_{0};
    bool ready_{false};
    bool hello_{false};
    bool stopping_{false};
    bool failure_{false};
    bool warmAfterResidency_{false};
    QString state_{QStringLiteral("UNLOADED")};
    QString detail_{QStringLiteral("Native Granite is not loaded.")};
    QString residency_{QStringLiteral("UNLOADED")};
    QElapsedTimer clock_;
    QList<qint64> restarts_;
    quint64 restartEpoch_{0};
    quint64 requestEpoch_{0};
    double lastTokensPerSecond_{0.0};
    double lastDecodeTokensPerSecond_{0.0};
    double lastGenerationTokensPerSecond_{0.0};
    quint64 metricsRequestId_{0};
    qint64 lastOutputTokens_{0};
    double lastDecodeMs_{0.0};
    double lastGenerationMs_{0.0};
};
} // namespace roclint::host
