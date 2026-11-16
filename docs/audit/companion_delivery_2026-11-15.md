# Roclint companion / adaptive interview / independent review — delivery, 2026-11-15

Status: implementation and Windows verification in progress on the combined
integration branch `topic/integration-2026-11-15`. Nothing has been pushed,
merged into `feat/roc-hud-refresh`, deployed, or installed over the user's app.
Native macOS/Metal and IDE-provider parity remain **NOT RUN**.

Baseline source: `d5c8101e42b45d5d3b0796c97f01024214fd5d57` (`feat/roc-hud-refresh`).
Combined branch base: the same commit. The user's uncommitted
`docs/CURRENT_IMPLEMENTATION.md` and planning files in the main checkout were
preserved; integration was done in a separate worktree.

## 1. Delivered behavior

### Word/phrase companion

- Compact non-activating popup anchored from the host's `wordRefinementAnchorRect`.
- Shows the frozen original span, proposed replacement, plain-language effect,
  meaningful alternatives with their own consequences, a disclosed assumption,
  clarification state, working state and explicit Apply / Explore / Keep original.
- Apply remains unavailable while clarification is required, a request is
  applying, or no validated replacement exists.
- Long content scrolls inside a bounded popup instead of covering the composer.
- The popup never takes editor focus by design; keyboard access continues through
  Explore into the HUD, which is focusable.

### Shared intent and substantial adaptive interview

- Revision/workspace/snapshot-bound `IntentBrief` derived from the existing
  `AnalysisSnapshot`, `ConfirmedIntent`, `WorkingIntentState` and source
  constraints. It distinguishes source requirements, confirmed decisions,
  preferences, prohibitions, grounded evidence, proposed assumptions, open
  choices and superseded decisions.
- Raw answers and verbatim source excerpts remain authoritative; the display
  summary is explicitly non-authoritative.
- `openInterviewFromCompanion` carries the draft and selected phrase, forces an
  explicit review trigger, and revives an automatic ORIGINAL/QUIET route instead
  of leaving a dead end.
- The interview has no fixed two/three-question ceiling. Each turn is one
  bounded model request with its own deadline; follow-ups are generated from the
  current brief and highest-impact unresolved choice.
- Custom answers, "I don't know", skip and explicit correction are supported.
  Skip leaves the topic open; unknown records uncertainty; neither confirms a
  value. Correction supersedes the prior decision, retains provenance and
  invalidates dependent proposals.
- Direct full rewrite, rewrite-without-questions and keep-original remain
  available.
- Deterministic guards reject an unchanged "rewrite", an instruction *about*
  the prompt ("Determine...", "Clarify...") and a rewrite that drops a supplied
  exact value/unit. The automatic route does not treat an S1 RULE_CONFLICT
  signal alone as proof that an exact requirement is contradictory.

### Independent comprehension and constraint review

- New fresh-context review contract `granite.comprehension_review.v1` in
  `GraniteGoldfishAdapter`: the reviewer receives the candidate, original source,
  authoritative confirmed decisions, source-linked constraints and verbatim
  evidence only. It never receives the drafting conversation or generation
  rationale.
- The reviewer first states the outcome, requirements, assumptions and
  unresolved decisions it infers, then compares them with authoritative intent.
  Findings are source-linked and actionable (`kind`, `severity`, `summary`,
  `source_ref`, `source_excerpt`, `requires_user_decision`, `proposed_repair`).
- Task-relative readiness: creative prompts do not need invented file paths or
  signatures; code prompts respect explicit targets/preservation constraints.
- Fail-closed: malformed/empty/truncated responses, unknown severities/kinds,
  duplicate finding IDs, non-verbatim excerpts and transport failure never
  authorize apply. `reviewAuthorizesApply` requires a completed review,
  non-empty interpretation, `ready_for_task`, and no blocking/required or
  user-decision finding.
- Host binding includes session, revision, proposal ID and proposal hash, plus
  workspace epoch. A stale review cannot authorize a changed proposal. Skipped
  or unresolved findings keep apply locked and route back to the interview.
- Same-model fresh context is explicitly described as fallible; it is not proof
  of intent.

### Runtime/scheduling instrumentation

- Opt-in JSONL trace (`ROCLINT_TRACE_TIMING=<file>`) shared by the host native
  runtime and helper. Events carry monotonic `ts_ms`, trace/request/revision
  identity, surface, stage, phase, elapsed time and bounded numeric detail. No
  prompt, instruction, schema, source text or token text is logged.
- The helper reports queue wait/depth, cold/warm model-ready spans, prefill,
  first token, decode, sampler, setup/finish and total time. The host reports
  truthful BUSY while a semantic call is pending and cancellation reasons.
- A caller-remaining-budget field lets the single helper reject queued work that
  already outlived the caller's deadline before inference starts. It cannot
  extend a caller's deadline.
- `granite.comprehension_review.v1` uses the existing 1535-token rewrite answer
  reservation (2048 with thinking off) instead of the 512-token audit profile,
  so the open findings array is not silently truncated. No weights, sampling,
  temperature, top-p, seed or thinking default changed.

## 2. Interfaces

The complete Host/UI v1 contract is in
[`handoffs/DeepSeek/D00_interface_contract_2026-11-15.md`](../../handoffs/DeepSeek/D00_interface_contract_2026-11-15.md).
The shared review value types are in
[`include/roclint/review/ComprehensionReview.hpp`](../../include/roclint/review/ComprehensionReview.hpp).
The QML adapter `CompanionContract.qml` binds those names directly and hides
surfaces when a host does not provide them; it does not invent fallback content.

## 3. Measured baseline (unchanged `d5c8101`)

Source: `docs/audit/companion_quality_measurement_2026-11-15.md` and raw
`docs/audit/companion_evidence_2026-11-15/`.

- **Music-app acceptance prompt:** Julia scored 0.1366 ACTION → ORIGINAL;
  Granite was never called; after 40.5 s the HUD said "Ready to use" with no
  typography advice.
- **Explicit music review:** 113.3 s to the first question on a cold, contended
  host (Julia 36.0 s + Granite 64.2 s + discovery 12.9 s). The question drifted
  to the era fusion; the proposal dropped "not fewer features or shortened
  labels" and added a circular "most important aspect of the ... fusion" clause.
  Audit Unresolved; apply blocked.
- **500 ms / 95% / unchanged schema:** Julia RULE_CONFLICT 0.9999; asked about
  the 95% context and offered unrelated database/caching tactics.
- **Skip:** proposal identical to the original. **"I don't know":** proposal
  became a meta-instruction ("Determine the specific mood categories..."). Both
  were non-approvable but not useful.
- **Warm latency (contended):** discovery median 13.28 s (10.67–17.51), rewrite
  median 7.09 s (6.29–16.88), clarification check median ≈5.42 s. Cold readiness
  dominated: Julia 36.0–40.2 s, Granite 64.2–100.9 s.
- Editorial baseline: 1 pass / 3 partial / 8 fail / 1 not exercised across 12
  scenarios. The conversational-skill comparison is **NOT RUN** (no competitor
  access); the reproducible protocol is shipped.

These are Windows, contended-host, small-sample development measurements. No
percentile guarantee follows.

## 4. After-change evidence

Full details, raw paths and limitations are in
[the combined after-run report](companion_after_evidence_2026-11-15.md).

Key results on the combined build (one cold process, warm siblings, Windows,
Q8_0/Vulkan, thinking off):

| Scenario | Before | After |
|---|---|---|
| Music app, automatic | ORIGINAL after 40.5 s, no typography advice | ORIGINAL remains; explicit Explore now revives a bounded interview |
| Music app, explicit | 113.3 s cold to question; fusion drift; dropped "not fewer features or shortened labels" | 119.8 s cold to a typography-focused question; custom answer preserved (token overlap 0.952); negative constraint and one-tap browsing retained; independent review ready, no findings |
| Cinematic | "cinematic with high drama" (generic), model Pass | concrete side lighting + shallow depth of field; independent review ready, no findings |
| 500 ms / 95% / unchanged schema | false RULE_CONFLICT question after 12.4 s with unrelated database/caching options | 0.48 s, no question, no Granite call, exact values retained |
| Skip / "I don't know" | meta-instructions or an identical proposal | both remain open; a later custom answer is the only confirmed category wording and is carried into the rewrite |
| Changed preference | not exercised in baseline | explicit supersession of an 80s choice by a muted 1970s palette; final text uses the new preference (successful live sample in `companion_evidence_after_fix_2026-11-15`) |
| Independent review | no comprehension stage | fresh-context review caught a generated meta-instruction as blocking in the first after attempt; valid music/cinematic rewrites passed with source-linked readiness |

Warm first-question time was 5.37 s (cinematic), 3.84 s (gallery) and 6.83 s
(changed preference); warm rewrite 2.39–3.41 s; warm review 4.56–5.53 s. Cold
readiness still dominates (Julia 44.3 s + Granite 70.1 s). These are single-run,
contended Windows measurements, not percentile guarantees.

The first after attempt exposed and fixed: a missing native attempt identity in
the interview custom check, an unsupported `granite_interview` helper operation,
a probe review-state race, a first-sentence-only meta-instruction guard, and a
review-card Apply path that ignored pending audit acknowledgments. Those fixes
are in the combined commit; the retained failure evidence is under
`companion_evidence_after_2026-11-15/`.

## 5. UI evidence

Offscreen/headless QML renders from `docs/audit/companion_ui_2026-11-15/` show
the compact word card (ready, clarification, long/narrow), the substantial
interview (question, understanding, narrow 360 px), and the review surface
(not run, stale, ready with findings). These are labeled QML test-double
renders, not full-app screenshots. Real-host dry-run and real-host interaction
evidence are recorded separately once the combined build is exercised.

## 6. Verification

### Build

CMake/Visual Studio 2022 Release with `BUILD_TESTING=ON`, native Granite ON
(pinned llama.cpp `da2ee1e`, Vulkan), Q8_0 model present for the token-budget
test, `ROCLINT_DEPLOY_QT=OFF` for the integration tree. `roclint_host`,
`roclint_granite_worker`, `roclint_granite_contracts` and all registered C++
test targets built successfully. Raw build logs are under `build/` in the
integration worktree.

### Complete CTest

```
ctest --test-dir D:\Stardust\roclint-wt-integration\build -C Release --output-on-failure
```

**65/65 enabled tests passed, 0 failed, 334.83 s.** 66 tests are registered;
`roclint_retrieval_integration` is disabled because real embedding assets are
not present. Log: `build/final-ctest-2.log`. This includes
`roclint_product_alignment` (91.19 s), `roclint_host_controller` (37.28 s),
`roclint_ast_freshness_integration` (38.19 s), `roclint_dual_mode` (14.17 s),
`roclint_native_granite` (27.79 s), `roclint_e2e` (8.10 s) and the complete
retrieval/rewrite/storage gates.

### Python, structure and whitespace

- `python -m unittest discover -s tools -p "test_*.py"`: **86 tests, 1 skipped,
  48.63 s, OK**. The standalone run needs Qt on `PATH` and
  `ROCLINT_CONTRACTS_TOOL`; CTest configures its own test environment.
- `python tools/check_structure.py`: **passed, 94/94 original paths preserved,
  0 errors, 649 source files.** The new evaluation/docs directories are recorded
  in `docs/alignment_baseline.json`; the standalone probe and synthetic fixture
  are documented as evaluation-only in `CMakeLists.txt`.
- `git diff --check` and `git diff --check d5c8101..HEAD`: clean.
- Offscreen startup: `roclint_host --dry-run --db :memory:` exit 0, all four QML
  roots loaded with no QML warnings.

### UI and real-model evidence

- Real-controller QML smoke test loads `CompanionInterview` from the actual
  `HostController` and renders the question/choices:
  `docs/audit/companion_ui_2026-11-15/companion_interview_real_controller.png`.
- The full `roclint_product_alignment` suite (including the real-controller
  interview test, manual-remainder custom answer, review gates and the legacy
  keyboard flow) passed in the complete suite.
- Live after-run: 5 scenarios, 179.7 s, final code in
  `docs/audit/companion_evidence_after_final_2026-11-15/` and
  `companion_after_evidence_2026-11-15.md`. A successful changed-preference
  sample is in `docs/audit/companion_evidence_after_fix_2026-11-15/`.
- Native insertion/readback on a disposable Windows field was **not rerun** on
  this combined build. Shared tests use the scripted accessibility bridge and
  exact-readback policy. The installed application was not replaced or
  terminated.
- macOS/Metal, Apple Clang, TCC/signing and IDE-provider parity: **NOT RUN**.

## 7. Remaining limitations

- The automatic ORIGINAL route still does not volunteer a typography question
  for the music prompt. Explicit Explore now produces one; the automatic HUD
  remains quiet for that classifier result.
- One music interview option makes a reference-specific creative proposal about
  "Revolver-era song titles." It is disclosed as a proposed assumption, but it
  is not verified artwork fact and the final rewrite does not rely on it.
- The live changed-preference sample comes from the second after run; the final
  run's model generated a different question and the fixture's fixed pick no
  longer matched. Host tests cover supersession and invalidation.
- No automatic repair follows a blocking review finding. The user returns to
  the interview or keeps the original, within the bounded-repair rule.
- Independent review is an explicit step after a substantial rewrite; apply is
  locked until it completes. It is not an extra automatic call on every edit.
- Native insertion/readback on a disposable Windows field was **NOT rerun** on
  this combined build. Shared tests use the scripted accessibility bridge and
  exact-readback policy; the user's prior native insertion confirmation belongs
  to the installed build, not this binary.
- Native macOS build, Apple Clang, Metal, TCC/signing and native AX range
  insertion remain **NOT RUN**. Mac `injectRange` still inherits `NotSupported`.
- Windows/shared tests do not establish macOS or IDE-provider parity.
- The popup is non-activating by design; keyboard access is via the HUD.
- No universal inline underlines or universal undo claim.
- The prefix-alignment gap observed by the runtime (157 withheld tokens) and the
  ~52 s cold SHA-256 verification are measured opportunities, not changed in
  this delivery; changing either requires an explicit quality/security decision.
- Cross-session/global preference memory, broad RAG and model changes are out of
  scope.
- A completed same-model-family review can still be wrong; findings and source
  links are evidence for the user, not a guarantee.
