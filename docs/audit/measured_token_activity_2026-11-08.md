# Measured generation and decode activity

Date: 2026-11-08. Baseline: `c3b7490` on `feat/roc-hud-refresh`.

The user rejected passive motion and reported inflated token readings. This change supersedes the ambient-motion behavior and acceptance recorded in `wave_motion_runtime_fix_2026-11-08.md`.

## Timing defect and correction

The pinned llama.cpp implementation submits GPU operations asynchronously. `llama_process(...DECODE...)` delegates to the context's decode function; reading logits later synchronizes the backend. Roclint timed the submission without an explicit completion wait, so much of the forward-pass time landed inside the next sampling timer. Dividing output tokens by that incomplete decode duration inflated decode throughput.

`GraniteEngine::decode` now calls `llama_synchronize` before closing the timed phase. Prompt prefill also completes before the generation-loop clock starts. Successful generation time is captured at loop completion, before cache maintenance and response validation. Failed-call timing no longer falls back to whole-request/prefill time when generation never began. Cancellation and worker-thread ownership remain unchanged.

Both speeds count the **same native output tokens**, including structured model output for the current request:

- Generation: output tokens / generation-loop elapsed time, including sampling, detokenization, streaming callbacks and completed forward passes.
- Decode: the same output tokens / completed forward-pass elapsed time, including the GPU completion wait. It is compute throughput, not another stream of output tokens.

Neither rate includes model loading or prompt prefill. Neither represents visible rewrite words, combined analysis/rewrite/audit token totals, or the full user-operation latency. Settings and the meter tooltip explain this scope.

The runtime now uses generation throughput for its generic speed, and the host and QML no longer substitute decode throughput for unavailable generation data. Missing rates display a dash. Streaming and final metrics share bounded, finite, integral-count and monotonic-time validation, including `0 < decode_ms <= generation_ms`; invalid final metrics clear the displayed measurements without changing the accepted text or request-binding rules.

## Live Vulkan evidence

One synthetic `clear_bounds` call per worker build used installed Q8_0 weights and the production probe. This is a timing attribution check, not a performance comparison. No application history was used.

| Measurement | Before | After |
|---|---:|---:|
| Output tokens | 42 | 42 |
| Decode time | 248.958 ms | 1,583.940 ms |
| Sampling time | 1,552.731 ms | 50.251 ms |
| Generation-loop time | 1,804.533 ms | 1,636.522 ms |
| Generation throughput | 23.275 tok/s | 25.664 tok/s |
| Decode throughput | 168.703 tok/s | 26.516 tok/s |

The output SHA-256 is identical: `0bf4cd09b4b2c112a7307f6fdb7df2bacce2c2bb144cac0dd66a1afa82465bfd`. Moving the GPU wait from sampling to forward-pass timing explains the correction; it does not claim an inference speedup. Durable counts, timings, worker hashes and scope are in `token_timing_comparison_2026-11-08.json`. Full local probe output is in `build/manual-testing/token-timing-{before,after}-20261108.json`.

## UI behavior

- No idle or zero-token preparation loop. A stalled stream settles and stops even while the pipeline remains busy.
- Separate G/D traces reuse the selected original pixel-wave PNG. Height responds to each measured batch rate; positions change from the corresponding accumulated generation/decode duration. Short, bounded interpolation makes arrivals visible. No clock-driven phase loop runs between arrivals.
- The last arrival holds intensity for 350 ms, then fades for 350 ms. Completion does not extend that tail. Last labels identify completed-request rates.
- The normal header has 12 additional pixels of height so both traces remain legible; compact-window header sizing is preserved.
- Numeric readings update at most twice per second during work and immediately on completion. Reduced motion keeps both traces static while the rates update. Hidden/minimized windows stop the visual timer.
- Controller snapshots are coalesced before QML reads their counts, durations and rates, avoiding a new count paired with the prior duration. Native counter/time rollback clears the preceding request's presentation.
- Existing generation/decode Settings modes, rate colors, rewrite candidates, background audit and explicit approval remain present.

Reference/Qt evidence: `docs/research/roclint-ui-2026-11-08-measured-token-traces.png`, `roclint-ui-2026-11-08-measured-token-idle.png`, and `roclint-ui-2026-11-08-measured-token-traces.gif`. These are Qt-rendered synthetic telemetry fixtures; their numbers are not live benchmark results. The GIF shows discrete synthetic arrivals between still idle/completed states.

## Validation and deployment

| Gate | Result |
|---|---|
| Release host, helper and affected test targets | PASS |
| Focused rendered-motion test | PASS, 8.63 s |
| Full current product-alignment UI target | PASS, 57.82 s |
| Native protocol/runtime target | PASS, 13.57 s |
| Host-controller target | PASS, 11.89 s |
| Structure and diff whitespace checks | PASS |
| Installed startup with developer Qt PATH/import overrides removed | PASS, exit 0, no stderr |
| Real installed-weight Vulkan before/after probe | PASS, same output hash and token count |

The new rapid-stream fixtures initially observed the old GUI READY snapshot after the background future had finished. The tests now drain the expected bound chunks and final GUI state before checking final telemetry. They do not use arbitrary sleeps or relax metric assertions. Regression cases cover invalid/backward partial metrics, oversized/fractional/backward final counts, missing/zero/backward times, decode duration exceeding generation duration, no rate substitution, idle/preparation/stall pixel equality, arrival-driven pixel changes, completion and reduced motion.

This is three affected CTest targets, plus the focused UI invocation; no new whole-repository or macOS test pass is claimed.

Installed host SHA-256: `A276C421E70D8CB04A47DC0379169CB36D2A22A08D1353CB090866E0A9A02F12`.
Installed helper SHA-256: `070B0C7C227BC9F9DAEE2CF41272B07A5EBC0E6E9DD578432FB7B841D87DFEDA`.
Build, staged and installed copies are synchronized. Ordinary executable shortcuts remain unchanged. A local consistent SQLite backup was made before replacing the host; database contents are not published. The existing AST tuning audit remains untouched.

Computer Use observed the installed window with separate G/D traces, still preparation, and real native request metrics. The synthetic parseItems check completed at 37.0 generation / 45.3 decode tok/s; the subsequent wording-rewrite workflow reached review with all three candidate pills and Last 35.4 generation / 49.4 decode tok/s. These are per-native-request observations, not the isolated timing probe or throughput gates. Native screenshots captured preparation and completed review; arrival-driven motion is established by the rendered-frame regression, rather than a claimed desktop recording of the full rewrite stream. No generated prompt was approved, copied or inserted. The synthetic draft was cleared after testing.

No arbitrary speed clamp or simulated token count was introduced. Native macOS timing/visual behavior remains unverified. The traces are bounded visualizations of request-level measurements, not individual-token timestamps or an oscilloscope of GPU kernels.
