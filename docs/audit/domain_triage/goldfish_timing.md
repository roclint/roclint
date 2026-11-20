# Goldfish and clarification timing

On the 16 saved discovery outputs from the [timing pilot](review_timing.md), adding the Goldfish discovery audit and repair loop changed the form of one useful question. It left every review outcome in the same category.

| Measure under the pilot’s S1 gate | Before Goldfish | After Goldfish |
| --- | ---: | ---: |
| Unfinished drafts eligible for a premature question if reviewed immediately | 3/4 | 3/4 |
| Settled or delegated final drafts eligible for an unnecessary question | 2/8 | 2/8 |
| Settled or delegated final drafts eligible for optional help | 2/8 | 2/8 |
| Open final drafts that surfaced the owner’s actual decision as a question | 3/4 | 3/4 |

The earlier pilot counted all four unnecessary candidates together. In the host policy, the settled lesson and narrator drafts qualified for questions, while their delegated variants qualified for optional help. The delegated archive draft was routed ORIGINAL and skipped under the gate. Forcing its saved discovery through the auditor produced a pass, but the routed snapshot rejected the automatic discovery both before and after audit.

The API rename was the only repair. Goldfish challenged the supporting evidence and distinction between options, pruned the choice group, and passed the remaining free-text question about keeping the alias. The user would still have a decision to answer. In contrast, the archive owner explicitly had a storage choice to make. Goldfish passed discovery’s claim that this choice was a contradiction. Host policy marked it for manual review with an active conflict. Strict grading counts that as a miss.

All 16 final audits passed. Seventeen native audit calls ran, including one re-audit and zero regeneration calls. Median added model time per snapshot was 5.0 seconds. This measures model time on the replay machine.

## Method and scope

The replay parsed the original saved replies and ran the C++ Goldfish repair loop with one retry and a local Q8_0 Granite worker. It rebuilt S1 evidence for each frozen prompt, evaluated routed snapshots with the same automatic review trigger, and used production materiality and intervention policies before and after repair. The fixture mirrors the host’s empty-result fallback and final review-outcome mapping. No fixture triggered the empty-result fallback or qualified for the host’s quoted mechanical-edit exception.

The run kept worker operation deadlines and omitted the shared host deadline to isolate the audit’s effect on content. Discovery time plus audit time exceeded 20 seconds for two snapshots, so eligibility here does not predict whether a timed host run would deliver every result. Four synthetic families, one model run, previously graded target decisions, no independent rater and no live editor. The typing prefixes are shared across continuations.

Run `build/runtime/julia-python/bin/python3 tools/probe_goldfish_timing.py` from the checkout root after building `roclint_granite_contracts`. The [replay receipts](goldfish_timing.json) preserve the worker replies, timings, resulting questions, and before/after policy outcomes. The [original receipts](review_timing.json) contain the frozen discovery text and runtime manifest.
