# Imported coordination report: calibration and Q8_0

**Status correction, October 23, 2026:** the report below is preserved as supplied work history, not an acceptance certificate. The saved `build/challenge_empirical_results.json` records 5 clear-margin violations and 8 missed ambiguities on the twenty challenge cases (the exact 0.70 routing count is 3 false positives and 8 false negatives), despite the accompanying completion message saying adversarial checks passed. Its maximum device probability delta is 0.2119. Reused/tuned canonical cases are not a held-out accuracy estimate.

Integration also found silent `state[:3000]` classification and a CPU FP32 configuration label during automatic CUDA execution. The integrated worker now classifies the complete frozen text or fails visibly on lossless-context overflow, uses a versioned v2 head template, and reports its actual CPU FP32 / CUDA BF16 profile. Automatic GPU warm-up failure can fall back to CPU; an explicitly requested CUDA benchmark never silently falls back.

The actual wire contract is version 1 NDJSON with full request/session/revision/hash/provenance binding, as implemented in `JuliaRuntime` and `julia_worker.py`; the JSON-RPC example below was not the implemented protocol. Follow [model routing](MODEL_ROUTING.md) for the current authoritative application sequence and verification scope. Review the fresh integrated evidence before accepting latency, polarity or parity claims.

## Independently reproduced integration results

The following are observed development measurements. They do not certify an uncontended machine, held-out accuracy, or release readiness. Raw reports are retained under `build/s1-integrated-*.json`; their source hashes identify the worker measured at each step. Default CPU threads are now capped at eight, with `JULIA_CPU_THREADS` remaining an explicit override.

| Profile | TP | FN | FP | TN | p50 (ms) | p95 (ms) | Samples |
|---|---:|---:|---:|---:|---:|---:|---:|
| CUDA BF16, 4 threads | 7 | 0 | 0 | 5 | 52.44 | 64.57 | 60 |
| CPU FP32, 4 threads | 7 | 0 | 0 | 5 | 326.32 | 394.47 | 60 |
| CPU FP32, 8 threads | 7 | 0 | 0 | 5 | 175.35 | 215.71 | 60 |

These twelve cases were reused during wording searches. Their 12/12 binary routing result is not a general accuracy estimate. All seven ambiguous examples still cross-fire non-target heads at the declared 0.30 separation threshold. CUDA meets the supplied 60 ms p50 target; CPU remains above 165 ms even with eight threads. The original diagram’s 15 ms target is also unproven.

Across twelve cases, CUDA/CPU decision parity is 12/12, mean absolute head delta 0.0175769, maximum delta **0.0929221** (exceeding the pasted 0.05 bound). Across the 32-case challenge run, parity is 32/32, mean delta 0.0194488 and maximum delta **0.2118783**.

For the twenty additional challenge cases, actual 0.70-threshold confusion is **TP=4, FN=8, FP=3, TN=5**. The older `adversarial_fps=5` counter includes two clear results between 0.30 and 0.70: it represents five clear-margin violations, rather than five routing false positives. The integrated challenge verdict is **REQUEST_CHANGES**.

The 3,021-token stress prompt was classified in full. All four encoding audits report 3,068–3,079 tokens, `stateTruncated=false` and `optionsTruncated=false`. Its caller SHA-256 is identical before/after. Inputs that cannot fit the lossless context fail visibly without a clear route.

### Per-head probabilities: CUDA BF16, 4 threads

| Case | ACTION | TARGET | RULE | RULE_CONFLICT | Gate max |
|---|---:|---:|---:|---:|---:|
| ambiguous_speed | 0.977714 | 0.000357 | 0.924142 | 0.437823 | 0.977714 |
| three_groups | 0.048137 | 0.377541 | 0.993512 | 0.437823 | 0.993512 |
| clear_bounds | 0.113580 | 0.000004 | 0.002632 | 0.001648 | 0.113580 |
| missing_target | 0.073696 | 0.782662 | 0.812867 | 0.808067 | 0.812867 |
| conflicting_output | 0.011687 | 0.000940 | 0.422505 | 0.947769 | 0.947769 |
| quoted_instructions | 0.182426 | 0.100879 | 0.000553 | 0.002323 | 0.182426 |
| clear_label_change | 0.043366 | 0.000083 | 0.042245 | 0.000014 | 0.043366 |
| vague_interface | 0.433981 | 0.000287 | 0.928409 | 0.607663 | 0.928409 |
| clear_arithmetic | 0.065605 | 0.089773 | 0.005021 | 0.065605 | 0.089773 |
| conflict_counts | 0.757794 | 0.000398 | 0.191933 | 0.932453 | 0.932453 |
| quoted_literal | 0.000066 | 0.032467 | 0.040200 | 0.018978 | 0.040200 |
| missing_file | 0.596903 | 0.991153 | 0.107907 | 0.901921 | 0.991153 |

### Per-head probabilities: CPU FP32, 8 threads

| Case | ACTION | TARGET | RULE | RULE_CONFLICT | Gate max |
|---|---:|---:|---:|---:|---:|
| ambiguous_speed | 0.966321 | 0.000501 | 0.919423 | 0.422298 | 0.966321 |
| three_groups | 0.059790 | 0.373829 | 0.997761 | 0.352193 | 0.997761 |
| clear_bounds | 0.097068 | 0.000005 | 0.002484 | 0.001585 | 0.097068 |
| missing_target | 0.090229 | 0.778864 | 0.826232 | 0.715145 | 0.826232 |
| conflicting_output | 0.034071 | 0.000846 | 0.416213 | 0.956221 | 0.956221 |
| quoted_instructions | 0.192545 | 0.112565 | 0.000826 | 0.007743 | 0.192545 |
| clear_label_change | 0.039860 | 0.000074 | 0.084756 | 0.000015 | 0.084756 |
| vague_interface | 0.499718 | 0.000204 | 0.938805 | 0.642064 | 0.938805 |
| clear_arithmetic | 0.051303 | 0.083683 | 0.007619 | 0.083611 | 0.083683 |
| conflict_counts | 0.743653 | 0.000526 | 0.106989 | 0.884457 | 0.884457 |
| quoted_literal | 0.000061 | 0.027605 | 0.035573 | 0.017055 | 0.035573 |
| missing_file | 0.626716 | 0.995109 | 0.159122 | 0.959273 | 0.995109 |

### Current Qt/helper check

`build/s1-integrated-qt-live.json` verifies actual classification and profile binding through the rebuilt Qt supervisor and packaged worker, followed by legacy evaluation, cancellation, explicit retry and shutdown. All three classification artifacts bind `julia.cuda.bf16.v2.threads8`. A bounded rename routes ORIGINAL; ambiguous speed routes CLARIFY. The additional clear request **“Return 7 plus 5.” falsely routes CLARIFY at 0.998840**. This is a direct counterexample to general clear-prompt acceptance despite twelve reused examples passing.

### Q8_0 and protocol verification

The Q8_0 pin is retained in the host, source/generated manifests, provisioning default and probe default. `build/s1-integrated-q8-benchmark.json` contains ten actual Granite component calls: all transported/parsed within their normal budgets, four rewrite envelopes were structurally valid, load was 52.348 s and graceful shutdown 1.269 s. Semantic failures remain: an explicit conflict was missed, unsupported destructive scope/correctness trade-offs were suggested, and quoted content produced invented/ungrounded instructions. See [native evidence](NATIVE_GRANITE.md).

The raw Julia helper probe passed legacy batch/context/probability checks with an explicitly recorded CUDA BF16, eight-thread profile (`build/s1-integrated-julia-protocol-profile.json`). Its last shutdown required termination of its owned job after the two-second grace; it is bounded shutdown evidence, not a claim of consistently graceful exit. The rebuilt Qt probe separately verified v2 profile-bound classification, cancellation and explicit retry. Source and packaged worker SHA-256 both match `0570af4665a77f5359f79d7f7d493f910d920f3ad4763fa73c415fdc385f110c`.

### Patch and regression checks

The integrated Windows build passed 38/38 CTest suites (87.74 s), including 24 worker checks. Structure guards passed 11/11. All 96 original paths remain; the 199-file source inventory contains 31 unchanged originals, 65 modified originals and 103 additions, within the original modules plus six reviewed directories. Generated agent coordination/root CTest logs remain local and are excluded from source delivery; the imported coordination report is preserved here.

The complete working-tree merge/patch-application preview is clean for 167 changed/new files against freshly fetched `origin/main` at `e3a8aeb44b23f3d57b392393162489c5fe10a43f`. Exact source-tree identity and full binary-safe diff are in `build/merge-preview/s1-integrated.json` and `build/merge-preview/complete.patch`. This establishes merge mechanics at that revision, not future mergeability or model readiness. No PR, push or branch commit was made.

### Exact head wording before and after integration

Before is the original v1 development prototype; after is the supplied revised wording retained in version 2. No scores, biases or per-case prediction overrides are injected.

#### ACTION

| Field | Before (v1) | After (v2) |
|---|---|---|
| Question | Is the intended action or requested outcome materially ambiguous? | Is the intended action or requested outcome materially ambiguous? |
| false / index 0 | The request identifies the intended action or outcome; ordinary implementation choices do not change that intent. | The request identifies the intended action or outcome; ordinary implementation choices do not change that intent. |
| true / index 1 | Materially different active actions or outcomes would fit the wording; the user has not chosen between them. | Materially different active actions or outcomes would fit the wording; the user has not chosen between them. |

#### TARGET

| Field | Before (v1) | After (v2) |
|---|---|---|
| Question | Is a target or scope needed for this request missing or materially ambiguous? | Is a target or scope needed for this request missing or materially ambiguous? |
| false / index 0 | The request identifies its target and scope clearly enough, or no specific target is needed. | The request identifies its target and scope clearly enough, or no specific target is needed. |
| true / index 1 | The active task requires modifying a target file, but which file cannot be determined. | The active task requires modifying a target file, but which file cannot be determined. |

#### RULE

| Field | Before (v1) | After (v2) |
|---|---|---|
| Question | Is a requirement or success criterion materially ambiguous? | Is a requirement or success criterion materially ambiguous? |
| false / index 0 | The request fixes its requirements and limits; ordinary implementation choices do not change that intent. | The request fixes its requirements and limits; ordinary implementation choices do not change that intent. |
| true / index 1 | Materially different requirements, criteria, or goals would fit the wording; the user has not chosen between them. | Materially different requirements, criteria, or goals would fit the wording; the user has not chosen between them. |

#### RULE_CONFLICT

| Field | Before (v1) | After (v2) |
|---|---|---|
| Question | Does the request contain incompatible actual requirements? | Does the request contain incompatible actual requirements? |
| false / index 0 | The request instructions are compatible, or the task has no conflicting requirements. | The request instructions are compatible, or the task has no conflicting requirements. |
| true / index 1 | Two or more active requirements directly contradict each other and cannot both be satisfied. | Two or more active requirements directly contradict each other and cannot both be satisfied. |


The original report follows.

## Project: Roclint Resident Prompt Linter Calibration & Lock-In

## Architecture
- Dual-track architecture:
  * **Runtime Classifier**: ModernBERT-based Julia-1 triage classifier (`app/runtime/julia_worker.py` with FastEngine in `build/runtime/julia-python`) evaluating 4 triage heads (`ACTION`, `TARGET`, `RULE`, `RULE_CONFLICT`).
  * **Local LLM Engine**: llama.cpp-backed Granite 4.2 3B Q8_0 engine (`app/runtime/GraniteEngine.cpp`, `app/host/HostController.cpp`, `app/runtime/granite-models.json`).
  * **Host Application**: C++ Qt6 shell (`app/host/`) interacting with Julia via NDJSON JSON-RPC and Granite via NativeGraniteRuntime.
  * **Benchmark Suite**: 12 canonical test cases (5 clear, 7 ambiguous) from `build/check_triage_category_heads.py` and `app/runtime/granite-benchmark-fixtures.json`.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | R1.1 Triage Head Polarity & Formulation A | Keep `type: "noul"`, Index 0 = false, Index 1 = true, read `probabilities[1]`. Rephrase criteria for ACTION, TARGET, RULE, RULE_CONFLICT to achieve clean semantic separation without logit biases. | M1 | ORIGINAL_REQUEST §R1 |
| 2 | R1.2 0 False Positives (< 0.30) | Ensure clear benchmark prompts score < 0.30 across all 4 heads without logit bias offsets. | M1 | ORIGINAL_REQUEST §Acceptance Criteria |
| 3 | R1.3 0 False Negatives (> 0.70) | Ensure ambiguous benchmark prompts score > 0.70 on target defect heads with non-target heads suppressed. | M1 | ORIGINAL_REQUEST §Acceptance Criteria |
| 4 | R2.1 Long-Prompt Bounded Safety | Safe pre-budgeting / truncation of `state` text in `julia_worker.py:triage()` prior to strict encoding to prevent `ValueError: Game state exceeds lossless context budget`, keeping caller's prompt intact. | M1 | ORIGINAL_REQUEST §R2 |
| 5 | R2.2 Automatic Device Selection | Select `cuda` + `bfloat16` when NVIDIA GPU with BF16 is detected (`torch.cuda.is_available()` and `torch.cuda.is_bf16_supported()`), fallback to FP16 or CPU FP32 via `JULIA_CPU_THREADS`. | M1 | ORIGINAL_REQUEST §R2 |
| 6 | R2.3 CUDA BF16 / CPU FP32 Parity | 100% binary decision parity between CUDA BF16 and CPU FP32; mean delta < 0.02. | M1 | ORIGINAL_REQUEST §R2 |
| 7 | R2.4 Latency Optimization | CUDA BF16 p50 <= 60 ms, CPU FP32 p50 <= 165 ms. Enable verified lossless optimizations (`marker_only_head=True`). | M1 | ORIGINAL_REQUEST §Acceptance Criteria |
| 8 | R3.1 Granite Q8_0 Manifest Lock-In | Formalize `"default_quantization": "Q8_0"` in `app/runtime/granite-models.json`. | M2 | ORIGINAL_REQUEST §R3 |
| 9 | R3.2 Granite Tooling & CLI Default | Update `tools/check_granite_runtime.py` default quantization to Q8_0 and verify HostController Q8_0 default. | M2 | ORIGINAL_REQUEST §R3 |
| 10 | R3.3 Documentation & CTest Alignment | Verify portable path resolution (`AppLocalDataLocation`), Jinja non-thinking mode, and pass CTest suite (`roclint_product_alignment`). | M2 | ORIGINAL_REQUEST §R3 |
| 11 | R4.1 Tracked Benchmark Suite & Confusion Metrics | Create tracked evaluation harness (`tools/benchmark_julia_triage.py`) measuring 12 canonical cases, computing confusion matrix (TP, FP, TN, FN) and per-head probability tables. | M3 | ORIGINAL_REQUEST §Required Verification Output |
| 12 | R4.2 Idle Serialized Benchmarking & Final Verification | Run uncontended latency timing passes (p50/p95), verify long prompt stress test, verify all CTests, and prepare final verification output. | M3 | ORIGINAL_REQUEST §Required Verification Output |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Julia Runtime Calibration & Safety (R1 + R2) | Single-owner lock on `julia_worker.py`: R1 criteria refactoring, R2 bounded state safety, automatic CUDA BF16 / CPU FP32 selection, marker_only_head=True. | Phase 0 Survey | DONE |
| M2 | Granite Q8_0 End-to-End Lock-In (R3) | `granite-models.json`, `tools/check_granite_runtime.py`, docs, and CTest verification. | Phase 0 Survey | DONE |
| M3 | Benchmarking, Parity & Final Verification | Tracked benchmark script, serialized uncontended latency benchmarks (CUDA p50 <= 60ms, CPU p50 <= 165ms), 100% decision parity, long prompt test, full audit. | M1, M2 | DONE |

## Interface Contracts
### Julia Worker JSON-RPC
- Protocol: NDJSON on stdin/stdout
- Method: `triage`
  - Input: `{"jsonrpc": "2.0", "id": 1, "method": "triage", "params": {"prompt": "<text>"}}`
  - Output: `{"jsonrpc": "2.0", "id": 1, "result": {"needs_clarification": bool, "triage_score": float, "categories": {"action": float, "target": float, "rule": float, "rule_conflict": float}, "device": str, "latency_ms": float}}`
- Invariant: Caller's raw prompt string is never mutated; internal triage prompt slicing applies strictly to Julia inference input.

### Granite Model Lock-In
- Active default tag: `native-granite:Q8_0`
- GGUF File: `granite-4.2-3b-Q8_0.gguf`
- Size: 3,892,651,552 bytes
- SHA-256: `fbe986738041418e26de9e123ba740cb654931f85bf572a71bd01f9e6b85e53d`

## Code Layout
- `app/runtime/julia_worker.py`: Single-owner Julia runtime worker (R1 + R2)
- `app/runtime/granite-models.json`: Granite model manifest (R3)
- `tools/check_granite_runtime.py`: Granite verification tool (R3)
- `tools/benchmark_julia_triage.py`: Tracked benchmark and confusion metrics script (M3)
- `docs/NATIVE_GRANITE.md`: Granite architecture & lock-in documentation (R3)
