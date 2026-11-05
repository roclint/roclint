# Roclint Phase 5 - Packaging, Intent, Runtime and QA Execution Report

**Date:** 2026-11-05  
**Repository:** `D:\Stardust\roclint`  
**Branch:** `feat/codebase-decoupling`  
**Baseline:** `738a1e0` (Phase 4 remediation, clean working tree)  
**Environment:** Windows 11, AMD Ryzen 7 7840HS, 16 GB RAM, NVIDIA RTX 4060 Laptop, MSVC 2022 C++20, Qt 6.8.2 (`msvc2022_64`), llama.cpp `da2ee1e`, Vulkan backend.

---

## 1. Executive Summary

| Gate | Result |
|---|---|
| Structure continuity | **PASS** - `94/94` original paths, `0` errors |
| Release build | **PASS** - first-party targets `/W4 /WX` |
| CTest | **PASS** - `43/43`, `57.67 s` |
| Deployed host launch without Qt on `PATH` | **PASS** - HUD process from `build/Release/bin` |
| Model checksum verification | **PASS** - local Julia-1 + Granite Q8_0 |
| Vulkan discovery benchmark | **PASS** - `20/20` samples, `10/10` deterministic, `0` contract rejections, exit `0` |
| KV reuse parity | **PASS** - cold and warm outputs identical on the aligned test |
| Verification probe | **PASS** - `9/9` cases |
| Metal benchmark | **NOT RUN** - no macOS host available |

Phase 5 commit sequence:

| Commit | Purpose |
|---|---|
| `f1d2ff3` | `feat(packaging): automate Qt deployment and model download verification` |
| `0610985` | `feat(intent): implement canonical action/target/property schema and disposition tracking` |
| `be50a8b` | `test(benchmarks): record Vulkan Decision Contract v2 latency and KV reuse profiles` |
| final audit commit | `docs(audit): sign off Phase 5 validation and system continuity` |

No production sampling parameter, Decision Contract version, GBNF strategy, model quantization, or Job Object isolation policy was changed.

---

## 2. Phase 5.1 - Packaging and Developer Onboarding

### 2.1 Self-contained Qt deployment

`CMakeLists.txt` now defines `roclint_deploy_qt` on Windows:

1. Copies `roclint_host.exe` to `$<TARGET_FILE_DIR:roclint_host>/bin`.
2. Copies the owned `runtime/` helpers beside it.
3. Runs `windeployqt` through `tools/deploy_qt.cmake` with:
   - `--qmldir app/qml`
   - Qt Quick/QML/QuickControls/QuickEffects/QuickShapes/QuickLayouts
   - `Qt6Svg` via deployed icon/image plugins
   - no system D3D compiler, no compiler runtime, no software OpenGL
4. Verifies `Qt6Core.dll` and `platforms/qwindows.dll` exist, retrying once on a transient deploy failure.
5. Copies `report-verification-probe.exe` into the deployed bin directory.

`start_roclint.bat` now prefers `build/Release/bin/roclint_host.exe`, stages the deployment when missing, and only falls back to the Qt kit `PATH` if no self-contained build can be produced. It derives all paths from `%~dp0`; the hard-coded developer paths were removed. `launch_roclint.ps1` follows the same deployed-first policy and derives the lock path from `%LOCALAPPDATA%`.

**Verification:**

```text
build/Release/bin/roclint_host.exe --dry-run
```

with `PATH=C:\Windows\System32;C:\Windows;...`, no `QT_PLUGIN_PATH`, and no `QML2_IMPORT_PATH`:

```text
[Roclint Host] Version: 0.1.0
[Roclint Host] Core Version: 0.1.0
[Roclint Host] Host State: RESIDENT
[Roclint Host] Qt Version: 6.8.2
[Roclint Host] Dry-run passed. QML root object verified. Exiting cleanly.
```

`start_roclint.bat` was also launched with Qt removed from `PATH`; the running process path was:

```text
D:\Stardust\roclint\build\Release\bin\roclint_host.exe
```

The deployed tree contains `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Quick.dll`, `Qt6Qml.dll`, `Qt6Svg.dll`, `platforms/qwindows.dll`, the required QML imports, and the owned `runtime/` helpers.

### 2.2 Model first-run and integrity pipeline

`tools/download_all_models.py` is now v3:

- Detects local `models/granite-4.2-3b` and `models/julia-1` before defaulting to AppData.
- Supports both local layouts (`julia-1`/`granite-4.2-3b`) and AppData layouts (`s1`/`s2`).
- Adds `--verify`, which never downloads, never creates a virtual environment, and checks pinned sizes and SHA-256 digests.
- Verifies every file listed in `roclint-julia.json`, not only the primary weights.
- Prints checksum confirmations for each verified file.
- Keeps download, registry update, and explicit-recovery behavior when not in verify mode.

`download_models.bat --verify` was executed against the local installation:

```text
[OK] Julia checksum df853bf7... (models/julia-1/model.safetensors)
[OK] Granite Q8_0 checksum fbe98673... (models/granite-4.2-3b/granite-4.2-3b-Q8_0.gguf)
[OK] All selected model weights are present and match their pinned SHA-256 digests.
```

Exit code: `0`.

### 2.3 Windows shell integration

- The self-contained launch exercised the existing tray, global shortcut registration path, and `WH_MOUSE_LL` outside-click monitor setup.
- The HUD process remained alive after the clean-PATH launch; it was terminated by the test harness after confirmation.
- No elevation prompt or missing-DLL dialog occurred.
- The compiled host and QML tests verify the focus/HUD paths added by the preceding Phase 4 remediation.

---

## 3. Phase 5.2 - Canonical Intent Representation

### 3.1 Typed model

Added `include/roclint/rewrite/CanonicalIntent.hpp`:

```cpp
enum class IntentAction { ADD, REMOVE, MODIFY, CONSTRAIN, PRESERVE };

struct CanonicalIntent {
    IntentAction action;
    std::string target;
    std::string property;
    std::string value;
    std::string disposition;
    std::string condition;
    std::string nodeId;
    analysis::SourceSpan source;
};
```

`condition` holds nested `EXCEPT` / `ONLY_IF` / `WHEN` clauses. `nodeId` is `nid_` plus a deterministic SHA-256 prefix over action, target, property, value, condition and source offsets. `source` is the resolved target span when unique, otherwise the clause span.

`CanonicalIntentExtractor`:

- Extracts explicit action triples from multi-sentence and multi-paragraph prompts.
- Masks double-quoted and single-quoted data so quoted instructions are not executed.
- Treats backticks as code targets, not quoted data.
- Handles negation (`do not modify` -> `PRESERVE`).
- Handles nested conditions (`MUST ... EXCEPT ... ONLY_IF ...`).
- Normalizes time units in values.
- Emits compact JSON for model instructions.

**Implementation note:** the current branch does not contain the historical Tree-sitter codebase-context target. Grounding therefore uses deterministic lexical source tokens (filenames, paths, identifiers, CamelCase, backticked code, headings) plus the canonical action triple. No claim of full AST parsing is made.

### 3.2 Integration

- `PreFlightExtractionReport` now carries `canonicalIntents` alongside `detectedExplicitConflicts`.
- `buildDiscoveryPrompt` includes `canonical_intents` and a grounding instruction when triples exist.
- `buildRewritePrompt` includes `<canonical_intents>...</canonical_intents>` and instructs the model to preserve action, target, property and value.
- `GraniteAdapter` keeps its deterministic conflict short-circuit; canonical triples are available in the same preflight report.

### 3.3 Tests

`tests/rewrite/testStructuredEntityExtractor.cpp` now covers:

- ADD/REMOVE/MODIFY/CONSTRAIN/PRESERVE triples in one prompt.
- `Rename offset to cursor in readPacket` target/value/condition grounding.
- Negation (`Do not modify tests` -> PRESERVE).
- Nested `MUST ... EXCEPT ... ONLY_IF` condition capture.
- Quoted data exclusion (`"Remove the cache."` does not produce a REMOVE intent).
- Multi-paragraph directives and ordered source spans.
- Stable node IDs, deterministic repeated extraction, valid source spans, and JSON shape.

`tests/bridge/test_granite_adapter.cpp` verifies canonical intents in the discovery prompt and the non-empty clarification grammar branches. `tests/rewrite/testRewriteProfiles.cpp` verifies canonical intents in the rewrite prompt.

Measured extraction cost remains sub-millisecond:

```text
StructuredEntityExtractor average = 0.387 ms
Pre-model conflict latency      = 0.228 ms
```

---

## 4. Phase 5.3 - Live Runtime and KV Benchmarks

### 4.1 KV cache correction

The first Phase 5 cold/warm run exposed output drift: reusing all but one prompt token changed the final-token prefill batch from a large chunk to a single token. The reuse policy was corrected to:

- Reuse only complete 512-token prefill chunks.
- Always recompute the same final chunk as a cold prefill.
- Never reuse a partial final chunk.

The pure helper is tested for 512/513/700/1024-token cases in `roclint_native_granite`.

Result for an identical 2621-token prompt:

| Repeat | Reused tokens | Prefill | TTFT | Generation | Output hash |
|---:|---:|---:|---:|---:|---|
| 0 cold | 0 | 760.15 ms | 809.27 ms | 2296.58 ms | identical |
| 1 warm | 2560 | 12.75 ms | 44.07 ms | 1595.99 ms | identical |
| 2 warm | 2560 | 19.04 ms | 45.55 ms | 1652.27 ms | identical |

The aligned policy preserves exact output parity and still removes ~95% of prefill time.

### 4.2 Vulkan full corpus

Command:

```text
python tools/benchmark_granite_backends.py \
  --worker build/Release/roclint_granite_worker.exe \
  --config build/generated/granite-runtime-config.json \
  --contracts-tool build/Release/roclint_granite_contracts.exe \
  --model-dir models/granite-4.2-3b --quantization Q8_0 \
  --temperature 1.0 --repeat 2 --cold-per-case --diagnostic-no-deadlines \
  --backend-label VULKAN
```

Configuration: `granite.vulkan.ctx8192.threads4.gpu99.t1.p095.s42.jinja.json0ws.rejection.review_polish.v8`, Q8_0, seed 42, top-p 0.95, temperature 1.0.

**Aggregate result:**

| Metric | Cold | Warm | Change |
|---|---:|---:|---:|
| Samples | 10 | 10 | - |
| Median TTFT | 819.42 ms | 118.40 ms | **-85.55%** |
| Median prefill | 726.62 ms | 29.73 ms | **-95.91%** |
| Median throughput | - | 33.24 tok/s | - |
| Deterministic fixtures | - | 10/10 | - |
| Contract-valid outputs | - | 20/20 | - |
| Worker failures | - | 0 | - |

Per-fixture medians:

| Fixture | Cold TTFT | Warm TTFT | Cold prefill | Warm prefill | Output tokens | Tok/s |
|---|---:|---:|---:|---:|---:|---:|
| `ambiguous_speed` | 843.16 | 82.96 | 769.89 | 34.86 | 81 | 34.31 |
| `three_groups` | 801.75 | 88.17 | 720.23 | 27.04 | 95 | 36.14 |
| `clear_bounds` | 837.09 | 117.99 | 733.01 | 25.34 | 42 | 33.42 |
| `missing_target` | 869.22 | 71.11 | 826.31 | 40.42 | 64 | 35.52 |
| `conflicting_output` | 1046.63 | 118.81 | 926.60 | 34.12 | 87 | 33.32 |
| `quoted_instructions` | 936.07 | 92.28 | 873.81 | 30.48 | 72 | 30.92 |
| `literal_constraints_GENERIC@1` | 303.80 | 151.35 | 193.41 | 26.85 | 38 | 32.66 |
| `literal_constraints_ANTHROPIC_STRUCTURED@1` | 352.47 | 202.23 | 210.19 | 34.39 | 42 | 33.54 |
| `literal_constraints_OPENAI_STRUCTURED@1` | 384.91 | 162.93 | 246.58 | 28.98 | 40 | 33.21 |
| `literal_constraints_LOCAL_COMPACT@1` | 322.90 | 121.13 | 200.97 | 25.44 | 37 | 33.58 |

The benchmark used explicit `--diagnostic-no-deadlines` so every output completed; these timings are component latency evidence, not production 12 s deadline evidence.

### 4.3 Contract validation

The benchmark now runs `roclint_granite_contracts --validate` on the completed report.

```text
mechanical validation: 20/20 parse-valid
structurally invalid : 0
benchmark exit        : 0
```

This required a grammar correction. The prior grammar allowed `status:"options_generated"` with every typed array empty, while the production parser rejects that payload. The clarification branch is now split into four `oneOf` alternatives, each requiring a non-empty `choice_groups`, `assumptions`, `missing_decisions`, or `conflicts` array. The adapter test asserts all four clarification branches carry the non-empty invariant.

### 4.4 CPU reference delta

A CPU reference run used the same Q8_0 weights with `gpu_layers=0` (CPU FP32 compute) on two representative fixtures:

| Fixture | Vulkan vs CPU | Character similarity |
|---|---|---:|
| `clear_bounds` | exact JSON match | 1.0000 |
| `three_groups` | semantically different clarification path | 0.3577 |

`clear_bounds` is exact. `three_groups` diverges because the GPU and CPU sampling paths chose different valid clarification strategies from the same prompt. The max observed character delta is therefore on `three_groups` at 0.3577 similarity; this is a sampling-path difference, not a parser or transport difference. No full-corpus CPU FP32 run was attempted because a single CPU call took ~76 s.

### 4.5 Metal status

Metal cannot run on this Windows host. The benchmark harness is platform-neutral:

```text
python tools/benchmark_granite_backends.py --backend-label METAL --temperature 1.0 --repeat 2 --cold-per-case
```

It must be run on Apple Silicon with a Metal build before any Vulkan-vs-Metal latency claim is made.

---

## 5. Phase 5.4 - QA Gate

### 5.1 Structure

```text
python tools/check_structure.py --root .
```

Result:

```text
passed: true
original_paths: 94
preserved_original_paths: 94
source_files: 317
additions: 223
errors: []
```

### 5.2 Release build and CTest

```text
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure -j 4
```

Result:

```text
100% tests passed, 0 tests failed out of 43
Total Test time (real) = 57.67 sec
```

Key gates:

| Test | Result |
|---|---|
| `roclint_native_granite` | PASS |
| `roclint_product_alignment` | PASS |
| `roclint_dual_mode` | PASS |
| `roclint_upstream_compatibility` | PASS |
| `roclint_granite_adapter` | PASS |
| `roclint_structured_entity_extractor` | PASS |
| `roclint_rewrite_profiles` | PASS |

The only build warning is the pre-existing llama.cpp `MSB8027` duplicate `llama.cpp` source-name warning from the pinned dependency; no first-party warning was emitted.

### 5.3 Verification probe

`build/Release/bin/report-verification-probe.exe` was built and run:

```text
baseline_conflict|ok=1|transport_calls=0|conflicts=1|sealed=1
equivalent_units|ok=1|transport_calls=1|conflicts=0|sealed=1
independent_dimensions|ok=1|transport_calls=1|conflicts=0|sealed=1
disjoint_mandatory_scope|ok=1|transport_calls=1|conflicts=0|sealed=1
equivalent_permissions|ok=1|transport_calls=1|conflicts=0|sealed=1
quoted_data|ok=1|transport_calls=1|conflicts=0|sealed=1
same_scope_opposite_actions|ok=1|transport_calls=1|conflicts=0|sealed=1
nonadjacent_paragraphs|ok=1|transport_calls=0|conflicts=1|sealed=1
disjoint_imperative_scope|ok=1|transport_calls=1|conflicts=0|sealed=1
```

Exit code: `0`.

---

## 6. Safety Invariants

| Invariant | Status |
|---|---|
| Windows Job Object isolation (`OwnedProcessJob.hpp`) | unchanged |
| Decision Contract v2 hash binding | preserved; empty suggestion keeps legacy hash |
| GBNF `llama_common_rejection_v1` grammar | preserved |
| Grammar whitespace transform | preserved |
| Host-minted IDs and sealed snapshots | preserved |
| Source-span validation / unique-quote grounding | preserved |
| Quoted data treated as data | preserved |
| No automatic approval or delivery | preserved |
| C++20 / MSVC 2022 first-party warnings | clean |
| Production sampling temperature | pinned at 1.0 |

---

## 7. Limitations

1. **Metal not executed** - requires macOS/Apple Silicon; no cross-backend claim is made.
2. The temperature comparison remains 1.0 only for Phase 5 acceptance; the Phase 4 0.7 experiment was rejected after contract-invalid empty outputs.
3. The CPU reference covers two fixtures, not the full corpus, because CPU inference is very slow.
4. Canonical intent grounding uses deterministic lexical source tokens because the current branch has no Tree-sitter target.
5. The canonical-intent accuracy lift from the historical ~26% baseline is not claimed without a held-out model evaluation.

---

## 8. Changed Files

| Area | Files |
|---|---|
| Packaging | `CMakeLists.txt`, `start_roclint.bat`, `launch_roclint.ps1`, `tools/deploy_qt.cmake` |
| Models | `download_models.bat`, `tools/download_all_models.py` |
| Canonical intent | `include/roclint/rewrite/CanonicalIntent.hpp`, `src/rewrite/CanonicalIntentExtractor.cpp` |
| Intent integration | `include/roclint/rewrite/StructuredEntityExtractor.hpp`, `src/rewrite/StructuredEntityExtractor.cpp`, `src/bridge/GraniteAdapter.cpp`, `src/bridge/GraniteRewriteAdapter.cpp` |
| Grammar | `src/bridge/GraniteAdapter.cpp` clarification branches |
| Runtime benchmark | `app/runtime/GraniteEngine.cpp`, `app/runtime/GraniteEngine.hpp`, `app/runtime/GranitePrefixCache.hpp`, `app/runtime/granite_worker.cpp` |
| Benchmark tooling | `tools/benchmark_granite_backends.py`, `tools/check_granite_runtime.py`, `tools/report_verification_probe.cpp` |
| Tests | `tests/rewrite/testStructuredEntityExtractor.cpp`, `tests/rewrite/testRewriteProfiles.cpp`, `tests/bridge/test_granite_adapter.cpp`, `tests/bridge/test_native_granite_runtime.cpp` |
| Evidence | `docs/audit/phase5-benchmark-summary.json`, `docs/audit/phase5_execution_report.md` |

---

## 9. Recommendation

Phase 5 should proceed to commit. The production path remains deterministic and contract-bound, the deployed Windows host is self-contained, model integrity is explicit, canonical triples ground both discovery and rewrite without replacing model decisions, and the KV path now preserves exact output while cutting prefill by ~96% on aligned repeats.

Do not promote: Metal latency claims, full-corpus CPU parity, or a measured canonical-intent accuracy lift until the corresponding evaluation is run.
