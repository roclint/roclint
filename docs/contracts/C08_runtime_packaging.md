# C08 — Runtime, credentials, persistence lifetime, and packaging contract

## Desktop lifetime

- Qt 6 + Qt Quick/QML native user-session application on Windows and macOS.
- The application normally stays resident after the floating widget is closed.
- Windows uses system-tray presence; macOS uses menu-bar presence.
- Explicit Quit terminates the host and owned child runtimes.
- Start-at-login is a user-controlled setting; it is not silently enabled during installation.

## Core/storage

- C++20 core linked in-process.
- SQLite remains the authoritative local persistence layer under C02.
- The background lifetime may keep a repository connection/controller alive, but persistence correctness must not depend on the app never exiting.

## Model/runtime composition

The v4.2 Granite family is fixed to:

`ibm-granite/granite-4.2-3b-GGUF`

The runtime family is fixed to:

`llama.cpp`

The final MVP quantization is chosen between `Q4_K_M` and `Q5_K_M` using the C04 benchmark gate before the evaluation freeze. The chosen quantization, exact model revision, and exact `llama.cpp` revision/config are then frozen for the evaluation build.

Granite remains resident when the configured residency policy permits. Jev remains a required cloud dependency for the full ranked/scored semantic path.

The floating widget never blocks on model startup; it displays Granite `LOADING` / `READY` / `BUSY` state asynchronously.

## Granite Residency Policy

`GraniteRuntimeState` continues to describe operational readiness. v4.2 adds a separate resource-policy concept:

```text
GraniteResidencyTier
HOT
IDLE_MAPPED
UNLOADED
```

Interpretation:

- `HOT` — runtime is ready/recently used and normal inference can start without a model reload;
- `IDLE_MAPPED` — use mmap-backed loading where supported and allow the OS/runtime to reclaim physical pages according to platform/runtime behavior; do not promise a fixed reactivation time;
- `UNLOADED` — Granite process/model resources are deliberately released and a later request transitions through `LOADING`.

Policy inputs may include:

- recent-use/idle threshold;
- OS memory-pressure signal where a supported platform API exists;
- configured user preference (`Keep warm`, `Balanced`, `Low memory`);
- battery/power state only if a later implementation spike validates a stable API and policy.

Important boundaries:

- v4.2 does **not** require Stardust to call `madvise(MADV_DONTNEED)` directly on llama.cpp-owned weight buffers;
- do not promise “page back in under 200 ms” or any other unmeasured recovery number;
- do not use `mlock` as the default warm strategy because it intentionally prevents normal OS reclamation;
- the exact mmap/load-mode choice is frozen only after the B10R benchmark/spike on the supported Windows/macOS runtime path;
- GPU-offloaded memory may not follow the same reclaim behavior as CPU mmap pages and must be measured separately.

The widget itself still appears immediately. If Granite must return from `IDLE_MAPPED` or `UNLOADED`, the UI exposes `LOADING`/warming rather than freezing.

## Warm/runtime benchmark policy

Record at minimum for both Q4_K_M and Q5_K_M on reference hardware:

- disk footprint;
- initial load time;
- HOT time-to-first-token and representative latency;
- IDLE_MAPPED recovery time-to-first-token where supported;
- UNLOADED reload time-to-first-token;
- steady RSS/working set;
- optional VRAM usage and GPU-offload configuration;
- idle CPU cost and, if practical, coarse power/battery observations;
- output quality/constraint-preservation fixture results.

These measurements decide the final quantization and residency policy. They are not pre-decided performance promises.

## Jev availability

Jev is cloud-based and mandatory for the full ranked/scored path. A Jev outage changes semantic availability to `DEGRADED_UNRANKED`; it does not change `GraniteRuntimeState` and it never causes Stardust to substitute a local fake scorer.

## Credentials

- Jev credentials never appear in command-line args, environment variables, SQLite, exported JSON, or logs.
- Credential access uses macOS Keychain / Windows Credential Manager.
- If credentials are handed to a helper, use an ephemeral child-process channel or direct secure-store access and avoid echo/logging.
- Local Granite requires no cloud credential.

## Platform integration

C09 owns global shortcut, tray/menu-bar, floating-widget placement, clipboard capture, optional host-context hint, and optional direct host injection.

## Packaging

- Confirm Mac hardware/runner access before planning macOS validation work.
- Maintain dependency/license ledger for Qt, `llama.cpp`, Granite GGUF/model notices, Python/provider helper dependencies, Jev SDK, and third-party libraries.
- Decide how the Granite GGUF is delivered: bundled, managed first-run download, or preprovisioned study machines. Record integrity/version checks either way.
- Pilot installation/signing/notarization expectations are decided before external participants install builds.
- Clean-install validation includes background lifetime, tray/menu-bar presence, shortcut registration, model availability, clipboard-first invocation, host-context opt-out, keyboard-only normal path, and explicit Quit.
