# Parity checklist — step 7.4, the device strip and the DAW-drawn panel

Drafted by the window lane (ADR-0208) from the Live 12 manual (`live12-manual-en.pdf`, the PDF's
pages), for win to run against Live 12 (ADR-0108). Rows describe the behaviour in our words.
Status as in `parity-7.2-shell.md`: built, partial, open, measure.

| # | Live's behaviour | Manual / ADR | ADI in this PR | Status |
|---|---|---|---|---|
| 1 | The Device View shows the selected track's chain, devices left to right | §3; §23, p. 443 | The strip follows the arrangement's selected track; devices left to right; it scrolls sideways, never down | built |
| 2 | The Device View has one height; ADI lets the strip grow upward only, from a floor of Live's height | ADR-0184, ADR-0203 d7 | Floor 169 units (unverified), a desktop default; the top edge drags it taller; the ceiling leaves the transport, the ruler and one row | built; measure the 169 |
| 3 | A device's title bar holds its Activator, which turns it off like deleting it for the time being | §23.2.1, p. 443–444 | The activator submits `device.setEnabled`, one undo step | built |
| 4 | A device folds to a narrow bar from its title bar | §23 | Double-click the title, or the right-click menu's Fold; remembered per window in `ui_view`, outside the history | built; measure the gesture (double-click versus the title's toggle) |
| 5 | Every device has a context menu from its title bar (Cut, Copy, Rename, Delete and the device's own) | §23.2.1, p. 446 | In Live's order: Delete; Rename; Fold/Unfold and Configure. Rename, from the menu or Ctrl/Cmd+R with the device focused, is typed in the title bar: Enter, Tab or a click away commits one `device.rename`; Esc or an empty name keeps the name | partial: Cut, Copy, Paste and Duplicate wait for a device's copy (window log, N18); Group and Save as Default for their ops |
| 6 | Delete removes the selected device; undo brings it back | §23 | Delete or Backspace on a focused device submits `device.remove` | built |
| 7 | A plug-in's panel shows its parameters as Live draws them; Configure chooses which (Live: click a parameter in the plug-in's window) | §23.3.1.2, p. 462 | Configure shows a search; its menu adds or removes parameters (`device.setPanel`), and restores the default panel; Live's way (clicking in the plug-in's window) needs the floating windows of 7.5 | partial |
| 8 | Deleting a configured parameter that has automation, envelopes or mappings warns first | §23.3.1.2, p. 462; STEP-7-PLAN §7 item 6 | — | open: the warning needs the lanes and mappings read from the snapshot |
| 9 | Live's count rule: 64 parameters or fewer show by default, more need Configure | ADR-0198, `panel::resolve` | The strip shows what `panel::resolve` returns and its hint when it asks for Configure | built |
| 10 | A parameter's value reads in the plug-in's own words | ADR-0198 | `DeviceInstance::paramText` through the publication | built |
| 11 | Automated parameters are marked; an overridden one too | §21; ADR-0162 | A red dot when automated, orange when overridden | measure the colours and the mark's place |
| 12 | Dragging a control changes the parameter, one undo step per gesture | ADR-0124, ADR-0181 d5 | The control sends a bounded begin/value/end gesture; the document refuses it and the Status Bar says why: ParamOps has no UI producer yet (window log, N7) | open |
| 13 | The mouse wheel over a control only scrolls | ADR-0203 d11 | Sliders ignore the wheel | built |
| 14 | An Airwindows suite device shows only its active algorithm's controls | ADR-0198 | `panel::activeAlgorithmParams` | built |
