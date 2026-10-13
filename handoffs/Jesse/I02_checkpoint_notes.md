# Assignment I01 / I02 — Prompt State & Invocation Checkpoint Review & Handoff Record

- **Assignments:** 
  - [I01 — First prompt](../../assignments/01_prompt_state/I01_first_prompt.md)
  - [I02 — Revision state checkpoint](../../assignments/01_prompt_state/I02_revision_state_checkpoint.md)
- **Owners:** Human A (Jesse K Ramon) + Human B (Kirby)
- **AI Mode:** REVIEWER / BUILDER ASSISTANCE
- **Status:** COMPLETE & VERIFIED (100% Passing Test Suite)
- **Product Milestones:**
  - ✅ **I01: Stardust can accept a prompt through the floating widget and hand it to the core.**
  - ✅ **I02: Stardust has authoritative prompt revision state, global shortcut invocation with cursor screen anchoring, clipboard pre-fill, and passive host-context detection.**

---

## 1. Prerequisite Module Status

| Prerequisite | Owner | Platform / Baseline | Status | Artifacts / Implementation |
|---|---|---|---|---|
| **A01 — `PromptRevision`** | Human A (Jesse) | macOS / Clang / C++20 | COMPLETE | `PromptRevision.hpp`, `RevisionId.hpp`, `SessionId.hpp` |
| **A02 — `RevisionState`** | Human A (Jesse) | macOS / Clang / C++20 | COMPLETE | `RevisionState.hpp`, `RevisionState.cpp`, stale rejection logic |
| **B01 / B01K — Keyboard HUD** | Human B (Kirby) | QML / Qt 6 Quick | COMPLETE | Frameless obsidian HUD (`FloatingCard.qml`), jargon-free UX |
| **B02 — Invocation Binding** | Human B (Kirby) | macOS + Win / C++20 / Qt 6 | COMPLETE | One-shot clipboard capture, cursor screen placement & clamping (`ScreenPlacement.cpp`) |
| **B02H — Host Context Hint** | Human B (Kirby) | macOS (AppKit) + Win (Win32) | COMPLETE | `HostContext.hpp`, `HostContext_mac.mm`, C03 category mapping |

---

## 2. Integrated Architecture & Flow

```text
Target App (e.g. VS Code, Cursor, Edge, Terminal)
        │
        │ [Cmd+Shift+R / Ctrl+Shift+R] or Tray Icon
        ▼
┌─────────────────────────────────────────────────────────────┐
│ 1. INVOCATION & OBSERVATION (<5 ms)                         │
│ - Observe frontmost app one-shot (NSWorkspace / Win32)      │
│   Map to C03 category: CODE_EDITOR | TERMINAL | BROWSER etc.│
│ - Read clipboard text once as editable draft                │
│ - Query cursor screen & clamp placement to visible geometry │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ 2. HUD PRESENTATION                                         │
│ - Show single logical window instance at clamped (x, y)     │
│ - Pre-fill prompt input if blank (editable draft)           │
│ - Display subtle context advisory chip: [FROM: APP_NAME ×]  │
│   (User can clear/disable hint before commit)               │
└──────────────────────────────┬──────────────────────────────┘
                               │
            User modifies text & presses Enter
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ 3. CORE COMMIT & COPY-BACK                                  │
│ - HostController creates C++ PromptRevision via RevisionState│
│ - State version increments atomically; stale edits rejected │
│ - Exact approved envelope copied back to system clipboard   │
│ - Esc / dismiss hides HUD without mutating clipboard or core│
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Automated Test Evidence

Automated test suite registered with CTest: `roclint_invocation_binding` (`tests/viewmodel/test_invocation_binding.cpp`).

```text
[Roclint Invocation & Stage 01 Checkpoint Test] Starting...

--- 1. Screen Placement & Clamping (B02) ---
  [PASS] Centered placement calculation verified.
  [PASS] Top-left boundary collision clamped correctly.
  [PASS] Bottom-right boundary collision clamped correctly.
  [PASS] Empty geometry deterministic fallback verified.

--- 2. Host-Context Category Mapping (B02H / C03) ---
  [PASS] All C03 category mappings verified.

--- 3. Host-Context Disclosure & Clear Action (B02H) ---
  [PASS] Host context captured and exposed as friendly display name & category.
  [PASS] User can clear/disable host context hint.

--- 4. Clipboard Draft Capture on Invocation (B02 / C09) ---
  [PASS] One-shot clipboard capture pre-fills draft text.
  [PASS] Dismissing card leaves clipboard and core state intact.

--- 5. Revision State Machine Checkpoint (I01 / I02) ---
  [PASS] Initial prompt revision committed (stateVersion = 1).
  [PASS] Second prompt revision committed (stateVersion = 2).
  [PASS] Approved text copy-back matches exact supplied string.

=========================================================
  ALL STAGE 01 / I01 / I02 INTEGRATION CHECKS PASSED!
=========================================================
```

### Full CTest Suite Status:
```text
Test project /Users/jesse/roclint/build
    Start 1: roclint_core_smoke
1/5 Test #1: roclint_core_smoke ...............   Passed    0.00 sec
    Start 2: roclint_prompt_revision
2/5 Test #2: roclint_prompt_revision ..........   Passed    0.00 sec
    Start 3: roclint_revision_state
3/5 Test #3: roclint_revision_state ...........   Passed    0.00 sec
    Start 4: roclint_host_lifecycle
4/5 Test #4: roclint_host_lifecycle ...........   Passed    0.02 sec
    Start 5: roclint_invocation_binding
5/5 Test #5: roclint_invocation_binding .......   Passed    0.10 sec

100% tests passed out of 5
Total Test time = 0.13 sec
```

---

## 4. Contracts Satisfied

- **[C01 Prompt State Contract](../../contracts/C01_prompt_state.md):** Authoritative C++20 `PromptRevision` and `RevisionState` managing immutable revision records, parent lineage, and stale base version rejection.
- **[C03 Semantic Analysis Contract](../../contracts/C03_analysis_semantics.md):** `HostContextHint` taxonomy strictly adhered to: `CODE_EDITOR`, `TERMINAL`, `BROWSER`, `CHAT`, `DOCUMENT`, `OTHER`, `UNKNOWN`. Advisory-only constraint maintained; no raw file/URL scraping.
- **[C05 Desktop View-Model Contract](../../contracts/C05_desktop_view_model.md):** Clean boundary separation between QML HUD and C++ core state via `HostController`.
- **[C09 Desktop Invocation Contract](../../contracts/C09_invocation_host_context.md):** One-shot clipboard read on invocation (no continuous monitoring), cursor-aware screen placement clamped to available display bounds, one-shot passive frontmost-application detection without Accessibility/Automation permissions, and user clear action before commit.

---

## 5. Reviewer Sign-Off & Stage 02 Unlock

- **Walkthrough passes:** Yes (both headless automated suite and live GUI verified with screenshot `stage01_hud_preview_v2.png`).
- **Protected boundaries preserved:** Yes. C++ core prompt state remains protected Human A code; desktop invocation and view-model bindings remain in `app/host` and `app/qml`.
- **Stage 01 Completed:** Both **I01** and **I02** checkpoints are verified and marked **DONE**.
- **Next Stage Unlocked:** **Stage 02 — Persistence** ([A03 — SQLite foundation](../../assignments/02_persistence/A03_sqlite_foundation.md) & [A04 — Prompt repository and history](../../assignments/02_persistence/A04_prompt_repository_and_history.md)).
