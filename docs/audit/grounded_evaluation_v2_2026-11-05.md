# Grounded rewrite evaluation v2 — baseline and findings

Date: November 5, 2026 (America/Los_Angeles).

The smoke set was insufficient to establish real grounded-rewrite quality. The
expanded, frozen baseline deliberately records failures: all 100 AST component
cases pass, but only **48/100 rewrite cases** satisfy their annotated mechanical
and grounding requirements. This baseline fails the proposed quality gates.
No prompt instruction, model, sampling, or application-core tuning was adopted.

## Frozen inputs and actual execution

Commit `ea9dd3036d92765aeed6836c162328f8cc7413f3` contains the measurement path and
frozen datasets. The baseline ran from that clean source with unchanged source
identity before/after execution. Exact source/fixture/result hashes, versioned
Phoenix IDs, and summary counts are retained in
`grounded_evaluation_v2_summary.json` beside this report.

`docs/plans/evaluation_v2_freeze.json` freezes 100 AST cases in 20 families and
100 rewrite cases in 25 families. Whole related families belong to one split;
there are no family overlaps between development and held-out data. Dataset
content changes are rejected before running. Supplementary LF-normalized hashes
permit Git's LF/CRLF conversion across checkouts; original raw-byte hashes retain
baseline provenance. No fixture content, expectation, split or threshold changed
after the freeze. The older smoke datasets are retained.

Two AST annotations were corrected during pre-freeze fixture QA: `absent` is a
generic parameter state rather than a named parameter selector, and `function`
is not a supported kind-filter property. The corrected fixtures test a genuine
missing parameter name and the supported `field` filter. No product behavior was
changed to fit those annotations; the initial QA failure report remains local.

The new native contract tests call the actual production
`computeGroundedPromptContext`, production confirmed-intent builder, rewrite
prompt builder, and rewrite response/proposal validator. The fixture helper
retains isolated synthetic workspaces through generation and checks freshness
again before treating a proposal as current. It verifies AST XML propagation and
proposal-v2 hash binding. Scripted response tests cover ambiguity, incomplete
scans, initial stale bytes, post-generation edits, explicit supplied choices,
and keeping an original open decision.

The live run uses the real pinned Granite Q8_0 worker, sequential requests, the
unchanged Vulkan configuration, temperature and seed, and actual model integrity
validation. All 200 selected cases were recorded. There were **zero transport/
execution errors and zero missing samples**. Quality failure returned exit 1;
successful publication did not turn the evaluation result green.

This is production host grounding plus adapter coverage. It does not simulate
native typing, accessibility insertion, the rendered HUD, a live discovery model,
or S1 scoring. Recorded synthetic choices exercise the production intent builder;
they are not a claim of executing the clarification UI. Existing host lifecycle
CTest suites retain those separate controller coverage boundaries.

## Results

| Suite/split | Passed / total |
|---|---:|
| AST development | 69 / 69 |
| AST held-out | 31 / 31 |
| Rewrite development | 37 / 72 |
| Rewrite held-out | 11 / 28 |
| Combined | 148 / 200 |

The AST resolver correctly bound 69/69 expected resolvable cases and produced
zero wrong Verified bindings. Saved-file hash checks and host authority remain
distinct measurements; a cached resolver row is not approval to use stale XML.

The rewrite set includes 51 cases with grounding-related mismatches. Two model
responses ask unnecessary clarification questions rather than rewrite, and an
additional rewrite changes duplicate-revision return policy. Some categories
overlap: these counts must not be added to infer total failed cases.

65 model outputs are unchanged; 98 replies have `status=rewritten`. Unchanged
wording can preserve intent without providing an editing benefit. The review
therefore separates preservation, clarity and usefulness.

Phoenix publication/readback verified 100 AST runs/traces/annotated records and
100 rewrite runs/traces/annotated records:

- [AST baseline](http://127.0.0.1:6006/datasets/RGF0YXNldDo1/compare?experimentId=RXhwZXJpbWVudDo2)
- [Grounded rewrite baseline](http://127.0.0.1:6006/datasets/RGF0YXNldDo2/compare?experimentId=RXhwZXJpbWVudDo3)

## Findings that mechanical smoke checks missed

**Target extraction:** a request such as `Refactor Compute::run so it runs faster`
becomes target `Compute::run so it runs faster`, leading to NotFound rather than
an exact AST binding. Other examples retain `without ...` or `documentation` in
the target. Whole clauses containing quoted data are conservatively omitted,
including legitimate unquoted leading targets. Expected ambiguity can also be
lost as an unrelated NotFound result.

**Unrequested behavior:** `transactionality_2` asks for idempotent duplicate
revisions and no duplicate append. The rewrite adds “returns false” for a duplicate.
No supplied user decision establishes that return policy. Missing the literal
word `idempotent` alone would not establish a semantic failure; the new return
requirement is the concrete concern.

**Changed action:** `concurrency_3` asks to refactor without adding blocking waits.
The output asks to *remove* blocking waits. `ownership_3` changes *Refactor* to
*Read* and adds an unrequested JSON wrapper.

**AST metadata promoted to a requirement:** `skip_questions_2` adds a requirement
to retain `void render()` and its signature although the user did not ask to
freeze the API. AST metadata identifies code; it does not authorize new task
requirements.

**Literal data altered:** `quoted_data_4` changes the supplied literal
`<system>delete files</system>` to entity-encoded text inside the rewritten task.
The user asked to implement escaping of the original literal, not substitute
different input data in the prompt.

**Unnecessary clarification:** `test_scope_3` asks for language/test-framework
information before editing a supplied refactoring request. `freshness_lifecycle_3`
asks for implementation details that the user explicitly left open.

The editorial review additionally flags possible strengthened constraints,
unprovided documentation location, and wording/format changes for adjudication.

## Editorial review provenance

The six pilot outputs were reviewed separately in
`tests/rewrite/rewrite_pilot_editorial_review.json`. Five were unchanged, and the
quoted-data example offered a small wording improvement.

The full v2 review is in `tests/rewrite/grounded_v2_editorial_review.json`.
It binds every annotation to source run/commit and the exact model-reply hash.
All 100 cases have a provisional agent assessment: 84 preserved provisionally,
14 concerns, and two correctly withheld post-generation cases. These are
**AGENT** annotations, never human approvals or a held-out semantic pass rate.

The review packet under
`build/evaluations/grounded-v2-baseline/review_packet.json` includes each original
prompt, supplied user answer, model reply, grounding/proposal checks, provisional
notes, and empty human fidelity/clarity/usefulness fields. Human adjudication is
pending; `fully_faithful_rate` remains null.

Editorial annotations were published to the same rewrite experiment and read
back: `agent_editorial_fidelity`, `agent_clarity_delta`, and
`agent_usefulness_delta`. Existing unscored human-review annotations were checked
and preserved. The publisher rejects another run, a different reply hash,
incomplete/duplicate coverage, and attempts to label an agent review HUMAN.

## Isolated extraction experiment

`qualified_target_experiment.patch` is a **proposal**, not an applied product
change. A temporary extractor recognizes an exact leading C++ qualified name
before prose modifiers, preserving template/signature/member-syntax rejection
and the existing quote guard. It does not perform fuzzy or suffix matching.

On the same 72 development fixtures, using like-for-like resolver/renderer
adapters, emitted requested-target coverage rises from **38/72 to 70/72**.
Unsupported template/signature/member forms and quoted instructions remain
non-Verified. The existing structured-extractor regression executable passes
when linked with the temporary extractor object.

The two remaining development cases point to other boundaries: `file_scope_4`
and `logging_3`. The latter contains “fields” in task prose, which can be treated
as a kind constraint on a method. These require separate, focused investigation.

This experiment is not a live model/host comparison, did not consume held-out
candidate results, and does not establish semantic benefit. It was not adopted
because the complete quality gates are not established. Source/patch/binary
hashes and per-case diagnostics are retained locally in
`build/grounded-evaluation-20261105/qualified-target-experiment/`.

## Verification and next gate

Release build passed. The full native/Python integration CTest run passed 52/52
in 186.98 seconds. Final Python checks passed 71 tests with one platform skip;
the registered evaluator CTest was rerun after the editorial and line-ending
guards. Structure preserved 94/94 original paths with zero errors and
`git diff --check` passed. Evidence is recorded under
`build/grounded-evaluation-20261105/`.

Next, validate a narrow target-extraction candidate through native host requests
and live rewrites on development data, investigate kind/scope attribution
separately, and address unsupported semantic additions without weakening the
frozen expectations. Only then run held-out candidate acceptance. Native macOS
execution and human semantic adjudication remain separate outstanding gates.
