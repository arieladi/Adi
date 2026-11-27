# Queued Session launch targets

TM-SV-04 and PLAN M3-LAUNCH L4 cover edits made while a clip or scene is
waiting on its launch grid. A scene launch retains the scene's identity
through a reorder. A clip launch retains the track, scene and clip that
were admitted together. When that cell ceases to contain the named clip,
the launch is discarded. Deleting a queued scene discards its master
launch and followers. The active run carries on when its queued successor
is discarded.

`adi_capture_scene_tests` exercises those cases through the normal
Store, journal and Session publication. At 48 kHz and 120 BPM it publishes
the edits before sample 96,000. The reordered scene and its MIDI
follower fire 55 frames into a 64-frame callback. For a deleted scene
and a moved or deleted queued clip, the callbacks at the grid carry
the original active run and no new launch. In the clip cases the old
cell has a replacement clip with a different ID.

## Win Live 12.4 readings

M3-LAUNCH L4 remains open on installed Live. The Win readings branch
contains no result for these cases. The public manual documents scene
reordering and launch quantization. It leaves the fate of a queued
identity during these edits to an installed reading.
[Live manual §7.4.3](https://www.ableton.com/en/live-manual/12/session-view/#editing-scenes)
and [§16.4](https://www.ableton.com/en/live-manual/12/launching-clips/#clip-launch-quantization).

In a disposable Set at 120 BPM, use distinguishable tones on the
currently playing clip, the queued clip and any replacement. Set launch
quantization to one bar. Make each edit after the target starts blinking
and before the next downbeat. Record the target indicators, the tone
heard after the downbeat, whether the current tone stops and the
transition's sample relative to the downbeat.

1. **L4.1** Queue a scene and reorder it to a different numbered row. Check
   whether the scene's original clips or the old row's current clips fire.
2. **L4.2** Queue a scene, delete it and put a different scene at the
   former row. Check the master launch and each follower track.
3. **L4.3** Queue a clip, move it to an empty cell on its track and fill
   its former cell with a different tone. Check the moved clip, replacement
   and previously playing clip independently.
4. **L4.4** Queue a clip, delete it and replace its cell. Check whether
   the replacement fires or the prior run continues.
5. **L4.5** Repeat the applicable edits with a due scene or clip Follow
   Action and compare its result with the manual-launch case.

Record the exact Live version and results against these L4 subcases in
the Win readings log. The native test establishes ADI's current behavior
while that comparison is pending.
