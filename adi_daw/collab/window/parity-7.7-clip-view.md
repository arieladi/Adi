# Parity checklist — step 7.7, the Clip View and the MIDI Note Editor

Drafted by the window lane (ADR-0208) from Ableton's online Live 12 manual, chapters 8 "Clip View"
and 10 "Editing MIDI" (ableton.com/en/live-manual/12/, read 28 October 2026), for win to run against
Live 12 (ADR-0108). Rows describe the behaviour in our words, never the manual's. The plan is
STEP-7-PLAN §7.7; the picture is `pictures/2026-10-28-clip-view.png`, awaiting the director.

Status: **built**, **partial**, **open** (planned, not built), **waits** (a Need first),
**later** (the Master Reference places it later), **measure** (win checks Live and reports).

| # | Live's behaviour | Manual | ADI's plan | Status |
|---|---|---|---|---|
| 1 | The Clip View opens on a double-click on a clip, its toggle in the Status Bar, or Ctrl/Cmd+Alt+3; it always shows the selected clip | §8 | The same three ways; the detail row | open |
| 2 | Opened with the Device View open, the Clip View stacks above it | §8 | The detail row holds both, the Clip View on top | open; measure the split between them |
| 3 | Its top border drags to resize it; dragged to the window's bottom, it closes | §8 | The same, through the dock's resizer | open |
| 4 | Ctrl/Cmd+Alt+E toggles it to its full height | §10.2 | The same | open |
| 5 | The title bar: the clip's activator, name and colour; Rename and the colour from its context menu; a double-click folds the panels | §8.1.1 | The activator through `clip.setMute`; the name and colour shown once the snapshot carries them (N6, N32); Rename through `clip.setName` | waits (N6, N32) |
| 6 | Panels on the left, an editor on the right; the panels go vertical when dragged narrow, or by the View menu | §8.1.2 | Horizontal first; vertical later | open (horizontal), later (vertical) |
| 7 | The editor's tabs: Notes, Envelopes, MPE for MIDI; Sample, Envelopes for audio; Alt/Option+Tab switches | §8.1.3 | The tabs drawn; Notes and Sample first | open; Envelopes, MPE later |
| 8 | Main panel: Start and End with Set buttons; Loop; Position and Length with Set; Signature; Groove; Scale | §8.2 | Shown from the snapshot's position and length, read-only until the clip ops exist | waits (N33) |
| 9 | Duplicate (MIDI): doubles the loop and its notes, Ctrl/Cmd+D on the loop brace | §8.9, §10.7.3 | Through `note.insert` and the loop op | waits (N32, N33) |
| 10 | The Clip View Selector in the Status Bar: the whole clip in miniature; drag in it to scroll and zoom | §8.7, §10.2 | In the Status Bar's second field, which is free (the flip arrows are in Live's strip, #293) | open |
| 11 | Zoom and scroll as in the arrangement: drag the time ruler; Ctrl/Cmd+wheel zooms; Z zooms to the selection, X back step by step | §8.7, §10.2 | Shares the arrangement's zoom code | open |
| 12 | The note ruler: C-2 to C8 with a piano keyboard; scroll it for octaves, drag sideways to zoom key tracks; Alt/Option+wheel zooms them | §10.1, §10.2 | Middle C is C3, English names (the director's defaults; Master Reference 5.1) | open |
| 13 | Page Up/Down scroll an octave; with Shift, one key track | §10.2 | The same | open |
| 14 | A double-click on the note or time ruler zooms to the selection, or to the notes when nothing is selected | §10.2 | The same | open |
| 15 | The grid: its chooser on the editor's header, Ctrl/Cmd+4 switches snapping, Alt (Cmd on the Mac) bypasses it; notes snap to their offset from the grid | §10.2.1 | The arrangement's grid code; offset snapping is new | open |
| 16 | Follow scrolls the editor with playback; an edit pauses it | §8.7, §10.2.2 | The arrangement's Follow | open |
| 17 | A double-click adds a note or deletes one; Draw Mode (B) draws and deletes, pitch-locked by a setting, Alt/Option for the other way | §10.4 | `note.insert` and `note.delete`, one undo step per gesture | waits (N32) |
| 18 | Preview: the key sounds as you click the piano or add and move a note; with the track armed, step recording | §10.4.2 | Drawn, and says it waits for the engine's preview path | waits |
| 19 | Selecting: a click places the insert marker; a drag selects time and the notes in it; Enter toggles between the two; Esc collapses; Shift adds or removes; Shift on the piano takes a key track | §10.5.2 | The same | waits (N32) |
| 20 | The arrows: move the insert marker by the grid, or to note edges with Ctrl/Option; with notes selected, left and right move them, up and down transpose, Shift by octaves; Shift+left/right changes length | §10.5.2, §10.5.4, §10.5.5 | `note.move` and `note.resize` | waits (N32) |
| 21 | Ctrl/Cmd+A selects all notes; Ctrl/Cmd+Shift+A inverts the selection; Ctrl/Option+up/down selects the next note | §10.5.2 | The same | waits (N32) |
| 22 | Dragging a note moves it; its edges change its length; Ctrl/Option+drag copies, even after the drag began | §10.5.4, §10.5.5 | `note.move`, `note.resize`, `note.insert` | waits (N32) |
| 23 | A new note over an old one: it overwrites the old note's start or shortens its end | §10.5.4 | The same, in the gesture's undo step | waits (N32) |
| 24 | Note Stretch markers below the scrub area for a selection | §10.5.6 | Moves and resizes in one step | waits (N32) |
| 25 | 0 deactivates a note: grey, silent | §10.5.7 | An op for the note's active flag | waits (N34) |
| 26 | Split (E, Ctrl/Cmd+E), Chop on the grid (Ctrl/Cmd+E), Join (Ctrl/Cmd+J) | §10.5.8 | Once a note's identity is known | waits (N32) |
| 27 | Pitch and Time: Transpose, Fit to Scale, Invert, Interval and Add Interval, Stretch with x2 and /2, Duration with Set Length, Humanize, Reverse, Legato; with nothing selected they act on the whole clip | §8.5, §10.5.9 | Built from the five note ops; Fit to Scale needs a clip's scale | waits (N32; N33 for Fit to Scale) |
| 28 | Quantize: Ctrl/Cmd+U with the Quantize tool's settings (Ctrl/Cmd+Shift+U) | §10.5.11 | `note.move` | waits (N32) |
| 29 | The Velocity lane: drag a marker, Shift for several, type a value, Ctrl/Cmd+up/down by 10, Alt/Cmd+drag on a note; the lane's Randomize, Ramp and Deviation; note saturation shows velocity | §10.5.12 | `note.setVelocity`; Deviation later | waits (N32) |
| 30 | The lanes' selector shows Velocity, Release Velocity and Chance; the divider resizes them | §10.1, §10.5.13 | Velocity first; Chance later (P2) | waits (N32), later |
| 31 | Fold to Notes (F), Fold to Scale (G), Highlight Scale (K) | §10.6 | Fold first; the scale parts with Scale Mode (P1) | open (Fold), later |
| 32 | The loop brace and the start and end markers in the editor; the arrows nudge the brace, Ctrl/Cmd+arrows change its length | §8.8, §8.9 | Drawn from the clip's loop once it is readable | waits (N32, N33) |
| 33 | Crop Clip, Ctrl/Cmd+Shift+J | §10.7.1 | `note.delete` outside the loop | waits (N32) |
| 34 | Duplicate, Delete and Insert Time inside the clip | §10.7.2 | `note.move` and `note.insert` | waits (N32) |
| 35 | Multi-clip editing and Focus Mode (N) | §10.8 | Later (P2) | later |
| 36 | The audio clip: the Sample Editor's header (file, rate, depth, channels), the Audio Utilities panel (Warp, modes, BPM, Reverse, Edit, RAM, HiQ, Gain, Pitch) | §8.4, §8.10 | Drawn; the waveform and warping wait | waits (N5; 4.2) |
| 37 | — (ADI addition) the Clip View can be torn out to its own window | Master Reference 4.1, ADR-0063 | Through the floating host, reparented | open |
