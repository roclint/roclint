# Roclint evaluation runner and initial Phoenix smoke baseline

Date: November 5, 2026, America/Los_Angeles.

The AST correctness baseline is committed as `deadd39`. See
[independent verification](ast_baseline_verification_2026-11-05.md).
The separate development runner starts the evaluation milestone; it does not
declare tuning, held-out acceptance, packaging, or native macOS complete.

## Implementation and scope

- `tools/roclint_eval.py`: offline execution, versioned synthetic fixtures,
  per-case results, failure accounting, source fingerprints, and optional
  loopback-only publication. Standard-library offline path; no Phoenix import
  or collector is required by the desktop application.
- `tools/ast_grounding_evaluator.cpp`: links production storage/core libraries,
  builds isolated synthetic workspaces, and calls the actual index, resolver,
  renderer and saved-file hash validator. It never scans a real project.
- `tools/granite_contracts.cpp --rewrite-fixture`: uses the existing production
  rewrite prompt builder and confirmed literal-only intent preparation.
- Rewrite execution reuses the native benchmark worker and production response
  parser/validator. This measures an isolated rewrite path, not the HUD,
  discovery, scoring, clarification UI, insertion, or a multi-turn session.
- `tests/storage/ast_grounding_evaluation.json`: 30 manually annotated smoke
  cases, extending the 13-case acceptance corpus with 17 additional cases.
- `tests/rewrite/rewrite_fidelity_evaluation.json`: six synthetic rewrite smoke
  cases covering local rename, numeric constraints, file scope, action order,
  quoted instructions as data, and a no-clarification request.

The original 13-case test corpus is unchanged. Cases are grouped by failure
category, but this initial set is explicitly smoke-only. Development/held-out
splits and the proposed 100 AST / 100 rewrite datasets remain future work.

## Reproduction

Run from the repository root after building the normal Release targets plus
`roclint_ast_evaluator` and `roclint_granite_contracts`:

```powershell
& 'C:\Python313\python.exe' tools/roclint_eval.py `
  --suite ast --output build/evaluations/my-ast-run

& 'E:\phoenix_arize\.venv\Scripts\python.exe' tools/roclint_eval.py `
  --suite all --qt-bin D:/Qt/6.8.2/msvc2022_64/bin `
  --output build/evaluations/my-baseline --publish --strict-publication

# Publish saved measurements without repeating model execution:
& 'E:\phoenix_arize\.venv\Scripts\python.exe' tools/roclint_eval.py `
  --publish-existing build/evaluations/my-baseline --strict-publication
```

An output directory must be new. `--limit` records the exact restricted case
selection. `--suite rewrite` runs one worker sequentially using pinned Q8_0,
the generated runtime configuration, and its existing seed/temperature/deadlines.
The actual weight hash must match the pinned manifest. Loading/hash validation
is reported separately from per-case inference latency.

`report.json` includes source HEAD, dirty state and whole source-content
fingerprint, fixture hashes, evaluator version, selected IDs, platform,
configuration/model/tokenizer-container/native-binary hashes, actual outcomes,
timestamps, measurements, publication status and readback IDs. Source identity
is checked again after execution. `cases.jsonl` is a companion local artifact.
Errors and missing samples remain in denominators; correct abstentions are not
counted as resolutions. All reports are retained under ignored `build/`.

## Initial observed results

Both pilot runs used source-content fingerprint
`9327d7d9057a3f0465ef96e661c5099f19b8a2d7532774261e320c41651d45f4`
on committed core `deadd39` with the then-uncommitted evaluation tooling.
These are pilot measurements, not a claim that the later publisher changes had
already been present during inference.

| Check | Observed |
|---|---|
| AST cases | 30/30 checks passed; 20/20 expected resolvable bindings; zero wrong Verified bindings |
| Rewrite cases | 6/6 production-contract and literal/prohibition smoke checks passed |
| Samples | No execution errors or missing cases |
| Model/runtime | Pinned Granite 4.2 3B Q8_0, Vulkan; native manifest configuration and seed |
| Model load | 44,656 ms in this run, including the helper's loading/integrity work |
| Rewrite IPC exchange | 924–1,259 ms; median 1,089 ms across six short prompts |
| First token | 101–316 ms; median 147 ms |
| AST publication | 30 runs, 30 traces and 30 annotated records read back |
| Rewrite publication | Six runs, six traces and six annotated records read back |
| Full integration CTest | 51/51 passed in 184.46 seconds, including the registered runner tests |
| Final Python checks | 57 tests passed with one platform skip; eight evaluator regressions included |

After the final REST readback checks were tightened, the registered evaluator
CTest was rerun successfully. Structure verification preserved 94/94 original
paths with zero errors, and `git diff --check` passed.

Five rewrite outputs retained their original wording; the quoted-data example
was rephrased. No claim of improved writing quality follows from these smoke
checks. Every rewrite carries an explicit **human_review_required**, unscored
semantic-fidelity annotation. The summary's fully-faithful rate stays null.

The stale-file fixture distinguishes resolver evidence from saved-file hash
validation. Raw resolver status is not a host authorization claim; host lifecycle,
workspace invalidation and refresh recovery are covered by the native CTest
integration suites. Rewrite smoke cases currently do not consume AST XML.

Pilot experiments:

- [AST](http://127.0.0.1:6006/datasets/RGF0YXNldDox/compare?experimentId=RXhwZXJpbWVudDoy)
- [Rewrite](http://127.0.0.1:6006/datasets/RGF0YXNldDoy/compare?experimentId=RXhwZXJpbWVudDoz)

The Phoenix UI was checked: the AST dataset showed 30 examples and 30 runs with
score 1.00, and the rewrite comparison displayed all six cases, passing exact
checks and unscored semantic-review labels.

## Publication, failure behavior and retention

Publication uses the installed server's REST API and OTLP/HTTP protobuf on the
configured loopback origin. Phoenix's top-level Python import pulls in notebook
and pandas dependencies and stalled in this environment; the publisher avoids
that import. Only the lightweight generated OTLP protobuf messages are imported
from the existing Phoenix virtual environment. No package upgrades were made.

Content-addressed dataset names allow unchanged fixtures to reuse a dataset
version. Experiments pin the returned version ID. Publication never re-executes
inference. It saves created IDs incrementally, retains failed attempt metadata,
and compares stored outputs, scores, explicit unscored annotations and trace
counts with local results. Trace readback tolerates a bounded indexing delay.

Each trace represents an actual recorded case interval. Genuine native stage
durations are measurements on that root span; no artificial child waterfall is
created from upload time. Model loading is not relabeled as user-visible
generation. The local report keeps runtime details and full synthetic outputs;
publication excludes real source bodies, user history and absolute project paths.

The first AST publication successfully wrote records but encountered a transient
404 before the trace project was queryable. The failed attempt remains in the
local publication history and as experiment #1; successful readback is experiment
#2. It is not an additional independent evaluation sample.

The outage regression uses an unavailable loopback port, checks a nonzero strict
publication exit, and proves existing local results/summary are preserved.
Publication status and evaluation correctness remain separate.

There is no automatic deletion or retention change. Local outputs remain under
`build/evaluations/<run-id>/`; Phoenix records remain in its existing configured
storage. Remove specific experiment/dataset IDs through Phoenix's normal UI or
documented API only when intentionally discarding them. Never purge the database
or shared trace project as cleanup for one evaluation.

## Next acceptance work

Expand to the planned grouped development/held-out datasets, annotate semantic
fidelity independently, add real grounded-rewrite and multi-turn host cases, and
freeze thresholds before tuning. The measured cold load warrants broader
residency/recovery measurements; six short rewrites do not establish a dominant
production bottleneck or justify a runtime configuration change.

References: [Phoenix dataset/experiment API](https://arize-phoenix.readthedocs.io/projects/client/api/experiments.html),
[OpenTelemetry OTLP](https://opentelemetry.io/docs/specs/otel/protocol/).
