# AST target extraction and rewrite fidelity remediation

Date: 2026-11-06, America/Los_Angeles.

The frozen mechanical/grounding checks improved from **148/200 to 200/200**.
All 100 AST scenarios and all 100 rewrite scenarios pass, including the held-out
groups. There are zero wrong Verified bindings, missing cases or execution errors.
**Human semantic acceptance remains pending.** Two provisional editorial concerns
remain; a mechanical pass does not settle their meaning.

## Source and measurement identity

- Application fix: `bbc05e5621abe9d8810e5c3fb2c899d01fa19007`.
- Inference source fingerprint: `854d2a3896ae767755e356118f71fdf570cf9b3852949f4b6d4d3ec953105458`.
- Both the complete development run and final 200-case run started clean, ended
  unchanged, and shut down the worker gracefully.
- Final run: `build/evaluations/grounded-v2-fixed-all/report.json`.
- Development run: `build/evaluations/grounded-v2-fixed-development/report.json`.
- Publication-only recovery fix: `51a01c3`; its source identity is recorded
  separately under `publication.publisher_source`. Inference was not repeated
  for publication recovery.
- The two v2 datasets, grouped splits, expected values and frozen thresholds
  were not edited. The baseline remains the original `grounded-v2-baseline` run.
- Model weights, llama.cpp revision, Vulkan backend, Q8_0 quantization, seed,
  temperature and sampling policy are unchanged. The manifest now correctly
  identifies `granite.rewrite.format.v6`.

The machine-readable comparison is in `ast_rewrite_tuning_2026-11-06.json`.
Historical reports and the earlier isolated experiment remain intact.

## Implemented corrections

### Exact targets and explicit bounds

`CanonicalIntentExtractor` recognizes a leading qualified C++ identifier before
ordinary prose. `Refactor Compute::run so it runs faster` now identifies
`Compute::run`, rather than searching for the entire sentence fragment.
Templates, signatures with trailing qualifiers, member-access/operator suffixes
and declarator keywords are not reduced to an apparently supported name prefix.
Exact lookup and ambiguity handling remain in the AST resolver; no fuzzy or
embedding fallback was introduced.

Quoted and fenced instruction examples are masked at their original byte
offsets. An unquoted requested target in the same clause remains usable. Code
references, quoted scopes and quoted file paths remain intact; masking a scope
such as `in "Missing"` must not widen the query globally. Unclosed data delimiters
fail conservatively. Inline prose examples cannot supply actions. Leading
whitespace no longer shifts clause source offsets.

Conditions preserve the complete trailing context from its first marker. An
`if`/`when` clause cannot discard an earlier file constraint. The resolver removes
the file clause's own preposition before parsing the remaining scope, including
case and whitespace variants, without consuming a separate explicit scope.
This fixes the invented `for` scope in `in file ... for readability`.

The word `fields` elsewhere in a task no longer imposes the Field AST kind on a
method target. An actual field noun in the requested target still supplies that
hint; a method literally named `Log::field` does not acquire it from its name.

### Rewrite contract

The rewrite prompt distinguishes declaration metadata from user requirements.
Signatures, return types, locations and overload candidates do not authorize API
freezes, return policies or overload selection. The user's original prompt now
follows the reference metadata and decisions, with an explicit plain-prose
response example for the generic/compact profiles.

The contract preserves constraint strength, requested actions and literal data,
keeps unspecified implementation choices open, and permits restating a task
without asking for implementation details. XML transport escaping is decoded
once; fallback canonical JSON is itself escaped before insertion into the prompt
wrappers. Existing approval, freshness, hash, 350-byte renderer and body-exclusion
contracts were not relaxed.

The final recorded responses preserve the original request in the earlier
blocking-wait, RAII, duplicate-revision, AST-signature and XML-literal witnesses.
In particular, duplicate-revision idempotence no longer invents `returns false`,
and a ban on adding waits no longer becomes a request to remove existing waits.
These are observations of these outputs, not a general semantic guarantee.

## Frozen results

| Suite / split | Baseline | Fixed candidate |
|---|---:|---:|
| AST development | 69/69 | 69/69 |
| AST held-out | 31/31 | 31/31 |
| Rewrite development | 37/72 | 72/72 |
| Rewrite held-out | 11/28 | 28/28 |
| Combined | 148/200 | 200/200 |

All 69 expected-resolvable AST cases resolve correctly; wrong Verified count is
zero. The mandatory grounding/freshness controls pass. The two post-generation
saved-file edits withhold their proposals (`proposal_published=false`); those are
negative-control passes, not approved rewrites.

The complete development run passed 141/141 before the final run. No application
changes were made in response to the final held-out observations. The same
200-case evaluation returned exit 0 with source unchanged and graceful shutdown.

The model returned `rewritten` in all 100 scenarios. Raw wording is unchanged in
86/100; excluding the two withheld controls, 85/98 are unchanged. Preservation is
therefore better demonstrated than editing usefulness. Four supplied user
choices are incorporated through the production intent builder. This does not
claim that the discovery model or clarification UI was executed.

Median recorded generation was 934.61 ms, prefill 31.97 ms and input length 981
tokens for these short fixtures. This was a correctness exercise, not a controlled
performance comparison or a claim about production bottlenecks.

## Editorial review and open gate

`tests/rewrite/grounded_v2_fixed_editorial_review.json` binds every annotation to
the actual run, source commit and exact model-reply SHA-256. All labels are
**AGENT**, never human acceptance: 96 provisionally preserved, two concerns and
two withheld. `fully_faithful_rate` remains null. The full review packet, including
supplied choices, outputs, grounding and empty human fields, is retained at
`build/evaluations/grounded-v2-fixed-all/review_packet.json`.

The two concerns are:

- `callback_rules_4`: “follow the existing error path” becomes exceptions being
  “propagated according to” that path. This could imply an exception propagation
  policy the original leaves to the existing handling.
- `multi_action_1`: separate refactoring, regression-test and example-update
  actions become “Refactor ... to add ...”. The words remain, but their task
  relationship could be read differently.

Human adjudication must resolve these and assess fidelity/clarity/usefulness
before asserting the frozen human semantic acceptance threshold. No held-out
semantic pass rate is inferred from agent labels or literal checks.

## Verification

- Release build: exit 0, including self-contained Qt staging.
- Full CTest: 52/52, 190.97 s, on the application candidate without concurrent
  model inference.
- Python after the publication recovery fix: 76 tests, one platform skip,
  35.181 s; all pass.
- Registered evaluation-runner CTest after that fix: pass, 3.19 s.
- Structure: 94/94 original paths, zero errors. `git diff --check`: pass.
- Native regression coverage includes ordinary prose boundaries; unsupported
  suffixes/declarator tails; quoted/fenced data; real source spans; field hints;
  explicit, quoted and missing scopes; complete file/conditional bounds; fallback
  escaping; actual host XML and proposal hashes; and existing stale/incomplete
  snapshot controls.

An early concurrent test run exposed the unescaped fallback and stale manifest
label, both corrected before final verification. Its 500-ms workspace helper
assertion also failed once under competing load; the independent rerun and final
uncontended full suite passed. Intermediate logs are retained, not substituted
for the final suite.

The first development candidate passed 140/141 but leaked metadata into some
task strings; it was not selected. A second run was interrupted after 57/72
rewrites and is explicitly retained as incomplete, not acceptance evidence.
The clean final candidate was subsequently measured in full.

## Phoenix publication

The existing local service was started using its configured working directory
`E:\phoenix_arize`. No package upgrade, reset or purge was performed.

- [AST final experiment](http://127.0.0.1:6006/datasets/RGF0YXNldDo1/compare?experimentId=RXhwZXJpbWVudDoxMA==)
- [Rewrite final experiment](http://127.0.0.1:6006/datasets/RGF0YXNldDo2/compare?experimentId=RXhwZXJpbWVudDoxMQ==)

Readback verified 100 runs, 100 traces and 100 deterministic/unscored-semantic
annotation records for each suite, plus all 100 output-bound agent reviews on
the rewrite experiment. Human-review annotations remain unscored.

An initial publication checkpoint hit a Windows sharing lock during atomic
report replacement. The saved evaluation remained complete. `write_json` now
retries PermissionError at most eight times (2.54 s total backoff), preserving
the prior report and surfacing permanent failures. Regression coverage proves
both transient recovery and permanent-failure preservation. The successful
publication used the same saved observations; it did not replay inference.
Failed experiments 8 (100 AST runs) and 9 (no rewrite runs) remain recorded under
`publication_attempts`. Successful experiments 10/11 are not extra evaluation
samples. No historical records were deleted.

## Remaining scope

Grounding remains saved-file C/C++ syntax with exact matching and conservative
abstention, not compiler type resolution. The evaluation exercises production
host grounding and rewrite adapters, not native typing, accessibility insertion,
the visible HUD, or a live multi-turn user session. Windows evidence does not
establish native macOS correctness; macOS checks remain NOT RUN.

The next quality step is human adjudication and a broader usefulness corpus,
followed by the separately planned Windows application packaging and redesign.
Phoenix remains a development service, not a shipped application dependency.
