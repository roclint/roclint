# C06 — Intent, rewrite, and approval contract

## ConfirmedIntent

A `ConfirmedIntent`/intent version is bound to:

- exact source `AnalysisSnapshot` ID/hash;
- exact prompt revision;
- explicit visible/selected/confirmed/rejected/unresolved item IDs and values;
- custom user-authored intent additions/corrections.

## RewriteProposal

Granite rewrite is bound to exact revision + confirmed intent + config/model version. The app computes the diff locally. Model-provided constraint explanations are review evidence, not proof.

## Approval

Approval persists explicit proof fields (revision, snapshot where relevant, intent, proposal/replacement hash, validator/policy versions, edit generations) plus a binding hash. An opaque hash alone is not the only durable evidence.

Any bound input change invalidates current approval immediately. Label: **Wording reviewed**, not **Safe to execute**.
