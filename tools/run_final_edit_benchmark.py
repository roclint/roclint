#!/usr/bin/env python3
"""Evaluates IBM Granite 3B on the 100-case Final-Edit Review benchmark suite."""
import argparse
import datetime
import json
import os
import platform
import statistics
import sys
import time
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from check_granite_runtime import Worker, find_config, find_default_granite_model_dir

FIXTURES = ROOT / "tests/e2e/fixtures/benchmark/final-edit-review-cases.json"

SCHEMA = {
    "type": "object",
    "properties": {
        "verdict": {
            "type": "string",
            "enum": ["PASS", "DEFECT", "UNCERTAIN"]
        },
        "defects": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "dimension": {
                        "type": "string",
                        "enum": [
                            "answer_not_incorporated",
                            "stale_decision",
                            "lost_constraint",
                            "unsupported_addition",
                            "semantic_contradiction"
                        ]
                    },
                    "description": {"type": "string"},
                    "original_quote": {"type": "string"},
                    "final_quote": {"type": "string"}
                },
                "required": ["dimension", "description", "original_quote", "final_quote"]
            }
        },
        "summary": {"type": "string"}
    },
    "required": ["verdict", "defects", "summary"]
}

SCHEMA_V2 = {
    "type": "object",
    "properties": {
        "verdict": {
            "type": "string",
            "enum": ["PASS", "DEFECT", "UNCERTAIN"]
        },
        "requirements": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "id": {"type": "string"},
                    "status": {
                        "type": "string",
                        "enum": ["PRESERVED", "RESOLVED", "LOST_OR_WEAKENED", "CONTRADICTED", "UNCERTAIN"]
                    },
                    "notes": {"type": "string"}
                },
                "required": ["id", "status"]
            }
        },
        "findings": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "dimension": {
                        "type": "string",
                        "enum": [
                            "answer_not_incorporated",
                            "stale_decision",
                            "lost_constraint",
                            "unsupported_addition",
                            "semantic_contradiction"
                        ]
                    },
                    "evidence": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "source": {
                                    "type": "string",
                                    "enum": [
                                        "original_prompt",
                                        "developer_answer",
                                        "clarification_question",
                                        "final_prompt",
                                        "authorized_edit_plan"
                                    ]
                                },
                                "quote": {"type": "string"}
                            },
                            "required": ["source", "quote"]
                        }
                    },
                    "explanation": {"type": "string"}
                },
                "required": ["dimension", "evidence", "explanation"]
            }
        },
        "summary": {"type": "string"}
    },
    "required": ["verdict", "requirements", "findings", "summary"]
}

def xml_escape(text: str) -> str:
    return (text.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;")
                .replace('"', "&quot;")
                .replace("'", "&apos;"))

def build_ledger_items(original_prompt: str, clarification_question: str, developer_answer: str) -> list:
    import re
    raw_pieces = re.split(r'[;,\.\n]+', original_prompt)
    clauses = [p.strip() for p in raw_pieces if p.strip()]
    if not clauses and original_prompt.strip():
        clauses = [original_prompt.strip()]

    def is_placeholder(s: str) -> bool:
        low = s.lower()
        return any(k in low for k in ["[tbd", "tbd", "todo", "fixme", "placeholder", "whatever", "decide later", "as needed", "to be determined"])

    ledger = []
    for idx, c in enumerate(clauses, 1):
        treatment = "Replace" if is_placeholder(c) else "Preserve"
        ledger.append({
            "id": f"req_{idx}",
            "source": "Original prompt",
            "expectedTreatment": treatment,
            "instruction": c
        })

    if developer_answer.strip():
        ledger.append({
            "id": "decision_1",
            "source": "Developer answer",
            "expectedTreatment": "Incorporate",
            "instruction": developer_answer.strip()
        })
    return ledger

def build_review_prompt_v2(case: dict) -> str:
    ledger = build_ledger_items(case["original_prompt"], case["clarification_question"], case["developer_answer"])
    prompt = ("Instruction contract: granite.final_edit_review.v2\n"
              "You are a software engineering requirement reviewer inspecting an automated edit to a prompt.\n"
              "The developer was asked a clarification question and provided an explicit answer.\n"
              "The automated tool constructed a proposed final prompt incorporating that answer.\n"
              "Your task is to review the proposed final prompt against the original prompt and developer answer using the provided Requirements Ledger.\n\n"
              "Evaluate across these 5 defect dimensions:\n"
              "1. answer_not_incorporated: The proposed final prompt failed to incorporate the developer's answer.\n"
              "2. stale_decision: The proposed final prompt retains placeholder, ambiguous, or unresolved wording that the answer was supposed to resolve.\n"
              "3. lost_constraint: The proposed final prompt omitted or weakened unrelated requirements, numbers, negations, or constraints from the original prompt.\n"
              "4. unsupported_addition: The proposed final prompt added requirements, technologies, or rules never requested or authorized by the developer.\n"
              "5. semantic_contradiction: The proposed final prompt contains incompatible requirements. NOTE: Mutually exclusive conditions (if-else logic, disjoint scopes) are valid conditional branches, NOT contradictions.\n\n"
              "Requirements Ledger Evaluation Rules:\n"
              "For each item in the Requirements Ledger, evaluate its status:\n"
              "- \"PRESERVED\": Requirement is faithfully maintained in the proposed final prompt.\n"
              "- \"RESOLVED\": Superseded placeholder or outdated constraint was cleanly resolved or replaced.\n"
              "- \"LOST_OR_WEAKENED\": A requirement marked 'Preserve' was omitted, relaxed, or negated.\n"
              "- \"CONTRADICTED\": A requirement is contradicted by another instruction.\n"
              "- \"UNCERTAIN\": Status cannot be verified with certainty.\n\n"
              "Evidence Grounding Rules:\n"
              "- For EVERY defect finding, you MUST cite explicit evidence with an exact literal quote from its declared source:\n"
              "  * \"original_prompt\": Quote must appear verbatim in the original prompt.\n"
              "  * \"developer_answer\": Quote must appear verbatim in the developer answer.\n"
              "  * \"clarification_question\": Quote must appear verbatim in the clarification question.\n"
              "  * \"final_prompt\": Quote must appear verbatim in the proposed final prompt.\n"
              "  * \"authorized_edit_plan\": Quote must appear verbatim in the edit summary.\n"
              "- Do NOT alter, summarize, or fabricate quotes. Unverified quotes will cause the review to be rejected.\n"
              "- If any requirement is LOST_OR_WEAKENED or CONTRADICTED, or if defects exist, verdict MUST be \"DEFECT\".\n"
              "- If all requirements are PRESERVED or RESOLVED and no defects exist, verdict MUST be \"PASS\".\n"
              "- If you cannot determine with certainty, return verdict \"UNCERTAIN\".\n\n"
              "Reply with exactly one JSON object following this schema:\n"
              "{\"verdict\": \"PASS\" | \"DEFECT\" | \"UNCERTAIN\", \"requirements\": [{\"id\": string, \"status\": \"PRESERVED\" | \"RESOLVED\" | \"LOST_OR_WEAKENED\" | \"CONTRADICTED\" | \"UNCERTAIN\", \"notes\": string}], \"findings\": [{\"dimension\": string, \"evidence\": [{\"source\": string, \"quote\": string}], \"explanation\": string}], \"summary\": string}\n\n"
              "Input Data:\n"
              f"<original_prompt>{xml_escape(case['original_prompt'])}</original_prompt>\n"
              f"<clarification_question>{xml_escape(case['clarification_question'])}</clarification_question>\n"
              f"<developer_answer>{xml_escape(case['developer_answer'])}</developer_answer>\n"
              f"<proposed_final_prompt>{xml_escape(case['proposed_final_prompt'])}</proposed_final_prompt>\n")
    if case.get("edit_summary"):
        prompt += f"<edit_summary>{xml_escape(case['edit_summary'])}</edit_summary>\n"
    if ledger:
        prompt += "<requirements_ledger>\n"
        for item in ledger:
            prompt += f"  - id: {item['id']} | source: {item['source']} | expected: {item['expectedTreatment']} | text: {xml_escape(item['instruction'])}\n"
        prompt += "</requirements_ledger>\n"
    return prompt

def validate_and_ground_review_v2(raw_output: str, case: dict) -> dict:
    stripped = raw_output.strip()
    if stripped.startswith("```"):
        lines = stripped.split("\n")
        if lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        stripped = "\n".join(lines).strip()

    try:
        data = json.loads(stripped)
    except Exception as e:
        return {
            "verdict": "UNCERTAIN",
            "findings": [],
            "requirements": [],
            "summary": "Malformed JSON output",
            "evidence_validated": False,
            "grounding_error": str(e)
        }

    verdict = data.get("verdict", "UNCERTAIN")
    summary = data.get("summary", "")
    requirements = data.get("requirements", [])
    findings = data.get("findings", [])
    if not findings and "defects" in data:
        findings = []
        for d in data["defects"]:
            ev = []
            if d.get("original_quote"):
                ev.append({"source": "original_prompt", "quote": d["original_quote"]})
            if d.get("final_quote"):
                ev.append({"source": "final_prompt", "quote": d["final_quote"]})
            findings.append({
                "dimension": d.get("dimension", "unknown"),
                "explanation": d.get("description", ""),
                "evidence": ev
            })

    has_violated_req = any(r.get("status") in ("LOST_OR_WEAKENED", "CONTRADICTED") for r in requirements)

    all_grounded = True
    grounding_error = ""
    grounded_findings = []

    def strip_quotes(s: str) -> str:
        s = s.strip()
        if len(s) >= 2 and ((s[0] == '"' and s[-1] == '"') or (s[0] == "'" and s[-1] == "'")):
            return s[1:-1].strip()
        return s

    source_map = {
        "original_prompt": case.get("original_prompt", ""),
        "developer_answer": case.get("developer_answer", ""),
        "clarification_question": case.get("clarification_question", ""),
        "final_prompt": case.get("proposed_final_prompt", ""),
        "authorized_edit_plan": case.get("edit_summary", "")
    }

    for f in findings:
        ev_list = f.get("evidence", [])
        if not ev_list:
            all_grounded = False
            if not grounding_error:
                grounding_error = "finding missing evidence"
            grounded_findings.append({**f, "evidence_valid": False})
            continue

        f_valid = True
        grounded_ev = []
        for ev in ev_list:
            src = ev.get("source", "unknown")
            q = strip_quotes(ev.get("quote", ""))
            ok = False
            if src in source_map:
                ok = bool(q and q in source_map[src])
            else:
                for s_key, text in source_map.items():
                    if q and q in text:
                        src = s_key
                        ok = True
                        break
            if not ok:
                f_valid = False
                all_grounded = False
                if not grounding_error:
                    grounding_error = f"quote '{q}' not in declared source '{src}'"
            grounded_ev.append({"source": src, "quote": q, "verified": ok})

        grounded_findings.append({
            "dimension": f.get("dimension", "unknown"),
            "explanation": f.get("explanation", ""),
            "evidence": grounded_ev,
            "evidence_valid": f_valid
        })

    if verdict == "PASS":
        if findings or has_violated_req:
            return {
                "verdict": "UNCERTAIN",
                "findings": grounded_findings,
                "requirements": requirements,
                "summary": summary,
                "evidence_validated": False,
                "grounding_error": "PASS verdict contains findings or lost requirements"
            }
        return {
            "verdict": "PASS",
            "findings": [],
            "requirements": requirements,
            "summary": summary,
            "evidence_validated": True,
            "grounding_error": ""
        }

    if verdict == "DEFECT":
        if not findings and not has_violated_req:
            return {
                "verdict": "UNCERTAIN",
                "findings": [],
                "requirements": requirements,
                "summary": summary,
                "evidence_validated": False,
                "grounding_error": "DEFECT verdict contains no findings or violated requirements"
            }
        if not all_grounded:
            return {
                "verdict": "UNCERTAIN",
                "findings": grounded_findings,
                "requirements": requirements,
                "summary": summary,
                "evidence_validated": False,
                "grounding_error": f"unverified_evidence: {grounding_error}"
            }
        return {
            "verdict": "DEFECT",
            "findings": grounded_findings,
            "requirements": requirements,
            "summary": summary,
            "evidence_validated": True,
            "grounding_error": ""
        }

    return {
        "verdict": "UNCERTAIN",
        "findings": grounded_findings,
        "requirements": requirements,
        "summary": summary,
        "evidence_validated": True,
        "grounding_error": ""
    }

def build_review_prompt(case: dict) -> str:
    prompt = ("Instruction contract: granite.final_edit_review.v1\n"
              "You are a software engineering requirement reviewer inspecting an automated edit to a prompt.\n"
              "The developer was asked a clarification question and provided an explicit answer.\n"
              "The automated tool constructed a proposed final prompt incorporating that answer.\n"
              "Your task is to review the proposed final prompt against the original prompt and developer answer.\n\n"
              "Evaluate across these 5 dimensions:\n"
              "1. answer_not_incorporated: The proposed final prompt failed to incorporate the developer's answer.\n"
              "2. stale_decision: The proposed final prompt retains placeholder or unresolved wording that the answer was supposed to resolve.\n"
              "3. lost_constraint: The proposed final prompt omitted or weakened unrelated requirements or constraints from the original prompt.\n"
              "4. unsupported_addition: The proposed final prompt added requirements, technologies, or rules never requested or authorized by the developer.\n"
              "5. semantic_contradiction: The proposed final prompt contains incompatible requirements. NOTE: Mutually exclusive conditions (if-else logic, disjoint scopes) are valid conditional branches, NOT contradictions.\n\n"
              "Rules:\n"
              "- If the proposed final prompt faithfully preserves requirements, incorporates the answer, and introduces no contradictions or additions, return verdict \"PASS\" and defects [].\n"
              "- If there are defects, return verdict \"DEFECT\" and list each defect item.\n"
              "- For each defect, you MUST provide an exact literal quote in original_quote (from original prompt) and/or final_quote (from proposed final prompt). Do not paraphrase quotes.\n"
              "- If you cannot determine with certainty, return verdict \"UNCERTAIN\".\n"
              "- Reply with exactly one JSON object following this schema:\n"
              "{\"verdict\": \"PASS\" | \"DEFECT\" | \"UNCERTAIN\", \"defects\": [{\"dimension\": string, \"description\": string, \"original_quote\": string, \"final_quote\": string}], \"summary\": string}\n\n"
              "Input Data:\n"
              f"<original_prompt>{xml_escape(case['original_prompt'])}</original_prompt>\n"
              f"<clarification_question>{xml_escape(case['clarification_question'])}</clarification_question>\n"
              f"<developer_answer>{xml_escape(case['developer_answer'])}</developer_answer>\n"
              f"<proposed_final_prompt>{xml_escape(case['proposed_final_prompt'])}</proposed_final_prompt>\n")
    if case.get("edit_summary"):
        prompt += f"<edit_summary>{xml_escape(case['edit_summary'])}</edit_summary>\n"
    return prompt

def validate_and_ground_review(raw_output: str, original_prompt: str, proposed_final: str) -> dict:
    stripped = raw_output.strip()
    if stripped.startswith("```"):
        lines = stripped.split("\n")
        if lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        stripped = "\n".join(lines).strip()

    try:
        data = json.loads(stripped)
    except Exception as e:
        return {
            "verdict": "UNCERTAIN",
            "defects": [],
            "summary": "Malformed JSON output",
            "quotes_validated": False,
            "grounding_error": str(e)
        }

    verdict = data.get("verdict", "UNCERTAIN")
    defects = data.get("defects", [])
    summary = data.get("summary", "")

    if verdict == "PASS":
        if defects:
            return {
                "verdict": "UNCERTAIN",
                "defects": defects,
                "summary": summary,
                "quotes_validated": False,
                "grounding_error": "PASS verdict contains defects"
            }
        return {
            "verdict": "PASS",
            "defects": [],
            "summary": summary,
            "quotes_validated": True,
            "grounding_error": ""
        }

    if verdict == "DEFECT":
        if not defects:
            return {
                "verdict": "UNCERTAIN",
                "defects": [],
                "summary": summary,
                "quotes_validated": False,
                "grounding_error": "DEFECT verdict contains no defects"
            }

        all_grounded = True
        grounding_error = ""
        grounded_defects = []

        for d in defects:
            orig_q = d.get("original_quote", "")
            final_q = d.get("final_quote", "")

            orig_ok = True
            if orig_q:
                orig_ok = (orig_q in original_prompt)
            final_ok = True
            if final_q:
                final_ok = (final_q in proposed_final)

            has_quote = bool(orig_q or final_q)

            if orig_ok and final_ok and has_quote:
                grounded_defects.append({**d, "quotes_grounded": True})
            else:
                grounded_defects.append({**d, "quotes_grounded": False})
                all_grounded = False
                if not grounding_error:
                    if not orig_ok:
                        grounding_error = f"original_quote not in text: '{orig_q}'"
                    elif not final_ok:
                        grounding_error = f"final_quote not in text: '{final_q}'"
                    else:
                        grounding_error = "defect missing quote evidence"

        if not all_grounded:
            # Strict Tricorder safety: downgrade ungrounded defect to UNCERTAIN
            return {
                "verdict": "UNCERTAIN",
                "defects": grounded_defects,
                "summary": summary,
                "quotes_validated": False,
                "grounding_error": f"unverified_quote_evidence: {grounding_error}"
            }
        return {
            "verdict": "DEFECT",
            "defects": grounded_defects,
            "summary": summary,
            "quotes_validated": True,
            "grounding_error": ""
        }

    return {
        "verdict": "UNCERTAIN",
        "defects": defects,
        "summary": summary,
        "quotes_validated": True,
        "grounding_error": ""
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, default=ROOT / "build/roclint_granite_worker")
    parser.add_argument("--model-dir", type=Path, default=find_default_granite_model_dir())
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=0, help="Limit number of cases (0 for all)")
    parser.add_argument("--offset", type=int, default=0, help="Offset start index in cases")
    parser.add_argument("--contract", choices=["v1", "v2"], default="v2", help="Review contract version")
    parser.add_argument("--fixtures", type=Path, default=FIXTURES, help="Path to evaluation fixtures JSON")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    with open(args.fixtures, "r") as f:
        cases_data = json.load(f)["cases"]

    if args.offset > 0:
        cases_data = cases_data[args.offset:]
    if args.limit > 0:
        cases_data = cases_data[:args.limit]

    print(f"Loaded {len(cases_data)} fixtures from {args.fixtures.name} (Contract: {args.contract})")
    schema_str = json.dumps(SCHEMA_V2 if args.contract == "v2" else SCHEMA)

    manifest = json.loads(find_config("granite-runtime-config.json").read_text(encoding="utf-8-sig"))
    worker_args = types.SimpleNamespace(
        worker=args.worker,
        model_dir=args.model_dir,
        diagnostic_no_deadlines=True,
        no_thinking_reference=True
    )

    print("Starting Granite worker process...")
    worker = Worker(worker_args, manifest, "Q8_0", os.environ.copy())

    try:
        print("Performing hello handshake...")
        h = worker.exchange("hello")
        if not h["response"].get("ok"):
            raise RuntimeError(f"Hello handshake failed: {h}")

        print("Loading local Granite 3B Q8_0 weights...")
        l = worker.exchange("load")
        if not l["response"].get("ok"):
            raise RuntimeError(f"Model load failed: {l}")
        print(f"Granite ready! (Load time: {l['elapsed_ms']/1000.0:.2f}s)\n")

        results = []
        latencies = []

        for idx, case in enumerate(cases_data, 1):
            if args.contract == "v2":
                prompt = build_review_prompt_v2(case)
            else:
                prompt = build_review_prompt(case)

            fixture_meta = {
                "intent_id": "explicit_developer_input",
                "rewrite_profile_id": "GENERIC@1"
            }
            payload = {
                "prompt": prompt,
                "instructions": prompt,
                "schema": schema_str,
                "thinking_enabled": False
            }

            res = worker.exchange("granite_rewrite", payload, fixture_meta)
            elapsed_ms = res["elapsed_ms"]
            latencies.append(elapsed_ms)
            resp = res["response"]

            if not resp.get("ok"):
                parsed = {
                    "verdict": "ERROR",
                    "defects": [],
                    "findings": [],
                    "requirements": [],
                    "summary": f"Worker error: {resp.get('error', 'unknown')}",
                    "quotes_validated": False,
                    "evidence_validated": False,
                    "grounding_error": "Worker error"
                }
            else:
                model_output = resp.get("result", {}).get("text", "")
                if args.contract == "v2":
                    parsed = validate_and_ground_review_v2(model_output, case)
                else:
                    parsed = validate_and_ground_review(
                        model_output,
                        case["original_prompt"],
                        case["proposed_final_prompt"]
                    )

            record = {
                "case_id": case["id"],
                "category": case["category"],
                "expected_verdict": case["expected_verdict"],
                "expected_dimension": case.get("expected_dimension"),
                "actual_verdict": parsed["verdict"],
                "evidence_validated": parsed.get("evidence_validated", parsed.get("quotes_validated", False)),
                "grounding_error": parsed.get("grounding_error", ""),
                "findings": parsed.get("findings", parsed.get("defects", [])),
                "requirements": parsed.get("requirements", []),
                "summary": parsed.get("summary", ""),
                "latency_ms": elapsed_ms,
                "input": case["original_prompt"]
            }
            results.append(record)

            # Incremental persistence
            with open(args.output / "records.jsonl", "a", encoding="utf-8") as rf:
                rf.write(json.dumps(record) + "\n")

            match_symbol = "✓" if parsed["verdict"] == case["expected_verdict"] else "✗"
            print(f"[{idx:3d}/{len(cases_data)}] {case['id']} | Exp: {case['expected_verdict']:6s} | Got: {parsed['verdict']:9s} {match_symbol} | {elapsed_ms/1000.0:4.2f}s | {parsed['summary'][:60]}", flush=True)

    finally:
        worker.close()

    # Metrics calculation
    tp = sum(1 for r in results if r["expected_verdict"] == "DEFECT" and r["actual_verdict"] == "DEFECT")
    fp = sum(1 for r in results if r["expected_verdict"] == "PASS" and r["actual_verdict"] == "DEFECT")
    tn = sum(1 for r in results if r["expected_verdict"] == "PASS" and r["actual_verdict"] == "PASS")
    fn = sum(1 for r in results if r["expected_verdict"] == "DEFECT" and r["actual_verdict"] == "PASS")
    un_defect = sum(1 for r in results if r["expected_verdict"] == "DEFECT" and r["actual_verdict"] == "UNCERTAIN")
    un_pass = sum(1 for r in results if r["expected_verdict"] == "PASS" and r["actual_verdict"] == "UNCERTAIN")

    total_pass = sum(1 for r in results if r["expected_verdict"] == "PASS")
    total_defect = sum(1 for r in results if r["expected_verdict"] == "DEFECT")

    sensitivity = (tp / total_defect) * 100.0 if total_defect else 0.0
    specificity = (tn / total_pass) * 100.0 if total_pass else 0.0
    false_alarm_rate = (fp / total_pass) * 100.0 if total_pass else 0.0

    dims = ["answer_not_incorporated", "stale_decision", "lost_constraint", "unsupported_addition", "semantic_contradiction"]
    dim_metrics = {}
    for dim in dims:
        dim_cases = [r for r in results if r.get("expected_dimension") == dim]
        dim_detected = sum(1 for r in dim_cases if r["actual_verdict"] == "DEFECT")
        dim_metrics[dim] = {
            "total": len(dim_cases),
            "detected": dim_detected,
            "detection_rate": (dim_detected / len(dim_cases) * 100.0) if dim_cases else 0.0
        }

    report = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "model": "IBM Granite 4.2 3B Q8_0 (Metal local)",
        "contract": f"granite.final_edit_review.{args.contract}",
        "total_cases": len(results),
        "confusion_matrix": {
            "true_positives": tp,
            "false_positives": fp,
            "true_negatives": tn,
            "false_negatives": fn,
            "uncertain_on_defect": un_defect,
            "uncertain_on_pass": un_pass
        },
        "rates": {
            "sensitivity_tpr": sensitivity,
            "specificity_tnr": specificity,
            "false_alarm_rate_fpr": false_alarm_rate,
            "tricorder_10pct_rule_passed": (false_alarm_rate <= 10.0)
        },
        "dimension_breakdown": dim_metrics,
        "latency_stats": {
            "mean_ms": statistics.mean(latencies) if latencies else 0.0,
            "median_ms": statistics.median(latencies) if latencies else 0.0,
            "p95_ms": statistics.quantiles(latencies, n=20)[18] if len(latencies) >= 20 else 0.0,
            "min_ms": min(latencies) if latencies else 0.0,
            "max_ms": max(latencies) if latencies else 0.0
        },
        "records": results
    }

    report_path = args.output / "final_edit_benchmark_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"\nSaved benchmark results to {report_path}")

    # Output executive scorecard
    print("\n" + "=" * 60)
    print("      ROCLINT PHASE 2: GRANITE FINAL-EDIT REVIEW SCORECARD")
    print("=" * 60)
    print(f"Total Cases Evaluated: {len(results)} (50 Valid PASS, 50 Defective DEFECT)")
    print("-" * 60)
    print(f"True Positives  (Defects Detected):  {tp:2d} / {total_defect} ({sensitivity:.1f}%)")
    print(f"False Positives (False Alarms):      {fp:2d} / {total_pass} ({false_alarm_rate:.1f}%) [Tricorder limit: <=10%]")
    print(f"True Negatives  (Preserved Valid):   {tn:2d} / {total_pass} ({specificity:.1f}%)")
    print(f"False Negatives (Missed Defects):    {fn:2d} / {total_defect}")
    print(f"Uncertain on Valid Cases:            {un_pass:2d} / {total_pass}")
    print(f"Uncertain on Defective Cases:        {un_defect:2d} / {total_defect}")
    print("-" * 60)
    print("DEFECT DETECTION BY DIMENSION:")
    for dim, ddata in dim_metrics.items():
        print(f"  • {dim:25s}: {ddata['detected']:2d} / {ddata['total']} ({ddata['detection_rate']:.1f}%)")
    print("-" * 60)
    print(f"LATENCY: Mean = {report['latency_stats']['mean_ms']/1000.0:.2f}s | Median = {report['latency_stats']['median_ms']/1000.0:.2f}s | p95 = {report['latency_stats']['p95_ms']/1000.0:.2f}s")
    print(f"TRICORDER 10% RULE: {'PASSED (FPR <= 10%)' if false_alarm_rate <= 10.0 else 'FAILED (FPR > 10%)'}")
    print("=" * 60 + "\n")

if __name__ == "__main__":
    main()
