# Three devices against their models — lists, 28 November 2026

**What this is.** The director put the devices' sheets (`collab/window/design/proposals/devices-6` and
`devices-7`, on the branch `win/devices-sheet-1` until a stack carries them) to his outside panel. Its
answer: the sheets are a look, and three devices do less than the tools they are measured against. A
sheet draws only what a device has today, so it cannot show what is missing. This job makes the missing
part visible: one list for each device, feature by feature. Rows SC-481, SC-482 and SC-483 of
`docs/SANITY-CHECK.md`.

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is one file,
`adi_daw/docs/review/device-parity-2026-11-28.md`, on a branch `basics/parity-lists`, with a log beside
the other sessions' (`adi_daw/collab/basics/parity.md` on `basics/parity-log`). You change no code.

## The three lists

1. **ADI-Shaper against its model** (SC-481). The model is the multi-shaper it was measured against; its
   maker's manual is public, and the session that built ADI-Shaper named the version it measured in its
   own log: use that version and say which it is. ADI-Shaper today: `pd/devices/ADI-Shaper.pd`, 418
   controls, seven shapers (Volume, Width, Pan, Filter, Sweep, Drive, Crush), each with up to three bands
   and, for a band, eight fixed points, a mix and a clock.
2. **OneShot against Live 12's Simpler** (SC-482). Live's side is the Simpler part of the Live 12
   Instrument Reference (the manual, public on its maker's site). OneShot today: `pd/devices/OneShot.pd`.
3. **Sampler against Live 12's Sampler** (SC-483). Live's side is the Sampler part of the same
   reference. Sampler today: `src/adi/sampler_declarations.hpp` and the engine behind it.

## The form of each list

One table for each device, one row for each thing the model can do:

| What the model does | What ADI does today | Where it would sit on the card | State | A test |
|---|---|---|---|---|

- **What the model does:** one plain sentence, in your own words. Do not copy the manual's text.
- **What ADI does today:** from the code, with the file and line. "Nothing" where there is nothing.
- **Where it would sit:** on the card's face, in the opened view, or in a menu, by the rules of the
  devices' sheets (the main control large, a card shows what is reached for, the arrow opens the rest).
- **State:** `has`, `part`, `missing`, or `out` (deliberately left out, with the ADR or the director's
  word that says so). A drawn control is not `has`: only code that does it and a test that proves it.
- **A test:** the smallest check that would prove it, in one sentence.

Group the rows as the model's manual groups them. At the head of each table: how many rows are `has`,
`part`, `missing` and `out`.

## What the director's panel named (check each; they are claims, not findings)

- ADI-Shaper, first of the three, by the director's word: "need to make sure it dident miss any quality or
  ability", and "it needs a browser, presets, and lots of templates". What win read on 28 November in the
  model's own support pages and a published review, each to be checked against its manual: eleven shapers
  today (pitch, reverb, time, drive, noise, filter, a flanger and phaser, crush, volume, pan, width; an
  earlier version had nine), a compressor of up to three bands and an oscilloscope beside them; the
  shapers' order changed by dragging; a free curve with drawing tools for curves, steps and ramps, a grid
  and a smoothing; ready-made shapes for each shaper and the user's own, copied between shapers; several
  shapes held ready and switched by MIDI notes; the shape restarted by any MIDI note, once or looping, or
  by a transient in the track or in a side-chain, with a sensitivity, a threshold and a filter on what is
  listened to; an envelope follower that can be laid over the shape; more filter kinds than ADI's five;
  the user's own noise samples and reverb responses; a browser of several hundred presets with styles,
  tags and favourites. For every shaper ADI has, add how its sound was compared with the model's (the
  session that built it measured: find its measurements) and what the difference was. Presets, shapes
  and templates for ADI are made for ADI; none of the model's own are taken. Also from the panel: the
  number of modules in the model (the panel read eleven in its manual's list and nine in
  its introduction); what ADI's Sweep corresponds to; whether the shapers' order can change; free points
  and the drawing tools (lines, arcs, S-curves, smoothing, snapping to a grid, randomising); triggers
  (a MIDI note restarting the shape, MIDI switching between stored shapes, a transient in the track or a
  side-chain restarting it); an envelope follower, which is not a trigger; stored shapes and presets.
- OneShot: Warp and its modes, fitting a sample to a number of bars, halving and doubling; what Slice
  really does (by transient with a sensitivity, by beat, by equal regions, by hand; moving and deleting
  slices; mono, poly and through); separate amplitude, filter and pitch envelopes; the filter's circuits,
  slopes and drive; the LFO's attack, retrigger, offset and key tracking; glide and portamento; pan and
  spread; reverse and crop. Note: OneShot's card shows Retrig and Gate, not glide.
- Sampler: a real zone editor (key, velocity and sample-select zones with fades; the layer list and its
  commands; round robin); what belongs to one sample (root key, tuning, level, pan, start and end,
  sustain and release loops, crossfade, reverse) against what is global; the pitch and filter envelopes
  and the auxiliary one; three LFOs with destinations; the modulation oscillator; the filter's circuits
  and its shaper; MIDI routing and MPE.

## What it is for

Win turns every `missing` and `part` row that the plan does not already carry into a plan row, and the
cards gain controls as the devices seat builds them. So for each such row say, in a last column or a
note, which `docs/PLAN.md` row already covers it (search the DEV rows), or "no row".

ADI behaves as Live 12 unless Live lacks the ability; what ADI adds, it adds beside Live's way.
