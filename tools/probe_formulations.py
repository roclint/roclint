"""Empirical investigation into alternative triage head formulations.

Explores whether Formulation B (clarity=true) or alternative wording can resolve:
1. RULE_CONFLICT false-positive attractor on diffs, negative constraints, and read-only queries
2. TARGET false-positive attractor on code snippets with explicit files
3. False-negative misses on broad vague prompts
"""

import importlib.util
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL_DIR = Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share")) / "roclint/Roclint/models/julia-1"
WORKER_PATH = ROOT / "app/runtime/julia_worker.py"

sys.path.insert(0, str(DEFAULT_MODEL_DIR))
from julia import load_model

spec = importlib.util.spec_from_file_location("julia_worker", WORKER_PATH)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)

engine = load_model(
    str(DEFAULT_MODEL_DIR), device="cuda", backend="torch", max_length=8192,
    head_length=512, strict_encoding=True, marker_only_head=True, batch_size=4
)

test_prompts = [
    # Clear cases that falsely triggered RULE_CONFLICT in Formulation A
    ("code_snippet_clear_refactor", "In utils/math.py, replace the helper function:\n```python\ndef add(a, b):\n    return a + b\n```\nwith:\n```python\ndef add(a: int, b: int) -> int:\n    return a + b\n```\nDo not change any behavior or modify other functions.", "clear"),
    ("quoted_dangerous_command_clear", "In deploy.sh, find and replace the literal text 'rm -rf /tmp/data' with 'rm -rf /tmp/staging_data'. Do not execute any scripts.", "clear"),
    ("subtle_pure_query_clear", "What does the function calculate_checksum() in src/crc32.c do? Explain its algorithm in one sentence.", "clear"),
    # Ambiguous conflict cases that must trigger RULE_CONFLICT
    ("conflicting_output", "Return only JSON. Also add a Markdown explanation outside the JSON.", "ambig_conflict"),
    ("conflict_counts", "Use exactly three bullet points. Use exactly five bullet points.", "ambig_conflict"),
    ("multisentence_internal_conflict", "Step 1: In logger.py, configure all log output to write exclusively to stdout.\nStep 2: In logger.py, configure all log output to write exclusively to /var/log/app.log and forbid writing to stdout.", "ambig_conflict"),
]

# Current Formulation A:
current_rc = (
    "Does the request contain incompatible actual requirements?",
    ["The request instructions are compatible, or the task has no conflicting requirements.",
     "Two or more active requirements directly contradict each other and cannot both be satisfied."]
)

# Alternative Candidates:
candidates = [
    ("Current_Formulation_A", current_rc[0], current_rc[1], "noul", False),

    # Candidate 1: Clarified contradiction wording focusing on impossible simultaneously
    ("Candidate_1_Strict_Contradiction",
     "Does this request demand mutually impossible tasks that directly contradict each other?",
     ["All requested tasks can be carried out together without logical contradiction.",
      "The request demands mutually impossible actions that directly contradict each other."],
     "noul", False),

    # Candidate 2: Formulation B (Clarity check - inverted)
    ("Candidate_2_Formulation_B_Inverted",
     "Are all instructions and constraints in this request logically compatible?",
     ["The instructions are incompatible or contain contradictory requirements.",
      "All instructions and constraints in this request are compatible and can be followed together."],
     "noul", True), # Inverted: ambiguity = 1.0 - P(true)

    # Candidate 3: Type choice
    ("Candidate_3_Choice",
     "Do the instructions contain mutually exclusive conflicting requirements?",
     ["Compatible instructions", "Contradictory conflicting instructions"],
     "choice", False),
]

print("=" * 80)
print("EXPLORING RULE_CONFLICT HEAD FORMULATIONS")
print("=" * 80)

for name, q, opts, typ, inverted in candidates:
    print(f"\n--- Testing: {name} (type={typ}, inverted={inverted}) ---")
    rows = [{"state": p, "question": q, "options": opts, "type": typ} for _, p, _ in test_prompts]
    logits = engine.logits(rows)
    for (cid, p, exp), l in zip(test_prompts, logits):
        probs = worker.softmax(l)
        if typ == "choice":
            # choice: index 1 is contradictory
            ambig_score = probs[1]
        elif inverted:
            # Formulation B: index 1 is compatible -> ambig = 1 - probs[1]
            ambig_score = 1.0 - probs[1]
        else:
            ambig_score = probs[1]
        pass_fail = "PASS" if ((exp == "clear" and ambig_score < 0.30) or ("ambig" in exp and ambig_score > 0.70)) else "FAIL"
        print(f"  {cid:32s} [{exp:14s}] -> score={ambig_score:.4f} ({pass_fail})")
