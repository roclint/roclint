# IDE and AI workspace compatibility

Execution update: see [the November 7 checkpoint](../audit/ide_compatibility_2026-11-07.md)
for native findings, capture guards, full regression results and outstanding
platform/provider limitations. The identity-only observations below are historical.

Date: 2026-11-06. Windows-first implementation proposal, with one bounded
Antigravity IDE identity correction implemented during inspection.

## Current evidence

The installed applications are distinct:

- `C:\Users\kirby\AppData\Local\Programs\Antigravity\Antigravity.exe`
- `C:\Users\kirby\AppData\Local\Programs\Antigravity IDE\Antigravity IDE.exe`
- `C:\Program Files\Android\Android Studio\bin\studio64.exe`

Antigravity IDE's installed product.json identifies Antigravity IDE 2.5.5 and
application name `antigravity-ide`. The executable name includes a space.

Before the correction, HostContext.cpp explicitly classified `antigravity.exe`
but not `antigravity ide.exe`. Its substring-based ambient filter admitted both,
while WinAccessibilityBridge.cpp's exact Electron list admitted only the former.
The correction adds the installed IDE executable to classification, the explicit
ambient list, and Electron provider activation / editable-ancestor / renderer
window handling. It does not establish native capture or insertion success.

Android Studio already has `studio64.exe` in classification and ambient
eligibility. The Windows bridge consumes UI Automation Value/Text patterns and
focuses on eligible edit/document controls. There is no dedicated Java Access
Bridge adapter in the inspected host implementation. Whether the installed IDE's
particular chat composer exposes sufficient UIA behavior needs a native probe;
its executable being listed is not proof that this works.

The macOS code lists both Antigravity bundle identifiers, but the ambient list
does not explicitly include Android Studio or the JetBrains IDE identifiers.
macOS changes need a real platform build and native test, separate from Windows.

## Compatibility means separate capabilities

Track each application version and each specific composer/tool window for:

1. App identification and global shortcut invocation.
2. Focused prompt capture, change notification, caret and badge placement.
3. HUD review, selected/custom answers, Skip and Full Rewrite.
4. Return to the exact composer and replace only the expected current draft.
5. Exact text readback, or an explicit clipboard fallback with no false success.
6. Workspace root and active-file association, including multiple windows,
   worktrees and remote projects.

Use separate statuses such as verified, partial, clipboard-only, blocked and not
tested. Mark status per capability, app version, OS and composer. Never promote
a mocked test or recognized process name into native verified support.

## Implementation sequence

### 1. Verify the two Antigravity applications

Start with the already installed standalone app and IDE. Use synthetic drafts
in dedicated test sessions. Confirm provider activation, contenteditable/Monaco
ancestor resolution and main/renderer process association in both installations.
Check that the prompt composer is captured rather than chat history, search,
settings, or the source editor. Record supported surface identity explicitly.

Test multiline/Unicode drafts, empty/short prompts, draft edits during review,
two windows, focus switches, cancellation, retained answers, exact replacement
and clipboard restoration. Never submit the downstream prompt automatically.
Require approval before replacement and retain existing stale-field checks.

### 2. Add Android Studio / JetBrains coverage

Probe the installed Android Studio version and the actual AI composer chosen for
testing. Try the existing UIA path first. Record the available patterns and
focus behavior without changing global accessibility settings automatically.

If the composer does not expose reliable capture/readback, decide between a
bounded Java accessibility adapter and a small JetBrains companion plugin after
the probe. A companion plugin may be more suitable for exact project/file/draft
identity. Do not assume public editor APIs can read or replace another plugin's
private chat composer. Validate a supported composer-specific integration or
provide an explicit Roclint action plus clipboard fallback.

### 3. Consolidate compatibility policy

Replace duplicated process-name lists with one application profile registry
consumed by classification and platform accessibility policy. Profiles should
state known identities, UI technology and available capabilities; actual field
capabilities still require runtime checks. Treat standalone/IDE identities and
OS differences explicitly. Avoid broad substring matches as evidence of support.

Expose useful status and a reason for degraded behavior in Roclint, such as
prompt capture available, clipboard delivery only, or no editable prompt found.
Keep diagnostics free of captured prompt contents by default.

### 4. Add optional editor bridges where they provide proven value

Investigate VS Code-family and JetBrains bridges for explicit workspace roots,
active files, selected text and revision identity, including unsaved buffers.
Roclint's native host should continue to own analysis and approval. Bind bridge
messages to the current session/workspace/revision; do not infer workspace
identity solely from window titles or background application databases.

Remote SSH, WSL, containers and multi-root workspaces need explicit path mapping
or a remote-side bridge. An unresolved mapping must not ground against an
unrelated local directory. Preserve the existing complete-snapshot and stale
proposal contracts.

### 5. Expand the verified matrix

After the first three installed applications: VS Code, Cursor, Windsurf, Visual
Studio, selected JetBrains IDEs, Codex desktop, Claude desktop and supported
terminal composers. Add applications based on measured shared adapter behavior;
do not claim every editor in a technology family works automatically.

## Language boundary

Desktop prompt compatibility and AST language support are independent.
Roclint can assist with a Java/Kotlin prompt while source grounding is
unavailable. Current verified AST extraction remains saved C/C++ files.

Java/Kotlin grounding would require additional pinned Tree-sitter grammars,
language dispatch, independently annotated fixtures, structural body-free
signatures and the same identity/freshness/budget contracts. Keep that pure-AST
extension as a separate milestone; do not block initial Android Studio prompt
integration on it or claim compiler-level resolution.

## Validation and rollout

Build the host and run the existing host-context, invocation and dual-mode
regressions after the identity fix. Native per-application interaction tests
remain a separate acceptance gate. The preceding Phoenix component evaluations
do not exercise desktop accessibility and predate this identity-only patch.

First delivery should be verified Windows coverage for Antigravity standalone,
Antigravity IDE and one Android Studio AI composer, with explicit fallbacks.
Broader language support and the final visual redesign follow independently.

### Identity correction verification

Release build passed for `roclint_host` and the three selected regression
executables. CTest passed 3/3 in 10.95 seconds: `roclint_host_context`,
`roclint_invocation_binding` and `roclint_dual_mode`. `git diff --check` passed.
Logs: `build/manual-testing/session-20261106/ide-compatibility-build.log` and
`ide-compatibility-ctest.log`.

The new host is `build/Release/roclint_host.exe`. The separately staged
`build/Release/bin/roclint_host.exe` and currently running QA copy were not
replaced or restarted during this compatibility inspection. Native interaction
testing in Antigravity IDE and Android Studio remains NOT RUN. No full CTest
rerun, Phoenix rerun, commit or push is claimed for this bounded identity patch.

## Primary references

- [Antigravity surfaces](https://antigravity.google/docs/overview?tab=ide).
- [Antigravity IDE integrations](https://antigravity.google/docs/ide/extensions):
  integration features vary by editor/surface; this is not a Roclint API contract.
- [Android Studio accessibility](https://developer.android.com/studio/intro/accessibility).
- [JetBrains accessibility](https://www.jetbrains.com/help/idea/accessibility.html).

The adapter/plugin choices above are implementation proposals, not capabilities
established by those product documents or by the current native testing.
