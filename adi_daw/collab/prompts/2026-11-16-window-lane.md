# Window lane: continue (16 November 2026)

From win. Your standing prompt (`2026-10-27-window-lane.md`) and the worklist
(`collab/window/worklist-2026-11-05.md`) stand; this note only says what comes next and what has
changed around you since E2.

You are free to use anything that helps: install tools, download what you need, build, run, probe,
measure, write harnesses. How you do the work is yours.

## Where things stand

- **Main is green at `ac5050bd`.** Everything of yours through E2 (#549) is on it. Twenty-four of
  the worklist's thirty-seven items are done.
- **The Mix View seat is working now, in the window's own files.** Its open PRs (#579, #580, more
  coming) change `src/juce/ui_devices.*`, `src/juce/ui_stack.*`, the `mix_*` files and
  `tests/test_ui_mix_functions.cpp`, and add checks to `tests/test_ui_shell.cpp`. The items below
  were chosen so you do not need those files. If one does, say so in your log and win orders the
  merges; in `test_ui_shell.cpp` expect to rebase.
- **The control slice (#512) merged on 16 November** (`729892e2`). Its fence on `ops.hpp`,
  `ops.cpp`, `ops_catalog.cpp`, `changeset.*` and `src/main.cpp` is lifted, and the register of
  everything postponed, `docs/DEFERRED.md`, is on main.
- **The engine seat is away.** Raise what you need as N rows, as before, and build what the window
  can do without them. Open with the engine today: N44, N45, N47 to N51, one undo step for a Split
  of several clips (E1), the paste's kind check (E2).
- **One row of yours is unblocked:** `audio.delayCompensation` is in the registry on main (N24).

## What needs doing, in order

1. **Finish group E**, which is the editing a user does all day:
   - E3, the …Time commands and Insert Silence.
   - E4, fade handles, Create Fade and Crossfade (C4's waveforms are in).
   - E5, the import prompt.
   - E6, the bounce prompts.

2. **Recording from the window.** #406 records audio inputs and nothing in the worklist itemises
   its surface. The strip has an Arm button and the catalogue has `track.setArm` and
   `transport.setRecord`. Read what the Arm button and the control bar's Record button do today
   against #406 and Live 12's recording chapter, write the parity rows, and build what is missing so
   that the director can arm an audio track, press Record, and see and hear the clip he recorded.
   Say plainly in the PR what still does not record (MIDI, N31's open part).

3. **C7, automation lanes and breakpoints, Automation Mode, Lock Envelopes.** The largest item
   left. The sends' lanes (#433) belong with it.

4. **B6, cueing** (Solo/Cue, Cue Out, Cue Volume, the Cue switches) **and B7, Split Stereo Pan.**

5. **The Options menu's Delay Compensation** (N24), now that its row exists.

6. **F3, the Edit menu in Live 12's measured order.**

7. **The other surfaces that landed with the engine's stack and have no item yet:** Freeze Track
   (#416), Capture (#430 to #432), the export dialog's NI Stems and normalization (#407, #418). For
   each: the parity rows first, then the build, one PR each.

## The Clip View, and one picture the director has to see

- **The Clip View (G1 to G3): approved on 16 November, and win builds it, not you.** The director
  said yes to the picture of 28 October and gave the build to win so that your list above is not
  held up. Win's files are new: `src/juce/ui_clipview.*`, `src/adi/ui/note_geometry.hpp`,
  `src/adi/ui/note_edits.hpp`, `src/adi/ui/clip_edits.hpp` and their tests, on branches
  `window/clip-view-*`. Win will also touch three of yours lightly: the command ids and the detail
  row in `src/juce/ui_shell.*`, a clip's double-click in `src/juce/ui_arrangement.cpp`, and
  `ProjectDocument`. New command ids go before `LastId` on both sides; expect a small rebase when
  either of us merges. Leave a clip's double-click and Ctrl/Cmd+Alt+3 to win.
- **C5, remark pips and the Info View's split.** You offered to draw it. Please do: the pips on a
  track, a clip, a device and a parameter, and the split Info View, as one picture with a few lines
  on each choice. It goes to him as a picture; nothing of C5 is built before he answers.

## Still held, and not yours to unblock

- B8, Main's loudness meter, until the Live 12 meter readings.
- N10's home for `midi.port.remote` and N38's `tracks.role` column are win's and still owed.
  Nothing in the list above waits on them.
- The Mix View's ★ Reference (N38) and the withdrawn Q and L slots (N16) stay as the worklist says.

## How it lands

- One `window/<topic>` PR per item, base main, with its headless checks, its parity rows and its
  picture beside Live 12's figure, as before. First line of the PR: what the director can now do.
- Build the Pd-off tree before pushing anything that branches on `ADI_WITH_PD`, as you found in D4.
- Win reviews, proves each on MSVC `/WX`, merges and logs. The app is never driven on the
  director's desktop; leave a short brief of what to try for each PR he would want to see running.
- Your three notes for win from 10 November (the ZIP's progress, the sidebar's labels, the place of
  Rec-Q and Play-Q) are with win.
