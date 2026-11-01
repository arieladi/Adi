# Parity checklist — the Mix View (ADR-0209)

Drafted by the window lane (ADR-0208) from ADR-0209 as the director ruled it on 1 November 2026
(§10: d9 the keys, d11 the P1 set, d13 the name) and UI-ARCHITECTURE §12. The Mix View is ADI's
addition: Live has no such view, so its rows hold it to ADI's decisions and to Live's look
(`collab/window/look-live11.md`), and name the products the draft follows where one applies
(FabFilter Pro-Q's instance list for STACK). Status as in `parity-7.2-shell.md`.

| # | Behaviour | ADR / reference | ADI | Status |
|---|---|---|---|---|
| 1 | F4 opens and closes the Mix View; the function keys do functions | ADR-0209 d9; ADR-0203 d6 as amended | The View menu's Mix View, ticked while open, on F4 (`window/mix-view-shell`) | built |
| 2 | A button in the top bar opens and closes it | ADR-0209 d11 ("F4 and the top-bar button") | Mix, a control bar field left of Live's Draw Mode, lit while open | built; the place is the window's, to strike if the director prefers another |
| 3 | Docked, it takes the centre in place of the arrangement | UI-ARCHITECTURE §12 ("docked in the centre slot of `MainSplit` in place of the arrangement") | It takes the arrangement's area with its Mixer View and its track controls; the browser, the strip down the edge and the Detail View stay; closed, everything returns where it was | built |
| 4 | Detached onto a second display through the floating-window host | ADR-0209 d11; ADR-0180 d3 | The button at the band's right end, and the View menu's Mix View in a Window, move the same panel into a floating window and back (`window/mix-view-detach`). The arrangement returns to the centre; F4 then opens and closes the window; its own close button closes the Mix View; where it opens is kept per window, and the window's place as every floating window's; a project reopened with it open in a window opens it there | built; the button's glyph is the window's, to strike if the director prefers another |
| 5 | Its tabs, in the draft's order and names: OVERVIEW, STACK, SCOPE & PHASE, SPECTROGRAM, CTZ CASCADE | ADR-0209 §5, d13 | Five tabs on the panel's band, the shown one lit; the tab is the window's view state, kept in `ui_view` | built; measure Live 11's look for a lit view chooser (accent here) |
| 6 | STACK is P1: the "one window" of ADR-0204 d1, every channel of the selected group or filter, dormant slots dimmed, the hovered row zoomed for ADI-Q, drawers for the sidechain slot and ADI-L | ADR-0209 d11; FabFilter Pro-Q's instance list | STACK shows first and holds the Channel EQ window's rows: a row per channel in the arrangement's order, ADI-Q and ADI-L off, the shared display | partial: the instance list's shape, the filter, the hovered row and the drawers are the next STACK item (`window/mix-view-stack`); the slots wait for the engine (N16) |
| 7 | OVERVIEW, SCOPE & PHASE, SPECTROGRAM and CTZ CASCADE are P2, after the first-window gate | ADR-0209 d11, d12 | Each tab is there and says it comes after the first window | built as ruled |
| 8 | The ★ Reference strip, the [👁 Reference Track] toggle, [REF] and Shift+R (tap under 200 ms latches, hold auditions) | ADR-0209 d9, d11 | Not in this PR | the window's item after STACK (`window/mix-view-reference`); the role and the route wait for the engine (N23) |
| 9 | Nothing in it touches the audio thread; it reads the snapshot only, and a steady frame builds nothing | UI-ARCHITECTURE §2, §12; ADR-0201 | STACK's rows are rebuilt only when the tracks change; a steady frame with the Mix View open allocates nothing | built |
