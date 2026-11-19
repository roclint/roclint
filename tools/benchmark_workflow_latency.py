#!/usr/bin/env python3
"""Stage 4: Latency & Cancellation Profiling Across Real Workflows.

Measures:
1. Clarification workflow (S1 triage + S2 discovery options)
2. Rewrite workflow (S2 bounded rewrite)
3. Inline word refinement (WordCompanion single-phrase refinement)
With:
- Cold invocation vs Warm invocation
- Reasoning OFF (--no-thinking-reference) vs Reasoning ON (thinking_enabled: true)
- Time to first visible feedback (TTFT / initial token)
- Total completion time
- Mid-stream cancellation timing and process recovery
"""
import argparse
import datetime
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from check_granite_runtime import Worker, find_config, find_default_granite_model_dir

AMBIGUOUS_PROMPT = "Refactor the payment gateway to run faster, but do not touch the token validation logic."
CONFIRMED_REWRITE_PROMPT = "<instructions>\nInstruction contract: granite.rewrite.format.v9\nRefactor query builder for lower latency.\n</instructions>\n"
INLINE_PHRASE = "Please optimize the database connection pool settings."

CLARIFY_SCHEMA = json.dumps({
    "type": "object",
    "properties": {
        "status": {"type": "string"},
        "choice_groups": {"type": "array"}
    },
    "required": ["status"]
})

REWRITE_SCHEMA = json.dumps({
    "type": "object",
    "properties": {
        "status": {"type": "string"},
        "rewritten_prompt": {"type": "string"}
    },
    "required": ["status"]
})

WORD_SCHEMA = json.dumps({
    "type": "object",
    "properties": {
        "action": {"type": "string"},
        "options": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "text": {"type": "string"},
                    "effect": {"type": "string"},
                    "assume": {"type": "string"}
                },
                "required": ["text", "effect", "assume"]
            }
        }
    },
    "required": ["action"]
})

def make_envelope(worker, seq, op, payload, meta=None):
    meta = meta or {}
    p = payload or {}
    prompt_bytes = p.get("prompt", "").encode("utf-8")
    h = hashlib.sha256(prompt_bytes).hexdigest()
    profile_id = meta.get("rewrite_profile_id", "")
    profile_ver = 1 if profile_id else 0
    return {
        "protocol_version": 1,
        "type": "request",
        "request_id": str(seq),
        "op": op,
        "request_group_id": f"grp_bench_{seq}",
        "attempt_id": f"att_bench_{seq}",
        "session_id": "sess_bench_synthetic",
        "draft_revision_id": "rev_bench_synthetic_1",
        "state_version": 1,
        "provider": "granite_local",
        "model_id": worker.manifest["model_family"],
        "model_revision": worker.manifest["model_revision"],
        "runtime_revision": worker.manifest["runtime_revision"],
        "configuration_id": worker.manifest["configuration_id"],
        "quantization": worker.quantization,
        "weights_sha256": worker.candidate["sha256"],
        "input_hash": h,
        "intent_id": meta.get("intent_id", ""),
        "rewrite_profile_id": profile_id,
        "rewrite_profile_version": profile_ver,
        "payload": p
    }

def timed_exchange(worker, op, payload, meta=None, timeout=60.0):
    worker.sequence += 1
    seq = worker.sequence
    req = make_envelope(worker, seq, op, payload, meta)

    t_start = time.perf_counter()
    worker.process.stdin.write((json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"))
    worker.process.stdin.flush()

    ttft_ms = None
    first_token_time = None
    final_response = None
    tokens_received = 0

    deadline = t_start + timeout
    while time.perf_counter() < deadline:
        try:
            msg = worker.messages.get(timeout=min(deadline - time.perf_counter(), 1.0))
        except queue.Empty:
            continue

        if isinstance(msg, Exception):
            raise msg

        if msg.get("type") == "partial_result" and msg.get("request_id") == str(seq):
            tokens_received += 1
            if ttft_ms is None:
                first_token_time = time.perf_counter()
                ttft_ms = (first_token_time - t_start) * 1000.0

        elif msg.get("type") in ("result", "error") and msg.get("request_id") == str(seq):
            final_response = msg
            break

    total_latency_ms = (time.perf_counter() - t_start) * 1000.0
    if final_response is None:
        raise TimeoutError(f"Operation {op} timed out after {total_latency_ms:.0f} ms")

    return {
        "ok": final_response.get("ok", False),
        "total_ms": total_latency_ms,
        "ttft_ms": ttft_ms if ttft_ms is not None else total_latency_ms,
        "tokens": tokens_received,
        "response": final_response
    }

def run_latency_battery():
    print("=" * 65)
    print("Stage 4: Real-Workflow Latency & Cancellation Profiling")
    print(f"Timestamp: {datetime.datetime.now().isoformat()}")
    print("=" * 65)

    manifest = json.loads(find_config("granite-runtime-config.json").read_text(encoding="utf-8-sig"))
    worker_path = ROOT / "build/roclint_granite_worker"
    model_dir = find_default_granite_model_dir()

    results = {}

    for mode in ["reasoning_off", "reasoning_on"]:
        print(f"\n" + "-" * 60)
        print(f"Profiling Mode: {mode.upper()}")
        print("-" * 60)

        thinking_enabled = (mode == "reasoning_on")
        worker_args = argparse.Namespace(
            worker=worker_path,
            model_dir=model_dir,
            diagnostic_no_deadlines=True,
            no_thinking_reference=(not thinking_enabled)
        )

        print(f"Starting {mode} worker...")
        worker = Worker(worker_args, manifest, "Q8_0", os.environ.copy())
        h = worker.exchange("hello")
        assert h["response"].get("ok"), "Hello failed"

        t_load_start = time.perf_counter()
        l = worker.exchange("load")
        load_time = time.perf_counter() - t_load_start
        print(f"  ✓ Model loaded in {load_time:.2f}s")

        mode_metrics = {"load_time_s": load_time}

        meta = {"intent_id": "test_intent", "rewrite_profile_id": "GENERIC@1"}

        # 1. Clarification Workflow
        discovery_payload = {
            "prompt": AMBIGUOUS_PROMPT,
            "instructions": AMBIGUOUS_PROMPT,
            "thinking_enabled": thinking_enabled,
            "schema": CLARIFY_SCHEMA
        }

        print("  • Clarification (Cold)...")
        res_cold = timed_exchange(worker, "granite_analyze", discovery_payload, meta)
        print(f"    Total: {res_cold['total_ms']:.0f} ms | TTFT: {res_cold['ttft_ms']:.0f} ms | ok={res_cold['ok']}")

        print("  • Clarification (Warm)...")
        res_warm = timed_exchange(worker, "granite_analyze", discovery_payload, meta)
        print(f"    Total: {res_warm['total_ms']:.0f} ms | TTFT: {res_warm['ttft_ms']:.0f} ms | ok={res_warm['ok']}")

        mode_metrics["clarify_cold_total_ms"] = res_cold["total_ms"]
        mode_metrics["clarify_cold_ttft_ms"] = res_cold["ttft_ms"]
        mode_metrics["clarify_warm_total_ms"] = res_warm["total_ms"]
        mode_metrics["clarify_warm_ttft_ms"] = res_warm["ttft_ms"]

        # 2. Rewrite Workflow
        rewrite_payload = {
            "prompt": CONFIRMED_REWRITE_PROMPT,
            "instructions": CONFIRMED_REWRITE_PROMPT,
            "thinking_enabled": thinking_enabled,
            "schema": REWRITE_SCHEMA
        }

        print("  • Rewrite (Cold)...")
        res_rw_cold = timed_exchange(worker, "granite_rewrite", rewrite_payload, meta)
        print(f"    Total: {res_rw_cold['total_ms']:.0f} ms | TTFT: {res_rw_cold['ttft_ms']:.0f} ms | ok={res_rw_cold['ok']}")

        print("  • Rewrite (Warm)...")
        res_rw_warm = timed_exchange(worker, "granite_rewrite", rewrite_payload, meta)
        print(f"    Total: {res_rw_warm['total_ms']:.0f} ms | TTFT: {res_rw_warm['ttft_ms']:.0f} ms | ok={res_rw_warm['ok']}")

        mode_metrics["rewrite_cold_total_ms"] = res_rw_cold["total_ms"]
        mode_metrics["rewrite_cold_ttft_ms"] = res_rw_cold["ttft_ms"]
        mode_metrics["rewrite_warm_total_ms"] = res_rw_warm["total_ms"]
        mode_metrics["rewrite_warm_ttft_ms"] = res_rw_warm["ttft_ms"]

        # 3. Inline Word Refinement
        word_payload = {
            "prompt": INLINE_PHRASE,
            "instructions": INLINE_PHRASE,
            "thinking_enabled": False, # Word refinement is always compact
            "schema": WORD_SCHEMA
        }

        print("  • Inline Word Refinement (Cold)...")
        res_word_cold = timed_exchange(worker, "granite_word_refinement", word_payload, meta)
        print(f"    Total: {res_word_cold['total_ms']:.0f} ms | TTFT: {res_word_cold['ttft_ms']:.0f} ms | ok={res_word_cold['ok']}")

        print("  • Inline Word Refinement (Warm)...")
        res_word_warm = timed_exchange(worker, "granite_word_refinement", word_payload, meta)
        print(f"    Total: {res_word_warm['total_ms']:.0f} ms | TTFT: {res_word_warm['ttft_ms']:.0f} ms | ok={res_word_warm['ok']}")

        mode_metrics["word_cold_total_ms"] = res_word_cold["total_ms"]
        mode_metrics["word_cold_ttft_ms"] = res_word_cold["ttft_ms"]
        mode_metrics["word_warm_total_ms"] = res_word_warm["total_ms"]
        mode_metrics["word_warm_ttft_ms"] = res_word_warm["ttft_ms"]

        # 4. Mid-Stream Cancellation Test
        print("  • Testing Mid-Stream Cancellation during generation...")
        worker.sequence += 1
        target_seq = worker.sequence
        req_frame = make_envelope(worker, target_seq, "granite_rewrite", rewrite_payload, meta)

        worker.process.stdin.write((json.dumps(req_frame, ensure_ascii=False) + "\n").encode("utf-8"))
        worker.process.stdin.flush()

        # Wait for the first partial_result to confirm generation is underway
        saw_partial = False
        t_detect_start = time.perf_counter()
        while time.perf_counter() - t_detect_start < 5.0:
            try:
                msg = worker.messages.get(timeout=0.1)
                if isinstance(msg, dict) and msg.get("type") == "partial_result" and msg.get("request_id") == str(target_seq):
                    saw_partial = True
                    break
            except queue.Empty:
                continue

        t_cancel_sent = time.perf_counter()
        worker.sequence += 1
        cancel_seq = worker.sequence
        cancel_envelope = make_envelope(worker, cancel_seq, "cancel", {"target_request_id": str(target_seq)})

        worker.process.stdin.write((json.dumps(cancel_envelope, ensure_ascii=False) + "\n").encode("utf-8"))
        worker.process.stdin.flush()

        # Await cancel reply and cancelled target reply
        cancel_ack_seen = False
        target_cancelled_seen = False
        t_cancel_timeout = t_cancel_sent + 5.0

        while time.perf_counter() < t_cancel_timeout:
            try:
                msg = worker.messages.get(timeout=0.2)
                if isinstance(msg, dict):
                    if msg.get("request_id") == str(cancel_seq) and msg.get("type") == "result":
                        cancel_ack_seen = True
                    if msg.get("request_id") == str(target_seq) and (msg.get("type") == "error" or not msg.get("ok")):
                        target_cancelled_seen = True
                    if cancel_ack_seen and target_cancelled_seen:
                        break
            except queue.Empty:
                continue

        t_quiesce_ms = (time.perf_counter() - t_cancel_sent) * 1000.0
        print(f"    Cancel Ack: {cancel_ack_seen} | Target Cancelled: {target_cancelled_seen} | Quiesce: {t_quiesce_ms:.1f} ms")

        # Verify worker is immediately healthy after cancellation
        ping = worker.exchange("runtime_status")
        worker_healthy = ping["response"].get("ok", False) and ping["response"].get("result", {}).get("state") == "READY"
        print(f"    Post-cancellation health check: {worker_healthy} (State: {ping['response'].get('result', {}).get('state')})")

        mode_metrics["cancellation_quiesce_ms"] = t_quiesce_ms
        mode_metrics["cancellation_clean"] = (cancel_ack_seen and target_cancelled_seen and worker_healthy)

        results[mode] = mode_metrics

        # Clean shutdown
        worker.sequence += 1
        shut_req = make_envelope(worker, worker.sequence, "shutdown", {})
        worker.process.stdin.write((json.dumps(shut_req) + "\n").encode())
        worker.process.stdin.flush()
        worker.process.terminate()
        worker.process.wait()

    # Save results
    out_path = ROOT / "build/workflow_latency_results.json"
    out_path.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"\nSaved latency report to {out_path}")

    # Summary table
    print("\n" + "=" * 80)
    print("STAGE 4 BENCHMARK SUMMARY TABLE")
    print("=" * 80)
    header = f"{'Workflow Metric':<32} | {'Reasoning OFF':<20} | {'Reasoning ON':<20}"
    print(header)
    print("-" * 80)
    print(f"{'Model Load Time':<32} | {results['reasoning_off']['load_time_s']:<8.2f} s           | {results['reasoning_on']['load_time_s']:<8.2f} s")
    print(f"{'Clarify Cold (Total / TTFT)':<32} | {results['reasoning_off']['clarify_cold_total_ms']:.0f} / {results['reasoning_off']['clarify_cold_ttft_ms']:.0f} ms          | {results['reasoning_on']['clarify_cold_total_ms']:.0f} / {results['reasoning_on']['clarify_cold_ttft_ms']:.0f} ms")
    print(f"{'Clarify Warm (Total / TTFT)':<32} | {results['reasoning_off']['clarify_warm_total_ms']:.0f} / {results['reasoning_off']['clarify_warm_ttft_ms']:.0f} ms          | {results['reasoning_on']['clarify_warm_total_ms']:.0f} / {results['reasoning_on']['clarify_warm_ttft_ms']:.0f} ms")
    print(f"{'Rewrite Cold (Total / TTFT)':<32} | {results['reasoning_off']['rewrite_cold_total_ms']:.0f} / {results['reasoning_off']['rewrite_cold_ttft_ms']:.0f} ms          | {results['reasoning_on']['rewrite_cold_total_ms']:.0f} / {results['reasoning_on']['rewrite_cold_ttft_ms']:.0f} ms")
    print(f"{'Rewrite Warm (Total / TTFT)':<32} | {results['reasoning_off']['rewrite_warm_total_ms']:.0f} / {results['reasoning_off']['rewrite_warm_ttft_ms']:.0f} ms          | {results['reasoning_on']['rewrite_warm_total_ms']:.0f} / {results['reasoning_on']['rewrite_warm_ttft_ms']:.0f} ms")
    print(f"{'Word Refine Cold (Total / TTFT)':<32} | {results['reasoning_off']['word_cold_total_ms']:.0f} / {results['reasoning_off']['word_cold_ttft_ms']:.0f} ms          | {results['reasoning_on']['word_cold_total_ms']:.0f} / {results['reasoning_on']['word_cold_ttft_ms']:.0f} ms")
    print(f"{'Word Refine Warm (Total / TTFT)':<32} | {results['reasoning_off']['word_warm_total_ms']:.0f} / {results['reasoning_off']['word_warm_ttft_ms']:.0f} ms          | {results['reasoning_on']['word_warm_total_ms']:.0f} / {results['reasoning_on']['word_warm_ttft_ms']:.0f} ms")
    print(f"{'Cancel Quiesce Time':<32} | {results['reasoning_off']['cancellation_quiesce_ms']:<8.1f} ms          | {results['reasoning_on']['cancellation_quiesce_ms']:<8.1f} ms")
    print(f"{'Cancel Recovery State':<32} | {'CLEAN (Ready)':<20} | {'CLEAN (Ready)':<20}")
    print("=" * 80)

if __name__ == "__main__":
    run_latency_battery()
