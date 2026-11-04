# Roclint: intent preservation across industries

Date: October 28, 2026  
Purpose: Research findings and an experimental agenda for the GPT-6.1 sol agent.  
Status: Research synthesis; no new model evaluation or product change accompanies this report.

## Assessment

Roclint aims to help users express their intended task through useful questions, concrete choices, and faithful final requests, while preserving control and reducing downstream surprises. The latest experiment demonstrates a narrow gain from processing captured discrete answers deterministically. Reliable freeform interpretation and complete editing remain unresolved.

The most useful cross-industry connection is **controlled change to an evolving specification**. Conversational systems, requirements engineering, formal verification, configuration management, and operational handoffs each address part of this problem. Their mechanisms supply testable hypotheses for Roclint; they do not automatically solve natural-language interpretation.

Priority: study requirement traceability and explicit preservation obligations on the actual staged freeform path, then evaluate mixed replies and downstream outcomes. Retain the working discrete-answer path as the experimental baseline.

## 1. Evidence baseline and limits

The primary baseline is the user-supplied **Julia integration audit and trusted-choice hybrid**, October 28, 2026. Local report: [full audit](/Users/jesse/roclint_notes/2026-10-28-julia-integration-trusted-choice.md). These are reported results, not fresh measurements made for this synthesis.

| Measurement | Staged control | Trusted-choice hybrid |
|---|---:|---:|
| Complete faithful result, 96 cases | 22 | 30 |
| Faithful model-selected proposal | 22 | 22 |
| Faithful zero-Granite-call answer view | 0 | 8 |
| Unsafe complete proposal | 2 | 2 |
| Semantically uncertain proposal | 4 | 4 |
| Questions offered | 24 | 21 |
| Unnecessary questions | 18 | 15 |
| Useful questions | 1 | 1 |
| Median attempt | 10.14 s | 10.04 s |

All eight gains were deterministic captured-answer views. The 86 freeform cases produced 22 faithful outcomes in each arm. These denominators are a synthetic evaluation corpus, not population accuracy or a percentage of product completion. The questions above concern the correction/edit workflow; they do not measure all initial discovery behavior.

The same two unsafe cases occurred in both arms:

- `placement_settings`: replacing a named Save profile task with “Put it...” loses the feature/referent.
- `hybrid_compound_colors`: changing both colors omits “add no dependencies.”

The staged freeform path uses source-unit selection, strict parsing, span validation, and a minimal renderer. The newer question-bound classifier and lexical coverage ledger are **not used on this path**. Their tests cannot establish freeform semantic completeness.

An isolated downstream Julia assumption-description polarity repair changed no initial triage route in 192 comparisons. Capitalization changed routes in 9/64 cases; terminal punctuation changed routes in 8/64. Score changes do not establish semantic improvement, and probabilities are not calibrated certainty.

Reported verification: 43 CTests and 261 Python tests passed. The final mixed-reply guard received mechanical tests and replay; live-model quality on newly tagged mixed replies was not evaluated. Captured controls were synthetic producer events. Independent semantic review, real Mac event ownership, app/persistence integration, and downstream task benefit remain unverified. Current-attempt confirmations, approvals, and deliveries were zero.

## 2. Cross-industry findings

### A. Conversational products: corrections and dialogue state

**Observed practice:** Rasa publishes an explicit correction flow that can confirm a proposed slot correction before applying it. It represents conversational repair as a state transition. [Rasa correction-flow implementation](https://github.com/RasaHQ/rasa-calm-demo/blob/main/data/flows/patterns.yml).

**Roclint hypothesis:** represent whether an answer adds, replaces, retracts, retains, delegates, or leaves a requirement unresolved. Bind the interpretation to the actual question/event and current requirement. Treat inferred operations as proposals, not confirmed user intent.

**Tests:** a correction to an earlier database choice; Yes with an additional prohibition; retaining one decision while retracting another; changing the subject; answering two questions at once; a bare unbound Yes. Test the resulting active requirements, not only successful event receipt.

**Boundary:** predefined slots have a more constrained domain than arbitrary coding requests. An extensible representation needs unresolved free text and provenance; it must not force all user intent into a fixed interview template.

### B. Aerospace: requirements traceability and change impact

**Observed practice:** NASA maintains traceability across requirement sources, design, implementation, and verification, and evaluates the effects of changes. Its March 25, 2026 guidance emphasizes objective evidence and requirement-level traceability. [Requirements management](https://www.nasa.gov/reference/6-2-requirements-management/), [2026 objective-evidence guidance](https://sma.nasa.gov/news/articles/newsitem/2026/03/25/identifying-objective-evidence-improves-requirement-implementation).

**Roclint hypothesis:** account for every original and newly supplied requirement through a source-linked disposition: retained, replaced by a confirmed requirement, explicitly withdrawn, or unresolved. Rendering and validation should inspect the current requirement state.

**Tests:** preserve the named task during placement changes; retain no-dependencies restrictions during multiple color changes; preserve triggers, recipients, units, and prohibitions; distinguish an implementation change from a changed user goal.

**Boundary:** extracting requirements is itself fallible. A complete-looking model-generated list is not proof of coverage. Evaluate extraction against independently reviewed raw inputs and retain the original text. Borrow traceability mechanics without importing organizational approval bureaucracy.

### C. Formal verification: frame conditions

**Observed practice:** Dafny specifications restrict the locations an operation may modify through frame conditions. [Dafny reference: framing](https://dafny.org/latest/DafnyRef/DafnyRef).

**Roclint hypothesis:** give an edit an explicit modification scope plus preservation obligations for the surrounding task. Span validity and semantic preservation are separate checks.

**Tests:** a footer-only edit leaves the button unchanged; a database correction retains the public API restriction; unrelated requirements survive removal of a conflict; independent edits commute when their targets and dependencies are genuinely disjoint; rejected changes leave the active state unchanged.

**Boundary:** formal verification checks an explicit formal specification. It cannot prove that an extracted specification fully captures natural language. Avoid claiming semantic soundness from schema or span checks.

### D. Configuration management: ownership and conflicts

**Observed practice:** Kubernetes Server-Side Apply tracks field managers and reports conflicting field updates, with explicit mechanisms for overriding or transferring ownership. [Server-Side Apply](https://kubernetes.io/docs/reference/using-api/server-side-apply/).

**Roclint hypothesis:** distinguish user-authored requirements, captured choices, model assumptions, and pending proposals. Record explicit supersession relationships and prevent an inference from silently replacing a confirmed requirement.

**Tests:** stale proposals against newly confirmed requirements; obsolete option IDs; repeated labels with distinct identities; proposals that widen a selection; retraction followed by later reintroduction; conflicting pending changes.

**Boundary:** ownership does not establish truth or permanent priority. Users may intentionally revise prior requirements. The goal is explicit, scoped supersession. Roclint already covers many mechanical variants; new tests should exercise the semantic target mapping as well.

### E. Aviation and healthcare: readback and closed-loop communication

**Observed practice:** FAA procedures require checking the correctness of items read back; healthcare closed-loop communication includes repeating an interpretation and verifying it with the sender. [FAA readback procedures](https://www.faa.gov/air_traffic/publications/atpubs/atc_html/chap2_section_4.html), [AHA closed-loop communication](https://www.aha.org/center/project-firstline/teamstepps-video-toolkit/closed-loop-communication).

**Roclint hypothesis:** show a concise interpretation preview that exposes the changed target, resulting value, and critical preserved restrictions. Evaluate its ability to reveal mistakes with low user effort.

**Tests:** inject a wrong target, dropped negation, changed number, or answer bound to the wrong question; measure detection and correction time. Include faithful previews to measure false alarms and unnecessary friction.

**Boundary:** users and reviewers can miss incorrect readbacks. Confirmation is not semantic proof. Do not add mandatory repetitive confirmation to every interaction; measure the tradeoff with Roclint's low-friction goal.

## 3. Relevant 2026 AI research

These studies motivate experiments; their reported improvements should not be projected onto Roclint.

| Source | Relevant finding | Application and limit |
|---|---|---|
| [Asking What Matters, April 2026](https://arxiv.org/abs/2604.14624) | CLARITI trains an 8B clarifier using task relevance and user answerability; reports matching GPT-5 resolution with 41% fewer questions in its SWE setting. | Score the usefulness and answerability of questions. Simulated users limit direct claims about real-user benefit. |
| [Structured Uncertainty Guided Clarification, ACL Findings, July 2026](https://aclanthology.org/2026.findings-acl.2028/) | Separates specification uncertainty from model uncertainty and uses value/cost criteria for questioning. | Test concrete unresolved decisions instead of equating a score with useful clarification. Tool-parameter results do not validate arbitrary prompt interpretation. |
| [When Models Edit Too Much, September 2026; revised October](https://arxiv.org/abs/2609.04061) | Edit fidelity is a distinct dimension from successful code repair; greater size/reasoning does not consistently remove over-editing. | Evaluate unintended semantic changes separately from successful requested changes. Code-repair findings require testing for prompt edits. |
| [Clarification Is Not Correction, September 2026](https://arxiv.org/abs/2609.25337) | Controlled Gemini tasks show order effects and persistence of early interpretations despite later clarification. | Test explicit supersession and rebuild current intent from resolved state. This is not a universal claim about all models. |
| [MT-InfoSeek, Microsoft Research, August 2026](https://www.microsoft.com/en-us/research/publication/do-llms-know-what-to-ask-and-when-evaluating-multi-turn-information-seeking/) | Separates acquiring sufficient information from generating a correct answer. | Isolate target discovery, requirement extraction, editing, and downstream interpretation. |
| [SEQUOR, May 2026](https://arxiv.org/abs/2605.06353) | Constraint adherence deteriorates in longer simulated conversations, including changing constraints. | Test sequences of additions, replacements, retractions, and rejected changes. |
| [Trust but Verify, May 2026](https://arxiv.org/abs/2605.25133) | Prover–verifier selection can distinguish reliability, but weaker/domain-mismatched pairs can lose or invert the signal. | Evaluate an independent omission checker as an experiment, including false rejections and cost. No semantic guarantee follows. |
| [PriceCheck, September 2026](https://arxiv.org/abs/2609.37493) | Evaluates calibrated error among accepted answers alongside coverage and checking cost. | Report unsafe-offer rate together with faithful completion and deferral. Mathematical-answer results do not certify editing safety. |
| [Microsoft DELEGATE-52 clarification, May 2026](https://www.microsoft.com/en-us/research/blog/further-notes-on-our-recent-research-on-ai-delegation-and-long-horizon-reliability/) | Repeated transformations can degrade artifact fidelity; authors distinguish the stress test from overall task success and user outcomes. | Add sequential preservation tests, then measure downstream success separately. |
| [Anthropic evaluation guidance, January 2026](https://www.anthropic.com/research/demystifying-evals-for-ai-agents) | Distinguishes transcripts from environment outcomes; recommends repeated trials and appropriate code, model, and human graders. | Preserve mechanical tests while adding independent semantic and downstream outcome evaluation. |

## 4. Proposed research sequence

### Study 1 — Locate the freeform bottleneck

Hold the model, visible context, contracts, runtime, budgets, and output validation constant. Compare current inputs against independently supplied correct target regions, complete requirement inventories, and both together. These are diagnostic oracle conditions, never autonomous-success claims. Account for the extra information in interpretation.

Measure target correctness, requirement coverage, faithful complete proposals, wrong scope, unnecessary questions, rejection, timeouts, and latency. Include fresh families as well as separately reported regressions. A stronger model under matched conditions can serve as an additional diagnostic reference; do not infer a model ranking from one generation.

### Study 2 — Complete freeform change accounting

Instrument the staged path with source-linked requirement dispositions. Begin in diagnostic mode. Compare detections against human-reviewed omissions and faithful paraphrases before enabling any new rejection gate. Do not simply activate the older lexical classifier/ledger and assume it improves quality; previous configurations had substantial rejection and latency costs.

### Study 3 — Mixed captured choices and comments

Compare the current mixed-reply freeform path with preserving the canonical choice and proposing separately scoped comment effects. Include comments that add restrictions, contradict the selected option, change the goal, and annotate keep-original. Explicit contradiction must remain visible until resolved. Keep canonical selection, raw text, producer metadata, and bindings intact. This requires fresh live-model evaluation; old dispatcher replay is insufficient.

### Study 4 — Semantic checking and operational value

Use faithful/defective candidate pairs to test omission and wrong-scope detection. Calibrate checks using development data and evaluate on a separate family-held-out set. Report correct rejections, unsafe acceptance, false rejection, useful coverage, and added latency together.

Then compare unchanged requests, preservation controls, and proposed final requests on sandbox downstream tasks and a small independently reviewed interaction study. Score both final task behavior and user correction effort. A shorter or better-looking prompt alone is not product success.

## 5. Proposed test families

| Family | Example | Required observation |
|---|---|---|
| Compound correction | “Button green, footer black, and no dependencies.” | Both changes and restriction retained; unrelated task preserved. |
| Referential preservation | “Put it in Settings” after a Save profile question. | Named feature survives; unresolved referents remain visible. |
| Restriction scope | “Customer only; no SMS or staff broadcast.” | Recipient exclusivity and both prohibitions survive. |
| Goal replacement | “Explain the cache only; do not change code.” | Prior implementation goal superseded explicitly; prohibition retained. |
| Repeated targets | Blue button and blue footer; select footer only. | Correct identity and scope; button unchanged. |
| Mixed event | Select visual-only; type “Also preserve the API.” | Canonical option meaning and full comment retained. |
| Contradictory mixed event | Select email-only; comment “Actually use SMS instead.” | Explicit unresolved conflict or confirmed supersession; no silent precedence guess. |
| Retraction/reintroduction | Withdraw a constraint, later request it again. | Current state follows confirmed events without reviving unrelated history. |
| Independent edits | Change two disjoint requirements in opposite orders. | Equivalent final state only where independence is established. |
| Necessary vs unnecessary clarification | Unknown recipient versus recipient explicitly supplied/delegated. | Useful question in the first; no reopening of the settled decision in the second. |
| Surface stability | Reviewed meaning-equivalent capitalization/punctuation variants. | Measure route and semantic stability; exclude meaning-changing code/quotation variants. |
| Sequential preservation | Several confirmed edits, one rejection, then a goal revision. | Correct active state after every step and faithful downstream interpretation. |

For each family, include a faithful candidate, an omission, a wrong target, an unauthorized replacement, and a legitimate intentional revision where applicable. Include invalid/stale cases separately from semantic failures. Keep model-induced errors and harness/infrastructure failures visible with appropriate classifications.

## 6. Evaluation and integration boundaries

- Obtain intended-behavior judgments before output review; allow multiple valid expressions and unresolved reviewer disagreements.
- Use independent reference/candidate review. A human-calibrated model judge may assist; it should not silently define ground truth.
- Freeze cases, contracts, examples, model configuration, and analysis criteria before generation. Keep research/exposed regressions distinct from untouched family holdouts.
- Use repeated independent trials for variability measurement. Do not add production semantic retries or select the best response without separately evaluating that policy and its cost.
- Record hashes, exact visible model inputs, outputs, route/dispatch decisions, budgets, tokens, timeouts, and code versions. Reference effects and labels must not leak into non-oracle model inputs.
- Keep initial triage, initial discovery, correction questions, rewrite fidelity, and delivery metrics separate.
- Report failed, deferred, unresolved, and rejected cases in denominators. Lower output volume alone is not progress.
- Keep the Julia SDK contract repair separate from routing/threshold experiments.
- Preserve the dirty worktree and existing evidence. This report authorizes no deployment, installation, app restart, production routing change, model download/training, commit, merge, or push.

## Recommended next decision

Run the failure-localization study first. It should establish whether the next engineering investment belongs in target discovery, requirement extraction, edit generation, or integration context. Use the cross-industry mechanisms to produce falsifiable tests and controlled candidates, while retaining uncertainty about natural-language meaning.
