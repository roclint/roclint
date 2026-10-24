# C04 — Model bridge contract

## Runtime split

Stardust has two deliberately different model routes in the MVP. They are not interchangeable providers.

### GraniteLocal

Frozen model family:

`GraniteModelFamily = ibm-granite/granite-4.2-3b-GGUF`

Frozen runtime family:

`GraniteRuntime = llama.cpp`

The **model family is pinned for v4.2**, while the final quantization is selected by a bounded benchmark before the evaluation freeze:

- `Q4_K_M`
- `Q5_K_M`

The benchmark records at minimum model load time, steady RAM usage, optional VRAM usage when acceleration is enabled, time-to-first-token, generation throughput, representative analysis/rewrite latency, and output-quality/constraint-preservation checks on the frozen fixture set. v4.2 additionally records HOT / IDLE_MAPPED / UNLOADED recovery behavior under C08. After the benchmark, record the selected quantization, exact model repository revision/commit, exact `llama.cpp` revision, context settings, template/config, and hardware profile used for the decision.

GraniteLocal:

- runs on the user's machine;
- performs `granite_analyze` and `granite_rewrite`;
- remains resident/warm while the Stardust background host is alive when the residency policy permits;
- never confirms user intent;
- requires no cloud credential.

### JevCloud

Jev remains a required **cloud** dependency for the full ranked/scored semantic pass.

JevCloud:

- performs `jev_evaluate_snapshot`;
- uses one logical batched request for one `NormalizedAnalysis`;
- pins the exact model/API version for evaluation builds;
- never confirms intent for the user;
- has no local generative-model substitute in the MVP.

If Jev is unavailable, Stardust enters `DEGRADED_UNRANKED`: validated alternatives may still be presented for manual resolution, but Stardust does not invent probabilities, ranking, or a replacement local scorer.

## Runtime and semantic states are separate

Do not overload one state machine with both local-model readiness and Jev/cloud availability.

### `GraniteRuntimeState`

```text
UNLOADED
   ↓
LOADING
   ↓
READY ↔ BUSY
   ↓
FAILED
```

The minimum user-visible hot-path states are `LOADING`, `READY`, and `BUSY`. `UNLOADED` and `FAILED` exist for lifecycle/recovery.

### `SemanticAvailability`

```text
FULL_RANKED
DEGRADED_UNRANKED
```

`DEGRADED_UNRANKED` refers to the loss of the required Jev ranking/scoring path, not to Granite warming.

The floating widget may be shown in any Granite runtime state. If Granite is `LOADING`, the widget appears immediately and presents a warming state rather than blocking on model load.

## Rewrite Profiles

v4.2 adds a versioned **Rewrite Profile**. It controls the structural formatting strategy of `granite_rewrite`; it must not alter confirmed meaning.

MVP profile IDs:

```text
GENERIC
ANTHROPIC_STRUCTURED
OPENAI_STRUCTURED
LOCAL_COMPACT
```

Each profile has an explicit profile version, for example `OPENAI_STRUCTURED@1`.

Rules:

- the profile is presentation/generation configuration, not confirmed intent;
- profile switching must not modify `ConfirmedIntent`;
- the exact profile ID/version is included in the `granite_rewrite` request and provider/model-attempt metadata so a proposal can be reproduced;
- profile instructions are version-controlled text/config owned by the rewrite adapter, not ad-hoc QML prompt strings;
- no profile may invent examples, permissions, constraints, or requirements absent from confirmed intent;
- `GENERIC` is the default and the primary research condition uses a frozen profile unless the experiment explicitly studies profiles.

The profile names are target-oriented conventions, not claims that a model accepts only one syntax. For example, `ANTHROPIC_STRUCTURED` may prefer explicit XML-style sections and `OPENAI_STRUCTURED` may prefer concise Markdown/schema-oriented structure, while both remain semantically equivalent to the same confirmed intent.

## Host-context field for analysis

`granite_analyze` may include the optional coarse `HostContextHint.category` defined by C03/C09.

Rules:

- no raw window title, path, URL, application document content, or accessibility-tree data;
- the hint is advisory and cannot become confirmed intent;
- the exact hint category used for an analysis attempt is recorded with that attempt configuration;
- absence of the hint must be supported.

## Transport

The default bridge remains asynchronous local IPC owned by Qt. Versioned newline-delimited UTF-8 JSON over `QProcess` is preferred for Stardust-owned helpers. A loopback transport may be introduced only if the chosen `llama.cpp` integration route requires it and that decision is recorded. Any loopback service must remain local-only and must not become a browser/web-app surface.

Installed builds do not require system Python.

## Canonical operations

- `granite_analyze` — local Granite only;
- `jev_evaluate_snapshot` — cloud Jev only;
- `granite_rewrite` — local Granite only;
- `downstream_execute` — study harness only.

`jev_evaluate_snapshot` contains bounded independent Choice/Noul/Score questions and returns results bound to the local semantic item/group IDs.

## Identity/binding

Requests carry request ID, request-group ID, session ID, revision ID, state version, operation, and relevant input hash. Responses echo them. Stale/unknown/duplicate IDs are rejected by the core.

`granite_rewrite` additionally carries `rewrite_profile_id` + `rewrite_profile_version`. `granite_analyze` may carry `host_context_category`.

## Failure behavior

- handshake before semantic requests;
- per-operation and request-group deadlines;
- maximum frame/input sizes;
- malformed/oversized response errors;
- cancellation and late-result discard;
- bounded runtime restart attempts;
- no automatic replay of potentially billed Jev requests;
- no fabricated Jev score when Jev is unavailable.

## Evaluation constraints

- hidden provider/SDK retries are disabled where possible or surfaced as separately accounted attempts;
- payload-level DEBUG logging is prohibited for real/evaluation user data;
- exact Granite GGUF repository revision, quantization, `llama.cpp` revision/config and Jev model/API version are recorded with every frozen evaluation build;
- the primary Stardust research experiment freezes the Rewrite Profile and host-context policy across matched conditions unless one of those features is itself the experimental variable.
