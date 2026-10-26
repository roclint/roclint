# Async SQLite persistence

The desktop host uses one `StorageService` thread for SQLite opening, migrations,
recovery, queries, writes and closing. Repository objects and SQLite handles stay
on that thread. GUI state remains on the GUI thread; repository RPC arguments and
results are owned values. History accessors return cached data and signal refreshes.

## Command completion

Persisted QML commands return an exact integer request ID. Zero means rejection.
An accepted request completes through
`storageCommandFinished(requestId, name, succeeded, errorCode, message)`.
A nonzero return value means acceptance, not success. UI consumers must wait for
that signal before showing success or clearing entered answers. `isStorageAvailable`
reports initialized, healthy storage; `storageBusy` reports pending mutations and
completion delivery. SQLite diagnostics remain internal; UI errors preserve the draft.

Draft edits invalidate analysis and approval immediately. Consecutive edits to the
same revision flush after 150 ms idle or 1 second of continuous input. A revision
freeze, analysis, or approval flushes them first. Request ranges keep burst metadata
bounded; completion signals yield every 64 notifications. Durable commands have a
128-entry admission limit. SQL transactions compare the expected durable version,
revision and monotonic generations; draft ranges do not create text revisions.

Only confirmed writes publish non-draft state. Edits during pending writes cancel
stale continuations; approval also revalidates after emitting Qt publication signals,
before clipboard or AX delivery. A database failure blocks dependent actions. The
explicit Retry history action reconciles the database with the visible draft and
advances beyond both stored and previously observed versions; it never silently
reverts the editor. Shutdown flushes asynchronously, then interrupts outstanding
SQLite work after a two-second grace period. Busy and progress handlers honor
cancellation. No GUI-thread database waits or nested event loops are used in production.

Test-only wait helpers pump events to observe asynchronous completion. Regression
fixtures use isolated databases and verify writer contention, a 512-edit burst,
queue rejection, stale and reentrant approval, busy failure, explicit recovery,
transaction rollback, and shutdown. This change does not implement the separate
POSIX guardian, IPC deadline, or migration-serialization phase.

## Verification

On macOS, the refreshed build, all 41 registered CTests, all 48 Python unit tests,
the structure checker, and `git diff --check` passed. The native Cocoa desktop
`--dry-run` also exited successfully. An additional offscreen desktop probe
crashed in the existing `configureNonActivatingPanel` Objective-C setup: that
backend supplies no native NSWindow. This is separate from the SQLite regression
gate. Windows and Linux execution were not available in this session.
