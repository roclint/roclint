# Hands-on finding: vague goals bypass clarification

Date: 2026-11-06. Observed running Windows build: rewrite v7 at `92d142e`.
User confirmed draft editing/rewrite works, then reported too few clarity
options and a flow that mainly rewrites.

## Confirmed evidence

Read-only inspection of `build/manual-testing/session-20261106/history-fixed.sqlite3`:

- Exact frozen prompt: `Make Reader::open better.`
- Automatic Julia score: `0.58936189760699254`, below the `0.70` threshold.
- Saved automatic snapshot: no discovery attempt, no choice groups,
  `assistance_kind=0`.
- Saved rewrite: `Improve the Reader::open method to be more user-friendly and efficient.`
- The source prompt supplied neither user-friendliness nor efficiency as its
  desired goal. The rewrite selected goals instead of retaining the ambiguity.
- A later explicit deeper review of the edited Reader prompt did call Granite
  and produced two choices: clearer error messages or automatic format detection.
  Its confirmed choice was preserved in the next rewrite. This establishes that
  the question path works, while its routing/discoverability and option quality
  need improvement. Automatic format detection is a proposed feature, not a
  fact established by AST evidence or the original request.

Source contracts explain the behavior: automatic discovery requires Julia's
clarify route; explicit review can proceed with a low score. The discovery
prompt deliberately prefers zero/one question and two/three choices per group.
The existing policy is conservative, but the score missed this vague goal.

The user's screenshot is a Connect editor notification. It says the review is
retained while the user selects an insertion destination; it does not display
clarification choices or prove a clarification result.

## Next implementation requirements

1. Make user-requested clarification discoverable alongside rewriting, including
   after a quiet automatic check. A low classifier score must not hide that action.
2. For a vague code-improvement goal, offer a focused question about the outcome.
   Goal choices may include readability, reliability/error handling or performance;
   they must remain explicitly optional proposals rather than inferred requirements.
3. Provide a custom answer and an explicit skip/rewrite-as-is path. Do not force
   more questions merely to fill a quota, or select a suggested goal automatically.
4. A skipped/omitted goal stays unspecified in the rewrite. In particular, the
   Reader reproduction must not silently add efficiency, usability, file-format
   detection or a new implementation strategy.
5. Cover automatic quiet → explicit clarification → user choice/custom answer,
   plus skip → no invented goal, in controller/QML tests and real-model development
   checks. Retain immutable revisions, stale-result checks and explicit approval.

This is an observed finding and implementation brief. No routing, model,
threshold or UI behavior was changed during this inspection. The user's live
host was left running. Existing unrelated audit-file edits were preserved.

## Implementation follow-up

The user authorized implementation while retaining Julia's automatic decision.
The original inspection above describes the pre-change behavior.

- Julia template v3 adds one focused question about a missing improvement goal.
  The normal host's extended checklist grows from six to seven questions; the
  plain checklist grows from four to five. These are internal model questions,
  not extra compulsory questions shown to the user.
- Corroboration requires the broad ACTION score to be at least 0.50 and the
  focused goal score to be at least 0.70 before the goal evidence promotes the
  ACTION routing score. Global routing thresholds and the existing uncertainty
  band are unchanged. Julia still decides automatically, without a keyword
  override. Raw atomic scores remain distinct from the four routing scores.
- Core validation continues to accept sealed historical v1/v2 template records.
  New records use the versioned v3 identity; model weights and hash domains are
  unchanged. New helper/template identity must match the host.
- Clarify remains visible after a quiet result and from a rewrite preview.
  It requests deeper review regardless of a low Julia score. Custom answers
  remain available; discovery v8 asks for relevant goal alternatives rather than
  inventing new features to offer as interpretations.
- Rewrite without questions defers unanswered choice groups and assumptions,
  preserving confirmed selections/custom answers and carrying the open
  questions into the rewrite request. It does not consume an answered-question
  round, select an alternative, approve or deliver text. Revision/version and
  workspace checks apply before synthesis. Blocking conflicts and required
  missing decisions still require an explicit answer or draft edit.
- Rewrite v9 applies additional deferral guidance only when the bound intent
  actually contains unanswered questions. Confirmed goals remain binding.

### Development experiments

`tools/probe_julia_clarification.py` compares pinned pre-change source `1133aa5`
with the actual new worker, using the same pinned local Julia weights/device
and a 16-case development corpus. It is diagnostic evidence, not held-out
acceptance or a human assessment of question quality.

The initial three-question expansion caught Reader but introduced additional
routes for clear prompts. A short yes/no formulation then missed Reader. Both
failed experiments are retained as `build/evaluations/julia-focused-development-1.json`
and `julia-focused-development-2.json`; neither was selected.

The selected production-worker comparison is
`build/evaluations/julia-focused-development-final.json`:

| Measurement | Previous | Focused goal check |
|---|---|---|
| Vague requests routed to Granite | 4/5 | 5/5 |
| Clear controls routed to Granite | 7/11 | 6/11 |
| Reader ACTION routing score | 0.58936 | 0.98527 |
| Median classification time in this small run | 79.42 ms | 72.05 ms |

There were no newly routed clear controls. Additional batching can change
numerical outputs, and this single small run does not establish a latency
improvement or broad calibration. Six clear controls still invoke Granite;
Granite must judge whether a real question is useful rather than manufacture
one to justify Julia's score. The extra failure-context and performance checks
were withheld because their development outputs were too noisy.

## Final verification and limitations

Implementation commits: `3284363` (routing and controls), `4420987` (scope
deferral to unanswered questions and pin the historical test fixture). Not pushed.
Julia question template: v3; Granite discovery: v8; Granite rewrite: v9.

| Check | Result / evidence |
|---|---|
| Complete Release build and staging | PASS; `build/manual-testing/session-20261106/clarify-v9-build.log` |
| Staged executable QML dry-run | PASS; `clarify-v9-dry-run.log` in that session directory |
| CTest | 52/52, `-j 2`, 102.33 s; `clarify-v9-ctest.log` |
| Python | 78 tests, one platform skip, 29.78 s; `clarify-python.log` (Python unchanged by v9) |
| Structure | 94/94 preserved paths, zero errors; `clarify-structure-final.json` |
| Focused actual-QML regression | PASS; `clarify-qml-focused-final.log` |
| Native-model development cases | 13/13 automatic checks; `build/evaluations/clarification-v9-development/report.json` |
| Frozen corpus regression replay | **199/200 automatic checks; strict runner FAIL**; `build/evaluations/clarification-v9-frozen-replay/report.json` |

The replay has 100/100 AST cases and 99/100 rewrite cases, 69/69 supported
resolutions, zero wrong `Verified`, zero missing cases and zero execution errors.
Source: `4420987252fa4400616eb191c737c9ef23d6060a`, fingerprint
`bd778076a893cf13d64f94aa9114464d3cf78561612f38c434e683b222e36ef3`;
`source_changed=false`. The dirty snapshot contains this pending audit update
and the unrelated user edit of `ast_rewrite_tuning_2026-11-06.md`.

The single failing replay is `clarification_4`: the user confirmed higher
throughput, but the output retained the original generic “runs faster” request
and omitted that selected goal. This is a real remaining fidelity issue. The
runner's failure, original fixture, expected requirement and raw output are
retained. No thresholds or frozen annotations were relaxed. Human semantic
acceptance remains pending for all rewrite cases. These previously exercised
fixtures are regression evidence, not a new unseen acceptance set.

### Integration corrections and honest negative evidence

- The first QML build attempted to assign Flow's read-only `implicitHeight`.
  Removing that assignment restored loading. A crashed test image remained
  locked; the old build artifact was renamed and the corrected target relinked.
  Focused and full suites then passed. The failed logs remain in the session.
- The historical golden-hash test had relied on the current template default.
  It now explicitly sets its original v2 template. Its pinned digest remains
  `762e300f25de33539ca5aaf21d0519d2c24c79e81affdc47aa2785ddbb24dc89`.
  No hash algorithm or expected digest was changed to conceal a failure.
- An unconditional v8 instruction about open goals suppressed some confirmed
  answers. The v8 replay recorded 196/200 checks. The v9 correction scopes the
  extra instruction to `unresolvedItemIds`; three added development cases cover
  confirmed Reader readability, confirmed Reader reliability, and a confirmed
  Reader goal alongside an open Queue objective.
- Early raw rewrite diagnostics also selected unrequested goals when no
  discovery context was supplied. The actual skip-flow fixtures now supply
  their `discovery_reply`, with unselected groups bound as unanswered questions.
  This corrects their preparation to match the controller path; the original
  prompts and prohibition checks remain. Earlier failed reports are preserved
  at `clarification-v8-usefulness` and `clarification-v8-usefulness-final`.
  Passing the bound skip path does not establish that an unclassified direct
  rewrite is safe or that every paraphrase is faithful.

Actual v9 development outputs include:

- Unanswered Reader goal: `Improve Reader::open.`
- Confirmed readability: `Improve the readability of Reader::open.`
- Confirmed reliability: `Make Reader::open more reliable.`
- Confirmed Reader with an open Queue goal:
  `Improve Reader::open for readability. Make Queue::push faster.`

### Real automatic pipeline

`clarify-live-automatic.log` records the installed local Julia → native Granite
controller pipeline on the exact Reader prompt, using isolated settings, an
in-memory history and the automatic review trigger. It completed without a
proposal or delivery:

```text
s1_score=0.985271; configuration=julia.cuda.bf16.v2.threads8
outcome=OPTIONAL_HELP
question=What outcome should improving Reader::open prioritize?
option=Better performance (speed or resource use)
option=Improved usability (easier navigation, clearer feedback)
option=Enhanced error handling and robustness
```

This ran at `3284363`; its Julia, discovery and triage-controller source is
unchanged at `4420987`. Cold end-to-end startup took 102,072 ms, including
loading both models. That is not the warm 85 ms Julia classification timing.

The actual staged v9 host was then launched with a separate
`history-clarification-v9.sqlite3`, the Reader prompt and no `--enhance`.
Read-only history inspection confirms the correct Reader revision, successful
Julia/discovery attempts, three offered choices, zero proposals and zero
approvals at observation. This GUI preparation uses explicit deeper review;
the isolated pipeline above establishes the automatic path separately.
Receipts: `clarify-v9-host-receipt.json` and `clarify-v9-live-state.json`.
Executable SHA-256:
`EF4967064E93921BE81A9429025C4645C1A62B695B62FBFD9044D86C1A5BB408`.

The live GUI question was “What primary improvement matters most for
Reader::open?” Its choices included initial load speed, error handling/recovery,
and navigation controls. **Navigation controls are not established by the
prompt or AST.** This illustrates remaining option-quality work; these are
optional suggestions, not accepted requirements. Custom answers and explicit
skip remain available. Neither the mock-rendered three-goal UI nor the numerical
route score proves that all generated options are relevant.

Layout evidence from the actual QML test with scripted choices is saved at
`build/manual-testing/session-20261106/clarification-ui/clarification-options.png`.
Desktop visibility was not independently observed. Native macOS and insertion
are not established by these Windows checks.

Original test histories and their backups remain in the session directory,
including `history-before-confirmed-goal-fix.sqlite3`. User actions in the older
history included approvals; those are preserved and are not agent-generated
delivery tests. The unrelated audit-file edit and research assets were not
changed or staged by this work.
