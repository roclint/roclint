# Windows IDE compatibility checkpoint

Date: 2026-11-07. Native observations were collected November 6; final regression
verification and this checkpoint were completed November 7.

## Changes

- Recognize the separately installed `Antigravity IDE.exe` in app classification,
  explicit ambient policy and Electron accessibility handling.
- Reject Chromium `RootWebArea` documents as prompt fields. Read-only/mixed
  document ranges are not evidence of an editable composer.
- Resolve the active caret's enclosing editor within the same process before
  bounded ancestor lookup; accept writable combobox composers.
- Recheck editor eligibility on every read and insertion readback.
- Try an editor's text range and legacy accessibility value when its value
  pattern is empty. Empty rich-editor values remain unsupported: Chromium can
  report an empty value for a visibly nonempty draft. This conservative rule
  prevents authorizing replacement against a falsely empty value, at the cost
  of refusing genuine empty rich-editor destinations too.

## Native evidence and limits

Standalone Antigravity originally captured 1,671 characters of page/sidebar
content instead of its 103-character synthetic prompt. After the page-root/caret
correction, the captured revision matched that 103-character draft exactly.
The exact-capture check also passed after the initial text-provider fallback.
These observations predate the final conservative empty-value rule; nonempty
values continue through the same path, but no later native retest is claimed.

Antigravity IDE launched and exposed its composer, but its accessibility value
remained empty while its text child visibly held a 73-character draft. Direct
capture/insertion is NOT verified. The final policy refuses empty rich-editor
reads; this is containment, not a claim that the provider problem is solved.

Android Studio Quail 1 / 2026.1.1 launched. The user handled its usage-statistics
choice. Opening the isolated fixture reached a project-trust dialog with a
Microsoft Defender exclusions option. Automation left that security dialog
untouched. The actual AI composer remains NOT TESTED.

The computer-use provider could observe Roclint's HUD in desktop captures but
did not return it as a targetable window. No Roclint control was clicked through
another application's handle. Approved insertion, exact inserted readback,
clipboard restoration, multi-window/worktree association, native macOS behavior
and complete per-editor compatibility remain open.

No downstream prompt was sent. Existing conversations and project source files
were preserved. The initial accidental page capture stayed in the isolated local
history; it was not published to Phoenix.

## Validation

- Release host and all 48 native test targets rebuilt successfully.
- Full CTest: **52/52 passed**, 111.68 seconds, `-j2`.
- Added policy regressions cover page roots, writable documents/comboboxes,
  falsely empty rich-editor values and preserved ordinary empty edit behavior.
- Logs: `build/manual-testing/session-20261106/pre-design-final-build.log` and
  `pre-design-final-ctest.log`.

The previously reported Phoenix replay remains 199/200 with the confirmed
throughput omission unresolved. It is separate component evidence and was not
rerun for this host-only change.

## Handoff

The user has prioritized the native HUD redesign over further compatibility
testing. Preserve the approved rounded blue/Roc-bird references and implement the
new presentation without changing analysis, approval or delivery authority.
The unrelated user edit to `ast_rewrite_tuning_2026-11-06.md` is excluded from
this checkpoint commit.
