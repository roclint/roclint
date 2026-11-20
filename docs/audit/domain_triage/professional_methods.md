# Asking only when the answer matters

The clinician-inspired instruction produced the fewest unnecessary suggestions in this pilot. All three methods surfaced two of the three owner decisions, but they missed different decisions.

| Discovery instruction | Unnecessary questions | Unnecessary optional suggestions | Owner decision surfaced |
| --- | ---: | ---: | ---: |
| Current | 5/9 | 2/9 | 2/3 |
| Check the evidence like a code reviewer | 3/9 | 1/9 | 2/3 |
| Consider whether two answers would change the action | 2/9 | 0/9 | 2/3 |

Three synthetic task families each had four fresh drafts. One reserved a decision for the owner, one supplied the answer, one delegated it, and one asked for an explanation of current behavior. Current behavior was supplied in each draft. Every method saw exactly the same text. The baseline used the existing automatic discovery instruction. The two variants added a single instruction before the request. The script froze prompts, targets and scoring criteria before inference and rotated method order. It ran one local Q8_0 Granite decode per draft and method. All 36 replies passed the production parser.

| Open decision | Current | Reviewer | Clinician-inspired |
| --- | --- | --- | --- |
| Reject the incoming job or evict the oldest | Surfaced | Surfaced, with imprecise sign-off wording | Surfaced |
| Floor caption timestamps or round to nearest second | Missed | Surfaced | Surfaced |
| Print organization or workshop room on badge line two | Surfaced | Missed | Missed |

The reviewer question on queue admission introduced “pending sign-off” on the rejection option. It gets credit for surfacing the named choice because both actions remain identifiable. Requiring an unqualified statement of both alternatives would lower the reviewer score to 1/3.

On the nine quiet controls, a clarification group or a direct question counted as an unnecessary question. A creative choice group counted separately as optional. The clinician-inspired method’s two unnecessary questions both came from the queue family. It asked operations to make a choice already delegated and asked what happens to an arriving job even though the prompt supplied that fact. The reviewer method returned suggestions on all three delegated choices.

These are discovery outputs. S1 routing, Goldfish, editor presentation and human interruptions were outside the experiment. One evaluator graded three deliberately matched families, with a single model and no repeat decodes. The test measures these short instructions, not professional performance. A larger unseen holdout should include cases where the author reserves a choice that seems easy for the model to make.

[Replay script and frozen inputs](../../../tools/probe_professional_methods.py) · [Worker receipts and raw replies](professional_methods.json)
