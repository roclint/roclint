# Retrieval Subsystem — Handoff Document

**Date:** 2026-11-13\
**Branch:** `feat/roc-hud-refresh`\
**HEAD at time of work:** c39d82140f0a2ab15337f174b3a134b7d41024fe\
**Subsystem:** R00 — Semantic Retrieval for Context Supply\

---

## 1. What Was Built

A self-contained retrieval subsystem that provides workspace-scoped hybrid
semantic/lexical search to Roclint's generation layer. It runs entirely locally,
off the GUI thread, with no external service dependencies in normal use.

### New files

```
include/roclint/retrieval/
    RetrievalTypes.hpp          # Shared value types (R00)
    IEmbeddingRuntime.hpp       # Abstract embedding interface (R01)
    RetrievalIndex.hpp          # Persistent hybrid index interface (R02)
    TextChunker.hpp             # Structure-aware chunker interface (R03)
    RetrievalEngine.hpp         # Top-level integration interface (R04)
    EmbeddingWorkerRuntime.hpp  # Subprocess runtime interface (R05)

src/retrieval/
    RetrievalTypes.cpp
    TextChunker.cpp             # Paragraph/C++ boundary chunking + workspace walker
    RetrievalIndex.cpp          # SQLite WAL index (schema v1, dense blobs + FTS5)
    RetrievalEngine.cpp         # Indexing + RRF hybrid retrieval + fingerprinting
    EmbeddingWorkerRuntime.cpp  # Python subprocess I/O (Windows + POSIX)

app/runtime/
    embedding_worker.py         # Python worker: granite-embedding-311m-multilingual-r2
    embedding-models.json       # Pinned model manifest (revision d9ade47c9b2b...)
    embedding-requirements.txt  # sentence-transformers>=3.0, torch>=2.0, etc.

tools/
    setup_embedding_model.py    # Download + verify the embedding model
    retrieval_benchmark.cpp     # Benchmark: indexing speed, retrieval latency

tests/retrieval/
    MockEmbeddingRuntime.hpp    # Deterministic hash-based mock (no model needed)
    test_retrieval_index.cpp    # 20 unit tests (chunker, index, engine, types)
    test_retrieval_integration.py  # Real-embedding quality evaluation
```

### CMake targets

| Target | Description |
|---|---|
| `roclint_retrieval` | Production static library |
| `roclint_retrieval_testing` | Test variant (ROCLINT_ENABLE_TEST_MOCKS) |
| `roclint_retrieval_index_tests` | CTest unit test (runs with mock, no model) |
| `roclint_retrieval_integration` | CTest Python test (DISABLED; needs model assets) |
| `roclint_retrieval_benchmark` | Benchmark tool |

---

## 2. Model — IBM Granite Embedding 311M Multilingual R2

| Property | Value |
|---|---|
| Model ID | `ibm-granite/granite-embedding-311m-multilingual-r2` |
| Pinned revision | `d9ade47c9b2b2afee75f29fd8a1cef279a0e9aea` |
| Output dimensions | 768 (full; Matryoshka supports 256/512 after quality baseline) |
| Pooling | Mean over all tokens |
| Normalization | L2 required (applied by worker) |
| Query prefix | None required (per published model card) |
| Document prefix | None required |
| Max sequence length | 8192 tokens |
| Languages | 12 (en, de, es, fr, ja, ko, pt, ar, cs, it, ru, zh) |
| Architecture | ModernBERT encoder |
| License | Apache-2.0 |

The revision hash is immutable on HuggingFace. Never silently substitute another model.
Full 768-dim embeddings are used by default. Evaluate reduced dimensions only after a
documented retrieval quality baseline on this corpus.

---

## 3. Architecture

```
HostController (Gemini)
       │
       ▼
RetrievalEngine                    ← caller entry point
  ├─ indexWorkspace() / indexPaths()   ← off-GUI-thread indexing
  └─ retrieve()                        ← off-GUI-thread retrieval
       │
       ├─► TextChunker               ← structure-aware chunking
       ├─► IEmbeddingRuntime         ← interface (EmbeddingWorkerRuntime in prod)
       │      └─► embedding_worker.py subprocess (Python/sentence-transformers)
       └─► RetrievalIndex (SQLite)
              ├─ retrieval_chunks (metadata + text)
              ├─ retrieval_embeddings (768-dim float32 blobs)
              ├─ retrieval_fts (FTS5 porter tokenizer)
              ├─ retrieval_sources (freshness tracking)
              └─ retrieval_meta (policy, generation, coverage)
```

### Retrieval algorithm (Reciprocal Rank Fusion)

1. Dense: scan all embeddings, compute inner product (cosine after L2 normalization).
2. Lexical: FTS5 BM25 on query words against chunk text.
3. RRF fusion: `score = 1/(60+rank_dense) + 1/(60+rank_lex)`.
4. Sort by fused score, return top-k.

For small indexes (< 50K chunks), full scan is fast enough (~0.9ms p50 warm). An ANN
library (e.g. hnswlib) can replace the dense scan step for large repositories without
changing the interface.

---

## 4. Interface for Gemini's Integration

Gemini's integration must call `RetrievalEngine` from a worker thread, never the GUI.

### Minimal integration pattern

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
    .minFusedScore = 0.05f,
};
auto engine = std::make_shared<RetrievalEngine>(runtime, cfg);

// 2. Index workspace (on a QtConcurrent worker)
engine->indexWorkspace(workspaceRoot, workspaceIdentity, limits, cancellation);

// 3. Retrieve on each prompt analysis
RetrievalRequest req{
    .requestId     = "req_" + sessionId + "_" + revisionId,
    .sessionId     = sessionId,
    .revisionId    = revisionId,
    .promptHash    = sha256(frozenPromptText),
    .workspace     = {workspaceRoot, workspaceIdentity, epoch, activeFile},
    .promptText    = frozenPromptText,
    .purpose       = RetrievalPurpose::ClarificationAid,
    .maxHits       = 20,
    .contextBudgetTokens = 512,
};
auto resp = engine->retrieve(req);

// 4. Reject stale responses
if (resp.isStale(revisionId, epoch)) return; // discard

// 5. Inject into Granite context (resp.hits are bounded, injection-flagged)
for (const auto& hit : resp.hits) {
    if (hit.promptInjectionFlagged) continue;
    if (!hit.sourceHashValid) continue;
    // append hit.excerpt to retrieval context section
}
// Store resp.contextFingerprint alongside any approval that uses this context.
```

### Stale-response rejection

Every `RetrievalResponse` carries the caller's `revisionId` and `workspaceEpoch`.
Call `resp.isStale(revisionId, epoch)` before consuming hits. This covers:
- Workspace switch during retrieval
- Draft edit since the retrieval was started
- Superseded retrieval requests

### AST-retrieval boundary

Retrieval results go in a **separate labeled section** outside the existing 350-byte
AST XML block. They must never be:
- Promoted to `GroundingStatus::Verified`
- Used to prove symbol absence or uniqueness
- Mixed into `GroundedPromptContext.xml` or `astSnapshotHash`

The `AstLink` in `RetrievedHit` carries advisory symbol metadata only. Status
`AstLinkStatus::Linked` means a symbol was found at index time, not re-verified now.

### Context budget

`contextBudgetTokens` in the request limits total excerpt characters using a 4-char/token
approximation. Set it to the remaining Granite context budget after the AST XML, prompt,
and system instructions. The `contextFingerprint` covers only actually emitted excerpts.

### Proposal-hash extension (coordinate with Gemini)

The existing `astSnapshotHash` must not be modified. If retrieval context needs to be
bound to an approval, store `contextFingerprint` as a separate field alongside the
existing hash. The two domains are independent and must stay separate.

---

## 5. Deployment Requirements (Windows App)

### Python prerequisites

The embedding worker requires Python 3.10, 3.11, or 3.12 and the following packages:

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

or just:
```
pip install -r app/runtime/embedding-requirements.txt
```

### Model download

```
python tools/setup_embedding_model.py
```

Default installation directory on Windows:
```
%LOCALAPPDATA%\roclint\Roclint\models\embedding\
```

The script verifies the downloaded assets and writes a local manifest.
`embedding_worker.py` accepts `--model-dir PATH` to point to this directory.

### Model asset sizes

| File | Size |
|---|---|
| `model.safetensors` | ~588 MB |
| `tokenizer.json` + config | ~1 MB |
| Total | ~590 MB |

### Runtime process

The embedding worker is a persistent Python subprocess. It is launched once per
Roclint session and kept alive for the duration. It communicates via JSON-newline
on stdin/stdout. Startup (including model load) takes 10–60 seconds depending on
hardware. After startup, each embedding call takes 100–500ms on CPU (50ms on GPU).

### Memory budget

The embedding model uses approximately 600–650 MB resident memory in bfloat16/fp32.
This is in addition to Granite (Q8_0: ~4 GB, Q4: ~2.3 GB) and Julia (~550 MB).
On machines with 8 GB RAM, enable retrieval selectively or disable when memory is
constrained. The `Unavailable` state is surfaced correctly when the worker fails to start.

### No terminal required

The subprocess is started and managed by `EmbeddingWorkerRuntime`. Users do not
need to run any terminal commands during normal use.

---

## 6. Known Limitations

1. **Dense search is a full scan** for small indexes (< 50K chunks). For large
   repositories (> 50K chunks), replace the inner-product scan in `RetrievalIndex::retrieve`
   with an ANN library without changing the calling interface.

2. **Retrieval quality is not yet measured with real embeddings** in this branch.
   The integration test (`test_retrieval_integration.py`) defines the evaluation corpus
   and thresholds. Run it with model assets downloaded to establish the baseline.

3. **Context fingerprint tokens are approximated** at 4 chars/token. Use the actual
   Granite tokenizer for precise budget enforcement when it matters.

4. **Source injection detection is heuristic** (prefix matching on common instruction
   markers). It is not a security boundary.

5. **No macOS validation** has been performed for the subprocess pipe code (POSIX path).
   The POSIX path in `EmbeddingWorkerRuntime.cpp` follows standard UNIX practices.

6. **Image and binary content** cannot be embedded by this model. The chunker excludes
   binary files. Do not claim image understanding from imported image files.

7. **The multilingual model's code retrieval quality** has not been separately measured
   against the monolingual-English-only baseline. The integration test corpus covers only
   English; multilingual cases need a separate held-out dataset.

---

## 7. Files Gemini Must NOT Edit

Gemini's generation, clarification, Julia, Granite, Goldfish, and QML components should
not need changes to any retrieval files. The integration boundary is `RetrievalEngine.hpp`.

Gemini's ownership list (from the task spec) is unchanged:
- `HostController.cpp/hpp`, `HostControllerTriage.cpp`, `HostControllerResidency.cpp`
- `GraniteAdapter.*`, `GraniteGoldfishAdapter.*`, `GraniteRewriteAdapter.*`
- `JuliaRuntime.*`, `NativeGraniteRuntime.*`
- `FloatingCard.qml`, `IntentReviewPanel.qml` and other QML files
- `AstGroundingEngine.*`, `CanonicalIntent.*`

If Gemini needs to surface retrieval status in the HUD, the minimal integration is:
1. Add a `Q_PROPERTY` for retrieval status (string from `toString(RetrievalStatus)`)
2. Create/own a `RetrievalEngine` instance alongside the existing CodebaseIndex
3. Call `retrieve()` from the grounding worker thread
4. Pass `RetrievedHit::excerpt` entries to the Granite prompt builder as a separate section

No changes to AST hashing, proposal hashes, or existing storage contracts are required.

---

## 8. Verification Results

**Platform:** Windows 10/11, MSVC 14.44 (VS 2022)\
**Build configuration:** Release\
**CTest:** 58/58 passed (0 failures, 0 regressions)\

```
roclint_core_smoke            PASS  0.02s
roclint_storage               PASS  0.10s
roclint_retrieval_index       PASS  0.28s   ← new
[... 55 existing tests all pass ...]
```

**Benchmark (synthetic 100-file workspace, mock embeddings):**
```
Files indexed: 100 in 131ms (763 files/s)
Index size:    400 KB (~4 KB/chunk)
Retrieval p50: 0.87ms  p95: 1.13ms  (no model I/O)
```

**Integration test:** Registered as CTest `roclint_retrieval_integration` (DISABLED).
Enable and run with model assets:
```
ctest -C Release -R roclint_retrieval_integration --no-label-summary
```

**Python syntax check:** All three Python files parse cleanly.

---

## 9. What Remains for Gemini's Integration

1. Create a `RetrievalEngine` instance in `HostController` or `HostControllerWorkspace`,
   parallel to the existing `CodebaseIndex`.
2. Start `EmbeddingWorkerRuntime` on host startup (after user consent and model check).
3. Call `indexWorkspace` / `indexPaths` from the codebase-watcher worker, alongside
   the existing AST index refresh.
4. Add a `retrieve()` call in the grounding worker thread, before Granite prompt assembly.
5. Pass retrieval hits as a separate section in the Granite prompt (outside AST XML).
6. Store `contextFingerprint` alongside any approval that relies on retrieved context.
7. Expose retrieval status as a `Q_PROPERTY` for the HUD (optional for initial integration).
8. Run `test_retrieval_integration.py` with model assets to establish quality baseline.
9. Decide on context budget allocation: how many tokens to reserve for retrieval context
   within the existing 8192-token Granite context window.

None of steps 1–9 require changes to AST grounding, proposal hashing, or the approval
record schema.
