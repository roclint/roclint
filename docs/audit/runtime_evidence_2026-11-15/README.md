# Runtime timing evidence — 2026-11-15

Helper-only experiment for the runtime trace, queue wait, cancellation and
expired-queued-work behavior. It is not a full-app, host, UI or Julia measurement.

## Configuration

- Worktree build: `D:/Stardust/roclint-wt-runtime/build`, Release, Vulkan,
  `roclint_granite_worker.exe` built from this branch.
- Model: pinned `granite-4.2-3b-Q8_0.gguf` from `D:/Stardust/roclint/models/granite-4.2-3b`.
- Sampling: manifest `v13.context_quality`, temperature 1.0, top-p 0.95, seed 42.
- Thinking: disabled (the production default; `thinking_enabled=false` in both
  the envelope and the payload).
- Prompt/schema: production discovery builders via the existing
  `roclint_granite_contracts.exe` and `app/runtime/granite-benchmark-fixtures.json`,
  cases `ambiguous_speed` and `three_groups`. Prompts are synthetic.
- No text is written to the trace file.

## Sequence

1. `hello`, `load` (cold readiness).
2. Analysis A (`ambiguous_speed`).
3. Analysis Q (`three_groups`) submitted while A runs; it must wait.
4. Analysis B (same instructions as A) submitted while A/Q run, then cancelled
   before its turn; no inference should start for B.
5. Analysis D (same instructions as A) submitted with `remaining_budget_ms=100`
   and no cancel; it must be rejected before inference when dequeued.
6. Analysis C (same instructions as A) after the queue drains, as a warm repeat.
7. `shutdown`.

## Run

```powershell
$out = "...\runtime_evidence_2026-11-15"
python "$out\live_driver.py" `
  --worker D:\Stardust\roclint-wt-runtime\build\Release\roclint_granite_worker.exe `
  --model-dir D:\Stardust\roclint\models\granite-4.2-3b `
  --manifest D:\Stardust\roclint-wt-runtime\build\generated\granite-runtime-config.json `
  --case "$out\contracts.json" `
  --trace "$out\runtime_trace.jsonl" `
  --out "$out\receipt.json"
```

`contracts.json` is the output of
`roclint_granite_contracts.exe app/runtime/granite-benchmark-fixtures.json`.
`receipt.json` retains the synthetic raw model text and numeric metrics;
`runtime_trace.jsonl` contains no prompt or output text.

## Result (second run)

Worker SHA-256:
`2DE1FE7F362904BFA717358D48317F0C903D76FC0024F1868F07AD744A707123`
(built 2026-11-15 from this branch; Vulkan backend).

| Span | Value |
|---|---:|
| cold `model_ready` | 66,718 ms |
| — SHA-256 verification | 52,216 ms |
| — model map/load | 12,393 ms |
| — context init | 1,448 ms |
| — warm-up | 135 ms |
| A warm request total | 3,668.5 ms |
| A prefill / first token / decode | 376.8 / 381.9 / 2,719.9 ms |
| Q queue wait / service | 3,298.6 / 1,946.4 ms |
| B cancelled at dequeue | yes, no inference |
| D expired at dequeue (`remaining=-4,889.3 ms`) | yes, no inference |
| C warm repeat total | 3,486.5 ms |
| C prefill / first token | 126.0 / 129.2 ms |
| KV reused / candidate (C) | 512 / 669 |

The first run (before the pre-dequeue cancellation check) recorded cold 70,066 ms,
A total 3,810.9 ms, Q queue wait 3,450.2 ms and C total 3,314.9 ms; the second run
is the retained receipt/trace.
