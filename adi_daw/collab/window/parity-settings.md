# Parity checklist: the Settings window

Drafted by the window lane (ADR-0208) from ADR-0203 d11, Master Reference Part V.1 (whose
keyboard rows come verbatim from Live 12 §40.1.2), and Live 11.2.7's Preferences window as
captured on the director's Mac on 2026-10-27. It is for win to run against Live 12 (ADR-0108).
Status as in `parity-7.2-shell.md`.

| # | Live's behaviour | Source | ADI in this PR | Status |
|---|---|---|---|---|
| 1 | Ctrl/Cmd+, opens the Settings; the Live menu on the Mac, Options on Windows | §41.1, p. 985; V.1 §2 | Ctrl/Cmd+, opens one floating window, kept for the session, with the same place and size when reopened | built; remembering it across launches waits for an app-scoped home for window places (N4's kind) |
| 2 | A page chooser down the left, one page at a time | V.1 §1; Live 11 measured | Live's tabs: 80-unit rounded boxes on the frame's grey, a word to a line, the open one joined to its page; the page a rounded panel | built |
| 3 | Live's ten tab names in Live's order, ADI's own after them | ADR-0203 d11; V.2 | The registry's pages in the registry's order | open: the names are the registry's (window log, N14) |
| 4 | Each page in titled sections with a rule (Audio Device, Sample Rate, Latency, Test) | Live 11 measured | The Audio page's device part has Live's three sections. Every page has one heading, its name, until the registry carries sections | partial (N14) |
| 5 | Values as switches, choosers and value boxes; names at 17, controls at 191, choosers 197 wide, boxes 97, a 21-unit pitch | Live 11 measured | As measured, typed from the registry: switches read On or Off, choosers list the allowed values, ranged numbers are value boxes, text is a field | built |
| 6 | A change applies at once; no OK button | V.1 §1 | Every change is set and written at once; a refused value is not written and the help line says why. What the application keeps of the settings follows at once too: the Info View, tooltips (on, off and their delay) in every open window, Zoom on Selection and the shortcut preset (`window/settings-apply`; until then those waited for a relaunch) | built |
| 7 | The Audio page: driver type, input and output devices, sample rate, buffer size, the latencies | §40.2; Live 11 measured | `AudioDevicePage` over the application's device manager; the manager's listener writes `audio.*` | built; Channel Configuration and Driver Error Compensation beside the latencies are open |
| 8 | Tab / Shift+Tab move between controls | §40.1.2 | JUCE's focus order | built |
| 9 | Up/Down toggle, adjust or cycle a focused value; Enter toggles | §40.1.2 | A switch takes either arrow or Enter; a value box steps by one (a real number by a hundredth of its range); a chooser cycles (JUCE). Each change is applied and written at once. The keyboard's focus is outlined in the accent; a click's is not | built |
| 10 | Ctrl+Tab / Ctrl+Shift+Tab (Option+Tab on the Mac), or the arrows when the chooser is focused, move between pages | §40.1.2 | All three | built |
| 11 | Esc closes | §40.1.2 | Esc closes, from the page or the Find box | built |
| 12 | REAPER's Find box; typing jumps to the hits | R-02; V.1 §1 | Every match from every page, under its page's heading; clearing it goes back to the page | built; related words ("beat" finds "tempo") are the registry's search (win) |
| 13 | The hovered setting's help in a line at the foot | V.1 §1 | The help line at the page's foot | built |
| 14 | A Defaults button per page | V.1 §1 (Cubase) | Restore Defaults puts the open page back | built |
| 15 | A setting's scope is shown: Project and Device settings carry their mark | R-03; d11 | A Project or Device mark beside the name; the control is greyed, since the value is changed by its op or kept by its device | built; editing a project's settings through their ops is open |
| 16 | Language change requires a restart, said beside the chooser | §40.1.2 | — | open: no language setting yet |
| 17 | Named profiles saved and loaded | d11 | — | open |
