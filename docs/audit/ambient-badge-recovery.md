# Ambient badge recovery after local restart

Date: October 27, 2026

The user reported the small amber card beside the prompt composer was missing while the main HUD was hidden.

## Confirmed runtime blocker

The restarted native host reported `trusted=false`, so observing was disabled. After the user enabled Accessibility in System Settings, it reported `trusted=true` and `observing=true` without another restart. The saved ambient-enabled preference was already true. Do not attribute this incident solely to the separate QML defect below.

The current build is a linker/ad-hoc-signed unbundled Mach-O (`roclint_host`, no bound Info.plist or team identity). When launched through Codex, the observed Accessibility entry was ChatGPT/Codex; changing that entry independently by the user restored trust to the host. A stable bundled/signed app launch and permission-identity workflow remains a distribution task. No TCC database, entitlement, or OS permission was changed by code or automation.

## Separate confirmed settings defect and repair

The running log showed repeated `setAmbientModeEnabled is not a function` errors from DashboardWindow.qml. That method is a Q_PROPERTY writer, not a QML-invokable method. The two dashboard actions and InvocationSettings checkbox now assign the existing `ambientModeEnabled` property. No new public invokable or capture-ownership change was introduced.

A regression invokes the actual QML handlers with an isolated controller/bridge: enable starts observing, a valid prompt yields the amber badge with the HUD hidden, disable stops observing/hides the badge, and the settings checkbox changes and saves the same property. Existing dual-mode tests cover badge placement, hidden/visible HUD behavior, capture ownership and permissions.

## Safe diagnostics and verification

Capture diagnostics are opt-in. Two old debug statements that included complete drafts now report character counts; the badge debounce trace reports only active/field/HUD-visible booleans. No prompt content is needed to identify permission/focus gating.

Refreshed build succeeded; all 43 registered CTests passed, including the new QML handler regression. The focused three-test capture/product/upstream suite and native Cocoa fixtures passed. Diff check passed. The production process was restarted with its HUD initially hidden, preserving the user's reported presentation state.

The user retested the real prompt field with the main HUD hidden and confirmed: “Yes, the amber card appears.” Trust, observing and the real composer-to-badge path are now confirmed for this local session. This does not establish behavior for every supported editor or future rebuild/signing identity.
