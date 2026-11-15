# Output-quality smoke check — November 14, 2026

## Verdict

The restored AST index does not establish improved output usefulness. This
small current-build check still finds missed refinements, awkward replacements,
malformed clarification output, and useful creative choices rejected because
their evidence violates the contract. There are promising model capabilities,
but the model/contract/policy combination is not consistently delivering them.

## Follow-up clarification

The direct discovery fixture used the no-argument schema. The actual HUD already
used a prompt-bound quotation schema. Invalid source quotations in these direct
results do not show that the HUD grammar accepted them. The subsequent
[context-quality remediation](context_quality_remediation_2026-11-14.md) aligns
the contract tool with the HUD schema and distinguishes proposed choices from
quoted facts. This report remains the record of the earlier diagnostic run.

## Scope

Ten synthetic requests used the installed native helper, installed pinned Q8_0
weights, Vulkan configuration, current production prompt builders and response
parsers. Thinking was explicitly disabled per request, matching the current
default; all responses reported zero thinking tokens. No deadline bypass,
development no-thinking override, model download or prompt tuning was used.

This is a direct-helper diagnostic, not a complete HUD → Julia → discovery →
Goldfish → approval run. Rewrite workspaces were synthetic and indexed through
the production grounding/context implementation. This is not held-out acceptance
evidence, a statistical quality score, or a controlled before/after experiment.
No user draft was edited or sent, and no proposal was approved or inserted.

Evidence: `D:\codex-evidence\roclint-output-check-20261114`. `contracts.json`
contains the actual generated contracts; `results.json` contains raw complete
responses and timings. `word-validation.log` and `contract-validation.json`
record production parser/policy results. The owned evaluation helper shut down
afterward. No implementation files were changed during this assessment.

## Observations

| Input / task | Actual result | Assessment |
|---|---|---|
| Dashboard feeling **faster** on slow laptops | No candidate and no question | Missed a useful distinction for the stated product goal |
| **Warmer** onboarding copy | No candidate and no question | No useful assistance in this sample |
| Exactly **500 milliseconds** | No candidate and no question | Appropriate preservation of an explicit value/unit |
| **Atmospheric** plant-photo treatment | Suggested `a clear, airy atmosphere suitable for observation` | Insertion produces `Apply an a clear, airy atmosphere suitable for observation treatment…`; fails grammar and usefulness |
| **Compact** typography | Suggested `concise and simple font style` | Produces awkward `Use more concise and simple font style typography…`; does not resolve the typography distinction |
| Inline **cinematic** | No candidate and no question | Missed refinement/clarification opportunity |
| Compact-text music app mixing 1990s design and Revolver-era artwork | Empty choices and an empty conflict description | Production parser rejects: `Field 'description' must not be blank` |
| Cinematic potted-plant shot, deeper discovery | Composition and lighting alternatives | Useful raw directions, but invalid evidence/materiality; policy rejects them |
| Change `demo::Queue::push`, preserving signature and forbidding logging | Original input returned unchanged | Correct target verified and constraints retained; no demonstrated clarity improvement; the original was already specific |
| Make missing `demo::Queue::flush` non-blocking | Original input returned unchanged | Missing entity was not extracted for lookup; raw rewrite offers no resolution |

All ten requests completed at the transport level. All six word responses passed
the production word parser, including the two awkward replacements. That
demonstrates why JSON/span validity is not sufficient for sentence quality.
Both rewrite responses parsed and passed structural validation. Those results
are mechanical checks, not semantic-usefulness passes.

## Useful generation rejected by its own evidence contract

The plant discovery offered these concrete directions:

- Low-angle, centered framing with shallow depth of field versus an overhead
  wide shot with surrounding context.
- Soft diffused light versus directional side light with subtle fill.

However, its `evidence` fields contained generated explanations such as
`Low angle emphasizes height and form; shallow depth of field isolates the plant.`
Those are not quotations from the user's prompt. The alternatives were marked
material, and production validation reported four ungrounded evidence entries
and `UNGROUNDED_MATERIALITY`. Rejecting fabricated source evidence is correct.
The next change should represent proposed creative directions honestly and bind
their source references correctly, rather than weakening the evidence checks.

The model returned the music-app response as `options_generated` with no actual
choices, no question, and `conflicts:[{"description":"","blocking":false}]`.
The parser correctly rejected that unusable object.

## Intent extraction still pollutes AST context

The Queue fixture declares `bool demo::Queue::push(int item)`. Its actual code
target was Verified, but the extractor also submitted the prose constraint
`public signature unchanged and do not add logging` as an entity. That generated
a misleading `NOT_FOUND_IN_SCOPE` record and consumed some of the 350-byte XML
budget.

In the `Make demo::Queue::flush non-blocking` case, the function target was absent
from the extracted evidence entirely. Only `public signature unchanged` was
submitted as a missing target. No false Verified binding occurred, but the
pipeline failed to ask the index about the entity that mattered. A healthy
index cannot compensate for that extraction failure.

## Responsiveness

| Operation | Measured wall time |
|---|---:|
| Word no-change responses | 2.18–2.59 s |
| Word replacement responses | 9.56–10.43 s |
| Music-app discovery | 8.29 s |
| Plant-shot discovery | 36.71 s |
| Rewrite samples | 3.73 / 5.95 s |

These are one-run device measurements, not performance gates. The default
thinking-off host analysis-group budget is 20 seconds, while the native
analysis operation permits 45 seconds. The useful 36.71-second direct response
exceeds that group budget. This identifies a budget/response-size concern; the
actual HUD route was not run here, so an end-to-end timeout is not claimed.

## Next work indicated by this check

1. Extract actual code entities while retaining general preservation/negative
   constraints as constraints; cover ordinary phrasing such as `Make …`.
2. Make creative choices, source quotations and proposed assumptions distinct
   in the model contract, with bounded recovery for unusable model output.
3. Validate a word replacement in its complete sentence, not just as a valid
   span/JSON object. Match question size and generation work to the host budget.

The unchanged clear rewrite and the explicit-unit no-op should remain valid
outcomes. Useful assistance should be measured by resolved meaning and faithful
edits, not by forcing every prompt to change or merely counting generated tokens.
