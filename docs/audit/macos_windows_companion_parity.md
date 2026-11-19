# Windows and macOS companion parity audit

November 18, 2026 · feat/codebase-decoupling · Windows source baseline `374fada`

The implemented Mac bridge supports verified inline word geometry, scroll updates, pane-specific terminal capture and bounded editor reads. The visible correction and Undo flow passed in a VS Code webview. VS Code’s main code editor still exposes zero-size bounds for its word ranges on this host.

## Findings at the Windows source baseline

The Windows reference is the UI Automation implementation at the commit above. The comparison used source and automated tests. Native Mac acceptance results appear below.

| Priority | User-visible difference | Source evidence | Acceptance check |
|---|---|---|---|
| First | The Mac can position a suggestion by the caret or field, but cannot underline the observed word. Windows also searches for a recent misspelling when the caret is at the end of the draft. | WinAccessibilityBridge.cpp lines 895–996 supplies word offsets and rectangles. MacAccessibilityBridge.mm lines 541–568 reads a zero-length caret range. HostController.cpp lines 3878–3925 consumes both forms of geometry. | In TextEdit and a Chromium composer, type an earlier typo followed by a correct final word. Verify the earlier typo is selected and marked. Moving the caret into another word should target that word. Check a wrapped line and a second display. |
| First | Scrolling or reflow can leave the Mac suggestion at its old position. | MacAccessibilityBridge.mm lines 603–621 suppresses observations with unchanged text, offset, token and field bounds. WinAccessibilityBridge.cpp lines 1084–1094 also compares caret and word rectangles. | Scroll the caret inside a fixed editor without editing. The suggestion should follow the visible target and marks should disappear when the target is offscreen. |
| Next | A VS Code integrated terminal can be classified as a GUI editor on Mac or skipped. Windows recognizes the xterm helper and reads its accessibility rows. | MacAccessibilityBridge.mm lines 52–56 and 536 classify terminals by application bundle. WinAccessibilityBridge.cpp lines 666–670, 725–739 and 811 classify and read an embedded terminal separately. | Switch between the VS Code editor and its integrated terminal in the same window. Capture only the active terminal draft, preserve scrollback, and verify GUI capture resumes in the editor. |
| Next | A rich composer with an unavailable AX value can look empty during Mac observation. A draft longer than 16,000 characters is silently shortened. Delivery separately checks the full value, so capture and delivery can disagree. | MacAccessibilityBridge.mm lines 541 and 672–687. WinAccessibilityBridge.cpp lines 813–892 tries alternate text providers and checks reliability and length. | Test a genuinely empty composer, an inaccessible value, placeholder-only text, and a 16,001-character draft. Only a verified empty value should be treated as empty. Never offer a partial draft as the whole field. |

## Baseline verification

Both platforms load the dashboard, floating card, ambient badge and word companion through app/host/main.cpp. MacAccessibilityBridge.mm implements whole-field and range delivery, target identity checks and read-back verification. A separate Mac QML implementation is unnecessary for the gaps above.

Run the local shared checks with:

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure -R '^roclint_(product_alignment|range_injection_policy|dual_mode|terminal_adapter)$'
```

For native acceptance, repeat the same drafts on both systems. Capture the selected span, its screen rectangle, routed field kind, observed text length and verified Apply/Undo outcome. Offscreen tests can exercise shared presentation and policy, but cannot establish what TextEdit, Chromium or VS Code exposes through accessibility on a live desktop.

The four selected suites passed on the baseline Mac build in 46.52 seconds.

## Implementation and native acceptance

Tested on macOS 26.6.2 with Qt 6.11.2 and VS Code 1.146.0, using a disposable VS Code profile. The production UI used the local Granite Q8_0 worker and an in-memory Roclint database. The Mac bridge now checks the existing application allowlist before observing fields.

| Surface | Observed result |
|---|---|
| Visible word companion in a VS Code webview | For “Keep all existing tests unchnaged on Windows.” the app displayed a 98-pixel-wide mark under “unchnaged”. Clicking it opened the word companion. Clicking “unchanged” updated exactly that word. Clicking Undo restored the full original draft. Both native delivery completions reported `INJECTED`. |
| Scrolling and wrapping | With text, caret offset and field bounds held constant, a six-point scroll moved the reported word rectangle six points. Scrolling the word out of view cleared its rectangles. Scrolling back restored them. A word wrapped across two visible lines produced two clipped rectangles. The verified UTF-16 span was [3, 29) after a leading emoji and space. |
| VS Code main code editor | The native EditContext provider supplied the correct word string and zero-size character bounds. Point-to-character queries returned offset zero at multiple positions. The running app showed the field indicator. Exact underlining for this provider remains open. |
| Integrated and standalone terminals | Two VS Code split panes yielded distinct field tokens and the exact unsubmitted drafts “roclint pane alpha” and “roclint pane bravo”. Delivery against the old pane returned `FIELD_CHANGED` and the current draft stayed intact. Returning to the editor resumed GUI capture. Disabling xterm accessibility rows invalidated capture and returned `NOT_SUPPORTED` on delivery. A separate Terminal.app session yielded its exact seeded draft. |
| Large and empty webview drafts | Verified empty, 16,000 and 16,001 UTF-16 units against the actual webview content. Chromium exposed only 10,000 units in AXValue for both large drafts. The focused control’s text-marker length and parameterized string range recovered all 16,000 units. The 16,001-unit case cleared the previous observation. This is the native capture limit. The shared host separately applies its ambient suggestion length limit. |
| Protected fields and placeholders | The VS Code fixture’s empty input yielded a verified empty draft with its placeholder excluded. A focused password field was flagged as secure and emitted zero characters. |
| Other providers | Isolated bridge probes on TextEdit 1.20 and Chrome 157.0.8103.79 located the earlier typo and verified native range replacement and reversal. The product’s visible interaction checks used VS Code. |

The test host had one display. Moving an overlay between physical displays and a side-by-side Windows desktop run remain unverified. The native webview interaction used pointer clicks. An accessibility-action-only Apply attempt returned `FIELD_CHANGED` during focus handoff.

## Final automated verification

The Release build succeeded. The full macOS CTest run passed all 66 enabled suites in 183.19 seconds. The repository disables `roclint_retrieval_integration` in this build. Earlier runs intermittently timed out waiting for the native Granite worker to return to READY. Its independent rerun and the final full run both passed. The structure checker reported zero errors and `git diff --check` passed.
