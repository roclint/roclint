# Timing-event gap list and proposed contract — 2026-11-15

This is the measurement-side request to Agent 2 (host/controller) and Agent 5
(runtime/worker). It follows the binding trace contract in
`handoffs/DeepSeek/D00_interface_contract_2026-11-15.md`:

```
ROCLINT_TRACE_TIMING=<file>
one JSON object per line, UTF-8, no prompt or source text
{"ts_ms":0,"trace_id":"...","request_id":"...","revision_id":"...",
 "surface":"word|interview|rewrite|review|apply",
 "stage":"debounce|route|queue|model_ready|generate|audit|review|visible|apply",
 "phase":"start|end|mark","elapsed_ms":0,"detail":{}}
```

Do not emit a stage that has no event. Emit best effort; a trace failure must
never block or fail the product request. Keep the trace disabled by default.

## What this baseline could observe without instrumentation

| Observed span | Source | Includes | Excludes |
|---|---|---|---|
| status feedback | HostController `analysisStatusChanged` | host transition to ANALYZING | QML paint, OS show, debounce |
| Julia cold load | status text "S1: Starting/ Loading" → "Checking your prompt." | worker spawn, Python import, model load, warm-up | Julia internal phases, OS cache state |
| Julia triage + route | "Checking…" → "Warming the pinned native model." | classifyPrompt round trip, score validation, route choice | queue wait before triage |
| Granite cold warm | "Warming…" → "Analyzing your prompt." | worker spawn, mmap, KV/context allocation, warm decode | internal load phases, GPU driver shader compile |
| discovery visible | "Analyzing…" → READY | native request, generation, parse, materiality policy, Goldfish audit | prompt construction time, first token, audit split |
| Julia attempt | `ModelAttemptRecord` stage `julia_triage` | classifyPrompt call | load |
| discovery attempt | `ModelAttemptRecord` stage `discovery` | request dispatch → parsed result/audit completion | queue wait before attempt start, UI |
| clarification check | `ModelAttemptRecord` stage `clarification_check` | answer audit round trip | answer typing/debounce |
| rewrite request → ready | synthesize call → proposal property ready | queue, possible re-warm, generation, rewrite audit, parse | QML render, approval/apply |
| rewrite attempt | `ModelAttemptRecord` stage `rewrite` | native rewrite + audit | pre-attempt warm/queue |

The raw NDJSON and parsed rows retain every timestamp used above. These spans
are real but coarse: generation and audit are not separable, and no UI or
debounce is present because the probe calls the host invokable directly.

## Missing observations (ordered by product impact)

1. **UI debounce and capture.** The 150 ms draft debounce plus accessibility
   capture/selection time is absent from the headless host. The user-visible
   delay before the host even starts is therefore unmeasured.
2. **Queue wait.** With one worker, an interview turn, rewrite and review can
   compete. Attempt `startedAtMs` begins after queueing, so queue delay is
   currently invisible. A busy model logs "Waiting for the model to finish an
   earlier request" only as status text, with no wait span.
3. **Granite cold-load internals.** One 60–90 s number hides worker spawn,
   mmap, KV allocation and warm decode. The largest single delay in this
   baseline cannot be attributed to a component.
4. **Julia cold-load internals.** One ~68 s number hides Python import, CUDA
   context creation, weight load and warm-up.
5. **First token / streaming preview.** `streamingPreview` changes have no
   timestamps. First feedback vs first useful token is a product distinction
   the baseline cannot make.
6. **Generate vs audit.** Discovery and rewrite attempts include a second model
   audit. A slow audit looks like slow generation. This matters because the
   audit is the main quality gate and must not be silently removed for speed.
7. **Goldfish review as its own stage.** There is no `review` span even though
   explicit review and comprehension checks exist; only the enclosing attempt.
8. **Visible rendering.** No QML paint/first-frame timestamp. The headless
   property is not proof that a usable card was visible.
9. **Apply/insert/readback.** Approval and insertion are out of scope here, but
   the contract needs them for the complete interaction.
10. **Cancellation/supersession.** Time from a newer edit to the stale request's
    cancellation/discard is not observable, so "coalesce obsolete work" cannot
    be verified.
11. **Word surface.** The word popup trigger → debounce → generation → candidate
    audit → visible card chain has no event coverage in this baseline.

## Proposed events (minimum to close the list)

Use the D00 fields unchanged. Add only `detail` keys; do not create a parallel
trace format.

| Gap | Emitter | Events |
|---|---|---|
| UI debounce/capture | Agent 4 (QML) / Agent 2 | `surface=word|interview`, `stage=debounce`, `phase=start` at capture; `phase=end` with `detail.delay_ms`, `detail.reason="quiet_period|max_wait"` |
| Queue | Agent 2 | `stage=queue`, `phase=start` when request waits; `phase=end` when admitted; `detail.depth`, `detail.reason="busy|residency|storage"` |
| Granite cold load | Agent 5 | `stage=model_ready`, `phase=start` before spawn; `phase=end` when READY; `detail.runtime="granite"`, `detail.reason="cold|warm|reload"`, `detail.quantization`, `detail.backend`, `detail.load_ms` |
| Julia cold load | Agent 5 | same `stage=model_ready`, `detail.runtime="julia"`, `detail.device`, `detail.load_ms` |
| First token | Agent 5 | `stage=generate`, `phase=mark`, `detail.first_token_ms`, `detail.output_tokens`, `detail.contract` |
| Generate span | Agent 5 | `stage=generate`, `phase=start|end`, `detail.attempt_id`, `detail.contract`, `detail.input_bytes`, `detail.output_bytes` (never text) |
| Audit span | Agent 2 | `stage=audit`, `phase=start|end`, `detail.attempt_id`, `detail.contract="goldfish_discovery|clarification_check|rewrite_audit"`, `detail.outcome` |
| Review span | Agent 2/3 | `stage=review`, `phase=start|end`, `detail.attempt_id`, `detail.review_kind="comprehension|word"`, `detail.findings` |
| Visible | Agent 4 | `stage=visible`, `phase=mark`, `detail.surface="question|proposal|word_card"`, `detail.render_ms`, `detail.revision_id` |
| Apply | Agent 2 | `stage=apply`, `phase=start|end`, `detail.readback_ok`, `detail.provider` |
| Cancellation | Agent 2 | `stage=queue`, `phase=mark`, `detail.reason="superseded|cancelled"`, `detail.age_ms` |

### Correlation

- `trace_id` per user interaction (stable across surfaces when a word popup
  expands into an interview).
- `request_id` per model/audit request; joins to `ModelAttemptRecord.attemptId`.
- `revision_id` and `surface` on every event.
- `ts_ms` monotonic; consumers derive spans, so emitters must not invent
  `elapsed_ms` for a stage they do not own.
- Add `detail.parent_request_id` only if a review is derived from an existing
  request; do not duplicate text.

### Acceptance for the trace

1. Default build with no environment variable writes nothing.
2. A trace write failure cannot fail, delay or retry a product request.
3. The trace contains no prompt, source, answer, candidate or generated text.
4. One interview answer + rewrite produces ordered `queue`, `model_ready`,
   `generate`, `audit`, `visible` events with joinable IDs.
5. `ROCLINT_TRACE_TIMING` output can be replayed through
   `tools/companion_eval/parse_trace.py` (to be added with the trace) into the
   same span table used here.

## Measurement request to later agents

Do not change sampling, thinking default, deadlines or audit presence to make a
receipt look faster. Add events first, rerun this exact scenario file, and
report the same cold/warm labels. A speed claim must name the span it improves
and show that usefulness did not regress on the same raw responses.
