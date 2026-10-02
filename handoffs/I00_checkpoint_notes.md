# Assignment I00 — Foundation Checkpoint Review & Handoff Record

- **Assignment:** [I00 — Foundation checkpoint](../../assignments/00_foundation/I00_foundation_checkpoint.md)
- **Owners:** Human A (Jesse K Ramon) + Human B (Kirby)
- **AI Mode:** REVIEWER
- **Status:** COMPLETE & VERIFIED
- **Product Milestone:** ✅ **Foundation ready: native shell and core workspace can evolve independently.**

---

## 1. Prerequisite Module Status

| Prerequisite | Owner | Platform / Baseline | Status | Handoff Record |
|---|---|---|---|---|
| **A00 — Core workspace** | Human A (Jesse) | macOS arm64 / Apple Clang / Ninja | COMPLETE | [`handoffs/Jesse/A00_handoff_notes.md`](Jesse/A00_handoff_notes.md) |
| **B00 — Desktop workspace** | Human B (Kirby) | Windows 11 x64 / MSVC 2022 / Qt 6.8.2 | COMPLETE | [`handoffs/Kirby/B00_handoff_notes.md`](Kirby/B00_handoff_notes.md) |

---

## 2. Integrated Toolchain Baselines

| Component | Human A (macOS) | Human B (Windows) |
|---|---|---|
| **Operating System** | macOS (Apple Silicon arm64) | Windows 11 x64 |
| **C++ Standard** | C++20 (`-std=c++20`) | C++20 (`/std:c++20`) |
| **Compiler** | Apple Clang 21.0.0 (clang-2100.1.1.101) | Microsoft Visual C++ 2022 (MSVC 19.44) |
| **Build System** | CMake 4.4.3 & Ninja 1.13.2 | CMake 3.25+ & MSBuild |
| **GUI Framework** | Qt 6 (Homebrew auto-detected) | Qt 6.8.2 LTS (`msvc2022_64`) |

---

## 3. Integration Walkthrough Verification

1. **Clean Configure & Build**:
   - `cadroc_core` static library compiles cleanly on both Clang and MSVC.
   - `cadroc_host` desktop executable compiles and links `cadroc_core` and `cadroc_host_lib`.
   - `cadroc_core_tests` and `cadroc_viewmodel_tests` compile and link against their respective libraries.

2. **Core / Host Boundary Wiring**:
   - `cadroc_host` links `cadroc_core` to access version metadata (`cadroc::core::get_version_string()`) without coupling to product semantics.
   - Strict boundary maintained: zero SQLite, zero model providers, zero prompt-state logic in the desktop shell.
   - Core library has zero dependencies on Qt, GUI, or desktop host headers.

3. **Lifecycle & Residency Invariant**:
   - HostController initializes in `Starting` and transitions to `Resident` upon successful QML engine load.
   - `QApplication::setQuitOnLastWindowClosed(false)` guarantees the resident host remains alive when the floating card is hidden or closed.
   - System tray (`QSystemTrayIcon`) provides persistent resident control.
   - Single-instance invariant: invoking show/open toggles or focuses the existing card instance rather than creating duplicates.
   - Explicit `Quit` via tray menu is the sole mechanism for process termination.

4. **Automated Test Evidence**:
   ```text
   Internal ctest changing into directory: D:/Stardust/Cadroc/build
   Test project D:/Stardust/Cadroc/build
       Start 1: cadroc_core_smoke
   1/2 Test #1: cadroc_core_smoke ................   Passed    0.04 sec
       Start 2: cadroc_host_lifecycle
   2/2 Test #2: cadroc_host_lifecycle ............   Passed    0.09 sec

   100% tests passed, 0 tests failed out of 2
   ```

5. **Dry-Run Non-Blocking Verification**:
   ```text
   [Cadroc Host] Version: 0.1.0
   [Cadroc Host] Core Version: 0.1.0
   [Cadroc Host] Host State: RESIDENT
   [Cadroc Host] Qt Version: 6.8.2
   [Cadroc Host] Dry-run passed. QML root object verified. Exiting cleanly.
   ```

---

## 4. Contracts Satisfied

- **[C01 Prompt State Contract](../../contracts/C01_prompt_state.md):** Clean boundary maintained; no prompt-state semantics introduced ahead of Stage 01.
- **[C05 Desktop View-Model Contract](../../contracts/C05_desktop_view_model.md):** Typed lifecycle states (`Starting`, `Resident`, `Quitting`) and card visibility states (`Hidden`, `Visible`, `Focused`).
- **[C08 Runtime & Packaging Contract](../../contracts/C08_runtime_packaging.md):** Native C++20 user-session resident host with system tray presence and explicit quit termination.
- **[C09 Invocation & Host Context Contract](../../contracts/C09_invocation_host_context.md):** System tray presence, non-duplicating single-instance card focus/toggle.

---

## 5. Reviewer Sign-Off & Handoff Summary

- **Prerequisite failures:** None. Both A00 and B00 met all acceptance criteria.
- **macOS Live QA:** Conducted by Jesse K Ramon (Human A) in [`handoffs/Jesse/I00_foundation_review.md`](Jesse/I00_foundation_review.md). Confirmed working card dragging, resident menu-bar presence, hide-on-esc, and clean termination.
- **QA Polish Implemented:**
  - Added native `Menlo` monospace font fallback for macOS (`rootWindow.monoFont`) alongside Windows `Consolas` for pixel-perfect ASCII rendering on both OSes.
  - Made `[-]` button minimize on Windows and hide to tray on macOS (frameless panels cannot dock-minimize on macOS).
- **Unresolved boundary issues:** None.
- **Bypassed authorship boundaries:** None. Human A owned core workspace; Human B owned desktop workspace.
- **Stage 00 Complete:** Foundation checkpoint **I00** is verified and marked **DONE**.
- **Next Stage Unlocked:** **Stage 01 — Prompt State** ([A01](01_prompt_state/A01_promptrevision.md) & [B01](01_prompt_state/B01_prompt_editor_ui.md)).
