#!/usr/bin/env python3
"""Agent 5 runtime timing driver.

Drives the owned native Granite helper directly over its JSONL protocol with a
disposable synthetic prompt. It measures cold readiness, a serialized queued
request that is cancelled, and a warm repeat. It never approves, inserts or
sends generated text.

Usage:
  python agent5-live-driver.py --worker PATH --model-dir PATH --manifest JSON \
      --case JSON --trace PATH --out PATH
"""
import argparse
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time


# Pinned production default: bounded thinking is opt-in and off.
THINKING = False


def empty_hash() -> str:
    return hashlib.sha256(b"").hexdigest()


def read_frames(stream, sink):
    try:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                sink.put(json.loads(line))
            except json.JSONDecodeError:
                sink.put({"type": "unparseable", "raw": line[:200]})
    finally:
        sink.put(None)


class Helper:
    def __init__(self, worker, model_dir, quantization, trace_path):
        env = dict(os.environ)
        env["ROCLINT_TRACE_TIMING"] = trace_path
        self.process = subprocess.Popen(
            [worker, "--model-dir", model_dir, "--quantization", quantization],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1, env=env)
        self.frames = queue.Queue()
        self.reader = threading.Thread(target=read_frames, args=(self.process.stdout, self.frames), daemon=True)
        self.reader.start()
        self.stderr = []
        threading.Thread(target=lambda: self.stderr.extend(self.process.stderr.readlines()), daemon=True).start()
        self.next_id = 0
        self.sent = {}
        self.received = {}

    def send(self, op, payload, identity, group, attempt, revision, state_version=0, prompt="", surface=None,
             remaining_budget_ms=None):
        self.next_id += 1
        wire = str(self.next_id)
        request = dict(identity)
        request.update({
            "protocol_version": 1,
            "type": "request",
            "request_id": wire,
            "op": op,
            "request_group_id": group,
            "attempt_id": attempt,
            "session_id": "sess_agent5_runtime",
            "draft_revision_id": revision,
            "state_version": state_version,
            "intent_id": "",
            "rewrite_profile_id": "",
            "rewrite_profile_version": 0,
            "input_hash": hashlib.sha256(prompt.encode("utf-8")).hexdigest() if prompt else empty_hash(),
            "payload": payload,
        })
        if op in ("granite_analyze", "granite_rewrite", "granite_word_refinement"):
            request["thinking_enabled"] = THINKING
        if surface:
            request["surface"] = surface
        if remaining_budget_ms is not None:
            request["remaining_budget_ms"] = int(remaining_budget_ms)
        self.sent[wire] = {"op": op, "sent_ms": time.monotonic() * 1000.0}
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        return wire

    def wait(self, wire, timeout=300.0):
        if str(wire) in self.received:
            frame = self.received.pop(str(wire))
            return frame, time.monotonic() * 1000.0 - self.sent[str(wire)]["sent_ms"]
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                frame = self.frames.get(timeout=1.0)
            except queue.Empty:
                if self.process.poll() is not None:
                    raise RuntimeError(f"helper exited {self.process.returncode}: {''.join(self.stderr)[-2000:]}")
                continue
            if frame is None:
                raise RuntimeError(f"helper closed output: {''.join(self.stderr)[-2000:]}")
            if frame.get("type") == "partial_result":
                continue
            other = str(frame.get("request_id"))
            if other == str(wire):
                elapsed = time.monotonic() * 1000.0 - self.sent[str(wire)]["sent_ms"]
                return frame, elapsed
            self.received[other] = frame
        raise TimeoutError(f"request {wire} did not complete")

    def wait_ready(self, wire, timeout=600.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame, elapsed = self.wait(wire, timeout=max(1.0, deadline - time.monotonic()))
            return frame, elapsed
        raise TimeoutError("ready")

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.close()
                self.process.wait(timeout=20)
        except Exception:
            try:
                self.process.kill()
            except Exception:
                pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", required=True)
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--case", required=True, help="contracts JSON output file")
    parser.add_argument("--trace", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    manifest = json.load(open(args.manifest, encoding="utf-8"))
    case = json.load(open(args.case, encoding="utf-8"))
    cases = case["cases"] if isinstance(case.get("cases"), list) else [case]
    analysis = cases[0]
    queued_case = cases[1] if len(cases) > 1 else None
    instructions = analysis["instructions"]
    schema_text = analysis["schema"]
    prompt = analysis["prompt"]

    quantization = manifest["default_quantization"]
    candidate = next(c for c in manifest["candidates"] if c["quantization"] == quantization)
    identity = {
        "provider": "granite_local",
        "model_id": manifest["model_family"],
        "model_revision": manifest["model_revision"],
        "runtime_revision": manifest["runtime_revision"],
        "configuration_id": manifest["configuration_id"],
        "quantization": quantization,
        "weights_sha256": candidate["sha256"],
    }
    for path in (args.trace,):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass

    helper = Helper(args.worker, args.model_dir, quantization, args.trace)
    receipt = {
        "worker": args.worker,
        "model_dir": args.model_dir,
        "configuration_id": manifest["configuration_id"],
        "backend": manifest["backend"],
        "quantization": quantization,
        "model_revision": manifest["model_revision"],
        "runtime_revision": manifest["runtime_revision"],
        "trace": args.trace,
        "prompt_id": analysis.get("id", "synthetic"),
        "thinking_enabled": False,
        "requests": {},
    }
    try:
        hello, hello_ms = helper.wait(helper.send("hello", {}, identity, "runtime", "hello", "runtime"), 30)
        receipt["requests"]["hello"] = {"ok": hello.get("ok"), "elapsed_ms": hello_ms}
        if not hello.get("ok"):
            raise RuntimeError(f"hello failed: {hello}")

        load, load_ms = helper.wait(helper.send("load", {}, identity, "runtime", "load", "runtime"), 600)
        receipt["requests"]["load"] = {"ok": load.get("ok"), "elapsed_ms": load_ms,
                                       "result": load.get("result")}
        if not load.get("ok"):
            raise RuntimeError(f"load failed: {load}")

        def analyze(wire_id, attempt, extra_sleep=0.0):
            return helper.send("granite_analyze",
                {"prompt": prompt, "instructions": instructions, "schema": schema_text,
                 "thinking_enabled": THINKING},
                identity, "trace_agent5_" + attempt, attempt, "rev_agent5_" + attempt,
                prompt=prompt, surface="interview")

        a = analyze("a", "a")
        time.sleep(0.35)
        q = None
        if queued_case:
            q = helper.send("granite_analyze",
                {"prompt": queued_case["prompt"], "instructions": queued_case["instructions"],
                 "schema": queued_case["schema"], "thinking_enabled": False},
                identity, "trace_agent5_queued", "queued", "rev_agent5_queued",
                prompt=queued_case["prompt"], surface="interview")
        time.sleep(0.35)
        b = analyze("b", "b")
        # A deliberately short caller budget with no cancel: the helper must reject
        # this queued request before inference when its turn arrives.
        d = helper.send("granite_analyze",
            {"prompt": prompt, "instructions": instructions, "schema": schema_text,
             "thinking_enabled": False},
            identity, "trace_agent5_expiring", "expiring", "rev_agent5_expiring",
            prompt=prompt, surface="interview", remaining_budget_ms=100)
        time.sleep(0.9)
        cancel = helper.send("cancel", {"target_request_id": b}, identity,
                             "trace_agent5_cancel", "cancel_b", "rev_agent5_cancel")
        cancel_reply, cancel_ms = helper.wait(cancel, 30)
        receipt["requests"]["cancel_b"] = {"elapsed_ms": cancel_ms, "reply": cancel_reply.get("result")}

        frame_a, elapsed_a = helper.wait(a, 600)
        receipt["requests"]["analyze_a"] = {
            "ok": frame_a.get("ok"), "elapsed_ms": elapsed_a,
            "latency_ms": frame_a.get("latency_ms"),
            "error": frame_a.get("error"),
            "result": frame_a.get("result"),
        }
        if q is not None:
            frame_q, elapsed_q = helper.wait(q, 600)
            receipt["requests"]["analyze_queued_behind_a"] = {
                "ok": frame_q.get("ok"), "elapsed_ms": elapsed_q,
                "latency_ms": frame_q.get("latency_ms"),
                "error": frame_q.get("error"),
                "result": frame_q.get("result"),
            }
        frame_b, elapsed_b = helper.wait(b, 60)
        receipt["requests"]["analyze_b_queued"] = {
            "ok": frame_b.get("ok"), "elapsed_ms": elapsed_b,
            "latency_ms": frame_b.get("latency_ms"),
            "error": frame_b.get("error"),
            "error_code": frame_b.get("error_code"),
        }
        frame_d, elapsed_d = helper.wait(d, 60)
        receipt["requests"]["analyze_expiring_queued"] = {
            "ok": frame_d.get("ok"), "elapsed_ms": elapsed_d,
            "latency_ms": frame_d.get("latency_ms"),
            "error": frame_d.get("error"),
            "error_code": frame_d.get("error_code"),
        }
        if frame_a.get("ok"):
            c = analyze("c", "c")
            frame_c, elapsed_c = helper.wait(c, 600)
            receipt["requests"]["analyze_c_warm_repeat"] = {
                "ok": frame_c.get("ok"), "elapsed_ms": elapsed_c,
                "latency_ms": frame_c.get("latency_ms"),
                "error": frame_c.get("error"),
                "result": frame_c.get("result"),
            }
        receipt["process_exit"] = helper.process.poll()
    finally:
        helper.close()
        receipt["stderr_tail"] = "".join(helper.stderr)[-4000:]
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(receipt, handle, indent=2)
    print(json.dumps({k: v for k, v in receipt["requests"].items() if isinstance(v, dict)},
                     indent=2)[:6000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
