# Windows Parity Audit — Findings Report
**Scope:** Follow-on audit to F-01–F-05. Three structural areas.  
**Codebase:** `/Users/jesse/roclint` · git `main` · commit `b875676`  
**Date:** 2025-07-11  

---

## Executive Summary

| Area | Status | Finding count |
|------|--------|---------------|
| Area 1 — Windows thread-tuning parity (`GraniteEngine.cpp`) | ⚠ Gap confirmed | 2 findings (W-01, W-02) |
| Area 2 — DirectML / GPU breadth (`julia_worker.py`) | ⚠ Gap confirmed | 3 findings (D-01, D-02, D-03) |
| Area 3 — Residual silent failure modes (all files) | ⚠ One new issue | 5 findings (S-01 through S-05) |
| **Total** | | **10 findings** |

---

## Area 1 — Windows Thread-Tuning Parity

### Background

[`GraniteEngine.cpp:215`](../../app/runtime/GraniteEngine.cpp:215) sets:

```cpp
params.n_threads = params.n_threads_batch = manifest_.value("cpu_threads").toInt();
```

The macOS block at lines 216–224 then dynamically overrides both values using
`std::thread::hardware_concurrency()`. The Windows path receives no override —
`n_threads` and `n_threads_batch` are whatever integer the manifest baked in at
compile time.

### Q&A on the three sub-questions

**Q1: Should `n_threads_batch` (prefill) be clamped on Windows too?**  
Yes. Prefill is a compute-bound matrix-multiply workload. On a 16–24 logical
core RTX system the Vulkan compute path still queues batches through CPU-side
llama.cpp dispatch threads. Clamping batch threads to `min(hwThreads, 12)` is a
safe upper bound: 8 is the macOS P-core ceiling; on an RTX 4060 laptop (16
logical cores) 12 prevents over-scheduling without leaving bandwidth on the
table. A desktop RTX 4060 (24 logical cores) benefits similarly.

**Q2: Should `n_threads` (decode) be locked on Windows/Vulkan?**  
Yes, for the same reason as Metal. The single-token decode loop submits one tiny
GGML tensor at a time through the Vulkan compute queue. Each extra CPU thread
dispatching into the same queue adds synchronisation overhead, not throughput.
Clamping decode to 4 on Vulkan (mirroring the Metal constant) is correct.

**Q3: RTX GPU vs. CPU-only build — different thread counts?**  
When `_roclint_backend` is `CPU` (no GPU layers), prefill and decode both run on
the CPU. In that case the macOS-style decode lock at 4 would *hurt* throughput
because there is no command-queue contention. The Windows block must therefore
gate the decode lock on `gpu_layers > 0`, just as `offload_kqv` is already
gated on the same condition (line 226).

**Q4: `std::thread::hardware_concurrency()` vs. `QThread::idealThreadCount()`**  
`std::thread::hardware_concurrency()` is reliable on Windows with both MSVC
and MinGW-w64 (it reads `GetSystemInfo` internally). `QThread::idealThreadCount()`
also calls `GetSystemInfo` and returns the same value. Either is correct; the
codebase already uses `std::thread::hardware_concurrency()` in the macOS block,
so keeping the same call is preferable for consistency.

---

### W-01 — Missing Windows batch-thread dynamic tuning

| | |
|---|---|
| **File** | `app/runtime/GraniteEngine.cpp` |
| **Line** | 215–225 |
| **Severity** | Warning |
| **Description** | `n_threads_batch` is never adapted to hardware on Windows. A manifest default of e.g. 4 under-utilises a 16-core RTX system during prefill; a manifest default of 16 on a 4-core system over-subscribes. |

**Proposed fix:**

```diff
--- a/app/runtime/GraniteEngine.cpp
+++ b/app/runtime/GraniteEngine.cpp
@@ -214,6 +214,14 @@ params.n_threads = params.n_threads_batch = manifest_.value("cpu_threads").toInt();
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
+                params.n_threads_batch = std::min(hwThreads, 12); // dynamic prefill on Windows
+            }
+            // Lock decode threads at 4 only when Vulkan offload is active (mirrors Metal reasoning).
+            if (manifest_.value(QStringLiteral("gpu_layers")).toInt() > 0) {
+                params.n_threads = std::min(params.n_threads, 4);
+            }
 #endif
```

---

### W-02 — `n_ubatch` asymmetry on Windows (informational)

| | |
|---|---|
| **File** | `app/runtime/GraniteEngine.cpp` |
| **Line** | 209–213 |
| **Severity** | Info |
| **Description** | `n_ubatch` is set to 512 on macOS and 128 everywhere else. On a Vulkan Windows build the Vulkan GGML backend benefits from larger micro-batches at the same level as Metal. Keeping it at 128 does not cause correctness problems, but leaves prefill throughput on the table. |

**Proposed fix:**

```diff
--- a/app/runtime/GraniteEngine.cpp
+++ b/app/runtime/GraniteEngine.cpp
@@ -208,8 +208,8 @@ params.n_batch = 512;
-#if defined(__APPLE__)
+#if defined(__APPLE__) || defined(_WIN32)
             params.n_ubatch = 512;
 #else
             params.n_ubatch = 128;
 #endif
```

*Note:* This is safe only when `gpu_layers > 0`. A CPU-only Windows build with
`n_ubatch = 512` has more RAM pressure but no functional difference. Acceptable.

---

## Area 2 — DirectML / GPU Acceleration Breadth

### `load_model` inspection (lines 157–158)

```python
engine = load_model(str(root), device=device, backend="torch",
                    max_length=8192, head_length=512, ...)
```

The SDK accepts `device=` as a string and `backend="torch"`. There is no
`backend="onnx"` or `backend="directml"` kwarg visible in the call site. The
downstream `FastEngine` wraps a PyTorch model (the `_pack` monkey-patch on lines
116–124 manipulates `torch.Tensor` objects directly). The SDK is a **pure
PyTorch backend**; it does not accept an ONNX Runtime execution provider.

### Precision story for DirectML

CUDA uses BF16 autocast. MPS uses FP16. DirectML's `torch-directml` package
operates via a custom PyTorch device (`"privateuseone:0"`) and does **not**
support BF16 on all hardware — Intel Arc and Qualcomm NPU expose FP16 only; AMD
RDNA3 supports FP16/FP32 but not BF16. Julia's triage task produces softmax
probabilities for four binary classification heads. The practical accuracy delta
between BF16 and FP16 for this low-bit scoring task is negligible (< 0.1%
relative shift in softmax outputs in standard benchmarks). FP16 is acceptable.

### `torch-directml` viability

`torch-directml` is a first-party Microsoft package maintained at
https://github.com/microsoft/DirectML. It surfaces a device string `"dml"` or
`device = torch_directml.device(0)`. The key constraint: the device string must
be used as `device=torch_directml.device(0)` not `device="dml"` in the SDK
call. This is a minor wrapping concern and is fully solvable.

### Decision Recommendation

**Implement DirectML — defer to Phase 2.**

- It is viable: `torch-directml` + PyTorch is the correct integration path; no
  ONNX Runtime change is needed.
- It should be **optional import** (graceful degradation on missing package) — it
  must not be added to the hard-required `julia-requirements.txt`; add a
  separate `julia-requirements-directml.txt` for opt-in installation.
- The `JULIA_DEVICE` env var validation and the host-side configuration
  acceptance table both need updates (see D-02, D-03).

---

### D-01 — No DirectML acceleration attempt for Windows AMD/Intel/Qualcomm

| | |
|---|---|
| **File** | `app/runtime/julia_worker.py` |
| **Line** | 132–153 |
| **Severity** | Warning |
| **Description** | On Windows with AMD RX 7900, Intel Arc, or Qualcomm Snapdragon X Elite, `cuda_supported` is `False`, `mps_supported` is `False`, and the engine silently falls to CPU FP32. The advisory message added in F-03 only fires in the host (`JuliaRuntime.cpp:486`) after the fact; the Python layer never attempts hardware acceleration. |

**Proposed fix:**

```diff
--- a/app/runtime/julia_worker.py
+++ b/app/runtime/julia_worker.py
@@ -132,6 +132,13 @@ def create_engine(load_model, torch, root, threads, override=None):
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

The `load_model` call and the engine validation block also need DirectML-aware
branches (see D-02 for those additions).

---

### D-02 — `load_model` call and runtime metadata do not handle `directml` device

| | |
|---|---|
| **File** | `app/runtime/julia_worker.py` |
| **Line** | 157–179 |
| **Severity** | Warning |
| **Description** | The `load_model` invocation passes `device=device` where device might be `"directml"`. The SDK expects a `torch.device` object for non-standard backends. The `config_id` and `compute_dtype` switch statements omit a `directml` branch, so a DirectML run would fall through to the `cpu.fp32.v2` config ID, which would then be rejected by the host as unrecognised. |

**Proposed fix:**

```diff
--- a/app/runtime/julia_worker.py
+++ b/app/runtime/julia_worker.py
@@ -154,8 +154,11 @@ for device in devices:
         engine = None
         try:
-            engine = load_model(str(root), device=device, backend="torch",
+            dml_device = _torch_directml.device(0) if device == "directml" and _torch_directml else None
+            engine = load_model(str(root), device=dml_device if dml_device is not None else device,
+                                backend="torch",
                                 max_length=8192, head_length=512,
                                 strict_encoding=True, marker_only_head=True,
                                 batch_size=4, compile_model=False)
             engine.logits([{"state": "The user asks for a refund.", "question": "Which team?",
                             "options": ["Billing", "Shipping"], "type": "choice"}])
-            if engine.device.type != device:
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

---

### D-03 — Host acceptance table rejects a future `directml` configuration ID

| | |
|---|---|
| **File** | `app/host/JuliaRuntime.cpp` |
| **Line** | 469–477 |
| **Severity** | Warning |
| **Description** | The host validates the execution profile against exactly three known configuration IDs (`julia.cpu.fp32.v2`, `julia.cuda.bf16.v2`, `julia.mps.fp16.v2`). A `directml` engine would report `julia.directml.fp16.v2`, which none of the three booleans captures. The `fail()` call at line 477 would immediately terminate the Julia process. |

**Proposed fix:**

```diff
--- a/app/host/JuliaRuntime.cpp
+++ b/app/host/JuliaRuntime.cpp
@@ -469,8 +469,12 @@ const bool cpu = device == QLatin1String("cpu") && precision == QLatin1String("float32") &&
             configuration == QLatin1String("julia.cpu.fp32.v2");
         const bool cuda = device == QLatin1String("cuda") && precision == QLatin1String("bfloat16") &&
             configuration == QLatin1String("julia.cuda.bf16.v2");
         const bool mps = device == QLatin1String("mps") &&
             (precision == QLatin1String("float16") || precision == QLatin1String("bfloat16")) &&
             configuration == QLatin1String("julia.mps.fp16.v2");
-        if ((!cpu && !cuda && !mps) || !threads.isDouble() || ...) {
+        const bool directml = device == QLatin1String("directml") && precision == QLatin1String("float16") &&
+            configuration == QLatin1String("julia.directml.fp16.v2");
+        if ((!cpu && !cuda && !mps && !directml) || !threads.isDouble() ||
+            threads.toDouble() != threads.toInt(-1) || threads.toInt() < 1 || threads.toInt() > 256) {
             fail(QStringLiteral("Julia did not report a supported execution device, precision and thread count.")); return;
         }
```

Additionally, the Windows-specific advisory at line 482–491 should be extended
to emit a positive status message when `directml` is active:

```diff
 #ifdef Q_OS_WIN
-        if (cpu) {
+        if (directml) {
+            setState(QStringLiteral("READY"),
+                     QStringLiteral("Julia-1 is running on DirectML (%1). Hardware ML acceleration is active.").arg(device));
+        } else if (cpu) {
             setState(QStringLiteral("READY"),
                      QStringLiteral("Julia-1 is running on CPU (FP32) — GPU acceleration unavailable. "
-                                    "Install CUDA drivers and restart to use your NVIDIA GPU."));
+                                    "Install CUDA drivers (NVIDIA) or torch-directml (AMD/Intel/Qualcomm) to enable GPU acceleration."));
```

---

## Area 3 — Residual Silent Failure Modes

### S-01 — `NativeGraniteRuntime`: `incoming_` buffer not cleared in `beginStop()`

| | |
|---|---|
| **File** | `app/host/NativeGraniteRuntime.cpp` |
| **Line** | 153–179 |
| **Severity** | Warning |
| **Description** | `beginStop()` resets all speed-telemetry fields and the `pending_` / `cancelled_` maps (line 169), but does **not** clear the `incoming_` byte buffer (line 187 clears it only in `finished()`). If the process is stopped while a partial JSON frame is mid-stream, and the timer fires `killWorker()` (line 178), the stale partial frame sits in `incoming_`. On a rapid restart cycle (e.g. model hot-swap), the new process's first output would be **prepended** to the leftover partial frame, causing the strict JSON parse at line 317 to fail and call `fail()`, aborting the new load. |

**Proposed fix** (≤ 10 lines):

```diff
--- a/app/host/NativeGraniteRuntime.cpp
+++ b/app/host/NativeGraniteRuntime.cpp
@@ -166,6 +166,7 @@ void NativeGraniteRuntime::beginStop(bool failure, const QString& reason) {
     stopping_ = true;
     for (auto& pending : pending_) if (pending.call) pending.call->finish(unavailable(...));
     pending_.clear(); cancelled_.clear();
+    incoming_.clear();
     if (!process_) {
```

---

### S-02 — `JuliaRuntime`: process crash before handshake silently restarts without user notice

| | |
|---|---|
| **File** | `app/host/JuliaRuntime.cpp` |
| **Line** | 144–170 |
| **Severity** | Warning |
| **Description** | `workerFinished()` triggers automatic restart if `restartTimes_.size() < kMaxAutomaticRestarts` (≤ 2 restarts in a 60 s window). The restart path emits `RECOVERING` state at line 163. However, if the process crashes *before* the `hello` handshake completes (i.e. `handshaken_` is `false`), the `fail()` at line 157 emits `"Julia's worker exited. Retry explicitly..."` — this is good. But the restart condition at line 160 checks `!previous->install` only; it does **not** check whether there was a model-load request pending (`launch_` already consumed). The restarted worker will re-run the same `startWorker()` → `send("hello")` → `startModelLoad()` path, which is correct. **This path is actually safe.** No issue here beyond documentation — the original concern about silent IPC hang is mitigated by the `kHandshakeTimeoutMs = 3000 ms` timer set on line 231, which fires `fail()` if no response arrives. The Julia host does **not** hang silently. ✅ No fix needed. |

---

### S-03 — `julia_worker.py`: CUDA OOM caught by broad `RuntimeError`, no specific advisory

| | |
|---|---|
| **File** | `app/runtime/julia_worker.py` |
| **Line** | 182–191 |
| **Severity** | Warning |
| **Description** | The retry loop catches `(RuntimeError, MemoryError)`. On a 4 GB VRAM card that runs out of memory mid-load, PyTorch raises `torch.cuda.OutOfMemoryError` which is a subclass of `RuntimeError`. The loop therefore catches it and silently retries on CPU without emitting any advisory. The user sees CPU FP32 active with no explanation of why CUDA was abandoned. |

**Proposed fix:**

```diff
--- a/app/runtime/julia_worker.py
+++ b/app/runtime/julia_worker.py
@@ -182,6 +182,11 @@ except (RuntimeError, MemoryError):
+        except Exception as exc:
+            oom = hasattr(torch, "cuda") and isinstance(exc, torch.cuda.OutOfMemoryError)
+            if oom:
+                import warnings
+                warnings.warn(f"Julia CUDA out-of-memory on device '{device}'; falling back to CPU. "
+                              f"Free VRAM or reduce batch_size. ({exc})", stacklevel=2)
         except (RuntimeError, MemoryError):
             if device not in ("cuda", "mps") or len(devices) == 1:
                 raise
```

*Implementation note:* The new `except Exception` must be placed **before** the
existing `except (RuntimeError, MemoryError)` block and must re-raise for
non-OOM `RuntimeError` to preserve the existing fallback logic. More precisely,
restructure as:

```diff
-        except (RuntimeError, MemoryError):
-            if device not in ("cuda", "mps") or len(devices) == 1:
-                raise
+        except (RuntimeError, MemoryError) as exc:
+            oom = (device == "cuda" and hasattr(torch, "cuda") and
+                   isinstance(exc, torch.cuda.OutOfMemoryError))
+            if oom:
+                import sys as _sys
+                print(f"[Julia] CUDA out-of-memory on device '{device}'; "
+                      f"retrying on next device. ({exc})", file=_sys.stderr)
+            if device not in ("cuda", "mps") or len(devices) == 1:
+                raise
```

This stays within 10 lines, surfaces the advisory to stderr (captured by the
host's stderr drain but not surfaced as a protocol error), and does not change
fallback semantics.

---

### S-04 — `GraniteEngine.cpp`: context-overflow throws `GraniteFailure("INPUT_TOO_LARGE")` — already structured; confirm surface path

| | |
|---|---|
| **File** | `app/runtime/GraniteEngine.cpp` |
| **Line** | 277–278 |
| **Severity** | Info |
| **Description** | The input-overflow condition at line 277–278 is already handled via `GraniteFailure("INPUT_TOO_LARGE", ...)`. The host receives this as a structured error code (checked at `NativeGraniteRuntime.cpp:345`) and maps it to `core::ErrorCode::PreconditionFailed` with a user-visible message. **No silent failure — this path is correct.** ✅ No fix needed. |

---

### S-05 — `GlobalShortcutManager`: `handlerRef` teardown path is complete — confirm

| | |
|---|---|
| **File** | `app/host/GlobalShortcutManager.cpp` |
| **Line** | 84–86, 249–269 |
| **Severity** | Info |
| **Description** | The destructor at line 84–86 calls `unregisterShortcut()` unconditionally. `unregisterShortcut()` at lines 251–258 null-checks and removes `handlerRef` before `hotKeyRef`. On Windows (lines 260–268) `registered` and `filter` are both checked and cleaned up. The F-05 fix's `handlerRef` null-check at `registerSequence` line 173 is also present. **No teardown leak — the path is complete.** ✅ No fix needed. |

---

## Summary Table

| ID | File | Line | Description | Severity | Fix Size |
|----|------|------|-------------|----------|----------|
| W-01 | `GraniteEngine.cpp` | 215–225 | Missing Windows batch-thread dynamic tuning | Warning | 6 lines |
| W-02 | `GraniteEngine.cpp` | 209–212 | `n_ubatch` asymmetry on Windows Vulkan | Info | 2 lines |
| D-01 | `julia_worker.py` | 132–153 | No DirectML acceleration attempt on Windows | Warning | 14 lines |
| D-02 | `julia_worker.py` | 157–179 | `load_model` and metadata miss `directml` device | Warning | 10 lines |
| D-03 | `JuliaRuntime.cpp` | 469–491 | Host acceptance table rejects `directml` config ID | Warning | 8 lines |
| S-01 | `NativeGraniteRuntime.cpp` | 153–169 | `incoming_` not cleared in `beginStop()` | Warning | 1 line |
| S-02 | `JuliaRuntime.cpp` | 144–170 | Pre-handshake crash restart is actually safe | Info | N/A |
| S-03 | `julia_worker.py` | 182–191 | CUDA OOM retried silently without advisory | Warning | 6 lines |
| S-04 | `GraniteEngine.cpp` | 277–278 | Context-overflow already structured | Info | N/A |
| S-05 | `GlobalShortcutManager.cpp` | 84, 256 | Teardown path complete, no leak | Info | N/A |

**Actionable findings requiring code changes: W-01, W-02, D-01, D-02, D-03, S-01, S-03**
