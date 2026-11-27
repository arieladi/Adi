# Readings of Ableton Live 12 for the basics, 27 November 2026

**What this is.** ADI behaves as Live 12 unless Live lacks the ability. The sanity check of 26 November
(`docs/SANITY-CHECK.md`) left questions that only a look at Live itself answers. You have Live 12 on your
machine: answer them by doing each thing in Live and writing down what happens.

**Rules.**
- Use a new Set for each group of readings. Never save over any Set that exists; when Live asks, do not
  save. Change no Setting: where a reading asks for one, read it and leave it. Touch nothing in the
  library.
- The default Set may be the user's own template. Say at the top of your report whether it looks like
  Live's factory default (two MIDI and two audio tracks, two returns) or not.
- Write what you measured: values, texts, positions in pixels with the window's size. Commit no
  screenshot, no machine name, no user name, no path.
- If a reading needs an audio input and there is none, use an audio track whose input is Resampling
  while another clip plays, and say so.

**Your report.** `adi_daw/collab/basics/live-readings.md` on the branch `basics/live-readings-log`,
pushed after every few readings:

| R | What Live 12 does | How it was observed |
|---|---|---|

Give Live's exact version at the top. When the list is done, fetch this file again from the branch
`win/live-readings`: win adds readings as the deeper check asks for them.

## The readings

**The Arrangement's ruler and grid (SC-70, SC-71, SC-76)**

- R1. A new Set, Arrangement View. Give the window's width, the bar number at the timeline's left and
  right edges, and so the pixels per bar. Which bar numbers carry a label?
- R2. Where exactly does the grid's spacing text sit (which row, which corner, in pixels from the
  timeline's edges), and what does it say in a new Set? Where is the time ruler (seconds) and what are
  its labels?
- R3. Zoom in step by step from the whole song to the closest zoom. At twelve or more levels, write the
  pixels per bar, the grid text, and the form of the ruler's labels (1, 1.2, 1.1.2, and so on). The aim
  is the rule that turns a zoom into a grid spacing and a label step.
- R4. How do bar lines, beat lines and finer lines differ in a lane (brightness, thickness)?
- R5. In a new Set, is Follow on?

**The transport (SC-02, SC-03, SC-72, SC-73, SC-75)**

- R6. While the song plays, click in an empty part of a lane at a later bar. Does playback jump, or does
  only the insert marker move? The same click in the scrub area above the tracks. The same two clicks
  while recording.
- R7. Play, stop at some bar, then press Stop again. Where is the position after each press? Where does
  Space start from then? What does Shift+Space do after a stop?
- R8. Press Home. Does the view scroll to bar 1? Type a far position into the Control Bar's position
  field: does the view scroll to show it? Jump to a locator far away: the same question.
- R9. The Control Bar's tempo, position, loop start, loop length and signature fields: for each, does a
  drag change it, the arrow keys, the mouse wheel, typing? What are the steps, and what does Shift or
  Ctrl change?

**Tracks and the mixer (SC-01, SC-05, SC-80, SC-82, SC-90, SC-92)**

- R10. The Main track: can it be deleted, renamed, moved, duplicated? List its context menu.
- R11. The Edit menu's wording and what Delete does in three states: a clip selected; a track's header
  selected; a time range selected. With three track headers selected, does Delete remove all three in
  one undo step?
- R12. Select a track with clips, a device and automation, press Ctrl+D. Where does the copy appear,
  what is it called, what came along, is it armed?
- R13. A volume fader, a pan control and a send knob: what does a double-click do, the Delete key, a
  Shift+drag, typing a number? What are the reset values?
- R14. Deactivate a track: how do its clips and its header look? Switch a device off: how does it look?
- R15. The label of the Audio To chooser's entry for the main output.

**MIDI (SC-50 to SC-54, SC-62, SC-63)**

- R16. In a new Set, drag an instrument onto an empty MIDI track. Is the track armed afterwards? Create
  a new MIDI track with Ctrl+Shift+T: is it armed? Is the previous one disarmed?
- R17. In Settings, the MIDI ports table: read which of Track, Sync and Remote are on for the input and
  output ports. Leave it as it is.
- R18. A MIDI track with no instrument: what does its meter show when notes arrive? Does anything
  sound?
- R19. Arm a MIDI track and record into the Arrangement. What is drawn while recording: do the notes
  appear as played, does the clip grow? Does the computer keyboard (the M key) play notes, and which
  keys change octave and velocity?
- R20. In a MIDI clip's Note Editor, click among the notes at a later bar and press Space. Where does
  playback start? Did the Arrangement's insert marker move?
- R21. In the Note Editor, click a note, draw a note, move a note: is each heard, and under which switch?
  Do the piano keys light while a note sounds?

**Recording audio (SC-20 to SC-28)**

- R22. Record audio into the Arrangement. How does the take look while it records (the waveform growing,
  its colour, its name)? About how often does the picture update?
- R23. The count-in: its choices, and what the position display and the screen show during it.
- R24. Punch-in and punch-out with the loop: what is recorded and what is the resulting clip's length?
- R25. Record a second take over the first: what happens to the first clip?

**Audio clips (SC-11, SC-30 to SC-39)**

- R26. Drop an audio file with Warp off, then raise the tempo by 30 BPM. Does the clip's length in bars
  change? Does the whole sample still play?
- R27. Drag a clip's left edge to the right and back to the left past its first start. What is revealed?
- R28. Reverse a clip: what happens, and how long does it take on a long file?
- R29. Drop a file onto the middle of an existing clip in a lane: what does it replace?
- R30. A looped clip stretched over several repeats: how are the repeats marked?

**The project and the Browser (SC-07, SC-100, SC-105, SC-106, SC-110, SC-111)**

- R31. In a new unsaved Set, record a clip, then save the Set into a new folder. Where is the recording
  afterwards? Then Save As into a second folder: is the recording copied, or does the new Set point at
  the first folder (see File, Manage Files)?
- R32. File, Open Recent Set: how many entries, in what order, is there a Clear command? Where does
  File, Open Live Set start?
- R33. Save As over an existing name: what is asked?
- R34. In the Browser, on a sample: what do a double-click, Enter, and a drag below the last track each
  do? The same for an instrument and an audio effect.
- R35. The widths of the Browser and of the Info View at the default window size, and the Info View's
  height. What does the search field read when empty?

**From the deeper check (added 26 November)**

- R36. New Set, one long clip, click an empty place so that nothing is selected, Loop switch off: open Export. Read Render Start and Render Length (the whole song, or the loop brace?). Note whether Include Return and Main Effects starts ticked. (SC-394, SC-397)
- R37. Click once in the body of an Arrangement clip and press Delete: is the clip deleted? Then drag inside a clip from beat 6 to beat 8 and press Delete: what goes? (SC-175)
- R38. Select time, then right-click inside it: does the selection stay? Write down every entry of that menu, of a clip's menu and of a track name's menu. With three tracks selected, right-click one: are all three still selected? (SC-174, SC-176)
- R39. Select time across three tracks over several clips, press on a clip inside it and drag: does everything selected move? (SC-181)
- R40. Copy a clip, click an empty place, press Ctrl+V twice: two copies end to end, or one? (SC-183)
- R41. Insert Silence with only the insert marker placed: does it ask an amount? Create a MIDI clip over a clip that is there: is it replaced? (SC-187)
- R42. Ctrl+E with nothing selected: what is split? Select All with no clips: is all time selected? Rename with several tracks or clips selected: are all renamed?
- R43. Add Locator twice in a row, stopped and while playing: two locators, or does the second press delete the first? Is a new locator selected when it is made? (SC-354)
- R44. In a fresh Set, read Monitor on a new audio track and on a new MIDI track. (SC-249)
- R45. Record over an existing clip with Monitor on Auto, then on Off: through the overlap, is the input heard, the old clip, or both? (SC-245, SC-26)
- R46. With no track armed, press Arrangement Record: does the transport roll in record? (SC-335)
- R47. Arm a track and press Ctrl+Z: is the arm undone, or the edit before it? The same for Solo, Monitor, Record Quantization, the Loop switch and the Metronome. (SC-280)
- R48. While Arrangement Record runs, press Ctrl+Z. (SC-258)
- R49. Click the unit box beside Track Delay: does it switch units? (SC-274)
- R50. With an audio track selected, double-click an instrument in the Browser: a new MIDI track, or a refusal? (SC-287)
- R51. Drag a sample onto a MIDI track's lane in the Arrangement: is an instrument made? (SC-283)
- R52. Copy a device, click another track's header, press Ctrl+V. (SC-300)
- R53. Select a device and press 0: the device goes off, or the track? Select a track's header and press 0.
- R54. Open a third-party plug-in's window and switch to another program: does the plug-in window stay above it? (SC-313)
- R55. In Automation Mode, click a breakpoint once: deleted, or selected? (SC-346)
- R56. In Automation Mode, select time over a lane that has breakpoints and a clip, press Delete: the automation, the clip, or both? Copy it and paste on another control's lane. (SC-336)
- R57. Automation: Alt and a drag on a segment; Shift while dragging a point (finer? held to one axis?); a point dragged past its neighbour. (SC-347, SC-348)
- R58. Loop a MIDI clip in the Arrangement over several repeats, open it and play: does the editor's playhead stay inside the loop on the second pass, and does Follow keep the notes in view? Note the pointer over a note's two edges. (SC-222, SC-223)
- R59. Select a MIDI clip in the Arrangement without clicking in its editor, press Ctrl+U: quantized? (SC-229)
- R60. In the Note Editor with a note selected: Alt+Left and Right (walk to the next note, or nudge off the grid?); Ctrl+Shift+Up and Down; Alt against Shift with the arrows on the clip's region; plus and minus on a selected group track; Ctrl+Shift+F. (SC-235)
- R61. Legato on the notes of a looped clip: does the last note reach the loop's end? Read the Quantize chooser's two mixed values. (SC-236, SC-244)
- R62. In the Clip View of an unlooped Arrangement clip, move End later: does the clip grow in the Arrangement? Transpose a clip with Warp off; read the Sample Editor's ruler for such a clip. (SC-201, SC-209)
- R63. Save a Set that uses a sample outside its Project, move the Set file alone to another folder, open it: is the sample found? Record into a new Set and save it: are the recordings moved into the new Project? (SC-150, SC-148, SC-07)
- R64. Drop a Set file from the file manager onto Live: opened? Copy a clip, open another Set, paste. (SC-164, SC-190)
- R65. Drag in a MIDI file of several tracks: what is made, and is its tempo taken? (SC-57)
- R66. Drop several audio files with Ctrl held: onto the tracks below, or onto new tracks? Does the Browser show file extensions by default? Does Esc cancel a drag?
- R67. Open a plug-in's window (a third-party VST3), click in it away from any text field, select a track in Live's window first, and press Delete with the plug-in window in front: is anything in Live deleted (the track, the device, a clip)? The same with Backspace. Then Ctrl+Z. (SC-323; the walk of 27 November)

**The Browser's preview, loading and keys (added 27 November; owed by ADR-0254 d26; ADR-0261)**

- R68. The Preview switch (the headphones at the Browser's foot): is it on in a new Set? Switch it, open another Set, then restart Live: is its state kept, and so where does it live, with the Set or with the program? (SC-123, SC-429)
- R69. With Preview on, click a short one-shot and then a loop, first while stopped and then while the song plays: what sounds, when does it start (at once, or on the next bar), does it loop, does it follow the tempo? Press Raw and do the same. (SC-123, TM-MD-06)
- R70. With Preview off: Shift+Enter and the right arrow on a sample. A click in the preview's waveform: does it play from there, and can it be dragged to scrub? What stops a preview: another click, Esc, the switch? (SC-123)
- R71. The preview's level: which control sets it? With Cue Out on the same pair as Main, is the preview heard over the song? With Cue Out on another pair, where is it heard? (SC-123, SC-431, MX-08)
- R72. Does a preview play while a take records, and is it in the recording (record with the input set to Resampling)? (SC-123)
- R73. Which rows preview: a sample, a clip file, an instrument preset, a MIDI file, a Set, a device? What does the preview area show for each? Is the preview area there at all when a device is selected, or when nothing is? (SC-429)
- R74. Enter and a double-click on a sample, in the Arrangement: with an audio track selected, with a MIDI track selected, and with no track selected: on which track and at what time does it land? Enter on an audio effect with a device selected in the chain: before it or after it; and with only the track selected? (SC-105, WIN-06)
- R75. The arrows in the Browser's list: left and right on a folder, on a file and on a heading. Does opening one folder close another, and what does Ctrl change? The X in the Hot-Swap bar. (SC-126, SC-147, SC-285)
- R76. Show Similar Files on a sample (Ctrl+Shift+F): what is listed, and how is the list left? The Filter View (Ctrl+Alt+G): which filter groups show for Samples, and which for Plug-Ins? Is there a Modulators label? (TM-MD-07, SC-140, SC-141)
