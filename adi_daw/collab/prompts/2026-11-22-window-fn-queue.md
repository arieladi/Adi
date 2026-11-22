# window-fn — standing queue, 22 November 2026

This is your whole queue from win: work it until your usage limit. It replaces every earlier, shorter list.

Your seat and the window lane now split the window's paths by item, not by path (collab/README.md, 22 November).
You take the window's function rows: commands, keys, gestures, undo steps, and the wiring from the window to the
engine (recording, MIDI input, export, plug-in loading, faders heard while dragging). The window lane keeps the look,
the shell and the test kit (WIN-01 and the shell rows are its). When you both touch a file, each edits it for its own
items. The second branch to land merges main, and each seat names in its log the other's items it touched.

## How to work

- Fetch main first. Read the top entry of collab/win.md and the rows of docs/DEFERRED.md that name you.
- Work the items top to bottom. Use one branch per item, `window-fn/<topic>`, cut from origin/main unless the item
  says to stack.
- Write the test first where the item names one.
- Push the branch. Write the entry in collab/window-fn.md on window-fn/log, end it with "PR requested", and start
  the next item in the same turn.
- If an item is blocked on win, another seat or a decision, park it with one line in your log and take the next.
- A question for the director goes under "FOR THE DIRECTOR" at the top of your log, with your recommendation. Then
  continue.
- Stay in your paths (collab/README.md). What you need from other seats' paths goes in your log's needs table.
- When this list is empty, take the next unbuilt row of your lane in docs/PLAN.md, in milestone order. Work meant for
  after 3 December goes on branches marked "lands after 3 December".
- Never stop because "nothing names this lane". Stop only at your usage limit or a broken environment.

## Queue

Items 1-8 serve the one usable session before 3 December (ADR-0248 d2). Items 9-18 come after: mark each of their
branches "lands after 3 December", plus any dependency the item names.

1. **SHELL-BROWSER-FIXTURE: fix the browserDevices crash on main**
   Hybrid Reverb is now "ready" (builtin_catalog.cpp:24) and loads on track 4; the test still expects it not to load
   (test_ui_shell.cpp about 2570-2585). The plug-in subcase then clears the selection and reads an empty strip.
   Update the Hybrid Reverb check, undo its insertion, restore track 4's selection, guard the strip access.
   File: tests/test_ui_shell.cpp (browserDevices). Test: adi_ui_shell_tests, 0 failures and no crash, fresh profile,
   Pd on and Pd off. This is yours, not the window lane's.

2. **DF-E16 (window half): one RowIds per prepared edit**
   Replace `COALESCE(MAX(id),0)+1` and live-max+1 with one `adi::RowIds` per prepared edit: landTake clips/media
   (project_document.cpp 1084-85), the import job (682-83), prepareImport (652), pasteDevice (624),
   addAutomationPoint lane/data (ui_shell.cpp 2832-33), ui_main.cpp:770. Name the window lane in your log before you
   touch track_edits.hpp and audio_drop.hpp.
   Test: delete a clip, track or lane that has a remark, then record, drop or add a point; the new id skips the held
   id, and Undo/Redo restore the rows. WTS-01 2a, 6a, 6b unchanged. RowIds is on main (src/adi/row_ids.hpp).

3. **PD06-SAVEAS-GUARD: Save As during Record keeps the take**
   Save As, Open, New or a project replace during Record, or while a take's placement is pending, waits until the
   take has landed in the current file, then switches. Today landTake prepares against the new file and drops it.
   Files: src/adi/ui/project_document.*, src/juce/ui_main.cpp, ui_shell.cpp; tests/test_ui_document.cpp.
   Test: Save As during Record and during pending placement keeps the take as one undo step, then the file
   switches. WTS-01 2a PASS. Independent of PD-06's recovery branch.

4. **OWNER-REVIEW-1122: review other seats' hunks in your paths**
   Merge each on main and review: engine-win/export-host-owner (N62, 6b78d94c: Export dispatch in ui_main.cpp,
   owner teardown, Stop joining the worker while it waits on an owner call); engine-win/recording-failed-prefix (N66,
   0747821d, project_document.cpp); engine-win/play-first-block (DF-E64, c7f3d9cc, project_document.cpp,
   window_state.hpp); the clones loan sites #708's Held list names as unreviewed (the ui_main.cpp editor factory, the
   ui_devices.cpp wrench checks); the Mix View seat's window loans (ProjectDocument::pasteDevice and the synchronise
   retiree loop on mixview/device-identity-dfm60a-20261122 and device-key-shape-dfm60b-20261122, the window_state.hpp
   command drain on mixview/focus-match-locate-20261122).
   Test: each builds merged on main with the window's suites green; WTS-01 7d PASS with N62, 2b PASS with DF-E64.
   Findings in your log for win.

5. **DFE44-DISCARD: recorder cleanup refusals in the Status Bar**
   Show discard() refusals and the retained folder at the three discard sites (project_document.cpp 1031, 1077,
   1097). Stack: cut from engine-win/recorder-failure-recovery (DF-E44, 0ed2c355) and merge
   engine-win/recording-failed-prefix (N66) into it, since both touch project_document.cpp. Engine-win's N66 item
   rebuilds that branch on DF-E44 and says in its log whether you stack on its joined head or take its
   project_document.cpp hunk: read that first and follow it; if it has not answered, stack on 0747821d and rebase
   onto the joined head when it is pushed. Mark "lands after DF-E44 and N66". Test: an injected removal refusal names
   the folder in the Status Bar; a retry clears it.

6. **N61 (window half): heal a MIDI port's record by name**
   Write `midi.port.name` on each record. A port whose identifier has no record adopts a record of the same
   direction and name, re-keyed to the new identifier.
   Files: src/juce/app_midi_ports.*, src/adi/ui/midi_ports.hpp, src/juce/ui_settings.*. `midi.port.name` is on main
   (registry.cpp:545). Test: identifier changes, name kept, Track/MPE switches survive; an unrelated name gets no
   record.

7. **AU-04 (lane and fader): per-lane Re-Enable Automation**
   On the Automation Mode lane's right-click and on the track volume fader; the fader ignores the right button so the
   menu opens. Files: ui_arrangement.cpp, ui_shell.cpp (addReenableAutomation), ui_mixer.cpp.
   Test: an overridden Volume lane offers the item; the lane follows its envelope; no undo step; other lanes stay
   overridden.

8. **DF-W28 (rest): a drop or a duplicate onto a clip removes the covered part**
   As for moves (the move half is on main): the covered part goes, one undo step, a Status Bar reason where the
   planner does not place. Files: ui_shell.cpp, src/adi/ui/audio_drop.hpp, the clipboard duplicate path.
   Test: drop and Duplicate onto a clip, the covered part removed, one step, Undo restores.

9. **DF-W38: Re-Enable finds a device lane stored as "001"**
   Compare parsed ids for owner_kind device, not raw strings (automation_override.hpp, ui_devices.cpp:137). Stack on
   gemini/df-e53-canonical-param-ref (03be0b99) and use its parser, src/adi/device_param_ref.hpp. Mark "lands after
   DF-E53". Test (test_ui_seams): a "001" lane is found as parameter 1 and the item is available.

10. **MX-23 (drive): the group summing drive heard while dragging**
    Through gesture::Coordinator and GroupSummingDrive (gesture_producers.hpp:15). Files: ui_mixer.*,
    project_document.*. Test (document): drive heard mid-drag with nothing written, one step at release, Esc
    restores. Track Delay's drag waits on the engine (needs below).

11. **HI-01 (Tap/latch): Tap Tempo and the momentary key latch through the gesture session**
    Through QuietSession: one undo step, no undo-and-recommit branches. Files: ui_shell.cpp (tapTempo, about 3187),
    ui_controlbar.cpp. Test: four taps make one undo step and no forked branch in readTimeline.

12. **CT-02 (window half): record with the loop on**
    AudioRecordingLoop capture and `recording::prepareLoopCommit` (a take per pass, a main-lane copy of the last
    pass), replacing the refusal at project_document.cpp 1008-1011. Test (document): two passes land as takes plus a
    main-lane copy in one undo step.

13. **MX-02: Split Stereo Pan**
    Enable the item (ui_mixer.cpp:1130), write mixer.setPanMode, two pan sliders with the shared painters, each drag
    a gesture, Re-Enable per pan. The look stays the window lane's sheet. Test (shell): the mode ticks, one step per
    drag, double-click resets, Re-Enable on each pan.

14. **N64 (window): Arm and monitoring read the snapshot**
    Read TrackNode::recordArmed and monitorMode, and drop the once-a-frame session rows. Files: arm_edits.hpp,
    window_state.hpp, ui_mixer.*, ui_main.cpp. Stack on engine/n64-recording-snapshot; mark "lands after N64".
    Test: shell and document suites; WTS-01 2a.

15. **MI-01 (window): every note batch as one note.applyDiff**
    Through `notes::prepareBatch` in AdiRootComponent::submitEdits. Stack on engine/n63-ordered-note-batch; mark
    "lands after N63". Test: the 13 note checks that failed the earlier translator pass; one op per edit.

16. **EX-06 (page): Shaped 1-3 on the Export page**
    Offer the shaped-dither slots and lift the page's refusal (export_choices.hpp, ui_export.*). Stack on
    engine-win/export-quantizer; mark "lands after" it. Test (shell): the shaped choice renders; the refusal is gone.

17. **N68 (window): open after a plug-in scan, by saved path**
    Remove needs row 44, re-check the "Opening X once the plug-in scan has finished" rule, and test an open after a
    job that loads a plug-in by its saved path (plugin_scan.hpp, ui_main.cpp). Stack on
    engine-win/project-plugin-saved-path (43787b7b). Test: shell test over the stand-in port.

18. **AU-01 (curves): draw curved automation segments**
    Use the engine's curve formulas (SPEC 6.3.2) instead of straight joins (automation_mode.hpp,
    ui_arrangement.cpp). Test (seams): drawn y equals the engine's value at sample points of a curved segment.

## Waiting on win

Do not redo these; win lands them.
- window-fn/midi-live fe24a5b0 (N60 drain, WTS-01 3b/3d)
- window-fn/lane-chooser 305e9b3a (DF-W37, WTS-01 6b)
- window-fn/monitor-auto 5ac33b36 (N67 and N65 window halves). Win lands it in one window with
  engine/n67-auto-monitor-default; once track.create defaults to Auto, drop your extra setMonitorMode.
- window-fn/delay-compensation ea67a671 (MX-04)
- window-fn/log 8f31f9d2
- Your parked parity checklists (recording and export, from the Live manual): win decides whether you get the
  manual or the window lane drafts them. Do not restart it until win answers.
- Win reserves N69 for your DF-W37 need, and adds DEFERRED rows naming you for DF-W28's rest, DF-E16's window half,
  the browser-fixture crash and the Save As guard.

## Needs from other seats

- engine: return a device parameter to its stored value when its lane goes (DF-W37; N69 once win reserves it).
- engine: a Track Delay live-preview producer, so MX-23's Track Delay drag can be heard.
- engine: AU-05 Lock Envelopes, the planner's switch.
- engine-win (its CT-03 item, on engine/midi-notes-new-clip): a MIDI recorder into Arrangement clips, so MX-09 drops
  its "only MIDI armed" refusal.
- window lane: window-fn (item 2) takes DF-E16's sites in track_edits.hpp and audio_drop.hpp; DF-W33 and WTS-01 5b
  stay on window/return-in-group. Whichever of the two lands second merges main.

## For the director

- Live 12's default Track switch for a newly connected MIDI input port (ADI defaults to off; affects WTS-01 3b/3d).
- Where Delay Compensation sits among Live 12's Options menu entries (MX-04 put it before Solo in Place,
  provisional).
