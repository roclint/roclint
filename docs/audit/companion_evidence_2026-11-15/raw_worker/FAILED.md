# Raw worker stage — NOT COMPLETED

The helper-only raw discovery stage was attempted twice on 2026-11-15 and did
not produce a usable raw-model receipt. This is a limitation of the existing
`tools/check_granite_runtime.py` helper relative to the v13 worker, not a
negative result about the host product path.

## Attempt 1 — stale capability check

Command: `run_companion_eval.py --stage raw`.

`Worker.exchange("hello")` expected the older 12 s analysis capability
(`analysis_timeout_ms=12000`), while the v13 worker advertises 45000. The helper
raised `RuntimeError: Native operation deadline capabilities were incompatible`
before any model call. Receipt: `raw_results.json` (error), `raw_driver.log`.

## Attempt 2 — diagnostic mode hung during generation

To match the handshake, the worker was launched with its explicit diagnostic
deadline mode. It loaded the Q8 model (working set ≈4.3 GB) and then generated
for more than five minutes at 97% GPU utilization with no response. The
diagnostic mode removes the production analysis deadline, so this is not a
release-path behavior; the run was terminated to free the GPU. No case result
was produced.

## Attempt 3 — v13 capability wrapper hung

`V13Worker` reuses the checker's process/frame/request transport but validates
the v13 45 s/60 s capabilities and allows 60 s for analysis. The worker loaded
the model again and then hung during the first generation at ~97% GPU with no
response for over five minutes. It was terminated to free the GPU.

## Consequence

No raw model JSON (source quotations, option `effect`, assumed premises) was
captured by this helper. The host NDJSON (`host_*.ndjson`) retains every
question, option, answer, proposal, constraint and audit verdict verbatim as the
user-visible response. The host stage is the authoritative product evidence.

The likely cause is that the raw helper launches the worker with only
`--model-dir` and `--quantization`, while the host passes the full v13 runtime
configuration/launch contract. This is consistent with the older worker copy
being rejected earlier. Do not fix this by weakening a deadline or sampling
setting; a follow-up should reuse the production runtime launch path or update
the helper with the coordinator's approval.
