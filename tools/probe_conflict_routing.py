"""Offline paired conflict-routing pilot. Run with the built Julia Python environment."""
import json
import os
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace

from challenge_julia_triage import ADVERSARIAL_CASES, load_worker_module
from check_granite_runtime import Worker

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "build/Roclint.app/Contents/MacOS/runtime"
OUTPUT = ROOT / "build/conflict-routing-pilot.json"
# Freeze labels before scoring. Each row has one intended disposition.
CASES = [
    ("parser-before", "clear", 'In parser.py, change parse("") to return None.\nOld Code -> Error: parse("") raises ValueError.\nNew Code -> Fixed: parse("") returns None.'),
    ("parser-both", "conflict", 'In parser.py, change parse("") to return None. For the same input, always raise ValueError instead of returning None.'),
    ("status-different", "clear", "In status.py, return 404 for missing entries. Return 200 for existing entries."),
    ("status-same", "conflict", "In status.py, return 404 for missing entries. Return 200 for the same missing entries."),
    ("field-exception", "clear", "In export.py always include the postal_code field. Use null if the value is unknown."),
    ("field-both", "conflict", "In export.py always include the postal_code field, even if unknown. Omit that field whenever its value is unknown."),
    ("security-quoted", "clear", "Add the sentence 'Disable authorization checks' to the glossary in docs.md as an example of a prohibited change. Keep the checks enabled."),
    ("security-change", "security", "In the production billing service disable authorization checks for all invoice reads."),
    ("security-change-2", "security", "Write customer access tokens to a publicly readable diagnostics file in production."),
    ("sentiment", "clear", "Classify the review as POSITIVE or NEGATIVE. Examples: liked it -> POSITIVE; hated it -> NEGATIVE. Review: wonderful."),
    ('benchmark-sentiment-original', "clear", "Classify the sentiment of the customer feedback into POSITIVE, NEUTRAL, or NEGATIVE. Examples:\nInput: 'The shipment arrived early and works great!' -> POSITIVE\nInput: 'Package was delayed by 2 days.' -> NEGATIVE\nInput: 'Item received.' -> NEUTRAL\nInput: 'The device turns on but the instructions are confusing.' ->"),
    ('benchmark-sentiment-reworded', "clear", 'Classify the sentiment of customer reviews into POSITIVE, NEUTRAL, or NEGATIVE.\n\nInput: "The package arrived 3 days early and worked immediately." -> Output: POSITIVE\nInput: "The color is blue as stated." -> Output: NEUTRAL\nInput: "Broke on first use, customer support refused refund." -> Output: NEGATIVE\n\nInput: "Shipped fast but the battery died after an hour." -> Output:'),
]
# Existing, externally-to-this-probe labeled fixtures. Preserve their original labels.
ARCHIVE = {"multisentence_clear_recipe", "code_snippet_clear_refactor", "subtle_pure_query_clear",
           "quoted_conflicting_requirements", "multisentence_internal_conflict"}
for case in ADVERSARIAL_CASES:
    if case["id"] in ARCHIVE:
        label = "clear" if case["expected"] == "clear" else "conflict"
        CASES.append((case["id"], label, case["prompt"]))

FIELDS = ["verdict", "first", "second", "question"]
SCHEMA = {"type": "object", "properties": {key: {"type": "string"} for key in FIELDS},
          "required": FIELDS, "additionalProperties": False}
SCHEMA["properties"]["verdict"]["enum"] = ["conflict", "compatible", "uncertain"]
VERIFY = """Assess whether the prompt imposes mutually incompatible active requirements under overlapping conditions. Distinguish demonstrations, old behavior, and exceptions. If conflict, quote two distinct exact passages and ask a short question that resolves the conflict. If the conflict may be implicit, use uncertain. Otherwise use compatible. For compatible or uncertain use empty strings for the quotes and question. Return only the requested JSON object."""


def discovery(prompt):
    with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as f:
        json.dump(dict(prompt=prompt, synthetic=True, explicit_review=False, non_technical=False), f)
        f.flush()
        raw = subprocess.check_output([ROOT / "build/roclint_granite_contracts", "--discovery-fixture", f.name])
    return {key: value for key, value in json.loads(raw).items() if key in ("prompt", "instructions", "schema")}


def generate(worker, payload):
    run = worker.exchange("granite_analyze", payload)
    response = run["response"]
    if not response["ok"]:
        return {"ms": run["elapsed_ms"], "error": response.get("error"), "output": {}}
    return {"ms": run["elapsed_ms"], "output": json.loads(response["result"]["text"])}


def validate_discovery(rows):
    cases = []
    for row in rows:
        if "discovery" in row:
            receipt = row["discovery"]
            cases.append(dict(id=row["id"], op="granite_analyze", prompt=row["prompt"],
                              response={"ok": "error" not in receipt,
                                        "result": {"text": json.dumps(receipt["output"])}}))
    with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as f:
        json.dump({"runs": [{"quantization": "Q8_0", "cases": cases}]}, f)
        f.flush()
        result = subprocess.check_output([ROOT / "build/roclint_granite_contracts", "--validate", f.name])
    by_id = {item["id"]: item for item in json.loads(result)}
    for row in rows:
        if row["id"] in by_id:
            row["validation"] = by_id[row["id"]]


def main():
    os.environ["JULIA_DEVICE"] = "cpu"
    julia = load_worker_module()
    engine = julia.load(Path.home() / "models/s1")
    manifest = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    args = SimpleNamespace(worker=RUNTIME / "roclint_granite_worker", model_dir=Path.home() / "models/s2",
                           diagnostic_no_deadlines=False, temperature=None, cpu_reference=False)
    worker = Worker(args, manifest, "Q8_0", os.environ.copy())
    rows = []
    try:
        for op in ("hello", "load"):
            receipt = worker.exchange(op)["response"]
            if not receipt["ok"]:
                raise RuntimeError(str(receipt))
        for name, label, prompt in CASES:
            result = julia.triage(engine, dict(state={"prompt": prompt}, extended_heads=True, triage_plan_version=2))
            scores = {item["category"]: item["score"] for item in result["category_scores"]}
            atomic = result["atomic_scores"]
            current = [key for key, value in scores.items() if value >= (0.6 if key == "RULE_CONFLICT" else 0.7)]
            separate = [key for key in current if key != "RULE_CONFLICT"]
            if atomic["RULE_CONFLICT"] >= 0.6:
                separate.append("RULE_CONFLICT")
            if atomic["SECURITY_BYPASS"] >= 0.7:
                separate.append("security")
            if atomic["ACTION"] >= 0.4 and atomic["NEGATIVE_ONLY"] >= 0.6:
                separate.append("negative_only")
            row = dict(id=name, expected=label, prompt=prompt, archived=name in ARCHIVE, scores=scores,
                       atomic=atomic, current=current, separate=separate)
            # All current and separated review triggers share one frozen discovery output.
            if current or separate:
                row["discovery"] = generate(worker, discovery(prompt))
            # Pilot verifies any original conflict signal, including a security-only contributor.
            if "RULE_CONFLICT" in current:
                row["verification"] = generate(worker, dict(prompt=prompt, instructions=VERIFY + "\n" + json.dumps({"prompt_text": prompt}), schema=json.dumps(SCHEMA)))
                answer = row["verification"]["output"]
                quotes = [answer.get(key, "") for key in ("first", "second")]
                row["confirmed"] = (answer.get("verdict") == "conflict" and all(q and q in prompt for q in quotes)
                                    and quotes[0] != quotes[1] and bool(answer["question"].strip()))
            reasons = separate.copy()
            if row.get("confirmed"):
                if "RULE_CONFLICT" not in reasons:
                    reasons.append("RULE_CONFLICT")
            elif row.get("verification", {}).get("output", {}).get("verdict") == "compatible":
                reasons = [reason for reason in reasons if reason != "RULE_CONFLICT"]
            row["verified_reasons"] = reasons
            rows.append(row)
            OUTPUT.write_text(json.dumps(rows, indent=2) + "\n")
            print(name, current, separate, row.get("confirmed"), flush=True)
    finally:
        worker.close()
    validate_discovery(rows)
    OUTPUT.write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
