# Word companion review remediation and measured gaps

Date: 2026-11-13. Repository: `D:\Stardust\roclint`. Branch: `feat/roc-hud-refresh`.
Baseline HEAD: `c39d82140f0a2ab15337f174b3a134b7d41024fe`.
Changes are uncommitted; nothing was pushed. Existing retrieval, Julia, other UI work and research assets were preserved.

## Confirmed engineering findings and fixes

1. **Contradictory no-change contract.** The prompt and native JSON schema now permit an empty candidate array. The native schema has three coherent alternatives: no change, one/two direct refinements, or a clarification question without a forced replacement. The parser retains compatibility with legacy responses up to four candidates. An unchanged replacement remains invalid; it is never repaired into invented advice.
2. **Popup clearing without notification.** Clearing centrally invalidates the suggestion, stops the debounce, drops pending work, requests scoped cancellation and emits the property notification. Short/empty drafts, field loss, ambient disable, dismissal, workspace/pipeline invalidation and HUD opening clear pending/displayed work. A question-only result remains visible without an empty struck-through replacement preview. Application also refuses a response that still needs clarification.
3. **Overlapping superseded word requests.** There is one active generation and one replaceable pending snapshot, separate from the insertion guard. The 550 ms quiet period remains. Completion always releases inference counters, even for stale results. Native work has an optional request-scoped cancellation token; cancellation does not advance the global epoch or invalidate unrelated transports. Cancellation state is published before waking the caller. Latest work waits for READY rather than the ready flag alone, which is also true during BUSY. Shutdown waits for an active word operation to finish/cancel.
4. **UTF-16/UTF-8 selection mismatch.** The bridge now maps UTF-16 offsets to validated UTF-8 boundaries before slicing or validating spans. It rejects a selection inside a surrogate pair and invalid UTF-8. Nearby context also ends on UTF-8 boundaries.
5. **Structure/test registration.** Reviewed and recorded the separate retrieval interfaces, implementation and test directories, plus the documentation-only DeepSeek handoff directory, in the alignment manifest. Upstream and preserved specification hashes were retained. Registered the previously orphaned adversarial fixture source as a separate CTest target, removed machine-specific fixture seed paths, and replaced a Release-disabled assertion inside concurrent mock embedding calls with an always-on failure count. These checks validate fixture behavior; they are not production retrieval or real-model evidence.
6. **Previously masked test coverage.** AST freshness and upstream compatibility fixtures classified every `<instructions>` block as a rewrite. Discovery audits therefore fired rewrite mutations early, inflated call counts or consumed the wrong timing budget. Fixtures now route audit and rewrite contracts explicitly. Assertions, including deadline limits and stale-proposal rejection, were retained. Timeout diagnostics identify the call site.

## Validation

Final verification:

| Gate | Result |
| --- | --- |
| Full desktop Release build (native Vulkan enabled) | PASS |
| Full CTest run before adding the fixture-only target | 63/63 enabled gates PASS; one real embedding gate disabled; 260.61 s |
| Newly registered retrieval fixture stress | 1/1 PASS; 1.31 s |
| Python Julia worker unit tests | 39/39 PASS |
| Structure | 94/94 original paths; zero errors; checker regression tests 11/11 PASS |
| Staged application `--dry-run` | Exit 0; QML root verified; process exited |
| Live native Windows provider fixture | Exact insertion/readback and stale-request refusal PASS |
| Real Q8 word responses through production parser | 6/6 structurally valid; semantic quality remains mixed |

This is 64 enabled CTest gates exercised across the full run and the subsequent newly added fixture target, not a claim that the disabled embedding-model integration ran.

The host regressions cover selected candidate identity, stale versions/text, double clicks, clarification-only results, quiet no-change results, short/empty field clearing, ambient disable, field loss, coalescing and shutdown during active generation. A rapid-edit burst executes the initial and latest requests only (`calls=2`, `peak=1`); the subsequent shutdown case starts a third request and verifies that quit waits for it without publishing a stale suggestion.

Native protocol coverage verifies scoped cancellation and reuse of a previously frozen unrelated transport. Existing cancellation, recovery, residency, integrity and telemetry checks remain enabled.

The original full run exposed three failures. Workspace inspection passed unchanged on an idle rerun after its 500 ms helper deadline had expired during concurrent build activity. The other two failures exposed the outdated audit-routing fixtures described above. The final verification is run without a concurrent build or model benchmark; earlier failures remain in the evidence directory.

## Live Windows insertion

A disposable Win32 EDIT control was exercised through the real Windows accessibility bridge, not a mock. Its executable was named `AntigravityRangeFixture.exe` to enter the existing supported-app policy; this is a controlled provider fixture, not proof of Antigravity itself.

```
request=1 outcome=INJECTED
exact_native_readback=1 unicode_prefix=1
request=2 outcome=FIELD_CHANGED
```

The draft contained an emoji and an accented character before the selected word. Exact post-write text was observed; repeating the request against the old frozen draft was refused. No existing editor draft was changed and no Enter/Send was pressed. Computer Use could select the isolated fixture after an initial executable-name ownership mismatch; the fixture was closed after testing. This closes the missing native-provider smoke evidence, not the full IDE/provider matrix.

## Real Granite measurements and quality

The build cache had both desktop/native build paths disabled during earlier work. Validation restored `BUILD_DESKTOP=ON`, `ROCLINT_BUILD_NATIVE_GRANITE=ON` and retained the already-selected VULKAN backend. The generated manifest and compiled backend now agree. No global user model setting or model manifest sampling default was changed.

Final real-model probe used pinned Granite Q8_0, Vulkan on NVIDIA GeForce RTX 4060 Laptop GPU, the production 24-second operation deadline, seed/sampling from the manifest, and no diagnostic deadline bypass. Six exact prompts were built by the production adapter and their raw responses were checked by its production parser: **6/6 parsed**. Explicit `500 milliseconds` returned no suggestion. Examples came from unrelated tasks; no word-to-answer lookup or fallback menu was added.

Measured model-request wall time: **1.327–5.637 seconds**, median **4.865 seconds**. These numbers exclude the host's 550 ms debounce and the initial model load. Cold load, including integrity verification and native warmup, was **65.081 seconds**. These are a six-case local measurement, not a latency distribution or instant-interaction claim. Earlier CPU experiments include deadline failures under build load and are not comparable performance baselines.

**Semantic quality is not complete.** The model generated distinct responses, but valid JSON is not proof of a useful local edit. `compact -> convenient` failed the qualitative check; some other candidates remain broad or fit their sentence awkwardly. The renderer/parser enforce identity and safety, not linguistic correctness. The revised instructions, coherent schema and generic examples improve the contract but do not constitute a validated semantic-quality gate. A further model-quality effort must judge specificity, sentence fit, unsupported assumptions and unnecessary questions on a larger held-out corpus. This report does not claim that gap is closed.

## Boundaries

- Saved-file AST grounding and approval semantics are unchanged.
- Non-native transports are serialized and stale results discarded; scoped interruption is implemented for native Granite. Other transports retain their existing bounded timeout.
- Actual IDE-specific UIA selection/fallback, focus-loss and clipboard behavior require provider-matrix testing; the live fixture primarily exercises native ValuePattern replacement and exact readback.
- Real retrieval embedding assets remain unavailable; that integration gate stays disabled. No mock result is presented as a real embedding-model run.
- macOS build, native insertion and Metal were not run.
- The development build was rebuilt/staged and launched only for its offscreen `--dry-run`, which exited with code 0. The installed app was not replaced or launched.

Evidence: `D:\codex-evidence\roclint-word-fixes-20261113` (build logs, CTest logs, live insertion harness/receipt, exact prompt/schema dump, raw model JSON and parser receipt). Original reproductions: `D:\codex-evidence\roclint-deepseek-review-20261113`.
