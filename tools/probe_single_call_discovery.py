"""Compare two automatic discovery instructions with one native call per draft and arm."""
import argparse
import json
import os
from pathlib import Path
from types import SimpleNamespace

from probe_conflict_routing import ROOT, RUNTIME, Worker, discovery, generate, validate_discovery

QUALIFIER = (
    "Before offering a question or choice, identify the decision only the user can make. "
    "Return {\"kind\":\"none\"} when the draft already supplies the answer, when reading the named "
    "code is the requested way to find it, or when answering the question would do the task for the user. "
    "You do not need to inspect the code to recognize such a request. "
    "A named file can establish current behavior but cannot settle a requested future policy. "
    "If different answers change that policy, ask about that decision. Surface genuinely incompatible "
    "active requirements even when the relevant code is named.\n"
)
# Freeze the intended decision before reading either arm's answers.
FRESH = [
    ("span-inspect", "clear", "Read src/analysis/SourceSpan.cpp and explain what findUniqueSpan returns when the excerpt appears twice."),
    ("span-policy", "duplicate selection", "Change findUniqueSpan in src/analysis/SourceSpan.cpp to select a match when the excerpt appears twice. We still need to decide whether to use the first or last occurrence."),
    ("triage-inspect", "clear", "Read routePromptTriage in src/analysis/PromptTriage.cpp. Summarize how it handles a score just below its threshold."),
    ("triage-policy", "borderline routing", "Change routePromptTriage in src/analysis/PromptTriage.cpp for borderline scores. Product has two alternatives open: continue silently or ask for clarification."),
    ("timeout-settled", "clear", "In client.py, set the request timeout to 12 seconds for both uploads and downloads. Preserve every other setting."),
    ("timeout-open", "timeout duration", "In client.py, set the same request timeout for uploads and downloads. Product has approved either 12 seconds or 30 seconds, with the final duration still undecided."),
    ("output-settled", "clear", "Write the status summary in JSON only. Example from the old report: a Markdown table. That example is historical."),
    ("output-conflict", "output format", "Write a single status summary. Its entire contents must be valid JSON only. The same summary must consist solely of a Markdown table."),
]
PRIOR = {"benchmark-sentiment-original":"clear", "benchmark-sentiment-reworded":"clear",
         "subtle_pure_query_clear":"clear", "parser-both":"empty input behavior",
         "status-same":"missing entry status", "field-both":"unknown field handling",
         "multisentence_internal_conflict":"log destination"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recheck", action="store_true", help="Replay missed decisions in reverse arm order and a fully specified summary control.")
    recheck = parser.parse_args().recheck
    archive = json.loads((ROOT / "docs/audit/conflict_routing_pilot_2026-11-19.json").read_text())
    cases = FRESH + [(r["id"], PRIOR[r["id"]], r["prompt"]) for r in archive if r["id"] in PRIOR]
    if recheck:
        cases = [entry for entry in cases if entry[0] in ("timeout-open", "output-conflict", "field-both")]
        cases.append(("output-complete", "clear", 'Write a JSON-only status summary for build 17, which passed all 12 tests. Include only the build number, result, and test count. Last week the report used a Markdown table.'))
    manifest = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    args = SimpleNamespace(worker=RUNTIME / "roclint_granite_worker", model_dir=Path.home() / "models/s2",
                           diagnostic_no_deadlines=False)
    w = Worker(args, manifest, "Q8_0", os.environ.copy())
    path = ROOT / "build" / ("single-call-recheck.json" if recheck else "single-call-discovery.json")
    rows = []
    try:
        for op in ("hello", "load"):
            receipt = w.exchange(op)["response"]
            if not receipt["ok"]:
                raise RuntimeError(receipt)
        for i, (name, target, prompt) in enumerate(cases):
            payload = discovery(prompt)
            head, sep, data = payload["instructions"].partition("Request:\n")
            assert sep and json.loads(data)["prompt_text"] == prompt
            arms = ("current", "qualified") if recheck or i % 2 == 0 else ("qualified", "current")
            if recheck and name == "field-both":
                arms = ("qualified", "current")
            for arm in arms:
                instruction = head + (QUALIFIER if arm == "qualified" else "") + sep + data
                result = generate(w, dict(payload, instructions=instruction))
                rows.append(dict(id=f"{name}/{arm}", case=name, arm=arm, prior=name in PRIOR,
                                 target=target, prompt=prompt, discovery=result))
                path.write_text(json.dumps(rows, indent=2) + "\n")
                print(rows[-1]["id"], result["output"].get("kind"), flush=True)
    finally:
        w.close()
    validate_discovery(rows)
    path.write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
