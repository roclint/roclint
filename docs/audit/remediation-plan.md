# Roclint Cross-Platform Remediation Plan
**Based on:** `docs/audit/platform-findings.md`
**Findings addressed:** F-01 through F-05

> **Action gate:** All phases below are proposed. No source files will be modified until this plan receives explicit approval.

---

## Phase 1 — Critical Logic & Resource Leaks

### P1-A: Lock Granite Decode Thread Count at 4 on macOS (F-01)
**File:** [`app/runtime/GraniteEngine.cpp`](app/runtime/GraniteEngine.cpp) — lines 215–220

**Root cause:** The Apple-specific block correctly limits batch-prefill threads (`n_threads_batch`) to `min(hwCores, 8)` but leaves decode threads (`n_threads`) at whatever `cpu_threads` the manifest supplies. On a 12-core M3 Pro with a manifest value of 8, decode threads run at 8, potentially saturating Metal's command scheduler.

**Fix:**
```cpp
// After the existing Apple block (around line 220), add:
#if defined(__APPLE__)
    params.n_threads = std::min(params.n_threads, 4);   // lock decode at 4 to prevent Metal queue starvation
#endif
```

**Test impact:** None — `GraniteEngine` does not have a direct unit test for thread count, but `test_native_granite_runtime.cpp` exercises the full load/generate path and would continue to pass.

**Risk:** Low. If `cpu_threads` in the manifest is already ≤ 4 this is a no-op.

---

### P1-B: Clear Stale `lastTokensPerSecond_` on Abort (F-04)
**File:** [`app/host/NativeGraniteRuntime.cpp`](app/host/NativeGraniteRuntime.cpp) — inside `beginStop()` / `cancelCall()` paths

**Root cause:** When an inference is aborted (cancel or timeout), `lastTokensPerSecond_` retains its value from the previous completed generation. The UI displays a speed readout for a failed request.

**Fix:** Reset `lastTokensPerSecond_` (and companion fields) to `0.0` inside `beginStop()`:
```cpp
void NativeGraniteRuntime::beginStop(bool failure, const QString& reason) {
    ready_ = false;
    lastTokensPerSecond_ = 0.0;   // clear stale telemetry
    lastOutputTokens_ = 0;
    lastDecodeMs_ = 0.0;
    lastGenerationMs_ = 0.0;
    // … existing code …
}
```

Also emit `speedMetricsChanged()` at the end of `beginStop()` so QML clears the SPEED strip immediately on cancel or stop.

**Test impact:** `test_host_controller.cpp` and `test_host_lifecycle.cpp` — no change expected; metric tests would need new assertions if added.

**Risk:** Minimal. Cosmetic improvement only.

---

## Phase 2 — Path Normalization

*No hardcoded Unix or Windows path separators were found in production code. Qt APIs (`QDir`, `QStandardPaths`, `QFileInfo`) handle separator normalization automatically. `pathlib.Path.as_posix()` is used correctly in `julia_worker.py` for manifest keys.*

**No action required in Phase 2.**

---

## Phase 3 — Shell & Process Portability

### P3-A: Fix `acceleratorName()` to Reflect Actual Build Backend (F-02)
**File:** [`app/host/HostController.hpp`](app/host/HostController.hpp) — lines 346–354

**Root cause:** The function is marked `CONSTANT` (QML compile-time binding) and returns a hardcoded string based on OS. On a Windows CPU build (no Vulkan SDK detected at CMake time), the returned value `"VULKAN"` is incorrect.

**Fix option A — Read from the manifest (runtime, most accurate):**
```cpp
[[nodiscard]] QString acceleratorName() const {
    const auto backend = NativeGraniteRuntime::manifest().value("backend").toString().toUpper();
    if (!backend.isEmpty()) return backend;
#if defined(__APPLE__)
    return QStringLiteral("METAL");
#elif defined(_WIN32)
    return QStringLiteral("VULKAN");
#else
    return QStringLiteral("GPU");
#endif
}
```
Since the manifest `backend` field is baked in at build time by CMakeLists.txt (line 275–278), this reads the same CMake-selected backend that `GraniteEngine` actually uses. The property must change from `CONSTANT` to `NOTIFY inferenceSpeedChanged` (or a new signal) if switching to runtime reads. Since the manifest never changes at runtime, `CONSTANT` can be preserved by reading it in the constructor.

**Fix option B — CMake-supplied compile definition (zero runtime overhead):**
In `CMakeLists.txt`, after the backend is resolved:
```cmake
target_compile_definitions(roclint_host_lib PRIVATE
    ROCLINT_ACCELERATOR_NAME="${_roclint_backend}")
```
Then in `HostController.hpp`:
```cpp
[[nodiscard]] QString acceleratorName() const {
    return QStringLiteral(ROCLINT_ACCELERATOR_NAME);
}
```

**Recommended:** Option B — simpler, zero overhead, stays `CONSTANT`.

**Test impact:** `test_product_alignment.cpp` checks the QML `acceleratorName` binding; no test currently asserts the string value, so no test update needed.

---

### P3-B: Surface CUDA Degradation to User on Windows (F-03)
**File:** [`app/runtime/julia_worker.py`](app/runtime/julia_worker.py) — `create_engine()` function

**Root cause:** When CUDA is unavailable on Windows, the function silently falls back to CPU FP32. The `runtime_status` response includes `device: "cpu"` and `compute_dtype: "float32"`, but the host side shows the same `READY` state to the user.

**Fix:** In `JuliaRuntime.cpp`, when the returned profile has `device == "cpu"` on Windows, emit a one-time advisory via `setState()`:
```cpp
// In JuliaRuntime::receive(), after validating the profile:
#ifdef Q_OS_WIN
if (device == QLatin1String("cpu")) {
    setState(QStringLiteral("READY"),
        QStringLiteral("Julia-1 is running on CPU (FP32) — GPU acceleration unavailable. "
                       "Install CUDA drivers for RTX hardware to improve performance."));
} else {
    setState(QStringLiteral("READY"), ...existing message...);
}
#endif
```

The status string is visible in JuliaSettings.qml. No separate error modal is needed.

**Test impact:** `test_host_controller.cpp` mock path exercises `julia.cpu.fp32.v2`; the string change only affects the display message, no test assertions need updating.

---

### P3-C: macOS `handlerRef` Leak on Rapid Re-Registration (F-05)
**File:** [`app/host/GlobalShortcutManager.cpp`](app/host/GlobalShortcutManager.cpp) — `registerSequence()` macOS branch

**Root cause:** `InstallApplicationEventHandler()` is called on every `registerSequence()` call. If the previous `handlerRef` was not cleared by `unregisterShortcut()` before calling again, the old handler is orphaned. In the normal code path this cannot happen (because `changeShortcut()` calls `unregisterShortcut()` first), but it is a defensive gap.

**Fix:** Guard the installation:
```cpp
#if defined(Q_OS_MAC)
    // Remove any existing handler before installing a new one.
    if (m_impl->handlerRef) {
        RemoveEventHandler(m_impl->handlerRef);
        m_impl->handlerRef = nullptr;
    }
    OSStatus status = InstallApplicationEventHandler(…);
```

**Test impact:** None — the Carbon path is not exercised by automated tests. Manual regression: verify hotkey still fires after a shortcut change in System Settings.

---

## Phase 4 — Cross-Platform Test Verification

### P4-A: Add Decode-Thread Count Assertion to Native Granite Test
**File:** `tests/bridge/test_native_granite_runtime.cpp`

After P1-A lands, add a load-and-inspect test:
```cpp
// After loading the model on macOS, query the context thread count via llama_n_threads
// (if exposed), or verify indirectly via the manifest + compile flag.
```
Since `llama_context` thread count is not directly queryable via the current API, this may be validated via build-time static assert or a manifest-check fixture.

### P4-B: Add `acceleratorName()` Value Assertion
**File:** `tests/viewmodel/test_product_alignment.cpp`

After P3-A lands, assert that `acceleratorName()` matches the `backend` field in the compiled manifest:
```cpp
ROCLINT_CHECK(controller.acceleratorName().toLower() ==
              NativeGraniteRuntime::manifest().value("backend").toString().toLower());
```

### P4-C: Add `inferenceSpeed` Cleared-After-Abort Assertion
**File:** `tests/viewmodel/test_host_lifecycle.cpp` or `test_host_controller.cpp`

After P1-B lands, add a test that:
1. Completes a native inference (mock transport)
2. Verifies `tokensPerSecond() > 0`
3. Cancels the runtime (`controller.stop()`)
4. Verifies `tokensPerSecond() == 0.0` and `inferenceSpeed().isEmpty()`

---

## Summary Table

| ID | Finding | Phase | Effort | Risk |
|---|---|---|---|---|
| F-01 | Decode threads not locked at 4 on macOS | P1-A | 2 lines | Low |
| F-04 | Stale `lastTokensPerSecond_` on abort | P1-B | ~8 lines | Minimal |
| F-02 | `acceleratorName()` hardcoded `"VULKAN"` on Windows | P3-A | ~5 lines + CMake | Low |
| F-03 | Silent CUDA → CPU degradation on Windows | P3-B | ~10 lines | Low |
| F-05 | macOS Carbon `handlerRef` orphan on rapid re-call | P3-C | 3 lines | Low |

**All changes are additive / narrowly scoped. No refactors, no API breaks, no schema changes.**
