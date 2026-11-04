# Live capture while the HUD remains visible

The user reported that new source-editor typing did not update the HUD, so the
new automatic analysis scheduler received no fresh draft.

Two native macOS guards suppressed both AX notifications and heartbeat polling
whenever `m_cardVisible` was true. A broad `[NSApp keyWindow] != nil` test also
treated a retained Roclint window as ownership of editor focus. These were suited
to a transient popup, not a persistent HUD. The bridge now retains the watched
target while Roclint itself is foreground, and polls/handles notifications when
the external source application is foreground. HUD visibility is not a capture
pause condition. Notification reads are restricted to the watched foreground PID.

The controller also conflated reviewing clarification controls with authoring a
local HUD draft. Review interaction now anchors that review to its source field
without declaring the HUD text independently edited. A genuine text change in
that same field invalidates old analysis, intent, proposal and approval, updates
the visible draft and schedules the existing typing debounce. Returning focus
without changing text preserves the review, and another field cannot replace it.
Actual HUD edits and restored history remain authoritative. Internal capture
updates use a distinct edit path so they do not permanently pause their own sync.

The prior UI tests injected source observations, so they could not reveal the
native bridge's visibility suppression. New controller/QML cases cover same-field
typing after an unapproved rewrite, unrelated-field protection, focus return,
continued updates, local HUD draft ownership, delivery echoes and automatic
analysis. Cocoa tests use real windows and a mock AX source, not actual capture
from another application's accessibility tree. The macOS bridge changes compile
in the desktop build; a real third-party editor retest is still needed.

At investigation time the resident process had started at 16:16, before the
automatic-analysis binary rebuilt at 16:31. This is a separate deployment issue:
the application must be fully quit and relaunched to load the latest changes.
Closing the HUD alone leaves the old resident process running.

The refreshed macOS build passed all 43 registered CTests (61.07 seconds), all 49
Python unit tests, the structure checker and `git diff --check`. The five-suite
targeted gate and focused native Cocoa run also passed. Existing Qt control-style,
focus and layout warnings remain. Model quality, real third-party AX behavior and
non-macOS execution remain separate checks.
