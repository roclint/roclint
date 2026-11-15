#include "roclint/retrieval/EmbeddingWorkerRuntime.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Platform process management.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <cstring>
#  include <fcntl.h>
#  include <signal.h>
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#endif

namespace roclint::retrieval {
namespace {

using Clock = std::chrono::steady_clock;

// Generate a monotonically increasing request ID.
std::string nextRequestId() {
    static std::atomic<std::uint64_t> counter{1};
    return "req_" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Cross-platform subprocess I/O wrappers
// ---------------------------------------------------------------------------

#if defined(_WIN32)

struct Process {
    HANDLE hProcess{INVALID_HANDLE_VALUE};
    HANDLE hThread{INVALID_HANDLE_VALUE};
    HANDLE hStdinWrite{INVALID_HANDLE_VALUE};
    HANDLE hStdoutRead{INVALID_HANDLE_VALUE};

    ~Process() { terminate(); }

    void terminate() {
        // Close stdin first so the Python side sees EOF.
        if (hStdinWrite != INVALID_HANDLE_VALUE) {
            CloseHandle(hStdinWrite);
            hStdinWrite = INVALID_HANDLE_VALUE;
        }
        if (hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(hProcess, 1);
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess);
            hProcess = INVALID_HANDLE_VALUE;
        }
        if (hThread != INVALID_HANDLE_VALUE) {
            CloseHandle(hThread);
            hThread = INVALID_HANDLE_VALUE;
        }
        if (hStdoutRead != INVALID_HANDLE_VALUE) {
            CloseHandle(hStdoutRead);
            hStdoutRead = INVALID_HANDLE_VALUE;
        }
    }

    bool alive() const {
        if (hProcess == INVALID_HANDLE_VALUE) return false;
        DWORD code = 0;
        return GetExitCodeProcess(hProcess, &code) && code == STILL_ACTIVE;
    }

    // Write a single JSON line followed by '\n'. Returns false on failure.
    bool writeLine(const std::string& line) {
        if (hStdinWrite == INVALID_HANDLE_VALUE) return false;
        std::string msg = line + "\n";
        DWORD written = 0;
        return WriteFile(hStdinWrite, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr)
            && written == static_cast<DWORD>(msg.size());
    }

    // Read one newline-terminated line from stdout within deadline_ms.
    // Returns false on timeout, process death, or pipe error.
    bool readLine(std::string& out, int timeoutMs) {
        out.clear();
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            if (Clock::now() >= deadline) return false;
            char c = 0;
            DWORD avail = 0;
            if (!PeekNamedPipe(hStdoutRead, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (avail == 0) {
                if (!alive()) return false;
                Sleep(5);
                continue;
            }
            DWORD rd = 0;
            if (!ReadFile(hStdoutRead, &c, 1, &rd, nullptr) || rd == 0) return false;
            if (c == '\n') return true;
            out += c;
        }
    }

    // Drain any pending output without blocking past deadline_ms.
    // Used to discard delayed replies after a timeout.
    void drainStdout(int drainMs) {
        if (hStdoutRead == INVALID_HANDLE_VALUE) return;
        auto deadline = Clock::now() + std::chrono::milliseconds(drainMs);
        while (Clock::now() < deadline) {
            DWORD avail = 0;
            if (!PeekNamedPipe(hStdoutRead, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail == 0) {
                Sleep(5);
                continue;
            }
            char buf[256];
            DWORD rd = 0;
            ReadFile(hStdoutRead, buf, sizeof(buf), &rd, nullptr);
            // Discard: we just want to empty the pipe.
        }
    }
};

static std::unique_ptr<Process> startProcess(const EmbeddingWorkerConfig& cfg,
                                               std::string& errOut) {
    // Build command: python embedding_worker.py [--model-dir DIR]
    std::string cmd = "\"" + cfg.pythonExecutable + "\" \"" +
                      cfg.workerScriptPath.string() + "\"";
    if (!cfg.modelDirectory.empty())
        cmd += " --model-dir \"" + cfg.modelDirectory.string() + "\"";

    // Create pipes.
    HANDLE hStdinR, hStdinW, hStdoutR, hStdoutW;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&hStdinR, &hStdinW, &sa, 0)) { errOut = "CreatePipe(stdin) failed"; return nullptr; }
    if (!CreatePipe(&hStdoutR, &hStdoutW, &sa, 0)) {
        CloseHandle(hStdinR); CloseHandle(hStdinW);
        errOut = "CreatePipe(stdout) failed"; return nullptr;
    }
    SetHandleInformation(hStdinW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinR;
    si.hStdOutput = hStdoutW;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    std::wstring wcmd(cmd.begin(), cmd.end());
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hStdinR);
    CloseHandle(hStdoutW);

    if (!ok) {
        CloseHandle(hStdinW); CloseHandle(hStdoutR);
        errOut = "CreateProcess failed: " + std::to_string(GetLastError());
        return nullptr;
    }

    auto proc = std::make_unique<Process>();
    proc->hProcess = pi.hProcess;
    proc->hThread  = pi.hThread;
    proc->hStdinWrite = hStdinW;
    proc->hStdoutRead = hStdoutR;
    return proc;
}

#else // POSIX

struct Process {
    pid_t pid{-1};
    int fdStdinW{-1};  // write end
    int fdStdoutR{-1}; // read end

    ~Process() { terminate(); }

    void terminate() {
        if (fdStdinW >= 0) { ::close(fdStdinW); fdStdinW = -1; }
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, WNOHANG);
            pid = -1;
        }
        if (fdStdoutR >= 0) { ::close(fdStdoutR); fdStdoutR = -1; }
    }

    bool alive() const {
        if (pid < 0) return false;
        int status = 0;
        return ::waitpid(pid, &status, WNOHANG) == 0;
    }

    bool writeLine(const std::string& line) {
        if (fdStdinW < 0) return false;
        std::string msg = line + "\n";
        const char* p = msg.data();
        std::size_t rem = msg.size();
        while (rem > 0) {
            ssize_t n = ::write(fdStdinW, p, rem);
            if (n <= 0) return false;
            p += n; rem -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool readLine(std::string& out, int timeoutMs) {
        out.clear();
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            if (Clock::now() >= deadline) return false;
            char c = 0;
            ssize_t n = ::read(fdStdoutR, &c, 1);
            if (n == 0) return false; // EOF
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!alive()) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                return false;
            }
            if (c == '\n') return true;
            out += c;
        }
    }

    void drainStdout(int drainMs) {
        if (fdStdoutR < 0) return;
        auto deadline = Clock::now() + std::chrono::milliseconds(drainMs);
        while (Clock::now() < deadline) {
            char buf[256];
            ssize_t n = ::read(fdStdoutR, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                break;
            }
        }
    }
};

static std::unique_ptr<Process> startProcess(const EmbeddingWorkerConfig& cfg,
                                               std::string& errOut) {
    int stdinPipe[2], stdoutPipe[2];
    if (::pipe(stdinPipe) != 0 || ::pipe(stdoutPipe) != 0) {
        errOut = "pipe() failed";
        return nullptr;
    }

    pid_t pid = ::fork();
    if (pid < 0) { errOut = "fork() failed"; return nullptr; }

    if (pid == 0) {
        // Child.
        ::dup2(stdinPipe[0], STDIN_FILENO);
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::close(stdinPipe[0]); ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]); ::close(stdoutPipe[1]);

        std::string script = cfg.workerScriptPath.string();
        std::string modelDir = cfg.modelDirectory.string();
        if (!modelDir.empty()) {
            ::execlp(cfg.pythonExecutable.c_str(), cfg.pythonExecutable.c_str(),
                     script.c_str(), "--model-dir", modelDir.c_str(), nullptr);
        } else {
            ::execlp(cfg.pythonExecutable.c_str(), cfg.pythonExecutable.c_str(),
                     script.c_str(), nullptr);
        }
        ::_exit(127);
    }

    // Parent.
    ::close(stdinPipe[0]);
    ::close(stdoutPipe[1]);
    // Make stdout read-end non-blocking.
    ::fcntl(stdoutPipe[0], F_SETFL, O_NONBLOCK);

    auto proc = std::make_unique<Process>();
    proc->pid = pid;
    proc->fdStdinW = stdinPipe[1];
    proc->fdStdoutR = stdoutPipe[0];
    return proc;
}

#endif // Platform

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Very small escape-only JSON encoder for string values.
// Handles the common ASCII range; non-ASCII bytes pass through as UTF-8.
static std::string jsonEscapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        if (c == '"')       { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else if (c < 0x20)  { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
        else                { out += static_cast<char>(c); }
    }
    out += '"';
    return out;
}

// Extract the string value for a key from a simple flat JSON object.
// Does not handle nested objects or arrays as values, but handles escaped chars.
static std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\') {
                ++pos;
                if (pos >= json.size()) break;
                char esc = json[pos++];
                switch (esc) {
                    case '"':  val += '"'; break;
                    case '\\': val += '\\'; break;
                    case 'n':  val += '\n'; break;
                    case 'r':  val += '\r'; break;
                    case 't':  val += '\t'; break;
                    default:   val += esc; break;
                }
                continue;
            }
            val += json[pos++];
        }
        return val;
    }
    // Bare value (bool/number)
    std::string val;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}') val += json[pos++];
    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
    return val;
}

static bool jsonGetBool(const std::string& json, const std::string& key, bool defaultVal = false) {
    auto v = jsonGetString(json, key);
    if (v == "true") return true;
    if (v == "false") return false;
    return defaultVal;
}

static int jsonGetInt(const std::string& json, const std::string& key, int defaultVal = 0) {
    auto v = jsonGetString(json, key);
    if (v.empty()) return defaultVal;
    try { return std::stoi(v); } catch (...) { return defaultVal; }
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct EmbeddingWorkerRuntime::Impl {
    EmbeddingWorkerConfig config;
    std::unique_ptr<Process> process;
    std::atomic<EmbeddingRuntimeState> state_{EmbeddingRuntimeState::Unloaded};
    mutable std::mutex mutex_; // Serialises process I/O (one request at a time).
    IndexPolicy policy_{};
    std::string statusMsg_{"Not started"};
    // Expected model identity (set from ping response, used to validate embed responses).
    std::string expectedModelId_{};
    std::string expectedModelRevision_{};
    int expectedDimensions_{768};

    explicit Impl(EmbeddingWorkerConfig cfg) : config(std::move(cfg)) {}

    // Build the index policy once the subprocess reports its model info.
    void setPolicy(const std::string& modelId, const std::string& modelRevision, int dims) {
        policy_.modelId = modelId;
        policy_.modelRevision = modelRevision;
        policy_.dimensions = dims;
        policy_.normalized = true;
        policy_.chunkerVersion = "text_chunker.v1.target512.overlap64";
        policy_.preprocessVersion = "sentence_transformers.v1";
        expectedModelId_ = modelId;
        expectedModelRevision_ = modelRevision;
        expectedDimensions_ = dims;
    }

    // Terminate and clear the process. Must be called with mutex_ held.
    void killProcess() {
        if (process) {
            process->terminate();
            process.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// EmbeddingWorkerRuntime
// ---------------------------------------------------------------------------

EmbeddingWorkerRuntime::EmbeddingWorkerRuntime(EmbeddingWorkerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{}

EmbeddingWorkerRuntime::~EmbeddingWorkerRuntime() {
    stop();
}

core::Status EmbeddingWorkerRuntime::start() {
    std::unique_lock<std::mutex> lock(impl_->mutex_);
    if (impl_->state_.load() == EmbeddingRuntimeState::Ready) return core::Status::success();
    if (impl_->state_.load() == EmbeddingRuntimeState::Loading)
        return core::makeError(core::ErrorCode::PreconditionFailed, "Already starting");

    // Check that the worker script exists.
    if (!std::filesystem::exists(impl_->config.workerScriptPath)) {
        impl_->state_.store(EmbeddingRuntimeState::Unavailable);
        impl_->statusMsg_ = "embedding_worker.py not found: " +
                             impl_->config.workerScriptPath.string();
        return core::makeError(core::ErrorCode::ProviderUnavailable, impl_->statusMsg_);
    }

    impl_->state_.store(EmbeddingRuntimeState::Loading);
    impl_->statusMsg_ = "Loading embedding model...";

    std::string launchErr;
    impl_->process = startProcess(impl_->config, launchErr);
    if (!impl_->process) {
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Failed to start embedding worker: " + launchErr;
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    // Send a ping and wait for the response (startup includes model loading).
    std::string pingId = nextRequestId();
    std::string pingReq = R"({"id":)" + jsonEscapeString(pingId) + R"(,"op":"ping"})";

    if (!impl_->process->writeLine(pingReq)) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Failed to write ping to embedding worker";
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    std::string respLine;
    if (!impl_->process->readLine(respLine, impl_->config.startupTimeoutMs)) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Embedding worker did not respond within startup timeout";
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    // Validate response ID binds to our ping request.
    const std::string respId = jsonGetString(respLine, "id");
    if (respId != pingId) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Ping response ID mismatch: expected=" + pingId +
                             " got=" + respId;
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    if (!jsonGetBool(respLine, "ok")) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Embedding worker ping failed: " + jsonGetString(respLine, "error");
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    // ---- Blocker 6: Model integrity verification ----
    //
    // Extract model identity from ping response and validate it against the
    // pinned values.  Trust must not be established merely by accepting whatever
    // the subprocess reports — an arbitrary local model labelled with the pinned
    // metadata must not pass.
    //
    // The pinned constants are compiled into the host so that the subprocess
    // cannot lie about its identity.
    static constexpr const char kPinnedModelId[]  =
        "ibm-granite/granite-embedding-311m-multilingual-r2";
    static constexpr const char kPinnedRevision[] =
        "44399559930365213510b1ee2eb15ded83374f0e";
    static constexpr int kPinnedDimensions = 768;

    const std::string modelId  = jsonGetString(respLine, "model_id");
    const std::string modelRev = jsonGetString(respLine, "model_revision");
    const int dim              = jsonGetInt(respLine, "dim", 768);
    const std::string pooling  = jsonGetString(respLine, "pooling");

    if (modelId.empty() || modelRev.empty()) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Ping response missing model_id or model_revision";
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    // Reject models that do not match the pinned identity.
    if (modelId != kPinnedModelId) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = std::string("Loaded model_id '") + modelId +
                             "' does not match pinned model '" + kPinnedModelId + "'";
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }
    if (modelRev != kPinnedRevision) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = std::string("Loaded model_revision '") + modelRev +
                             "' does not match pinned revision '" + kPinnedRevision + "'";
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    if (dim <= 0 || dim > 65536) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Ping response reported invalid dimensions: " + std::to_string(dim);
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }
    if (dim != kPinnedDimensions) {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Loaded model reports " + std::to_string(dim) +
                             " dimensions; expected " + std::to_string(kPinnedDimensions);
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    // Reject missing or contradictory pooling configuration.
    // The selected model uses CLS pooling; mean pooling or unknown produce
    // different (and incorrect) embedding representations.
    if (pooling.empty() || pooling == "unknown") {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = std::string("Ping response reports pooling='") + pooling +
                             "': cannot verify pooling mode (1_Pooling/config.json may be missing)";
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }
    if (pooling != "cls") {
        impl_->killProcess();
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = std::string("Ping response reports pooling='") + pooling +
                             "'; expected 'cls' (ibm-granite/granite-embedding-311m-multilingual-r2 "
                             "uses CLS-token pooling)";
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    impl_->setPolicy(modelId, modelRev, dim);
    impl_->state_.store(EmbeddingRuntimeState::Ready);
    impl_->statusMsg_ = "Ready: " + modelId + " rev=" + modelRev.substr(0, 12) +
                         " dim=" + std::to_string(dim) + " pooling=" + pooling;
    return core::Status::success();
}

void EmbeddingWorkerRuntime::stop() {
    std::unique_lock<std::mutex> lock(impl_->mutex_);
    if (impl_->process) {
        impl_->process->writeLine(R"({"id":"quit","op":"quit"})");
        // Give the subprocess a moment to exit cleanly.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        impl_->killProcess();
    }
    impl_->state_.store(EmbeddingRuntimeState::Unloaded);
    impl_->statusMsg_ = "Stopped";
}

core::Result<EmbeddingBatch> EmbeddingWorkerRuntime::embed(
    const std::vector<std::string>& texts,
    const EmbedOptions& options)
{
    if (texts.empty()) {
        EmbeddingBatch batch;
        batch.rows = 0;
        batch.dimensions = impl_->expectedDimensions_;
        return batch;
    }

    // Bound batch size.
    static constexpr std::size_t kMaxBatchSize = 256;
    if (texts.size() > kMaxBatchSize)
        return core::makeError(core::ErrorCode::InvalidInput,
            "Batch size " + std::to_string(texts.size()) +
            " exceeds maximum " + std::to_string(kMaxBatchSize));

    std::unique_lock<std::mutex> lock(impl_->mutex_);
    const auto state = impl_->state_.load();

    if (state == EmbeddingRuntimeState::Unavailable)
        return core::makeError(core::ErrorCode::ProviderUnavailable, "Model unavailable");
    if (state != EmbeddingRuntimeState::Ready)
        return core::makeError(core::ErrorCode::ProviderUnavailable,
            "Embedding runtime not ready (state: " + std::string(toString(state)) + ")");
    if (!impl_->process || !impl_->process->alive())
        return core::makeError(core::ErrorCode::ProviderFailed, "Embedding worker process died");

    // Assign a unique request ID for exact request/response binding.
    std::string reqId = nextRequestId();

    // Build JSON request.
    std::string req = R"({"id":)" + jsonEscapeString(reqId) + R"(,"op":"embed","texts":[)";
    for (std::size_t i = 0; i < texts.size(); ++i) {
        if (i > 0) req += ",";
        req += jsonEscapeString(texts[i]);
    }
    req += "]}";

    if (!impl_->process->writeLine(req))
        return core::makeError(core::ErrorCode::ProviderFailed, "Failed to write embed request");

    const int timeoutMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(options.timeout).count());

    std::string respLine;
    if (!impl_->process->readLine(respLine, timeoutMs)) {
        // On timeout: the process may still deliver a delayed reply on the next
        // read. We cannot safely re-use the connection. Mark failed and kill.
        // The next embed() call will also fail (state == Failed).
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Embedding worker timed out for request " + reqId +
                             "; process killed to prevent consuming delayed reply";
        impl_->killProcess();

        if (options.cancellation && options.cancellation->load())
            return core::makeError(core::ErrorCode::ProviderFailed, "Embedding cancelled");
        return core::makeError(core::ErrorCode::ProviderFailed,
            "Embedding worker timed out for request " + reqId);
    }

    // --- Exact request/response binding ---
    const std::string respId = jsonGetString(respLine, "id");
    if (respId != reqId) {
        // Response from a different (previous, delayed) request arrived.
        // This connection is poisoned; kill and fail.
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Response ID mismatch: expected=" + reqId + " got=" + respId +
                             "; connection poisoned";
        impl_->killProcess();
        return core::makeError(core::ErrorCode::ProviderFailed,
            "Embed response ID mismatch: expected=" + reqId + " got=" + respId);
    }

    if (!jsonGetBool(respLine, "ok")) {
        return core::makeError(core::ErrorCode::ProviderFailed,
            "Embed failed: " + jsonGetString(respLine, "error"));
    }

    // --- Model identity validation ---
    const std::string respModelId  = jsonGetString(respLine, "model_id");
    const std::string respModelRev = jsonGetString(respLine, "model_revision");
    const int respDim              = jsonGetInt(respLine, "dim", 0);

    if (!respModelId.empty() && respModelId != impl_->expectedModelId_) {
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Response model_id mismatch: expected=" + impl_->expectedModelId_ +
                             " got=" + respModelId;
        impl_->killProcess();
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }
    if (!respModelRev.empty() && respModelRev != impl_->expectedModelRevision_) {
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Response model_revision mismatch: expected=" +
                             impl_->expectedModelRevision_ + " got=" + respModelRev;
        impl_->killProcess();
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }
    if (respDim != 0 && respDim != impl_->expectedDimensions_) {
        impl_->state_.store(EmbeddingRuntimeState::Failed);
        impl_->statusMsg_ = "Response dimensions mismatch: expected=" +
                             std::to_string(impl_->expectedDimensions_) +
                             " got=" + std::to_string(respDim);
        impl_->killProcess();
        return core::makeError(core::ErrorCode::ProviderFailed, impl_->statusMsg_);
    }

    // --- Parse embeddings array from JSON ---
    // Format: "embeddings":[[f,f,...],[f,f,...]]
    const std::string kEmbKey = "\"embeddings\":";
    auto embPos = respLine.find(kEmbKey);
    if (embPos == std::string::npos)
        return core::makeError(core::ErrorCode::ProviderFailed, "No embeddings in response");

    embPos += kEmbKey.size();
    while (embPos < respLine.size() && respLine[embPos] == ' ') ++embPos;

    if (embPos >= respLine.size() || respLine[embPos] != '[')
        return core::makeError(core::ErrorCode::ProviderFailed, "Malformed embeddings: missing outer [");

    ++embPos; // skip outer [
    EmbeddingBatch batch;
    batch.dimensions = impl_->expectedDimensions_;
    std::vector<float> rowBuf;
    rowBuf.reserve(static_cast<std::size_t>(batch.dimensions));

    while (embPos < respLine.size() && respLine[embPos] != ']') {
        while (embPos < respLine.size() && respLine[embPos] == ' ') ++embPos;
        if (embPos >= respLine.size() || respLine[embPos] == ']') break;
        if (respLine[embPos] != '[') break;
        ++embPos; // skip inner [
        rowBuf.clear();
        while (embPos < respLine.size() && respLine[embPos] != ']') {
            while (embPos < respLine.size() &&
                   (respLine[embPos] == ' ' || respLine[embPos] == ',')) ++embPos;
            if (embPos >= respLine.size() || respLine[embPos] == ']') break;
            char* endptr = nullptr;
            float v = std::strtof(respLine.data() + embPos, &endptr);
            if (endptr == respLine.data() + embPos) break;
            rowBuf.push_back(v);
            embPos = static_cast<std::size_t>(endptr - respLine.data());
        }
        ++embPos; // skip inner ]
        while (embPos < respLine.size() && respLine[embPos] == ',') ++embPos;

        // --- Per-row shape validation ---
        if (static_cast<int>(rowBuf.size()) != batch.dimensions) {
            return core::makeError(core::ErrorCode::ProviderFailed,
                "Row " + std::to_string(batch.rows) + " has " + std::to_string(rowBuf.size()) +
                " dims; expected " + std::to_string(batch.dimensions));
        }

        // --- Finite-value check ---
        for (std::size_t fi = 0; fi < rowBuf.size(); ++fi) {
            if (!std::isfinite(rowBuf[fi])) {
                return core::makeError(core::ErrorCode::ProviderFailed,
                    "Row " + std::to_string(batch.rows) + " element " +
                    std::to_string(fi) + " is non-finite");
            }
        }

        for (auto f : rowBuf) batch.data.push_back(f);
        ++batch.rows;
    }

    // --- Total shape validation ---
    if (batch.rows != texts.size()) {
        return core::makeError(core::ErrorCode::ProviderFailed,
            "Response row count mismatch: expected " + std::to_string(texts.size()) +
            " got " + std::to_string(batch.rows));
    }
    const std::size_t expectedDataSize = batch.rows * static_cast<std::size_t>(batch.dimensions);
    if (batch.data.size() != expectedDataSize) {
        return core::makeError(core::ErrorCode::ProviderFailed,
            "Response data size mismatch: expected " + std::to_string(expectedDataSize) +
            " floats, got " + std::to_string(batch.data.size()));
    }

    return batch;
}

EmbeddingRuntimeState EmbeddingWorkerRuntime::state() const noexcept {
    return impl_ ? impl_->state_.load() : EmbeddingRuntimeState::Unloaded;
}

IndexPolicy EmbeddingWorkerRuntime::policy() const {
    return impl_ ? impl_->policy_ : IndexPolicy{};
}

std::string EmbeddingWorkerRuntime::statusMessage() const {
    return impl_ ? impl_->statusMsg_ : "Not initialized";
}

} // namespace roclint::retrieval
