# Context quality remediation — November 14, 2026

The acceptance target is useful, faithful assistance rather than identical generated wording.
This change addresses entity/constraint separation, proposal provenance, bounded recovery,
and complete-context validation of local word edits. It does not claim that every vague
prompt now receives useful assistance.

## Entity and constraint separation

`CanonicalIntent` now distinguishes `CodeEntity` from `GeneralConstraint` and retains the
complete explicit source clause, including preservation and negation. `Make` is recognized
as an action; explicitly named lower-case entities and quoted references are supported.
General constraints remain in discovery/rewrite input even when AST context exists. They
are excluded from AST lookups, rendered symbol evidence, and AST snapshot hashing. Negative
addition constraints have a prohibited value rather than the inverted generic required value.
The node-ID algorithm and proposal v1/v2 hash domains are unchanged; correcting an
extracted negative value can change that intent's identifier.

This remains conservative syntactic extraction, not a natural-language parser or compiler.
Explicit code spelling is stronger evidence than generic prose; unsupported declarators
continue to fail closed. Tests cover multiple verbs and roles, not model response strings.

## Creative proposal contract

Discovery v11 uses a compact wire contract: one literal `source` quotation per group,
option `text`/`effect`, and a separately disclosed `assume` premise. The adapter expands
this into the existing evidence spans and `proposed_assumptions` metadata. Added premises are displayed as proposed assumptions and sealed in
alternative metadata. A rewrite receives that metadata only for an explicitly selected
alternative. Goldfish sees standalone missing-decision questions and proposed assumptions as well as the actual source quotations and proposal metadata and is told
that a creative direction need not already be a user requirement. Provenance and explicit
constraint checks remain mandatory. Independent dimensions may use the same source phrase.

Malformed or ungrounded discovery gets at most one contract-repair attempt. Semantic
Goldfish regeneration remains separately bounded to one attempt and cannot silently change
optional assistance into a required clarification. All native calls share the host's
original monotonic deadline; repair does not reset it. Expiration fails visibly rather than
manufacturing an empty successful review. Short host budgets constrain discovery to one
question with two alternatives and concise questions/labels; longer budgets allow
up to two. Output reservations are bounded independently of private reasoning. Source quotations are
limited to 64 bytes without cutting tokens or inventing excerpts. Native/default automatic
and inline work retain a 20-second group budget; explicit review gets 45 seconds and one
question per round, reflecting measured Q8 generation time. Experimental thinking retains
its existing 90-second group policy and can allow two questions. Custom test deadlines stay
unchanged. Word budgets do not inherit a previous explicit HUD review's longer allowance.

## Complete-context word validation

After candidate generation and span validation, a separate cold audit receives each
host-constructed complete edited draft, its original, disclosed assumptions, confirmed
choices, explicit constraints, and available AST context. It checks grammar, preservation,
and practical usefulness. The compact word-v2 contract omits repeated drafts/spans and unused response arrays. Its
keep decision must supply a nonempty reason and an actual quotation from the frozen draft
or an approved decision, beyond merely repeating the selected word. Provenance is necessary
but does not prove that the model's explanation is semantically sufficient. The prompt asks
whether different competent implementers could produce materially different outcomes; an
understandable dictionary meaning is not enough to call a broad instruction settled. The
host binds each replacement to the frozen selected span. The word-audit-v2 contract requires
separate grammar, preservation and usefulness verdicts. Only exact host-minted candidate IDs
with Pass on all three survive. A conservative English determiner-collision check additionally
withholds unsafe splice pairs even if the model returns Pass. It never repairs a candidate
into an unrequested broader edit. Duplicate,
unknown, omitted, malformed, unavailable, timed-out, Fail, and Unresolved verdicts cannot
authorize a candidate. Generation and audit share one deadline and existing cancellation,
revision/workspace/field bindings, and exact range insertion checks. No-op and question-only
responses do not incur a candidate audit. There are at most two candidates per request.

A semantic model audit is an additional quality gate, not proof of English correctness or
user intent. Regression tests verify complete-draft construction and fail-closed handling;
a small live-model challenge is recorded separately below. No golden rewritten phrase is
required in the tests. The local determiner guard is deliberately conservative: valid
idioms or quoted uses can require a wider companion edit. It is not a general grammar checker.

The initial v10 live challenge demonstrated that a generic model Pass was insufficient:
it accepted `Apply an a clear, airy atmosphere treatment`. One discovery response also
took 36.89 seconds, exceeding the default host budget. Those receipts are retained as
`v10-live-results.json` and `v10-live.log`; the compact contracts and splice gate were added
in response. The final measurements below supersede that candidate, not the historical receipt.

## Evaluation correction

The earlier direct-helper smoke used the no-argument discovery schema, while the actual
HUD already used the prompt-bound quotation schema. Its fabricated quotations therefore
were not evidence that the HUD grammar allowed arbitrary quotations. The contract tool
now uses the same prompt-bound schema. Source-quote validation has not been weakened.

## Test harness reliability

The full run exposed an existing waveform-fixture dependency on active QML bindings.
Synthetic telemetry now uses `QQmlProperty::write` and explicitly emits a host metrics
notification after setting its phase; all existing rate, phase and visual assertions remain.
Qt documents the binding distinction in its
[C++/QML integration guide](https://doc.qt.io/qt-6.8/qtqml-cppintegration-interactqmlfromcpp.html).
The selective-review and typing-debounce tests now expect two calls for unusable replies:
initial attempt plus bounded recovery. They still require no additional automatic retry
and preserve the existing Unavailable/failure presentation. The QML harness checks Qt's
failure flag after each named case, so a failed QTRY assertion cannot silently return and
produce unrelated PASS lines before only failing at the end of the process.

A deferred-rewrite test exposed a test-only idle-wait race: the helper could dispatch
queued work during its final event pump and return without rechecking busy state. It now
pumps events first and requires two idle turns within the original timeout. A dedicated
regression schedules delayed model work through a zero-delay timer. The explicit deferred
rewrite case then passed five consecutive runs (6.01 s total); its original 3-second
deadline and proposal assertion remain. Production scheduling was not changed by this fix.

## Reference knowledge

A named reference is distinct from its contents. Generation and Goldfish instructions now
require reference-specific claims to be treated as unverified when actual source content is
absent. Tentative aesthetic proposals can be offered, but must not be represented as quoted
source facts. This task does not add RAG or claim that a model audit proves factual accuracy.

The intermediate compact-v11 run still produced unconditional keep responses, a discovery
round exceeding 20 seconds, and unverified reference-specific claims. Those receipts remain
in `v11-live-results.json` / `v11-live.log`. The intermediate configuration `v12.context_quality`
added keep evidence, the explicit-review budget, short source quotes, and reference-content
instructions. Final configuration `v13.context_quality` also binds native keep quotations to frozen draft
excerpts and approved decisions in the generation schema. All native measurements use the
same pinned Q8 weights and thinking disabled.

## Verification

Final model configuration: `v13.context_quality`, pinned Q8_0 / Vulkan, temperature 1,
top-p 0.95, seed 42, thinking disabled (all semantic responses report zero reasoning tokens).
Seven synthetic cases used the production builders and parsers through a direct owned
native helper. No draft was approved, inserted, or sent; no RAG model was downloaded.
The owned evaluation helper shut down afterward.

| Synthetic check | Observed result | Native generation / audit |
|---|---|---|
| Malformed `Apply an a clear…` plus a grammatical alternative | Auditor rejects malformed candidate; retains `airy, softly lit`; a separate regression also blocks a false model Pass | 6.796 s audit |
| Faster dashboard on slow laptops | Asks about frame times versus UI responsiveness | 3.093 s |
| Compact typography | Asks whether compact means font size or visual density | 2.684 s |
| Cinematic product photo | Asks about dramatic lighting, depth of field, or color grading | 3.431 s |
| Exactly 500 milliseconds | Evidence-bound keep; no candidate or question | 2.518 s |
| Cinematic potted plant discovery | Mood question, meditative versus vibrant proposals; exact source quote; audit Pass | 10.079 + 2.629 = 12.708 s |
| Music app / compact text / reference fusion | Asks what compact text means; options drift toward UI reduction and short labels; audit Pass | 9.128 + 2.495 = 11.623 s |

These timings exclude cold model loading, Julia, UI debounce, and full HUD integration;
they do not establish an end-to-end latency guarantee. Native host tests separately cover
shared deadlines, cancellation, binding and the candidate-audit sequence. This small
regression probe is not a held-out quality score or evidence that Granite always understands
intent. The music-app options still need human editorial improvement; the model's Pass and
the mechanical policy's OK do not establish expert-quality advice. Some wording, including
frame-time terminology, would benefit from plainer explanations.

The final AST-only property guard was added after the live model pass: `function behavior`
and `function signature` stay constraints even when actual symbols with those names exist.
It is covered by extraction and real-index regressions; the wire contracts did not change.

Evidence root: `D:\codex-evidence\roclint-context-quality-20261114`.
`live-results.json` / `live-summary.json` retain final responses and timings;
`source-receipt.json` identifies the live-check source/binaries, and
`source-receipt-final.json` identifies the final build. Intermediate v10/v11/v12 observations
remain available. `harness-protocol-error.log` records an initial evaluator binding error;
it was corrected before successful semantic measurement. An early UI link failure caused
by running the old test binary during relink was corrected; that isolated old-binary result
is not final verification evidence.

Final Windows gates on the final source/build:

| Gate | Result | Receipt |
|---|---|---|
| Explicit Release build, host/worker/contracts and all test executables | Exit 0 | `handoff-build.log` |
| Complete CTest run | 65/65 enabled passed, 297.39 s; real-embedding integration disabled | `handoff-ctest.log` |
| Python unit discovery | 86 tests, one skipped, 50.334 s; exit 0 | `python-tests.log` |
| Structure check | Exit 0, no errors | `structure-final.log` |
| Whitespace check | `git diff --check` exit 0; LF/CRLF notices only | `diff-check-final.log` |
| Staged host offscreen startup with in-memory DB | `--dry-run --db :memory:` exit 0 | `staged-dry-run.log` |

The host, native worker, model manifest and generated runtime configuration were copied
into the existing self-contained `build/Release/bin` staging directory. Each copied file's
SHA-256 matches its build/source input (`staging-receipt.json`). The generated and staged
configuration selects `v13.context_quality`. The large Julia/Python environment was not
recopied. This dry run establishes startup only, not interactive HUD or native insertion.

The installed app under `C:/Users/kirby/AppData/Local/Programs/Roclint` remains the earlier
build and was left running to preserve the user's session. No installer update, commit or
push was performed. Native macOS/Metal checks and real-embedding integration were not run.



An earlier full run passed 64/65 enabled tests, with a standalone watcher atomic-save
commit failure. Its production watcher was unchanged. The test now prints `errorString()`
if a commit fails; no retry or relaxed assertion was added. Five consecutive isolated
runs passed (26.22 s total). The original failure's cause is unconfirmed; these repeat
passes do not establish that its underlying intermittent condition was fixed.
