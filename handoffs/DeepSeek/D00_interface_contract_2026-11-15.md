# Coordinator contract — Roclint companion/intent/review, 2026-11-15

Status: binding for this session. Authored by the implementation coordinator.
Baseline `d5c8101e42b45d5d3b0796c97f01024214fd5d57`, branch
`feat/roc-hud-refresh`. All specialist work happens in the worktrees listed
below; the main checkout `D:/Stardust/roclint` is the integration tree and is
read-only to specialists.

## Worktrees and ownership

| Agent | Worktree | Branch | Owns |
|---|---|---|---|
| 1 Eval | `D:/Stardust/roclint-wt-eval` | `topic/eval-2026-11-15` | new `tools/` measurement scripts, new fixtures, evidence report only |
| 2 Intent | `D:/Stardust/roclint-wt-intent` | `topic/intent-2026-11-15` | `include/roclint/intent/`, `src/intent/`, `GraniteAdapter`, `GraniteRewriteAdapter`, `HostController*`, matching core/bridge/host tests |
| 3 Review | `D:/Stardust/roclint-wt-goldfish` | `topic/goldfish-2026-11-15` | `include/roclint/bridge/GraniteGoldfishAdapter.hpp`, `src/bridge/GraniteGoldfishAdapter.cpp`, its test, review-contract report |
| 4 UX | `D:/Stardust/roclint-wt-ux` | `topic/ux-2026-11-15` | `app/qml/shell/WordCompanion.qml`, `FloatingCard.qml`, `IntentReviewPanel.qml`, theme/components/`qml.qrc`, product-alignment/QML tests |
| 5 Runtime | `D:/Stardust/roclint-wt-runtime` | `topic/runtime-2026-11-15` | `app/runtime/GraniteEngine.*`, `app/host/NativeGraniteRuntime*`, runtime tests |

Nobody edits another agent's files. CMake, manifest and final integration belong
to the coordinator. Request CMake registration in the final report instead of
editing `CMakeLists.txt`. Never touch the main worktree or another worktree.

## Non-negotiable invariants

Preserve revision/workspace/field/session identity, stale-result refusal, approval
hashes, exact readback, UTF-16/UTF-8 correctness, the 350-byte AST XML bound,
bounded retries, frozen deadlines, cancellation, superseded-result handling,
keep-original, direct full rewrite, skip semantics (never confirms a value), and
"never press Send/Enter". Do not weaken a test, assertion or deadline. Do not
download models, upgrade dependencies, add cloud services, change weights,
sampling, temperature, top-p, seed or thinking defaults. Do not push, merge into
the shared branch, reset/clean/stash, replace the installed app, or terminate a
running application.

## Shared intent model (authoritative)

Reuse `analysis::AnalysisSnapshot`, `core::intent::ConfirmedIntent`,
`WorkingIntentState`, `InterventionStateMachine`, source constraints and the
existing revision snapshot. Do not create a parallel memory store that bypasses
those bindings. The new value object is a revision-bound **IntentBrief**, derived
from source text + snapshot + explicit decisions:

- source requirement (verbatim excerpt + offsets)
- confirmed decision (user-selected or custom, raw answer retained)
- preference (explicit; may narrow or reverse a prior preference)
- prohibition / negative constraint
- grounded evidence (workspace/AST/citation reference)
- proposed assumption (model-proposed, never confirmed unless the user accepts)
- open choice (unanswered or skipped)
- superseded decision (kept for provenance, explicitly not current)

Raw user answers and source excerpts are authoritative. Generated summaries are
display-only. A skip leaves the item open. "I don't know" creates an explicit
unknown/open item, not a default. A changed answer supersedes the earlier item
and invalidates proposals derived from it. Inferences are labelled proposed.

## Host/UI contract v1 (Agent 2 implements; Agent 4 binds)

Names and semantics are frozen. Agent 2 may add fields, not rename these.

Word surface (existing, keep compatible):

- `hasWordRefinement`, `wordRefinementOriginal`, `wordRefinementReplacement`,
  `wordRefinementExplanation`, `wordRefinementAlternatives`,
  `wordRefinementCandidates` (`id,label,replacement,explanation,assumption,requiresClarification`),
  `wordRefinementSelectedCandidateId`, `wordRefinementAssumption`,
  `wordRefinementNeedsClarification`, `wordRefinementClarificationQuestion`,
  `wordRefinementApplying`, `wordRefinementAnchorRect`
- `applyWordRefinement(expectedVersion, candidateId)`,
  `selectWordRefinementCandidate(candidateId)`, `dismissWordRefinement()`,
  `exploreInCompanion()`

New intent/interview surface:

- `Q_PROPERTY(bool hasIntentBrief ... NOTIFY intentBriefChanged)`
- `Q_PROPERTY(QVariantList intentBriefItems ...)` — each map contains exactly:
  `id` (QString), `kind` (QString: `source_requirement`,
  `confirmed_decision`, `preference`, `prohibition`, `evidence`,
  `proposed_assumption`, `open_choice`, `superseded_decision`),
  `text` (QString display), `sourceExcerpt` (QString verbatim, may be empty),
  `sourceRef` (QString, may be empty), `rawAnswer` (QString verbatim, may be
  empty), `status` (QString: `confirmed|proposed|open|superseded`),
  `consequential` (bool), `analysisItemId` (QString, may be empty)
- `Q_PROPERTY(QStringList intentOpenChoiceIds ...)`
- `Q_PROPERTY(QString intentBriefSummary ...)` — display-only, never authority
- `Q_PROPERTY(bool isInterviewActive ...)`
- `Q_PROPERTY(bool canExploreInterview ...)`
- `Q_PROPERTY(int interviewTurnsCompleted ...)`
- `Q_PROPERTY(QString interviewQuestion ...)`
- `Q_PROPERTY(QString interviewConsequence ...)` — plain-language effect of
  the different answers; may be empty
- `Q_PROPERTY(QVariantList interviewChoices ...)` — `id,label,consequence`
- `Q_PROPERTY(bool isInterviewBusy ...)`
- `Q_PROPERTY(QString interviewFeedback ...)` — relevance/correction feedback
- `Q_INVOKABLE quint64 openInterviewFromCompanion(quint64 expectedVersion)` —
  expands the current word/draft into the interview without losing the draft,
  selection, confirmed answers or open choices; idempotent on re-entry.
- `Q_INVOKABLE quint64 submitInterviewResponse(const QString& text,
  const QString& disposition, quint64 expectedVersion)` — `disposition` is
  `answer`, `custom`, `unknown` or `skip`. `skip` leaves the choice open;
  `unknown` records explicit uncertainty. Returns the new state version; stale
  version/selection refuses.
- `Q_INVOKABLE quint64 changeIntentDecision(const QString& itemId,
  const QString& newText, quint64 expectedVersion)` — supersedes the prior
  decision, retains provenance, invalidates dependent proposals.
- Existing `confirmChoice`, `submitCustomIntent`, `submitClarification`,
  `skipCurrentItem`, `resolveActiveAssumption`, `keepOriginalPrompt`,
  `synthesizeRewrite`, `rewriteWithoutQuestions`, `switchToManual` remain
  supported. Agent 2 may route the new invokable through these internally.

New comprehension-review surface:

- `Q_PROPERTY(QString comprehensionReviewState ... NOTIFY comprehensionReviewChanged)`
  — `not_run`, `running`, `ready`, `failed`, `stale`
- `Q_PROPERTY(QString comprehensionInterpretation ...)` — reviewer's inferred
  outcome; explicitly non-authoritative
- `Q_PROPERTY(QVariantList comprehensionFindings ...)` — each map contains:
  `id`, `severity` (`blocking|required|advisory`), `kind`
  (`comprehension|omission|meaning_drift|unsupported_assumption|negative_constraint|readiness`),
  `summary`, `sourceRef`, `sourceExcerpt`, `requiresUserDecision` (bool),
  `proposedRepair`
- `Q_PROPERTY(bool canApplyReviewedOutput ...)` — true only for the current
  revision/proposal with a completed fresh review, no unresolved blocking or
  required findings, and all required acknowledgments. Partial/unreviewed text
  is never applicable.
- `Q_INVOKABLE quint64 requestComprehensionReview(quint64 expectedVersion)`
- `Q_INVOKABLE quint64 resolveComprehensionFinding(const QString& findingId,
  const QString& resolution, quint64 expectedVersion)` — `return_to_interview`,
  `acknowledge`, `keep_original`. A blocking finding cannot be acknowledged into
  applicability without an explicit user decision.
- Existing `canApprove`, `pendingReviewCount`, `audit*`, `discardProposal`,
  `approveAndCopyRewrite`, `beginInsertionTargetSelection` stay. `canApprove`
  must not be true while a current substantial proposal is unreviewed/stale.

All `*Changed` signals must be emitted on transitions. Every invokable rejects a
stale `expectedVersion` without changing state.

## Review adapter contract (Agent 3 implements; Agent 2 consumes)

In `roclint::bridge`:

The canonical types are in `include/roclint/review/ComprehensionReview.hpp`
(coordinator-owned). They are:

```cpp
enum class ReviewFindingSeverity : std::uint8_t { Advisory, Required, Blocking };
enum class ReviewFindingKind : std::uint8_t {
    ComprehensionGap, MeaningDrift, Omission, UnsupportedAssumption,
    NegativeConstraint, Readiness
};
enum class ComprehensionReviewStatus : std::uint8_t {
    Completed, Unavailable, Malformed, Rejected
};
struct ComprehensionReviewRequest {
    std::string candidatePrompt;
    std::string originalPrompt;
    std::vector<core::ScopedConstraint> constraints;
    std::vector<std::string> confirmedDecisions;   // authoritative wording only
    std::vector<std::string> sourceExcerpts;       // verbatim evidence
    std::string taskKind;   // "creative" | "code" | "performance" | "general"
    std::string sessionId;
    std::string revisionId;
};
struct ComprehensionFinding {
    std::string id;
    ReviewFindingKind kind{ReviewFindingKind::ComprehensionGap};
    ReviewFindingSeverity severity{ReviewFindingSeverity::Advisory};
    std::string summary;
    std::string sourceRef;
    std::string sourceExcerpt;
    bool requiresUserDecision{false};
    std::string proposedRepair;
};
struct ComprehensionReviewResult {
    ComprehensionReviewStatus status{ComprehensionReviewStatus::Unavailable};
    std::string interpretedOutcome;
    std::vector<std::string> interpretedRequirements;
    std::vector<ComprehensionFinding> findings;
    bool readyForTask{false};
    std::string rationale;
};
```

`GraniteGoldfishAdapter::reviewComprehension(request, transport)` builds a fresh
prompt containing candidate + original + confirmed decisions + source evidence
only. It must not include the drafting conversation, prior prompts, hidden
reasoning or persuasive generation rationale. Malformed, empty, truncated or
unparseable output fails closed (`Uncertain`/`Failed`), never authorizes apply.
Optional deterministic prechecks may add findings but must not invent content.
Same-model agreement is fallible; the report and UI must say so. Readiness is
task-relative: creative prompts do not need file paths or signatures.

## Timing trace contract (Agent 5/2 implement; Agent 1 consumes)

Opt-in only, enabled by environment variable `ROCLINT_TRACE_TIMING=<file>`.
Append one JSON object per line, UTF-8, no prompt or source text:

```json
{"ts_ms":0,"trace_id":"...","request_id":"...","revision_id":"...","surface":"word|interview|rewrite|review|apply","stage":"debounce|route|queue|model_ready|generate|audit|review|visible|apply","phase":"start|end|mark","elapsed_ms":0,"detail":{}}
```

`elapsed_ms` is per event where meaningful; consumers derive spans. Events must
be best-effort and must never block or fail the production request. Keep the
trace disabled by default. Agent 5 owns worker/runtime spans; Agent 2 owns host
spans; Agent 1 parses and reports them. Do not fabricate a stage with no event.

## Build and test commands

CMake/CTest (not on PATH):

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
```

Configure a specialist worktree without native Granite (fast, no GPU) and point
third-party sources at the main checkout's populated dependencies:

```powershell
& $cmake -S <WORKTREE> -B <WORKTREE>\build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.8.2/msvc2022_64 -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=ON -DROCLINT_DEPLOY_QT=OFF -DROCLINT_BUILD_NATIVE_GRANITE=OFF `
  -DROCLINT_SQLITE_SOURCE_DIR=D:/Stardust/roclint/build/_deps/roclint_sqlite-src `
  -DROCLINT_TREE_SITTER_SOURCE_DIR=D:/Stardust/roclint/build/_deps/roclint_tree_sitter-src `
  -DROCLINT_TREE_SITTER_CPP_SOURCE_DIR=D:/Stardust/roclint/build/_deps/roclint_tree_sitter_cpp-src
& $cmake --build <WORKTREE>\build --config Release --target <target>
& $ctest --test-dir <WORKTREE>\build -C Release -R <regex> --output-on-failure
```

Agent 5 needs native Granite: configure with
`-DROCLINT_BUILD_NATIVE_GRANITE=ON -DROCLINT_LLAMA_SOURCE_DIR=D:/Stardust/roclint/build/deps/llama.cpp`
(fresh configure only; do not edit the main build). Do not run live model
generation until the coordinator releases the GPU; Agent 1 owns the baseline run.

## Hardware coordination

Only Agent 1 may run the pinned model for the initial baseline. Agent 5 may run
mock/CPU tests and implement instrumentation, then report and wait for a
follow-up before benchmark experiments. Do not run heavyweight builds or
GPU work in parallel with Agent 1's live baseline. If a build is required,
prefer a single focused target and serialize with the coordinator.

## Deliverable per agent

A local commit (or commits) on the topic branch, never pushed. Final report:
baseline/result commits, exact owned files changed, implemented behavior,
interface changes, tests/commands/results, raw evidence paths, known
limitations, work not run, and requested CMake/manifest edits for the
coordinator. Label helper-only, mocked, headless, native-fixture and full-app
evidence separately. Code that only compiles or only tests a mock is not enough.
