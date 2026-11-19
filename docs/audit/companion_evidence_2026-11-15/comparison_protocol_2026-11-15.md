# Fair conversational-skill comparison protocol — 2026-11-15

Status: **NOT RUN**. No competitor model, skill, cloud provider or API credential was
available or used. Nothing in this file is a competitor result. It is the
reproducible protocol that a later evaluator must execute before any comparison
claim is made.

## Arms

| Arm | What it is | Model/context | Status |
|---|---|---|---|
| A. Roclint baseline | Unmodified `d5c8101` production HostController, Julia S1 routing, Granite 4.2 3B Q8_0, v13.context_quality, thinking off | Pinned local weights; host-supplied source/workspace evidence | Measured in `baseline_host.json` |
| B. Conversational skill | A well-written static system prompt/skill conducting the same clarification conversation in the user's editor or a chat surface | Must be recorded exactly (provider, model, version, system prompt, context) | NOT RUN |
| C. Same-model static-prompt control (optional) | The same pinned Granite worker with a hand-written conversational system prompt and no Roclint routing/interview/review | Same weights; isolates orchestration/UI from model capacity | NOT RUN |

Arm C is optional but valuable: it separates "Roclint's interaction helps" from
"the 3B model is the limit". It must not be presented as a competitor.

## Tasks and context

Use exactly the 12 tasks in
`tests/fixtures/companion_eval/scenarios_2026-11-15.json`, marked
development/unseen. Supply the same source prompt verbatim. For the code task,
supply the same synthetic `code_project` files. Do not add hints that Roclint
received (or withhold hints it received) unless the condition is recorded as a
deliberate treatment.

## Turn protocol

For each task, in each arm:

1. Submit the prompt once.
2. Record the first response verbatim, including any question, options,
   assumptions and the exact wording.
3. Give the same planned answer(s) as the Roclint run when the arm asks an
   equivalent question. If the arm asks a different question, answer it
   truthfully in the user's voice and record the divergence; do not feed
   Roclint's option labels.
4. Record whether the arm asks again, rewrites, or stops.
5. Ask for the rewrite only when the arm offers it; otherwise record "no direct
   rewrite path" instead of inventing one.
6. Do not approve, insert, send or copy into a real document. Use synthetic
   text only.

## Measures (per task)

- Time from submitting the prompt to the first visible response.
- Time to the first *usable* question or suggestion (not a spinner/status).
- Time to a complete rewrite/preview.
- Number of user turns and repeated explanations.
- Consequential choices uncovered (editorial).
- Unwanted/unsupported assumptions (editorial).
- Whether the supplied answer appears in the output (mechanical overlap plus
  editorial read).
- Preservation of explicit values/constraints (500 ms, 95%, schema, function
  signature, browsing).
- Whether a skip or "I don't know" remains open instead of becoming a default.
- Whether the arm invented reference facts (music-app condition).
- User preference between outputs, if a real user is available. Model self-
  judgment is not a substitute.

Keep mechanical, model and editorial judgments in separate columns. A valid
schema, a model "Pass" or a polished sentence does not establish usefulness.

## Confounds to record

- Different model/provider/version or quantization.
- Different context (files, AST, prior decisions).
- Different sampling (temperature/top-p/seed/thinking).
- Different host latency (local GPU vs cloud queue).
- Different turn budget or output-token limit.
- Evaluator-authored answers or post-editing.
- Retry policy and failure handling.

## Required output shape

Use the same `roclint.companion_eval.baseline.v1` row shape as
`baseline_host.json`, adding `arm`, `provider`, `model`, `system_prompt_sha256`,
`context_supplied`, `turns`, and `editorial`. Store raw transcripts under
`docs/audit/companion_evidence_2026-11-15/comparison/<arm>/`. Mark the run
`NOT RUN` until every task has a real transcript.
