# AST workspace, rendering and stale-recovery remediation

**Review inputs:** `build/ast-additional-review.md`, `build/ast-remediation-rereview.md`.  
**Baseline:** `240c74f` plus the uncommitted AST working tree.  
**Scope:** pure AST grounding reliability.

## Ownership and sequencing

| Owner | Exclusive files |
|---|---|
| Specialist A | `app/host/HostController.cpp`, `app/host/HostControllerWorkspace.cpp`, `tests/viewmodel/test_ast_freshness_integration.cpp` |
| Specialist B | `include/roclint/rewrite/AstGroundingEngine.hpp`, `src/rewrite/AstGroundingEngine.cpp`, `tests/storage/testAstGroundingEngine.cpp` |
| Specialist C (after A) | `app/host/HostControllerWorkspace.cpp`, `app/host/CodebaseWatcher.hpp`, `app/host/CodebaseWatcher.cpp`, `tests/viewmodel/test_ast_stale_recovery.cpp` |
| Coordinator | `app/host/HostController.hpp`, `CMakeLists.txt`, `docs/**`, final integration |

A and C both need `HostControllerWorkspace.cpp`; A runs first, then C takes exclusive ownership. No
concurrent edits. C must not change A's workspace-epoch, ticket, proposal, or failed-revalidation
semantics. A adds the stale-recovery call site using the declarations below; C implements it.

## Workspace/scope epoch

`PipelineTicket` adds `std::uint64_t workspaceEpoch`. `HostController` owns
`std::uint64_t m_workspaceEpoch`. `makeTicket()` captures it and `isTicketCurrent()` requires equality.

- A different canonical workspace increments the epoch immediately.
- Re-selecting the same workspace path is an explicit refresh and does not increment it.
- A→B→A increments again, so a request from the older A epoch never revives.
- A deliberate active-file/scope change increments the epoch and invalidates current grounding and
  proposal artifacts rather than silently re-resolving existing evidence.
- Workspace change clears the pending proposal/analysis/approval synchronously through
  `invalidateDownstreamPipeline()`; persisted approval invalidation follows the existing repository
  contract. `canApprove()` requires the proposal's captured epoch to equal the current epoch.
- Grounding inputs/context carry `workspaceEpoch`; `applyGroundingContext()` ignores a context whose
  epoch is not current. Discovery/rewrite/ambient callbacks check ticket/epoch before applying views
  or publishing results.

## Failed revalidation

`GroundedPromptContext` distinguishes unavailability from a readable different generation:

- `available=false` (or an unavailable/zero-generation snapshot) after ownership checks means
  **current validation failed**, not superseded.
- A readable `snapshotGeneration` different from the captured/current generation means superseded
  and is discarded.
- Current failure invalidates the exact pending AST-bound proposal/approval, with the queued
  invalidation rechecking proposal/session/revision/workspace/epoch/serial.

## Renderer identity

A Verified block is emitted only if it contains a useful target identity. Optional symbol, location
and signature fields are dropped first. If the required target cannot fit with an explicit
truncation marker (or in full), the whole record is omitted. Omitted records never enter
`PromptRenderResult::rendered`, the AST hash or HUD evidence. No unmarked prefix may masquerade as a
complete identifier. The 350-byte bound, UTF-8/XML validity, body exclusion and v1/v2 compatibility
are unchanged.

## Stale-evidence recovery

`GroundedPromptContext` adds `std::vector<std::string> stalePaths` populated when rendered evidence
files fail hash validation. After A's epoch/ownership checks, `applyGroundingContext()` calls
`scheduleStalePathRecovery(context.stalePaths)`.

`HostController` owns bounded/coalesced recovery state:

```cpp
void scheduleStalePathRecovery(const std::vector<std::string>& relativePaths);
void resetStalePathRecovery();
QHash<QString, int> m_staleRecoveryAttempts;
```

- Paths are normalized, deduplicated, workspace-relative and validated.
- Hashing/parsing stays off the UI thread via the existing coalesced `startCodebasePathsRefresh`.
- Repeated detections while a refresh is pending/in flight coalesce; per-path attempts are bounded
  and backed off (maximum three until a successful validated grounding clears the path).
- Recovery from a superseded workspace/epoch is discarded.
- The stale request remains withheld. A later request after recovery uses the refreshed snapshot.
- Existing watcher recovery/overflow bounds are preserved. A controlled healthy fallback cadence may
  be used for missed creation/deletion events, but no per-keystroke full rescan.

## Final implementation notes

- Workspace and deliberate active-file scope changes increment the epoch; re-selecting the same
  workspace/scope does not. Grounding application requires exact epoch equality.
- Proposal acceptance/restore records the epoch; `hasProposal`, `canApprove`, approval context,
  acknowledgment, insertion targeting and approval persistence all reject a non-current epoch.
- Unavailable/zero-generation revalidation is a current failure (invalidate), while a readable
  different generation is superseded (discard).
- `PromptRenderResult::rendered` contains only records with useful target identity; omitted records
  do not enter the hash or HUD.
- `GroundedPromptContext::stalePaths` is populated only from rendered evidence whose saved hash
  failed. Recovery is coalesced through the existing 200 ms path queue, capped at three attempts per
  path, and reset by validated grounding. Healthy watchers arm the existing five-minute fallback.
- Verified on Windows: Release build PASS; 50/50 CTest; Python 49 (one platform skip); structure
  94/94; verification probe 9/9. Native macOS execution remains NOT RUN.
