# Roclint Phase 6 — AST-Level Grounding Execution Report

**Date:** 2026-11-05  
**Repository:** `D:\Stardust\roclint`  
**Branch:** `feat/codebase-decoupling`  
**Baseline:** `0a17b8d` (Phase 5 sign-off, clean working tree)  
**Environment:** Windows 11, AMD Ryzen 7 7840HS, 16 GB RAM, NVIDIA RTX 4060 Laptop, MSVC 2022 C++20, Qt 6.8.2 (`msvc2022_64`), Tree-sitter runtime `da6fe9b`, Tree-sitter C++ grammar `f41e1a0`, SQLite 3.46.1 with FTS5.

> **Historical vs fresh evidence.** Sections 1–7 record earlier Phase 6 sign-offs and review
> remediations (`fbd2f78`/`49dc23a` and `767ae70`/`b0eb13b`). They are retained as historical
> evidence and are superseded by Section 8, which records the boundary remediation and fresh
> measurements. Do not cite their metrics as current.

---

## 1. Executive summary

| Gate | Result |
|---|---|
| Structure continuity | **PASS** — `94/94` original paths, `0` errors, `244` additions |
| Release build | **PASS** — full tree, exit `0` |
| CTest | **PASS** — `45/45`, `141.70 s` |
| AST extraction unit gate | **PASS** — hierarchy, spans, recovery, no function bodies |
| AST grounding unit gate | **PASS** — scope, ambiguity, hallucination, type mismatch, XML budget |
| Grounding lookup budget | **PASS** — `0.035 ms` average in-test; `0.086 ms` for four intents on the full repo (`0.021 ms`/intent) |
| Repo-scale index | **PASS** — `164` C/C++ files, `4,679` symbols, `30` recovered parses, `829 ms` cold index |
| Proposal v2 continuity | **PASS** — AST-bound proposals use v2 domain; ungrounded proposals retain v1 byte-for-byte |
| Verification probe | **PASS** — `9/9` cases, all sealed |
| Decision Contract v2 / GBNF / Job Objects | **UNCHANGED** — no sampling, grammar, contract-version or isolation change |

Phase 6 closes the lexical-only gap in Phase 5.2: canonical intents can now be matched to typed
Tree-sitter symbols before Granite is invoked, and unresolved or contradictory targets fail
closed instead of becoming model evidence.

---

## 2. Delivered architecture

### 2.1 Pinned native dependencies

`CMakeLists.txt` now builds:

- Tree-sitter C runtime `da6fe9beb4f7f67beb75914ca8e0d48ae48d6406`.
- Tree-sitter C++ grammar `f41e1a044c8a84ea9fa8577fdd2eab92ec96de02`.
- SQLite 3.46.1 amalgamation with `SQLITE_ENABLE_FTS5=1` and `SQLITE_THREADSAFE=1`.

Sources are fetched into `build/_deps` when absent and are compiled as third-party targets with
`/W3` so grammar tables do not trip first-party `/W4 /WX`. `roclint_storage` links them privately;
host and test targets keep first-party warning policy.

### 2.2 Typed symbol model

`include/roclint/storage/AstSymbol.hpp` defines:

- `AstNodeKind`: namespace, class, struct, function, method, parameter, field, enum, enumerator,
  type alias.
- `AstSymbol`: deterministic `symbolId`, name, qualified name, parent ID, workspace-relative path,
  source file SHA-256, byte span, 1-based line/column, signature, type name, constness.

Function and field bodies are never stored: `AstSymbolExtractor` truncates a signature at the
declaration body and caps it at 1,200 bytes before indexing.

### 2.3 Tree-sitter extraction

`AstSymbolExtractor` performs typed traversal rather than executing grammar queries at runtime.
The declarative query shapes are retained in `src/storage/tree_sitter_queries.def` as the reference
contract. The traversal:

1. Links namespaces → classes/structs/enums → methods/functions → parameters.
2. Resolves function declarators from nested pointer/reference/qualified forms.
3. Emits exact byte spans and point ranges for every symbol.
4. Emits no symbol from an ERROR or missing subtree.
5. For recovered files, rejects any leaf declaration whose own subtree contains an error, which
   prevents macro invocations such as `Q_PROPERTY(...)` from becoming fake methods/parameters.
6. Caps output at 10,000 symbols per file and 2 MiB of source.

### 2.4 SQLite FTS5 index

`CodebaseIndex` maintains:

- `workspaces(root)`.
- `files(id, root, path, hash)` with a unique `(root,path)` key.
- `ast_symbols(...)` with name, qualified name, kind, parent, signature, spans, type and constness.
- `ast_symbols_fts` using the FTS5 trigram tokenizer.
- Indexes on `name`, `parent_id` and `qualified_name` for exact grounding lookups.

Scan bounds: 5,000 files, 2.0 s per pass, 2 MiB per file, SHA-256 skip on unchanged files, and
exclusion of `.git`, build/dependency/cache directories, symlinks and unsupported extensions.
The database lives under `AppLocalDataLocation/codebase-index.db`; the workspace stays read-only.

### 2.5 Grounding resolver

`AstGroundingEngine::groundIntents`:

1. Parses an explicit scope from `intent.condition` (`in`, `within`, `inside`).
2. Resolves the scope by exact qualified name, then by unique exact name.
3. Looks up the target as an exact child of the resolved scope, or as an exact qualified name,
   or as an exact name in the workspace.
4. Ranks candidates by active file, same directory, exact qualified target, resolved scope and
   property/kind agreement.
5. Emits `Verified`, `Ambiguous`, `NotFoundInScope`, `TypeMismatch` or `Ungrounded`.
6. Fails closed when a scope was named but the target is absent; it never widens the search.

Fuzzy FTS fallback is deliberately disabled for semantic grounding. FTS remains available for
future exploratory retrieval, but grounding stays exact and bounded.

### 2.6 Prompt integration

Grounded evidence is rendered as:

```xml
<canonical_intents>
  <intent action="MODIFY" target="offset" status="VERIFIED">
    <grounded_symbol>SessionManager::readPacket::offset</grounded_symbol>
    <location file="src/net/SessionManager.cpp" line="142" />
    <signature>bool readPacket(size_t offset, uint32_t flags)</signature>
  </intent>
</canonical_intents>
```

- Discovery, ambient and rewrite requests carry `groundedCanonicalIntentXml`.
- The bridge prefers that block and does not also inject the lexical canonical JSON.
- Ungrounded/ambiguous entries render only status and bounded candidates.
- The signature payload defaults to **300 bytes** total; truncation never splits an XML entity.
- Function bodies are never rendered. A unit test asserts `"return"` does not appear.

### 2.7 Hash-sealed proposals

`AstGroundingEvidence` now carries the source file SHA-256. `computeAstSnapshotHash` canonicalises:

```
workspace identity
+ verified count
+ per verified symbol: qualified name, file path, file hash, byte span, signature
```

`RewriteProposal` gained `astSnapshotHash`:

- Non-empty → hash domain `roclint.rewrite_proposal.v2`.
- Empty → historical `roclint.rewrite_proposal.v1` domain unchanged, including field order.
- `PayloadCodec` writes `ast_snapshot_hash` and treats it as optional on decode, so pre-Phase-6
  payloads still load and validate on v1.

This preserves Phase 5 continuity while binding new grounded proposals to the exact code snapshot
used during grounding.

---

## 3. Recovery policy and coverage

Tree-sitter cannot expand preprocessor macros. The C++ grammar therefore reports errors on Qt
`Q_PROPERTY`, `Q_OBJECT`, `signals:` and some Windows headers.

Policy adopted:

| Parse quality | Behavior |
|---|---|
| Clean | All declarations eligible. |
| Recovered | ERROR/missing subtrees skipped; leaf declarations containing an error rejected; clean declarations in the same file indexed. |
| Failed | No tree; file contributes no symbols. |

Implementation notes:

- `validNode()` rejects null, missing and type-less nodes before any child access; this fixed an
  access violation on `operator bool()` and malformed declarators.
- `AstExtractionResult` distinguishes `parseOk` (clean) from `recovered` (usable but degraded).
- `CodebaseIndexStats.filesRecovered` records the degraded count per pass.
- A recovered file is hashed and recorded even when it yields no symbols, so it is not re-parsed
  every pass; it contributes no grounding candidates.

Repo-scale result: `30/164` files recovered, `164/164` indexed, `4,679` symbols. This includes
`app/host/HostController.hpp` (736 real symbols after macro rejection) and
`include/roclint/bridge/GraniteAdapter.hpp` (87 symbols; it has only a zero-width missing
`type_identifier`, not a true ERROR). No `Q_PROPERTY`, `READ`, `NOTIFY` or `WRITE` symbol remains
in the index.

---

## 4. Verification evidence

### 4.1 Build and structure

```text
cmake --build build --config Release     -> exit 0
python tools/check_structure.py --root . -> passed=true, 94/94 paths, 0 errors, 244 additions
```

### 4.2 CTest

```text
100% tests passed, 0 tests failed out of 45
Total Test time (real) = 141.70 sec
```

The two new gates are `roclint_ast_symbol_extractor_tests` and
`roclint_ast_grounding_tests`. Existing 43 tests remain green, including storage, Granite adapter,
rewrite profiles, host controller, workspace inspection, product alignment and IPC hardening.

### 4.3 Extraction and grounding latency

```text
roclint_ast_symbol_extractor_tests:
  AST extraction average = 0.061 ms

roclint_ast_grounding_tests:
  AST grounding average = 0.035 ms   (single verified intent, warm index; gate < 2.0 ms)
```

### 4.4 Repo-scale benchmark

Command:

```text
build/Release/ast-grounding-benchmark.exe D:\Stardust\roclint src\bridge\GraniteAdapter.cpp
```

Output:

```text
files_visited=164 files_indexed=164 files_recovered=30 symbols=4679
index_ms=828.649 incomplete=0
readPacket=3        (NOT_FOUND_IN_SCOPE)
offset=3            (NOT_FOUND_IN_SCOPE)
GraniteAdapter=2    (AMBIGUOUS)
HostController=2    (AMBIGUOUS)
grounding_average_ms=0.0856165   (four intents -> 0.021 ms/intent)
```

`readPacket` and `offset` do not exist as exact symbols in the Roclint tree, so failing closed is
correct. `GraniteAdapter` and `HostController` each have more than one exact declaration and no
active-file declaration at the root; reporting `Ambiguous` is the intended decision rather than
guessing. When the active file is changed to a file that declares the symbol, active-file ranking
selects it in the unit tests.

### 4.5 Verification probe

```text
baseline_conflict|ok=1|sealed=1
equivalent_units|ok=1|sealed=1
independent_dimensions|ok=1|sealed=1
disjoint_mandatory_scope|ok=1|sealed=1
equivalent_permissions|ok=1|sealed=1
quoted_data|ok=1|sealed=1
same_scope_opposite_actions|ok=1|sealed=1
nonadjacent_paragraphs|ok=1|sealed=1
disjoint_imperative_scope|ok=1|sealed=1
probe exit = 0
```

### 4.6 Test coverage added

- Extraction: namespaces, nested classes, enums/structs, methods, free functions, parameters,
  exact qualified parents, byte-span validity, signature body exclusion, malformed input,
  Qt-macro recovery, latency.
- Grounding: exact scoped match, cross-file ambiguity, active-file ranking, missing scope,
  hallucinated target, ADD as ungrounded, property/kind mismatch, recovered header, XML entity-safe
  byte budget, deterministic workspace-bound snapshot hash, latency gate.
- Bridge: discovery and rewrite prompts prefer grounded XML and do not duplicate lexical JSON.
- Proposal: v2 domain switch, v1 byte-for-byte stability, adapter propagation, payload round-trip.

---

## 5. Deliberate limitations

1. **C/C++ only.** The index accepts `.c`, `.h`, `.cpp`, `.hpp`, `.cc`, `.hh`, `.cxx`, `.hxx`.
   Other languages are not parsed in Phase 6.
2. **Exact grounding only.** No fuzzy FTS match is used for `Verified`; a near-miss target is
   `NotFoundInScope`, not a guess.
3. **Recovered parse is best-effort.** Error-free declarations in macro-heavy files are indexed,
   but symbols inside macro invocations are intentionally invisible.
4. **Refresh cadence.** Index refresh starts when a workspace is selected. Re-selecting the same
   folder schedules an incremental pass instead of returning only cached files, and the host
   exposes `refreshCodebaseIndex()` as an explicit Q_INVOKABLE refresh hook. There is no continuous
   file watcher yet, so edits are reflected when one of those actions runs. The stored file hash
   makes that refresh incremental.
5. **Benchmark scope.** The repo-scale probe measures warm in-process lookups after indexing. The
   one-time SQLite open and the cold filesystem scan are reported separately (`989 ms` in the
   fresh run; `829 ms` in the historical run).
6. **Budget unit.** The 300-byte cap is a rendered XML signature payload, not a tokenizer count.
   It is deliberately stricter than the 350-token contract ceiling.
7. **Active-file ambiguity.** When multiple declarations tie and none is in the active file,
   `Ambiguous` is preserved so the HUD can ask the user instead of selecting silently.

---

## 6. Commit plan

```text
feat(grounding): add AST-level canonical intent grounding
docs(audit): record Phase 6 AST grounding verification
```

No push is performed. The working tree is left clean except for unrelated pre-existing UI research
assets, which are not staged.

---

## 7. Independent review remediation (fresh)

- **Reviewed implementation:** `49dc23a`
- **Historical report:** `fbd2f78`
- **Review:** `build/phase6-review.md`
- **Fresh probe output:** `build/phase6-review-probe-fixed.txt`

The historical sections above are retained unchanged as evidence of what was signed off before
the independent review. This section supersedes their refresh-cadence description and metrics.
All six review findings are addressed; the independent probe now produces the corrected results
listed in 7.3.

### 7.1 Finding-to-fix map

| Finding | Fix | Regression evidence |
|---|---|---|
| **1. Cross-workspace evidence and clone ID collisions** | Index schema is now version 2. `ast_symbols` carries `workspace_root`; symbol and parent IDs are namespaced as `ws_<workspace-hash>_<symbol-id>`. Exact-name, qualified-name, parent/child and FTS queries all filter the bound canonical root. `CodebaseIndex` binds on construction, `bindWorkspace`, and `refresh`; an unbound instance fails closed. Opening an older schema drops and rebuilds the cache explicitly. The host only grounds after a successful refresh and clears readiness on failure. | `testWorkspaceIsolationAndCloneIdentity`, `testFailedWorkspaceRefreshDoesNotFallBack`, `testLegacyIndexSchemaRebuilds`; review probe `cross_workspace`, `clone_workspace`. |
| **2. Qualified `LIMIT 1` hid overloads and declaration sites** | `findQualified` returns every exact candidate. The resolver merges children across all resolved scope candidates, ranks, and preserves ties as `Ambiguous`. Candidate evidence uses `qualified @ file:line:column` labels so same-named overloads remain distinguishable. | `testQualifiedOverloadsRemainAmbiguous`, `testAmbiguousScopeCandidates`, `testDeclarationAndDefinitionSites`; review probe `overloads`. |
| **3. Unresolved explicit scope widened globally** | Scope parsing accepts backticks, bold, underline and quoted identifiers; normalization is applied to both targets and scopes. When an explicit scope cannot be resolved, the result is `NotFoundInScope` and global lookup is never attempted. Ambiguous scopes remain `Ambiguous` unless exactly one child resolves the target. | `testExplicitScopeIsFailClosedAndNormalized`, `testRealExtractorOverloadAndMissingScope`; review probe missing-scope lines. |
| **4. Empty/unreadable/oversized files retained stale symbols** | A zero-byte file is indexed as a new empty snapshot. Unreadable or oversized paths invalidate the previous file and symbol rows immediately. Deletions are only confirmed when the scan is complete; any iterator error, file budget limit or time-budget stop marks the pass incomplete and suppresses stale deletion. Per-refresh limits make the incomplete path testable. Re-selecting the same workspace schedules a refresh, and `refreshCodebaseIndex()` is an explicit Q_INVOKABLE hook. | `testRefreshInvalidatesTruncatedEvidence`, `testRefreshDeletionOversizedAndUnreadable`, `testRefreshFailedReadInvalidatesEvidence`, `testIncompleteScanPreservesUnvisitedEvidence` (now `testIncompleteRefreshWithholdsStaleEvidence`); host check for the fail-closed refresh hook. |
| **5. Field signatures retained lambda bodies and exceeded the cap** | Signature extraction is structural: function bodies, constructor `field_initializer_list`, `default_value`/`value` initializers (including the preceding `=`), lambda bodies and comments are removed before indexing. The result is trimmed and capped at 1,200 UTF-8-safe bytes for every supported declaration kind. Optional/defaulted parameters are extracted and also body-free. | `testBodyFreeBoundedSignatures`, `testPromptXmlExcludesMemberInitializerBodies`, `testSourceCodeHasNoBodies`; review probe `lambda_initializer`, `field_signature`. |
| **6. Markdown-formatted identifiers failed exact grounding** | Grounding normalizes backticks, `**bold**`, `__underline__`, single/double quotes, wrapping whitespace and trailing punctuation before target/scope matching. The original intent target remains unchanged in rendered output. | `testExplicitScopeIsFailClosedAndNormalized` uses the real `CanonicalIntentExtractor` → `AstGroundingEngine` pipeline for quoted and unquoted prompts. |

### 7.2 Fresh validation

| Gate | Fresh result |
|---|---|
| Structure | **PASS** — `94/94` original paths, `0` errors, `244` additions |
| Release build | **PASS** — full tree, exit `0` |
| CTest | **PASS** — `45/45`, `129.59 s` |
| AST extraction unit gate | **PASS** — `0.084277 ms` average |
| AST grounding unit gate | **PASS** — `0.141747 ms` average for a verified intent; gate `< 2.0 ms` |
| Repo-scale index | **PASS** — `164` files visited/indexed, `31` recovered, `4,763` symbols, `988.733 ms` cold |
| Four-intent lookup batch | **PASS** — `0.107399 ms`, about `0.0268 ms`/intent |
| Verification probe | **PASS** — `9/9` cases, all sealed |
| Proposal v1/v2 continuity | **PASS** — ungrounded v1 hashes unchanged; v2 sealing and payload round-trip tests pass |
| Macro recovery | **PASS** — `Q_PROPERTY`, `READ`, `NOTIFY` symbols remain absent; `Widget` members still ground |

### 7.3 Fresh independent probe output

```text
cross_workspace|refresh_a=1|refresh_b=1|target_from_other_workspace=NotFoundInScope|file=
clone_workspace|refresh_a=1|refresh_b=1
empty_file_refresh|first=1|refresh=1|bytes_now=0|old_target=NotFoundInScope
overloads|refresh=1|declarations=2|qualified=Ambiguous|second_parameter=Verified|missing_scope_qualified=NotFoundInScope
extracted|prompt=Rename Engine::run to step.|target=Engine::run|condition=|status=Ambiguous
extracted|prompt=Rename Engine::run to step in MissingClass.|target=Engine::run|condition=in MissingClass|status=NotFoundInScope
extracted|prompt=Rename `Engine::run` to `step`.|target=`Engine::run`|condition=|status=Ambiguous
extracted|prompt=Rename Engine::run to step in `MissingClass`.|target=Engine::run|condition=in `MissingClass`|status=NotFoundInScope
lambda_initializer|refresh=1|status=Verified|body_in_xml=0|signature=int value ;
field_signature|bytes=18
```

Notes on the corrected semantics:

- `Engine::run` is `Ambiguous` in the probe fixture because that fixture deliberately contains two
  overloads. A single-declaration fixture is `Verified` in the unit test.
- The backticked target is now grounded rather than `NotFoundInScope`; it remains `Ambiguous` only
  because the same two overloads exist.
- `field_signature|bytes=18` is the field `const char* text` after its 1,500-character initializer
  was structurally excluded. The separate long-name test verifies the 1,200-byte cap on a
  signature that genuinely exceeds it.
- `int value ;` is body-free; a cosmetic space before the semicolon remains but no executable text
  enters the index or prompt XML.

### 7.4 Remaining limitations

1. **No continuous file watcher.** Refresh happens on workspace selection/reselection, on the
   explicit `refreshCodebaseIndex()` hook, or on host restart.
2. **C/C++ only.** Other languages remain outside the indexer.
3. **Exact semantic grounding.** FTS is not used to promote a near miss to `Verified`.
4. **Old index migration is a rebuild.** Schema version 2 discards the previous unscoped cache and
   re-indexes source; this is intentional because the old identities cannot be safely reinterpreted.
5. **Recovered parses remain best-effort.** Error-free declarations are indexed; declarations whose
   own subtree contains an error, including macro invocations, are rejected.
6. **True OS read errors** follow the same invalidation path as the deterministic unreadable-path
   test (replacing the source file with a directory).
7. **Warm-lookup benchmark.** The 0.027 ms/intent figure excludes the cold scan (`989 ms`).

### 7.5 Review state

The fixes and regression tests are ready for review. No push has been performed, and the unrelated
`docs/research/` UI document and images remain untouched.

---

## 8. Boundary remediation (fresh)

- **Reviewed fix:** `767ae70`
- **Documentation:** `b0eb13b`
- **Latest review:** `build/phase6-remediation-review.md`
- **Boundary probe:** `build/phase6-remediation-boundary-probe-fixed.txt`
- **Original review probe:** `build/phase6-remediation-review-original-probe-fixed.txt`

This section supersedes Section 7's metrics. The original six review reproductions remain
corrected, and both remaining boundary findings are now fixed.

### 8.1 Finding-to-fix map

| Finding | Fix | Regression evidence |
|---|---|---|
| **1. Scoped matching missed members after 128 children** | The resolver no longer enumerates a fixed child window. For each resolved scope candidate it performs an exact-name lookup under that parent (`findByName(name, parentId, …, false)`), merges matches across overload/redeclaration candidates, and keeps the existing ranking and ambiguity rules. A truncated enumeration can no longer be interpreted as absence. | `testLargeScopeMatchesLateMembers`, `testLargeScopeAmbiguityAcrossCandidates`; boundary probe `large_class` now reports `late=Verified`. |
| **2. Incomplete refresh authorized stale cached evidence** | Schema version 3 adds per-file `validated_generation` and a per-workspace `generation`. A refresh allocates a new generation, touches only files it actually revalidates (including unchanged-hash files), and retains unvisited rows at their older generation. Every exact-name, qualified-name, parent/child and FTS query filters to the current generation, so retained rows cannot surface through a new or existing `CodebaseIndex` instance. The host now propagates the structured `incomplete` result, marks itself not ready, and withholds both grounded XML and AST snapshot hashes until a complete refresh succeeds. | `testIncompleteRefreshWithholdsStaleEvidence`, extended host compatibility test for readiness/context gating; boundary probe `incomplete_refresh` now reports `old_target=NotFoundInScope` and `ast_hash_bytes=0`. |

### 8.2 Chosen freshness model

- Each completed or incomplete refresh advances the workspace generation by one.
- Files visited during the pass are assigned the new generation. Files whose hash is unchanged are
  revalidated without re-parsing by updating the file and symbol generation.
- Files that become empty/unreadable/oversized are invalidated explicitly.
- A complete pass deletes files whose generation was not advanced. An incomplete pass leaves those
  rows in the cache, but they remain at an older generation and are invisible to queries.
- Queries read the workspace's current generation and filter both exact and FTS lookups. A newly
  opened `CodebaseIndex` therefore cannot bypass the freshness protection.
- The host is stricter than the library: any incomplete pass sets `m_codebaseIndexReady=false`, so
  all grounding is withheld, including rows that were freshly validated during that partial pass.
  Library callers can still query the freshly validated generation directly.

### 8.3 Fresh validation

| Gate | Fresh result |
|---|---|
| Structure | **PASS** — `94/94` original paths, `0` errors, `244` additions |
| Release build | **PASS** — full tree, exit `0` |
| CTest | **PASS** — `45/45`, `162.94 s` |
| AST extraction unit gate | **PASS** — `0.078908 ms` average |
| AST grounding unit gate | **PASS** — `0.101499 ms` average; gate `< 2.0 ms` |
| Repo-scale index | **PASS** — `164` files visited/indexed, `31` recovered, `4,784` symbols, `939.164 ms` cold |
| Four-intent lookup batch | **PASS** — `0.139813 ms`, about `0.0350 ms`/intent |
| Boundary probe | **PASS** — early/139th members both `Verified`; incomplete refresh yields `NotFoundInScope` and an empty AST hash |
| Original review probe | **PASS** — all six earlier reproductions remain corrected |
| Verification probe | **PASS** — `9/9`, all sealed |
| Proposal v1/v2, payload, macro recovery, body-free signatures | **PASS** — unchanged CTest coverage |

### 8.4 Boundary probe output

```text
large_class|refresh=1|symbols=141|direct_member139_hits=1|early=Verified|late=Verified
incomplete_refresh|first=1|refresh=1|incomplete=1|source_bytes_now=0|old_target=NotFoundInScope|ast_hash_bytes=0
```

### 8.5 Remaining limitations

1. **No continuous file watcher.** Refresh still happens on workspace selection/reselection, the
   explicit `refreshCodebaseIndex()` hook, or host restart.
2. **Host completeness gating is conservative.** An incomplete scan disables all host grounding
   until a complete pass, even though per-file generations could expose freshly validated rows.
3. **Schema version 3 rebuilds the previous cache.** The index is a cache and is re-created from
   source because older rows lack generation identity.
4. **C/C++ only and exact semantic matching.** FTS is not used to promote near misses to `Verified`.
5. **Recovered parses remain best-effort.** Declarations containing errors or macro invocations
   are rejected while clean declarations in the same file remain eligible.
6. **Warm-lookup benchmark.** The `0.035 ms`/intent figure excludes the cold scan (`939 ms`).

### 8.6 Review state

The boundary fixes, regression tests and fresh evidence are ready for review. No push has been
performed, and the unrelated `docs/research/` UI assets remain untouched.
