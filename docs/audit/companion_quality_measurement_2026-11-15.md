# Companion/intent quality and latency — baseline measurement, 2026-11-15

Status: baseline captured on the unchanged product source. The fair
conversational-skill comparison is **NOT RUN**. This is a 12-scenario
development baseline plus one explicit-review follow-up, not a held-out quality
score and not a full-app latency guarantee.

Baseline commit: `d5c8101e42b45d5d3b0796c97f01024214fd5d57` (`feat/roc-hud-refresh`),
measured in `D:/Stardust/roclint-wt-eval` on `topic/eval-2026-11-15`. Production
sources, CMake, model settings and the installed application were not edited.

## 1. What was measured

Two stages were attempted; only the host stage produced evidence:

1. **Host stage (product path).** An evaluation-only probe
   (`tools/companion_probe/`) links the already-built Release
   `roclint_host_lib_testing` / `roclint_core_testing` / `roclint_storage_testing`
   libraries read-only and drives the unmodified `HostController` through its
   public invokables and test seams. It uses an in-memory database, isolated
   QSettings, the pinned Julia-1 scorer and the pinned Q8_0 native Granite
   worker. It never approves, inserts, sends or touches a user editor/history.
2. **Raw worker stage (helper-only, not completed).** The intent was to feed
   the production discovery prompt builder and schema
   (`roclint_granite_contracts`) to the existing
   `tools/check_granite_runtime.py` Worker transport to retain raw model JSON.
   The existing helper expects the older 12 s analysis capability; the v13
   worker advertises 45 s. Two compatibility attempts (diagnostic mode and a
   v13 capability wrapper) loaded the model but hung during the first
   generation at ~97% GPU for over five minutes and were terminated to free the
   GPU. **No raw model JSON was captured.** The host NDJSON is the
   authoritative raw user-visible record. Details:
   `companion_evidence_2026-11-15/raw_worker/FAILED.md`.

The probe records the host's own status transitions and the production
`ModelAttemptRecord` start/end timestamps. No stage is invented. Debounce, QML
render, queueing under contention and apply/readback are not observable in this
headless path and are reported as gaps (§6).

### Identity

| Field | Value |
|---|---|
| Worktree HEAD | `d5c8101e42b45d5d3b0796c97f01024214fd5d57` |
| Main checkout HEAD | `d5c8101e42b45d5d3b0796c97f01024214fd5d57` |
| Branch | `topic/eval-2026-11-15` |
| Production paths modified | none (`git status -- src include app CMakeLists.txt` empty) |
| Native worker | `D:/Stardust/roclint/build/Release/roclint_granite_worker.exe`, SHA-256 `eb22f436…` |
| Host probe binary used | SHA-256 `ad5cf63d…` (recorded in `identity.json`) |
| Granite | `ibm-granite/granite-4.2-3b-GGUF`, Q8_0, revision `c40945d7…`, weights digest `fbe98673…` |
| Runtime configuration | `granite.vulkan.ctx8192.threads4.gpu99.t1.p095.s42.jinja.json0ws.rejection.review_polish.v13.context_quality` |
| Backend | Vulkan, RTX 4060 Laptop 8 GB, driver 617.14 |
| Sampling | temperature 1.0, top-p 0.95, seed 42; thinking off |
| Analysis / rewrite / clarification contracts | `granite.discovery.selective.v11` / `granite.rewrite.format.v9` / `granite.clarification_check.v1` |
| Julia | `SupersonicLabs/Julia-1`, revision `a85b1273…`, weights `df853bf7…`, configuration `julia.cuda.bf16.v2.threads8` |
| Julia Python | `D:/Stardust/roclint/build/runtime/julia-python/Scripts/python.exe`, Python 3.13.3, torch 2.13.0+cu130, **transformers 5.0.0** |
| OS / CPU | Windows 11 build 26300, AMD64, 16 logical CPUs |

The host baseline was captured with probe binary `ad5cf63d…`. Later builds of the
same new probe source add attempt event tags and grounding/index-readiness fields;
they do not change prompts, sampling or model calls. The raw worker stage was
attempted separately and did not complete.

Two environment findings are part of the baseline:
- The system `C:/Python313/python.exe` has transformers **5.14.1**, which cannot
  run the pinned Julia-1 model code (`ModernBertModel` no longer has
  `_update_attention_mask`; inference raises `AttributeError`). The documented
  isolated venv with transformers 5.0.0 was used. This is an environment drift,
  not a source change; a clean install needs the pinned environment.
- `build/Release/bin/roclint_granite_worker.exe` is older (Nov 13) than the
  test/host build and was rejected with "Native reply ID was invalid". The
  matching `build/Release/roclint_granite_worker.exe` was used. Staged copies
  must be kept in sync with the built protocol.

### Ambient load

CPU utilization was sampled every 2 s for the 399.6 s primary run: min 9.3%,
median **60.8%**, mean 61.7%, max 100%. Other Python/build processes were active
on the machine. The timings below are therefore **contended-host** values and
will be rerun once on a quiet machine. GPU utilization was 0–1% outside the
worker. This does not invalidate the content findings; it does inflate
readiness and generation times.

## 2. Primary baseline rows (automatic surface unless stated)

| # | Scenario | Class | Route / score | First usable question | Rewrite | Audit | Editorial |
|---|---|---|---|---|---|---:|---|
| 1 | music app: compact typography + 1990s/Revolver fusion + browsing | development | ORIGINAL, 0.1366 | none (40.49 s cold to "Ready") | none | — | **fail** |
| 2 | unseen recipe typography, 1970s cookbook, phone legibility | unseen | CLARIFY, 0.9985 | 16.84 s discovery (118.25 s cold total) | 10.58 s | Pass | **pass** |
| 3 | "make the product photo look cinematic" (automatic) | unseen | ORIGINAL, 0.0037 | none (1.07 s warm) | none | — | **fail** |
| 4 | same cinematic prompt (explicit review) | unseen | ORIGINAL, 0.0037 → explicit discovery | 12.06 s | 6.50 s | Pass | **fail** |
| 5 | code: t-digest for `p95LatencyMs`, preserve signature/API | unseen | CLARIFY, 0.9399 | 12.36 s | 10.86 s | Unresolved | **partial** |
| 6 | dashboard faster on slow laptops | development | CLARIFY, 0.8311 | 14.07 s | 6.87 s | Unresolved | **partial** |
| 7 | already-clear 500 ms / 95% / unchanged schema | unseen | CLARIFY, **0.9999** | 12.38 s (question) | none | — | **fail** |
| 8 | settings screen "calmer", custom color/spacing/order answer | unseen | CLARIFY, 0.9994 | 13.71 s | 8.46 s | Unresolved | **partial** |
| 9 | gibberish answer to onboarding prompt, then skip | unseen | CLARIFY, 0.9887 | 17.51 s | 16.88 s | Unresolved | **fail** |
| 10 | "I don't know" to mood-gallery question, then skip | unseen | CLARIFY, 0.7491 | 10.67 s | 7.07 s | Unresolved | **fail** |
| 11 | skip the confidence question, then rewrite | unseen | CLARIFY, 0.8250 | 12.85 s | 6.29 s | Unresolved | **fail** |
| 12 | changed preference (two-turn intended) | unseen | ORIGINAL, 0.0685 → explicit discovery | 14.06 s | 6.66 s | Pass | **not exercised** |

"First usable question" is the host READY transition with a non-empty question;
for original-route rows there is no question and the value is the READY time.
"Rewrite" is the synthesize request to a proposal visible as a host property.
All times are wall-clock on a contended CPU; no percentiles are claimed.

Explicit-review follow-up for the acceptance scenario (same prompt, same
planned custom answer):

| Scenario | Julia cold load | Granite cold warm | Discovery | Total to question | Rewrite | Audit | Editorial |
|---|---:|---:|---:|---:|---:|---|---|
| `music_app_typography_explicit` | 36.00 s | 64.20 s | 12.88 s | **113.30 s** | 13.14 s | Unresolved | **fail** |

The explicit follow-up exists because the automatic run produced no advice at
all. It is not a replacement row for #1.

### Stage distribution (warm process)

- Julia triage attempt: 0.32–1.03 s (prompt-triage latency 83–540 ms).
- Discovery (host attempt, includes native generation, parse and audit):
  **10.67–17.51 s, median 13.28 s**.
- Clarification check (answer audit): 3.84–11.91 s, median ≈5.42 s.
- Rewrite attempt (generation + audit + parse): **6.29–16.88 s, median 7.09 s**.
- Cold Granite warm: **64.20–100.86 s**; cold Julia load: **36.00–40.21 s**.
- Primary run wall time: 399.6 s for 12 scenarios (one cold process).

## 3. Top measured delay

**Cold model readiness before the first useful output.** In the explicit
follow-up, a fresh process spent **36.0 s loading Julia + 64.2 s warming
Granite**, then 12.9 s on discovery, for **113.3 s to the first question**. In
the primary process, Julia cold load was 40.2 s and the first Granite warm was
100.9 s, so the first Granite question came **158.9 s after process launch**
(40.2 + 100.9 + 16.8 discovery + ~1 s routing/startup). The component ranges are
Julia 36.0–40.2 s and Granite 64.2–100.9 s. They are ambient-contended; the split is real but the exact
seconds are not a quiet-machine guarantee.

The top *avoidable* delay for a single task is the **12.9 s spent generating a
question for the already-clear 500 ms requirement**, followed by an unusable
interruption. The acceptance scenario's automatic path is worse in product
terms: **40.5 s of cold wait ends with no usable suggestion**.

**Minimal recommendation (speed):** add the D00 trace first, then reduce cold
readiness as a separate, explicit state: prewarm at idle when the user has
selected native Granite, keep the model resident for the session, and do not
present "Ready to use" as if it were a useful result. Do not remove the
discovery/audit stages to shorten the warm 13 s / 7 s numbers; measure whether
prewarming actually moves the first usable timestamp. A "first token" event is
needed to distinguish model warm from generation.

## 4. Top intent failure

**The known acceptance prompt receives no typography help in the automatic
path.** Julia-1 scored "I'm building a music app. I want compact typography, a
fusion of 1990s and 1960s/Revolver-era references, and easy browsing." at
ambiguity **0.1366** (ACTION primary) and routed ORIGINAL. Granite was never
called; the host reported "Ready to use… Review prompt for optional help" after
40.5 s. This is the exact failure mode the plan warned about: a low classifier
score suppresses the useful interview.

Directly measured supporting failures:

1. **False-positive clarification of a settled value.** "Make the search API
   respond within 500 milliseconds for 95% of requests without changing the
   response schema." was classified **RULE_CONFLICT 0.9999** and asked "What
   system or context defines the 95% response time requirement?" with the
   options "Optimize database queries" and "Implement caching layer". The
   question and options do not answer each other, and the requirement is
   already testable. This is the clearest evidence that the route and discovery
   question are not aligned with intent quality.
2. **Explicit music-app rewrite drops an explicit prohibition.** The follow-up
   asked about the era fusion rather than what "compact" means, then produced:
   *"…smaller type sizes and tighter line spacing for dense lists, keeping
   artist and track names readable. Keep browsing one tap away. A fusion of
   1990s and 1960s/Revolver-era references should be the most important aspect
   of the 1990s/1960s fusion."* It omits "not fewer features or shortened
   labels" and appends a circular requirement the user never stated. The model
   audit flagged the missing words but not the circular clause.
3. **Skip and "I don't know" produce meta-instructions, not usable output.**
   After a rejected gibberish answer and skip, the proposal was the original
   instruction plus "Clarify what specific aspects…". After "I don't know" and
   skip, it was "Organize the photo gallery by mood. Determine the specific
   mood categories…". After a plain skip, the proposal was **identical to the
   original prompt**. Correctly, none was approvable; unhelpfully, none offered
   a labeled best effort.
4. **Model Pass is not usefulness.** The cinematic explicit rewrite passed the
   audit with "Make the product photo look cinematic with high drama." It
   replaced one vague adjective with another. Conversely, the code and settings
   rewrites were editorially faithful but blocked by the audit for missing
   surface words ("one", "keep", "exact", "header's"; "but"). The gate is both
   too permissive and too lexical.

**Minimal recommendation (intent):** do not lower the S1 threshold to chase
this case. Make the explicit "Explore this prompt" path immediate and obvious
when S1 routes ORIGINAL, and/or run a bounded optional-help discovery that
offers a question without pretending the original is settled. Add two
regression scenarios from this evidence: the music-app prompt must produce
typography-relevant options and preserve "not fewer features or shortened
labels"; the 500 ms prompt must not ask a question or offer implementation
tactics. Treat a missing negative constraint in a custom answer as a blocking
finding that returns to the interview, and separate semantic preservation from
missing function words in the audit.

Secondary, bounded recommendations:

- When a custom answer contains a prohibition, mark the omission blocking, not
  advisory; do not let question framing ("most important aspect") become a
  requirement.
- Replace lexical missing-token checks with evidence of each semantic
  constraint (target, signature, color, order); the code/settings rewrites are
  faithful and should not be blocked by "but" or "exact".
- For skip/unknown, return either a visible best-effort proposal with labeled
  assumptions or an explicit open-question state; never a no-op or a
  meta-instruction.
- The codebase question "are there existing t-digest functions?" should be
  answered from the indexed workspace, not the user. (The fixture is synthetic
  and the index-readiness evidence is limited; see §7.)

## 5. Mechanical and model results (kept separate)

- **Mechanical:** every executed discovery parsed and produced a question; every
  rewrite produced text; all attempts completed `SUCCEEDED`; no prompt/answer
  text was truncated in the recorded NDJSON. `canApprove` was true only for the
  unseen typography and cinematic explicit rows, and for the single-turn
  changed-preference row.
- **Model verdicts:** discovery/rewrite audits returned Pass for unseen
  typography, cinematic explicit and changed preference; Unresolved for code,
  performance, settings, gibberish, "don't know" and skip. The gibberish
  clarification check correctly rejected the answer; the "I don't know" check
  returned a restatement and did not confirm it. Pass did not predict editorial
  usefulness (cinematic explicit), and Unresolved did not predict uselessness
  (code/settings).
- **Raw responses:** `host_*.ndjson` retains every host question, option,
  answer, proposal and constraint verbatim; it is the authoritative raw
  user-visible record. The separate raw-worker stage did **not** complete (see
  §7), so the model's private JSON envelope is not retained for this baseline.
  The host does not expose that envelope; the two are not conflated.

Editorial judgments are in `judgments_2026-11-15.json`; summary: 1 pass,
3 partial, 8 fail, 1 not exercised. Best: unseen recipe typography. Worst:
automatic music-app (no help), already-clear 500 ms (false question), plain skip
(unchanged prompt as a proposal).

## 6. Proposed timing events

The full gap list and event proposal is
`companion_evidence_2026-11-15/timing_gap_proposal_2026-11-15.md`. The measured
spans above use only the D00-compatible status transitions and production
attempt records. Missing observations, in impact order:

1. UI debounce/capture before the host starts.
2. Queue wait when one worker is busy.
3. Granite cold-load internals (spawn, mmap, KV allocation, warm decode).
4. Julia cold-load internals.
5. First token / streaming preview timestamps.
6. Generation vs audit as separate spans.
7. Review as its own stage.
8. QML paint/visible timestamp.
9. Apply/insert/readback.
10. Cancellation/supersession latency.
11. The word popup chain (capture → debounce → generation → candidate audit →
    visible card).

The proposed additions use the existing `stage=model_ready|generate|audit|
review|visible|queue|debounce|apply`, with `detail` keys only. No prompt or
source text may enter the trace; failures must be best-effort and non-blocking.

## 7. Limitations and what was not run

- **Headless, not full app.** The probe calls `analyzePrompt`/`reviewPrompt`
  directly. It does not measure the 150 ms draft debounce, accessibility
  capture, QML layout/paint, focus/anchor, or the installed application. Host
  READY is not proof that a usable card was visible.
- **No apply/insert.** Approval, insertion readback, undo and Send/Enter
  behavior were not exercised. No user draft was touched.
- **No word-companion surface.** The word popup requires an observed native
  field/selection; that path was not driven. This baseline covers the
  interview/rewrite surface only.
- **Ambient contention.** CPU median 60.8%; other agent/process activity was
  present. A quiet rerun is planned; do not cite these seconds as quiet-host
  performance. The content findings are unaffected.
- **One run per scenario, temperature 1.0.** Generation is stochastic; no
  repeat distribution or percentile is claimed. Each parsed output is one
  sample.
- **Codebase/AST evidence is limited.** The primary code row used the synthetic
  fixture, but its `getProjectFiles` idle check returned in 255 ms and the
  attempt detail contains no `workspace_context` or AST field. A dedicated
  follow-up waited the full 180 s and `codebaseIndexReadyForTesting()` remained
  **false**; that run was then interrupted by a harness restart during Julia
  load. The code row therefore demonstrates target/preservation in the prompt
  and rewrite, **not** a verified AST authority path. Do not cite it as
  AST-grounding proof. This is a readiness/harness gap to resolve with the
  coordinator, not a reason to fake AST evidence.
- **Raw worker stage did not run to completion.** The existing raw checker is
  stale relative to the v13 worker (12 s vs 45 s capability) and the two
  compatibility attempts hung during generation; see
  `raw_worker/FAILED.md`. The host NDJSON is the raw user-visible record. Do not
  cite a raw-model JSON result for this baseline.
- **Environment drift.** System Python/transformers cannot run Julia-1; the
  documented venv was used. The older staged worker was rejected. Both are
  readiness/deployment findings, not model-quality findings.
- **No comparator.** No competitor model, skill or API credential was
  available. The comparison is **NOT RUN**; the protocol is in
  `comparison_protocol_2026-11-15.md`. Nothing here is a competitive claim.
- **Not run:** QML/product-alignment tests, full CTest/Python gates (the shared
  baseline already reports 65/65 CTests and 86 Python tests; they were not
  rerun because no product source changed), native macOS/Metal, RAG, thinking
  mode, multi-user/concurrent queueing, and a quiet rerun.

## 8. Reproduction

```powershell
# 1. Build the evaluation-only probe against the existing Release libraries.
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S D:\Stardust\roclint-wt-eval\tools\companion_probe `
  -B D:\Stardust\roclint-wt-eval\build-eval-probe -G "Visual Studio 17 2022" -A x64
& $cmake --build D:\Stardust\roclint-wt-eval\build-eval-probe --config Release --target companion_probe
Copy-Item -Recurse -Force D:\Stardust\roclint\build\Release\runtime `
  D:\Stardust\roclint-wt-eval\build-eval-probe\Release\runtime

# 2. Primary 12-scenario host baseline (one cold then warm process).
C:/Python313/python.exe D:\Stardust\roclint-wt-eval\tools\companion_eval\run_companion_eval.py `
  --stage host --out D:\Stardust\roclint-wt-eval\docs\audit\companion_evidence_2026-11-15

# 3. Explicit-review follow-up for the music-app acceptance prompt.
C:/Python313/python.exe D:\Stardust\roclint-wt-eval\tools\companion_eval\run_companion_eval.py `
  --stage host `
  --scenarios D:\Stardust\roclint-wt-eval\tests\fixtures\companion_eval\scenarios_explicit_followup_2026-11-15.json `
  --out D:\Stardust\roclint-wt-eval\docs\audit\companion_evidence_2026-11-15\followup_explicit

# 4. Attempted helper-only raw discovery JSON. The existing checker is stale
#    relative to the v13 worker and did not complete; see raw_worker/FAILED.md.
C:/Python313/python.exe D:\Stardust\roclint-wt-eval\tools\companion_eval\run_companion_eval.py `
  --stage raw --out D:\Stardust\roclint-wt-eval\docs\audit\companion_evidence_2026-11-15

# 5. Re-attach editorial judgments without rerunning the model.
C:/Python313/python.exe D:\Stardust\roclint-wt-eval\tools\companion_eval\run_companion_eval.py `
  --stage merge --out D:\Stardust\roclint-wt-eval\docs\audit\companion_evidence_2026-11-15
```

The probe requires `D:/Qt/6.8.2/msvc2022_64/bin` and the worker directory on
`PATH`. It uses the documented Julia venv and points the host at
`models/julia-1`, `models/granite-4.2-3b`, and the matching
`build/Release/roclint_granite_worker.exe`. The runner records the exact
settings, hashes, GPU state and ambient samples for each run.

## 9. CMake/manifest requests

None required for this evidence. `tools/companion_probe/` is a standalone
CMake project that links the main Release libraries read-only; it is deliberately
not a CTest because it loads models. If the coordinator wants it in-tree,
register it as an optional manual target (for example
`roclint_companion_probe`) and keep it out of the default test suite. No
production manifest, sampling or model-file change is requested.

## 10. Handoff summary

- Baseline evidence: `docs/audit/companion_evidence_2026-11-15/`.
- Harness: `tools/companion_probe/` + `tools/companion_eval/`;
  fixtures: `tests/fixtures/companion_eval/`.
- Top delay: cold readiness 100.2–141.1 s of model warming (Julia 36.0–40.2 s +
  Granite 64.2–100.9 s; 158.9 s from launch to the first Granite question in the
  primary process) plus 12.9–16.8 s discovery; warm discovery median 13.28 s,
  rewrite median 7.09 s. For the acceptance prompt the automatic path spends
  40.5 s and returns no help.
- Top intent failure: S1 suppresses the music-app interview (0.1366 → ORIGINAL);
  the explicit surface then drifts from "compact typography" to the era fusion
  and drops the user's negative constraint. The 500 ms requirement is a
  false-positive RULE_CONFLICT with incoherent options.
- Raw model JSON: not captured (helper/worker incompatibility, three attempts;
  see `raw_worker/FAILED.md`). Host NDJSON retains the raw user-visible
  questions, options, answers and proposals.
- Do not treat a model Pass, a valid schema or `canApprove=true` as proof of
  usefulness. The code/settings audits show the opposite failure (faithful
  rewrites blocked by function words), and the cinematic Pass is not useful.
- Comparison: NOT RUN. Quiet rerun: planned, not performed.
