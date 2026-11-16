# Companion, interview and review UI evidence

Date: 2026-11-15. Originally authored in `D:/Stardust/roclint-wt-ux` on
`topic/ux-2026-11-15` from baseline `d5c8101`, then integrated into
`topic/integration-2026-11-15`. The two `*_real_controller.png` files were
captured from the combined worktree against the actual `HostController`.

Scope: Agent 4 (Companion, Interview and Review UX), plus coordinator
integration evidence. No installed application was started, replaced or
terminated. Nothing was pushed, merged, reset, cleaned or stashed.

## Evidence classification

The PNGs in this directory are **headless QML renders** captured with
`QQuickWindow::grabWindow()` under `QT_QPA_PLATFORM=offscreen` and
`QT_QUICK_BACKEND=software`. Most state-variation renders are driven by a
**QML test double** that mirrors the frozen Host/UI v1 names
(`tests/viewmodel/CompanionMockController.qml`); they are not full-app
screenshots. The two `*_real_controller.png` files are different evidence: they
load the production QML against the actual integrated `HostController` with a
scripted model transport. The real host was also loaded against
`WordCompanion.qml` to prove the guarded pre-contract and current paths.

No installed application was started, replaced or terminated. No live model
was loaded and no GPU work was performed.

## Screens

| File | State shown |
|---|---|
| `word_companion_ready.png` | Compact popup, original → replacement, consequence, proposed assumption, three alternatives with consequences, explicit Apply/Explore/Keep original |
| `word_companion_clarification.png` | Candidate needs one detail; Apply is absent, Explore says “answer” |
| `word_companion_long_narrow.png` | Long replacement + seven alternatives; the popup stays 376×372 and the body scrolls |
| `interview_question.png` | Question, plain-language consequence, three distinct choices, custom answer, I don't know, skip note, full rewrite |
| `interview_understanding.png` | Interview after an answer: saved state, next question, confirmed/open/proposed/replaced history |
| `interview_narrow_360.png` | Same interview in a 360 px window; wrapping and stacked choices |
| `review_not_run.png` | Unreviewed rewrite: Apply absent, explicit not-applicable reason |
| `review_stale.png` | Stale reread: no Apply, direct Keep original and Reread current |
| `review_ready_findings.png` | Ready reread with blocking/required/advisory findings, excerpts, repair text and per-finding decisions; Apply still locked while two decisions remain |
| `companion_interview_real_controller.png` | Real `HostController` + scripted model transport; adaptive interview QML renders the question, choices, source phrase and custom/skip controls |
| `review_real_controller.png` | Real `HostController` + `FloatingCard.qml`; independent reread READY, interpretation, zero findings, Apply reviewed rewrite and Keep original |

## Implemented

- `WordCompanion.qml` is a compact, editor-anchored, non-activating popup. It
  shows only the frozen phrase, the replacement and host-provided
  consequences. It never invents alternatives, never applies while
  `wordRefinementNeedsClarification` or `wordRefinementApplying`, and keeps an
  explicit Keep original path. Height is bounded to 62% of the screen and the
  body scrolls; width clamps to the screen.
- `CompanionInterview.qml` expands the word/draft surface without restarting
  it. It carries the quoted phrase, confirmed decisions, preferences,
  prohibitions, verbatim source requirements, proposed assumptions, open
  choices, evidence (on demand) and replaced answers (on demand). It supports
  answers, custom answers, “I don't know”, skip (never a confirmed value),
  `changeIntentDecision` correction with provenance text, full rewrite, keep
  original, edit draft and a word-level view with return to the interview.
  Busy and answered are distinct (`isInterviewBusy` vs the turns counter’s
  SAVED acknowledgement); stale host state is not simulated.
- `ComprehensionReview.qml` distinguishes `not_run`, `running`, `ready`,
  `stale` and `failed`. It presents the reviewer interpretation as explicitly
  non-authoritative, findings with severity/kind/summary/verbatim excerpt/
  source/repair, and decisions (`return_to_interview`, `acknowledge`,
  `keep_original`). Blocking findings have no acknowledge control. Apply is
  shown only when `reviewState === "ready"` **and**
  `canApplyReviewedOutput` is true, so even an inconsistent host flag cannot
  make unreviewed or stale text look applicable.
- `IntentReviewPanel.qml` integrates both surfaces, hides the legacy
  clarification panel only when the new intent surface is populated, adds a
  NOT APPLICABLE banner and dims proposal text while a current rewrite is not
  independently reread, and routes “return to interview” through
  `openInterviewFromCompanion` before focusing the question.
- `FloatingCard.qml` gates the footer APPROVE action with
  `canApplyReviewedOutput` when the host exposes it; otherwise the previous
  approval path is unchanged.
- Reduced motion uses the existing `appearance/reducedMotion` `Settings`
  value. Animations are indeterminate pixel pulses, not fabricated progress.
  Measured generation telemetry remains in `GenerationMeter.qml` unchanged.

## Bound Host/UI v1 names

Word: `hasWordRefinement`, `wordRefinementOriginal`,
`wordRefinementReplacement`, `wordRefinementExplanation`,
`wordRefinementCandidates`, `wordRefinementSelectedCandidateId`,
`wordRefinementAssumption`, `wordRefinementNeedsClarification`,
`wordRefinementClarificationQuestion`, `wordRefinementApplying`,
`wordRefinementAnchorRect`, `stateVersion`; invokables
`applyWordRefinement`, `selectWordRefinementCandidate`,
`dismissWordRefinement`; `exploreInCompanion` remains the pre-contract
fallback.

Interview: `hasIntentBrief`, `intentBriefItems`, `intentOpenChoiceIds`,
`intentBriefSummary`, `isInterviewActive`, `canExploreInterview`,
`interviewTurnsCompleted`, `interviewQuestion`, `interviewConsequence`,
`interviewChoices`, `isInterviewBusy`, `interviewFeedback`; invokables
`openInterviewFromCompanion`, `submitInterviewResponse`,
`changeIntentDecision`, `keepOriginalPrompt`.

Review: `comprehensionReviewState`, `comprehensionInterpretation`,
`comprehensionFindings`, `canApplyReviewedOutput`; invokables
`requestComprehensionReview`, `resolveComprehensionFinding`; existing
`approveAndCopyRewrite`, `discardProposal`, `keepOriginalPrompt`,
`proposalStatus`, `proposalText`, `proposalConstraints`, `canApprove` remain
in use by the surrounding HUD.

## Interface gaps and ambiguities (reported, not patched around)

1. **`submitInterviewResponse(text, disposition, expectedVersion)` has no
   choice id.** For `answer` the UI sends the choice’s authoritative `label`
   (so `rawAnswer` retains the user-visible wording), not the opaque choice
   `id`. Agent 2 must map that label to the selected choice, or the contract
   needs an explicit id parameter/overload.
2. **No word-surface generation/stale/failure property exists in D00.** Only
   `wordRefinementApplying` and `wordRefinementNeedsClarification` describe
   state. The popup labels `WORKING` when `hasWordRefinement` is true with no
   replacement or candidates, and the host must clear the suggestion on a
   stale/failed result. If a real `wordRefinementState` (or error text) is
   added, bind it instead of this inference.
3. **No review failure text** exists; `comprehensionReviewState === "failed"`
   is shown with a generic fail-closed explanation.
4. **Findings have no resolved/acknowledged flag.** The UI trusts the host’s
   `comprehensionFindings` list to contain only unresolved findings. If the
   host retains resolved items, they will still be displayed.
5. **`wordRefinementAlternatives` has no documented element shape.** The UI
   renders the structured `wordRefinementCandidates` list only and does not
   guess at alternatives.
6. `intentOpenChoiceIds` is bound but the interview derives open topics from
   `intentBriefItems` (`status === "open"` or `kind === "open_choice"`), which
   also carries the display text. The id list is not used for rendering.
7. `submitInterviewResponse`/`openInterviewFromCompanion` refusal semantics
   are not specified; the UI reads `interviewFeedback` and state changes
   rather than treating a particular return value as failure.

## Validation

Configured with native Granite OFF per D00:

```powershell
cmake -S D:/Stardust/roclint-wt-ux -B D:/Stardust/roclint-wt-ux/build `
  -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=D:/Qt/6.8.2/msvc2022_64 `
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DROCLINT_DEPLOY_QT=OFF `
  -DROCLINT_BUILD_NATIVE_GRANITE=OFF `
  -DROCLINT_SQLITE_SOURCE_DIR=D:/Stardust/roclint/build/_deps/roclint_sqlite-src `
  -DROCLINT_TREE_SITTER_SOURCE_DIR=D:/Stardust/roclint/build/_deps/roclint_tree_sitter-src `
  -DROCLINT_TREE_SITTER_CPP_SOURCE_DIR=D:/Stardust/roclint/build/_deps/roclint_tree_sitter_cpp-src
```

| Gate | Result |
|---|---|
| `roclint_product_alignment_tests` target | Build PASS |
| Companion-only cases (`ROCLINT_COMPANION_UI_ONLY=1`) | 4/4 PASS, 7.6 s |
| Full `roclint_product_alignment` suite (existing + new) | PASS, 92.6 s |
| `roclint_host --dry-run` (offscreen, memory database) | Exit 0; four QML roots loaded; no QML warnings; “Dry-run passed. QML root object verified. Exiting cleanly.” |

The four new cases are:

- `testCompanionWordSurfaceCompactAndActionable` — compact bounds, original/
  replacement/consequence/assumption, alternative consequences, Apply,
  Explore, Keep original, clarification blocks Apply, long content scrolls.
- `testCompanionInterviewContextCorrectionsAndBusy` — carried phrase and
  decisions, busy vs answered, choice/custom/unknown/skip dispositions,
  change-and-save through `changeIntentDecision`, word-view return, narrow
  wrapping.
- `testComprehensionReviewStatesAndApplyGate` — not_run/running/ready/stale/
  failed, stale/unreviewed not applicable, blocking acknowledgement absent,
  finding decisions, apply signal only when unlocked.
- `testCompanionContractGuardsPreContractHost` — a word-only pre-contract host
  hides the new surfaces and keeps the Explore fallback; the real integrated
  `HostController` exposes both surfaces and loads without binding errors.
- `testCompanionInterviewBindsRealController` — the adaptive interview QML is
  loaded against the actual `HostController` (scripted model transport), renders
  the real question and choices, and saves
  `companion_interview_real_controller.png`.

The test double is deliberately labeled in
`tests/viewmodel/CompanionMockController.qml` and
`tests/viewmodel/CompanionWordOnlyMock.qml`. It is loaded through
`QFINDTESTDATA` (with a build-relative fallback), so no CMake registration is
needed for a compiled target. If CI packages only the build directory, those
two QML files must be copied beside the test binary; that is a packaging
request, not a source-registration request.

## Combined integration verification

The combined `topic/integration-2026-11-15` worktree contains Agent 2’s real
intent/interview/review host implementation. The real-controller interview smoke
test and the full `roclint_product_alignment` suite pass. The full-suite render
`review_real_controller.png` shows the real `FloatingCard` review panel with the
independent reread marked READY and its Apply control gated by the host. These
are headless QML renders, not installed-app screenshots.

## Limitations / not run

- The word popup keeps `Qt.WindowDoesNotAcceptFocus` from the existing host
  contract, so it cannot receive keyboard input. Keyboard access is the
  deliberate Explore → HUD route; the popup itself exposes accessible names
  but not a fake focus promise. If popup keyboard access is required, the
  host window flags/focus contract must change.
- Placement near `wordRefinementAnchorRect` is performed by
  `app/host/main.cpp`’s `ScreenPlacement::determineAnchorPosition`; this agent
  did not edit it. Repositioning after content-height changes is host-owned and
  untested.
- Reduced motion honors the app setting only; Qt 6.8 exposes no portable
  system reduced-motion query in `QStyleHints`.
- No undo and no universal inline underline are shown, because the integration
  does not support them.
- This UI pass did not run real model generation, GPU measurement, a native
  window/focus test, an installed-app screenshot, macOS or IDE parity checks.
  The combined delivery’s live host evidence is recorded separately in
  `../companion_after_evidence_2026-11-15.md`.
