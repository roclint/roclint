# C02 — Storage repository contract

**Authority:** human-written SQLite schema/migrations/queries/transactions and C++ repository code.

## Required behavior

- SQLite lives at `QStandardPaths::AppLocalDataLocation/history.db` unless a test override is supplied.
- Foreign keys are enabled and session-scoped relationships cannot cross sessions.
- Migrations are explicit and versioned.
- Prepared statements bind user/model data; no string-concatenated SQL for dynamic values.
- A repository transaction either commits the complete logical mutation or leaves prior authoritative state intact.
- Reopen reconstructs current session/revision/intent/proposal/approval state exactly enough for the core to revalidate it.
- Session deletion removes session-owned logical state and completes deletion of newly unreferenced content blobs before the UI reports logical deletion complete, where feasible in the same transaction. This is not a claim of forensic erasure.

## Boundary

The repository returns typed data/result objects to the core. UI/model adapters do not issue SQL.
