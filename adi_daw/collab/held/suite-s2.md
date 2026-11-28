# Track S, wave S2: the mix suite's first additions (held chain)

**This chain lands only when the director says so.** It is built ahead of the wave's opening (ADR-0254
d1, d25: the wave was to open after 3 December); the director asked to advance his phases now. No pull
request is open for it, and none is opened until he gives the word.

- Chain: `held/suite-s2-chain`. Started on 28 November from `origin/land/basics-1128c`; that stack is
  now an ancestor of origin/main, so BASE is origin/main (merged in at `48b8cc20`). Merge the newest
  BASE in before each push.
- This log: `adi_daw/collab/held/suite-s2.md` on `held/suite-s2-log`.
- Mission: PLAN Track S, wave S2, the engine and device parts, in this order: DEV-33, MX-38, MX-39,
  MX-40, AU-10's engine part. DEV-34 is another session's (OneShot). The window parts are written
  down per row below, for the window work that follows.
- Build and test: the build PC's slot 21, through `slot.sh` (core = MSVC Debug, warnings as errors,
  `ctest -LE performance`, the acceptance suite included).

## Rows

| Row | Chain commit | What changed | How it was proved | State |
|---|---|---|---|---|
| DEV-33 | `e486c1e4` | Echo's ducker is one kernel (`src/adi/dsp/ducker.hpp`); Echo runs on it unchanged. Reverb (ids 32 to 34) and Delay (ids 28 to 30) gain Duck On, Duck Threshold, Duck Release after their own parameters, off by default, Echo's ranges and defaults; the canvases declare them. Delay's feedback is not ducked | slot 21 core at `e486c1e4`: build OK with /WX; 290 of 291 tests pass, among them `adi_reverb_tests`, `adi_delay_tests` (off nulls bit for bit; the law against Echo's written out; the tail returns), `adi_pd_native_devices_tests` (two fixtures with Duck on, canvas against core, bit-exact), the Pd suites. The one failure, `adi_link_playback_tests` (Link tempo timings), touches nothing of this row: see Notes | built, proved |
| MX-38 | `e7ee40999` | `Session::setHearDifference(deviceId)`; the graph captures the armed device's aligned dry (the line it keeps for bypass) minus its output, as a copy, never while exporting (`Graph::setDifferenceNode`, `copyDifference`); `GraphHost` hands the armed node to each graph that plays (`setDifferenceNode`, `copyRenderedDifference`); the cue monitor puts it on the physical Main pair with the reference's five-ms swap, before the Main auditions. A rebuild keeps it on its device and ends it when the device is gone | `adi_cue_monitor_tests` `hearDifference`, both precisions: -6 dB of input for a gain of 0.5 at latencies 0, 7 and 33; a device that only delays nulls exactly; a bypassed device gives silence; off gives Main back untouched; no allocation; a render with it on equals one with it off | slot 21 core check of `48b8cc20` queued |
| MX-39 | `e109f6af1` | Every device node carries an `engine::OutputMatch` (`src/adi/engine/output_match.hpp`): Off (default, untouched bit for bit), Match (ADR-0166's matcher holds the output at the input's loudness), Fixed (Match Output Gain: the matcher's gain at that moment, kept, the matcher stopped). `DeviceNode::setOutputMatch`, `matchedOutputDb`. The matcher gains a 64-bit path; its float path is the same arithmetic. **Not stored yet**: the flag and gain wait for their schema minor (Reserve below) | `adi_device_model_tests` `testOutputMatchHoldsTheInputLoudness`, 32 and 64 bit: off bit-identical; a device adding 6 dB reads within 0.5 dB of its input; the committed gain keeps the level and no longer follows the device | engine half built; check queued; storage blocked on the minor |
| MX-40 | `f8d77038c` | `Session::setPreviewTrack(trackId)`: the Browser's preview at an audio track's junction beside its input monitor (N65's seam), heard through its devices and mixer path; 0 (default) is Cue Out, as Live's. A callback in which the track played it leaves it off Cue Out (`PreviewThroughTrack`, `CueMonitor::setPreviewThroughTrack`). Nothing written, nothing recorded. Refused for a missing, non-audio or frozen track, no player, mono, an export | `adi_cue_monitor_tests` `previewThroughTrack`: transport stopped, the preview passes the track's device and Main, Cue Out silent; off, Cue Out only; no undo step | check queued |
| AU-10 (engine) | `09d86cd3f` | `automation::prepareThrow(store, laneId, ThrowSpec, rate)` (`src/adi/automation_throw.*`): one sealed `arranger.splice` (AU-02's shared splice) over [begin, end + release]: from the lane's own value it rises to `open`, holds, and falls back to the lane's own value at the release's end. `laneValueAt` reads a lane as it plays | `adi_automation_throw_tests` (new), both clocks, empty lane and a curve, with and without a rise: outside unchanged within 1e-9, hold and fall, the old curve back at the edge, one Undo removes every point, Redo; refusals leave no undo step | written; first check owed |

## What each row's window part will need

### DEV-33, the Duck inside Reverb and Delay
- **Controls:** three on each device's panel: Duck (a switch), Threshold (dB, -80 to 0, default -24),
  Release (ms, 1 to 5000, default 200, log). They are ordinary declared parameters (`adi.param` in
  `pd/devices/Reverb.pd` and `Delay.pd`), so today's generic device panel already shows them as it
  shows Echo's, and they automate and map as any parameter does. No command and no sheet.
- **Look (director):** their place once the panels get their agreed look (ADR-0243). Item 1 below.

### MX-38, Hear Difference
- **Command:** "Hear Difference" on a device: a toggle in the device's context menu (and its title
  bar once the look is agreed), calling `session.setHearDifference(deviceId)`; again, or on another
  device, `setHearDifference(0)` / the other id. One device at a time. Outside undo; not saved.
- **State shown:** the device's toggle lit while it is on; the window reads `hearDifference()` after
  each refresh, since a deleted device turns it off.
- **Mix View:** its Hear What Is Removed for the native stages shares this delivery: arm the stage's
  node with `GraphHost::setDifferenceNode` (the Mix View seat's half; ADR-0247 d8).
- **Look (director):** where the toggle sits. Item 2.

### MX-39, auto gain-match and Match Output Gain
- **Storage first:** a schema minor for the per-device flag and the fixed gain (Reserve 1), and op
  names (Reserve 2). The session then applies them at refresh with `DeviceNode::setOutputMatch`, as
  it applies `devices.enabled`.
- **Controls:** a switch "Auto Gain Match" on a device (context menu, later its title bar), and a
  command "Match Output Gain" that reads `DeviceNode::matchedOutputDb()` and commits it with the op
  (one undo step). While matching, the gain applied can be shown in dB.
- **Look (director):** item 3.

### MX-40, preview through the selected track
- **Control:** a switch in the Browser's preview row, off by default, shown only with a previewable
  row selected (SC-429); its state is kept with the program, not the project (ADR-0261 d13).
- **Wiring:** while on, the window calls `session.setPreviewTrack(selectedTrackId)` on every track
  selection change, and `setPreviewTrack(0)` when it is switched off. A refused track (MIDI, group,
  return, Main, frozen) keeps the preview on Cue Out; the switch can say "Cue Out" then. Each change
  rebuilds the graph, so the window calls it only when the target really changes.
- **For the director:** item 4 (MIDI tracks).

### AU-10, the Throw
- **Command:** "Throw" over an Arrangement time selection. The window resolves the target to a lane
  (a send's level lane; a device's mix parameter from SRF-14's role tags, or the one the user picked
  for a hosted plug-in), converts rise and release from milliseconds to the lane's native clock, and
  commits `prepareThrow`'s single request. Where the target is chosen it says what ADR-0254 d3 says:
  on a send the tail rings on; on a mix the tail closes with the mix and the release holds it open.
- **Owed in the engine (next round):** a target with no lane yet. `prepareThrow` refuses a missing
  lane; the lane's creation and the Throw must be one undo step, so the engine half moves onto
  `stageEnvelopeSplice` with the caller's `RowEdits` (the lane staged first). The rack form waits for
  DEV-01.
- **For the director:** item 5 (default rise and release).

## For the director

1. **DEV-33, where the Duck sits on Reverb's and Delay's panels.** Live has no Duck on either.
   (a) Echo's three controls in Echo's order, a small group at the right edge after Dry/Wet;
   (b) under Dry/Wet. Recommendation: (a). Reply "Duck: a" or "b".
2. **MX-38, where Hear Difference is switched.** (a) the device's context menu now, its title bar
   when the look is agreed; (b) the title bar only. Recommendation: (a). Reply "Difference: a" or "b".
3. **MX-39, where Auto Gain Match and Match Output Gain are.** (a) the device's context menu, the
   matched gain shown in the title bar while on; (b) a small switch in every device's title bar.
   Recommendation: (a), since (b) adds a control to every device Live does not show. Reply "Match: a"
   or "b".
4. **MX-40, a MIDI track selected.** The preview could go into the instrument's output or stay on
   Cue Out. Recommendation: Cue Out (an instrument does not take audio in; the preview through its
   effects after the instrument is a later choice). Reply "Preview MIDI: Cue Out" or "after the
   instrument".
5. **AU-10, the Throw's defaults.** Recommendation: open at once (no rise), release 20 ms on a send
   (the tail rings on) and 250 ms on a mix (a short tail held open); the open value 0 dB on a send and
   100 % on a mix. Reply "Throw: as recommended" or with other numbers.

## Reserve

1. **Reserve: schema minor for MX-39**, the per-device auto gain-match flag and its fixed gain (two
   columns on `devices`, or one side table keyed by device). RESERVATIONS.md: "MX-39's per-device
   gain-match flag and fixed gain", reserved when the row starts; it has started. The next free is
   1.29.
2. **Reserve: op names for MX-39**: `device.setGainMatch` (the flag) and `device.matchOutputGain`
   (the commit: the measured gain written, the flag off), or one `device.setOutputMatch` with a mode.
   Recommendation: the two, as the two gestures are.

## Notes for whoever continues

- Next: push the AU-10 commit with the chain (merge BASE first, run the five validators), run
  `slot.sh run 21 origin/held/suite-s2-chain core`, read the result, fix what fails, update the Rows.
  Then AU-10's staged form (a lane created in the same step), then MX-39's storage once the minor and
  the op names are reserved on main.
- `adi_link_playback_tests` failed in the first check (six Link tempo and phase checks, 40 s, while
  about twenty sessions shared the build PC). DEV-33 touches only Reverb, Delay, Echo and two
  canvases, none of which Link playback uses. Run it alone (`slot.sh run 21 <ref> core
  '^adi_link_playback_tests$'`) to see whether it is the load; if it fails alone, compare with
  origin/main.
- MX-38's difference is taken at the device's own output point. It is not delayed to Main's
  latency: the five-ms swap between Main and the difference crosses whatever downstream latency there
  is. A later refinement can delay it by the graph latency minus the device's output latency.
- MX-39 measures the device's input as handed to it, not delayed by the device's latency: the
  matcher's windows are a second long, so a few milliseconds move it by that much and no more.
- The slot's first build is cold (up to an hour). `bash /d/adibuild-tools/slot.sh wait 21` again while
  it says STILL RUNNING.
- `collab/devices.md` (the devices seat's log) still lists Reverb's and Delay's parameters without the
  Duck; it is that seat's file, so it is not edited here.
