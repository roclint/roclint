# Pixel-wave runtime motion correction

Date: 2026-11-08. Baseline: `658dca36384a1ef485fb2c804dba076292ae815a`, branch `feat/roc-hud-refresh`.

## Failure and change

The user reported that animation still did not work after additional product work. The previous implementation started its animation timer only when Granite token counts increased. Julia checks and model warmup could therefore show no motion. Its moving highlight over a static PNG was also too subtle, and native subrequest counter resets could erase activity.

`GenerationMeter.qml` now transforms the original reference PNG with quantized height and horizontal motion. It moves gently while the HUD is visible, strengthens immediately throughout processing (including zero-token checks/preparation), holds working intensity for 1.8 seconds after completion, and transitions back to idle over 0.7 seconds. Working and idle cycles are approximately 1.1 and 3.2 seconds; the visible timer ticks every 50 ms. Reduced motion preserves a static reference image and numeric telemetry. Hidden/minimized windows stop the animation timer. Counter rollback clears measured presentation without cutting off work feedback.

The wave is cosmetic activity feedback. Rate values remain measured, update at most twice per second during work, and retain Last on completion. No inference delay or fabricated token count is introduced. Generation/decode display modes, their colors and Settings selector, rewrite candidates and background audit are preserved. Backend files are unchanged.

## Verification

- Release `roclint_host` and `roclint_product_alignment_tests` build passed.
- The focused regression compares actual Qt-rendered pixels across frames at idle, checks zero-token work and native counter rollback, exercises completion hold/fade and rate throttling, and requires identical reduced-motion frames.
- The focused CTest invocation passed in 5.10 seconds (5.14 seconds total). The complete current `roclint_product_alignment` CTest target passed in 50.18 seconds (50.20 seconds total). This is one UI test target, not a new full-repository CTest run.
- Structure check and `git diff --check` passed.
- Installed startup passed with developer Qt PATH/import overrides removed: exit 0, QML roots verified, no stderr.
- Installed and build host SHA-256: `E5931375AFC6F93BF90824A6B6B3464BF5B0B2DAF4011C804BF4882DFB5D7A72`.

Commands:

```powershell
cmake --build build --config Release --target roclint_host roclint_product_alignment_tests --parallel 4
# CTest supplies the configured Qt plugin/import environment.
$env:ROCLINT_GENERATION_METER_ONLY = '1'
ctest --test-dir build -C Release -R '^roclint_product_alignment$' --output-on-failure
Remove-Item Env:ROCLINT_GENERATION_METER_ONLY
ctest --test-dir build -C Release -R '^roclint_product_alignment$' --output-on-failure
python tools/check_structure.py
git diff --check
```

An initial build command named the CTest name rather than the actual `_tests` build target; the corrected command above passed. A direct executable launch without the configured QML import environment failed to load the root; the CTest invocation supplies that environment. These attempts are not counted as passing checks.

Local logs and captures are under `build/manual-testing/`: `wave-motion-fix-build-20261108.log`, `wave-motion-fix-test-20261108.log`, `wave-motion-fix-ui-20261108.log`, `wave-motion-fix-structure.json`, `wave-motion-fix-installed-smoke.json`, and `wave-motion-fix/`.

## Native observation and visual evidence

Computer Use targeted the ordinary installed window from `%LOCALAPPDATA%/Programs/Roclint/Roclint.exe`. Native screenshots showed the wave changing shape between frames at idle and during Preparing / Loading model / Checking before tokens arrived. A synthetic local Reader::open prompt completed with optional clarification choices; its displayed final generation/decode rates were 41.0 and 263.7 tok/s. These are observations, not performance gates. Completion occurred before the intended cancellation. No prompt was approved, copied, inserted or sent to another app. The synthetic draft was cleared afterward.

The original reference `docs/research/roclint-ui-2026-11-07-streaming-meter-reference.png` and current Qt header crop were inspected together. The implementation retains the cobalt/periwinkle stepped artwork and excludes reference-canvas padding from the native meter.

- `docs/research/roclint-ui-2026-11-08-visible-wave-motion.gif`: Qt-rendered header frames, enlarged with nearest-neighbor sampling. Its 123.4 tok/s label is synthetic fixture data, not a native benchmark or desktop recording.
- `docs/research/roclint-ui-2026-11-08-wave-working-no-tokens.png`: Qt-rendered zero-token preparation fixture.

A consistent SQLite backup was created locally before replacing the installed executable. History/database content is not published. The installed host, build host and build/bin host are synchronized; existing ordinary shortcuts remain unchanged.

## Scope and limitations

Only the meter QML, its existing UI regression test, and documentation/evidence changed. Newer product work is retained. `docs/audit/ast_rewrite_tuning_2026-11-06.md` remains untouched at SHA-256 `4688D0DD0BBA04487F203F765A7A29501FCA38A7CB839D91D98B24E54F1B636F`.

This verifies Windows motion and the current UI target. It does not establish macOS parity, native insertion compatibility, model quality, release-installer readiness, or whole-app UX acceptance. A later design iteration may replace the transformed reference artwork with richer independent pixel motion; this correction makes the current selected reference visibly animate in the actual application.
