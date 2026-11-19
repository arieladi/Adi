# The window lane — Claude work seat, cloud (19 November 2026): until your limit

As the director handed it over. Win's prompt; the seat's log is the record of what came of it.

```
Window lane of ADI (adi_daw in arieladi/Adi), Claude work seat, cloud session.

HOW YOU WORK: UNTIL YOUR LIMIT
This is not a one-answer task. Work through the queue below, item after item, until your usage limit ends the session.
- After each piece: build, run the suites, render its picture, commit, push the branch, write the log entry and
  "PR requested", then start the next piece in the same turn. Do not end your turn to report: the log is the report.
- A question only the director can answer does not stop you. Write it at the top of your log under "FOR THE DIRECTOR"
  (the question, your recommendation, what it blocks), park that item and take the next.
- An item waiting on the engine, on CI or on win: note it and move on; come back when it clears.
- Before each new item: git fetch, merge main, and read the top entry of adi_daw/collab/win.md and the Window table
  of adi_daw/docs/DEFERRED.md.
- Stop only if your environment is broken beyond retry (one line, and ask the director for nothing on his machine),
  or if every remaining item is parked on the director.
- Told "continue": fetch, read your log's last entry, carry on from there.
Use everything you have: build, run, render, write harnesses.

READ ONCE (all on main)
adi_daw/collab/prompts/2026-11-18-window-lane.md (its order of work still holds); adi_daw/collab/win.md top entry;
ADR-0243, ADR-0244 and ADR-0245; adi_daw/docs/RESEARCH-INTAKE.md section 6.2; the sheets in
adi_daw/collab/window/design/.

THE LOOK RULE (ADR-0243), AND HOW IT LETS YOU KEEP WORKING
A surface's LOOK is built only from a sheet the director has agreed. That never leaves you idle:
- For an agreed surface, build it.
- For a surface with no agreed sheet, do two things and keep going: draw its sheet as a proposal (HTML source and a
  1920x1080 PNG under adi_daw/collab/window/design/proposals/, named in words), list it under FOR THE DIRECTOR, and
  meanwhile build everything of that surface that is not look: its commands, keys, model, undo steps and tests, on
  the look it has today.

HOW THE DIRECTOR JUDGES
From pictures. Every built screen: one picture at 1920x1080, real size, beside its sheet, on a project with real
content (a drum pattern, held chords, a bass line, a melody, audio imported through the app so the waveforms are
real). Never a composite of small windows, never random test notes. Clips are drawn as Live 12 draws them, which is
ruled (ADR-0245 d7) and on main.

THE QUEUE
A. What is in hand:
   1. window/shared-controls (step 3): when ready, "PR requested".
   2. Step 4, the Arrangement from its sheet; then the rest of the morning prompt's list in its order (the Plug-Ins
      browser DF-W9, DF-W19, DF-W18, and on).
   3. DF-W32: the mixer strip takes the engine's held peak once a frame through one owner per strip.
   4. DF-W28 (a clip moved onto a clip: the covered part goes, as in Live; the same for a dropped file and a
      duplicate), DF-W29, DF-W30, DF-W31.
   5. The plan rows corrected from Live's measurements: TM-SV-01 (below one bar the launch grid counts from the song's
      start) and AU-03 (in a loop a mouse-written automation value goes on into the later passes).
B. Proposals to draw now, so the director can rule while you build (each a sheet as above):
   1. Take lanes and comping (CT-04, with CT-01). He uses them daily and sent a picture of Live: the track's main lane
      on top, its pieces named after the takes they came from; under it one lane per take, each with its name, a
      "Lane" header and a speaker button to audition it; the spans copied to the main lane highlighted in the track's
      colour on their lanes; Ctrl+Alt+U shows and hides the lanes. Draw ADI's in the agreed language.
   2. A return inside a group (DF-W33): shown in its group in the Arrangement and the Mixer, how it is created, its
      send knob only on the tracks inside that group, a send's tap in its context menu, the mixed Pre/Post state.
   3. The Mixer and the track rows in detail; the Browser; the Clip View; menus, dialogs and Settings (DF-W20, DF-W23).
C. Then adi_daw/docs/PLAN.md, the long run: every row whose lane column names "window" and whose status is not built,
   in milestone order M1 to M7, skipping a row whose engine side is not on main yet (ask for it in your log) and any
   row that waits on a choice of the director's. One row or one step of a row per branch, with its tests and its
   picture. Read the row's detail section and the ADRs it cites first.

WORKING VALUES (RESEARCH-INTAKE 6.2)
The send knob's taper (bottom minus infinity, top 0.0 dB; 0.248 is -35.0, 0.575 is -17.0, 0.857 is -5.7 dB); Track
Delay plus or minus 1,000 ms; Live's default clip fade, only when Create Fades is on (192 frames, knots 0, 2/27, 7/27,
1/2, 20/27, 25/27, 1). No cable diagram in the Device View or the Mix View.

REPORT
Your log adi_daw/collab/window.md on branch window/log; "PR requested" per branch with its picture's path. Win opens
and lands the pull requests and shows the director the pictures.
```
