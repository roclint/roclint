# Native Granite integration

Development implementation, October 23, 2026. This does not freeze an evaluation build or certify a distributable installation.

## Preserved boundaries

The existing C++ bridge adapters still build and validate analysis/rewrite contracts. `app/host/NativeGraniteRuntime` owns asynchronous helper supervision and supplies their transport. `app/runtime/GraniteEngine` performs actual inference; the production helper contains no scripted model fallback. The original dashboard and settings expose the new route alongside explicit Ollama development mode. Julia now classifies first as S1 under [the current model flow](MODEL_ROUTING.md). The live generative flow requires the pinned native Granite model; Ollama remains a legacy development adapter.

| Component | Pin |
|---|---|
| Model family | `ibm-granite/granite-4.2-3b-GGUF` |
| Model revision | `c40945d71cd90f249a56985e8155551a9188dc30` |
| llama.cpp revision | `da2ee1e7a640445fc562fe5529598cee1917e0f5` |
| Q8_0 (Locked default) | 3,892,651,552 bytes; SHA-256 `fbe986738041418e26de9e123ba740cb654931f85bf572a71bd01f9e6b85e53d` |
| Q4_K_M (Secondary profile) | 2,244,011,552 bytes; SHA-256 `e0406663965846ae22a403456eb826ccce5f450840491f71952f18a7cb78e7d5` |
| Q5_K_M (Secondary profile) | 2,613,634,592 bytes; SHA-256 `982e797413f842329266a00c2a85ce540bb5c1b65ac4646276bfb9d2886c5efe` |

`app/runtime/granite-models.json` is the source manifest. CMake resolves its CPU/Vulkan/Metal profile into the same generated configuration for host and helper. The generated configuration is also copied beside the executable. Current settings are context 8192, input limit 6144, output reserve/limit 2048, four CPU threads, temperature 1.0, top-p 0.95, fixed seed 42 and the GGUF's embedded Jinja template with thinking disabled. Input exceeding the budget fails without truncation. GPU profiles request all layers and reject an unavailable accelerator instead of silently changing the measured configuration. The user-selected default is locked to `granite-4.2-3b-Q8_0.gguf` (`"default_quantization": "Q8_0"`). Q4_K_M and Q5_K_M remain explicit development alternatives. This selection does not certify model quality, release packaging or a frozen evaluation build.

Manifest edits trigger CMake regeneration; the native runtime test compares the compiled identity/candidates/sampling/grammar policy with the source profile. The current configuration ID contains `t1.p095.s42` and ends in `json0ws.rejection.review_polish.v8`, separating the revised discovery/rewrite instruction contracts from the measured v5 sampler configuration. The v5 measurements below retain their original identities and outputs. SDK DEBUG tracing is disabled in the production helper.

## Setup

```sh
python tools/setup_granite.py --runtime-only
cmake -S . -B build -DCMAKE_PREFIX_PATH="<Qt kit>" -DROCLINT_GRANITE_BACKEND=CPU
cmake --build build --config Release --parallel
```

The native dependency is fetched beneath ignored `build/deps`; CMake requires the exact clean revision. Native dependency code uses its supported C++17 mode, while Roclint remains C++20. No upstream dependency source patch is applied. Windows Vulkan builds require a Vulkan SDK; Metal is available only on macOS. Installed Qt libraries/plugins must still be deployed using the matching Qt kit.

Weights download only after explicit action. Dashboard download/repair streams into a temporary `QSaveFile`, checks exact size and SHA-256, and atomically replaces the destination after validation. Cancel or invalid data preserves an existing file. The host releases a mapped candidate before repair. For command-line provisioning:

```sh
python tools/setup_granite.py --model-dir "<model folder>" --fetch-runtime
```

Choose that folder in Settings. The default is the application's local-data `models/granite-4.2-3b` directory (`QStandardPaths::AppLocalDataLocation`). All candidates may coexist. The CLI provisioner supports resuming its own partial download and verifies the complete result; the GUI downloader currently restarts a canceled download. File listing establishes presence/size only; the helper verifies the full hash before loading.

`granite-4.2-3b-Q8_0.gguf` is locked deterministically as the active production default across `app/runtime/granite-models.json`, `app/host/HostController.hpp:776` (`"native-granite:Q8_0"`), `app/host/HostController.cpp:2541`, and `DashboardWindow.qml:1253`. The benchmark runner (`tools/check_granite_runtime.py`) defaults to evaluating `("Q8_0",)`.

The provisioner's `all` selection includes all three candidates. Use `--quantization Q8_0` to download the active production default, or `--quantization Q4_K_M` / `--quantization Q5_K_M` to download secondary candidates.

## Lifecycle and request binding

- The widget starts without loading Granite. First Analyze, first requested rewrite after unload, or explicit Load model warms asynchronously.
- Versioned UTF-8 NDJSON runs over owned `QProcess` pipes. A 3-second handshake precedes loading; load has 180 seconds, analysis 12 seconds, rewrite 60 seconds, shutdown a 2-second grace period. A shared 20-second monotonic analysis budget spans discovery and Julia scoring once the ready model begins discovery. Warm-up remains separately bounded and visibly asynchronous. Scorer timeout retains validated alternatives in a final unranked snapshot; discovery timeout produces no analysis artifact. Late replies cannot replace either outcome. The group ID and budget are recorded with both attempts; retry requires explicit user action.
- Every response must echo the request ID, group, attempt, session, revision, state version, operation, input hash, model and configuration identity. Rewrites additionally bind intent and versioned profile. Frames are limited to 1 MiB and generated text to 64 KiB.
- Editing/configuration changes cancel pending work. Local completion does not wait for an in-progress kernel to acknowledge cancellation; late results are discarded. A queued rewrite resumes only if its observed version, intent, profile and configuration still match.
- Unexpected exits allow at most two restarts per minute, loading only. Semantic work is never automatically replayed. Explicit load/retry starts a new recovery budget. Protocol violations fail visibly.
- On Windows, a private Job Object owns helper descendants and terminates them after the grace period. Helper control input uses available-byte pipe polling independently of native inference. Native macOS lifecycle validation remains open.

Readiness and residency are separate. `HOT` keeps the context/model ready. CPU `IDLE_MAPPED` releases the inference context and retains mmap-backed weights, allowing normal OS page reclamation without a fixed recovery promise or default `mlock`. A GPU profile deliberately falls back from requested `IDLE_MAPPED` to actual `UNLOADED`; it does not claim that CPU mapping reclaims GPU memory. `UNLOADED` releases model/context resources while the lightweight helper may remain.

The host implements `GraniteResidencyPolicyV1` and persists the Settings choice:

| Preset | Idle behavior |
|---|---|
| Keep warm | Retain a recently loaded model; never load a model merely by changing this preference. |
| Balanced (default) | Request mapped idle after 5 minutes; release remaining CPU mappings after 30 minutes. GPU profiles release weights at the first transition. |
| Low memory | Release unused model resources after 30 seconds. |

These are declared engineering defaults, **not a measured release policy or recovery promise**. Final thresholds remain subject to the C08 benchmark. A five-second Qt timer checks idle time and, on Windows, the nonblocking [memory-resource notification API](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-querymemoryresourcenotification). Low-memory notification requests full release once work finishes, including for Keep warm. It does not induce pressure, trim another process or modify system memory settings. Unsupported/failed monitoring is reported as unavailable; macOS currently uses idle thresholds only.

Loading, semantic work, pending analysis/rewrite, downloads and shutdown protect resources from policy transitions. A subsequent explicit Load or semantic request warms asynchronously. A warm request received while a residency transition is pending reuses the owned helper after acknowledgment. Resource choices do not change confirmed intent or prompt versions. Native attempts record the policy version, preset and starting tier alongside the model configuration.

## Evidence and remaining gates

A real Q4 CPU protocol smoke run completed inference, mapped idle, warm, full unload and graceful shutdown. It measured initial load at 36,359 ms, a minimal 53-input/11-output-token generation at 2,803 ms (first token 1,690 ms), mapped recovery at 544 ms and unload at 559 ms. These are local development observations on a Ryzen 7 7840HS, not representative full-prompt latency or a quantization decision. Evidence: ignored `build/native-granite-smoke.log` and its diagnostic log. The GPU build and full adapter contract corpus require separate results.

`roclint_granite_contracts app/runtime/granite-benchmark-fixtures.json` emits exact production adapter instructions/schemas for six analysis fixtures and four rewrite profiles. Literal-only rewrite preparation is explicitly synthetic; it does not claim an executed discovery/scoring path. Mechanical parsing and full output inspection, Q4/Q5 timings, HOT/mapped/unloaded recovery, working set, GPU memory and idle measurements are required before freezing a configuration.

An opt-in probe runs that corpus using installed weights, preserves complete synthetic outputs/configuration and validates them with production core parsers:

```sh
python tools/check_granite_runtime.py --model-dir "<model folder>" --qt-bin "<Qt kit>/bin"
```

It reads the generated build configuration and packaged helper. Override `--worker`, `--contracts-tool`, `--config` and `--output` for another build layout. `--quantization Q4_K_M` restricts a run; by default the locked production candidate Q8_0 runs once. Resource transitions record their actual returned tier, including unsupported GPU mapped-idle fallback. Windows working set is sampled; GPU memory and idle CPU are not measured by this tool. Results remain development evidence and require semantic inspection, repeated runs and both-platform resource benchmarking before a freeze.

For a diagnostic subset, repeat `--case <fixture ID>`. Unknown IDs fail before starting a helper. The report records the selected IDs and that the full corpus was not requested; a filtered run cannot establish the complete quality gate. Numeric `decode_ms` and `sampler_ms` separate inference processing from token selection, including failed calls, without returning partial generated text.

The user temporarily disabled semantic deadlines for investigation. This is now an explicit `--diagnostic-no-deadlines` option on the development probe/helper. It leaves helper semantic calls uncapped while the probe waits at most 300 seconds per call; cancellation and shutdown remain available. The handshake and report declare that diagnostic mode, which is not release-deadline evidence. The desktop host rejects a helper advertising unbounded mode and applies its own normal 12/60-second budgets. Use the matching worker/configuration from the selected output folder, including `build/preview/Release/runtime` when that preview build option is set.

Use `--skip-recovery` for a HOT-only prompt iteration; it explicitly omits C08 recovery measurements. The default performs both recovery transitions. A parsed clarification response is distinct from an approvable rewrite; the synthetic literal-only fixture requires an actual rewrite, and semantic review must still check meaning even when parsing succeeds.

### First full Vulkan baseline (`v1`)

On the local Ryzen 7 7840HS / RTX 4060 Laptop system, both candidates completed load, resource transitions and graceful shutdown. Neither passed the full analysis/quality gate:

| Observation | Q4_K_M | Q5_K_M |
|---|---:|---:|
| Initial load | 33.397 s | 38.458 s |
| Working set immediately after load | 2,375,540,736 bytes | 2,745,376,768 bytes |
| Analysis fixtures exceeding the 12 s operation budget | 4 / 6 | 5 / 6 |
| Recovery after requested mapped idle (actual tier UNLOADED) | 29.779 s | 35.853 s |
| Recovery after full unload | 31.804 s | 34.362 s |
| Graceful shutdown | 1.108 s | 0.775 s |

All four literal-only rewrite profile cases returned clarification rather than an approvable rewrite. Q4 also missed the explicit JSON/Markdown conflict and treated a quoted report instruction as a competing requirement. These observations prompted the versioned instruction changes; they do not justify selecting either candidate. Complete synthetic output, mechanical validation, hashes and resource samples are in ignored `build/native-granite-benchmark-v1.json`; hardware observations are in `build/native-granite-hardware.json`. Working-set samples are not a steady-state/idle-CPU study, and system-wide GPU memory samples do not establish process-specific VRAM.

### Revised HOT fixtures (`compact.v2`)

The revised build passed all 37 regression suites (66.16 s), including compiled/source manifest consistency. The actual packaged helper then ran all ten synthetic cases per candidate with `--skip-recovery`; this run deliberately does not repeat the resource benchmark.

| Observation | Q4_K_M | Q5_K_M |
|---|---:|---:|
| Initial load | 25.239 s | 31.964 s |
| Analysis timeouts | 2 / 6 | 4 / 6 |
| Rewrite responses parsed and structurally bound | 4 / 4 | 4 / 4 |
| Rewrite elapsed range | 2.888–3.192 s | 2.926–3.218 s |
| Graceful shutdown | 0.814 s | 0.837 s |

Full inspection still **fails the semantic gate**. Both candidates missed the explicit output conflict and treated quoted report text as competing instructions. Q4 introduced an unnecessary choice for an exact variable rename; its deletion evidence changed the source text's capitalization and was correctly rejected as ungrounded by core policy. Q5's `OPENAI_STRUCTURED@1` output changed “Do not modify tests” into “ensuring existing tests continue to pass,” weakening the restriction. Both candidates also omitted the target-oriented XML/Markdown structure in the corresponding rewrite fixtures; local validation exposed those layouts for review rather than certifying them. Parsing/binding success is not meaning preservation or approval.

Neither candidate is selected/frozen for release. Further prompt/runtime work and independent semantic evaluation are required; the faster observed Q4 results do not override these failures. Complete inputs/outputs, metadata, hashes, typed failures and validation results are in `build/native-granite-benchmark-v2.json` and its log. Both owned helpers exited normally and were absent after the probe. This is one development run, not a quality estimate or a participant study.

Open release gates: representative model quality/performance and quantization freeze; measured residency thresholds/recovery and macOS pressure integration; participant study and exact downstream execution/export; full dependency/model license ledger; bundled Julia dependencies; clean installations/signing and native Windows/macOS accessibility/lifecycle checks. Native integration does not implement the planned Tree-sitter/FTS5 RAG extension.

### Sampling correction (`compact.v3`)

The [IBM model card at revision `4e9c275e232f10211ec88644d106a1f6a1b60e06`](https://huggingface.co/ibm-granite/granite-4.2-3b/blob/4e9c275e232f10211ec88644d106a1f6a1b60e06/README.md) specifies temperature 1.0 and top-p 0.95 for all serving modes. The earlier greedy configuration did not follow that guidance. The helper now uses grammar-constrained nucleus sampling with those parameters and a declared seed of 42 for reproducibility. There is no automatic retry, seed search or selection of a favorable output. Each operation creates a fresh sampler and inference context state; GPU floating-point behavior may still differ across hardware/builds.

The manifest pins the model-card reference. Model-attempt detail records temperature, top-p, seed and reference alongside model/runtime/configuration/template identity. The adapter instructions and fixture corpus remain unchanged from v2 for the first comparison. Previous greedy results remain valid observations of those configurations; they cannot establish quality for the corrected sampler or decide a release quantization.

The v3 HOT run passed all 37 regression suites (69.24 s), but five of six analysis cases per candidate exceeded the operation budget. Q4 still missed the explicit output conflict; Q5 asked for clarification about it. All four rewrites per candidate parsed, but the structured profiles still omitted their XML/Markdown sections. This does not pass the quality gate. Q4 loaded in 28.693 s and Q5 in 38.223 s; both shut down gracefully in under 0.9 s. The Q5 phase overlapped a separate project's GPU verifier starting at 12:12:53 local time; those timings are contended development observations, unsuitable for an isolated performance comparison. The user chose to leave that verifier running. Evidence: `build/native-granite-benchmark-v3.json` and its log.

### Compact JSON envelope and diagnostics (`json0ws.v4`)

The pinned JSON-schema converter permits optional whitespace outside strings. A narrow, versioned grammar transform replaces only its exact `space` rule with an empty rule. String characters, escapes, schema properties and adapter instructions remain intact. An absent, duplicate or changed converter rule fails closed. This bounds format overhead without repairing returned text or altering spaces inside user-visible wording. The development probe independently rejects successful responses that violate the declared compact format.

Failures now report token counts, prefill completion, first-token time and generation time without exposing partial generated text. The first-token measurement begins at prompt decoding, after grammar/template preparation; it is not end-to-end UI latency. Load metadata records the actual selected device and description, included in subsequent attempt configuration details.

The Windows Release build and all **38 regression suites passed (70.11 s)** before the shared-deadline changes; the shared-deadline build then passed all **38 suites (76.54 s)**. Checks cover the exact grammar transformation, preserved string/escape rules, incompatible converter rejection, probe format validation, compiled/source manifest agreement and deadline/cancel/late-result/retry behavior across discovery and Julia.

After the separate verifier ended on its own, both candidates completed a single v4 HOT run. Load metadata confirmed `Vulkan0`, **NVIDIA GeForce RTX 4060 Laptop GPU**. Four of six analysis cases per candidate timed out. Their first tokens arrived in 0.49–0.61 s with prefill completed; roughly 100–130 output tokens were emitted before the 12-second deadline. This identifies slow generation rather than load/prefill as the immediate timing problem. All eight successful rewrite outputs and both successful analysis outputs per candidate complied with the compact envelope check; failed calls returned metrics without partial text.

Quality still fails: Q4 missed the explicit output conflict, both candidates treated explicitly quoted report instructions as needing clarification, and Q5's OpenAI-profile rewrite again weakened “Do not modify tests” to keeping tests passing. Both structured profiles still omitted their requested section format. Initial loads were 40.256 s (Q4) and 36.826 s (Q5); graceful exits took 1.016 s and 1.118 s. Evidence: `build/native-granite-benchmark-v4.json` and its log. This run omitted recovery measurements and is not a calibrated quality estimate. Neither quantization is selected or frozen.

### Grammar sampling investigation (`rejection.v5`)

A subsequent Q4 diagnostic subset ran after the temporary deadline removal. `ambiguous_speed` completed in 13.781 s: **12.433 s in sampling/grammar and 1.297 s in decoding**, with 195 output tokens. The literal rewrite spent 2.440 s in sampling and 0.305 s in decoding. This isolates full-vocabulary grammar application as the main cost in that run. The analysis still failed grounding (four evidence strings did not exactly quote the input). Completing a response beyond 12 seconds in diagnostic mode does not pass the normal operation budget or the semantic gate.

Evidence: `build/native-granite-phase-profile-v4.json`; its matching worker/configuration were preserved under ignored `build/native-profile-v4`. This was a two-case diagnostic subset, not the frozen corpus.

The v5 implementation uses the pinned library's [`common_sampler` grammar-rejection path](https://github.com/ggml-org/llama.cpp/blob/da2ee1e7a640445fc562fe5529598cee1917e0f5/common/sampling.h). It checks the sampled token against the same grammar and applies a full-vocabulary mask only when the proposed token is invalid. Each accepted token advances the grammar exactly once. The chain explicitly contains top-p, temperature and the seeded distribution, without default top-k/min-p or penalty samplers. This changes the random stream and conditional sampling order relative to v4, so its quality and timings require a fresh comparison under the new configuration ID. It is token sampling inside one operation, not a replay of a model request or a favorable-output search.


### Integrated Q8_0 smoke (`balanced.v6`)

`build/s1-integrated-q8-benchmark.json` records one serialized run of all ten synthetic component cases under normal 12-second analysis / 60-second rewrite deadlines. Model pin, exact size/hash, template and runtime identities matched. Load took 52.348 s; the six analysis calls took 1.323–8.935 s; four rewrites took 1.086–1.228 s. Shutdown was graceful in 1.269 s. All ten responses passed transport/JSON validation, and all four rewrite envelopes were structurally valid. The XML and Markdown examples produced literal tags and an actual newline, respectively, while retaining the test-modification prohibition.

**Semantic quality is not accepted.** Explicitly contradictory output requirements were returned as `no_ambiguity`. Missing-target analysis offered unsupported scope such as whole-directory deletion/secure erase and required manual review for overlap. Quoted-data analysis invented output-format instructions and had two ungrounded excerpts. The speed example offered correctness degradation despite an ordinary refactoring request. Mechanical success is not semantic or release acceptance.

This is a Granite component check, not a complete real-model S1/user-interaction study. Recovery measurements were explicitly skipped. Baseline GPU activity was 15% with 2,145 MiB in use on the RTX 4060 Laptop GPU (`build/s1-q8-gpu-baseline.csv` and process listing); other applications were not stopped, so the run is not certified uncontended. The Q8_0 default is retained while these quality gates remain open.
