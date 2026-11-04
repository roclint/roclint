# IPC, lifecycle and migration hardening

Phase 3 follows the parser, workspace and asynchronous storage changes pushed as
`44ed23f`. Model schemas and operation budgets remain unchanged. The host never
automatically replays an interrupted semantic request.

## Process ownership and runtime threads

`OwnedProcessJob` starts POSIX helpers through `roclint_process_guardian`. The
guardian places the worker in an owned process group before releasing its exec
gate. A host-only liveness writer and separate private control socket are closed
in the worker. Worker stdin, stdout and stderr stay connected directly to Qt's
asynchronous pipes. Host disappearance sends group SIGTERM, followed by SIGKILL
after two seconds; explicit deadline enforcement can request immediate group
SIGKILL. The guardian reaps its direct worker. Linux also adopts and reaps orphaned
descendants; macOS relies on launchd to reap terminated descendants. The leader
PID remains reserved until group cleanup. Windows retains suspended launch into
a non-inheritable kill-on-close Job Object.

The Julia and native runtime facades keep GUI-owned cached snapshots. Each
runtime's process, protocol parser, request map and timers belong to a dedicated
I/O thread. Posting to a facade is serialized with its destruction. Shutdown
stops the service asynchronously; no GUI-thread process waits or thread joins
were introduced. Workspace helpers use the same process ownership mechanism.

## Deadlines and cancellation

Requests carry absolute steady-clock deadlines. Queueing consumes the existing
operation budget, and expired work is checked again immediately before pipe
submission. Native transports use `wait_until`, complete once, and remain bounded
even when the GUI or I/O event loop is delayed. Julia also has a GUI-side deadline
watchdog. Responses processed after a deadline cannot publish semantic success.

Stdout parsing reads at most 64 KiB and processes at most 32 frames per turn;
continuations yield to timers. Both output pipes continue draining during
shutdown. Existing frame and request limits remain in force. Native partial
responses verify request bindings and have a 2,048-frame/64 KiB aggregate limit.

A cancel acknowledgement confirms receipt only. A bound original terminal frame
proves the operation ended; Julia can also confirm idle through its existing
`runtime_status` operation. Until then, readiness stays false. If cancellation
does not quiesce within two seconds, the owned group is killed. Generation and
request epochs, cancellation tombstones and single-completion guards reject
late or restart-era results. Unexpected crashes retain the limit of two automatic
worker restarts per sixty seconds, without replaying failed work.

## Migration and recovery

Each migration obtains BEGIN IMMEDIATE before reading `user_version`. It rereads
under that lock, applies one required step, checks foreign keys and commits,
then reacquires the lock for another step. Per-step rollback and refusal of
newer or invalid schema versions remain intact; the schema version stays at 5.
SQLite busy handling is configured before WAL initialization.

Storage holds a canonical per-database ownership lock before opening, migrating
or recovering attempts. Another live storage owner is refused before recovery
can mark its attempts interrupted. The connection closes before releasing the
lock. Independent SQLite connections still serialize migrations through SQLite,
without relying on this application lock.

## Regression fixtures and verification

The deterministic fixtures exercise payloads above pipe capacity in both
directions, stderr floods, stalled dispatch, cancellation acknowledgements without
completion, successful idle confirmation, host SIGKILL with a SIGTERM-ignoring
worker and descendant, concurrent migrations, and competing recovery owners.
Existing suites cover malformed and late replies, worker crashes, restart limits,
stale results, storage contention and persistence before delivery.

On macOS, the refreshed desktop/native-runtime build passed all 43 registered
CTests (56.51 seconds), all 49 Python unit tests, the structure checker and
`git diff --check`. Native Cocoa `roclint_host --dry-run` verified the QML root
and exited cleanly. An initial sandboxed run lacked localhost socket, clipboard
and process-inspection access; rerunning with those fixture capabilities passed.

Linux execution and Windows Job Object CI checks were unavailable in this session
and remain unverified. Mechanical fixtures do not establish real-model quality
or inference latency.
