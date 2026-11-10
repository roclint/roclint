# Architecture remediation re-review — 2026-11-09

## Verdict and scope

Do not close all F01–F09/R01–R06 findings yet. The implementation now includes asynchronous selected-candidate audits, persisted audit results, stricter response parsing, and Advisory filtering. Independent boundary probes still reproduce four code issues, and the current full Release CTest run fails one UI test.

Repository: `D:\Stardust\roclint`; branch `feat/roc-hud-refresh`; HEAD `c22ac3018e6936dc23b090cbc359d7645630179c` plus the existing 18 modified source/test files. This is a review of that uncommitted working tree. No implementation or existing test files were edited by this review. No commit, push, deployment, installed-app restart, approval, clipboard delivery or native insertion was performed.

## R07 — P1: Reselecting an existing candidate does not persist the latest selection or audit

Locations: `src/storage/PromptRepository.cpp:934–957`, `app/host/HostController.cpp:3203–3224`, and restore at `src/storage/PromptRepository.cpp:1211–1226` / `app/host/HostController.cpp:3694`.

Selected candidates reuse their content-derived proposal IDs; audit results are deliberately outside that hash. `insertProposal` uses `ON CONFLICT(proposal_id) DO NOTHING`. Reselecting a previously saved text therefore succeeds without writing its new audit or making it the latest selected proposal. Restore chooses the most recently inserted row by `created_at, rowid`, rather than the user's latest selection.

Independent host reproduction: base audit Pass; select candidate 1 (Pass), candidate 2 (Pass), then candidate 1 again (fresh hard Unresolved). No uncertainty was acknowledged.

```text
reselect_before_restore|audits=4|audit=Unresolved|pending=1|can_approve=0
reselect_after_restore|audit=Pass|pending=0|can_approve=1|same_text=0
```

Restore changes the selected text and removes its uncertainty gate. This is a remaining persistence/restore authorization defect, even though first-time selection and simple restoration now work.

Required correction: durably track the latest selected candidate and its exact audit evidence/attempt on every selection, including reused text. Use explicit selection/audit records or another coherent transactional model; preserve historical approvals and their evidence. Do not merely overwrite historical approved evidence. Cover A→B→A, return to base, same-text re-audits with different outcomes, reopen, save failures and stale completions.

## R08 — P2: Apostrophes break negation and reject valid preservation

Locations: `src/core/constraint_reconciler.cpp:45–64`, negation vocabulary near line 100, and mutation lookback near lines 335–351.

`extractWords` splits apostrophes into separate tokens. `Don't` becomes `don`, `t`; neither matches `not`, `dont` or the literal `don't` in the negation set. A complying prohibition becomes a deterministic hard violation, and the adapter fails fast before model review.

Frozen hard user prohibition: `Do not modify token validation`.

```text
preservation|text=Do not modify token validation.|outcome=Pass|violations=0|unresolved=0
preservation|text=Don't modify token validation.|outcome=Fail|violations=1|unresolved=0
```

Required correction: normalize/tokenize contractions consistently while retaining negation scope. Add straight/curly apostrophe controls, genuine prohibited mutations, and negative preservation predicates. Do not weaken hard-boundary enforcement to accommodate the spelling.

## R09 — P1: Concise still removes mandatory positive requirements

Locations: `src/rewrite/CreativeExpansion.cpp:479–498`, positive-requirement checks at lines 529–561, and the any-substantive-word require check in `src/core/constraint_reconciler.cpp:542–563`.

The Concise branch treats phrases such as `explaining` and `to improve` as disposable elaboration. Its preservation checks accept any shared substantive word, so retaining `write` or `guide` is enough to conceal loss of the actual requirement.

Original prompt and a Direct/User/Hard Require constraint both contain `Write a migration guide explaining how to roll back safely.` The base rewrite is identical to that prompt.

```text
concise_requirement|candidate=alt_base|rollback=1|text=Write a migration guide explaining how to roll back safely.
concise_requirement|candidate=alt_concise|rollback=0|text=Write a migration guide.
```

The bad alternative is emitted rather than omitted. Its initial Unresolved label and a later model audit are useful defenses, but do not establish the claimed fidelity-preserving candidate generation.

Required correction: retain mandatory content clauses and objectives regardless of their introductory words. Omit a shorter candidate when equivalent meaning cannot be established. Add rollback, audience, purpose, exact-target and multi-requirement fixtures; a generic shared verb must not prove preservation.

## R10 — P2: Deterministic-only execution still appears as a successful model audit

Locations: `app/host/HostController.cpp:1093`, `src/bridge/GraniteGoldfishAdapter.cpp:244–246`, `app/host/HostController.cpp:2680–2720`, and `app/qml/shell/IntentReviewPanel.qml:1020–1022`.

The null-ticket bug is fixed. However, if the selected native runtime is not ready, `currentTransport` still returns empty and the audit can return DeterministicOnly/Pass without any required review. The new execution-status property is not consumed anywhere in the QML. The visible Pass summary still says: “All user constraints and negative boundaries verified compliant by zero-memory audit pass.”

Independent host receipt after synthetic base setup, removing the override, and waiting for asynchronous native-route selection to finish:

```text
native_route|selected=1
native_without_ready_transport|outcome=Pass|execution=DeterministicOnly|pending=0|can_approve=1
```

This probe does not claim native model inference was run. It exercises the unavailable native transport branch. The earlier completion receipt printed `can_approve=0` immediately after asynchronous dispatch; that is not evidence of the final outcome or of a completed native audit.

Required correction: make runtime unavailability explicit, apply the intended uncertainty/best-effort review policy, and use execution coverage in the actual HUD labels and summary. A useful deterministic check must not be presented as completed model verification. Test both the final settled state and the displayed wording, with a real native-runtime check separately when available.

## Validation gate — review geometry fails reproducibly

Full Release CTest: **56/57 passed**, exit 8, **192.62 s**. `roclint_product_alignment` fails at `tests/viewmodel/test_product_alignment.cpp:232`:

```text
Review geometry failed at 620x250 item 2
fullyInViewport(window,check) && fullyInViewport(window,label)
```

An isolated rerun fails identically, exit 8, **3.50 s**. The test requires the next review checkbox and label to be in the viewport during keyboard review. Investigate focus/scroll/layout behavior; do not remove or weaken the assertion simply to make the suite green. This review has not established whether the failure predates the current patch or depends on the local rendering environment. The agent's historical 57/57 receipt is not reproduced in the current run.

## Verification and evidence

- Rebuilt Release `roclint_core`, `roclint_host_controller_tests` and their dependencies successfully.
- Confirmed `roclint_product_alignment_tests` and dependencies build successfully and are current.
- Executed the complete 57-test Release suite, then reran the failing UI test once independently. All other suites passed, including host controller, constraint reconciler, creative expansion and Goldfish adapter.
- Compiled new independent probes against current rebuilt production core / testing host libraries using MSVC C++20. Both exit 0; their printed observations intentionally expose defects rather than assert the desired behavior.
- `git diff --check` passes, with existing line-ending conversion notices.
- No full top-level application rebuild, Python/structure run, real model inference, interactive desktop acceptance test, or macOS verification was performed in this re-review.

Durable evidence beside this report:

- `architecture_remediation_rereview_core_2026-11-09.log`
- `architecture_remediation_rereview_host_2026-11-09.log`
- `architecture_remediation_rereview_ctest_2026-11-09.log`
- `architecture_remediation_rereview_product_retry_2026-11-09.log`

Disposable probe sources/response files/executables: `build/manual-testing/remediation-rereview-core-20261109.*` and `remediation-rereview-host-20261109.*`. The host probes use synthetic responses, in-memory databases, temporary settings and offscreen Qt. Existing review reports and original probe sources were preserved.
