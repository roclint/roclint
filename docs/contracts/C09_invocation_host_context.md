# C09 — Desktop invocation & host-context contract

## Purpose

C09 owns the **user-session invocation shell** around Stardust. It strictly isolates global hotkeys, clipboard access, frontmost-app context, active-screen/cursor anchoring, widget lifetime, and copy-back from the core semantic contracts.

C09 does **not** own prompt meaning, analysis semantics, SQLite state, Jev scoring, confirmed intent, rewrite approval, or any other authoritative core decision.

## MVP form factor

Stardust is a persistent native background application with:

- Windows system-tray / macOS menu-bar presence;
- one registered global invocation shortcut;
- a compact frameless QML floating widget;
- clipboard-first prompt capture;
- active-screen/cursor-aware placement;
- optional passive frontmost-application context hint;
- copy-back of the approved rewrite.

Direct arbitrary-app text extraction/replacement is not required for the MVP. It may be added later or as an optional capability, but it must not make accessibility/automation permissions a Day-1 dependency.

## Required Day-1 flow

```text
Copy prompt in browser / IDE / Slack / terminal
        ↓
Press Stardust global hotkey
        ↓
Floating widget appears immediately
        ↓
Widget is pre-filled from the clipboard
        ↓
Optional coarse host-context hint is shown
        ↓
User edits / analyzes / confirms intent / reviews rewrite
        ↓
User approves rewrite
        ↓
Approved rewrite is copied back to the clipboard
        ↓
User pastes into the original host app
```

This is the portable **Grammarly-style MVP path**.

## Host application lifetime

Stardust is a normal **per-user desktop application**, not a privileged OS service/root daemon. Closing or dismissing the floating widget does not quit the background host. Explicit Quit from the tray/menu-bar terminates Stardust and its owned child runtimes.

The host owns:

- tray/menu-bar lifecycle;
- shortcut registration/unregistration;
- widget show/hide/focus;
- active screen and cursor-location observation used only for placement;
- one-shot frontmost-application observation used only for optional `HostContextHint`;
- clipboard capture/copy-back;
- runtime supervision through C08;
- presentation of runtime/semantic availability through C05.

## Screen anchoring

On invocation:

1. determine the screen containing the current cursor when available;
2. place the floating widget near the cursor using a documented offset;
3. clamp the widget to the screen's available geometry so it is never placed off-screen;
4. fall back to a deterministic active-screen position if cursor information is unavailable;
5. do not send cursor/screen metadata into the semantic core.

Screen anchoring is presentation context only.

## Shortcut behavior

- Shortcut is user-configurable.
- Registration failure is visible/actionable and does not remove tray/menu-bar invocation.
- A conflicting shortcut is never silently stolen.
- Repeated invocation while the widget is already visible follows one documented focus/toggle rule and never creates duplicate widgets.

## Clipboard policy

Required:

- read clipboard text only after explicit hotkey/tray capture or an explicit in-widget capture action;
- never continuously monitor clipboard contents;
- show the captured text as an editable draft before semantic analysis;
- an empty/non-text clipboard opens the widget with a blank/editable draft state;
- copy back only the exact user-approved rewrite;
- never infer approval from capture, focus, or clipboard ownership.

## Passive host-context hint

At the same explicit invocation event, Stardust may inspect the **frontmost application identity** through ordinary platform APIs that do not require accessibility-tree access.

MVP data flow:

```text
frontmost app identity
   ↓ local mapping
coarse category
   ↓
HostContextHint.category
```

Allowed coarse categories are defined by C03:

`CODE_EDITOR`, `TERMINAL`, `BROWSER`, `CHAT`, `DOCUMENT`, `OTHER`, `UNKNOWN`.

Rules:

- the local UI may display a friendly app name such as “Visual Studio Code” or “Slack”;
- the default semantic/model payload sends only the coarse category;
- do not capture or transmit window title, current URL, document/file path, process command line, selected host text, screen contents, or accessibility-tree contents;
- the user can clear or disable the hint before Analyze;
- host context is advisory only and cannot become `ConfirmedIntent`;
- unsupported/ambiguous app identity maps to `UNKNOWN` rather than guessing;
- failure to obtain host context never blocks clipboard-first invocation;
- the feature must not require Accessibility/Automation permission for the v4.2 MVP.

Platform planning sources should use the supported frontmost-window/application APIs selected in B02H; do not add process scraping or continuous monitoring.

## Optional direct injection

If direct host-app injection is later implemented, it:

- is explicitly enabled;
- uses platform-supported accessibility/automation mechanisms;
- discloses permissions;
- falls back to the clipboard-first path on denial/failure;
- never becomes a prerequisite for the v4.2 MVP gates.

## Privacy boundary

Granite analysis/rewrite are local under C04/C08. Jev scoring is cloud-based for the full ranked/scored path. C09 does not describe the full product as offline/private merely because Granite is local.

The host-context hint is local metadata by default. Only the coarse category may be passed to local Granite under C03/C04. Jev does not receive it in v4.2.

## Acceptance evidence

- tray/menu-bar host survives closing the widget;
- shortcut opens/focuses exactly one widget;
- widget appears before Granite is ready;
- widget anchors to the cursor's screen and remains on-screen;
- clipboard is read only after explicit invocation/capture;
- copied text is editable draft, not confirmed intent;
- approved rewrite can be copied back without direct injection;
- frontmost-app capture produces an optional coarse category without Accessibility permission;
- disabling/clearing the hint leaves analysis usable;
- shortcut conflicts are actionable;
- explicit Quit shuts down owned child runtimes cleanly.
