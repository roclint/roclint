# Architecture remediation verification — 2026-11-08

## Verdict

The F01–F09 completion claim is not supported. The original fixes improve several happy paths and the four affected Release suites pass, but independent probes confirm six remaining issues. Do not treat this work as ready to close all nine findings.

Baseline: `D:\Stardust\roclint`, branch `feat/roc-hud-refresh`, HEAD `c22ac3018e6936dc23b090cbc359d7645630179c`, plus the agent's ten uncommitted source/test changes. This review changed no implementation or existing tests, did not deploy or restart the installed app, and did not commit or push.

## R01 — P1: Native alternative selection skips model audit

`app/host/HostController.cpp:3051–3055` calls `currentTransport(..., nullptr, ...)`. The native branch at line 1069 returns an empty transport whenever the ticket is null, independently of model readiness. The adapter then returns deterministic-only results (`src/bridge/GraniteGoldfishAdapter.cpp:203–206`), which can be Pass. The host/HUD exposes that Pass without distinguishing execution coverage.

The selection audit is also synchronous on the GUI thread; the base audit runs on a worker. A blocking non-native transport can stall the HUD. Supplying a valid native ticket alone is insufficient: the native worker rejects inference on the GUI thread.

Independent host receipt (synthetic generation/audit setup, native selection branch exercised after removing the test override; no native model inference claimed):

```text
selection_runtime|native=1
selection_thread|worker_audits=1|gui_audits=1|can_approve=1
native_selection_no_override|audit=Pass|can_approve=1|gui_audits=1|worker_audits=1
```

Required fix: run selection through an asynchronous, ticket-bound pipeline; keep approval unavailable until the selected candidate's audit/explicit uncertainty handling and successful save finish. Bind results to candidate hash, state version, workspace and session, and reject stale completions. Expose deterministic-only coverage honestly. Add native-route and delayed-transport tests, rather than testing only immediate synchronous overrides.

## R02 — P1: Session restore bypasses hard-uncertainty acknowledgment

`app/host/HostController.cpp:3421` invalidates downstream state and clears `m_lastAuditResult`. The restored proposal reaches ReadyForReview at line 3557 without restoring or rerunning its audit. `validateCurrentProposal` only enforces these findings when `m_lastAuditResult` exists (line 2660). Thus restoring the same session removes the new F09 gate.

Independent host receipt, without acknowledgment or approval:

```text
before_restore|audit=Unresolved|can_approve=0|pending=1
after_restore|audit=|can_approve=1|pending=0|has_proposal=1
```

Required fix: persist candidate-bound audit outcome, execution coverage and findings with the proposal, or require a fresh audit/review before restored proposals become approvable. A missing audit must not mean all audit requirements passed. Test restore/reopen before acknowledgment, including selected alternatives and unavailable/malformed audits.

## R03 — P1: Negative-boundary scope still leaks within a clause

`src/core/constraint_reconciler.cpp:233–244` treats any preservation phrase in a clause as preserving every target in that clause. Splitting a few conjunctions fixes the original period-separated reproduction but does not associate preservation with its object.

Hard user constraint: `Do not modify token validation`.

```text
boundary|text=Keep tests unchanged and modify token validation.|outcome=Fail|violations=1
boundary|text=Keep tests unchanged then modify token validation.|outcome=Pass|violations=0
boundary|text=Modify token validation without changing tests.|outcome=Pass|violations=0
```

Required fix: bind mutation/preservation evidence to the protected target, rather than adding an unbounded list of separators. When scope cannot be determined, preserve uncertainty instead of claiming deterministic success. Add these ordinary-language counterexamples and positive controls for genuine token-validation preservation.

## R04 — P1: Alternative generation still deletes requirements

`src/rewrite/CreativeExpansion.cpp:75–84` discards a whole sentence if it contains a prohibition, including positive requirements in the same sentence. With `Refactor Reader::open for readability, but do not modify tests.`, both Step-by-Step and Conservative lose `Reader::open` and readability, replacing them with generic task language.

The generic Concise branch at line 257 still truncates to the first sentence. Given `Write a concise release note explaining the change for ordinary users. Do not disclose internal codenames.`, it drops the complete prohibition. These alternatives are now labeled Unresolved initially, which is an improvement, but generation still fails the fidelity requirement and R01 can promote a selected candidate through deterministic-only checking.

```text
mixed_sentence|candidate=alt_prose|target=1
mixed_sentence|candidate=alt_steps|target=0
mixed_sentence|candidate=alt_conservative|target=0
concise_boundary|candidate=alt_base|prohibition=1
concise_boundary|candidate=alt_concise|prohibition=0
```

Required fix: preserve all positive and negative requirements across every candidate family, including mixed sentences and generic prompts. Skip an alternative when it cannot be produced without losing required meaning. Test exact target, objective, multiple constraints, and visual/generic/refactoring families through the selected-candidate pipeline.

## R05 — P2: Advisory-only uncertainty still blocks approval

`src/bridge/GraniteGoldfishAdapter.cpp:292–297` filters an Advisory unresolved finding, but lines 336–342 recreate it as generic `goldfish_audit` uncertainty because the original verdict was Unresolved. The host then requires acknowledgment, granting a filtered Advisory finding blocking authority.

Frozen constraint: Model/Assumed/Advisory `optional_glow`. Model response: Unresolved with only that ID, no violations.

```text
advisory_unresolved|outcome=Unresolved|unresolved=1|id=goldfish_audit
```

Required fix: distinguish explicit verdicts with no usable explanation from findings deliberately filtered by authority/applicability. Advisory-only uncertainty must not be upgraded into mandatory review; unknown IDs and malformed responses should retain honest uncertainty. Test both Advisory Fail and Advisory Unresolved plus mixed hard/Advisory and inactive conditional cases.

## R06 — P2: Malformed finding records can still become Pass

`src/bridge/GraniteGoldfishAdapter.cpp:142–174` makes both arrays optional and silently drops empty finding objects. The new enum validation rejects `{}` and unknown verdicts but still accepts:

```text
parser_shape|text={"outcome":"Pass"}|accepted=1|outcome=Pass
parser_shape|text={"outcome":"Pass","violations":[{}],"unresolved":[]}|accepted=1|outcome=Pass
```

Required fix: validate the full response schema at the parser boundary, including required arrays and required nonempty finding fields, before merging outcomes. Return Malformed/Unresolved rather than silently erasing invalid evidence. Native grammar constraints may prevent these shapes in constrained generation; they do not validate every other transport or direct parser caller.

## Verification performed

- Rebuilt Release core, testing host library, and four affected test executables successfully.
- CTest: constraint reconciler, Granite Goldfish adapter, creative expansion and host controller — **4/4 PASS**, 15.09 s.
- Compiled and ran independent core and offscreen host probes against the rebuilt libraries; both exited 0. Their printed observations above deliberately demonstrate bugs, rather than asserting the desired behavior.
- `git diff --check` passed; Git printed line-ending conversion notices.
- No full CTest run, real Granite/native inference, native UI/clipboard insertion, installed-app acceptance session, or macOS verification was performed. The GUI-thread observation uses an instrumented synthetic transport. The null-ticket native transport bypass is established by production control flow and an isolated host lifecycle probe.

Evidence: `architecture_remediation_review_core_2026-11-08.log` and `architecture_remediation_review_host_2026-11-08.log` in this directory. Disposable probe sources, response files and executables are `build/manual-testing/remediation-review-core.*` and `remediation-review-host.*`. They use synthetic prompts, an in-memory database, temporary settings and offscreen Qt, and do not approve/copy/insert text.

Fix R01/R02 first, then R03/R04, then the authority/schema cases. Finish with all affected regression tests, a full current build/CTest pass, and a real native-runtime selection/restore acceptance check. Preserve the existing uncommitted work and unrelated research assets; update the completion report only after these cases are covered.
