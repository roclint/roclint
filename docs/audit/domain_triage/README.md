# Domain-aware routing benchmark

**Retain current routing.** Neither candidate changed a single S1 route across the 96 held-out draft–host combinations. The task question added a median 38 ms of CPU inference per held-out draft. Neither candidate meets the criterion for advancement.

| Domain | Unnecessary review candidates on clear drafts | Genuine decisions missed on ask drafts |
|---|---:|---:|
| Coding | 7/12 | 6/12 |
| Creative writing | 9/12 | 8/12 |
| Enterprise data | 4/12 | 3/12 |
| Tutoring | 4/12 | 7/12 |
| All domains, each policy | 24/48 | 24/48 |

Each denominator of 12 represents four distinct drafts observed in three hosts. The 69 discovery calls produced 69 parser-valid results. The remaining 27 observations took the original route. There were zero uncertainty-buffer routes, model errors or invalid discovery outputs on the held-out set.

**Where the 24 misses occurred:** 15 required decisions took the original route, four reached discovery and returned no issue, and five elicited an off-target response. The task classifier did choose different settings across drafts (strict for 20, relaxed for 11, default for one). However, 22 of the 32 drafts already crossed a RULE or RULE_CONFLICT threshold. Only one draft had either ACTION or TARGET in the interval from 0.65 inclusive to 0.85 exclusive, and it independently crossed the other thresholds. These results explain this tie without establishing that the task label or thresholds would never matter elsewhere.

**Host sensitivity occurred downstream.** S1 routes stayed stable across hosts for every draft, while 11 of 32 drafts changed graded final disposition across host contexts. For the fictional artist profile, code-editor discovery surfaced the actual conflict between anonymity and publishing the artist's full name. Browser and document discovery both asked about prose style. These are within-run differences with one decode per host, not an estimate of repeatability or a live UI measurement.

## Design and replay

The frozen corpus has 20 matched families (40 distinct drafts). Each of four domains contributes one development family and four held-out families. This allocation reconciles the requested totals of eight development drafts and 32 held-out drafts. Each held-out draft is run in a code editor, browser and document context. These 96 observations cover 32 drafts in 16 families. Eight code-editor development observations are retained separately. Spoken output appears across all four domains.

The context ID is a simulated input to the production automatic discovery prompt. Workspace evidence consists of the fixture's file and symbol names; the harness does not read source contents. Julia's task question sees only the draft and that workspace evidence. All arms reuse the same extended S1 evidence. A native fixture helper seals it, applies the default and experimental production routing functions, and passes the evidence into automatic discovery. For each draft and host that any arm sends to discovery, all qualifying arms share a single Granite result. The current discovery instructions, schema and production parser are used with a one-question limit. Only the routing policy varies across arms.

The current ACTION and TARGET thresholds are 0.70. The host policy uses 0.65 for code editors, 0.85 for documents, and 0.70 otherwise. The task policy uses a separate Julia finite-choice decision with the same three threshold levels for implementation/data changes, explanation/composition/transformation, and mixed work respectively (0.65, 0.85 and 0.70). Both policies retain RULE at 0.70, RULE_CONFLICT at 0.60, and a 0.08 uncertainty band. The run records the exact task question and answers, full weights identity, model configuration, corpus digest, scores and wall-clock times. Task inference is paid once per draft only by the task-aware policy. The thresholds and task question were specified before model execution and held fixed throughout.

For grading, deduplicate identical review content for each case and order opaque response IDs by hash. The grading sheet exposes the frozen draft, intended decision, acceptable paraphrases and actual model output. It omits hosts, policy identities, thresholds and routes. An on-target response must surface the named decision or accurately describe the conflict, including when Granite labels it as a creative choice. Asking a user to choose a delegated detail or supply the result of the requested work counts as unnecessary. A general question unrelated to a required decision counts as a miss. Nonblocking advisories and original routes produce no discovery question. Runtime and parser errors are recorded separately and cannot earn credit for surfacing a required decision.

From the checkout root, with the existing Release build and local models:

```sh
cmake --build build --target roclint_granite_contracts roclint_granite_worker
build/runtime/julia-python/bin/python3 tools/benchmark_domain_triage.py --check
build/runtime/julia-python/bin/python3 tools/benchmark_domain_triage.py --out build/domain-triage/receipts.json
python3 tools/score_domain_triage.py --blind --receipts build/domain-triage/receipts.json > build/domain-triage/grading-sheet.json
```

Grade the sheet before joining policy identities. Save an object mapping its keys to on_target or off_target for a required decision, unnecessary for an unjustified question on a clear draft, or optional for a genuinely non-interrupting proposal. The archived grades can be reused only when the sets of response IDs match exactly.

```sh
python3 tools/score_domain_triage.py --receipts build/domain-triage/receipts.json --grades build/domain-triage/grades.json --out build/domain-triage
```

The scorer also runs without arguments to reproduce the archived summaries. Outcome rows retain individual routes and decisions; decisions.json lists every policy change against current routing. The by_model directory breaks the same evidence down by domain. The comparison is conditional on the specified model stack, simulated hosts, synthetic fixtures and a single grader. Here an interruption is an eligible question candidate, not an observed popup or lost keystroke.
