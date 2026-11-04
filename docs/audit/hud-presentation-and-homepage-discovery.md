# HUD presentation and homepage discovery follow-up

## HUD behavior

The floating HUD previously reset `isDetailedExpanded` every time an ambient
window became visible. QML focus-loss handling, the macOS outside-click monitor,
and successful insertion also dismissed it automatically. Reopening a dirty draft
could replace its editable text with the older frozen revision.

The HUD now keeps its presentation through editing, analysis, insertion and the
next prompt. Editing still invalidates stale analysis, proposals and approval.
Explicit expand/compact, hide, minimize and close actions remain authoritative;
late analysis or delivery callbacks do not reopen a hidden HUD. A dirty editor
survives hide/show and minimize/restore. Explicitly choosing an insertion target
can still hide the HUD as part of that user-requested action. This change does not
persist layout preferences across application restarts.

The focused native-window regression can be run with
`ROCLINT_NATIVE_UI_TEST=1 QT_QPA_PLATFORM=cocoa build/roclint_product_alignment_tests`.
It uses native Cocoa windows and focus/minimize events with synthetic drafts and
a mock accessibility target. It does not establish real third-party AX insertion.

## Homepage failure

The saved failure for the user's scoped 133-character homepage prompt was
`Granite did not supply any grounded clarification choices for the ambiguity
flagged by Julia`. The JSON parser had succeeded. The host reported the failed
semantic contract as an unusable model reply. The scoped prompt included
`Workspace: /Users/jesse/testBed` and a restriction to `about.html`.

Discovery instructions no longer force alternatives just because S1 flags a
prompt. They permit an explicit `no_ambiguity` result when there is no material
decision. An example shows grounded visual directions that both preserve
“modern and playful”, without inventing frameworks or numeric targets.

A valid no-question discovery retains the original wording and records both
classifier and discovery provenance. It cannot authorize a rewrite or approval.
The final artifact retains the original S1 evidence unchanged; its existing v4
format and database schema remain unchanged. Persistence validates the successful
discovery attempt before accepting/restoring that artifact. Empty
`options_generated` responses are still rejected. Strict JSON parsing, explicit
retry and original-prompt preservation remain in place.

The real-model follow-up also exposed a Phase 3 stream-envelope mismatch: the
host required bound partial results, but the worker emitted only an ID and chunk.
The worker now copies the request context into each partial frame while excluding
the payload. New protocol cases accept a bound multi-frame stream and reject both
wrong-context and unbound streams. Request binding checks were not weakened.

## Verification

The targeted HUD gate passed all four selected suites. The discovery/snapshot/HUD
gate passed all five selected suites. Native streaming and IPC passed both
selected suites. The focused native Cocoa HUD check passed.

With isolated settings and in-memory history, the exact scoped homepage prompt
and its selected workspace completed through installed Julia MPS FP16 and native
Granite Q8_0. The result asked “How expressive should the playful style be?” and
offered “Bold layout with playful visual accents” and “Minimal layout with subtle
playful details”. No rewrite, approval, insertion or sending occurred. The cold
run took 43.536 seconds including model startup; one observation does not establish
general latency or model quality. Without workspace context, S1 instead retained
the original and skipped Granite; context must be included when reproducing this
case.

The native macOS run emitted existing Qt native-control customization/focus
warnings. An initial attempt to run the entire offscreen-oriented UI suite under
Cocoa failed a keyboard-choice assertion before reaching the HUD case; the
native opt-in now explicitly runs the focused presentation regression. The
regular full suite remains separate.

The final refreshed build passed all 43 registered CTests (62.89 seconds), all 49
Python unit tests, the structure checker and `git diff --check`. The focused
native Cocoa presentation regression also passed on that build. Real third-party
editor interaction and Linux/Windows execution remain unverified in this change.
The separately recorded rewrite-fidelity, overlapping-evidence, and production
diagnostic-copy issues are still outstanding.
