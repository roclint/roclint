# Roclint architecture: fix-ready issue backlog

**Baseline:** `c22ac3018e6936dc23b090cbc359d7645630179c`, `feat/roc-hud-refresh`. **Date:** 2026-11-08.

This is a finding/implementation handoff, not a remediation claim. Nine implementation defects or assurance-policy gaps are confirmed below. No runtime, UI or existing test source was changed. Only this report and sanitized evidence were added; nothing was committed or pushed by this review. Scope is the supplied architecture brief and its candidate/audit/approval paths, not an exhaustive repository or security audit.

## Summary and order

| ID | Priority | Issue | Primary owning code |
|---|---|---|---|
| F01 | P1 | Explicit audit verdicts and malformed responses become Pass | `src/bridge/GraniteGoldfishAdapter.cpp:108`, `:251` |
| F02 | P1 | An unavailable model audit is indistinguishable from successful verification | `src/bridge/GraniteGoldfishAdapter.cpp:190` |
| F03 | P1 | An unrelated preservation phrase suppresses a prohibited edit | `src/core/constraint_reconciler.cpp:187` |
| F04 | P2 | A model can give an Advisory constraint veto power | `src/bridge/GraniteGoldfishAdapter.cpp:220` |
| F05 | P1 | Creative candidates discard intent/boundaries and inherit an unearned Pass | `src/rewrite/CreativeExpansion.cpp:148`, `app/host/HostController.cpp:2619` |
| F06 | P2 | Candidate selection retains checks derived from the previous text | `app/host/HostController.cpp:2942` |
| F07 | P1 | Candidate selection reuses review acknowledgments and does not advance state | `app/host/HostController.cpp:2933` |
| F08 | P1 | Selected candidates are not persisted: approval fails and restore loses them | `app/host/HostController.cpp:2944`, `src/storage/PromptRepository.cpp:324` |
| F09 | P2 | Hard audit uncertainty does not enter required review gating | `app/host/HostController.cpp:2596`, `:2654` |

Recommended sequence: establish the audit result/authority contract (F01–F04); correct candidate construction/validation (F05); fix selection as one versioned, durable operation (F06–F08); implement the explicit uncertainty policy (F09); then correct architecture documentation. F06–F08 share the same host lifecycle and should be implemented together. Preserve the current AST freshness, proposal hash, approval binding, runtime cancellation and telemetry behavior.

## F01 — Audit verdict/schema handling

**Confirmed reproduction:** the production core library parses schema-valid `{outcome: Fail, violations: [], unresolved: []}` as Fail but the adapter merges it into Pass. Unresolved with empty arrays also becomes Pass. The parser accepts `{}` and an unknown outcome `maybe` as Pass. Native grammar normally prevents the latter malformed shapes, but the Fail/empty-array response satisfies the current native schema.

**Change required:** validate required fields, enum values, field types and finding records. Do not default missing/unknown status to Pass. Preserve an authoritative non-Pass outcome, or classify a contradictory/incomplete response as unresolved/invalid; never silently turn it into successful verification. Do not let this fix give Advisory findings veto power (F04).

**Acceptance tests:** explicit Fail/Unresolved with empty arrays; missing/unknown/wrong-type outcome; missing/wrong-type findings; malformed/unknown finding IDs; contradictory verdict/findings. Assert both parser and adapter behavior, including the valid native-schema failure case.

## F02 — Audit execution failure loses provenance

**Confirmed reproduction:** a synthetic transport error returns `Pass`, with no unresolved finding or indication that the model audit did not execute. The same occurs with a prohibited mutation that slips through the heuristic in F03. Existing tests deliberately accept deterministic fallback, so this is also a gap between that fallback policy and the new architecture's assurance claim.

**Change required:** represent execution/coverage separately from semantic outcome, e.g. completed / unavailable / malformed / deterministic-only. Bind that state to the exact candidate hash and show the honest verification level. Best-effort rewriting may remain available; it must not be labeled as completed successful model verification. Apply the explicitly chosen hard-uncertainty policy in F09.

**Acceptance tests:** null transport, timeout, crash/provider-unavailable, malformed response, successful audit; prove no prior Pass leaks into a new candidate, retry or restored session. Deterministic-only checks remain useful without impersonating a completed semantic audit.

## F03 — Negative-boundary matching is not target-scoped

**Confirmed reproduction:** hard rule `Do not modify token validation`; candidate `Keep tests unchanged. Modify token validation.` The global `keep` match suppresses the mutation finding. A simulated audit timeout then yields Pass with zero violations/unresolved items.

**Change required:** associate preservation/negation with the protected target and relevant clause, not any phrase anywhere in the rewrite. Where the rule cannot be verified conservatively, retain uncertainty instead of inventing coverage. Do not broaden unrelated edits or use a model-origin preference as authority over a user prohibition.

**Acceptance tests:** preserve unrelated tests while mutating token validation; multiple prohibitions where only one is preserved; explicit compliant preservation of the protected target; mixed clauses; negation variants; omission/paraphrase uncertainty. Keep existing legitimate-preservation cases green and measure false positives as well as false negatives.

## F04 — Advisory findings can veto the user's request

**Confirmed reproduction:** the supplied constraint `optional_glow` is Model/Assumed/Advisory. A synthetic model response reports it as a violation; the adapter returns Fail with one violation. The merge never checks the frozen constraint's authority before applying veto power. Existing advisory tests only use a model that obeys the instruction and returns Pass.

**Change required:** resolve returned IDs against the frozen constraint set and apply authority/applicability rules in code. An Advisory finding may be reported as a nonblocking suggestion; it cannot reject an otherwise acceptable rewrite. Unknown IDs or inconsistent responses must not become authoritative hard violations. Coordinate with F01 so malformed or contradictory responses are classified honestly rather than silently promoted to verified Pass.

**Acceptance tests:** advisory-only alleged violation; inactive conditional constraint; known active hard violation; unknown ID; duplicate/conflicting IDs; mixed hard/advisory findings. Prove the model cannot upgrade a constraint's authority.

## F05 — Alternatives are not fidelity-validated/audited candidates

**Confirmed reproduction:** the Reader::open readability fixture has API, tests and logging boundaries. Step-by-Step/Conservative drop the exact target, API requirement and logging boundary, invent latency work, and still carry Pass. Refactoring templates use only `prohibitions.front()`; visual templates can replace the request wholesale. Expansion runs after the base audit; selecting another text does not invoke another audit.

**Change required:** build each candidate from the confirmed intent and complete invariant set. Preserve targets, all positive/negative boundaries and the user's actual objective. Avoid generic task replacement or unrequested aesthetics/performance requirements. Validate the final candidate's fidelity/profile/grounding; audit the exact selected text and bind the result to its hash. Unreviewed candidates must not default to Pass. An acceptable design may validate/audit lazily on selection, with approval unavailable until that selected candidate's checks finish.

**Acceptance tests:** two or more prohibitions; positive API/target requirements; readability without a speed request; visual prompts with explicit style prohibitions; generic/concise variants that would drop later sentences; base-to-alternative audit hash binding; stale/delayed results after another selection or workspace change. Retain explicit human delivery approval.

## F06 — Text-derived checks become stale on selection

**Confirmed reproduction:** a generated Step-by-Step alternative fails the production validator after selection/resealing with `Proposal constraint claims are not reproducible by the validator`. The old proposal's stored constraint claims no longer match `deriveConstraints()` for the selected text.

**Change required:** recompute local diff, text-derived constraints and all validation before making the selection approvable. A new hash alone is insufficient. Preserve the existing validator's refusal to trust stale claims. Ensure the highlighted selected pill actually corresponds to the displayed proposal; do not assume candidate zero always equals the base text.

**Acceptance tests:** changes in mechanically verified versus semantic-review-required claims; all profile families; selecting back to the base; unknown/out-of-range index; unchanged-text variants; candidate zero differs from base; UI canApprove and displayed review checks match the selected text.

## F07 — Old review acknowledgments authorize a changed text

**Confirmed host reproduction:** base proposal has one required review item. Acknowledge it, then select a different candidate whose derived item still requires semantic review. The proposal hash changes, but pending review remains zero, canApprove remains true, stateVersion is unchanged, and no new model call occurs.

**Change required:** make selection part of the normal serialized/versioned proposal lifecycle. Clear acknowledgments and approvals bound to the prior text; bind any new acknowledgment and audit to the selected hash/generation. Reject or cancel stale callbacks/commands when selection changes. Preserve the existing post-persistence proposal-ID/workspace guard in approval delivery.

**Acceptance tests:** acknowledge base then select another candidate; delayed acknowledgment/audit/approval callback after selection; double selection during validation; switch workspace/session; identical-text candidates; no delivery on a stale approval. Selection must not reuse old semantic review merely because constraint IDs are unchanged.

## F08 — Selected candidate is missing from persistent storage

**Confirmed host reproduction:** choose a different Step-by-Step candidate that passes structural checks. canApprove is true; audit call count stays one and version is unchanged. Approval fails with `SQLite step failed: FOREIGN KEY constraint failed` because the selected proposal ID has never been inserted. Restoring the session then restores the base hash, losing the selection.

**Change required:** validate and persist the selected proposal and session state before exposing approval availability. Persist/recover the selected identity/text and its bound verification state. On failed persistence, leave or restore a coherent previous selection with an actionable error. Do not weaken foreign keys or approval hash checks to make this pass.

**Acceptance tests:** every candidate can complete the normal approval path after its checks; saved approval references an existing proposal row; reopen/restore retains selected text/hash; failed save cannot expose an approvable unsaved text; delayed approval for a superseded selection cannot deliver. Use offscreen Qt/mock delivery to avoid a live desktop clipboard or editor.

## F09 — Unresolved hard audit findings bypass required review

**Confirmed host reproduction:** the model returns Unresolved with a finding for the known hard `intent_grp_0` requirement. The host publishes READY_FOR_REVIEW with `canApprove=1` and `pending=0`. The ordinary proposal validator/approval context does not require acknowledgment of this audit finding.

**Policy decision needed:** best-effort approval may be an intended feature; it is not a fully verified hard invariant. Choose and encode the rule rather than silently equating it with verified success. For a strict assurance route, block until resolved. If explicit best-effort review is allowed, require a clearly presented, candidate-bound acknowledgment of the hard uncertainty and preserve its unverified status. Advisory uncertainty must not gain veto power.

**Acceptance tests:** known hard unresolved versus advisory unresolved; no automatic acknowledgment; acknowledgment bound to candidate hash; new candidate/retry/restore cannot inherit it; UI and persisted status distinguish verified from accepted uncertainty. Test actual canApprove/approval behavior, not only the adapter enum.

## Related contract/documentation work (not additional runtime findings)

- The helper implements/advertises `reset_cache`, but the production host audit path never invokes it. Prompt differences can naturally prevent KV reuse; no actual cross-audit KV reuse is claimed by this review. Either implement an ordered reset barrier and prove zero reuse for the audit, including cancellation/error handling, or correct the claimed reset-based cold-audit guarantee. Reset context is not unloading model weights and does not make model verification a mathematical proof.
- Describe Julia's additional worker, the custom versioned JSON-over-stdio envelope, batched streaming, lifecycle ownership rather than an established security sandbox, actual Ctrl+Shift+R default, completed-decode wall time rather than pure GPU kernel timing, post-prefill generation rather than whole user-operation latency, AST freshness/evidence binding and explicit human approval/delivery.
- Remove unsupported absolute correctness and zero main-thread latency claims. The audit may increase confidence; hashes bind content and state rather than proving semantics.

## Evidence and verification

Sanitized receipts are alongside this report in `docs/audit/`:

- `architecture_fix_core_probe_2026-11-08.log`: calls the actual production core expansion, parser, reconciler, audit and validator functions.
- `architecture_fix_host_probe_2026-11-08.log`: runs actual HostController lifecycle methods with synthetic transports, isolated settings/database and offscreen Qt. No normal installed app was launched or edited; no live editor insertion or desktop clipboard delivery occurred. The attempted selected-candidate approval failed before delivery.
- `architecture_fix_existing_tests_2026-11-08.log`: Goldfish adapter, creative-expansion and host-controller targets all pass, **3/3**, **13.20 s total**. Existing tests miss the reproduced cases; this is not a whole-repository pass.

Disposable probe sources, compiler response files and build logs remain in `build/manual-testing/architecture-review-probe.*` and `architecture-host-probe.*`. The host probe uses the existing host-test helper setup, including a structurally valid `lower call latency` selection for F08, and a pending semantic-review base for F07. Transport errors/verdicts are injected fixtures; no real model failure or new inference performance benchmark is claimed.

## Implementation handoff requirements

1. Add failing regressions for each ID before modifying the relevant path. Reuse the existing core/host test helpers and a real temporary SQLite repository; avoid assertions limited to an arbitrary word such as `unchanged` or to hash changes alone.
2. Resolve F01/F04 and F02/F09 together so strictness does not grant Advisory constraints authority or remove the intentional best-effort route by accident.
3. Implement F06–F08 through one coherent selection transaction: bind input state; construct/validate the final text; clear stale review/approval; finish its audit or explicit uncertainty review; save the proposal/state; publish readiness. Discard stale asynchronous results and handle save failure without leaving a half-selected state. Internal API details may differ if these observable contracts hold.
4. Preserve model timing corrections, current waveform/UI work, pure-AST grounding and freshness protection, proposal v1/v2 compatibility, pinned dependencies, no-automatic-Send behavior, and unrelated research assets. Do not solve lifecycle bugs by removing validators, foreign keys or approval checks.
5. Build and run affected core, host, storage, approval, runtime and product-alignment targets; then the normal release verification gates. Report new failing reproductions separately from previously green tests and distinguish Windows from macOS evidence. Do not claim completion from static compilation alone.

Completion means every ID has a code/policy resolution, a meaningful regression and exact fresh verification. This report contains confirmed findings and acceptance requirements; the fixes themselves remain outstanding.
