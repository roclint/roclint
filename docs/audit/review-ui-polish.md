# Review UI polish and leaner rewrites

Date: October 26, 2026
Status: Implemented locally; build, regression gate and native screenshot verification passed.

## Behavior

The HUD uses one presentation mapping for badge and primary action. Optional ideas are neutral, required clarification is amber, pending proposals show Review, and failed suggestions/rewrites do not advertise readiness. Required checks precede the diff in a natural-height panel; short diffs take their actual content height, while long diffs scroll within an 80–240 px cap derived from 40% of the available workflow height. The workflow uses an explicitly sized Flickable instead of circular ScrollView content sizing.

Only the fixed footer approves or delivers. It displays the remaining check count, and approval uses the existing durable acknowledgement checks. Unbound drafts copy to the clipboard, including when live capture is enabled; connecting/changing an editor remains explicit. Explicit Review expands an ambient compact HUD, then focuses and reveals the first unchecked item. Incoming results and late callbacks never perform that expansion. Edit prompt restores the preserved draft as an editable HUD field without changing its ownership until the user actually edits it.

Routine metrics and verification statuses are hidden in the initially closed Technical details section. Delivery destination remains visible and is not labeled as live-following state. Redundant profile/review controls disappear during preview. Acknowledgements do not scroll the user back to the heading.

Rewrite instruction contract `granite.rewrite.format.v5` asks for the shortest faithful wording and discourages unrequested filler while retaining explicit quality, usability, accessibility, API, numerical and formatting requirements. The manifest/configuration suffix is `review_polish.v8`; discovery remains v7. No post-generation phrase stripping, automatic retry, model-weight/sampling change, public persisted-command change, SQLite change or semantic payload migration was added.

## Composer observation

Deterministic bridge fixtures reproduce both independent clipboard/HUD ownership and bound-editor behavior. An independent clipboard review deliberately remains intact while another composer changes to “make” or becomes empty. Clearing the same bound composer invalidates clarification, proposal and approval stages and leaves the HUD visible. Existing cases cover real text edits during review, delivery echoes, subsequent typing, unrelated fields, HUD-owned edits and history restoration.

The two supplied screenshots show a clipboard delivery label, but do not establish the user's capture path or intended ownership. Real production Roclint cannot be addressed by the available app-automation interface (`Invalid app: Roclint` for the unbundled host). Therefore the specific Codex/macOS AX incident remains unverified. No speculative capture-ownership fix was made. The UI now displays delivery destination explicitly; it does not claim that an insertion target is being followed.

## Verification

The new QML fixtures restore a valid persisted v5 optional-help snapshot. Geometry checks map checkbox and label rectangles through every clipping ancestor into the window viewport, rather than checking only object existence. Cases cover 820×520, 560×400, and ambient 620×250/420×200; compact cases exercise a real result arrival without expansion, then keyboard Review/acknowledgement. Other cases cover three checks, wrapped answers, long diffs, diagnostics, discarded/restored proposals, explicit hide/minimize, and a locked-database acknowledgement failure with GUI heartbeat timers and no approval/delivery.

Seven isolated installed-model rewrite observations returned proposals: a short thank-you request, a confirmed bold/playful website direction, explicit visual/usability/accessibility goals, an API invariant, a 100 ms output-preserving target, Markdown structure and XML structure. The creative result was one sentence without the previous filler; requested quality goals and explicit API/quantity requirements remained present. These are a small development corpus with manual semantic assessment, not a general accuracy benchmark. No real editor insertion or Send action was performed by the probe.

Final gate:

- Refreshed build: successful.
- Registered CTests: **43/43 passed**. A strengthened long-diff keyboard assertion first exposed missing End/Page navigation; explicit bounded keyboard scrolling fixed it and the full suite then passed.
- Python unit suites: **49/49 passed**.
- Structure checker and `git diff --check`: passed.
- Native Cocoa fixture checks passed for persisted v5 optional styling, all four geometry cases, acknowledgement keyboard input, explicit expansion, hide/minimize and stale callbacks. The native fixture now uses the normal Qt Quick renderer rather than forcing the offscreen software backend. Screenshots wait for window settling and completed render frames.
- Screenshot-fixture limitation: a compact-to-detailed capture was blank under the forced software renderer despite passing geometry checks. Captures with the normal native renderer, settled windows and completed render frames show the complete workflow. This is not evidence of a production capture/ownership defect.
- Manual screenshot review: neutral Optional and Review labels, checks before the diff, visible footer count/destination, no default metrics or mechanical badges. The 560×400 case scrolls; compact Review expands to 820×520.
- The long diff is capped geometrically, and Home/End/Page/arrow keys scroll inside it. Short diffs use their actual content height. Checking any acknowledgement still does not approve or deliver.

Native Qt emits existing customization/deprecated-handler/focus warnings; no test failures remain. Real third-party AX capture from the user's Codex composer remains unverified. Linux and Windows UI checks are unavailable on this Mac. No general model-quality, latency or downstream-execution claim follows from these checks.

The rebuilt local executable is `build/roclint_host`. A resident process must be quit/relaunched to load the new embedded QML and instruction configuration; it is not silently restarted during implementation because it may hold a user's draft. Existing local changes are preserved; this pass is uncommitted and unpushed.
