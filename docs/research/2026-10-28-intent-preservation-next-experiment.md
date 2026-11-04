# Roclint intent preservation: mechanisms, gaps, and the next experiment

Date: October 28, 2026 (America/Los_Angeles).  
Status: research and implementation-ready experimental plan; **no new inference or product change**.  
Recommendation: first test a source-linked change plan in diagnostic replay, then isolate model discovery and context construction. Do not repeat the failed verifier or promote the freeform editor.

## 1. Decision and scope

Roclint's goal remains worthwhile help: ask a useful question when a user decision matters, offer concrete optional choices when useful, and preserve a complete, faithful request with little interaction cost. It should not turn every prompt into a technical specification interview.

The strongest connection to other industries is **controlled change to a current specification**, rather than another generic rewriting prompt. Borrow traceability, scoped changes, explicit supersession and receiver checks. Retain the existing immutable records, version bindings, span checks, atomic application and separate confirmation. These mechanisms already exist; rebuilding them would miss the remaining semantic problem.

**Recommended first experiment:** a private, no-inference replay with independently reviewed requirement dispositions and edit authority. Test whether the proposed representation can express the intended change, render a usable current request, and distinguish complete changes from omissions without preventing legitimate revisions. It is a test of representation and host mechanics, not model understanding. Only after that gate should a newly scoped model experiment compare autonomous discovery with diagnostic supplied targets/requirements and current-state context.

This report does not authorize any live-model run, download, training, production integration, runtime-policy change, installation, restart, commit, merge, push or message to another chat. Those are separate tasks. No model or application tests were rerun during this research task.

## 2. Evidence refreshed before proposing work

I read the [companion synthesis](/Users/jesse/roclint/docs/research/2026-10-28-cross-industry-intent-preservation-report.md) first, then inspected the current experiment files and newer evidence. The synthesis is a useful historical report, but its recommended mixed-reply, verifier and sequence studies have since been performed.

| Evidence | What it establishes | What it does not establish |
|---|---|---|
| [Earlier trusted-choice study](/Users/jesse/roclint_notes/2026-10-28-julia-integration-trusted-choice.md) | Across 96 paired cases, eight additional faithful views came entirely from deterministic captured choices; freeform remained 22/86 in both arms. | Autonomous freeform improvement, real Mac producer ownership, or representative accuracy. |
| [Newer 244-attempt study](/Users/jesse/roclint_notes/2026-10-28-semantic-failure-study.md) | Actual four-condition diagnostics, 32 mixed attempts, 60 verifier calls, 40 edit steps and 16 downstream toy attempts. Frozen evidence inventory verifies without mismatches. | Independently reviewed semantic truth, a calibrated omission detector, or actual downstream frontier-agent benefit. |
| [Newest diagnostic records](/Users/jesse/roclint_notes/semantic-failure-study-2026-10-28/diagnostics/results.json) | Faithful offered proposals: current inputs 10/24; author target hints 13/24; answer requirements 12/24; both 9/24. First-stage selection 15, 20, 15, 13 respectively. | A pure causal decomposition: controls were soft, author-provisional hints; target hints also included addition-mode information. Requirements covered the new answer, not a fully reviewed inventory of all active obligations. |
| [Mixed records](/Users/jesse/roclint_notes/semantic-failure-study-2026-10-28/mixed/results.json) | Split capture/comment: two faithful conflict-replacement proposals, one wrong-scope proposal; no faithful additive proposal on the twelve noncontradictory replies. Combined arm offered none. | Reliable mixed-reply handling. The synthetic split harness is separate from production capture and from the dispatcher's `contains_freeform` guard. |
| [Verifier assessment](/Users/jesse/roclint_notes/semantic-failure-study-2026-10-28/review/provisional-verifier-assessment.json) | Same-weight verifier accepted all twelve authored omission defects and both previously unsafe recorded proposals; blocked five previously judged faithful proposals. | A reliable safety gate or general verdict about every model/verifier architecture. These judgments are provisional and the balanced artificial pool is not production prevalence. |
| [Sequence assessment](/Users/jesse/roclint_notes/semantic-failure-study-2026-10-28/review/provisional-sequence-assessment.json) | Eight faithful color changes; later edits mostly rejected; one incomplete accepted channel proposal; one valid rejected rename leaves state identical. Complete reference requests also fail some toy interpretation checks. | Long-horizon deterioration of already accepted requirements: most requested changes were never accepted. The toy executor was Granite itself, not the user's coding agent. |

The latest recorded mechanical verification is **43 CTests and 296 Python tests**, superseding the earlier 261-test count. Three final sandbox failures were retained, then passed unchanged with localhost/process-inspection permissions. These remain historical test results, not tests executed for this report or proof of semantic completeness.

The older Julia assumption-description polarity mismatch is an isolated downstream SDK-contract issue. Its repair changed no initial route in the recorded 192 comparisons. It should not be called an initial-routing fix. Surface sensitivity remains separate. [Julia audit summary](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/docs/audit/julia-integration-trusted-choice.md:3).

**Review independence:** I have seen candidate outputs and authored earlier judgments. I cannot provide an independent blind review now. The [new control references](/Users/jesse/roclint_notes/semantic-failure-study-2026-10-28/control-reference-review.json), [new masked candidates](/Users/jesse/roclint_notes/semantic-failure-study-2026-10-28/review/identity-masked-edit-review.json) and [older reference packet](/Users/jesse/roclint_notes/julia-integration-trusted-choice-2026-10-28/review/blind-reference-review.json) still need an independent reviewer. Do not relabel these author controls as human-reviewed gold.

## 3. Actual path and the specific gaps

The inspected experiment worktree is `/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint`, branch `codex/slm-handoff-experiment`, HEAD `44ed23fbfde4a8e66537f39f72b2552ab4938ebc`. Source files are dirty/untracked; HEAD alone is not a source identity. The accompanying source audit records content hashes. The report is saved in the existing research directory of `/Users/jesse/roclint`, branch `feat/codebase-decoupling`, HEAD `103bd12afe25e1d69391e86ac396b5e86ea5f3ae`. Writing research there does not integrate the experiment into the app.

### Dispatch, inference and rendering

1. [Trusted dispatcher](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/trusted_choice_hybrid.py:14) constructs the question/event binding and checks captured provenance, event identity, option membership and the `contains_freeform` flag. Authenticated simple choices/Boolean/assumption/keep-original events use `BoundSession.receive`. Bare unbound short answers and combined comments must not acquire that authority merely from their words.
2. The [matched experiment's freeform branch](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/trusted_choice_experiment.py:69) uses staged unit selection followed by exact edits. It does **not** call `BoundSession.classify` or its lexical `coverage` method. Importing the question-bound module for types/context is not activation of that algorithm. For that branch, the matched harness uses its separately constructed `freeform_session` and common payload, not the dispatcher owner's typed renderer. The guard and the newest synthetic split harness are therefore distinct integration surfaces.
3. [Common visible context](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/question_bound_experiment.py:47) includes the historical original, current sources, question record, current answer/event, selection and syntactic answer parts. [Staged contracts](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/edit_ablation_contracts.json) say current sources are authoritative and new text must be an exact excerpt of the answer. Sentence/newline units are mechanical segmentation, not independent semantic requirements.
4. [Staged validation](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/edit_ablation_experiment.py:72) requires that edits represent the units and addition flag **the model selected**. If the model omits a requirement before that selection, the validator can still accept the incomplete proposal. This is not a whole-answer completeness proof.
5. [Minimal session](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/minimal_edit_prototype.py:100) retains frozen raw inputs and a separate current source view. It validates old/new excerpts, UTF-8 boundaries, user selections, non-overlap, current bindings and one separate confirmation. [Confirmation/application](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/minimal_edit_prototype.py:226) can require `complete_answer=True`, but the Boolean represents the confirming event's assertion; it does not establish actual coverage. Its plain renderer preserves source/question context and refuses pending corrections. It is not a general semantic compiler.
6. The [separate question-bound prototype](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/question_bound_prototype.py:304) already has locked targets, explicit operations, answer parts and a lexical coverage ledger. Its candidate target restrictions and confirmation checks also exist. They should be reused as reference mechanics where appropriate, not silently enabled on the better staged baseline. Lexical accounting can reject faithful paraphrases and cannot prove correct scope or relations.

### Rechecked failure signatures

| Failure | Exact evidence | Mechanism still needed |
|---|---|---|
| Relevant targets, incomplete answer | `mixed_change_complete / both`: green/black edits omit **No new dependencies**, despite its supplied answer requirement. | Coverage independent of model-selected units; source-linked dispositions for all obligations, tested against raw answers. |
| Wrong semantic ownership | `mix_yes_1 / split_choice_comment`: **Never send SMS** replaces **Yes** to **Should email remain enabled?** | A comment cannot acquire write authority over the captured decision merely by choosing its source ID. Distinguish a new restriction from a replacement. |
| Lost referent/task | Placement proposals replace **Add a Save profile button ...** with **Put it ... instead**. | Preserve the referent and task as obligations even when the placement span is valid. |
| Old interpretation reappears | `sequence_0_restriction`: new prohibition answer produces an attempted green-to-red edit. | Test historical versus current-state context, with complete current-state references and explicit supersession. |
| Honest fallback but no help | Many later sequence edits reject before a proposal. | Report completion/coverage as well as prevented corruption; do not call rejection itself progress. |
| Correct reference, wrong receiver behavior | Complete reference requests produce preview, but inbox is missed in all eight toy executions. | Receiver interpretation tests separate from rewrite correctness. |

The [native runner clears model memory before generation](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/edit_ablation_engine.cpp:334). The old-color response is **not proof of stale KV state**. Historical-context confusion is a hypothesis, not an established root cause. Changing context and source construction, target discovery and edit generation remain distinguishable candidates.

Existing [dispatcher regressions](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/test_trusted_choice_hybrid.py) cover obsolete IDs, stale bindings, duplicates, captured meanings, annotated replies and selection clipping. [Study regressions](/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/test_semantic_study.py) explicitly show that an omission can pass mechanical checks. None supplies missing independent semantic review or Mac event ownership evidence.

## 4. Cross-industry mechanisms mapped to this code

These are observed engineering practices, followed by **hypotheses for Roclint**. They are not empirical evidence that borrowing them will improve this model.

| Mechanism and verified primary source | Already present | Genuine gap and falsifiable test | Boundary |
|---|---|---|---|
| Rasa correction pattern: scoped slot correction, affirmation/denial, and a reset-only bypass. [Published flow](https://raw.githubusercontent.com/RasaHQ/rasa-calm-demo/main/data/flows/patterns.yml). | Typed question/answer records, current bindings, explicit replacement confirmation, rejection/idempotence. | Test **Yes + Never SMS** as two distinct effects; test a database answer replacement without rewriting the task. Expected: captured meaning survives unless an explicitly linked correction supersedes it. | Predefined slots are narrower than arbitrary coding intent. Do not copy an “always confirm everything” policy; the example itself has a bypass. |
| NASA: bidirectional source/requirement/verification traceability and change-impact management. [Requirements management](https://www.nasa.gov/reference/6-2-requirements-management/); March 25, 2026 [objective-evidence guidance](https://sma.nasa.gov/news/articles/newsitem/2026/03/25/identifying-objective-evidence-improves-requirement-implementation). | Raw originals/answers, source IDs, bound effects, frozen traces and tests. | Add a diagnostic disposition for **task, trigger, referent, new colors, no dependencies**, not merely edited spans. Inject a missing prohibition and a faithful paraphrase; determine both detection and false alarms. | A structured inventory can omit a requirement. Keep raw text and independent source-to-inventory review. No organizational approval bureaucracy is proposed. |
| Dafny: frame clauses constrain permitted memory writes; formal specifications also state postconditions. [Reference §7.1.6](https://dafny.org/latest/DafnyRef/DafnyRef). | Explicit user selections, source spans, non-overlap and atomic application already constrain edits. | Add candidate **semantic preservation obligations** around permitted changes. Footer-only change preserves button; placement change preserves named task; intentional goal replacement remains expressible. | Bytes outside a frame can remain identical while the resulting instruction loses meaning. This analogy does not formally verify natural-language intent or justify requiring the old goal after deliberate revision. |
| Kubernetes Server-Side Apply: field-manager ownership, conflicts and explicit takeover. [Official reference](https://kubernetes.io/docs/reference/using-api/server-side-apply/). | Provenance, question/decision identity and stale-result protection. | Bind proposed writes to an independently established decision/requirement capability; test a comment trying to replace an unrelated captured Boolean, and an intentional takeover of the linked value. | Ownership is not permanent priority or truth. Do **not** copy deletion-on-omission: an omitted prompt requirement must not silently disappear. |
| FAA readback checks and healthcare closed-loop communication. [FAA §2-4-3](https://www.faa.gov/air_traffic/publications/atpubs/atc_html/chap2_section_4.html); [AHA toolkit](https://www.aha.org/center/project-firstline/teamstepps-video-toolkit/closed-loop-communication). | Before/after transcripts and a separate confirm event. | Compare existing diff with a brief interpretation showing target, changed value and critical retained restrictions. Inject plausible defects and measure detection, false alarms, elapsed time and abandonment. | Existing confirmation does not prove understanding. Healthcare guidance is not evidence of zero added time in Roclint. No mandatory extra review screen is justified without a user-effort study. |

The common lesson is **traceable authority plus explicit preservation**, not keyword matching or a second unvalidated model verdict. Engineering practices constrain and expose changes; they do not solve extraction by analogy.

## 5. What the 2026 AI papers add—and do not add

All links below were checked against primary publication/author pages for this report. These are experimental motivations, not transferred performance estimates.

| Source, date/version checked | Relevant result or method | Specific use and limit |
|---|---|---|
| [Asking What Matters / CLARITI](https://arxiv.org/abs/2604.14624), April 16, 2026 | Clarification value depends on task relevance and what users can answer; trained 8B clarifier evaluated with simulated users. | Grade user-facing question relevance/answerability separately from ambiguity scores. No training or expected Granite gain follows. |
| [Structured Uncertainty guided Clarification](https://aclanthology.org/2026.findings-acl.2028/), ACL Findings, July 2026 | Separates specification uncertainty from model uncertainty; models question value and cost over tool parameters. | A low-confidence model need not ask the user about a decision already supplied or delegated. Tool domains are more structured than open coding requests. |
| [When Models Edit Too Much](https://arxiv.org/abs/2609.04061v2), September 3 / revised October 20, 2026 | Code-repair correctness and edit fidelity differ; bigger models/reasoning do not automatically remove over-editing. | Score requested-change completion and unrelated requirement loss separately. Code-edit metrics are not prompt-semantic ground truth. |
| [Clarification Is Not Correction](https://arxiv.org/abs/2609.25337), September 21, 2026 | Controlled Gemini dialogues show order effects and persistence of early interpretations. | Motivate a current-state versus historical-context ablation. It does not establish that Granite has the same failure mechanism. |
| [MT-InfoSeek](https://www.microsoft.com/en-us/research/publication/do-llms-know-what-to-ask-and-when-evaluating-multi-turn-information-seeking/), August 2026 | Measures information sufficiency separately from final answer generation. | Score target selection, inventory completeness and rendering independently. Its math/logic/knowledge tasks are not this editor. |
| [SEQUOR](https://arxiv.org/abs/2605.06353v2), revised May 8, 2026 | Tests constraints through simulated conversations with additions/replacements. | Inspect active requirements after every event. Eight mostly rejected five-step traces do not demonstrate the same long-horizon phenomenon. |
| [Trust but Verify](https://arxiv.org/abs/2605.25133), May 24, 2026 | Prover–verifier coverage/precision depends on verifier competence; formal proof guarantees do not transfer to imperfect models. | The local failed same-weight verifier is evidence against that candidate, not a reason to add it as protection. A different checker needs its own acceptance/error/cost test. |
| [PriceCheck](https://arxiv.org/abs/2609.37493), September 27, 2026 | Calibrates selection rules using labelled data and evaluates risk among accepted answers alongside coverage/cost. | Report useful coverage with unsafe offers and checker expense. Do not call small unreviewed editing pools calibrated or borrow its mathematical-task guarantee. |
| [Microsoft DELEGATE-52 clarification](https://www.microsoft.com/en-us/research/blog/further-notes-on-our-recent-research-on-ai-delegation-and-long-horizon-reliability/), May 15, 2026 | Artifact-preservation stress tests differ from overall capability and user outcomes. | Separate accepted-requirement loss, unfulfilled changes, toy outcomes and actual user benefit. |
| [Anthropic evaluation guidance](https://www.anthropic.com/research/demystifying-evals-for-ai-agents), January 9, 2026 | Distinguishes trials, traces and environment outcomes; recommends repeated trials and suitable graders. | Freeze analysis, retain failures, use independent review and score the receiving task. Repeated research trials are not production retries or best-of selection. |

## 6. Candidate requirement/change representation

This is a **private experimental design**, not a public or persisted schema and not an assertion that the inventory is complete. Avoid turning all prose into a rigid domain ontology.

| Record | Minimum fields and semantics |
|---|---|
| `SourceEvent` | Host-issued event ID, actor/provenance, immutable raw text/hash, revision/analysis/decision-set/edit generation; question ID/version/hash and option ID where captured. Raw text survives any derived interpretation. |
| `RequirementVersion` | Stable requirement ID plus immutable version; source-event IDs, validated UTF-8 anchors and exact source quotes; optional tentative semantic roles such as task, entity/referent, trigger, recipient, prohibition, number/unit, format or delegation. Preserve groups when splitting would lose a relationship. |
| `RequirementState` | Proposed, active, superseded, explicitly retracted, unresolved. Record interpretation origin and independent-review status separately from user confirmation. Model confidence is neither authority nor review. |
| `ChangeIntent` | Add, replace, retract, retain, delegate or unresolved; links to exact prior requirement versions, complete raw answer, proposed effects, scope and relevant dependency/group IDs. Mixed effects remain one pending transaction until all are accounted for. |
| `EditAuthority` | Host-validated allowed decision/requirement IDs and versions, source ranges and permitted operation classes. Origin: actual captured mapping, explicit user target selection, confirmed target resolution or **diagnostic reference**. A model cannot mint authority by naming a valid source ID. |
| `Disposition` | For each known prior requirement: retained or explicitly superseded/retracted by a linked change. For every new reviewed requirement: added, incorporated by a replacement, declined/withdrawn explicitly, delegated, or unresolved. Include evidence linking it to effects and resulting request. |
| `UnresolvedMaterial` | Raw ranges and semantic questions that the inventory cannot confidently explain; unresolved referent/target/operation/conflict and whether the user can resolve it. A parser/model failure is not automatically a new user decision. Byte accounting is a diagnostic, not proof of semantic completeness. |
| `Confirmation` | Separate trusted event bound to proposal, scope, dependencies and current state. Reject/stale/duplicate semantics reuse current machinery. Confirming a proposal does not convert incomplete extraction into proven intent or authorize downstream destructive actions. |

Keep an append-only source/event history and derive a current projection. Supersession links target versions; do not apply “latest answer wins” globally. Retraction does not imply the opposite action: withdrawing **no dependencies** permits reconsideration, not a requirement to add dependencies. Rejecting an assumption means **do not assume it**, not that its negation is true. Reintroducing a withdrawn requirement creates a new event/version, not reuse of an obsolete ID.

A current requirement may still need its earlier referent/trigger context. Do not remove original context unless the current projection retains that information or explicitly marks the gap unresolved. Requirements can be related rather than disjoint “slots.” Unaffected bytes alone are inadequate preservation evidence.

### Worked dispositions for reviewed pilot cases

These illustrate the candidate design. They are development examples, never fresh evaluation references.

- **Compound colors:** existing task/navigation remain active; button color is superseded by green; footer color by black; dependency prohibition is added. A plan with just the two replacements is incomplete. A compact rendering can retain the named task and append the restriction without repetitive labels.
- **Captured Yes + new restriction:** **Should email remain enabled? Yes** stays active; **Never send SMS** is an addition. The comment has no authority to replace the email decision. A later explicit **Actually disable email** may propose a linked replacement requiring confirmation.
- **Placement:** a placement field changes while the Save profile feature, add-feature action and mockup/backend restriction remain. **Put it ...** alone cannot be marked faithful as a standalone handoff merely because the placement is right.
- **Goal revision:** **Improve performance** can be superseded by **Investigate only; do not change code**, retaining the API restriction. Preserving the superseded improvement goal as an active requirement would be wrong.
- **Ambiguous broader scope:** selecting the footer does not authorize changing the button too. A coordinated-color suggestion may name both targets as expanded scope and request separate confirmation; it is not an automatic inference from color theory.

“Complete” must have an origin: *reviewed inventory complete for this fixture*, *model claims complete*, and *user confirmed this preview* are distinct. Only the first supports diagnostic coverage scoring; none is a general natural-language guarantee.

## 7. Ranked experimental sequence

### Experiment 0 — reviewed change-plan replay (first, no inference)

**Learning:** can the representation and host application express a complete intended correction without destroying legitimate goal changes or demanding spurious user answers?

**Inputs:** independently review sixteen exposed interactions selected before replay: four compound/restriction cases, four captured choice/comment cases, four prior-answer/goal corrections, four referent/repeated-target cases. Use current state before each event, not a model-corrupted later state. Include the known dependency, Save profile, email/SMS and exclusivity failures. Allow reviewer disagreement and multiple faithful renderings; do not resolve uncertainty by author fiat.

For each case, the reviewer supplies (a) intended current obligations, (b) intended effects/authority, (c) unresolved information, and (d) faithful alternative wordings. Create a faithful plan and one deliberate incomplete/wrong-scope variant. **32 deterministic plan replays**, plus state-machine fixtures, consume no model tokens. These are diagnostic supplied plans, not autonomous results.

Run the existing source-bound renderer/application beneath a private adapter. First establish that the faithful plans can be represented. Then inspect dispositions independently of the existing validator. A missing dependency requirement should be visible even if selected-unit validation passes; a faithful paraphrase must not fail merely for different tokens. For general paraphrases the disposition may require semantic review rather than an automatic verdict. An unknown result is preferable to inventing a proof.

**Smallest reviewable implementation, later task:** three new private files under `/Users/jesse/.codex/worktrees/slm-handoff-experiment/roclint/tools/`: a requirement-state/disposition adapter, a recorded-input replay/report command, and focused tests; plus reviewed fixtures in the existing tools layout. Reuse `Session`, `CoreBridge` and confirmation logic. Do not edit frozen runners/contracts, activate the old lexical gate, add a new model call, alter schemas/registry/SQLite or integrate the Mac HUD.

Proposed private functions: `project_current(events)`, `validate_authority(plan, state)`, `account_dispositions(before, answer_inventory, plan)`, `render_candidate(state, plan)` and `compare_replay(candidate, review)`. Their outputs must distinguish mechanically established facts, supplied reference facts and unresolved semantic claims. No blanket Boolean “semantically verified.”

**Reject/revise the candidate if:** a reviewed faithful goal change cannot be represented; referents or triggers must be discarded; a proposed field mapping silently grants wider scope; omission cannot be surfaced even with a complete supplied inventory; or the output claims completeness from token coverage/confirmation alone. Passing this gate establishes only an adequate experimental representation—not extraction accuracy or product readiness.

### Experiment 1 — separately scoped model component diagnostic

Only after independent reference review and a separately authorized implementation/run. This is **not** a restart of the 244 attempts. It replaces soft author hints with an explicit component boundary, inventories all reviewed active obligations, and tests context construction. Preserve prior evidence and run under a new version/output directory.

Use sixteen exposed development interactions. Freeze contracts, examples, source projections, model/runtime settings, seeds/trial schedule and metrics before generation. Compare the four T/R conditions below under two contexts: C0 current historical-original-plus-current-input construction; C1 a complete current-state projection with current question/event, latest answer, required referent/trigger context and supersession links. Raw historical evidence remains in the host archive; C1 does not delete it. Build autonomous C1 only from captured host source/event records, without filling gaps from human references. Review source-to-projection equivalence separately: if a referent or obligation is missing, record projection failure and keep that case in the denominator. Do not compact unresolved requirements away or use reference information in autonomous C1.

| T/R condition | Target discovery | Requirement extraction/completeness | Edit generation | Interpretation |
|---|---|---|---|---|
| Baseline | Model's staged selection | Existing implicit selection/addition representation | Existing narrow edit task | Autonomous baseline for this new protocol; use C0 and C1. |
| Supplied target | Independently reviewed units/operation scope bypass stage-one selection | Model must still account for the complete answer and active obligations | Same narrow edit task within supplied target/addition scope | **Diagnostic oracle**. New versus old study: an enforced/bypassed component, not another soft hint. |
| Supplied complete requirements | Model discovers target | Reviewed active requirements, complete answer obligations and unresolved facts are supplied | Same narrow edit task | **Diagnostic oracle** for extraction. No target offsets/IDs or completed output hidden in this inventory. Wording can still indirectly reveal targets. |
| Both | Reviewed target/operation scope supplied | Complete reviewed inventory supplied | Model generates edits | **Diagnostic oracle** for the remaining generation/representation problem; never an autonomous score. |

T controls include replacement, removal, addition and group dependencies; “no target” can mean a genuine addition. The model does not receive a ready final prompt. Reviewed controls are semantic assistance intentionally supplied only to their declared oracle arms. Scope restrictions cannot grant permission to alter an unrelated requirement.

Use the same two-stage examples/contracts where the task is unchanged; version only interfaces required by the candidate inventory/context. Count any extra call or changed instruction as a workflow difference. Oracle target bypass frees model time/tokens; report that explicitly, and score the remaining generation component separately. An oracle's faster time or higher completion is not a production gain. Context also changes length/representation, so improved C1 would not by itself prove a particular cognitive theory or stale-cache defect.

**Validation is a separate, paired replay factor:** feed identical offered candidates to V0 existing validators and V1 shadow dispositions/frame checks. Do not regenerate candidates under V1 or silently introduce a new rejection policy. Measure flags on both faithful and defective candidates, including source-quote-valid wrong scope. Known reviewed inventories provide an upper-bound diagnostic; repeat with extracted inventories to expose inventory omissions. A missing requirement from the inventory must not disappear from evaluation merely because V1 sees nothing wrong.

**Requirement extraction needs its own observable output.** The current baseline does not emit a semantic inventory, so its unit selection must not be scored as if it did. Add a separate frozen extraction probe: sixteen development interactions × three trials = **48 single-call attempts**, each with its own 18-second/2,048-token cap. Ask for source-grounded requirements/groups, semantic relationships and unresolved material from raw current sources/question/answer; score omissions, inventions and scope against the independent inventory. Do not feed probe results into primary arms or treat this as a hidden third pipeline call. This measures the model on an explicit extraction task, not proof of what the original editing call internally understood. If the probe extracts a requirement correctly but generation drops it, that supports testing an explicit intermediate representation; it does not establish that the representation will improve end-to-end behavior.

**Evaluation budget proposed for the later task:** sixteen cases × eight T/R/context cells × three independently scheduled research trials = **384 attempts**, at most 768 generation calls, a common 18-second/2,048-generated-token case cap, at most two calls, no automatic retry or best-of selection. Worst-case work budget: 115.2 minutes and 786,432 generated tokens, excluding model loads, setup and review. Repeating identical fixed-seed inputs may be identical: record that fact; predeclare a seed schedule if variability is to be measured, with each seed matched across arms. Seed changes are experiment-only, not a production policy change.

**Read the results component by component:**

- Bad target selection, successful supplied-target generation: investigate target discovery and question/decision mapping.
- Correct targets, incomplete inventory; supplied inventory helps: investigate extraction/disposition coverage.
- Correct supplied target and complete inventory but bad edit: generation or representation remains limiting; stop adding discovery complexity.
- Current-only context helps on the same reviewed intention: investigate historical/current construction, preserving referent information. Confirm on fresh data before causal/general claims.
- Faithful candidates blocked by V1: false rejection; do not enable it as a gate.
- Complete reference request fails downstream: isolate receiver competence or request-format compatibility before blaming the editor.

### Experiment 2 — fresh evaluation and downstream value

Select at most one candidate on development evidence, freeze it, and compare with baseline. Proposed budget: **24 fresh cases × two arms × three trials = 144 attempts**, at most 288 generation calls, 43.2 minutes and 294,912 generated tokens at the same caps. The primary diagnostics and fresh comparison total 528 attempts and at most 1,056 calls. Including the separate 48-call extraction probe, the full proposed upper bounds are **576 attempts, 1,104 calls, 172.8 minutes of work and 1,179,648 generated tokens**, excluding loads, setup, reference review, downstream execution and user studies. Actual counts include failures, not automatic replacement attempts.

Reserve fresh cases by entire semantic-composition family, not random rows or renamed nouns. Proposed held-out family candidates include conditional exceptions, scope-sensitive comparative limits, cross-question referents, retraction with conditional reintroduction, dependency-coupled goal changes and coordinated edits with explicit scope enlargement. These are **not certified untouched**: the independent curator must audit all earlier fixtures/reports for exposure, replace overlapping families or label them exposed, and keep held-out inputs/intentions away from prompt/example authors until the protocol is frozen. If genuine family holdouts cannot be established, report fresh instances of exposed families rather than a false holdout claim. Examples in this report are development material.

Keep reference judgments and candidate judgments separate and identity-masked; a second reviewer adjudicates disagreement. Permit multiple faithful expressions and valid unresolved intentions. Freeze the output set before independent review; no tuning to held-out failures.

Downstream test inputs should include unchanged original-plus-confirmed answers, append-preservation control, candidate handoff and a reviewed complete reference request. Give every receiver arm the same permitted workspace/context/tools. Score request fidelity before execution. Use a deterministic configuration/state interpreter to verify known cases and, in a separately scoped task, the actual intended coding agent on isolated disposable repositories. Do not infer real-agent benefit from another Granite toy run.

For explain-only goals, separate **actual-handoff compliance** from **full intended-goal completion**. A goal correction that never became active is unmet; applying the still-active old update task is not evidence of violating an accepted explain-only instruction. No destructive real data or external execution belongs in these fixtures.

### Later, if fidelity passes — readback value study

Compare current diff-only review with a concise target/value/retained-restrictions readback using both faithful and deliberately defective previews. Suggested pilot: 6–8 consenting participants, including nontechnical builders and engineers, 12 randomized/counterbalanced tasks each; no real production delivery. Measure detection, correction time, false alarms, abandoned edits and perceived workload. Do not equate number of confirmations or clicks with comprehension. This pilot estimates feasibility/effort, not population safety. It is separately authorized work, not new app integration in this task.

## 8. Concrete test families and expected observations

Include benign counterparts, legitimate revisions and faithful paraphrases for every failure pattern. Old examples are regressions; future fresh families need the exposure audit above.

| Family | Development example / contrast | Expected observable behavior |
|---|---|---|
| Compound edits | Button green, footer black, no dependencies; versus either color alone. | Three obligations accounted for; unrelated navigation/task retained. Partial color-only candidate is incomplete, not faithful. |
| Referents and task identity | Move **it** to Settings after **Add a Save profile button**; ambiguous two-button counterpart. | Named feature/action survive. Ask which target only when context does not settle it. |
| Negation and exclusivity | Email affected customer **only**, no SMS or staff broadcast; remove one prohibition explicitly. | Trigger, recipient, quantifier and both negatives stay linked correctly; permitted withdrawal affects only its target. |
| Numerical limits | Change 8 KiB to 16 KiB; contrast request containing both a byte limit and timeout. | Correct number/unit/relation and target; no conversion or inequality inversion without support. |
| Intentional goal changes | Improve speed → investigate only, do not change code; preserve API. | Old implementation goal superseded, new prohibition and API constraint active. Do not reject merely for contradicting the superseded goal. |
| Repeated targets | Two blue elements; footer selected; then request both as a broader suggestion. | Footer-only scope respected. Broadening names both targets and waits for explicit confirmation. |
| Mixed option/comment | Visual only + preserve API; Yes to email + never SMS; keep original + restriction. | Complete canonical meaning and complete comment survive; no captured answer overwritten by unrelated text; pending comment prevents premature completion. |
| Genuine versus apparent conflict | Plain text only + wrap in HTML; compare fix root cause + show a toast. | First conflict stays visible pending resolution. Compatible multiple steps do not become an invented conflict. |
| Retraction / restoration | Withdraw no-dependencies rule; later reintroduce it. | Explicit new versions, no implied instruction to add dependencies, no obsolete event reuse. |
| Stale / duplicate events | Same option label with a new ID/version; late result after edit; duplicate confirmation. | Existing guards still reject stale identities and preserve idempotence; no new model run is needed for these mechanics. |
| Surface variants | Meaning-equivalent initial capitalization/punctuation; contrast quoted case-sensitive identifiers. | Score Julia route stability and editing fidelity in separate tracks. Do not demand invariance for meaning-changing code/string cases. |
| Multi-step preservation | Add restriction, revise channel, reject rename, retract a limit, change goal. | Inspect active requirements and provenance after every step. Record unfulfilled attempts separately from accepted losses; no silent restoration from history. |
| Necessary help versus delegated work | Modern/playful existing bakery site with visual discretion delegated; investigate bottleneck first; unknown Save profile location. | Optional visual ideas or proceed for delegated work. Ask a user-answerable, consequential question only for the unresolved decision. Permit more than one useful question order where reviewed context supports it. |
| Unambiguous consequences | Explicit irreversible data-deletion request in a nonexecuting fixture. | Clear intent is not “ambiguous” because consequences matter. Prompt review/confirmation is distinct from authorization to perform downstream deletion. |

A question-count target is not fixed. Evaluate whether another answer could materially change the intended task, whether the coding agent could investigate it, whether the user delegated the choice, and whether the user can answer. Placement and behavior can both matter; references should not decree one universal first question without context.

## 9. Metrics, adjudication and rejection rules

Keep separate denominators for initial Julia triage, initial discovery/options, correction questions, proposal fidelity, confirmed state transitions and downstream delivery/execution. The present editing studies do not measure initial discovery usefulness.

| Metric | Definition / denominator |
|---|---|
| Faithful offered completion | Complete, reviewed-faithful candidate / all accepted editing attempts, including rejected/unresolved/timeout attempts in the denominator. Count proposal and actually confirmed outcomes separately. |
| Unsafe offers | Definite omission, wrong scope, unsupported requirement or unresolved contradiction in an offered candidate / all attempts; also report conditional rate among offers. Do not include superseded requirements as still-active preservation obligations. |
| Target / inventory / generation | Correct target/operation scope; complete active-and-new inventory against raw reviewed intention; faithful rendering given its supplied inputs. Score independently and record uncertainty. |
| Omissions and inventions | Identify which requirement/relation/quantifier/trigger changed, not just edit distance. Multiple faithful wordings allowed. |
| Useful / unnecessary questions | Relevant user-answerable missing decision versus supplied/delegated/investigable information. Poor or uncertain questions get their own categories. Separate deferral from a helpful question. |
| Validation precision/coverage | False acceptance and false rejection on independently labelled candidates, useful retained completions and cost. A reject-all system has zero useful coverage and undefined error among accepted outputs. |
| State transitions | Accepted losses, unmet requested changes, rejected proposal identity, stale/duplicate handling, supersession and restoration after each event. |
| Budgets | Absolute end-to-end attempt time including dispatch/validation, caller cleanup overhead separately, calls, input/final/reasoning/termination tokens, timeouts and cold-load time. Do not combine two separately budgeted calls and claim one 18-second pipeline. |
| Downstream outcome / effort | Actual assertions and preserved artifacts under matched context, actual-handoff compliance, full reference-goal completion, human detection/correction time and abandonment. |

Report uncertainty and reviewer disagreements as uncertainty, not as faithful by default. Model judges may assist only after human calibration and with separate error analysis; the failed same-weight verifier is not the arbiter. For repeated trials, show per-case distributions and paired differences; cluster uncertainty at case/family level rather than treating correlated variants as independent samples.

Predeclare these conservative development decisions: any independently reviewed unauthorized loss prevents promotion; the sixteen reviewed faithful replay plans must be representable without unexplained loss; shadow false-rejection flags must be visible rather than silently gating; all stale/binding/delivery-separation regressions remain mandatory. A candidate with fewer questions only because it rejects more is rejected as an improvement. Stop/revise if oracle-both still cannot render complete changes, context projection removes necessary intent, or candidate gains vanish when incomplete/rejected cases remain in the denominator. Numeric production-risk guarantees and optimal user-effort thresholds require larger independently reviewed data; this pilot cannot establish them.

## 10. Freeze, implementation boundaries and deliverables for a later task

The later implementation should add only a private experimental adapter, replay/report harness, fixtures and focused tests. New model runs require separate authorization after independent intentions are recorded. No automatic retry, best-of selection, production policy change, new public API, persisted schema, database change, registry edit, app installation or Mac capture change is proposed.

Before generation, freeze: source/event/state versions; intended-behavior reviews and disagreement records; fixture family/exposure register; contract/examples; actual serialized model inputs; T/R/C/V condition definitions; sampling/budgets; hashes; order/trial schedule; semantic grading criteria; cost/stop rules. Keep references and labels outside autonomous model payloads. Use opaque case IDs and sentinel-leakage tests. Oracle fields are deliberately labelled diagnostic in reports and never counted as autonomy.

Focused mechanics should cover immutable raw records; UTF-8/repeated ranges; complete-source limits; stale/unknown/duplicate IDs; authority narrowing/broadening; atomic multi-effect application; confirmation rejection; explicit retraction/restoration; dependencies; shadow validation and retained incomplete raw text. These extend existing tests instead of duplicating their successful coverage. Add human-reviewed paraphrase counterexamples to expose lexical overreach.

Separate classes in the output: model semantic error, invalid/ungrounded proposal, missing user information, unjustified question, timeout/budget, deliberate deferral, stale state, rejected confirmation, fixture/reference disagreement and infrastructure/environment failure. A pilot repair after generation gets a new version/run, with the failed evidence retained.

Deliver frozen inputs/configuration, per-component inputs/outputs and hashes, complete raw final responses, token/timing data without internal reasoning text, before/after state/dispositions, confirmation transcripts, reference packets, masked candidate packets, detailed failure cases and a continue/revise/stop recommendation. Refresh and run the relevant build/tests only in that separately authorized implementation task; report actual counts and unavailable platform/UI checks. A later rollout must separately establish persistence, actual Mac event ownership, format/profile handling, downstream compatibility and user benefit.

## 11. Audit of this research task

Only new research documents were written under the existing `/Users/jesse/roclint/docs/research` directory. No experiment runner was imported or invoked, no model run was restarted, and no production source or old evidence was changed. At the start, read-only fingerprints recorded 337 experiment-worktree files and 242 main-checkout files. The [source audit](/Users/jesse/roclint/docs/research/2026-10-28-intent-preservation-source-audit.json) records preservation results, exact inspected source hashes, evidence identity and verified external references.

The right immediate investment is to make **complete intended change** inspectable and representable independently of model choice, then test whether Granite can supply it. More prompts, mandatory confirmations or a second weak verifier are not substitutes for that evidence.
