# Combined companion/intent/review after-run — 2026-11-15

Status: Windows development evidence on the combined integration branch. One
cold process plus warm sibling scenarios. This is not a held-out quality score,
a percentile latency claim, or macOS/IDE-provider parity. Nothing was approved,
inserted or sent.

## Build and inputs

- Combined branch: `topic/integration-2026-11-15`.
- Pinned model: Granite 4.2 3B Q8_0, configuration
  `v13.context_quality`, temperature 1.0, top-p 0.95, seed 42, thinking off.
- Backend: Vulkan, NVIDIA RTX 4060 Laptop 8 GB, driver 617.14.
- Julia-1: `julia.cuda.bf16.v2.threads8`, isolated transformers 5.0.0 venv.
- Harness: `tools/companion_probe` linked against the combined Release testing
  libraries; `tools/companion_eval/run_companion_eval.py`.
- Raw evidence:
  - `docs/audit/companion_evidence_after_final_2026-11-15/` — final code for the
    music, cinematic, 500 ms and unknown/skip scenarios.
  - `docs/audit/companion_evidence_after_fix_2026-11-15/` — the successful
    changed-preference scenario (the final-run model generated a different
    question, so its fixed pick no longer matched; this is a fixture drift, not
    a host failure).
  - `docs/audit/companion_evidence_after_2026-11-15/` — first after attempt,
    retained because it exposed real integration defects listed below.
  - `timing.jsonl` in each directory — opt-in host/helper spans.

## Final scenario results

| Scenario | First usable question | Rewrite | Review | Result |
|---|---:|---:|---:|---|
| music: compact typography + 1990s/1960s fusion + easy browsing | 119.8 s (cold; Julia 44.3 s + Granite 70.1 s + explicit discovery) | 3.41 s | 5.53 s, ready, no findings | custom answer preserved; token overlap 0.952; "not fewer features or shortened labels" and one-tap browsing retained; `canApplyReviewedOutput=true`; one existing audit item still requires acknowledgment (`canApprove=false`) |
| cinematic product photo | 5.37 s (warm) | 2.52 s | 4.56 s, ready, no findings | concrete side lighting + shallow depth of field; no generic synonym substitution |
| 500 ms / 95% / unchanged schema | none | none | not requested | 0.48 s total; exact values kept; no Granite call; no false clarification |
| photo gallery: unknown, skip, then a custom answer | 3.84 s (warm) | 2.39 s | not requested | "I don't know" remains open; skip remains open; only the custom answer becomes a confirmed decision; proposal carries it |
| changed preference (successful sample in `after_fix`) | 6.83 s (warm) | 3.16 s | not requested | 80s choice superseded by "muted 1970s palette, not neon"; superseded provenance retained; final text uses only the new palette; token overlap 1.0 |

The music automatic route is still ORIGINAL (Julia 0.1366). The measured fix is
the explicit Explore path: it now forces a bounded explicit discovery and
produces a typography-focused question instead of the baseline dead end. The
automatic HUD still does not volunteer a question for this prompt; the UI exposes
Explore. This is an honest remaining product gap, not a claimed automatic win.

## Review behavior

The fresh-context review is functional on the production transport. In the first
after attempt it caught a generated meta-instruction as a blocking comprehension
finding (the cinematic candidate asked the user to specify lighting/composition
instead of producing a direction). After the interview answer and rewrite were
carried correctly, the final cinematic and music reviews returned ready with no
findings. Findings are source-linked; malformed or stale results never authorize
apply. The review card and host both require `canApplyReviewedOutput` **and**
`canApprove`, so a pending audit acknowledgment cannot be bypassed by the card's
Apply control.

## Latency distribution (this run, not a percentile claim)

- Cold readiness dominated the music scenario: Julia 44.3 s, Granite 70.1 s.
- Warm explicit first question: cinematic 5.37 s; gallery 3.84 s; changed
  preference 6.83 s.
- Warm rewrite: 2.39–3.41 s.
- Warm independent review: 4.56–5.53 s.
- The already-exact 500 ms prompt completed in 0.48 s with no model call.

The runtime trace remains opt-in and contains no prompt/source text. The
52 s cold SHA-256 verification and the 157-token prefix-alignment gap are
measured opportunities, not changed here.

## Integration defects found by the after-run and fixed

1. The interview custom-answer check created the native transport before
   `beginAttempt` assigned an attempt ID; the helper rejected it with
   "missing its authoritative context." The transport is now created after the
   attempt identity exists.
2. Interview follow-up and custom-check requests used the unsupported
   `granite_interview` helper operation. They now use `granite_analyze` with
   `surface=interview`, preserving the analysis deadline and trace label.
3. The probe recorded `not_run` for a queued review and leaked the real review
   into the next scenario. It now waits for the review to start and finish.
4. The meta-instruction guard only inspected the first sentence; it now scans
   every sentence for a generated imperative plus a meta marker, while allowing
   a sentence already present in the user's draft.
5. The review card's Apply path ignored pending audit acknowledgments; it now
   also requires `canApprove`. Stale review interpretation/findings from a
   different revision are hidden rather than displayed under the new prompt.

## Failures and limitations retained

- The automatic ORIGINAL route still gives no proactive typography question.
- The music interview option that mentions "Revolver-era song titles" makes a
  reference-specific creative proposal. It is labeled as a proposed assumption,
  not a source fact, but the final rewrite does not rely on it. Do not treat
  that option as verified artwork fact.
- The changed-preference live sample is from the second run because the final
  run's generative question differed and the fixed pick did not match. Host
  tests cover explicit supersession and invalidation; the live sample is labeled
  accordingly.
- No automatic repair follows a blocking review finding. The user returns to
  the interview or keeps the original, as permitted by the bounded-repair rule.
- Independent review is an explicit step after a substantial rewrite, not an
  automatic extra call on every edit. Apply is locked until it is completed.
- Native insertion/readback on a disposable Windows field was **not rerun** on
  this combined build. The shared tests use the scripted accessibility bridge
  and exact-readback policy; the previously confirmed native insertion belongs
  to the installed build, not this binary.
- macOS/Metal, Apple Clang, TCC/signing and IDE-provider parity remain **NOT
  RUN**. Mac `injectRange` still inherits `NotSupported`.
