"""Benchmark the packaged native Granite helper on the current backend.

Runs the production discovery fixtures through the real worker, records complete
per-call metrics (prefill, first token, decode, KV-prefix reuse) and compares
pinned-temperature generation with an optional experiment temperature. This is a
development measurement tool; it does not approve rewrites or inspect user data.

Examples:
  python tools/benchmark_granite_backends.py --backend-label VULKAN --temperature 1.0 --temperature 0.7
  python tools/benchmark_granite_backends.py --backend-label METAL --temperature 1.0
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

from check_granite_runtime import ROOT, Worker, find_binary, find_config, select_cases


def text_hash(value):
    return None if not isinstance(value, str) else hashlib.sha256(value.encode("utf-8")).hexdigest()


def run_temperature(args, manifest, contracts, environment, temperature):
    # Worker inspects args.temperature; give it the scalar for this run rather
    # than the command-line list used to drive the comparison.
    args.temperature = temperature
    run = {"temperature": temperature, "quantization": args.quantization, "cases": [], "controls": [], "samples": []}
    worker = None
    try:
        worker = Worker(args, manifest, args.quantization, environment)
        for operation in ("hello", "load"):
            started = time.perf_counter()
            control = worker.exchange(operation)
            run["controls"].append({"op": operation, **control,
                                    "load_ms": round((time.perf_counter() - started) * 1000, 2)})
            print(run["temperature"], operation, control["elapsed_ms"], control["response"]["ok"], flush=True)
            if not control["response"]["ok"]:
                raise RuntimeError("Native helper could not load the selected configuration")
        for fixture in contracts["cases"]:
            for repeat_index in range(args.repeat):
                if getattr(args, "cold_per_case", False) and repeat_index == 0:
                    reset = worker.exchange("reset_cache")
                    run["controls"].append({"op": "reset_cache", "case_id": fixture["id"], **reset})
                    if not reset["response"]["ok"]:
                        raise RuntimeError("Native helper could not reset the KV cache")
                result = worker.exchange(fixture["op"],
                                         {key: fixture[key] for key in ("prompt", "instructions", "schema")},
                                         fixture)
                response = result["response"]
                metrics = response.get("result", {}) if response.get("ok") else response.get("metrics", {})
                sample = {
                    "case_id": fixture["id"],
                    "repeat_index": repeat_index,
                    "ok": bool(response.get("ok")),
                    "elapsed_ms": result["elapsed_ms"],
                    "output_sha256": text_hash(response.get("result", {}).get("text")),
                    "input_tokens": metrics.get("input_tokens"),
                    "output_tokens": metrics.get("output_tokens"),
                    "first_token_ms": metrics.get("first_token_ms"),
                    "prefill_ms": metrics.get("prefill_ms"),
                    "decode_ms": metrics.get("decode_ms"),
                    "sampler_ms": metrics.get("sampler_ms"),
                    "generation_ms": metrics.get("generation_ms"),
                    "kv_cache_reused_tokens": metrics.get("kv_cache_reused_tokens"),
                    "sampling_temperature": metrics.get("sampling_temperature"),
                }
                sample["cache_state"] = "warm" if (sample["kv_cache_reused_tokens"] or 0) > 0 else "cold"
                if sample["output_tokens"] and sample["generation_ms"]:
                    sample["tokens_per_second"] = round(
                        sample["output_tokens"] * 1000.0 / sample["generation_ms"], 3)
                if sample["output_tokens"] and sample["decode_ms"]:
                    sample["decode_tokens_per_second"] = round(
                        sample["output_tokens"] * 1000.0 / sample["decode_ms"], 3)
                run["samples"].append(sample)
                run["cases"].append({**fixture, "repeat_index": repeat_index, "phase": "HOT", **result})
                print(run["temperature"], fixture["id"], repeat_index, result["elapsed_ms"],
                      response.get("ok"), metrics.get("output_tokens"), metrics.get("kv_cache_reused_tokens"),
                      flush=True)
    except Exception as error:
        run["error"] = str(error)
        print(run["temperature"], "FAILED:", error, flush=True)
    finally:
        if worker:
            run["shutdown"] = worker.close()
            run["diagnostic_tail"] = worker.diagnostics
    return run


def median(values):
    values = [value for value in values if value is not None]
    return statistics.median(values) if values else None


def summarize(run, expected_count=None):
    successful = [sample for sample in run["samples"] if sample["ok"]]
    cold = [sample for sample in successful if (sample["kv_cache_reused_tokens"] or 0) == 0]
    warm = [sample for sample in successful if (sample["kv_cache_reused_tokens"] or 0) > 0]
    by_case = {}
    for sample in successful:
        by_case.setdefault(sample["case_id"], []).append(sample)
    cases = []
    for case_id, samples in sorted(by_case.items()):
        hashes = [sample["output_sha256"] for sample in samples if sample["output_sha256"]]
        cases.append({
            "case_id": case_id,
            "samples": len(samples),
            "unique_outputs": len(set(hashes)),
            "deterministic": len(set(hashes)) == 1 and len(hashes) == len(samples),
            "median_output_tokens": median([sample["output_tokens"] for sample in samples]),
            "median_generation_ms": median([sample["generation_ms"] for sample in samples]),
            "median_prefill_ms": median([sample["prefill_ms"] for sample in samples]),
            "median_first_token_ms": median([sample["first_token_ms"] for sample in samples]),
            "median_tokens_per_second": median([sample["tokens_per_second"] for sample in samples]),
            "median_kv_cache_reused_tokens": median(
                [sample["kv_cache_reused_tokens"] for sample in samples]),
        })
    requested = expected_count if expected_count is not None else len(run["samples"])
    failed = requested - len(successful)
    if run.get("error"):
        failed = max(failed, 1)
    return {
        "requested": requested,
        "successful": len(successful),
        "failed": max(0, failed),
        "deterministic_cases": sum(1 for case in cases if case["deterministic"]),
        "cold_samples": len(cold),
        "warm_samples": len(warm),
        "median_cold_first_token_ms": median([sample["first_token_ms"] for sample in cold]),
        "median_warm_first_token_ms": median([sample["first_token_ms"] for sample in warm]),
        "median_cold_prefill_ms": median([sample["prefill_ms"] for sample in cold]),
        "median_warm_prefill_ms": median([sample["prefill_ms"] for sample in warm]),
        "median_tokens_per_second": median([sample["tokens_per_second"] for sample in successful]),
        "cases": cases,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, default=find_binary("roclint_granite_worker", "runtime"))
    parser.add_argument("--config", type=Path, default=find_config("granite-runtime-config.json"))
    parser.add_argument("--contracts-tool", type=Path, default=find_binary("roclint_granite_contracts"))
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--quantization", choices=("Q4_K_M", "Q5_K_M", "Q8_0"), default="Q8_0")
    parser.add_argument("--temperature", type=float, action="append",
                        help="Sampling temperature to measure; repeat for a controlled comparison. Omit for pinned.")
    parser.add_argument("--repeat", type=int, default=3, help="Repeats per fixture (1-10)")
    parser.add_argument("--case", action="append", help="Run only the named fixture(s)")
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--backend-label", default=None)
    parser.add_argument("--diagnostic-no-deadlines", action="store_true",
                        help="Let discovery finish beyond the production deadline; token-count evidence only")
    parser.add_argument("--skip-contract-validation", action="store_true",
                        help="Skip the production parser/Decision Contract validation pass")
    parser.add_argument("--cpu-reference", action="store_true",
                        help="Force gpu_layers=0 with the same weights (development CPU reference)")
    parser.add_argument("--cold-per-case", action="store_true",
                        help="Reset the KV cache before each fixture so every case records cold and warm samples")
    parser.add_argument("--min-success-rate", type=float, default=1.0,
                        help="Fail when the successful-sample fraction is below this value (default 1.0)")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "granite-backend-benchmark.json")
    args = parser.parse_args()
    if args.repeat < 1 or args.repeat > 10:
        parser.error("--repeat must be between 1 and 10")
    for temperature in args.temperature or []:
        if temperature <= 0 or temperature > 4:
            parser.error("--temperature must be in (0, 4]")
    if args.min_success_rate < 0 or args.min_success_rate > 1:
        parser.error("--min-success-rate must be in [0, 1]")
    environment = os.environ.copy()
    if args.qt_bin:
        environment["PATH"] = str(args.qt_bin) + os.pathsep + environment.get("PATH", "")
    manifest = json.loads(args.config.read_text(encoding="utf-8-sig"))
    corpus = ROOT / "app/runtime/granite-benchmark-fixtures.json"
    contracts = json.loads(subprocess.run([str(args.contracts_tool), str(corpus)], check=True,
        capture_output=True, env=environment).stdout.decode("utf-8"))
    try:
        cases = select_cases(contracts["cases"], args.case)
    except ValueError as error:
        parser.error(str(error))
    contracts = {**contracts, "cases": cases}
    temperatures = args.temperature or [None]
    report = {
        "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "backend_label": args.backend_label or ("CPU_REFERENCE" if args.cpu_reference else manifest.get("backend", "unknown")),
        "manifest_backend": manifest.get("backend"),
        "cpu_reference": args.cpu_reference,
        "cold_per_case": args.cold_per_case,
        "configuration": manifest,
        "quantization": args.quantization,
        "worker_sha256": hashlib.sha256(args.worker.read_bytes()).hexdigest(),
        "corpus_sha256": hashlib.sha256(corpus.read_bytes()).hexdigest(),
        "selected_case_ids": [case["id"] for case in cases],
        "repeat": args.repeat,
        "diagnostic_no_deadlines": args.diagnostic_no_deadlines,
        "runs": [],
    }

    def save():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    expected_samples_per_run = len(cases) * args.repeat
    for temperature in temperatures:
        run = run_temperature(args, manifest, contracts, environment, temperature)
        run["expected_samples"] = expected_samples_per_run
        run["summary"] = summarize(run, expected_samples_per_run)
        report["runs"].append(run)
        save()

    comparison = []
    for case in cases:
        entry = {"case_id": case["id"], "temperatures": []}
        for run in report["runs"]:
            samples = [sample for sample in run["samples"]
                       if sample["case_id"] == case["id"] and sample["ok"]]
            if not samples:
                entry["temperatures"].append({"temperature": run["temperature"], "missing": True})
                continue
            hashes = [sample["output_sha256"] for sample in samples if sample["output_sha256"]]
            entry["temperatures"].append({
                "temperature": run["temperature"],
                "samples": len(samples),
                "unique_outputs": len(set(hashes)),
                "median_output_tokens": statistics.median(
                    [sample["output_tokens"] for sample in samples if sample["output_tokens"] is not None]),
                "median_generation_ms": statistics.median(
                    [sample["generation_ms"] for sample in samples if sample["generation_ms"] is not None]),
                "median_prefill_ms": statistics.median(
                    [sample["prefill_ms"] for sample in samples if sample["prefill_ms"] is not None]),
                "median_first_token_ms": statistics.median(
                    [sample["first_token_ms"] for sample in samples if sample["first_token_ms"] is not None]),
            })
        comparison.append(entry)
    report["comparison"] = comparison
    report["measurement_scope"] = (
        "Single-machine development measurement. Timings include the numeric helper protocol but exclude "
        "desktop UI work. Cross-backend Metal/Vulkan comparison requires running this tool on each platform "
        "with the same fixtures, model candidate, quantization and sampling configuration.")
    save()

    validation_failed = False
    if not args.skip_contract_validation:
        validation = subprocess.run(
            [str(args.contracts_tool), "--validate", str(args.output)],
            capture_output=True, env=environment)
        if validation.returncode != 0:
            validation_failed = True
            report["mechanical_validation_error"] = validation.stderr.decode("utf-8", errors="replace").strip()
            report["mechanical_validation"] = []
        else:
            report["mechanical_validation"] = json.loads(validation.stdout.decode("utf-8"))
            validation_failed = any(
                not item.get("transport_ok") or not item.get("parse_valid") or
                item.get("structurally_valid") is False
                for item in report["mechanical_validation"])
        save()

    for run in report["runs"]:
        summary = run["summary"]
        print("temperature", run["temperature"], "successful", summary["successful"], "/", summary["requested"],
              "deterministic_cases", summary["deterministic_cases"], "/", len(summary["cases"]), flush=True)
    print("Benchmark report:", args.output, flush=True)
    has_failures = validation_failed or any(
        run["summary"]["failed"] > 0 or
        bool(run.get("error")) or
        run["summary"]["successful"] < run.get("expected_samples", 0) or
        (run["summary"]["successful"] / run.get("expected_samples", 1)) < args.min_success_rate
        for run in report["runs"]
    )
    return 1 if has_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
