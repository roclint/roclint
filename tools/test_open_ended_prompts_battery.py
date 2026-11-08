#!/usr/bin/env python3
"""
Comprehensive battery test for open-ended prompts across:
1. S1 Goal Escalation (Host AST open-ended detection)
2. IBM Granite 3B Q8_0 Discovery Inference
3. C++ MaterialityPolicyV1 Policy Verdict & Span Collision Check
4. Choice Group Inspection (questions, alternatives, agent discretion)
"""

import subprocess
import json
import time
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKER_BIN = str(ROOT / "build" / "roclint_granite_worker")
CONTRACTS_BIN = str(ROOT / "build" / "roclint_granite_contracts")
BUILDER_BIN = "/Users/jesse/.gemini/antigravity/brain/8b9565d1-f062-445f-b9f2-18f892ffa8b4/scratch/roclint_prompt_builder"
MODEL_DIR = "/Users/jesse/roclint_notes/2026-10-31-final-mac-143729/recovery/dependencies/models-s2"

PROMPTS = [
    {
        "id": "PERF-1",
        "category": "performance",
        "prompt": "make my website more faster",
        "desc": "User's exact open-ended speed prompt"
    },
    {
        "id": "PERF-2",
        "category": "performance",
        "prompt": "speed up the website",
        "desc": "Short generic speed prompt"
    },
    {
        "id": "PERF-3",
        "category": "performance",
        "prompt": "improve loading performance of the site",
        "desc": "Loading performance goal"
    },
    {
        "id": "PERF-4",
        "category": "performance",
        "prompt": "optimize website performance",
        "desc": "Unbounded optimization goal"
    },
    {
        "id": "PERF-5",
        "category": "performance",
        "prompt": "Make the application much faster and more responsive.",
        "desc": "Multi-word responsiveness goal"
    },
    {
        "id": "AESTH-1",
        "category": "aesthetic",
        "prompt": "Make the dashboard look more modern and clean.",
        "desc": "Modern dashboard aesthetic"
    },
    {
        "id": "AESTH-2",
        "category": "aesthetic",
        "prompt": "Make the landing page feel like an 80s retro synthwave arcade game.",
        "desc": "Synthwave retro arcade aesthetic"
    },
    {
        "id": "AESTH-3",
        "category": "aesthetic",
        "prompt": "Give the checkout page an elegant, high-end boutique atmosphere.",
        "desc": "Boutique checkout styling"
    },
    {
        "id": "AESTH-4",
        "category": "aesthetic",
        "prompt": "Redesign the profile card with a sleek futuristic aesthetic.",
        "desc": "Futuristic UI redesign"
    },
    {
        "id": "ARCH-1",
        "category": "architecture",
        "prompt": "Refactor the authentication flow for better maintainability.",
        "desc": "Architectural maintainability refactor"
    }
]

def get_discovery_instructions(prompt_str):
    p = subprocess.run([BUILDER_BIN, "discovery", prompt_str], capture_output=True, text=True)
    return p.stdout

def main():
    print("=" * 80)
    print("ROCLINT OPEN-ENDED PROMPT BATTERY TEST")
    print(f"Target Model: IBM Granite 4.2 3B Q8_0 ({WORKER_BIN})")
    print(f"Evaluating {len(PROMPTS)} open-ended prompts across S1, Granite S2, and MaterialityPolicyV1")
    print("=" * 80)

    # 1. Start Granite worker
    print("\n[1/3] Initializing IBM Granite 3B resident worker...")
    proc = subprocess.Popen(
        [WORKER_BIN, "--model-dir", MODEL_DIR, "--quantization", "Q8_0"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1
    )

    manifest_file = ROOT / "build" / "generated" / "GraniteConfig.hpp"
    config_id = "granite.metal.ctx8192.threads4.gpu99.t1.p095.s42.jinja.json0ws.rejection.review_polish.v8"
    if manifest_file.exists():
        import re
        m = re.search(r'"configuration_id"\s*:\s*"([^"]+)"', manifest_file.read_text())
        if m:
            config_id = m.group(1)

    seq = 0
    def send_recv(op, payload=None, extra=None):
        nonlocal seq
        seq += 1
        payload = payload or {}
        extra = extra or {}
        req = {
            "protocol_version": 1,
            "type": "request",
            "request_id": str(seq),
            "op": op,
            "request_group_id": f"grp_{seq}",
            "attempt_id": f"att_{seq}",
            "session_id": "sess_battery",
            "draft_revision_id": "rev_battery",
            "state_version": 1,
            "provider": "granite_local",
            "model_id": "ibm-granite/granite-4.2-3b-GGUF",
            "model_revision": "c40945d71cd90f249a56985e8155551a9188dc30",
            "runtime_revision": "da2ee1e7a640445fc562fe5529598cee1917e0f5",
            "configuration_id": config_id,
            "quantization": "Q8_0",
            "weights_sha256": "fbe986738041418e26de9e123ba740cb654931f85bf572a71bd01f9e6b85e53d",
            "input_hash": hashlib.sha256(payload.get("prompt", "").encode("utf-8")).hexdigest(),
            "payload": payload
        }
        req.update(extra)
        proc.stdin.write(json.dumps(req) + "\n")
        proc.stdin.flush()
        while True:
            line = proc.stdout.readline()
            if not line:
                return None
            data = json.loads(line)
            if data.get("type") in ("result", "error", "hello_ack", "runtime_status"):
                return data

    send_recv("hello")
    t0 = time.time()
    send_recv("load")
    print(f"Worker ready in {time.time()-t0:.2f}s.")

    schema = json.dumps({
        "type": "object",
        "properties": {
            "status": {"type": "string", "enum": ["options_generated", "no_ambiguity", "needs_clarification"]},
            "choice_groups": {"type": "array", "maxItems": 2, "items": {
                "type": "object",
                "properties": {
                    "dimension": {"type": "string"}, "question": {"type": "string"}, "material": {"type": "boolean"},
                    "alternatives": {"type": "array", "minItems": 2, "maxItems": 3, "items": {
                        "type": "object",
                        "properties": {
                            "label": {"type": "string"}, "why_material": {"type": "string"},
                            "evidence": {"type": "array", "minItems": 1, "items": {"type": "string"}}
                        },
                        "required": ["label", "why_material", "evidence"]
                    }}
                },
                "required": ["dimension", "question", "material", "alternatives"]
            }},
            "assumptions": {"type": "array", "items": {
                "type": "object",
                "properties": {
                    "statement": {"type": "string"}, "material": {"type": "boolean"}, "evidence": {"type": "array", "items": {"type": "string"}}
                },
                "required": ["statement", "material", "evidence"]
            }},
            "missing_decisions": {"type": "array", "items": {
                "type": "object",
                "properties": {"question": {"type": "string"}, "blocking": {"type": "boolean"}},
                "required": ["question", "blocking"]
            }},
            "conflicts": {"type": "array", "items": {
                "type": "object",
                "properties": {"description": {"type": "string"}, "blocking": {"type": "boolean"}},
                "required": ["description", "blocking"]
            }},
            "clarification_question": {"type": "string"}
        },
        "required": ["status", "choice_groups", "assumptions", "missing_decisions", "conflicts", "clarification_question"]
    })

    # 2. Run inference on each prompt
    print("\n[2/3] Running Granite S2 discovery on all test prompts...")
    cases_for_cxx = []
    responses = {}

    for i, test in enumerate(PROMPTS, 1):
        pid = test["id"]
        prompt = test["prompt"]
        desc = test["desc"]
        print(f"  [{i:02d}/{len(PROMPTS):02d}] Prompt: \"{prompt}\" ({desc})")

        instr = get_discovery_instructions(prompt)
        t_start = time.time()
        res = send_recv("granite_analyze", {"prompt": prompt, "instructions": instr, "schema": schema})
        dt_ms = (time.time() - t_start) * 1000.0

        raw_text = res.get("result", {}).get("text", "{}") if res else "{}"
        parsed_json = {}
        try:
            parsed_json = json.loads(raw_text)
        except Exception:
            pass

        responses[pid] = {
            "test": test,
            "latency_ms": dt_ms,
            "raw_text": raw_text,
            "parsed": parsed_json,
            "worker_ok": res.get("ok", False) if res else False
        }

        cases_for_cxx.append({
            "id": pid,
            "op": "granite_analyze",
            "prompt": prompt,
            "response": {
                "ok": True,
                "result": {"text": raw_text}
            }
        })

    proc.terminate()

    # 3. Validate each response against compiled C++ MaterialityPolicyV1
    print("\n[3/3] Evaluating C++ MaterialityPolicyV1 & HUD Eligibility...")
    report_input = {
        "runs": [{
            "quantization": "Q8_0",
            "cases": cases_for_cxx
        }]
    }

    report_path = ROOT / "build" / "temp_battery_report.json"
    with open(report_path, "w") as f:
        json.dump(report_input, f)

    p_val = subprocess.run([CONTRACTS_BIN, "--validate", str(report_path)], capture_output=True, text=True)
    report_path.unlink(missing_ok=True)

    cxx_evals = {}
    if p_val.returncode == 0:
        val_results = json.loads(p_val.stdout)
        for vr in val_results:
            cxx_evals[vr["id"]] = vr
    else:
        print(f"ERROR calling granite_contracts: {p_val.stderr}")

    # 4. Print Summary Table
    print("\n" + "=" * 95)
    print(f"{'ID':<8} | {'Status':<18} | {'Policy Verdict':<20} | {'Groups':<6} | {'Overlap?':<8} | {'Agent Option?':<13} | {'Latency':<8}")
    print("-" * 95)

    all_eligible = True
    for test in PROMPTS:
        pid = test["id"]
        resp = responses[pid]
        parsed = resp["parsed"]
        cxx = cxx_evals.get(pid, {})

        status = parsed.get("status", "error")
        policy_verdict = cxx.get("policy_verdict", "UNKNOWN")
        policy_reason = cxx.get("policy_reason", "UNKNOWN")
        groups = parsed.get("choice_groups", [])
        latency = f"{resp['latency_ms']:.0f}ms"

        # Check if 3rd alternative ("Let agent decide...") is present
        has_agent_option = False
        for g in groups:
            for alt in g.get("alternatives", []):
                label = alt.get("label", "").lower()
                if "agent" in label or "workspace" in label or "conventions" in label:
                    has_agent_option = True

        overlap_str = "COLLISION" if policy_reason == "UNSAFE_SPAN_OVERLAP" else "Clean"
        is_eligible = (policy_verdict == "ELIGIBLE_FOR_CHOICE")
        if not is_eligible:
            all_eligible = False

        print(f"{pid:<8} | {status:<18} | {policy_verdict:<20} | {len(groups):<6} | {overlap_str:<8} | {str(has_agent_option):<13} | {latency:<8}")

        # Print details for each group
        for g_idx, g in enumerate(groups, 1):
            dim = g.get("dimension", "")
            q = g.get("question", "")
            print(f"         Group {g_idx} [{dim}]: \"{q}\"")
            for alt in g.get("alternatives", []):
                lbl = alt.get("label", "")
                ev = alt.get("evidence", [""])[0]
                print(f"           - {lbl} (evidence: \"{ev}\")")

    print("=" * 95)
    if all_eligible:
        print("RESULT: ALL 10 OPEN-ENDED PROMPTS ARE 100% ELIGIBLE FOR CHOICE (NO UNSAFE_SPAN_OVERLAP)!")
    else:
        print("RESULT: SOME PROMPTS WERE NOT ELIGIBLE FOR CHOICE.")
    print("=" * 95)

if __name__ == "__main__":
    main()
