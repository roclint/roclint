#include "JuliaRuntime.hpp"
#include <QPointer>
#include <QTimer>
#include <functional>
namespace roclint::host {
JuliaRuntime::JuliaRuntime(QObject* parent, QString path) : QObject(parent), shared_(std::make_shared<Shared>()) {
    auto* worker = new JuliaRuntimeWorker(nullptr, std::move(path));
    mailbox_ = std::make_shared<RuntimeMailbox<JuliaRuntime>>(this);
    const auto mailbox = mailbox_; const auto shared = shared_;
    const auto publish = [mailbox, worker, shared](std::function<void(JuliaRuntime*)> after = {}) {
        Snapshot snapshot{worker->isReady(), worker->isRunning(), worker->supportsPromptTriage(), worker->state(), worker->detail(),
            worker->executionConfigurationId(), worker->executionDevice(), worker->executionPrecision(), worker->lastRecommendedOption(),
            worker->lastChoicePolarity(), worker->executionCpuThreads(), worker->automaticRestartCount(), worker->lastChoiceConfidence()};
        mailbox->post([snapshot, epoch = shared->epoch, after](auto* owner) {
            owner->apply(snapshot, epoch); if (after) after(owner);
        });
    };
    connect(worker, &JuliaRuntimeWorker::stateChanged, worker, [publish]() { publish(); });
    connect(worker, &JuliaRuntimeWorker::stopped, worker, [publish]() { publish([](auto* front) { emit front->stopped(); }); });
    connect(worker, &JuliaRuntimeWorker::requestQueueFull, worker, [publish]() { publish([](auto* front) { emit front->requestQueueFull(); }); });
    const auto failed = [shared, publish](quint64 inner, const QString& reason) {
        const auto found = shared->requests.find(inner); if (found == shared->requests.end()) return;
        const auto outer = found.value(); shared->requests.erase(found);
        publish([outer, reason](auto* front) {
            const auto request = front->pending_.find(outer); if (request == front->pending_.end()) return;
            const bool triage = request->triage; front->pending_.erase(request);
            if (triage) emit front->promptClassificationFailed(outer, reason); else emit front->evaluationFailed(outer, reason);
        });
    };
    connect(worker, &JuliaRuntimeWorker::evaluationFailed, worker, failed);
    connect(worker, &JuliaRuntimeWorker::promptClassificationFailed, worker, failed);
    connect(worker, &JuliaRuntimeWorker::evaluationFinished, worker, [shared, publish](quint64 inner, const bridge::SnapshotEvaluationResponse& result) {
        if (!shared->requests.contains(inner)) return;
        const auto outer = shared->requests.take(inner);
        publish([outer, result](auto* front) {
            const auto request = front->pending_.find(outer); if (request == front->pending_.end()) return;
            const bool current = request->lifetime == front->lifetime_;
            const bool onTime = std::chrono::steady_clock::now() < request->deadline; front->pending_.erase(request);
            if (current && onTime) emit front->evaluationFinished(outer, result);
            else emit front->evaluationFailed(outer, onTime ? "Julia configuration changed before completion." : "Julia exceeded its operation deadline.");
        });
    });
    connect(worker, &JuliaRuntimeWorker::promptClassified, worker, [shared, publish](quint64 inner, const core::analysis::PromptTriage& result) {
        if (!shared->requests.contains(inner)) return;
        const auto outer = shared->requests.take(inner);
        publish([outer, result](auto* front) {
            const auto request = front->pending_.find(outer); if (request == front->pending_.end()) return;
            const bool current = request->lifetime == front->lifetime_;
            const bool onTime = std::chrono::steady_clock::now() < request->deadline; front->pending_.erase(request);
            if (current && onTime) emit front->promptClassified(outer, result);
            else emit front->promptClassificationFailed(outer, onTime ? "Julia configuration changed before completion." : "Julia exceeded its operation deadline.");
        });
    });
    io_ = std::make_unique<RuntimeThread<JuliaRuntimeWorker>>(worker);
}
JuliaRuntime::~JuliaRuntime() { mailbox_->close(); }
void JuliaRuntime::apply(Snapshot snapshot, quint64 epoch) {
    if (epoch != epoch_ || snapshot == cache_) return;
    cache_ = std::move(snapshot); emit stateChanged();
}
void JuliaRuntime::post(std::function<void(JuliaRuntimeWorker*)> command) {
    auto* worker = io_->worker; const auto shared = shared_; const auto epoch = epoch_;
    QMetaObject::invokeMethod(worker, [worker, shared, epoch, command = std::move(command)]() {
        shared->epoch = epoch; command(worker); worker->stateChanged();
    }, Qt::QueuedConnection);
}
void JuliaRuntime::load(const QString& directory, const QString& python, bool install) {
    ++epoch_; ++lifetime_; cache_.ready = false; cache_.running = true; cache_.restarts = 0; cache_.triage = false;
    post([=](auto* worker) { worker->load(directory, python, install); });
}
void JuliaRuntime::stop() { ++epoch_; ++lifetime_; cache_.ready = false; post([](auto* worker) { worker->stop(); }); }
void JuliaRuntime::setJuliaDevice(const QString& device) { post([device](auto* worker) { worker->setJuliaDevice(device); }); }
quint64 JuliaRuntime::accept(bool triage) {
    if (!cache_.ready || nextId_ >= 9007199254740991ULL) return 0;
    if (pending_.size() >= 8) { emit requestQueueFull(); return 0; }
    const auto id = ++nextId_; pending_.insert(id, {triage, lifetime_}); return id;
}
void JuliaRuntime::watchDeadline(quint64 id, int budgetMs) {
    QTimer::singleShot(budgetMs, Qt::PreciseTimer, this, [this, id]() {
        if (pending_.contains(id)) cancel(id, QStringLiteral("Julia exceeded its operation deadline. Retry explicitly; the original prompt remains available."));
    });
}
quint64 JuliaRuntime::evaluate(const core::analysis::NormalizedAnalysis& analysis, const QString& prompt, const RequestContext& input) {
    if (input.sessionId.toStdString() != analysis.sessionId || input.revisionId.toStdString() != analysis.baseRevisionId ||
        input.requestGroupId.isEmpty() || input.attemptId.isEmpty() || input.stateVersion > 9007199254740991ULL ||
        prompt.contains('\r') || prompt.toUtf8().size() > 65536) return 0;
    const auto outer = accept(false); if (!outer) return 0;
    const auto budget = timeoutOverride_ > 0 ? timeoutOverride_ : kEvaluationTimeoutMs;
    auto context = input; context.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget);
    pending_[outer].deadline = context.deadline;
    watchDeadline(outer, budget);
    const auto shared = shared_; const auto mailbox = mailbox_;
    post([=](auto* worker) {
        const auto inner = worker->evaluate(analysis, prompt, context);
        if (inner) shared->requests.insert(inner, outer);
        else mailbox->post([outer](auto* owner) { if (owner->pending_.remove(outer)) emit owner->evaluationFailed(outer, "Julia did not accept the request before its deadline."); });
    }); return outer;
}
quint64 JuliaRuntime::classifyPrompt(const QString& prompt, const RequestContext& input, const QJsonObject& metadata, const QStringList& choices) {
    if (!cache_.triage || input.stateVersion == 0 || input.stateVersion > 9007199254740991ULL || prompt.trimmed().isEmpty() ||
        prompt.contains('\r') || prompt.toUtf8().size() > 65536) return 0;
    for (const auto& field : {input.requestGroupId, input.attemptId, input.sessionId, input.revisionId})
        if (field.isEmpty() || field.toUtf8().size() > 256) return 0;
    const auto outer = accept(true); if (!outer) return 0;
    const auto budget = timeoutOverride_ > 0 ? timeoutOverride_ : kTriageTimeoutMs;
    auto context = input; context.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget);
    pending_[outer].deadline = context.deadline;
    watchDeadline(outer, budget);
    const auto shared = shared_; const auto mailbox = mailbox_;
    post([=](auto* worker) {
        const auto inner = worker->classifyPrompt(prompt, context, metadata, choices);
        if (inner) shared->requests.insert(inner, outer);
        else mailbox->post([outer](auto* owner) { if (owner->pending_.remove(outer)) emit owner->promptClassificationFailed(outer, "Julia did not accept the request before its deadline."); });
    }); return outer;
}
void JuliaRuntime::cancel(quint64 id, const QString& reason) {
    const auto found = pending_.find(id); if (found == pending_.end()) return;
    const bool triage = found->triage; pending_.erase(found);
    ++epoch_; cache_.ready = false; cache_.state = "CANCELLING"; emit stateChanged();
    const auto shared = shared_;
    post([id, reason, shared](auto* worker) {
        for (auto it = shared->requests.cbegin(); it != shared->requests.cend(); ++it)
            if (it.value() == id) { worker->cancel(it.key(), reason); return; }
        // A terminal result already left the I/O thread; no inference remains to cancel.
    });
    if (triage) emit promptClassificationFailed(id, reason); else emit evaluationFailed(id, reason);
}
void JuliaRuntime::cancelAllEvaluations() { const auto ids = pending_.keys(); for (const auto id : ids) cancel(id); }
#ifdef ROCLINT_ENABLE_TEST_MOCKS
void JuliaRuntime::stallIoForTesting(int ms) { post([ms](auto*) { QThread::msleep(static_cast<unsigned long>(ms)); }); }
#endif
} // namespace roclint::host
