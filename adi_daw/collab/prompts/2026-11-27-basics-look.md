# basics-look — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/look.md` on
`basics/look-log`.

**Your part:** what the screen shows, checked in the real window. You are the session that walked the
real window on Windows 11 (the walker of `collab/walk/`), so you can see what you fix.

## First: pass 9

One whole pass of the nine steps on a clean build of origin/main, as win asked on 26 November, with
win's two fixes of #790 checked in the window. Report it in your walk report. The hold of ADR-0248 d2
ends when it is clean.

## Queue, in this order

1. **SC-70: the ruler and grid as the approved sheet and as Live.** The director's own remark: "the
   grid looked like seconds and not bars", "the grid lines look like R1 R2", "the 1/4 sits in a weird
   place". The sheet is `collab/window/design/arrangement.png` (source in `design/src/`). Four faults in
   `src/juce/ui_arrangement.cpp`:
   - each ruler number has a hook as tall and as bright as the digit (about 1812-1819), so "1" reads
     "r1". The sheet draws plain numbers with a thin tick;
   - every grid line has one shade (about 42-45). Bar lines are darker, as the Clip View's already are;
   - the grid's spacing is drawn inside the time ruler's row (about 1281, 1304-1309). Live shows it
     above the time ruler at the lower right;
   - a new project opens at 48 pixels per quarter note (`src/adi/ui/arrangement_geometry.hpp`, about
     62), about nine times closer than a new Live Set, so the ruler counts beats and single seconds.
     Open on whole bars as Live does. Many window tests assume the old value: change the default for a
     new project, not the tests' own geometry.
   Send before and after pictures in your report's text as measured positions, not as image files.
2. **SC-112 and SC-111: the Info box** (ADR-0257 d12; his remark of 26 November: "a 90s spectrum that
   shows nothing").
   - It is as wide as the Browser's column (`kInfoWidth` in `src/juce/ui_shell.cpp`, 200 today).
   - Its analyser is the **spectrogram by default**, as ADR-0244 d7 ruled. Today it follows the
     Analysers page's mode (`ui_shell.cpp`, about 1529), so it shows a plain spectrum.
   - The box has its own switch, kept per window, with four choices: the spectrogram, the spectrum in
     SPAN's look, the spectrum in the look of Live's Spectrum, and the Classic Scope small (today the
     box turns the Classic Scope into the plain one). Every mode is built in `SharedSpectrumView`
     (`src/juce/ui_analysis.hpp`): this is wiring, and a right-click menu on the box is enough.
   - With no audio it shows the analyser's own empty display, not five lines and "Waiting for audio".
   - A click still opens the Analysers.
   - Each look "as its source" stays the Mix View seat's (DF-W22). Say in your report what each of the
     four looks like against its source, so that seat starts from your list.
3. **SC-110: no AI tab at the Browser's foot** (ADR-0257 d12). Remove `aiTab` and the chat area it
   opens (`ui_shell.cpp`, about 1309 and 1745; `aiChatOpen` in `src/adi/ui/window_state.hpp` stays
   readable in an old project and is ignored). Done (#808). ADR-0261 replaced the idea of
   a row that sends: an AI label in the Categories list, built later with the agent's side (AGT-05).
4. **SC-72: the view scrolls to the insert marker** on Home, a locator jump and a typed position.
5. **SC-73: the Control Bar's fields** (tempo, position, loop start and length, signature) answer a
   drag, the arrow keys and the wheel, as Live's.
6. **SC-76: Follow in a new project.** *Reading owed:* whether a new Live Set has Follow on. Build it as
   the default being one constant.
7. **SC-06: the CPU meter** reads a constant "0 %". Feed it from the engine's callback load if a number
   is already published; otherwise hide the number until it is true.
8. **SC-37: a clip's own colour** in the Arrangement. The Clip View's chooser sets it and the lanes
   ignore it (`ui_arrangement.cpp`, about 2379 and 2397).
9. **SC-90: silent things look silent.** A deactivated track's clips are dimmed; a bypassed device is
   greyed as a whole, not only its switch.
10. **SC-92:** the Audio To chooser reads "Main".

## After these (wave 2)

SC-71 (the grid and the ruler's labels below a quarter note; *reading owed*), SC-12 (the thirteen
Control Bar buttons that answer "not built yet": each wired, or greyed with its reason), SC-38 (where a
dragged sample will land), SC-74 (a tempo change placed in the song), SC-75 (Stop pressed while
stopped), SC-89, SC-91, SC-63, SC-04.

## Added by round 2 (26 November)

- **Before pass work, the readings of Live:** `2026-11-27-live-readings.md`, R1 to R66, if the director
  gave you that prompt. Other sessions wait on its answers.
- **SC-06 can be fed now:** the device manager already gives the load and the missed-buffer count
  (`src/adi/engine/play.cpp`, about 793), so the number need not be hidden.
- **SC-12 is fifteen buttons, not thirteen,** listed one by one in round 2's note on the row. Draw Mode
  is only not wired to the command that already works in the Note Editor.

Your second queue, when this one is empty, is the browser queue only if the director gives it to you: it has one owner (`2026-11-27-basics-browser.md`).
