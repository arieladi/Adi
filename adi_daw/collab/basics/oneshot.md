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

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

## For the director

(nothing yet)

## Next

Order of the mission: separate amplitude, filter and pitch envelopes; the filter's circuits, slopes
and drive; the LFO's attack, retrigger, offset and key tracking; glide and portamento; pan and spread;
reverse and crop; then Slice (by transient with a sensitivity, by beat, by equal regions, by hand;
moving and deleting slices; Mono, Poly and Thru). Warp last, and only through the engine's own
stretcher if its interface is on the base; otherwise blocked on the stretcher seat.
