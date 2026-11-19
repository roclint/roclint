# Companion/intent measurement evidence — 2026-11-15

Raw and machine-readable evidence for
`docs/audit/companion_quality_measurement_2026-11-15.md`.

Baseline commit: `d5c8101e42b45d5d3b0796c97f01024214fd5d57` on `feat/roc-hud-refresh`.
Measurement worktree: `D:/Stardust/roclint-wt-eval`, branch `topic/eval-2026-11-15`.
No production source, CMake, model setting or installed application was edited.

## Files

| File | Meaning |
|---|---|
| `identity.json` | Source/build/model/backend/Python/GPU identity, hashes, sampling configuration |
| `baseline_host.json` | Parsed baseline rows per scenario, stage spans, attempts, mechanical checks, model verdicts, editorial judgments |
| `host_*.ndjson` | Verbatim probe events (status transitions, questions, options, answers, raw proposal text, attempts) |
| `host_config_*.json` | Exact probe configuration (prompts, planned answers, model paths) |
| `events_parsed.json` | The same NDJSON parsed as a JSON array for tooling |
| `ambient.json` | CPU/GPU ambient samples before/during/after the run |
| `raw_worker/` | Attempted helper-only raw discovery capture against the pinned native worker. **Not completed**: see `raw_worker/FAILED.md`. Host NDJSON below is the authoritative raw user-visible record. |
| `baseline_raw.json` | Failed raw-stage attempt receipt (0 cases; capability mismatch). Not a model result. |
| `followup_explicit/` | Explicit-review follow-up for the music-app acceptance prompt (the automatic route retained the original). Complete. |
| `followup_code_ast/` | Partial code-index follow-up: the index never reported ready within 180 s; the run was interrupted before analysis. Retained as readiness evidence; see `run_manifest.json`. |
| `probe-settings/` | Isolated QSettings for the probe (no user settings or history database) |
| `comparison_protocol_2026-11-15.md` | Fair conversational-skill protocol; comparison is **NOT RUN** |
| `timing_gap_proposal_2026-11-15.md` | Missing timing observations and the proposed D00 event contract |
| `judgments_2026-11-15.json` | Editorial judgments, kept separate from mechanical/model results |

## Reproduction

```powershell
# Build the evaluation-only probe once (links the main Release libs read-only)
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S D:\Stardust\roclint-wt-eval\tools\companion_probe -B D:\Stardust\roclint-wt-eval\build-eval-probe `
  -G "Visual Studio 17 2022" -A x64
& $cmake --build D:\Stardust\roclint-wt-eval\build-eval-probe --config Release --target companion_probe
Copy-Item -Recurse -Force D:\Stardust\roclint\build\Release\runtime D:\Stardust\roclint-wt-eval\build-eval-probe\Release\runtime

# Capture host + raw worker evidence
C:/Python313/python.exe D:\Stardust\roclint-wt-eval\tools\companion_eval\run_companion_eval.py `
  --stage all --out D:\Stardust\roclint-wt-eval\docs\audit\companion_evidence_2026-11-15
```

The probe uses the documented isolated Julia Python environment
`D:/Stardust/roclint/build/runtime/julia-python/Scripts/python.exe`
(transformers 5.0.0). The system `C:/Python313/python.exe` has
transformers 5.14.1, which is incompatible with the pinned Julia-1 model code;
that incompatibility is recorded in the report. The Granite worker is
`D:/Stardust/roclint/build/Release/roclint_granite_worker.exe` (the newer worker
that matches the test binary; the older `build/Release/bin` copy was rejected
with "Native reply ID was invalid").
