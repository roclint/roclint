#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"

#include <QHash>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>

#include <optional>
#include <memory>

namespace roclint::host {
class OwnedProcessJob;

// Owns one resident local worker. All process I/O is asynchronous on the Qt event loop.
class JuliaRuntime : public QObject {
    Q_OBJECT
public:
    explicit JuliaRuntime(QObject* parent = nullptr, QString helperPath = {});
    ~JuliaRuntime() override;

    static constexpr const char* kModelId = "SupersonicLabs/Julia-1";
    static constexpr const char* kRevision = "a85b127321d580d65176c89ced8273f305745d85";
    static constexpr const char* kWeightsSha256 = "df853bf7fe424420011f3d0c47a05d7341aa9eefa7fb9f203ea4aada4ad95b72";
    [[nodiscard]] static QString defaultModelDirectory();
    [[nodiscard]] static QString defaultPython();
    [[nodiscard]] bool isReady() const noexcept { return ready_; }
    [[nodiscard]] bool supportsPromptTriage() const noexcept { return promptTriageSupported_; }
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] QString detail() const { return detail_; }
    [[nodiscard]] QString executionConfigurationId() const { return executionConfigurationId_; }
    [[nodiscard]] QString executionDevice() const { return executionDevice_; }
    [[nodiscard]] QString executionPrecision() const { return executionPrecision_; }
    [[nodiscard]] int executionCpuThreads() const { return executionCpuThreads_; }

    struct RequestContext {
        QString requestGroupId;
        QString attemptId;
        QString sessionId;
        QString revisionId;
        quint64 stateVersion{0};
    };
    static constexpr int kProtocolVersion = 1;
    static constexpr int kHandshakeTimeoutMs = 3000;
    static constexpr int kShutdownTimeoutMs = 2000;
    static constexpr int kLoadTimeoutMs = 180000;
    static constexpr int kInstallTimeoutMs = 600000;
    static constexpr int kEvaluationTimeoutMs = 60000;
    static constexpr int kTriageTimeoutMs = 8000;
    static constexpr int kMaxAutomaticRestarts = 2;
    static constexpr int kRestartWindowMs = 60000;
    static constexpr qsizetype kMaxFrame = 256 * 1024;
    static constexpr const char* kConfigurationId = "julia.auto.bf16-fp32.v2";
    [[nodiscard]] bool isRunning() const noexcept { return process_ != nullptr; }
    [[nodiscard]] qsizetype automaticRestartCount() const noexcept { return restartTimes_.size(); }
    // Allowed values: empty string (unset / auto), "auto", "cpu", "cuda", "mps", "directml".
    [[nodiscard]] QString juliaDevice() const { return juliaDevice_; }
    void setJuliaDevice(const QString& device);
    void cancel(quint64 requestId, const QString& reason = QStringLiteral("Julia scoring was cancelled."));
    void cancelAllEvaluations();

    void load(const QString& directory, const QString& python, bool install = false);
    void stop();
    [[nodiscard]] quint64 evaluate(const core::analysis::NormalizedAnalysis& analysis, const QString& prompt, const RequestContext& context);
    [[nodiscard]] quint64 classifyPrompt(const QString& prompt, const RequestContext& context);

signals:
    void stateChanged();
    void stopped();
    void evaluationFinished(quint64 requestId, const roclint::bridge::SnapshotEvaluationResponse& response);
    void evaluationFailed(quint64 requestId, const QString& reason);
    void promptClassified(quint64 requestId, const roclint::core::analysis::PromptTriage& triage);
    void promptClassificationFailed(quint64 requestId, const QString& reason);
    // Emitted when a new request is dropped because the pending queue is full.
    void requestQueueFull();

private:
    struct Pending {
        QString operation;
        QString snapshot;
        QJsonObject envelope;
        QString prompt;
    };
    struct Launch { QString directory; QString python; bool install; };
    quint64 send(const QString& operation, QJsonObject payload, QString snapshot, int timeoutMs,
                 const RequestContext* context = nullptr);
    void startWorker();
    void beginStop(bool failed, const QString& reason);
    void workerFinished(QProcess* process);
    void startModelLoad();
    void killWorker();
    void readOutput();
    void receive(const QJsonObject& object);
    void fail(const QString& reason);
    void setState(const QString& state, const QString& detail);

    QProcess* process_{nullptr};
    std::unique_ptr<OwnedProcessJob> job_;
    QString helperPath_;
    QHash<quint64, Pending> pending_;
    QHash<quint64, QJsonObject> cancelled_;
    std::optional<Launch> nextLoad_;
    std::optional<Launch> launch_;
    QByteArray incoming_;
    QByteArray outgoing_;
    quint64 nextId_{0};
    bool ready_{false};
    bool stopping_{false};
    bool failed_{false};
    bool handshaken_{false};
    bool promptTriageSupported_{false};
    QString juliaDevice_;           // JULIA_DEVICE override; empty = unset (auto)
    int activePlanVersion_{0};      // Negotiated triage plan version; 0 = not yet negotiated
    QString executionConfigurationId_;
    QString executionDevice_;
    QString executionPrecision_;
    int executionCpuThreads_{0};
    QElapsedTimer clock_;
    QList<qint64> restartTimes_;
    quint64 restartEpoch_{0};
    quint64 processGeneration_{0};
    QString state_{QStringLiteral("NOT_LOADED")};
    QString detail_{QStringLiteral("Julia is not loaded. Download it or choose its local model folder.")};
};

} // namespace roclint::host

Q_DECLARE_METATYPE(roclint::bridge::SnapshotEvaluationResponse)
Q_DECLARE_METATYPE(roclint::core::analysis::PromptTriage)
