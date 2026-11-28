# oneshot — log

The OneShot stream: OneShot up to Live 12's Simpler (row SC-482), sound, controls and tests only, no
look. The feature list is `docs/review/device-parity-2026-11-28.md`, part SC-482 (89 features, each
with the smallest test that would prove it). The how-to is `collab/prompts/2026-11-27-basics-howto.md`.

- **Code:** one chain, `basics/oneshot-chain`. **This log:** `basics/oneshot-log`.
- **Base:** `origin/land/basics-1128c` until it is an ancestor of `origin/main`, then `origin/main`.
  The chain also merges `origin/main` (the parity list lives there; it had not reached the landing
  stack on 28 November).
- **Rules for this device:** a project saved today opens and sounds the same (a test proves it);
  parameter ids are added, never renumbered; nothing allocates or locks on the audio thread; the
  card's look (`src/juce/ui_devices.cpp`) is the devices seat's and is not touched.
- **Earlier work looked for:** `walk/a5-oneshot-sample-tile`, `walk/a7-oneshot-in-browser` and every
  `devices/oneshot*` branch are already in the base; the devices lane's log holds no unlanded OneShot
  work. Nothing to take over.
- **Base baseline (slot 8, 28 Nov):** a full core check of `origin/land/basics-1128c` (16064c0a)
  passes 290 of 291; `adi_link_playback_tests` fails there (Link tempo/clock checks), not ours.

## How the work is laid out (for whoever continues)

- Everything is in `src/adi/dsp/oneshot.{hpp,cpp}`, the canvas `pd/devices/OneShot.pd`, its notes
  `pd/devices/OneShot.md` and the tests `tests/test_oneshot.cpp` (one function per item, called from
  `main` in order, after `liveDefaults`).
- A new control is: an `enum Param` entry before `Count` (never in the middle), a `specs[]` row in
  the same place, an `adi.param` + `msg p N` pair in the canvas placed before the `#X connect` block
  with its two connect lines appended at the end of that block (object numbers continue: the next
  free object is **148**, the next free canvas id **66**, the next free `p` index **65**), and a
  `paramText` case if it is a menu. `adi_oneshot_pd_tests` checks every canvas declaration against
  the native names, units, ranges and defaults, so a mismatch fails there.
- `OneShot::loadState` accepts any count from `kLegacyCount` (41, before SC-482) up to `Count`; a
  missing value takes its default. The built-in host does the same for the canvas (DF-E57). Every
  new default must play exactly what OneShot played before (the item's test says so).
- Per voice, `p[]` is constant over a stretch (events split stretches); `settle()` recomputes the
  derived values after events. Things that may cost (exp2, tan) are cached per voice on their input,
  as the read increment already was.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-482 (1) envelopes | `basics/oneshot-chain` 2d2ed1d2 | Filter envelope (A, D, S, R, amount +-72 st) and pitch envelope (on, A, D, S, R, amount +-48 st) beside the amp envelope; ids 42..52. Old states (41 controls) load with defaults. | `adi_oneshot_tests`: `legacyState` (a 41-control state opens, takes defaults, renders bit-identical), `filterEnvelope` (cutoff follows a changed attack; negative amount; block-size invariance), `pitchEnvelope` (starts an octave up, falls through the decay to the held pitch). Slot 8, core, filter `oneshot\|sampler\|builtin\|device_presets`, at 324fba66: 6 of 6 pass. | PR requested |
| SC-482 (2) filter | `basics/oneshot-chain` 79da2ef6 | Live's two circuit choosers (Clean/OSR/MS2/SMP/PRD for LP/HP, Clean/OSR for BP/notch), Drive 0..24 dB, Filter < Vel, Filter < Key; ids 53..57. Clean is the old stage, bit for bit. | `filterCircuits`: the four types at both slopes against their analogue responses two octaves either side; each circuit clips a resonant loud tone and passes a quiet one as Clean; Drive raises MS2's third harmonic; Clean and notch ignore Drive; BP reads its own chooser; key and velocity move the cutoff. Slot 8 at 324fba66: 6 of 6. | PR requested |
| SC-482 (3) LFO | `basics/oneshot-chain` 324fba66 | LFO Attack, Retrigger (on by default), Offset (degrees), LFO < Key; ids 58..61; a free-running LFO clock that runs through silence. | `lfoControls`: phase read off a full-depth square on the volume: attack depth at a known sample, offset moves the first switch, retrigger off joins the free clock, key 100 % doubles the rate an octave up. Slot 8 at 324fba66: 6 of 6. | PR requested |
| SC-482 (4) glide | `basics/oneshot-chain` 87e12e70 | Glide (Off/Glide/Portamento) and Glide time; ids 62, 63. Glide is mono legato; Portamento slides each new voice from the newest sounding pitch. | `glideModes`: on a ramp the step is the sum of read increments: Glide starts at the held pitch, passes the half-way pitch, ends alone at its own; Portamento keeps both voices; Slice mode never glides. | check pending |
| SC-482 (5) pan | `basics/oneshot-chain` 9fd485a5 | Pan (50 L .. 50 R) and Pan < Rnd (own generator); ids 64, 65. | `panControls`: hard left; centred with 0; random pan moves notes at constant power. | check pending |
| SC-482 (6) spread | `basics/oneshot-chain` cd0482a0 | Spread (canvas id 66, `p 65`, enum `Spread`, 0..100 %, default 0): two voices a note at half level, the upper detuned up and panned left, the lower down and right, both by the amount; at 100 % 50 cents apart and hard left/right (`kSpreadCents`, ADI's span). Each voice has its own read position (`Voice::position2`, `increment2`), loops and fades out on its own; the note ends when both have. At 0 the old path runs untouched. | `spreadVoices`: on a ramp each channel's slope is its voice's increment (2^(+-0.25/12)); the upper voice stops first, the note when both have, `sounding()` false; Spread 50 on DC gives 0.5 (sqrt 1.5 + sqrt 0.5) per side; no allocation. `legacyState` still renders a 41-control state bit-identically. | check pending |
| SC-482 (7) reverse, crop | `basics/oneshot-chain` 1e0d2111 | `prepareSampleReverse`, `prepareSampleCrop` in `src/adi/sample_slots.{hpp,cpp}`: a changed copy of the slot's sample written to `Samples/Processed/Reverse` or `.../Crop` in the project folder (original name + command, numbered when taken), assigned through the existing `media.import` + `device.loadState` path, one Undo step; Crop adds two `device.setParam` (Start 0 %, End 100 %). The original file and media row stay. No new op, no schema change. **Not wired to the tile:** its menu is the devices seat's (`ui_devices.cpp`), see "For the director" 3. | `reverseAndCrop` in `adi_sample_slots_tests`: a stereo ramp mirrored frame by frame per channel; a new file and a second media row; Crop at 25..75 % keeps frames 100..300 of 400 and resets the markers; Undo restores sample and region. | check pending |
| SC-482 (8) sensitivity; Mono, Poly | `basics/oneshot-chain` 2fb9bfd8 | Sensitivity for Slice by Transient (canvas id 67, `p 66`, enum `SliceSensitivity`, 0..100 %, default 50 %): `prepareSample` runs 21 detectors in one pass (5 % steps), each the old law with its thresholds x 2^((10 - k) / 5); step 10 multiplies by exactly 1, so saved projects slice as before. `PreparedSample::transients` is now an array of 21 lists. | `sliceSensitivity`: clicks of 0.03 / 0.08 / 0.3 / 0.9 give 3, 2 and 4 slices at 50, 0 and 100 %; `sliceKeys` adds Mono one-at-a-time vs Poly overlap, and Poly's Retrigger on/off on one slice. | check pending |
| SC-482 (9) slices by hand | `basics/oneshot-chain` 3a19f22f | `prepareSampleSlices` (`src/adi/sample_slots.cpp`): Slice by Manual's markers as an optional `slices` field of the slot's sample state (sorted, unique, 0 implied, <= 63, before the end), one `device.loadState`, one Undo step; adding, moving and deleting are each this call with the changed list. `restoreBuiltinSample` (`src/juce/builtin_samples.cpp`) passes the field to `prepareOneShotSample`. `OneShot::transientSlices` gives the window a Sensitivity's list (to turn a transient into a marker). `docs/ENGINE-SAMPLE-SLOTS.md` names the field. Not wired to the tile (devices seat). | `adi_sample_slots_tests` `reverseAndCrop` tail: markers placed, moved, deleted, refused past the end, undone, each read by playing the restored slot through OneShot; `adi_oneshot_tests` reads each Sensitivity's list. | check pending |

## For the director

1. **New controls in today's generic panel.** OneShot's card builds its controls from the canvas
   list, so the 24 controls added here (filter and pitch envelopes, circuits, Drive, Filter < Vel and
   < Key, LFO Attack/Retrigger/Offset/< Key, Glide, Glide time, Pan, Pan < Rnd) appear in today's
   generic panel after the 41 that were there. Their places on the card follow sheet 7 when the
   devices seat builds it; nothing of the card's look was touched. **Recommendation:** leave them in
   the generic list until the card is built from sheet 7.
2. **Live readings these items leave as named values** (one sitting with Live 12 and Simpler, each a
   single question): the filter and pitch envelopes' default times and sustain, and whether their
   Sustain is a level; the segment slopes of all three envelopes; Filter < Vel's span (ADI: four
   octaves at 100 % and full velocity, `kFilterVelocityOctaves`); whether Drive acts on Clean (ADI:
   no) and on the notch (ADI: no); each circuit's law (ADI's are saturations around Clean's stage);
   the LFO Attack range (ADI: 0..10 s); Glide time's range and default (ADI: 0..5 s at 50 ms) and
   whether legato Glide restarts the sample (ADI: no). **Recommendation:** add these to the one
   readings list for his session.
3. **Reverse and Crop in the sample tile's menu** (something a user sees; Live has both in Simpler's
   context menu). The commands exist and are tested (`prepareSampleReverse`, `prepareSampleCrop`,
   then `commitSampleAssignment`); nothing calls them from the window, because the tile is the
   devices seat's file (`src/juce/ui_devices.cpp`). **Recommendation:** the devices seat adds
   "Reverse" and "Crop" to the tile's menu as Live names them, beside its Folder and Hot-Swap
   actions, submitted like the sample drop.
5. **The sample state's new optional `slices` field** (item 9). It is a field of the built-in sample
   state, which SPEC.md versions apart from the project schema and whose unknown fields older builds
   keep; no schema minor was taken. **Recommendation:** name it in SPEC.md's "Built-in sample roles"
   beside mediaHash and source_f32le when the format owner next edits it.
6. **Markers on the tile** (add by double-click, drag, delete by double-click, as Live does) are the
   devices seat's to draw; the commands under them exist (item 9). **Recommendation:** as 3.
7. **Sensitivity's default and law** are readings (ADI: 50 %, 5 % steps, 2^(1/5) a step).
4. **Spread's law** is a reading: ADI plays two voices at half level, detuned +-25 cents and panned
   hard apart at 100 % (`kSpreadCents` = 50), both scaling with the amount. **Recommendation:** add
   it to the readings list.

## Next

The chain stands at 3a19f22f (items 1..9). Next free: object **152**, canvas id **68**, `p` **67**,
enum after `SliceSensitivity`.

1. Checks: slot 8 had a check of 00778800 (items 4, 5 + origin/main) waiting for a build place from
   17:33; then run `slot.sh run 8 origin/basics/oneshot-chain core 'oneshot|sampl|builtin|device_presets'`
   for the head and mark items 4..7.
2. Slice rows left: Trigger/Gate against the slice or the whole Thru region (fades measured on the
   slice vs the Thru region: one test); by beat and equal regions already have tests.
3. Then: Morph (needs a choice: extending Filter type's menu changes the normalized value of every
   saved Filter type, so a separate control is the safe way); envelope loop modes; Warp last, only
   through the engine's stretcher if its interface is on the base, otherwise blocked on that seat.
