#pragma once
#include <QObject>
#include <QThread>
#include <mutex>
#include <functional>
namespace roclint::host {
// Serialize posting with facade destruction. A cross-thread QPointer check alone
// cannot keep a QObject alive between the check and invokeMethod.
template<class Facade> class RuntimeMailbox {
public:
    explicit RuntimeMailbox(Facade* owner) : owner_(owner) {}
    template<class F> void post(F function) {
        const std::lock_guard lock(mutex_);
        if (!owner_) return;
        QMetaObject::invokeMethod(owner_, [target = owner_, function = std::move(function)]() { function(target); }, Qt::QueuedConnection);
    }
    void close() { const std::lock_guard lock(mutex_); owner_ = nullptr; }
private:
    std::mutex mutex_; Facade* owner_;
};
template<class Worker> class RuntimeThread {
public:
    explicit RuntimeThread(Worker* worker) : worker(worker), thread(new QThread) {
        worker->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }
    ~RuntimeThread() {
        QMetaObject::invokeMethod(worker, [target = worker, io = thread]() {
            QObject::connect(target, &Worker::stopped, io, &QThread::quit, Qt::DirectConnection);
            target->stop();
            if (!target->isRunning()) io->quit();
        }, Qt::QueuedConnection);
    }
    Worker* const worker;
private:
    QThread* thread;
};
} // namespace roclint::host
