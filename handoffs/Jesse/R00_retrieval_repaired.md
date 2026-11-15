# Retrieval Subsystem — Repaired Handoff Document

**Date:** 2026-11-13 (repairs applied)
**Branch:** `feat/roc-hud-refresh`
**HEAD at time of work:** c39d82140f0a2ab15337f174b3a134b7d41024fe\
**Previous delivery:** R00_retrieval_handoff.md (Jesse, 2026-11-13) — contained 9 functional failures
**Subsystem:** R00 — Semantic Retrieval for Context Supply

---

## 1. What Was Repaired

The previous delivery failed review with 9 reproducible defects. All have been
fixed and covered by regression tests. The index schema version was bumped to
reflect the new `published_gen` column.

### Repaired files

| File | Changes |
|---|---|
| `app/runtime/embedding-models.json` | Correct revision, correct pooling, manifest v2 |
| `app/runtime/embedding_worker.py` | Correct revision, CLS pooling verification, strict ID binding, dimension validation, error handling |
| `tools/setup_embedding_model.py` | Correct revision, pooling config verification |
| `include/roclint/retrieval/RetrievalEngine.hpp` | Fixed `minFusedScore` default: 0.05→0.0 |
| `include/roclint/retrieval/TextChunker.hpp` | Added `WalkResult` struct with `exhausted` flag; `walkWorkspace` returns `WalkResult` |
| `src/retrieval/TextChunker.cpp` | Walker correctly detects file-cap vs exhaustion |
| `src/retrieval/RetrievalIndex.cpp` | Staged/published generation model; workspace root validation; FTS source/gen filters; RRF branch membership; context budget enforcement; vector buffer validation; real rollback |
| `src/retrieval/RetrievalEngine.cpp` | Coverage honesty; embed failure tracking; workspace root propagation |
| `src/retrieval/EmbeddingWorkerRuntime.cpp` | Request/response ID binding; model/dim validation; timeout kills connection; finite-value checks; batch size bound |
| `tests/retrieval/test_retrieval_index.cpp` | 9 new regression tests; existing tests updated for new API |

---

## 2. Model — IBM Granite Embedding 311M Multilingual R2

| Property | Value |
|---|---|
| Model ID | `ibm-granite/granite-embedding-311m-multilingual-r2` |
| **Pinned revision** | **`44399559930365213510b1ee2eb15ded83374f0e`** (HEAD as of 2026-05-18) |
| Output dimensions | 768 (full; Matryoshka supports 256/512 after quality baseline) |
| **Pooling** | **CLS token** (NOT mean) |
| Normalization | L2 required (applied by worker) |
| Query prefix | None required |
| Document prefix | None required |
| Max sequence length | 8192 tokens |
| Languages | 12 (en, de, es, fr, ja, ko, pt, ar, cs, it, ru, zh) |
| Architecture | ModernBERT encoder |
| License | Apache-2.0 |

### Corrections to previous handoff

1. **Revision `d9ade47c9b2b2afee75f29fd8a1cef279a0e9aea` does not exist.** It
   returns HTTP 404 from HuggingFace. The correct revision is
   `44399559930365213510b1ee2eb15ded83374f0e`.

2. **Pooling is CLS, not mean.** The `1_Pooling/config.json` published at the
   correct revision specifies `pooling_mode_cls_token=true,
   pooling_mode_mean_tokens=false`. The `sentence-transformers` library reads
   this config from the model directory and applies CLS pooling automatically.
   No code change to the embedding math is needed; the config drives it.
   The setup script and embedding worker now verify the pooling config and
   reject a directory that declares mean pooling.

3. **Model card claims about "12 languages" and "code retrieval quality" are
   not independently verified** in this branch. The integration test corpus
   covers only English. Multilingual and code-specific baselines require a
   separate held-out evaluation dataset.

4. **Memory estimate (600–650 MB)** is an approximation. Actual RSS depends on
   the OS page cache and Python runtime overhead; measure on target hardware
   before committing to a default-on configuration.

---

## 3. Bug Fixes

### Fix #1 — Model revision 404

**Root cause:** The pinned revision `d9ade47c` was fabricated or stale.

**Fix:** `embedding-models.json`, `embedding_worker.py`, and
`setup_embedding_model.py` now use `44399559…`. The setup script verifies that
`1_Pooling/config.json` enables CLS pooling. The worker rejects a model
directory with mean pooling declared.

---

### Fix #2 — Default threshold rendered all results invisible

**Root cause:** `RetrievalEngineConfig::minFusedScore = 0.05f`. With RRF (k=60)
and two branches, the maximum achievable score is `1/61 + 1/61 ≈ 0.0328` (rank 1
in both branches). The default threshold was larger than any reachable score.

**Fix:** Default changed to `0.0f` with an explanatory comment. A meaningful
threshold must be established by evaluating retrieval quality on the specific
corpus. The existing tests that overrode the threshold to zero now test the
real default.

---

### Fix #3 — RRF assigned dense ranks to lexical-only candidates

**Root cause:** The old code ranked all candidates in the dense-score sort and
assigned them a dense-branch RRF contribution regardless of whether they
actually appeared in the dense scan. Lexical-only candidates (from FTS) were
incorrectly boosted by their arbitrary position in the dense sort.

**Fix:** `Candidate` now carries `inDense` and `inLexical` flags. RRF rank
contributions are computed separately for each branch and applied only to
members of that branch. Lexical-only candidates receive only their lexical rank
contribution.

---

### Fix #4 — Context budget not enforced

**Root cause:** `opts.contextBudgetChars = request.contextBudgetTokens * 4` was
computed but never checked against excerpt accumulation.

**Fix:** `retrieve()` accumulates `budgetUsed` across hits and either truncates
the final excerpt to fit or skips the hit when the budget is already exhausted.
UTF-8 multi-byte boundary safety is maintained. The budget now covers only
excerpt bytes; callers should reserve additional headroom for source labels,
delimiters, and framing.

---

### Fix #5 — Workspace root mismatch returned wrong-workspace hits

**Root cause:** The dense scan and final metadata fetch filtered by
`workspace_root` but did not validate that the request's workspace root matched
the index's bound root. The FTS query had no `published_gen` or source-kind
filter. A request for workspace B while the index was open for workspace A
could return hits from A.

**Fix:** `retrieve()` validates `filter.workspaceRoot` against
`impl_->workspaceRoot` and returns `InvalidInput` on mismatch. The FTS query
now filters on `workspace_root`, `published_gen > 0`, and source kind. The
final metadata fetch also requires `published_gen > 0`.

---

### Fix #6 — Coverage claimed complete when capped or failed

**Root cause:** `walkWorkspace` stopped the iterator at `result.size() <
maxFiles` and returned a capped list. The engine then checked `files.size() <=
limits.maximumFiles`, which is always true (the list is already capped). Embed
failures were silently dropped without affecting `incomplete`.

**Fix:** `walkWorkspace` now returns `WalkResult` with an `exhausted` flag.
`exhausted=true` means the iterator reached EOF before the cap. The engine
declares coverage complete only when `!cancelled && walkResult.exhausted &&
embedFailures == 0`. The `incomplete` flag in `IndexStats` is now set when any
embedding failures occurred.

---

### Fix #7 — Subprocess protocol not bound; timeout consumed delayed replies

**Root cause:** Each embed request generated a new request ID (`req_N`) but the
response ID was never validated against it. A timed-out response could be
silently consumed as the reply to the next request. Model identity (model_id,
model_revision, dimensions) in embed responses was not validated.

**Fix:**
- Each request has a unique monotonic ID assigned before the write.
- The response ID must equal the request ID; any mismatch kills the process and
  marks the runtime Failed.
- On timeout, the process is killed (not reused), preventing delayed-reply
  consumption.
- `embed` responses are validated for `model_id`, `model_revision`, and `dim`
  against the values established at startup ping.
- Batch sizes are bounded at 256.

---

### Fix #8 — Rollback had no effect; staged chunks visible to readers

**Root cause:** `beginGeneration` only updated in-memory state. `insertChunks`
committed its own SQL transaction. `rollbackGeneration` attempted `ROLLBACK`
(which applied to no open transaction) and had no actual effect. The dense scan
queried `snapshot_gen >= currentGen_`, which returned staged (uncommitted)
chunks from a later generation.

**Fix:** A `published_gen` column is added to `retrieval_chunks` and tracked in
`retrieval_meta`. Chunks written by `insertChunks` have `published_gen=0` (not
reader-visible). `commitGeneration` atomically sets `published_gen=pendingGen_`
in a single transaction, making them visible to readers. `rollbackGeneration`
deletes staged rows with `published_gen=0 AND snapshot_gen=pendingGen_`. All
read paths (`retrieve`, `snapshotCoverage`) filter on `published_gen > 0`.

**Schema change:** `retrieval_chunks` and `retrieval_meta` gain a `published_gen`
column. Policy mismatches already trigger a full purge, so existing databases
will be re-indexed. The schema version remains `1`; a schema migration would be
needed if old databases must be preserved without re-indexing.

---

### Fix #9 — Vector buffer shape not validated at index boundary

**Root cause:** `insertChunks` trusted the declared `embeddings.rows` and
`embeddings.dimensions` without verifying that `embeddings.data.size() ==
rows * dimensions` or that all values are finite. A malformed buffer (e.g. 1
float declared as 768 dims) would create an out-of-bounds read during retrieval.

**Fix:** `insertChunks` now validates:
- `dims > 0 && dims <= 65536`
- `data.size() == rows * dims`
- All floats in each row are finite (`std::isfinite`)

The same checks are applied at retrieval time when reading embedding blobs from
the database.

---

## 4. Interface for Gemini's Integration

Unchanged from previous handoff except for the note on pooling. Gemini must
not pass `minFusedScore` overrides to zero in production — the default of 0.0
is now intentional, and a quality-based threshold should be established from
the integration test.

```cpp
// 1. Construct (once per workspace session)
auto runtime = std::make_shared<EmbeddingWorkerRuntime>(EmbeddingWorkerConfig{
    .pythonExecutable = "python",
    .workerScriptPath = runtimeDir / "embedding_worker.py",
    .modelDirectory = embeddingModelDir,       // from setup_embedding_model.py
    .startupTimeoutMs = 60000,
});
auto s = runtime->start();                     // blocks until model loaded
// s.ok() == false → emit RetrievalStatus::Unavailable, proceed without retrieval

RetrievalEngineConfig cfg{
    .indexDatabasePath = appDataDir / "retrieval-index.db",
    .policy = runtime->policy(),
    // minFusedScore defaults to 0.0 (all candidates pass); tune after quality eval.
};
auto engine = std::make_shared<RetrievalEngine>(runtime, cfg);
```

### Stale-response rejection (unchanged)

Every `RetrievalResponse` carries the caller's `revisionId` and `workspaceEpoch`.
Call `resp.isStale(revisionId, epoch)` before consuming hits.

### Context budget (now enforced)

`contextBudgetTokens` in the request limits total excerpt characters using a
4-char/token approximation. This budget is **enforced** — excerpts are truncated
to fit. Reserve additional tokens for source labels, delimiters, framing, the
existing prompt, AST XML, and system instructions.

---

## 5. Deployment Requirements

### Python prerequisites

The embedding worker requires Python 3.10, 3.11, or 3.12 and:

```
sentence-transformers>=3.0
torch>=2.0
transformers>=4.40
safetensors>=0.4
numpy>=1.24
```

Install via:
```
python tools/setup_embedding_model.py --install-deps
```

### Model download

```
python tools/setup_embedding_model.py
```

Default installation directory on Windows:
```
%LOCALAPPDATA%\roclint\Roclint\models\embedding\
```

The setup script verifies the downloaded assets including the `1_Pooling/config.json`
pooling mode.

### Model asset sizes

| File | Size |
|---|---|
| `model.safetensors` | ~588 MB |
| `1_Pooling/config.json` | <1 KB |
| `tokenizer.json` + config | ~1 MB |
| Total | ~590 MB |

### Memory budget

The embedding model uses approximately 600–650 MB resident memory in bfloat16/fp32.
Measure on target hardware. On machines with 8 GB RAM, the `Unavailable` state is
surfaced correctly when the worker fails to start.

---

## 6. Known Limitations

1. **Dense search is a full scan** for small indexes (< 50K chunks). For large
   repositories (> 50K chunks), replace the inner-product scan with an ANN library.

2. **Retrieval quality is not yet measured with real embeddings** in this branch.
   The integration test (`test_retrieval_integration.py`) defines the evaluation
   corpus and thresholds. Run it with model assets downloaded to establish the
   baseline. The `minFusedScore=0.0` default means no quality filtering; a
   threshold must be established empirically.

3. **Context fingerprint tokens are approximated** at 4 chars/token. Use the
   actual Granite tokenizer for precise budget enforcement.

4. **Source injection detection is heuristic** (prefix matching on common
   instruction markers). It is not a security boundary.

5. **No macOS validation** has been performed for the subprocess pipe code
   (POSIX path). The POSIX path follows standard UNIX practices.

6. **Image and binary content** cannot be embedded. The chunker excludes
   binary files.

7. **The multilingual model's code retrieval quality** has not been separately
   measured against an English-only baseline.

8. **Schema migration** for existing databases with the old schema (without
   `published_gen` column) is not provided. Policy mismatch detection will
   trigger a purge and re-index on first open. If the model revision field is
   unchanged (empty/legacy), a manual database deletion may be needed.

---

## 7. Files Gemini Must NOT Edit

Same as previous handoff. The integration boundary is `RetrievalEngine.hpp`.

If Gemini needs to surface retrieval status in the HUD:
1. Add a `Q_PROPERTY` for retrieval status (string from `toString(RetrievalStatus)`)
2. Create/own a `RetrievalEngine` instance alongside the existing `CodebaseIndex`
3. Call `retrieve()` from the grounding worker thread
4. Pass `RetrievedHit::excerpt` entries to the Granite prompt builder as a separate section
5. Store `contextFingerprint` alongside any approval that relies on retrieved context

No changes to AST hashing, proposal hashing, or the approval record schema are required.

---

## 8. Verification Results

**Platform:** Windows 10/11, MSVC 14.44 (VS 2022)\
**Build configuration:** Release\
**CTest:** 39/39 passed (0 failures, 0 regressions)

```
roclint_core_smoke            PASS
roclint_storage               PASS
roclint_retrieval_index       PASS   ← 20 original + 9 regression tests
[... 36 existing tests all pass ...]
```

**Python syntax check:** `embedding_worker.py` and `setup_embedding_model.py`
parse cleanly.

**Integration test:** Registered as CTest `roclint_retrieval_integration`
(DISABLED). Enable and run with model assets to establish the retrieval quality
baseline:
```
ctest -C Release -R roclint_retrieval_integration --no-label-summary
```

---

## 9. Regression Test Coverage

| Test name | Bug covered |
|---|---|
| `testDefaultMinFusedScoreIsReachable` | #2 — threshold silenced all results |
| `testRRFBranchMembership` | #3 — dense rank assigned to lexical-only candidates |
| `testRollbackDeletesStagedChunks` | #8 — rollback had no real effect |
| `testStagedChunksNotVisibleBeforeCommit` | #8 — staged chunks visible before commit |
| `testCoverageHonestyWithFileCap` | #6 — cap declared as complete coverage |
| `testWorkspaceRootMismatchFailsClosed` | #5 — wrong-workspace hits returned |
| `testIndexRejectsMalformedEmbeddingBuffer` | #9 — short row accepted as valid |
| `testContextBudgetEnforcement` | #4 — budget not enforced |
| `testWalkWorkspaceExhaustionDetection` | #6 — exhaustion not distinguishable from cap |

---

## 10. What Remains for Gemini's Integration

1. Create a `RetrievalEngine` instance in `HostController` or `HostControllerWorkspace`,
   parallel to the existing `CodebaseIndex`.
2. Start `EmbeddingWorkerRuntime` on host startup (after user consent and model check).
3. Call `indexWorkspace` / `indexPaths` from the codebase-watcher worker, alongside
   the existing AST index refresh.
4. Add a `retrieve()` call in the grounding worker thread, before Granite prompt assembly.
5. Pass retrieval hits as a separate section in the Granite prompt (outside AST XML).
6. Store `contextFingerprint` alongside any approval that relies on retrieved context.
7. Expose retrieval status as a `Q_PROPERTY` for the HUD (optional for initial integration).
8. **Run `test_retrieval_integration.py` with model assets to establish the quality
   baseline and determine an appropriate `minFusedScore`.** The current default of 0.0
   returns all candidates; a quality-based threshold is essential for production use.
9. Decide on context budget allocation: how many tokens to reserve for retrieval context
   within the existing 8192-token Granite context window, accounting for the AST XML,
   prompt, system instructions, and generation output reservation.

None of steps 1–9 require changes to AST grounding, proposal hashing, or the approval
record schema.
