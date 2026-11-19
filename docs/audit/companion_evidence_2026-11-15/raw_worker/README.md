# Raw worker stage (helper-only)

This directory is produced by `tools/companion_eval/raw_discovery.py`.

**Status: NOT COMPLETED.** The existing checker/worker combination did not
produce a raw receipt; see `FAILED.md`. The host NDJSON is the authoritative raw
user-visible record.

It reuses the existing `Worker` process/frame/request transport from
`tools/check_granite_runtime.py` and the production discovery prompt builder and
schema emitted by `roclint_granite_contracts` for
`tests/fixtures/companion_eval/raw_discovery_2026-11-15.json`.

Scope and limits:

- Direct native worker only. No Julia S1 routing, no HostController interview,
  no codebase index/AST, no QML, no apply/insert.
- One sample per prompt at temperature 1.0 / top-p 0.95 / seed 42, thinking off.
- The checker still expects the older 12 s analysis capability. `V13Worker`
  reuses its transport but validates the v13 45 s / 60 s capabilities and gives
  the 45 s operation enough observer time. This does not test deadline
  enforcement.
- Raw model JSON is retained verbatim; do not treat it as the user-visible
  result. The host stage parses it and is authoritative for user-visible
  behavior.
- The rewrite contracts in the corpus exist only to satisfy the frozen 6+4
  corpus shape required by `roclint_granite_contracts`; they are filtered out
  and not executed.
