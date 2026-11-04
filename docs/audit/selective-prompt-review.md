# Selective prompt review — October 26, 2026

## Scope

First implementation slice of the agreed selective prompt assistant. Existing local resilience work is retained. This is not a new six-class checkpoint, automatic visual mapping, a silent freeze rule, or a downstream execution feature.

The user confirmed live source typing now updates the HUD and starts checking after a pause. That is a user-observed baseline; the automated UI fixtures still simulate source AX observations.

## Changes

- Keep automatic checking conservative and debounced. Add an explicit request-ID `reviewPrompt` command that requests deeper discovery even after low S1 scores, without changing their evidence. Persistence, current versions, cancellation and strict parsing remain required.
- Seal review trigger and assistance kind in v5 analysis blobs; retain v2-v4 readers and verify discovery-attempt trigger provenance during writes and restore. Tables/schema version do not change.
- Discovery contract v7 distinguishes ready, clarification and optional creative/refinement/decomposition help. Its generation schema couples status/kind and restricts optional help to one material card with two or three alternatives, without assumptions, conflicts or missing decisions. Existing strict envelopes, response bounds and clarification ceilings remain. Rejected suggestions report an unavailable fallback, never usable optional help.
- Permit an explicit wording request for a quiet v5 analysis without a fake choice. Such a request records its rewrite attempt before inference. Ready outcomes never start a rewrite automatically.
- Prompt-specific generation schemas restrict evidence strings to at most 32 unique literal excerpts of the frozen prompt: each at most 256 bytes, at most 3 KiB of serialized quote values, and a complete schema under the existing 64 KiB transport limit. Excerpts include short full prompts and samples of intact UTF-8 tokens/phrases across the prompt. The same candidates reach the instruction builder. No quote is inferred or attached after parsing; the parser still independently requires a unique exact match. Quote provenance is not proof of semantic relevance. Long prompts may lack a relevant excerpt in this bounded selection.
- Hide routine verification badges and internal scores/statuses by default. Show technical details on demand and include the exact answer beside a required semantic-review item. Preserve the locally computed diff and original prompt.
- Validator v3 narrowly rejects the observed semantic-limit broadening, failure to retain common explicit API clauses, and unsupported timing/size/percentage quantities. It runs during response parsing and approval validation. It does not establish general semantic fidelity; conservative phrase/unit checks can reject equivalent paraphrases/conversions.
- Discarded discovery callbacks finish their attempt records instead of leaving them in progress. An unmatched recommendation cannot fall back to the first alternative; delegated choices require one exact current match.

## Regression evidence

Deterministic fixtures cover six everyday prompts (known button color, open creativity, symptom-only bug, search behavior, API invariant, multiple features), low-score explicit discovery, preserved score evidence, optional keep-original behavior, explicit wording help without decisions, history restore/trigger tampering, malformed assistance kinds, stale completion, and the narrow rewrite regressions. Scripted replies establish mechanics, not model quality.

## Validation

- Refreshed build succeeded. All 43 registered CTests passed (21.85 seconds in the final run with four jobs). The 49 Python unit tests passed. Structure and diff checks passed.
- Native Cocoa review/HUD checks passed: presentation continuity, typing debounce, next prompt after delivery, and technical badges hidden by default. Review screens were rendered and inspected. The authoring canvas is hidden during a rewrite preview to leave space for the diff; Edit draft restores it without discarding the draft text.
- Live probes use isolated temporary settings, an in-memory database and synthetic prompts. They perform no AX insertion, approval or Send action. The original low-score development phrase reached optional help through explicit review without changing its S1 score (0.352354); its final prompt-bound-schema check took 44,649 ms cold including startup and showed three actionable directions.
- Early live probes exposed inconsistent optional-help contents and then ungrounded quotations. A development example alone did not fix the different phrase. This led to coupled schema branches, one-card optional help, accurate unavailable fallback, and prompt-bound quotation generation rather than weaker parsing or automatic retry.
- With the prompt-bound schema, “Give the landing page a friendlier vibe.” produced a usable optional card, with S1 0.993757 and a 44,758 ms cold end-to-end observation including startup. The question was “How expressive should the friendlier vibe be?” with bold/playful versus minimal/subtle directions.

These are development observations and one additional creative phrase, not an accuracy benchmark or a held-out evaluation suite. Cold startup dominates these observations; no p50/p95 or warm-latency claim is supported. S1 weights, its four heads and thresholds remain unchanged. Broad semantic fidelity, long-prompt quote coverage, complex visual grounding and behavior on Linux/Windows remain unverified. The user confirmed the source-sync baseline; an updated real-editor end-to-end manual retest remains separate from native fixtures and installed-model probes.

All work remains local alongside the existing resilience changes; no push was performed in this implementation slice.


Reproduction commands and local evidence:

- Build: `cmake --build build -j 6`.
- Registered gate: `ctest --test-dir build --output-on-failure -j 4` (`/private/tmp/roclint-selective-final-ctest.log`).
- Python: `python3 -m unittest discover -s tools -p 'test_*.py'` (`/private/tmp/roclint-selective-python.log`).
- Structure: `python3 tools/check_structure.py`; `git diff --check`.
- Native UI: `ROCLINT_NATIVE_UI_TEST=1 ROCLINT_TEST_ARTIFACT_DIR=/private/tmp/roclint-selective-ui QT_QPA_PLATFORM=cocoa build/roclint_product_alignment_tests`. Existing Qt control-style, focus and ScrollView binding warnings remain; no failed assertions. These warnings are not counted as environmental test failures.
- Isolated live checks use `ROCLINT_LIVE_EXPLICIT_REVIEW=1 build/roclint_native_granite_tests --live-host-pipeline` with the installed model directories and synthetic prompt files. Final logs: `/private/tmp/roclint-selective-development-final.log` and `/private/tmp/roclint-selective-heldout-fixed.log`. Earlier diagnostic failures remain separate; they are not counted as passing quality checks.
