# Upstream integration and review boundaries

## Baseline

The supplied `roclint-main/roclint-main` snapshot is the source tree of upstream commit `ca55398661093ae4c61e3771942e3f1eb0a7ff5d`. GitHub access was confirmed with Git on October 22, 2026. `origin/main` advanced by one documentation commit to `e3a8aeb44b23f3d57b392393162489c5fe10a43f`; this commit was fast-forwarded into the working repository without replacing local work.

The integration keeps the existing C++20/Qt/SQLite application, directory structure, resident executable, controller, domain types and upstream HUD/dashboard design. The previous wholesale QML replacement is no longer the default UI. The original window chrome, navigation matrix, workspace/file menus, editor canvas, scope strip, statistics and action bars are retained and connected to the corrected workflow. New clarification/review and detailed runtime controls are additive QML components.

This remains a substantial coordinated hardening patch. A clean Git merge does not prove source API compatibility, feature quality or release completeness. Do not describe this as a tiny patch or promise conflict-free future merges.

## Feature continuity

| Upstream feature | Integrated behavior |
|---|---|
| Resident tray/menu-bar host and original HUD/dashboard | Preserved, including pinning, resizing, minimization, themes and navigation. Click controls gain keyboard/accessibility activation. |
| Ctrl/Cmd+Shift+L invocation | Restored as the default. Explicitly saved preferences take precedence; registration conflicts are disclosed. |
| Clipboard capture and Send as-is | Preserved. Captured drafts are reviewed before analysis; raw dispatch uses the actual draft and current version. |
| Workspace selection and recent projects | Restored with the native folder picker and saved real paths. No developer-specific default paths or fabricated project lists. |
| Entire-project and single-file scope | Restored. Filename-only scanning runs off the GUI thread, prunes hidden/build/dependency directories, excludes symlinks and has traversal/time/result bounds. Apply scope writes the selected boundary into the visible draft. It neither reads source content nor implements RAG. |
| Julia/Jev selection and Julia download | Preserved. Julia is the default and performs real pinned CPU inference. Unavailable or invalid scoring remains unranked; no keyword score or fixed option probability is substituted. |
| Local model folder and runner models | Preserved. Native Granite verifies the pinned Q4/Q5 candidate directly; explicit managed repair writes only the selected model destination. Optional Ollama development mode retains local GGUF import using a temporary Modelfile and bounded asynchronous process. |
| Rewrite profiles | Preserved, canonicalized and persisted. Profiles format confirmed intent instead of inventing scope or permissions. |
| Analysis and synthesis actions | Preserved as asynchronous commands. They no longer turn Enter/Tab into an implicit first-choice confirmation or copy an unreviewed generation. |
| Diff, constraints and final delivery | Added to the original HUD in `IntentReviewPanel.qml`. Durable approval and explicit constraint review precede approved delivery. |
| Optional direct delivery | Uses the current captured-field/accessibility path, stale-text checks and clipboard fallback. Generic arbitrary-text injection into whichever app happens to be focused is not retained. |
| SQLite history, restore, delete and purge | Preserved with schema v5, typed payloads and approval invalidation. Existing schema-v3 databases migrate forward; use a backup for development. Purge requires a second click in the dashboard. |
| CMake/CTest entry points | Existing libraries, executable and CTest names retained. `roclint_core_tests` remains available as a compatibility build target. Test-only transports are separated from production libraries. |
| Upstream overview and latest architecture dossier | Preserved unchanged. Current behavior and measured evidence belong in separate implementation/integration documents. |

## Intentional changes contributors must review

- **Versioned host commands:** current-session draft, clarification, rewrite, review, approval, profile and restore commands require the observed state version. Core mutations advance it; artifact persistence uses repository compare-and-swap transactions. `lastErrorCode` retains the typed failure at the Qt boundary. `rewriteProfile` is read-only with an explicit versioned setter. QML/CLI/C++ callers are updated together; do not add wrappers that silently supply a version. See [command migration](VERSIONED_COMMANDS.md).
- **Async model interfaces:** synchronous string-returning synthesis and arbitrary live sample calls are replaced by analysis/proposal/runtime state notifications. The UI does not block on inference.
- **Core/storage contracts:** draft and proposal edit generations, session-specific revision IDs, SHA-256 bindings, typed analysis/intent/proposal/approval payloads and model attempts form one coordinated migration. Old headers/implementations/tests must not be cherry-picked separately.
- **Production behavior:** the native C++ template/heuristic fallback is not represented as model inference. Its old control now checks the real local runtime; the live model selection remains explicit.
- **Julia implementation:** native Python/PyTorch CPU inference replaces the old ONNX-file presence check and fabricated probabilities. This is a development integration exception to the upstream native-only/no-daemon aspiration. A distributable native/bundled runtime and measured residency/latency remain release work.
- **Direct delivery:** the retained setting controls optional captured-field delivery. Without an eligible capture or permission, the app uses the clipboard. It does not promise arbitrary-app injection from manual widget text.
- **CLI automation:** `--dashboard`/`--show-dashboard`, dimensions, pinning, screenshots, `--dry-run`, isolated `--db`, `--enhance`, `--resolve`, `--scope` and `--open-file-dropdown` are retained. `--workspace`/`--file` supply real targets. Requested choices wait for completed analysis; enhance requests a proposal and stops for review instead of silently copying an unreviewed generation.

## Suggested review order

1. `core`, `analysis`, `intent`, `rewrite`, `review`, storage codecs/schema and their tests: verify binding, validation and migration as one packet.
2. Controller and local runtime transport: verify async lifecycle, stale result handling, exact attempt identity and explicit failures.
3. Julia worker/runtime: review checkpoint pin, protocol bounds and actual inference separately from Granite.
4. Upstream HUD/dashboard diff plus added QML components: confirm retained appearance/controls and explicit review.
5. Workspace/model-folder restoration and optional platform bridges: review filesystem boundaries, capture eligibility and delivery outcomes.
6. Developer setup, merge preview, CMake/CTest and documentation.

These are review boundaries, not a claim that arbitrary intermediate cherry-picks compile. Shared type/schema/caller changes must land together. Keep unrelated cosmetic changes out of follow-up commits.

## Merge procedure

Run from the repository root after completing changes:

```sh
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python tools/merge_preview.py --fetch
```

The preview uses Git's merge algorithm on a temporary complete-working-tree snapshot, including currently untracked source. It writes `build/merge-preview/latest.json` with exact commit/tree IDs and `complete.patch` with all source additions/edits. It also checks that patch against the recorded upstream tree using a temporary index. It does not stage the real index or publish a branch/PR.

Before a PR, include all new source/QML/runtime files; exclude build outputs, model weights, environments and local history. Refresh upstream and repeat this check immediately before review/merge. Future edits to the same controller/core/QML lines may still conflict and require an ordinary rebase/merge.

## Remaining release gaps

The [structure and vision audit](STRUCTURE_AND_VISION.md) confirms retention of every original snapshot path. The subsequently implemented [final scored analysis](FINAL_ANALYSIS_SNAPSHOT.md) closes its identified pre-score provenance gap: intent, rewrite and approval now require the persisted post-evaluation artifact. Schema v5 retains earlier history and clears current approval on pre-score artifacts. Core/storage headers, callers, migration and tests form one coordinated change. Remaining semantic/API, runtime, study, packaging and RAG requirements are still open.

Native pinned Granite/llama.cpp is integrated with lazy startup, bound IPC, integrity-checked candidate delivery and separate resource tiers. Its final quantization/configuration is not frozen. No production Jev transport, bundled Julia installation, complete study/telemetry system or codebase RAG is claimed. Native macOS behavior, screen readers, clean-install dependencies, representative model quality, automatic residency policy and measured latency remain release work. See [native Granite](NATIVE_GRANITE.md), [product alignment](PRODUCT_ALIGNMENT.md) and [Julia runtime](JULIA_LOCAL_SCORER.md).

## Verification from the preceding compatibility pass, October 22, 2026

- Windows Release build: successful, including production host, test libraries and runtime-file delivery.
- CTest: **31/31 passed**. This includes the restored actual QML workflow, typing without implicit choices, keyboard activation of the original Settings control, explicit approval, compact resizing, settings/history, stale results, real bounded workspace listings/scope checks, and Julia child-process protocol identity/snapshot/probability cases.
- QML lint: successful with context-property/qualification warnings. Review/settings screenshots were rendered and inspected; the editor background, action sizing, scrollbars and resize focus behavior were corrected.
- Isolated host dry-run: exit 0 with the restored workspace, scope, file-dropdown, resolve and enhance flags; no model startup or native invocation hooks were enabled.
- Upstream project overview and latest architecture dossier: unchanged relative to the fetched upstream commit.
- Complete-working-tree merge and patch-application preview: clean against `e3a8aeb44b23f3d57b392393162489c5fe10a43f`. Exact final tree IDs and included new files are recorded in `build/merge-preview/latest.json`.
- No branch was published and no PR/remote commit was created. Local source changes remain available for review; the generated complete patch includes new files that a tracked-files-only diff would omit.

Local evidence is under `build/upstream-final-build.log`, `build/upstream-final-tests.log`, `build/upstream-cli-smoke.log`, `build/upstream-qmllint.log` and `build/upstream-previews/`. Build artifacts and downloaded model weights are excluded from the source patch.

All prior local source was backed up before this integration to `D:\Stardust\backups\roclint-before-upstream-compatibility-20261022-184423.zip`. The downloaded source/specification folders were left unchanged.
