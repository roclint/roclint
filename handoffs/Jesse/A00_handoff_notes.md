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