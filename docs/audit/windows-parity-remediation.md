# Windows Parity Audit — Remediation Plan
**Depends on:** `docs/audit/windows-parity-findings.md`  
**Codebase:** `/Users/jesse/roclint` · git `main` · commit `b875676`  
**Date:** 2025-07-11  

---

## Overview

Seven findings require code changes. They are grouped into three phases ordered
by risk and deployment dependency.

| Phase | Title | Findings | Est. lines changed | Risk |
|-------|-------|----------|--------------------|------|
| 1 | Thread-tuning parity | W-01, W-02 | ~12 | Low — pure C++, no behavioural change on macOS/CPU builds |
| 2 | DirectML / GPU breadth | D-01, D-02, D-03 | ~55 | Medium — new Python dependency, new host protocol branch |
| 3 | Residual silent failures | S-01, S-03 | ~8 | Low — defensive resets, advisory-only stderr |

---

## Phase 1 — Thread-Tuning Parity (C++ only)

### Scope
File: [`app/runtime/GraniteEngine.cpp`](../../app/runtime/GraniteEngine.cpp)  
Lines changed: ~12  
No manifest changes, no Python changes, no IPC protocol changes.

### Changes

#### 1a — Add Windows dynamic thread block (W-01)

Location: after the `#endif` of the existing `#if defined(__APPLE__)` block
at line 225.

```diff
--- a/app/runtime/GraniteEngine.cpp
+++ b/app/runtime/GraniteEngine.cpp
@@ -208,6 +208,8 @@ params.n_batch = 512;
-#if defined(__APPLE__)
+#if defined(__APPLE__) || defined(_WIN32)
             params.n_ubatch = 512;
 #else
             params.n_ubatch = 128;
 #endif
             params.n_seq_max = 1;
             params.n_threads = params.n_threads_batch = manifest_.value(QStringLiteral("cpu_threads")).toInt();
 #if defined(__APPLE__)
             const auto hwThreads = static_cast<int>(std::thread::hardware_concurrency());
             if (hwThreads > params.n_threads) {
                 params.n_threads_batch = std::min(hwThreads, 8);  // dynamic prefill on macOS
             }
             // Lock decode threads at 4 to prevent Metal command queue starvation.
             params.n_threads = std::min(params.n_threads, 4);
+#elif defined(_WIN32)
+            const auto hwThreads = static_cast<int>(std::thread::hardware_concurrency());
+            if (hwThreads > params.n_threads) {
+                // RTX/AMD/Intel systems benefit from filling all logical cores during prefill.
+                params.n_threads_batch = std::min(hwThreads, 12);
+            }
+            // Lock decode threads at 4 when Vulkan offload is active: the single-token decode
+            // loop is GPU-bound; extra CPU threads only contend the Vulkan compute queue.
+            if (manifest_.value(QStringLiteral("gpu_layers")).toInt() > 0) {
+                params.n_threads = std::min(params.n_threads, 4);
+            }
 #endif
```

*Combined with W-02 (the `n_ubatch` hunk above), this is a single coherent diff
applied to lines 208–225.*

### Test assertions for Phase 1

Add to the Granite worker unit tests (or a new `GraniteThreadTuningTest`):

```
WHEN: compiled with _WIN32 defined, gpu_layers > 0, hwConcurrency = 16
THEN: params.n_threads == 4
      params.n_threads_batch == 12

WHEN: compiled with _WIN32 defined, gpu_layers == 0, hwConcurrency = 16
THEN: params.n_threads == <manifest value>  (not clamped)
      params.n_threads_batch == 12

WHEN: compiled with _WIN32 defined, hwConcurrency = 4 (manifest = 8)
THEN: params.n_threads_batch == 8  (not upgraded; hwThreads <= n_threads)

WHEN: compiled without _WIN32 (macOS or Linux)
THEN: macOS block is unchanged; Linux receives no override (existing behaviour)
```

The existing macOS thread-tuning tests (if present) must continue to pass
unmodified.

---

## Phase 2 — DirectML / GPU Acceleration Breadth

### Scope
Files changed:
- [`app/runtime/julia_worker.py`](../../app/runtime/julia_worker.py) — D-01, D-02
- [`app/host/JuliaRuntime.cpp`](../../app/host/JuliaRuntime.cpp) — D-03
- *(New file)* `app/runtime/julia-requirements-directml.txt` — optional pip manifest

Lines changed: ~55

### Dependency strategy

`torch-directml` must **not** be added to `app/runtime/julia-requirements.txt`
(the hard requirement file that is pip-installed for all users). It should live
in a new optional manifest:

```
# app/runtime/julia-requirements-directml.txt
# Install this file in addition to julia-requirements.txt to enable
# DirectML acceleration for AMD, Intel, and Qualcomm GPU users on Windows.
torch-directml>=0.2.5.dev240914
```

Installation is an explicit user or CI step (`pip install -r
julia-requirements-directml.txt`), not the default build path.

### Changes

#### 2a — Device detection and list construction (D-01)

Location: [`app/runtime/julia_worker.py:132–153`](../../app/runtime/julia_worker.py:132)

```diff
--- a/app/runtime/julia_worker.py
+++ b/app/runtime/julia_worker.py
@@ -132,6 +132,7 @@ def create_engine(load_model, torch, root, threads, override=None):
     """The pinned SDK uses BF16 autocast on CUDA and FP16 on Apple MPS; unsupported devices use CPU FP32."""
-    if override not in (None, "auto", "cpu", "cuda", "mps"):
+    if override not in (None, "auto", "cpu", "cuda", "mps", "directml"):
         raise ValueError("JULIA_DEVICE must be auto, cpu, cuda or mps")
     cuda_supported = torch.cuda.is_available() and torch.cuda.is_bf16_supported()
     mps_supported = hasattr(torch, "backends") and hasattr(torch.backends, "mps") and torch.backends.mps.is_available()
+    directml_supported = False
+    _torch_directml = None
+    try:
+        import torch_directml  # optional; not in julia-requirements.txt
+        if torch_directml.is_available():
+            directml_supported = True
+            _torch_directml = torch_directml
+    except ImportError:
+        pass
+    if override == "directml" and not directml_supported:
+        raise ValueError("Explicit DirectML requires torch-directml to be installed and available")
     if override == "cuda" and not cuda_supported:
         raise ValueError("Explicit CUDA requires BF16 support in the pinned Julia runtime")
     if override == "mps" and not mps_supported:
         raise ValueError("Explicit MPS requires Apple Metal support in the pinned Julia runtime")
     if override == "cuda":
         devices = ["cuda"]
     elif override == "mps":
         devices = ["mps"]
+    elif override == "directml":
+        devices = ["directml"]
     elif override == "cpu":
         devices = ["cpu"]
     elif cuda_supported:
         devices = ["cuda", "cpu"]
+    elif directml_supported:
+        devices = ["directml", "cpu"]
     elif mps_supported:
         devices = ["mps", "cpu"]
     else:
         devices = ["cpu"]
```

#### 2b — `load_model` call and metadata (D-02)

Location: [`app/runtime/julia_worker.py:154–179`](../../app/runtime/julia_worker.py:154)

```diff
--- a/app/runtime/julia_worker.py
+++ b/app/runtime/julia_worker.py
@@ -154,10 +154,14 @@ for device in devices:
         engine = None
         try:
-            engine = load_model(str(root), device=device, backend="torch",
+            dml_device = _torch_directml.device(0) if device == "directml" and _torch_directml else None
+            engine = load_model(
+                str(root),
+                device=dml_device if dml_device is not None else device,
+                backend="torch",
                                 max_length=8192, head_length=512,
                                 strict_encoding=True, marker_only_head=True,
                                 batch_size=4, compile_model=False)
             engine.logits([{"state": "The user asks for a refund.", "question": "Which team?",
                             "options": ["Billing", "Shipping"], "type": "choice"}])
-            if engine.device.type != device:
+            # torch-directml exposes its device as type "privateuseone" internally.
+            effective_device_type = "privateuseone" if device == "directml" else device
+            if engine.device.type != effective_device_type:
                 raise RuntimeError("Julia loaded on an unexpected device")
             if device == "cpu" and next(engine.model.parameters()).dtype != torch.float32:
                 raise RuntimeError("Julia CPU execution requires FP32 weights")
             config_id = (
                 "julia.cuda.bf16.v2" if device == "cuda"
                 else "julia.mps.fp16.v2" if device == "mps"
+                else "julia.directml.fp16.v2" if device == "directml"
                 else "julia.cpu.fp32.v2"
             )
             compute_dtype = (
                 "bfloat16" if device == "cuda"
                 else "float16" if device == "mps"
+                else "float16" if device == "directml"
                 else "float32"
             )
```

Also update the `roclint_runtime` dict to record `"device": "directml"` (the
logical name, not `"privateuseone"`) for protocol clarity. The `device` variable
already holds `"directml"` at that point, so no additional change is needed in
the `engine.roclint_runtime` assignment block.

#### 2c — Host acceptance table and status advisory (D-03)

Location: [`app/host/JuliaRuntime.cpp:469–491`](../../app/host/JuliaRuntime.cpp:469)

```diff
--- a/app/host/JuliaRuntime.cpp
+++ b/app/host/JuliaRuntime.cpp
@@ -469,6 +469,10 @@ const bool mps = device == QLatin1String("mps") &&
             (precision == QLatin1String("float16") || precision == QLatin1String("bfloat16")) &&
             configuration == QLatin1String("julia.mps.fp16.v2");
-        if ((!cpu && !cuda && !mps) || !threads.isDouble() ||
+        const bool directml = device == QLatin1String("directml") &&
+            precision == QLatin1String("float16") &&
+            configuration == QLatin1String("julia.directml.fp16.v2");
+        if ((!cpu && !cuda && !mps && !directml) || !threads.isDouble() ||
             threads.toDouble() != threads.toInt(-1) || threads.toInt() < 1 || threads.toInt() > 256) {
             fail(QStringLiteral("Julia did not report a supported execution device, precision and thread count.")); return;
         }
         executionConfigurationId_ = configuration; executionDevice_ = device; executionPrecision_ = precision;
         executionCpuThreads_ = threads.toInt();
         ready_ = true;
 #ifdef Q_OS_WIN
-        if (cpu) {
+        if (directml) {
+            setState(QStringLiteral("READY"),
+                     QStringLiteral("Julia-1 is running on DirectML (%1). "
+                                    "Hardware ML acceleration is active.").arg(precision));
+        } else if (cpu) {
             setState(QStringLiteral("READY"),
                      QStringLiteral("Julia-1 is running on CPU (FP32) — GPU acceleration unavailable. "
-                                    "Install CUDA drivers and restart to use your NVIDIA GPU."));
+                                    "Install CUDA drivers (NVIDIA) or torch-directml (AMD/Intel/Qualcomm) "
+                                    "to enable GPU acceleration."));
         } else {
```

### Test assertions for Phase 2

**`julia_worker.py` unit tests** (mock `load_model`):

```
WHEN: torch-directml unavailable (ImportError)
THEN: directml_supported == False
      devices == ["cpu"] (if no CUDA/MPS)

WHEN: torch-directml available, override=None, cuda_supported=False, mps_supported=False
THEN: devices == ["directml", "cpu"]

WHEN: override="directml", torch-directml unavailable
THEN: ValueError("Explicit DirectML requires torch-directml...")

WHEN: load_model succeeds on "directml" device (mock engine.device.type="privateuseone")
THEN: config_id == "julia.directml.fp16.v2"
      compute_dtype == "float16"
      engine.roclint_runtime["device"] == "directml"

WHEN: load_model called with dml_device (torch_directml.device(0) mock)
THEN: SDK receives torch.device object, not string "directml"
```

**`JuliaRuntime` host tests**:

```
WHEN: hello-ack received with device="directml", precision="float16",
      configuration_id="julia.directml.fp16.v2", cpu_threads=4
THEN: ready_ == true
      executionDevice_ == "directml"
      state message contains "DirectML"

WHEN: hello-ack received with device="directml", precision="bfloat16"
THEN: fail() called ("Julia did not report a supported execution device")
```

---

## Phase 3 — Residual Silent Failure Modes

### Scope
Files changed:
- [`app/host/NativeGraniteRuntime.cpp`](../../app/host/NativeGraniteRuntime.cpp) — S-01
- [`app/runtime/julia_worker.py`](../../app/runtime/julia_worker.py) — S-03

Lines changed: ~8

### Changes

#### 3a — Clear `incoming_` in `beginStop()` (S-01)

Location: [`app/host/NativeGraniteRuntime.cpp:167`](../../app/host/NativeGraniteRuntime.cpp:167)

```diff
--- a/app/host/NativeGraniteRuntime.cpp
+++ b/app/host/NativeGraniteRuntime.cpp
@@ -167,6 +167,7 @@ void NativeGraniteRuntime::beginStop(bool failure, const QString& reason) {
     stopping_ = true;
     for (auto& pending : pending_) if (pending.call) pending.call->finish(unavailable(...));
     pending_.clear(); cancelled_.clear();
+    incoming_.clear();  // Discard any partial frame so a rapid restart does not prepend stale bytes.
     if (!process_) {
```

#### 3b — CUDA OOM advisory before silent retry (S-03)

Location: [`app/runtime/julia_worker.py:182`](../../app/runtime/julia_worker.py:182)

```diff
--- a/app/runtime/julia_worker.py
+++ b/app/runtime/julia_worker.py
@@ -182,6 +182,12 @@ for device in devices:
-        except (RuntimeError, MemoryError):
-            if device not in ("cuda", "mps") or len(devices) == 1:
-                raise
+        except (RuntimeError, MemoryError) as exc:
+            oom = (device == "cuda" and hasattr(torch, "cuda") and
+                   isinstance(exc, torch.cuda.OutOfMemoryError))
+            if oom:
+                print(f"[Julia] CUDA out-of-memory ('{device}'); retrying on next device. ({exc})",
+                      file=sys.stderr)
+            if device not in ("cuda", "mps", "directml") or len(devices) == 1:
+                raise
```

*Note:* `"directml"` is added to the fallback guard in the same hunk so that
a Phase-2 DirectML OOM also triggers a retry rather than a hard raise. Without
Phase 2 this is a no-op.

### Test assertions for Phase 3

**`NativeGraniteRuntime` tests**:

```
WHEN: beginStop() called while incoming_ holds a partial JSON frame ("{"abc")
THEN: incoming_.isEmpty() == true after beginStop()

WHEN: rapid stop → startWorker() → readOutput() cycle
THEN: first output frame from new process parses correctly (no prefix corruption)
```

**`julia_worker.py` unit tests**:

```
WHEN: load_model raises torch.cuda.OutOfMemoryError on device "cuda", devices=["cuda","cpu"]
THEN: stderr contains "[Julia] CUDA out-of-memory"
      engine retried on "cpu" successfully
      no RuntimeError propagated to caller

WHEN: load_model raises torch.cuda.OutOfMemoryError on device "cuda", devices=["cuda"]
      (i.e. override="cuda" explicit single-device)
THEN: OutOfMemoryError propagates (len(devices) == 1 guard)
```

---

## Execution Order and Merge Strategy

```
Phase 1  →  isolated PR "feat(engine): windows-thread-tuning-parity"
             Files: app/runtime/GraniteEngine.cpp only
             Review: confirm constants (12, 4) are acceptable for the target SKUs

Phase 2  →  isolated PR "feat(julia): directml-acceleration-windows"
             Files: app/runtime/julia_worker.py
                    app/host/JuliaRuntime.cpp
                    app/runtime/julia-requirements-directml.txt (new)
             Review: validate torch-directml device wrapping with a real DML device
             Gate: integration test on an AMD or Intel Windows CI runner

Phase 3  →  can merge with either Phase 1 or Phase 2, or as its own micro-PR
             "fix(runtime): defensive-state-resets"
             Files: app/host/NativeGraniteRuntime.cpp
                    app/runtime/julia_worker.py
             Risk: minimal — defensive resets and advisory prints only
```

---

## Total Change Estimate

| Phase | Files | Net lines added | Net lines removed |
|-------|-------|-----------------|-------------------|
| 1 | 1 | 10 | 2 |
| 2 | 3 (+1 new) | 48 | 5 |
| 3 | 2 | 8 | 2 |
| **Total** | **5 (+1 new)** | **66** | **9** |
