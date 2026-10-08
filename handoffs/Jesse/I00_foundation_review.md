# Integration Checkpoint I00 — macOS Verification & QA Notes

- **Checkpoint:** [I00 — Foundation checkpoint](../../assignments/00_foundation/I00_foundation_checkpoint.md)
- **Reviewer:** Jesse K Ramon (Human A)
- **Component Reviewed:** `roclint_host` (Kirby / Human B Assignment B00)
- **Tested Platform:** macOS Apple Silicon (arm64), Apple Clang 21, Qt 6.11.2

---

## 1. Verified Working Behaviors on macOS

1. **Floating Card Display & Dragging:**
   - The card floats frameless on top of the desktop with dark theme `#18181B`.
   - Click-and-drag moves the window smoothly across screen coordinates.
2. **Resident Menu-Bar Presence:**
   - The Roclint tray icon appears in the macOS menu bar (next to the battery and clock icons).
3. **Hide / Dismiss via `Esc` & `[x]`:**
   - Pressing `Esc` or clicking `[x]` immediately hides the card.
   - The background host process stays resident in the macOS menu bar (does not quit).
4. **Reopening from Tray:**
   - Clicking the menu bar icon $\rightarrow$ "Open Roclint" re-focuses the existing card window without spawning duplicates.
5. **Clean Termination:**
   - Menu Bar icon $\rightarrow$ "Quit" cleanly shuts down the application.
6. **Cross-Module Linkage:**
   - `roclint_host` successfully links `roclint_core` and prints `Core Version: 0.1.0`.

---

## 2. QA Notes & Polish Items for Kirby (Human B)

These items were observed during live macOS testing:

### Note 1: Minimize `[-]` Button on macOS
* **Observation:** Clicking the `[-]` button does not minimize the card on macOS.
* **Root Cause:** In `FloatingCard.qml`, line 123 calls `rootWindow.showMinimized()`. macOS does not minimize frameless floating utility panels to the Dock.
* **Recommended Fix:** Change `onClicked` for `[-]` to call `hostController.hideCard()` (hide to tray) so it behaves consistently across both macOS and Windows.

### Note 2: Monospace Font Alignment for ASCII Art
* **Observation:** The ASCII art displays, but character spacing looks slightly different on Mac compared to Windows.
* **Root Cause:** `FloatingCard.qml` specifies `font.family: "Consolas"`. macOS does not ship with Consolas by default, so it falls back to a font with slightly different character metrics.
* **Recommended Fix:** Update the font family in `FloatingCard.qml` to:
  ```qml
  font.family: "Menlo, SF Mono, Consolas, monospace"
  ```
  This gives macOS its native fixed-width Menlo / SF Mono font, ensuring pixel-perfect ASCII rendering on both Mac and Windows.

### Note 3: Keyboard Shortcut Architecture
* **Observation:** Pressing `Esc` dismisses the card, but pressing `Esc` again does not re-summon it once hidden.
* **Architecture Clarification:** Working as intended for B00. `Esc` is an in-window key event (only heard when the card is focused). Re-summoning from anywhere will be handled by the Global OS Hotkey in Stage 01b (Contract C09).

### Note 4: Menu Bar Tray Branding
* **Observation:** Tray menu action and tooltip correctly set to "Open Roclint" and "Roclint - Resident Host".

### Note 5: HUD Resizing Behavior & Monospace Graphics
* **Observation:** When resizing the floating widget, the background container expands, but the internal ASCII graphics and ambient matrix do not scale.
* **Architecture Clarification:** Working as intended. The UI graphics are monospace text glyphs (11px fixed font), not vector/SVG images. The ambient dithering is a 34x15 character grid and the center title is an ASCII banner. As a Spotlight/Raycast-style HUD card (Contract C09), it is meant to stay compact (~520x270). In Stage 01 (B01), the center ASCII graphic will be replaced by the Prompt Text Editor, where window resizing will expand the text input area rather than scaling graphic elements.

---

## 3. Product Milestone Status

**✅ FOUNDATION READY:**
Both `roclint_core` (C++20 static library) and `roclint_host` (Qt resident tray daemon) build, link, and run cleanly across macOS and Windows 11. Stage 00 is officially passed!
