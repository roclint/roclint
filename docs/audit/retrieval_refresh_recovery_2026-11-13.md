# Retrieval refresh and cache recovery fixes

Date: 2026-11-13. Branch: `feat/roc-hud-refresh`.

This change fixes the two independently reproduced refresh failures and the five failing schema/cache checks from the Gemini M1 verification. It does not mark the complete RAG project or real-model evaluation finished.

## Changes

- A source becomes reusable only after vectors have been stored successfully. Schema v3 adds `retrieval_sources.indexed_complete`; failed embeddings never publish a reusable source marker, and truncated files publish an incomplete marker. Existing caches default this marker to false and revalidate on refresh.
- Reuse requires a complete source marker and corresponding published chunk/vector evidence. All model, normalization, preprocessing, dimension and chunking policy fields participate in cache compatibility.
- `beginGeneration(RefreshType)` explicitly distinguishes full and incremental work. A complete full pass establishes coverage; successful incremental work preserves existing coverage; any failed or incomplete pass revokes it. Incremental work cannot repair an incomplete full snapshot.
- Read/hash/chunk/embedding failures, truncation, cancellation and elapsed indexing budget stops report incomplete work. Source-write and commit errors are propagated. An incomplete empty retrieval result remains `Partial` rather than asserting absence.
- Schema migration and table creation run transactionally. Fresh metadata uses the same authoritative schema version as the public header. Repeated refreshes reuse their database connection.
- Startup cache repair removes historical one-byte `"0"` excerpts whose declared source spans require a different length, along with their vectors, FTS records and reusable source markers. Affected workspaces lose complete coverage. Genuine one-byte `"0"` content and unaffected cached records remain intact. Original workspace files are not changed.
- Publishing a complete recovery avoids collisions between an old partial chunk and its newly embedded counterpart. FTS carry-forward also avoids duplicate copies of those records.

## Regression coverage

Six maintained E2E regressions cover failed embedding recovery after restart, incomplete full refresh across restart, truncated-source recovery, failed incremental work, preprocessing-policy changes, and preservation of legitimate `"0"` source content.

The existing per-file cap unit test now proves its fixture exceeds the cap and asserts incompleteness unconditionally. Successful incremental tests now explicitly identify the refresh type. A previous test that expected `Ready` after a failed update now verifies `Partial`, retained cached rows and failed source-hash validation.

## Verification

Baseline with the four initial new lifecycle regressions: 103 E2E cases, 93 passed, 9 failed, 1 not run.

After fixes and the additional policy/legitimate-zero regressions: 105 E2E cases, **104 passed, 0 failed, 1 not run**. The retrieval unit executable passed. The repository's Release targets `roclint_retrieval_index_tests` and `roclint_e2e_tests` built successfully; both selected CTest targets passed.

Independent production-API reproductions showed:

```text
unchanged_refresh|embedding_calls=0
unchanged_chunk_rollback|before=1|during=1|after=1
per_file_cap|actual_chunks=13|stored_chunks=1|incomplete=1|coverage_complete=0
incomplete_full_after_complete|stats_incomplete=1|coverage_complete=0|status=PARTIAL
reuse_after_embedding_failure|first_incomplete=1|recovery_embedding_calls=2|recovery_incomplete=0|coverage_complete=1|hits=2
```

Commands used, with CMake and CTest from the installed Visual Studio toolchain:

```powershell
cmake --build build --config Release --target roclint_retrieval_index_tests roclint_e2e_tests --parallel 3
ctest --test-dir build -C Release -R '^(roclint_retrieval_index|roclint_e2e)$' --output-on-failure
```

Logs, the structured E2E report, baseline, independent harness and before-edit source copies are under `D:\codex-evidence\roclint-retrieval-fixes-20261113`.

## Limits and integration notes

The one NOT RUN case reports missing local embedding model assets. No real-model semantic-quality result is claimed. These changes do not implement or validate the broader model-asset trust, generation-tokenizer packaging, reference-corpus ingestion, host/approval integration, or native GUI work owned by other workstreams.

Callers using the index directly must select `RefreshType::Incremental` explicitly and pass whether that pass actually completed to `commitGeneration`. Its boolean no longer doubles as a refresh-type hint. Existing source markers from older cache versions are intentionally revalidated.

Changes remain uncommitted. No push, installed-application update, or workspace source rewrite was performed.
