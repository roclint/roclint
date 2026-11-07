# Phoenix AST, rewrite and clarification regression run

Date: 2026-11-06, America/Los_Angeles.

## Result

The frozen regression replay passed 199/200 automatic checks: AST 100/100,
rewrite 99/100. Its strict evaluation verdict is FAIL. The separate clarification
development replay passed 13/13. Both publications succeeded and all 213 runs,
213 traces and 213 annotated records were read back from local Phoenix.
No execution errors or missing samples occurred.

| Suite | Development | Reused held-out | Total |
|---|---|---|---|
| AST | 69/69 | 31/31 | 100/100 |
| Rewrite | 72/72 | 27/28 | 99/100 |
| Clarification development | 13/13 | Not applicable | 13/13 |

AST supported resolutions were 69/69, resolution coverage 1.0, with zero wrong
Verified outcomes among 69 Verified decisions. Frozen fixtures, labels, splits
and thresholds were unchanged. Previously inspected held-out cases are reused
regression evidence, not a fresh unseen acceptance sample.

## Reproduced defect

`clarification_4` confirms alternative `alt_0_1`, **Higher throughput**, for the
question **What should faster mean?**

Input and resulting rewrite were identical:

> Refactor Compute::run so it runs faster. Do not change result ordering.

The selected throughput goal is missing. The exact failed assertion is
`missing_requirement:(?:higher|increas|improv).{0,30}throughput`.
This reproduces the prior v9 result; it is not a newly introduced regression.
The runner exited 1 for evaluation failure despite successful publication.
Do not weaken the fixture or tune against this reused held-out example.
The next product fix should preserve confirmed clarification meaning, developed
against independent development examples before rerunning the frozen set.

## Scope and remaining judgments

These runs exercise production AST/index/rendering and Granite rewrite adapters
with pinned native runtime configuration and Q8_0 weights. Clarification fixture
discovery/answers are supplied synthetic inputs. This does not test live Julia
routing, generated discovery-option quality, the native HUD, or insertion.

All 113 rewrite records retain an unscored `human_review_required` semantic
annotation. Mechanical passes do not settle semantic usefulness. For example,
`vague_goal_explicit_skip` adds meta-instructions about preserving task meaning,
and `rough_independent_actions` adds "code quality" / "refreshed functionality"
phrasing. These are provisional wording concerns for review, not additional
scored failures or completed human adjudication.

Confirmed Reader readability/reliability development examples retained their
selected goals; the mixed Reader/Queue case kept the unselected Queue goal open.

## Provenance and measurements

- HEAD: `c4bd6f86fdd407b34312a5a5069356eb9a78285c`.
- Source fingerprint for both runs:
  `87c59c9e1c1616a452f7c59a3e66f3412cb7cc358479ff45800a5bf9feb074ee`.
- Both before/after source checks matched; `source_changed=false`.
- The pre-existing user edit to `docs/audit/ast_rewrite_tuning_2026-11-06.md`
  was preserved. This report was added only after both source checks finished.
- Evaluator version 3. Full manifests preserve fixture, model, tokenizer-container,
  native binary and runtime/configuration hashes.
- Frozen replay rewrite exchange: n=100, median 1,121.335 ms, nearest-rank p95
  1,327.57 ms. Separate model load: 65,015.35 ms.
- Clarification replay exchange: n=13, median 1,005.37 ms, nearest-rank p95
  1,360.76 ms. Separate model load: 49,599.20 ms.
- These are component measurements, not end-to-end HUD latency or a controlled
  performance comparison. Both workers shut down gracefully.

## Phoenix records

- [AST experiment 12](http://127.0.0.1:6006/datasets/RGF0YXNldDo1/compare?experimentId=RXhwZXJpbWVudDoxMg==):
  100 runs/traces/annotated records verified.
- [Rewrite experiment 13](http://127.0.0.1:6006/datasets/RGF0YXNldDo2/compare?experimentId=RXhwZXJpbWVudDoxMw==):
  100 runs/traces/annotated records verified, including the failure.
- [Clarification experiment 14](http://127.0.0.1:6006/datasets/RGF0YXNldDo3/compare?experimentId=RXhwZXJpbWVudDoxNA==):
  13 runs/traces/annotated records verified.

Readback checked case identity, stored checks/status, score annotations,
unscored semantic labels and trace counts against local results.

## Reproduction and artifacts

From `D:\Stardust\roclint`, after building the existing native targets:

```powershell
& E:\phoenix_arize\.venv\Scripts\python.exe tools/roclint_eval.py `
  --suite all --split all --qt-bin D:/Qt/6.8.2/msvc2022_64/bin `
  --output build/evaluations/<new-frozen-run> --publish --strict-publication

& E:\phoenix_arize\.venv\Scripts\python.exe tools/roclint_eval.py `
  --suite rewrite --split development `
  --rewrite-fixtures tests/rewrite/rewrite_usefulness_development.json `
  --no-freeze-check --qt-bin D:/Qt/6.8.2/msvc2022_64/bin `
  --output build/evaluations/<new-development-run> --publish --strict-publication
```

The development fixture is intentionally outside the frozen manifest; the
explicit unfrozen flag is not used for the 200-case frozen run.

Local reports and case JSONL:

- `build/evaluations/phoenix-regression-20261106/`
- `build/evaluations/phoenix-clarification-20261106/`

Logs are under `build/manual-testing/session-20261106/`, named
`phoenix-regression-20261106.log`, `phoenix-clarification-20261106.log`,
and `phoenix-cli-server.*.log`.

## Operational notes

The configured E: drive was initially absent and was reconnected by the user.
The existing `E:\phoenix_arize\run_server.py` failed because it calls
`main(["serve"])`, while the installed CLI main accepts no positional arguments.
The supported installed `phoenix.exe serve` launcher started successfully.
The failed launch log remains preserved; no external launcher or package was edited.

Installed versions: Phoenix 21.0.0, client 3.7.0, OTEL 0.17.2, OTLP protobuf
1.45.2. No upgrades or reinstallations were performed. Phoenix stays bound to
127.0.0.1 and is left running for inspection.

The isolated computer-use QA app and its verified owned model workers were
paused to free GPU memory for sequential evaluation. Its database and earlier
history were preserved; the same QA launch is restored after evaluation.
No production implementation changes, commits or pushes are part of this run.
