# Sampler up to Live 12's Sampler (SC-483): the log

Queue: the 116 rows of `docs/review/device-parity-2026-11-28.md`, part SC-483 (on main; the row
numbers below count that table from 1). Sound, controls and tests only; the card's look is the devices
seat's.

- Code: branch `basics/sampler-chain`, started from `land/basics-1128c` (one chain, a commit or a few
  per item, SC-483 in each message).
- This log: `adi_daw/collab/basics/sampler.md` on `basics/sampler-log`.
- No pull request is opened: win merges the chain into a landing stack.

## Where things are (read this first if you continue the work)

- The device a user loads is the built-in `org.adidaw.builtin.sampler`: `src/juce/builtin_host.cpp`
  (`BuiltinDevice`) hosts `SamplerNative` (`src/adi/pd_builtins/native_devices.cpp`), which wraps the
  core `device::Sampler` in slot mode (`src/adi/dsp/sampler.cpp`): 32 layers, each a `OneShot` voice
  engine (`src/adi/dsp/oneshot.cpp`) playing the host's sample slot.
- **Parameters** are the lines of `src/adi/sampler_declarations.hpp`, one per core parameter, same
  order. Ids are fixed: add at the end, never renumber. A state without a parameter gives it its
  default (DF-E57), so a project saved before a parameter existed opens with the default, and the
  default must be the old sound. Today: 1 to 24 as on 28 November, 25 Reverse, 26 the loop fades' law,
  27 Round Robin, 28 its mode. In the core the Sampler's own parameters come after the 23 it hands to
  every layer: `kSelect`, `kReverse`, `kLoopFadePower`, `kRoundRobin`, `kRoundRobinMode` in
  `sampler.hpp`.
- **What belongs to one sample or one zone is not a parameter.** It is the zone table: the state
  stream `zones` of the built-in (role name `samplerZonesRole`), beside `state` and `sample:1` to
  `sample:32`. In Live these values are not automated either. `Sampler::Zone` in `sampler.hpp` is the
  list of fields; `zoneJson` and `zoneFrom` in `sampler.cpp` write and read them (a new field: add it
  to the struct with the old behaviour as its default, to both functions, to `valid`, and to
  `playLayer` or `applyZone`). The stream lists only slots that differ from a fresh zone.
- A zone edit from anywhere (a future editor, a control surface, a test) is one ordinary
  `device.loadState` op with role `zones`. So Undo, presets (they take every role) and reopening work
  as for any device state, and the project's schema did not change: `plugin_state.stream_role` is free
  text. **For whoever builds the editor:** the first edit should write the fresh table first, as the
  sample ops write an empty slot first ("the opener" in `src/adi/sample_slots.cpp`), or Undo of the
  first edit removes the row and a running device keeps the edited table until the project is reopened.
- The voice engine is shared with the OneShot instrument. What the Sampler adds to a voice goes in
  through `OneShot::Travel` (set per layer per block) and the per-note ticket of `queueNoteGain`; a
  fresh `Travel` must leave OneShot's arithmetic bit for bit as it was.
- The devices lane's Sampler branches (`devices/sampler`, `devices/sampler-loadable`,
  `devices/perf-sampler`, the `devices/oneshot*` ones) are all merged into main: nothing unlanded was
  left to take over.
- Build and test: `slot.sh run 9 origin/basics/sampler-chain core 'adi_sampler_tests|adi_builtin_devices_tests|adi_oneshot|adi_builtin_realtime|adi_sample_slots'`
  for a quick turn; `all` before the round ends.

## Round 1 (28 November 2026)

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-483, the sound of 28 November | `basics/sampler-chain` 08a4d16bc | A test loads a state written as today's build writes it (24 parameters away from their defaults, two samples), plays two notes and prints 26 probes of the render. The probes are to be held in the test as the proof that a project saved today sounds the same. | Round 2: slot 9 ran 84528d456 (the five Sampler suites passed, MSVC Debug, warnings as errors); a probe-only branch `basics/sampler-probe` (3d261c606, made from 84528d456 to fail on purpose and print, not for merging) printed the 26 probes; they are held in `samplerSavedBefore` at 536b20062, within 1e-6. The chain head running that test: see "State of the builds". | probes held; proof on the head pending |
| SC-483, rows 2, 32 to 37, 44, 45, 47 to 49 (and 3, 10 in part) | `basics/sampler-chain` 84528d456 | The zone table reaches the loaded Sampler and is saved with the project: state stream `zones`. Root key, tuning, level, pan, start, end, loop, and the key, velocity and sample-select ranges with their fades, per slot. Host seam: `NativePdDevice::stateRoles/saveState/loadState/pumpMainThread`. | Tests written: core (`slotZoneState` in `tests/test_sampler.cpp`: each field read off a ramp sample, the stream's round trip, eight refusals); hosted and in a project through reopening and Undo (`samplerZones` in `tests/test_builtin_devices.cpp`). Round 2: passed at 84528d456 on slot 9 (`adi_sampler_tests`, `adi_builtin_devices_tests`, `adi_oneshot*`, `adi_builtin_realtime`, `adi_sample_slots`: 5 of 5). | proved (core, MSVC) |
| SC-483, rows 11, 42, 51, 52, 54, 55, 56 | `basics/sampler-chain` 8c27325aa | Sustain loop with its own end, forward or forward and back; after note-off: stay in the loop, play on, release loop forward or forward and back, with its own crossfade (zone fields `loopEnd`, `pingPong`, `release`, `releaseLoop`, `releaseFade`). Parameter 26: loop fades constant-power or linear. Parameter 25: Reverse, taken at note-on. | Tests written: `slotLoops` in `tests/test_sampler.cpp` (every mode read off the ramp frame by frame; both fade laws; Reverse whole, trimmed, through a loop, latched). NOT RUN YET. | pushed, unproved |
| SC-483, rows 17, 18 | `basics/sampler-chain` 4eae49e4b | Round Robin: parameter 27 on/off, 28 Forward, Backward, Random, Other. | Tests written: `slotRoundRobin` in `tests/test_sampler.cpp`. NOT RUN YET. | pushed, unproved |

## Round 2 (28 November 2026, a second session)

The row numbers count the SC-483 table's features from 1, its section rows (Zone Tab, Sample Tab and
so on) not counted, as round 1 did.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-483, rows 43, 57, 58 | `basics/sampler-chain` 66fb9006a | Zone fields `loopDetune`, `releaseDetune` (cents, -50 to 50): the pitch inside the sustain loop and inside the release loop, the attack untouched. Parameter 29 Interpolation: None, Normal (the straight line of before, the default), Good (a four-point Catmull-Rom cubic). Parameter 30 Snap: start, end, loop start, loop end and release-loop start move to the next zero crossing. | Tests written: `slotQuality` in `tests/test_sampler.cpp` (each detune read off a ramp; a parabola read at half speed: None holds the frame below, Normal the straight line, Good lands on the curve; a sample whose sign turns every 100 frames for Snap). NOT RUN YET. | pushed, unproved |
| SC-483, rows 66, 68, 69 | `basics/sampler-chain` 043f9958e | Parameter 31 Detune (the instrument's cents, added to each zone's), 32 Spread (two voices a note, the lower left, the higher right, 25 cents from the note at 100 %), 33 Zone shift (-48 to 48 st: the note chooses zones as that key, keeps its pitch). A Spread pair's second voice does not retrigger the first (`partner` on OneShot's note ticket). | Tests written: `slotPitch` in `tests/test_sampler.cpp` (both channels of `renderSlots` now). NOT RUN YET. | pushed, unproved |
| SC-483, rows 65 (the sample's pitch; the oscillator's is not built), 78 | `basics/sampler-chain` af5f32bd9 | The pitch envelope (parameters 34 to 42: on, amount -48..48 st, attack, decay, sustain, release, initial, peak, end) and the filter envelope (43 to 51, amount -72..72 st of cutoff), per voice: Initial to Peak over the attack, to Sustain over the decay, held, from where it is at note-off to End over the release, straight segments (`OneShot::Envelope`, `EnvelopeState::advance`). Off by default; on at amount 0 nothing changes (the filter keeps its block coefficients). The Sampler's block values now have their own size, `Sampler::kParamCapacity` = 128. | Tests written: `slotEnvelopes` in `tests/test_sampler.cpp` (the read speed of a ramp through attack, sustain and release; a 4 kHz tone through a 500 Hz low-pass opened and closed by the filter envelope). NOT RUN YET. | pushed, unproved |

### State of the builds

Slot 9 was asked for its first build at the start of the round and stood at "waiting for a build
place" for the whole of the writing (other sessions were building). Nothing above has been compiled
yet. The order to run, once the slot is free:

1. `slot.sh run 9 84528d456 core '<the filter above>'`: this commit has today's voice engine untouched,
   so the line `METRIC Sampler, the state of 28 November: ...` in the test's output is the old sound.
   Read it with `slot.sh grep 9 core.ctest 'METRIC Sampler, the state'`.
2. Put those 26 numbers into `samplerSavedBefore` (`tests/test_builtin_devices.cpp`) as the expected
   values, compared within 1e-6, in place of the "finite and sounds" check.
3. `slot.sh run 9 origin/basics/sampler-chain core '<filter>'`, fix what the compiler and the tests say,
   then `all`.

## Decisions taken here, and why

- **Zones are device state, not parameters.** 32 slots times some 25 values would be 800 parameters on
  the card and in every automation chooser, and Live does not automate them. The stream is the device's
  own state; the project's schema is untouched.
- **A separate stream, not a part of `state`.** `state` holds the parameters; an Undo of a zone edit
  must not carry parameter values with it.
- **Reverse is global**, as in Live (the plan's ADR-0169 d4 names it per zone; the parity list noted the
  difference). It is taken at note-on: a sounding note keeps its direction.
- **Loop positions are fractions of the whole sample**, as start, end and loopStart already were.
  `loopEnd` below 0 means "at the zone's end", so every zone and state written before keeps its loop.
- **Release modes act only when the sustain loop is on**, as Live's manual says.
- **The loop crossfade** keeps ADI's shape (the end of the loop blended with what follows the loop's
  start, over up to half the loop). Forward-and-back loops have no crossfade: there is no join.
- **Round 2. A loop's Detune acts while the voice is inside that loop** (from its first pass over the
  loop's start), so the attack before the loop keeps the sample's pitch. Live's manual says the control
  corrects the loop's pitch; when exactly it starts is not written there.
- **Interpolation's default is Normal**, ADI's straight line, because a project saved before must sound
  the same and a missing parameter takes its default. If Live's default for a new Sampler is Good, a
  new instrument would need a different start value than an old project's: that is a question below.
- **Snap and Interpolation are instrument-wide parameters**, as Live shows them; Snap acts on all of a
  zone's points (Live's per-marker snap is an editor's gesture, not built).
- **Spread's law is ADI's**: 25 cents each way at 100 %, the two voices hard left and right (with the
  layer pan law, sqrt(2) on the voice's side). Named `Sampler::kSpreadCents`. A pair uses two of the
  Voices.
- **Detune, Spread and Zone shift are read once a block**, as the other Sampler-own parameters are.
- **The parameter arrays** (`values_`, `modulation_`, `initial`) were sized `OneShot::Count` (41); the
  envelopes took the Sampler to 51, so they are `Sampler::kParamCapacity` (128) now.
- **The envelopes' segments are straight** and their slopes are not built (row 83, Live's curvature
  controls); their loop modes (row 85) neither. Their parameters are the Sampler's own and are read
  once a block; each voice keeps its own stage and level. The defaults (attack 0, decay 600 ms,
  sustain 0 %, release 50 ms, initial 0 %, peak 100 %, end 0 %) are ADI's; Live's are a reading.
- **The filter envelope's amount is in semitones of cutoff** (±72 st, six octaves), as Live labels it.

## For the director

1. **Two things a user sees are new on the Sampler's card, in today's generic look:** the switches
   Reverse and "Loop fade: constant power", and Round Robin with its mode menu. They are plain
   parameters. Recommendation: keep; they move to their places when the Sampler's sheet is built.
2. **Reverse with a release loop: what does Live do?** The manual does not say. Built: the release loop
   stays "the last stretch the voice reaches". Recommendation: leave until someone reads it in Live;
   it is one line in `OneShot::noteOn`.
3. **Round Robin's reset interval (row 19)** is not built. It needs the song position in the device.
   Recommendation: next round, as a parameter "Round Robin reset" (Off, 1/16 to 4 bars), reset on the
   transport's bar grid.
4. **Round 2 adds 23 more controls to the generic card:** Interpolation, Snap, Detune, Spread, Zone
   shift, and nine for each of the pitch and filter envelopes. The generic card now lists 51.
   Recommendation: keep; they move to the Pitch/Osc, Filter/Global and Sample sections when the
   Sampler's sheet is built. If 51 rows on one card is too many for today, the devices seat could fold
   the envelope rows; this session did not touch the card.
5. **Readings owed from Live** (each is one named constant or one line): Interpolation's menu words and
   the default of a new Sampler; Spread's detune at 100 % (`kSpreadCents`) and whether the two voices are
   fully apart; Zone shift's range and direction (built: +12 makes a key choose the zones an octave up);
   whether a loop's Detune also acts before the voice reaches the loop. Recommendation: one Live session
   with a ramp sample answers all of them.

## Reserve

- Nothing reserved. Not taken: no schema minor, no ADR number, no op name, no register id. If win
  wants the `zones` stream written down as a decision, it is one ADR ("a built-in may keep further
  state streams; the Sampler's zones are one") which this session did not take.

## Next (in the mission's order)

1. Prove the items above (see "State of the builds"), then run `all` once: the window suites have
   not seen the new parameters on the generic card.
2. Still in "one sample": the zone's Scale (row 46, key tracking in percent), the Link of start and loop
   start (row 53, an editor's convenience).
3. Zones: Round Robin's reset interval (row 19); the instrument-wide Lin/Pow of the zone fades (row
   16: today a field of each zone).
4. The auxiliary envelope with its two destinations (rows 90, 91): `OneShot::Envelope` and
   `EnvelopeState` are ready for a third; the destinations want a small fixed target list first
   (cutoff, pan, volume, pitch, sample offset), each a term like the filter envelope's. Then the
   envelopes' slopes (row 83) and loop modes (row 85); three LFOs with destinations; Glide (row 70);
   the filter's circuits and its shaper; the modulation oscillator; MIDI routing and MPE last.
