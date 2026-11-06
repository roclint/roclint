# AST review remediation — interface and ownership

**Review:** `build/ast-code-review.md` (four findings).  
**Baseline:** `240c74f` plus the uncommitted AST reliability working tree.  
**Scope:** pure AST grounding reliability. No packaging, redesign, embeddings or vector retrieval.

## Exclusive ownership

| Owner | Files |
|---|---|
| Specialist A | `include/roclint/storage/CodebaseIndex.hpp`, `src/storage/CodebaseIndex.cpp`, `tests/storage/testCodebaseIndexFreshness.cpp` |
| Specialist B | `app/host/HostController.cpp`, `app/host/HostControllerWorkspace.cpp`, `app/host/CodebaseWatcher.hpp`, `app/host/CodebaseWatcher.cpp`, `tests/viewmodel/test_ast_freshness_integration.cpp`, `tests/viewmodel/test_codebase_watcher.cpp` |
| Specialist C | `include/roclint/rewrite/AstGroundingEngine.hpp`, `src/rewrite/AstGroundingEngine.cpp`, `tests/storage/testAstGroundingEngine.cpp` |
| Coordinator | `app/host/HostController.hpp`, `CMakeLists.txt`, `docs/**`, final integration/review |

No two agents edit the same file. Shared declarations in `HostController.hpp` are added by the
coordinator before B implements against them. A and C may add public fields to their own headers.
Any further shared change must be requested from the coordinator first.

## Finding 1 — coverage completeness

`CodebaseIndex` schema becomes v5 with `workspaces.coverage_complete` (distinct from
`last_refresh_complete`):

```cpp
struct CodebaseIndexStats {
    // existing fields...
    std::vector<std::string> unprocessedPaths{}; // relative paths omitted by this pass
};

struct SymbolQueryResult {
    std::vector<AstSymbol> symbols{};
    bool truncated{false};
    bool complete{true}; // candidate universe/coverage is complete
};

struct CodebaseSnapshotInfo {
    std::uint64_t generation{0};
    bool complete{false};          // full scan complete AND paths coverage complete
    bool fullScanComplete{false};  // last full scan only
    bool coverageComplete{false};  // paths coverage in the current generation
    bool available{false};
};
```

Semantics:

- Full refresh: set both flags to the pass completeness.
- `refreshPaths`: set `coverage_complete = !stats.incomplete`; never change
  `last_refresh_complete`. It therefore cannot repair an incomplete full scan.
- Unprocessed requested paths are marked dirty and returned in `unprocessedPaths`.
- `snapshotInfo.complete = fullScanComplete && coverageComplete`.
- Every query returns `complete = snapshotInfo.complete` (and `truncated` for the caller limit).
- v3→v4→v5 migrations preserve unrelated tables. v4→v5 derives coverage from the previous
  completeness/dirty state.

Host: readiness for a paths refresh is `snapshot.complete && !stats.incomplete`; never
`wasReady || ...`. Incomplete paths are retried at most twice, then grounding stays withheld until
explicit/later recovery. Full-scan retry behavior remains bounded.

Grounding: any incomplete snapshot or query coverage makes the result non-authoritative. No
`Verified`, no `NotFoundInScope` absence proof, and no AST hash; evidence becomes
`Ambiguous`/`complete=false` (or remains `Ungrounded` with `complete=false` for ADD-style
non-existence claims). Reliable changed-file and truncation handling is preserved.

## Finding 2 — proposal revalidation

The host stores the grounding inputs and evidence used to create the current proposal:

- prompt, active file, workspace;
- evidence views (with `operator==`);
- AST snapshot hash;
- proposal ID, session ID, revision ID;
- a monotonically increasing validation serial.

`scheduleGroundingRevalidation()` always re-resolves the proposal's bound prompt/active file against
the current index. It compares current evidence and AST hash with the stored evidence/hash. It does
not require a generation change or selected-file byte change to invalidate. Unrelated changes
preserve the proposal only when current evidence and hash are demonstrably equivalent.

The worker result carries the serial/proposal/session/revision/workspace it checked. The UI callback
and any queued invalidation must recheck all of them; an older result must not invalidate a newer
proposal. Historical approvals remain immutable.

## Finding 3 — C10 token budget

The renderer must enforce the 350-token total code-context budget using the pinned tokenizer or a
mathematically valid worst-case bound. This remediation uses the worst-case byte bound: every
token in the pinned byte-level BPE covers at least one UTF-8 byte, so a complete document of at most
**350 bytes** is guaranteed to be at most **350 tokens**. This is a bound, not a bytes-per-token
estimate.

- `kMaximumPromptTokens = 350`, `kMaximumPromptBytes = 350`.
- The cap includes framing, attributes, paths, signatures and candidates.
- The signature allowance remains a separate sub-limit (default 300 bytes) but cannot exceed the
  total.
- Reduce at structural boundaries; never split UTF-8 or XML entities, always emit well-formed XML.
- Tokenizer unavailability does not change the worst-case guarantee.
- `computeAstSnapshotHash` remains unchanged; evidence/hash accounting follows the evidence
  actually rendered (verified evidence only).
- Update comments to remove the old 1000-byte / bytes-per-token claim.

## Finding 4 — watcher recovery

- A successful full scan must not restart a watcher merely because it is unhealthy.
- A running partial/degraded watcher is retained; its fallback refresh timer provides a controlled
  cadence. One overflow/failure notification may request one full refresh, not a restart loop.
- If the watcher is not running, recovery is bounded (max 3 attempts) with backoff. After exhaustion
  the host enters stable degraded operation: no automatic watcher restart; explicit refresh or
  workspace reselection is the deliberate recovery path.
- Workspace changes, shutdown and superseded callbacks cancel pending recovery.
- Watcher startup traversal is bounded by existing limits and is not repeated on every scan.

Host declarations supplied by the coordinator:

```cpp
void maybeRecoverCodebaseWatcher();
void resetCodebaseWatcherRecovery();
void scheduleCodebaseWatcherRecovery();
int m_codebasePathsRetries{0};
int m_codebaseWatcherRecoveryAttempts{0};
bool m_codebaseWatcherDeliberateRecovery{false};
bool m_codebaseWatcherDegraded{false};
quint64 m_codebaseWatcherRecoverySerial{0};
int m_codebaseRefreshStartsForTesting{0};
std::optional<CodebaseWatcher::Limits> m_codebaseWatcherLimitsForTesting;
```

Testing hooks: `setCodebaseWatcherLimitsForTesting`, `codebaseRefreshStartsForTesting`,
`codebaseWatcherDegradedForTesting`.
