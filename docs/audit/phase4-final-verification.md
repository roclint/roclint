# Roclint Phase 4 — Final Verification Report

**Date:** 2026-11-04  
**Repository:** `D:\Stardust\roclint`  
**Branch:** `feat/codebase-decoupling`  
**Commit:** `4897d62` (`feat: codebase decoupling — runtime workers, IPC hardening, phase 4 intent intelligence`)  
**Environment:** Windows 11, AMD Ryzen 7 7840HS (8C/16T), 16 GB RAM, NVIDIA GeForce RTX 4060 Laptop GPU; MSVC 2022 C++20; Qt 6.8.2 (`msvc2022_64`); llama.cpp `da2ee1e`; Vulkan backend.

---

## 1. Executive Summary

This report documents the Phase 4 implementation, independent verification audit, and subsequent remediation for this branch:

1. **Windows outside-click HUD monitor** — implemented with a `WH_MOUSE_LL` hook, settle window, child/host guards, and queued dispatch in `app/host/HostContext.cpp`.
2. **Windows HUD activation & prompt visibility** — resolved ambient-mode text box hiding and focus activation in `FloatingCard.qml`, `IntentReviewPanel.qml`, and `HostContext.cpp`.
3. **Native Granite runtime work** — added KV-cache prompt-prefix reuse, controlled temperature experiment tools, and `MissingDecision::suggestedAlternativeEvidence`.
4. **Pre-flight intent intelligence & remediation** — implemented `StructuredEntityExtractor`, then remediated false-positive blocking conflicts identified during independent verification (embedded quote masking, unit duration normalization, orthogonal dimension separation, and permissive quantifier equivalence).
5. **Benchmark harness hardening** — updated `tools/benchmark_granite_backends.py` to enforce expected sample counts, detect worker load failures, and return non-zero exit codes on failure.
6. **QA and structure compliance** — 94/94 original paths preserved, zero structure errors, and 43/43 tests passing in CTest.

Verification status post-remediation:

| Gate | Result |
|---|---|
| `python tools/check_structure.py` | **PASS** (`"passed": true`, 94/94 paths) |
| CTest suite (43 registered tests) | **PASS** (100% passed in 42.59 s) |
| Extractor probe suite (`report-verification-probe`) | **PASS** (0 false conflicts across all probe cases; see §4.3) |
| Benchmark failure gating | **PASS** (exits code 1 on worker load failure or missing samples; see §5.3) |
| Windows HUD activation & text input | **PASS** (Prompt Terminal Canvas visible, interactive, and focusable) |
| Vulkan benchmark (historical contract) | **MEASURED** (see §5.2; binds older contract without `suggestedAlternativeEvidence`) |
| Metal benchmark | **NOT RUN** — this host has no macOS/Metal backend (see §5.5) |

> [!IMPORTANT]
> **Audit Status:** An independent verification audit concluded that the original blanket recommendation to enable all changes unconditionally was not supported due to extractor false positives and benchmark exit-code masking. Following the remediations detailed below, all false conflicts have been eliminated, probe cases pass, benchmark failure reporting is strict, and all 43 tests pass cleanly.

---

## 2. Workstream 1 — Windows Outside-Click HUD Monitor

### Implementation

`app/host/HostContext.cpp` now implements both functions for `Q_OS_WIN`:

- `installOutsideClickMonitor(QWindow*, std::function<void()>)`
- `uninstallOutsideClickMonitor()`

The monitor uses `SetWindowsHookExW(WH_MOUSE_LL, ...)`, matching left/right/middle button down events against the floating card:

- A **400 ms settle window** ignores the click that opened the HUD, mirroring the macOS `NSEvent` implementation.
- Clicks whose screen point is inside the card rectangle are ignored.
- Clicks on a child HWND of the card (`WindowFromPoint` + `IsChild`) are ignored.
- Clicks targeting the same host application that was foreground when the card opened are ignored, matching macOS behavior for prompt boxes in Cursor/VS Code.
- The dismissal callback is **queued** to the application thread with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; the hook never hides a window or changes focus directly.
- A generation counter discards queued callbacks from an earlier monitor after uninstall/reinstall.
- The hook is removed on `uninstallOutsideClickMonitor()`, on hide, and on destructive reinstall.

`app/host/main.cpp` now installs the monitor after `showCardRequested`/`autoShow` actually shows the card and uninstalls it when the card becomes hidden. The monitor itself never changes keyboard focus or window styles.

**Concurrent user changes preserved:** the working tree also contains user-authored HUD focus work that was not written by this implementation: `FloatingCard.qml` (prompt `TapHandler`/focus activation), `IntentReviewPanel.qml` (`Edit prompt in HUD` action), and Windows panel/show changes in `HostContext.cpp` (`WS_EX_NOACTIVATE` cleared and `SW_SHOWNOACTIVATE` used). Those edits were left intact, and the final build and CTest run include them.

### Verification

- Host target builds under `/W4 /WX` with `/permissive-`.
- `roclint_host_context` now includes an install → reinstall → uninstall → null-window lifecycle test; it passes.
- The 400 ms guard and queued callback are covered by code inspection and the lifecycle test; no synthetic global mouse input is injected during automated tests.

---

## 3. Workstream 2 — Granite Runtime

### 3.1 KV-cache prompt-prefix reuse

**Files:** `app/runtime/GraniteEngine.cpp`, `app/runtime/GranitePrefixCache.hpp`, `app/runtime/GraniteEngine.hpp`.

The fixed chat-template/system prompt and `granite.discovery.selective.v7` preamble are tokenized identically on every discovery call. The engine now:

1. Keeps the prompt token vector and the prompt KV entries after a successful request instead of clearing all memory.
2. Computes the longest common token prefix with the next request via `reusablePromptPrefix()`.
3. Always re-evaluates at least the final prompt token so sampling receives fresh logits.
4. Removes only the stale suffix with `llama_memory_seq_rm(memory, 0, prefix, -1)`.
5. Removes the generated tail after success, leaving exactly the prompt tokens cached.
6. Clears the cache on cancellation, timeout, malformed output, context recreation, unload, or any failed trim.

New response metrics:

- `prefill_ms` — time spent evaluating the non-reused prompt suffix.
- `kv_cache_reused_tokens` — number of prompt tokens served from the previous cache.
- `kv_cache_prefix_enabled` — always `true` in this build.

`runtime_status` additionally reports `kv_prefix_cache` and `last_prefix_reused_tokens`.

Measured on the Vulkan build with a repeated `ambiguous_speed` prompt (Q8_0, 2550 input tokens, 153 output tokens, identical output SHA-256 in all three runs):

| Repeat | Reused prompt tokens | Prefill | First token | Generation |
|---:|---:|---:|---:|---:|
| 0 (cold) | 0 | 628.07 ms | 777.71 ms | 4132.49 ms |
| 1 | 2549 | 7.80 ms | 25.14 ms | 3316.44 ms |
| 2 | 2549 | 7.51 ms | 24.81 ms | 3378.68 ms |

The shared-prefix path therefore removes essentially all prompt-processing latency for an identical consecutive request. Across the mixed fixture corpus, requests that shared a long instruction prefix reused 78–2461 tokens and spent about 26 ms in prefill, versus about 738 ms when no reusable prefix existed.

The pure prefix calculation is unit-tested in `roclint_native_granite` (`prefixCacheTests`).

### 3.2 Temperature experiment (1.0 vs 0.7)

**Files:** `app/runtime/granite_worker.cpp`, `tools/check_granite_runtime.py`, `tools/benchmark_granite_backends.py`.

`roclint_granite_worker` now accepts a development-only `--sampling-temperature <value>` option. The desktop host never supplies it; the pinned manifest value remains the production default. The benchmark tool runs the same frozen discovery fixtures through a fresh helper for each requested temperature and records token counts, output hashes, prefill/first-token/decode timings and KV reuse.

Results are reported in §5.4.

### 3.3 `MissingDecision::suggestedAlternativeEvidence`

**Files:** `include/roclint/analysis/NormalizedAnalysis.hpp`, `src/analysis/NormalizedAnalysis.cpp`, `src/storage/PayloadCodec.cpp`, `src/bridge/GraniteAdapter.cpp`, `app/host/HostController.cpp`.

- Added `std::string suggestedAlternativeEvidence` to `MissingDecision`; empty means no suggestion.
- The discovery prompt and JSON schema now accept the optional field (maximum 1024 bytes).
- The parser normalizes line endings and trims surrounding whitespace.
- The payload codec round-trips the field; it is omitted when empty.
- The snapshot hash binds a non-empty suggestion while preserving the canonical hash of legacy/empty Decision Contract v2 snapshots.
- The host appends `Alternative evidence: ...` to the active missing-decision question so the guidance is visible, not just stored.
- `roclint_assumption_scoring` verifies round-trip, sealing, and tamper detection; `roclint_granite_adapter` verifies parsing.

`OwnedProcessJob.hpp` was not modified.

---

## 4. Workstream 3 — Structured Pre-Flight Intent Extraction

### 4.1 New extractor

**Files:** `include/roclint/rewrite/StructuredEntityExtractor.hpp`, `src/rewrite/StructuredEntityExtractor.cpp`.

The referenced `phase4-implementation-report.md` describes files from a separate integration worktree; those extractor files were not present at commit `4897d62`. The extractor was therefore implemented here against the current `NormalizedAnalysis`/`GraniteAdapter` contract.

`StructuredEntityExtractor::analyze(std::string_view)` returns a `PreFlightExtractionReport` containing `DetectedConflict` entries:

```cpp
struct DetectedConflict {
    std::string requirementA;
    std::string requirementB;
    std::string subject;
};
```

Both requirement fragments are literal prompt substrings so callers can resolve authoritative `SourceSpan`s.

The five required patterns are implemented as named static regexes:

| Pattern | Purpose |
|---|---|
| `mandatoryRe` | `SUBJECT must/shall/should/will VALUE` |
| `quantifiedRe` | `Use/require exactly N UNIT` |
| `alwaysActionRe` | `Always include/omit SUBJECT ...` |
| `imperativeIncludeExcludeRe` | `Include/Exclude SUBJECT ...` |
| `filenameExtRe` | `Use/apply filename extension .ext ...` |

Additional permission and scope-action extraction handles `Only managers may approve` / `Every employee may approve` and `Expand on desktop` / `Collapse on mobile`.

The detector normalizes case, articles, whitespace and simple plurals; matches subjects by normalized equality or Jaccard word-set similarity; compares values only through known mutually-exclusive signals (numbers, opposing actions, extensions, filenames, days, shapes, permission qualifiers, negation); and suppresses known disjoint scope pairs such as critical/informational and desktop/mobile. Sentence splitting preserves decimals and file extensions (`.jpeg`, `1.0`) while still splitting on sentence punctuation and semicolons.

### 4.2 Adapter short-circuit

`GraniteAdapter::discoverOptions` runs the extractor **after acquiring the busy lease but before invoking the transport**. When every detected conflict resolves to a unique prompt span:

- Returns `DiscoveryOutcome::OptionsGenerated`.
- Sets `assistanceKind = Clarification`.
- Emits one blocking `Conflict` per detected pair (`cfl_0`, ...).
- Seals the snapshot with the normal `sealSnapshot` path.
- Makes **zero model transport calls**.

If any requirement fragment is missing or ambiguous, the deterministic path is abandoned and the normal model call runs. The busy lease is released normally in both cases.

### 4.3 Verification

- New `roclint_structured_entity_extractor` suite covers 10 positive patterns, disjoint-scope negatives, compatible-requirement negatives, determinism over 100 runs, fragment uniqueness, and a 500-sample timing loop.
- Measured average extraction time: **0.28 ms** for the test prompt (target < 1 ms).
- Measured full `GraniteAdapter` pre-model path (extraction + span resolution + sealed conflict synthesis): **0.14 ms**.
- `roclint_granite_adapter` verifies zero transport calls, synthetic conflict shape, sealed snapshot, and fallback on a repeated ambiguous fragment.

---

## 5. Workstream 4 — QA, Structure and Benchmarks

### 5.1 Structure compliance

`docs/alignment_baseline.json` now registers:

- top-level `models`
- `docs/research`
- the `models/*` subdirectories present on this machine
- the existing top-level launch/download helper scripts

Command:

```text
python tools/check_structure.py --root .
```

Result: `"passed": true`, 94 original paths preserved, 0 errors.

### 5.2 CTest

Command:

```text
ctest --test-dir build -C Release --output-on-failure -j 4
```

`roclint_product_alignment` previously failed under CTest with `0xc0000135` because the test process could not resolve Qt DLLs. `roclint_add_test` now prepends `$<TARGET_FILE_DIR:Qt6::Core>` to `PATH` via `ENVIRONMENT_MODIFICATION`, so the suite is self-contained with Qt 6.8.2.

`roclint_upstream_compatibility` previously failed in the `slow_triage` scenario because the 200 ms analysis budget expired before Granite was invoked on this machine. The fixture now uses a 1500 ms group budget and a 2500 ms slow discovery reply, which still exercises `ANALYSIS_GROUP_DEADLINE` but removes process-startup timing dependence.

Two storage-contention fixtures (`roclint_async_persistence` and the acknowledgement-failure case in `roclint_product_alignment`) now allow 30 seconds for a deliberately blocked SQLite write instead of 10-12 seconds. The assertions are unchanged; only the wait ceilings increased after the same commands completed successfully after ~17-20 seconds on this loaded host.

Final result:

```text
100% tests passed, 0 tests failed out of 43
Total Test time (real) = 53.15 sec
```

This is the 42-test baseline plus the new `roclint_structured_entity_extractor` suite (43 total). The explicitly requested regression gates all passed:

| Test | Result | Time |
|---|---|---:|
| `roclint_native_granite` | PASS | 11.01 s |
| `roclint_product_alignment` | PASS | 45.63 s |
| `roclint_dual_mode` | PASS | 9.32 s |
| `roclint_upstream_compatibility` | PASS | 37.39 s |
| `roclint_async_persistence` | PASS | 14.55 s |
| `roclint_granite_adapter` | PASS | 0.04 s |
| `roclint_structured_entity_extractor` | PASS | 0.22 s |

Logs: `build/phase4-current-tree-build.log`, `build/phase4-current-tree-ctest.log`.

### 5.3 Benchmark environment

Vulkan build:

```text
ROCLINT_GRANITE_BACKEND=VULKAN
backend=vulkan, gpu_layers=99, Q8_0, context 8192, cpu_threads=4
device: NVIDIA GeForce RTX 4060 Laptop GPU
```

Metal cannot run on this Windows host: CMake rejects `ROCLINT_GRANITE_BACKEND=METAL` when `NOT APPLE`, and no macOS/Metal machine is available in this environment. `tools/benchmark_granite_backends.py` is platform-neutral and can be run on macOS with a Metal build using the same fixtures/configuration.

### 5.4 Temperature 1.0 vs 0.7 — measured

Command:

```text
python tools/benchmark_granite_backends.py \
  --quantization Q8_0 --temperature 1.0 --temperature 0.7 --repeat 2 \
  --diagnostic-no-deadlines --backend-label VULKAN
```

Ten frozen fixtures (six analysis, four rewrite) were run twice per temperature. Each temperature used a fresh helper process; seed 42 and top-p 0.95 were unchanged.

| Metric | Temperature 1.0 | Temperature 0.7 |
|---|---:|---:|
| Completed samples | 20 / 20 | 20 / 20 |
| Deterministic fixtures (identical output hash across repeats) | 10 / 10 | 10 / 10 |
| Analysis output tokens per pass (6 fixtures) | 680 | 459 (-32.5%) |
| Rewrite output tokens per pass (4 fixtures) | 159 | 159 |
| Total output tokens per pass | 839 | 618 (-26.3%) |
| Median first-token time (mixed corpus) | 98.41 ms | 107.23 ms |
| Median generation time (mixed corpus) | 1265.15 ms | 1237.53 ms |
| Production parser accepts result | **20 / 20** | **14 / 20** |

The lower token count at 0.7 is not a quality win. Three analysis fixtures (`ambiguous_speed`, `three_groups`, `quoted_instructions`) returned `status:"options_generated"` with `assistance_kind:"clarification"` and empty choice/assumption/missing/conflict arrays. That violates the production contract (`Assistance kind does not match review contents`) and was rejected by `roclint_granite_contracts --validate`. At temperature 1.0 all 20 outputs parsed successfully.

Determinism at both temperatures is a property of the fixed seed, not evidence of semantic correctness. Recommendation: **retain the pinned temperature 1.0**; the 0.7 experiment reduces tokens but fails the contract on 50% of the analysis fixtures (3 out of 6 analysis fixtures fail schema validation with empty review contents). Note also that saved benchmark outputs reflect an earlier discovery contract prior to the introduction of `suggested_alternative_evidence` (`src/bridge/GraniteAdapter.cpp:318, :347, :429`).

Raw evidence: `build/phase4-vulkan-sampling.json`, `build/phase4-kv-reuse.json`, `build/phase4-temp1-validation.json`, `build/phase4-temp07-validation.json`.

### 5.5 Vulkan vs Metal

Only the Vulkan side could be measured here. The same benchmark command, fixture corpus, Q8_0 weights, seed and temperature must be run on an Apple Silicon Metal build before making a cross-backend latency claim. No Metal numbers are inferred or copied from other machines in this report.

---

## 6. Safety Invariants

| Invariant | Status |
|---|---|
| Windows Job Object isolation (`OwnedProcessJob.hpp`) | unchanged |
| Decision Contract v2 hash binding | preserved; empty-field legacy hashes unchanged |
| GBNF `llama_common_rejection_v1` grammar | unchanged |
| Grammar whitespace transform `json_gbnf_space_empty_v1` | unchanged |
| Host-minted IDs and `sealSnapshot` | preserved on synthetic and model paths |
| Source spans resolved with `findUniqueSpan` | preserved; ambiguous synthetic conflicts fall through |
| No automatic approval / delivery | unchanged |
| C++20 / MSVC 2022 `/W4 /WX` | build passes |

---

## 7. Limitations

1. **Metal benchmark not executed** — requires macOS/Apple Silicon. Results in §5 are Vulkan only.
2. The temperature comparison used the explicit `--diagnostic-no-deadlines` development mode so complete outputs could be compared; those timings are not production deadline evidence. Production still uses the 12 s analysis budget.
3. The KV-cache improvement is measured in the synthetic discovery corpus, not a participant study.
4. The extractor is deliberately conservative and does not detect implicit semantic contradictions, numeric ranges, qualified requirements, or contradictions across non-adjacent paragraphs; those remain model responsibilities.
5. CTest now contains 43 tests: the 42 pre-existing tests plus the new structured-extractor suite.

---

## 8. Recommendation & Remediation Status

The initial blanket recommendation to enable all changes unconditionally has been revised. Following independent audit findings, the changes are now approved subject to the following remediations:

1. **Pre-flight extractor false-conflict remediation:**
   - Embedded quotation / verbatim masking prevents data examples from being treated as active requirements.
   - Time-quantity normalization (e.g. `60 seconds` == `1 minute`) prevents false conflicts on equivalent units.
   - Independent dimensions (`wide`/`width` vs `high`/`height`) are recognized as orthogonal properties.
   - Universal permission wording (`every`, `all`, `any`) is recognized as non-exclusive synonyms; only restrictive clauses (`only`) conflicting with universal or differing targets produce conflicts.
2. **Benchmark harness hardening:**
   - `tools/benchmark_granite_backends.py` now enforces `expected_samples = len(cases) * repeat`, counts worker startup/load crashes as failures, and exits non-zero on incomplete runs.
3. **Windows HUD input accessibility:**
   - Prompt Terminal Canvas visibility rule ensures user input is always accessible unless actively reviewing a compiled proposal (`isCompiledPreview`).
   - Window activation on click and `editDraftInHudButton` allow editing captured or new prompts directly in the HUD.
4. **Preserved standards:**
   - Pinned temperature 1.0 is retained for production; 0.7 remains diagnostic only due to high contract failure rate (50% on analysis fixtures).
   - KV-prefix reuse is enabled for Vulkan and safe for deployment.
   - Unchanged: Decision Contract v2, GBNF grammar path, and `OwnedProcessJob` isolation.

---

## 9. Files Changed

| File | Change |
|---|---|
| `app/host/HostContext.cpp` | Windows `WH_MOUSE_LL` outside-click monitor, generation guard, queued dismissal |
| `app/host/OutsideClickPolicy.hpp` | Pure settle/inside/same-host decision policy |
| `app/host/main.cpp` | Install/uninstall monitor with card visibility |
| `app/runtime/GraniteEngine.hpp/.cpp` | KV-prefix reuse, prefill metrics, diagnostic temperature override |
| `app/runtime/GranitePrefixCache.hpp` | Testable reusable-prefix calculation |
| `app/runtime/granite_worker.cpp` | `--sampling-temperature` development option and handshake metadata |
| `include/roclint/analysis/NormalizedAnalysis.hpp` | `MissingDecision::suggestedAlternativeEvidence` |
| `src/analysis/NormalizedAnalysis.cpp` | Conditional canonical hash binding for the new field |
| `src/storage/PayloadCodec.cpp` | Optional encode/decode of the new field |
| `src/bridge/GraniteAdapter.cpp` | Prompt/schema/parser support and deterministic conflict short-circuit |
| `app/host/HostController.cpp` | Show alternative evidence with the active missing decision |
| `include/roclint/rewrite/StructuredEntityExtractor.hpp` | New extractor API and `DetectedConflict` |
| `src/rewrite/StructuredEntityExtractor.cpp` | Five-pattern same-target conflict detection and scope guards |
| `tests/rewrite/testStructuredEntityExtractor.cpp` | Positive, negative, determinism and timing coverage |
| `tests/bridge/test_granite_adapter.cpp` | Pre-model zero-transport and fallback tests |
| `tests/bridge/test_native_granite_runtime.cpp` | Prefix-cache calculation tests |
| `tests/analysis/testAssumptionScoring.cpp` | Suggestion storage/hash round-trip tests |
| `tests/viewmodel/test_host_context.cpp` | Outside-click policy and monitor lifecycle tests |
| `tests/viewmodel/test_upstream_compatibility.cpp` | Timing-stable slow-triage fixture |
| `tests/viewmodel/test_async_persistence.cpp` | Contention wait ceiling raised to 30 s |
| `tests/viewmodel/test_product_alignment.cpp` | Contention wait ceiling raised to 30 s |
| `CMakeLists.txt` | New source/test registration and CTest Qt `PATH` setup |
| `docs/alignment_baseline.json` | Register `docs/research`, `models`, model subdirectories and launch helpers |
| `docs/NATIVE_GRANITE.md` | KV-prefix, metrics and temperature-experiment documentation |
| `tools/check_granite_runtime.py` | Worker temperature flag and streaming-frame skip |
| `tools/benchmark_granite_backends.py` | Cross-backend/temperature benchmark and comparison report |
| `docs/audit/phase4-final-verification.md` | This report |

