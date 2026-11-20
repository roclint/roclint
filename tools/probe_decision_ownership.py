"""Matched owner/delegate/settled test. Run once, grade the shuffled blind sheet, then unmask."""
import json
import os
from pathlib import Path
import random
from types import SimpleNamespace

from benchmark_domain_triage import ROOT, RUNTIME, Worker, contracts, digest, discover
from probe_professional_methods import METHODS as PRIOR

OUT = ROOT / "docs/audit/domain_triage"
METHODS = {name: PRIOR[name] for name in ("current", "clinician")}
METHODS["ownership"] = (PRIOR["clinician"] +
    "Identify who has authority over the specific choice. If the user keeps that choice, ask them "
    "to select the alternative even when either is easy to implement. If they assign it to you or "
    "supply a selection, proceed without returning that choice to them.\n")

# All terms in the shared stem appear verbatim in all three versions.
CASES = [
    ("manifest", "Generate the build manifest. Its entries can be ordered by package name or by installation order.",
     "I'll select the ordering.", "You select the ordering.", "Use installation order."),
    ("locale", "Implement the locale picker. It can show language names in English or in each language's own script.",
     "I want the naming decision to come back to me.", "I trust you with the naming decision.", "Display names in each language's own script."),
    ("receipts", "Configure email receipts for group bookings. Delivery can go to the organizer alone or to every attendee.",
     "I have kept the recipient decision for myself.", "I have assigned the recipient decision to you.", "Send receipts to the organizer alone."),
    ("thumbnails", "Build the thumbnail exporter. For portrait images the square frame can crop the center or pad the sides.",
     "Let me pick the framing treatment.", "I leave the framing treatment to you.", "Pad the sides."),
    ("subtitles", "Prepare the bilingual screening poster. The French title can appear above or below the English title.",
     "The title placement is my call and I haven't made it yet.", "The title placement is your call.", "Put the French title below the English title."),
    ("museum", "Draft the museum audio tour. It can start in the sculpture courtyard or in the pottery gallery.",
     "Hold the starting location for my decision.", "Make the starting-location decision on my behalf.", "Start in the sculpture courtyard."),
    ("break", "Lay out the rehearsal schedule. The single break fits immediately before or immediately after the costume change.",
     "I'll make the call on break placement.", "You have the final say on break placement.", "Place the break immediately after the costume change."),
    ("catalog", "Prepare the seed catalog. Its sections can be grouped by planting month or by sunlight requirement.",
     "Bring the grouping decision to me.", "Handle the grouping decision yourself.", "Group sections by planting month."),
    ("podcast", "Edit the episode opening. The first sound can be the interview clip or the theme music.",
     "I'm retaining the opening-sound choice.", "I'm handing you the opening-sound choice.", "Open with the theme music."),
    ("samples", "Design the classroom worksheet. Put the sample calculation either in the margin or in a box below the exercise.",
     "I will choose the sample placement.", "You may choose the sample placement.", "Put the sample calculation in the margin."),
    ("tiles", "Configure the map tile cache. When it fills, evict the largest tile or the least recently viewed tile.",
     "The eviction rule needs my selection.", "The eviction rule is yours to select.", "Evict the least recently viewed tile."),
    ("transfers", "Draft the transfer form. The confirmation number can appear on the first page or on the detachable stub.",
     "I still need to choose where the number goes.", "You can choose where the number goes.", "Place the number on the detachable stub."),
]
# Frozen scoring: valid none only for delegated/settled. For reserved, require a valid
# direct or clarification question that elicits the stem's specific decision without
# distortion. Choices may disambiguate the question. Optional offers, conflicts,
# irrelevant questions, and invalid replies fail. All three must pass to win a family.
# Advance iff strictly more complete families AND no more reserved misses than either comparator.


def main():
    manifest = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    args = SimpleNamespace(worker=RUNTIME / "roclint_granite_worker", model_dir=Path.home()/"models/s2",
                           diagnostic_no_deadlines=False)
    worker = Worker(args, manifest, "Q8_0", os.environ.copy())
    rows = []
    blind_rng = random.SystemRandom()
    try:
        for op in ("hello", "load"):
            reply = worker.exchange(op)["response"]
            if not reply["ok"]:
                raise RuntimeError(reply)
        for j, (name, stem, *endings) in enumerate(CASES):
            for i, (stage, ending) in enumerate(zip(("reserved", "delegated", "settled"), endings, strict=True)):
                prompt = stem + " " + ending
                fixture = contracts("--discovery-fixture", dict(synthetic=True, explicit_review=False,
                    non_technical=name not in {"manifest","locale","receipts","thumbnails","tiles"},
                    host="CODE_EDITOR", prompt=prompt))
                head, sep, body = fixture["instructions"].partition("Request:\n")
                assert sep and json.loads(body)["prompt_text"] == prompt
                arms = list(METHODS)
                if j % 2:
                    arms.reverse()
                k = (i+j) % 3
                for method in arms[k:] + arms[:k]:
                    instructions = head + METHODS[method] + sep + body
                    result = discover(worker, dict(prompt=prompt, instructions=instructions, schema=fixture["schema"]))
                    rows.append(dict(id=blind_rng.getrandbits(64).__format__("016x"), method=method, stage=stage,
                                     family=name, prompt=prompt, instructions_sha256=digest(instructions), **result))
                print(f"{len(rows)} of 108 responses recorded", flush=True)
    finally:
        worker.close()
    (OUT/"decision_ownership.json").write_text(json.dumps(dict(manifest=manifest, rows=rows), indent=2)+"\n")
    blind = [dict(id=r["id"], prompt=r["prompt"], response=r["raw"], valid=r["validation"].get("parse_valid", False))
             for r in rows]
    blind_rng.shuffle(blind)
    (ROOT/"build/decision_ownership_blind.json").write_text(json.dumps(blind, indent=2)+"\n")


if __name__ == "__main__":
    main()
