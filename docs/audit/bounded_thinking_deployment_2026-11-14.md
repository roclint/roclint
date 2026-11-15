# Bounded thinking: Windows deployment follow-up

Date: November 14, 2026. This closes the executable-lock deployment blocker in
[the implementation report](bounded_thinking_2026-11-13.md). It does not change
the model-quality verdict or the recorded test results in that report.

Before deployment, every source checksum in the November 13 receipt matched
the current working tree. Roclint and roclint_host were stopped. The deployment
target rebuilt/staged successfully, including the bundled Qt/runtime files.
The existing installed host and Granite helper were backed up under
`D:\codex-evidence\roclint-thinking-20261114\before-update`.

The staged files were copied into
`C:\Users\kirby\AppData\Local\Programs\Roclint` with an incremental copy
without deletion/mirroring, excluding development executables; the host was
installed as `Roclint.exe`. The existing Start-menu shortcut already targets
that executable directly, with empty arguments. No terminal launch is needed.

| Check | Result |
|---|---|
| `roclint_deploy_qt` Release target | PASS, exit 0 |
| Incremental installed runtime copy | PASS, Robocopy exit 3, zero failed files |
| Installed host SHA-256 equals staged host | PASS |
| Installed runtime helper SHA-256 equals built/staged helper | PASS |
| Installed `--dry-run`, Qt removed from PATH and QML/plugin overrides removed | PASS, exit 0; QML roots verified; clean exit |
| Normal installed app launch | PASS: native HUD observed with empty draft and disabled empty-draft actions |
| Settings opening | Settings window observed behind pinned HUD |
| Manual thinking toggle verification | NOT COMPLETED: computer-use window binding failed |

Installed host SHA-256:
`C4369D173653338DAFAFEC4E9C97B5A96DDC416F4001FDBF6AB37AE71151748A`.
Installed helper SHA-256:
`9161B13F6570FA1E1F734F7C5BDCF03831F58ECC6FE8EB258F0EDF770EF3BDF6`.

Computer-use first captured the normal installed HUD and its Settings action.
While navigating the related settings window, an accessibility click reported
an out-of-bounds point. Fresh selection/reopening then returned a HUD ID that
`get_window` rejected with `window id 68614 was not found`. Automation stopped;
no successful manual toggle, saved setting change or final visible-window
state is claimed. The Roclint process remained running after the first lost
binding. The implementation's controller/QML persistence and meter checks are
the automated evidence for the new setting. A later hands-on check should
verify the control in the installed Settings window.

The installed HUD also reported `Codebase index refresh incomplete; grounding
withheld` for its saved Stardust workspace. That warning is recorded separately:
this deployment does not establish a complete AST snapshot for that workspace.
No prompt was rewritten, approved, copied, inserted or sent during this check.

Evidence: `D:\codex-evidence\roclint-thinking-20261114`, including deployment
build/copy logs, installed dry-run stdout/stderr, executable backups and the
deployment receipt. Existing limitations remain: experimental/off-by-default
thinking, inconsistent measured word-suggestion gains, development deployment
rather than clean-machine release certification, and native macOS NOT RUN.
No source implementation changes, commit or push were made in this follow-up.
