# macOS Phase 4/5 Parity Audit

**Date:** 2026-11-05  
**Audit host:** Windows 11 (10.0.26300), audit performed without modifying implementation source  
**Target checkout:** macOS version of Roclint  
**Verdict:** **Unable to verify macOS parity.** Phase 4/5 source appears present in the shared tree,
but no current macOS checkout, build, test run, native UI run, deployment, or Metal benchmark was
available. A concrete macOS packaging/deployment gap is also present in source.

---

## 1. Checkout and environment

### 1.1 Current checkout (Windows audit host)

| Field | Value |
|---|---|
| Repository | `D:\Stardust\roclint` |
| Branch | `feat/codebase-decoupling` |
| HEAD | `79728a7edd7ab0610754fdda891e9b424d7682aa` |
| Remote | `origin https://github.com/roclint/roclint.git` |
| Remote branch | `origin/feat/codebase-decoupling` = `0a17b8d7aaf1c8060db9be4bcd589475be5c35bc` |
| Remote main | `origin/main` = `193506f5c2d00348fdf0cb10080b97f582148122` |
| Working tree | Clean except eight pre-existing untracked `docs/research/` UI assets |
| Separate local checkout | `D:\Stardust\roclint-feat-codebase-decoupling` at `4897d62`, clean |
| Pinned llama.cpp | `da2ee1e7a640445fc562fe5529598cee1917e0f5` (`app/runtime/granite-models.json`) |
| Audited OS compiler | MSVC 2022 / Qt 6.8.2 / Python 3.13.3 (Windows only) |

The current Windows branch is ahead of origin by the Phase 6 AST commits. Origin contains Phase 4
and Phase 5 through `0a17b8d`, but not Phase 6.

### 1.2 macOS host/environment

No macOS host was usable for this audit:

- `Jesses-MacBook-Pro.local` resolves at `10.0.0.199`, but SSH (`22`), SMB (`445`) and VNC (`5900`)
  are closed; no mounted share or SSH host entry exists.
- There is no `.github/workflows` directory or configured macOS runner. `gh` is authenticated, but
  no CI workflow can be dispatched.
- `docs/STRUCTURE_AND_VISION.md:115` records the user's confirmation that no Mac/macOS runner is
  available and native macOS verification is unproven.

Consequently these values are **NOT AVAILABLE** for the actual macOS checkout:

- repository path, branch, HEAD, remotes, working-tree status on the Mac;
- macOS version and architecture;
- Xcode/Clang version;
- Qt and Python versions;
- actual llama.cpp checkout revision;
- whether the Mac has the Phase 6 AST commit or only the Phase 5 tip.

The historical Phase 4 implementation report describes an older worktree
`/Users/jesse/.codex/worktrees/delivery-integration/roclint` at base `8477480`, macOS 26.6.2 arm64,
10-core M1 Pro, 32 GB RAM. That is useful historical context, not evidence for the current Mac
checkout or current HEAD.

### 1.3 Required commit ancestry

All six named Phase 4/5 commits are ancestors of both local HEAD and `origin/feat/codebase-decoupling`:

| Commit | Subject | Ancestor of HEAD | Ancestor of origin |
|---|---|---:|---:|
| `4897d62` | runtime workers, IPC hardening, intent intelligence | yes | yes |
| `738a1e0` | Phase 4 remediation | yes | yes |
| `f1d2ff3` | Qt deployment and model verification | yes | yes |
| `0610985` | canonical intent schema/disposition | yes | yes |
| `be50a8b` | Decision Contract v2 benchmarks / KV reuse | yes | yes |
| `0a17b8d` | Phase 5 validation report | yes | yes |

Commit ancestry establishes source availability in this checkout. It does **not** establish that
the inaccessible Mac checkout contains those commits or equivalent changes.

---

## 2. Reports and contracts reviewed

- `docs/CURRENT_IMPLEMENTATION.md`
- `docs/audit/phase4-final-verification.md`
- `docs/audit/phase5_execution_report.md`
- `docs/audit/phase5-benchmark-summary.json`
- `docs/audit/phase4-implementation-report.md` (historical macOS worktree)
- `docs/audit/ipc-lifecycle-hardening.md`
- `docs/audit/ambient-badge-recovery.md`
- `docs/audit/live-capture-synchronization.md`
- `docs/audit/hud-control-overflow.md`
- `docs/contracts/C04_model_bridge.md`, `C05_desktop_view_model.md`,
  `C06_intent_rewrite_approval.md`, `C08_runtime_packaging.md`,
  `C09_invocation_host_context.md`, `C10_codebase_context.md`
- `docs/STRUCTURE_AND_VISION.md`, `docs/UPSTREAM_INTEGRATION.md`

Historical evidence is separated from fresh evidence below. Earlier macOS reports describe
different worktrees/commits and cannot be cited as validation of current HEAD.

---

## 3. Feature parity matrix

Legend: **PASS-source** = source present and statically inspected; **PASS-Win** = shared automated
behavior verified on this Windows host; **NOT RUN** = no macOS execution; **GAP** = implementation
missing or incomplete.

| Area | Source | macOS compile | Automated behavior | Native UI | Deployment | Performance |
|---|---|---|---|---|---|---|
| Phase 4 intent extractor before transport | PASS-source | NOT RUN | PASS-Win / 9-case probe | n/a | n/a | n/a |
| Sealed source-bound deterministic conflicts, zero transport | PASS-source | NOT RUN | PASS-Win | n/a | n/a | n/a |
| Ambiguous/repeated fragments fall back; quoted data excluded | PASS-source | NOT RUN | PASS-Win | n/a | n/a | n/a |
| Equivalent durations/dimensions/permissions/disjoint scopes | PASS-source | NOT RUN | PASS-Win | n/a | n/a | n/a |
| `suggestedAlternativeEvidence` parse/bound/persist/restore/HUD | PASS-source | NOT RUN | PASS-Win | NOT RUN | n/a | n/a |
| Native runtime helper launch, IDs, cancel, timeout, restart, stale reply | PASS-source | NOT RUN | PASS-Win | NOT RUN | GAP (mac bundle) | NOT RUN |
| Production/test mock isolation | PASS-source | NOT RUN | n/a | n/a | n/a | n/a |
| macOS outside-click monitor | PASS-source | NOT RUN | NOT RUN | NOT RUN | GAP | n/a |
| macOS accessibility capture/insertion | PASS-source | NOT RUN | NOT RUN (mock only) | NOT RUN | GAP | n/a |
| Canonical intent ADD/REMOVE/MODIFY/CONSTRAIN/PRESERVE | PASS-source | NOT RUN | PASS-Win | n/a | n/a | n/a |
| Canonical intent in discovery and rewrite | PASS-source | NOT RUN | PASS-Win | n/a | n/a | n/a |
| Approval bound to exact prompt/choices | PASS-source | NOT RUN | PASS-Win | NOT RUN | n/a | n/a |
| Model discovery/manifests/SHA verification | PASS-source | NOT RUN | PASS-Win `--verify` | n/a | partial (no app bundle) | n/a |
| macOS self-contained deployment/Finder launch | **GAP** | NOT RUN | NOT RUN | NOT RUN | **GAP** | n/a |
| Metal backend selection and runtime | PASS-source | NOT RUN | NOT RUN | n/a | GAP | **NOT RUN** |
| Corrected 512-token KV prefix reuse | PASS-source | NOT RUN | PASS-Win unit | n/a | n/a | **NOT RUN on Metal** |

---

## 4. Phase 4 findings

### 4.1 Intent intelligence — source present, Windows behavior verified

`GraniteAdapter::discoverOptions` runs `StructuredEntityExtractor::analyze` after acquiring the
busy lease and before the transport (`src/bridge/GraniteAdapter.cpp:847-891`). When all conflict
fragments resolve to unique spans, it seals the analysis and returns a clarification response
without calling the transport. Missing/ambiguous spans fall through to the model.

The nine-case verification probe executed on this Windows host:

```text
baseline_conflict|ok=1|transport_calls=0|conflicts=1|sealed=1
equivalent_units|ok=1|transport_calls=1|conflicts=0|sealed=1
independent_dimensions|ok=1|transport_calls=1|conflicts=0|sealed=1
disjoint_mandatory_scope|ok=1|transport_calls=1|conflicts=0|sealed=1
equivalent_permissions|ok=1|transport_calls=1|conflicts=0|sealed=1
quoted_data|ok=1|transport_calls=1|conflicts=0|sealed=1
same_scope_opposite_actions|ok=1|transport_calls=1|conflicts=0|sealed=1
nonadjacent_paragraphs|ok=1|transport_calls=0|conflicts=1|sealed=1
disjoint_imperative_scope|ok=1|transport_calls=1|conflicts=0|sealed=1
```

Exit code `0`. This is Windows shared-C++ evidence, not macOS evidence. macOS compilation and
execution remain NOT RUN.

### 4.2 Alternative evidence — source and storage tests present

`MissingDecision::suggestedAlternativeEvidence` is present (`include/roclint/analysis/NormalizedAnalysis.hpp:52-55`),
parsed with a 1,024-byte bound (`src/bridge/GraniteAdapter.cpp:659-663`), encoded/decoded
optionally (`src/storage/PayloadCodec.cpp:405-406, 476-477`), included in the snapshot hash when
non-empty (`src/analysis/NormalizedAnalysis.cpp:68-69`), and appended to the active HUD question
(`app/host/HostController.cpp:1697-1698`).

`roclint_assumption_scoring` covers round-trip, sealing and tamper detection; the Windows suite
passed. The native macOS HUD rendering path and real missing-decision presentation are NOT RUN.

### 4.3 Runtime and IPC — source present, Windows tests pass

- `OwnedProcessJob.hpp` implements Windows Job Objects and POSIX guardian/socket/process-group
  ownership; `process_guardian.cpp` is built on UNIX and copied to `runtime/`.
- Runtime workers enforce request IDs, monotonic deadlines, cancellation quiescence, stale-result
  rejection, bounded parsing, restart limits and shutdown.
- `roclint_host_lib` links production core/storage; only `roclint_host_lib_testing` carries
  `ROCLINT_ENABLE_TEST_MOCKS`, and production `roclint_host` links the production library.

Fresh Windows CTest included `roclint_native_granite`, `roclint_host_controller`,
`roclint_ipc_hardening` and related suites; all passed. macOS compilation and native process
ownership are NOT RUN.

### 4.4 macOS HUD/accessibility — source present, native behavior NOT RUN

`HostContext_mac.mm` and `MacAccessibilityBridge.mm` are registered only on `APPLE` and link
Carbon/AppKit/ApplicationServices (`CMakeLists.txt:292-296`). The outside-click monitor has the
400 ms settle window, inside-window guard, same-host-app guard and queued dismissal.

`MacAccessibilityBridge.mm` implements Accessibility permission polling, focused-field observation,
password/unsupported-field refusal, terminal draft extraction, focus-token checks and readback.
No Return/Enter key is sent; terminal and Electron fallback use Cmd+A/Cmd+V and terminal editing
keys.

**Source-level finding:** the Electron/GUI fallback does not require exact readback. After ten
attempts it accepts any text change as success:

```text
app/host/MacAccessibilityBridge.mm:660-663
if (after.has_value() && norm(after->text) != norm(before->text)) {
    ok = true;
}
```

That can report `Injected` when the field contains different or partial text. The existing
controller/QML tests use `MockAccessibilityBridge`, so this native path has no automated coverage.
This conflicts with the requirement that insertion verifies the prompt text. It does not press
Send/Enter.

Historical Cocoa screenshots and local captures exist for earlier worktrees, but there is no
current-HEAD native run. `docs/audit/live-capture-synchronization.md:28-29` itself records that a
real third-party editor retest was still needed.

---

## 5. Phase 5 findings

### 5.1 Canonical intent — source and Windows tests present

`CanonicalIntent` includes action, target, property, value, disposition, condition, deterministic
`nid_` node ID, source span and optional AST grounding. `CanonicalIntentExtractor` handles all five
actions, negation, nested `EXCEPT/ONLY_IF/WHEN`, quoted-data exclusion, backticks, multi-paragraph
prompts, stable IDs and valid source spans.

`StructuredEntityExtractor` populates `canonicalIntents`; discovery includes `canonical_intents`,
and rewrite includes `<canonical_intents>` (`src/bridge/GraniteAdapter.cpp:258-310`,
`src/bridge/GraniteRewriteAdapter.cpp`). Approval/proposal tests bind exact revision/intent/
proposal hashes. Windows CTest passed these tests; macOS compilation and native HUD review are
NOT RUN.

Phase 5's own report records that at `0a17b8d` canonical grounding was lexical because Tree-sitter
was not present. Current Windows HEAD adds Phase 6 AST; a Mac checkout at origin does not contain
that later commit.

### 5.2 Model discovery/integrity — source supports macOS, Windows verify passes

`tools/download_all_models.py` supports `~/Library/Application Support/roclint/Roclint/models`,
repo-local `julia-1`/`granite-4.2-3b`, and `s1`/`s2` layouts. `--verify` returns before any venv
creation or download, checks pinned sizes and SHA-256 values for selected Granite candidates and
every file listed in `roclint-julia.json`, and exits non-zero on errors.

Fresh Windows verification:

```text
[OK] Julia checksum df853bf7...(models/julia-1/model.safetensors)
[OK] Granite Q8_0 checksum fbe98673...(models/granite-4.2-3b/granite-4.2-3b-Q8_0.gguf)
[OK] All selected model weights are present and match their pinned SHA-256 digests.
exit=0
```

No download or installation was performed. The equivalent macOS path/layout was not executed.

### 5.3 Deployment and normal launch — **GAP**

Phase 5's self-contained deployment is Windows-only:

- `ROCLINT_DEPLOY_QT` is described as a Windows host deployment and is guarded by
  `if(WIN32 AND ROCLINT_DEPLOY_QT)` (`CMakeLists.txt:193, 498`).
- `tools/deploy_qt.cmake` invokes `windeployqt`.
- There is no `MACOSX_BUNDLE`, `Info.plist`, `macdeployqt`, `codesign`, `notarytool`, entitlements
  or app-bundle target.
- `README.md` tells macOS users to run `build/roclint_host` with Qt runtime on `PATH` or deployed
  beside it; that is a development executable, not a self-contained `.app`.
- `docs/audit/ambient-badge-recovery.md:11` records that the earlier Mac host was an unbundled
  linker/ad-hoc-signed Mach-O with no bound Info.plist or team identity, and identifies stable
  bundled/signed launch as a remaining distribution task.
- `docs/STRUCTURE_AND_VISION.md:71` lists installed product, signing and native macOS checks as
  open.

This is a Phase 5 packaging parity gap independent of the unavailable Mac. Signing and
notarization status are **not implemented and not run**, not failed.

### 5.4 Metal and corrected KV reuse — source present, NOT RUN

Source supports Metal:

- `ROCLINT_GRANITE_BACKEND=AUTO` selects `METAL` on Apple and sets `GGML_METAL=ON`
  (`CMakeLists.txt:387-421`).
- The manifest has a `METAL` profile with 99 GPU layers and the same context/sampling contract.
- `GraniteEngine.cpp` selects `metal`/`mtl`, sets Apple `n_ubatch=512`, batch threads up to 8 and
  decode threads capped at 4 (`app/runtime/GraniteEngine.cpp:238-263`).
- Corrected KV reuse uses complete 512-token chunks and always recomputes the final chunk;
  `prefixCacheTests()` covers 512/513/700/1024-token cases.

No Apple Silicon hardware or Metal build was available. Therefore there are no Metal prefill,
TTFT, token/s, reused-token, output-hash, determinism or contract-failure results. The historical
Phase 5 numbers in `docs/audit/phase5-benchmark-summary.json` are **VULKAN only** and are not
substituted for Metal.

---

## 6. Fresh verification executed here

These results are on the Windows audit host and do not validate macOS compilation or native
behavior.

| Check | Command | Result |
|---|---|---|
| Structure | `python tools/check_structure.py --root .` | PASS, `94/94`, `0` errors, `245` additions (including this report), `338` source files |
| Python tests | `python -m unittest discover -s tools -p "test_*.py"` | PASS, `49` tests, `OK (skipped=1)`, `16.601 s` |
| Verification probe | `build/Release/report-verification-probe.exe` | PASS, `9/9`, exit `0` |
| Windows CTest | `ctest --test-dir build -C Release --output-on-failure` | PASS, `45/45`, `129.48 s` |
| Model integrity | `python tools/download_all_models.py --verify` | PASS, all selected files match pinned SHA-256, exit `0` |

The single skipped Python test is the platform-specific process-guardian test on Windows; it is
not evidence of macOS execution. The CTest count is 45 because current HEAD includes the two Phase 6
AST suites; origin/Phase 5 has 43.

---

## 7. NOT RUN / blockers

| Check | Status | Reason |
|---|---|---|
| Native macOS Release build | NOT RUN | No reachable Mac and no macOS runner |
| macOS CTest suite | NOT RUN | Same |
| macOS Python tests | NOT RUN | Same |
| macOS `--dry-run` / QML launch | NOT RUN | Same |
| Finder-style `.app` launch | NOT RUN | No macOS host and no app bundle target |
| Native Cocoa outside-click/focus/choice tests | NOT RUN | No macOS host |
| Native Accessibility capture/insertion | NOT RUN | No macOS host; existing automated tests use a mock bridge |
| macOS model verification/layout | NOT RUN | No macOS host |
| Metal discovery benchmark | NOT RUN | No Apple Silicon/Metal host |
| Metal cold/warm KV parity | NOT RUN | Same |
| Signing/notarization | NOT IMPLEMENTED | No macOS bundle/signing configuration in source |

---

## 8. Prioritized remediation plan

### 8.1 Suggested macOS audit commands (once access exists)

```sh
sw_vers
uname -m
xcodebuild -version
clang --version
python3 --version
git rev-parse HEAD && git status --short

cmake -S . -B build-macos-audit \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DROCLINT_GRANITE_BACKEND=METAL
cmake --build build-macos-audit --config Release --parallel
ctest --test-dir build-macos-audit -C Release --output-on-failure
python -m unittest discover -s tools -p "test_*.py"
python tools/check_structure.py --root .
python tools/download_all_models.py --verify
```

Run `build-macos-audit/roclint_host --dry-run` and the verification probe; retain the logs with
the exact HEAD and environment.

### 8.2 Ordered fixes

1. **P0 — provide a macOS execution path.** Configure SSH or a runner on the M1/M-series Mac,
   record `sw_vers`, `uname -m`, `xcodebuild -version`, `clang --version`, Qt/Python versions,
   repository path/branch/HEAD/status, and use a separate audit build directory. Without this,
   no native parity claim is possible.
2. **P1 — implement macOS packaging.** Add a `MACOSX_BUNDLE` target with `Info.plist`, icon and
   bundle identifier, `macdeployqt` deployment of Qt frameworks/plugins/QML imports, copy
   `runtime/` helpers with executable permissions, then add codesign entitlements, notarization
   and stapling. Test from Finder on a clean machine with Qt/QML environment variables removed.
   A shell launcher is not a substitute.
3. **P1 — rebuild and test current HEAD on macOS.** Run the native Release build, all CTests,
   Python tests, structure check, `--dry-run`, and the nine-case verification probe. Attach logs
   and exact commit identity.
4. **P2 — harden macOS insertion verification.** Remove the “any text change means success”
   fallback in `MacAccessibilityBridge.mm:660-663`; require exact normalized readback or return
   `Failed`/`FieldChanged` with a clear reason. Add a native AX fixture covering focus loss,
   stale expected text, partial writes, permission denial, terminal draft replacement and focus
   restoration.
5. **P2 — run Metal/KV benchmark.** Build with `ROCLINT_GRANITE_BACKEND=METAL`, run the current
   benchmark harness with Q8_0, context 8192, seed 42, temperature 1.0, top-p 0.95, cold/warm
   repeats, and validate outputs with `roclint_granite_contracts --validate`. Record hardware,
   model hash, reused tokens, prefill/TTFT/generation latency, output hashes, contract failures and
   determinism.
6. **P2 — verify macOS model locations.** Run `--verify` against the actual
   `~/Library/Application Support/roclint/Roclint/models` layout, including missing/corrupt/partial
   cases, without downloading.
7. **P3 — add macOS CI.** Add a macOS workflow/runner only if the project permits CI changes; it
   must build, run CTests and publish logs. Do not treat a green Windows run as macOS evidence.
8. **P3 — update documentation.** Replace historical macOS claims with current-HEAD evidence, and
   keep the unbundled development executable, app bundle, signing and notarization statuses
   separate.

---

## 9. Verdict

**Unable to verify macOS parity.** The Phase 4/5 implementation source and macOS-specific
registration are present in the current Windows checkout, and shared Windows tests pass. The
inaccessible Mac, absence of a macOS runner/build, stale historical Mac evidence, and missing
macOS bundle/deployment/signing work prevent any supported claim that the current macOS version is
integrated and working natively. Metal performance and cold/warm KV parity are likewise unmeasured
on Apple Silicon.
