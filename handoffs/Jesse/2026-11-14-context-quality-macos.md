# macOS handoff — context quality, word companion and runtime updates

Delivery branch: `feat/roc-hud-refresh` in `https://github.com/roclint/roclint.git`.
Previous shared HEAD: `c39d82140f0a2ab15337f174b3a134b7d41024fe`.
This delivery includes the accumulated source, tests and reports since that HEAD.
Historical reports saying "uncommitted" or "not pushed" describe their original
verification sessions; this handoff records the subsequent source delivery.

## Get the source

Preserve any local Mac changes before switching branches. Do not reset a dirty or
diverged checkout to match this delivery.

```sh
git fetch origin
git switch feat/roc-hud-refresh
git pull --ff-only origin feat/roc-hud-refresh
git rev-parse HEAD
git status --short
```

If the branch does not exist locally, use
`git switch --track origin/feat/roc-hud-refresh` instead of `git switch` above.
Record the resulting HEAD in the Mac validation report. No Windows build directory,
model weights, local databases, credentials or installed application is included.

## What this snapshot contains

- Entity/constraint separation: ordinary `Make` phrasing, original source clauses,
  preservation and negative constraints, and protection against grounding generic
  property prose as a similarly named code entity.
- Compact discovery v11: literal source quotations, generated alternatives,
  consequences and separately disclosed proposed assumptions. Only selected
  proposal metadata reaches the rewrite. Contract recovery and Goldfish semantic
  regeneration are bounded and share the original deadline.
- Word refinement v2 and complete-draft audit v2: host-bound selected spans,
  evidence-bound keep decisions, separate grammar/preservation/usefulness verdicts,
  conservative unsafe-splice rejection, stale-result cancellation and coalescing.
- Experimental bounded native thinking, off by default; profile-specific answer
  reservations, cancellation and separate reasoning/answer telemetry. Configuration
  suffix is `v13.context_quality`; the pinned Granite Q8_0 weights/runtime are unchanged.
- Word-companion QML, review/clarification flow changes and waveform telemetry work.
- AST scan diagnostics/cache recovery and the separately built retrieval subsystem,
  its lifecycle repairs, worker/setup sources and maintained regression fixtures.

Useful entry points:

- [Current implementation](../../docs/CURRENT_IMPLEMENTATION.md)
- [Latest quality contracts and results](../../docs/audit/context_quality_remediation_2026-11-14.md)
- [Word companion review](../../docs/audit/word_companion_review_remediation_2026-11-13.md)
- [Bounded thinking](../../docs/audit/bounded_thinking_2026-11-13.md)
- [AST cache recovery](../../docs/audit/codebase_index_recovery_2026-11-14.md)
- [Retrieval lifecycle repairs and boundaries](../../docs/audit/retrieval_refresh_recovery_2026-11-13.md)

## Design direction is preserved; it is not complete

Keep the rounded ink-blue surfaces, cobalt actions, pixel Roc bird, readable body
type and monospace accents. The selected interaction direction is the **word-level
companion**: a small contextual popup beside the user's editor, escalating to
whole-prompt review only when useful. The user must retain a direct full-rewrite
action with skippable questions.

- [Approved direction and image archive](../../docs/research/2026-11-05-roclint-ui-ux-direction.md)
- [Archived inline interaction concepts](../../docs/research/2026-11-14-inline-companion-concepts.html)

The HTML is an archived demonstration, with illustrative fixed text and choices;
it is not model output or an implemented editor integration. The selected word-level
variant is visible when opened directly. Other variants are included as hidden
sections originally used by the concept carousel.

The actual detector currently examines a word near the caret. It does not provide
universal per-word underlines throughout arbitrary editors. Consistent contextual
choices, typography/spacing, motion, undo and provider coverage remain unfinished.

## Verified on Windows

Final Release build succeeded. Complete CTest: **65/65 enabled passed, 297.39 s**;
the real-embedding integration gate remained disabled. Python: **86 tests, one
skip, 50.334 s**. Structure: **94/94 original paths, zero errors**.
Whitespace check and staged offscreen startup with an in-memory DB passed.

Seven synthetic native Q8/Vulkan checks used production builders/parsers. Focused
word questions took roughly 2.7–3.4 seconds excluding cold load, debounce and Julia.
The complete-sentence challenge rejected an invalid insertion and retained a valid
alternative. Creative quality remains mixed: the music-app typography options
drifted despite passing the semantic audit. These are not held-out quality scores
or complete HUD/Julia measurements.

An earlier standalone watcher atomic-save test failed intermittently. The final
full run and five isolated repeats passed, but the original cause is unconfirmed;
only diagnostic logging was added to that test. Do not describe it as a proven fix.

## Mac validation still required

No current-snapshot macOS build, Apple Clang check, CTest, native AX insertion,
watcher/HUD session or Metal benchmark was run from this Windows machine.

For an Apple Silicon development checkout with CMake, Ninja, Qt 6 (including Test)
and the existing Python/model prerequisites installed, use a separate build folder:

```sh
cmake -S . -B build-macos-quality -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_DESKTOP=ON -DBUILD_TESTING=ON \
  -DROCLINT_BUILD_NATIVE_GRANITE=ON \
  -DROCLINT_GRANITE_BACKEND=METAL
cmake --build build-macos-quality --parallel 4
ctest --test-dir build-macos-quality --output-on-failure
ROCLINT_CONTRACTS_TOOL="$PWD/build-macos-quality/roclint_granite_contracts" \
  python3 -m unittest discover -s tools -p 'test_*.py'
python3 tools/check_structure.py
QT_QPA_PLATFORM=offscreen build-macos-quality/roclint_host --dry-run --db :memory:
```

If Qt is not autodetected, set `CMAKE_PREFIX_PATH` to the actual Mac Qt installation
when configuring. Verify that configuration reports the desktop host and native
Metal helper enabled; a core-only build does not establish desktop parity. Windows
test counts need not equal Mac counts; report the actual enabled, skipped and
disabled gates. Inspect generated `granite-runtime-config.json` for METAL and the
current configuration suffix. Never copy Windows runtime configuration onto Mac.

Priority native checks:

1. The Mac bridge currently inherits the default `injectRange` implementation,
   which returns `NotSupported`; only Windows has the new native range-write path.
   The shared QML/policy tests do not establish Mac word-insertion support.
   Implement and validate native AX range writes before advertising that parity,
   keeping exact readback, UTF-16 boundaries and stale-field refusal.
2. Test whole-field capture/rewrite/approve, permission denial, focus changes,
   partial writes, Unicode, cancellation and no automatic Send/Enter. Audit the
   historical Mac full-field readback concerns against current native behavior.
3. Test the non-activating companion, mixed-DPI/multi-display placement, keyboard
   access, review flow, waveform phases and off/on thinking persistence.
4. Exercise AST complete/incomplete scans, save/rename/delete events, workspace
   switches and stale approval invalidation on macOS filesystems.
5. Run Metal quality/timing comparisons with thinking off and on using the same
   fixtures; retain raw responses and separate cold load from warm request time.

Retrieval is a separate subsystem, not completed host RAG. Real embedding assets,
asset trust, generation-tokenizer budgeting, corpus ingestion and approval-bound
host integration remain separate work. Do not present mock fixtures as a real
embedding run. Revalidate bundle deployment, helper paths, signing/TCC identity and
Finder launch independently; this push does not certify an updated macOS `.app`.
