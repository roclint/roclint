"""Helper-only raw discovery capture for the 2026-11-15 companion measurement.

Reuses the existing Worker transport in tools/check_granite_runtime.py and the
production prompt builder/schema emitted by tools/granite_contracts.cpp. It does
not route through Julia, the HostController, a codebase index or the UI.

This stage exists to retain raw model JSON (source quotations, option effects,
assumed premises) that the host's user-facing properties do not expose. It is
helper-only evidence, not full-app behavior.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from types import SimpleNamespace


WORKTREE = Path(__file__).resolve().parents[2]


def load_runtime_module():
    path = WORKTREE / "tools/check_granite_runtime.py"
    spec = importlib.util.spec_from_file_location("roclint_check_granite_runtime", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load_runtime_module()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class V13Worker(runtime.Worker):
    """Compatibility wrapper for the v13 worker capabilities.

    tools/check_granite_runtime.py still expects the older 12 s analysis
    capability; the v13 worker advertises 45 s. This wrapper reuses the parent's
    process, frame parser and request construction, but validates the v13
    deadlines and gives a 45 s analysis enough observer time. It is helper-only
    and does not change or claim product deadline behavior.
    """

    def exchange(self, operation, payload=None, fixture=None):
        self.sequence += 1
        payload = payload or {}
        fixture = fixture or {}
        request = {"protocol_version": 1, "type": "request", "request_id": str(self.sequence), "op": operation,
            "request_group_id": "grp_native_benchmark_" + str(self.sequence), "attempt_id": "att_native_benchmark_" + str(self.sequence),
            "session_id": "sess_benchmark_synthetic", "draft_revision_id": "rev_sess_benchmark_synthetic_1", "state_version": 1,
            "provider": "granite_local", "model_id": self.manifest["model_family"], "model_revision": self.manifest["model_revision"],
            "runtime_revision": self.manifest["runtime_revision"], "configuration_id": self.manifest["configuration_id"],
            "quantization": self.quantization, "weights_sha256": self.candidate["sha256"], "input_hash": runtime.digest(payload.get("prompt", "")),
            "intent_id": fixture.get("intent_id", ""), "rewrite_profile_id": fixture.get("rewrite_profile_id", ""),
            "rewrite_profile_version": 1 if fixture.get("rewrite_profile_id") else 0, "payload": payload}
        started = runtime.time.perf_counter()
        self.process.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8"))
        self.process.stdin.flush()
        timeout = 3 if operation == "hello" else 2 if operation == "shutdown" else 60 if operation == "granite_analyze" else 90 if operation == "granite_rewrite" else 185
        response = self.messages.get(timeout=timeout)
        if isinstance(response, Exception):
            raise response
        while isinstance(response, dict) and response.get("type") == "partial_result":
            response = self.messages.get(timeout=timeout)
            if isinstance(response, Exception):
                raise response
        for key, value in request.items():
            if key not in ("type", "payload") and response.get(key) != value:
                raise RuntimeError("Native response binding mismatch: " + key)
        expected_type = "error" if response.get("ok") is False else "hello_ack" if operation == "hello" else "runtime_status" if operation == "runtime_status" else "result"
        if response.get("type") != expected_type or not isinstance(response.get("ok"), bool):
            raise RuntimeError("Native response type/status mismatch")
        if operation == "hello" and response["ok"]:
            capabilities = response.get("result", {})
            if capabilities.get("diagnostic_unbounded") is not False:
                raise RuntimeError("Native diagnostic deadline mode did not match the explicit launch")
            if (capabilities.get("analysis_timeout_ms") != 45000 or
                    capabilities.get("rewrite_timeout_ms") != 60000):
                raise RuntimeError("Native operation deadline capabilities were incompatible with v13")
        if (operation in ("granite_analyze", "granite_rewrite") and response["ok"] and
                self.manifest.get("output_format") == "compact_json" and
                not runtime.compact_json(response.get("result", {}).get("text"))):
            raise RuntimeError("Native helper violated the declared compact JSON output format")
        return {"elapsed_ms": round((runtime.time.perf_counter() - started) * 1000, 2), "response": response,
                "memory": runtime.windows_working_set(self.process.pid)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contracts", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--contracts-tool", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path)
    args = parser.parse_args()

    runtime = load_runtime_module()
    contracts = json.loads(args.contracts.read_text(encoding="utf-8-sig"))
    manifest = json.loads(args.config.read_text(encoding="utf-8-sig"))
    cases = contracts["cases"]
    environment = os.environ.copy()
    if args.qt_bin:
        environment["PATH"] = str(args.qt_bin) + os.pathsep + environment.get("PATH", "")

    worker_args = SimpleNamespace(worker=args.worker, model_dir=args.model_dir, quantization="Q8_0",
                                  temperature=None, cpu_reference=False,
                                  diagnostic_no_deadlines=False, qt_bin=args.qt_bin)
    report = {
        "schema": "roclint.companion_eval.raw_worker.v1",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "scope": contracts.get("review_scope", ""),
        "deadline_mode": "v13_release_capabilities",
        "deadline_note": ("The existing raw checker expects the older 12 s analysis capability; "
                          "the v13 worker advertises 45 s. V13Worker reuses the checker's process/"
                          "frame/request transport and validates the v13 45 s/60 s capabilities. "
                          "This helper-only capture does not test deadline enforcement."),
        "platform": platform.platform(),
        "worker": {"path": str(args.worker), "sha256": sha256(args.worker)},
        "model_directory": str(args.model_dir),
        "configuration": manifest,
        "cases": [],
    }
    worker = None
    try:
        worker = V13Worker(worker_args, manifest, "Q8_0", environment)
        for operation in ("hello", "load"):
            control = worker.exchange(operation)
            report.setdefault("controls", []).append({"op": operation, **control})
            print("control", operation, control["elapsed_ms"], control["response"].get("ok"), flush=True)
            if not control["response"].get("ok"):
                raise RuntimeError(f"{operation} failed")
        for case in cases:
            if case.get("op") != "granite_analyze":
                continue
            result = worker.exchange(case["op"], {key: case[key] for key in ("prompt", "instructions", "schema")}, case)
            report["cases"].append({**case, "phase": "HOT", **result})
            print("case", case["id"], result["elapsed_ms"], result["response"].get("ok"), flush=True)
    except Exception as error:  # keep the partial receipt
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if worker:
            report["shutdown"] = worker.close()
            report["diagnostic_tail"] = worker.diagnostics

    # Production parser/policy validation for the recorded responses. The helper
    # report shape matches roclint_granite_contracts --validate expectations.
    validation_path = args.out.with_name(args.out.stem + "_validation.json")
    validation_report = {"runs": [{"quantization": "Q8_0", "cases": [
        {**case, "response": case.get("response", {})} for case in report["cases"]]}]}
    validation_input = args.out.with_name(args.out.stem + "_validation_input.json")
    validation_input.write_text(json.dumps(validation_report, ensure_ascii=False, indent=2) + "\n",
                                encoding="utf-8")
    validation = subprocess.run([str(args.contracts_tool), "--validate", str(validation_input)],
                                capture_output=True, env=environment)
    if validation.returncode == 0:
        validation_path.write_bytes(validation.stdout)
        try:
            report["mechanical_validation"] = json.loads(validation.stdout)
        except ValueError:
            report["mechanical_validation_error"] = validation.stdout.decode("utf-8", errors="replace")[:2000]
    else:
        report["mechanical_validation_error"] = validation.stderr.decode("utf-8", errors="replace")[:4000]

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0 if "error" not in report else 1


if __name__ == "__main__":
    raise SystemExit(main())
