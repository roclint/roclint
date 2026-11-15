# DeepSeek remediation handoff — 2026-11-13

Branch: `feat/roc-hud-refresh`
Base reviewed: `c39d82140f0a2ab15337f174b3a134b7d41024fe`
Evidence: `D:\codex-evidence\roclint-gemini-review-20261113`
Receipts: `D:\codex-evidence\roclint-deepseek-remediation-20261113`

This handoff describes the current working tree only. No commit or push was made.
Bob's retrieval/RAG subsystem was not edited.

## Implemented

1. **Contextual word refinement replaces the eight hardcoded rules.**
   - `granite.word_refinement.v1` prompt + strict parser in `GraniteAdapter`.
   - Request carries the exact draft, selected span/offsets, surrounding before/after,
     approved decisions, explicit constraints, grounded AST XML, and intervention type.
   - Candidates carry host-minted identity, exact original span, replacement, practical
     explanation, proposed assumption, and a clarification flag.
   - `CreativeExpansion` no longer has visual/cinematic/architecture/bug keyword menus or
     the `layer 3` test branch. Alternatives are structural re-expressions of the user's text.

2. **Non-default alternative selection.**
   - `applyWordRefinement(version, candidateId)` resolves the chosen candidate.
   - Validates suggestion id, candidate id, field token, workspace epoch, pipeline serial,
     state version, exact field text, offsets, and original span.
   - Freezes an in-flight request; delayed completions cannot clear a newer suggestion.
   - HUD/IntentReviewPanel popovers removed; new `WordCompanion.qml` window uses candidate ids.

3. **Range insertion safety.**
   - `RangeInjectionPolicy` validates UTF-16 bounds, exact range text, field identity, and
     fallback preconditions.
   - `WinAccessibilityBridge` checks `MoveEndpointByUnit` results, reads and compares the
     selected range, verifies the actual selection, and never overwrites a changed/partial
     field with an old snapshot.
   - Terminal range edits are refused explicitly; the base bridge emits `NotSupported` when a
     platform has no range editor.

4. **Discovery audit/repair is connected.**
   - Discovery worker runs a stage-specific audit inside the shared analysis deadline, using
     the real transport when available.
   - `correctDiscoveryWithRepairLoop` prunes alternative-level findings, preserves valid
     siblings, converts exhausted decisions into blocking missing decisions, rejects repeated
     candidates, and is bounded.
   - The host no longer prunes with `nullptr` or turns an empty result into `NoAmbiguity`.
   - Regression: forbidden purple is removed, blue/green decision remains open.

5. **One recommendation policy.**
   - Host uses `S1TriageAdapter::extractJustifiedRecommendation` on sanitized Julia results.
   - Recommendation carries group/alternative identity and cannot cross decision groups.
   - Julia prohibition filtering distinguishes preservation wording; exact ties remain
     unrecommended (`unresolved_tie`, confidence 0).

6. **Overlapping decisions.**
   - `MaterialityPolicyV2` excuses overlapping evidence only when both groups supply distinct,
     explicit dimensions. Missing/same dimensions remain conservative.
   - Policy version bumped to `materiality_policy_v2`.

7. **Lightweight word companion.**
   - Separate non-activating `WordCompanion.qml` window anchored to the observed caret rect.
   - Small preview, explanation, assumption, candidate chips, Apply/Explore/Dismiss.
   - Debounced requests, stale-generation rejection, dismissed-key suppression, and graceful
     model/geometry fallback. No editor underlining claim.

8. **Strict audit parsers.**
   - Discovery and clarification audit parsers require enumerated outcomes, required typed
     fields/arrays, and consistent findings.
   - Unavailable/malformed audits remain `Unresolved`, not `Pass`.

## Validation

- 41/41 applicable CTest tests passed with `-E "retrieval|e2e"`.
- Host controller, Granite adapter, Goldfish adapter, materiality, creative expansion, and
  S1 adapter suites passed; product-alignment QML suite passed with the offscreen platform.
- Python Julia worker suite: 39/39 passed.
- Real Granite Q4_K_M word-refinement examples and timings are in the receipts directory.
- Dry run: `build\Release\bin\roclint_host.exe --dry-run` passed.
- `roclint_e2e` is Bob's retrieval suite and still fails 5 schema-migration cases.

## Limitations

- A live external-editor insertion could not be performed in this session: the Windows
  foreground stayed on an existing Edge window and refused activation of a disposable test
  host. Native mutation-boundary logic is covered by policy tests; no real draft was modified.
- Granite's discovery model still sometimes asks to confirm an exact mechanical edit when no
  workspace/AST grounding is supplied. A narrow host guard removes a confirmation-only result
  when the audit passed and the prompt quotes old/new values for a named file with an explicit
  boundary.
- Word companion model latency on this CPU was 19.4 s for one case, under the 24 s operation
  budget but noticeably slower than the ambient HUD.
- `BUILD_DESKTOP=OFF` in this build does not register the desktop CTest targets; the host and
  product-alignment binaries were built and run directly.
