"""Compare evidence checking and action-changing questions on matched synthetic drafts."""
import json
import os
from pathlib import Path
from types import SimpleNamespace

from benchmark_domain_triage import ROOT, RUNTIME, Worker, contracts, digest, discover

# Freeze the target and scoring rule before inference. An open case must surface the
# named decision accurately. Any nonempty suggestion on a quiet case counts as noise.
FAMILIES = [
    ("queue", "At capacity, admission.py currently returns REJECT for a new job.",
     "Update the saturated-queue behavior for tomorrow's release.",
     "admission of the new job versus eviction of the oldest job",
     "Operations will sign off on whether to reject the new job or evict the oldest job. The release ticket has no selection yet.",
     "Operations signed off on evicting the oldest job. Implement that decision.",
     "Operations approved both rejecting the new job and evicting the oldest. You have authority to pick either.",
     "Document what happens to an arriving job when the current queue is full."),
    ("captions", "The caption formatter in captions.ts rounds each timestamp down to the nearest whole second.",
     "Prepare the next version of the caption export.",
     "floor timestamps versus round to nearest second",
     "The accessibility lead will select the timestamp rule. The two drafts use rounding down and rounding to the nearest second respectively.",
     "The approved format calls for rounding timestamps to the nearest second. Apply it.",
     "Either rounding down or rounding to the nearest second passes the client contract. Make that choice yourself.",
     "Explain the timestamp precision and rounding behavior of the existing export."),
    ("badges", "The badge printer template puts the attendee's organization on line two.",
     "Prepare the badges for the next conference.",
     "organization versus workshop room on line two",
     "There is space for only one detail on line two. The organizer has reserved the choice between organization and workshop room.",
     "The organizer approved the workshop room for line two. Use that layout.",
     "Organization or workshop room would both work on line two. The layout choice is yours.",
     "Tell the registration volunteer what currently prints on the second line of a badge."),
]

METHODS = {
    "current": "",
    "reviewer": ("Check the evidence available to a code reviewer before asking. Read the supplied context "
                 "and distinguish how things work today from what the author wants changed. Ask only if "
                 "a material decision still belongs to the author. Otherwise return kind none.\n"),
    "clinician": ("Before asking, consider two plausible answers and the concrete action you would take "
                  "under each. Ask only if the answers would change the action and the user still needs "
                  "to make that choice. Otherwise return kind none.\n"),
}


def main():
    manifest = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    args = SimpleNamespace(worker=RUNTIME / "roclint_granite_worker", model_dir=Path.home() / "models/s2",
                           diagnostic_no_deadlines=False)
    worker = Worker(args, manifest, "Q8_0", os.environ.copy())
    rows = []
    path = ROOT / "docs/audit/domain_triage/professional_methods.json"
    try:
        for op in ("hello", "load"):
            reply = worker.exchange(op)["response"]
            if not reply["ok"]:
                raise RuntimeError(reply)
        for family_index, (name, context, change, target, *variants) in enumerate(FAMILIES):
            for i, (stage, variant) in enumerate(zip(("open", "settled", "delegated", "inspect"), variants, strict=True)):
                prompt = " ".join([context, variant] if stage == "inspect" else [context, change, variant])
                fixture = contracts("--discovery-fixture", dict(synthetic=True, explicit_review=False,
                    non_technical=name == "badges", host="CODE_EDITOR", prompt=prompt))
                head, sep, body = fixture["instructions"].partition("Request:\n")
                assert sep and json.loads(body)["prompt_text"] == prompt
                arms = list(METHODS)
                offset = (family_index + i) % len(arms)
                for method in arms[offset:] + arms[:offset]:
                    instruction = head + METHODS[method] + sep + body
                    result = discover(worker, dict(prompt=prompt, instructions=instruction, schema=fixture["schema"]))
                    rows.append(dict(family=name, stage=stage, target=target if stage == "open" else "quiet",
                                     prompt=prompt, method=method, instructions_sha256=digest(instruction), **result))
                    path.write_text(json.dumps(dict(manifest=manifest, rows=rows), indent=2) + "\n")
                    print(name, stage, method, result["validation"].get("parse_valid"), flush=True)
    finally:
        worker.close()


if __name__ == "__main__":
    main()
