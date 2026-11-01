# Parity checklist — step 7.5, the floating-window host

Drafted by the window lane (ADR-0208) from the Live 12 manual (`live12-manual-en.pdf`, the PDF's
pages), STEP-7-PLAN 7.5 and ADR-0180 d3, d7, for win to run against Live 12 (ADR-0108). Rows
describe the behaviour in our words. Status as in `parity-7.2-shell.md`.

| # | Live's behaviour | Manual / ADR | ADI in this PR | Status |
|---|---|---|---|---|
| 1 | A plug-in's own window floats, opened from the wrench toggle in its title bar | §23.3, p. 461–462 | — | open: the hosts expose no editor yet (window log, N8) |
| 2 | Ctrl/Cmd+Alt+P hides and shows every open plug-in window; Ctrl/Cmd on the toggle opens several | §41.4, p. 987 | — | open, with row 1 |
| 3 | Undocking moves the same view into a window and back, never a copy | ADR-0063 d1, ADR-0180 d3 | `FloatingHost::detach` reparents the component; closing docks it at its exact bounds | built |
| 4 | A second view of one model, both alive at once (the analyser's docked panel and its big window) | ADR-0116 d1/d4, ADR-0183 d4 | `FloatingHost::openView` with a shared `AnalyserModel`; both views read one measurement | built |
| 5 | A window reopens where it was, on the same monitor, clamped to the screens there are now | SPEC §8.4 (MUST) | The rectangle and the monitor's identity are kept in `ui_view`; a lost monitor clamps onto one that exists | built |
| 6 | Each window has its own frame clock | ADR-0180 d1 | One `VBlankAttachment` per floating window; its drain counts its own frames | built |
| 7 | Keys work in every window: Space stops the transport from a plug-in window too | ADR-0180 d6 | A floating window's keys go to the application's command layer | built |
| 8 | Deleting a device closes its window; undo brings the device back, not the window | ADR-0180 d7 (the director's ruling) | The host polls the device's retirement each frame; the closed state is saved | built |
| 9 | Floating tooltips in every window | ADR-0203 d12 | One tooltip window per floating window | built |
| 10 | A floating window's interface zoom | ADR-0203 d6; ADR-0129 d3 | Ctrl/Cmd + and - in an ADI view's window (Channel EQ, the analyser) zoom that window alone, between 50 % and 200 %, as the main window's; every other key there is still the main window's. Plug-in windows are never scaled by ADI | partial: held for the session, until each window's zoom has an app-scoped home (window log, N4) |
| 11 | The analyser's big window | ADR-0183 d4 | The view and its shared model are built and tested; nothing opens it until the analyser device (the devices lane's) declares its second view. #204's per-device "Analyse" button is gone: Live has none | open |
