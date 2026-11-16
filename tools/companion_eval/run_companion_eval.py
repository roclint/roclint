"""Repeatable companion/intent baseline driver for the 2026-11-15 measurement.

This is evaluation tooling. It does not modify product behavior, does not
approve/insert/send anything, and does not use the user's history database.

Stages:
  host : drives the unmodified production HostController through
         tools/companion_probe (cold then warm; multi-turn answer + rewrite).
  raw  : helper-only raw discovery JSON from the pinned native worker, using the
         production prompt builders/schemas from roclint_granite_contracts and
         the existing Worker transport in tools/check_granite_runtime.py.

Only the standard library is used. Run from this repository/worktree.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import threading
import time

WORKTREE = Path(__file__).resolve().parents[2]
DEFAULT_OUT = WORKTREE / "docs/audit/companion_evidence_2026-11-15"

# The baseline receipt used the main checkout's Release dir. The combined
# after-run points these at the integration build through environment
# overrides so the same protocol measures the same production code it links.
def env_path(name: str, default: str) -> Path:
    return Path(os.environ.get(name, default))


BUILD_DIR = env_path("ROCLINT_EVAL_BUILD", "D:/Stardust/roclint/build/Release")
TEST_EXE = BUILD_DIR / "roclint_native_granite_tests.exe"
CONTRACTS_EXE = BUILD_DIR / "roclint_granite_contracts.exe"
WORKER = BUILD_DIR / "roclint_granite_worker.exe"
JULIA_DIR = env_path("ROCLINT_EVAL_JULIA", "D:/Stardust/roclint/models/julia-1")
GRANITE_DIR = env_path("ROCLINT_EVAL_GRANITE", "D:/Stardust/roclint/models/granite-4.2-3b")
JULIA_PYTHON = env_path("ROCLINT_EVAL_JULIA_PYTHON",
                        "D:/Stardust/roclint/build/runtime/julia-python/Scripts/python.exe")
RUNTIME_CONFIG = env_path("ROCLINT_EVAL_RUNTIME_CONFIG",
                          "D:/Stardust/roclint/build/generated/granite-runtime-config.json")
QT_BIN = env_path("ROCLINT_EVAL_QT_BIN", "D:/Qt/6.8.2/msvc2022_64/bin")


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(*args: str, cwd: Path = WORKTREE) -> str:
    return subprocess.check_output(["git", *args], cwd=cwd).decode("utf-8", errors="replace").strip()


def python_probe(executable: Path) -> dict:
    code = (
        "import json,platform,sys\n"
        "out={'version':sys.version,'executable':sys.executable}\n"
        "for name in ('torch','transformers','safetensors'):\n"
        "    try:\n"
        "        module=__import__(name); out[name]=getattr(module,'__version__','unknown')\n"
        "    except Exception as error:\n"
        "        out[name]='MISSING: '+type(error).__name__\n"
        "print(json.dumps(out))\n"
    )
    try:
        result = subprocess.run([str(executable), "-c", code], capture_output=True, text=True, timeout=120)
        return json.loads(result.stdout.strip().splitlines()[-1]) if result.stdout.strip() else {
            "error": result.stderr.strip()[:2000]}
    except Exception as error:  # pragma: no cover - diagnostic only
        return {"error": f"{type(error).__name__}: {error}"}


def nvidia() -> dict:
    query = ["nvidia-smi", "--query-gpu=name,driver_version,memory.total,memory.used,utilization.gpu",
             "--format=csv,noheader,nounits"]
    try:
        result = subprocess.run(query, capture_output=True, text=True, timeout=20)
        line = result.stdout.strip().splitlines()[0] if result.stdout.strip() else ""
        parts = [part.strip() for part in line.split(",")]
        return {"name": parts[0] if len(parts) > 0 else "",
                "driver": parts[1] if len(parts) > 1 else "",
                "memory_total_mib": int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else None,
                "memory_used_mib": int(parts[3]) if len(parts) > 3 and parts[3].isdigit() else None,
                "utilization_pct": int(parts[4]) if len(parts) > 4 and parts[4].isdigit() else None,
                "error": result.stderr.strip()[:500]}
    except Exception as error:
        return {"error": f"{type(error).__name__}: {error}"}


def gpu_compute_processes() -> list:
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-compute-apps=pid,process_name,used_memory", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=20)
        return [line.strip() for line in result.stdout.splitlines() if line.strip()]
    except Exception as error:
        return [f"error: {type(error).__name__}: {error}"]


class CpuSampler(threading.Thread):
    """Windows system-wide CPU utilization sampled without third-party packages."""

    class FILETIME(ctypes.Structure):
        _fields_ = [("low", ctypes.c_uint32), ("high", ctypes.c_uint32)]

    def __init__(self, interval=2.0):
        super().__init__(daemon=True)
        self.interval = interval
        self.samples = []
        self._stop = threading.Event()
        self._kernel = ctypes.WinDLL("kernel32", use_last_error=True)

    def _times(self):
        idle, kernel, user = self.FILETIME(), self.FILETIME(), self.FILETIME()
        self._kernel.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
        to_int = lambda value: (value.high << 32) | value.low
        return to_int(idle), to_int(kernel), to_int(user)

    def run(self):
        previous = self._times()
        while not self._stop.wait(self.interval):
            current = self._times()
            idle = current[0] - previous[0]
            total = (current[1] - previous[1]) + (current[2] - previous[2])
            if total > 0:
                self.samples.append(round(100.0 * (1.0 - idle / total), 1))
            previous = current

    def stop(self):
        self._stop.set()
        self.join(timeout=5)


def summarise(values):
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    return {"count": len(values), "min": ordered[0], "median": ordered[len(ordered) // 2],
            "max": ordered[-1], "mean": round(sum(values) / len(values), 1)}


def identity(args, probe_exe: Path) -> dict:
    return {
        "generated_at_utc": utc_now(),
        "worktree": str(WORKTREE),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "head": git("rev-parse", "HEAD"),
        "dirty": bool(git("status", "--porcelain")),
        "main_checkout": {"path": "D:/Stardust/roclint",
                          "head": git("rev-parse", "HEAD", cwd=Path("D:/Stardust/roclint")),
                          "branch": git("rev-parse", "--abbrev-ref", "HEAD", cwd=Path("D:/Stardust/roclint"))},
        "platform": platform.platform(),
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "python_system": sys.version,
        "julia_python": python_probe(JULIA_PYTHON),
        "gpu": nvidia(),
        "binaries": {
            "probe": {"path": str(probe_exe), "sha256": sha256(probe_exe)},
            "native_worker": {"path": str(WORKER), "sha256": sha256(WORKER),
                              "mtime": dt.datetime.fromtimestamp(WORKER.stat().st_mtime, dt.timezone.utc).isoformat()},
            "native_test_exe": {"path": str(TEST_EXE), "sha256": sha256(TEST_EXE)},
            "contracts_exe": {"path": str(CONTRACTS_EXE), "sha256": sha256(CONTRACTS_EXE)},
        },
        "models": {
            "granite_directory": str(GRANITE_DIR),
            "granite_manifest_sha256": sha256(GRANITE_DIR / "roclint-granite.json"),
            "julia_directory": str(JULIA_DIR),
            "julia_manifest_sha256": sha256(JULIA_DIR / "roclint-julia.json"),
            "runtime_config_sha256": sha256(RUNTIME_CONFIG),
        },
        "runtime_config": json.loads(RUNTIME_CONFIG.read_text(encoding="utf-8-sig")),
        "settings": {
            "backend": "native-granite:Q8_0",
            "thinking": False,
            "temperature": 1.0,
            "top_p": 0.95,
            "seed": 42,
            "julia_python": str(JULIA_PYTHON),
            "julia_worker": str(env_path("ROCLINT_EVAL_JULIA_WORKER",
                "D:/Stardust/roclint/build/Release/runtime/julia_worker.py")),
        },
    }


def run_host(args, scenarios: list, evidence: Path) -> dict:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    settings_dir = evidence / "probe-settings" / stamp
    probe_config_path = evidence / f"host_config_{stamp}.json"
    ndjson_path = evidence / f"host_{stamp}.ndjson"
    stderr_path = evidence / f"host_{stamp}.stderr.log"

    probe_scenarios = []
    # A codebase-indexed scenario runs last so workspace evidence does not leak
    # into unrelated creative scenarios earlier in the same host process.
    ordered = sorted(scenarios, key=lambda item: bool(item.get("project_directory")))
    for scenario in ordered:
        item = {
            "id": scenario["id"],
            "prompt": scenario["prompt"],
            "answers": scenario.get("answers", []),
            "rewrite": scenario.get("rewrite", True),
            "max_turns": scenario.get("max_turns", 3),
            "explicit_review": scenario.get("explicit_review", False),
            "analysis_timeout_ms": 400000,
            "rewrite_timeout_ms": 240000,
        }
        if scenario.get("project_directory"):
            item["project_directory"] = str(WORKTREE / scenario["project_directory"])
            item["index_timeout_ms"] = 180000
        probe_scenarios.append(item)
    config = {
        "settings_directory": str(settings_dir),
        "settings": {
            "python": str(JULIA_PYTHON),
            "julia_directory": str(JULIA_DIR),
            "granite_directory": str(GRANITE_DIR),
            "worker": str(WORKER),
            "active_model": "native-granite:Q8_0",
        },
        "index_timeout_ms": 180000,
        "scenarios": probe_scenarios,
    }
    probe_config_path.parent.mkdir(parents=True, exist_ok=True)
    probe_config_path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    environment = os.environ.copy()
    environment["PATH"] = str(QT_BIN) + os.pathsep + str(WORKER.parent) + os.pathsep + environment.get("PATH", "")

    cpu = CpuSampler()
    before = {"cpu": None, "gpu": nvidia(), "gpu_processes": gpu_compute_processes(), "utc": utc_now()}
    cpu.start()
    started = time.perf_counter()
    with ndjson_path.open("w", encoding="utf-8", newline="\n") as stdout, \
            stderr_path.open("w", encoding="utf-8", newline="\n") as stderr:
        process = subprocess.run([str(args.probe), str(probe_config_path)], stdout=stdout, stderr=stderr,
                                 timeout=args.timeout, env=environment)
    wall_ms = round((time.perf_counter() - started) * 1000, 1)
    cpu.stop()
    after = {"cpu": summarise(cpu.samples), "gpu": nvidia(), "gpu_processes": gpu_compute_processes(),
             "utc": utc_now()}
    ambient = {"before": before, "during_cpu_pct": summarise(cpu.samples), "after": after}

    events = [json.loads(line) for line in ndjson_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    return {
        "process": {"exit_code": process.returncode, "wall_ms": wall_ms,
                    "probe_config": str(probe_config_path), "ndjson": str(ndjson_path),
                    "stderr": str(stderr_path)},
        "ambient": ambient,
        "events": events,
        "rows": build_rows(scenarios, events, ambient),
        "stamp": stamp,
    }


def attempt_map(events):
    merged = {}
    for event in events:
        if event.get("event") != "attempts":
            continue
        for attempt in event.get("attempts", []):
            merged[attempt.get("attempt_id")] = attempt
    return list(merged.values())


def find_status(events, predicate):
    for event in events:
        if event.get("event") == "status" and predicate(event):
            return event
    return None


def build_rows(scenarios, events, ambient):
    # Attempts are emitted between scenario_start and scenario_end. Older probe
    # builds omitted the scenario field, so associate by position as well as by
    # the explicit field when present.
    attempts_by_scenario = {}
    seen_attempts = set()
    current = None
    for event in events:
        if event.get("event") == "scenario_start":
            current = event.get("scenario")
        elif event.get("event") == "scenario_end" or (
                event.get("event") == "observation" and event.get("phase") == "scenario_end"):
            current = None
        elif event.get("event") == "attempts" or "attempts" in event:
            sid = event.get("scenario") or current
            if not sid:
                continue
            for attempt in event.get("attempts", []):
                attempt_id = attempt.get("attempt_id")
                if attempt_id in seen_attempts:
                    continue
                seen_attempts.add(attempt_id)
                attempts_by_scenario.setdefault(sid, {})[attempt_id] = attempt
    rows = []
    for index, scenario in enumerate(scenarios):
        sid = scenario["id"]
        selected = [event for event in events if event.get("scenario") == sid]
        start = next((e for e in selected if e.get("event") == "scenario_start"), None)
        end = next((e for e in selected
                    if e.get("event") == "observation" and e.get("phase") == "scenario_end"), None)
        observations = {e.get("phase"): e for e in selected if e.get("event") == "observation"}
        analysis = observations.get("analysis_complete", {})
        rewrite = observations.get("rewrite_complete", {})
        answers = [e for e in selected if e.get("event") == "observation" and e.get("phase") == "answer"]
        statuses = [e for e in selected if e.get("event") == "status"]

        def first_status(text):
            return next((e for e in statuses if text in (e.get("message") or "")), None)

        julia_start = first_status("S1: Starting")
        julia_ready = first_status("Checking your prompt.")
        granite_warm = first_status("Warming the pinned native model")
        granite_ready = first_status("Analyzing your prompt.")
        ready = next((e for e in statuses if e.get("status") == "READY"), None)
        failed = next((e for e in statuses if e.get("status") == "FAILED"), None)

        def span(a, b):
            if not a or not b:
                return None
            return max(0, b["t_ms"] - a["t_ms"])

        start_t = start["t_ms"] if start else 0
        stages = {
            "status_feedback_ms": span(start, next((e for e in statuses if e.get("status") == "ANALYZING"), None)),
            "julia_cold_load_ms": span(julia_start, julia_ready),
            "julia_triage_and_route_ms": span(julia_ready, granite_warm or ready),
            "granite_cold_warm_ms": span(granite_warm, granite_ready),
            "discovery_visible_ms": span(granite_ready or julia_ready, ready),
            "analysis_to_ready_ms": analysis.get("elapsed_ms"),
            "first_usable_question_ms": span(start, ready) if ready and analysis.get("question") else None,
            "rewrite_request_to_ready_ms": rewrite.get("elapsed_ms"),
            "total_to_rewrite_ms": (rewrite.get("t_ms") - start_t) if rewrite.get("t_ms") else None,
            "total_scenario_ms": (end.get("t_ms") - start_t) if end and start else None,
        }
        attempts = list(attempts_by_scenario.get(sid, {}).values())
        stage_attempts = {a["stage"]: a for a in attempts}
        stages["julia_triage_attempt_ms"] = stage_attempts.get("julia_triage", {}).get("duration_ms")
        stages["discovery_attempt_ms"] = stage_attempts.get("discovery", {}).get("duration_ms")
        stages["clarification_check_attempt_ms"] = stage_attempts.get("clarification_check", {}).get("duration_ms")
        stages["rewrite_attempt_ms"] = stage_attempts.get("rewrite", {}).get("duration_ms")

        q = analysis.get("question") or ""
        proposal = rewrite.get("proposal_text") or analysis.get("proposal_text") or ""
        lower_proposal = proposal.lower()
        answer_texts = [a.get("requested", {}).get("text", "") for a in answers if a.get("requested", {}).get("text")]
        stop = set("the a an and or of to in on for with without keep not use more than is are be as at by from into your their it this that".split())
        def content_tokens(text):
            return {t for t in re.findall(r"[a-z0-9][a-z0-9.-]{3,}", text.lower()) if t not in stop}
        overlap = {}
        for answer in answer_texts:
            answer_tokens = content_tokens(answer)
            proposal_tokens = content_tokens(proposal)
            matched = sorted(answer_tokens & proposal_tokens)
            overlap[answer] = {"matched": matched,
                               "ratio": round(len(matched) / len(answer_tokens), 3) if answer_tokens else None}

        mechanical = {
            "question_present": bool(q),
            "option_count": len(analysis.get("alternatives") or []),
            "original_route": bool(analysis.get("is_original_route")),
            "proposal_present": bool(rewrite.get("has_proposal")),
            "proposal_changed": bool(proposal.strip()) and proposal.strip() != scenario["prompt"].strip(),
            "confirmed_item_count": len((rewrite.get("confirmed_items") or analysis.get("confirmed_items") or [])),
            "pending_review_count": rewrite.get("pending_review_count", analysis.get("pending_review_count")),
            "can_approve": rewrite.get("can_approve", analysis.get("can_approve")),
            "audit_outcome": rewrite.get("audit_outcome", analysis.get("audit_outcome")),
            "audit_execution_status": rewrite.get("audit_execution_status", analysis.get("audit_execution_status")),
            "answer_token_overlap": overlap,
            "typography_terms_in_question": sorted(set(re.findall(
                r"typography|typeface|font|type size|leading|line spacing|density|hierarchy|legib\\w*",
                q.lower()))),
            "typography_terms_in_proposal": sorted(set(re.findall(
                r"typography|typeface|font|type size|leading|line spacing|density|hierarchy|legib\\w*",
                lower_proposal))),
            "browsing_mentioned": "brows" in lower_proposal,
            "gibberish_leaked": any(token in lower_proposal for token in ("asdf", "qwer", "zxcv")),
            "exact_values": {value: value in proposal or value in scenario["prompt"]
                             for value in ("500", "95%", "schema")},
            "reference_terms_in_proposal": sorted(set(re.findall(r"revolver|1990s|1960s", lower_proposal))),
        }
        rows.append({
            "id": sid,
            "class": scenario.get("class"),
            "surface": scenario.get("surface"),
            "prompt": scenario["prompt"],
            "explicit_review": bool(scenario.get("explicit_review")),
            "thermal_label": "cold_start" if index == 0 else "warm_process",
            "status": analysis.get("analysis_status"),
            "analysis_message": analysis.get("analysis_message"),
            "review_outcome": analysis.get("review_outcome"),
            "verdict": analysis.get("verdict"),
            "prompt_triage": analysis.get("prompt_triage"),
            "question": q,
            "alternatives": analysis.get("alternatives") or [],
            "assumption_reviews": analysis.get("assumption_reviews") or [],
            "clarification_checks": [e for e in selected if e.get("event") == "clarification_check"],
            "answers": [{k: a.get(k) for k in ("turn", "requested", "command_completed", "idle_ok",
                                               "clarification_check", "question", "analysis_status",
                                               "last_error", "last_error_code", "confirmed_items")}
                        | {"elapsed_ms": a.get("elapsed_ms")} for a in answers],
            "rewrite": {k: rewrite.get(k) for k in (
                "proposal_status", "proposal_message", "has_proposal", "proposal_text", "last_diff",
                "audit_outcome", "audit_execution_status", "audit_violations", "audit_unresolved",
                "pending_review_count", "can_approve", "creative_alternatives",
                "proposal_constraints", "confirmed_items", "unresolved_items", "elapsed_ms",
                "idle_ok", "posted_ok", "last_error", "last_error_code")},
            "stages": stages,
            "attempts": attempts,
            "mechanical": mechanical,
            "model_verdict": {
                "audit_outcome": rewrite.get("audit_outcome", analysis.get("audit_outcome")),
                "audit_execution_status": rewrite.get("audit_execution_status",
                                                      analysis.get("audit_execution_status")),
                "pending_review_count": rewrite.get("pending_review_count",
                                                    analysis.get("pending_review_count")),
                "note": "Model Pass/valid schema is not a usefulness judgment.",
            },
            "editorial": None,
            "events_file": None,
            "ambient_cpu_pct": ambient.get("during_cpu_pct"),
        })
    return rows


def attach_editorial(rows, path: Path):
    if not path.is_file():
        return
    judgments = json.loads(path.read_text(encoding="utf-8-sig"))
    by_id = {item["id"]: item for item in judgments.get("judgments", [])}
    for row in rows:
        row["editorial"] = by_id.get(row["id"])


def run_raw(args, evidence: Path) -> dict:
    raw_dir = evidence / "raw_worker"
    raw_dir.mkdir(parents=True, exist_ok=True)
    corpus = WORKTREE / "tests/fixtures/companion_eval/raw_discovery_2026-11-15.json"
    environment = os.environ.copy()
    environment["PATH"] = str(QT_BIN) + os.pathsep + environment.get("PATH", "")
    contracts_path = raw_dir / "contracts.json"
    generated = subprocess.run([str(CONTRACTS_EXE), str(corpus)], capture_output=True, env=environment,
                               timeout=120)
    if generated.returncode != 0:
        return {"error": "contracts generation failed",
                "stderr": generated.stderr.decode("utf-8", errors="replace")[:4000]}
    contracts_path.write_bytes(generated.stdout)
    output = raw_dir / "raw_results.json"
    command = [sys.executable, str(WORKTREE / "tools/companion_eval/raw_discovery.py"),
               "--contracts", str(contracts_path), "--config", str(RUNTIME_CONFIG),
               "--model-dir", str(GRANITE_DIR), "--worker", str(WORKER),
               "--contracts-tool", str(CONTRACTS_EXE), "--out", str(output),
               "--qt-bin", str(QT_BIN)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
    (raw_dir / "raw_driver.log").write_text(result.stdout + "\n---STDERR---\n" + result.stderr,
                                            encoding="utf-8")
    payload = json.loads(output.read_text(encoding="utf-8")) if output.is_file() else {"error": "no output"}
    payload["driver_exit_code"] = result.returncode
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, default=WORKTREE / "build-eval-probe/Release/companion_probe.exe")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--scenarios", type=Path,
                        default=WORKTREE / "tests/fixtures/companion_eval/scenarios_2026-11-15.json")
    parser.add_argument("--judgments", type=Path,
                        default=WORKTREE / "docs/audit/companion_evidence_2026-11-15/judgments_2026-11-15.json")
    parser.add_argument("--stage", choices=("host", "raw", "all", "merge", "reparse"), default="all")
    parser.add_argument("--timeout", type=int, default=2400)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    scenario_doc = json.loads(args.scenarios.read_text(encoding="utf-8-sig"))
    scenarios = scenario_doc["scenarios"]
    if args.stage == "merge":
        baseline_path = args.out / "baseline_host.json"
        report = json.loads(baseline_path.read_text(encoding="utf-8"))
        attach_editorial(report["host"]["rows"], args.judgments)
        baseline_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("merged editorial judgments into", baseline_path, flush=True)
        return 0
    if args.stage == "reparse":
        baseline_path = args.out / "baseline_host.json"
        report = json.loads(baseline_path.read_text(encoding="utf-8"))
        ndjson_path = Path(report["host"]["process"]["ndjson"])
        events = [json.loads(line) for line in ndjson_path.read_text(encoding="utf-8").splitlines()
                  if line.strip()]
        report["host"]["rows"] = build_rows(scenarios, events, report["host"].get("ambient", {}))
        for row in report["host"]["rows"]:
            row["events_file"] = ndjson_path.name
        attach_editorial(report["host"]["rows"], args.judgments)
        baseline_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("reparsed rows from", ndjson_path, flush=True)
        return 0
    identity_doc = identity(args, args.probe)
    (args.out / "identity.json").write_text(json.dumps(identity_doc, ensure_ascii=False, indent=2) + "\n",
                                            encoding="utf-8")

    report = {"schema": "roclint.companion_eval.baseline.v1", "generated_at_utc": utc_now(),
              "scenario_file": str(args.scenarios), "identity": identity_doc, "host": None, "raw": None}
    if args.stage in ("host", "all"):
        host = run_host(args, scenarios, args.out)
        events = host.pop("events")
        attach_editorial(host["rows"], args.judgments)
        for row in host["rows"]:
            row["events_file"] = Path(host["process"]["ndjson"]).name
        report["host"] = host
        (args.out / "events_parsed.json").write_text(
            json.dumps(events, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (args.out / "baseline_host.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("host:", host["process"]["exit_code"], host["process"]["wall_ms"], "ms",
              len(host["rows"]), "scenarios", flush=True)
    if args.stage == "raw":
        report["raw"] = run_raw(args, args.out)
        raw_baseline = args.out / "baseline_raw.json"
        raw_baseline.write_text(json.dumps(report["raw"], ensure_ascii=False, indent=2) + "\n",
                                encoding="utf-8")
        print("raw:", (report["raw"] or {}).get("driver_exit_code"), flush=True)
        return 0
    if args.stage == "all":
        report["raw"] = run_raw(args, args.out)
        (args.out / "baseline_host.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("raw:", (report["raw"] or {}).get("driver_exit_code"), flush=True)
    ambient = report.get("host") or {}
    (args.out / "ambient.json").write_text(
        json.dumps(ambient.get("ambient", {}), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
