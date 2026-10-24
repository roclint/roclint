"""Check pinned offline Julia CPU inference with bounded synthetic batches; never download models."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

MODEL = "SupersonicLabs/Julia-1"
REVISION = "a85b127321d580d65176c89ced8273f305745d85"
WEIGHTS = "df853bf7fe424420011f3d0c47a05d7341aa9eefa7fb9f203ea4aada4ad95b72"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def unit(value):
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value) and 0 <= value <= 1


class WindowsJob:
    """Own the probe's launcher and acknowledged interpreter; never enumerate/kill by name."""
    def __init__(self):
        import ctypes as c
        from ctypes import wintypes as w
        self.c = c
        class Basic(c.Structure):
            _fields_ = [("process_time", c.c_int64), ("job_time", c.c_int64), ("flags", w.DWORD),
                ("minimum", c.c_size_t), ("maximum", c.c_size_t), ("active", w.DWORD),
                ("affinity", c.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]
        class IO(c.Structure):
            _fields_ = [(name, c.c_uint64) for name in ["read_ops", "write_ops", "other_ops", "read", "write", "other"]]
        class Extended(c.Structure):
            _fields_ = [("basic", Basic), ("io", IO), ("process_memory", c.c_size_t), ("job_memory", c.c_size_t),
                ("peak_process", c.c_size_t), ("peak_job", c.c_size_t)]
        self.kernel = c.WinDLL("kernel32", use_last_error=True)
        for name, args, result in [
            ("CreateJobObjectW", [w.LPVOID, w.LPCWSTR], w.HANDLE),
            ("SetInformationJobObject", [w.HANDLE, c.c_int, w.LPVOID, w.DWORD], w.BOOL),
            ("OpenProcess", [w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
            ("AssignProcessToJobObject", [w.HANDLE, w.HANDLE], w.BOOL),
            ("TerminateJobObject", [w.HANDLE, w.UINT], w.BOOL),
            ("CloseHandle", [w.HANDLE], w.BOOL)]:
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = args, result
        self.handle = self.kernel.CreateJobObjectW(None, None)
        require(self.handle, "Could not create the probe's owned process job")
        limits = Extended()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, 9, c.byref(limits), c.sizeof(limits)):
            self.close()
            raise RuntimeError("Could not configure the probe's owned process job")
        self.pids = set()

    def attach(self, pid):
        if pid in self.pids:
            return
        process = self.kernel.OpenProcess(0x100 | 0x1, False, pid)  # SET_QUOTA | TERMINATE
        require(process, "Could not open the probe's acknowledged owned process")
        try:
            require(self.kernel.AssignProcessToJobObject(self.handle, process), "Could not own the probe's interpreter")
            self.pids.add(pid)
        finally:
            self.kernel.CloseHandle(process)

    def terminate(self):
        if self.handle:
            self.kernel.TerminateJobObject(self.handle, 1)

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", required=True, type=Path)
    parser.add_argument("--model-dir", required=True, type=Path)
    parser.add_argument("--worker", type=Path, default=root / "app/runtime/julia_worker.py")
    parser.add_argument("--output", type=Path, default=root / "build/assumption-scoring-live.json")
    args = parser.parse_args()
    worker = args.worker.resolve()
    worker_hash = hashlib.sha256(worker.read_bytes()).hexdigest()
    environment = os.environ.copy()
    environment.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", PYTHONUTF8="1")
    process = subprocess.Popen([str(args.python.resolve()), "-u", str(worker), "--model-dir", str(args.model_dir.resolve())],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8",
        bufsize=1, env=environment, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    messages = queue.Queue()
    errors = []

    def output():
        while True:
            line = process.stdout.readline(256 * 1024 + 1)
            if not line:
                messages.put(None)
                return
            try:
                require(len(line.encode("utf-8")) <= 256 * 1024 and line.endswith("\n"), "Oversized worker response")
                messages.put(json.loads(line))
            except (ValueError, RuntimeError):
                messages.put({"ok": False, "error": "Malformed or oversized worker response"})
                return

    def stderr():
        for line in process.stderr:
            if len(errors) < 10:
                errors.append(line.strip()[:200])

    threading.Thread(target=output, daemon=True).start()
    threading.Thread(target=stderr, daemon=True).start()

    def send(payload):
        payload = dict(payload)
        identifier, operation = payload.pop("id"), payload.pop("op")
        request = {"protocol_version": 1, "type": "request", "request_id": str(identifier), "op": operation,
            "request_group_id": "live_group", "attempt_id": "live_attempt_%d" % identifier,
            "session_id": "live_session", "draft_revision_id": "live_revision", "state_version": 1,
            "provider": "julia_local", "model_id": MODEL, "revision": REVISION, "weights_sha256": WEIGHTS,
            "configuration_id": "julia.auto.bf16-fp32.v2",
            "input_hash": hashlib.sha256(payload.get("prompt", "").encode("utf-8")).hexdigest(), "payload": payload}
        process.stdin.write(json.dumps(request) + "\n")
        process.stdin.flush()
        sent[request["request_id"]] = request
        return request

    def receive(request, timeout=None):
        operation = request["op"]
        deadline = time.monotonic() + (timeout if timeout is not None else 3 if operation == "hello" else 180 if operation == "load" else 60)
        identifier = request["request_id"]
        while identifier not in inbox:
            try:
                response = messages.get(timeout=max(0.001, deadline - time.monotonic()))
            except queue.Empty:
                raise RuntimeError("Julia timed out during " + operation) from None
            require(response is not None, "Julia exited: " + str(errors))
            target = response.get("request_id")
            require(target in sent and target not in inbox, "Unknown/duplicate worker response")
            expected = sent[target]
            require(all(response.get(key) == value for key, value in expected.items() if key not in {"type", "payload"}), "Worker returned another request context")
            require(response.get("ok") is True, response.get("error", "Julia operation failed"))
            expected_type = "hello_ack" if expected["op"] == "hello" else "runtime_status" if expected["op"] == "runtime_status" else "result"
            require(response.get("type") == expected_type, "Incorrect response type")
            inbox[target] = response
        sent.pop(identifier)
        return inbox.pop(identifier)

    def exchange(payload, timeout=None):
        return receive(send(payload), timeout)

    records = []
    job = None
    inbox = {}
    sent = {}
    try:
        print("Loading installed pinned Julia model offline...", flush=True)
        hello = exchange({"id": 1, "op": "hello"})
        if os.name == "nt":
            worker_pid = hello["result"].get("process_id")
            require(type(worker_pid) is int and 0 < worker_pid < 2**32 and worker_pid != os.getpid(), "Invalid worker PID")
            require(worker_pid == process.pid or hello["result"].get("parent_process_id") == process.pid, "Unrelated worker process")
            job = WindowsJob()
            job.attach(worker_pid)
            job.attach(process.pid)
        load_request = send({"id": 2, "op": "load"})
        time.sleep(0.2)
        status = exchange({"id": 3, "op": "runtime_status"}, timeout=3)
        require(status["result"]["state"] in {"busy", "unloaded", "ready"}, "Invalid runtime status")
        print("Control/status request completed while model load was pending.", flush=True)
        loaded = receive(load_request)
        require(loaded["result"]["state"] == "ready", "Model did not become ready")
        print("Loaded in %.0f ms" % loaded["latency_ms"], flush=True)
        for identifier, count in [(4, 3), (5, 0)]:
            definitions = [("Which tests?", "Unit tests", "Integration tests"), ("Which service?", "Billing", "Shipping"),
                           ("What output?", "Patch", "Explanation")]
            payload = {"id": identifier, "op": "evaluate", "plan_version": 2, "snapshot_id": "live_synthetic_%d" % identifier,
                "prompt": "Update the service tests. Preserve the public API and keep the tests deterministic.",
                "groups": [{"id": "grp_%d" % index, "question": question, "alternatives": [
                    {"id": "a%d0" % index, "label": first}, {"id": "a%d1" % index, "label": second}]}
                    for index, (question, first, second) in enumerate(definitions[:count])],
                "assumptions": [{"id": "asm_%d" % index, "statement": statement} for index, statement in enumerate([
                    "The public API must stay unchanged.", "The tests must remain deterministic.",
                    "The service uses PostgreSQL.", "The repository has integration tests."])]}
            response = exchange(dict(payload))
            result = response["result"]
            require(result["snapshot_id"] == payload["snapshot_id"] and result["plan_version"] == 2, "Incorrect result binding")
            require(len(result["groups"]) == count and len(result["assumptions"]) == 4, "Incorrect batch cardinality")
            require({item["group_id"] for item in result["groups"]} == {item["id"] for item in payload["groups"]}, "Incorrect group IDs")
            require({item["assumption_id"] for item in result["assumptions"]} == {item["id"] for item in payload["assumptions"]}, "Incorrect assumption IDs")
            for group in result["groups"]:
                source = next(item for item in payload["groups"] if item["id"] == group["group_id"])
                probabilities = group["probabilities"]
                require(len(probabilities) == 2 and {item["id"] for item in probabilities} == {item["id"] for item in source["alternatives"]}, "Invalid alternative IDs")
                require(all(unit(item["probability"]) for item in probabilities), "Invalid probabilities")
                require(abs(sum(item["probability"] for item in probabilities) - 1) < 0.01, "Invalid probability sum")
                best = max(item["probability"] for item in probabilities)
                require(any(item["id"] == group["winner"] and item["probability"] >= best for item in probabilities), "Incorrect winning alternative")
                require(unit(group["ambiguity"]) and isinstance(group["noul_certainty"], bool), "Invalid ambiguity metadata")
            require(all(unit(item["uncertainty"]) and isinstance(item["noul_certainty"], bool) for item in result["assumptions"]), "Invalid assumption scores")
            records.append({"groups": count, "assumptions": 4, "latency_ms": response["latency_ms"], "result": result})
            print("PASS: %d groups + 4 assumptions, %.1f ms" % (count, response["latency_ms"]), flush=True)
        shutdown_started = time.monotonic()
        exchange({"id": 6, "op": "shutdown"}, timeout=2)
        graceful = True
        try:
            require(process.wait(timeout=2) == 0, "Julia exited with an error after shutdown")
        except subprocess.TimeoutExpired:
            graceful = False
            if job:
                job.terminate()
            process.kill()
            process.wait(timeout=5)
        shutdown = {"acknowledged": True, "graceful": graceful, "forced": not graceful,
            "elapsed_ms": (time.monotonic() - shutdown_started) * 1000, "exit_code": process.returncode}
        print("Shutdown: " + ("graceful" if graceful else "owned process group terminated after 2-second grace"), flush=True)
        require(hashlib.sha256(worker.read_bytes()).hexdigest() == worker_hash, "Worker changed during verification")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"worker_sha256": worker_hash, "model": MODEL, "revision": REVISION,
            "load_latency_ms": loaded["latency_ms"], "load_status": status["result"],
            "runtime_profile": loaded["result"].get("runtime"), "shutdown": shutdown, "batches": records,
            "scope": "Offline local Julia inference/protocol bounds only; actual device is recorded separately; not a quality study or latency guarantee"}, indent=2) + "\n", encoding="utf-8")
    finally:
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            if job:
                job.terminate()
            process.kill()
            process.wait(timeout=5)
        finally:
            if job:
                job.close()


if __name__ == "__main__":
    main()
