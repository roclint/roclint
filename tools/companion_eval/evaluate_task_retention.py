"""Replay synthetic task-retention cases through the native model and C++ audit.

This is a small semantic challenge, not a full-app quality or approval gate.
It never reads history, downloads weights, edits preferences, or inserts text.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from types import SimpleNamespace

from raw_discovery import V13Worker, WORKTREE


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    binary = WORKTREE / "build" / "Release"
    suffix = ".exe" if os.name == "nt" else ""
    parser.add_argument("--worker", type=Path, default=binary / ("roclint_granite_worker" + suffix))
    parser.add_argument("--contracts-tool", type=Path, default=binary / ("roclint_granite_contracts" + suffix))
    parser.add_argument("--config", type=Path, default=WORKTREE / "build/generated/granite-runtime-config.json")
    parser.add_argument("--cases", type=Path, default=WORKTREE / "tests/fixtures/companion_eval/task_retention_2026-11-16.json")
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    corpus_bytes = args.cases.read_bytes()
    corpus = json.loads(corpus_bytes)
    if corpus.get("synthetic") is not True or not corpus.get("cases"):
        parser.error("A nonempty, explicitly synthetic corpus is required")
    seen = set()
    for case in corpus["cases"]:
        key = case.get("id", "")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", key) or key in seen:
            parser.error("Case IDs must be unique and safe filenames")
        seen.add(key)
        if case.get("expected") not in ("Pass", "Fail") or not all(
            isinstance(case.get(field), str) and case[field].strip() for field in ("prompt", "candidate")
        ):
            parser.error("Each case needs source/candidate text and a Pass/Fail expectation")
    manifest = json.loads(args.config.read_text(encoding="utf-8-sig"))
    environment = os.environ.copy()
    if args.qt_bin:
        environment["PATH"] = str(args.qt_bin) + os.pathsep + environment.get("PATH", "")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fixtures = args.out.parent / (args.out.stem + "-fixtures")
    fixtures.mkdir(exist_ok=True)
    report = {
        "schema": "roclint.task_retention_replay.v1",
        "utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "scope": "Single-sample synthetic model challenge plus production C++ authority reconciliation; not full-app or downstream executor evidence.",
        "corpus_sha256": hashlib.sha256(corpus_bytes).hexdigest(),
        "configuration": manifest,
        "worker_sha256": hashlib.sha256(args.worker.read_bytes()).hexdigest(),
        "contracts_tool_sha256": hashlib.sha256(args.contracts_tool.read_bytes()).hexdigest(),
        "thinking_enabled": False,
        "cases": [],
    }

    def save():
        args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    def export(path):
        return json.loads(subprocess.check_output(
            [str(args.contracts_tool), "--audit-fixture", str(path)], env=environment
        ))

    lock = (args.out.parent / ".task-retention.lock").open("a+b")
    try:
        if os.name == "nt":
            import msvcrt
            lock.seek(0)
            if not lock.read(1):
                lock.write(b"0")
                lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock.close()
        parser.error("Another task-retention replay is running in this evidence directory")

    worker = None
    try:
        worker_args = SimpleNamespace(worker=args.worker, model_dir=args.model_dir,
            temperature=None, cpu_reference=False, diagnostic_no_deadlines=False)
        worker = V13Worker(worker_args, manifest, "Q8_0", environment)
        for operation in ("hello", "load"):
            result = worker.exchange(operation)
            report.setdefault("controls", []).append({"op": operation, **result})
            if not result["response"].get("ok"):
                raise RuntimeError(f"Native {operation} failed")
        for case in corpus["cases"]:
            fixture = {**case, "synthetic": True}
            path = fixtures / (case["id"] + ".json")
            path.write_text(json.dumps(fixture), encoding="utf-8")
            contract = export(path)
            if "granite.goldfish_audit.v2" not in contract["instructions"]:
                raise RuntimeError("The contract exporter is stale: audit v2 is required")
            reset = worker.exchange("reset_cache")
            if not reset["response"].get("ok"):
                raise RuntimeError("Could not clear KV context before the independent audit")
            payload = {key: contract[key] for key in ("prompt", "instructions", "schema")}
            payload["thinking_enabled"] = False
            result = worker.exchange(contract["op"], payload, contract)
            raw = result["response"].get("result", {}).get("text", "")
            response_path = fixtures / (case["id"] + "-response.json")
            response_path.write_text(json.dumps({**fixture, "model_response": raw}), encoding="utf-8")
            verdict = export(response_path)
            passed = bool(result["response"].get("ok")) and verdict["outcome"] == case["expected"]
            report["cases"].append({**case, "passed": passed, "raw": raw, "parsed": verdict, **result})
            save()
            print(case["id"], "PASS" if passed else "MISMATCH", verdict["outcome"], flush=True)
    except Exception as error:
        report["error"] = str(error)
    finally:
        if worker is not None:
            report["shutdown"] = worker.close()
        lock.close()
        report["passed"] = sum(item["passed"] for item in report["cases"])
        report["requested"] = len(corpus["cases"])
        save()
    return 0 if not report.get("error") and report["passed"] == report["requested"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
