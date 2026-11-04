#pragma once

#include <QProcess>
#include <QCoreApplication>
#include <QFileInfo>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace roclint::host {
// A Windows venv launcher can spawn the real interpreter. Assign its suspended
// root to a non-inheritable kill-on-close job before letting it create children.
class OwnedProcessJob {
public:
#ifdef Q_OS_WIN
    OwnedProcessJob() {
        handle_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (handle_ && !SetInformationJobObject(handle_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }
    ~OwnedProcessJob() { if (handle_) CloseHandle(handle_); }
    bool valid() const { return handle_ != nullptr; }
    void prepare(QProcess* process) {
        process->setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_SUSPENDED | CREATE_NO_WINDOW;
            information_ = reinterpret_cast<PROCESS_INFORMATION*>(args->processInformation);
        });
    }
    bool resume() {
        if (!handle_ || !information_ || !AssignProcessToJobObject(handle_, information_->hProcess)) return false;
        const bool resumed = ResumeThread(information_->hThread) != static_cast<DWORD>(-1);
        information_ = nullptr;
        return resumed;
    }
    bool terminate() { return handle_ && TerminateJobObject(handle_, 1); }
    void start(QProcess* process) { process->start(); }
private:
    HANDLE handle_{nullptr};
    PROCESS_INFORMATION* information_{nullptr}; // Qt owns it; used only by the started callback.
#else
    OwnedProcessJob() {
        if (::pipe(liveness_) != 0) { liveness_[0] = liveness_[1] = -1; return; }
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, control_) != 0) { closeDescriptors(); return; }
        for (const auto fd : {liveness_[0], liveness_[1], control_[0], control_[1]})
            if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) { closeDescriptors(); return; }
        ::fcntl(control_[0], F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        const int enabled = 1; ::setsockopt(control_[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    }
    ~OwnedProcessJob() { closeDescriptors(); }
    bool valid() const { return liveness_[0] >= 0 && control_[0] >= 0 && QFileInfo(guardian()).isExecutable(); }
    void prepare(QProcess* process) {
        const auto read = liveness_[0], write = liveness_[1], childControl = control_[1], hostControl = control_[0];
        process->setChildProcessModifier([read, write, childControl, hostControl]() {
            ::close(write); ::close(hostControl);
            ::fcntl(read, F_SETFD, 0); ::fcntl(childControl, F_SETFD, 0);
        });
    }
    bool resume() {
        // Only the host retains the liveness writer. The guardian alone retains its reader.
        if (liveness_[0] >= 0) { ::close(liveness_[0]); liveness_[0] = -1; }
        if (control_[1] >= 0) { ::close(control_[1]); control_[1] = -1; }
        return true;
    }
    bool terminate() {
        if (control_[0] < 0) return false;
        const char command = 'K';
#ifdef MSG_NOSIGNAL
        const auto sent = ::send(control_[0], &command, 1, MSG_NOSIGNAL);
#else
        const auto sent = ::send(control_[0], &command, 1, 0);
#endif
        if (sent != 1 && liveness_[1] >= 0) { ::close(liveness_[1]); liveness_[1] = -1; }
        // Never SIGKILL the guardian: it must kill and reap its worker first.
        return true;
    }
    void start(QProcess* process) {
        auto arguments = process->arguments(); arguments.prepend(process->program());
        arguments.prepend(QStringLiteral("--"));
        arguments.prepend(QString::number(::getpid())); arguments.prepend(QStringLiteral("--parent-pid"));
        arguments.prepend(QString::number(control_[1])); arguments.prepend(QStringLiteral("--control-fd"));
        arguments.prepend(QString::number(liveness_[0])); arguments.prepend(QStringLiteral("--liveness-fd"));
        process->setArguments(arguments); process->setProgram(guardian()); process->start();
    }
private:
    static QString guardian() { return QCoreApplication::applicationDirPath() + QStringLiteral("/runtime/roclint_process_guardian"); }
    void closeDescriptors() {
        for (auto* pair : {liveness_, control_}) for (int i = 0; i < 2; ++i)
            if (pair[i] >= 0) { ::close(pair[i]); pair[i] = -1; }
    }
    int liveness_[2]{-1, -1};
    int control_[2]{-1, -1};

#endif
};

} // namespace roclint::host
