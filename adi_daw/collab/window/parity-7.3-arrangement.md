# Parity checklist — step 7.3, the arrangement

Drafted by the window lane (ADR-0208) from the Live 12 manual (`live12-manual-en.pdf`, the PDF's
pages) and ADR-0129's table, which is adopted, not duplicated (STEP-7-PLAN §6). For win to run
against Live 12 (ADR-0108). Rows describe the behaviour in our words. Status as in
`parity-7.2-shell.md`: built, partial, open, measure.

| # | Live's behaviour | Manual / ADR | ADI in this PR | Status |
|---|---|---|---|---|
| 1 | Tracks stack vertically; the track headers sit to the right of the clips, beside the mixer controls | §6.1, p. 162 | Headers on the right of the canvas, 128 units wide; the mixer panel beyond them | measure: confirm the side, and the default header width |
| 2 | + and - zoom around the current selection | §6.2, p. 163; ADR-0129 | Around the time selection's middle, else the insert marker, else the playhead (ADR-0129 deviation 2) | built |
| 3 | Ctrl/Cmd+wheel zooms around the selection | §6.2, p. 163; ADR-0203 d7 | As + and -, when `lookfeel.zoomOnSelection` is on (the default); around the cursor when off. ADR-0129's cursor deviation is reversed by ADR-0203 d7 | built |
| 4 | Ctrl+Alt / Cmd+Option drag pans | §6.2, p. 163 | Pans both ways | built |
| 5 | Shift+wheel scrolls horizontally | §41.9, p. 991 | Built | built |
| 6 | Alt+wheel sizes the track under the mouse; with a time selection, every track with selected content | §6.2, p. 163 | Built | built |
| 7 | Alt while resizing one track resizes all; Alt+pinch sizes tracks | §6.9; ADR-0129 | Built (the header's lower drag; pinch with Alt) | built |
| 8 | Z zooms to the time selection; X goes back one step per press | §6.2, p. 163; §41.16 | Up to 32 steps back | built |
| 9 | H and W optimise the arrangement's height and width | §6.1, p. 162 | Built; the View menu holds both | built |
| 10 | Beat-time ruler: drag sideways scrolls, vertically zooms; double-click zooms to the selection, or out to the whole arrangement | §6.1, p. 161 | Built | built |
| 11 | The ruler counts bars-beats-sixteenths in the project's meters | §6.1, p. 161 | Counts in 4/4: the snapshot carries no time signatures yet (collab/window.md, N6); the tooltip says so | partial |
| 12 | A click in the track area places the insert marker; Space plays from it | §6.3; §41.20 | Built; the marker is drawn as a line when there is no time selection | built |
| 13 | Home moves the insert marker to the beginning | §41.20, p. 1001 | Built | built |
| 14 | Shift+Space continues from the stop point | §41.20, p. 1001 | Built | built |
| 15 | Insert Audio Track Ctrl/Cmd+T; Insert MIDI Track Ctrl/Cmd+Shift+T, after the selected track, which the new one replaces as the selection | §41.19, p. 1000 | Built, in the Create menu and the right-click menus | built |
| 16 | A track header's right-click menu holds the track's commands | §6, the track header's context menu | Insert Audio/MIDI Track, Rename and Delete Track so far | partial: Group, Freeze and the rest arrive with their ops |
| 16a | Rename (Ctrl/Cmd+R, the Edit menu) edits the selected track's name in its title bar; Tab moves on to the next track's title to rename a series | §18.2, p. 394; §41.5 | Built (`window/rename`): Enter commits one undo step, Esc cancels, Tab and Shift+Tab move on; focus leaving commits | built |
| 16b | A name that begins with # shows the track's number, which follows the track when it moves; more # add leading zeros | §18.2, p. 394 | Built: new tracks are named "#-Audio" and "#-MIDI" and show their position | built; measure Live's default names exactly |
| 17 | The Main track is the arrangement's last row | §6 | Listed where the snapshot puts it, since the snapshot does not say which track is Main (N6) | partial |
| 18 | Clips show their names | §6 | Drawn unnamed until the snapshot carries clip names (N6) | partial |
| 19 | Dropping an audio file on a track makes a clip there | §6.1; §9 | The drop is accepted and the Status Bar says it waits for the engine's audio-clip op (N5); nothing is written | open |
| 20 | The Overview above the ruler: the whole arrangement in miniature, an outline around what is on screen; drag sideways scrolls, vertically zooms; a double-click zooms out fully | §6.1, p. 160–161; ADR-0129 | Built (`window/overview`): 16 units tall, a row per track, the outline drawn over it; the drag moves by the Overview's own scale | built; measure its height and the outline's colour. ADR-0129's edge handles (deviation 3) wait until these rows pass |
| 21 | Scrub area: a click launches playback from that point | §6.1, p. 161 | — | open: with locators |
| 22 | The playhead never repaints the arrangement | ADR-0050 d2 | A one-pixel component above the canvas, transparent to the mouse; a moving playhead allocates nothing | built |
