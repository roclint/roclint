# Live pixel waveform renderer

Date: 2026-11-08. Baseline: `cfe85cf` on `feat/roc-hud-refresh`.

The user requested an actual waveform in the selected pixel style. Moving or stretching the same image did not meet that request. This supersedes the renderer in `measured_token_activity_2026-11-08.md`; its backend timing correction and metric definitions remain current.

## Change

`GenerationMeter.qml` now paints an evolving waveform with a Qt Canvas. It does not render `pixel-signal.png`. The original artwork remains a style reference and repository asset.

Each new native token batch adds a crest to a bounded 48-cell history. The measured generation/decode batch rates determine their respective amplitudes; larger coalesced token batches produce wider crests. Bounded interpolation moves the history between arrivals. Neighbor smoothing joins hard 5 × 3 pixel cells into a stepped, filled envelope, with layered cobalt generation and periwinkle decode contours. Both modes share one plot; the existing single-metric settings render their selected layer.

The crests visualize arriving **batches**, not measured per-token timestamps or GPU kernel waveforms. Their shape is a presentation envelope; numeric tok/s remains the authoritative throughput measurement. The two layers use the same output-token count. The corrected GPU completion timing, unavailable-rate behavior, rate colors and 500 ms numeric throttle are unchanged.

Idle and zero-token preparation show a still baseline. The existing 350 ms hold / 350 ms fade settles the waveform after arrivals stop. Reduced motion shows a static baseline while numbers continue updating. Hidden/minimized surfaces stop the interpolation and visual timer. No passive loop supplies activity.

## Verification

- Release host and product-alignment test target build: PASS.
- Focused meter regression: PASS, 10.71 seconds (10.75 total).
- Full current `roclint_product_alignment` target: PASS, 54.19 seconds (54.23 total).
- Structure and `git diff --check`: PASS.
- Installed startup without developer Qt PATH/import overrides: PASS, exit 0, no stderr.
- Installed/build/staged host SHA-256: `F8DBE9893A4E4A38EAF6356F053B40A9AF3F84698A417495E8B0242256385FE6`.
- Native helper remains unchanged at `070B0C7C227BC9F9DAEE2CF41272B07A5EBC0E6E9DD578432FB7B841D87DFEDA`.

The regression now compares pixels within the waveform alone, excluding the rate labels. It requires changing waveform pixels on progress, distinct generation/decode histories with actual crests, no old image renderer, still idle/preparation/stalled/completed frames, and reduced-motion stability. Existing no-rate-substitution and timing checks remain. The full UI workflow also covers the current candidate/approval controls and compact layouts.

This is a UI renderer change. No new full-repository, native inference benchmark or macOS pass is claimed. The preceding real Vulkan timing probe remains evidence for the unchanged telemetry backend. Normal installed launch is checked separately from the Qt fixture.

## Visual evidence

The original source reference and Qt-rendered waveform were inspected together. The initial literal rate-history renderer produced a broad plateau; the final arrival-crest envelope restores the reference's separated, layered pixel peaks while allowing the shape to evolve.

- `docs/research/roclint-ui-2026-11-08-live-pixel-waveform.png`: an active Qt-rendered fixture frame.
- `docs/research/roclint-ui-2026-11-08-live-pixel-waveform-idle.png`: still baseline.
- `docs/research/roclint-ui-2026-11-08-live-pixel-waveform.gif`: nearest-neighbor enlarged header crop from actual Qt animation frames. Playback is a compact visual preview, not a native desktop recording or real-time inference benchmark.

All preview rates/counts are synthetic fixture input. Original captures and logs remain under `build/manual-testing/pixel-waveform*`. A consistent history backup was kept locally before replacing the installed host; its contents are not published. Prior research assets and the AST tuning audit remain untouched.
