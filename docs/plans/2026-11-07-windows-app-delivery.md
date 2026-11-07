# Windows app delivery

Date: 2026-11-07. User requirement: daily use must not require a BAT file or terminal setup.

## Implemented local launch

- Windows GUI subsystem, embedded Roc icon/version resource and matching tray icon.
- No-argument launch opens the retained HUD. It does not request capture from the previously focused editor or import clipboard text.
- Per-user lock plus a bounded, user-restricted local activation channel: a second launch reopens the existing HUD instead of starting another model host. Dashboard activation is also supported. Repeating `--background` exits without opening a window.
- Manual HUD uses a normal app window; a bound editor companion remains a tool window. Closing hides the resident HUD; the tray menu provides Quit.
- Developer deployment helper copies a staged build into the user's Programs directory and creates Start/optional desktop shortcuts targeting `Roclint.exe` directly. No startup registration or administrator access is added.
- Settings, model caches and history stay in their existing user locations. The deployment helper does not delete or migrate them.

## Release work still required

This is a local development deployment, not a distributable installer. In particular, the current Julia venv references the development computer's Python and system packages. Copying it does not make a clean-machine package.

1. Bundle a relocatable, pinned Julia runtime and all native runtime dependencies; verify model acquisition/integrity through first-run UI.
2. Produce a per-user installer with Start menu registration, upgrade/rollback and clean uninstall that keeps user data unless deletion is explicitly requested.
3. Sign the app and installer; validate normal launch, tray/hotkey, model setup and errors on a clean Windows machine without Qt, Python, SDKs or repository paths.
4. Offer launch-at-login as an opt-in preference using the existing `--background` path.
5. Test upgrade with a resident host, duplicate launch during startup, missing/corrupt runtime, interrupted model download and preserved history/preferences.

The existing BAT/PowerShell development launchers are retained for compatibility. They are not the everyday application entry point.
