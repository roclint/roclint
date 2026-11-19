# Source excerpts in automatic discovery

Keep the filename-only baseline for now. On three inspection requests, supplying the relevant implementation reduced unnecessary questions from 3/3 to 2/3. On three paired requests for a change, correct questions about the intended future behavior fell from 3/3 to 2/3.

| Function | Inspection with excerpt | Change request with excerpt |
| --- | --- | --- |
| findUniqueSpan | Offered the actual return value and an incorrect alternative as a user choice | Asked first versus last occurrence, as required |
| routePromptTriage | Asked what detail to explain | Asked what score range counts as borderline, missing the decision about whether to continue or ask |
| normalizeSha256Digest | Returned none when asked how uppercase is handled | Asked whether future policy should accept or reject uppercase |

Each pair used the same real source lines in both excerpt trials. The comparator contained the same frozen draft, filename, host, instructions, and schema. Only the excerpt arm added a source_excerpt field to the request object consumed by Granite. Case order alternated the first arm. Both arms used automatic one-question discovery on the same local release-configured Metal Q8_0 worker. All twelve replies passed the production parser. A useful question had to surface the named pending decision. Asking a different question counted as a miss.

These are hand-selected relevant excerpts, one decode per condition, one evaluator, two previously used prompt pairs and one new pair. This tests interpretation of supplied evidence in discovery, independently of S1 routing, automatic retrieval or editor presentation. The companion JSON records the prompts, snippets, outputs, validation and timing. Replay with build/runtime/julia-python/bin/python3 tools/probe_source_discovery.py from the checkout root.
