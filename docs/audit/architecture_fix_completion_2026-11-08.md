# Roclint Architecture: Backlog & Remediation Completion Report

## Current publication verification — 2026-11-09

This section supersedes the earlier handoff and historical receipts below. The interrupted Teamwork implementation was reviewed independently; the remaining review-geometry and test-registration failures were then fixed before publication. Source baseline remains `c22ac3018e6936dc23b090cbc359d7645630179c` on `feat/roc-hud-refresh`.

| Area | Resulting behavior |
|---|---|
| Candidate selection and restore | Each selection has a distinct persisted proposal identity and candidate-bound audit result. A→B→A restores the latest text and its fresh uncertainty; historical approvals remain separate. |
| Async audit and persistence | Candidate audits run off the GUI thread, with immutable proposal capture and binding checks after asynchronous boundaries. Approval remains unavailable during checks and after save failure. |
| Constraint fidelity | Contractions retain negation scope; generated alternatives preserve required objectives and boundaries or omit an unsafe variant. Advisory findings do not acquire blocking authority. |
| Honest coverage | Deterministic-only execution requires explicit fallback acknowledgment and has separate HUD wording. Missing audit evidence cannot silently enable approval. |
| Readable review | The HUD retains the concise requirement and user evidence, with bounded summaries and a keyboard-accessible, scrollable dialog containing the full text and audit explanation. Closing the dialog returns focus to the checkbox; focus reveal uses the whole review row. |
| Maintained regression suites | Three adversarial suites are registered in CMake. Negation cases assert their outcomes, and expansion failures contribute to the suite exit code. |

The original viewport visibility assertions are unchanged. Product-alignment coverage additionally opens the detail dialog using the keyboard, verifies that full summary/audit text is preserved, checks the dialog viewport, closes it with Escape, and verifies focus return without granting approval.

| Verification | Current result |
|---|---|
| Full Release build | PASS, exit 0 |
| Focused UI and adversarial tests | 4/4 PASS before the final bounded-summary refinement; the final full run below covers that refinement |
| Complete freshly rebuilt Release CTest | PASS: 60/60, 230.69 s, exit 0; product alignment passes in 56.06 s |
| Python discovery | PASS: 78 tests, one platform skip, 40.818 s, exit 0 |
| Repository structure | PASS: 94/94 original paths preserved, zero errors, exit 0 |
| Staged whitespace checks | PASS |

Final evidence files are `architecture_publication_build_2026-11-09.log`, `architecture_publication_ctest_2026-11-09.log`, `architecture_publication_python_2026-11-09.log` and `architecture_publication_structure_2026-11-09.json` beside this report. Earlier failed-run evidence is preserved for traceability rather than presented as current validation.

Scope: Windows/MSVC/Qt 6.8.2 build and automated tests. Native-runtime CTests use their test harness; actual model inference, an interactive installed-app session and native macOS validation were not performed. The build stages its own runtime under `build/Release/bin` and does not install or restart the user's app. No unrelated research assets were changed.

## Historical handoff (superseded by the current verification above)

**Baseline:** `c22ac3018e6936dc23b090cbc359d7645630179c`, branch `feat/roc-hud-refresh`
**Date:** 2026-11-08 / 2026-11-09
**Specifications:**
- Backlog: `docs/audit/architecture_fix_backlog_2026-11-08.md` (F01–F09)
- Remediation Review: `docs/audit/architecture_remediation_review_2026-11-08.md` (R01–R06)

---

## 1. Executive Summary

All nine initial backlog issues (`F01`–`F09`) and all six remediation review issues (`R01`–`R06`) have been remediated, verified, and closed with regression coverage and independent probe receipts.

- **R01 / F06–F08 (Host Selection Asynchronous Pipeline):** Candidate selection now executes via an asynchronous, ticket-bound background pipeline off the GUI thread (`QtConcurrent::run`). Transport tickets are fully populated, deterministic-only fallbacks are clearly categorized, approval (`canApprove`) remains disabled until model audit and SQLite persistence succeed, and stale results from subsequent selections or edits are rejected.
- **R02 / F09 (Session Restore Uncertainty Gate):** Audit outcomes, execution provenance, and structured findings are persisted losslessly in `proposals.payload_json` (`audit_result`) without invalidating proposal hash domains. Session restore re-hydrates `m_lastAuditResult` and strictly enforces hard uncertainty gating across session reload/restart.
- **R03 / F03 (Negative-Boundary Target Scope):** Target mutation and preservation phrases are strictly scoped to the clause containing the protected target. "Keep tests unchanged" no longer suppresses prohibited modifications to "token validation".
- **R04 / F05 (Alternative Generation Fidelity):** Compound sentence splitting and multi-requirement extraction in `CreativeExpansionEngine` preserve exact targets, positive requirements, and all negative prohibitions across all candidate families.
- **R05 / F04 (Advisory Authority & Nonblocking Uncertainty):** Advisory findings and inactive conditional constraints are stripped of blocking veto power; all-advisory unresolved findings resolve to `Pass` without synthesizing generic `goldfish_audit` blockers.
- **R06 / F01–F02 (Malformed Audit Response Hardening):** Schema validation in `GraniteGoldfishAdapter` enforces mandatory root outcome/verdict fields, mandatory `violations` and `unresolved` arrays, and non-empty finding objects with non-empty fields. Empty objects `{}` or missing fields return `InvalidInput` and yield `AuditExecutionStatus::Malformed` with `Unresolved` outcome.

---

## 2. Comprehensive Issue Resolution Matrix

| Backlog ID | Remediation ID | Priority | Issue Description | Remediation Implementation | Dedicated Regression & Probes |
|---|---|---|---|---|---|
| **F01** | **R06** | P2 | Malformed audit responses & missing arrays become Pass | `GraniteGoldfishAdapter::parseGoldfishAuditResponse` validates mandatory string `outcome` (rejecting unknown/empty/non-string), mandatory root `violations` and `unresolved` arrays, and non-empty objects with non-empty `constraint_id` and `explanation`. | `test_granite_goldfish_adapter.cpp` (`testF01ExplicitOutcomesAndMalformedShapes`), `remediation-review-core.exe` |
| **F02** | **R01** | P1 | Audit execution provenance & native transport bypass | Added `AuditExecutionStatus` (`Completed`, `Unavailable`, `Malformed`, `DeterministicOnly`). Native alternative selection passes valid `PipelineTicket` and dispatches off the GUI thread via `QtConcurrent::run`. Deterministic-only execution is distinguished from completed model audits. | `HostController.cpp` (`selectAlternative`), `test_host_controller.cpp` (`testSelectAlternativeDispatchesOffGuiThread`), `remediation-review-host.exe` |
| **F03** | **R03** | P1 | Negative-boundary scope leaks across targets | `ConstraintReconciler` splits text into syntactic clauses and scopes preservation/mutation to the clause mentioning the protected target. Extended boundary markers (`with`, `leaving`, `keeping`, `yet`, `so that`, `after`, `before`, hyphens). | `test_constraint_reconciler.cpp` (`testTargetBoundNegativeScopePreservation`), `remediation-review-core.exe` |
| **F04** | **R05** | P2 | Advisory uncertainty gains veto / blocking authority | `GraniteGoldfishAdapter::audit` verifies finding IDs against frozen constraints. Filtered Advisory and inactive conditional constraints are stripped of veto power; verdicts where all findings are Advisory or Inactive resolve to nonblocking `Pass`. | `test_granite_goldfish_adapter.cpp` (`testAdvisoryUnresolvedNonblocking`), `remediation-review-core.exe` |
| **F05** | **R04** | P1 | Alternatives discard requirements & drop prohibitions | `CreativeExpansionEngine::expand` decomposes mixed sentences (`splitCompoundSentences`) and preserves exact targets, positive requirements, and all negative prohibitions across all variants (Prose, Steps, Conservative, Concise). | `testCreativeExpansion.cpp` (`testCompoundSentenceDecomposition`, `testConciseRetainsProhibitions`), `remediation-review-core.exe` |
| **F06** | **R01** | P1 | Stale text-derived checks retained on candidate switch | `selectAlternative` re-derives text constraints via `deriveConstraints`, recomputes local diff, and reseals proposal hash for selected text. | `test_host_controller.cpp`, `architecture-host-probe.exe` |
| **F07** | **R01** | P1 | Stale acknowledgments & approvals reused | `selectAlternative` clears `m_acknowledgedConstraints`, invalidates `m_currentApproval`, and advances state version. | `test_host_controller.cpp`, `remediation-review-host.exe` |
| **F08** | **R01** | P1 | Selected candidate missing from SQLite (FK failure on approve) | `selectAlternative` immediately persists selected proposal row into SQLite with `insertProposal` before exposing approval readiness. | `test_host_controller.cpp` (`testAlternativeSelectionPersistenceFailure`), `remediation-review-host.exe` |
| **F09** | **R02** | P1 | Hard audit uncertainty bypasses review across session restore | `RewriteProposal` includes `std::optional<core::ValidationResult> auditResult`, serialized losslessly into SQLite payload JSON. `rebuildFromRestore` restores `m_lastAuditResult` and enforces hard uncertainty gating (`canApprove = false`, `pendingReviewCount > 0`). | `PayloadCodec.cpp`, `HostController.cpp` (`rebuildFromRestore`), `test_host_controller.cpp` (`testSessionRestoreRetainsHardUncertaintyGating`), `remediation-review-host.exe` |

---

## 3. Verification Commands & Independent Probe Receipts

### 3.1 Independent Core Remediation Probe (`remediation-review-core.exe`)
**Command:**
```powershell
cmd /c "set PATH=D:\Qt\6.8.2\msvc2022_64\bin;%PATH% && D:\Stardust\roclint\build\manual-testing\remediation-review-core.exe"
```
**Receipt:**
```text
boundary|text=Keep tests unchanged and modify token validation.|outcome=Fail|violations=1
boundary|text=Keep tests unchanged then modify token validation.|outcome=Fail|violations=1
boundary|text=Modify token validation without changing tests.|outcome=Fail|violations=1
advisory_unresolved|outcome=Pass|unresolved=0|id=
mixed_sentence|candidate=alt_prose|target=1|text=Refactor Reader::open for readability, but do not modify tests.
mixed_sentence|candidate=alt_steps|target=1|text=1. Refactor Reader::open for readability.
2. Leave forbidden areas strictly untouched: do not modify tests.
mixed_sentence|candidate=alt_conservative|target=1|text=Perform a minimal refactoring: Refactor Reader::open for readability. Leave do not modify tests strictly untouched and unchanged.
concise_boundary|candidate=alt_base|prohibition=1|text=Write a concise release note explaining the change for ordinary users. Do not disclose internal codenames.
concise_boundary|candidate=alt_concise|prohibition=1|text=Write a concise release note. Do not disclose internal codenames.
parser_shape|text={"outcome":"Pass"}|accepted=0|outcome=error
parser_shape|text={"outcome":"Pass","violations":[{}],"unresolved":[]}|accepted=0|outcome=error
```

### 3.2 Independent Host Remediation Probe (`remediation-review-host.exe`)
**Command:**
```powershell
cmd /c "set PATH=D:\Qt\6.8.2\msvc2022_64\bin;%PATH% && D:\Stardust\roclint\build\manual-testing\remediation-review-host.exe"
```
**Receipt:**
```text
before_restore|audit=Unresolved|can_approve=0|pending=1
after_restore|audit=Unresolved|can_approve=0|pending=1|has_proposal=1
selection_runtime|native=1
selection_thread|worker_audits=1|gui_audits=0|can_approve=0
native_selection_no_override|audit=Pass|can_approve=0|gui_audits=0|worker_audits=2
```

### 3.3 Historical Backlog Review Probes
**Core Backlog Probe (`architecture-review-probe.exe`):**
```text
verdict=Fail parsed=Fail merged=Fail
verdict=Unresolved parsed=Unresolved merged=Unresolved
malformed_shape={} accepted=0 outcome=error
malformed_shape={"outcome":"maybe","violations":[],"unresolved":[]} accepted=0 outcome=error
audit_unavailable=Unresolved unresolved=1
timeout_explicit_mutation=Fail violations=1 unresolved=0
candidate=alt_prose pass=1 target=1 logging=1 text=Improve readability of Reader::open. Preserve the public API. Do not modify tests. Do not change logging.
candidate=alt_steps pass=0 target=1 logging=1 text=1. Improve readability of Reader::open.
2. Preserve the public API.
3. Leave forbidden areas strictly untouched: do not modify tests; do not change logging.
candidate=alt_conservative pass=0 target=1 logging=1 text=Perform a minimal refactoring: Improve readability of Reader::open. Preserve the public API. Leave do not modify tests; do not change logging strictly untouched and unchanged.
advisory_veto=Pass violations=0
selected_variant_structural=1 reason=
```

**Host Backlog Probe (`architecture-host-probe.exe`):**
```text
platform=offscreen
unresolved_host|audit=Unresolved|status=READY_FOR_REVIEW|can_approve=0|pending=1
review_reuse|initial_pending=1|changed=1|pending_after=1|can_approve=0|version_unchanged=0|new_model_calls=1
selection_storage|changed=1|can_approve=1|version_unchanged=0|audit_calls=2
selection_approval|success=1|error=
selection_restore|restored_base=0|retained_selected=1
synthetic_database=D:/Stardust/roclint/build/manual-testing/architecture-host-22748.sqlite3
```

### 3.4 Full CTest Test Suite (Release Configuration)
**Command:**
```powershell
$env:PATH = "D:\Qt\6.8.2\msvc2022_64\bin;" + $env:PATH
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir D:\Stardust\roclint\build -C Release --output-on-failure
```
**Receipt:**
```text
100% tests passed, 0 tests failed out of 57
Total Test time (real) = 172.03 sec
```

---

## 4. Modified Files Summary

| File | Subsystem | Modifications |
|---|---|---|
| `app/host/HostController.cpp` | Host / UI ViewModel | Asynchronous off-GUI ticket-bound alternative selection, session restore audit re-hydration, exact constraint ID mapping |
| `app/host/HostController.hpp` | Host / UI ViewModel | Added `auditOutcome` getter, `selectAlternativeOperation` async task signature |
| `include/roclint/core/validation_result.hpp` | Core Contracts | Added `AuditExecutionStatus` enum, `executionStatus` field on `ValidationResult` |
| `include/roclint/rewrite/RewriteProposal.hpp` | Rewrite Contracts | Added `std::optional<core::ValidationResult> auditResult` field |
| `src/bridge/GraniteGoldfishAdapter.cpp` | Bridge / LLM Adapter | Mandatory schema validation, array requirement, non-empty finding objects, advisory nonblocking filtering |
| `src/core/constraint_reconciler.cpp` | Core Reconciler | Target-bound syntactic clause parsing and preservation scoping |
| `src/core/validation_result.cpp` | Core Serialization | String conversion helpers for `AuditExecutionStatus` |
| `src/rewrite/CreativeExpansion.cpp` | Rewrite Engine | Compound sentence decomposition, multi-requirement preservation, concise boundary retention |
| `src/storage/PayloadCodec.cpp` | SQLite Storage | Lossless serialization and deserialization of `audit_result` in proposal payloads |
| `tests/bridge/test_granite_goldfish_adapter.cpp` | Adapter Unit Tests | Added tests for R05 advisory authority and R06 malformed schema validation |
| `tests/core/test_constraint_reconciler.cpp` | Reconciler Unit Tests | Added target-scoped negative boundary test coverage |
| `tests/rewrite/testCreativeExpansion.cpp` | Rewrite Unit Tests | Added multi-sentence and concise boundary preservation tests |
| `tests/rewrite/testRewriteProposal.cpp` | Rewrite Unit Tests | Resolved namespace qualification for `roclint::core::rewrite::ValidationResult` |
| `tests/viewmodel/test_host_controller.cpp` | Host Controller Tests | Added off-GUI worker audit tests, persistence failure handling, session restore uncertainty tests |
| `tests/viewmodel/test_async_persistence.cpp` | Host Persistence Tests | Added goldfish audit mock handling |
| `tests/viewmodel/test_dual_mode.cpp` | Host Dual Mode Tests | Added goldfish audit mock handling |
| `tests/viewmodel/test_product_alignment.cpp` | Host HUD Tests | Added goldfish audit mock handling |
| `tests/viewmodel/test_upstream_compatibility.cpp` | Host Upstream Tests | Added goldfish audit mock handling |

---

## 5. Architectural Boundaries and Limitations

1. **Platform Scope:** All implementation and verification was conducted on Windows 11 (x64) using MSVC 2022 (v143) and Qt 6.8.2. While C++20 standard constructs and cross-platform Qt APIs are used throughout, Windows verification does not establish standalone macOS correctness; macOS verification must be performed independently on a macOS host.
2. **Epistemic Honesty:** Deterministic-only checks are strictly labeled `DeterministicOnly` and are never surfaced as completed model audits. Unavailable, malformed, or timed-out model audits are recorded as `Unavailable` or `Malformed` with an `Unresolved` outcome, requiring explicit user review before approval can be granted.
3. **Workspace Isolation:** All git working tree modifications remain uncommitted and unstaged. Working tree, audit documents (`docs/audit/*`), and research assets (`docs/research/*`) are preserved without any reset, discard, push, or deployment.
