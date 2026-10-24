# Upstream structure and Stardust vision

Reviewed October 23, 2026 against the supplied source folders. This is an implementation audit, not a declaration of product completion.

## Structure result

**All 96 original files from `roclint-main/roclint-main` remain at their original relative paths.** Their reference contents match Git commit `ca55398661093ae4c61e3771942e3f1eb0a7ff5d` after CRLF/LF normalization. The working repository also includes the subsequent upstream architecture document from `e3a8aeb44b23f3d57b392393162489c5fe10a43f`.

The original CMake project, C++ libraries, host executable, headers/implementations, QML shells, test layers, handoffs and session materials are retained.

| File comparison with the supplied snapshot | Count |
|---|---:|
| Original paths retained | 96 / 96 |
| Original files unchanged after CRLF/LF normalization | 31 |
| Original files modified in place | 65 |
| Added files, including documentation and tests | 103 |
| Missing original paths | 0 |

Path retention establishes layout continuity. Changes to the 65 original files still require code review; retaining their names does not establish behavioral compatibility. Use `python tools/check_structure.py --details` for the full modified/unchanged/added inventory. Source changes remain concentrated in the original module tree, with these six directory additions:

```text
app/runtime                 native Granite helper, Julia worker, manifests and fixtures
docs                        current implementation, contracts and review evidence
docs/contracts              unchanged copies of the ten source contract cards
include/roclint/core/crypto  shared SHA-256 API
src/core/crypto              shared SHA-256 implementation
tools                       developer setup and read-only checks
```

The original HUD/dashboard shells and controls remain the UI baseline. Clarification, review and runtime settings are separate QML components registered in the existing `qml.qrc`. The original overview and upstream architecture dossier remain unchanged.

### Map the vision onto the upstream layout

The bundle's `curriculum/MODULE_LAYOUT.md` explicitly calls its `/stardust` layout a **planning boundary, not implemented code**. The user's choice is to preserve Roclint's physical structure. Keep the same responsibilities within these existing paths:

| Vision responsibility | Roclint location |
|---|---|
| Authoritative revisions and semantic policy | `include/roclint/{core,analysis,intent,rewrite,review}`, matching `src/` directories |
| SQLite repository, migrations and durable artifacts | `include/roclint/storage`, `src/storage` |
| Resident host, invocation, view model and platform integration | `app/host` |
| Native HUD/dashboard | `app/qml/shell` |
| Model adapters and transport validation | `include/roclint/bridge`, `src/bridge`; asynchronous supervisors in `app/host` |
| Native Granite and Julia inference helpers | `app/runtime` |
| Verification | Existing `tests/{core,analysis,intent,rewrite,review,storage,bridge,viewmodel,support}` and developer tools |

Study, packaging and RAG modules require real implementation before adding their source directories. Empty directories would not close those gaps. Future RAG can extend the existing bridge/storage/host boundaries without moving the upstream application.

## Specification authority and explicit choices

The recorded sources are `START_HERE.md`, the master architecture, decision log, evaluation plan, planning freeze checklist, module layout, future RAG document and all ten contract cards. Their hashes and the original path inventory are in [alignment_baseline.json](alignment_baseline.json). The contract cards take precedence over conflicting reference prose.

- **User choice: Julia.** Local Julia replaces the original Jev-only scoring requirement. This changes the provider, not the rules for explicit intent confirmation, final analysis provenance or validated scores. No production Jev/cloud fallback is configured.
- **Latest user diagram: S1 first.** Julia must classify the frozen prompt before Granite. Below 0.70, pass through the original wording; at or above 0.70, Granite supplies options, the user explicitly chooses, and only Granite generates a rewrite. This supersedes the earlier discovery-first/post-score order. [Routing requirements and evidence](MODEL_ROUTING.md) describe the new live gate, v4 artifacts and open classifier-quality requirements. Correct routing is separate from model accuracy and latency acceptance.
- **User choice: preserve and improve upstream UI and structure.** Keep the existing application boundaries, original HUD/dashboard and Ctrl/Cmd+Shift+L default; saved preferences take precedence.
- **Development exceptions:** explicit external loopback Ollama mode and the separately prepared Julia Python/PyTorch environment. Native pinned Granite/llama.cpp is integrated separately; quantization/configuration freeze and clean-install requirements remain open.
- **Authorship:** the source bundle reserves protected core/storage contracts for human authorship/review. Current code and passing tests cannot certify that authorship requirement. Contributor review and attribution remain necessary; the unchanged contract copies preserve that requirement.

## Implementation against the vision

| Requirement | Current evidence | Status / next work |
|---|---|---|
| Native, resident, clipboard-first workflow; no workspace prerequisite (C05/C09) | Existing host/tray, original QML, explicit Analyze, visible review and copy route | Implemented in the Windows development app; native macOS and assistive-technology validation remain open. |
| Canonical revisions, stale-input guards, SQLite history and invalidated approvals (C01/C02/C06) | Revision state, repository/schema v5, typed payloads and host lifecycle tests | Current-session draft/clarification/rewrite/review/restore commands require the observed version; core mutations advance it and repository writes compare-and-swap it. Artifact/version changes commit together. See [versioned commands](VERSIONED_COMMANDS.md) for scope and verification. |
| Bounded discovery (master architecture/C03/C04) | Prompt and JSON schema cap discovery at three independent groups | Parser limit corrected from eight to three in this audit; regression checks accept one–three and reject four. |
| Actual local classifier, unselected material choices | Julia worker, async supervisor and sealed four-head prompt classification; v4 route artifact | Current live flow classifies first and enforces the 0.70 gate. Older choice/assumption scoring contracts remain readable. Raw model decisions are not calibrated and fail development examples; model quality and 15 ms latency remain open. Scores never confirm intent. See [model routing](MODEL_ROUTING.md). |
| Distinct final analysis artifact (C03), exact source artifact for intent (C06) | Separate `AnalysisSnapshot`; v4 sealed S1 triage plus actual Granite discovery when required; historical score/attempt bindings/order, schema v5 and exact restoration | Implemented. V4 clear routes contain no fabricated Granite attempt, and alternative scoring is explicitly not requested. Intent/rewrite/approval bind the final artifact; v2/v3 history keeps its reader/hash and old pre-score history requires Analyze/review. See [final analysis details](FINAL_ANALYSIS_SNAPSHOT.md). |
| User-confirmed intent, local diff, typed constraints and approval of exact wording (C06) | Intent/proposal/approval types and QML review path | Implemented safeguards with final artifact binding; the remaining semantic/API audit is listed separately. Approval is wording review; it does not authorize executing generated instructions. |
| Frozen local model and bounded helper protocol (C04/C08) | Pinned Granite 4.2 3B/llama.cpp native helper; independent Julia v1 helper; handshake/context/hash validation, cancellation, bounded crash recovery and owned Windows shutdown; lazy first-use load; explicit integrity-checked weight delivery | Partial. Native integration is implemented; the shared 20-second Julia/S1 and Granite deadline is implemented. Full-prompt quality/performance, configuration freeze and release packaging remain open. See [native Granite](NATIVE_GRANITE.md) and [Julia lifecycle](JULIA_LOCAL_SCORER.md). |
| Residency distinct from readiness (C08) | Independent HOT/IDLE_MAPPED/UNLOADED operations; persisted Keep warm/Balanced/Low memory presets; idle policy and Windows memory-resource notifications; busy/queued work guards and explicit GPU full-unload fallback | Policy mechanism implemented. Measure/freeze thresholds and representative recovery/resource costs; macOS pressure integration and native validation remain open. The minimal CPU smoke is not the release benchmark. |
| Study and reproducibility (C07/evaluation plan) | Model-attempt history | Open required evaluation work: participant/session/attempt/preparation hierarchy, immutable assigned condition, exact downstream `ExecutionRequest` bytes/hash/config, fresh downstream contexts, outcomes, export and matched-condition harness. Failed/outage trials must remain in denominators. |
| Installed product on both platforms (C08) | Windows developer build and locally prepared dependencies | Open. Bundled helpers/model delivery, integrity/license ledger, start-at-login choice, clean installation, signing and native Windows/macOS checks. A system Python/PATH dependency is not the packaged product. |
| Local codebase context (C10/future RAG document) | Explicit workspace/file selection and bounded filename catalog | **Planned extension, not implemented RAG.** Add native Tree-sitter symbol extraction, SQLite FTS5 retrieval, session working memory and bounded signature-only local context. No file contents are currently indexed. |

### RAG boundary to preserve

The RAG document labels itself “Planned Extension / Future Architecture.” It requires structural symbols/signatures rather than raw source-body dumps, a context budget of at most 350 tokens, working-set decay/eviction, and graceful empty-context behavior on parser/index failure. Code context must remain local and excluded from Jev and telemetry. Workspace/window-title discovery suggested by that future document conflicts with frozen MVP C09; use explicit user-selected context until that contract is deliberately revised.

## Order of remaining work

The post-scorer artifact and coordinated C06 binding/migration are implemented; verification is recorded in [final analysis details](FINAL_ANALYSIS_SNAPSHOT.md). Remaining work:

1. Continue the semantic/presentation and provider/system-event boundary audit. Bounded material-assumption scoring, durable ordering and current-session command-version enforcement are implemented.
2. Complete model quality/performance and quantization/residency decisions, then deliver the bundled runtime. The native helper, shared group deadline and automatic residency mechanism are implemented.
3. Implement study artifacts/export and the matched-condition evaluation harness; measure model quality, latency, memory and recovery.
4. Complete both-platform packaging/accessibility/clean-install validation. Implement the planned RAG extension within the retained modules with explicit context consent.

These gaps are not resolved by preserving file paths or obtaining a clean Git merge. They also do not require a wholesale rewrite of the original UI or module tree.

## Repeatable checks

From the repository root:

```sh
python tools/check_structure.py
python -m unittest discover -s tools -p test_check_structure.py
ctest --test-dir build -C Release --output-on-failure
python tools/merge_preview.py --fetch
```

To recheck the exact supplied materials on this machine:

```powershell
python tools/check_structure.py --details --baseline-dir 'D:\Stardust\roclint-main\roclint-main' --spec-dir 'D:\Stardust\Stardust_v4_2_with_RAG-20261001T213521Z-1-001\Stardust_v4_2_with_RAG'
```

The structure check reads Git's tracked and non-ignored new source paths. It checks original path retention, reviewed directory/top-level boundaries, C++/QML source registration, preserved contract hashes (normalizing checkout line endings) and optional external source hashes. New directories require an explicit reviewed manifest update. It does not prove semantic compliance, appearance, runtime correctness or future mergeability. The manifest is reviewable reference data, not a substitute for contributor review.

### Current verification, October 23, 2026

- External snapshot/specification check: passed; **96/96 original paths**, 199 current files, all 17 recorded vision sources match. The detailed comparison reports 31 unchanged originals, 65 modified originals, 103 additions and no missing originals.
- **38/38 CTest suites passed** (87.74 seconds), including final scored-analysis provenance, versioned commands, assumption scoring, the original QML keyboard workflow, S1-first routing at 0.70, no-Granite clear delivery, full-prompt coverage and shared Julia/Granite deadlines, persisted residency presets/pressure deferral, loader cancellation, native protocol/recovery and atomic installation. Compiled/source sampler/grammar configuration and rejection of an unbounded diagnostic helper are checked too.
- Windows Release preview build and Qt deployment: passed. The separate output avoided overwriting the earlier app executable; no unrelated process was stopped. Both packaged helper deadline modes negotiated and exited with only deployed application DLLs and System32 on PATH, without loading a model. This is developer deployment evidence, not a clean-install result. **11/11 structure-guard regressions passed**, including runtime/test/tool CMake registration, detailed inventory partitioning and line-ending-independent content comparison in disposable copies.
- Complete-working-tree merge and patch-application preview: **clean for all 167 changed/new files** against freshly fetched upstream `e3a8aeb44b23f3d57b392393162489c5fe10a43f`. This result applies to that revision and source snapshot; repeat before publishing or merging.
- Native Granite integration, the shared 20-second analysis deadline and automatic residency mechanism are implemented. Q4/Q5 development runs through `json0ws.v4` still failed the analysis/semantic gate. The v5 sampler substantially improved throughput but retained semantic failures. The supplied Q8_0 choice is now the host/manifest/tooling default; model acceptance remains separate. The integrated Julia v2 template, automatic CUDA BF16/CPU FP32 profile binding and eight-thread CPU tuning are verified mechanically; twelve reused examples route correctly, while added challenges and several separation/parity/latency gates still fail. See [reproduced calibration evidence](TEAM_CALIBRATION_REPORT.md). Residency freeze, representative quality/performance, study/export, installed packaging/platform validation and actual RAG remain open. See [native runtime evidence](NATIVE_GRANITE.md).
- The user confirmed that no Mac/macOS runner is available; continue Windows implementation. Native macOS verification is explicitly unproven.

Current local evidence: `build/structure-s1-integrated.json`, `build/s1-integrated-structure-tests.log`, `build/s1-integrated-build.log`, `build/s1-integrated-tests.log`, `build/s1-integrated-worker-package.json`, `build/s1-integrated-qt-live.json`, `build/s1-integrated-julia-protocol-profile.json`, `build/s1-integrated-cuda.json`, `build/s1-integrated-cpu-8threads.json`, `build/s1-integrated-challenge.json`, `build/s1-integrated-q8-benchmark.json`, `build/s1-integrated-ui/` and `build/merge-preview/s1-integrated.json`. The [calibration verification](TEAM_CALIBRATION_REPORT.md) contains full probability tables, exact before/after wording, parity/latency data and failed acceptance gates. Source and packaged worker hashes match. The merge report records exact upstream and working-tree identities; repeat it after any further edit. Nothing was published, and the supplied reference folders were not edited.

### Results from the preceding structure audit

- External snapshot/specification check: passed; **96/96 original paths**, 159 current source files, all 17 recorded vision sources match. The six additive directories listed above are the only directory additions to the original snapshot.
- Structure guard regression checks: **8/8 passed**, using disposable copies. Cases include removed original paths, new application directories, missing CMake/QML registration, altered contract content and missing specification sources; checkout line-ending conversion remains accepted.
- Windows Release rebuild: passed. **31/31 CTest suites passed**, including acceptance of up to three discovery groups and rejection of four.
- Complete working-tree merge and patch-application preview: clean against fetched upstream `e3a8aeb44b23f3d57b392393162489c5fe10a43f`. Repeat after further edits or upstream changes.
- Whitespace check: passed outside the verbatim contract copies. The supplied C10 card contains a Markdown hard break (two trailing spaces after its authority line); that source formatting is intentionally preserved and is flagged by an unrestricted `git diff --check` of the complete patch.

Local evidence: `build/structure-vision-check.json`, `build/structure-vision-build.log`, `build/structure-vision-tests.log`, `build/structure-vision-merge.log` and `build/merge-preview/latest.json`. No changes were made to the supplied snapshot/specification folders and nothing was pushed to GitHub.
