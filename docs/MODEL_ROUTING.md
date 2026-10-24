# Model routing required by the user

The user's diagram, supplied October 23, 2026, takes precedence over the earlier discovery-first sequence in the reference bundle. Julia is the selected S1 model.

1. The HUD supplies the user's draft. The C++ core freezes its canonical revision and owns all state/binding invariants.
2. **Julia/S1 classifies the raw prompt first**, producing a real model ambiguity score and an advisory category: Action, Target, Rule or Rule Conflict.
3. A score **below 0.70** takes the original-prompt route. Preserve the original wording; do not call Granite discovery or rewrite.
4. A score **at least 0.70** sends the frozen prompt and S1 classification to local IBM Granite 3B. Granite supplies two or three concrete alternatives for each bounded independent decision.
5. The HUD requires an explicit user choice. A score or model response must never select or confirm that choice.
6. **Only Granite generates the rewrite**, using the exact confirmed choices. The existing diff, constraints and wording-review step remain before delivery.
7. The selected original or reviewed rewrite becomes the exact request delivered to the user's coding-agent workflow. This requirement does not authorize this assistant to send messages or execute generated tasks now.

The diagram's core <2 ms, classifier <15 ms and user-choice <2 s labels are targets, not verified measurements. A missing/unavailable classifier fails visibly and cannot be treated as a clear score or replaced with keywords, invented probabilities or a mock in production.

## Implementation and evidence boundaries

The live host now starts with local Julia prompt classification. Four independent Boolean model heads check Action, Target, Rule and Rule Conflict in one batch. The core uses the greatest head score as the gate and its category as an advisory hint. These independent estimates are **not a calibrated probability that any ambiguity exists**, nor a distribution over Granite's alternatives. The exact comparison is `< 0.70` versus `>= 0.70`; no rounding enters routing.

The version 4 final analysis embeds the sealed classification and records alternative ranking as `NOT_REQUESTED`. Clear results contain no discovery attempt or fabricated options. Ambiguous results require actual successful Granite discovery. The successful `julia_triage` attempt retains the complete sealed classification in its model detail even if subsequent Granite generation fails; restore checks that this exact evidence matches the final artifact. Older v2/v3 hashes and readers remain intact. Historical discovery/scoring regression tests opt into an explicit test-only legacy seam; new route tests use the production default with a scripted out-of-process model.

The existing 20-second active analysis budget spans S1 and Granite; local model warm-up is separately bounded and pauses that budget without resetting the time already spent. S1 itself has an eight-second operation limit. A failed, stale, cancelled or malformed classifier result never establishes a clear route. The original draft remains available for explicit user delivery. Scores never confirm choices, and clear routes cannot request a rewrite. Explicit alternative selection, custom clarification and accept/reject of a visible material assumption are recorded as user decisions; an automatically quiet analysis is not a choice. Ambient capture asks the user to open/freeze the draft before any model call.

**Model quality remains an open gate.** Actual pinned Julia CPU probes are saved under `build/julia-triage-category-heads.json`: four-head latency was 304–416 ms on the twelve development examples, with clear-prompt false positives and incorrect categories. These examples are development evidence, not a held-out accuracy evaluation; the worker template is not release-calibrated. Correct routing mechanics must not be reported as classifier accuracy or the diagram's 15 ms target. Earlier Granite real-model quality failures remain recorded in [the native runtime evidence](NATIVE_GRANITE.md).

Granite grammar sampling, resource policy and runtime integrity work remain applicable. Temporary uncapped investigations use the explicit diagnostic helper/probe option; normal operations remain bounded. The original source layout and HUD/dashboard remain the physical/UI baseline. Native macOS validation remains unproven because the user reported no Mac or runner is available; Windows implementation continues.


### Earlier v1 Qt/helper verification

`build/s1-live-qt-julia.json` records an opt-in run of the pinned checkpoint through the actual Qt supervisor and packaged worker (worker SHA-256 `e6162e62f37dfc228634309919cbc3f8aa863dfabc25c798416d9c15cbd2f651`). Loading and warming took 52.369 seconds, separately from the active analysis budget. Three classified development prompts took 361–418 ms: a bounded rename scored 0.0811 and routed ORIGINAL, ambiguous speed scored 0.9523 and routed CLARIFY with the incorrect Rule Conflict category, and a simple arithmetic request falsely routed CLARIFY at 0.9672. The run also verified the retained legacy scoring operation, cancellation without replay, explicit retry and owned shutdown. Its `verified` field means protocol/execution integrity, **not** semantic quality acceptance.


### Integrated supplied calibration work (v2)

The supplied revised head wording is retained, under `julia.prompt_triage.categories.v2`; v1 history remains readable with its original hashes. GPU/CPU auto-selection now reports and binds the actual BF16/FP32 profile and thread count. Unsupported BF16 hardware uses CPU; an automatic GPU warm-up failure can fall back to CPU. No logits, per-case predictions, or benchmark fixtures are altered.

The supplied `state[:3000]` shortcut was removed: the full frozen prompt is audited and classified or the operation fails visibly. The actual 3,021-token stress case passes all four full-input encoding audits. Q8_0 is the user-selected Granite default across the host, manifest, setup and probe tools; release acceptance is a separate question.

Fresh probability, timing, parity and challenge results, exact before/after wording, and limitations are recorded in [the calibration verification document](TEAM_CALIBRATION_REPORT.md). The integrated work is **not an all-criteria victory**: extra challenge cases, non-target head separation, CPU latency and maximum device probability deltas still fail the stated gates.
