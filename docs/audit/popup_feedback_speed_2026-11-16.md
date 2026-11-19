# Popup visibility, Apply feedback and startup — November 16, 2026

## Scope and status

Implementation in `D:\Stardust\roclint-wt-integration`, branch
`topic/integration-2026-11-15`, baseline
`30929c91d2a481f5a68336d55b8916f7c7f0cf6f`. The earlier uncommitted UI/word-action
changes and research references are preserved. The matching v16 host, native
worker and runtime configurations are installed and the Windows startup smoke
passed. Live popup placement and one manual native Apply are independently
verified in the disposable Windows editor; this does not establish other IDE or
macOS insertion parity.

## Changes under verification

### Popup geometry and visibility

Windows UI Automation reports native pixel bounds. An explicit observation flag
now causes normalization on the GUI thread, once, to the selected monitor's Qt
logical geometry. Mapping accounts for monitor origins and separate scales,
including negative origins; an unknown monitor uses the unknown-anchor fallback.
Mac/test observations remain logical by default. The installed Windows run
confirmed DISPLAY1 native-to-logical mapping at 200% scaling; maximizing the
disposable editor moved the existing popup below the caret near the screen edge.
The minimized HUD remained hidden while the editor was activated and edited.

Geometry-only UIA changes now emit observations, and a matching word request
updates its anchor without issuing another model request. The Windows resident
HUD hides to its tray when minimized, avoiding a minimized topmost card returning
over the editor. Popup dimensions, colors, fonts and the Roc bird are retained.

The coordinate boundary follows [Qt 6.8's high-DPI coordinate model](https://doc.qt.io/qt-6.8/highdpi.html).
Actual mixed-monitor placement and native macOS behavior require separate live
verification; pure mapping tests do not establish universal editor compatibility.

### Honest Apply feedback

The word surface exposes checking, ready, applying, applied and failed states.
Loading shows actual pending work without a fabricated candidate. A queued write
shows Applying. Applied is set only by a matching native `Injected` completion,
which the bridge defines as exact readback. Failures/refusals explain the problem
and offer an explicit Check again that rechecks the draft without writing. A
model-load failure before dispatch now produces a visible failure instead of an
indefinite Loading state. Checking hides previous replacement, assumption,
explanation, alternatives and clarification content even if old fields linger
during a state transition.

Completion feedback is bound to the request, workspace, pipeline, field/process
and original/result text. Dismissal, another field/draft and superseding work
withhold stale feedback. A short success confirmation survives its own text echo;
its timer cannot dismiss a later suggestion. Existing bridge write, focus,
readback and partial-write guards remain in place. Diagnostic events contain
request/version/field metadata, never prompt or replacement text.

The earlier user reported a successful editor change, while screenshots/readback
and logs did not independently confirm it. That discrepancy remains historical
evidence; this implementation must not retroactively turn it into a passing test.

### Full model verification, faster on Windows

Every model load still hashes the entire file and compares the pinned SHA-256.
Windows now streams bounded chunks through CNG; other platforms retain the Qt
SHA-256 path. Cancellation/deadlines are checked between reads and before the
final digest. Read or hashing failures prevent loading. No timestamp cache,
trusted-ping shortcut or integrity-check bypass was introduced.

The provider setup follows [Microsoft's CNG hashing API](https://learn.microsoft.com/en-us/windows/win32/seccng/creating-a-hash-with-cng).
Same-file sequential comparison (CNG first, Qt second), each reading all
**3,892,651,552 bytes**:

| Measurement | Result |
| --- | --- |
| Windows CNG SHA-256 | 5,687 ms |
| Previous Qt SHA-256 reference | 53,461 ms |
| Both full-file digests | `fbe986738041418e26de9e123ba740cb654931f85bf572a71bd01f9e6b85e53d` |
| New worker readiness, real corpus | 21,281 ms |
| New worker verification stage | 6,216.6924 ms |
| Installed worker readiness trace | 20,495.493 ms |
| Installed worker verification stage | 5,504.562 ms |
| Earlier v15 readiness / verification | 47,908 / 37,530.9732 ms |

The same-file hash comparison matches digests and demonstrates substantially less
hashing time on this Windows machine. Total readiness fell in the observed runs,
but disk-cache, device load and thermal conditions were not controlled. These are
cold-process measurements, not guaranteed cold-disk or cross-machine results.
Warm corpus checks took 893–3,898 ms. Model loading/context setup remain meaningful
startup costs; this change does not eliminate them.

Runtime configuration is v16.popup_feedback_fast_verify. Model weights, pinned
llama runtime, sampling, reasoning preference/budgets and word prompt/audit
contracts are unchanged from v15. Host and worker must be installed together.

## Verification gates

| Gate | Result |
| --- | --- |
| Release host, worker and all test binaries | PASS |
| Full enabled CTest suite | 66/66 PASS, 375.84 s; one real-embedding integration test disabled |
| Final loading-content UI follow-up | PASS, companion UI subset, 10.81 s |
| SHA vectors, multi-chunk input, cancellation, closed input/read errors | PASS |
| Placement mapping, geometry-following, stale Apply callbacks/timers, dismissal, retry/no-write, native load-failure feedback | PASS in relevant rebuilt suites |
| Actual pinned Q8_0 word corpus, default thinking off | 7/7 PASS, no experimental instruction override |
| Same-file full hash comparison | PASS, identical pinned digests |
| Structure / whitespace | PASS / PASS |
| Installed Windows-platform `--dry-run --db :memory:` | PASS, exit 0; developer Qt removed from PATH |
| Live 200% placement / minimized HUD | Observed in disposable native editor |
| Live native Apply and editor readback | PASS: matching request 1 / field 33 completed INJECTED; independent editor UIA readback shows corrected sentence |
| Mixed-scale multi-monitor / native macOS | NOT RUN |

The real corpus includes spelling, grammar, qualified code names, literal labels,
performance ambiguity and creative ambiguity. Mechanical routing checks passing
seven examples does not establish general semantic quality. The raw first pass
still unnecessarily asks questions about obvious spelling errors; the existing
bounded surface check and independent audit produce the accepted corrections.
The creative question remains broad. Neither behavior was changed in this task.

Visual QA inspected the generated loading, applying/applied and failure states;
the final loading preview contains no stale recommendation details. UI artifacts
and logs are under `D:\codex-evidence\roclint-popup-feedback-speed-20261116`.
The explicit `roclint_test_binaries` build target avoids ALL_BUILD's large runtime
copy/deployment targets. The existing pinned llama MSBuild duplicate-output warning
remains; a successful build is not a claim of zero warnings.

Installation replaces only the host, both installed native-worker copies, model
manifest and runtime config. Source/installed SHA-256 values match in
`installation.json`; the previous five files are backed up in
`installed-v15-backup`. No model assets, Python environment, preferences or history
were replaced. Diagnostic logs contain request/field/geometry metadata; corpus
logs contain only synthetic fixtures. Installation initially stopped before any
copy when the old worker exited between enumeration and termination; the script
now tolerates that shutdown race and the retry passed.

## Live Apply follow-up

After the user reported clicking Apply, the installed log recorded:

```text
[Roclint inline apply] phase=request expected_version= 0 current_version= 0
[Roclint inline apply] phase=queued request= 1 field= 33
[Roclint range insertion] token= 33 outcome= INJECTED detail= ""
[Roclint inline apply] phase=completed request= 1 outcome= INJECTED
```

The immediately preceding observation identifies field 33 as the disposable
`AntigravityRangeFixture.exe` editable control. Fresh Computer Use UI Automation
readback from window 2820702, edit control 101, independently showed:
**Please keep all existing tests unchanged**. The user performed the mouse click;
no assistant input helper or cross-window click bypass was used. The captured
window image was occluded by other applications, so it is not offered as visual
proof of the editor's text. The independent UIA value and native completion log
agree. `live-apply-verification.json` records this receipt.

The brief APPLIED label itself was not captured live. Its binding to native
completion and visual rendering passed automated tests. The earlier v15 manual
verification discrepancy remains historical; this new v16 pass does not rewrite
that evidence.

## Remaining boundaries

- Popup placement was observed on one 200% display; other scales/monitor layouts
  and unsupported caret providers need separate coverage. Field-rectangle fallback
  remains when caret geometry is unavailable.
- Completion feedback is intentionally suppressed for dismissed/superseded drafts,
  different fields and stale workspace/pipeline bindings. A changed/partial draft
  echo can therefore suppress a later failure message; no success is fabricated.
- The non-activating Qt Tool popup is visible in Computer Use screenshots but is
  not exposed as a selectable window by that helper. Native mouse Apply requires
  a manual click; do not target it through the editor's window coordinates.
- Existing full-draft model audit, literal/code guards, approval and exact native
  insertion readback remain in force. This task does not add editor-wide
  underlines or establish IDE/macOS parity.

No commit or push has been made.
