"""Replay the frozen domain study with the discovery host field masked."""
import json
import os
from types import SimpleNamespace
from pathlib import Path

from benchmark_domain_triage import CORPUS, ROOT, RUNTIME, Worker, digest, discover, fixture, load_worker_module

AUDIT = ROOT/"docs/audit/domain_triage"


def main():
    archive = json.loads((AUDIT/"receipts.json").read_text())
    manifest = json.loads((RUNTIME/"granite-runtime-config.json").read_text())
    assert archive["config"]["granite"] == manifest
    assert digest(CORPUS.read_text()) == archive["config"]["corpus_sha256"]
    julia = load_worker_module()
    args = SimpleNamespace(worker=RUNTIME/"roclint_granite_worker", model_dir=Path.home()/"models/s2",
                           diagnostic_no_deadlines=False)
    w = Worker(args,manifest,"Q8_0",os.environ.copy())
    rows = []
    try:
        for op in ("hello","load"):
            response = w.exchange(op)["response"]
            if not response["ok"]:
                raise RuntimeError(response)
        for case in json.loads(CORPUS.read_text()):
            if case["split"] != "held":
                continue
            evidence = archive["evidence"][case["id"]]
            unknown = fixture(case,"UNKNOWN",evidence,julia)
            baseline = [r for r in archive["observations"] if r["case"] == case["id"]]
            assert len(baseline) == 3
            for row in baseline:
                known = fixture(case,row["host"],evidence,julia)
                assert digest(known["instructions"]) == row["instructions_sha256"]
                needle = '"host_context_category":"'+row["host"]+'"'
                assert known["instructions"].count(needle) == 1
                assert known["instructions"].replace(needle,'"host_context_category":"UNKNOWN"') == unknown["instructions"]
                assert known["schema"] == unknown["schema"] and row["routes"]["current"] == unknown["routes"]["default"]
            record = dict(case=case["id"], route=unknown["routes"]["default"], instructions_sha256=digest(unknown["instructions"]))
            if record["route"] == "CLARIFY":
                record["discovery"] = discover(w,{k:unknown[k] for k in ("prompt","instructions","schema")})
            rows.append(record)
            (AUDIT/"unknown_host.json").write_text(json.dumps(rows,indent=2)+"\n")
            print(case["id"],flush=True)
    finally:
        w.close()


if __name__ == "__main__":
    main()
