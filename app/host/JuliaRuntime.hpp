#pragma once
#include "JuliaRuntimeWorker.hpp"
#include "RuntimeThread.hpp"
#include <functional>
#include <QSet>
#include <QJsonArray>
namespace roclint::host {
// GUI facade. Protocol parsing, pending requests, timers and process ownership live on I/O.
class JuliaRuntime : public QObject {
    Q_OBJECT
public:
    using RequestContext = JuliaRuntimeWorker::RequestContext;
    static constexpr const char* kModelId = "SupersonicLabs/Julia-1";
    static constexpr const char* kRevision = "a85b127321d580d65176c89ced8273f305745d85";
    static constexpr const char* kWeightsSha256 = "df853bf7fe424420011f3d0c47a05d7341aa9eefa7fb9f203ea4aada4ad95b72";
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
    explicit JuliaRuntime(QObject* parent = nullptr, QString helperPath = {});
    ~JuliaRuntime() override;
    static QString defaultModelDirectory() { return JuliaRuntimeWorker::defaultModelDirectory(); }
    static QString defaultPython() { return JuliaRuntimeWorker::defaultPython(); }
    bool isReady() const { return cache_.ready; }
    bool isRunning() const { return cache_.running; }
    bool supportsPromptTriage() const { return cache_.triage; }
    QString state() const { return cache_.state; }
    QString detail() const { return cache_.detail; }
    QString executionConfigurationId() const { return cache_.configuration; }
    QString executionDevice() const { return cache_.device; }
    QString executionPrecision() const { return cache_.precision; }
    int executionCpuThreads() const { return cache_.threads; }
    qsizetype automaticRestartCount() const { return cache_.restarts; }
    QString lastRecommendedOption() const { return cache_.recommendation; }
    double lastChoiceConfidence() const { return cache_.confidence; }
    QString lastChoicePolarity() const { return cache_.polarity; }
    void load(const QString& directory, const QString& python, bool install = false);
    void stop();
    void setJuliaDevice(const QString& device);
    void cancel(quint64 id, const QString& reason = QStringLiteral("Julia scoring was cancelled."));
    void cancelAllEvaluations();
    quint64 evaluate(const core::analysis::NormalizedAnalysis& analysis, const QString& prompt, const RequestContext& context);
    quint64 classifyPrompt(const QString& prompt, const RequestContext& context, const QJsonObject& metadata = {}, const QStringList& choices = {});
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    void setTimeoutForTesting(int milliseconds) { timeoutOverride_ = milliseconds; }
    void stallIoForTesting(int milliseconds);
#endif
signals:
    void stateChanged(); void stopped(); void requestQueueFull();
    void evaluationFinished(quint64 id, const roclint::bridge::SnapshotEvaluationResponse& response);
    void evaluationFailed(quint64 id, const QString& reason);
    void promptClassified(quint64 id, const roclint::core::analysis::PromptTriage& response);
    void promptClassificationFailed(quint64 id, const QString& reason);
private:
    struct Snapshot {
        bool ready{false}, running{false}, triage{false};
        QString state{"NOT_LOADED"}, detail{"Julia is not loaded."}, configuration, device, precision, recommendation, polarity;
        int threads{0}; qsizetype restarts{0}; double confidence{0};
        bool operator==(const Snapshot&) const = default;
    };
    struct FrontRequest { bool triage; quint64 lifetime; std::chrono::steady_clock::time_point deadline{}; };
    struct Shared { quint64 epoch{0}; QHash<quint64, quint64> requests; };
    void apply(Snapshot snapshot, quint64 epoch);
    void post(std::function<void(JuliaRuntimeWorker*)> command);
    quint64 accept(bool triage);
    void watchDeadline(quint64 id, int budgetMs);
    Snapshot cache_;
    quint64 epoch_{0}, lifetime_{0}, nextId_{0};
    int timeoutOverride_{0};
    QHash<quint64, FrontRequest> pending_;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<RuntimeMailbox<JuliaRuntime>> mailbox_;
    std::unique_ptr<RuntimeThread<JuliaRuntimeWorker>> io_;
};
} // namespace roclint::host
