# Julia local classification and historical scoring

The user's October 22, 2026 request to use Julia takes precedence over the original Jev-only scoring plan in the unchanged Stardust v4.2 contract cards. The desktop host now defaults to `JULIA_LOCAL`. Jev remains an explicit alternative and is unconfigured in this build.

## What Julia does

The current live order is **Julia/S1 classification → original if score < 0.70, otherwise Granite choices → explicit choice → Granite rewrite**. See [the model-routing contract](MODEL_ROUTING.md). Julia evaluates four independent Boolean heads on the raw frozen prompt; the C++ core derives the gate as their maximum. No Granite call is needed for a clear route. Scores do not confirm user intent and are not calibrated probabilities.

The older post-discovery Choice/Noul scoring APIs remain available for historical contracts and tests, but the current route does not run them after Granite. Version 4 artifacts say `NOT_REQUESTED`, rather than fabricating per-alternative confidence or reporting an outage.

Readiness requires loading and warming the actual pinned model. The upstream ONNX adapter's fixed probabilities are not used. The integrated v2 template reproduces all twelve tuned development routing decisions, while the twenty added challenge prompts still produce three routing false positives and eight missed ambiguities. Observed p50 was 52.4 ms on CUDA BF16 and 175.4 ms on CPU FP32 with eight threads. Accuracy/separation and the supplied CPU latency target remain open. See the [reproduced tables and wording](TEAM_CALIBRATION_REPORT.md).

## Use in a development checkout

Create an environment with `python tools/setup_julia.py --build-dir build`, or choose an existing compatible Python environment. The default setup is isolated; reusing system packages requires the explicit `--reuse-system-packages` flag. Both single-configuration and multi-configuration build layouts are searched.

Build the `roclint_host` CMake target and run `build/Release/roclint_host.exe`.

- Settings → Models & Rewrite → **Julia runtime**.
- **Load Julia** loads the existing checkpoint. Startup also loads it when Julia is selected.
- **Download / repair** explicitly fetches the pinned checkpoint, verifies it and loads it. Loading and scoring otherwise use local files only.
- **Model folder…** selects a directory installed by this helper, containing `roclint-julia.json`.
- **Python…** selects an environment with `app/runtime/julia-requirements.txt` installed.
- Choose a separately installed analysis/rewrite model below, then analyze a prompt in the widget. Julia classifies before Granite is requested. Review the displayed route and explicitly confirm any Granite choice.

Prepared local paths:

```text
Python: D:\Stardust\roclint\build\runtime\julia-python\Scripts\python.exe
Model:  C:\Users\kirby\AppData\Local\roclint\Roclint\models\julia-1
```

The Python environment isolates `transformers==5.0.0` while reusing this machine's installed PyTorch and other dependencies. It is a development environment, not a redistributable installation. The helper is copied beside the executable under `runtime/` during each host build. Runtime lookup first tries an application-local Python installation, then the development environment, then PATH. Settings preserve explicit Python and model-directory choices.

## Model identity and process boundary

The [official Julia native runtime](https://huggingface.co/SupersonicLabs/Julia-1) supports local CPU/CUDA inference using the original PyTorch checkpoint. The browser ONNX/WebGPU bundle is not used by this implementation.

```text
Model:    SupersonicLabs/Julia-1
Revision: a85b127321d580d65176c89ced8273f305745d85
Weights:  df853bf7fe424420011f3d0c47a05d7341aa9eefa7fb9f203ea4aada4ad95b72
Format:   FP32 safetensors
License:  Apache-2.0 (model repository metadata)
```

The explicit download pins the repository revision and writes a per-file SHA-256 manifest. Each load checks that manifest and independently verifies the pinned weight digest before importing the local model implementation. The checkpoint remains outside Git.

One resident Python child process communicates using bounded NDJSON over stdin/stdout. Model imports, loading and inference are outside the Qt GUI process. Prompts travel only through that local process pipe; worker diagnostic output is drained without being saved. Results carry snapshot, group and alternative IDs, checkpoint identity and latency. The host validates binding, probability coverage/ranges/sums and the winning ID before using scores. Per-stage attempt history records Julia identity separately from Granite.

Limits: 256 KiB protocol frames, 64 KiB prompt input, at most three material groups with two to three alternatives each and four material-assumption checks, strict model token budgets, eight outstanding protocol operations, 180-second load / 600-second installation / 60-second scoring deadlines. Triage has an eight-second operation limit. Every classifier head receives the complete frozen text; lossless-context overflow fails without a clear route and leaves the original available for explicit delivery. Quit stops the owned Julia process.

## Versioned helper lifecycle

Julia uses NDJSON protocol v1. The host requires a compatible `hello_ack` within three seconds before sending load/install/scoring work. Its `evaluate` operation is the user's local Julia replacement for the original cloud scoring operation; this does not implement the Granite transport.

Every operation/result carries `protocol_version`, string `request_id`, `request_group_id`, `attempt_id`, `session_id`, `draft_revision_id`, `state_version`, `input_hash`, provider/model/revision/weight identity and `configuration_id`. Scoring uses the discovery attempt as its request-group identity, a separately recorded scoring attempt, and the exact frozen prompt's SHA-256. The current policy is `julia.auto.bf16-fp32.v2`. Load results declare `julia.cuda.bf16.v2` or `julia.cpu.fp32.v2` plus the actual thread count; classification results must match the loaded profile, which is sealed into the final artifact. CUDA BF16 is selected only when supported, with CPU FP32 fallback on automatic GPU warm-up failure. Explicit CUDA diagnostics fail rather than silently measuring CPU. CPU threads default to the available core count capped at eight, overridden by `JULIA_CPU_THREADS`. The worker checks the prompt hash; the host compares echoed bindings before accepting results. Unknown/duplicate IDs, incompatible versions/types, malformed UTF-8/JSON and oversized frames fail the helper. Protocol failures are not automatically retried.

The worker reads control messages independently of its bounded, single inference queue. Native imports/inference run on its main thread. On Windows, `PeekNamedPipe` bounds reads to bytes already available so an idle control reader does not hold the CRT stdin lock during native-library initialization. A blocking reader reproduced a real-model import deadlock consistent with [NumPy issue 24290](https://github.com/numpy/numpy/issues/24290); preloading NumPy alone did not resolve subsequent native imports on this development environment. See Microsoft's [pipe API](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-peeknamedpipe) for the selected availability check. The reader polls only while idle with a 10 ms wait; its idle CPU cost remains part of the required resource measurements.

`runtime_status`, `cancel` and `shutdown` can be processed while loading/scoring runs. Cancellation is terminal locally: the controller cancels pending scoring when its pipeline becomes stale, and the supervisor discards late results. A running PyTorch kernel may finish before releasing resources; cancelling does not claim to interrupt a kernel. Cancellation records are bounded to 256; reaching that bound requires reloading the worker rather than forgetting which results must be discarded.

Explicit Quit sends `shutdown` and waits asynchronously for the owned process to exit, with a two-second kill deadline. Windows starts its root process suspended, assigns it to a private kill-on-close Job Object, then resumes it. The timeout and host exit therefore own the venv launcher and its interpreter descendants, without terminating an external Ollama daemon. Job setup failure prevents the helper from running. See [Windows job ownership](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects). Reconfiguration shuts down the previous child before starting its replacement. Hiding the widget leaves the helper resident. Destructor cleanup has a bounded fallback for embeddings that destroy the controller without its normal Quit sequence. The live checker records whether shutdown completed during the grace interval or required terminating its owned job; acknowledgement alone is not proof of process exit.

Unexpected exits allow at most two automatic worker/model-load restarts in a rolling 60-second window. In-flight scoring fails once and is never resubmitted by recovery. Downloads/install operations and protocol failures are not automatically replayed. Explicit Load/retry starts a new recovery budget. Missing dependencies and incompatible helpers remain visible failures with unranked manual operation.

The current Julia load/scoring timeouts are development budgets, not proof of the frozen 20-second whole-analysis deadline. Lazy first-use startup, whole-pipeline request-group deadlines, native Granite supervision and independent residency policy remain release work. Readiness is not an assertion of HOT/IDLE_MAPPED resource behavior.

## Remaining release work

Bundle an isolated Python runtime and compatible native dependencies for clean Windows/macOS installations, collect dependency and model notices, measure resource use and validate score quality on representative prompts. The current implementation makes no latency, calibration, frozen-study-condition or clean-install portability claim. Changes to model/preprocessing must use a new recorded revision/configuration.

Development evidence: the pinned native worker loaded on this machine and completed real choice/Boolean inference. The Windows Release host built successfully and Qt runtime files were deployed beside it. The subsequent upstream integration runs 31 CTest suites, including the restored QML workflow and a test-only child-process fixture covering Julia protocol identity, snapshot rejection and malformed probability degradation. Automated protocol fixtures do not establish model accuracy.

Before Julia edits, the source tree was backed up to `D:\Stardust\backups\roclint-before-julia-20261022-182545.zip`. The supplied contract bundle and downloaded `roclint-main` tree were left intact.

## Durable scoring evidence

The worker evaluates the normalized discovery input. The controller subsequently builds and persists a separate final `AnalysisSnapshot` containing its validated result (or explicit degradation), model/attempt identity and presentation order. Intent and approval bind to that final artifact. History restoration uses the saved scores without starting Julia. See [final analysis and migration](FINAL_ANALYSIS_SNAPSHOT.md).

[Assumption scoring](ASSUMPTION_SCORING.md) describes batch plan v2, the version-3 final artifact, old-history compatibility, explicit unscored items and reproducible offline checks.

## Lifecycle verification, October 23, 2026

- Windows Release build succeeded; **36/36 CTest suites passed** (58.96 seconds). The Julia worker suite includes **13 Python cases** covering envelope/UTF-8/frame/depth validation, handshake ordering, responsive control messages, cancellation, shutdown and error redaction.
- Qt pipe fixtures cover partial/multiple responses, bad identity/context/hash/version/type, unknown/duplicate IDs, malformed/oversized replies, handshake timeout, late cancelled results, crash recovery without scoring replay and the two-restart ceiling. A real Windows child process remains alive before shutdown and is verified terminated by its owner's job after the two-second fallback.
- Controller checks confirm that editing cancels pending Julia scoring, its attempt is marked `DiscardedStale`, no late analysis is restored, draft/history stay intact, and explicit Quit waits for shutdown.
- The offline Python probe used the pinned actual model and current helper hash `7ffb6ea8fabf5b9de377186855f7994cb4581a07c2e645f9ab746aea8721a6e8`. A status response arrived while loading was pending. Both the three-group/four-assumption batch and the assumption-only batch passed binding/probability validation. This run required termination of its owned job after the two-second grace interval.
- An additional opt-in **Qt supervisor + packaged worker + actual model** probe passed: three validated scored groups, two accepted completions, one cancelled request with no accepted late result, and owned runtime shutdown in 1,519 ms. Its model load took 35,826 ms. These are development-machine diagnostics, not reference-hardware latency/residency or model-quality results. The checked source and packaged worker hashes match.
- All **96 original paths** remain in place; recorded external snapshot/specification hashes match. No user history or source reference folders were opened for mutation by these probes.

Run the optional Qt probe after building the compatibility-test target:

```powershell
build/Release/roclint_upstream_compatibility_tests.exe --live-julia '<Julia Python executable>' '<installed model directory>' '<absolute path to build/Release/runtime/julia_worker.py>'
```

It is not registered as an automatic CTest model test and downloads nothing. It uses synthetic prompts, temporary in-memory state and its owned runtime; the normal application database is not used.

Evidence: `build/runtime-protocol-build.log`, `build/runtime-protocol-probe-build.log`, `build/runtime-protocol-tests.log`, `build/runtime-protocol-live.json`, `build/runtime-protocol-live.log`, `build/runtime-protocol-qt-live.log` and `build/runtime-protocol-structure.json`. Earlier diagnostic attempts exposed the native-import deadlock and a shutdown-grace overrun; the recorded passing runs above include the resulting pipe and process-ownership fixes. The pre-change source backup is `D:\Stardust\backups\roclint-before-runtime-protocol-20261023-095704.zip`.

Earlier v1 opt-in Qt/helper evidence is `build/s1-live-qt-julia.json`; [model routing](MODEL_ROUTING.md) records the exact scope and incorrect outputs. Packaged worker SHA-256: `e6162e62f37dfc228634309919cbc3f8aa863dfabc25c798416d9c15cbd2f651`.
