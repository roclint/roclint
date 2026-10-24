# C03 — Semantic analysis contract

## Lifecycle naming

```text
Frozen PromptRevision
   ↓
Granite response
   ↓
NormalizedAnalysis   # pre-Jev, validated typed structure
   ↓
mandatory batched Jev evaluation
   ↓
AnalysisSnapshot     # persisted, versioned, user-reviewable semantic state
```

Do not call the pre-Jev object an `AnalysisSnapshot`.

## Typed items

`NormalizedAnalysis` may contain:

- `LiteralEvidence`
- `ChoiceGroup` with 2–3 structurally distinct `ChoiceAlternative`s
- `Assumption`
- `MissingDecision`
- `Conflict`

C++ validates schema, IDs, source bindings, bounds, and **policy-defined structural distinction**. C++ does not claim to prove arbitrary natural-language alternatives semantically mutually exclusive. If a group cannot safely be represented as bounded alternatives, mark it manual/needs-review rather than forcing a Choice.

## IDs and dedup

Local semantic IDs are deterministic from canonical item type/path/content under a versioned algorithm. Only exact canonical duplicates are auto-merged.

## Materiality

`MaterialityPolicyV1` is human-authored C++ policy. Granite/Jev may supply evidence or ranking but cannot silently decide that a mechanically represented material difference disappears.

## Jev presentation policy

`JevPresentationPolicyV1` consumes the mandatory batch result to:

- order alternatives within a group;
- order which unresolved item is asked first;
- show provider-supported uncertainty/score metadata;
- annotate `other/unresolved` behavior.

Material alternatives begin **unselected**. Jev never confirms intent for the user.

## Host context hint

v4.2 permits one optional, non-authoritative `HostContextHint` captured by C09 at explicit invocation time.

Canonical fields:

```text
HostContextHint {
  category: CODE_EDITOR | TERMINAL | BROWSER | CHAT | DOCUMENT | OTHER | UNKNOWN
  display_name?: local-only user-facing name
  source: FRONTMOST_APPLICATION
  captured_at_invocation: true
}
```

Rules:

- the hint is **advisory evidence only**;
- it may influence Granite's prioritization of likely ambiguity classes, but it may not create confirmed intent, remove a material alternative, or override literal prompt evidence;
- raw window title, document path, URL, file contents, selected host text, and accessibility-tree contents are out of scope for the MVP;
- C09 may keep a local `display_name` such as “Visual Studio Code”, but the default Granite payload uses the coarse `category`, not the raw application identifier/name;
- the user may clear/disable host context before Analyze;
- Jev receives no host-context field unless a future explicit contract revision requires it.

## v4.2 provider responsibility boundary

- Local Granite proposes the bounded pre-Jev `NormalizedAnalysis` and may receive the optional coarse `HostContextHint.category`.
- Cloud Jev evaluates/ranks the bounded questions required by the full ranked path.
- Human-authored C++ policy validates structure/materiality rules and decides whether the UI is quiet/clarify/manual under the frozen policy.
- The user alone confirms/corrects intent.

The local/cloud runtime split and optional host hint change deployment/context, not semantic authority.
