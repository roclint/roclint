# Runtime and scheduling evidence — November 15, 2026

Owner: Agent 5 (runtime/scheduling). Worktree `D:/Stardust/roclint-wt-runtime`,
branch `topic/runtime-2026-11-15`, baseline `d5c8101e42b45d5d3b0796c97f01024214fd5d57`.
No model weights, sampling values, temperature, top-p, seed, thinking default, token
budgets, deadlines or installed assets were changed. No dependency was downloaded.

## What this change does

1. Opt-in JSONL timing trace shared by the native helper and the host runtime
   worker, matching the D00 trace contract.
2. Queue accounting in the helper: every queued request records its wait, and a
   request that outlived the caller's remaining budget is rejected before
   inference starts.
3. Truthful readiness: while another semantic call is still pending, a completed
   request no longer reports `READY`. The worker reports `BUSY` until the local
   queue drains.
4. Additional engine measurements that existed in no metric before: cold/warm
   preparation stages, prompt/schema/sampler setup, structured-output finish and
   the unaligned KV-prefix candidate. Existing `latency_ms`, token counts and
   outputs keep their original meaning.

## Instrumentation contract

Environment: `ROCLINT_TRACE_TIMING=<file>`. Unset or empty means no file is
created and no tracing code runs beyond one environment read per potential event.
The helper process inherits the variable from the host's process environment.

One JSON object per line, UTF-8, append-only:

```json
{"ts_ms":0,"trace_id":"...","request_id":"...","revision_id":"...","surface":"word|interview|rewrite|review|apply|",
 "stage":"queue|model_ready|generate","phase":"start|end|mark","elapsed_ms":0.0,
 "detail":{"source":"granite_helper|native_runtime","operation":"granite_analyze","queue_wait_ms":0.0}}
```

- `ts_ms` is monotonic milliseconds since boot on `std::chrono::steady_clock`.
  The host and helper share that clock on one machine, so events remain orderable
  across both processes and are unaffected by wall-clock adjustments.
  `elapsed_ms` is the span/event duration on the same clock.
- `trace_id` is the request group ID, `request_id` the attempt ID and
  `revision_id` the frozen draft revision. IDs are opaque host-generated values.
- `surface` is derived only when it is unambiguous: `granite_word_refinement` ->
  `word`, `granite_rewrite` -> `rewrite`. `granite_analyze` is used by discovery,
  interview and review, so it stays empty unless the host supplies the additive
  `RequestContext::surface` field. No surface is guessed.
- No prompt, instruction, schema, source excerpt, token text or generated text is
  recorded. Details contain only identifiers, operation names, outcomes, error
  codes, counts and durations.
- `detail.contract` is the bounded instruction-contract identifier from the first
  line (for example `granite.discovery.selective.v11` or
  `granite.goldfish_audit.v1`), emitted only when it matches an identifier shape.
  It lets consumers distinguish a generation call from an audit even though both
  use the `granite_analyze`/`granite_rewrite` wire operation. First-token timing is
  reported as `detail.first_token_ms` on `generate.end`; there is no separate
  first-token mark because the engine does not expose that instant as a callback.
- Best effort: a failed open/write disables that path and never fails or blocks a
  request beyond the opt-in file append itself. Each line is written and flushed
  independently so a crashed process does not lose earlier events.

Stages produced by the runtime (D00 reserves host stages `debounce|route|audit|review|visible|apply` for Agent 2):

| stage | phase | source | meaning |
|---|---|---|---|
| `queue` | start/end | helper | enqueue to dequeue; `elapsed_ms` is queue wait |
| `queue` | mark | helper/host | `submitted_behind_local_work`, `cancelled_before_start`, `expired_before_start` |
| `model_ready` | start/end | helper/host | cold process start or warm-on-demand to READY; `runtime`, `reason=cold|warm|policy`, `quantization`, `backend`, `load_ms`, `prepare_ms`, `verify_ms`, `model_load_ms`, `context_init_ms`, `warmup_ms` |
| `generate` | start/end | helper/host | semantic work; end carries `outcome`, `contract`, `prefill_ms`, `first_token_ms`, `decode_ms`, `sampler_ms`, `generation_ms`, `setup_ms`, `finish_ms`, token counts, `input_bytes`, `output_bytes`, KV reuse |
| `generate` | mark | host | `cancelled` (`caller_cancelled`, `deadline_expired`, `cancel_all`), `superseded_before_dispatch` |

Host `generate.end.elapsed_ms` spans submit to terminal reply (queue + inference +
IPC). Helper spans separate the queue wait from inference, so the two layers can
be compared without inventing a stage. The helper `queue` span is authoritative
for local inference wait; the host `queue` mark only states that a submission
arrived while another semantic call was pending.

## Static findings

1. **One inference loop, FIFO queue.** `granite_worker.cpp` reads frames on a
   dedicated thread but executes `GraniteEngine::execute` on one main loop with
   `std::deque<Work>` and a cap of 8. Two semantic requests never decode
   concurrently. A second request waits behind the first and consumes its own
   deadline while waiting.
2. **Deadline accounting was helper-local.** Before this change the helper
   computed `now + operation_budget` when it received a request and ignored how
   much of the caller's budget had already elapsed. The host still enforced its
   own deadline, but a queued request could be started by the helper after the
   caller had abandoned it. `effectiveNativeDeadline` now takes the earlier of
   the helper budget and the caller's remaining budget (`remaining_budget_ms`,
   additive envelope field).
3. **False READY between queued requests.** After any semantic result the worker
   published `READY` even if another semantic call was still pending in the
   helper. `HostController` gates word work on `state() == "READY"`, so a queued
   audit could be overtaken by a new word request. The worker now reports `BUSY`
   until no semantic call remains.
4. **KV prefix reuse is real but chunk-aligned.** `reusablePromptPrefix(..., 512)`
   aligns reuse down to whole 512-token prefill chunks and never reuses the final
   token. A first request has no reusable prefix, but repeated or sibling prompts
   that share a long source prefix do reuse 512 tokens. Live measurement observed
   an unaligned common prefix of 669 tokens and 512 reused tokens on the second
   and third requests, cutting prefill from 377 ms to 126–159 ms and time to
   first token from 382 ms to 129–162 ms. The 157 withheld tokens are the
   alignment gap. Changing the alignment would change prefill batching and
   therefore model numerics; it remains a separate quality experiment (E6), not a
   silent optimization.
5. **Cold work is dominated by verified model mapping, not prompt work.** The
   helper hashes the complete pinned GGUF (3.89 GB for Q8_0) on every cold load,
   then maps it, creates the context and runs a warm-up decode. `model_ready`
   separates these stages. Skipping or caching the hash would weaken the pinned
   integrity gate, so it was not changed.
6. **Cancellation already reaches the helper.** The host posts a `cancel` frame
   per pending call, and the helper's reader thread sets the target's atomic
   flag. A request cancelled while queued is never executed because the main loop
   checks the control before `execute`. The new trace records this as
   `cancelled_before_start`/`cancelled` with the wire and attempt IDs.
7. **Duplicate suppression is not implemented.** Current host call sites are
   sequential (word generation then audit, analysis discovery then audit) and a
   second word request is blocked while one is generating, so there is no
   observed duplicate generation. Coalescing byte-identical in-flight requests
   would need waiter/primary cancellation ownership; implementing it without
   live duplicate evidence would add risk without measured benefit. It remains a
   recorded experiment (E5) with a trigger condition.

## Experiments

Each experiment is separate; no measurement is inferred from another.

### E1 — timing trace (implemented)

Hypothesis: the missing stages (queue wait, cold load, prefill/first token,
structured-output finish) can be exposed without changing outputs or deadlines.
Change: trace helper + engine metrics above.
Result: mocked protocol/queue tests pass; live spans are reported in the
measurement section. The trace produced 22 events for the live sequence without
prompt text.

### E2 — helper queue budget (implemented)

Hypothesis: a queued request whose caller deadline has expired can be dropped
before inference.
Change: additive `remaining_budget_ms` envelope field, `effectiveNativeDeadline`,
pre-dequeue rejection with `TIMEOUT`.
Result: `queuePolicyTests` cover every branch. Live queued work expired at
`remaining_ms=-4,889.3` and was rejected with `TIMEOUT` without inference; a
cancelled queued request was rejected with `CANCELLED` without inference. No
output or deadline was widened.

### E3 — truthful BUSY (implemented)

Hypothesis: reporting `READY` with a pending call lets the host start more work
while the single worker is occupied.
Change: `settleReady()` keeps `BUSY` while any semantic call remains pending.
Result: covered by the queue/trace test, which observes `BUSY` between two queued
requests and `READY` only after both complete.

### E4 — cold/warm stage metrics (implemented)

Hypothesis: prefill/generation telemetry hides verified load, setup and finish
costs.
Change: `prepare_ms`, `verify_ms`, `model_load_ms`, `context_init_ms`,
`warmup_ms`, `setup_ms`, `finish_ms`, `total_ms`, `kv_cache_candidate_tokens`.
Result: additive; `latency_ms` definition unchanged.

### E5 — duplicate in-flight coalescing (not implemented)

Trigger: live trace shows byte-identical pending calls for the same operation,
instructions and schema (currently impossible through the reviewed host call
graph). If triggered, the design must allow a waiter to detach without
cancelling the primary, and promote/re-run a waiter if the primary is cancelled.
Not attempted without evidence.

### E6 — prefix alignment relaxation (not implemented; measured opportunity)

Trigger met: live repeated/sibling prompts had 669 unaligned common tokens but
reused only 512. A 157-token prefill gap remains. Relaxing the 512 alignment
would change prefill batch shape and may change logits; requires a separate
quality comparison against the pinned configuration. Not a latency-only change.

## Comprehension review profile mapping (Agent 3 request)

`graniteReasoningPlan` previously recognized only `granite.goldfish_*` as the
128-reasoning/512-answer `audit` profile. `granite.comprehension_review.v1`
therefore received the generic `analysis` profile (512 answer tokens). The review
schema returns an interpreted outcome, three arrays of prose, a rationale and an
open-ended `findings` array whose entries each carry a summary, verbatim excerpt
and proposed repair. A 512-token reservation can truncate that object, and native
truncation fails closed rather than producing a usable review.

Mapping: instructions starting with
`<instructions>\nInstruction contract: granite.comprehension_review.` now select
the existing `rewrite` budget profile (answer 1535, reasoning 512 when thinking
is enabled; reserved 2048 with the delimiter, equal to the frozen output budget).
The `audit` profile was not chosen because 512 tokens was not established as
sufficient for the schema. No manifest field, profile budget, sampling value,
temperature, top-p, seed, thinking default or deadline changed. The match is
anchored to the instruction first line, so the contract name quoted inside user
data or source text cannot select the larger reservation.

Test: `roclint_granite_reasoning_policy_tests` covers the mapping, the
thinking-off zero-reasoning case, the 2048 reservation bound and the quoted-data
negative case. Exact command/result are recorded under measurements when the
build ran.

## Measurements

Status: helper-only live run completed after the eval baseline stopped. Raw files
are in `docs/audit/runtime_evidence_2026-11-15/` (`runtime_trace.jsonl`,
`receipt.json`, `contracts.json`, `live_driver.py`). Two runs were performed; the
numbers below are the second run, with the first run in parentheses where they
differ. No other Granite worker ran during the second run; a CPU-only host test
from another worktree overlapped for part of it. No prompt or output text is in
the trace.

Configuration: pinned Q8_0, Vulkan, `v13.context_quality`, temperature 1.0,
top-p 0.95, seed 42, thinking disabled, production discovery prompts from
`granite-benchmark-fixtures.json` (`ambiguous_speed`, `three_groups`). Synthetic
prompts; nothing was approved, inserted or sent.

### Cold readiness (`load`)

| Stage | Run 2 | Run 1 |
|---|---:|---:|
| total `model_ready` | 66,718 ms (66,730 request) | 70,066 ms |
| SHA-256 verification | 52,216 ms | 52,418 ms |
| model map/load | 12,393 ms | 14,491 ms |
| context init | 1,448 ms | 1,857 ms |
| warm-up decode | 135 ms | 403 ms |
| queue wait | 1.0 ms | 0.8 ms |

Verification is 78% of cold readiness. It is the pinned integrity gate; nothing
in this change shortens or bypasses it. Caching the verified hash across process
restarts would need an explicit coordinator decision because it weakens that gate.

### Warm single request A (`ambiguous_speed`)

| Metric | A (cold cache) | C (warm repeat) | Q (queued, different prompt) |
|---|---:|---:|---:|
| input tokens | 852 | 852 | 969 |
| output tokens | 149 | 149 | 80 |
| queue wait | 0.3 ms | 0.2 ms | **3,298.6 ms** |
| prefill | 376.8 ms | 126.0 ms | 159.4 ms |
| first token | 381.9 ms | 129.2 ms | 162.3 ms |
| decode | 2,719.9 ms | 2,801.1 ms | 1,494.3 ms |
| sampler | 504.6 ms | 541.4 ms | 276.1 ms |
| setup | 39.1 ms | 9.5 ms | 11.3 ms |
| structured-output finish | 0.2 ms | 0.1 ms | 0.1 ms |
| total `generate` (helper) | 3,668.5 ms | 3,486.5 ms | 1,946.4 ms |
| host-observed request | 3,679.5 ms | 3,493.1 ms | 5,267.3 ms |
| KV reused / candidate | 0 / 0 | 512 / 669 | 512 / 669 |

A and C produced byte-identical synthetic JSON, as expected for a fixed seed and
identical input; this is one sample, not a general determinism guarantee. Prefix
reuse cut prefill and time to first token by about 3x and total request time by
about 5%. Grammar-constrained sampling is 14–16% of generation time in these
requests; it was not changed.

### Queue, cancellation and obsolete work

| Request | Sent while | Observed |
|---|---|---|
| Q (`three_groups`) | A generating | waited 3,298.6 ms, then generated 1,946.4 ms |
| B (repeat of A) | A and Q pending | cancelled by caller; dequeued after 4,889.7 ms and rejected `CANCELLED` before inference |
| D (repeat of A) | A and Q pending, `remaining_budget_ms=100` | dequeued after 4,889.3 ms, `expired_before_start` at `remaining_ms=-4,889.3`, rejected `TIMEOUT` without inference |

The single helper loop serialized all semantic work. Q's 5.27 s request time was
63% queue wait. B and D consumed no GPU inference; before this change B would have
run to completion because the helper used a fresh 45 s budget, and D would have
been admitted on the host's timeout race. The cancellation reader and the
pre-dequeue expiry check both worked live. The trace recorded
`cancelled_before_start` and `expired_before_start` marks with the wire/attempt
IDs.

The first run had the same shape: Q queue wait 3,450.2 ms, cold readiness 70.1 s,
and C total 3,314.9 ms. It also showed a 2.8 ms `generate` span for B because the
pre-dequeue cancellation check landed after that run; the second run removed it.

### What was not measured

Full host/UI/Julia end-to-end latency, word popup debounce, approval/apply,
macOS/Metal, and multiple-run distributions. The helper-only spans above are not a
full-app latency claim and no percentile is implied.

## Verification

Build (native, Vulkan, pinned llama.cpp `da2ee1e`; `ROCLINT_BUILD_NATIVE_GRANITE=ON`):

```powershell
cmake -S D:/Stardust/roclint-wt-runtime -B D:/Stardust/roclint-wt-runtime/build `
  -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=D:/Qt/6.8.2/msvc2022_64 `
  -DBUILD_TESTING=ON -DROCLINT_DEPLOY_QT=OFF -DROCLINT_BUILD_NATIVE_GRANITE=ON `
  -DROCLINT_LLAMA_SOURCE_DIR=D:/Stardust/roclint/build/deps/llama.cpp
cmake --build D:/Stardust/roclint-wt-runtime/build --config Release `
  --target roclint_granite_worker roclint_native_granite_tests roclint_granite_reasoning_policy_tests
```

Results:

| Gate | Result |
|---|---|
| `ctest -R "^roclint_native_granite$"` | Passed, 26.94 s |
| `ctest -R "^roclint_granite_reasoning_policy$"` | Passed, 0.04 s |
| Native build | exit 0, `roclint_granite_worker.exe` + tests |
| Live helper run | completed; 22 trace events, no text |

The native test includes the new queue-policy, mock trace, cancellation and
deadline cases. The reasoning-policy test includes the comprehension-review
mapping. The live run is helper-only; the full host/native/UI path was not rerun
by this agent.

## Limitations

- Mock helper evidence cannot exercise the helper's dedicated cancellation reader;
  the fixture is single-threaded while sleeping. The live run covers the helper's
  cancellation/expiry behavior; queue spans in the mock test are host-side only.
- `surface` is empty for `granite_analyze` until the host supplies the additive
  context field. This is deliberate rather than mislabeling review as interview.
- The trace adds one append+flush per event when enabled. It is off by default
  and was not used to change any scheduling decision. It cannot be a zero-cost
  guarantee under a stalled filesystem; the opt-in design is the mitigation.
- Both processes use monotonic steady-clock milliseconds for `ts_ms`. On one
  machine the clock is shared, but sub-millisecond cross-process ordering is not
  meaningful; per-span `elapsed_ms` is the authoritative duration.
- The live measurement is helper-only, two runs, with one CPU-only host test from
  another worktree overlapping part of the second run. It is not a full-app or
  distributional result. Cold-load hash time depends on filesystem cache state;
  the two runs agreed closely (52.2 s vs 52.4 s).
- No macOS/Metal execution was performed. Windows/Vulkan is the only native
  configuration available here.
- The cold model hash gate, prefix alignment, sampler cost and duplicate
  coalescing were not changed. Each needs a separate decision/experiment as
  recorded above.
