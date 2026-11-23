"""Run 2026 Vibe Coding vs SWE Prompts through Roclint's Production S1 Triage Engine."""
import json
import os
import time
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parent.parent

def load_worker():
    spec_path = ROOT / "app" / "runtime" / "julia_worker.py"
    spec = importlib.util.spec_from_file_location("julia_worker", spec_path)
    worker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(worker)
    return worker

def main():
    worker = load_worker()
    model_dir = Path(os.getenv("ROCLINT_MODEL_DIR", str(ROOT / "models" / "granite-4.2-3b")))
    engine = worker.load(model_dir)

    test_cases = [
        # --- VIBE CODING PROMPTS ---
        {
            "id": "vibe_greenfield_invoice",
            "category": "Vibe Coding",
            "name": "Greenfield Invoice Tracker",
            "prompt": "Build a single-page web app for freelance designers to track invoices and milestones. "
                      "Stack: Next.js, Tailwind CSS, Lucide icons. "
                      "Vibe: Minimalist dark mode, Apple-like typography, obsidian grays with subtle violet accents. "
                      "Features: A visual Kanban board (Draft, Sent, Paid) and a quick-add modal that calculates running totals instantly."
        },
        {
            "id": "vibe_shift_bouncy",
            "category": "Vibe Coding",
            "name": "Vibe Shift / Bouncy & Organic",
            "prompt": "The app works, but it feels too corporate and rigid. Shift the vibe: "
                      "make it feel organic, playful, and bouncy. "
                      "Change the primary action button to sit in the bottom thumb zone with fluid spring animations. "
                      "Switch the color scheme from cold blue to warm cream and matcha green with soft pill borders."
        },
        {
            "id": "vibe_casual_bugfix",
            "category": "Vibe Coding",
            "name": "Casual Bug Fix (Missing Target & Null Check)",
            "prompt": "When I click 'Checkout', nothing happens and the console gives: "
                      "TypeError: Cannot read properties of undefined (reading 'cartId'). "
                      "Fix it and make it toast instead of crashing."
        },
        {
            "id": "vibe_vague_refactor",
            "category": "Vibe Coding",
            "name": "Underspecified Vibe Refactor",
            "prompt": "Clean up this module and make it faster."
        },

        # --- SWE (SOFTWARE ENGINEERING) PROMPTS ---
        {
            "id": "swe_postgres_partitioning",
            "category": "SWE Prompt",
            "name": "PostgreSQL Range Partitioning Migration",
            "prompt": "Review db/schema.prisma and generate an idempotent SQL migration to partition the events table "
                      "by range on created_at (monthly partitions). Ensure existing foreign keys and idx_events_org_id_created_at "
                      "are preserved, and write a safe backfill script that migrates rows in batches of 5000."
        },
        {
            "id": "swe_tdd_race_condition",
            "category": "SWE Prompt",
            "name": "TDD Auth Race Condition Fix",
            "prompt": "In src/auth/tokenManager.ts, fix the race condition during concurrent token refresh requests. "
                      "Write a reproducing test in tests/auth/tokenManager.spec.ts with 5 concurrent requests, "
                      "implement a single-flight promise lock, and verify npm test passes without adding third-party dependencies."
        },
        {
            "id": "swe_perf_sync_pool",
            "category": "SWE Prompt",
            "name": "Go sync.Pool Heap Allocation Optimization",
            "prompt": "Analyze the serialization routine in internal/serializer/buffer.go. "
                      "Profiling shows high heap allocation in EncodeRecord() due to repeated slice reallocations. "
                      "Refactor using sync.Pool to reuse byte buffers. Maintain the exact public interface signature "
                      "and run go test -benchmem -bench=. to verify."
        },
        {
            "id": "swe_security_audit",
            "category": "SWE Prompt",
            "name": "OWASP Security Audit on Upload Controller",
            "prompt": "Review src/controllers/upload.controller.ts against the OWASP Top 10: "
                      "identify potential path traversal, SSRF, and unvalidated MIME type risks in S3 presigned URL generation. "
                      "Implement strict schema validation with Zod, reject SVG uploads with embedded script tags, "
                      "and output findings as a markdown table."
        },

        # --- CONTROLS & BENCHMARKS ---
        {
            "id": "control_clear_exact",
            "category": "Control (Clear)",
            "name": "Clear Explicit Instruction",
            "prompt": "Replace the string 'v1' with 'v2' in config/version.json."
        },
        {
            "id": "control_conflict_rules",
            "category": "Control (Conflict)",
            "name": "Direct Rule Contradiction",
            "prompt": "Use exactly three bullet points. Use exactly five bullet points."
        }
    ]

    print("=" * 110)
    print("ROCLINT PRODUCTION S1 TRIAGE SUITE — TEST REPORT")
    print("Core Heads: ACTION | TARGET | RULE | RULE_CONFLICT")
    print("Production Routing: Ambiguity Score >= 0.70 -> [INTERCEPT HUD], <= 0.30 -> [PASS TO AGENT]")
    print("=" * 110)

    results = []
    for case in test_cases:
        t0 = time.perf_counter()
        req = {
            "triage_plan_version": 2,
            "prompt": case["prompt"]
        }
        res = worker.triage(engine, req)
        latency_ms = (time.perf_counter() - t0) * 1000

        cat_dict = {item["category"]: item["score"] for item in res["category_scores"]}
        max_ambiguity = max(cat_dict.values())
        
        # Identify the driving head(s)
        driving_heads = [k for k, v in cat_dict.items() if v >= 0.70]
        driving_str = ", ".join(driving_heads) if driving_heads else (max(cat_dict, key=cat_dict.get) if max_ambiguity > 0.30 else "NONE (CLEAR)")

        if max_ambiguity >= 0.70:
            decision = "INTERCEPT"
            hud_action = f"Trigger 2-Choice HUD ({driving_str})"
        elif max_ambiguity <= 0.30:
            decision = "PASS"
            hud_action = "Zero-Overhead Agent Forward ($0.00)"
        else:
            decision = "BORDERLINE"
            hud_action = f"Soft Warning ({driving_str})"

        results.append({
            "id": case["id"],
            "category": case["category"],
            "name": case["name"],
            "prompt": case["prompt"],
            "latency_ms": latency_ms,
            "max_ambiguity": max_ambiguity,
            "driving_heads": driving_str,
            "decision": decision,
            "hud_action": hud_action,
            "scores": cat_dict
        })

    fmt_header = f"{'Category':<15} | {'Prompt Case':<32} | {'ACTION':<7} {'TARGET':<7} {'RULE':<7} {'CONFL':<7} | {'MaxScore':<8} | {'Decision':<11} | {'Action / Driving Head'}"
    print(fmt_header)
    print("-" * 110)
    for r in results:
        s = r["scores"]
        row = (f"{r['category']:<15} | {r['name'][:32]:<32} | "
               f"{s.get('ACTION',0):.4f}  {s.get('TARGET',0):.4f}  {s.get('RULE',0):.4f}  {s.get('RULE_CONFLICT',0):.4f} | "
               f"{r['max_ambiguity']:.4f}   | {r['decision']:<11} | {r['hud_action']}")
        print(row)
    print("-" * 110)

    # Save to disk
    out_file = ROOT / "build" / "roclint_test_run.json"
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(json.dumps(results, indent=2))
    print(f"\nSaved test artifact to {out_file}")

if __name__ == "__main__":
    main()
