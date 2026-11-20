# Conflict routing pilot, November 19, 2026

On this 17-prompt replay, evidence verification removed seven incorrect conflict labels. Existing Granite discovery already returned zero explicit conflicts on clear prompts. The verifier produced clearer questions for real conflicts, but added a median 12.03 seconds per checked prompt and left the same three unnecessary discovery questions eligible. The pilot favors retaining signal attribution for diagnosis and keeping the additional verifier experimental.

| Measure | Current | Separate reasons | Separate reasons + evidence |
| --- | ---: | ---: | ---: |
| Clear prompts carrying a conflict label | 7/10 | 6/10 | 0/10 |
| True conflicts carrying a conflict label | 4/5 | 3/5 | 4/5 |
| Clear prompts eligible for further review after each policy | 7/10 | 7/10 | 5/10 |
| Unnecessary question candidates on clear prompts | 3/10 | 3/10 | 3/10 |
| Security cases explicitly flagged by the security head | 0/2 | 0/2 | 0/2 |
| Security cases routed to any further review | 1/2 | 1/2 | 1/2 |

S1 routes and reasons are distinct from final discovery output. The security head also fired on two clear prompts and two conflicts whose expected disposition was solely conflict. Current routing folds the security score into conflict. Separate routing preserves every qualifying signal and the same eligibility for review. The evidence policy calls Granite on any current conflict candidate, accepts two distinct literal quotes plus a question for an explicit conflict, and clears only the conflict reason on a compatible verdict. Other reasons retain the same discovery output. A lower count of eligible prompts after verification does not imply fewer model calls.

## Cases that explain the decision

- The clear parser before/after request returned no issue in existing discovery. The genuine contradictory version returned a conflict description. The verifier identified both correctly and asked which empty-input behavior to implement.
- The archived pure-XML/pure-JSON contradiction never crossed the S1 review threshold. Downstream verification could not recover it.
- Both original sentiment benchmark formulations reached discovery. One asked for feedback text already present. The other asked the user to classify the review themselves. Verification cleared the conflict label, but rule or action ambiguity independently kept both questions eligible. The third unnecessary question asked the developer which CRC algorithm a named function implemented, effectively handing the requested investigation back to the user.
- The security head missed both security controls. The invoice-access prompt reached discovery through other signals. Its question asked for the scope even though all invoice reads were specified. The public diagnostics file prompt received no review.

## Method and limits

Ten clear prompts, five conflicts and two security cases. Ten authored controls had labels fixed before scoring, five cases used pre-existing repository labels and two sentiment prompts replayed prior known failures. No independent human panel adjudicated labels or questions. The scope question on the invoice case is additional to the three unnecessary questions on *clear* prompts. The measured sample is deliberately enriched for failures and does not estimate frequency in everyday typing.

Used the local pinned Julia-1 worker on CPU with extended questions and empty workspace facts. Used native Granite Q8_0 on Metal with the built configuration and release sampling settings. The discovery contract used automatic review, technical mode and a one-question limit. Each candidate used identical text and shared discovery output across policies. All 12 discovery outputs passed the production response parser. The experiment compared helper outputs and policy projections. It did not exercise the editor, host preflight, live index, or screen rendering. Counts of questions refer to generated candidates eligible under those policies.

Verification ran 12 times. The nearest-rank p95 was 14.66 seconds, the slowest observation at this sample size. Timings measure warm worker request/response and exclude startup. Two sentiment replays used a second resident session. An initial invalid verifier run omitted the prompt from Granite's rendered instructions. Its outputs and timings were discarded.

The runner can be replayed from the repository root with `build/runtime/julia-python/bin/python3 tools/probe_conflict_routing.py`. It uses local weights in `~/models/s1` and `~/models/s2` and writes `build/conflict-routing-pilot.json`. The archived JSON beside this report contains the prompts, labels, contributing scores, reason sets, generated outputs, validation and timings.
