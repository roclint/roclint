"""Run only synthetic production-adapter fixtures against the actual native helper.

Explicit opt-in: loads installed weights, records full synthetic outputs, and never
uses application history/settings. This is a development probe, not a study or
the Q4/Q5 evaluation freeze. Put the matching Qt runtime on PATH or use --qt-bin.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import subprocess
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
MAX_FRAME = 1024 * 1024
DEADLINE_KEYS = {
    "granite_analyze": "analysis_timeout_ms",
    "granite_rewrite": "rewrite_timeout_ms",
    "granite_word_refinement": "word_refinement_timeout_ms",
}


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def compact_json(text):
    """Validate without rewriting strings or repairing generated output."""
    if not isinstance(text, str):
        return False
    quoted = escaped = False
    for character in text:
        if quoted:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
        elif character == '"':
            quoted = True
        elif character in " \t\r\n":
            return False
    try:
        return not quoted and isinstance(json.loads(text), dict)
    except (ValueError, TypeError):
        return False


def select_cases(cases, identifiers=None):
    if not identifiers:
        return list(cases)
    requested = set(identifiers)
    unknown = requested - {case["id"] for case in cases}
    if unknown:
        raise ValueError("Unknown fixture IDs: " + ", ".join(sorted(unknown)))
    return [case for case in cases if case["id"] in requested]


def windows_working_set(pid):
    if os.name != "nt":
        return None  # No guessed cross-platform RSS or CPU-mapping claim.
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ("peak_working_set", "working_set", "peak_paged_pool",
                "paged_pool", "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile", "private_bytes")]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    memory = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    memory.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    handle = kernel.OpenProcess(0x410, False, pid)
    if not handle:
        return None
    try:
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if not memory(handle, ctypes.byref(counters), counters.cb):
            return None
        return {"working_set_bytes": counters.working_set, "peak_working_set_bytes": counters.peak_working_set,
                "private_bytes": counters.private_bytes}
    finally:
        kernel.CloseHandle(handle)


class Worker:
    def __init__(self, args, manifest, quantization, environment):
        self.manifest = manifest
        self.quantization = quantization
        self.candidate = next(item for item in manifest["candidates"] if item["quantization"] == quantization)
        self.sequence = 0
        self.messages = queue.Queue()
        self.diagnostics = []
        self.diagnostic_unbounded = args.diagnostic_no_deadlines
        self.operation_deadlines = {}
        for operation, key in DEADLINE_KEYS.items():
            value = manifest.get(key)
            if type(value) is not int or value <= 0:
                raise ValueError(f"Missing or invalid {key}. Use granite-runtime-config.json from the current build.")
            self.operation_deadlines[operation] = (
                0 if self.diagnostic_unbounded and operation != "granite_word_refinement" else value
            )
        command = [str(args.worker), "--model-dir", str(args.model_dir), "--quantization", quantization]
        if getattr(args, "temperature", None) is not None:
            command += ["--sampling-temperature", str(args.temperature)]
        if getattr(args, "cpu_reference", False):
            command.append("--cpu-reference")
        if getattr(args, "no_thinking_reference", False):
            command.append("--no-thinking-reference")
        if self.diagnostic_unbounded:
            command.append("--diagnostic-no-deadlines")
        self.process = subprocess.Popen(command,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
        threading.Thread(target=self.read_frames, daemon=True).start()
        threading.Thread(target=self.read_diagnostics, daemon=True).start()

    def read_frames(self):
        try:
            while True:
                frame = self.process.stdout.readline(MAX_FRAME + 1)
                if not frame:
                    raise RuntimeError("Native helper stdout closed")
                if len(frame) > MAX_FRAME or not frame.endswith(b"\n"):
                    raise RuntimeError("Native helper returned an oversized/incomplete frame")
                self.messages.put(json.loads(frame.decode("utf-8")))
        except Exception as error:
            self.messages.put(error)

    def read_diagnostics(self):
        for line in self.process.stderr:
            self.diagnostics.append(line.decode("utf-8", errors="replace").rstrip())
            del self.diagnostics[:-100]

    def exchange(self, operation, payload=None, fixture=None):
        self.sequence += 1
        payload = payload or {}
        fixture = fixture or {}
        request = {"protocol_version": 1, "type": "request", "request_id": str(self.sequence), "op": operation,
            "request_group_id": "grp_native_benchmark_" + str(self.sequence), "attempt_id": "att_native_benchmark_" + str(self.sequence),
            "session_id": "sess_benchmark_synthetic", "draft_revision_id": "rev_sess_benchmark_synthetic_1", "state_version": 1,
            "provider": "granite_local", "model_id": self.manifest["model_family"], "model_revision": self.manifest["model_revision"],
            "runtime_revision": self.manifest["runtime_revision"], "configuration_id": self.manifest["configuration_id"],
            "quantization": self.quantization, "weights_sha256": self.candidate["sha256"], "input_hash": digest(payload.get("prompt", "")),
            "intent_id": fixture.get("intent_id", ""), "rewrite_profile_id": fixture.get("rewrite_profile_id", ""),
            "rewrite_profile_version": 1 if fixture.get("rewrite_profile_id") else 0, "payload": payload}
        started = time.perf_counter()
        self.process.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8"))
        self.process.stdin.flush()
        if operation in self.operation_deadlines:
            budget = self.operation_deadlines[operation]
            timeout = budget / 1000 + 3 if budget else 300
        else:
            timeout = {"hello": 3, "shutdown": 2}.get(operation, 185)
        deadline = started + timeout
        while True:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                raise TimeoutError(f"Native helper timed out during {operation}")
            try:
                response = self.messages.get(timeout=remaining)
            except queue.Empty as error:
                raise TimeoutError(f"Native helper timed out during {operation}") from error
            if isinstance(response, Exception):
                raise response
            if response.get("type") != "partial_result":
                break
        for key, value in request.items():
            if key not in ("type", "payload") and response.get(key) != value:
                raise RuntimeError("Native response binding mismatch: " + key)
        expected_type = "error" if response.get("ok") is False else "hello_ack" if operation == "hello" else "runtime_status" if operation == "runtime_status" else "result"
        if response.get("type") != expected_type or not isinstance(response.get("ok"), bool):
            raise RuntimeError("Native response type/status mismatch")
        if operation == "hello" and response["ok"]:
            capabilities = response.get("result", {})
            if capabilities.get("diagnostic_unbounded") is not self.diagnostic_unbounded:
                raise RuntimeError("Native diagnostic deadline mode did not match the explicit launch")
            for semantic_operation, key in DEADLINE_KEYS.items():
                if capabilities.get(key) != self.operation_deadlines[semantic_operation]:
                    raise RuntimeError(f"Native {key} did not match the generated runtime configuration")
        if (operation in DEADLINE_KEYS and response["ok"] and
                self.manifest.get("output_format") == "compact_json" and
                not compact_json(response.get("result", {}).get("text"))):
            raise RuntimeError("Native helper violated the declared compact JSON output format")
        return {"elapsed_ms": round((time.perf_counter() - started) * 1000, 2), "response": response,
                "memory": windows_working_set(self.process.pid)}

    def close(self):
        started = time.perf_counter()
        graceful = False
        if self.process.poll() is None:
            try:
                self.exchange("shutdown")
                self.process.wait(timeout=max(0.01, 2 - (time.perf_counter() - started)))
                graceful = True
            except Exception:
                if self.process.poll() is None:
                    self.process.kill()
                    self.process.wait(timeout=3)
        return {"graceful": graceful, "exit_code": self.process.returncode,
                "elapsed_ms": round((time.perf_counter() - started) * 1000, 2)}


def find_binary(name, subpath=""):
    exts = (".exe", "") if platform.system() == "Windows" else ("", ".exe")
    search_dirs = [
        ROOT / "build/Release" / subpath,
        ROOT / "build" / subpath,
        ROOT / "build/Release",
        ROOT / "build",
    ]
    for d in search_dirs:
        for ext in exts:
            p = d / f"{name}{ext}"
            if p.is_file():
                return p
    ext = ".exe" if platform.system() == "Windows" else ""
    return ROOT / "build/Release" / subpath / f"{name}{ext}"


def find_config(name):
    for d in [ROOT / "build/generated", ROOT / "build/Release/runtime", ROOT / "build/runtime"]:
        p = d / name
        if p.is_file():
            return p
    return ROOT / "build/generated" / name


def find_default_granite_model_dir():
    candidates = [
        Path.home() / "models/s2",
        Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share")) / "roclint/Roclint/models/s2",
        Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share")) / "roclint/Roclint/models/granite-3b",
    ]
    for c in candidates:
        if c.is_dir() and any(c.glob("*.gguf")):
            return c
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default_model = find_default_granite_model_dir()
    parser.add_argument("--model-dir", type=Path, default=default_model, required=default_model is None)
    parser.add_argument("--worker", type=Path, default=find_binary("roclint_granite_worker", "runtime"))
    parser.add_argument("--config", type=Path, default=find_config("granite-runtime-config.json"))
    parser.add_argument("--contracts-tool", type=Path, default=find_binary("roclint_granite_contracts"))
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--quantization", action="append", choices=("Q4_K_M", "Q5_K_M", "Q8_0"))
    parser.add_argument("--case", action="append", help="Run only the named development fixture(s); records the restricted scope")
    parser.add_argument("--diagnostic-no-deadlines", action="store_true",
        help="Explicit diagnostic mode: helper semantic calls are uncapped, observer waits at most 300 seconds; not release evidence")
    parser.add_argument("--skip-recovery", action="store_true", help="Run HOT fixtures only; explicitly omit the C08 recovery measurements")
    parser.add_argument("--output", type=Path, default=ROOT / "build/native-granite-benchmark.json")
    args = parser.parse_args()
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
    report = {"started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "platform": platform.platform(),
        "processor": platform.processor(), "logical_cpus": os.cpu_count(), "configuration": manifest,
        "worker_sha256": hashlib.sha256(args.worker.read_bytes()).hexdigest(), "corpus_sha256": hashlib.sha256(corpus.read_bytes()).hexdigest(),
        "recovery_requested": not args.skip_recovery,
        "selected_case_ids": [case["id"] for case in cases], "full_corpus_requested": not args.case,
        "diagnostic_no_deadlines": args.diagnostic_no_deadlines,
        "release_deadline_policy_requested": not args.diagnostic_no_deadlines,
        "review_scope": contracts["review_scope"], "measurement_scope": "Single synthetic run per configuration; working set is process-specific. GPU memory, idle CPU and native macOS validation are not measured.", "runs": []}

    def save():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    failed = False
    for quantization in args.quantization or ("Q8_0",):
        run = {"quantization": quantization, "cases": [], "controls": []}
        report["runs"].append(run)
        worker = None
        try:
            worker = Worker(args, manifest, quantization, environment)
            for operation in ("hello", "load"):
                control = worker.exchange(operation)
                run["controls"].append({"op": operation, **control})
                print(quantization, operation, control["elapsed_ms"], control["response"]["ok"], flush=True)
                if not control["response"]["ok"]:
                    raise RuntimeError("Native helper could not load the selected configuration")
            for fixture in cases:
                result = worker.exchange(fixture["op"], {key: fixture[key] for key in ("prompt", "instructions", "schema")}, fixture)
                run["cases"].append({**fixture, "phase": "HOT", **result})
                failed = failed or not result["response"]["ok"]
                print(quantization, fixture["id"], result["elapsed_ms"], result["response"]["ok"], flush=True)
                save()
            for tier in (() if args.skip_recovery else ("IDLE_MAPPED", "UNLOADED")):
                resource = worker.exchange("set_residency", {"tier": tier})
                run["controls"].append({"op": "set_residency", "requested": tier, **resource})
                warmed = worker.exchange("warm")
                run["controls"].append({"op": "warm", "after_requested": tier, **warmed})
                fixture = cases[0]
                result = worker.exchange(fixture["op"], {key: fixture[key] for key in ("prompt", "instructions", "schema")}, fixture)
                run["cases"].append({**fixture, "phase": "AFTER_" + tier, **result})
                failed = failed or not resource["response"]["ok"] or not warmed["response"]["ok"] or not result["response"]["ok"]
                save()
        except Exception as error:
            run["error"] = str(error)
            failed = True
            print(quantization, "FAILED:", error, flush=True)
        finally:
            if worker:
                run["shutdown"] = worker.close()
                run["diagnostic_tail"] = worker.diagnostics
                failed = failed or not run["shutdown"]["graceful"]
            save()
    validation = subprocess.run([str(args.contracts_tool), "--validate", str(args.output)], capture_output=True, env=environment)
    if validation.returncode:
        report["validation_error"] = validation.stderr.decode("utf-8", errors="replace").strip()
        report["mechanical_validation"] = []
        failed = True
    else:
        report["mechanical_validation"] = json.loads(validation.stdout.decode("utf-8"))
    failed = failed or any(not item.get("transport_ok") or not item.get("parse_valid") or
        item.get("structurally_valid") is False for item in report["mechanical_validation"])
    save()
    print("Full synthetic report:", args.output, flush=True)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
