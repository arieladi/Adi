# devices — standing queue, 22 November 2026

This is the whole queue to work until your usage limit. It replaces every earlier list win gave this lane.

## How to work

- Fetch main first (`git fetch origin`), then read the top entry of `collab/win.md`, your own log, and the entries
  addressed to you on `clones/log` and `engine/log`.
- Work the items top to bottom. One branch per item, `devices/<topic>`, cut from `origin/main` unless the item says
  to stack.
- Test first where the item names a test. Build on MSVC (/W4 /WX), run the tests, push.
- Log the entry in `collab/devices.md` on `devices/log`, end it with "PR requested", and start the next item in the
  same turn.
- An item blocked on win, another seat or a decision: park it with one line in your log and take the next.
- A question for the director goes under "FOR THE DIRECTOR" at the top of your log, with your recommendation. Then
  continue.
- Stay in your own paths (`collab/README.md`): `src/adi/dsp/**`, `src/adi/pd_builtins/**`, `cmake/devices/**`,
  `pd/**`, `src/juce/pd_*`, and the lent parts of `src/adi/builtin_catalog.cpp` and `src/juce/builtin_host.cpp`.
  `adi_shaper*` files are the clones seat's wherever they sit. Put needs in other seats' paths in your log's needs
  table.
- When this list is empty, take the next unbuilt row of your lane in `docs/PLAN.md` in milestone order. Work meant
  for after 3 December goes on branches marked "lands after 3 December" in the log entry.
- Never stop because "nothing names this lane". Stop only at your usage limit or a broken environment.

## Queue

Items 1 to 4 are before 3 December (ADR-0248 d2). The rest land after 3 December.

1. **perf-sampler: time the Sampler in the realtime suite**
   The suite measures only catalogue entries with a Pd canvas, so the Sampler is skipped. #706's review asked for
   this fix. Time the native-only instruments too, with 32 slots filled.
   Files: `tests/test_builtin_realtime.cpp`, `cmake/devices/realtime.cmake`.
   Test: `adi_builtin_realtime_tests` (`-L performance`) lists the Sampler at 64 and 128 frames, 0 allocations,
   p99 under 15 %. A plant that drops the Sampler from the list fails.
   Stack on `devices/perf-pass` (in #706) until #706 lands.

2. **perf-debug-timeout: the Debug run fits its CTest timeout**
   The suite takes about 37 min in Debug, over its 900 s TIMEOUT (the other fix #706's review asked for). In Debug,
   run a short allocation-only pass; keep the timing for optimised builds.
   Files: as item 1.
   Test: `ctest -L performance` finishes in Debug under 900 s with the allocation checks intact; RelWithDebInfo still
   runs its 156 checks.
   Same stack as item 1.

3. **denormal-tail: every built-in core settles after silence**
   The callback already flushes denormals (DF-E11, on main as `ScopedFlushDenormals`). This item is per-core
   robustness: the Compressor and Glue cores showed the stuck-denormal defect, so audit every built-in core for the
   same pattern and settle each recursive state on exact 0.
   Files: `src/adi/dsp/**` per core, `tests/test_builtin_realtime.cpp`. Leave `adi_shaper*` alone (clones' paths).
   Test: a signal-then-60 s-silence pass run under the callback's own flush scope; silence-tail p99 no worse than
   the signal pass for every built-in at 64 and 128 frames; per-core checks that the states reach 0; plants caught.
   Stack on `devices/perf-pass`.

4. **DF-E39: port the MIDI effect kernels** (blocked: waits on the engine's DEV-17 and DEV-18 reaching main; skip to
   the next)
   Port #207's kernels (MF1, from `7f421f4d` with its listed fixes) onto DEV-17 and DEV-18, under DEV-19. No second
   transform path meanwhile.

5. **shaper-scope-feed: ADI-Shaper's rack panel shows its live scope** (blocked: `adi_shaper_scope.hpp` exists only
   on the clones panel stack held in #708, and the `builtin_host.cpp` loan must be widened by win; skip to the next)
   When it clears: `NativeDevice` forwards the core's `shaperScope()` through `NativePdDevice`, and `BuiltinDevice`
   implements the scope-source interface. You write only `native_devices.*` and the `builtin_host.cpp` hunk.
   Test: a registered ADI-Shaper in a Session yields non-empty input and output frames through `resolveDevice()`;
   clones' rack-editor capture shows a trace and moving meters.

6. **time-locate-revision: a locate to the same position reaches ADI-Shaper's Time**
   Pass `timelineSample` and `transportRevision` (`TransportInfo` in `graph.hpp`) through the native adapter's
   position path. Your change is `native_devices.*` only; the handler side (`adi_shaper_transport.hpp`) is clones'.
   Test: clones' same-position Time regression passes on the native path and fails before the change. If clones has
   not supplied it yet, test the forwarded fields directly and ask for the regression in your needs table.

7. **glue-device: Glue Compressor as a loadable built-in** (lands after 3 December)
   External, canonical canvas, native dispatch, catalogue entry. Parameter ids fixed now; ranges stay ADI's until
   reading 19.
   Files: `pd_builtins/glue_compressor_tilde.cpp`, `pd/devices/Glue Compressor.pd`, `native_devices.cpp`,
   `cmake/devices/glue_compressor.cmake`, `builtin_catalog.cpp` (lent).
   Test: native vs Pd parity, max error 0 (`adi_pd_native_devices_tests`); `adi_builtin_devices_tests` count +1;
   object walk; CTest at PD_FLOATSIZE 32 and 64.
   On `devices/glue-compressor`; stack on `devices/compressor` for the catalogue order.

8. **gate-device: Gate as a loadable built-in** (lands after 3 December)
   External, canvas, dispatch, catalogue; reports the Lookahead latency.
   Files: `pd_builtins/live_gate_tilde.cpp`, `pd/devices/Gate.pd`, `native_devices.cpp`,
   `cmake/devices/live_gate.cmake`, `builtin_catalog.cpp` (lent).
   Test: parity 0; latency kept while bypassed; `adi_builtin_devices_tests`; CTest 32 and 64.
   On `devices/live-gate`, stacked after item 7.

9. **limiter-device: Live's Limiter as a loadable built-in** (lands after 3 December)
   Standard, Soft Clip and True Peak modes (the LD5 / DEV-22 link).
   Files: `pd_builtins/live_limiter_tilde.cpp`, `pd/devices/Limiter.pd`, `native_devices.cpp`,
   `cmake/devices/live_limiter.cmake`, `builtin_catalog.cpp` (lent).
   Test: parity 0; latency 1.5, 3 and 6 ms (+10 samples in True Peak) reported; ceiling held in every link;
   `adi_live_limiter_tests` (58) still pass.
   On `devices/live-limiter` (which sits on `limiter-offline` in #706).

10. **hybrid-reverb-nupc: Hybrid Reverb's steady cost** (lands after 3 December)
    p99 is 17-20 % with a 5 s IR at 64 frames. Move to non-uniform partitioned convolution.
    Files: `src/adi/dsp/hybrid_reverb.*`, `tests/test_hybrid_reverb*.cpp`.
    Test: exact against direct convolution; bit-identical across block sizes; Pd parity; realtime p99 under the
    common 15 %, and its 25 % exception removed.
    Stack on `devices/perf-pass`.

11. **lfo-kernel: Live's LFO modulator kernel** (lands after 3 December; ADR-0249 d8)
    DEV-07's device side, core only, built not verified. The engine's DEV-07 evaluator wires it later.
    Files: `src/adi/dsp/lfo_modulator.*`, `cmake/devices/`, tests.
    Test: waveforms, sync and free rate, phase and offset exact at block sizes 32 to 4096; 0 allocations; plants.

12. **shaper-mod-kernel: Live's Shaper modulator kernel** (lands after 3 December; ADR-0249 d8)
    Breakpoint envelope. Its default shape is ADI's until reading 11.
    Files: `src/adi/dsp/shaper_modulator.*`, tests.
    Test: breakpoint curves exact; sync rates; block-size invariance; no allocation.

13. **beat-repeat-core: Beat Repeat core and device** (blocked: reading 18 and the contract branch
    `devices/beat-repeat-contract` landing; skip to the next)
    Per the contract and ADR-0249 d7. Files: `dsp/beat_repeat.*`, `pd_builtins`, `pd/devices/Beat Repeat.pd`,
    catalogue.
    Test: ADR-0249 d7's tests: marker frame oracle at 44.1, 48 and 96 kHz; irregular blocks; Chance 0 and 100 %;
    locate and wrap; no allocation.

14. **two-head-reader-shifter: the Shifter on the shared two-head reader** (blocked: clones' extraction, step 1 of
    the agreement, exists nowhere yet; skip to the next)
    The Shifter takes `dsp/two_head_reader.hpp`, bit-exact. If clones hands step 1 over, write it yourself.
    Test: every Shifter test bit-exact; clones' cross-tests pass.

15. **align-delay: Align Delay** (lands after 3 December)
    Core plus device, built not verified; add its reading to the one list in your log.
    Files: `dsp/align_delay.*`, `pd_builtins`, `pd/devices`, `cmake/devices`.
    Test: delay exact in ms, samples and distance; latency reported; parity 0. After the item 7-9 stack.

16. **auto-pan-tremolo: Auto Pan-Tremolo** (lands after 3 December)
    Live 12.4's uid and browser name; core plus device; may reuse item 11's kernel.
    Files: `dsp/auto_pan.*`, `pd_builtins`, `pd/devices`, `cmake/devices`.
    Test: LFO shapes, phase and spin, sync exact, block-invariant; parity 0.

17. **channel-eq: Channel EQ** (lands after 3 December)
    Core plus device. Files: `dsp/channel_eq.*`, `pd_builtins`, `pd/devices`, `cmake/devices`.
    Test: band responses within a stated tolerance; parity 0.

18. **eq-three: EQ Three** (lands after 3 December)
    Core plus device, 24 and 48 dB/oct kill bands. Files: `dsp/eq_three.*`, `pd_builtins`, `pd/devices`,
    `cmake/devices`.
    Test: crossover sums; kill depth; parity 0.

19. **tuner: Tuner** (lands after 3 December)
    Core with a pitch detector that Auto Shift shares later. Files: `dsp/pitch_detector.*`, `dsp/tuner.*`,
    `pd_builtins`, `pd/devices`.
    Test: pitch within 1 cent on sines from 40 Hz to 4 kHz; no allocation.

20. **erosion-vinyl-filter-delay: Erosion, Vinyl Distortion, Filter Delay** (lands after 3 December)
    Three cores, one branch each, built not verified. Files: `dsp/<device>.*`, `pd_builtins`, `pd/devices`,
    `cmake/devices`.
    Test: per-device unit tests; parity 0; no allocation.

21. **ext-audio-effect-contract: External Audio Effect contract** (lands after 3 December)
    A document: this lane's canvas and panel contract. The routing is the engine's (MX-24, DEV-05 for the build).
    File: `pd/devices/External Audio Effect.md`.
    Done when it names the controls and ids, the latency fields and the engine hand-off.

22. **dj-filter: DJ Filter on Auto Filter's cores** (lands after 3 December)
    Files: `dsp/auto_filter.*`, `pd/devices`.
    Test: response equal to Auto Filter's core at the matched settings; parity 0.

## Waiting on win

Do not redo these.

- #706 (`land/devices-1122`): `sampler-loadable`, `df-e57` (DF-E57 and DF-E61), `pd-exact-position`, `dev09-gaps`,
  `loudness-robust`, `limiter-offline`, `loudness-reference`, `dsp-provenance`, `loudness-bench`, `pd-ar8`,
  `pd-fuzz`, `loudness-ffmpeg`, `perf-pass`, `devices/log`.
- After 3 December: `devices/compressor-core`, `devices/compressor`, `devices/glue-compressor`, `devices/live-gate`,
  `devices/live-limiter`, `devices/beat-repeat-contract`, `devices/pd-block-quantum`, `devices/pd-midi-out`
  (the last two need the engine's DF-E40).
- `devices/amxd-translator` steps 3-4: DEV-16 is held at step 2 until DEV-10.
- The register: DF-E36, DF-E37, DF-E46, DF-E50 and DF-E20 (d)(e) are on main (#688, #697) and move to Done; DF-E57
  and DF-E61 close with #706.
- The `builtin_host.cpp` loan widened to the per-instance scope-feed forwarding (item 5).
- CI (`.github`): the ASan leg (AR18); the libebur128 v1.2.6 pin and its oracle leg; the ffmpeg leg with
  `ADI_REQUIRE_ORACLES=1`; the libebur128 EXTERNAL-CODE row; PLAN LD1's wording; the ADR-0188 d3 amendment for the
  two true-peak fields.
- Win's rulings: AR8's cross-instance dropout (about 10 ms per large Pd open), recorded or an overlay funded; whether
  runaway `[until]` loops fall under "a patch cannot stall ADI"; the definition of the "LRA not yet stable" flag.

## Needs from other seats

- clones: the scope-source interface (`adi_shaper_scope.hpp`) landed or handed over (item 5); the same-position Time
  regression (item 6); the two-head reader extraction, step 1, or a hand-over (item 14).
- engine: DEV-17 and DEV-18 on main (item 4); DF-E40 (`segmentQuantum`) for `pd-block-quantum` and `pd-midi-out`;
  a profile of the empty project's 90-150 us per 64-frame callback; N13 (Main's meter) and Auto Gain Stage rewired
  onto `dsp::LoudnessMeter`; a check of #706's `snapshot.cpp` hunk.
- engine-win: the export half of EX4 / LD5 (export normalization on `LoudnessMeter`, `finish()` instead of padding).
- window: whether the Devices panel shows `openError()` for a patch device that failed to open (DF-E20 e).

## For the director

- Live readings 1-21 in one sitting: 18 unblocks Beat Repeat, 17 and 19-21 the Compressor, Glue, Gate and Limiter
  parity, 11 the Shaper kernel's default.
- EX4: should the export brickwall read finer than BS.1770's 4x (more CPU), or keep the Annex reading with the
  -1 dBTP margin?
- A Release run of `adi_block_benchmark --loudness` on your PC, CSV back (ADR-0203 d7's measured figure).
