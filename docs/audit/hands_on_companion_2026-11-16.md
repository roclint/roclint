# Installed companion hands-on session — November 16, 2026

Baseline: `topic/integration-2026-11-15`, commit `30929c91d2a481f5a68336d55b8916f7c7f0cf6f`.
Evidence: `D:\codex-evidence\roclint-hands-on-20261116`.

## Deployment and observed behavior

The installed application and small runtime files were updated from the verified
integration build, with SHA-256 comparisons and a backup recorded in
`installed-update-receipt.json`. Settings, history, models and Python environments
were retained. The installed Windows-platform dry-run passed with developer Qt
directories removed from PATH. An earlier offscreen dry-run crashed because this
Windows deployment contains `qwindows.dll`, not the offscreen platform plugin;
that failed check remains in the evidence directory.

Computer Use exercised the actual installed HUD and local models with a synthetic
music-app draft: small text, 1990s/1960s Beatles/Revolver inspiration, easy browsing.
These were live observations, not scripted model replies:

- Clicking **Rewrite prompt** from a fresh draft ended at READY/original wording,
  without a proposal. The separate **Improve wording** button was initially below
  the visible viewport. The first classification was observed complete by 47 s;
  this is an observation bound, not a precise inference latency.
- **Improve wording** warmed Granite and produced a conservative wording edit,
  observed complete by 60 s. It retained the vague small-text/style requirements;
  this was not a demonstration of deeper intent discovery.
- A manual independent reread failed immediately after the model had been
  released. Apply remained locked.
- Explicit **Clarify** did reach Granite and produced a visual-elements question
  with two alternatives. The custom-answer test did not accept gibberish, but the
  visible feedback was **model not ready**, so this does not establish live semantic
  rejection of gibberish.
- Notepad is classified as a document but is not on the ambient eligibility list.
  A blank test tab was created; existing documents were not edited. The existing
  disposable native fixture captured its field with **Ctrl+Shift+R**. Its captured
  Qt tool overlay was visible in a screenshot but was not exposed as a targetable
  window by the Computer Use helper; native word replacement/insertion remains
  unverified in this session. The fixture is not evidence of real Antigravity parity.

## Repairs

1. Keep **Rewrite prompt** available while automatic analysis is running; latch
   the user's request with a visible **Rewrite queued…** state and reuse that
   analysis instead of cancelling or duplicating it. Editing/cancelling clears
   the request. Preserve a requested rewrite through startup notifications before
   the new draft is committed. Compare committed revision identity only after
   analysis is ready. Optional questions can be deferred through the existing host
   command; blocking conflicts/details still prevent rewriting.
2. Warm and resume a cold native custom-answer check. Bind the pending action to
   version, workspace, revision, active question, configuration and request serial.
   A changed context cannot accept the earlier answer.
3. Warm and resume a cold independent reread. Bind to the complete proposal/revision
   identity, version, configuration and serial. A changed draft/proposal/workspace
   invalidates the pending review. Pending checks protect active work from idle
   residency transitions, without disabling the normal idle policy.

No answer is approved by warming the model. Existing semantic checks and approval
gates remain in force. No downstream prompt was submitted to an executing AI.

## Validation

- Explicit Release builds passed for the production host and every test linked
  against the modified controller. No default build/runtime-environment copy was
  needed.
- Complete enabled CTest suite: **65/65 passed**, 266.85 s; real embedding
  integration remains disabled. After the final QML queue change, both affected
  interaction suites were rebuilt and rerun: **2/2 passed**, 67.80 s.
- New native-protocol regression: cold custom-answer warm-up, unusable-answer
  rejection, cold independent review, one version advance and stale review after
  editing during load. It uses a synthetic subprocess response, not a real-model
  quality assertion.
- The early rewrite-continuation regression failed on the baseline and passed
  after repair. The final QML regression additionally starts an automatic check,
  requests a rewrite while it runs, checks the queued state, and verifies exactly
  one discovery and one unapproved rewrite.
- Structure check: zero errors. `git diff --check`: clean.
- Installed host SHA-256 matches the final built host; see
  `hands-on-fix-install.json`. The repaired installed app launched successfully.
  A final live request reached Granite from **Rewrite prompt**, without the
  separate **Improve wording** action. Cold loading is still slow.
- Final live follow-up: the music-app proposal was observed ready by 75 s after
  the explicit rewrite click. The actual Granite independent reread was observed
  complete by 28 s after its click, interpreted the original small-text/style/
  browsing requirements, and unlocked **Approve & Copy**. These are observation
  bounds including time between inspections, not exact generation timings. The
  app was left open on that unapproved review; no prompt was inserted or sent.
- Original failed command/test receipts are retained: wrong initial native target
  name and the test assertion that confused adapter readiness with native
  residency. These were corrected without weakening the production gates.

Changes are uncommitted and unpushed. This session does not claim a fresh Python
suite run; Python sources and dependencies were not changed.

## Product work still open

- Reduce cold model readiness time and expose preparation clearly.
- Put the requested rewrite and its review on a direct, visible path.
- Improve question/alternative relevance; the music example remains generic.
- Test actual word suggestions, range replacement, full insertion and stale-field
  recovery in supported real editors with a targetable UI.
- Run the planned downstream-artifact comparison against raw prompts and a skill
  baseline. This session does not establish superiority over either baseline.
- macOS, Metal and real embedding integration were not exercised.
