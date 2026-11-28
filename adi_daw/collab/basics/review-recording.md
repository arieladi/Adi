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
| SC-447 (guard, not a finding) | same chain: `71ff4941b` | **One bounded attempt to reproduce the cut take, headless.** `takeIsWholeWhileTheOwnerWorks` runs the device callback on a thread of its own, as the program does, for 20 seconds of audio in uneven blocks, while the owner thread ticks the timer, reads the live waveform and edits other tracks (volume, mute, pan, rename, a new track, a new clip, Undo). It then compares the landed take with the input sample for sample and takes it through Undo and Redo. | Slot check: see Checks. | see Checks |

## Checks

Pending: the slot was busy with other sessions' cold builds when this was written. The next lines are
filled in as the checks finish.

- Base alone (`land/basics-1128c` at `16064c0a6`), core: 290 of 291 passed. `adi_link_playback_tests`
  failed there ("both tempo directions detect old history on repeated loops…"), before any change of
  this stream.

## Suspected, not proved

These were read in the code. No test reproduces them yet, so they are not findings.

1. **A loop-recorded take is never latency-corrected.** `ProjectDocument::startRecording` sets
   `spec.placementOffsetSamples = spec.loop ? 0 : deviceLatencySamples`, and `prepareLoopCommit` places
   each pass at its captured samples. Every pass of a loop recording would sound late by the round trip.
   `collab/window/parity-recording.md` row 15 says "open for loop recording" for Keep Monitoring
   Latency only. Next step: a document test like `recordWithDeviceLatency` with the loop on, then shift
   the pass boundaries in the source by the correction inside `prepareLoopCommit` (the first pass hides
   its first frames, the last pass ends that much earlier) and the same in `latestPass`.
2. **SC-447's likely cause is outside what ADI can see today.** The recorder writes on its own thread
   and nothing in the window can stall it; the stream clock (`DeviceCore::streamTime_`) is a count of
   the frames ADI was handed, not of time. When the system skips an audio cycle (an overload), the
   driver drops that input and calls again; ADI sees an unbroken stream and the take goes on with a
   piece missing. That matches what the director saw (parts cut, the take went on). It cannot be
   reproduced headless. See "For the director".
3. **A long stall of the owner thread ends a MIDI take.** `MidiInputCapture` holds 4096 packets and
   writes one per callback slice; only the owner's timer drains it. At 64-frame blocks that is about
   five seconds of a blocked window (a slow save, a plug-in opening) before "the MIDI recording buffer
   filled". The audio recorder has its own writer thread and no such limit.
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
