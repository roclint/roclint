# AST baseline verification — November 5, 2026

Reviewed the combined uncommitted AST implementation on `240c74f`, including
workspace epochs, unavailable-index invalidation, renderer identity, and
hash-triggered stale recovery. This is a Windows verification; native macOS
execution remains unverified.

## Independent reproductions

The diagnostic sources from `build/ast-additional-review.md` and
`build/ast-remediation-rereview.md` were rebuilt against current libraries.
The workspace diagnostic's helper was adapted to expect discarded publication
when switching during generation; its former unconditional READY assertion
described the old behavior and aborted before recording the corrected outcome.
Production behavior was not relaxed to accommodate the diagnostic.

Observed:

```text
workspace_switch|during_rewrite=0|new_workspace_has_no_run=1|ready=0|proposal=0|can_approve=0|status=UNAVAILABLE
workspace_switch|during_rewrite=1|new_workspace_has_no_run=1|ready=0|proposal=0|can_approve=0|status=UNAVAILABLE
unavailable_index_before|can_approve=1
unavailable_index|ready=0|proposal_present=0|can_approve=0|status=STALE
missed_notification|first_xml_empty=1|again_xml_empty=0|full_refresh_delta=1|status=CURRENT
render_identity|no_empty_or_one_character_target_in_fixture_sweep
```

The stale request remains withheld; a subsequent request sees the refreshed
snapshot. The separate registered stale-recovery suite disables watcher delivery
and checks coalescing, bounded attempts, removal, and workspace ownership.

## Additional boundary corrected

A new diagnostic found that a requested target padded with 50 spaces on each
side could compact to `target=" ..."` with a 178-byte preceding identifier.
The result was 350 bytes and reported two rendered records despite the second
having no non-whitespace identifying content.

The compactor now rejects retained prefix/suffix content that consists only of
whitespace. Such a record is omitted rather than hashed or displayed as emitted
evidence. A 229-budget regression sweep verifies meaningful content, XML/UTF-8
validity, the byte cap, emitted/subset correspondence, and omission from the hash.
The production-library diagnostic now reports:

```text
whitespace_identity|no_whitespace_only_compaction
```

## Evidence and acceptance

Local evidence is under ignored `build/ast-baseline-20261105/`:

- `source-before.json`: starting revision and per-file hashes before review edits.
- `build.log`, `ctest.log`: initial Release build and 50/50 passing tests.
- `build-final.log`, `ctest-final.log`: rebuilt implementation and final test run
  after the whitespace regression fix.
- `host-probe.log`, `unavailable-probe.log`, `render-final.log`,
  `whitespace-final.log`: focused native reproductions.
- `python.log`: 49 Python tests, one platform skip.
- `structure.json`: 94/94 original paths preserved, zero structure errors.

Final Release build passed. The post-fix full CTest run passed **50/50** in
178.83 seconds. The pinned-tokenizer suite executed (not skipped): 350 bytes /
182 tokens, 350 / 113, and 340 / 111. The nine-case verification probe passed
9/9, all sealed. Python passed 49 tests with one platform skip, and the final
structure check preserved 94/94 original paths with zero errors.

The historical review files remain unchanged as evidence of the original failures.
This baseline does not establish general rewrite fidelity, a release installer,
or native macOS behavior. Those require their own evaluation/platform gates.
