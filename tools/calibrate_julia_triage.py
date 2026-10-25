"""Offline calibration and threshold optimization for Julia S1 triage.

Outputs a JSON calibration report containing:
  - Reliability diagram data (ECE per head at T=1.0)
  - Per-head optimal thresholds (F1-maximizing sweep from 0.30 to 0.90)
  - Pareto frontier: Granite invocation rate vs. ambiguity recall
  - Optimal temperature T (ECE-minimizing across a configurable grid)
  - Recommended TriagePolicy JSON (drop into HostController config or env vars)

Usage:
  python tools/calibrate_julia_triage.py \\
      --model-dir <path-to-julia-1-checkpoint> \\
      --corpus   <path-to-labeled-corpus.json> \\
      --output   build/calibration_report.json \\
      [--device  cpu|cuda|mps] \\
      [--pareto-delta 0.08]

Corpus format — JSON array of objects:
  {
    "id":           "unique_case_id",
    "prompt":       "the prompt text",
    "expectation":  "clear" | "ambiguous",
    "target_heads": ["RULE_CONFLICT"],  // optional, for diagnostic only
    "notes":        "..."               // optional
  }
"""

import argparse
import json
import math
import os
from pathlib import Path
import sys
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
WORKER_PATH = ROOT / "app/runtime/julia_worker.py"
HEADS = ("ACTION", "TARGET", "RULE", "RULE_CONFLICT")
TEMPERATURE_GRID = [0.5, 0.7, 0.9, 1.0, 1.2, 1.5, 2.0]
N_BINS = 10
THRESHOLD_STEP = 0.01


def find_default_model_dir():
    candidates = [
        Path.home() / "models/s1",
        Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share")) / "roclint/Roclint/models/s1",
        Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share")) / "roclint/Roclint/models/julia-1",
    ]
    for c in candidates:
        if c.is_dir() and (c / "model.safetensors").is_file():
            return c
    return candidates[0]


def load_worker_module():
    spec = importlib.util.spec_from_file_location("julia_worker", WORKER_PATH)
    worker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(worker)
    return worker


def softmax_temperature(values, T):
    """Numerically stable softmax with temperature scaling."""
    scaled = [v / T for v in values]
    top = max(scaled)
    exps = [math.exp(v - top) for v in scaled]
    total = sum(exps)
    return [e / total for e in exps]


def collect_raw_results(engine, worker, corpus):
    """Run triage on all corpus cases with JULIA_TRIAGE_DIAGNOSTICS=1 to get raw logits."""
    os.environ["JULIA_TRIAGE_DIAGNOSTICS"] = "1"
    results = []
    try:
        for case in corpus:
            req = {"triage_plan_version": worker.TRIAGE_PLAN_VERSION, "prompt": case["prompt"]}
            res = worker.triage(engine, req)
            results.append({
                "id": case["id"],
                "expectation": case["expectation"],
                "target_heads": case.get("target_heads", []),
                "category_scores": res["category_scores"],
                "raw_logits": res.get("raw_logits"),  # None if worker predates diagnostics
            })
    finally:
        del os.environ["JULIA_TRIAGE_DIAGNOSTICS"]
    return results


def compute_ece(results, head_name, n_bins=N_BINS):
    """Expected Calibration Error for a single head.

    Returns (ece_float, reliability_list) where each reliability entry is
    {"conf": float, "acc": float, "n": int} describing one bin.
    """
    scores = [
        next(s["score"] for s in r["category_scores"] if s["category"] == head_name)
        for r in results
    ]
    labels = [1 if r["expectation"] == "ambiguous" else 0 for r in results]
    bins = [[] for _ in range(n_bins)]
    for score, label in zip(scores, labels):
        idx = min(int(score * n_bins), n_bins - 1)
        bins[idx].append((score, label))
    ece = 0.0
    n = len(scores)
    reliability = []
    for b in bins:
        if not b:
            continue
        conf = sum(s for s, _ in b) / len(b)
        acc = sum(lbl for _, lbl in b) / len(b)
        reliability.append({"conf": round(conf, 4), "acc": round(acc, 4), "n": len(b)})
        ece += (len(b) / n) * abs(conf - acc)
    return round(ece, 6), reliability


def sweep_threshold(results, head_name):
    """Sweep threshold for a single head from 0.30 to 0.90.

    Returns a list of dicts: {thr, tp, fp, tn, fn, f1, precision, recall, fpr}
    """
    scores = [
        next(s["score"] for s in r["category_scores"] if s["category"] == head_name)
        for r in results
    ]
    labels = [1 if r["expectation"] == "ambiguous" else 0 for r in results]
    sweep = []
    thr = 0.30
    while thr <= 0.901:
        tp = sum(1 for s, l in zip(scores, labels) if s >= thr and l == 1)
        fp = sum(1 for s, l in zip(scores, labels) if s >= thr and l == 0)
        tn = sum(1 for s, l in zip(scores, labels) if s < thr and l == 0)
        fn = sum(1 for s, l in zip(scores, labels) if s < thr and l == 1)
        precision = tp / (tp + fp) if (tp + fp) > 0 else 0.0
        recall    = tp / (tp + fn) if (tp + fn) > 0 else 0.0
        f1        = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0.0
        fpr       = fp / (fp + tn) if (fp + tn) > 0 else 0.0
        sweep.append({
            "thr": round(thr, 2),
            "tp": tp, "fp": fp, "tn": tn, "fn": fn,
            "f1": round(f1, 4),
            "precision": round(precision, 4),
            "recall": round(recall, 4),
            "fpr": round(fpr, 4),
        })
        thr = round(thr + THRESHOLD_STEP, 2)
    return sweep


def pareto_frontier(results, optimal_thresholds, delta=0.08):
    """Granite invocation rate vs. ambiguity recall as per-head thresholds scale proportionally.

    Returns a list of {scale, thresholds, invocation_rate, recall, tp, fp, tn, fn}.
    The scale parameter multiplies all per-head thresholds uniformly from 0.30 to 1.00.
    """
    pareto = []
    base = list(optimal_thresholds)
    scale = 0.30
    while scale <= 1.01:
        scaled = [min(t * scale / base[i] if base[i] > 0 else t, 1.0) for i, t in enumerate(base)]
        tp = fp = tn = fn = 0
        for r in results:
            scores = [
                next(s["score"] for s in r["category_scores"] if s["category"] == h)
                for h in HEADS
            ]
            clarify   = any(scores[i] >= scaled[i] for i in range(4))
            uncertain = (not clarify) and any(scores[i] >= scaled[i] - delta for i in range(4))
            invoked = clarify or uncertain
            is_ambig = r["expectation"] == "ambiguous"
            if invoked and is_ambig:     tp += 1
            elif invoked:                fp += 1
            elif is_ambig:               fn += 1
            else:                        tn += 1
        recall = tp / (tp + fn) if (tp + fn) > 0 else 0.0
        invocation_rate = (tp + fp) / len(results) if results else 0.0
        pareto.append({
            "scale": round(scale, 2),
            "thresholds": [round(s, 3) for s in scaled],
            "invocation_rate": round(invocation_rate, 4),
            "recall": round(recall, 4),
            "tp": tp, "fp": fp, "tn": tn, "fn": fn,
        })
        scale = round(scale + 0.05, 2)
    return pareto


def temperature_sweep(results, grid=TEMPERATURE_GRID):
    """Compute mean ECE across all heads for each temperature in grid.

    Requires raw_logits in results (JULIA_TRIAGE_DIAGNOSTICS=1 must have been set).
    Returns {str(T): mean_ece} dict, or empty dict if raw logits are unavailable.
    """
    has_logits = all(r.get("raw_logits") is not None for r in results)
    if not has_logits:
        return {}

    temp_ece = {}
    for T in grid:
        rescored = []
        for r in results:
            new_scores = []
            for i, head in enumerate(HEADS):
                probs = softmax_temperature(r["raw_logits"][i], T)
                new_scores.append({"category": head, "score": probs[1]})
            rescored.append({**r, "category_scores": new_scores})
        mean_ece = sum(compute_ece(rescored, h)[0] for h in HEADS) / 4
        temp_ece[str(T)] = round(mean_ece, 6)
    return temp_ece


def main():
    parser = argparse.ArgumentParser(
        description="Julia S1 Triage — Offline Calibration & Threshold Optimization",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.strip(),
    )
    parser.add_argument("--model-dir", default=str(find_default_model_dir()),
                        help="Path to Julia-1 checkpoint directory")
    parser.add_argument("--corpus", required=True,
                        help="Path to labeled corpus JSON (see format in module docstring)")
    parser.add_argument("--device", default="cpu", choices=["cpu", "cuda", "mps"],
                        help="Inference device (default: cpu)")
    parser.add_argument("--pareto-delta", type=float, default=0.08,
                        help="Uncertainty dead-band δ used in Pareto frontier computation (default: 0.08)")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/calibration_report.json",
                        help="Output path for the JSON calibration report")
    args = parser.parse_args()

    model_dir = Path(args.model_dir)
    corpus_path = Path(args.corpus)
    if not corpus_path.is_file():
        sys.exit(f"error: corpus file not found: {corpus_path}")
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    if not isinstance(corpus, list) or not corpus:
        sys.exit("error: corpus must be a non-empty JSON array")

    n_clear = sum(1 for c in corpus if c.get("expectation") == "clear")
    n_ambig = sum(1 for c in corpus if c.get("expectation") == "ambiguous")
    print(f"Corpus: {len(corpus)} cases ({n_clear} clear, {n_ambig} ambiguous)")
    if n_clear == 0 or n_ambig == 0:
        sys.exit("error: corpus must contain at least one clear and one ambiguous case")

    worker = load_worker_module()
    os.environ["JULIA_DEVICE"] = args.device
    os.environ["JULIA_TRIAGE_TEMPERATURE"] = "1.0"  # always collect at T=1 for calibration
    print(f"Loading Julia on {args.device}...")
    engine = worker.load(model_dir)
    print(f"Loaded. Running inference on {len(corpus)} cases...")

    # Step 1: collect scores + raw logits
    results = collect_raw_results(engine, worker, corpus)
    has_logits = all(r.get("raw_logits") is not None for r in results)
    if not has_logits:
        print("  [warn] Raw logits not available — temperature sweep skipped. "
              "Ensure julia_worker.py is at Phase-2 or later.")

    # Step 2: ECE and reliability diagram at T=1.0
    print("\n--- ECE at T=1.0 ---")
    ece_report = {}
    for head in HEADS:
        ece, reliability = compute_ece(results, head)
        ece_report[head] = {"ece": ece, "reliability": reliability}
        print(f"  {head:15s}: ECE={ece:.4f}  ({len(reliability)} non-empty bins)")

    # Step 3: per-head threshold sweep
    print("\n--- Per-Head Threshold Sweep (F1 maximization) ---")
    sweep_report = {}
    optimal_thresholds = []
    for head in HEADS:
        sweep = sweep_threshold(results, head)
        best = max(sweep, key=lambda x: x["f1"])
        sweep_report[head] = sweep
        optimal_thresholds.append(best["thr"])
        print(f"  {head:15s}: optimal thr={best['thr']:.2f}  "
              f"F1={best['f1']:.3f}  Prec={best['precision']:.3f}  "
              f"Rec={best['recall']:.3f}  FPR={best['fpr']:.3f}")

    # Step 4: Pareto frontier
    print("\n--- Pareto Frontier (Granite invocation rate vs. ambiguity recall) ---")
    pareto = pareto_frontier(results, optimal_thresholds, delta=args.pareto_delta)
    print(f"  {'scale':>6s}  {'invoc_rate':>10s}  {'recall':>7s}")
    for point in pareto[::2]:
        print(f"  {point['scale']:>6.2f}  {point['invocation_rate']:>10.4f}  {point['recall']:>7.4f}")

    # Step 5: temperature sweep
    print("\n--- Temperature Sweep (ECE minimization) ---")
    t_ece = temperature_sweep(results, TEMPERATURE_GRID)
    best_T = "1.0"
    if t_ece:
        best_T = min(t_ece, key=t_ece.__getitem__)
        for T_str, ece in sorted(t_ece.items(), key=lambda kv: float(kv[0])):
            marker = " <-- optimal" if T_str == best_T else ""
            print(f"  T={float(T_str):.2f}: mean ECE={ece:.4f}{marker}")
    else:
        print("  Skipped (raw logits unavailable).")

    # Step 6: recommended policy
    recommended_policy = {
        "thresholds": optimal_thresholds,
        "uncertainty_delta": args.pareto_delta,
        "temperature": float(best_T),
        "policy_id": "roclint.triage_policy.calibrated.v1",
    }
    print("\n--- Recommended Policy ---")
    print(f"  Thresholds:         {dict(zip(HEADS, optimal_thresholds))}")
    print(f"  UncertaintyDelta:   {args.pareto_delta}")
    print(f"  Temperature:        {best_T}")

    # Assemble and save report
    report = {
        "scope": "Calibration from a labeled corpus; accuracy depends on corpus representativeness",
        "model_dir": str(model_dir),
        "device": args.device,
        "corpus_path": str(corpus_path),
        "corpus_size": len(corpus),
        "n_clear": n_clear,
        "n_ambiguous": n_ambig,
        "heads": list(HEADS),
        "ece_at_T1": ece_report,
        "optimal_thresholds": dict(zip(HEADS, optimal_thresholds)),
        "threshold_sweep": sweep_report,
        "pareto_frontier": pareto,
        "temperature_sweep": t_ece,
        "optimal_temperature": float(best_T),
        "recommended_policy": recommended_policy,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"\nCalibration report saved to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
