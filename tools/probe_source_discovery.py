"""Paired offline discovery replay with hand-selected source evidence."""
import json
import os
from pathlib import Path
from types import SimpleNamespace

from benchmark_domain_triage import ROOT, RUNTIME, Worker, contracts, discover, digest

# Pair an inspection with an open product choice about the same implementation.
CASES = [
    ("src/analysis/SourceSpan.cpp", "findUniqueSpan", 36, 51,
     "Read src/analysis/SourceSpan.cpp and explain what findUniqueSpan returns when the excerpt appears twice.",
     "Change findUniqueSpan in src/analysis/SourceSpan.cpp to select a match when the excerpt appears twice. We still need to decide whether to use the first or last occurrence."),
    ("src/analysis/PromptTriage.cpp", "routePromptTriage", 116, 128,
     "Read routePromptTriage in src/analysis/PromptTriage.cpp. Summarize how it handles a score just below its threshold.",
     "Change routePromptTriage in src/analysis/PromptTriage.cpp for borderline scores. Product has two alternatives open: continue silently or ask for clarification."),
    ("src/core/crypto/Sha256.cpp", "normalizeSha256Digest", 163, 184,
     "Explain how normalizeSha256Digest in src/core/crypto/Sha256.cpp handles uppercase hexadecimal digits.",
     "Update the public input policy for normalizeSha256Digest in src/core/crypto/Sha256.cpp. The API owner still needs to decide whether uppercase hexadecimal digits should be accepted or rejected."),
]


def main():
    config = json.loads((RUNTIME / "granite-runtime-config.json").read_text())
    args = SimpleNamespace(worker=RUNTIME / "roclint_granite_worker", model_dir=Path.home()/"models/s2",
                           diagnostic_no_deadlines=False)
    worker = Worker(args, config, "Q8_0", os.environ.copy())
    rows = []
    try:
        for op in ("hello", "load"):
            reply = worker.exchange(op)["response"]
            if not reply["ok"]:
                raise RuntimeError(reply)
        for index, (file, symbol, start, end, inspect, policy) in enumerate(CASES):
            excerpt = "\n".join((ROOT/file).read_text().splitlines()[start-1:end])
            for i, prompt in enumerate((inspect, policy)):
                fixture = contracts("--discovery-fixture", dict(synthetic=True, explicit_review=False,
                    non_technical=False, host="CODE_EDITOR", prompt=prompt, workspace_files=[file]))
                header, divider, raw = fixture["instructions"].partition("Request:\n")
                data = json.loads(raw)
                assert data["prompt_text"] == prompt
                for arm in (("filename","excerpt") if (index+i)%2==0 else ("excerpt","filename")):
                    request = dict(data)
                    if arm == "excerpt":
                        request["source_excerpt"] = dict(path=file, start_line=start, text=excerpt)
                    instructions = header + divider + json.dumps(request, ensure_ascii=False, separators=(",",":"))
                    result = discover(worker, dict(prompt=prompt, instructions=instructions, schema=fixture["schema"]))
                    rows.append(dict(symbol=symbol, expected="ask" if i else "quiet", arm=arm,
                                     prompt=prompt, excerpt=excerpt if arm=="excerpt" else None,
                                     instructions_sha256=digest(instructions), **result))
                    print(symbol,i,arm,result["validation"].get("parse_valid"),flush=True)
    finally:
        worker.close()
    path = ROOT/"docs/audit/domain_triage/source_discovery.json"
    path.write_text(json.dumps(dict(config=config,rows=rows),indent=2)+"\n")


if __name__ == "__main__":
    main()
