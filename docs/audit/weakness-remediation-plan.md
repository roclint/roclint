# Weakness Remediation Plan
**Based on:** post-audit strengths/weaknesses assessment  
**Codebase:** `/Users/jesse/roclint` · git `main`  
**Date:** 2025-07-11  

Each weakness is assigned an ID (R-01 through R-06), a risk tier, an effort
estimate, and a concrete implementation plan. Changes are ordered from lowest
to highest risk so they can be merged independently without blocking each other.

---

## R-01 — Repair the stale `alignment_baseline.json` manifest

**Weakness:** `check_structure` has 3 pre-existing failures caused by two
missing `bob_sessions/bob_artifacts` paths and the unregistered `docs/audit`
directory. The guard is supposed to catch unreviewed additions — but it cannot
catch new violations when it is already failing.

**Risk:** Low. Only JSON and a test file change.  
**Effort:** ~30 min.

### Root cause
`docs/alignment_baseline.json` lists two artifact paths in
`upstream.files_lf_sha256` that no longer exist in the working tree:
```
bob_sessions/bob_artifacts/roclint-code-review-security-correctness-findings.html
bob_sessions/bob_artifacts/s1-decision-triage-rigorous-code-review.html
```
`docs/audit` was added as a new directory but is absent from
`allowed_directories`.

### Fix

**Step 1 — Remove the two stale artifact paths from the manifest hash table.**  
They are tracked at lines 24–25 of `docs/alignment_baseline.json`. These files
are present under `bob_sessions/Jesse/bob_artifacts/` (their canonical home),
so removing the root-level duplicates does not lose content.

**Step 2 — Add `docs/audit` to `allowed_directories`.**  
Append `"docs/audit"` to the `allowed_directories` array (currently lines
146–179).

**Step 3 — Verify the guard passes.**
```bash
python3 tools/check_structure.py
# Expected: "passed": true
python3 -m unittest discover -s tools
# Expected: 45/45 pass
```

**Diff sketch:**
```diff
--- a/docs/alignment_baseline.json
+++ b/docs/alignment_baseline.json
@@ upstream.files_lf_sha256
-    "bob_sessions/bob_artifacts/roclint-code-review-security-correctness-findings.html": "...",
-    "bob_sessions/bob_artifacts/s1-decision-triage-rigorous-code-review.html": "...",

@@ allowed_directories
+    "docs/audit",
```

**Ongoing rule:** Any new directory added by a PR must also add its path to
`allowed_directories` in the same commit, and the PR checklist must include
`python3 tools/check_structure.py` returning `"passed": true`.

---

## R-02 — Wire `julia-requirements-directml.txt` into the build system

**Weakness:** The new optional DirectML requirements file exists on disk but
`CMakeLists.txt` does not copy it to the runtime output directory. Windows AMD
users who follow a setup script will not find it.

**Risk:** Low. Build-system-only change, no C++ or Python logic.  
**Effort:** ~15 min.

### Fix

Add `julia-requirements-directml.txt` to both `copy_if_different` blocks in
[`CMakeLists.txt`](../../CMakeLists.txt) (the `POST_BUILD` command at line 326
and the `roclint_runtime_files` target at line 337).

**Diff:**
```diff
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -326,6 +326,7 @@ add_custom_command(TARGET roclint_host POST_BUILD
             COMMAND ${CMAKE_COMMAND} -E copy_if_different
                 "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia_worker.py"
                 "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements.txt"
+                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements-directml.txt"
                 "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json"
                 "${CMAKE_CURRENT_BINARY_DIR}/generated/granite-runtime-config.json"
                 "$<TARGET_FILE_DIR:roclint_host>/runtime"

@@ -337,6 +337,7 @@ add_custom_target(roclint_runtime_files ALL
             COMMAND ${CMAKE_COMMAND} -E copy_if_different
                 "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia_worker.py"
                 "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements.txt"
+                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements-directml.txt"
                 "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json"
                 "${CMAKE_CURRENT_BINARY_DIR}/generated/granite-runtime-config.json"
                 "$<TARGET_FILE_DIR:roclint_host>/runtime"
```

**Test assertion:** After build, confirm
`$BUILD/runtime/julia-requirements-directml.txt` exists.

---

## R-03 — Surface `JULIA_DEVICE` and `JULIA_CPU_THREADS` to the host and UI

**Weakness:** The allowed values for `JULIA_DEVICE` (`auto`, `cpu`, `cuda`,
`mps`, `directml`) and the `JULIA_CPU_THREADS` knob exist only as validation
strings inside `julia_worker.py`. Non-technical users have no way to configure
or diagnose these without reading Python source.

**Risk:** Low–Medium. Adds read/write paths through `JuliaRuntime.cpp` and
`HostController`; no new protocol messages needed (these are process environment
variables, not IPC fields).  
**Effort:** ~2–3 hours.

### Implementation plan

#### 3a — Expose a `juliaDevice()` getter and `setJuliaDevice()` setter on `JuliaRuntime`

`JuliaRuntime::startWorker()` already builds `QProcess` arguments. Add:

```cpp
// JuliaRuntime.hpp — new members
[[nodiscard]] QString juliaDevice() const { return juliaDevice_; }
void setJuliaDevice(const QString& device);   // validated before store
void setJuliaCpuThreads(int threads);          // 1..256; 0 means auto

// JuliaRuntime.cpp — startWorker() before process->start()
if (!juliaDevice_.isEmpty())
    process->setProcessEnvironment(env with JULIA_DEVICE=juliaDevice_);
if (juliaCpuThreads_ > 0)
    process->setProcessEnvironment(env with JULIA_CPU_THREADS=...);
```

Valid `juliaDevice_` values: `{"auto","cpu","cuda","mps","directml"}`. Reject
anything else with a `fail()` before the process starts so the UI sees
`"FAILED"` with a descriptive reason rather than a Python traceback.

#### 3b — Persist the setting through `HostController`

Add `Q_PROPERTY(QString juliaDevice ...)` to `HostController`. Store the value
in `QSettings` under `julia/device` so it survives restarts.

#### 3c — Show device status in the UI

The `executionDevice_` field is already populated after the hello handshake
([`JuliaRuntime.hpp:39`](../../app/host/JuliaRuntime.hpp:39)). Wire it to a
QML label in the settings drawer (wherever the Julia model directory is
currently shown). No new IPC traffic — just expose the existing field.

#### 3d — Add a user-visible tooltip/label for CPU fallback
When `executionDevice_ == "cpu"` on Windows, the existing advisory state
message (added in F-03) is correct but appears only in a transient status bar.
Pin it as a persistent chip or warning label in the Julia settings section so
the user sees it without knowing to open the log.

**Test assertions:**
```
WHEN: setJuliaDevice("invalid") called
THEN: fail() fires before process starts; state == "FAILED"

WHEN: setJuliaDevice("directml") and worker reports julia.directml.fp16.v2
THEN: executionDevice_ == "directml"; QSettings persists "directml"

WHEN: app restarts
THEN: juliaDevice_ restored from QSettings; passed to next startWorker()
```

---

## R-04 — Add IPC queue-full backpressure signal

**Weakness:** `JuliaRuntime::send()` silently returns `0` when
`pending_.size() >= 8`. Callers that receive `0` have no user-visible feedback
and no way to distinguish "accepted" from "queue full".

**Risk:** Low. One-line change to `send()`, plus a new signal.  
**Effort:** ~45 min.

### Root cause (exact location)

[`JuliaRuntime.cpp:245`](../../app/host/JuliaRuntime.cpp:245):
```cpp
if (!process_ || pending_.size() >= 8 || nextId_ >= kMaxId) return 0;
```

### Implementation plan

#### 4a — Distinguish the three `0` return cases

```cpp
// Return 0 for "not running" (existing behaviour — caller checks isReady()).
if (!process_ || nextId_ >= kMaxId) return 0;
// Return 0 but emit a new signal for "queue full".
if (pending_.size() >= 8) {
    emit evaluationFailed(0, QStringLiteral("Julia's request queue is full; retry when scoring completes."));
    return 0;
}
```

`evaluationFailed(0, ...)` uses request ID 0 as the sentinel for
queue-full (0 is never a valid issued ID since `nextId_` starts at 0 and is
pre-incremented). Callers that check `id != 0` before storing the result will
discard it correctly; callers that display the error signal will surface it.

#### 4b — Add `queueFull()` signal (optional cleaner alternative)

If distinguishing queue-full from evaluation-error is important in the UI, add:
```cpp
signals:
    void requestQueueFull();  // emitted when a new request is dropped
```
and emit it instead of `evaluationFailed(0, ...)`. This keeps the signal
semantics clean.

**Test assertions:**
```
WHEN: 8 evaluate() requests in pending_, 9th evaluate() called
THEN: return value == 0
      evaluationFailed(0, ...) emitted OR requestQueueFull() emitted
      UI state message updated

WHEN: 7 evaluate() requests in pending_
THEN: 8th evaluate() accepted, returns non-zero ID
```

---

## R-05 — Add a protocol negotiation escape hatch for `TRIAGE_PLAN_VERSION`

**Weakness:** `TRIAGE_PLAN_VERSION = 2` is a hard constant in both
`julia_worker.py` and `JuliaRuntime.cpp`. Any version bump requires a perfectly
coordinated binary + Python worker deployment. A partial upgrade (new host,
old worker, or vice versa) triggers an immediate `fail()` with no graceful path.

**Risk:** Medium. Touches the IPC handshake on both sides.  
**Effort:** ~3–4 hours.

### Implementation plan

The IPC already has a `hello` / `hello_ack` handshake. The worker reports
supported operations in `result.operations`. The fix uses this existing
mechanism rather than adding a new round-trip.

#### 5a — Report supported triage plan versions in `hello_ack`

In `julia_worker.py` `hello` handler, add to the `hello_ack` result:
```python
"triage_plan_versions": [TRIAGE_PLAN_VERSION]   # e.g. [2] today; [2, 3] when v3 ships
```

#### 5b — Host reads and stores the supported version set

In [`JuliaRuntime.cpp`](../../app/host/JuliaRuntime.cpp) `receive()` hello
branch (around line 407):
```cpp
const auto planVersions = result.value(QStringLiteral("triage_plan_versions")).toArray();
// If absent (old worker), default to [2] for backward compatibility.
if (!planVersions.isEmpty()) {
    // Store highest mutually supported version.
    activePlanVersion_ = negotiatedPlanVersion(planVersions, kSupportedPlanVersions);
}
```

Where `kSupportedPlanVersions = {2}` today. When the host supports version 3,
it adds 3 to its own set and negotiates the max common version.

#### 5c — Use the negotiated version in requests and validation

Replace hard-coded `2` in:
- `julia_worker.py:207` — `if request["triage_plan_version"] != TRIAGE_PLAN_VERSION`
  → validate against the version the host declared in the request envelope.
- `JuliaRuntime.cpp` triage result check at line 426 — compare against
  `activePlanVersion_` rather than the literal `2`.

#### 5d — Graceful mismatch handling

If negotiation yields no common version (worker supports only `[3]`, host
supports only `[2]`):
```cpp
if (activePlanVersion_ == 0) {
    fail(QStringLiteral("Julia's triage plan version is incompatible with this host. "
                        "Update the Julia worker or reinstall Roclint."));
    return;
}
```
This is still a hard fail, but it is an *informed* fail with a diagnostic
message rather than a silent version mismatch.

**Test assertions:**
```
WHEN: worker reports triage_plan_versions=[2], host supports [2]
THEN: activePlanVersion_ == 2; triage proceeds normally

WHEN: worker reports triage_plan_versions=[2,3], host supports [2]
THEN: activePlanVersion_ == 2 (highest common); triage proceeds

WHEN: worker reports triage_plan_versions=[3], host supports [2]
THEN: fail() called with descriptive mismatch message

WHEN: old worker sends hello_ack without triage_plan_versions field
THEN: activePlanVersion_ defaults to 2; backward compatible
```

---

## R-06 — Add Granite KV-cache warm reuse for rapid successive rewrites

**Weakness:** `GraniteEngine::generate()` calls `llama_memory_clear()` at the
start of every generation, discarding the entire KV cache. A user who adjusts
one choice and immediately re-rewrites pays full prefill cost (~700–900 ms on
Apple M2) again. There is also no token streaming to the host.

**Risk:** Medium–High. Touches the core inference loop in `GraniteEngine.cpp`
and the IPC protocol between the Granite worker and `NativeGraniteRuntime.cpp`.
Streaming requires a new `partial_result` message type.  
**Effort:** ~1–2 days. **Recommend deferred to a dedicated sprint.**

### Implementation plan (design only — not a single PR)

#### 6a — KV cache reuse for same-session rewrites

The `generate()` call already receives a `payload` containing
`instructions` (the prompt). Add a `cache_key` field (SHA-256 of the rendered
token sequence). On entry:

```cpp
if (lastCacheKey_ == cacheKey && !llama_memory_empty(llama_get_memory(context_.get()))) {
    // KV cache is still valid for this prompt — skip prefill, go straight to sampling.
    skipPrefill = true;
}
```

This requires tracking `lastCacheKey_` and invalidating it on any
`llama_memory_clear()` call (including the explicit clear at generation end,
which must be deferred until the next `generate()` call instead).

Gate: only reuse the cache if `input_tokens` is identical to the previous run.
If the instruction changed at all, clear and re-prefill.

#### 6b — Token streaming

Add a `partial_result` IPC message type from the worker to the host emitted
every N tokens (e.g. N=4). The host forwards it through `NativeGraniteRuntime`
as a progress event. The UI can show a live token stream in the rewrite
preview, reducing perceived latency from 1.5 s to near-zero.

Requires:
- New `partial_result` JSON frame type in `granite_worker.cpp` emit loop
- `NativeGraniteRuntime::receive()` handler for `partial_result`
- New `partialResultReceived(QString chunk)` signal on `NativeGraniteRuntime`
- QML binding to append chunks to the preview text area

---

## Execution Order

| ID | Title | Risk | Effort | Recommended sprint |
|----|-------|------|--------|-------------------|
| R-01 | Repair stale alignment manifest | Low | 30 min | Immediately — unblocks CI |
| R-02 | Wire DirectML requirements into build | Low | 15 min | Same PR as R-01 |
| R-03 | Expose Julia device setting to host/UI | Low–Med | 2–3 hrs | Next sprint |
| R-04 | IPC queue-full backpressure signal | Low | 45 min | Next sprint |
| R-05 | Protocol version negotiation | Medium | 3–4 hrs | Next sprint |
| R-06 | KV cache reuse + token streaming | Med–High | 1–2 days | Dedicated sprint |

R-01 and R-02 should be merged first because R-01 restores the structure guard
(which validates that all new files are registered) and R-02 closes the gap
left by the DirectML audit work. R-03 through R-05 can be done in parallel.
R-06 is a standalone performance sprint.
