# HUD draft editing and rewrite usefulness

Date: 2026-11-06. Implementation: `92d142e`; clipboard-test isolation:
`0607d41`. Windows development build only. Nothing pushed.

## Result

The reported edit/rewrite failure had two reproducible causes. Editing from a
proposal preview focused an editor that remained hidden. Separately, the v6
rewrite contract copied a rough Queue prompt unchanged. The actual v7 host now
persists that rough draft and produces a changed rewrite with the same target,
no-additional-blocking-waits requirement and producer ordering.

The broader model-quality gate remains open. Automatic preservation checks pass,
but agent comparison found seven provisional semantic concerns. This build is
available for reviewed hands-on testing, not established semantic acceptance.

## Changes

- **Edit prompt in HUD** discards the current proposal through the existing
  asynchronous command, then reveals and focuses the editable draft. It does
  not approve the proposal. The next analysis/rewrite freezes the new text.
- Identical original/rewrite text is described as **No wording changes
  suggested. This is your original prompt.** This also covers restored reviews.
- Rewrite instruction contract v7 actively corrects rough prose while retaining
  explicit requirements, identifiers and literal data. Already polished wording
  may stay unchanged.
- The non-AST fallback context supplies action/target/node references rather
  than inferred properties/values/conditions. An extractor default such as
  `behavior: changed` is not an authorized user requirement. Actual AST evidence
  and its hashes are unchanged.
- The response schema requires either nonempty rewritten text or a nonempty
  clarification question, with the matching status. Parser checks still apply.
- A development-only eight-case corpus measures wording changes separately
  from preservation. Evaluator v3 supports `requires_wording_change`; frozen
  v2 fixtures, labels, splits and acceptance thresholds remain unchanged.

Model weights, sampling, approval/delivery rules, AST indexing and freshness
contracts were not changed by this remediation.

## Verification

Evidence directory: `build/manual-testing/session-20261106/`.

| Check | Result / evidence |
|---|---|
| Release build and deployment staging | PASS; `reword-final-build.log` |
| CTest | 52/52, 186.96 s; `reword-final-ctest-isolated.log` |
| Python | 77 tests, one platform skip, 29.33 s; `reword-final-python.log` |
| Structure | 94/94 preserved paths, zero errors; `reword-final-structure.json` |
| Real QML negative reproduction | Fails before fix; `edit-draft-before-strict.log` |
| Real QML edit/new-draft regression | PASS; `edit-draft-after.log`, full product-alignment suite |
| QTest failure guard | Deliberate failed assertion produces exit 1; `qt-assertion-guard.log` |
| Selected development usefulness corpus | 8/8; `build/evaluations/usefulness-v7-development-5/report.json` |
| Selected frozen development preservation | 141/141; `build/evaluations/rewrite-v7-preservation-development-2/report.json` |
| Final frozen all-case evaluation | 200/200 automatic checks, zero missing/errors; `build/evaluations/rewrite-v7-final/report.json` |

The final all-case run contains 100 AST and 100 rewrite cases, resolves 69/69
supported resolvable targets, and has zero wrong `Verified` results. Its source
is `0607d41bc2af20ffbaf3d7bfb0b701a5a398f396`, with fingerprint
`95b499c988217aef62ceaa283c44810c86f8eb2e7b46f80e70ecd8738d70efed`;
`source_changed=false`. The dirty snapshot contains the pre-existing audit-file
edit and the untracked testing guide. Neither was hidden or reset for the run.

Failed development candidates 1–4 and the original v6 rough-prompt reproduction
remain under `build/evaluations/`. In particular, earlier candidates produced
empty quoted-literal output or invented functionality/behavior requirements;
they are not acceptance evidence. There was no favorable-seed search.

### Clipboard-test diagnosis

The first full run passed 51/52. `roclint_async_persistence` failed its unchanged
clipboard assertion. With Qt stderr enabled, the exact run reported
`OleSetClipboard ... OpenClipboard Failed` and `Unable to obtain clipboard`.
The fixture had not established reliable ownership of its sentinel.

The test now uses Qt's offscreen platform, matching other controller tests, and
checks the sentinel immediately after setup. It retains the final unchanged
clipboard assertions and stale-approval checks. The isolated test passed in
9.63 s, then passed in the full 52-suite run. This isolates asynchronous
controller behavior; it does not establish native Windows clipboard delivery.

## Actual host replay

Rebuilt staged executable:
`D:\Stardust\roclint\build\Release\bin\roclint_host.exe`.
SHA-256: `3DB7D0C37C8E1EEB2D554D97E1693BC759A3EDAE173A26C1BE8FD3DE875A521F`.

The host was launched directly with a fresh `history-fixed.sqlite3`, Workspace A,
the rough input and explicit `--enhance`. Those automation flags request a
review/preview; they do not approve, copy, insert or send. Read-only history
inspection confirms Julia triage, native Granite discovery and native Granite
rewrite all succeeded, with rewrite contract `granite.rewrite.format.v7`.

Input:

> pls refactor Queue::push so its easier to read, dont add blocking waits & keep the same producer ordering

Actual saved output:

> Refactor Queue::push to be easier to read, without adding blocking waits and preserving the same producer ordering.

The proposal's AST hash is empty. This replay proves actual host rewriting of
the correct draft, not live workspace-grounding readiness. Native AST/HUD
verification and user confirmation of visible startup remain separate checks.
The desktop inspection helper failed to activate the Explorer target twice
after refreshing its handle, so visible startup was not claimed from process
presence alone. The normal staged host and updated **Roclint Guided Test**
shortcut remain available for the user.

## Semantic and presentation issues still open

All 100 rewrite outputs received an output-hash-bound provisional agent review:
`build/evaluations/rewrite-v7-final/editorial_review.json`. Results: 91
provisionally preserved, seven concerns, two withheld by freshness validation.
Human adjudication is pending; these labels are not a human fidelity score.

| Case | Provisional concern |
|---|---|
| `retry_values_4` | Changes “for explicitly disabled retries” into “to explicitly disable retries,” shifting a condition into an action. |
| `ownership_1` | Adds “it created,” potentially narrowing which temporary handles must close. |
| `ownership_3` | Adds a restriction on all other behavior beyond the supplied lifetime/cleanup constraints. |
| `logging_4` | Replaces an explicit no-suppression prohibition with a potentially weaker reporting promise. |
| `test_scope_4` | Drops “or weaken” from “Do not remove or weaken existing assertions.” |
| `skip_questions_1` | Adds an unrequested behavior-preservation restriction; also starts with a stray period. |
| `skip_questions_2` | Assumes a provided architecture and changes the original restriction into preserving it. |

`skip_questions_4` and `overload_ambiguity_4` also start with a stray period;
`concurrency_3` has awkward sentence coordination. These presentation issues do
not themselves demonstrate changed task meaning. The prior callback propagation
and combined-action purpose-clause concerns are absent from their new outputs.

The raw unchanged rate fell from 86/100 in the v6 frozen run to 56/100 here.
This is an editing-activity measure, not a quality improvement score. More
editing also exposes additional semantic risks. No semantic release gate is
declared passed, and all output still requires the existing explicit review.

Next work: confirm draft editing in the visible HUD; reproduce the development
semantic concerns as separate fidelity work; independently adjudicate frozen
outputs. Do not tune on held-out outputs or weaken the frozen rubric. Windows
packaging, the approved redesign and native macOS validation remain unfinished.
