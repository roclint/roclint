// POSIX-only parent-death guardian. Protocol stdin/stdout/stderr belong directly
// to the worker; supervision never buffers or parses model traffic.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
namespace {
volatile std::sig_atomic_t stopping = 0;
void stopSignal(int) { stopping = 1; }
int number(const char* text) {
    char* end = nullptr; errno = 0;
    const auto value = std::strtol(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' && value >= 0 && value <= std::numeric_limits<int>::max() ? static_cast<int>(value) : -1;
}
}
int main(int argc, char** argv) {
    if (argc < 9 || std::string_view(argv[1]) != "--liveness-fd" || std::string_view(argv[3]) != "--control-fd" ||
        std::string_view(argv[5]) != "--parent-pid" || std::string_view(argv[7]) != "--") return 125;
    const int liveness = number(argv[2]), control = number(argv[4]), parent = number(argv[6]);
    if (liveness < 3 || control < 3 || liveness == control || parent <= 1 || ::getppid() != parent) return 125;
    struct sigaction action{}; action.sa_handler = stopSignal; sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr); sigaction(SIGINT, &action, nullptr); sigaction(SIGHUP, &action, nullptr);
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_DFL);
#ifdef __linux__
    static_cast<void>(prctl(PR_SET_CHILD_SUBREAPER, 1));
#endif
    // A gate prevents exec and descendant creation until the new process group exists.
    int gate[2]; if (::pipe(gate) != 0) return 125;
    const pid_t worker = ::fork();
    if (worker < 0) return 125;
    if (worker == 0) {
        ::close(gate[1]); ::close(liveness); ::close(control);
        if (::setpgid(0, 0) != 0) _exit(125);
        char ready = 0; ssize_t count;
        do { count = ::read(gate[0], &ready, 1); } while (count < 0 && errno == EINTR);
        ::close(gate[0]);
        if (count != 1 || ready != 'R') _exit(125);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        ::execvp(argv[8], argv + 8); _exit(127);
    }
    ::close(gate[0]);
    // Either side may win setpgid; the child is still behind the gate.
    const bool grouped = ::setpgid(worker, worker) == 0 || errno == EACCES;
    if (!grouped || ::getppid() != parent || stopping) {
        ::close(gate[1]); ::kill(worker, SIGKILL); ::waitpid(worker, nullptr, 0); return 125;
    }
    const char ready = 'R'; static_cast<void>(::write(gate[1], &ready, 1)); ::close(gate[1]);
    ::close(STDIN_FILENO); ::close(STDOUT_FILENO); ::close(STDERR_FILENO);
    using Clock = std::chrono::steady_clock;
    auto terminateAt = Clock::time_point::max();
    bool terminating = false;
    for (;;) {
        // WNOWAIT retains the leader PID until group cleanup, preventing PID reuse.
        siginfo_t info{};
        if (::waitid(P_PID, static_cast<id_t>(worker), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == worker) {
            ::kill(-worker, SIGKILL);
            int status = 0; while (::waitpid(worker, &status, 0) < 0 && errno == EINTR) {}
#ifdef __linux__
            const auto reapDeadline = Clock::now() + std::chrono::seconds(2);
            for (;;) {
                const auto reaped = ::waitpid(-1, nullptr, WNOHANG);
                if (reaped < 0 && errno == ECHILD) break;
                if (reaped == 0) {
                    if (Clock::now() >= reapDeadline) break;
                    ::poll(nullptr, 0, 5);
                }
            }
#endif
            return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
        }
        pollfd descriptors[2]{{liveness, POLLIN | POLLHUP, 0}, {control, POLLIN | POLLHUP, 0}};
        static_cast<void>(::poll(descriptors, 2, 25));
        bool immediate = false;
        if (descriptors[1].revents & POLLIN) {
            char commands[32]; const auto count = ::read(control, commands, sizeof(commands));
            for (ssize_t i = 0; i < count; ++i) if (commands[i] == 'K') immediate = true;
        }
        const bool ownerGone = ::getppid() != parent || (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) ||
                               (descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL));
        if (!terminating && (ownerGone || stopping || immediate)) {
            terminating = true; terminateAt = Clock::now() + std::chrono::seconds(2);
            ::kill(-worker, immediate ? SIGKILL : SIGTERM);
        }
        if (immediate || (terminating && Clock::now() >= terminateAt)) ::kill(-worker, SIGKILL);
    }
}
