#pragma once

#include <QProcess>
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
    void terminate() { if (handle_) static_cast<void>(TerminateJobObject(handle_, 1)); }
private:
    HANDLE handle_{nullptr};
    PROCESS_INFORMATION* information_{nullptr}; // Qt owns it; used only by the started callback.
#else
    bool valid() const { return true; }
    void prepare(QProcess*) {}
    bool resume() { return true; }
    void terminate() {}
#endif
};

} // namespace roclint::host
