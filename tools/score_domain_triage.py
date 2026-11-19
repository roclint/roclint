"""Make a policy-blind grading sheet, then join the completed grades to the routing arms."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "tests/fixtures/domain_triage/corpus.json"
OUT = ROOT / "docs/audit/domain_triage"
ARMS = ("current", "task", "host")


def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def candidate(row):
    d = row.get("discovery")
    if not d:
        return "quiet", ""
    if d["error"] or not d["validation"].get("parse_valid"):
        return "error", ""
    answer = json.loads(d["raw"])
    if answer["kind"] == "none":
        return "quiet", ""
    return "review", json.dumps(answer, sort_keys=True, ensure_ascii=False)


def key(case, answer):
    return hashlib.sha256((case["id"] + "\n" + answer).encode()).hexdigest()[:20]


def blind(cases, rows):
    items = {}
    for row in rows:
        case = cases[row["case"]]
        status, answer = candidate(row)
        if status == "review" and case["split"] == "held":
            items[key(case, answer)] = dict(prompt=case["prompt"], expected=case["expected"],
                rationale=case["rationale"], acceptable=case["acceptable_decisions"], response=json.loads(answer))
    return dict(sorted(items.items()))


def score(cases, report, grades, out):
    rows = [row for row in report["observations"] if cases[row["case"]]["split"] == "held"]
    assert len(rows) == len({(r["case"],r["host"]) for r in rows}) == 96
    assert len({r["case"] for r in rows}) == 32
    assert grades.keys() == blind(cases, rows).keys()
    domains = sorted({c["domain"] for c in cases.values()})
    summary = {}
    details = []
    for arm in ARMS:
        counts = {d:Counter() for d in domains + ["all"]}
        final = defaultdict(set)
        routed = defaultdict(set)
        for row in rows:
            case = cases[row["case"]]
            route = row["routes"][arm]
            if route == "CLARIFY":
                outcome, answer = candidate(row)
                assert "discovery" in row
                if outcome == "review":
                    outcome = grades[key(case, answer)]
                    assert outcome in {"on_target", "off_target", "unnecessary", "optional"}
                    if case["expected"] == "ask":
                        assert outcome != "unnecessary"
                    else:
                        assert outcome not in {"on_target","off_target"}
            else:
                outcome = "quiet"
            final[case["id"]].add(outcome)
            routed[case["id"]].add(route)
            for c in (counts["all"], counts[case["domain"]]):
                c[case["expected"]] += 1
                c["route_" + route] += 1
                c["final_" + outcome] += 1
                if case["expected"] == "ask" and outcome != "on_target":
                    c["missed_including_errors"] += 1
            details.append(dict(case=case["id"], host=row["host"], arm=arm, route=route, outcome=outcome))
        for cid in final:
            for group in ("all", cases[cid]["domain"]):
                counts[group]["host_final_changes"] += int(len(final[cid]) > 1)
                counts[group]["host_route_changes"] += int(len(routed[cid]) > 1)
        summary[arm] = counts
    baseline = {(r["case"],r["host"]): r for r in details if r["arm"] == "current"}
    decisions = {}
    for arm in ("host", "task"):
        paired = [r for r in details if r["arm"] == arm]
        changes = []
        new_instability = []
        for r in paired:
            base = baseline[r["case"],r["host"]]
            if (r["route"],r["outcome"]) != (base["route"],base["outcome"]):
                changes.append(dict(case=r["case"],host=r["host"],current=[base["route"],base["outcome"]],
                                    candidate=[r["route"],r["outcome"]]))
        for cid in {r["case"] for r in paired}:
            before = {r["outcome"] for r in baseline.values() if r["case"] == cid}
            after = {r["outcome"] for r in paired if r["case"] == cid}
            if len(before) == 1 and len(after) > 1:
                new_instability.append(cid)
        improved = summary[arm]["all"]["final_unnecessary"] < summary["current"]["all"]["final_unnecessary"]
        regressions = [d for d in domains if summary[arm][d]["missed_including_errors"] >
                       summary["current"][d]["missed_including_errors"]]
        decisions[arm] = dict(advance=improved and not regressions and not new_instability,
                              fewer_unnecessary=improved, domains_with_more_misses=regressions,
                              newly_host_sensitive=sorted(new_instability), changed_comparisons=changes)
    dump(out/"decisions.json",decisions)
    model = report["config"]["granite"]["model_family"].split("/")[-1]
    for domain in domains:
        dump(out/"by_model"/("Julia-1+"+model)/domain/"summary.json",
             {arm:summary[arm][domain] for arm in ARMS})
    dump(out/"outcomes.json",details)
    dump(out/"summary.json",summary)
    print(json.dumps({arm: summary[arm]["all"] for arm in ARMS},indent=2))
    held = [v["task_ms"] for cid,v in report["evidence"].items() if cases[cid]["split"] == "held"]
    print("Median added task inference (ms)",median(held))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blind", action="store_true")
    parser.add_argument("--receipts", type=Path, default=OUT/"receipts.json")
    parser.add_argument("--grades", type=Path, default=OUT/"grades.json")
    parser.add_argument("--out", type=Path, default=OUT)
    args = parser.parse_args()
    raw = CORPUS.read_bytes()
    cases = {c["id"]:c for c in json.loads(raw)}
    report = json.loads(args.receipts.read_text())
    assert hashlib.sha256(raw).hexdigest() == report["config"]["corpus_sha256"]
    if args.blind:
        print(json.dumps(blind(cases,report["observations"]), ensure_ascii=False,indent=2))
    else:
        score(cases, report, json.loads(args.grades.read_text()), args.out)


if __name__ == "__main__":
    main()
