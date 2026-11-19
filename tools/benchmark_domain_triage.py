"""Offline policy comparison. Uses local pinned Julia and Granite weights and a built contracts tool."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from time import perf_counter
from types import SimpleNamespace

from challenge_julia_triage import load_worker_module
from check_granite_runtime import Worker

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "tests/fixtures/domain_triage/corpus.json"
RUNTIME = ROOT / "build/Roclint.app/Contents/MacOS/runtime"
TOOL = ROOT / "build/roclint_granite_contracts"
HOSTS = ("CODE_EDITOR", "BROWSER", "DOCUMENT")
QUESTION = "Which description best fits the requested work, using the draft and available workspace information?"
OPTIONS = ["Implement a change to code, an API, or structured data.",
           "Explain, compose, or transform content with routine details delegated to the author.",
           "Mixed work or insufficient information to classify the task."]
TASK_THRESHOLDS = ("0.65", "0.85", "0.70")


def host_threshold(host):
    return {"CODE_EDITOR": "0.65", "DOCUMENT": "0.85"}.get(host, "0.70")


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


def contracts(op, data):
    with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as f:
        json.dump(data, f)
        f.flush()
        return json.loads(subprocess.check_output([TOOL, op, f.name]))


def infer(case, julia, engine):
    prompt = case["prompt"]
    state = {"prompt": prompt, "matched_files": case["workspace_files"]}
    start = perf_counter()
    result = julia.triage(engine, dict(state=state, extended_heads=True, triage_plan_version=2))
    triage_ms = (perf_counter() - start) * 1000
    # Only these workspace fields and the frozen draft can enter the task question.
    task_input = json.dumps(dict(draft=prompt, files=case["workspace_files"], symbols=case["workspace_symbols"]))
    start = perf_counter()
    logits, = engine.logits([dict(state=task_input, question=QUESTION, options=OPTIONS, type="choice")])
    elapsed = (perf_counter() - start) * 1000
    winner = max(range(len(OPTIONS)), key=logits.__getitem__)
    return dict(scores=[c["score"] for c in result["category_scores"]], atomic=result["atomic_scores"],
                configuration=result["execution_configuration_id"], task=winner, task_logits=logits,
                triage_ms=triage_ms, task_ms=elapsed)


def fixture(case, host, evidence, julia):
    prompt = case["prompt"]
    technical = bool(re.search(r'\.(?:html|css|js|ts|tsx|jsx|qml|cpp|hpp|c|h|py|rs|go|java|swift|json|yaml|sql)\b|\b(?:function|class|const|let|var|import|export|co_return|struct|enum|void|public|private)\b', prompt, re.I)
                     or len(re.findall(r'[{}<>()\[\];:=#]', prompt)) >= 3)
    return contracts("--discovery-fixture", dict(synthetic=True, explicit_review=False, non_technical=not technical,
                     prompt=prompt, host=host, workspace_files=case["workspace_files"], workspace_symbols=case["workspace_symbols"],
                     model_revision=julia.REVISION, weights_sha256=julia.WEIGHTS_SHA256,
                     configuration_id=evidence["configuration"], category_scores=evidence["scores"]))


def check_routes(julia):
    case = dict(prompt="Synthetic routing boundary.", workspace_files=[], workspace_symbols=[])
    assert host_threshold("UNKNOWN") == "0.70"
    for level in (0.65, 0.70, 0.85):
        for i, threshold in enumerate((level, level, 0.70, 0.60)):
            for score, expected in [(threshold, "CLARIFY"), (threshold - 0.08, "UNCERTAIN_BUFFER"),
                                    (threshold - 0.080001, "ORIGINAL")]:
                scores = [0.0] * 4
                scores[i] = score
                routes = fixture(case, "UNKNOWN", dict(scores=scores, configuration="boundary_check"), julia)["routes"]
                assert routes[f"{level:.2f}"] == expected and routes["default"] == routes["0.70"]


def check_regressions(julia):
    archive = ROOT / "docs/audit/conflict_routing_pilot_2026-11-19.json"
    prior = json.loads(archive.read_text())
    for row in prior:
        scores = [row["scores"][key] for key in ("ACTION", "TARGET", "RULE", "RULE_CONFLICT")]
        case = dict(prompt=row["prompt"], workspace_files=[], workspace_symbols=[])
        routes = fixture(case, "UNKNOWN", dict(scores=scores, configuration="regression"), julia)["routes"]
        assert (routes["default"] == "CLARIFY") == bool(row["current"])
        assert len(set(routes.values())) == 1
    print(f"Confirmed native route parity for {len(prior)} archived cases.", flush=True)


def check_fixture_inputs(cases, julia):
    for case in cases:
        receipt = fixture(case, "CODE_EDITOR", dict(scores=[0.1]*4, configuration="input_check"), julia)
        payload = json.loads(receipt["instructions"].split("Request:\n", 1)[1])
        assert payload["prompt_text"] == case["prompt"] and payload["host_context_category"] == "CODE_EDITOR"
        assert payload.get("workspace_files", []) == case["workspace_files"]
        assert payload.get("workspace_symbols", []) == case["workspace_symbols"]
        assert not ({"domain","task","medium","split","rationale","acceptable_decisions","expected"} & payload.keys())
    for split, families_per_domain in (("dev", 1), ("held", 4)):
        for domain in {c["domain"] for c in cases}:
            actual = {c["family"] for c in cases if c["domain"] == domain and c["split"] == split}
            assert len(actual) == families_per_domain


def discover(w, payload):
    call = w.exchange("granite_analyze", payload)
    response = call["response"]
    validation, = contracts("--validate", {"runs":[{"quantization":"Q8_0","cases":[dict(
        id="response", op="granite_analyze", prompt=payload["prompt"], response=response)]}]})
    return dict(ms=call["elapsed_ms"], validation=validation, error=response.get("error"),
                raw=response.get("result", {}).get("text"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/domain-triage/receipts.json")
    parser.add_argument("--check", action="store_true", help="Validate cases and native routes without model inference.")
    args = parser.parse_args()
    os.environ["JULIA_DEVICE"] = "cpu"
    julia = load_worker_module()
    check_routes(julia)
    cases = json.loads(CORPUS.read_text())
    assert len({c["id"] for c in cases}) == len(cases) == 40
    for family in {c["family"] for c in cases}:
        pair = [c for c in cases if c["family"] == family]
        assert {c["expected"] for c in pair} == {"ask", "proceed"} and len({c["split"] for c in pair}) == 1
    check_fixture_inputs(cases, julia)
    check_regressions(julia)
    if args.check:
        return
    manifest = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    config = dict(corpus_sha256=digest(CORPUS.read_text()), julia_revision=julia.REVISION,
                  julia_sha256=julia.WEIGHTS_SHA256, granite=manifest, question=QUESTION, options=OPTIONS,
                  task_thresholds=TASK_THRESHOLDS, host_thresholds={h:host_threshold(h) for h in HOSTS},
                  current=[0.70,0.70,0.70,0.60], uncertainty_delta=0.08)
    engine = julia.load(Path.home() / "models/s1")
    worker = Worker(SimpleNamespace(worker=RUNTIME/"roclint_granite_worker", model_dir=Path.home()/"models/s2",
                                    diagnostic_no_deadlines=False),manifest,"Q8_0",os.environ.copy())
    report = dict(config=config, evidence={}, observations=[])
    try:
        for op in ("hello", "load"):
            receipt = worker.exchange(op)["response"]
            if not receipt["ok"]:
                raise RuntimeError(receipt)
        for case in sorted(cases, key=lambda c:c["split"] != "dev"):
            evidence = infer(case, julia, engine)
            report["evidence"][case["id"]] = evidence
            for host in HOSTS if case["split"] == "held" else ("CODE_EDITOR",):
                contract = fixture(case, host, evidence, julia)
                routes = contract["routes"]
                assert routes["default"] == routes["0.70"]
                arms = dict(current=routes["default"], host=routes[host_threshold(host)],
                            task=routes[TASK_THRESHOLDS[evidence["task"]]])
                row = dict(case=case["id"], host=host, routes=arms, instructions_sha256=digest(contract["instructions"]))
                if "CLARIFY" in arms.values():
                    row["discovery"] = discover(worker,{k:contract[k] for k in ("prompt","schema","instructions")})
                report["observations"].append(row)
                args.out.parent.mkdir(parents=True, exist_ok=True)
                tmp = args.out.with_suffix(".tmp")
                tmp.write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n")
                tmp.replace(args.out)
            print(case["id"], len(report["observations"]), flush=True)
    finally:
        worker.close()


if __name__ == "__main__":
    main()
