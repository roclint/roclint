# Few-shot conflict scoring and companion behavior

Audited checkout `d4d3baa` on macOS, November 19, 2026. Ran the checkout's Julia worker on CPU with the pinned local S1 weights. Each of nine prompts was scored once with the benchmark's standard heads and once with the desktop's extended heads. The probe supplied prompt text alone, modeling the case with no workspace facts available. The routes below apply the desktop's default per-category thresholds to the measured scores. These are classifier probes, not recorded UI sessions.

| Input | Raw conflict | Extended effective conflict | Desktop route | Categories crossing threshold |
| --- | ---: | ---: | --- | --- |
| Original benchmark sentiment prompt | .9991 | .9991 | Clarify | Rule, conflict |
| Post-merge benchmark sentiment prompt | .8850 | .8850 | Clarify | Action, conflict |
| Short sentiment control | .0024 | .0024 | Original | None |
| Parser instruction alone | .3719 | .9896 | Clarify | Target, conflict |
| Parser instruction + old/new labels only | .5388 | .5388 | UncertainBuffer | None |
| Parser instruction + plain before/after example | .4013 | .8091 | Clarify | Rule, conflict |
| Same example wrapped in XML | .4039 | .4039 | Original | None |
| Parser instruction + fenced code | .0766 | .0766 | Original | None |
| Parser instruction + contradictory requirement | .0427 | .8585 | Clarify | Rule, conflict |

The two saved benchmark scripts use different prompt texts for the same case name. Both saved conflict scores reproduced to four decimals. The report's identical-score claim cannot establish pre/post stability. An identical-input comparison would require running the same prompt and configuration on both revisions. On the original prompt, the rule score of .9999 would independently trigger clarification.

The desktop sends workspace metadata and enables extended heads. Effective conflict takes the maximum of raw conflict and two gated scores for prohibition-only requests and security bypass. In the plain code example the action score was .0294, which disables the prohibition-only contribution. The .8091 conflict therefore came from the security-bypass head. The same head pushed the instruction alone to .9896. These cases show a genuine false positive and a competing mechanism. They establish neither a population error rate nor an attention-distance cause. XML and fenced variants passed in this sample.

## Prompt inputs

The two benchmark texts are the “Few-Shot Sentiment Classifier” entries in the local corpus scripts `original_benchmark_script.py` and `evaluate_post_merge.py`. The short control was “Classify each review as POSITIVE or NEGATIVE. Return only its label.\nExamples: loved it -> POSITIVE; hated it -> NEGATIVE.\nReview: wonderful.” Here and below, \n denotes a newline.

All parser cases start with `In parser.py, change parse("") to return None.` Append the following for each variant.

~~~text
Labels (newline before suffix)
Old Code -> Error. New Code -> Fixed.

Plain (newline before suffix)
Old Code -> Error: parse("") raises ValueError.
New Code -> Fixed: parse("") returns None.

XML uses the same plain suffix between <example> and </example> on separate lines.

Fenced (newline before suffix)
Before:
```python
if not s: raise ValueError()
```
After:
```python
if not s: return None
```

Contradictory (space before suffix)
For the same input, always raise ValueError instead of returning None.
~~~

## What the user sees

The full-prompt triage callback routes a clarification to discovery. The ambient badge reads word-replacement readiness and surface visibility separately. Its 28-pixel navy, non-focusing indicator uses a mint dot for a ready replacement and is usually placed beside the field. A setting can hide it until a suggestion exists. Activating an eligible suggestion requests the word companion. A conflict score has no dedicated amber corner indicator. The pre-merge first parent's FloatingCard.qml defines a floating window, 620 × 520 by default in ambient mode. Historical Windows screenshots described a large unsolicited checking card. Neither establishes a full-screen modal triggered by this specific classifier result.

Five targeted CTest suites passed (S1 adapter, dual mode, host controller, product alignment, upstream compatibility). The host controller needed a rerun with localhost access for its test server. Dual mode exercises typing, badge visibility and closed cards with a simulated accessibility bridge. Live VS Code focus and typing were outside this run.

Implementation references: app/runtime/julia_worker.py (triage), app/host/HostControllerTriage.cpp (routing), app/host/HostController.cpp (isAmbientBadgeVisible and activateAmbientIndicator), app/qml/shell/AmbientBadge.qml, tests/viewmodel/test_dual_mode.cpp.
