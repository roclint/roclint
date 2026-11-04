# Copy-ready prompt for the GPT-6.1 sol agent

Investigate Roclint's remaining intent-preservation failures using the research findings below. Read the companion report first:

`/Users/jesse/roclint/docs/research/2026-10-28-cross-industry-intent-preservation-report.md`

Produce a concrete, source-backed research report and an implementation-ready experimental/test plan. Begin with read-only inspection of the newest evidence and active code. Do not restart the earlier experiments, assume the pasted baseline is still newest, or make production changes as part of this task.

## Goal

Roclint should help users express their intended task through worthwhile questions, concrete choices, and complete faithful final requests. It must preserve explicit requirements, support intentional corrections, keep user control, and reduce downstream surprises with low interaction cost.

Determine which established mechanisms from other products and industries are useful for this goal, which Roclint already implements, and which experiments would isolate the remaining bottleneck. Translate analogies into specific tests with observable expected behavior; avoid a generic literature summary.

## Starting context — verify before using

Experiment worktree:

`/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint`

Latest report known to the sending chat:

`/Users/jesse/roclint_notes/2026-10-28-julia-integration-trusted-choice.md`

Independent-review packet:

`/Users/jesse/roclint_notes/julia-integration-trusted-choice-2026-10-28/review/blind-reference-review.json`

Record intended behavior before consulting candidate outputs, identity keys, or author labels if participating as an independent reviewer. The sending chat has already seen author judgments and cannot supply an independent blind review.

The October 28 report records 96 paired cases: staged control 22 faithful results; trusted-choice hybrid 30. All eight gains are deterministic captured-answer views. Both arms have 22 faithful outcomes across 86 freeform cases and two unsafe complete proposals. The hybrid offered 21 correction questions: 15 unnecessary, one useful, four uncertain, one poorly scoped. These are provisional author judgments, not representative accuracy or all initial discovery results.

Reported failures include losing the named Save profile task through “Put it...” and omitting “add no dependencies” from a compound color correction. Some proposals omit “only” or “do not change code” and remain semantically uncertain.

The actual freeform path uses staged source-unit selection and editing. It does not use the newer question-bound classifier/lexical ledger. The final `contains_freeform` guard has mechanical tests and replay; newly tagged combined-reply live-model quality remains unmeasured. Synthetic producer events do not establish real Mac UI ownership.

The downstream Julia assumption-description polarity mismatch is real. Its isolated repair changed no initial route in 192 comparisons; do not describe it as a routing or accuracy fix. Reported capitalization and punctuation sensitivity remains.

Reported regression verification is 43 CTests and 261 Python tests. These support tested mechanics, not semantic completeness, user benefit, or full integration.

## Inspect the active implementation

Start with `tools/trusted_choice_hybrid.py`, `tools/trusted_choice_experiment.py`, `tools/edit_ablation_experiment.py`, `tools/minimal_edit_prototype.py`, `tools/question_bound_prototype.py`, their contracts, fixtures, and tests. Trace the actual dispatch path and exact visible input before attributing behavior to a model. Check for newer reports/code and explain any changes to this baseline.

## Research connections to investigate

1. **Rasa/conversational repair:** typed dialogue state, correction of prior values, and scoped confirmation. Source: https://github.com/RasaHQ/rasa-calm-demo/blob/main/data/flows/patterns.yml
2. **NASA requirements engineering:** source-to-requirement-to-change-to-verification traceability and change-impact analysis. Sources: https://www.nasa.gov/reference/6-2-requirements-management/ and https://sma.nasa.gov/news/articles/newsitem/2026/03/25/identifying-objective-evidence-improves-requirement-implementation
3. **Dafny/formal verification:** frame conditions defining allowed modifications and preservation obligations. Source: https://dafny.org/latest/DafnyRef/DafnyRef
4. **Kubernetes configuration management:** field ownership, explicit conflict detection, and controlled supersession. Source: https://kubernetes.io/docs/reference/using-api/server-side-apply/
5. **Aviation/healthcare:** readback, receiver interpretation, and verification of meaning. Sources: https://www.faa.gov/air_traffic/publications/atpubs/atc_html/chap2_section_4.html and https://www.aha.org/center/project-firstline/teamstepps-video-toolkit/closed-loop-communication

Use the companion report's 2026 AI papers to connect these mechanisms to model behavior. Verify substantive claims with primary sources. Distinguish empirical findings, engineering practice, and hypotheses for Roclint. Preserve study/domain limitations; do not transfer benchmark gains to this product.

## Required output

Write a dated Markdown report that contains:

- An evidence-backed explanation of what works, what fails, and what remains untested.
- A mapping from each relevant industry mechanism to an existing Roclint mechanism or a genuine gap. Do not recommend rebuilding existing guards.
- A proposed requirement representation with source/event/question identity, current state, supersession, edit scope, and unresolved information. Treat it as a candidate design, not established truth; retain raw text and distinguish model inference from user confirmation.
- An experimental matrix isolating target discovery, requirement extraction/completeness, edit generation, context construction, and validation. Include current inputs, supplied-correct targets, supplied-complete requirements, and both. Label oracle conditions as diagnostic.
- Concrete test families covering compound edits, referents, negation/exclusivity, numerical constraints, deliberate goal changes, repeated targets, mixed options/comments, contradictions, retractions, stale events, surface variants, and multi-step preservation.
- An evaluation plan with faithful completion, unsafe offers, omissions, wrong scope, invented requirements, useful/unnecessary questions, false rejection, deferral, timeouts, tokens, latency, and downstream task outcomes. Keep initial discovery separate from correction questions.
- A ranked next experiment, expected learning, smallest reviewable implementation scope, and conditions that would cause us to reject the candidate.

Use independently reviewed intentions and permit multiple faithful expressions. Separate exposed regressions from fresh family holdouts. Prevent reference/label leakage. Keep failures and infrastructure problems visible with separate classifications. Measure useful coverage alongside error; rejection alone is not an improvement.

Do not assume lexical matching proves semantic completeness, a source span establishes correct scope, user confirmation proves understanding, or a structured requirement list is complete. A verifier must be evaluated for false acceptance, false rejection, and cost. A readback preview needs user-effort and error-detection evidence.

## Scope

Preserve all existing uncommitted work and frozen evidence. Follow applicable repository instructions. Make the research and test plan concrete without requiring a production rollout. Save the report in an appropriate existing research location and return its absolute path, the principal findings, and the recommended first experiment.

No live-model run, model download/training, production integration, runtime-policy change, installation, restart, commit, merge, push, or message to another chat is requested by this prompt. If a proposed follow-up needs one of those actions, specify it as a separately scoped next task with its costs and evidence requirements.
