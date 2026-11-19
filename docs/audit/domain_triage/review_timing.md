# Timing of clarification on evolving drafts

Four frozen typing prefixes each branch into three continuations. At the simulated Review click the author has settled the target decision, delegated it, or explicitly reserved it. Across 12 paths there are 16 distinct snapshots. The replay holds the automatic one-question Granite contract fixed even at Review to isolate scheduling. “First” means show at most one candidate per path: evaluate at the first CLARIFY snapshot; if it returns none, try again on the next CLARIFY snapshot. The review-only policy waits and checks once at the click. Both policies share model results for identical snapshots. A third control waits until Review and retains the S1 CLARIFY gate.

| Policy | Typing prefixes with early question | Settled/delegated paths receiving any question | Open paths receiving the right decision question by review |
| --- | ---: | ---: | ---: |
| First nonempty S1-triggered candidate | 3/4 | 6/8 | 1/4 |
| Wait until Review, retain S1 gate | 0/4 | 4/8 | 3/4 |
| Wait until Review, check every final draft | 0/4 | 5/8 | 3/4 |

All four prefixes triggered S1 review. The API rename produced no early question. Each other prefix produced a premature question that failed to address the target decision. These three prefixes represent six early interruptions among eight paths that later settled or delegated the decision (nine among all twelve paths, counting repeated prefixes). With review-only discovery, the alias, hints-versus-worked-solution and narrator questions addressed the open decision. The archive answer named the storage alternatives but called their pending selection a possible contradiction. Strict scoring counts this as an unresolved decision; a topic-only grading rule would count it as surfaced. The only final snapshot skipped by S1 was the delegated archive decision; checking it unconditionally asked the user to choose the storage policy they had just delegated.

This experiment supports testing deferred presentation with the existing gate. It does not establish that people were interrupted or answered these questions. The three missing decisions under the first policy depend on a one-question budget: a policy allowed to interrupt early and ask again at Review could recover the same final decisions while charging users for additional questions. Prefixes were deliberately unfinished tasks sampled at one sentence boundary, not measured typing pauses. Single evaluator, one decode per snapshot, four synthetic prompt families, fixed CODE_EDITOR context, no live UI. All 16 discovery replies passed the production parser. No production change.

Replay with build/runtime/julia-python/bin/python3 tools/probe_review_timing.py from the checkout root. The adjacent JSON stores the prompts, S1 routes, actual responses, model configuration, parser results and timings.
