# Serialize model completion with persisted user commands

A model-result callback could publish an analysis while a user command was waiting for SQLite. Both could submit writes against the same state version, causing a false history failure.

Discovery, triage, scoring and rewrite result application now use the existing bounded command queue. Delayed native rewrite resumption uses the same ordering and revalidates its captured intent, version and configuration. Existing draft-flush, generation, deadline and stale-version checks remain. Internal model continuations do not clear a user command error or emit a requested UI-command completion. Queue rejection preserves the draft and requires explicit retry.

Send as-is preserves typed commit errors. A stale gesture cannot deliver, poison healthy history, or replay itself automatically.

Regression fixtures cover a model write held by a SQLite lock while an old-version command is queued, GUI heartbeat progress, durable/controller version agreement after explicit retry, model completion with a full command queue, and stale Send as-is without clipboard changes. No model thresholds, classifier templates, discovery instructions, database schema, worker transport or public persisted-command API changes are included.
