# Bounded Julia evaluation of material assumptions

The user's Julia provider choice applies to both discovered choices and material-assumption checks. Granite still discovers the typed items; C++ controls materiality, bounds and user decisions. Julia estimates uncertainty locally and cannot accept, reject or remove an assumption for the user.

## Batch plan v2

| Input | Limit and evaluation |
|---|---|
| Material choice groups | Up to three structurally bounded groups; a choice row plus a separate ambiguity `noul` row for each |
| Material assumptions | Up to four, selected by earliest evidence byte and then ID; one independent `noul` row for each |
| Combined call | One helper request and one `engine.logits` call with at most ten independent rows; the SDK may use internal minibatches |
| Other assumptions | Retained in the normalized input. Nonmaterial assumptions are not scored; over-budget material assumptions remain explicitly unscored |

The assumption question distinguishes a premise explicitly established by the user's request from a premise left uncertain or unsupported. The recorded value is the latter option's softmax score. It is a model estimate, not calibrated truth or user consent. Choice/assumption rows receive the same frozen prompt and do not receive each other's answers. Assumption-only batches are supported.

Within the existing assumption tier, higher validated uncertainty is shown first; ties and unscored items use the established evidence/ID ordering. Conflict, missing-decision, materiality and clarification-budget rules remain C++ policy. A very low uncertainty estimate still cannot confirm a material assumption. The four-check model budget does not change the existing manual-review policy.

## Validation and presentation

- The worker rejects unsupported plans, excess groups/assumptions/alternatives, duplicate IDs, malformed fields, oversized UTF-8 text and noncanonical prompt newlines before inference. Model token overflow fails instead of truncating input.
- C++ binds results to the normalized input and pinned Julia identity, validates IDs/ranges/coverage and strips invalid or out-of-budget scores. Every material assumption has a scored record or an explicit unscored reason in the final artifact.
- SDK inference failures use a generic error so input fragments in an SDK exception are not echoed into host history.
- The HUD shows an advisory uncertainty label for the active assumption. Manual mode lists every material assumption, its user-decision state and score/unscored status in the saved presentation order. An item beyond the batch limit is visible as unscored.
- Assumption confirmation/rejection still uses an explicit versioned user command. No score writes an intent decision or grants approval.

## Durable format and previous history

| Layer | Current value |
|---|---|
| SQLite schema | v5, unchanged by this feature |
| SQL `analysis_snapshots.format_version` | `2`: final scored artifact; `1` still denotes old pre-score history |
| JSON artifact schema | `analysis_snapshot.v3` |
| Scoring plan | `2` |
| Presentation policy | `scorer_presentation.v2` |

The v3 artifact hash includes assumption scores, unscored reasons, scoring-plan version and final assumption order alongside the existing exact prompt, discovery, model/attempt and group evidence. Changing an assumption score changes the artifact identity and invalidates intent bound to the older artifact.

Version-2 final artifacts retain their original hash algorithm and group-only evidence. They decode and restore without rescoring or fabricating assumption annotations. A fixed v2 payload/hash regression checks this compatibility. Old pre-score history still requires Analyze/review under the earlier schema-v5 migration. Changing future scoring budgets or presentation rules requires a new recorded version while retaining old readers.

## Reproduce the checks

```sh
python tools/test_julia_worker.py
ctest --test-dir build -C Release --output-on-failure
python tools/check_julia_runtime.py --python "<Julia environment Python>" --model-dir "<installed checkpoint>" --worker "build/Release/runtime/julia_worker.py"
```

The worker unit tests use a deterministic engine and need no model packages or downloads. CMake registers them when a Python interpreter is available. `roclint_assumption_scoring` covers caps, nonmaterial exclusion, strict validation, immutable hashes, old-format reading, explicit user confirmation and file-backed restore. The Qt pipe test covers assumption-only requests and incompatible plans; the QML test checks that over-budget items remain visible and unconfirmed.

The live tool loads the installed pinned model offline, verifies identity and evaluates a maximum mixed batch and an assumption-only batch. It records the helper file's SHA-256 and checks that the file did not change during verification. It uses synthetic prompts, closes its owned helper on completion, and does not open user history. Its output establishes inference/protocol behavior only; model quality, calibration, reference-hardware latency and the frozen study remain separate work.

## Recorded results, October 23, 2026

- Windows Release build succeeded; **36/36 CTest suites passed** (38.04 seconds), including six worker unit cases and the old-hash, assumption-only pipe, saved-order and rendered manual-review regressions.
- The installed pinned Julia model completed offline CPU inference through the **packaged worker**: three groups + four assumptions, and zero groups + four assumptions. IDs, finite score bounds, probability sums, winning alternatives and checkpoint identity passed validation.
- The checked helper hash matches both the current source and packaged file. Live results are in `build/assumption-scoring-live.json`; the measured load/request times are diagnostics from this developer machine, not a performance or quality guarantee.
- Structure/reference check passed: **96/96 original paths retained**, 171 source files, and all recorded vision sources unchanged. **8/8 structure-guard regressions passed.**
- The packaged host passed `--dry-run --db :memory:` with its QML root verified and no model startup or invocation hooks.

Evidence: `build/assumption-scoring-build.log`, `build/assumption-scoring-tests.log`, `build/assumption-scoring-live.log`, `build/assumption-scoring-structure.json`, `build/assumption-scoring-dry-run.log` and `build/Testing/Temporary/LastTest.log`.

The source was backed up before this work to `D:\Stardust\backups\roclint-before-assumption-scoring-20261023-091932.zip`. Source snapshot/specification folders and normal user history were not edited by verification. Nothing was published to GitHub.
