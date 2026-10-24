# Current-session command versions

The C01 command boundary now advances the authoritative state version for draft edits, accepted clarification actions, proposal generations, review acknowledgments and wording approval. Text revisions keep their existing identity and sequence: answering a question does not create another text revision.

## Calling the controller

Capture the version when observing the state for an action:

```cpp
const auto observed = controller.stateVersion();
const auto alternative = controller.activeAlternatives().at(0).toMap().value("id").toString();
// If the state changes before this executes, the command must fail.
const bool accepted = controller.confirmChoice(alternative, observed);
if (!accepted) {
    const auto code = controller.lastErrorCode(); // e.g. STALE_VERSION
    const auto explanation = controller.lastError();
}
```

Successful commands expose the new version through `stateVersion` and `stateVersionChanged`. Rejected stale commands leave the in-memory decisions, text, proposal, approval and clipboard unchanged. Error codes come from the typed core result; unexpected host failures use `INTERNAL`.

The required observed-version argument covers:

- `noteDraftEdited`, `commitPrompt`, `analyzePrompt`, `hardBypass`;
- `confirmChoice`, `submitCustomIntent`, `keepOriginalPrompt`, `resolveActiveAssumption`, `skipCurrentItem`, `switchToManual`;
- `setRewriteProfile`, `synthesizeRewrite`, `acknowledgeConstraint`, `approveAndCopyRewrite`, `discardProposal`;
- `restoreRevision`, `restoreSession` (checking the state being replaced before loading another session).

`rewriteProfile` is now a read-only Qt property with an explicit versioned setter command. The upstream controls remain in place; their QML, CLI action wiring and C++ callers are updated together. No overload silently fills in an observed version for a delayed caller. Review acknowledgments come from explicit checkbox activation, not programmatic binding changes.

Versions belong to the open session. Callers must discard rendered action data when its session, snapshot or proposal changes. Private model-operation tickets additionally bind session/revision, state version, edit generations, configuration and pipeline serial.

## Core and storage

`RevisionState::advanceState`, `noteDraftEdit` and `noteProposalEdit` accept an expected version and return typed errors without changing state on a stale input or exhausted counter. Versions stop at the largest integer exactly representable by QML/JSON, rather than wrapping or losing precision.

The repository checks the stored session version and latest revision before applying a mutation. The version, edit generations and invalidation of prior current approval are written together. Confirmed intent, final analysis, proposal and approval writes can advance the state in the same transaction as their artifact. A failed artifact write rolls back the version change and preserves the prior approval.

The controller prepares copies before committing. It adopts a new in-memory state only after storage accepts it. A final clarification answer and its confirmed intent use the same transaction. A provider failure may retain the user's committed input and its accepted analysis request; it does not fabricate an analysis result.

Lower-level repository insertion without an expected-version argument remains available for history fixtures/import tooling. Production current-session commands use the checked path. Global settings, accessibility permissions and history deletion/purge retain their separate existing interfaces; this change is not a claim that every host setting is a revision mutation.

## Verification scope

The core tests cover stale draft/proposal/general mutations and exhausted versions. Repository regressions cover a second writer, monotonic edit generations and rollback of failed analysis/intent/proposal/approval inserts. Controller tests cover delayed clarification, rewrite, approval and discard actions, clipboard preservation and a stale cached controller against a newer stored session. Existing QML and delivery tests exercise the updated call sites.

[Bounded material-assumption scoring](ASSUMPTION_SCORING.md) is now implemented. Release runtime/residency, study/export, packaging and planned RAG remain product work. This command change does not establish their completion.

### Recorded results, October 23, 2026

- Windows Release build succeeded; **34/34 CTest suites passed** (37.52 seconds).
- The actual QML review checkbox regression passed: programmatic `checked` changes left review pending; Space acknowledged once and advanced the version once.
- Source structure/reference check passed against both supplied folders: **96/96 original paths retained**, 167 source files and unchanged recorded vision sources. **8/8 structure-guard regressions passed.**
- The packaged host passed `--dry-run --db :memory:` without model startup or invocation hooks.
- Evidence: `build/command-versions-build.log`, `build/command-versions-tests.log`, `build/command-versions-structure.json`, `build/command-versions-dry-run.log` and the detailed `build/Testing/Temporary/LastTest.log`.

Source was backed up before these changes to `D:\Stardust\backups\roclint-before-command-versions-20261022-230424.zip`. Verification used isolated histories/settings and did not open a normal user history database. The supplied snapshot/specification folders were not edited, and no branch was published.
