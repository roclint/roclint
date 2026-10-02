# Assignment A00 — Handoff Record

- **Assignment:** [A00 — Core workspace](../../assignments/00_foundation/A00_core_workspace.md)
- **Role/Lane:** Human A (TA / Learning Lab Mode)
- **Author:** Jesse K Ramon
- **Status:** COMPLETE & VERIFIED LOCALLY

---

## 1. Frozen Toolchain Baseline

| Component | Frozen Version / Path |
|---|---|
| Operating System | macOS (Apple Silicon arm64) |
| C++ Standard | C++20 (`-std=c++20`) |
| Compiler | Apple Clang 21.0.0 (clang-2100.1.1.101) |
| Build System | CMake 4.4.3 & Ninja 1.13.2 |
| Build Generator | Ninja |

---

## 2. Standard Build & Run Commands

```bash
# 1. Configure with Ninja
cmake -B build -G Ninja

# 2. Build core library and smoke test
cmake --build build

# 3. Run automated CTest suite
ctest --test-dir build --output-on-failure

# 4. Direct binary execution
./build/cadroc_core_tests
```

---

## 3. Completion Evidence Checklist

- [x] **Configure/build succeeds on current platform:** Clean build of `libcadroc_core.a` using Apple Clang 21 and Ninja.
- [x] **CTest runner executes smoke test:** `cadroc_core_smoke` passed in 0.00s.
- [x] **Single Source of Truth versioning:** Version configured in `CMakeLists.txt` and injected via compiler definitions (`CADROC_VERSION_...`).
- [x] **Editor ergonomics:** `#ifndef` preprocessor fallbacks implemented in `version.cpp` to satisfy VS Code IntelliSense.
- [x] **Dynamic internal consistency check:** Verified `get_version_string()` matches `ver.major.minor.patch` via `assert(str == constructed)`.
- [x] **Zero UI/Storage/Model leaks:** Strict module boundary maintained; `cadroc_core` depends only on C++20 standard library.

---

## 4. Contracts & Deliverables

* **[C01 Prompt State Contract](../../contracts/C01_prompt_state.md):** Target names (`cadroc_core`, `cadroc::core`) and C++20 compiler baseline established for future `PromptRevision` value types.
* **What Human B / Next Stage May Rely On:**
  * Public header: `include/cadroc/core/version.hpp`
  * Static library: `cadroc_core` linked via `target_link_libraries(<target> PRIVATE cadroc_core)`
  * Core version symbols: `cadroc::core::get_version()` and `cadroc::core::get_version_string()`