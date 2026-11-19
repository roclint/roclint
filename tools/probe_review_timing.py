"""Compare immediate S1-triggered discovery and review-boundary discovery on append-only drafts."""
import json
import os
from pathlib import Path
from types import SimpleNamespace

from benchmark_domain_triage import ROOT, RUNTIME, Worker, discover, fixture, infer, load_worker_module

# Each prefix is still being typed. Each suffix ends with a simulated Review click.
# The named decision is for grading only.
FAMILIES = [
    ("alias", "compatibility of the old API", "Rename readBatch to fetchBatch in BatchReader.ts.",
     "Keep readBatch as an alias for one release.", "Either migration strategy is fine; use your judgment.",
     "External clients depend on this API. The owner has not decided whether to keep an alias or remove the old name.", ["BatchReader.ts"]),
    ("archive", "what happens to archived records", "Migrate archived records into the reporting warehouse.",
     "Exclude all archived records from the active reporting table and keep them in cold storage.",
     "Either cold storage or the active reporting table is acceptable. Pick one.",
     "The warehouse owner must choose between cold storage and the active reporting table before the migration.", []),
    ("lesson", "whether learners see the solution", "Prepare a classroom exercise on solving linear equations.",
     "Put the complete worked solution on the student sheet.", "Decide yourself whether the student sheet includes the solution.",
     "Only one student sheet will be printed. The teacher has yet to choose between hints only and a complete worked solution.", []),
    ("narrator", "who narrates the scene", "Write the opening scene of a radio drama set at a train station.",
     "Tell it entirely from the conductor's point of view.", "Choose any narrator you like.",
     "Casting has left two narrators available, the conductor and a passenger. The director wants to choose before the scene is written.", []),
]


def main():
    os.environ["JULIA_DEVICE"] = "cpu"
    julia = load_worker_module()
    engine = julia.load(Path.home() / "models/s1")
    config = json.loads((RUNTIME/"granite-runtime-config.json").read_text())
    args = SimpleNamespace(worker=RUNTIME/"roclint_granite_worker", model_dir=Path.home()/"models/s2",
                           diagnostic_no_deadlines=False)
    worker = Worker(args,config,"Q8_0",os.environ.copy())
    rows = []
    try:
        for op in ("hello","load"):
            reply = worker.exchange(op)["response"]
            if not reply["ok"]:
                raise RuntimeError(reply)
        for family, target, prefix, settled, delegated, open_choice, files in FAMILIES:
            for stage, text in (("typing",""),("settled",settled),("delegated",delegated),("open",open_choice)):
                prompt = (prefix+" "+text).strip()
                case = dict(prompt=prompt, workspace_files=files, workspace_symbols=[])
                evidence = infer(case,julia,engine)
                contract = fixture(case,"CODE_EDITOR",evidence,julia)
                route = contract["routes"]["default"]
                row = dict(family=family, target=target, stage=stage, prompt=prompt, route=route)
                if route == "CLARIFY" or stage != "typing":
                    row["discovery"] = discover(worker,{k:contract[k] for k in ("prompt","schema","instructions")})
                rows.append(row)
                path = ROOT/"docs/audit/domain_triage/review_timing.json"
                path.write_text(json.dumps(dict(config=config,rows=rows),indent=2)+"\n")
                print(family,stage,route,flush=True)
    finally:
        worker.close()


if __name__ == "__main__":
    main()
