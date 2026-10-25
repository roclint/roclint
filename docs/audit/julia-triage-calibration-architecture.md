# Julia S1 Triage — Calibration & Routing Policy Architecture

> **Status:** Design review document. No source files are modified by this document.
> **Scope:** `include/roclint/analysis/PromptTriage.hpp`, `src/analysis/PromptTriage.cpp`,
> `app/host/HostControllerTriage.cpp`, `app/host/HostController.hpp`,
> `app/runtime/julia_worker.py`, `tools/benchmark_julia_triage.py`,
> `tools/challenge_julia_triage.py`

---

## 1. Executive Summary

The current Julia/S1 triage gate uses a single scalar comparison
(`ambiguityScore < kPromptTriageThreshold` = 0.70) against the maximum of four independent
Boolean classifier heads. This was a correct first iteration for establishing the architecture;
it is not calibrated for production. Four structural deficiencies must be addressed:

1. **The cryptographic hash embeds the routing threshold**, coupling the audit chain to policy
   parameters that must be tunable without breaking historical record verification.
2. **A single global threshold** does not respect the asymmetric error costs of each head.
   `RULE_CONFLICT` misses have higher consequence than a noisy `ACTION` hit.
3. **No uncertainty buffer** exists between binary outcomes, causing classifier thrashing near
   the boundary.
4. **Raw sigmoid output is uncalibrated** — deep transformer classifiers produce overconfident
   probabilities; statistical reliability requires temperature scaling.

This document defines the target architecture, proposes schema changes for both v1 compatibility
and a new v2 sealed record, recommends default per-head thresholds, and specifies the UI/UX
treatment for the new `UncertainBuffer` tier.

---

## 2. Current Architecture — Precise Assessment

### 2.1 Hash Construction Embeds Policy (`src/analysis/PromptTriage.cpp:49`)

```cpp
// Line 49 — roclint.prompt_triage.v1 domain
hashDouble(hash, kPromptTriageThreshold);  // 0.70 baked into the sealed record
```

**Why this is a correctness problem, not just a design smell:** `validatePromptTriage`
recomputes the hash from the same inputs (line 72) and compares it with the stored
`triageHash`. If `kPromptTriageThreshold` changes, every historical record will fail
`validatePromptTriage`. This makes threshold tuning a **breaking change to the audit chain**.

The hash *should* commit to model evidence (scores, latency, identity, prompt binding).
Routing decisions are policy-level computations over that evidence and belong outside the seal.

### 2.2 Single Global Threshold (`src/analysis/PromptTriage.cpp:93`)

```cpp
return triage.ambiguityScore < kPromptTriageThreshold
    ? PromptTriageRoute::Original : PromptTriageRoute::Clarify;
```

`ambiguityScore` is derived as `max(categoryScores)`. This has two problems:

- **Asymmetric head costs are ignored.** A `RULE_CONFLICT` score of 0.60 should likely
  escalate (conflict detection is high-consequence); an `ACTION` score of 0.71 for conversational
  wording should not necessarily invoke Granite.
- **The single threshold is shared across all four heads**, preventing independent tuning after
  real-world calibration data is collected.

### 2.3 Binary Boundary — No Dead Band (`src/analysis/PromptTriage.cpp:93`)

Scores of 0.699 and 0.701 produce opposite routes despite being statistically
indistinguishable given classifier variance. The benchmark in
[`tools/benchmark_julia_triage.py`](../../tools/benchmark_julia_triage.py) already identifies
a 0.30/0.70 separation gap; any score between 0.30 and 0.70 on the current clear/ambiguous
distribution is borderline. Introducing a dead band converts this grey zone into a third,
explicitly handled tier.

### 2.4 Uncalibrated Sigmoid Probabilities (`app/runtime/julia_worker.py:256`)

```python
probabilities = softmax(values)  # equivalent to sigmoid for 2-class head
scores.append({"category": category, "score": probabilities[1]})
```

The model outputs raw logits. Softmax/sigmoid maps them to [0,1] but does not calibrate
the resulting probability to a frequency — a score of 0.85 does not mean "this prompt is
ambiguous in 85% of cases at this threshold". Reliability diagrams on even a small labeled
set would likely show significant overconfidence above 0.50 and underconfidence below.

The current `noul_certainty` threshold in `evaluate()` (line 338, 345) uses the same
uncalibrated `0.70` cutoff inherited into the noul certainty logic.

---

## 3. Proposed Architecture

### 3.1 Upgrade 1 — Decouple Hash Domain (v1 → v2)

**Principle:** The sealed record commits to *evidence*. Policy is computed over evidence
separately and is never embedded in the seal.

#### New hash domain: `roclint.prompt_triage.v2`

The v2 hash includes everything in v1 **except** `hashDouble(hash, kPromptTriageThreshold)`.
Routing is then computed by an explicit `TriagePolicy` struct passed to the routing function.

Historical v1 records remain valid under their own domain and can be re-verified by
reconstructing the v1 hash (which still bakes in 0.70). The validator must continue to
accept both `roclint.prompt_triage.v1` (legacy read-only verification) and
`roclint.prompt_triage.v2` (new production records).

**Proposed version field in `PromptTriage`:**

```cpp
// include/roclint/analysis/PromptTriage.hpp (proposed addition)
enum class TriageHashVersion { V1 = 1, V2 = 2 };

struct PromptTriage {
    // ... existing fields ...
    TriageHashVersion hashVersion{TriageHashVersion::V2}; // new field; defaults to v2
};
```

**Revised `computePromptTriageHash` signature (proposed):**

```cpp
// Two overloads — backwards compatibility without the threshold argument for v2
[[nodiscard]] std::string computePromptTriageHash(const PromptTriage& triage);
```

v1 records are detected by the domain string stored in the hash prefix; v2 records omit
`hashDouble(hash, kPromptTriageThreshold)`. The domain suffix encodes the version:
`roclint.prompt_triage.v1` vs `roclint.prompt_triage.v2`.

**Impact on `validatePromptTriage`:** the validator already re-derives the hash; it will
dispatch on the stored `hashVersion` (or, conservatively, attempt v2 first and fall back to
v1 reconstruction) to support both generations in the same binary.

#### Validation contract preserved

`validatePromptTriage` continues to fully recompute and compare `triageHash`. Historical v1
records pass because their domain still includes the threshold; v2 records pass because they
don't. No existing sealed record's `id` or `triageHash` changes.

---

### 3.2 Upgrade 2 — Per-Head Threshold Policy (SCut Pattern)

#### `TriagePolicy` struct

```cpp
// include/roclint/analysis/PromptTriage.hpp (proposed addition)
struct TriagePolicy {
    // Per-head ambiguity thresholds: [ACTION, TARGET, RULE, RULE_CONFLICT]
    // Route to Clarify when categoryScores[i] >= thresholds[i].
    std::array<double, 4> thresholds{0.70, 0.70, 0.70, 0.60};

    // Dead-band half-width δ. A head score in [threshold[i]-delta, threshold[i])
    // contributes to UncertainBuffer when no head is clearly Clarify.
    double uncertaintyDelta{0.08};

    // Policy identity string for audit records and human tracing.
    std::string policyId{"roclint.triage_policy.v1"};

    static TriagePolicy defaults() noexcept { return {}; }
};
```

#### Routing logic (proposed `routePromptTriage` evolution)

```cpp
[[nodiscard]] Result<PromptTriageRoute> routePromptTriage(
    const PromptTriage& triage,
    const TriagePolicy& policy = TriagePolicy::defaults());
```

Decision tree:
1. If any `categoryScores[i] >= policy.thresholds[i]` → **`Clarify`**
2. Else if any `categoryScores[i] >= policy.thresholds[i] - policy.uncertaintyDelta`
   → **`UncertainBuffer`**
3. Else → **`Original`**

This preserves the existing two-tier contract for all clear and clearly-ambiguous cases;
the new tier only fires in the boundary zone.

#### Per-head threshold defaults — rationale

| Head | Default | Rationale |
|------|---------|-----------|
| `ACTION` (index 0) | 0.70 | Conversational imprecision is common; 0.70 provides good separation in existing benchmarks |
| `TARGET` (index 1) | 0.70 | Missing target is common and important; 0.70 is well-separated on benchmark data |
| `RULE` (index 2) | 0.70 | Requirement ambiguity has moderate consequence; 0.70 matches existing tuning |
| `RULE_CONFLICT` (index 3) | 0.60 | **Lowered**: conflict detection is high-consequence; a 0.55–0.65 score warrants review even if below the general gate |

The `RULE_CONFLICT` threshold lowering is the most impactful change. Current benchmark data
shows `conflict_counts` scoring 0.98+ and `multisentence_internal_conflict` scoring 0.95+,
so the practical difference in those cases is nil. The benefit appears at the boundary
(0.55–0.70) where a genuine conflict that the model is uncertain about still warrants a
Clarify route rather than silently passing.

The `uncertaintyDelta` of 0.08 means:
- `ACTION` dead band: [0.62, 0.70)
- `TARGET` dead band: [0.62, 0.70)
- `RULE` dead band: [0.62, 0.70)
- `RULE_CONFLICT` dead band: [0.52, 0.60)

These ranges correspond to model score distributions where classifier confidence is
statistically unreliable based on the separation margins observed in benchmark runs.

---

### 3.3 Upgrade 3 — Three-Tier Routing and `UncertainBuffer`

#### New enum value

```cpp
// include/roclint/analysis/PromptTriage.hpp (proposed addition)
enum class PromptTriageRoute { Original, Clarify, UncertainBuffer };
```

#### What the host should do with `UncertainBuffer`

`UncertainBuffer` is **not a blocking route**. It must not launch Granite (cost/latency
identical to `Clarify`) and must not silently assume clarity (`Original`). It is a
*soft advisory* that the classifier's evidence is inconclusive.

**Recommended host behavior in `HostControllerTriage.cpp`:**

| Scenario | Behavior |
|----------|----------|
| Ambient badge mode | Show a muted "?" badge rather than the normal ambiguity badge; non-blocking |
| Widget card mode | Display a non-blocking status message: "S1 score is borderline (score `X`, head `Y`). Prompt looks usable but clarity is not confirmed. Analyze explicitly to invoke Granite." |
| `isOriginalRoute` binding in `HostController.hpp` | Expose a new `isUncertainRoute` Q_PROPERTY returning `true` when the last triage result was `UncertainBuffer` |
| Explicit re-analyze | Always available; executes the normal full flow including Granite |

**Why not a fast heuristic re-check?** The uncertainty is about the *model's confidence in
its own prediction*, not about a data-quality issue. A heuristic re-check of the same prompt
will produce the same uncertain score. Granite is the appropriate second-stage tool; the dead
band should lead to "analyst judgement" (explicit user re-analyze) rather than automatic
Granite invocation.

**Status message proposal for `onPromptClassified` in `HostControllerTriage.cpp`:**

```cpp
// After UncertainBuffer route:
setAnalysisStatus(AnalysisStatus::Ready,
    QStringLiteral("S1 result is borderline (score %1 on %2) — original wording retained "
                   "but clarity not confirmed. Use Analyze to review with Granite.")
        .arg(triage->ambiguityScore, 0, 'f', 3)
        .arg(QString::fromUtf8(an::toString(triage->primaryCategory))));
```

The hardcoded `0.70` in the current status message on line 122 of `HostControllerTriage.cpp`
must be removed regardless; it is now derived dynamically from the active `TriagePolicy`.

---

### 3.4 Upgrade 4 — Temperature Scaling in `julia_worker.py`

#### Problem restated

Line 256 of `julia_worker.py`:

```python
probabilities = softmax(values)
scores.append({"category": category, "score": probabilities[1]})
```

For a 2-class head, `softmax(values)[1]` = `sigmoid(values[1] - values[0])`. Raw sigmoid
output is known to be overconfident for fine-tuned BERT-class classifiers. A temperature
`T > 1` compresses the logit spread before sigmoid, returning probabilities closer to the
empirical positive rate.

#### Proposed change

```python
# Configurable temperature T > 0: T=1.0 is identity (current behaviour)
# T > 1 softens overconfident predictions; T < 1 sharpens underconfident ones.
# Calibrated T should be fit offline from a reliability diagram on labeled data.
_TRIAGE_TEMPERATURE = float(os.environ.get("JULIA_TRIAGE_TEMPERATURE", "1.0"))

def triage(engine, request):
    ...
    for category, values in zip(TRIAGE_CATEGORIES, logits):
        scaled_values = [v / _TRIAGE_TEMPERATURE for v in values]
        probabilities = softmax(scaled_values)
        ...
```

#### Configuration mechanism — recommendation

**Environment variable `JULIA_TRIAGE_TEMPERATURE`** is the correct delivery channel for this
parameter. Reasons:

- It is already the pattern used by `JULIA_DEVICE` and `JULIA_CPU_THREADS` in the worker.
- Temperature is an operator/deployment-time parameter, not a per-request parameter.
- It does not belong in the model metadata JSON (which is pinned to the checkpoint revision
  and must not be edited without a new revision).
- It must not appear in the NDJSON protocol request (per-request temperature would allow
  callers to manipulate routing decisions independently of the sealed policy).
- The host should expose it in a `JuliaSettings.qml` calibration panel for advanced users.

The `execute_configuration_id` returned by `triage()` already encodes the hardware/dtype
profile. Once temperature scaling is added, the worker should also encode `T` in the
configuration ID: `julia.cpu.fp32.v2+T1.50` or similar. This allows the C++ seal to commit
to the exact inference configuration including the scaling parameter.

#### Logit passthrough for diagnostics (optional, recommended)

The worker can optionally return raw logits alongside calibrated probabilities when a
diagnostic mode is enabled:

```python
# Only when JULIA_TRIAGE_DIAGNOSTICS=1
if os.environ.get("JULIA_TRIAGE_DIAGNOSTICS") == "1":
    result["raw_logits"] = [[float(v) for v in head] for head in logits]
```

Raw logits must never be used by the routing path; they are purely for calibration tooling.
They must not appear in sealed production records.

---

## 4. Schema Changes — v1 Compatibility vs v2 Definition

### 4.1 `PromptTriage` struct additions

```cpp
// include/roclint/analysis/PromptTriage.hpp

// New enum for three-tier routing
enum class PromptTriageRoute { Original, Clarify, UncertainBuffer };  // adds UncertainBuffer

// New hash version enum
enum class TriageHashVersion { V1 = 1, V2 = 2 };

// Per-head policy struct (does not enter the seal)
struct TriagePolicy {
    std::array<double, 4> thresholds{0.70, 0.70, 0.70, 0.60};
    double uncertaintyDelta{0.08};
    std::string policyId{"roclint.triage_policy.v1"};
    static TriagePolicy defaults() noexcept { return {}; }
};

struct PromptTriage {
    // ... all existing fields unchanged ...
    TriageHashVersion hashVersion{TriageHashVersion::V2};  // NEW: v1 for historical, v2 for new
};
```

### 4.2 `computePromptTriageHash` — v1 vs v2 behaviour

```cpp
std::string computePromptTriageHash(const PromptTriage& triage) {
    const char* domain = (triage.hashVersion == TriageHashVersion::V1)
        ? "roclint.prompt_triage.v1"
        : "roclint.prompt_triage.v2";
    crypto::CanonicalHasher hash(domain);
    hash.field(triage.sessionId).field(triage.revisionId) /* ... same fields ... */;
    for (const auto score : triage.categoryScores) hashDouble(hash, score);
    hashDouble(hash, triage.ambiguityScore);
    hash.number(static_cast<std::uint64_t>(triage.primaryCategory));
    // v1 ONLY: bake in the threshold
    if (triage.hashVersion == TriageHashVersion::V1)
        hashDouble(hash, kPromptTriageThreshold);
    hashDouble(hash, triage.latencyMs);
    return hash.finalizeHex();
}
```

### 4.3 `validatePromptTriage` — version-aware dispatch

`validatePromptTriage` already recomputes the hash and compares; the only change is that
it no longer hard-codes which domain to use. The `hashVersion` field is read from the record
itself (defaulting to v1 for records that predate the field). This is fully backwards
compatible because v1 records that exist in storage have `triageHash` values computed with
the v1 domain.

### 4.4 `routePromptTriage` — new overload, old signature unchanged

The existing `routePromptTriage(const PromptTriage& triage)` signature is **preserved**.
A second overload accepting `TriagePolicy` is added. The zero-argument overload forwards to
the policy overload with `TriagePolicy::defaults()`:

```cpp
// Backwards-compatible default (same as current 0.70 global for ACTION/TARGET/RULE,
// stricter 0.60 for RULE_CONFLICT)
Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage) {
    return routePromptTriage(triage, TriagePolicy::defaults());
}

Result<PromptTriageRoute> routePromptTriage(
    const PromptTriage& triage, const TriagePolicy& policy);
```

All existing call sites (`HostControllerTriage.cpp:103`, `test_s1_triage_adapter.cpp:32`)
continue to compile unchanged — the default policy matches current semantics for
ACTION/TARGET/RULE heads, with a *slight behaviour change* on RULE_CONFLICT (threshold
lowered from 0.70 to 0.60 in defaults). The test for the exact 0.70 gate boundary
(`testPromptTriageOwnsTheExactGateBoundary`) must be updated to reflect the policy-driven
threshold.

---

## 5. UI/UX: Handling `UncertainBuffer` in the Host

### 5.1 `HostController` property surface

New Q_PROPERTY proposed:

```cpp
// HostController.hpp
Q_PROPERTY(bool isUncertainRoute READ isUncertainRoute NOTIFY analysisStatusChanged)

[[nodiscard]] bool isUncertainRoute() const {
    return m_currentSnapshot && m_currentSnapshot->promptTriage &&
           m_revisionState && !m_revisionState->isDraftDirty() &&
           /* last route was UncertainBuffer */;
}
```

This mirrors the existing `isOriginalRoute` pattern.

### 5.2 `FloatingCard.qml` / `AmbientBadge.qml`

| Location | Current | Proposed `UncertainBuffer` treatment |
|----------|---------|--------------------------------------|
| `FloatingCard.qml` | No intermediate state | Show muted inline status with a ⚠ icon and "Borderline — use Analyze to confirm" |
| `AmbientBadge.qml` | Green/red badge | Amber badge with "?" label when `isUncertainRoute` is true |
| `IntentReviewPanel.qml` | Not shown for clear routes | Show a non-blocking advisory row: "Classification borderline on [HEAD]" |

### 5.3 Hard-coded threshold string removal

`HostControllerTriage.cpp` line 122 currently emits:
```
"Prompt is clear (S1 score %1 < 0.70) — original wording retained..."
```

This must change to use the actual active policy threshold for the primary head, not a
hard-coded `0.70`. After the per-head policy is implemented, the host should display the
relevant head's threshold: `"... (score 0.634 on RULE_CONFLICT < 0.60) ..."`.

---

## 6. Offline Calibration Tooling Design

### 6.1 Labeled calibration corpus format

The benchmark tools currently use a fixed fixture corpus. For calibration, a labeled
dataset with ground-truth expectations and optionally annotated target heads is needed.
The format (extending the existing `granite-benchmark-fixtures.json` schema) should be:

```json
{
  "id": "example_prompt",
  "prompt": "...",
  "expectation": "clear | ambiguous",
  "target_heads": ["RULE_CONFLICT"],
  "annotator": "human | model_assisted",
  "notes": "..."
}
```

### 6.2 Tool: `tools/calibrate_julia_triage.py`

This tool (to be created in Phase 3) should compute:

1. **Reliability diagram and ECE:** Bin scores by predicted confidence; plot empirical
   positive rate per bin. Expected Calibration Error = weighted mean |conf − acc|.
2. **Per-head optimal threshold sweep:** For each head independently, sweep threshold from
   0.30 to 0.90 in steps of 0.01; compute F1, precision, recall, and FPR. Select the
   threshold maximising F1 subject to FPR constraints.
3. **Pareto frontier:** Plot Granite invocation rate (= `Clarify` + `UncertainBuffer`)
   vs. ambiguity catch rate (= TP / (TP + FN)) as threshold varies. The operating point
   is the minimum Granite cost for the required recall on the labeled corpus.
4. **Temperature sweep:** For a grid of `T ∈ {0.5, 0.7, 0.9, 1.0, 1.2, 1.5, 2.0}`,
   compute ECE and report the T that minimises ECE.

### 6.3 Integration with existing tooling

The calibration tool should reuse `load_worker_module()` and the engine loading logic from
`benchmark_julia_triage.py`. It should accept a labeled corpus path via `--corpus` and
emit a JSON calibration report with recommended per-head thresholds and the optimal T.

---

## 7. Summary Table: Current State vs. Target Architecture

| Dimension | Current | Target (v2) |
|-----------|---------|-------------|
| Hash domain | `roclint.prompt_triage.v1` (bakes 0.70) | `roclint.prompt_triage.v2` (evidence-only) |
| Historical records | Valid under v1 | Remain valid; validated by v1 domain reconstruction |
| Routing thresholds | Single global 0.70 | Per-head array via `TriagePolicy`; defaults: [0.70, 0.70, 0.70, 0.60] |
| Route outcomes | `Original`, `Clarify` | `Original`, `Clarify`, `UncertainBuffer` |
| Python probabilities | Raw sigmoid/softmax | Temperature-scaled sigmoid; `T` from `JULIA_TRIAGE_TEMPERATURE` |
| Diagnostics | None | Optional raw logit passthrough under `JULIA_TRIAGE_DIAGNOSTICS=1` |
| UI treatment of borderline | Not handled | Amber badge / non-blocking advisory; explicit re-analyze available |
| Calibration tooling | None (benchmark only) | `calibrate_julia_triage.py`: ECE, threshold sweep, Pareto, T-sweep |

---

## 8. Open Questions and Constraints

1. **`kPromptTriageThreshold` constant**: The constant must be kept for v1 backwards
   compatibility (hash reconstruction of historical records). It should be marked
   `// v1 compatibility only — do not use in new routing code` and not referenced outside
   `computePromptTriageHash` in v1 mode.

2. **`isOriginalRoute` in `HostController.hpp` line 332** still references
   `kPromptTriageThreshold` directly. After the policy migration, this must use the
   active policy's per-head thresholds, or alternatively store the last resolved route
   enum on the controller and expose it directly.

3. **`noul_certainty` at 0.70 in `julia_worker.py:338,345`**: This threshold is used in
   the `evaluate()` (per-alternative scoring) path, not in the `triage()` path. It is
   out of scope for the triage calibration work but should be noted for a follow-on
   calibration pass on the discovery-scoring path.

4. **Sealed `configurationId` and `templateId`**: The v2 seal should encode the temperature
   `T` in the `configurationId` so historical records remain reproducible. The exact format
   (e.g., suffix `+T1.00`) is TBD and should be finalised before production deployment.

5. **`UncertainBuffer` and storage persistence**: The `completeAttempt` path in
   `HostControllerTriage.cpp` records the sealed triage evidence. `UncertainBuffer` should
   result in `storage::AttemptStatus::Succeeded` (the model ran successfully; the uncertainty
   is in the *policy interpretation*, not in the execution). The route taken should be recorded
   in the attempt configuration string.
