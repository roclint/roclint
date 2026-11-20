"""Replay frozen discovery replies through the production Goldfish repair loop."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace

from benchmark_domain_triage import ROOT, RUNTIME, TOOL, Worker, fixture_inputs, infer, load_worker_module
from probe_review_timing import FAMILIES

AUDIT = ROOT / "docs/audit/domain_triage"


def replay(worker, inputs):
    calls = []
    with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as fixture:
        json.dump(inputs, fixture)
        fixture.flush()
        with subprocess.Popen([TOOL, "--discovery-fixture", fixture.name], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True) as process:
            try:
                for line in process.stdout:
                    frame = json.loads(line)
                    if "stage" not in frame:
                        if process.wait() != 0:
                            raise RuntimeError("Fixture failed after replay")
                        return dict(result=frame, calls=calls)
                    payload = dict(prompt=inputs["prompt"], instructions=frame["instructions"], schema=frame["schema"])
                    call = worker.exchange("granite_analyze", payload)
                    response = call["response"]
                    calls.append(dict(stage=frame["stage"], ms=call["elapsed_ms"], ok=response["ok"],
                                      raw=response.get("result", {}).get("text"), error=response.get("error")))
                    process.stdin.write(json.dumps(response) + "\n")
                    process.stdin.flush()
            finally:
                process.stdin.close()
    raise RuntimeError("Fixture exited without a result")


def main():
    saved = json.loads((AUDIT / "review_timing.json").read_text())
    manifest = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    if manifest != saved["config"]:
        raise ValueError("Worker configuration differs from frozen discovery")
    os.environ["JULIA_DEVICE"] = "cpu"
    julia = load_worker_module()
    engine = julia.load(Path.home() / "models/s1")
    files = {family[0]: family[-1] for family in FAMILIES}
    args = SimpleNamespace(worker=RUNTIME / "roclint_granite_worker", model_dir=Path.home() / "models/s2",
                           diagnostic_no_deadlines=False)
    worker = Worker(args, manifest, "Q8_0", os.environ.copy())
    rows = []
    try:
        for op in ("hello", "load"):
            reply = worker.exchange(op)["response"]
            if not reply["ok"]:
                raise RuntimeError(reply)
        for row in saved["rows"]:
            case = dict(prompt=row["prompt"], workspace_files=files[row["family"]], workspace_symbols=[])
            inputs = fixture_inputs(case, "CODE_EDITOR", infer(case, julia, engine), julia)
            inputs["saved_discovery"] = row["discovery"]["raw"]
            observation = replay(worker, inputs)
            rows.append(dict(family=row["family"], stage=row["stage"], route=row["route"], **observation))
            print(row["family"], row["stage"], observation["result"]["audit"], flush=True)
    finally:
        worker.close()
    (AUDIT / "goldfish_timing.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
