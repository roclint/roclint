# Word-action quality follow-up — November 16, 2026

## Outcome and current status

The default-off thinking profile now passes seven targeted real-Granite pipeline
cases. Clear spelling repairs no longer require answering an unnecessary question;
real intent questions remain available. The matching v15 host/worker are installed and Windows startup verification passed.
The live popup now displays the correct replacement with Apply enabled. Native
insertion/readback awaits one manual click because Computer Use does not expose
the non-activating popup as a targetable window. No commit or push has been made.

Worktree: `D:\Stardust\roclint-wt-integration`, branch
`topic/integration-2026-11-15`, baseline
`30929c91d2a481f5a68336d55b8916f7c7f0cf6f`.

## Changes

- Word prompt v4 distinguishes ordinary surface errors from consequential intent
  choices. Sentence audit v3 admits useful spelling/grammar repair while preserving
  names, literals, negations and other requirements.
- A question now receives at most one focused model surface check
  (`granite.word_surface_check.v1`). A repair still needs the independent
  full-sentence audit. A clean result retains the original intent question; a
  protected result suppresses the detour and must cite actual source text.
- The check uses the original transport ticket, shared deadline, cancellation,
  frozen field/span and revision bindings. It cannot approve, insert or send.
- Structural guards suppress word edits overlapping ASCII qualified identifiers
  (`::`, `->`, dotted access), backtick identifiers and double-quoted strings,
  even after false model approval. This is intentionally conservative: quoted
  stylistic terms are also withheld from word-only editing. A wider explicit
  review is needed to revisit them. It is not compiler name resolution or a
  general literal/proper-name detector.
- Guard scanning is linear rather than a backtracking identifier regex.
- Runtime configuration v15.word_actions_profile binds the matching host/worker
  instruction set. Model weights, runtime pin, sampling and reasoning budgets are
  unchanged. The user's thinking preference is unchanged.

No typo dictionary, fixed replacement list or canned clarification choices were
added. The corrections below are actual model output after production validation.

## Real-model verification

`roclint_native_granite_tests --live-word-corpus <model-dir> <worker>` explicitly
sets `thinkingEnabled=false`, matching the installed default. It uses pinned
Q8_0 through the native worker, production prompt/parser/surface-check helpers
and the independent complete-sentence audit. It observes no desktop fields,
opens no history database, performs no download, and never approves/inserts/sends.

Final run: **7/7, exit 0**, model readiness **47.908 s**.

| Case | Validated result | Request + any check/audit |
|---|---|---|
| Reported typo | `unchnaged` → `unchanged`, no question | 3.321 s |
| Held-out spelling | `accessable` → `accessible`, no question | 2.619 s |
| Grammar | `are` → `is`, no question | 1.799 s |
| Exact code name | No action; structural guard | 0.903 s |
| Exact literal label | No action; structural guard | 0.782 s |
| Performance | Asks which kind of speed | 1.135 s |
| Creative | Asks which aspect of cinematic style | 1.388 s |

These are single-machine observations, not p95 benchmarks or a startup
optimization. The readiness cost remains substantial. The ambiguity gate checks
that a decision is offered; it does not prove the questions are insightful.
Questions remain broad, explanations sometimes imprecise, and larger held-out
quality evaluation is still needed. No native macOS/Metal parity is established.

## Regression coverage

New adapter checks cover repair/clean/protected decisions, malformed fields,
unknown status, invented or insufficient source evidence, identical replacements,
invalid spans, false model approval of protected identifiers/quoted strings,
prose beside code and a long unqualified token. Host integration checks that an
unnecessary question can become a correction through exactly one focused check
and a complete-sentence audit, with no insertion until explicit Apply.

Final release build passed. Five rebuilt relevant CTest suites passed **5/5** in
**115.38 s**: Granite adapter, reasoning policy, native runtime, host controller and
product alignment. Structure check passed (94/94 original paths, zero errors);
`git diff --check` passed. The original UI build's 65/65 enabled suite is a separate
historical receipt; the full suite was not rerun after this follow-up.

## Live investigation and rejected results

The original installed v13 asked whether `unchnaged` meant `unchanged` or was a typo.
A v14 isolated run passed 7/7, but live testing still asked whether it was a literal
spelling. The probe had inherited `RequestContext`'s thinking-on default while the
host explicitly supplied its default-off preference. That mismatch is fixed in
the probe; the thinking-on timing/quality results do not describe the installed
default. Original failed outputs and receipts are retained.

A shorter single prompt fixed spelling but reduced ambiguous terms to synonyms;
it was rejected. A focused surface check preserves the separate intent task. The
first surface parser unnecessarily rejected a repair carrying an actual source
quote; it now accepts an empty or exact draft quotation for repair. Protected
results still require source provenance. Raw model outputs are retained so the
7/7 result cannot be mistaken for perfect first-pass model judgment.

Computer Use observed the installed v15 popup offering **unchnaged → unchanged**
with READY status and an enabled Apply button. It was present at the 54-second
observation after the cold request; this is an observation bound, not precise
inference latency. The editor still contained the original test text, proving no
automatic insertion. The popup was not returned by the fresh window inventory,
so a manual Apply click was requested. Exact native readback remains pending.
No executing-AI prompt has been sent.

The popup was visibly far from the caret in this high-DPI disposable fixture.
Field-rectangle fallback versus physical/logical coordinate conversion needs
measurement before claiming an anchoring fix. Cold load feedback, more specific
creative choices and a larger held-out quality corpus remain follow-up work.

## Evidence

`D:\codex-evidence\roclint-word-actions-20261116` contains build/CTest logs,
`live-word-corpus-final.jsonl`, matching trace and receipt, prior rejected corpus
runs, profile-mismatch observations and installation backups/checksums. The
optional fourth corpus argument loads experimental instructions and marks every
result `experimental_instructions=true`; it is not a production configuration.

The matching host, native worker (both installed locations), model manifest and
generated runtime configuration were installed together with SHA-256 verification.
The prior v14 files are backed up in `installed-backup-v14`; original UI and earlier
backups are retained. Installed host SHA-256:
`D778676DD91C0C053F516E79E9EAA7E2C6700EFB423CB8B717D336D58E49F9BC`.
Installed Windows-platform dry-run passed (explicit process exit 0), with developer
Qt paths removed. Settings, history, model assets and Python environments were
retained. The empty updated HUD was observed, then minimized for the synthetic
native-editor test.


## Manual Apply follow-up

The user could not see the first popup. Computer Use found the disposable editor
minimized and the main empty HUD visible. Restoring the editor, hiding the HUD
and requesting a fresh synthetic draft (`Please keep all existing tests unchnaged`)
restored READY / Apply with the correct `unchanged` candidate.

After the user reported clicking Apply, the native accessibility readback still
contained `Please keep all existing tests unchnaged`. Insertion is therefore
**not verified**; the click report alone is not success evidence. The initial
failure reason is unknown. The assistant-owned synthetic test host was restarted
with process-scoped capture/timing diagnostics and forced Qt stderr logging.
No production source, settings, history or model assets were changed for that
retry. `live-ui-apply-attempt1.json` records the failed readback, and the diagnostic
session/trace/output files are stored beside it.


The user subsequently reported that the text changed during the diagnostic
retry. Independent fresh readback of fixture 3934716 still returned
`Please keep all existing tests unchnaged`; fixture 131974 returned
`Make the dashboard feel faster`. No range-insertion completion was present in
the diagnostic log. The report and observed native state do not yet agree, so
this remains **unconfirmed**, not a passing insertion result. The user clarified that the corrected spelling appeared in the white editor
itself, rather than just the suggestion preview.


The Computer Use JavaScript/capture connection was reset and the known editor was
selected again from a fresh inventory. After hiding the covering empty HUD, the
unobstructed screenshot and fresh accessibility tree both still showed
`Please keep all existing tests unchnaged`. No range-insertion completion was
logged. Classification: **user-confirmed editor change; independent native
verification unresolved**. This does not prove a stale-readback cause, a native
insertion defect, or a focus-loss cause. The user's report is retained without
promoting it to an independently verified insertion pass. No source changes,
new test run, commit or push occurred during this verification follow-up.
`live-ui-apply-verification.json` records the distinct observations.
