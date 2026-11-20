# Single-call discovery comparison

**Keep current automatic discovery.** The candidate reduced unnecessary question candidates by one on the frozen labels and suppressed three decisions that the current instruction identified correctly.

| Primary sample | Current | Candidate |
| --- | ---: | ---: |
| Unnecessary question candidates on seven clear drafts | 5/7 | 4/7 |
| Genuine decisions or conflicts surfaced on eight drafts | 8/8 | 5/8 |
| Genuine decisions silently suppressed | 0/8 | 3/8 |
| Median native discovery latency across 15 drafts | 12.12 s | 11.49 s |

### What changed

- On the two new code-reading requests, the candidate correctly went quiet for routePromptTriage. For findUniqueSpan it handed the developer the exact question they had asked Roclint to investigate. Both answers can be found in the named source files.
- On the archived questions it went quiet on one sentiment example, but still requested already-present text on the other. On the CRC prompt, it again asked the developer to supply the requested explanation.
- Three genuine decisions disappeared completely. The candidate returned none for a choice between approved 12- and 30-second timeouts, a requirement for a single output to be both pure JSON and solely a Markdown table, and a requirement to both include and omit a field when its value is unknown. Current discovery returned an on-target choice or a correct conflict description in every case.
- One new case asked for a JSON summary with an old Markdown example. We labeled it clear before scoring its format requirement. On review, the candidate's question about which details to summarize could be justified because the source data were unspecified. Excluding this disputed prompt yields 5/6 unnecessary questions for current and 3/6 for the candidate. The three missed genuine decisions are unaffected.
- Among the eight new drafts alone, both arms asked on two of four drafts labeled clear. Current surfaced all four open decisions, and the candidate surfaced two. On the seven prior drafts the corresponding counts were three versus two unnecessary questions and four versus three genuine decisions surfaced.

### Method

The tested checkout was db29461. The benchmark asks the built production contract tool to generate automatic discovery instructions with technical mode, empty workspace facts and a maximum of one question. The current instructions already allow a compact none response. The candidate inserts a single paragraph immediately before the unchanged request object. It tells Granite to check whether the answer is supplied, discoverable in named code, or the requested work itself; it also instructs it to preserve consequential policy choices and conflicts.

Both arms received the same complete draft, base contract, output schema, pinned Q8_0 model, reasoning budget and release sampling configuration. One native Metal call per arm per case, paired in alternating order, bypassing S1 to isolate discovery. Inputs and intended decisions were frozen before model output. Fresh file-reading examples reference real code, but the helper had no file access or file contents. Successful handling of these cases means knowing when to leave the investigation to the downstream agent.

A question or choice on a clear draft counts as a potential interruption. For an open decision, an on-topic question or accurate conflict description counts as surfaced. All 30 primary outputs passed the production response parser. Neither parse validity nor a generic question counts as correct conflict detection. Timings include inference and exclude load. This is a purpose-built sample with a single evaluator, not an estimate of developer-wide error rates. No live editor was instrumented and production behavior was unchanged.

### Recheck

In a fresh worker session with the arm order reversed for each missed decision, current surfaced all three and the candidate again returned none for all three. Both arms stayed quiet on a separate JSON-summary control that supplied the actual build status and all required fields. All eight recheck outputs passed the parser. These fixed-seed replays check repeatability, not generalization. The companion JSON contains all 38 receipts. Run tools/probe_single_call_discovery.py with the built Julia Python environment for the primary set, and pass --recheck for the follow-up.
