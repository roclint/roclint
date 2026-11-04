# Automatic analysis after a typing pause

The user reported that typing did not start analysis after the HUD visibility fix.
Previously, editing only invalidated old results; Analyze was explicit. The open
HUD now debounces local and followed source-editor changes for 800 ms, then checks
the latest exact draft through the existing persisted analysis command.

Only one pending draft is kept. More typing replaces it and restarts the timer.
The pending check waits for storage readiness, draft flushes and cancellation
quiescence. Manual Analyze can start sooner and consumes the scheduled check.
Blank input, hiding/minimizing, history restore, explicit cancellation and storage
failure discard pending work. Failure/completion/status notifications never
reschedule the same draft. A fresh edit or explicit Analyze can try again.

The scheduler never confirms decisions, generates a rewrite without the existing
explicit-decision flow, approves, or delivers. It preserves the user's HUD layout.
Successful insertion resumes following only the same captured field and pipeline.
Its observed delivery echo is ignored; fresh source typing starts the next check.
Edits made in the HUD during pending delivery are not replaced or made followable
by a late completion. Unrelated editor fields cannot replace an unapproved review.
A subsequent synchronization follow-up separates review interaction from actual
HUD-authored text: changing the same source composer invalidates its review and
queues a fresh check. macOS capture no longer pauses just because the HUD is visible.

Deterministic QML regressions use short configurable debounce intervals and cover
typing bursts, source updates, SQLite writer contention, edits during inference,
stale results, explicit cancellation, malformed-reply failure, a fresh edit after
failure, hiding/showing, blank drafts, programmatic restore, delivery echoes and
new typing after insertion. The focused Cocoa opt-in exercises these cases with
native windows, synthetic data and a mock AX target; it is not a hands-on test of
third-party editor capture/insertion.

The targeted persistence, dual-mode and product-alignment gate passed all three
suites. The final refreshed build passed all 43 registered CTests (61.84 seconds),
all 49 Python unit tests, the structure checker and `git diff --check`. The focused
native Cocoa run also passed, with existing Qt control-style, focus and ScrollView
layout warnings. Model quality and real third-party editor integration remain
separate checks. The resident process must be fully quit and relaunched to load
the rebuilt QML and controller code.
