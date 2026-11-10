# Interrupted remediation run — independent review, 2026-11-09

## Verdict

The supplied Teamwork transcript ends with credit-exhaustion errors during Milestone 3 testing, not with a completed verification handoff. The current checkout fixes the four previous independent reproductions, but the review-panel geometry gate and test registration are still incomplete. The existing completion report contains historical receipts and has not been updated for this interrupted run.

Baseline: `D:\Stardust\roclint`, branch `feat/roc-hud-refresh`, HEAD `c22ac3018e6936dc23b090cbc359d7645630179c`, plus the existing uncommitted work. This review changed no implementation or existing test files, committed/pushed nothing, and did not deploy or restart the installed application. The normal Release build staged runtime files under `build/Release/bin`; it did not install the app.

## Previous reproductions now corrected

Independent probes were recompiled against the current rebuilt libraries. Synthetic host setup uses temporary settings, an in-memory database and offscreen Qt. It performs no approval, clipboard delivery or insertion.

### R07: Repeated selection and restored uncertainty

The host assigns a fresh proposal ID on each selection. The storage layer no longer silently ignores duplicate inserts, and the selection pipeline captures the proposal and rechecks state bindings after awaits. The reproduced A→B→A sequence now restores A with its fresh Unresolved finding:

```text
reselect_before_restore|audits=4|audit=Unresolved|pending=1|can_approve=0
reselect_after_restore|audit=Unresolved|pending=1|can_approve=0|same_text=1
```

This verifies the reported fixture and current storage/host tests; it is not a claim that every possible concurrency schedule has been exercised.

### R08: Contraction preservation

```text
preservation|text=Do not modify token validation.|outcome=Pass|violations=0|unresolved=0
preservation|text=Don't modify token validation.|outcome=Pass|violations=0|unresolved=0
preservation|text=Modify token validation and leave tests unchanged.|outcome=Fail|violations=1|unresolved=0
```

### R09: Rollback requirement

For `Write a migration guide explaining how to roll back safely.`, with the identical Direct/User/Hard Require constraint, the engine emits the complete base text and omits the unsafe Concise alternative:

```text
concise_requirement|candidate=alt_base|rollback=1|text=Write a migration guide explaining how to roll back safely.
```

### R10: Deterministic-only review

After synthetic base setup, removing the transport override and selecting another candidate through the unavailable native route produces:

```text
native_route|selected=1
native_without_ready_transport|outcome=Pass|execution=DeterministicOnly|pending=1|can_approve=0
```

The host now adds an explicit fallback acknowledgment item. `IntentReviewPanel.qml` consumes execution status in its summary and separates deterministic-only wording from the completed-model-audit summary. This is source inspection plus an isolated host probe; no actual native model inference or visual acceptance session is claimed.

## Remaining blocker 1 — P2: Review geometry still fails

Location: `tests/viewmodel/test_product_alignment.cpp:239`, with layout/reveal behavior in `app/qml/shell/FloatingCard.qml` and review content in `app/qml/shell/IntentReviewPanel.qml`.

After rebuilding the affected targets, focused CTest passes reconciler, repository, expansion and host controller, but fails product alignment:

```text
Review geometry failed at 560x400 item 0
check in viewport: 1 rect: 150 h: 33
label in viewport: 0 rect: 150 w: 444 h: 170 winW: 560
contentScroll y: 142 h: 155 contentY: 130
activeFocus: constraintReview_intent_item_grp_0
```

The focused review label is 170 px tall inside a 155 px workflow viewport. Existing reveal code aligns an oversized item at the top; scrolling cannot make the entire label fit. The stronger substantive-word checks also produce a long audit explanation in this fixture. The transcript mentions a FloatingCard fix, but there is no pending diff for that file in the reviewed checkout, and the current behavior still fails.

Required next work: repair the actual review layout/focus/reveal behavior, keeping the complete review evidence accessible and preserving the viewport assertions. Consider concise review summaries with accessible detail or an appropriate layout expansion; scrolling an oversized row alone is insufficient. Recheck the 560×400 case and the prior 620×250 case across keyboard navigation and long text. Do not weaken the test to hide clipped content.

## Remaining blocker 2 — P2: Diagnostic sources are unregistered and break repository checks

The structure checker reports all three new files absent from CMake registration:

- `tests/core/test_adversarial_comprehensive.cpp`
- `tests/core/test_adversarial_m1.cpp`
- `tests/storage/test_adversarial_m2.cpp`

`python tools/check_structure.py --root .` exits 1. The normal CTest inventory therefore does not execute these sources. Python discovery runs 78 tests and reports 3 failures, 1 skip; the failures are the structure guard's current-source and line-ending continuity checks.

Required next work: decide which artifacts are maintained regression suites. Register meaningful suites in CMake; place disposable diagnostic probes in an appropriate ignored evidence directory instead of leaving unregistered sources under `tests/`. Preserve their evidence. Do not bypass the structure checker or add blanket exclusions.

Before registering them, correct their failure signaling: `test_adversarial_m1.cpp` prints observations and always returns 0; `test_adversarial_comprehensive.cpp` increments `expFailCount` but returns failure only for `failCount`. These diagnostics must not become green CTest evidence while their tested properties fail. Use meaningful assertions or an exit code covering every tested section.

## Verification

- Rebuilt five focused Release test targets and dependencies successfully.
- Focused CTest: **4/5 passed**, exit 8, **19.42 s**; product alignment failed as above.
- Full Release build: **PASS**, exit 0, including the host, native worker, runtime staging and standard test targets. Build emitted an upstream llama object-name warning (MSB8027); no build error was reported.
- Independent core and host probes: compilation and execution **PASS**, exit 0. Their observations are included above.
- Structure: **FAIL**, exit 1; the three missing registrations are listed above. All 94 original paths remain present.
- Python discovery: **78 tests, 3 failures, 1 skip**, exit 1, **52.709 s**.
- `git diff --check`: **PASS**, with existing line-ending notices.
- Full freshly built Release CTest: **56/57 passed**, exit 8, **179.90 s**. The only failure is `roclint_product_alignment`, again at 560×400 item 0 with the same clipped-label geometry. Thus the failure occurs in both the focused run and the full run.
- Actual native inference, interactive HUD acceptance and macOS checks: **NOT RUN**.

## Evidence and next handoff

The new evidence is stored beside this report under `architecture_interrupted_run_*_2026-11-09.*`. Original reports, probes and receipts were preserved. Disposable copied probe sources, compiler response files and executables are in `build/manual-testing/credit-handoff-core-20261109.*` and `credit-handoff-host-20261109.*`.

Commands used (from the repository root; CMake/CTest resolved to the Visual Studio bundled executables, with Qt 6.8.2 on PATH):

```powershell
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
C:\Python313\python.exe tools/check_structure.py --root .
C:\Python313\python.exe -m unittest discover -s tools -p 'test_*.py'
git diff --check
```

Evidence files: `architecture_interrupted_run_build_2026-11-09.log`, `architecture_interrupted_run_ctest_2026-11-09.log`, `architecture_interrupted_run_focused_2026-11-09.log`, `architecture_interrupted_run_core_2026-11-09.log`, `architecture_interrupted_run_host_2026-11-09.log`, `architecture_interrupted_run_structure_2026-11-09.json` and `architecture_interrupted_run_python_2026-11-09.log`.

Finish the remaining UI/test-registration work, add the missing integration coverage, rerun the build and checks, then update the existing completion report with current receipts. Do not redo or discard the working fixes, mark the run complete from historical receipts, or change historical approvals to authorize fresh evidence. Keep the work uncommitted and undeployed unless separately instructed.
