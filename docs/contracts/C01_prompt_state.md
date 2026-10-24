# C01 — Prompt revision and state contract

**Authority:** human-written C++ core.

## Required concepts

- `SessionId`: non-empty stable identity for one prompt-working session.
- `RevisionId`: stable identity for one immutable committed prompt revision.
- `PromptRevision`: session, revision, parent revision when applicable, canonical prompt bytes/text, created ordering metadata.
- `StateVersion`: monotonically advances for authoritative mutations; callers provide expected version.
- Editing a draft immediately makes analysis, intent, proposal, and approval from the old revision stale.

## Canonical text

Persist/compare application-canonical UTF-8 text with LF newlines. Do not silently Unicode-normalize user text.

## Command behavior

Mutating commands return either a new state/version or a typed stale/invalid result. No UI or provider result may bypass the core to mutate authoritative state.

## Explicit non-goals

This contract does not define QML widgets, provider schemas, SQL tables, or external file mutation.
