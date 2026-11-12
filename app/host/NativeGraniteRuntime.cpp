#include "NativeGraniteRuntime.hpp"
#include <QPointer>
#include <functional>
namespace roclint::host {
NativeGraniteRuntime::NativeGraniteRuntime(QObject* parent, QString path) : QObject(parent), ioEpoch_(std::make_shared<quint64>(0)) {
    auto* worker = new NativeGraniteRuntimeWorker(nullptr, std::move(path));
    mailbox_ = std::make_shared<RuntimeMailbox<NativeGraniteRuntime>>(this);
    const auto mailbox = mailbox_; const auto stamp = ioEpoch_;
    const auto publish = [mailbox, worker, stamp](std::function<void(NativeGraniteRuntime*)> after = {}) {
        Snapshot snapshot{worker->isReady(), worker->isRunning(), worker->state(), worker->detail(), worker->residency(), worker->activeQuantization(),
            worker->configurationDetail(), worker->modelInfo(), worker->generationForFacade(), worker->requestEpochForFacade(),
            worker->lastTokensPerSecond(), worker->lastDecodeMs(), worker->lastGenerationMs(), worker->lastDecodeTokensPerSecond(), worker->lastGenerationTokensPerSecond(), worker->lastOutputTokens()};
        mailbox->post([snapshot, epoch = *stamp, after](auto* owner) { owner->apply(snapshot, epoch); if (after) after(owner); });
    };
    connect(worker, &NativeGraniteRuntimeWorker::stateChanged, worker, [publish]() { publish(); });
    connect(worker, &NativeGraniteRuntimeWorker::speedMetricsChanged, worker, [publish]() { publish(); });
    connect(worker, &NativeGraniteRuntimeWorker::stopped, worker, [publish]() { publish([](auto* front) { emit front->stopped(); }); });
    connect(worker, &NativeGraniteRuntimeWorker::partialResultReceived, worker, [mailbox, stamp](quint64 id, const QString& chunk) {
        mailbox->post([id, chunk, epoch = *stamp](auto* owner) { if (owner->epoch_ == epoch) emit owner->partialResultReceived(id, chunk); });
    });
    io_ = std::make_unique<RuntimeThread<NativeGraniteRuntimeWorker>>(worker);
}
NativeGraniteRuntime::~NativeGraniteRuntime() { mailbox_->close(); }
void NativeGraniteRuntime::apply(Snapshot snapshot, quint64 epoch) {
    if (epoch != epoch_) return;
    const bool changed = cache_.ready != snapshot.ready || cache_.running != snapshot.running || cache_.state != snapshot.state || cache_.detail != snapshot.detail ||
        cache_.configuration != snapshot.configuration || cache_.residency != snapshot.residency || cache_.generation != snapshot.generation || cache_.requestEpoch != snapshot.requestEpoch;
    const bool speed = cache_.speed != snapshot.speed || cache_.tokens != snapshot.tokens || cache_.decode != snapshot.decode || cache_.generationMs != snapshot.generationMs || cache_.decodeSpeed != snapshot.decodeSpeed || cache_.generationSpeed != snapshot.generationSpeed;
    cache_ = std::move(snapshot);
    if (cache_.ready || cache_.state == QLatin1String("FAILED") || !cache_.running) loading_ = false;
    if (changed) emit stateChanged(); if (speed) emit speedMetricsChanged();
}
void NativeGraniteRuntime::post(std::function<void(NativeGraniteRuntimeWorker*)> command) {
    auto* worker = io_->worker; const auto stamp = ioEpoch_; const auto epoch = epoch_;
    QMetaObject::invokeMethod(worker, [worker, stamp, epoch, command = std::move(command)]() { *stamp = epoch; command(worker); worker->stateChanged(); }, Qt::QueuedConnection);
}
void NativeGraniteRuntime::load(const QString& directory, const QString& quantization) {
    if (directory_ == directory && quantization_ == quantization && (loading_ || cache_.ready || cache_.state == QLatin1String("CANCELLING"))) return;
    directory_ = directory; quantization_ = quantization; loading_ = true;
    ++epoch_; cache_.ready = false; cache_.running = true;
    post([=](auto* worker) { worker->load(directory, quantization); });
}
void NativeGraniteRuntime::stop() { ++epoch_; cache_.ready = false; loading_ = false; post([](auto* worker) { worker->stop(); }); }
void NativeGraniteRuntime::cancelAll() { if (!cache_.running) return; ++epoch_; cache_.ready = false; post([](auto* worker) { worker->cancelAll(); }); }
void NativeGraniteRuntime::setResidency(const QString& tier) { if (tier != cache_.residency) cache_.ready = false; post([tier](auto* worker) { worker->setResidency(tier); }); }
bridge::GraniteTransport NativeGraniteRuntime::transportFor(const RequestContext& context, const QString& operation, const std::string& schema) const {
    // Only immutable mailbox ownership is read from the engine here. Tokens come from the GUI snapshot.
    return io_->worker->transportFor(context, operation, schema, cache_.generation, cache_.requestEpoch, timeoutOverride_);
}
#ifdef ROCLINT_ENABLE_TEST_MOCKS
bool NativeGraniteRuntime::setWorkerForTesting(QString path) {
    if (cache_.running) return false;
    post([path](auto* worker) { static_cast<void>(worker->setWorkerForTesting(path)); }); return true;
}
void NativeGraniteRuntime::stallIoForTesting(int ms) { post([ms](auto*) { QThread::msleep(static_cast<unsigned long>(ms)); }); }
#endif
} // namespace roclint::host
