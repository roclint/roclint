# Julia S1 Triage — Phased Implementation Plan

> **Status:** Implementation plan for review and approval. No source files are modified.
> **Companion document:** `docs/audit/julia-triage-calibration-architecture.md`
> **Rule:** All diffs are for review only. Apply only after explicit approval.

---

## Phase Overview

| Phase | Scope | Risk | Effort |
|-------|-------|------|--------|
| **Phase 1** | Hash decoupling + `TriageHashVersion` + v1 compatibility | Low — additive, all existing paths preserved | Medium |
| **Phase 2** | Per-head `TriagePolicy` + `UncertainBuffer` tier + Worker temperature scaling | Medium — adds enum value, new routing overload, Python env var | Medium |
| **Phase 3** | Offline calibration tool `calibrate_julia_triage.py` | Low — new file, no production path changes | Low |

Each phase is independently deployable. Phases 1 and 2 each require test changes.
Phase 3 is tooling-only and has no runtime impact.

---

## Phase 1 — Cryptographic Decoupling & Schema Versioning

### Goal

Remove `kPromptTriageThreshold` from the hash computation for all new records.
Preserve full v1 verification for existing records. No routing logic changes.

### 1.1 Diff — `include/roclint/analysis/PromptTriage.hpp`

```diff
--- a/include/roclint/analysis/PromptTriage.hpp
+++ b/include/roclint/analysis/PromptTriage.hpp
@@ -10,8 +10,15 @@ namespace roclint::core::analysis {
 
-inline constexpr double kPromptTriageThreshold = 0.70;
+// v1 compatibility only: used exclusively for reconstructing historical v1 hashes.
+// Do not use this constant in new routing or policy code.
+inline constexpr double kPromptTriageThreshold = 0.70;
+
 inline constexpr const char* kPromptTriageTemplateId = "julia.prompt_triage.categories.v2";
+
+// Version of the cryptographic seal. V1 bakes kPromptTriageThreshold into the hash;
+// V2 records only model evidence (scores, identity, latency, prompt binding).
+enum class TriageHashVersion { V1 = 1, V2 = 2 };
+
 enum class PromptCategory { Action, Target, Rule, RuleConflict };
 enum class PromptTriageRoute { Original, Clarify };
 enum class TriageExecution { NotExecuted, Model, Mock };
@@ -22,6 +29,7 @@ struct PromptTriage {
     // ... all existing fields ...
+    TriageHashVersion hashVersion{TriageHashVersion::V2};
     bool operator==(const PromptTriage&) const = default;
 };
```

### 1.2 Diff — `src/analysis/PromptTriage.cpp`

```diff
--- a/src/analysis/PromptTriage.cpp
+++ b/src/analysis/PromptTriage.cpp
@@ -40,12 +40,18 @@ const char* toString(PromptCategory category) noexcept {
 
 std::string computePromptTriageHash(const PromptTriage& triage) {
-    crypto::CanonicalHasher hash("roclint.prompt_triage.v1");
+    const char* domain = (triage.hashVersion == TriageHashVersion::V1)
+        ? "roclint.prompt_triage.v1"
+        : "roclint.prompt_triage.v2";
+    crypto::CanonicalHasher hash(domain);
     hash.field(triage.sessionId).field(triage.revisionId).field(triage.revisionTextHash).number(triage.stateVersion)
         .field(triage.requestGroupId).field(triage.attemptId).field(triage.provider).field(triage.modelId)
         .field(triage.modelRevision).field(triage.weightsSha256).field(triage.configurationId).field(triage.templateId)
         .number(static_cast<std::uint64_t>(triage.execution));
     for (const auto score : triage.categoryScores) hashDouble(hash, score);
     hashDouble(hash, triage.ambiguityScore);
     hash.number(static_cast<std::uint64_t>(triage.primaryCategory));
-    hashDouble(hash, kPromptTriageThreshold);
+    // v1 ONLY: bake in kPromptTriageThreshold for backwards-compatible hash reconstruction.
+    if (triage.hashVersion == TriageHashVersion::V1)
+        hashDouble(hash, kPromptTriageThreshold);
     hashDouble(hash, triage.latencyMs);
     return hash.finalizeHex();
 }
```

No changes to `validatePromptTriage`, `sealPromptTriage`, or `routePromptTriage` in Phase 1.
`sealPromptTriage` produces v2 records by default (new `PromptTriage` instances default to
`hashVersion = TriageHashVersion::V2`). Existing v1 records stored in the database continue
to verify because `validatePromptTriage` recomputes the hash using the record's own
`hashVersion` field (which defaults to V1 when deserializing records that predate the field,
since the zero value of the enum will map to V1 = 1; serialization code must handle this).

**Note on deserialization of historical records:** When loading a `PromptTriage` from
storage that predates `hashVersion`, the deserializer must set `hashVersion = V1`. The
`PayloadCodec` should treat a missing `hash_version` field as V1.

### 1.3 Tests — `tests/bridge/test_s1_triage_adapter.cpp`

**Add to `testPromptTriageOwnsTheExactGateBoundary`:**

```cpp
// --- Phase 1: v1 vs v2 hash isolation ---
// A v2 record sealed fresh must have a different hash than the same record with v1 hash version.
void testV1AndV2HashDomainsAreDistinct() {
    analysis::PromptTriage r;
    const std::string prompt = "Review this module.";
    r.sessionId = "sess_v2hash"; r.revisionId = "rev_v2hash_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 1; r.requestGroupId = "grp_v2"; r.attemptId = "att_v2";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'b'); r.configurationId = "test_only";
    r.execution = analysis::TriageExecution::Mock;
    r.categoryScores = {0.1, 0.2, 0.3, 0.4}; r.latencyMs = 2;

    r.hashVersion = analysis::TriageHashVersion::V2;
    const auto v2 = analysis::sealPromptTriage(r, prompt);
    ROCLINT_CHECK(v2.ok());
    ROCLINT_CHECK(analysis::validatePromptTriage(v2.value()).ok());

    r.hashVersion = analysis::TriageHashVersion::V1;
    const auto v1 = analysis::sealPromptTriage(r, prompt);
    ROCLINT_CHECK(v1.ok());
    ROCLINT_CHECK(analysis::validatePromptTriage(v1.value()).ok());

    // Hashes must be different because the v1 domain bakes in kPromptTriageThreshold.
    ROCLINT_CHECK(v2.value().triageHash != v1.value().triageHash);

    // A v2 record with the hashVersion manually corrupted to V1 must fail validation
    // (the recomputed hash won't match the stored one).
    auto tampered = v2.value();
    tampered.hashVersion = analysis::TriageHashVersion::V1;
    ROCLINT_CHECK(!analysis::validatePromptTriage(tampered).ok());

    // A v1 record manually changed to V2 must similarly fail.
    auto tampered2 = v1.value();
    tampered2.hashVersion = analysis::TriageHashVersion::V2;
    ROCLINT_CHECK(!analysis::validatePromptTriage(tampered2).ok());
}
```

**Add new test to `tests/analysis/` (new file `test_upstream_compatibility.cpp` or
extend `test_analysis_checkpoint.cpp`):**

```cpp
// Verify that the v1 hash is byte-for-byte identical to what the pre-Phase-1 code
// would have produced (regression guard for historical record integrity).
void testV1HashMatchesLegacyValue() {
    analysis::PromptTriage r;
    const std::string prompt = "Review this request.";
    r.sessionId = "sess_triage"; r.revisionId = "rev_sess_triage_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 2; r.requestGroupId = "grp_triage"; r.attemptId = "att_triage";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'a'); r.configurationId = "test_only";
    r.execution = analysis::TriageExecution::Mock;
    r.categoryScores = {0.1, 0.2, 0.69, 0.3}; r.latencyMs = 1;
    r.hashVersion = analysis::TriageHashVersion::V1;

    // This hash value is computed from the pre-Phase-1 code and pinned here.
    // It MUST NOT change; any change indicates a regression in historical record verifiability.
    const auto sealed = analysis::sealPromptTriage(r, prompt);
    ROCLINT_CHECK(sealed.ok());
    // Pin the known-good hash from the current code before the change:
    // Run the existing tests to capture the hash, then hard-code it here.
    // ROCLINT_CHECK(sealed.value().triageHash == "<known_v1_hash_pinned_here>");
    // Note: The exact hash string must be filled in after a pre-Phase-1 test run.
    ROCLINT_CHECK(analysis::validatePromptTriage(sealed.value()).ok());
}
```

---

## Phase 2 — Per-Head Policy, `UncertainBuffer`, and Temperature Scaling

### Goal

Add `TriagePolicy`, introduce `UncertainBuffer` route, update the host UI treatment,
and add temperature scaling to the Python worker.

### 2.1 Diff — `include/roclint/analysis/PromptTriage.hpp`

```diff
+// Per-head routing policy. Passed to routePromptTriage(); never enters the sealed record.
+struct TriagePolicy {
+    // Ambiguity thresholds per head: [ACTION, TARGET, RULE, RULE_CONFLICT].
+    // Route to Clarify when categoryScores[i] >= thresholds[i].
+    std::array<double, 4> thresholds{0.70, 0.70, 0.70, 0.60};
+
+    // Dead-band half-width δ.
+    // A head in [thresholds[i] - uncertaintyDelta, thresholds[i]) contributes
+    // to UncertainBuffer when no head is in the Clarify zone.
+    double uncertaintyDelta{0.08};
+
+    // Human-readable policy identifier for logging and status messages.
+    std::string policyId{"roclint.triage_policy.v1"};
+
+    [[nodiscard]] static TriagePolicy defaults() noexcept { return {}; }
+};
+
-enum class PromptTriageRoute { Original, Clarify };
+enum class PromptTriageRoute { Original, Clarify, UncertainBuffer };
+
+[[nodiscard]] const char* toString(PromptTriageRoute route) noexcept;
+
-[[nodiscard]] Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage);
+// Backwards-compatible overload: uses TriagePolicy::defaults().
+[[nodiscard]] Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage);
+// Policy-aware overload: uses the supplied policy for per-head thresholds and dead band.
+[[nodiscard]] Result<PromptTriageRoute> routePromptTriage(
+    const PromptTriage& triage, const TriagePolicy& policy);
```

### 2.2 Diff — `src/analysis/PromptTriage.cpp`

```diff
+const char* toString(PromptTriageRoute route) noexcept {
+    switch (route) {
+        case PromptTriageRoute::Original:       return "ORIGINAL";
+        case PromptTriageRoute::Clarify:        return "CLARIFY";
+        case PromptTriageRoute::UncertainBuffer: return "UNCERTAIN_BUFFER";
+    }
+    return "UNKNOWN";
+}
+
-Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage) {
-    if (const auto status = validatePromptTriage(triage); !status.ok()) return status.error();
-    return triage.ambiguityScore < kPromptTriageThreshold ? PromptTriageRoute::Original : PromptTriageRoute::Clarify;
-}
+Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage) {
+    return routePromptTriage(triage, TriagePolicy::defaults());
+}
+
+Result<PromptTriageRoute> routePromptTriage(
+    const PromptTriage& triage, const TriagePolicy& policy) {
+    if (const auto status = validatePromptTriage(triage); !status.ok()) return status.error();
+    bool inDeadBand = false;
+    for (std::size_t i = 0; i < triage.categoryScores.size(); ++i) {
+        const double score = triage.categoryScores[i];
+        const double thr   = policy.thresholds[i];
+        if (score >= thr)
+            return PromptTriageRoute::Clarify;
+        if (score >= thr - policy.uncertaintyDelta)
+            inDeadBand = true;
+    }
+    return inDeadBand ? PromptTriageRoute::UncertainBuffer : PromptTriageRoute::Original;
+}
```

### 2.3 Diff — `app/host/HostControllerTriage.cpp`

```diff
@@ -103,7 +103,7 @@ void HostController::onPromptClassified(
     const auto route = an::routePromptTriage(*triage);
 
@@ -112,10 +112,20 @@ void HostController::onPromptClassified(
     m_promptTriage = *triage;
     m_routedAnalysisGroup = ticket.analysisGroup;
     if (route.value() == an::PromptTriageRoute::Original) {
         ticket.analysisGroup->terminal = true;
         an::NormalizedAnalysis empty;
         empty.sessionId = ticket.sessionId; empty.baseRevisionId = ticket.revisionId;
         an::sealSnapshot(empty);
         if (applyAnalysis(std::move(empty), {}, {}, std::nullopt, *triage))
-            setAnalysisStatus(AnalysisStatus::Ready, QStringLiteral("Prompt is clear (S1 score %1 < 0.70) — original wording retained (Granite skipped to save battery/latency).")
+            setAnalysisStatus(AnalysisStatus::Ready, QStringLiteral("Prompt is clear (S1 score %1) — original wording retained (Granite skipped to save battery/latency).")
                 .arg(triage->ambiguityScore, 0, 'f', 3));
-    } else if (const auto status = startDiscovery(); !status.ok()) setLastError(status.error());
+    } else if (route.value() == an::PromptTriageRoute::UncertainBuffer) {
+        ticket.analysisGroup->terminal = true;
+        an::NormalizedAnalysis empty;
+        empty.sessionId = ticket.sessionId; empty.baseRevisionId = ticket.revisionId;
+        an::sealSnapshot(empty);
+        if (applyAnalysis(std::move(empty), {}, {}, std::nullopt, *triage))
+            setAnalysisStatus(AnalysisStatus::Ready,
+                QStringLiteral("S1 score is borderline (%1 on %2) — original wording retained "
+                               "but clarity is not confirmed. Use Analyze to review with Granite.")
+                    .arg(triage->ambiguityScore, 0, 'f', 3)
+                    .arg(QString::fromUtf8(an::toString(triage->primaryCategory))));
+    } else {
+        if (const auto status = startDiscovery(); !status.ok()) setLastError(status.error());
+    }
```

**`isOriginalRoute` in `HostController.hpp` line 330–333** — add `isUncertainRoute`:

```diff
+    [[nodiscard]] bool isUncertainRoute() const {
+        // Mirrors isOriginalRoute; true when the last sealed triage produced UncertainBuffer.
+        return m_currentSnapshot && m_currentSnapshot->promptTriage && m_revisionState &&
+               !m_revisionState->isDraftDirty() &&
+               m_promptTriageLastRoute == an::PromptTriageRoute::UncertainBuffer;
+    }
```

This requires storing `m_promptTriageLastRoute` as a new private member:

```diff
+    an::PromptTriageRoute m_promptTriageLastRoute{an::PromptTriageRoute::Original};
```

Set in `onPromptClassified` after `route.value()` is resolved.

Add `isUncertainRoute` as a Q_PROPERTY (emits `analysisStatusChanged`):

```diff
+    Q_PROPERTY(bool isUncertainRoute READ isUncertainRoute NOTIFY analysisStatusChanged)
```

### 2.4 Diff — `app/runtime/julia_worker.py`

```diff
@@ -27,6 +27,12 @@ TRIAGE_CATEGORIES = ("ACTION", "TARGET", "RULE", "RULE_CONFLICT")
+
+# Temperature scaling for triage head probabilities.
+# T=1.0 is the identity (current raw sigmoid behaviour).
+# T>1.0 softens overconfident predictions toward calibrated frequencies.
+# Calibrate T offline using tools/calibrate_julia_triage.py on a labeled corpus.
+_TRIAGE_TEMPERATURE = float(os.environ.get("JULIA_TRIAGE_TEMPERATURE", "1.0"))
+if not (0.1 <= _TRIAGE_TEMPERATURE <= 10.0):
+    raise ValueError("JULIA_TRIAGE_TEMPERATURE must be between 0.1 and 10.0")
+
+_TRIAGE_DIAGNOSTICS = os.environ.get("JULIA_TRIAGE_DIAGNOSTICS") == "1"

@@ -254,7 +260,11 @@ def triage(engine, request):
     scores = []
+    raw_logits_out = [] if _TRIAGE_DIAGNOSTICS else None
     for category, values in zip(TRIAGE_CATEGORIES, logits):
-        probabilities = softmax(values)
+        scaled_values = [v / _TRIAGE_TEMPERATURE for v in values]
+        probabilities = softmax(scaled_values)
         if len(probabilities) != 2:
             raise ValueError("Julia returned a malformed Boolean triage head")
         scores.append({"category": category, "score": probabilities[1]})
+        if raw_logits_out is not None:
+            raw_logits_out.append([float(v) for v in values])
     result = {"triage_plan_version": TRIAGE_PLAN_VERSION, "template_id": TRIAGE_TEMPLATE_ID, "category_scores": scores}
     if hasattr(engine, "roclint_runtime"):
         result["execution_configuration_id"] = engine.roclint_runtime["configuration_id"]
+    if raw_logits_out is not None:
+        result["raw_logits"] = raw_logits_out  # diagnostics only; never used in routing
     return result
```

**Important constraint:** `_TRIAGE_TEMPERATURE` is validated at module load time, not per
request. A misconfigured value (e.g., negative or zero) fails the worker startup, not a
single request. This prevents silent misconfiguration from producing incorrect routing
across all subsequent requests.

The `execution_configuration_id` should encode T when T ≠ 1.0 for auditability:

```diff
     config_id = (
         "julia.cuda.bf16.v2"      if device == "cuda"      else
         "julia.directml.fp16.v2"  if device == "directml"  else
         "julia.mps.fp16.v2"       if device == "mps"        else
         "julia.cpu.fp32.v2"
     )
+    if _TRIAGE_TEMPERATURE != 1.0:
+        config_id += f"+T{_TRIAGE_TEMPERATURE:.2f}"
```

---

## Phase 3 — Offline Calibration Tool (`tools/calibrate_julia_triage.py`)

### Goal

Provide a reproducible offline workflow for:
1. Measuring calibration (reliability diagram, ECE)
2. Optimizing per-head thresholds via threshold sweep
3. Plotting the Pareto frontier of Granite invocation cost vs. ambiguity catch rate
4. Selecting the optimal temperature `T`

### 3.1 New file: `tools/calibrate_julia_triage.py`

```python
"""Offline calibration and threshold optimization for Julia S1 triage.

Outputs:
  - Reliability diagram data (ECE per head)
  - Per-head optimal thresholds (F1-maximizing, FPR-bounded)
  - Pareto frontier: Granite invocation rate vs. ambiguity recall
  - Optimal temperature T (ECE-minimizing across a grid)

Usage:
  python tools/calibrate_julia_triage.py \\
      --model-dir <path> \\
      --corpus <path/to/labeled_corpus.json> \\
      --output build/calibration_report.json

Corpus format: list of objects with fields:
  id          (str)  unique case identifier
  prompt      (str)  the prompt text
  expectation ("clear" | "ambiguous")  ground truth
  target_heads (list[str], optional)  expected firing heads for ambiguous cases
"""

import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
WORKER_PATH = ROOT / "app/runtime/julia_worker.py"
DEFAULT_MODEL_DIR = ROOT / "models/s1"
TEMPERATURE_GRID = [0.5, 0.7, 0.9, 1.0, 1.2, 1.5, 2.0]
THRESHOLD_SWEEP = [round(t * 0.01, 2) for t in range(30, 91)]
HEADS = ("ACTION", "TARGET", "RULE", "RULE_CONFLICT")
N_BINS = 10


def load_worker_module():
    spec = importlib.util.spec_from_file_location("julia_worker", WORKER_PATH)
    worker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(worker)
    return worker


def sigmoid(logit):
    return 1.0 / (1.0 + math.exp(-logit))


def softmax_temperature(values, T):
    """Numerically stable softmax with temperature scaling."""
    scaled = [v / T for v in values]
    top = max(scaled)
    exps = [math.exp(v - top) for v in scaled]
    total = sum(exps)
    return [e / total for e in exps]


def collect_raw_logits(engine, worker, cases):
    """Collect raw logits from the model for each case, bypassing worker temperature."""
    import importlib
    # Temporarily set JULIA_TRIAGE_DIAGNOSTICS to get raw logits
    os.environ["JULIA_TRIAGE_DIAGNOSTICS"] = "1"
    try:
        results = []
        for case in cases:
            req = {"triage_plan_version": worker.TRIAGE_PLAN_VERSION, "prompt": case["prompt"]}
            res = worker.triage(engine, req)
            results.append({
                "id": case["id"],
                "expectation": case["expectation"],
                "raw_logits": res.get("raw_logits"),  # None if diagnostics not enabled in worker
                "category_scores": res["category_scores"],
            })
        return results
    finally:
        del os.environ["JULIA_TRIAGE_DIAGNOSTICS"]


def compute_ece(cases_with_scores, head_name, threshold, n_bins=N_BINS):
    """Expected Calibration Error for a single head at a given threshold."""
    scores = [
        next(s["score"] for s in r["category_scores"] if s["category"] == head_name)
        for r in cases_with_scores
    ]
    labels = [1 if r["expectation"] == "ambiguous" else 0 for r in cases_with_scores]
    bins = [[] for _ in range(n_bins)]
    for score, label in zip(scores, labels):
        bin_idx = min(int(score * n_bins), n_bins - 1)
        bins[bin_idx].append((score, label))
    ece = 0.0
    n = len(scores)
    reliability = []
    for b in bins:
        if not b:
            continue
        conf = sum(s for s, _ in b) / len(b)
        acc = sum(l for _, l in b) / len(b)
        reliability.append({"conf": conf, "acc": acc, "n": len(b)})
        ece += (len(b) / n) * abs(conf - acc)
    return ece, reliability


def sweep_threshold(cases_with_scores, head_name):
    """Sweep threshold for a single head, return list of {thr, tp, fp, tn, fn, f1, fpr, precision, recall}."""
    scores = [
        next(s["score"] for s in r["category_scores"] if s["category"] == head_name)
        for r in cases_with_scores
    ]
    labels = [1 if r["expectation"] == "ambiguous" else 0 for r in cases_with_scores]
    sweep = []
    for thr in THRESHOLD_SWEEP:
        tp = sum(1 for s, l in zip(scores, labels) if s >= thr and l == 1)
        fp = sum(1 for s, l in zip(scores, labels) if s >= thr and l == 0)
        tn = sum(1 for s, l in zip(scores, labels) if s < thr and l == 0)
        fn = sum(1 for s, l in zip(scores, labels) if s < thr and l == 1)
        precision = tp / (tp + fp) if (tp + fp) > 0 else 0.0
        recall    = tp / (tp + fn) if (tp + fn) > 0 else 0.0
        f1        = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0.0
        fpr       = fp / (fp + tn) if (fp + tn) > 0 else 0.0
        sweep.append({"thr": thr, "tp": tp, "fp": fp, "tn": tn, "fn": fn,
                      "f1": f1, "fpr": fpr, "precision": precision, "recall": recall})
    return sweep


def pareto_frontier(cases_with_scores, per_head_thresholds, delta=0.08):
    """Compute Granite invocation rate vs. ambiguity recall as global threshold scales."""
    # Use the ratio t_head / t_base to scale all thresholds proportionally.
    base_thresholds = list(per_head_thresholds)
    pareto = []
    for scale in [round(s * 0.02, 2) for s in range(15, 55)]:  # 0.30 .. 1.08
        scaled = [min(t * scale / 0.70, 1.0) for t in base_thresholds]
        tp = fp = tn = fn = 0
        for r in cases_with_scores:
            scores = [next(s["score"] for s in r["category_scores"] if s["category"] == h) for h in HEADS]
            clarify = any(scores[i] >= scaled[i] for i in range(4))
            uncertain = (not clarify) and any(scores[i] >= scaled[i] - delta for i in range(4))
            invoked = clarify or uncertain
            is_ambig = r["expectation"] == "ambiguous"
            if invoked and is_ambig: tp += 1
            elif invoked and not is_ambig: fp += 1
            elif not invoked and not is_ambig: tn += 1
            else: fn += 1
        recall = tp / (tp + fn) if (tp + fn) > 0 else 0.0
        invocation_rate = (tp + fp) / len(cases_with_scores)
        pareto.append({"scale": scale, "thresholds": scaled, "invocation_rate": invocation_rate,
                       "recall": recall, "tp": tp, "fp": fp, "tn": tn, "fn": fn})
    return pareto


def main():
    parser = argparse.ArgumentParser(description="Julia S1 Triage Calibration Tool")
    parser.add_argument("--model-dir", default=str(DEFAULT_MODEL_DIR))
    parser.add_argument("--corpus", required=True, help="Path to labeled corpus JSON")
    parser.add_argument("--device", default="cpu", choices=["cpu", "cuda", "mps"])
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/calibration_report.json")
    args = parser.parse_args()

    model_dir = Path(args.model_dir)
    corpus = json.loads(Path(args.corpus).read_text(encoding="utf-8"))

    worker = load_worker_module()
    os.environ["JULIA_DEVICE"] = args.device
    # Load model at T=1.0 (identity) to get raw logit access via diagnostics
    os.environ["JULIA_TRIAGE_TEMPERATURE"] = "1.0"
    engine = worker.load(model_dir)

    print(f"Loaded Julia on {args.device}. Corpus: {len(corpus)} cases.")

    # Step 1: Collect scores at T=1.0
    raw_results = collect_raw_logits(engine, worker, corpus)

    # Step 2: ECE and reliability diagram at T=1.0
    print("\n--- ECE at T=1.0 ---")
    ece_report = {}
    for head in HEADS:
        ece, reliability = compute_ece(raw_results, head, threshold=0.70)
        ece_report[head] = {"ece": ece, "reliability": reliability}
        print(f"  {head}: ECE={ece:.4f}")

    # Step 3: Per-head threshold sweep
    print("\n--- Per-Head Threshold Sweep (F1 maximization) ---")
    threshold_sweep = {}
    optimal_thresholds = []
    for head in HEADS:
        sweep = sweep_threshold(raw_results, head)
        best = max(sweep, key=lambda x: x["f1"])
        threshold_sweep[head] = sweep
        optimal_thresholds.append(best["thr"])
        print(f"  {head}: optimal thr={best['thr']:.2f}, F1={best['f1']:.3f}, "
              f"Precision={best['precision']:.3f}, Recall={best['recall']:.3f}, FPR={best['fpr']:.3f}")

    # Step 4: Pareto frontier
    print("\n--- Pareto Frontier (Granite invocation rate vs. recall) ---")
    pareto = pareto_frontier(raw_results, optimal_thresholds)
    for point in pareto[::3]:  # print every third point
        print(f"  scale={point['scale']:.2f}: invocation={point['invocation_rate']:.2f}, "
              f"recall={point['recall']:.2f}")

    # Step 5: Temperature sweep (uses raw logits if available)
    print("\n--- Temperature Sweep (ECE minimization) ---")
    temp_report = {}
    has_raw_logits = all(r.get("raw_logits") is not None for r in raw_results)
    if has_raw_logits:
        for T in TEMPERATURE_GRID:
            # Re-score using raw logits at temperature T
            rescored = []
            for r in raw_results:
                new_scores = []
                for i, head in enumerate(HEADS):
                    probs = softmax_temperature(r["raw_logits"][i], T)
                    new_scores.append({"category": head, "score": probs[1]})
                rescored.append({**r, "category_scores": new_scores})
            # Compute mean ECE across all heads at T
            mean_ece = sum(compute_ece(rescored, h, 0.70)[0] for h in HEADS) / 4
            temp_report[str(T)] = mean_ece
            print(f"  T={T:.2f}: mean ECE={mean_ece:.4f}")
        best_T = min(temp_report, key=temp_report.__getitem__)
        print(f"  Optimal T: {best_T} (ECE={temp_report[best_T]:.4f})")
    else:
        print("  Raw logits not available (JULIA_TRIAGE_DIAGNOSTICS not supported). "
              "Enable JULIA_TRIAGE_DIAGNOSTICS=1 in worker to run temperature sweep.")
        best_T = "1.0"

    # Save report
    report = {
        "model_dir": str(model_dir),
        "device": args.device,
        "corpus_path": args.corpus,
        "corpus_size": len(corpus),
        "heads": list(HEADS),
        "ece_at_T1": ece_report,
        "optimal_thresholds": dict(zip(HEADS, optimal_thresholds)),
        "threshold_sweep": threshold_sweep,
        "pareto_frontier": pareto,
        "temperature_sweep": temp_report,
        "optimal_temperature": best_T,
        "recommended_policy": {
            "thresholds": optimal_thresholds,
            "uncertainty_delta": 0.08,
            "temperature": float(best_T),
        }
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"\nCalibration report saved to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

---

## Test Cases to Add

### `tests/bridge/test_s1_triage_adapter.cpp` — New test functions

#### `testV1AndV2HashDomainsAreDistinct`
*(Full code given in Phase 1 section above)*

Asserts: v1 and v2 seals of the same record produce different hashes; tampered version
fields fail validation.

#### `testPerHeadPolicyRouting`

```cpp
void testPerHeadPolicyRouting() {
    using namespace roclint::core::analysis;

    const std::string prompt = "Apply the patch.";
    PromptTriage base;
    base.sessionId = "sess_policy"; base.revisionId = "rev_policy_1";
    base.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    base.stateVersion = 1; base.requestGroupId = "grp_policy"; base.attemptId = "att_policy";
    base.modelId = "fixture_julia"; base.modelRevision = "fixture_v1";
    base.weightsSha256 = std::string(64, 'c'); base.configurationId = "test_only";
    base.execution = TriageExecution::Mock;
    base.latencyMs = 1;

    TriagePolicy policy;
    policy.thresholds = {0.70, 0.70, 0.70, 0.60};
    policy.uncertaintyDelta = 0.08;

    // Case 1: RULE_CONFLICT at 0.62 — below global 0.70 but above 0.60 threshold → Clarify
    base.categoryScores = {0.10, 0.15, 0.20, 0.62};
    const auto sealed1 = sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed1.ok());
    const auto route1 = routePromptTriage(sealed1.value(), policy);
    ROCLINT_CHECK(route1.ok() && route1.value() == PromptTriageRoute::Clarify);

    // Case 2: All heads at 0.55 — below all thresholds, RULE_CONFLICT in dead band → UncertainBuffer
    base.categoryScores = {0.55, 0.55, 0.55, 0.55};
    const auto sealed2 = sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed2.ok());
    const auto route2 = routePromptTriage(sealed2.value(), policy);
    ROCLINT_CHECK(route2.ok() && route2.value() == PromptTriageRoute::UncertainBuffer);

    // Case 3: All heads below dead band → Original
    base.categoryScores = {0.20, 0.15, 0.10, 0.30};
    const auto sealed3 = sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed3.ok());
    const auto route3 = routePromptTriage(sealed3.value(), policy);
    ROCLINT_CHECK(route3.ok() && route3.value() == PromptTriageRoute::Original);

    // Case 4: ACTION at 0.65 (in dead band for thr=0.70, delta=0.08 → [0.62, 0.70)) → UncertainBuffer
    base.categoryScores = {0.65, 0.20, 0.15, 0.10};
    const auto sealed4 = sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed4.ok());
    const auto route4 = routePromptTriage(sealed4.value(), policy);
    ROCLINT_CHECK(route4.ok() && route4.value() == PromptTriageRoute::UncertainBuffer);

    // Case 5: Default overload (no policy) should use TriagePolicy::defaults()
    // — same result as passing defaults explicitly
    base.categoryScores = {0.62, 0.20, 0.15, 0.10}; // ACTION in dead band
    const auto sealed5 = sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed5.ok());
    const auto routeDefault = routePromptTriage(sealed5.value());
    const auto routeExplicit = routePromptTriage(sealed5.value(), TriagePolicy::defaults());
    ROCLINT_CHECK(routeDefault.ok() && routeExplicit.ok());
    ROCLINT_CHECK(routeDefault.value() == routeExplicit.value());
}
```

#### `testUncertainBufferDoesNotClarifyWithoutDeadBand`

```cpp
void testUncertainBufferDoesNotClarifyWithoutDeadBand() {
    using namespace roclint::core::analysis;

    const std::string prompt = "Delete it.";
    PromptTriage base;
    base.sessionId = "sess_db"; base.revisionId = "rev_db_1";
    base.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    base.stateVersion = 1; base.requestGroupId = "grp_db"; base.attemptId = "att_db";
    base.modelId = "fixture_julia"; base.modelRevision = "fixture_v1";
    base.weightsSha256 = std::string(64, 'd'); base.configurationId = "test_only";
    base.execution = TriageExecution::Mock; base.latencyMs = 1;

    // Zero delta policy: no dead band, binary routing only
    TriagePolicy noDeadBand;
    noDeadBand.thresholds = {0.70, 0.70, 0.70, 0.70};
    noDeadBand.uncertaintyDelta = 0.0;

    base.categoryScores = {0.65, 0.66, 0.67, 0.68};
    const auto sealed = sealPromptTriage(base, prompt);
    ROCLINT_CHECK(sealed.ok());
    const auto route = routePromptTriage(sealed.value(), noDeadBand);
    // All below threshold, delta=0 → no dead band → Original
    ROCLINT_CHECK(route.ok() && route.value() == PromptTriageRoute::Original);
}
```

### New file: `tests/analysis/test_upstream_compatibility.cpp`

```cpp
// Regression guard: verifies that v1 hash values are stable across code changes,
// ensuring historical audit records remain verifiable.

#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "support/check.hpp"

using namespace roclint::core;
using namespace roclint::core::analysis;

namespace {

void testV1HashStability() {
    PromptTriage r;
    const std::string prompt = "Review this request.";
    r.sessionId = "sess_triage"; r.revisionId = "rev_sess_triage_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 2; r.requestGroupId = "grp_triage"; r.attemptId = "att_triage";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'a'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock;
    r.categoryScores = {0.1, 0.2, 0.69, 0.3}; r.latencyMs = 1;
    r.hashVersion = TriageHashVersion::V1;

    const auto sealed = sealPromptTriage(r, prompt);
    ROCLINT_CHECK(sealed.ok());

    // Pinned v1 hash — must NOT change. Compute from pre-Phase-1 code and record here.
    // ROCLINT_CHECK(sealed.value().triageHash == "<PINNED_V1_HASH>");
    ROCLINT_CHECK(analysis::validatePromptTriage(sealed.value()).ok());
}

void testV2HashDoesNotIncludeThreshold() {
    PromptTriage r;
    const std::string prompt = "Review this request.";
    r.sessionId = "sess_v2"; r.revisionId = "rev_v2_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 1; r.requestGroupId = "grp_v2"; r.attemptId = "att_v2";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'e'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock;
    r.categoryScores = {0.1, 0.2, 0.3, 0.4}; r.latencyMs = 2;
    r.hashVersion = TriageHashVersion::V2;

    const auto sealed = sealPromptTriage(r, prompt);
    ROCLINT_CHECK(sealed.ok());
    ROCLINT_CHECK(validatePromptTriage(sealed.value()).ok());

    // Change kPromptTriageThreshold... we can't, but we verify the v2 hash is independent
    // by checking that the same record hashed as v1 differs.
    auto v1copy = r;
    v1copy.hashVersion = TriageHashVersion::V1;
    const auto sealedV1 = sealPromptTriage(v1copy, prompt);
    ROCLINT_CHECK(sealedV1.ok());
    ROCLINT_CHECK(sealed.value().triageHash != sealedV1.value().triageHash);
}

void testRouteWithPerHeadPolicyMatchesExpected() {
    // Integration test: verify the three routing tiers in combination.
    const std::string prompt = "Standardize all imports.";
    PromptTriage r;
    r.sessionId = "sess_it"; r.revisionId = "rev_it_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 1; r.requestGroupId = "grp_it"; r.attemptId = "att_it";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'f'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock; r.latencyMs = 3;

    TriagePolicy policy;
    policy.thresholds = {0.70, 0.70, 0.70, 0.60};
    policy.uncertaintyDelta = 0.08;

    struct TestVector { std::array<double, 4> scores; PromptTriageRoute expected; };
    const TestVector vectors[] = {
        {{0.05, 0.10, 0.08, 0.12}, PromptTriageRoute::Original},       // all clear
        {{0.65, 0.20, 0.10, 0.20}, PromptTriageRoute::UncertainBuffer}, // ACTION in dead band
        {{0.80, 0.20, 0.10, 0.20}, PromptTriageRoute::Clarify},         // ACTION above threshold
        {{0.20, 0.20, 0.20, 0.62}, PromptTriageRoute::Clarify},         // RULE_CONFLICT above 0.60
        {{0.20, 0.20, 0.20, 0.55}, PromptTriageRoute::UncertainBuffer}, // RULE_CONFLICT in [0.52, 0.60)
        {{0.20, 0.20, 0.20, 0.51}, PromptTriageRoute::Original},        // RULE_CONFLICT below dead band
    };

    for (const auto& v : vectors) {
        r.categoryScores = v.scores;
        const auto sealed = sealPromptTriage(r, prompt);
        ROCLINT_CHECK(sealed.ok());
        const auto route = routePromptTriage(sealed.value(), policy);
        ROCLINT_CHECK(route.ok() && route.value() == v.expected);
    }
}

} // namespace

int main() {
    std::cout << "[Test UpstreamCompatibility] Starting...\n";
    testV1HashStability();
    testV2HashDoesNotIncludeThreshold();
    testRouteWithPerHeadPolicyMatchesExpected();
    std::cout << "[Test UpstreamCompatibility] All tests PASSED.\n";
    return 0;
}
```

---

## Implementation Sequence and Validation Checkpoints

### Phase 1 Checklist

- [ ] `TriageHashVersion` enum added to `PromptTriage.hpp`
- [ ] `hashVersion` field added to `PromptTriage` struct (defaults to `V2`)
- [ ] `computePromptTriageHash` dispatches on `hashVersion`
- [ ] `validatePromptTriage` passes for both v1 and v2 sealed records
- [ ] `sealPromptTriage` produces v2 records by default
- [ ] `PayloadCodec` treats missing `hash_version` in stored records as V1
- [ ] `kPromptTriageThreshold` annotated `// v1 compatibility only`
- [ ] All existing tests pass unchanged
- [ ] `testV1AndV2HashDomainsAreDistinct` added and passes
- [ ] `test_upstream_compatibility.cpp` created with pinned v1 hash

### Phase 2 Checklist

- [ ] `TriagePolicy` struct added to `PromptTriage.hpp`
- [ ] `UncertainBuffer` added to `PromptTriageRoute` enum
- [ ] `toString(PromptTriageRoute)` added
- [ ] Two-overload `routePromptTriage` implemented in `PromptTriage.cpp`
- [ ] `HostControllerTriage.cpp` handles `UncertainBuffer` as non-blocking pass
- [ ] Hard-coded `< 0.70` string removed from status message
- [ ] `isUncertainRoute` Q_PROPERTY added to `HostController`
- [ ] `m_promptTriageLastRoute` member added and set in `onPromptClassified`
- [ ] `JULIA_TRIAGE_TEMPERATURE` environment variable read in `julia_worker.py`
- [ ] Temperature validation at module load
- [ ] `JULIA_TRIAGE_DIAGNOSTICS` raw logit passthrough added
- [ ] `execution_configuration_id` encodes T when T ≠ 1.0
- [ ] All existing tests pass unchanged
- [ ] `testPerHeadPolicyRouting` added and passes
- [ ] `testUncertainBufferDoesNotClarifyWithoutDeadBand` added and passes
- [ ] `testRouteWithPerHeadPolicyMatchesExpected` added and passes

### Phase 3 Checklist

- [ ] `tools/calibrate_julia_triage.py` created
- [ ] Tool runs end-to-end on a small labeled corpus (even 10 cases)
- [ ] Produces valid JSON report with `recommended_policy`
- [ ] Pareto frontier output is sane (monotone in invocation rate vs. recall)
- [ ] Temperature sweep produces lower ECE than T=1.0 for an overconfident model

---

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| Historical v1 records fail validation after Phase 1 | Low | High | `hashVersion` defaults to V1 on deserialization; pinned test guards the v1 hash |
| `UncertainBuffer` leaks into `isOriginalRoute` binding | Low | Medium | Explicit check in `isOriginalRoute`: tests `ambiguityScore < threshold` for primary head, not route enum |
| Temperature T=1.0 default breaks existing worker tests | None | — | T=1.0 is identity; no output change |
| `RULE_CONFLICT` threshold lowering (0.70→0.60) over-triggers | Medium | Low | Benchmark shows existing conflict cases score 0.95+; borderline conflicts were previously missed |
| Phase 2 new `PromptTriageRoute::UncertainBuffer` value breaks QML serialization | Low | Low | Route enum is not directly serialized to QML; `analysisStatus` string is used |
