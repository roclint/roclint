#pragma once
#include "NativeGraniteRuntimeWorker.hpp"
#include "RuntimeThread.hpp"
#include <functional>
namespace roclint::host {
class NativeGraniteRuntime : public QObject {
    Q_OBJECT
public:
    using RequestContext = NativeGraniteRuntimeWorker::RequestContext;
    explicit NativeGraniteRuntime(QObject* parent = nullptr, QString worker = {});
    ~NativeGraniteRuntime() override;
    static QJsonObject manifest() { return NativeGraniteRuntimeWorker::manifest(); }
    static QString defaultDirectory() { return NativeGraniteRuntimeWorker::defaultDirectory(); }
    static bool isNativeModel(const QString& value) { return NativeGraniteRuntimeWorker::isNativeModel(value); }
    static QString modelTag(const QString& value) { return NativeGraniteRuntimeWorker::modelTag(value); }
    static QString quantizationForTag(const QString& value) { return NativeGraniteRuntimeWorker::quantizationForTag(value); }
    bool isReady() const { return cache_.ready; }
    bool isRunning() const { return cache_.running; }
    QString state() const { return cache_.state; }
    QString detail() const { return cache_.detail; }
    QString residency() const { return cache_.residency; }
    QString activeQuantization() const { return cache_.quantization; }
    bridge::GraniteModelInfo modelInfo() const { return cache_.model; }
    QString configurationDetail() const { return cache_.configuration; }
    double lastTokensPerSecond() const { return cache_.speed; }
    qint64 lastOutputTokens() const { return cache_.tokens; }
    double lastDecodeMs() const { return cache_.decode; }
    double lastGenerationMs() const { return cache_.generationMs; }
    void load(const QString& directory, const QString& quantization);
    void setResidency(const QString& tier);
    void stop(); void cancelAll();
    bridge::GraniteTransport transportFor(const RequestContext& context, const QString& operation, const std::string& schema) const;
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    bool setWorkerForTesting(QString worker);
    quint64 generationForTesting() const { return cache_.generation; }
    void setTimeoutForTesting(int timeoutMs) { timeoutOverride_ = timeoutMs; }
    void stallIoForTesting(int durationMs);
#endif
signals:
    void stateChanged(); void stopped(); void speedMetricsChanged();
    void partialResultReceived(quint64 requestId, const QString& chunk);
private:
    struct Snapshot {
        bool ready{false}, running{false}; QString state{"UNLOADED"}, detail{"Native Granite is not loaded."}, residency{"UNLOADED"}, quantization, configuration;
        bridge::GraniteModelInfo model; quint64 generation{0}, requestEpoch{0};
        double speed{0}, decode{0}, generationMs{0}; qint64 tokens{0};
    };
    void post(std::function<void(NativeGraniteRuntimeWorker*)> command);
    void apply(Snapshot snapshot, quint64 epoch);
    Snapshot cache_; quint64 epoch_{0};
    QString directory_, quantization_; bool loading_{false}; int timeoutOverride_{0};
    std::shared_ptr<quint64> ioEpoch_;
    std::shared_ptr<RuntimeMailbox<NativeGraniteRuntime>> mailbox_;
    std::unique_ptr<RuntimeThread<NativeGraniteRuntimeWorker>> io_;
};
} // namespace roclint::host
