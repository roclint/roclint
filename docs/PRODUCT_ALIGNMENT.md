# Roclint product alignment

Reference: the user-supplied Stardust v4.2 bundle, reviewed October 22, 2026. The unchanged C01–C10 contract cards are preserved in [contracts/](contracts/README.md), with source hashes in `contracts/SOURCE.json`.

## Product boundary

The MVP is a resident Qt application with a compact keyboard-first widget. Its portable flow is copy → invoke → inspect/edit → analyze → resolve intent → request rewrite → review → approve → copy back. A workspace directory and accessibility permissions are not prerequisites.

**Current user override, October 23, 2026:** Julia/S1 classifies the raw frozen prompt first. Scores below 0.70 preserve the original without Granite. Scores at or above 0.70 request Granite choices, followed by explicit user confirmation and Granite-only rewriting. See [model routing](MODEL_ROUTING.md) for persistence, failure handling and the open model-quality gate. Julia uses real pinned local inference; missing or invalid classification fails visibly rather than supplying a clear score. Earlier post-discovery scoring contracts remain readable for history. The preserved C01–C10 cards record the original plan, not these subsequent user overrides.

Profiles control formatting only. Scope, constraints, permissions, file names and examples must come from the user or explicit confirmed choices; QML must not append them automatically.

The bundle places codebase RAG under `future/12_codebase_context_rag.md` as a planned extension. C10 defines that extension's boundary. Its suggested workspace/window-title discovery conflicts with frozen MVP C09; automatic title/path scraping is not enabled by this alignment work. A future extension needs explicit context consent and must keep code context local.

## Corrections implemented in this pass

| Contract | Correction |
|---|---|
| C05 keyboard | Custom buttons and pills support Tab focus, Enter/Space activation, accessible names/actions and visible focus. Alternative number keys work in the focused clarification group, including the clipboard route. Typing digits and Shift+Enter in the editor continues to edit text. |
| C05 final action | Ctrl/Cmd+Enter analyzes an uncommitted draft, redirects to unresolved clarification, requests a permitted rewrite, or approves a permitted proposal. It cannot silently select an alternative. The Analyze and Copy as typed actions also have visible controls. |
| C05 review | Every constraint state is visible with a textual state label, including mechanically verified and user-confirmed items. Diff text retains non-color change markers. The widget content scrolls at smaller sizes. |
| C09 capture | Captured text is shown before the first analysis. Empty/non-text clipboard capture clears the capture buffer. Replacing an unedited draft with different clipboard text marks the old pipeline stale; an edited in-progress draft is preserved. No clipboard watcher is installed. |
| C09 context | A persistent opt-out disables the one-shot app-category hint. Clearing a captured hint invalidates in-flight analysis using it. Only a category is sent to the local model. |
| C09 invocation | The shortcut is configurable and persisted only after successful OS registration. Invalid/conflicting combinations are reported and the previous shortcut is restored when possible. Repeated invocation toggles the same widget. |
| C09 placement | Actual window dimensions are used to clamp placement, including screens with negative coordinates. |
| C04 profiles | A valid profile is restored from settings and persisted using its canonical versioned ID. Changing it discards the proposal while preserving confirmed intent. |
| C04 model identity | No unselected installation assumes or downloads an 8B model. Users choose an actually installed model. Changing model configuration invalidates the previous pipeline/proposal/approval. Existing explicit selections remain user settings. |
| C04 provenance | Attempt detail records the exact coarse hint category or versioned rewrite profile used, alongside returned model digest/quantization. |
| C08 diagnostics | `--db PATH` supports isolated history. `--dry-run` defaults to an in-memory database, an unavailable accessibility bridge, no tray/hotkey registration, and no model health request. |

The current core/storage types, schema v5, revision/intent/proposal binding, durable approval, and production/test separation are retained. No downloaded source was copied over these implementations.

## Subsequent upstream compatibility pass

The user explicitly requested preservation of `roclint-main` and its original UI. The upstream HUD/dashboard shells, workspace/file-scope controls, default Ctrl/Cmd+Shift+L shortcut and diagnostic flags were restored around the corrected controller. Clarification/review and detailed runtime controls are additive components. Workspace scope is applied only through a visible draft edit; filenames are scanned without reading source content or enabling RAG. The original project overview and latest upstream architecture dossier are preserved unchanged. See [upstream integration](UPSTREAM_INTEGRATION.md) for feature continuity, API changes, current verification and merge evidence. This subsequent pass built successfully and all 31 CTest suites passed on Windows.

## Development runtime exception

The native pinned llama.cpp helper is now implemented for Granite. The retained external Ollama adapter uses loopback HTTP as a separate development integration. This is the existing **development integration**, not evidence that the C04/C08 frozen evaluation runtime is delivered. Calls are off the GUI thread, the URL is restricted to loopback, and model identity is reported rather than assumed. Roclint does not own the Ollama daemon and must not terminate unrelated runtime processes.

The frozen product family remains `ibm-granite/granite-4.2-3b-GGUF` with `llama.cpp`. Select Q4_K_M or Q5_K_M after measurement. Existing model preferences are not silently replaced with an invented Ollama tag, and no large model is downloaded by this pass.

## Final analysis provenance

The C03/C06 gap identified by the structure audit is implemented as a separate post-evaluation `AnalysisSnapshot`. Scores/degraded status, attempts and presentation order are persisted and hashed; intent, rewrite and approval use its exact identity. Schema v5 preserves older pre-score history while requiring fresh Analyze/review before reuse. See [final analysis details](FINAL_ANALYSIS_SNAPSHOT.md) for migration and verification scope.

## Current-session command versions

Draft and proposal generations now advance the authoritative version. Clarification, rewrite, review, approval and restore commands require the caller's observed version; stale commands preserve state and clipboard. Core copies are adopted after repository compare-and-swap, and final analysis/intent/proposal/approval writes advance the stored version in the same transaction. The Qt boundary exposes typed error codes alongside plain explanations. See [versioned commands](VERSIONED_COMMANDS.md).

## Material-assumption scoring

Julia now evaluates up to four material assumptions alongside up to three material choice groups in one independent batch. The v3 final artifact persists validated uncertainty, explicit unscored reasons and presentation order, while v2 history retains its original hashes. Manual mode keeps over-budget assumptions visible; no score confirms a premise. See [batch and history details](ASSUMPTION_SCORING.md).

## Remaining product work

Julia's v1 helper lifecycle now includes handshake/context/hash validation, responsive control I/O, terminal cancellation on edits, asynchronous shutdown, Windows descendant ownership and bounded recovery without scoring replay. Real-model and Qt verification are recorded in [Julia lifecycle details](JULIA_LOCAL_SCORER.md). This closes those Julia development-transport gaps; native Granite, whole-pipeline deadlines, lazy first use and installation/residency requirements remain in scope below.

These are explicit requirements, not claims of completion:

| Area | Remaining work |
|---|---|
| Semantic/presentation audit, C03 | Capped material-assumption scoring and its durable ordering/provenance are implemented. Continue reviewing provider/system-event and global host boundaries against their own contracts; current-session commands use observed versions and transactional artifact writes. |
| Optional Jev path, original C04/C08 | Implement the real bounded batched provider operation, pin its model/API and use OS credential storage if this cloud option is wanted. No configured Jev transport or automatic cloud fallback exists. Julia is the user's explicit default. |
| Julia release packaging | Bundle its isolated Python/native dependencies, license notices and model delivery for clean installations; measure resource use and decision quality. The local development environment is prepared separately. |
| Frozen local runtime, C04/C08 | Native pinned Granite/llama.cpp helper, exact size/hash verification and explicit managed download are integrated. Both measured candidate configurations failed the analysis/semantic gate; resolve those failures before choosing a quantization. Repeat representative benchmarking and finish deployment/license validation. See [native runtime](NATIVE_GRANITE.md). |
| Residency, C08 | Independent HOT/IDLE_MAPPED/UNLOADED operations are integrated with CPU mapping and honest GPU full-unload fallback. Windows memory-resource notifications and persisted user presets are implemented. Measure/freeze representative Q4/Q5 policy thresholds and recovery/resource costs; native macOS pressure support remains open. |
| Model/provider recovery | Native and Julia helpers implement binding, cancellation, late discard, bounded restart without semantic replay and owned Windows shutdown. The shared active analysis deadline spans Julia/S1 and Granite with warm-up excluded. Complete native macOS/release evidence. |
| Study/telemetry, C07 | Implement the pseudonymous study hierarchy, exact downstream ExecutionRequest artifacts, immutable assigned conditions, export and matched-condition harness. Current model-attempt history is not the complete study system. |
| Codebase RAG, C10 extension | Native Tree-sitter + SQLite FTS5 structural indexing; user-consented workspace context; ≤350-token signature-only injection; working-set weights/decay; strict exclusion from Jev and telemetry. No simulated project/file lists or raw file dumps are supplied as substitutes. |
| Release validation, C08 | Clean installations, dependency/license ledger, model delivery, signing expectations, keyboard/screen-reader checks and native Windows/macOS validation. Windows build results do not establish macOS parity. |

## Checks and reproducibility

Validation before the subsequent Julia changes on October 22, 2026: Windows Release build succeeded; all 30 CTest suites passed. The actual QML keyboard workflow, compact-width assertion and busy-model switch regression passed. Rendered review/settings previews were inspected, and the isolated host dry-run and diff whitespace check passed. QML lint returned success with context-property/qualification warnings. These checks do not establish live Jev/model quality, Julia UI integration or macOS parity.

- `roclint_product_alignment` loads the actual packaged QML with a scripted local transport. It checks no analysis on capture, ordinary digit/newline editing, unresolved-choice routing, keyboard-only rewrite and approval, clipboard preservation before approval, settings persistence, stale hint/configuration rejection and both screen loads.
- Other existing core, storage, bridge and host suites remain registered with CTest.
- Tests use temporary settings/history, offscreen rendering and controlled model responses. Native shortcut conflicts, actual accessibility providers, live Jev and real Granite output require platform/provider validation.
- `ROCLINT_TEST_ARTIFACT_DIR` optionally saves rendered review/settings previews during the QML test.
- Qt Test is a development dependency when both desktop and tests are enabled; production builds can use `-DBUILD_TESTING=OFF`.

Before changes, the tracked and untracked source tree was saved to `D:\Stardust\backups\roclint-before-product-alignment-20261022-175141.zip`. No changes were made to the downloaded GitHub folder or the supplied specification bundle.
