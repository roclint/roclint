# Model routing required by the user

The user's diagram, supplied October 23, 2026, takes precedence over the earlier discovery-first sequence in the reference bundle. Julia is the selected S1 model.

1. The HUD supplies the draft. The C++ core freezes its canonical revision and owns state and binding invariants.
2. Julia/S1 classifies the raw prompt using its existing four independent Action, Target, Rule and Rule Conflict heads. These are unchanged classifier signals, not six calibrated intervention classes.
3. **Automatic checks** use the existing threshold/dead band to decide whether to retain the original or request Granite. **Review prompt** is an explicit deeper-review request and can invoke Granite after a low S1 score. Neither path alters classifier scores to justify discovery.
4. Granite returns ready/no questions, consequential clarification, or optional creative/refinement/decomposition help. Optional help does not imply the prompt is broken or prevent keeping the original. Unresolved requirements/conflicts cannot be labeled optional.
5. Clear/quiet prompts can request **Improve wording** explicitly without fabricating a clarification answer. Confirming an actual answer can still start a preview. No automatic check approves, inserts or sends text.
6. Wording approval applies to the exact saved proposal and destination; it does not authorize downstream execution.

With the HUD open, editing queues the latest draft after an 800 ms pause. Draft persistence precedes analysis. Empty text, hide/minimize, explicit cancellation, stale edits and storage failures cancel pending work. Unchanged failed/cancelled drafts are not automatically retried. The user confirmed live source typing, HUD synchronization and automatic checking working on October 26, before this review-policy change.

New routed analyses use sealed `analysis_snapshot.v5` blobs recording the review trigger and assistance kind independently of S1 evidence. Discovery attempts record the trigger; storage verifies it on insertion and restore. Readers for v2-v4 remain. SQLite tables are unchanged. The old keyword-based conflict override was removed so host routing cannot silently disagree with its saved classifier evidence.

The existing 20-second active analysis budget spans S1 and Granite; local warm-up is separately bounded and pauses that budget without resetting elapsed active time. S1 has an eight-second operation limit. Failed, stale, cancelled or malformed classifier results never establish a clear route. Original text remains available. Explicit review still requires a valid bound S1 result in this release; it does not bypass an unavailable classifier.

Default review language hides scores, internal enums and routine nonempty-output checks. Technical details are available separately. Rewrite validator v3 adds narrow checks for the observed minor-to-broad semantic expansion, loss of common explicit API clauses, and new unsupported timing/size/percentage quantities. These checks are conservative and incomplete; they are not semantic-equivalence proof. A paraphrased API clause or converted unit can be rejected even if equivalent. Recommendations used after explicit deferral must match one current alternative exactly; unmatched or ambiguous matches retain the original.

**Model quality remains an open gate.** Actual pinned Julia CPU probes are saved under `build/julia-triage-category-heads.json`: four-head latency was 304–416 ms on the twelve development examples, with clear-prompt false positives and incorrect categories. These examples are development evidence, not a held-out accuracy evaluation; the worker template is not release-calibrated. Correct routing mechanics must not be reported as classifier accuracy or the diagram's 15 ms target. Earlier Granite real-model quality failures remain recorded in [the native runtime evidence](NATIVE_GRANITE.md).

Granite grammar sampling, resource policy and runtime integrity work remain applicable. Temporary uncapped investigations use the explicit diagnostic helper/probe option; normal operations remain bounded. The original source layout and HUD/dashboard remain the physical/UI baseline. Focused native macOS HUD and installed-model routing checks are recorded in [the HUD/homepage follow-up](audit/hud-presentation-and-homepage-discovery.md); these do not establish complete third-party AX integration or cross-platform parity.


### Earlier v1 Qt/helper verification

`build/s1-live-qt-julia.json` records an opt-in run of the pinned checkpoint through the actual Qt supervisor and packaged worker (worker SHA-256 `e6162e62f37dfc228634309919cbc3f8aa863dfabc25c798416d9c15cbd2f651`). Loading and warming took 52.369 seconds, separately from the active analysis budget. Three classified development prompts took 361–418 ms: a bounded rename scored 0.0811 and routed ORIGINAL, ambiguous speed scored 0.9523 and routed CLARIFY with the incorrect Rule Conflict category, and a simple arithmetic request falsely routed CLARIFY at 0.9672. The run also verified the retained legacy scoring operation, cancellation without replay, explicit retry and owned shutdown. Its `verified` field means protocol/execution integrity, **not** semantic quality acceptance.


### Integrated supplied calibration work (v2)

The supplied revised head wording is retained, under `julia.prompt_triage.categories.v2`; v1 history remains readable with its original hashes. GPU/CPU auto-selection now reports and binds the actual BF16/FP32 profile and thread count. Unsupported BF16 hardware uses CPU; an automatic GPU warm-up failure can fall back to CPU. No logits, per-case predictions, or benchmark fixtures are altered.

The supplied `state[:3000]` shortcut was removed: the full frozen prompt is audited and classified or the operation fails visibly. The actual 3,021-token stress case passes all four full-input encoding audits. Q8_0 is the user-selected Granite default across the host, manifest, setup and probe tools; release acceptance is a separate question.

Fresh probability, timing, parity and challenge results, exact before/after wording, and limitations are recorded in [the calibration verification document](TEAM_CALIBRATION_REPORT.md). The integrated work is **not an all-criteria victory**: extra challenge cases, non-target head separation, CPU latency and maximum device probability deltas still fail the stated gates.
