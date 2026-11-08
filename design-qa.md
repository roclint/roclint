# Roc HUD and Windows launch QA

Date: 2026-11-07. Scope: first native HUD refresh and ordinary Windows launch. The settings window receives shared colors/branding only; its full information architecture and control redesign remain a later slice.

## Visual truth and capture

- Approved source: `docs/research/roclint-ux-2026-11-05-approved-rounded-roc-long-prompt.png`, 1254 × 1254 pixels. Its HUD occupies approximately x474–1208, y49–627; editor/backdrop and the lower external composer are context, not Roclint window content.
- Decision record: `docs/research/2026-11-05-roclint-ui-ux-direction.md`. Its later user-controlled rewrite requirement supersedes the older mock's CTA wording.
- Implementation: `docs/research/roclint-ui-2026-11-07-clarification-implemented.png`, 820 × 600, actual production QML rendered by Qt at DPR 1; state: three alternative clarification choices, custom answer and explicit skip/rewrite. The original test artifact is `build/manual-testing/ui-refresh/clarification-options.png`. Test transport supplies synthetic content; this is not a real-model quality claim.
- Durable companion images: `docs/research/roclint-ui-2026-11-07-review-implemented.png` and `docs/research/roclint-ui-2026-11-07-compact-implemented.png`.
- Additional captures: `requested-rewrite.png`, `review-checks-560x400.png`, `hud-controls-420x200.png`, `hud-controls-620x250.png`, `hud-controls-820x520.png` in that directory. A full native software-rendered QML test, rather than a web clone, exercises focus, layout and workflow.
- Normalization: compare the source HUD region against the implementation window; do not compare the source editor/composer or the 1254-square canvas against an 820-wide app. No browser/CSS viewport applies to this native Qt app. The source's display density is unknown; do not infer exact CSS sizes from its bitmap.
- Full-view evidence: source and implementation were presented together in the same comparison tool call. Focused inspection covered their header/logo, large question, option rows, custom answer and bottom actions at readable resolution. The implementation's extra workspace/model controls and three-option explanation text are intentional product requirements, not omitted source elements.
- Installed native observation: computer-use returned `Roclint — prompt assistant` as a normal targetable window from `%LOCALAPPDATA%/Programs/Roclint/Roclint.exe`; its screenshot and accessibility tree exposed the prompt editor, Clarify, Rewrite prompt, settings and close controls. No terminal appeared.

## Live meter follow-up — 2026-11-07 (historical; superseded below)

The user's new reference is `docs/research/roclint-ui-2026-11-07-streaming-meter-reference.png` (502 × 144). It supersedes the decorative header waveform: remove Technical details and show tokens per second / streaming activity.

Implementation evidence: `docs/research/roclint-ui-2026-11-07-streaming-meter-implemented.png` and `roclint-ui-2026-11-07-streaming-meter-idle.png`, each 820 × 600, Qt DPR 1. The active screenshot uses deterministic synthetic token-arrival data (42.5 tok/s is fixture data, not a benchmark). Compare the source's cobalt stepped motif with the implementation header region; the source is a crop without the app body, so canvas sizes are intentionally different. Source and implementation were inspected together. The numeric readout and flat idle state are required functional changes, not an attempt to reproduce a fixed decorative silhouette.

- Typography: 12 px mono rate, 10 px in narrow space, bounded/elided to avoid overlapping window controls.
- Spacing: the meter stays within the former 185 × 32 header area; removal of the diagnostic link closes its old gap.
- Tokens/colors: same ink-blue/cobalt palette. Heights encode actual token arrival deltas with a relative recent peak; the text reports measured decode tok/s.
- Image quality: Roc branding is preserved; the waveform is a native data chart rather than a generated image or invented logo.
- Content: Technical details and its raw diagnostic summary are removed. Generating rewrite content shows friendly preparation/writing status instead of raw JSON. Idle has no invented speed; warmup hides the previous request's rate.
- Motion/accessibility: no random/sine activity; no new tokens yields zero-height activity; idle stops the timer. Reduced motion keeps numeric updates and a quiet graph. The readout explicitly cannot take keyboard focus.

Verification: full CTest 52/52, then the final UI workflow rerun passed. The focused meter fixture also passes token deltas, stalls, request rollback and reduced motion. A first screenshot fixture lost its synthetic processing flag when its window became visible; the fixture now sets that state after showing the window and asserts non-flat history before capture. Native installed observation confirmed ordinary launch, no Technical details action and actual completed decode rates of 220.5 and 181.8 tok/s on one synthetic local session. Those are observations, not release performance gates. Native before-completion telemetry is covered by the bound runtime fixture; the live desktop observations captured completed rates. No generated prompt was approved, copied or inserted.

## Reference-wave correction — 2026-11-08 (historical; superseded below)

The user rejected the histogram-like implementation as visually unlike the reference and too brief to perceive. This supersedes the November 7 chart assessment above.

Source truth: `docs/research/roclint-ui-2026-11-07-streaming-meter-reference.png` (502 × 144). The same original `assets/pixel-signal.png` now supplies the layered silhouette and cobalt/periwinkle pixels. A clipped image highlight travels across it. No replacement icon/art is drawn with bars.

Rendered evidence: `docs/research/roclint-ui-2026-11-08-pixel-wave-implemented.png` (820 × 600 Qt window, DPR 1) and `docs/research/roclint-ui-2026-11-08-pixel-wave-short-burst.gif`. The GIF is a 275 × 50 header crop, enlarged to 550 × 100 with nearest-neighbor sampling. Source and implementation were inspected in the same comparison input; multiple sweep positions were inspected at header scale. Source canvas padding is not part of the native meter. Its 123.4 tok/s value is synthetic fixture input, not measured performance.

- **P2 repaired — silhouette drift:** reused the actual layered pixel artwork instead of the uniform bars. The new meter remains bounded within the header (up to 250 × 40, shorter when compact).
- **P2 repaired — imperceptible short bursts:** activity is latched on token arrival, held for 1.8 seconds and faded for 0.7 seconds. It survives completion even if generation finishes before the first display tick. This adds no inference or review delay.
- **Typography/content:** mono rate updates at most twice a second; completed rates say Last. Newer generation/decode display options and colors are preserved. The animation fixture checks visible copy independently of that markup.
- **Colors/assets:** original pixel layers and transparency retained. Quiet reference art remains visible while idle; the sweep identifies recent activity rather than claiming continued generation.
- **Interaction/accessibility:** reduced motion suppresses the sweep, not the numeric rate. No Technical details action returns. The focused timing test and the full current product-alignment UI target pass. Installed startup and ordinary native launch are verified.

See `docs/audit/pixel_wave_visibility_2026-11-08.md` for exact scope and results. Final result for the reference-wave correction: passed.

## Runtime motion correction — 2026-11-08 (historical; superseded below)

The user reported that the animation still did not work after additional product changes. The prior sweep assessment above did not establish visible motion during actual preparation. This section supersedes its animation verdict.

- **P2 repaired — token-only activation:** Julia checks and model warmup can run before any Granite tokens arrive. The meter now moves gently while visible at idle and more strongly throughout `processing`, including zero-token work. Resetting a native subrequest's counters clears measurements without cutting off activity.
- **P2 repaired — imperceptible movement:** replaced the faint highlight with quantized height and horizontal movement of the original reference PNG. The visible timer ticks every 50 ms; idle and working cycles are approximately 3.2 and 1.1 seconds. Completion holds working intensity for 1.8 seconds, then eases to idle over 0.7 seconds. Hidden/minimized windows and reduced motion stop the timer.
- **Reference comparison:** the original 502 × 144 reference and current Qt header crop were inspected together. The cobalt/periwinkle silhouette comes from the same PNG; motion transforms that artwork rather than substituting uniform bars. Native header size differs from the reference canvas padding.
- **Evidence:** `docs/research/roclint-ui-2026-11-08-visible-wave-motion.gif` is a nearest-neighbor enlarged Qt-rendered header crop; 123.4 tok/s is synthetic fixture input. `docs/research/roclint-ui-2026-11-08-wave-working-no-tokens.png` shows the zero-token preparation fixture. The installed Windows window was separately observed changing shape between native screenshots at idle and during Preparing / Loading model / Checking, before tokens arrived.
- **Preserved product work:** generation/decode modes, their distinct colors and Settings selector, rewrite candidates and background audit remain present. Numeric speed still comes from measured telemetry, updates at most twice per second during work, and retains Last on completion. The wave is activity feedback, not a throughput graph; inference is not delayed.
- **Interaction:** the synthetic native check completed with optional clarification choices. Nothing was approved, copied or inserted; the synthetic draft was cleared afterward. Reduced-motion frames remain identical while rate updates continue.
- **Validation scope:** Release host and UI test target build; focused rendered-motion regression; full current `roclint_product_alignment` target; structure check; installed startup without developer Qt PATH. This correction does not claim a new full-repository CTest run or macOS validation.

Exact commands, results and installed binary identity: `docs/audit/wave_motion_runtime_fix_2026-11-08.md`. Final result for the runtime motion correction: passed.

## Measured generation/decode traces — 2026-11-08

The user rejected passive motion and identified inflated token readings. This supersedes the ambient motion requirement and verdict above.

- **P1 repaired — inflated decode throughput:** asynchronous GPU completion had been charged to sampling instead of decode. Forward-pass timing now includes `llama_synchronize`. One real Vulkan fixture produced the same 42-token output and hash before/after; decode changed from 168.7 to 26.5 tok/s, while corrected generation measured 25.7 tok/s. This is timing attribution evidence, not a speedup claim.
- **P2 repaired — passive/unrelated animation:** idle, zero-token preparation and stalled streams remain still. Separate G/D traces update from real token/duration snapshots, with bounded interpolation and a 700 ms arrival tail. Both reuse the original pixel-wave art. No free-running phase or fabricated token count remains.
- **P2 repaired — misleading metric substitution:** missing generation is never filled with decode throughput; unavailable measurements show a dash. The same count of native model-output tokens underlies both rates. Settings/tooltip explain that JSON/model output is included, and model loading/prefill/full-operation latency are excluded.
- **Visual comparison:** the selected source motif is retained in both traces. Normal header height increases by 12 px to keep the two silhouettes readable; compact header sizing remains unchanged. `docs/research/roclint-ui-2026-11-08-measured-token-traces.png` and the companion idle PNG/GIF are actual Qt renderer fixtures with synthetic rates. Their values are not native benchmark results.
- **Validation:** pixel comparisons establish still idle/preparation/stalled states, actual changes on arrivals and static reduced-motion traces with numeric updates. Full current product-alignment, native-runtime and host-controller targets pass; installed startup uses synchronized new host/helper binaries. No new full-repository or macOS run is claimed.

Exact telemetry definitions, live measurements, test scope and deployment hashes: `docs/audit/measured_token_activity_2026-11-08.md`. Final result for measured traces: passed.

## Required fidelity surfaces

| Surface | Assessment |
|---|---|
| Typography | Embedded Inter Variable for reading, 26 px question hierarchy, 14–15 px prose, Consolas/Menlo for compact utility labels. Corrected the initial `Inter` family mismatch that fell back to monospaced text. |
| Spacing/layout | Rounded 18 px outer panel, 20 px normal gutters, bounded draft and diff, persistent footer; question padding/row gaps reduced so all three alternatives, custom answer and skip action fit the 820 × 600 fixture. Extremely small windows scroll content while preserving window controls and delivery actions. |
| Colors/tokens | Ink-blue background, cobalt primary/keyboard choice, warm-white prose, pale-blue perimeter and focus, muted periwinkle utility text. Semantic warning remains in the clarity label; focus uses a pale-blue boundary. |
| Images/icons | Actual generated transparent Roc and official Phosphor SVG controls with licenses/provenance. Separate G/D traces reuse the original layered pixel-wave PNG and respond only to measured token arrivals, per the latest correction. No text/emoji substitutes for logo or window icons. |
| Copy/content | User-facing actions include Clarify, Rewrite prompt, Skip & rewrite, Keep original, Show changes and explicit Approve & Copy/Insert. No question is selected automatically; identical output says it is the original prompt. Empty drafts disable inapplicable actions. |

## Comparison and repair history

1. **P2 — incorrect font fallback.** Early clarification capture used a monospaced fallback. Corrected the embedded family to `Inter Variable`; recaptured `clarification-options.png` shows sans question, option text and controls.
2. **P2 — diff cut off after focusing review.** At 820 × 520 and 560 × 400 the diff's first delegate received focus and scrolled only its first line into view. Reveal the bounded ListView, defer focus until layout, and recheck changed heights. Existing geometry and keyboard regression checks pass after the fix.
3. **P2 — lower clarification actions clipped at normal size.** The larger input/font initially pushed actions below the viewport. Reduced panel margins, row gaps and alternative padding; the final clarification capture shows the entire skip/rewrite row above the persistent footer.
4. **P2 — empty-state actions looked usable.** Installed native observation showed enabled rewrite/review with no draft. Bound availability to the current HUD draft and added a regression assertion before/after typing.
5. **P2 — ordinary launch behaved like a tool popup.** Manual HUD now uses a regular app window, and app activation shows the retained draft without requesting capture from another editor. Native computer-use can now target it.

## Interaction evidence and limitations

- Product-alignment checks cover explicit questions, skip without fabricated answers, new-draft rewrite request, pending constraint review, keyboard use, focused diffs, preserved drafts and no unapproved delivery.
- Four resized HUDs cover 420 × 200 through 820 × 520 with long labels. The full CTest suite passes; final presentation-only changes also receive a focused product-alignment rerun.
- Reduced-motion preference disables the new hover color transitions. Existing persisted approval/hash/model contracts remain unchanged.
- A native installed launch and close/reopen check supplement the QML fixtures. No new claim is made about native insertion support or macOS parity.
- Follow-up polish: full settings/onboarding redesign, a distributable signed installer, and high-contrast/light-mode native acceptance beyond current shared-theme regression coverage.

## Implementation checklist

- [x] Approved palette, real Roc asset, legible question/rewrite content and rounded controls.
- [x] Explicit rewrite path and optional questions; preview and approval retained.
- [x] Normal Windows executable/shortcuts and repeat-launch activation.
- [x] Native QML layout and workflow checks; installed startup without developer Qt PATH.
- [ ] Release installer, relocatable Julia runtime, first-run model setup, upgrade/uninstall and signing (separate release scope).

Final result: passed
