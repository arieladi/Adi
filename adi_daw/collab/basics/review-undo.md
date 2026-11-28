# Review with proof: Undo and Redo

A review of the journal, batches and every operation that landed since 26 November 2026. A finding
counts only when a headless test reproduces it: the failing test first, then the fix, each a commit on
the chain `basics/review-undo-chain`. Proof is a check on the build PC (slot 11, MSVC Debug, warnings
as errors).

Base of the chain: `origin/land/basics-1128c` until it is in main, then `origin/main`.

## Findings

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| F1 (no SC row yet: "Reserve" below) | `basics/review-undo-chain`, test `d04dc807f`, fix `c590fab36` + the commit after the SC-11 one | **Undo refused for good after the Loop switch.** Insert Silence, Delete Time and the other time edits record the saved loop before and after, and `arranger.splice` refused whenever the stored loop was not the recorded one. The Loop switch and the loop brace (`transport.setLoop`) write that value with no Undo step. So: insert time before the loop, click the Loop switch, press Undo: "loop changed after edit preparation", and every older step is out of reach too. Redo the same way. Fix in `src/adi/clip_edit_ops.cpp` (`arrangerApply`): a NEW edit is still refused when the loop changed since it was prepared; a replay from history (Undo, Redo, a move in the history list) no longer refuses. A loop whose brace still stands where the edit left it takes the edit's brace and keeps its own switch; a brace the user placed since is left alone. The project rows are restored exactly in both cases. | `loopSetAfterTimeEdit` in `tests/test_clip_edit.cpp` (beside `pureLoopAndCut`): the Loop switch, then Undo and Redo; the brace dragged, then Undo and Redo; a stale new edit still refused. Every stored row compared by `digestProject`. See "Slot checks" below for the runs. | see "Slot checks" |
| SC-11 (proof only, no fault) | same chain, `5b142700a` | Nothing in the program. The tempo ops rewrite the length of Warp-off clips beside the tempo row (`_unwarpedLengths`), and that landed on 26 November with no Undo test at all. | `unwarpedRecordingTempoChanges` in `tests/test_recording_commands.cpp` now compares every stored row before the tempo change, after one Undo and after one Redo, at a faster and a slower tempo. | see "Slot checks" |

## Slot checks

(filled in as they finish)

- The warm-up of slot 11 on another branch, before my first check: 1 of 291 failed,
  `adi_link_playback_tests` (eight FAIL lines about the Link tempo clock, 60 s). Not from this chain:
  the chain had not been built yet. A timing test under a loaded build PC is my guess; I did not look.

## Read and found sound (no test added)

What I read op by op and could not fault. "Sound" here means: the inverse is built from the whole of
what the forward changes, Redo uses recorded values and not the surroundings, a stale change is refused.

- The journal itself (`ops.cpp` `commitImpl`): one SQL transaction for every op, its log row and the
  head; a fork keeps the Redo line as a branch; a play/stop/seek batch does not divert Redo.
  `History::undo`/`redo`/`moveBetween` (`history.cpp`): the inverses and the head move are one transaction.
- `project.insertTempoEvent` / `removeTempoEvent` with unwarped lengths (SC-11): the lengths before and
  after are recorded in the inverse, Undo writes "before", Redo reads "after" from the recorded inverse.
- `audioClip.setTranspose` (SC-201): a plain one-column op, the same shape as `clip.setGain`.
- `routing.connect` with `midiPort`/`midiChannel` (SC-404): Undo deletes the row, Redo writes it again
  from the payload; `routing.disconnect` captures the whole row with its `channel_map`.
- `audioClip.setSource` (SC-218), `arranger.splice`, the take-lane ops (`lane.*`, `clip.setLane`),
  `clip.delete`, the marker ops with `marker_details`: all go through captured rows (`edit_rows.cpp`),
  which refuse a row that changed since and refuse a delete whose cascade is not in the capture.
- `automation.movePoints` with the compact inverse of SC-422: I walked the restore by hand (moved
  records back to their old places, the others merged in their kept order, curve ranges re-indexed and
  the dropped ones put back). It holds, and it has its own tests.
- `track.create` with copied contents (Duplicate/Paste Track) and `track.delete`: digest-checked already
  in `tests/test_track_delete.cpp`.
- The window (`ui_shell.cpp`, `ui_arrangement.cpp`, `ui_clipview.cpp`): every command I followed commits
  once. The audio drop that replaces what it covers is one batch. Cut Track is one op.
- All the ops with no Undo step write `session_state` only. The loop is the only such value an
  undoable op also guards (F1). The song start, punch flags, count-in, metronome and cue controls are not
  read by any undoable op.

## Suspected, not proved

1. **Removing, copying or hot-swapping a hosted plug-in can add a hidden Undo step.**
   `ProjectDocument::deviceAction("device.remove")`, `copyDevice`, `editDeviceRange` and
   `hotSwapTargetAfterCapture` first call `capturePluginState(id)`. If the plug-in's own state changed
   since the last capture (the user turned something in the plug-in's own window), that writes a
   `device.loadState` step BEFORE the remove. One "Remove Device" is then two Undo presses, and Copy
   leaves an Undo step though Copy changes nothing. The code already avoids this for the built-in mix
   devices and says why (`project_document.cpp`, the comment at "Native mix controls already write
   their edits"). I could not reproduce it headless: it needs a hosted plug-in with state that moved.
   Where to look: `engine/param_ops.cpp` `snapshotAll`. A way to fix it: put the captured state and the
   remove in one journal batch.
2. **Recorded automation and the take are probably separate Undo steps.** `landTake` commits the take;
   `closeAutomationWrites` commits the automation written during the pass through `commitCaptured`, its
   own transaction. Not reproduced: it needs a running session. Record Quantize as its own step is not a
   fault: as I remember Live's manual, record quantization is its own step in Live's Undo history too.
3. **Orphan clip envelope lanes.** `clip.delete` captures `automation_data` of the clip but not an
   `automation_lanes` row with `owner_kind='clip'`. Undo is exact (the lane never left), so this is a
   leak, not an Undo fault. Not tested.

## For the director

1. **F1, what the loop does on Undo.** The loop brace and the Loop switch are not Undo steps in ADI. When
   you undo an Insert Silence that had pushed the brace, and you clicked the Loop switch in between, I
   chose: the brace goes back with the music, the switch stays as you set it. When you dragged the brace
   somewhere else in between, it stays where you put it. The alternative is to make the brace and the
   switch Undo steps of their own. Recommendation: keep what I built; it never throws away something you
   set after the edit. Reply "F1 keep" or "F1 loop is an Undo step".
2. **Arm and Solo are Undo steps in ADI.** `track.setArm` and `track.setSolo` are ordinary edits. So
   after an Undo, arming a track to record ends the Redo line (it is kept as a branch in the history
   list, but Ctrl+Y no longer reaches it). As far as I know Live does not put Arm or Solo in its Undo
   history, but I could not measure Live from here. Recommendation: a reading in Live 12 (press Solo,
   press Arm, press Mute, then Ctrl+Z each time), then make ADI match. Not changed by me.

## Reserve

- Reserve: a `docs/SANITY-CHECK.md` row id for F1 ("Undo refused after the Loop switch following a time
  edit"). I did not take one.

## For the next session

Done in round 1: F1 (test + fix), the SC-11 proof. Both are on the chain.

Not done, in the order I would take them:

1. A digest round trip (before, Undo, Redo) for the commands that landed since 26 November and whose
   tests check single fields only, all in `tests/test_clip_edit.cpp` and its neighbours:
   `prepareTakeToMain` (no core test at all), `prepareCompleteFragmentaryBar` (SC-186, none),
   `prepareStretchWarpedClip` (none), `prepareAudioDropReplacement` (only through the window),
   `prepareReverse` (`tests/test_clip_playback.cpp`), `prepareMidiConsolidation`, the import batch in
   `tests/test_material_commands.cpp` (six Undo calls, no digest).
2. `cc.write` / `cc.delete` with the pitch-wheel address 129, `capture.createMidi`, and
   `recording::prepareCommit` with audio and MIDI together: the same round trip.
3. `freeze::flatten` and `audioTrackConversion` (`freeze_commands.cpp`), the device hot-swap and preset
   recall (`device_presets.cpp`, `device_copy.cpp` `prepareDevicePaste` for a block).
4. Suspected 1 and 2 above need a test double for a hosted plug-in or a session; `tests/test_param_ops.cpp`
   has one for the first.

How I worked, to save you time: the engine's own ops are careful and row-captured, so reading them one
by one found nothing. F1 came from asking a different question: which values can change with NO Undo
step, and which undoable op checks them. The same question about files on disk and about plug-in state
is where I would look next.
