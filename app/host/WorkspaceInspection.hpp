#pragma once
#include <QCoreApplication>
#include "OwnedProcessJob.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <functional>
#include <memory>
#include <utility>

namespace roclint::host {
// No filesystem work or synchronous process waits on the caller's thread.
inline std::function<void()> inspectWorkspace(QObject* owner, const QJsonObject& request, int timeoutMs,
                                             std::function<void(QJsonObject)> completion,
                                             QString helper = {}, QStringList arguments = {}) {
    struct State { bool done = false; QByteArray output; };
    const auto state = std::make_shared<State>();
    auto* process = new QProcess;
    const auto job = std::make_shared<OwnedProcessJob>();
    const QPointer<QObject> receiver(owner);
    const QPointer<QProcess> guarded(process);
    const auto finish = [state, receiver, guarded, completion, job](QJsonObject result) {
        if (std::exchange(state->done, true)) return;
        if (guarded && guarded->state() != QProcess::NotRunning) {
            if (!job->terminate()) guarded->kill();
        }
        if (receiver) completion(std::move(result));
    };
    QObject::connect(owner, &QObject::destroyed, process, [process, state, job]() { state->done = true; if (!job->terminate()) process->kill(); });
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process, state, finish]() {
        state->output += process->readAllStandardOutput();
        if (state->output.size() > 1024 * 1024) finish({});
    });
    QObject::connect(process, &QProcess::readyReadStandardError, process, [process]() { process->readAllStandardError(); });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { finish({}); process->deleteLater(); }
    });
    QObject::connect(process, &QProcess::finished, process, [process, state, finish](int code, QProcess::ExitStatus status) {
        state->output += process->readAllStandardOutput();
        finish(code == 0 && status == QProcess::NormalExit && state->output.size() <= 1024 * 1024
                   ? QJsonDocument::fromJson(state->output).object() : QJsonObject{});
        process->deleteLater();
    });
    QTimer::singleShot(timeoutMs, process, [finish]() { finish({}); });
    const auto payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    QObject::connect(process, &QProcess::started, process, [process, payload, job, finish]() {
        if (!job->resume()) { finish({}); return; }
        process->write(payload); process->closeWriteChannel();
    });
    if (helper.isEmpty()) helper = QCoreApplication::applicationDirPath() + QStringLiteral("/runtime/roclint_workspace_helper")
#ifdef Q_OS_WIN
        + QStringLiteral(".exe")
#endif
    ;
    process->setProgram(helper);
    process->setArguments(arguments);
    if (payload.size() > 1024 * 1024 || !job->valid()) { finish({}); process->deleteLater(); }
    else { job->prepare(process); job->start(process); }
    return [guarded, finish]() { if (guarded) finish({}); };
}
} // namespace roclint::host
