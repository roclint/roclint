# Decision ownership on matched prompts

Keep the current discovery instruction. Adding an explicit ownership check to the clinician-inspired instruction made Roclint quieter on delegated and settled choices, but it surfaced fewer decisions the user reserved. Each method answered two complete families correctly. The frozen advancement rule required more complete families than both comparators and no additional reserved misses.

| Discovery instruction | Complete families | Correctly asked on reserved choice | Quiet on delegated choice | Quiet on settled choice |
| --- | ---: | ---: | ---: | ---: |
| Current | 2/12 | 6/12 | 2/12 | 7/12 |
| Clinician-inspired | 2/12 | 4/12 | 5/12 | 8/12 |
| Clinician-inspired plus ownership check | 2/12 | 2/12 | 8/12 | 10/12 |

For the build manifest, the user explicitly kept the choice between ordering by package name and ordering by installation order. Current discovery asked which ordering to use. Both variants returned no question. The ownership variant handled every version of the thumbnail and map-cache tasks correctly.

The run completed 108 scheduled calls across 36 synthetic drafts. Each family held the task and alternatives fixed and varied a single sentence. Every method used the same local Q8_0 Granite runtime and one decode per draft. The baseline used the automatic discovery instruction. The grading sheet shuffled responses and hid methods and call order. A valid empty reply was required for the two quiet conditions. A reserved decision required a direct or clarification question accurately eliciting the target. Optional offers, invented alternatives, unrelated questions and invalid replies failed. One evaluator judged semantic fit before joining grades to method identities. For example, the catalog question that introduced a combination of the two layouts failed the strict rule even though its option buttons listed the two original choices.

Ten calls exceeded the operation deadline and counted as failures, including eight in the podcast family and two in the worksheet family. The interruption coincided with an unfinished run; the receipts do not establish the cause of the timeouts. Excluding both affected families leaves each method at 2/10 complete families and preserves the reserved-choice ordering of 6/10, 4/10 and 2/10. One run per condition, one evaluator, and discovery-stage outputs only. Live editor behavior was outside the comparison.

[Frozen tasks and method instructions](../../../tools/probe_decision_ownership.py) · [Raw receipts](decision_ownership.json) · [Frozen blind grades keyed by response ID](decision_ownership_grades.json)
