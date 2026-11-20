"""Compare audited automatic questions on archived prompts whose ownership route changed."""
import json
import os
from pathlib import Path
from types import SimpleNamespace

from benchmark_domain_triage import ROOT, RUNTIME, Worker, contracts, discover, fixture_inputs, load_worker_module
from probe_goldfish_timing import replay

AUDIT = ROOT / "docs/audit/domain_triage"


def main():
    os.environ["JULIA_DEVICE"] = "cpu"
    julia = load_worker_module()
    engine = julia.load(Path.home() / "models/s1")
    corpus = json.loads((AUDIT / "decision_ownership.json").read_text())
    manifest = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    assert manifest == corpus["manifest"]
    changed = []
    for row in corpus["rows"]:
        if row["method"] != "current":
            continue
        prompt = row["prompt"]
        case = dict(prompt=prompt, workspace_files=[], workspace_symbols=[])
        classification = julia.triage(engine, dict(state=dict(prompt=prompt, matched_files=[]),
                                                  extended_heads=True, triage_plan_version=2))
        evidence = dict(scores=[head["score"] for head in classification["category_scores"]],
                        configuration=classification["execution_configuration_id"])
        inputs = fixture_inputs(case, "CODE_EDITOR", evidence, julia)
        routes = {}
        for label in ("before", "after"):
            contract = contracts("--discovery-fixture", dict(inputs, historical_score_only=label == "before"))
            routes[label] = contract["routes"]["default"]
        if routes["before"] != routes["after"]:
            changed.append(dict(family=row["family"], stage=row["stage"], inputs=inputs, routes=routes))
    args = SimpleNamespace(worker=RUNTIME / "roclint_granite_worker", model_dir=Path.home() / "models/s2",
                           diagnostic_no_deadlines=False)
    worker = Worker(args, manifest, "Q8_0", os.environ.copy())
    rows = []
    try:
        for op in ("hello", "load"):
            reply = worker.exchange(op)["response"]
            if not reply["ok"]:
                raise RuntimeError(reply)
        for repeat in range(3):
            for case in changed[repeat:] + changed[:repeat]:
                row = dict(**case, repeat=repeat + 1)
                for label, route in case["routes"].items():
                    if route != "CLARIFY":
                        row[label] = dict(review_outcome="READY", skipped=True)
                        continue
                    inputs = dict(case["inputs"], historical_score_only=label == "before")
                    contract = contracts("--discovery-fixture", inputs)
                    payload = {key: contract[key] for key in ("prompt", "instructions", "schema")}
                    discovery = discover(worker, payload)
                    observed = dict(discovery=discovery)
                    if discovery["validation"].get("parse_valid"):
                        observed.update(replay(worker, dict(inputs, saved_discovery=discovery["raw"])))
                    row[label] = observed
                rows.append(row)
                (AUDIT / "changed_ownership_questions.json").write_text(json.dumps(
                    dict(manifest=manifest, rows=rows), indent=2) + "\n")
                print(row["family"], row["stage"], row["repeat"], flush=True)
    finally:
        worker.close()


if __name__ == "__main__":
    main()
