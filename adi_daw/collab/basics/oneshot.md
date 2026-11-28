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
| SC-482 (1) envelopes | `basics/oneshot-chain` 2d2ed1d2 | Filter envelope (A, D, S, R, amount +-72 st) and pitch envelope (on, A, D, S, R, amount +-48 st) beside the amp envelope; ids 42..52. Old states (41 controls) load with defaults. | `adi_oneshot_tests`: `legacyState` (a 41-control state opens, takes defaults, renders bit-identical), `filterEnvelope` (cutoff follows a changed attack; negative amount; block-size invariance), `pitchEnvelope` (starts an octave up, falls through the decay to the held pitch). Check on slot 8: see below. | check running |
| SC-482 (2) filter | `basics/oneshot-chain` 79da2ef6 | Live's two circuit choosers (Clean/OSR/MS2/SMP/PRD for LP/HP, Clean/OSR for BP/notch), Drive 0..24 dB, Filter < Vel, Filter < Key; ids 53..57. Clean is the old stage, bit for bit. | `filterCircuits`: the four types at both slopes against their analogue responses two octaves either side; each circuit clips a resonant loud tone and passes a quiet one as Clean; Drive raises MS2's third harmonic; Clean and notch ignore Drive; BP reads its own chooser; key and velocity move the cutoff. | check running |
| SC-482 (3) LFO | `basics/oneshot-chain` 324fba66 | LFO Attack, Retrigger (on by default), Offset (degrees), LFO < Key; ids 58..61; a free-running LFO clock that runs through silence. | `lfoControls`: phase read off a full-depth square on the volume: attack depth at a known sample, offset moves the first switch, retrigger off joins the free clock, key 100 % doubles the rate an octave up. | check running |
| SC-482 (4) glide | `basics/oneshot-chain` 87e12e70 | Glide (Off/Glide/Portamento) and Glide time; ids 62, 63. Glide is mono legato; Portamento slides each new voice from the newest sounding pitch. | `glideModes`: on a ramp the step is the sum of read increments: Glide starts at the held pitch, passes the half-way pitch, ends alone at its own; Portamento keeps both voices; Slice mode never glides. | check running |
| SC-482 (5) pan | `basics/oneshot-chain` (after 87e12e70) | Pan (50 L .. 50 R) and Pan < Rnd (own generator); ids 64, 65. **Spread not built yet.** | `panControls`: hard left; centred with 0; random pan moves notes at constant power. | check running |

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

## Next

1. Spread (two detuned voices a note, panned apart): needs a second read position per voice that
   follows the loop and fade logic; give it id 66 (`spread`, 0..100 %, default 0).
2. Reverse and crop (a changed copy of the source, the original's hash kept): the copy is made off
   the audio thread like `prepareSample`; the sample slot's media model is in
   `src/adi/sample_slots.cpp` and `src/juce/builtin_samples.cpp` — check whether a derived sample
   needs the project's schema to change (if so: "Reserve: schema minor for ..." and stop that part).
3. Slice: Transient sensitivity (a new control; today the thresholds are fixed in `prepareSample`),
   then by beat, equal regions and by hand; moving and deleting slices (the manual list lives in the
   prepared sample, so this is again the sample slot's model); Mono, Poly and Thru tests the parity
   list asks for.
4. Then: Morph (needs a choice: extending Filter type's menu changes the normalized value of every
   saved Filter type, so a separate control is the safe way); envelope loop modes; Warp last, only
   through the engine's stretcher if its interface is on the base, otherwise blocked on that seat.
