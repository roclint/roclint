# Roc HUD and Windows launch QA

Date: 2026-11-07. Scope: first native HUD refresh and ordinary Windows launch. The settings window receives shared colors/branding only; its full information architecture and control redesign remain a later slice.

## Visual truth and capture

- Approved source: `docs/research/roclint-ux-2026-11-05-approved-rounded-roc-long-prompt.png`, 1254 × 1254 pixels. Its HUD occupies approximately x474–1208, y49–627; editor/backdrop and the lower external composer are context, not Roclint window content.
- Decision record: `docs/research/2026-11-05-roclint-ui-ux-direction.md`. Its later user-controlled rewrite requirement supersedes the older mock's CTA wording.
- Implementation: `docs/research/roclint-ui-2026-11-07-clarification-implemented.png`, 820 × 600, actual production QML rendered by Qt at DPR 1; state: three alternative clarification choices, custom answer and explicit skip/rewrite. The original test artifact is `build/manual-testing/ui-refresh/clarification-options.png`. Test transport supplies synthetic content; this is not a real-model quality claim.
- Durable companion images: `docs/research/roclint-ui-2026-11-07-review-implemented.png` and `docs/research/roclint-ui-2026-11-07-compact-implemented.png`.
- Additional captures: `requested-rewrite.png`, `review-checks-560x400.png`, `hud-controls-420x200.png`, `hud-controls-620x250.png`, `hud-controls-820x520.png` in that directory. A full native software-rendered QML test, rather than a web clone, exercises focus, layout and workflow.
- Normalization: compare the source HUD region against the implementation window; do not compare the source editor/composer or the 1254-square canvas against an 820-wide app. No browser/CSS viewport applies to this native Qt app. The source's display density is unknown; do not infer exact CSS sizes from its bitmap.
- Full-view evidence: source and implementation were presented together in the same comparison tool call. Focused inspection covered their header/logo, large question, option rows, custom answer and bottom actions at readable resolution. The implementation's extra workspace/model controls and three-option explanation text are intentional product requirements, not omitted source elements.
- Installed native observation: computer-use returned `Roclint — prompt assistant` as a normal targetable window from `%LOCALAPPDATA%/Programs/Roclint/Roclint.exe`; its screenshot and accessibility tree exposed the prompt editor, Clarify, Rewrite prompt, settings and close controls. No terminal appeared.

## Required fidelity surfaces

| Surface | Assessment |
|---|---|
| Typography | Embedded Inter Variable for reading, 26 px question hierarchy, 14–15 px prose, Consolas/Menlo for compact utility labels. Corrected the initial `Inter` family mismatch that fell back to monospaced text. |
| Spacing/layout | Rounded 18 px outer panel, 20 px normal gutters, bounded draft and diff, persistent footer; question padding/row gaps reduced so all three alternatives, custom answer and skip action fit the 820 × 600 fixture. Extremely small windows scroll content while preserving window controls and delivery actions. |
| Colors/tokens | Ink-blue background, cobalt primary/keyboard choice, warm-white prose, pale-blue perimeter and focus, muted periwinkle utility text. Semantic warning remains in the clarity label; focus uses a pale-blue boundary. |
| Images/icons | Actual generated transparent Roc and pixel signal assets; official Phosphor SVG controls with licenses/provenance. PNG assets retain aspect ratio and transparent surroundings. No text/emoji substitutes for logo or window icons. |
| Copy/content | User-facing actions include Clarify, Rewrite prompt, Skip & rewrite, Keep original, Show changes and explicit Approve & Copy/Insert. No question is selected automatically; identical output says it is the original prompt. Empty drafts disable inapplicable actions. |

## Comparison and repair history

1. **P2 — incorrect font fallback.** Early clarification capture used a monospaced fallback. Corrected the embedded family to `Inter Variable`; recaptured `clarification-options.png` shows sans question, option text and controls.
2. **P2 — diff cut off after focusing review.** At 820 × 520 and 560 × 400 the diff's first delegate received focus and scrolled only its first line into view. Reveal the bounded ListView, defer focus until layout, and recheck changed heights. Existing geometry and keyboard regression checks pass after the fix.
3. **P2 — lower clarification actions clipped at normal size.** The larger input/font initially pushed actions below the viewport. Reduced panel margins, row gaps and alternative padding; the final clarification capture shows the entire skip/rewrite row above the persistent footer.
4. **P2 — empty-state actions looked usable.** Installed native observation showed enabled rewrite/review with no draft. Bound availability to the current HUD draft and added a regression assertion before/after typing.
5. **P2 — ordinary launch behaved like a tool popup.** Manual HUD now uses a regular app window, and app activation shows the retained draft without requesting capture from another editor. Native computer-use can now target it.

## Interaction evidence and limitations

- Product-alignment checks cover explicit questions, skip without fabricated answers, new-draft rewrite request, pending constraint review, keyboard use, focused diffs, preserved drafts and no unapproved delivery.
- Four resized HUDs cover 420 × 200 through 820 × 520 with long labels. The full CTest suite passes; final presentation-only changes also receive a focused product-alignment rerun.
- Reduced-motion preference disables the new hover color transitions. Existing persisted approval/hash/model contracts remain unchanged.
- A native installed launch and close/reopen check supplement the QML fixtures. No new claim is made about native insertion support or macOS parity.
- Follow-up polish: full settings/onboarding redesign, a distributable signed installer, and high-contrast/light-mode native acceptance beyond current shared-theme regression coverage.

## Implementation checklist

- [x] Approved palette, real Roc asset, legible question/rewrite content and rounded controls.
- [x] Explicit rewrite path and optional questions; preview and approval retained.
- [x] Normal Windows executable/shortcuts and repeat-launch activation.
- [x] Native QML layout and workflow checks; installed startup without developer Qt PATH.
- [ ] Release installer, relocatable Julia runtime, first-run model setup, upgrade/uninstall and signing (separate release scope).

Final result: passed
