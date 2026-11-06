# Roclint AST grounding reliability — completion report

**Date:** 2026-11-05  
**Repository:** `D:\Stardust\roclint`  
**Branch:** `feat/codebase-decoupling`  
**Baseline:** `240c74f01b7d66d37c7c1b325d51c4c773a28718`  
**Final state:** working tree on the same branch; changes are uncommitted and not pushed.  
**Environment:** Windows 11 Home 10.0.26300, AMD Ryzen 7 7840HS, 16 GB RAM, MSVC 2022 C++20, Qt 6.8.2 (`msvc2022_64`), Tree-sitter runtime `da6fe9b`, C++ grammar `f41e1a0`, SQLite 3.46.1 with FTS5, Python 3.13.3.

> **Scope:** AST extraction, indexing, saved-file freshness, exact target resolution and the existing
> host/HUD flow. Packaging, installers, visual redesign, embeddings, vector retrieval, episodic
> journals and automatic source modification remain out of scope. macOS-native verification is
> **NOT RUN**.

---

## 1. Acceptance summary

| Gate | Result |
|---|---|
| Integrated Release build | **PASS** — exit `0`, first-party warnings-as-errors |
| CTest | **PASS** — `50/50`, `210.07 s` |
| Python tests | **PASS** — `49` run, `OK (skipped=1)`, `20.152 s` |
| Structure continuity | **PASS** — `94/94` original paths, `0` errors, `258` additions, `352` source files |
| Nine-case verification probe | **PASS** — `9/9`, exit `0` |
| Grounding corpus | **PASS** — `13/13`, coverage `1.0`, wrong `Verified` `0`, status mismatches `0` |
| Ambiguity preservation | **PASS** — `1/1` genuine ambiguity retained |
| Freshness/host integration | **PASS** — changed bytes, new competitors and incomplete coverage withhold/clear evidence and proposals |
| Workspace/scope epochs | **PASS** — old proposals, approvals, discovery/rewrite tickets and grounding views are superseded |
| Failed revalidation | **PASS** — unavailable current validation clears the bound proposal instead of being treated as superseded |
| Renderer identity | **PASS** — no empty/unidentified Verified record enters XML, rendered subset or hash |
| Stale recovery | **PASS** — hash-detected stale paths trigger coalesced, bounded reindexing |
| Watcher scenarios | **PASS** — modification, atomic save, rename, deletion, burst, new directory, overflow, forced failure, healthy reconciliation |
| Incomplete-scan recovery | **PASS** — coverage flags withhold authority; bounded retries and explicit recovery |
| Proposal v1/v2 compatibility | **PASS** — existing hash/payload/approval suites remain green |
| Body-free signatures / C10 budget | **PASS** — no bodies; complete prompt block `≤350` bytes, verified `≤350` Granite tokens |
| Review reproductions | **PASS** — all four corrected; see Section 8 |
| macOS build/CTest/UI/Metal | **NOT RUN** — no reachable Mac or macOS runner |

---

## 2. Delivered behavior

### 2.1 Freshness and snapshot model

Schema version is now **5**:

- `files.snapshot_generation` and `files.dirty` define current snapshot membership.
- `workspaces.last_refresh_complete` records full-scan completeness;
  `workspaces.coverage_complete` records whether a paths pass has covered its requested set.
  `CodebaseSnapshotInfo.complete` requires both; `fullScanComplete` and `coverageComplete` are
  exposed separately.
- Existing `workspaces.generation`, `files.validated_generation` and
  `ast_symbols.validated_generation` are retained.
- v3→v4→v5 and v4→v5 migrations preserve unrelated tables. v4 coverage is derived from the last
  full-scan claim and dirty rows; previous cached evidence is invalidated where structural columns
  are missing.
- Queries require the bound canonical workspace, `snapshot_generation = workspaces.generation`, and
  `dirty = 0`. Retained older rows can exist for recovery but cannot surface as evidence.
- `SymbolQueryResult.complete` is false whenever current coverage is incomplete; it is independent
  of caller-limit `truncated`.

`refreshPaths` semantics:

1. Allocate a new generation.
2. Carry forward only rows that were eligible in the previous generation
   (`snapshot_generation == previous generation && dirty == 0`).
3. Re-read and revalidate only the requested workspace-relative paths.
4. Invalidate missing, unreadable, oversized or invalid requested paths.
5. Record omitted paths in `CodebaseIndexStats.unprocessedPaths`, mark them dirty, and set
   `coverage_complete=false`. A paths pass never changes `last_refresh_complete`, so it cannot
   repair an incomplete full scan.
6. Keep unrelated unchanged files eligible; a generation advance alone cannot hide them.
7. Remain incomplete while any older dirty row is still hidden, so an unrelated paths pass cannot
   clear protection before those rows are revalidated.

A full refresh still allocates a new generation, revalidates visited files, and deletes unseen files
only on a complete pass. The 2-second scan budget, 5,000-file cap and 2 MiB per-file cap are
preserved. The host performs a bounded automatic retry (maximum two retries) when a first pass is
incomplete because unchanged files are hash-skipped on the next pass. If the workspace still cannot
finish within the budget, grounding remains withheld and the explicit refresh hook remains available.

Measured complete operation is reported rather than splitting the scan budget from the work:
`complete_index_ms` includes every pass needed to finish the snapshot.

### 2.2 Saved-file watcher

`CodebaseWatcher` uses `QFileSystemWatcher` with:

- recursive directory watching and individual source-file watches;
- the same exclusion/source-extension policy as the index;
- debounced batches (`250 ms` default) with duplicate suppression;
- bounded watched directories/files and a bounded pending-path set;
- atomic-save handling through parent-directory rescans and file-watch re-arming;
- full-refresh escalation for directory changes, removed directories, watch overflow and watcher
  failure;
- a periodic fallback refresh timer;
- explicit `pathsChanged`, `fullRefreshRequired`, `watchOverflowed` and `watchFailed` signals.

Notifications never establish truth. They schedule `refreshPaths`; the index re-reads and hashes the
named files before they can be queried. The host rejects callbacks from superseded workspace or
serial tokens. Full and incremental scans run through `QtConcurrent`, never on the UI thread.

Watcher recovery is stable and bounded:

- a running partial watcher is retained even when it reports overflow or a registration failure;
  its fallback timer provides a controlled refresh cadence instead of a restart loop;
- a stopped watcher is retried at most three times with backoff, then enters stable degraded mode;
- explicit refresh/workspace reselection deliberately restarts the watcher once and resets recovery;
- pending recovery is cancelled by workspace switching, shutdown or a superseding callback;
- watcher startup traversal is bounded by the existing limits and is not repeated on every scan.

### 2.3 Structural symbols and extraction

`AstSymbol` adds:

- `AstSymbolRole`: namespace, type, function, method, constructor, destructor, operator, variable,
  field, parameter, enumerator, type alias;
- declaration/definition, template, static, constexpr, inline, variadic and reference flags;
- 1-based `parameterIndex`;
- syntactic `signatureKey`;
- normalized function `qualifiers`.

`signatureKey` is explicitly syntactic: qualified name plus declared parameter type syntax and
qualifiers. It does not claim compiler-resolved types, typedef expansion, template instantiation,
macro expansion, overload resolution, runtime dispatch or complete C++ name resolution.

Extraction coverage now includes:

- namespace/global variables, including multiple declarators;
- anonymous namespace contents with file-local identity;
- out-of-class qualified method definitions;
- constructors, destructors and operators;
- representative templates and function-pointer declarations;
- full parameter metadata and overload-distinguishing keys;
- partial recovery that keeps a valid declaration/signature when only its implementation body is
  malformed;
- conservative omission of unsupported macro-generated, typedef-alias, friend, union and
  function-returning-function-pointer forms.

Bodies are never persisted or injected. Evidence remains bound to the actual file hash and byte
span; declaration/definition association is only a syntactic clue and never replaces those bindings.

### 2.4 Exact grounding

Supported exact requests:

- qualified names (`demo::Engine::run`);
- explicit scopes (`in Engine`, `within`, `inside`);
- explicit files (`file src/engine.cpp`);
- parameter selection by name or `#position` through property `parameter`;
- explicit syntactic overload signatures (`Engine::run(int,double)`).

Explicit file, scope, signature and parameter constraints are applied before ranking. Focus/active
file is only a tie-breaker and cannot override an explicit constraint. Fuzzy/FTS matches are never
promoted to `Verified`. Truncated or failed candidate queries, incomplete coverage and changed
snapshots produce `Ambiguous` with `complete=false`, not `Verified` or `NotFoundInScope`. An omitted
or dirty competitor can therefore never turn ambiguity into false uniqueness, and an incomplete
universe cannot prove absence.

`AstGroundingEvidence` carries `complete`, host-set `hashValidated`, and stable `matchReason`.
`computeAstSnapshotHash` retains the proposal-v2 domain: workspace identity plus verified file hash,
byte span and signature. The host hashes the `PromptRenderResult::rendered` subset, so evidence
omitted by the 350-byte prompt bound cannot enter the proposal hash. Ungrounded proposals keep the
v1 domain unchanged.

### 2.5 Host and HUD integration

- Grounding now runs in a worker-owned SQLite connection. The UI thread only captures inputs and
  applies the resulting bounded evidence views.
- Before promoting a grounded context, the host validates every verified evidence file hash and
  checks snapshot-generation coherence. If bytes changed, grounding is withheld and a refresh is
  scheduled.
- A rewrite is revalidated after model generation; if selected evidence changed during writing, the
  proposal is discarded instead of being promoted from a mixed snapshot.
- Refresh completion re-resolves the proposal's bound prompt and active file, then compares the
  current evidence and AST hash with the rendered evidence used to create it. New competing
  candidates, candidate/scope removal or incomplete coverage invalidate the current pending proposal
  and approval; unrelated changes preserving equivalent evidence do not. Validation callbacks and
  queued invalidations are bound to the workspace, session, revision, proposal and serial so an
  older result cannot invalidate a newer state. Saved historical approvals are not mutated.
- The existing HUD exposes `groundingStatus` and `groundedEvidence` under Technical details:
  status, qualified target, signature, relative file and line, match reason, completeness and stale
  state. No visual redesign was performed.
- Direct Full Rewrite, optional clarification, Skip Questions and best-effort preview behavior are
  unchanged. Roclint still inserts prompt text only; it never applies source changes or presses
  Send/Enter.

### 2.6 Privacy and budgets

- Source files are opened read-only; exclusions and canonical workspace boundaries are preserved,
  including symlink/reparse escapes.
- No source bodies, prompt contents or candidate source text enter routine diagnostics.
- The complete grounded prompt block is capped at **350 bytes**, a mathematically valid worst-case
  bound for the pinned byte-level BPE vocabulary because every token covers at least one UTF-8 byte.
  This makes `≤350` bytes guarantee `≤350` Granite tokens including framing, attributes, paths,
  signatures and candidates. The signature allowance remains a separate sub-limit (default 300
  bytes) and can never widen the total. Long values are compacted at XML/UTF-8 boundaries.
- The pinned tokenizer verification fixture renders `350` bytes and tokenizes to `182` Granite
  tokens; Unicode/escaped and ambiguity fixtures tokenize to `113` and `111`.
- Index, parser and grounding failures return unavailable/incomplete evidence rather than blocking
  ordinary widget invocation.

---

## 3. Specialist contributions

| Specialist | Contribution |
|---|---|
| **A — freshness/indexing** | Schema v5 coverage semantics, `refreshPaths` with `unprocessedPaths`, snapshot membership/dirty filtering, completeness-aware `SymbolQueryResult`, `validateFileHash`, `snapshotInfo`, `CodebaseWatcher`, workspace-serial refresh integration, bounded incomplete retries, freshness tests. |
| **B — extraction/symbols** | `AstSymbolRole` and structural flags, `parameterIndex`, syntactic `signatureKey`, qualifiers, variable/multi-declarator coverage, anonymous namespaces, out-of-class definitions, constructors/destructors/operators, templates, function pointers, partial recovery and body-free tests. |
| **C — grounding/evaluation** | Exact qualified/scope/file/signature/parameter resolution, focus tie-breaking, truncation/coverage-aware completeness, evidence metadata, 350-byte worst-case token bound, rendered-subset hashing, generation-coherence checks, 13-case annotated corpus and evaluation. |

No two agents edited the same file concurrently. `CanonicalIntent.hpp` was additively extended by
Specialist C because `AstGroundingEvidence` is defined there; the coordinator reviewed and retained
that change.

---

## 4. Verification evidence

### 4.1 Exact commands

```powershell
# Configure and build (Visual Studio 2022 generator; CMake from the VS installation)
cmake -S . -B build
cmake --build build --config Release

# Registered tests
ctest --test-dir build -C Release --output-on-failure

# Python and structure checks
python -m unittest discover -s tools -p "test_*.py"
python tools/check_structure.py --root .

# Previous grounding reproductions
build\Release\report-verification-probe.exe

# Corpus/extraction suites
build\Release\roclint_ast_grounding_tests.exe
build\Release\roclint_ast_symbol_extractor_tests.exe
build\Release\roclint_codebase_index_freshness_tests.exe
build\Release\roclint_codebase_watcher_tests.exe
build\Release\roclint_ast_freshness_integration_tests.exe
build\Release\roclint_ast_token_budget_tests.exe

# Review reproductions (probe sources under build/)
cmd /c build\ast-code-review-probes.cmd
build\ast-code-review-probe.exe
cmd /c build\ast-code-review-extra-probes.cmd
build\ast-code-review-token-probe.exe models\granite-4.2-3b\granite-4.2-3b-Q8_0.gguf build\ast-code-review-context.xml
build\ast-code-review-host-probe.exe

# Repo-scale measurements
build\Release\ast-grounding-benchmark.exe D:\Stardust\roclint src\bridge\GraniteAdapter.cpp
```

Logs are retained under `build/`:

- `ast-review-final-build.log`
- `ast-review-final-ctest.log`
- `ast-review-final-python.log`
- `ast-review-final-structure.json`
- `ast-review-final-probe.txt`
- `ast-review-final-benchmark.txt`
- `ast-review-final-reproduction-probe.txt`
- `ast-review-final-host-reproduction.txt`
- `ast-workspace-final-build.log`
- `ast-workspace-final-ctest.log`
- `ast-workspace-final-python.log`
- `ast-workspace-final-structure.json`
- `ast-workspace-final-probe.txt`
- `ast-workspace-additional-host.txt`
- `ast-workspace-unavailable-edge.txt`
- `ast-workspace-render-identity.txt`
- `ast-workspace-prev-probe.txt`
- `ast-workspace-prev-host-probe.txt`
- `ast-workspace-roclint_ast_freshness_integration_tests.txt`
- `ast-workspace-roclint_ast_stale_recovery_tests.txt`
- `ast-workspace-roclint_codebase_watcher_tests.txt`

### 4.2 Test and structure results

```text
100% tests passed, 0 tests failed out of 50
Total Test time (real) = 210.07 sec

Ran 49 tests in 20.152s
OK (skipped=1)

structure=True preserved=94/94 errors=0 additions=258 source_files=352
```

The single Python skip is the platform-specific process-guardian test on Windows; it is unchanged.

### 4.3 Corpus result

```text
ast corpus categories ambiguous=1 file=1 missing=2 overload=2 parameter=2 scope=1 stale=1 supported=2 unsupported=1
ast corpus total=13 correct=13 coverage=1 wrong_verified=0 status_mismatches=0 ambiguous=1/1 unsupported=1 missing=2 grounding_ms=3.9584 elapsed_ms=300.504
AST grounding average=0.198749 ms
AST extraction average=0.097589 ms
token_budget|case=long_identifier_and_path|bytes=350|granite_tokens=182|rendered=1
token_budget|case=unicode_and_escaped_xml|bytes=350|granite_tokens=113|rendered=1
token_budget|case=multiple_ambiguity_candidates|bytes=340|granite_tokens=111|rendered=1
```

The stale corpus case deliberately validates snapshot-valid evidence against changed bytes and
expects the host to withhold promotion. It is not counted as a wrong `Verified`.

### 4.4 Freshness scenarios

`roclint_codebase_index_freshness` covers:

- full index → single-path refresh preserving unrelated files and advancing generations;
- modified/deleted/new files, atomic-save replacement;
- incomplete full scan followed by incremental refresh staying incomplete and hiding old rows;
- workspace isolation and failed-rebind fail-closed behavior;
- hash validation true/false/missing/malformed/absolute/`..`/oversized and no mutation;
- query truncation (`limit + 1`), exactness versus FTS widening, and coverage completeness;
- the review's `Ambiguous → partial refresh` reproduction, false-absence case, database reopen,
  complete recovery and unchanged-file carry-forward;
- v3→v4→v5 and v4→v5 migrations preserving unrelated tables;
- pure watcher batching/overflow/clearing.

`roclint_codebase_watcher` covers real watcher modification, atomic save, rename, deletion, burst,
new-directory events, partial overflow staying running, and forced registration failure recovery.

`roclint_ast_freshness_integration` covers:

- host grounding of a saved workspace;
- saved-byte change without refresh → empty grounding and `STALE`;
- explicit refresh → grounded again;
- incomplete `maxFiles=1` paths refresh staying non-authoritative and recovering;
- changed saved evidence clearing a pending v2 proposal;
- a competing candidate in another file clearing the proposal while the selected file is unchanged;
- an unrelated source change preserving equivalent grounding and the proposal;
- relevant candidate removal clearing the proposal;
- a superseded revalidation result not invalidating a newer proposal;
- evidence changing during model generation discarding the proposal;
- an always-incomplete scan withholding grounding, then explicit refresh recovering;
- permanent watcher overflow and forced registration failure staying bounded, with explicit recovery;
- workspace switching and shutdown cancelling pending watcher recovery;
- missing consent leaving ordinary prompt help available;
- UI-side evidence application measured at `0.0221 ms`.

`roclint_ast_token_budget` loads the pinned Granite Q8_0 vocabulary (`vocab_only`) and asserts the
rendered XML is `≤350` bytes and `≤350` tokens for the review fixture, Unicode/escaped XML and
multiple ambiguity candidates.

### 4.5 Performance and resources

Command: `ast-grounding-benchmark.exe D:\Stardust\roclint src\bridge\GraniteAdapter.cpp`

| Measurement | Result |
|---|---|
| Cold complete index | `171` files, `171` indexed, `34` recovered, `7,567` symbols, `1.34–1.42 s` |
| Unchanged full refresh | `619–641 ms` for `171` files, `0` re-parsed symbols |
| Single-file `refreshPaths` | `98.7–100.3 ms` (includes the global FTS rebuild) |
| Selected-file hash validation | `0.83–0.89 ms` per file |
| Warm exact lookup | `0.088–0.093 ms` |
| Full grounding, four intents | `0.318–0.336 ms` |
| Prompt XML render | `323` bytes, `0.005–0.006 ms` |
| Tokenizer verification | `350` bytes → `182` Granite tokens (review fixture) |
| UI-side evidence application | `0.0243 ms` for the integration fixture |
| Benchmark peak working set | `12.9 MB` sampled |
| Benchmark CPU time | `≈2.6 s` |
| Corpus evaluation | `3.751 ms` for 13 cases (`274.481 ms` including fixture setup) |

The single-file refresh is dominated by rebuilding the FTS table for the whole index. That cost is
measured and documented; correctness and coverage were prioritized over an FTS incremental-update
change in this milestone.

One first invocation immediately after a full rebuild was cold-cache limited: it visited `116`
files at `2,135.96 ms` and reported incomplete. That is why the host has a bounded two-retry path
and why the benchmark reports `complete_index_ms` including retries; the table uses the subsequent
complete runs. No measurement is hidden behind the scan timer.

---

## 5. Unsupported cases and remaining limitations

1. **C/C++ only.** Other languages remain outside the indexer.
2. **Saved files only.** Unsaved editor buffers are not reflected; grounding reflects bytes on disk.
3. **No semantic C++ analysis.** No compiler-resolved types, template instantiation, macro
   expansion, overload resolution, call graph or local-variable indexing.
4. **Exact matching only.** Fuzzy/FTS matches never become `Verified`.
5. **Unsupported declarations are omitted** rather than guessed: macro-generated declarations,
   typedef aliases, `using` declarations/directives, namespace aliases,
   function-returning-function-pointer declarators, friend declarations, anonymous aggregate
   members, union definitions and K&R definitions.
6. **Anonymous namespaces** use the synthetic segment `(anonymous namespace)`. Cross-file name
   lookup can remain ambiguous; no file-local alias is invented.
7. **Template arguments in qualified scopes are dropped.** Explicit specializations may share a
   qualified name/signature key and remain conservatively ambiguous. Explicit template arguments in
   grounding requests are unsupported and fail closed.
8. **Signature syntax limits.** Parameter names, default arguments, nested parentheses, function
   pointer parameter syntax and trailing qualifiers after the closing `)` are not accepted in
   explicit overload requests. `requires` clauses and trailing return types are not part of
   `signatureKey`; typedef spellings and top-level `const` differences remain syntactic.
9. **Out-of-class parent links** are set only when the named scope is indexed in the same file.
   Otherwise the qualified name is retained and the parent link is left empty.
10. **Function spans** currently cover the declaration/definition site, not a separate
    declaration-only body-excluded range. Bodies are never stored.
11. **Incomplete scans.** Full scans retry at most twice; incomplete paths refresh retries only its
    returned unprocessed paths at most twice. Any remaining dirty row keeps coverage incomplete.
    The host withholds grounding until a later successful pass. The library exposes separate
    `fullScanComplete`/`coverageComplete` flags and `SymbolQueryResult.complete`.
12. **Global FTS rebuild** on each refresh remains the dominant single-file update cost.
13. **Watcher limits.** Very large workspaces retain a partial watcher and its fallback cadence; a
    stopped watcher enters bounded backoff and then stable degraded mode. Network filesystems and
    platform-specific overflow behavior are not stress-tested.
14. **No Tree-sitter incremental parsing.** Changed files are reparsed; parser/tree reuse was not
    implemented because the measured lookups and single-file scan did not justify the ownership and
    verification cost.
15. **Worst-case token bound.** The 350-byte cap guarantees the C10 token ceiling but is stricter than
    the tokenizer's actual density (`350` bytes → `182` tokens in the review fixture).
16. **Rendered field truncation.** XML fields may be compacted by middle truncation under the byte
    bound; the rendered intent retains the full evidence for the proposal hash, so the hash is
    conservative with respect to compacted display text.
17. **No native macOS validation.** See below.

---

## 6. macOS status

No Mac or macOS runner was available. The following are **NOT RUN**, not failed:

- native macOS Release build and CTest;
- macOS QML/native HUD launch and accessibility interaction;
- macOS watcher behavior under FSEvents/QFileSystemWatcher;
- Metal cold/warm benchmarking and KV-reuse parity;
- app-bundle deployment, signing and notarization (also out of milestone scope).

The shared implementation remains portable C++20/Qt with platform guards. Windows results do not
establish native macOS verification. The existing macOS Phase 4/5 parity report and its evidence are
preserved unchanged.

---

## 7. Final diff and review state

The working tree contains only the intended source, test, benchmark and documentation changes:

- index/schema/watcher: `CodebaseIndex` (v5 coverage), `CodebaseWatcher` (stable degraded recovery),
  `HostControllerWorkspace`;
- symbols/extraction: `AstSymbol`, `AstSymbolExtractor`;
- grounding: `AstGroundingEngine` (coverage gating, rendered subset, 350-byte bound), additive
  `AstGroundingEvidence` fields;
- host/HUD: `HostController` (proposal re-resolution and generation binding),
  `HostControllerWorkspace`, `IntentReviewPanel.qml`;
- tests: extraction, grounding/corpus, freshness/coverage, watcher recovery, host integration,
  stale-path recovery, pinned-tokenizer budget;
- workspace/scope epochs, failed revalidation, renderer identity and stale recovery contract:
  `docs/audit/ast_workspace_recovery_interface.md`;
- structure allowlist for the review-owned `docs/plans` addition;
- benchmark, review remediation interfaces and completion documentation.

The macOS parity audit and all `docs/research/` documents/reference images are untouched. No commit
or push was performed.

---

## 8. Post-review remediation — four findings

**Review artifact:** `build/ast-code-review.md`  
**Remediation interface:** `docs/audit/ast_review_remediation_interface.md`

| Review finding | Fix | Regression evidence |
|---|---|---|
| **1 [P1] Incomplete incremental refresh could prove false uniqueness** | Schema v5 adds `workspaces.coverage_complete` separate from `last_refresh_complete`. `refreshPaths` records `unprocessedPaths`, marks them dirty, sets coverage incomplete, and never changes the full-scan flag. `SymbolQueryResult.complete` and separate snapshot flags propagate coverage. The host requires strict snapshot completeness, retries only unprocessed paths (max two), and withholds otherwise. The resolver downgrades all evidence from an incomplete snapshot/query to `Ambiguous`/`complete=false` and emits no AST hash. | `testIncompletePathsRefreshCannotProveUniqueness`, `testIncompletePathsRefreshDoesNotProveAbsence`, `testV4MigrationDerivesCoverage`, `roclint_ast_freshness_integration` incomplete paths test, review probe. |
| **2 [P2] Pending proposals survived new ambiguity** | Proposal creation stores the rendered evidence, prompt, active file, proposal/session/revision IDs and AST hash. Refresh re-resolves the bound prompt and compares current evidence/hash, not just selected-file bytes. Invalidation results are bound to a validation serial plus workspace/proposal/session/revision and rechecked inside the queued invalidation. Equivalent unrelated changes preserve the proposal; new competitors/removals/incomplete coverage clear it. | `testCompetingCandidateInvalidatesPendingProposal`, `testUnrelatedChangePreservesPendingProposal`, `testRelevantCandidateRemovalClearsProposal`, `testStaleRevalidationCannotInvalidateNewerProposal`, `testPostGenerationEvidenceChangeDiscardsProposal`, host review probe. |
| **3 [P2] Byte limit did not enforce the C10 token budget** | The complete document is capped at 350 bytes, a valid worst-case bound for the pinned byte-level BPE (one token covers at least one UTF-8 byte). Framing, attributes, paths, signatures and candidates are included. Long values are compacted at XML/UTF-8 boundaries; verified evidence is prioritized. The rendered subset is hashed and displayed, so omitted evidence cannot enter proposal-v2. | `testPromptRenderContextHashesOnlyRenderedSubset`, review/Unicode/budget renderer tests, `roclint_ast_token_budget` using the pinned Granite vocabulary. |
| **4 [P2] Watcher overflow created a refresh/restart loop** | A successful scan retains a running partial/unhealthy watcher and its fallback cadence. A stopped watcher gets at most three backoff attempts, then stable degraded mode. Explicit refresh/workspace reselection deliberately restarts once and resets recovery. Pending recovery is cancelled by workspace switch, quit or superseding callbacks. | Host overflow/failure/switch/quit tests and watcher partial-overflow/forced-failure tests; native review host probe. |

### 8.1 Corrected review reproductions

```text
incremental_incomplete|initial=Ambiguous|refresh_incomplete=1|snapshot_complete=0|after=Ambiguous|evidence_complete=0|hash_bytes=0
duplicate_candidate|proposal_present=0|current_ambiguous=1
context_budget|bytes=350|granite_tokens=182
watcher_overflow|directories=2050|refresh_starts_in_10s=2
```

The token-budget test also reports:

```text
token_budget|case=long_identifier_and_path|bytes=350|granite_tokens=182|rendered=1
token_budget|case=unicode_and_escaped_xml|bytes=350|granite_tokens=113|rendered=1
token_budget|case=multiple_ambiguity_candidates|bytes=340|granite_tokens=111|rendered=1
```

The partial watcher is retained after overflow, so the 2 starts are the initial scan and the single
notification-triggered scan, not one per successful refresh.

### 8.2 Fresh verification

- Release build: **PASS**, exit `0`.
- CTest: **PASS**, `50/50`, `210.07 s` (includes the pinned-tokenizer, workspace-switch,
  renderer-identity, stale-recovery and 10-second overflow observations).
- Python: **PASS**, `49` tests, one platform skip, `20.152 s`.
- Structure: **PASS**, `94/94`, zero errors, `258` additions, `352` source files.
- Verification probe: **PASS**, `9/9`.
- Corpus: **PASS**, `13/13`, coverage `1.0`, wrong `Verified` `0`, status mismatches `0`.
- Review probes: all four corrected as listed above.
- `git diff --check`: exit `0`, line-ending warnings only.

### 8.3 Performance impact

| Metric | Before remediation | After remediation |
|---|---|---|
| Repo size | 169 files / 7,280 symbols | 171 files / 7,567 symbols |
| Cold complete index | 1.28–1.37 s | 1.34–1.42 s |
| Unchanged refresh | 613–616 ms | 619–641 ms |
| Single-file refresh | 90.6–106.6 ms | 98.7–100.3 ms |
| Hash validation | 0.52–0.57 ms | 0.83–0.89 ms |
| Four-intent grounding | 0.30–0.36 ms | 0.318–0.336 ms |
| Rendered XML | 998 bytes | 323 bytes (max 350) |
| Host integration suite | ~0.7 s | 23.26 s (includes deliberate 10 s overflow window) |

The repository grew by one source file and 201 symbols; coverage checks add a per-query snapshot
read. The measurements remain sub-2 s cold and sub-0.4 ms for four grounded intents. The token bound
trades renderer payload for a guaranteed C10 ceiling.

### 8.4 Remaining limitations

- The 350-byte worst-case bound is stricter than the actual tokenizer (`350` bytes → `182` tokens in
  the review fixture); a future optional tokenizer callback could use more of the 350-token budget.
- Rendered fields may be middle-truncated; the hash retains the full rendered evidence, so it is
  conservative rather than an exact hash of the truncated field bytes.
- Watcher startup still enumerates on the calling thread once per deliberate start/attempt; the
  restart loop is removed and retries are bounded.
- Global FTS rebuild, saved-files-only behavior and the other limitations in Section 5 remain.
- Native macOS build, CTest, HUD, watcher and Metal checks remain **NOT RUN**.

---

## 9. Second review remediation — workspace, revalidation, identity, stale recovery

**Review inputs:** `build/ast-additional-review.md`, `build/ast-remediation-rereview.md`.  
**Interface/ownership:** `docs/audit/ast_workspace_recovery_interface.md`.

| Finding | Fix | Regression evidence |
|---|---|---|
| **1 [P1] Workspace changes did not supersede old proposals/requests** | `PipelineTicket` and grounding contexts carry a workspace/scope epoch. A different workspace or a deliberate active-file change increments the epoch, immediately clears analysis/proposal/acknowledgment/approval/evidence state, persists approval invalidation through the existing repository path, and makes old tickets stale. Proposal approval additionally requires the proposal epoch to equal the current epoch. Discovery, rewrite, ambient, Julia and delivery callbacks reject old epochs before applying views or publishing. A→B→A gets a new epoch and cannot revive the old request. | `testWorkspaceSwitchInvalidatesProposalAndApproval`, `testWorkspaceSwitchDuringDiscoveryDoesNotPublishIntoNewWorkspace`, `testWorkspaceSwitchDuringRewriteDoesNotPublishIntoNewWorkspace`, `testWorkspaceSwitchBackDoesNotReviveOlderEpoch`, `testScopeChangeInvalidatesEvidenceAndProposal`; review probe output below. |
| **2 [P2] Failed current revalidation was treated as superseded** | Revalidation now distinguishes a readable result from another generation (superseded) from unavailable/zero-generation/current failure. After ownership checks, current failure invalidates the exact pending proposal/approval; generation-0 results are no longer dropped before equivalence. Queued invalidation rechecks serial/workspace/epoch/proposal/session/revision. Full and paths failure paths schedule revalidation; recovery remains possible. | `testUnavailableIndexFullRefreshFailureClearsProposalAndRecovers`, `testUnavailableIndexPathsRefreshFailureClearsProposalAndRecovers`, `testDelayedFailedRevalidationCannotClearNewerProposal`; edge probe output below. |
| **3 [P2] Verified evidence could have no target identity** | The renderer requires a non-blank requested target. Optional signature/symbol/location fields are dropped first; if the target cannot fit completely or as an explicitly `...`-marked compaction, the record is omitted. Omitted records are not added to `rendered`, the AST hash or HUD views. Unmarked prefix truncation is no longer used for identity. | `testPromptXmlVerifiedIdentityReviewReproduction`, `testPromptXmlVerifiedIdentityRequiresTarget`, `testPromptXmlVerifiedIdentityBudgetBoundaries`, `testPromptXmlVerifiedAndAmbiguousIdentity`, `testPromptXmlVerifiedLongUnicodeIdentity`; renderer sweep output below. |
| **4 [P2] Hash-detected staleness did not schedule recovery** | The grounding context returns deduplicated rendered-evidence paths whose saved hashes failed. After epoch validation, the host schedules bounded/coalesced path refresh through the existing 200 ms queue, with at most three attempts per path, reset by a validated grounding. In-flight scans are not overlapped; workspace/scope serial changes cancel old recovery. The stale result stays withheld; a later request uses refreshed evidence. A healthy watcher also arms the existing 5-minute reconciliation cadence for missed creation/deletion events. | `test_ast_stale_recovery`: automatic refresh, coalescing, three-attempt bound/reset, missing file, workspace switch; `test_codebase_watcher`: healthy fallback cadence plus existing overflow/failure bounds. |

### 9.1 Corrected reproductions

```text
workspace_switch|during_rewrite=0|new_workspace_has_no_run=1|ready=0|proposal=0|can_approve=0|status=UNAVAILABLE
unavailable_index|ready=0|proposal_present=0|can_approve=0|status=STALE
render_identity|no_empty_or_one_character_target_in_fixture_sweep
```

The during-rewrite switch and missed-notification cases are registered host tests rather than relying on
the diagnostic probe's old READY assertion. The stale-recovery suite demonstrates one coalesced scan
for repeated requests, exactly three scans for six stale cycles, and a validated-context reset.

### 9.2 Fresh verification

- Release build: **PASS**, exit `0`.
- CTest: **PASS**, `50/50`, `210.07 s`.
- Python: **PASS**, `49` tests, one platform skip, `20.152 s`.
- Structure: **PASS**, `94/94`, zero errors, `258` additions, `352` source files.
- Verification probe: **PASS**, `9/9`.
- Grounding corpus: **PASS**, `13/13`, coverage `1.0`, wrong `Verified` `0`.
- Previous four review reproductions remain corrected.
- `git diff --check`: exit `0`, line-ending warnings only.

One full-suite run hit a pre-existing timing-sensitive failure in `roclint_async_persistence`; five
direct reruns and the final full suite passed. No test was skipped, weakened or removed.

### 9.3 Performance impact

| Metric | Before second remediation | After second remediation |
|---|---|---|
| Repo size | 170 files / 7,482 symbols | 171 files / 7,567 symbols |
| Cold complete index | 1.37–1.78 s | 1.34–1.42 s |
| Unchanged refresh | 632–784 ms | 619–641 ms |
| Single-file refresh | 101–129 ms | 98.7–100.3 ms |
| Hash validation | 0.86–1.05 ms | 0.83–0.89 ms |
| Four-intent grounding | 0.35–0.51 ms | 0.318–0.336 ms |
| Rendered context | 323 bytes | 323 bytes |
| Host freshness integration | 23.26 s | 35.67 s (new epoch/switch/unavailable tests) |
| Stale recovery suite | n/a | 5.38 s |
| Watcher suite | 1.05 s | 4.98 s (healthy cadence + recovery) |

The epoch and stale checks add negligible lookup/index cost. The host-suite increase is deliberate
coverage: workspace switches, unavailable-index failure/recovery and delayed revalidation ordering.
The healthy watcher reconciliation is a five-minute cadence, not a keystroke-triggered scan.

### 9.4 Remaining limitations

- Workspace/scope epoch state is host-local and not persisted in proposal records. Restored proposals
  adopt the current epoch and remain subject to AST revalidation; historical approvals are never
  rewritten.
- A blank requested target is not replaced by the symbol name; fail-closed omission is intentional.
  Identity needs at least the marker plus one content byte (four escaped bytes) when truncated.
- Stale recovery attempts are tracked host-wide and cleared by any validated grounding; a stale path
  already queued by the watcher does not consume an attempt.
- Post-rewrite revalidation does not apply a stale context; recovery is scheduled by the next
  grounding request. It cannot publish stale evidence.
- Native macOS build, CTest, HUD, watcher/FSEvents and Metal checks remain **NOT RUN**.
