# C05 — Desktop view-model contract

QML renders state; it does not own authoritative prompt/analysis/intent/approval semantics.

## Product surface

The default MVP surface is a **compact floating widget** hosted by the persistent tray/menu-bar application defined by C09. A traditional large main document window is not required for the MVP.

The view model exposes presentation state for:

- hidden / visible / focused;
- idle / dirty;
- Granite `UNLOADED` / `LOADING` / `READY` / `BUSY` / `FAILED`;
- semantic `FULL_RANKED` / `DEGRADED_UNRANKED`;
- clarify / manual;
- rewrite pending / review;
- stale;
- error.

`GraniteRuntimeState` and `SemanticAvailability` are separate concepts. A warming local Granite runtime does not imply Jev degradation, and a Jev outage does not imply Granite failed.

## Invocation/presentation boundary

C09 owns hotkey registration, clipboard capture, active-screen/cursor anchoring, optional host-context hint capture, widget show/hide/focus, and copy-back. C05 receives those host events and exposes them to QML without moving any of that behavior into the core semantic state machine.

The widget appears immediately after invocation. If Granite is `LOADING`, QML renders a non-blocking warming state while remaining editable.

## Authority

UI actions map to core commands. `Rewrite` never silently confirms inferred intent. `Looks right` confirms only explicit visible/selected items from the bound `AnalysisSnapshot`. Hidden/collapsed inferred items do not become confirmed by implication.

Clipboard capture and host-context capture are draft/presentation events. Neither is confirmation, analysis, or approval.

## Keyboard-first HUD contract

The floating widget must support completion of the normal prompt-engineering path without requiring a mouse.

Minimum bindings:

```text
Tab / Shift+Tab   move among interactive groups/controls
1 / 2 / 3         choose visible alternative 1/2/3 in the focused ChoiceGroup
Enter             confirm/advance the current resolvable step
Ctrl/Cmd+Enter    request final rewrite/approve/copy only when the authoritative state allows it
Esc               dismiss/cancel without replacing clipboard contents
```

Rules:

- shortcuts may never bypass C01/C06 command preconditions;
- a material ChoiceGroup starts unselected even if Jev ranks one option first;
- `Ctrl/Cmd+Enter` is disabled or redirected to the next unresolved step until the core says the workflow is ready;
- focused state and shortcut hints are visible and screen-reader accessible;
- bindings must not conflict silently with text editing; when a text editor owns the keystroke, the view model resolves the conflict explicitly rather than swallowing typed content.

## Rewrite Profile presentation

The widget exposes a small sticky Rewrite Profile selector backed by C04. MVP choices are:

- Generic
- Anthropic Structured
- OpenAI Structured
- Local Compact

The selector changes generation configuration only. It never edits confirmed intent. The current profile must be visible before rewrite and included in the review metadata.

## Diff and constraint-lint presentation

The review surface renders a **locally computed** accessible diff and typed constraint states.

Required constraint states:

```text
MECHANICALLY_VERIFIED
USER_CONFIRMED
SEMANTIC_REVIEW_REQUIRED
FAILED
```

Presentation rules:

- additions/deletions/changes must have non-color cues (`+`, `−`, `~`, labels or accessible text);
- mechanically verified checks may receive an explicit verified badge;
- semantic/user-review items may not receive a “verified” badge merely because Granite claims preservation;
- failed/unresolved constraints remain visible before approval;
- exact spans use the existing UTF-8-byte → QString-range conversion boundary; QML does not reinterpret offsets independently.

## Responsiveness

Provider/runtime operations are asynchronous. No blocking process wait or local-model load occurs on the GUI thread.

## Degraded behavior

If Jev is unavailable, the UI enters `DEGRADED_UNRANKED`: validated alternatives remain visible without fabricated probabilities/ranking and the user may resolve them manually. This production state is distinct from the research term **ablation**.
