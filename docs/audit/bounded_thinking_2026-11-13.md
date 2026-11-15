# Bounded native Granite thinking — November 13, 2026

## Outcome and scope

The native Granite path can now spend a separate, bounded token budget thinking
before producing its schema-constrained JSON answer. Settings exposes **Think
before answering → More thinking**. The setting is experimental, persisted and
**off by default**: the real-model comparison below demonstrated extra
computation, but did not establish a consistent improvement in useful edits.

Working baseline: branch `feat/roc-hud-refresh`, HEAD
`c39d82140f0a2ab15337f174b3a134b7d41024fe`. These changes are uncommitted and
include integration with existing uncommitted word-companion work. No commit,
push, model download or native macOS validation was performed.

## Runtime contract

The pinned GGUF's actual Jinja template supports `enable_thinking` and opens
`<think>` when enabled. The engine verifies the template suffix and that
`</think>` is exactly one tokenizer token. Private reasoning is sampled without
the final-answer grammar. Reasoning tokens are counted, but their bytes are
never detokenized, streamed, returned or persisted by Roclint.

At the model's closing delimiter, or at the reasoning cap, the engine switches
to a fresh sampler with the existing compact JSON grammar and pinned sampling
parameters. A forced closing delimiter, when needed, is accounted separately
as a control token. Only final-answer JSON reaches parsers and UI text.
Cancellation, byte limits, exact input limits, operation deadlines, strict
request binding and KV cleanup remain enforced. An incomplete final object
fails closed; reasoning is never substituted for an answer.

Manifest configuration IDs advance to `v9.thinking`. Context remains 8,192
tokens, input remains at most 6,144, and each total output reservation remains
at most 2,048, including the possible forced delimiter.

| Profile | Thinking cap | Answer cap | Total reservation when enabled |
|---|---:|---:|---:|
| Word refinement | 384 | 384 | 769 |
| Analysis | 384 | 1,024 | 1,409 |
| Rewrite | 512 | 1,535 | 2,048 |
| Goldfish audit | 128 | 512 | 641 |

Audit selection requires the trusted instruction-contract prefix, rather than
a contract name occurring inside prompt data. Off mode skips thinking and keeps
the profile's answer cap. The development-only `--no-thinking-reference` helper
mode exists for comparisons; a normal host rejects helpers advertising that
override. The supported setting instead binds `thinking_enabled` in both the
request envelope and payload. Switching it invalidates pending downstream work
and proposals; delayed responses cannot acquire the new mode's binding.

Analysis/word operations have 45-second native limits; rewrites have 60 seconds.
The native analysis group allows 90 seconds only when thinking is enabled.
The existing 20-second group budget remains for off mode, overrides and
non-native transport. More thinking does not disable deadlines.

## HUD accounting

`output_tokens = reasoning_tokens + answer_tokens`, counting sampled non-EOG
tokens. Forced control delimiters are excluded from that generated count.
Progress and final telemetry validate integer, finite, nonnegative, monotonic
partitions and phase transitions. Legacy telemetry without partitions is
treated as answer-only. The existing shared measured waveform follows total
generation/decode progress, with a Thinking phase label and separate thinking
and answer counts. Compact layouts retain the split in tooltip/accessibility
text. No synthetic speed or idle animation was added.

## Actual-model evidence

Evidence directory: `D:\codex-evidence\roclint-thinking-20261113`.
Windows Vulkan, NVIDIA RTX 4060 Laptop GPU, pinned Q8_0 weights and llama.cpp
`da2ee1e7a640445fc562fe5529598cee1917e0f5`. Production operation deadlines were
used; no diagnostic deadline bypass was enabled.

Six frozen word cases use faster, warmer, milliseconds, atmospheric, compact
and cinematic in full prompts. `thinking-on.json` and `thinking-off.json` use
the same instructions, answer reservations and pinned sampling configuration.

| Measure | Thinking on | Thinking off reference |
|---|---:|---:|
| Completed JSON responses | 6/6 | 6/6 |
| Thinking tokens per case | 384 | 0 |
| Wall latency range | 10.744–12.666 s | 0.649–2.365 s |
| Median wall latency | 12.232 s | 0.811 s |

All six enabled cases reached the reasoning budget. Several outputs were still
bare synonyms, the precise milliseconds case received an unnecessary edit, and
atmospheric produced no candidate. Cinematic became a partially concrete
lighting/color-grading suggestion, but remained awkward in its full sentence.
This is **not** a semantic-quality pass, and is not evidence that increasing
the budget further would fix word refinement. The earlier 128-token experiment
is preserved as `thinking-on-128.json`; it also included placeholder output.
The production word parser now rejects blank/ellipsis-only candidate text
instead of presenting a placeholder as a useful suggestion.

`native-integration.json` records passing actual-helper checks:

- Cancellation during thinking completed in 79.10 ms after the cancel request;
  private chunks were empty and no partial answer was returned.
- A following supported off-mode request succeeded with zero thinking tokens,
  demonstrating recovery after cancellation.
- Analysis and rewrite fixtures naturally closed thinking at 98 tokens each.
- An audit fixture reached its 128-token cap and transitioned to valid JSON.
- Split totals remained consistent, final text contained no thinking tags, and
  reasoning/answer/control reservations stayed within the frozen output cap.

Those integration fixtures establish transport and phase behavior, not the
quality of natural-language analysis or rewrite decisions.

## Validation and deployment

The Release worker and host compile. All test targets compile. The first full
CTest run found a new fixture bug: its partial chunk stayed empty after the
thinking phase. The fixture now explicitly restores answer chunks, and the
native-runtime suite passes, including rejection of mismatched thinking
bindings, split telemetry checks and suppression of private text. The next full
run passed the other 64 enabled suites, but exposed an older asynchronous test
assertion: inference completed before the GUI applied its final telemetry.
That assertion now waits for the final GUI snapshot; production behavior was
unchanged. The rebuilt native suite then passed three consecutive runs
(19.34 / 19.25 / 19.55 seconds). Thus all 65 enabled suites have passing evidence
across the full run and corrective retest; a single all-green full run after
that test-only synchronization fix was not performed.

The focused generation-meter regression also passes. Actual QML screenshots
`screenshots/measured-wave-thinking.png` and
`screenshots/measured-wave-answer.png` were inspected: the phase label switches
and the split remains Thinking 64 / Answer 0, then Thinking 64 / Answer 16.
Those are synthetic telemetry fixtures, not screenshots of actual inference.

Python Julia tests: 39/39. Structure-checker regression tests: 11/11. Structure
continuity: 94/94 original paths, no errors. `git diff --check` passes.
The real embedding integration remains disabled because embedding assets are
not installed; this task did not claim retrieval real-model acceptance.

At the initial November 13 handoff, the running user-owned Roclint executable prevented replacement in
`build/Release/bin`. The application was not killed, preserving any current
draft. Deployment requires the user to quit it; a successful compile is not a
claim that the installed/running app has been updated. Native macOS
build/CTest/HUD/Metal checks remain NOT RUN.

The November 14 [deployment follow-up](bounded_thinking_deployment_2026-11-14.md)
records successful staging, installation, matching executable hashes and an
installed startup check after Roclint exited. It supersedes the deployment
blocker above; manual settings-toggle verification remains incomplete because
computer-use lost its native window binding.
