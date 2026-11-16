# Companion guard remediation — November 15, 2026

This repairs the four independently reproduced findings on
`topic/integration-2026-11-15`, starting at `0913c63`.
It does not establish the larger product's speed, semantic quality, native insertion
or macOS parity. Source changes are local; the installed app is untouched.

## Findings and changes

1. **False conflict suppression.** Removed the automatic numeric fast path and its
   policy helper. A RULE_CONFLICT-only classification now reaches ordinary discovery
   and semantic audit even when the conservative deterministic extractor finds no
   conflict. An exact number is not proof that the complete prompt is settled.
   The host regression uses the actual extractor, a sealed synthetic conflict-only
   classifier result and the real discovery path for mutually incompatible timeout
   requirements. It asserts discovery runs and the conflict remains visible.
2. **Substring quantity comparison.** Replaced substring matching with whole parsed
   quantities and exact decimal normalization. Time aliases and exact millisecond/
   second conversions are supported; decimal and binary storage units are distinct.
   Signs, scientific notation and integer precision beyond a double's exact range
   are tested. Extremely large exponents fall back to literal-only matching.
   The quantity check establishes presence of values/dimensions, not semantic
   equivalence of complete requirements: subject, scope and conditions still need
   semantic review. The local policy version is now `settled_requirement.v2`.
3. **Legitimate task rejection.** Removed the verb/keyword meta-instruction veto.
   `Identify which endpoint ... and fix it` and `Identify which failures ... and
   explain ...` are ordinary task instructions. Proposed rewrites remain unapproved
   previews until the existing independent comprehension review succeeds and all
   other approval checks pass. Host regressions exercise both legitimate task
   paraphrases and an unrelated meta-instruction whose blocking review cannot
   authorize application. The latter uses a scripted review result; it is gate
   coverage, not new evidence of the model's linguistic judgment.
4. **Broken UTF-8 excerpts.** Source-clause clipping backs up to a UTF-8 boundary.
   Excerpt byte spans describe the exact emitted source substring. A `truncated`
   flag distinguishes clipped clause excerpts; the original revision remains
   authoritative. Tests sweep two-, three- and four-byte characters around the
   512-byte limit at a nonzero source offset and verify an exact-boundary case.

The number of tests/clauses is not used to silently clear semantic ambiguity.
The existing bounded excerpt size and default clause count remain; this change
does not claim that the brief contains every part of an arbitrarily long draft.

## Intentional behavior changes

- The previously reported 0.48-second numeric fast-path result is historical.
  Conflict-only numeric prompts may now invoke Granite; no equivalent latency or
  false-positive improvement is claimed without a fresh model measurement.
- Ambiguous output is not classified as meta output solely from words such as
  `identify`, `which` or `determine`. The independent semantic approval gate remains.
- Candidate shape, source provenance, stale-state checks, exact insertion rules,
  AST context/hash limits, cancellation and shared deadlines are unchanged.

## Verification

Final Windows validation on the repaired source:

| Gate | Result | Receipt |
|---|---|---|
| Explicit Release build: all registered test executables, host, worker, contracts | Exit 0 | `full-build.log` |
| Quantity/Unicode maintained regression suite | 1/1 passed | `quantity-unicode-tests.log` |
| Controller routing/review regressions | 1/1 passed, 38.54 s | `host-regressions.log` |
| Complete CTest | 65/65 enabled passed, 282.92 s; real embedding integration disabled | `full-ctest.log` |
| Python discovery | 86 run: 85 passed, one skipped; 38.970 s; exit 0 | `python-tests.log` |
| Structure | 94/94 original paths, zero errors | `structure.log` |
| Whitespace | `git diff --check` clean | final working-tree check |
| Production host offscreen startup, in-memory DB | Exit 0, explicitly waited for process exit | `dry-run-receipt.json`, stdout/stderr logs |

`guard-regression-output.txt` extracts the execution markers from CTest's actual
`LastTest.log`, including the production-controller conflict and review cases.
Evidence root: `D:/codex-evidence/roclint-integration-fixes-20261115`.

The first Python run lacked Qt's runtime directory on PATH, so the contracts
executable failed to load with status `0xC0000135`. That failed receipt is retained
as `python-tests-missing-runtime-path.log`. Adding the configured Qt bin directory
allowed the full Python run to pass; no source or assertion was changed to fix
that environment error. CTest supplies its own Qt runtime environment.

The fixes are recorded on the local integration branch. No shared-branch merge,
push, installed-app replacement or user draft change was performed.

The original independent reproductions are preserved separately at
`D:/codex-evidence/roclint-integration-review-20261115`.
No live model/GUI insertion or downstream Claude/Gemini comparison has been run by
this remediation. Existing Windows fixture results do not establish macOS parity.
