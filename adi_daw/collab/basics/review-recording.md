# Review, with proof: recording

The stream reviews recording: audio and MIDI takes, the recorder's ring and its writer, take files,
punch and loop recording, the waveform drawn while recording, and what Undo does to a take. A finding
counts only when a headless test reproduces it.

- Code: `basics/review-recording-chain` (one chain, every finding a failing test and then its fix).
- Base: origin/main (the stack `land/basics-1128c` merged into main on 28 November; main is merged
  into the chain at `58126c791`).
- Checks: the build PC's slot 10 (MSVC Debug, warnings as errors). Linux and macOS have not run this chain.

## Findings

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-22 (new fault in a landed row) | `basics/review-recording-chain`: test `71ff4941b`, fix `968db0bb3` | **A take recorded from the song's start was late by the latency correction.** The correction moves a take earlier by the device's round trip. A take that began closer to the start than the correction (Record from 1.1.1) could not move that far; the whole file was put at the start, so everything in it sounded late by the part that did not fit. Now the clip starts at the song's start and hides those first frames (`engine::recordingPlacementRange`), as a punched take already did. The waveform drawn while recording shows the same range (`AudioRecordingPreview::latestPass`; the preview now carries the take's correction). A take that ended before its corrected start is refused with "the take ended before its latency-corrected start" and its recording kept. Files: `src/adi/engine/audio_recording.cpp`, `.hpp`, `src/adi/recording_commands.cpp`, `.hpp` (engine seat's and the recording commands). | `adi_ui_document_tests` (`recordWithDeviceLatency` now also runs from sample 0: the click lines up, the clip hides 192 frames, the live waveform matches the clip) and `adi_recording_commands_tests` (`latencyCompensatedPlacement`: heard audio, the placement function, the refused short take, the preview). Slot check: see "Checks" below. | see Checks |
| none (found by this review) | same chain: test `402caf8ce`, fix `379ed46fc` | **A block the device core refuses was cut out of a running take with nothing said.** `DeviceCore::process` refuses a callback with more frames than the driver granted: silence out, a counter up. Its stream clock stood still, so the recorder saw no gap and the take went on without that input. The clock now counts the refused frames; the recorder, which checks the clock, ends the take there, keeps what came before, and the Status Bar says the stream jumped. Files: `src/juce/device_core.cpp`, `.hpp`, `tests/test_device.cpp` (the pinned line "a refused callback does not advance the clock" is reversed, on purpose). | `adi_ui_document_tests` (`refusedDeviceBlockEndsTheTake`: a document behind a real `DeviceCore`, four blocks, one oversized block, four more; the take holds the first 1024 frames exactly and says it was interrupted) and `adi_device_tests`. | see Checks |
| SC-22 (loop half; SC-27's loop recording, on main since #808) | same chain: test `8af6a051d`, fix `51e58600f` | **A loop recording was never corrected for the device's round trip: every pass sounded late by it.** `ProjectDocument::startRecording` set the offset to zero with the loop on and `prepareLoopCommit` placed each pass at its captured samples. Now each pass keeps the timeline it was heard at and reads the shared file the round trip later (`engine::loopPassPlacement`): into the next pass, and the last pass ends that much earlier; a last pass shorter than the round trip lands no take (and is not counted in "N passes"). The live waveform's loop range (`AudioRecordingPreview::latestPass`) shows the same part. Keep Monitoring Latency now applies to a loop take as to a linear one (parity-recording row 15 said "open for loop recording"). A loop take recovered from a plan written before this keeps that plan's zero offset. Files: `src/adi/engine/audio_recording.cpp`, `.hpp`, `src/adi/recording_commands.cpp`, `src/adi/ui/project_document.cpp` (engine seat's, the recording commands, the document). | `adi_ui_document_tests` (`loopRecordWithDeviceLatency`: three passes with 192 samples of round trip; each take's source starts 192 later, the last is 448 of 640 frames, the first and last sample of each take are the pass that was heard; the main-lane copy and the live waveform match) and `adi_recording_commands_tests` (`loopPassPlacement` cases incl. a negative correction and a last pass shorter than it; the loop preview's ranges). Slot check: see Checks. | see Checks |
| none (found by this review; was "Suspected" 3) | same chain: test `69ca699b9`, fix `e13ce0aa8` | **A MIDI take ended when the window stalled for a few seconds.** Only the owner's timer drains `MidiInputCapture`, whose ring holds 4096 packets, and every callback slice took one span packet. At 64-frame blocks a stall of about 5.5 s (11 s at 128, 22 s at 256) filled it and the take stopped ("the MIDI recording buffer filled"), shorter than what was played. Now, while the owner is more than a quarter of the ring behind, complete slices are held back from it and the next slices grow the last held span in place; a stall costs one packet per note and per loop pass, not per block. Held slices are published as soon as the owner catches up and before any terminal state; a failure in an open slice still publishes none of it and restores a span it grew. With the owner keeping up nothing changes (the pinned packet layout of `driverMidiCapture` and its overflow case are unchanged). Files: `src/adi/engine/midi_input_capture.cpp`, `.hpp` (engine seat's). | `adi_midi_clips_tests` (`midiCaptureOutlastsAStalledOwner`: 6000 blocks of 64 frames with no drain and a note on and off; still Recording, Complete on stop, the spans cover all 384000 frames in order, both notes at their samples; `driverMidiCapture` unchanged). Slot check: see Checks. | see Checks |
| SC-447 (guard, not a finding) | same chain: `71ff4941b` | **One bounded attempt to reproduce the cut take, headless.** `takeIsWholeWhileTheOwnerWorks` runs the device callback on a thread of its own, as the program does, for 20 seconds of audio in uneven blocks, while the owner thread ticks the timer, reads the live waveform and edits other tracks (volume, mute, pan, rename, a new track, a new clip, Undo). It then compares the landed take with the input sample for sample and takes it through Undo and Redo. | Slot check: see Checks. | see Checks |

## Checks

Pending: the slot was busy with other sessions' cold builds when this was written. The next lines are
filled in as the checks finish.

- Round 2 (28 November, from 17:30 JST): slot 10 still held round 1's check of `71ff4941b` (the test
  commit), "waiting for a build place" since 15:53, and refused a new check ("BUSY"). The head check
  (`slot.sh run 10 origin/basics/review-recording-chain all`) follows as soon as the slot frees.
- Base alone (`land/basics-1128c` at `16064c0a6`), core: 290 of 291 passed. `adi_link_playback_tests`
  failed there ("both tempo directions detect old history on repeated loops…"), before any change of
  this stream.

## Suspected, not proved

These were read in the code. No test reproduces them yet, so they are not findings.

1. (Was: the loop take's latency. Reproduced and fixed in round 2: see Findings.)
2. **SC-447's likely cause is outside what ADI can see today.** The recorder writes on its own thread
   and nothing in the window can stall it; the stream clock (`DeviceCore::streamTime_`) is a count of
   the frames ADI was handed, not of time. When the system skips an audio cycle (an overload), the
   driver drops that input and calls again; ADI sees an unbroken stream and the take goes on with a
   piece missing. That matches what the director saw (parts cut, the take went on). It cannot be
   reproduced headless. See "For the director".
3. (Was: a long owner stall ends a MIDI take. Reproduced and fixed in round 2: see Findings.)
4. **A recovery marker that cannot be written fails the whole take.** In the writer's loop,
   `recording_recovery::writeStart` throws on a refused folder check, and the throw is counted as a
   writer failure ("the take could not be written to disk") although the audio itself was writable.

## For the director

1. **Takes from the song's start (changed behaviour, please confirm).** With the fix a take recorded
   from 1.1.1 starts at 1.1.1 with its first frames hidden (the device's round trip, a few
   milliseconds), so it plays in time. Before, the hidden part was shown and the whole take was late
   by it. This follows from timing, and it is what a punched take already did; I could not open
   Live 12 from here to read its clip's start marker after such a take. Recommendation: keep the fix.
2. **Telling the user when the device dropped input (SC-447).** Recommendation: let ADI compare the
   driver's own time between callbacks (JUCE hands it over on macOS) and, when a cycle was skipped
   during a take, say so when the take lands: "The audio device dropped input N times during this
   take." Live keeps recording through a dropout and shows an overload light; this would be ADI's
   plain-words equivalent. It adds one Status Bar sentence, so it waits for your yes.

## For win, when merging the chains

- **The refused device block was fixed twice, differently.** This chain's `379ed46fc` makes
  `DeviceCore::process` count a refused block on the stream clock, so the recorder's own continuity
  check ends the take there. The director's-test chain's `9c9947fbf` (`basics/test-chain-2`, SC-447)
  makes `process` return `CallbackResult::Oversized` and calls a new
  `AudioInputRecorder::reportOversizedCallback`, which fails the take with `BlockSize`. Both touch
  `src/juce/device_core.cpp`, `.hpp` and `tests/test_device.cpp`, and they will collide. Both keep the
  prefix and end the take. Recommendation: keep `9c9947fbf` (its message names the cause, "a block
  larger than the size prepared") and drop `379ed46fc`; the document-level test of `402caf8ce`
  (`refusedDeviceBlockEndsTheTake`) is worth keeping on top of it, with its expected sentence changed to
  the BlockSize one (not run that way yet).

## For the next session

Round 1, 28 November 2026.

- Done and pushed: the three rows above. If "Checks" still says pending, run
  `slot.sh run 10 origin/basics/review-recording-chain all` and fill it in.
- Next, in this order: (1) "Suspected" 1, the loop take's latency: write the failing test first;
  (2) "Suspected" 3, the MIDI capture's ring under a stalled owner: a test that runs 5000 callbacks
  with no tick reproduces it, the fix is to publish one span per pass instead of one per slice;
  (3) read `recording_recovery_wave.cpp` (`inspectWave`, `copyRepaired`) and `stageTake` /
  `publishStagedTake` for a file left half written: not read this round;
  (4) punch and count-in with a linear take (parity-recording row 29 says no check that a linear take
  starts after the Count-In).
- Not read this round: `take_lane_ops.cpp`, `track_recording.cpp`, `automation_recording.cpp`,
  `recording_recovery_wave.cpp`, the MIDI loop commit.

Round 2, 28 November 2026 (a restart of the stream; worked from a local branch `rr-chain` pushed to
`basics/review-recording-chain`, because round 1's worktree still holds that branch name).

- Done and pushed: the loop take's latency (Findings, SC-22 loop half), test `8af6a051d`, fix
  `51e58600f`. Its slot check waits behind round 1's.
- Done and pushed: the MIDI capture under a stalled owner (Findings), test `69ca699b9`, fix `e13ce0aa8`.
