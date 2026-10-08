# Assignment B00 — Handoff Record

- **Assignment:** [B00 — Background host + tray/menu-bar shell](../../assignments/00_foundation/B00_desktop_workspace.md)
- **Role/Lane:** Human B (Builder Assistance Mode)
- **Status:** COMPLETE & VERIFIED LOCALLY

---

## 1. Frozen Toolchain Baseline

| Component | Frozen Version / Path |
|---|---|
| Operating System | Windows 11 x64 |
| C++ Standard | C++20 (`/std:c++20`) |
| Compiler | Microsoft Visual C++ 2022 (MSVC 19.44 / VS 2022 Community) |
| Build System | CMake 3.25+ & MSBuild / Ninja |
| GUI Framework | Qt 6.8.2 LTS (`win64_msvc2022_64`) |
| Qt Location | `D:\Qt\6.8.2\msvc2022_64` |

---

## 2. Standard Build & Run Commands

```powershell
# 1. Configure with Qt 6 prefix
cmake -B build -S . -DCMAKE_PREFIX_PATH="D:/Qt/6.8.2/msvc2022_64"

# 2. Build Release targets (roclint_core, roclint_host_lib, roclint_host, tests)
cmake --build build --config Release

# 3. Run automated CTest suite (Core smoke + Host lifecycle)
$env:PATH = "D:\Qt\6.8.2\msvc2022_64\bin;$env:PATH"
ctest --test-dir build -C Release --output-on-failure

# 4. Dry-run verify desktop executable (headless, non-blocking)
$env:PATH = "D:\Qt\6.8.2\msvc2022_64\bin;$env:PATH"
.\build\Release\roclint_host.exe --dry-run

# 5. Launch resident host in system tray
.\build\Release\roclint_host.exe
```

---

## 3. Completion Evidence Checklist

- [x] **Native process starts without a browser/local web frontend:** Implemented via Qt 6 + Qt Quick/QML native C++ executable (`roclint_host`).
- [x] **Tray/menu-bar control exists:** `QSystemTrayIcon` integrated with context menu containing `Open Roclint` and `Quit`.
- [x] **Closing the placeholder card leaves the host resident:** `QApplication::setQuitOnLastWindowClosed(false)` and `HostController::hideCard()` ensure residency in tray.
- [x] **Reopen shows the same logical widget, not duplicates:** Single-instance invariant verified in `HostController::showCard()`.
- [x] **Explicit Quit exits cleanly:** Only `TrayManager -> Quit` or `HostController::requestQuit()` calls `QCoreApplication::quit()`.
- [x] **No core/SQLite/provider code exists in this assignment:** B00 boundary strictly respected.

---

## 4. Contracts Satisfied

* **[C05 Desktop View-Model Contract](../../contracts/C05_desktop_view_model.md):** Typed lifecycle states (Host: `Starting` -> `Resident` -> `Quitting`; Card: `Hidden`, `Visible`, `Focused`), non-blocking startup.
* **[C08 Runtime & Packaging Contract](../../contracts/C08_runtime_packaging.md):** User-session resident lifetime, explicit quit termination.
* **[C09 Desktop Invocation & Host Context Contract](../../contracts/C09_invocation_host_context.md):** System tray presence, single-instance focus/toggle rule.
