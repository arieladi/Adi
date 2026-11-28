# held-devices — the devices lane's unlanded work, gathered into one chain

**This chain lands only when the director says so.** Nobody merges `held/devices-chain` before his word.

- Chain: `held/devices-chain`. Log: this file, on `held/held-devices-log`. No pull request is open.
- Base: `origin/land/basics-1128c` while it is not on main; origin/main once it is. The base is merged into
  the chain before each push.
- Proof: the build PC's slot 14 (MSVC Debug, warnings as errors, `ctest -LE performance` with the
  acceptance suite; then the window suites and the app).
- A stranger continues from here: read "Where it stands" first, then the step you are on.

## Where it stands

Round 1 (28 November): step 1 (the survey) done. Step 2 (the chain) done: all seven stack tops merged,
the cut point is 578c95198. Step 3: core passes on the whole chain (318/318); the `all` check
(core and the window suites with the app) is running in slot 14.

## For the director

Each question has a one-line reply. Nothing here lands until you answer A.

- **A. Land the chain, and how much of it?**
  - A1: up to the cut point, 578c95198: the 25 new effects, Hybrid Reverb's cost, the Pd leak's other
    half, the two contracts.
  - A2: the whole chain, 086864eee: A1, plus the Pd note output (pd-midi-out) and the `.amxd`
    translator's steps 3 to 10.
  - A3: not yet; keep it held and merged up to the base.
  - **Recommendation: A1, after 3 December, as the lane's log asks.** Before it lands, one Linux and
    macOS CI run of the cut point (a workflow_dispatch, or the landing window's own run): this lane's
    seat had no GCC or Clang, none of these 87 commits ever ran in CI, and the DF-E77 sign warning
    shows that kind of finding is likely. One reply: "A1", "A2" or "A3".
- **B. The `.amxd` translator, steps 3 to 10 (`devices/amxd-audit`).** ADR-0248 d8 stops DEV-16 at step
  2 until DEV-10 (your own Pd patches as devices) exists, and DEV-10 is an M7 row not yet built. The
  steps are finished and audited, and they build and pass with the rest.
  - B1: keep your ruling: they wait for DEV-10 (they stay out under A1).
  - B2: lift it for these steps: they land with A2.
  - **Recommendation: B1.** A translator whose output cannot yet be loaded as a device gives you
    nothing to use. One reply: "B1" or "B2".
- **C. The Pd note output (`devices/pd-midi-out`).** A Pd patch's MIDI comes back on its frame, and a
  patch adds no delay when the buffer is a multiple of 64 (DEV-13, DEV-14 step 1). Nothing reads either
  yet: the engine's side (DF-E40, DEV-17, DEV-18) is not built.
  - C1: it waits for the engine's side (out under A1).
  - C2: it lands now as unused code (with A2).
  - **Recommendation: C1**, as its seat asks. One reply: "C1" or "C2".

The lane's ONE list of Live readings (readings 17 and 19 to 44, one per new effect) stays the
devices seat's, in `collab/devices.md` on `devices/log`: each new effect is "built, not verified" until
its reading comes back.

## Step 1 — the survey (28 November)

`git branch -r --no-merged origin/main`, those under `origin/devices/`, read against the seat's own log
(`devices/log`, `collab/devices.md`, entries of 22 to 25 November). 41 branches. `git cherry origin/main`
finds none of their commits on main under the same patch; one change is on main by another route (marked).

The lane's work sits in seven stack tops. Every other branch is an ancestor of one of them.

| Branch | Head | Top of which stack | State |
|---|---|---|---|
| devices/compressor-core | 431bc637 | spectral-resonator | contained in spectral-resonator |
| devices/compressor | 3ae7ab4c | spectral-resonator | contained in spectral-resonator |
| devices/glue-compressor | 98e479db | spectral-resonator | contained in spectral-resonator |
| devices/live-gate | c76d2952 | spectral-resonator | contained in spectral-resonator |
| devices/live-limiter | ac6c8718 | spectral-resonator | contained in spectral-resonator |
| devices/align-delay | caadc5ef | spectral-resonator | contained in spectral-resonator |
| devices/lfo-kernel | 46e5b70e | spectral-resonator | contained in spectral-resonator |
| devices/shaper-mod-kernel | 4c8ad6df | spectral-resonator | contained in spectral-resonator |
| devices/auto-pan-tremolo | 54eab5aa | spectral-resonator | contained in spectral-resonator |
| devices/channel-eq | e5141d7b | spectral-resonator | contained in spectral-resonator |
| devices/eq-three | c97c2582 | spectral-resonator | contained in spectral-resonator |
| devices/tuner | 9e314efa | spectral-resonator | contained in spectral-resonator |
| devices/erosion | 10b858ca | spectral-resonator | contained in spectral-resonator |
| devices/vinyl-distortion | b653df0b | spectral-resonator | contained in spectral-resonator |
| devices/filter-delay | 19835445 | spectral-resonator | contained in spectral-resonator |
| devices/dj-filter | 20b2cabf | spectral-resonator | contained in spectral-resonator |
| devices/auto-shift | 1f615326 | spectral-resonator | contained in spectral-resonator |
| devices/spectrum-device | 037c5c91 | spectral-resonator | contained in spectral-resonator |
| devices/live-amp | 0c8e39e4 | spectral-resonator | contained in spectral-resonator |
| devices/live-cabinet | 3366bfe6 | spectral-resonator | contained in spectral-resonator |
| devices/corpus | 2b134cc8 | spectral-resonator | contained in spectral-resonator |
| devices/drum-buss | 5f6c96db | spectral-resonator | contained in spectral-resonator |
| devices/dynamic-tube | 5daf45ba | spectral-resonator | contained in spectral-resonator |
| devices/grain-delay | 9ac311f9 | spectral-resonator | contained in spectral-resonator |
| devices/live-pedal | 5fe14707 | spectral-resonator | contained in spectral-resonator |
| devices/roar | e7dbc32e | spectral-resonator | contained in spectral-resonator |
| devices/spectral-time | a897fef8 | spectral-resonator | contained in spectral-resonator |
| **devices/spectral-resonator** | **920177d9** | itself (the DEV-22 effects stack, 87 commits) | **to take** |
| **devices/beat-repeat-contract** | **e243ab13** | itself (3 commits, docs only) | **to take** |
| **devices/hybrid-reverb-nupc** | **fb1c7e9a** | itself (2 commits and 2 merges of denormal-tail, which is on main) | **to take** |
| **devices/ext-audio-effect-contract** | **4054d859** | itself (1 commit, docs only) | **to take** |
| **devices/df-e67b-canvas-args** | **aab2e60c** | itself (1 commit) | **to take, in part**: main has DF-E67 (b) by another route (window-fn's 21937f3b, landed through `land/seats-1125`), which frees the pending arguments at teardown. This branch adds the other half (no copy stored for no arguments) and the forty-engine test. Taken with main's overlay kept. |
| devices/pd-block-quantum | b8ff1a6e | pd-midi-out | contained in pd-midi-out |
| **devices/pd-midi-out** | **52782524** | itself (DEV-13 Q-K2, DEV-14 step 1) | **held by its dependency**: the seat's own log (25 Nov, item 5) says it waits for the engine's DF-E40 consumer (`segmentQuantum` is read by nothing on the base) and DEV-17/18. Taken after the chain's cut point, so that it is proved with the rest and can be left out. |
| devices/amxd-translator | 74aefaff | amxd-audit | contained in amxd-audit |
| devices/amxd-bpatcher | 97037b35 | amxd-audit | contained in amxd-audit |
| devices/amxd-cycle-phase | 0b89f7b2 | amxd-audit | contained in amxd-audit |
| devices/amxd-line-ends | 539cce65 | amxd-audit | contained in amxd-audit |
| devices/amxd-modulo-biquad | 22fd659f | amxd-audit | contained in amxd-audit |
| **devices/amxd-audit** | **9fc81e1d** | itself (the `.amxd` translator, steps 3 to 10) | **held by a ruling**: ADR-0248 d8, "DEV-16 stops at step 2 until DEV-10 exists"; DEV-10 (user Pd patches as devices) is an M7 row, not built. Taken after the cut point, as pd-midi-out. |
| devices/log | b7c9e155 | the seat's log | not work: win lands the seat's log with its work at landing time (collab/README). Not merged into the chain. |

Also unmerged, not under `devices/` but the lane's: `land/devices-1123` (94a2c796) is **already on main by
another route**. Its four seat merges (perf-sampler, perf-debug-timeout, denormal-tail, shifter-nan) start
from the seat head 83f71d5e, which `land/devices-1123b` (#732) carried to main; what is left on it is
the stacking merge of `land/mixview-1123`, superseded by `-1123b`. Not taken.

Trial merges onto the base (`git merge-tree`): hybrid-reverb-nupc, beat-repeat-contract and
ext-audio-effect-contract merge clean. Conflicts: spectral-resonator in `tests/test_builtin_devices.cpp`;
df-e67b-canvas-args in `src/juce/pd_loader_fix.cmake`; pd-midi-out in `src/juce/pd_engine.cpp`;
amxd-audit in `src/juce/pd_amxd.cpp`.

## Step 2 — the chain

The order, oldest stack first (the effects stack's root, compressor-core, is of 19 November):
1. `devices/spectral-resonator` 920177d9
2. `devices/beat-repeat-contract` e243ab13
3. `devices/hybrid-reverb-nupc` fb1c7e9a
4. `devices/ext-audio-effect-contract` 4054d859
5. `devices/df-e67b-canvas-args` aab2e60c
6. **The cut point.** Everything above is the lane's finished work with no ruling against it.
7. `devices/pd-midi-out` 52782524 (held by its dependency)
8. `devices/amxd-audit` 9fc81e1d (held by ADR-0248 d8)

| # | Merged | Chain head after | Collisions and how they were resolved |
|---|---|---|---|
| 1 | devices/spectral-resonator 920177d9 | a374b5afa | `tests/test_builtin_devices.cpp`, `main()` only: the base's calls (hotSwapPlacement, moveDeviceRange, duplicatePrepareKeepsBuiltinState, builtinInstrumentPlacement) and the branch's `gateLatency()` and `limiterDevice()` (under `ADI_WITH_PD`) all kept. The catalogue merged by itself: 21 entries on the base, 25 added, 46; the test's count follows. |
| 2 | devices/beat-repeat-contract e243ab13 | 5cf027692 | none (`pd/devices/Beat Repeat.md`) |
| 3 | devices/hybrid-reverb-nupc fb1c7e9a | 991e038aa | none |
| 4 | devices/ext-audio-effect-contract 4054d859 | baca8c0dc | none (`pd/devices/External Audio Effect.md`) |
| 5 | devices/df-e67b-canvas-args aab2e60c | **578c95198 = the cut point** | `src/juce/pd_loader_fix.cmake`: main's teardown free (window-fn, 21937f3b) kept; the branch's other half, no copy stored for no arguments, added after it; the branch's own teardown hunk dropped as a duplicate of main's. Its test `testAbstractionArgumentsLeaveNothingBehind` kept. |
| 7 | devices/pd-midi-out 52782524 | 0d2b0840d | `src/juce/pd_engine.cpp`, `LibPdEngine::prepare`'s native path: the base's typed-MIDI scratch kept whole; the branch's atomic store of `adapterLatency_` in place of the plain assignment (the branch makes the member atomic). |
| 8 | devices/amxd-audit 9fc81e1d | 086864eee | `src/juce/pd_amxd.cpp`: the base's DF-E89 fix (push_back for the number box's one-element lists, GCC 12 -O3 -Warray-bounds) met the branch's rewrite of the number box as `numberBox()`. The branch's function taken, with DF-E89's pattern applied to its one-element inlets and outlets. |

The validators pass on a374b5afa (schema, ops, Pd, CI, parity).

## Step 3 — the proof

| Check | Chain head | Result |
|---|---|---|
| core | 578c95198 (the cut point) | build OK (validators, MSVC Debug /WX); CTest 314/318. The four failures are none of the lane's: `adi_agent_log_tests` (three child writers), `adi_library_tests` (timeout, 120 s), `adi_link_playback_tests` (owner preparation), `adi_ui_document_tests` ("opening yields to the owner between slow plug-in loads"). All four pass in the next row, on a superset of this tree, so they are the shared PC's load, not the merge. |
| core | 086864eee (the whole chain) | **build OK; CTest 318/318.** |

## Step 4 — the account

### What the chain adds that a user would notice (up to the cut point)

Twenty-five more of Live 12's audio effects in the Browser's Audio Effects, each one loadable on a track
with its controls in today's look, each "built, not verified" until its Live reading (the catalogue goes
from 21 entries to 46):
- **Dynamics:** Compressor; Glue Compressor; Gate (its lookahead reported as latency, kept when
  bypassed); Limiter (Stereo, L/R and M/S; Standard, Soft Clip and True Peak); Drum Buss.
- **EQ & Filters:** Channel EQ; EQ Three; DJ Filter.
- **Drive & Color:** Amp; Cabinet; Dynamic Tube; Pedal; Roar; Erosion; Vinyl Distortion.
- **Delay & Loop:** Filter Delay; Grain Delay; Spectral Time.
- **Reverb & Resonance:** Corpus; Spectral Resonator.
- **Pitch & Modulation:** Auto Pan-Tremolo; Auto Shift (on the Tuner's pitch detector and the Shifter).
- **Utilities:** Align Delay; Tuner; Spectrum (it loads and passes the signal; its graph needs the Mix
  View seat's tap and worker, per `pd/devices/Spectrum.md`, so the panel shows no spectrum yet).

Also in it:
- Hybrid Reverb with a long impulse costs less at small buffers: it convolves in non-uniform partitions,
  under the common 15 % bound at 64 frames.
- ADI's own brickwall holds a lowered Ceiling from the next sample (Live's Limiter is built on it). This
  touches the export's brickwall too; `adi_dsp_tests` pass unchanged.
- Under the hood: the Pd tier stores no copy of an abstraction's empty arguments (the other half of
  DF-E67 (b)); the LFO and Shaper modulator kernels (DEV-07's device side), which nothing uses until the
  engine's modulation is built; Beat Repeat's and External Audio Effect's written contracts (no device).

After the cut point (not recommended now, see B and C): a Pd patch's MIDI output with the host's reset
bang, no added delay on a 64-sample grid, and the `.amxd` translator's steps 3 to 10.

### The devices the director named in SC-456

- **Spectrum:** in the chain (the device; its graph waits on the Mix View seat, above).
- **The modulators:** only their kernels (LFO, Shaper). There is no modulator a user can load: the
  routes, the evaluator and the modulator nodes are the engine's DEV-07, right after 3 December
  (ADR-0249 d3, d8).
- **More of Bitwig's devices:** none in the chain; the lane built none. ADR-0249 d5: Bitwig's
  modulators are not taken now, and win owes an item-by-item list after 3 December (DF-E42); SC-456
  asks for the list against the plan first.

### Left out, and why

- `devices/log`: the seat's log, not work; win lands it with the chain.
- `land/devices-1123`: already on main through #732.
- Looper: never built; the seat keeps it for the engine's recording and transport rows.
- Beat Repeat's core: not built (blocked on reading 18 and the shared two-head reader); only its
  contract is here.

### Owed before landing (not the director's)

- A Linux and macOS CI run (GCC/Clang, -Wconversion and friends) of the head to land.
- The seat's notes that ride with the stack: the Spectrum tap (Mix View seat); reading 17 and 19 to 44.
