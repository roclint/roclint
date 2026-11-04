# HUD control overflow

Date: October 27, 2026
Status: Implemented in rebuilt local app; regression verification below.

## Report and confirmed cause

The user's compact HUD screenshot shows the rightmost window controls clipped while the model-loading label is displayed. The header placed branding, a workspace selector, source hint, model/runtime state, intervention state, expansion, pin, settings, minimize and close in a single horizontal row. Its intrinsic widths could exceed the compact width; clipping the inset hid controls rather than making them fit.

## Repair

- Window controls remain on the first row; model and intervention status have a separate row.
- Workspace names use a flexible bounded selector with elision; lengthy model labels have bounded width and elide. The source hint appears only when there is room; destination information remains in the footer.
- Narrow footers use separate information and action rows so destination, remaining checks and actions fit. Compact vertical spacing retains the controls at 420×200. At this smallest size, workflow content needs scrolling or explicit Show More; automatic results still do not expand the HUD.
- The workflow reserves a 12 px scrollbar gutter so Technical details and other right-edge controls are not covered during scrolling.
- Native show/reopen anchoring uses the retained window width and height rather than assuming 620×250. Expanded/resized HUDs are clamped using their actual dimensions, including monitors with negative coordinates.

No prompt ownership, analysis thresholds, persistence/approval, model contract, inference retry or automatic expansion change is included. Original drafts and unrelated local changes are preserved.

## Verification

Geometry regressions cover 620×250, 420×200, 560×400 and 820×520 with long workspace/model/source labels and remaining-check text. Every expansion, pin, settings, minimize, close, status, primary/bypass/hide control and footer label must be fully inside the clipped viewport. Existing tests still exercise explicit Review expansion, required checks, long diffs, keyboard access and persistence failures. Placement regressions cover retained compact/expanded/resized dimensions near screen edges and a negative-coordinate monitor.

Native Cocoa screenshots inspected: all window controls fit, footer actions remain inside the frame, long text elides and Technical details clears the scrollbar. Existing Qt style/deprecated-handler/focus warnings remain. Linux and Windows UI runs are unavailable on this Mac. Production source-editor typing/capture is not redefined or inferred from these fixtures.

Refreshed build succeeded. All **43 registered CTests** and **49 Python tests** passed; native Cocoa fixtures, structure checker and diff check passed. The final worst-case footer fixture also passed native verification after adding the long destination and check count. Screenshots are in `/private/tmp/roclint-hud-overflow-ui`.

The rebuilt executable is `/Users/jesse/roclint/build/roclint_host`. The existing resident process was left untouched because the user is actively testing; it must be quit/relaunched to load the new embedded QML. Changes remain local and uncommitted/unpushed.
