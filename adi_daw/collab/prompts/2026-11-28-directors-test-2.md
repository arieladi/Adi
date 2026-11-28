# The director's second test — queue, 28 November 2026

**What this is.** On 28 November the director worked in ADI on his Mac a second time: he recorded, drew
notes, saved, quit and reopened, edited, opened the built-in devices, drew automation, looked at the
analysers and exported. What follows is what was wrong, most harmful first (he corrected two points the same day; they are folded in). Each item is a row of
`docs/SANITY-CHECK.md` (SC-447 to SC-477, and a note on SC-347). His words are quoted where they decide
something.

Read `collab/prompts/2026-11-27-basics-howto.md` first. You are the session that worked
`collab/prompts/2026-11-28-directors-test.md`; your report stays `collab/basics/test.md` on
`basics/test-log`. Your first chain is on main (#830). Start a new chain, `basics/test-chain-2`, **from
`origin/main`**. For each item look first whether main already does it (several sessions published work
since), say so with the commit, and go on.

ADI behaves as Ableton Live 12 unless Live lacks the ability.

**Three things win had to fix on your first chain before it could land; do not repeat them.** A local
that hid an earlier one (MSVC C4456 under warnings as errors). Two headless suites outside the ones you
ran, Clip Playback and Link Playback, still held the old Loop rule: **run the whole headless set
(`ctest -LE performance`) before you call a chain verified, not only the suites you judge affected.**
And the acceptance walk's driver outran the recorder's writer once a new song had no returns. For window
checks: every word in a compared picture is blacked out; a picture read in a check is a
`juce::SoftwareImageType` picture whose `Graphics` is closed first; a path compared as text is written
the system's way.

## Queue, in this order

1. **SC-449: Redo on macOS.** Cmd+Shift+Z, Live's key there, does nothing, and neither does Cmd+Y. Undo
   works.
2. **SC-475, with SC-184: Split cuts where I am.** Cmd/Ctrl+E at the insert marker cuts the track the
   marker is on, or the selected clips, as Live's does. Today it cuts every track at that time. The safety
   session published SC-184 on `basics/sc-184-split-undo`: read it first, it may already answer this.
3. **SC-453, with SC-229: Quantize.** Quantize Settings opens its window; Cmd/Ctrl+U quantizes the
   selected notes. As far as he noticed they do nothing, and the settings are thin beside Live's: the
   window has Live's settings (Quantize To and its choices, Adjust Note Start and End, Amount) and each
   one acts.
4. **SC-451: pinch zooms.** A pinch on the trackpad zooms the Note Editor ("we should fix it as soon as
   we can"); check the Arrangement and the Sample Editor in the same pass.
5. **SC-459: an automated control moves.** The sound follows the automation and the dial in the Device
   View stands still.
6. **SC-460: a mixer control's menu.** A right-click on a track's volume, pan or send gives Show
   Automation, Delete Automation and what else Live's has. He drew automation on a track's volume by
   mistake and found no way to delete it. A check of every right-click menu against Live's is being
   made (SC-478): do the mixer's controls now, and expect more rows from it.
7. **SC-457, SC-462: the Info box.** Info text shows only while the pointer is on something that has
   text; over empty space in the Arrangement or the Browser the analyser stays. The analyser has no
   "Spectrogram" title and no empty row: it uses the whole box.
8. **SC-469, SC-470: Export.** It starts with no dither. When it is done the Status Bar names the file,
   with a button that shows it in Explorer or Finder. Playback keeps running during an export: his
   choice (ADR-0266 d4). Do not change that.
   Then **SC-476: a clip stops at the end of its audio.** His choice, as Live: a clip that does not
   loop cannot be drawn out past the end of its audio; a looped clip can, and repeats (SC-448).
9. **SC-452: the Note Editor shows its grid,** where Live writes it, following Cmd/Ctrl+1 and +2.
10. **SC-458: the lane follows the control I touch** in automation mode, as Live's does.
11. **SC-347: Alt and drag bends a segment.** "Option does nothing." Look on main first: #830 carried the
    automation chain's curve gestures.
12. **SC-448: a looped clip looks looped** in the Arrangement: each repeat drawn, with the mark where the
    loop turns, as Live draws it.
13. **SC-468: the loop brace's ends change the pointer,** as in Live.
14. **SC-472: the words shown while a project is read** end in odd characters, as from an ellipsis read
    in the wrong encoding.
15. **SC-467: the Analyser Dock switch** changes nothing he could see. Find what it is meant to do and
    make it do that visibly, or say in your report why it should go.
16. **SC-461: stray automation points after a clip is moved.** Do not fix yet: write in your report what
    ADI leaves at each edge, with the numbers, so that win can set it beside a reading of Live.
17. **Left from your first queue:** its items 15 and 16 (DF-W64, a double-click on the empty area under
    the tracks adds a track; DF-W65, the setting that lets the Loop switch take the selection), then
    SC-446 (in a window 960 wide the Control Bar draws Key, the field beside it and MIDI over one
    another, and the Arrangement and Mix switch covers the end of the position field).
18. **SC-450: Undo and Redo buttons,** two small arrows in the Control Bar, greyed when there is nothing
    to undo or redo, their Info text naming the step. An addition, his word.
19. **SC-447: a take with parts cut, seen once.** His first take of the day came out with parts of the
    audio missing; every take after it was whole, and his word is "don't break yourself over this".
    One bounded attempt: record while clicking, opening menus and dragging in the window, and compare the
    take with what was fed. If it does not show, write what you tried and stop. One thing holds either
    way: a take never loses audio silently; if audio was lost the take says so.

## Not yours: these wait for a picture or a decision

The look is settled from pictures before it is built (ADR-0243). Win draws these sheets with the
director; build none of them until a sheet is agreed. SC-454 (EQ Eight opens as the empty panel of a hosted
plug-in) went to the devices seat, which owns that file. The sheets: SC-455 (the built-in devices' panels, as Live's),
SC-464 to SC-466 and SC-463 (the Analysers page: the Classic Scope, one level meter panel, a window of
its own, the colours), SC-471 (the Export page). SC-456, SC-473, SC-474 and SC-477 are lists win makes against the plan.

## What he said works

Recording (the second take), playback, saving and reopening, edits kept after a quit without saving,
Cmd+D, copy and paste between tracks, drawing and resizing notes and their velocity, drawing automation
and hearing it, showing and hiding automation, Show Automation from a device's dial, the spectrogram, the
export itself (the file played elsewhere). Do not change these.
