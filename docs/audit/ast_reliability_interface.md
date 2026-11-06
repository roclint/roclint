# AST reliability milestone — shared interface and ownership

**Baseline:** `240c74f` / Phase 6 schema v3, 45 CTests green on Windows.  
**Scope:** saved-file freshness, coherent incremental snapshots, richer structural symbols, exact
grounding, existing host/HUD integration. No packaging, embeddings, vector retrieval, episodic
journal, or automatic code modification.

## Ownership

| Owner | Files |
|---|---|
| Specialist A | `include/roclint/storage/CodebaseIndex.hpp`, `src/storage/CodebaseIndex.cpp`, `app/host/CodebaseWatcher.hpp/.cpp` (new), `app/host/HostControllerWorkspace.cpp`, `tests/storage/testCodebaseIndexFreshness.cpp` (new) |
| Specialist B | `include/roclint/storage/AstSymbol.hpp`, `src/storage/AstSymbol.cpp`, `include/roclint/storage/AstSymbolExtractor.hpp`, `src/storage/AstSymbolExtractor.cpp`, `tests/storage/testAstSymbolExtractor.cpp` |
| Specialist C | `include/roclint/rewrite/AstGroundingEngine.hpp`, `src/rewrite/AstGroundingEngine.cpp`, `tests/storage/testAstGroundingEngine.cpp`, `tests/storage/ast_grounding_corpus.json` (new) |
| Coordinator | `CMakeLists.txt`, `app/host/HostController.hpp`, `app/host/HostController.cpp`, host/QML integration test, `app/qml/**`, docs/benchmarks |

No two agents edit the same file concurrently. If an interface must change, report it to the
coordinator before editing another owner's file.

## Shared symbol model

`AstSymbol` keeps existing fields and adds:

```cpp
enum class AstSymbolRole : std::uint8_t {
    Unknown = 0, Namespace, Type, Function, Method, Constructor, Destructor, Operator,
    Variable, Field, Parameter, Enumerator, TypeAlias
};

AstSymbolRole role{AstSymbolRole::Unknown};
bool isDeclaration{false};
bool isDefinition{false};
bool isTemplate{false};
bool isStatic{false};
bool isConstexpr{false};
bool isInline{false};
bool isVariadic{false};
bool isReference{false};
std::uint16_t parameterIndex{0};   // 1-based for parameters, 0 otherwise
std::string signatureKey{};        // normalized syntactic overload key
std::string qualifiers{};          // e.g. "const noexcept"
```

`signatureKey` is syntactic only: qualified name + `(` + comma-separated declared parameter type
syntax with parameter names/defaults removed + `)` + qualifiers. It must not claim compiler-resolved
types or template instantiation. Matching declaration/definition sites may share a `signatureKey`;
that is evidence for a relationship, not proof of semantic equivalence.

## Shared index interface

```cpp
struct SymbolQueryResult {
    std::vector<AstSymbol> symbols;
    bool truncated{false}; // true when at least one eligible result was omitted
};

struct CodebaseSnapshotInfo {
    std::uint64_t generation{0};
    bool complete{false};   // last full scan completed
    bool available{false};  // a bound workspace/generation exists
};

// New/changed methods:
core::Result<CodebaseIndexStats> refreshPaths(
    const WorkspaceContext& workspace,
    const std::vector<std::string>& relativePaths,
    const CodebaseIndexLimits& limits = {});
core::Result<SymbolQueryResult> findByName(
    std::string_view name, std::string_view parentSymbolId = {},
    std::string_view filePath = {}, std::size_t limit = 16,
    bool allowFtsFallback = true) const;
core::Result<SymbolQueryResult> findQualified(std::string_view qualifiedName,
                                              std::size_t limit = 64) const;
core::Result<SymbolQueryResult> children(std::string_view parentSymbolId,
                                         std::size_t limit = 64) const;
core::Result<bool> validateFileHash(std::string_view relativePath,
                                    std::string_view expectedHash) const;
core::Result<CodebaseSnapshotInfo> snapshotInfo() const;
```

Query implementations fetch `limit + 1` rows and set `truncated` when the extra row exists. A
truncated result must never be used to prove uniqueness or absence. Exact queries remain
workspace-bound and generation-bound.

### Snapshot generations

Schema version is now 5. Retain `workspaces.generation`,
`workspaces.last_refresh_complete`, `files.validated_generation`, and `ast_symbols.validated_generation`.
Add `files.snapshot_generation`, `files.dirty` (default 0) and
`workspaces.coverage_complete` (default 0).

- Full refresh: allocate a new generation; only visited files get the new
  `snapshot_generation`/`validated_generation`; a complete pass deletes unseen files, an incomplete
  pass leaves them at older generation and does not expose them.
- `refreshPaths`: allocate a new generation; carry forward only rows that were eligible in the
  previous generation (`snapshot_generation == previous generation && dirty == 0`), then re-read and
  revalidate only the requested paths. Unrelated unchanged files remain eligible. Retained rows from
  an older incomplete pass are not resurrected. Missing/unreadable requested files are invalidated.
  The pass is not a full-scan completeness claim.
- Queries join `files` and require `snapshot_generation = workspace.generation` and `dirty = 0`.
  `validated_generation` records actual evidence validation and remains useful for diagnostics.
- `validateFileHash` reads a workspace-relative file (read-only, bounded by the existing file-size
  limit), hashes it, and compares without mutating the index.
- `snapshotInfo.complete = fullScanComplete && coverageComplete`; incremental paths cannot turn an
  incomplete full scan into a complete one. `unprocessedPaths` on the stats record requested paths
  omitted by the pass. `SymbolQueryResult.complete` propagates coverage independently of
  `truncated`.

## Shared grounding metadata

`AstGroundingEvidence` keeps existing fields and adds:

```cpp
bool complete{true};       // false when a candidate query was truncated/failed or the snapshot changed
bool hashValidated{true};  // host-set; false when selected file bytes changed
std::string matchReason{}; // "qualified", "scope", "file", "parameter", "signature", "active-file", "unique"
```

`AstGroundingEngine::groundIntents` must use the richer `AstSymbol` fields and
`SymbolQueryResult::truncated`. Supported exact request forms are documented in the report:
qualified names, `in <scope>`, `file <relative-path>` in the condition, parameter name/`#position`
through the parameter property/value, and explicit overload signatures such as
`Engine::run(int,double)`. Focus is only a tie-breaker; it cannot override explicit scope, file,
signature, or parameter constraints. Any truncated candidate set that could change the result stays
`Ambiguous` with `complete=false`; it is not `Verified` or `NotFoundInScope`.

`computeAstSnapshotHash` remains the proposal v2 evidence hash. It must continue to bind
workspace identity, file hash, byte span, signature, and remain empty when nothing is verified.
Ungrounded proposals keep v1 compatibility.

## Host integration contract

`GroundedPromptContext` adds a `std::vector<GroundedEvidenceView>` (coordinator-owned type) with
target, qualified target, signature, relative file, line, status, match reason, completeness,
freshness, and candidates. The host:

- starts a bounded `CodebaseWatcher` after consent/workspace selection;
- debounces file/directory events and schedules `refreshPaths` or a full refresh;
- keeps ready evidence available during a paths refresh but rejects callbacks from superseded
  workspace/serial tokens;
- before promoting a grounded context, calls `validateFileHash` for every verified evidence file
  and checks `snapshotInfo().generation` did not change during enrichment; on mismatch it withholds
  the context and schedules a path refresh;
- invalidates the pending proposal/approval when the AST snapshot changes, without mutating saved
  history;
- keeps explicit refresh and workspace reselection working, and never blocks the UI thread on
  filesystem parsing.

Watcher notifications schedule validation; they do not establish truth. Unsaved editor buffers are
outside this milestone; grounding reflects saved files only.

## Final implementation notes (2026-11-05)

- `CodebaseIndex` schema v5 is implemented with v3→v4→v5 and v4→v5 migrations. Coverage is
  separate from full-scan completeness; dirty/unprocessed competitors cannot authorize uniqueness
  or absence.
  `refreshPaths` carries forward only previous-generation eligible rows rather than every historical
  row in the workspace.
- `CodebaseWatcher` is a recursive `QFileSystemWatcher` with a `250 ms` default debounce, bounded
  watched/pending sets, parent-directory rescans for atomic saves and renames, periodic fallback,
  and explicit overflow/failure signals. It is registered in `ROCLINT_HOST_SOURCES` with AUTOMOC.
- The host retries an incomplete full scan at most twice within the unchanged per-pass budget.
  Unchanged files are hash-skipped, so the retry completes without re-parsing them. If retries are
  exhausted, grounding is withheld until an explicit/later refresh succeeds.
- `GroundedPromptContext` carries `available`, `complete`, `validated` and `snapshotGeneration`
  alongside the evidence views. The host opens a worker-owned index connection for grounding and
  hash validation; the UI thread only applies the bounded view data.
- Grounding is revalidated after model generation and on every successful refresh. A changed
  selected file invalidates the current pending proposal/approval; historical records are not
  mutated.
- Grounded prompt XML is capped at **350 bytes**, a valid worst-case 350-token bound for the pinned
  byte-level BPE; signatures default to `300` bytes and remain a separate sub-limit. The renderer
  exposes `PromptRenderResult::rendered`; the host hashes/displays only that emitted subset, so
  omitted evidence cannot enter proposal v2. New evidence metadata never enters the hash.
- Proposal revalidation re-resolves the bound prompt/active file and compares the rendered evidence
  and hash, with serial/proposal/session/revision/workspace guards on delayed results.
- A running watcher is retained when unhealthy; stopped-watcher recovery is bounded to three
  backoff attempts before stable degraded mode. Explicit refresh is the deliberate recovery path.

Measured results and remaining limitations are recorded in
[`ast_reliability_completion.md`](ast_reliability_completion.md).

### Second boundary review (2026-11-05)

See [`ast_workspace_recovery_interface.md`](ast_workspace_recovery_interface.md) for the detailed
contracts added after the next review:

- `PipelineTicket`/`GroundingInputs`/`GroundedPromptContext` carry `workspaceEpoch`; a different
  workspace or deliberate active-file change increments it, invalidates derived proposal/approval
  state and makes old callbacks stale. A→B→A cannot revive an older epoch.
- Unavailable/zero-generation current revalidation invalidates the bound proposal; a readable but
  different generation is superseded.
- Verified rendering requires a useful target identity (complete or explicitly `...`-marked);
  otherwise the record is omitted from `PromptRenderResult::rendered`, the AST hash and HUD.
- `GroundedPromptContext::stalePaths` drives bounded, coalesced path recovery (max three attempts
  per path, reset by validated grounding). Healthy watchers also arm the existing five-minute
  reconciliation cadence. The stale request remains withheld.
