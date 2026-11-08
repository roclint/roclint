# One waveform scaled by generation and decode speed

Date: 2026-11-08. Baseline: `9cb5a4a`, branch `feat/roc-hud-refresh`.

The user requested that the size of the same waveform reflect both measured speeds. `GenerationMeter.qml` now renders each column once: generation fills its cobalt body, and decode defines its periwinkle extension/edge. Both share the baseline, crest shape and a fixed linear 0–100 tok/s amplitude scale. Above 100 tok/s the visual reaches its ceiling; numeric readings remain exact and unclamped. The two speeds are never added together.

Amplitude now follows the same request-average generation/decode measurements as the numeric meter, rather than a compressed per-batch rate. Each received update advances one fixed-width crest, so a larger IPC batch or a larger cumulative token count cannot inflate the wave's size. Existing pixel styling, bounded interpolation, arrival-only activity, completion fade, missing-rate behavior, reduced motion and Settings modes remain.

The regression compares rendered plot pixels at 10/20, 30/50 and 60/90 generation/decode tok/s. It requires increasing peak height; tests decode-only growth of the outer edge and generation-only growth of the body; and requires an identical waveform for identical speeds with ten times as many tokens per batch. Above-scale samples verify visual saturation. Prior idle, preparation, stall, completion, numeric-integrity and reduced-motion checks remain in the same target.

`docs/research/roclint-ui-2026-11-08-wave-speed-scale.gif` compares synthetic slow/medium/fast Qt-rendered states. It demonstrates the scale, not live inference timings. `roclint-ui-2026-11-08-wave-speed-fast.png` preserves the full fast fixture frame. Prior artwork and research evidence remain untouched.

Validation: Release host and UI test build; focused regression PASS (13.38 s, 13.42 total); full current product-alignment UI target PASS (55.76 s, 55.79 total); structure and whitespace PASS; installed startup PASS with developer Qt PATH/import overrides removed, exit 0 and no stderr. Exact logs are under `build/manual-testing/wave-speed-*`. This change touches the renderer and its existing test, not native token timing or inference behavior; no new full-repository or macOS validation is claimed.

Installed host SHA-256: `5808A1E79B4F6955DECEA694C032DAF9A705818A94A0254882A07488291DAC4B`. The helper remains unchanged at `070B0C7C227BC9F9DAEE2CF41272B07A5EBC0E6E9DD578432FB7B841D87DFEDA`. A local history backup precedes replacement; no database contents are published.
