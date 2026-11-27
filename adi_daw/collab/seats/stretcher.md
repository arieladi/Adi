Codex took the stretcher seat on 27 November 2026.

| Item | Change | Proof | State |
| --- | --- | --- | --- |
| TM-WT-04 | Streams Beats, Texture and Re-Pitch from bounded PCM pages on the Arrangement clock. Integrates linear tempo ramps per loop lap. Primes a worker-side R3 voice and declares its physical latency on the existing track source. | The 120→140 BPM fixture missed the grid by up to 530 ms on `origin/main`. With the change, Beats hit all 16 landmarks exactly at 48 kHz. Float64 Re-Pitch missed by at most one sample at 44.1 kHz. Texture's default grain and fluctuation moved its peaks by at most 781 samples at 48 kHz (16.271 ms). Typed Beats hit every landmark and heard the new tempo in the first callback. The callback audit counted zero allocations, releases, mutex calls and file I/O. Steady fixtures for all three native modes matched the prepared render within 0.002 peak and 0.0001 RMS. R3 reported 2,048 frames at 48 kHz, with no offset against the compensated dry landmark before the ramp. The repeated R3 ramp and loop probe measured 32 samples maximum error across 32 beats. | Engine slice on `seats/stretcher-chain`. Shared-clock integration and the steady R3 handover remain open. |

Shared implementation changes are confined to `src/adi/engine/clip_playback.cpp` and `CMakeLists.txt`. The former wires source demand, page publication, the track's physical delay and clip diagnostics. The latter registers the engines and tests. `docs/ENGINE-WARP.md` records the playback contract. No format, op or register reservation is needed.

`c3f023f8f` passed all 281 CTest suites and all five validators under GCC 14.2 Debug with `ADI_WERROR=ON`. Test binaries were linked with debug symbols stripped to fit the workspace disk. The first full run caught a source-phase change after nanosecond placement splits. The follow-up commit restores the canonical musical origin, and all 445 placement checks pass, including sample-exact Redo. In the Session latency proof the dry track received exactly 2,048 frames of delay. Complex was 22 frames (0.458 ms) from the dry landmark during the ramp and after stop/seek. The silent middle second measured zero RMS.

## For win

- When TM-TT-01 lands, connect the stream players to its shared linear and Bézier tempo clock. The shared implementation is already on `engine/tm-tt01-tempo-ramps-current` at `7c37df259`. Its continuous projection scans tempo events, so compile segment boundaries for per-sample lookup when reconciling the clocks. Transport, MIDI, automation and export bounds also need that shared clock for ramped projects.
- The existing steady R3 path preserves exact prepared PCM for steady-tempo editing, including trimmed and split loops. Moving these cases to streamed R3 and fully integrating live tempo changes across the R3 modes needs an equivalence rule for seek and loop history. Pinned R3 resets allocate, so any change must preserve worker ownership of the DSP.

## Readings owed

- Live 12 describes Beats' zero crossing and envelope, Texture's grain and fluctuation, and Re-Pitch's tape-speed behavior. The manual leaves exact slice fades, grain jitter and mode latency unquantified. Keep the named existing Beats attack and envelope constants and the captured Texture seed until the TM-WT-03 listening work establishes equivalents. The recorded Live 12.4 audit has Beats defaulting to Back-and-Forth. [Live 12 warp modes](https://www.ableton.com/en/live-manual/12/audio-clips-tempo-and-warping/#warping-modes)
- M2-A-WARP 7 still needs a Live 12.4 reference for playback, offline render and re-open, including R3 onset and latency behavior. These measurements use generated fixtures. [Clip View audit](https://github.com/arieladi/Adi/blob/main/adi_daw/collab/window/live-clip-view-audit-2026-11-16.md)
