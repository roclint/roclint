# Roclint AST completion and Phoenix evaluation plan

Date: 2026-11-05, America/Los_Angeles. Updated 2026-11-06. Milestone 0 is committed as `deadd39`; Milestone 1 is implemented. Version 2 freezes 100 AST and 100 rewrite cases with family-level development/held-out splits in `evaluation_v2_freeze.json`. The original baseline measured AST 100/100 and grounded rewrites 48/100. Application remediation `bbc05e5` now measures 100/100 in each suite, including the held-out groups, with zero wrong Verified bindings. Full Windows build/CTest and local publication/readback pass. Human semantic/usefulness acceptance remains open, with two provisional agent wording concerns. No native macOS result or release signoff is claimed.

Execution evidence: [AST baseline verification](../audit/ast_baseline_verification_2026-11-05.md).
Runner and initial publication evidence: [Phoenix smoke baseline](../audit/phoenix_evaluation_baseline_2026-11-05.md).
Expanded baseline, editorial findings, and candidate experiment: [Grounded evaluation v2](../audit/grounded_evaluation_v2_2026-11-05.md).
Implemented remediation and final frozen results: [AST/rewrite tuning](../audit/ast_rewrite_tuning_2026-11-06.md).

## Objective and boundary

Finish the remaining AST correctness work, establish reproducible AST and rewrite evaluations, and use measured results to tune Roclint. Then hand a stable core to Windows application packaging and the approved visual redesign.

Phoenix is initially a local development/evaluation service. The shipped desktop application must not require Phoenix, Python evaluation packages, or a running collector. Keep pure AST grounding; no embedding or episodic retrieval project is included.

The implementation should reuse current production adapters, runtime helpers, benchmarks, and tests. Distinguish component measurements from actual end-to-end host behavior.

## Verified starting point

- Repository: `D:\Stardust\roclint`; current source includes uncommitted AST changes on baseline `240c74f`. Record the actual revision and working-tree fingerprint when execution begins.
- Last independent full verification: Release build passed; CTest 49/49; structure 94/94 with zero errors. Passing these tests does not close the remaining diagnostic findings.
- Existing annotated AST corpus: 13 cases in `tests/storage/ast_grounding_corpus.json`.
- Existing tools: `tools/ast_grounding_benchmark.cpp`, `tools/benchmark_granite_backends.py`, `tools/check_granite_runtime.py`, and the host freshness integration tests.
- Phoenix root: `E:\phoenix_arize`; Python `E:\phoenix_arize\.venv\Scripts\python.exe`.
- Verified packages: Phoenix 21.0.0, client 3.7.0, OTEL 0.17.2, evals 3.9.5; Python 3.12.11. Use installed APIs after inspecting their signatures. No package upgrade is required by this plan.
- UI responds at `http://127.0.0.1:6006`; local OpenAPI advertises datasets, versions, experiments, and annotation endpoints. Trace ingestion and experiment publication still require an explicit end-to-end smoke test during implementation.

## Milestone 0 — close correctness findings and preserve a baseline

Before changes, save the current diagnostic outcomes and identify the exact source state. The revision alone is insufficient while the implementation is uncommitted: include a tracked-diff fingerprint and relevant untracked-source hashes.

Close these four remaining findings:

1. Workspace changes must invalidate affected pending proposals and supersede old model requests, including A → B → A and switching during generation. Old-workspace evidence cannot publish as CURRENT in the new workspace.
2. Current revalidation failure from an unavailable cache must invalidate the affected AST-bound proposal. Missing generation information is distinct from a genuinely superseded valid snapshot.
3. The context renderer must omit records whose required identifying target cannot fit. No empty-target Verified records; hash/HUD subsets must match meaningfully emitted evidence.
4. A detected selected-file hash mismatch must enqueue bounded, coalesced refresh recovery independently of watcher delivery. Recovery must respect workspace/request ownership and keep current stale evidence withheld.

Use `build/ast-additional-review.md` and `build/ast-remediation-rereview.md`, including their native diagnostic probes. Convert findings into registered regression tests.

Acceptance: all four reproductions corrected, earlier fixes retained, full required build/tests/structure checks pass, and a focused independent re-review closes the findings. Preserve immutable historical approvals and ordinary prompt assistance when optional AST context is unavailable.

## Milestone 1 — establish the evaluation runner and local publication

Add a small Python evaluation harness under the repository's development tools. Use Phoenix's existing venv, with an explicit interpreter path and documented dependencies; do not add it to desktop startup.

Suggested organization (names may follow repository conventions):

- `tools/evaluation/`: runner, result schema, deterministic evaluators, Phoenix publisher.
- `tests/evaluation/`: frozen fixtures and evaluator tests.
- `build/evaluations/<run-id>/`: ignored run manifest, JSONL case results, summaries, diagnostics.

Provide two run modes:

- Offline: produce complete local results without Phoenix.
- Publish: run the same evaluation and send results to the local Phoenix instance. Publishing errors remain explicit and do not erase local results. A strict publication mode fails if publication is required and unavailable.

Separate evaluation verdict from publication status. Timeouts, model failures, missing samples, and evaluator errors must remain counted outcomes; never remove them to improve reported accuracy.

Each run manifest records source revision/fingerprint, fixture version/split/hash, evaluator version, runtime/model/tokenizer artifact hashes, quantization, seed, temperature, backend, hardware, relevant configuration, and timestamps. Verify source/fixture identity did not change during the run.

Use trace-specific HTTP configuration:

```powershell
$env:OTEL_EXPORTER_OTLP_TRACES_ENDPOINT = 'http://127.0.0.1:6006/v1/traces'
$env:OTEL_EXPORTER_OTLP_TRACES_PROTOCOL = 'http/protobuf'
```

The generic `OTEL_EXPORTER_OTLP_ENDPOINT`, if used instead, must contain the base URL without `/v1/traces`.

Acceptance: run a synthetic smoke fixture offline, publish it, read back its run ID/result/score, and confirm a Phoenix outage preserves a usable local report. Confirm the UI shows the same case count and outcomes as local artifacts.

## Milestone 2 — build versioned evaluation sets

Create two versioned datasets, initially targeting at least 100 AST cases and 100 rewrite cases. Begin with a small smoke subset, then expand coverage by failure category rather than filling the count with trivial variants.

Group related variations into development and held-out splits so near-duplicate templates do not leak across splits. Freeze held-out cases and acceptance thresholds before tuning. Pin the Phoenix dataset version ID for every experiment.

### AST and host reliability

Cover exact names, qualified scopes/files, overloads, declaration/definition sites, parameters, formatting, missing targets, type mismatches, genuine ambiguity, unsupported syntax, and recovery. Include the four remaining findings and earlier large-scope/incomplete-scan cases as mandatory integration regressions.

Annotate expected outcomes independently of the resolver under test. Expected targets should include stable fixture identity, relative file, symbol kind, and supported signature/span evidence. Separate candidate retrieval, final resolution, rendering, freshness, and host proposal lifecycle checks.

Report:

- Correct resolution rate on supported, resolvable cases.
- Resolution coverage on that same population.
- Wrong Verified count, with the number of Verified decisions and total cases.
- Correct ambiguity/unsupported/missing outcomes by category.
- Candidate recall where the candidate list is the measured object.
- Workspace/stale-evidence rejection and recovery outcomes.
- Nonempty identifying targets, hash/render consistency, UTF-8/XML validity, body exclusion, and actual context token count.

Do not describe correct abstentions divided by all cases as resolution coverage.

### Rewrite fidelity

Use synthetic prompts with annotated required actions, targets, values/units, scope, exceptions, prohibitions, quoted data, and permitted open details. Include multi-action prompts, optional clarification, Skip Questions + Full Rewrite, and conflicting requirements.

Check target/value/scope preservation, negative constraints, unsupported additions, valid output contracts, and supported use of clarification. Do not require one exact rewritten sentence when several faithful phrasings are valid.

Use deterministic assertions for precise invariants and human annotations for semantic paraphrases. The production extractor must not be the sole oracle for its own correctness. An LLM judge is optional future research, not an initial acceptance dependency; do not send project data to a cloud judge.

Acceptance: fixtures and annotations reviewed, category counts reported, development/held-out versions frozen, and evaluator failure cases tested.

## Milestone 3 — measure the complete pipeline

Reuse existing numeric metrics before adding a C++ OpenTelemetry dependency.

Measure separately:

- Cold and unchanged workspace indexing.
- Saved-file update and missed-notification recovery.
- Selected-file hash validation, exact lookup, grounding, context rendering.
- Model loading, Granite prefill, first-token latency, decode/generation, KV reuse, input/output tokens.
- Actual end-to-end host request latency, cancellation, and stale-result rejection.
- Julia/routing stages when they are part of the exercised production host path.

A direct Granite worker benchmark is a component experiment, not an end-to-end Roclint result.

Label AST cache state, model-load state, and KV state independently. Use explicit cache reset/repeat controls for cold/warm comparisons. Compare candidates on the same machine/configuration/case order, with enough paired repetitions to report p50/p95 and sample counts; use p99 only when enough samples support it.

Create real root traces around evaluation cases. Put existing stage durations on traces as numeric measurements. Create child spans only when genuine stage boundaries/timestamps are available; do not fabricate a timing waterfall from upload time or mix unsynchronized process clocks.

Start with one model worker/concurrent generation to avoid resource contention. Keep model load and publication overhead separate from user-visible generation latency. Measure evaluation/export overhead with publishing on and off.

Acceptance: a reproducible baseline report identifies the dominant delay and quality failure categories, with complete sample counts and no inferred Metal timings.

## Milestone 4 — run controlled tuning experiments

Change one factor per experiment and compare against the frozen baseline:

1. Context selection/packing: preserve exact identifying information, prioritize useful supported evidence, and evaluate how much the conservative 350-byte cap omits. An optional pinned-tokenizer budget may use more of the existing 350-token ceiling only if it preserves all contracts and improves measured outcomes.
2. Grounding/clarification: improve supported exact constraints and candidate explanations without guessing through ambiguity.
3. Rewrite instructions: improve action/target/negative-constraint fidelity and reduce unnecessary clarification or invented requirements.
4. Refresh efficiency: test incremental FTS maintenance if measured refresh cost materially affects responsiveness. Preserve search integrity, migrations, stale-row filtering, and failure recovery.
5. Native generation settings: investigate only after baseline evidence identifies a bottleneck; preserve output contracts and verify cold/warm correctness. No broad model/runtime migration is included.

Tune on development data. Run the frozen held-out set for acceptance. Record every attempted candidate, including rejected ones. Keep a candidate only if it passes quality gates and provides measured benefit without a material regression elsewhere.

Proposed acceptance thresholds to freeze before tuning:

- All mandatory lifecycle/freshness/workspace regressions pass.
- Zero wrong Verified outcomes in the acceptance fixtures, with counts and coverage reported.
- At least 95% correct resolution on supported, resolvable held-out cases.
- All mandatory ambiguity and incomplete-evidence cases reject unsupported authority.
- Zero violations of annotated critical prohibitions/targets/values in mandatory rewrite fixtures.
- At least 95% fully faithful outcomes on the reviewed held-out rewrite set, with the human rubric and unresolved judgments shown.
- No missing samples, unreported contract failures, or hidden skipped requirements.
- No unexplained latency regression beyond the tolerance agreed from baseline repeat variability.

These are finite-test acceptance criteria, not proof that errors are impossible.

## Milestone 5 — stop tuning and hand off to the app milestone

Core tuning is complete when the remaining findings are closed, the evaluation runner is reproducible, the frozen quality gates pass, and measured remaining bottlenecks are either addressed or explicitly accepted. Do not keep expanding AST language support or model research before shipping the application shell.

Deliver:

- Regression fixes and updated architecture/contract notes.
- Fixture and evaluator versions.
- Offline results plus Phoenix experiment IDs/links.
- Baseline-versus-candidate comparison and accepted/rejected changes.
- A short list of accepted limits and deferred optimizations.

Then proceed with the agreed product sequence:

1. Lock essential HUD interactions: invocation, clarification, Full Rewrite, Skip Questions, preview, and insertion.
2. Package the existing Qt host as a proper Windows application with installation, normal launch, tray/settings, model setup, and clean uninstall.
3. Apply the approved rounded retro-modern/Roc-bird design and motion.
4. Re-run the frozen core evaluations and native end-to-end UX checks against the installed application.

macOS packaging/native AX/Metal validation remains a separate platform gate and requires a real Mac/runner.

## Execution ownership

A coordinator owns interfaces, test registration, source fingerprints, acceptance decisions, and final integration. Up to three specialists can work concurrently with exclusive file ownership:

- Core reliability: remaining AST/workspace/proposal/recovery fixes.
- Evaluation quality: fixture annotations, fidelity rubric, deterministic evaluators.
- Harness/observability: existing benchmark adapters, local result artifacts, Phoenix datasets/experiments/publication.

Fixture design and harness scaffolding can proceed while core fixes land. Final baselines and tuning depend on stable corrected source. Do not run competing builds or multiple heavyweight model workers against shared resources.

## Data and operational rules

Use synthetic fixtures initially. Store real project bodies, absolute workspace paths, credentials, and personal prompts outside Phoenix by default. Export case IDs, source/configuration fingerprints, synthetic expected/actual results, statuses, and measurements. Trace collection never grants authority to execute or modify source.

Keep the local collector on the configured loopback endpoints. Do not upgrade/reinstall Phoenix, change its database, enable production auto-instrumentation, or alter desktop startup as part of planning. During implementation, retain explicit local retention/deletion controls and report publication failures separately from product correctness.

## References

- Phoenix versioned datasets: https://github.com/Arize-ai/phoenix/blob/main/docs/phoenix/datasets-and-experiments/concepts-datasets.mdx
- Phoenix experiment API: https://arize-phoenix.readthedocs.io/projects/client/api/experiments.html
- Phoenix OTEL endpoint configuration: https://arize-phoenix.readthedocs.io/projects/otel/
- OpenTelemetry endpoint construction: https://opentelemetry.io/docs/specs/otel/protocol/exporter/
