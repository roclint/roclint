# Dynamic token-driven waveform correction

Date: 2026-11-08. Baseline: `8dd068f`, `feat/roc-hud-refresh`.

The user rejected the uniform wave sizes. Three renderer choices caused that result: every packet appended the same eight-cell sine crest; cumulative request averages hid within-stream speed changes; and `max(generation, decode)` let the usually-faster decode rate hide generation changes in the outer contour.

## Current behavior

- History stores actual recent token/timing delta rates, one sample per accepted progress update. It no longer appends a repeated crest template. The numeric readouts keep their accurate request-average definition.
- Incoming progress advances an interpolated, unequal traveling ripple contour. This supplies the requested moving pixel-wave style even when throughput stays steady. The contour is visual activity feedback, not a literal trace of GPU kernels; it never invents numeric token counts/rates. It stops advancing when no tokens arrive.
- Both recent rates affect the single outer contour through equal visual contributions; generation supplies the cobalt body and decode the periwinkle portion. The scale remains bounded at 0–100 tok/s per channel. The displayed rates are not summed or clamped.
- A 2-pixel vertical grid exposes smaller speed changes. A bounded 48-sample history and 140 ms progress-triggered interpolation retain the stepped cobalt/periwinkle style. Idle, preparation, hidden windows, stalls and reduced motion keep their prior quiet behavior; the 350 ms hold / 350 ms fade remains.

## Evidence

A real local Vulkan Q8_0 run of the synthetic production rewrite fixture captured ten partial-result count/timing samples. Recent generation ranged approximately 29.4–50.3 tok/s; recent decode approximately 49.0–56.4 tok/s. Only numeric metrics and synthetic fixture identity are retained in `dynamic_wave_live_trace_2026-11-08.json`; application history and generated text were not captured there. The helper remains the previously verified timing-corrected binary.

The Qt regression replays those exact samples through the current renderer. `docs/research/roclint-ui-2026-11-08-dynamic-wave-live.gif` and the companion PNG show that replay. Playback is retimed for readability; this is not a desktop recording or latency benchmark. `roclint-ui-2026-11-08-dynamic-wave-variable.gif` separately demonstrates a deterministic varying-speed stress stream.

The regression also requires actual plot-pixel changes while incoming tokens continue at constant rates, exact recent-rate response to 12 alternating-speed samples, both generation-only and decode-only outer-size changes, no batch-size inflation, and the existing still idle/preparation/stall/completion/reduced-motion and numeric-integrity checks. The plot comparisons exclude the numeric labels.

## Validation and scope

Release host/UI-test build and focused regression with live-telemetry replay pass (18.43 seconds, 18.47 total). Full current product-alignment UI target passes (62.56 seconds, 62.60 total). Structure and whitespace pass. Installed startup with developer Qt PATH/import overrides removed passes, exit 0 with no stderr. Logs/captures are under `build/manual-testing/dynamic-wave*`.

Installed host SHA-256: `56727648BBFFAE2F35FCAA6462F921FEB985998646EBDDB3FFC2631D126BE373`. Helper SHA-256 remains `070B0C7C227BC9F9DAEE2CF41272B07A5EBC0E6E9DD578432FB7B841D87DFEDA`.

Only the renderer and its existing UI test change production-facing behavior. Native timing, approval, AST, rewrite and model logic remain unchanged. No new full-repository or macOS pass is claimed. Prior research is preserved; a local database backup precedes installed replacement and is not published.
