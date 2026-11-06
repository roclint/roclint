# Hands-on finding: vague goals bypass clarification

Date: 2026-11-06. Observed running Windows build: rewrite v7 at `92d142e`.
User confirmed draft editing/rewrite works, then reported too few clarity
options and a flow that mainly rewrites.

## Confirmed evidence

Read-only inspection of `build/manual-testing/session-20261106/history-fixed.sqlite3`:

- Exact frozen prompt: `Make Reader::open better.`
- Automatic Julia score: `0.58936189760699254`, below the `0.70` threshold.
- Saved automatic snapshot: no discovery attempt, no choice groups,
  `assistance_kind=0`.
- Saved rewrite: `Improve the Reader::open method to be more user-friendly and efficient.`
- The source prompt supplied neither user-friendliness nor efficiency as its
  desired goal. The rewrite selected goals instead of retaining the ambiguity.
- A later explicit deeper review of the edited Reader prompt did call Granite
  and produced two choices: clearer error messages or automatic format detection.
  Its confirmed choice was preserved in the next rewrite. This establishes that
  the question path works, while its routing/discoverability and option quality
  need improvement. Automatic format detection is a proposed feature, not a
  fact established by AST evidence or the original request.

Source contracts explain the behavior: automatic discovery requires Julia's
clarify route; explicit review can proceed with a low score. The discovery
prompt deliberately prefers zero/one question and two/three choices per group.
The existing policy is conservative, but the score missed this vague goal.

The user's screenshot is a Connect editor notification. It says the review is
retained while the user selects an insertion destination; it does not display
clarification choices or prove a clarification result.

## Next implementation requirements

1. Make user-requested clarification discoverable alongside rewriting, including
   after a quiet automatic check. A low classifier score must not hide that action.
2. For a vague code-improvement goal, offer a focused question about the outcome.
   Goal choices may include readability, reliability/error handling or performance;
   they must remain explicitly optional proposals rather than inferred requirements.
3. Provide a custom answer and an explicit skip/rewrite-as-is path. Do not force
   more questions merely to fill a quota, or select a suggested goal automatically.
4. A skipped/omitted goal stays unspecified in the rewrite. In particular, the
   Reader reproduction must not silently add efficiency, usability, file-format
   detection or a new implementation strategy.
5. Cover automatic quiet → explicit clarification → user choice/custom answer,
   plus skip → no invented goal, in controller/QML tests and real-model development
   checks. Retain immutable revisions, stale-result checks and explicit approval.

This is an observed finding and implementation brief. No routing, model,
threshold or UI behavior was changed during this inspection. The user's live
host was left running. Existing unrelated audit-file edits were preserved.
