# Rewrite and clarification quality — November 16, 2026

## User report and observed failures

The user clarified that Rewrite and Clarify respond, but their results are
unhelpful. This is a semantic usefulness problem; passing UI and structural tests
cannot establish that the product meets that bar.

A bounded, read-only inspection of the four most recently updated local sessions
found the same two examples the user had already supplied in screenshots:

- The UI complaint rewrite added **"Summarize the user's feedback"**, which was
  not the requested task. It also acquired a hard prohibition/ambiguous protected
  target finding from the words **"I dont like"**.
- The music app's text/style prompt received mostly a grammar edit. That saved
  run had no discovery attempt: Julia's automatic route retained the original.
  This is evidence about that automatic run, not proof that an explicit Clarify
  click is blocked by Julia.
- A separate saved session contained `Work with Codex` as the draft. This confirms
  that placeholder-looking text reached history, but does not establish how the
  Windows provider produced it. Capture reliability remains a separate issue.

No history database rows or user preferences were edited. Raw private session
payloads were not copied to the evaluation corpus. Reproductions below are
explicitly synthetic and do not send drafts to an executing AI or insert text.

## Experiments: results that were not promoted

Evidence: `D:\codex-evidence\roclint-rewrite-clarify-quality-20261116`.
All trials use the pinned Q8_0 model, current native worker and schemas, the pinned
sampling/seed, thinking explicitly off and fresh KV cache per call. One sample per
case/variant on one Windows machine is not a statistical quality benchmark.

1. `comparison.json`: seven synthetic cases, current versus expanded instructions.
   More space produced longer answers, not consistent value. A music design label
   ended in a clipped `keepingb`; the cinematic candidate offered a non-cinematic
   alternative; several questions remained broad. Both music rewrites were largely
   restatements. The trial is rejected as a release candidate.
2. `second-trial/comparison.json`: the existing substantial-interview instructions
   versus the ordinary explicit-review contract on two clarification examples,
   plus a feedback-only rewrite comparison. The music question still confused
   compact typography with minimal labels, gestures and navigation. No generator
   change was adopted.
3. `minimal-role/comparison.json`: a smaller expression-helper role on three cases.
   The UI question wrongly treated quiet help as giving up context awareness. The
   music options remained generic. A more concrete camera question listed three
   approaches but supplied two options. This trial was also rejected.

The ordinary compact discovery contract still asks for a short question, short
labels and four-word effects. That is a genuine design constraint, but simply
relaxing it did not solve the measured failures. No canned answers or font/style
menus were installed to make these demonstrations look better.

## Implemented fixes (built, tested and installed)

### Opinion versus prohibition

`extractSessionConstraints` no longer mints a hard mutation prohibition merely
from a first-person negative opinion such as "I don't like" or "I do not enjoy".
The original text remains authoritative for the model; it is not discarded.
Scanning continues after an opinion so a later explicit "do not change" is kept.
"I don't want the API changed", "without changing" and imperative "Do not like or
share any posts" remain negative boundaries. Focused host tests passed before the
subsequent task-audit integration rebuild.

### Source-task retention and actionable Goldfish findings

Goldfish rewrite audit v2 compares what the receiving AI is being asked to do,
not only formal constraints and retained nouns. It asks for a concrete finding
when a candidate adds/substitutes a summary, description or report that the user
did not request, and still allows those tasks when requested.

The model trial `task-audit-v2/comparison.json` produced Fail findings for an added
summary and a build-to-description substitution, and Pass for a direct rewrite
and a requested summary (4/4 expected raw verdicts). Earlier trials are retained:
the v1 model often returned Unresolved with empty findings, and a weaker added
instruction did not reliably produce actionable findings. This is helper-only
model evidence, not a complete app or general quality pass.

The reserved `original_task` finding is now bound to a semantic-only User/Hard
constraint derived from the frozen original prompt. It is registered in the
semantic finding authority map, **not** passed to the lexical reconciler, which
would incorrectly demand the original words verbatim. Caller-supplied ID
collisions fail closed as malformed, and unrelated unknown IDs retain the old
honest-uncertainty behavior. Advisory model preferences retain zero veto power.

This binding matters: a previously unknown task finding was demoted to Unresolved,
which did not enter the actionable rewrite-repair branch. The recognized source
finding can trigger the existing bounded regeneration with the actual diagnostic.
No keyword blacklist or hardcoded replacement decides task meaning.

The repair flag now requires a passing re-audit. An unresolved second audit can
retain its candidate for review, but cannot be labeled a successful repair. The
existing transport cancellation, source/intent/revision/workspace bindings,
deadlines, retry count, repeat-candidate stops and independent approval review
remain in force.

## Verification and installation

- Release build: PASS, exit 0. Full enabled CTest: **66/66**, 352.80 s.
  The real-embedding integration test remains disabled; it is not a passing gate.
- Host opinion/prohibition, source-task authority, reserved-ID collision, bounded
  correction and unresolved re-audit regressions pass. Audit v2 test transports
  and the production contract builder were updated together.
- Fresh production-builder, pinned Q8_0 worker and production C++ authority
  reconciliation: **6/6** expected verdicts. Four development cases plus two
  held-out controls were evaluated after the audit instructions were frozen.
- The reusable runner and its checked-in synthetic corpus were then replayed:
  **6/6**, exit 0. This is a repeat of the same small corpus, not six additional
  independent examples or a general model-quality benchmark.
- Structure and whitespace checks: PASS. The first expanded build had a new test
  mistakenly call .ok() on boolean runtime-load results; this was corrected before
  the successful final build. Failed experiment/build receipts remain available.
- Installed all five matching artifacts (host, both worker locations, manifest,
  generated runtime config), with original-file backups and SHA-256 comparison.
  Installed Windows-platform startup with a memory-only database passed, exit 0,
  using bundled Qt with developer Qt paths removed. The normal background host
  restarted without forcing the HUD open. No fresh native insertion test was run
  for this semantic change; earlier Apply evidence is historical.
- Weights, sampling, thinking preference and configuration ID remain unchanged.
  The audit prompt contract is v2; runtime config remains v16.
- No history, saved drafts, preferences or model assets were replaced. No commit
  or push was made. Native macOS build/UI/model checks remain NOT RUN.

The saved runner is tools/companion_eval/evaluate_task_retention.py; corpus:
tests/fixtures/companion_eval/task_retention_2026-11-16.json. Example from the
repository root (Python standard library only):

```powershell
& C:\Python313\python.exe -S tools/companion_eval/evaluate_task_retention.py `
  --model-dir "$env:LOCALAPPDATA/roclint/Roclint/models/s2" `
  --qt-bin D:/Qt/6.8.2/msvc2022_64/bin `
  --out D:/codex-evidence/roclint-task-replay/results.json
```

The -S invocation bypassed a local Python startup stall observed during this
session; the cause was not diagnosed. It does not change the native model or its
instructions. The runner refuses concurrent runs in the same output directory.

Receipts under the evidence directory above:

- build-final.log, ctest-full.log, release-gate.json
- production-task-replay/results.json
- repeatable-task-retention-isolated.json and its log
- static-gates.json, installation.json, installed-backup/
- installed-smoke-stdout.log and installed-smoke-stderr.log

Installed host SHA-256:
8DBFE9EDCCC7382895DDD16B252D1DDBF8A2CC335FD7BD2737011CD2036E5862.
Installed native worker SHA-256:
A2F02F32863C8452FCAD8496EEDA35CE49BE9C8E0816B8E6E82AB9B1E457F47F.

## Quality work still open

These changes address task drift and a false guardrail. They do not establish that
Clarify reliably chooses the most useful question, translates aesthetic intuition
into concrete vocabulary, preserves all context in a final request, or produces a
better downstream artifact. A model rewrite that merely fixes grammar can be
faithful while still adding little product value. We must not solve that by
inventing unconfirmed requirements or presenting canned choices as model output.

The next quality gate needs held-out examples and independent human judgments of
question relevance, meaningful distinctions, consequence clarity, false choices,
confirmed-answer incorporation and resulting executor output. The lightweight
word-help route and the optional deeper interview need separate latency/usefulness
criteria. The larger review's hierarchy/typography and per-editor inline marks
also remain unfinished; this pass does not claim the UI/UX is ready.
