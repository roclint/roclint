# D01: Ambient capture (Grammarly-style mode)

Status: proposal for review. Nothing here is built yet.

## 1. Goal

Today Roclint only acts when you press Ctrl+Shift+L. It copies the clipboard into its own card and you edit there. The goal is to work where you already type: Roclint notices the prompt you are writing in an AI chat box (Cursor, Claude, Codex, T3 Chat and similar), checks it in the background, and offers help without getting in your way.

Non-goals for this phase:

- Reading terminals. Codex CLI and Claude Code inside Windows Terminal expose the whole scrollback, not the line you are typing, so there is no reliable "prompt" to read. Terminals keep using the hotkey.
- Any cloud processing. Text goes only to the local Ollama runtime, the same as today.
- Changing the approval rules. Nothing reaches the target app without an explicit approve action.

## 2. What it feels like

1. You type a prompt in Cursor's chat box and pause.
2. If the prompt is clear, nothing happens. No badge, no sound, no flicker.
3. If Roclint finds a choice that matters, a small badge appears at the edge of the text box: `2 questions`. It never takes focus, never covers your text, and never animates for attention.
4. Click the badge, or press Ctrl+Shift+L, to open the card already loaded with your text and the questions. Answering works as it does today.
5. Approve. Approve & Copy works as today. A new option, Approve & Replace, writes the approved rewrite back into the chat box. You still press Enter in Cursor yourself.
6. If you keep typing instead, the badge fades away, and the next pause starts a fresh check.

### Rules for avoiding the Clippy trap

These are product requirements, enforced in code and covered by tests:

1. **Never steal focus or keystrokes.** The badge is a non-activating tool window. Clicking it is the only way it gets focus.
2. **Silent when there is nothing worth saying.** A badge appears only for the same results that would produce questions in the card today: an eligible choice group, a blocking missing decision or a blocking conflict. Clear prompts, Manual-mode results and model failures show nothing in the text box. Failures appear only in the tray icon and the dashboard.
3. **No interruptions mid-thought.** Checks start only after 1.5 seconds without typing, and only for text of at least 4 words. Typing again cancels the check that is running.
4. **Dismissals stick.** Dismissing the badge silences it for that exact text. Snooze for this app (1 hour) and turn off for this app are one click away.
5. **Opt-in per app.** Nothing is read until you enable an app in Settings. The first run shows a single explanation screen with the list of supported apps, all off by default.
6. **Never read secrets.** Password fields are never read. Fields over 4,000 characters are skipped.
7. **One badge at most, never a modal.** No dialog boxes ever appear from ambient mode.

## 3. Reading the text box

### Windows

Windows UI Automation (UIA), the same interface Grammarly and screen readers use:

- A UIA focus-changed event handler runs on a dedicated COM thread. When focus moves to an editable element (`Edit` or `Document` control type, not `IsPassword`) inside an enabled app, Roclint subscribes to text-changed events for that element.
- Text is read with `TextPattern.DocumentRange.GetText`, or with `ValuePattern.Value` where TextPattern isn't supported.
- Apps are matched by exact executable file name, as host context matching does today (`Cursor.exe`, `Code.exe`, `Claude.exe`, `Codex.exe`, `chrome.exe`, `msedge.exe`, `firefox.exe`). Browser support is further limited to an allowlist of sites by window title (for example `T3 Chat`) so arbitrary web forms are never read.
- Electron and Chromium apps build their accessibility tree only when an accessibility client asks for it. Turning that on costs some performance in the target app, which is one reason the per-app opt-in matters.

### macOS (Jesse)

The same design using `AXUIElement`: `kAXFocusedUIElementAttribute`, `kAXValueAttribute` and `kAXValueChangedNotification`, gated by the Accessibility permission the app already requests for direct injection.

### Unknowns to settle in milestone 1

The first milestone is a measured spike, run only with your explicit consent on your own machine. It records, for each target app, whether the chat input exposes its text and caret bounds and whether SetValue works. It logs control types and lengths only, never text. Expected trouble spots:

- Rich editors (Lexical, ProseMirror, Monaco) sometimes expose only the visible lines or flatten line breaks.
- Some inputs report their text but not their screen bounds. The badge then falls back to the window corner.
- Write-back through `ValuePattern.SetValue` is often unsupported in rich editors (see section 5).

## 4. How it fits the contracts

This adds a new contract, C10 (ambient observation). C09 covers host context and injection; C10 covers reading another app's text.

| Step | What happens | Persisted? |
|---|---|---|
| Observe | Text read from the focused, enabled field after a pause | No. Memory only, keyed by its SHA-256 hash |
| Pre-check | Discovery runs on the observed text | No. Raw model reply cached in memory with model and transport metadata |
| Badge | Shown only if the pre-check would produce questions | No |
| Open card | Text is frozen as a new revision (`DRAFT_FREEZE` origin) in a new or current session | Yes, from here on |
| Rebind | The cached raw reply is parsed again against the frozen revision with `parseDiscoveryResponse`. This is deterministic, uses the same model output and makes no new model call. The model attempt is recorded at this point | Yes |
| Answer, rewrite, approve | Unchanged from today | Yes |

Consequences:

- Nothing you type is written to history unless you open the card. Dismissed or ignored checks leave no trace on disk.
- Analysis stays bound to a frozen revision (C03). If the field's text changed between the pre-check and opening the card, the cache misses and a normal analysis runs on the frozen text.
- The badge decision uses the existing materiality policy and intervention state machine. No new heuristics decide when to interrupt.
- Outbound prompts stay as they are: only the user's text, never paths, file lists or the target app's name.

## 5. Writing back

- Approve & Copy stays the default and is unchanged.
- Approve & Replace (opt-in per app):
  1. Re-read the target field. If its text no longer equals the frozen revision, refuse with "The text changed after you opened Roclint. Copy instead?". This is the stale guard.
  2. Try `ValuePattern.SetValue`.
  3. Otherwise focus the field, select all and paste with `SendInput`, then restore the previous clipboard contents.
  4. Re-read the field and confirm it equals the approved text. If not, tell the user and leave the approved text on the clipboard.
- The approval record is persisted before writing, exactly as Approve & Copy does today.

## 6. Compute budget

Roclint has no fast local classifier to screen out clear prompts. The fake one was removed under decision 2, and the Jev cloud scorer is not configured. Every pre-check is therefore a full 8B model call: about 5 to 7 seconds of GPU time on the RTX 4060 when the model is loaded, and up to about 12 seconds from cold. To keep that reasonable:

- Run at most one check at a time, and cancel any check that is out of date as soon as the text changes.
- Cache by text hash, so the same text is never checked twice.
- Skip checks while the card is open or the model is busy.
- On battery power, ambient checks pause and only the hotkey works. This can be overridden in Settings.

If Jev or another real classifier becomes available later, it slots in ahead of the model as a cheap first gate.

## 7. Components

| Component | Role |
|---|---|
| `FocusedTextSource` (interface) | Emits "focused field changed" and "text settled" events with app identity, field bounds and text |
| `UiaTextSource` (Windows), `AxTextSource` (macOS), `FakeTextSource` (tests) | Platform implementations |
| `AmbientCoordinator` | State machine: Off, Watching, Settling, Checking, Badge, Dismissed. Owns debounce, cache, dismissals and snoozes. Calls HostController only through Q_INVOKABLE-style commands |
| `HostController` additions | `openFromAmbient(observationId)` performs freeze, rebind and card open. `approveAndReplace()` performs approval, stale guard and write-back |
| `AmbientBadge.qml` | Frameless, non-activating, click-through except the badge itself, placed against the field bounds with DPI awareness |
| Settings | Per-app enable, write-back enable, snooze list, battery behavior. Stored in QSettings |

## 8. Tests

- `AmbientCoordinator` tested with `FakeTextSource` and a scripted transport:
  - debounce and cancellation;
  - minimum length;
  - cache hits;
  - dismissals and snoozes;
  - no badge for clear, Manual or failed results;
  - never more than one check in flight.
- Contract tests:
  - nothing is persisted before the card is opened;
  - rebind produces an analysis bound to the frozen revision;
  - a cache miss when the text changed;
  - password fields are never read;
  - an app that isn't enabled is never read.
- Write-back:
  - the stale guard refuses;
  - verification failure leaves the text on the clipboard;
  - the approval is persisted before the write.
- Manual checklist per target app (Cursor, VS Code, Claude desktop, Codex desktop, T3 Chat in Edge and Chrome), run by a person, since UIA behavior can't be faked faithfully.

## 9. Plan

| Milestone | Delivers | Exit criteria |
|---|---|---|
| M1 Spike (about 1 day) | `UiaTextSource` plus a consented diagnostic that reports per-app capability (text, bounds, SetValue) without logging text | Capability table for the target apps; go or no-go per app |
| M2 Coordinator (2 to 3 days) | `AmbientCoordinator`, cache, debounce, settings, tray status; no badge yet | Coordinator tests pass; observed in the dashboard's live status |
| M3 Badge and handoff (2 to 3 days) | `AmbientBadge.qml`, `openFromAmbient`, freeze and rebind | Contract tests pass; manual checklist passes in Cursor and one browser |
| M4 Approve & Replace (2 days) | Stale-guarded write-back with verification | Write-back tests pass; manual checklist per enabled app |
| M5 macOS parity (Jesse) | `AxTextSource` and the macOS badge | Same tests on macOS |

## 10. Decisions needed

1. Which apps should be in the first supported set? Proposed: Cursor, VS Code, Claude desktop and T3 Chat (Edge and Chrome).
2. Should Approve & Replace ship in the first release, or only Approve & Copy?
3. Battery behavior: should ambient checks pause on battery by default?
4. Is C10 acceptable as written, in particular "nothing persisted until the card opens"?
