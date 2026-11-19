# Bonsai 2 9B word-flow comparison — November 17, 2026

Decision: do not replace Roclint's installed Granite model on this evidence.
This is a helper-only synthetic evaluation, not native app/AST/RAG parity.

## Frozen setup

- Worktree: D:\Stardust\roclint-wt-integration; topic/integration-2026-11-15.
- Bonsai repo: https://huggingface.co/prism-ml/Ternary-Bonsai-2-9B-gguf
- Model revision: 82265ccc78ddaac4700286a35e9badcc32376dd9.
- PQ2_0 model: 2,416,851,840 bytes; SHA-256
  7a89ab48f5038c704480f60781f9b8d4035351f5f6beb7d11fad80001fde23f9.
- Runtime: Prism prism-b10828-a153697 Windows x64 Vulkan; version reports
  a15369706. Archive SHA-256
  a07f03ae262b814558b22d747b46e3add0607947838e0764589be85aa1442461.
- Both downloads independently matched their published digests.
- NVIDIA RTX 4060 Laptop, explicitly Vulkan0; context 8192, one slot,
  thinking off, output cap 256. Model-author non-thinking sampling:
  temperature .7, top-p .8, top-k 20, min-p 0, presence penalty 1.5,
  repeat penalty 1, last-n 64; seed 42. No vision projector or speculation.
- Granite: existing pinned 4.2-3B Q8_0 and production worker/settings.
- Same production v4 word prompt/schema and C++ response parser, with the
  five saved synthetic cases and explicit valid/invalid answers. Different
  runtimes and author-recommended sampling mean this is not a controlled
  parameter-count-only comparison. No private editor drafts were sent.

## Actual outcomes

| Case | Granite 3B | Bonsai 9B | Assessment of Bonsai |
|---|---|---|---|
| Compact music-app typography | Size vs layout-efficiency question | Replaces compact with dense | Unconfirmed interpretation; not a useful font choice |
| Cinematic fern photograph | Style vs medium question | Film grain/dark tones vs dramatic lighting question | More concrete, but limited alternatives |
| Less intrusive assistant | Asks about changing the term's tone | Replaces intrusive with non-intrusive | Creates less non-intrusive, reversing the requested direction |
| Immersive exhibit description | Broad engagement question | Visitor sensory experience vs narrative question | Still risks confusing a description with a physical exhibit |
| Airy dashboard, retain all controls | Literal vs descriptive question | Low visual weight vs whitespace question | More relevant presentation choices |
| Gibberish clarification | Rejected | Rejected | Expected behavior |
| Refreshing displayed data clarification | Accepted | Accepted | Expected behavior |
| Local wording after that accepted answer | Re-asks which interaction | Re-asks loading/animation/responsiveness | Both fail to use a settled decision |

All five raw word outputs parsed for each model. That is not a quality pass.
A separate fresh Bonsai sentence-audit call rejected each of its two proposed
replacements: dense failed preservation/usefulness; less non-intrusive failed
grammar/usefulness. The latter incorrectly passed preservation, so even this
successful rejection is not evidence of perfect semantic judgment. The app's
existing rule requires every audit criterion to pass before an edit is offered.
This helper captured actual audit JSON; it did not insert anything into an editor.

## Timing observations

Bonsai server readiness: 12.70 s. Its first question/replacement request took
29.96 s; later four word requests took 1.38–1.56 s. Warm reported decode rates
were about 49–52 tokens/s. Granite's five word calls took 0.89–1.23 s.
Bonsai's valid and invalid answer checks took 1.44 and 2.16 s, respectively;
its repeated-question response took 1.58 s. Independent replacement audits
took 1.10–1.45 s. These are individual observations with background build/test
activity, not p95 statistics, controlled cold-cache comparisons or UX guarantees.
The initial request cost must not be hidden in a warm throughput claim.

## Artifacts and next gate

Evidence: D:\codex-evidence\roclint-word-flow-20261116.
Files: bonsai-download-spec.json, bonsai-preparation.json,
bonsai-runtime-release.json, bonsai-word-flow.json, bonsai-word-audit.json,
granite-word-flow-final.json, native-final-verification.json.
Scripts in the same evidence directory retain the exact launch arguments,
requests, responses, timing metadata and local-server cleanup.

Keep the selected model isolated. Before any promotion, compare accepted-answer
incorporation and complete-sentence usefulness on unseen examples, then test
real editor interaction, cancellation, structured-output and approval binding.
A model with more parameters does not fix lost question context or a UI dead end;
those host/contract fixes are recorded separately in word_flow_2026-11-17.md.
