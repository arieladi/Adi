# Parity checklist — step 7.2, the application: projects, history, the audio device

Drafted by the window lane (ADR-0208) from the Live 12 manual (`live12-manual-en.pdf`, the PDF's
pages), for win to run against Live 12 (ADR-0108). Rows describe the behaviour in our words.
Status as in `parity-7.2-shell.md`: built, partial, open, measure.

| # | Live's behaviour | Manual | ADI in this PR | Status |
|---|---|---|---|---|
| 1 | New Live Set: Ctrl/Cmd+N | §41.3, p. 986 | A new project with its Main track, written at once into the default projects folder (`files.projectsFolder`, else the app's own folder): ADI has no unsaved state (ADR-0003) | built; a known difference: Live's new Set is untitled until saved |
| 2 | Open Live Set: Ctrl/Cmd+O | §41.3, p. 986 | A file chooser for `.adi`; a file that fails to open leaves the current project open | built |
| 3 | Save Live Set: Ctrl/Cmd+S | §41.3, p. 986 | **Take Snapshot**: a named point in the history, the prompt pre-filled with the project's name and the time; not an edit, not an undo step (ADR-0203 d6, d12, d13) | built (the director's deviation) |
| 4 | Save Live Set As: Ctrl/Cmd+Shift+S, a new file the Set carries on in | §41.3, p. 986 | The key and the menu item exist; the Status Bar says Save As waits for the engine | open: the store has no Save As (LANES §3 item 6) |
| 5 | Creating a Set is the first entry in the Undo History and cannot be undone | §5.4.2, p. 131 | Undo stops at the New-project step and says there is nothing to undo; the Edit menu greys out | built |
| 6 | Opening a Set starts a fresh history (Live's history is not saved with the Set) | §5.4.2, p. 131 | ADI's history is in the file, so a reopened project undoes its earlier edits | ADI's addition (ADR-0003); never its creation |
| 7 | The Edit menu's Undo and Redo name the action | §5.4.2, p. 131; §41.5 | "Undo Create track" and the like, from the history's own labels | built |
| 8 | Ctrl/Cmd+, opens the Settings; audio is set up on its Audio page | §40.2, p. 975; §41.1, p. 985 | Opens "Settings: Audio", the driver/device/rate/buffer chooser, stored in App scope (`audio.*`) | partial: the Settings window in Live's shape is LANES §3b item 3 |
| 9 | — (ADR-0203 d11) a device that cannot be opened always warns, never switches silently | ADR-0203 d11 | An unavailable driver or a failed open is reported, and nothing else is opened in its place | built |
| 10 | Quit: Ctrl+Q / Cmd+Q | §41.3, p. 986 | The Mac's Cmd+Q quits; nothing is asked, since every edit is already in the file | partial: Ctrl+Q on Windows is not bound yet |
| 11 | The menu bar: File, Edit, Create, View, Options, Navigate, Help; on the Mac at the top of the screen | Live's menus | File, Edit, View, as far as step 7 has commands; the Mac's main menu | partial |
| 12 | Space plays the project through the audio device | §41.20, p. 1001 | The driver drains the UI's queue between blocks; the bar reads its feedback; the tempo is read at the device's granted rate | built |
