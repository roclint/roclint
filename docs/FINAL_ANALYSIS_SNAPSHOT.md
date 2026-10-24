# Final scored analysis and history

## Lifecycle

```text
Frozen revision -> Granite -> NormalizedAnalysis
                           -> scorer result or explicit degradation
                           -> AnalysisSnapshot -> ConfirmedIntent -> rewrite -> wording approval
```

`NormalizedAnalysis` remains the pre-score discovery type at its existing path. Its legacy `id`, `snapshotHash`, `sealSnapshot` and scorer `analysisSnapshotId` names identify that input payload. They are not the final artifact. Existing bridge aliases remain available for scoring types now owned by the analysis layer.

The separate `analysis::AnalysisSnapshot` contains the normalized discovery, complete frozen-prompt SHA-256, validated evaluation, model version, execution status, confidence/presentation policy versions, discovery/scorer attempt IDs and presentation order. Its `analysis_<SHA-256>` identity covers all those fields, including probabilities, ambiguity scores, degradation reason and latency. Hashing normalizes signed zero; JSON preserves round-trip double precision. Storage and rewrite checks compare the complete prompt hash, so edits outside quoted evidence cannot reuse the source binding.

The presentation policy orders groups by ambiguity, then earliest evidence byte and ID; alternatives by probability, then ID. Unranked alternatives use ID order. The intervention state machine and QML controller use the same policy. Ranking never selects an alternative for the user.

## S1-first version 4

The current live route follows [the user's model diagram](MODEL_ROUTING.md). Version 4 embeds a sealed `PromptTriage` from actual Julia prompt classification. Below 0.70 it contains empty discovery data and no Granite attempt; at or above 0.70 it binds that classification to a successful Granite discovery. Alternative evaluation is explicitly `NOT_REQUESTED`, with no copied classifier probabilities on individual choices. Empty ambiguous discovery cannot be silently reclassified as clear.

The `julia_triage` attempt stores the exact serialized sealed classification as the final `prompt_triage_payload` field in its model detail. It survives a subsequent Granite failure. Insertion and restore require the successful same-session/revision/model attempt and this exact payload. The v4 artifact's hash covers the complete classifier provenance and head scores through the classifier hash. Production rejects mock classifier execution.

SQL schema v5's final-payload column also stores JSON v4; no destructive migration or rewrite of older rows is needed. JSON v2/v3 readers and hash algorithms remain unchanged. Their post-discovery scoring APIs below describe historical contracts, rather than the new live order.

## Persistence and binding

- The controller persists the final artifact after Julia finishes, or after producing an explicitly unranked outcome. It does not persist discovery as final scored evidence.
- The repository validates the final artifact, its frozen revision, and matching completed discovery/scorer attempts. A ranked artifact requires a scorer attempt and model identity. Production libraries reject mock-scored artifacts.
- Intent construction/validation, rewrite requests and approval contexts require the final type. A changed score, model, attempt, degradation or presentation order changes its identity and makes old intent/approval stale.
- Reopening history restores the saved scores and order. It neither runs a model nor substitutes a newly selected provider's evaluation.
- Corrupt saved score metadata, payload hashes, key/hash disagreement and missing/foreign attempt references are errors. Saved evidence is not repaired by silently dropping scores.

## Schema v5 and previous history

Schema v5 adds `analysis_snapshots.format_version`: old pre-score rows are version 1; final artifacts use SQL format version 2. The current routed JSON payload is `analysis_snapshot.v4`; previous `analysis_snapshot.v2` and `analysis_snapshot.v3` payloads retain their original readers and hashes. The migration retains old sessions, revisions and complete downstream history. It clears the current flag on approvals bound to version-1 analysis; their original proof fields and wording remain historical records.

Restoring version-1 analysis returns the prompt and a visible request to Analyze/review again. It does not invent old scoring evidence, silently rebind an old intent, reuse an old approval or trigger inference. The revised build must not share a live database with an older resident executable.

This is a coordinated analysis/bridge/intent/rewrite/approval/storage/controller change. Compile the callers and implementations together; the authoritative downstream APIs intentionally no longer accept a pre-score `NormalizedAnalysis` as final provenance.

## Verification scope

`roclint_analysis_snapshot` covers actual-model-shaped score evidence, exact codec round trips, changed-score bindings, invalid scores and ordering, file-backed reopen, missing attempt rejection, corrupt stored keys and v4-to-v5 legacy history migration. Its probability fixtures do not establish model quality.

`roclint_scored_history` exercises the controller with a test-only scorer, reordered alternatives, explicit confirmation and restoration without a configured scorer. Other existing tests cover the real Julia child-process protocol and the QML approval workflow. The subsequent [assumption-scoring change](ASSUMPTION_SCORING.md) adds a version-3 artifact with assumption evidence/order while retaining the original version-2 hashes. [Current-session command versions](VERSIONED_COMMANDS.md) are also implemented. Live inference quality, runtime/residency, study/export, packaging and planned RAG remain separate requirements.

`roclint_production_snapshot` links the production libraries without mock seams and checks that even a correctly sealed mock-scored payload is rejected, while a valid model-shaped payload is accepted. It tests the boundary, not actual inference.

### Recorded results from the preceding artifact pass, October 22, 2026

- Windows Release build: passed; all **34/34 CTest suites passed** (39.68 seconds).
- Structure check against both supplied folders: **96/96 original paths retained**, 166 source files; all recorded vision source hashes match. No new directories were needed for this work.
- Structure guard regressions: **8/8 passed**.
- Production host `--dry-run --db :memory:`: passed, with the packaged QML root verified and no model startup or invocation hooks.
- Developer evidence: `build/final-analysis-build.log`, `build/final-analysis-tests.log`, `build/final-analysis-structure.json` and `build/final-analysis-dry-run.log`.

The complete source was backed up before this change to `D:\Stardust\backups\roclint-before-final-analysis-20261022-223908.zip`. The source snapshot and specification folders were not edited. No normal user history was opened by verification, and no branch or commit was published.
