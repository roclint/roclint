# Companion evaluation tools (new, evaluation-only)

Two pieces:

- `../companion_probe/` — standalone CMake project that links the main
  checkout's built Release libraries read-only and drives the unmodified
  `HostController` through public invokables/test seams. It never approves,
  inserts, sends or touches user history/editor content.
- `run_companion_eval.py` — orchestrates identity capture, ambient sampling,
  the probe, NDJSON parsing, stage spans, mechanical checks and editorial merge.
- `raw_discovery.py` — helper-only raw worker attempt (did not complete on
  2026-11-15; see `docs/audit/companion_evidence_2026-11-15/raw_worker/FAILED.md`).

## Usage

```powershell
# host baseline
C:/Python313/python.exe tools/companion_eval/run_companion_eval.py `
  --stage host `
  --out docs/audit/companion_evidence_2026-11-15

# reparse after a parser change (no model call)
C:/Python313/python.exe tools/companion_eval/run_companion_eval.py `
  --stage reparse --out docs/audit/companion_evidence_2026-11-15

# attach judgments after editing judgments_2026-11-15.json (no model call)
C:/Python313/python.exe tools/companion_eval/run_companion_eval.py `
  --stage merge --out docs/audit/companion_evidence_2026-11-15
```

The runner uses these fixed production assets unless overridden in the fixture
or config:

- Native worker: `D:/Stardust/roclint/build/Release/roclint_granite_worker.exe`
- Granite model: `D:/Stardust/roclint/models/granite-4.2-3b`
- Julia model: `D:/Stardust/roclint/models/julia-1`
- Julia Python: `D:/Stardust/roclint/build/runtime/julia-python/Scripts/python.exe`
- Runtime config: `D:/Stardust/roclint/build/generated/granite-runtime-config.json`
- Qt: `D:/Qt/6.8.2/msvc2022_64/bin`

`run_host` sets `PATH` to Qt and the worker directory. The probe needs
`runtime/julia_worker.py` beside its executable; copy
`build/Release/runtime` into the probe's Release directory after building.

## Evidence rules

- Do not invent a stage without a host status/attempt timestamp or a future
  `ROCLINT_TRACE_TIMING` event.
- Keep cold, warm and contended labels explicit.
- Keep mechanical fields, model verdicts and editorial judgments separate.
- Never present a model Pass or `canApprove=true` as usefulness evidence.
- The raw worker stage is helper-only and currently not completed; the host
  NDJSON is the authoritative raw user-visible record.
