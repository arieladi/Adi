# Parity checklist — step 7.2, the root window and its keys

Drafted by the window lane (ADR-0208) from the Live 12 manual, for win to run against Live 12
(ADR-0108). The manual's text was read from `live12-manual-en.pdf`; the pages are the PDF's.
`reference/DOCS/_text/Ableton_live12/` is not on the Mac this lane runs on, so where the A2000's
rows exist they replace these. Rows describe the behaviour in our words, never the manual's.

Status: **built** (in this PR, with a check in `tests/test_ui_shell.cpp`), **partial**, **open**
(a later item), **measure** (win checks Live and reports the figure or the difference).

| # | Live's behaviour | Manual | ADI in this PR | Status |
|---|---|---|---|---|
| 1 | The Info View names the element under the mouse and says what it does | §2.2.2, p. 28 | The Info View shows the hovered control's title and tooltip text; empty over nothing | built |
| 2 | The Info View is shown or hidden from a toggle in the window's bottom-left corner, or Shift+? | §2.2.2, p. 28; §41.1, p. 984 | `?` toggle at the Status Bar's left end; Shift+? from any window; the App setting `lookfeel.showInfoView` holds it, so every window follows | built |
| 3 | The Info View sits in the bottom-left, beside the Device View | §2.2.2, figure on p. 29 | Bottom-left, the browser's width, beside the device strip, above the Status Bar | measure: does Live's browser stop above it, and how tall is it with the Device View hidden? |
| 4 | The Status Bar runs along the bottom and shows messages such as errors | §3.2, p. 34 | A refused swap, a failed save or a full transport queue says why there; the next success clears it | built |
| 5 | Hide/Show Browser: Ctrl+Alt+B or Ctrl+Alt+5 (Cmd+Option on the Mac) | §41.1, p. 984; §3.3, p. 36 | Collapses the Browser panel; its width is kept for when it returns; persisted per window in `ui_view` | built |
| 6 | Hide/Show Mixer: Ctrl+Alt+M; in Live 12 it is the View menu's Mixer, the Mixer View at the Arrangement's foot (§6.1, p. 162) | §41.1, p. 985 | Since `window/mixer-view`: Ctrl/Cmd+Alt+M and the Status Bar's bottom-right toggle open the Mixer View; the side panel of track controls is the View menu's "Arrangement Track Controls", with no key | built |
| 7 | Hide/Show Device View: Ctrl+Alt+4 | §41.1, p. 985 | Hides and shows the device strip; its height is kept | built |
| 8 | Undo Ctrl+Z / Cmd+Z; Redo Ctrl+Y on Windows, Cmd+Shift+Z on the Mac | §41.5, p. 988 | As Live under the `live` preset; the Cubase and Bitwig presets redo on Ctrl/Cmd+Shift+Z | built |
| 9 | Zoom In / Zoom Out Window: Ctrl/Cmd + and - | §41.9, p. 990 | Each window has its own interface zoom (ADR-0203 d6); the root lays out in logical units under it | built; measure Live's zoom steps (ADI uses 50 % to 200 % in 25 % steps, provisional) |
| 10 | Space plays from the start marker, or stops | §41.20, p. 1001 | Space toggles play and stop through the driver's queue | partial: the start marker arrives with the arrangement (7.3) |
| 11 | Shift+Space continues from the stop point | §41.20, p. 1001 | — | open: needs the transport's seek, with the arrangement |
| 12 | Esc closes a window or a dialog | §41.1, p. 985 | Esc no longer stops the transport (win_codex's original did) | built (no dialog exists yet) |
| 13 | Tab toggles Session and Arrangement | §41.1, p. 984 | — | open: Session View is P2 (ADR-0203 d10) |
| 14 | Ctrl/Cmd+, opens the Settings | §41.1, p. 985 | Opens the Settings window in Live's shape (`window/settings`, `parity-settings.md`) | built |
| 15 | The Device View has one height | ADR-0203 d7, ADR-0184 | Floor 169 zoom-independent units, a desktop default; the strip grows upward only and keeps its height when hidden | measure: 169 is the director's figure, UNVERIFIED (AWAITING) |
| 16 | — (ADI deviation) floating tooltips on after 600 ms | ADR-0203 d12, ADR-0131 | One tooltip window per native window, delay from `lookfeel.tooltipDelayMs`, off with `lookfeel.showTooltips` | built |
| 17 | — (ADI addition) the AI chat is a tab at the browser's foot, collapsed | ADR-0203 d6 | A 24-unit tab at the browser's bottom; open, it takes the lower two fifths; remembered per window | built |
