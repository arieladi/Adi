# The director's own test — queue, 28 November 2026

**What this is.** On 27 November the director opened ADI himself on his Mac, on main after the ninth
stack, and worked in it for a while. What follows is what stopped or annoyed him, in the order it
bothered him. These are the faults that matter most to the one person ADI is built for: each is a row of
`docs/SANITY-CHECK.md` (SC-433 to SC-445, and notes on SC-175, SC-176, SC-201 and SC-425).

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/test.md` on
`basics/test-log`; your work is one chain branch, `basics/test-chain`, started **from
`origin/basics/integrate-2`**, because the edit, clips and Browser sessions' newer work is gathered there
and some of it may already answer a row below. For each item, look first whether that branch already does
it, say so with the commit, and go on. Merge that branch into yours when it moves.

ADI behaves as Ableton Live 12 unless Live lacks the ability. He compared every point with Live open
beside ADI. His words are quoted where they decide something.

## Queue, in this order

1. **SC-433: no black box.** A hover today shows a large tooltip with the whole Info text and its list of
   keys over the lanes: "this black box with text is extremely annoying ... showing info in the info box
   is great but what is that". The Info box is the one place for that text. No tooltip carries it; a
   control that is only an icon may show its short name.
2. **SC-439, with SC-175: a click does not select.** "i am always marking even when i dont want to".
   A click in the lanes places the insert marker and selects nothing; the marker is drawn as Live's, a
   thin line with its flag at the top, where he stands; a blue range appears only by a drag. Today a
   click leaves one grid step selected (the Status Bar reads "Time Selection ... Length 1.0.0").
3. **SC-441: the metronome, and Loop.** With the metronome on, Play gives no click. With the Control
   Bar's Loop on, the song is silent, the click sounds, and Play does nothing until Loop is off. Seen on
   macOS with the built-in output; find whether it is the same elsewhere.
4. **SC-445: Loop does not hold the playback.** "i cant get out of it ... if i am trying to go elsewhere it
   doesn't play there it plays the loop". As Live: Play from before the brace runs into it and then loops;
   Play from after it plays on and never loops. This is also the second half of item 3: a new song's brace
   lies on its empty first bars, so the looped silence left only the click.
   Then **SC-440, with SC-176:** Ctrl/Cmd+L already loops the selection (he found the key the next day). The
   menu on a time selection, which offers only Delete, the two Insert Track commands and the grid today,
   gets Loop Selection and what else Live's has.
5. **SC-425: plug-ins on macOS.** None was found and none could be loaded. One unreadable folder inside
   the system's VST3 folder stops the whole scan. The scan skips what it cannot read, says so, and lists
   the rest. On a Mac this is first-hour.
6. **SC-442: Ctrl/Cmd+1 and Ctrl/Cmd+2** make the grid narrower and wider.
7. **SC-444, with SC-203: a fade's curve** bends the way Live's does when its handle is dragged. Today the
   drag is reversed (it behaves as Live's with Alt held).
8. **SC-201: the audio clip's editor.** It "feels mostly empty" beside Live's. Transpose in semitones and
   cents, and Warp's controls where the engine has them; say what the engine lacks.
9. **SC-435: Main and a return are resized from their top edge.** They are anchored at the foot and grow
   upward. Today Main's handle is at its foot.
10. **SC-436: a track's header follows the drag.** While a track's height is dragged the lane follows at
    once (he liked that: "even better then in ableton") and the header on the right jumps only when the
    mouse is released.
11. **SC-434: the colour chooser** offers Live's whole palette.
12. **SC-437: a new song opens clean,** with no return tracks in it: "i don't want them at all in the
    default preset, screen should be clean".
13. **SC-443: a magnifying glass** over the strip where a drag zooms the Arrangement.
14. **SC-438: an empty group, and a song with every track deleted.** ADI allows both where Live refuses,
    and he likes that. Prove it causes no trouble: save, reopen, export, routing, freeze, the agent's
    read. Fix what you find.
15. **DF-W64: a double-click on the empty area under the tracks adds a track,** his wish of 28 November
    (ADR-0265 d5, an addition: Live does nothing there). The first time it asks which kind, a MIDI track,
    an audio track or an FX channel, and it remembers the answer, as ADI's other remembered choices do; a
    setting changes it later.
16. **A setting: the Loop switch takes the selection** (his word of 28 November: "lets add that as an
    option in settings"). Off, the default, is Live's way: the switch turns the existing loop on and off.
    On, a click on the switch while time is selected loops that selection, as Ctrl/Cmd+L does; with
    nothing selected it turns the existing loop on and off. An addition (DF-W65, win registers it).

## What he said works

A file dropped in plays, its waveform and playhead run smoothly, the meters read right, Play and Stop are
exact, the tracks and groups look right, resizing a lane by a drag is live. Do not change these.

A note on Loop, from his second look: the Loop switch plays the brace where it lies and Ctrl/Cmd+L moves
the brace to what is selected. That is Live's way and it is right; only SC-445 is wrong there.
