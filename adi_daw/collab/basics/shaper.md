# ADI-Shaper against its model: the shaper queue's log

**This chain lands only when the director says.** The chain is `basics/shaper-chain`. It gathers the clones
seat's unlanded ADI-Shaper branches, most of which that seat marked "lands after 3 December", and then builds
what the parity list (SC-481) still shows missing. Nothing here opens a pull request.

Row: SC-481 (`docs/review/device-parity-2026-11-28.md` on main), grade A. Plan row: DEV-31. ADR-0267
decisions 7 and 8 (branch `win/adr-0267`).

## Where the work stands (read this first if you continue it)

Base: `origin/main` since round 2 (`land/basics-1128c` merged into main on 28 November). Build place: slot 7.

**Done in round 2 (28 November 2026, a second session):**

1. The first slot check of the chain (at `88b23c576`, core, all suites) failed two shaper suites; both are
   fixed (`697369409`): the hold follower's oracle modelled the old three-way split, and a Pd probe named an
   id the Sweep port had filled. Three other suites failed in that run and are not shaper code (see "Other
   suites" below).
2. The four Crush branches are ported, one commit each: bias (`599d639ea`, made by round 1 and left
   unpushed in its worktree; pushed now), treble (`4a5190b7d`), jitter (`0e90b9dfd`), prefilter
   (`292fbc558`). Each keeps today's samples when its control is at its default.
3. The Pd test's descriptor walks were extended for everything gathered (`a5a105670`, `e96e6fdaf`).
4. Score: rows 57, 59, 60 and 61 move to "has". On the chain **43 has / 48 part / 84 missing / 3 out**.

**Done in round 1 (28 November 2026):**

1. Step 1, the survey: all 78 `clones/shaper-*` and `clones/adi-shaper-*` branches are classed below.
2. Step 2, in part: the chain holds the whole DSP stack, the Time locate test, the Sweep stereo offset and
   the Crush hold follower. See "The chain". Four Crush branches and the panel stack are still outside.
3. Step 3, a first score: 178 rows, on main 21 has / 24 part / 130 missing / 3 out; on the chain today
   **39 has / 48 part / 88 missing / 3 out**. The table is at the foot. It is scored from the controls and the
   code the chain holds, not from listening.

**Half done, and where:**

- The chain at `e96e6fdaf` waits for slot 7 (core, filter `shaper_pd`). At `a5a105670` every shaper suite passed (`adi_shaper_tests`, `adi_shaper_crush_tests` with all four ports' fixtures, Time, Sweep, Filter, Drive, Room, follower filter, session) except `adi_shaper_pd_tests`, which stops at its first failed check and stopped at a hard-coded count fixed in `e96e6fdaf`. After it passes: a full `all` check, then the panel join (item 2 below).

**Other suites.** The full core run at `88b23c576` also failed `adi_project_files_tests` ("a busy external
writer refuses promptly..."), `adi_agent_log_tests` (three child writers) and `adi_link_playback_tests` (two
tempo checks). None touches shaper code and the chain changes no file they build from; they are timing
checks that ran while eight suites shared the build PC. Re-run them alone before calling them flaky.

**Not started. Do these next, in this order:**

1. (Done in round 2, kept for the record.) Port the four Crush branches the DSP stack left out: `shaper-crush-bias` (`5f1d6c4a2`, controls 865-867),
   `shaper-crush-treble` (`edc8ee1f2`, 868-870), `shaper-crush-jitter` (`0b6ca57ef`, 871-873),
   `shaper-crush-prefilter` (`f05454619`, 874-876). Each is one commit. **They are not append-only ports.**
   All four were written on the Crush kernel of 22 November, before the four Crush repairs that have since
   landed (resolution crossing, endpoint, hold grid, clock precision), and all four rewrite the same lines of
   `adi_shaper_crush.cpp`; they were never combined with each other either. So each needs a real merge of
   `quantize()` and `hold()`, then a core check, one at a time. What a trial pick of the bias branch showed:
   the base's `quantize(sample, previousLevels, codeScale)` keeps `previousCode_` and scales the earlier step
   when the resolution moves; the branch replaced that body with `integratedCode(before, now) - zeroCode`,
   where `zeroCode` is the same staircase driven by the bias and dither alone. For a resolution that does not
   move the two forms give the same number (the base's `priorCode` is then `floor(before + .5)`), so the join
   is: keep the base's body, add the branch's `biasInput_` into `perturbation`, keep `previousPerturbation_`,
   and subtract `zeroCode` from the base's mean. With bias 0 the result must be bit-identical to today: the
   existing Crush suite proves it. Under a moving resolution the bias reference is then built to the branch,
   not measured.
   For the control plumbing follow the two ports already made: ids are kept, the controls are appended at
   the end of `ADI-Shaper.pd` (a fresh column), `hasParameter` gets the range, the Pd test's descriptor
   indexes move to the end (the count is 760 now), and a test that copies an `AdiShaper` must replay its
   sends instead (the device is not copyable since Time owns its history).
2. Merge the panel stack `clones/shaper-curve-edit` at `b3d393b36`. It collides with the DSP stack in nine
   shaper files (`adi_shaper.cmake`, `ADI-Shaper.pd`, `adi_shaper.cpp`, `adi_shaper.hpp`, `adi_shaper_time.cpp`,
   `adi_shaper_time.hpp`, `test_adi_shaper.cpp`, `test_adi_shaper_pd.cpp`, `test_adi_shaper_time.cpp`) and with
   the base in three window files (`ui_devices.cpp`, `ui_main.cpp`, `test_ui_shell.cpp`). Both sides carry
   their own copy of the Time stage (the stack's is the newer: finite interpolation, stage identity, order);
   keep the stack's Time and bring the panel's wave bank (`adi_shaper_wave.hpp`, controls 2049-3072), Volume
   Trim (3073-3075) and Width Gain (3076-3078) onto it. The clones log of 25 November says the free points
   were only ever joined to the ten-stage device in a private comparison, so this join is new work. Change no
   look. Then a full check (`all`).
   What round 2 read of it (merge base with the chain `73e26e4aa`, seven commits `a801fcf93`..`b3d393b36`,
   29 files, about 9,700 lines added): besides the wave bank and the trims, the stack's `AdiShaper` has its
   own eight-stage Time (`TimeOn = CrushOn + ...`, the chain has it at 415 with ten stages), a
   `syncAtSample(quarterNotes, playing, sample, revision)` and a history tolerance on `sync`, an
   `AdiShaperScope` passed to `process`, `gain(kind, amount, const CurveBand&)` (the band now carries `trim`,
   `wetGain`, `midGain`), and `adi_shaper_corner.hpp`. New files that should come over unchanged:
   `adi_shaper_wave.hpp`, `adi_shaper_corner.hpp`, `adi_shaper_scope.hpp`, the panel sources in `src/juce/`,
   `cmake/devices/adi_shaper_panel.cmake`, and the four panel test files. The work is in `adi_shaper.hpp/.cpp`:
   take the chain's side for everything the DSP stack has, then add the wave bank, the scope tap and the
   trims, and check whether `syncAtSample` is the stack's newer transport or something the chain already
   does another way (the chain has `adi_shaper_transport.hpp` changes of its own). The Pd file: take the
   chain's and append the panel's 2049-3078 declarations as the four Crush ports did: each `adi.param` line
   and its `msg ... p <id - 1>` after the last object line (object n is file line n + 2), and the two
   connects (`n 0 n+1 0`, `n+1 0 1 0`) after the last connect. The undeclared-ID probe in
   `test_adi_shaper_pd.cpp` names 2049: change it once the bank is declared.
3. Score the list again (the note column names the branch that holds each row).
4. Step 4 of the mission. **The free curve is not to be built from nothing:** the panel stack already has it
   (`AdiShaperWaveBank`: points with x, y, a bend and a corner shape Hold / Line / Bend / SourceCorner; the
   panel has Edit, Draw, Line pen, Arc pen and S-curve pen, a marquee with group move, and the grid). What it
   lacks is the drawing operations as plain functions outside the panel (the pens live in
   `src/juce/adi_shaper_panel.cpp`), and steps, ramps, smoothing, snapping existing points and randomising.
   Build those beside the wave bank in `src/adi/dsp/`, with headless tests, then shapes held ready and
   switched by MIDI notes (rows 102 to 105), then what the score shows.
5. Pitch (rows 7 to 14) exists only in the first generation (`clones/adi-shaper-sources` and above:
   `adi_shaper_pitch.cpp`, `test_adi_shaper_pitch.cpp`) and as the clones seat's shared reader on
   `clones/two-head-reader` at `9b7cd7caa`, which its log offers to the Devices seat for review. It was not
   carried into the rebuilt device. Port it as a stage after the panel join.

## The chain

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-481 | `basics/shaper-chain` `c2ca47d20` | Merged `clones/shaper-time-block-cost` at `31df6e895`: the clones seat's DSP stack, 51 commits, 24 branches inside it. ADI-Shaper goes from 7 stages and 418 controls to 10 stages and 721: Time, Noise and Room; the saved order of stages; the Width follower; Volume retrigger from audio, a key and note-on; Filter stereo offset, peak and 24 dB; Pan transfer, delay range and four gain laws; Drive round clip, power saturation and folds. Shaper files only, no window file. | Merged clean. Five validators pass. No slot check yet: slot 7 was still finishing another branch's first build. | gathered, not yet built: slot 7 was busy |
| SC-481 | `basics/shaper-chain` `824c5b65b` | Picked the one test commit of `clones/shaper-time-locate-regression` (`9944e00a2`): Time history across a locate to the same position. One collision in `test_adi_shaper_pd.cpp`'s `main`, kept both, the call sits beside `timePd()`. Its Devices prerequisite (`80bbcb6bd`) is already on the base. | No slot check yet: slot 7 was still finishing another branch's first build. | gathered, not yet built: slot 7 was busy |
| SC-481 | `basics/shaper-chain` `8f0502e06` | Ported the one commit of `clones/shaper-sweep-stereo` (`099aaa4fd`): a signed stereo centre offset per Sweep band, controls 853-855, appended after the Filter controls. The Sweep kernel and its suite are the branch's, unchanged. One test line adapted: it wrote 100 to every id between the base controls and its own to show holes are ignored; that range now holds Time, Noise and Room, so it writes only the holes. | No slot check yet: slot 7 was still finishing another branch's first build. Built to the branch, measured by the clones seat on 22 November against the model on its own base; not measured again on the gathered device. | gathered, not yet built: slot 7 was busy |
| SC-481 | `basics/shaper-chain` `2bc49936e` | Merged `clones/shaper-hold-input` at `cf693f3ab` (four commits): a follower on each band's Crush hold rate, its detector source (split band or full stage input) and the detector's high-pass and low-pass, controls 877-912, descriptors 724-759. Joined by hand in the Crush call, the parameter switch and two tests (the commit message lists each). With the follower off the Crush call is the one the chain already made. | No slot check yet: slot 7 was still finishing another branch's first build. **One choice is this merge's reading, not measured:** while a follower moves the hold rate, Crush does not take the flat capture at a full-rate plateau (the landed endpoint repair takes it only for a constant contour). | gathered, not yet built: slot 7 was busy |

| SC-481 | `basics/shaper-chain` `88b23c576` | Round 1's last commit: a local in the Pd test no longer hides `main`'s (MSVC C4456). | First slot check (core, all suites) at this head: build OK; two shaper suites failed, fixed in the next row. | fixed |
| SC-481 | `basics/shaper-chain` `697369409` | The hold follower oracle (`holdFollowerOracle` in `test_adi_shaper.cpp`) splits as the gathered rack does: each outer band across its own edge first, the middle as the complement (the stack's `firstOrderBands`); the branch had modelled the 22 November serial split. The Pd test's undeclared-ID probe named 855, now the high Sweep offset; it names 860, a hole. No device code changed. | Slot 7 core, filter `shaper`, at `3d942454e`: `adi_shaper_tests` passes (it failed four times before). | gathered, Pd re-check pending |
| SC-481 | `basics/shaper-chain` `3d942454e` | Merged `origin/main` (BASE). Clean. | Five validators pass. | merged |
| SC-481 | `basics/shaper-chain` `599d639ea` | Crush quantiser bias per band, from `clones/shaper-crush-bias` at `5f1d6c4a2`, joined as round 1 planned (the landed quantiser body stays; the bias joins the perturbation; with dither on, the perturbation's own staircase is subtracted). Controls 865-867, descriptors 760-762. Round 1 made the commit and stopped before pushing it; round 2 pushed it unchanged. | `adi_shaper_crush_tests` passes, the branch's bias fixture included, and the older fixtures prove bias 0 keeps its samples. | gathered, Pd re-check pending |
| SC-481 | `basics/shaper-chain` `4a5190b7d` | Crush treble shelf per band, from `clones/shaper-crush-treble` at `edc8ee1f2`: a 350 Hz one-pole split after the hold, the high part scaled by treble + (1 - treble) x bits x hold. At full treble the gain is exactly one and the sample is untouched. Controls 868-870 (default 100), descriptors 763-765. | The branch's four Crush fixtures (steps, impulse tails at 44.1 and 96 kHz, moving contours, transfer, history) pass on the landed kernel unchanged; its native and editable Pd taps: see the state. Built to the branch, measured by the clones seat on 22 November; not measured again on the gathered device. | gathered, Pd re-check pending |
| SC-481 | `basics/shaper-chain` `0e90b9dfd` | Crush sample-clock jitter per band, from `clones/shaper-crush-jitter` at `0b6ca57ef`. Jitter 0 runs the landed hold sample for sample; jitter on runs the branch's clock in a separate `jitteredHold()` (increment = uncapped step x a speed drawn from the shared generator at its own seed; capture fraction = remainder / increment; a flat full-height contour pauses phase and draws). The phase stays the landed float. Controls 871-873, descriptors 766-768. | The branch's four fixtures (interval statistics, saturated capture markers at four rates, bypass pause, jitter with dither) pass. The markers pin ADI's own seed; the statistics were measured by the clones seat; not measured again on the gathered device. | gathered, Pd re-check pending |
| SC-481 | `basics/shaper-chain` `292fbc558` | Crush eight-pole prefilter before the hold, switch per band, from `clones/shaper-crush-prefilter` at `f05454619`; gain from a cubic table of the (landed, float) hold step, capped below Nyquist; skipped for a flat full-rate contour. Controls 874-876 (switches, off), descriptors 769-771. The static check that 875 was a hole now checks that the prefilter ends where the hold follower starts. | The branch's three fixtures (measured readings at four rates, endpoints and repair, moving contour) pass. Built to the branch, measured by the clones seat on 22 November; not measured again on the gathered device. | gathered, Pd re-check pending |
| SC-481 | `basics/shaper-chain` `a5a105670`, `e96e6fdaf` | The Pd test's walks over the descriptors count what was gathered: the combined-declaration walk ends with 853-855, 877-912, 865-876; the Filter project's retained count is the device's count less nine, not 712; the sparse ten-stage recall also requires 853-855 and 865-912 to regain their defaults. Test code only. | See the state. | gathered, Pd re-check pending |

No file of another seat was edited by hand: every file touched is the clones seat's own shaper code, taken as
that seat wrote it, and the shaper tests. Round 2 joined the four Crush commits to the landed kernel by hand;
each commit message says how.

## For the director

Nothing needs your answer for the work to go on. Three things you should know:

1. **Most of what the list calls missing is sound that exists and is waiting, not sound nobody built.** With
   the clones seat's DSP stack and two small branches gathered, "missing" falls from 130 to 88. Four more
   small branches and the panel stack hold about 22 more rows. Recommendation: let the chain finish gathering before anything is
   judged missing.
2. **The curve editor panel is a new thing a user would see.** The clones seat held it for your agreed sheet,
   and so does this chain: it will sit on the chain with the look those branches gave it, unchanged, and the
   chain lands only on your word.
3. **Pitch was built once (19 November) and dropped when the device was rebuilt.** It is on no current branch
   of the device. Recommendation: port it as an eleventh stage after the panel join; no decision needed
   unless you want it left out.

## Step 1: the survey of the clones seat's branches

78 branches, read against `origin/land/basics-1128c` on 28 November. "Ahead" is the number of commits the
branch has that the base lacks. They fall into seven groups:

- **10 already landed** (on the base).
- **25 make the DSP stack**: `shaper-time-block-cost` (`31df6e895`, 25 November) is the top and contains the
  other 24, among them the two integration stacks the mission named (`shaper-dsp-stack` `37c8da138`,
  `shaper-stage-integration` `5abd35d64`) and `shaper-filter-integration` `4315c3e72`. Its last word in the
  clones log (25 November): "Use the finite audio word check throughout ADI-Shaper. Lands after 3 December",
  ten headless suites passed at both Pd precisions, PR requested. **In the chain.**
- **1 test branch** on top of an ancestor of the stack: `shaper-time-locate-regression`. **In the chain.**
- **7 superseded by a later port that is in the stack** (their commits are not ancestors, their content is):
  `shaper-noise-port`, `shaper-reverb-port`, `shaper-audio-retrigger`, `shaper-power-saturation`,
  `shaper-rounded-clip`, `shaper-split-slew`, `shaper-drive-margin-1125`. Nothing to take.
- **7 left out of the stack**: `shaper-crush-bias`, `shaper-crush-treble`, `shaper-crush-jitter`,
  `shaper-crush-prefilter`, `shaper-sweep-stereo`, `shaper-hold-input`, and `shaper-crush-hold-follower`
  (superseded by `shaper-hold-input`). Each is marked "Lands after 3 December" in the clones log of
  22 November. **All six are in the chain** (Sweep stereo and the hold follower in round 1, the four Crush ones in round 2).
- **5 make the newest panel stack**: `shaper-curve-edit` (`b3d393b36`) on `shaper-panel-trim-1125`
  (`a0aa1b64f`), `-grid-1125`, `-rack-1125`, `-reconcile-1125`. Clones log, 25 November: "The panel remains
  held for the director's agreed proposal sheet and the after-3-December landing window." **To merge.**
- **14 older generations of the panel** (`shaper-trim` and the nine under it; `shaper-linear-trim` and the
  three under it): the same panel on older bases. Nothing to take.
- **9 first generation** (`adi-shaper` to `adi-shaper-follower`, 19 November): the device before it was
  rebuilt. Only its Pitch stage is not found later.

| Branch (`clones/`) | Head | Date | Ahead | What it is and where it stands |
|---|---|---|---|---|
| `adi-shaper` | `295056b66` | 11-19 | 1 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-drive-crush` | `a3891a368` | 11-19 | 6 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-editor` | `b34eb0896` | 11-19 | 11 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-follower` | `592f6ba7c` | 11-19 | 19 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-modulation` | `d18b7d117` | 11-19 | 4 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-order` | `f820812f2` | 11-19 | 16 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-performance` | `b8f22f7d7` | 11-19 | 12 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-sources` | `380335a2e` | 11-19 | 10 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `adi-shaper-time` | `9ead5d66a` | 11-19 | 9 | first generation (19 November), before the device was rebuilt as `shaper-rollup`, which landed. Its Pitch stage exists nowhere else: see the notes |
| `shaper-audio-retrigger` | `e08b932f1` | 11-22 | 3 | superseded: its Volume retrigger is in the chain through the later port `shaper-trigger-current-1125` (`58cb2115b`); controls 754-764 are the same |
| `shaper-clock-grid` | `80e2ad089` | 11-22 | 21 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-corners` | `afcad928c` | 11-19 | 10 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-crush-bias` | `5f1d6c4a2` | 11-22 | 1 | **in the chain** (round 2): ported as `599d639ea`. Crush quantiser bias per band, controls 865-867 |
| `shaper-crush-clock-precision` | `51901a1ab` | 11-22 | 0 | already on the base (landed) |
| `shaper-crush-endpoint` | `761744fba` | 11-22 | 0 | already on the base (landed) |
| `shaper-crush-hold-follower` | `6e9134be7` | 11-22 | 1 | superseded by `shaper-hold-input` (in the chain), which carries the same controls 877-903 and more; the clones log asks to retire it |
| `shaper-crush-hold-grid` | `9925a7054` | 11-22 | 0 | already on the base (landed) |
| `shaper-crush-jitter` | `0b6ca57ef` | 11-22 | 1 | **in the chain** (round 2): ported as `0e90b9dfd`. Crush clock jitter per band, controls 871-873 |
| `shaper-crush-prefilter` | `f05454619` | 11-22 | 1 | **in the chain** (round 2): ported as `292fbc558`. The low-pass before the Crush hold, controls 874-876 |
| `shaper-crush-resolution` | `2afb86d57` | 11-22 | 0 | already on the base (landed) |
| `shaper-crush-treble` | `edc8ee1f2` | 11-22 | 1 | **in the chain** (round 2): ported as `4a5190b7d`. Crush treble per band, controls 868-870 |
| `shaper-curve-cpu` | `f1c0ce15e` | 11-19 | 13 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-curve-edit` | `b3d393b36` | 11-25 | 7 | NOT in the chain yet. The top of the newest panel stack: the free curve (points with a bend and a corner shape), three pens, the grid, the rack opener, Volume Trim and Width Gain. Collides with the DSP stack in 9 shaper files and with the base in 3 window files |
| `shaper-drive-deadline` | `44de27f06` | 11-22 | 0 | already on the base (landed) |
| `shaper-drive-high-rates` | `188966bd0` | 11-25 | 10 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-drive-integration` | `519d1f17b` | 11-25 | 6 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-drive-margin-1125` | `dbc4c2cf6` | 11-25 | 1 | superseded: its Drive phase-group change is in the chain through `shaper-drive-high-rates` (`188966bd0`); judged by its names, not by a test |
| `shaper-drive-three-band-cost` | `220246df0` | 11-25 | 7 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-dsp-stack` | `37c8da138` | 11-25 | 46 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-filter-integration` | `4315c3e72` | 11-25 | 50 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-filter-peak` | `966700ee4` | 11-22 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-filter-steep` | `5fff5d0a1` | 11-22 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-filter-stereo` | `830d3a121` | 11-22 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-fold-shapes` | `e234b4dc0` | 11-25 | 2 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-hold-input` | `cf693f3ab` | 11-22 | 4 | **in the chain**: merged at this head as `2bc49936e`. The follower on the Crush hold rate, its detector source and its detector filter, controls 877-912 |
| `shaper-key-note-core-1125` | `527b7a677` | 11-25 | 10 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-linear-grid` | `ffcc76a3a` | 11-22 | 3 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-linear-rack` | `aea61bfc0` | 11-22 | 2 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-linear-reconcile` | `e8ff2ead8` | 11-22 | 1 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-linear-trim` | `3e8e20f5e` | 11-22 | 6 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-noise-current-1125` | `79c377a73` | 11-25 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-noise-port` | `d9d0e19ef` | 11-22 | 2 | superseded: its Noise stage is in the chain through the later port `shaper-noise-current-1125` (`79c377a73`); controls 536-625 are the same |
| `shaper-order-current-1125` | `0c16c68be` | 11-25 | 2 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-pan-edge` | `82caa7d5d` | 11-22 | 0 | already on the base (landed) |
| `shaper-pan-gain-laws` | `fb744c450` | 11-25 | 2 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-pan-range-1125` | `1d47fa1b2` | 11-25 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-pan-transfer` | `16cbbf6c8` | 11-25 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-panel` | `afe325331` | 11-19 | 9 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-panel-audio-reconcile-20261120` | `e37c8d4ed` | 11-20 | 18 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-panel-grid-1125` | `0ec16c922` | 11-25 | 3 | NOT in the chain yet: an ancestor of the panel stack `shaper-curve-edit` |
| `shaper-panel-rack-1125` | `3e494ff41` | 11-25 | 2 | NOT in the chain yet: an ancestor of the panel stack `shaper-curve-edit` |
| `shaper-panel-reconcile-1125` | `a801fcf93` | 11-25 | 1 | NOT in the chain yet: an ancestor of the panel stack `shaper-curve-edit` |
| `shaper-panel-trim-1125` | `a0aa1b64f` | 11-25 | 6 | NOT in the chain yet: an ancestor of the panel stack `shaper-curve-edit` |
| `shaper-pens` | `20b5f3b47` | 11-19 | 15 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-point-selection` | `eeb76d1fb` | 11-19 | 12 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-power-saturation` | `53f80b476` | 11-25 | 2 | superseded: its three switches (931-933) and code are in the chain through the Drive integration (`03a7210ab`) |
| `shaper-rack-editor` | `4b0c60360` | 11-22 | 20 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-reverb-current-1125` | `95b314756` | 11-25 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-reverb-port` | `97f9ffd7c` | 11-22 | 3 | superseded: its Room stage is in the chain through the later port `shaper-reverb-current-1125` (`95b314756`); controls 626-715 are the same |
| `shaper-rollup` | `e87f45dd7` | 11-19 | 0 | already on the base (landed) |
| `shaper-rounded-clip` | `ce4fcfbf4` | 11-25 | 1 | superseded: its three switches (928-930) and code are in the chain through the Drive integration (`03a7210ab`) |
| `shaper-session-gate` | `046e52486` | 11-22 | 0 | already on the base (landed) |
| `shaper-split-repair-1125` | `0ac2daa75` | 11-25 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-split-slew` | `ef2ca0238` | 11-22 | 3 | superseded: its moving crossover code is in the chain through `shaper-split-repair-1125` (`0ac2daa75`) and `shaper-steep-slew-response` (`7a322d2cc`); 23 of its 24 new names are there |
| `shaper-split-spacing` | `34ee3fbd5` | 11-22 | 0 | already on the base (landed) |
| `shaper-stage-integration` | `5abd35d64` | 11-25 | 16 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-stage-ranks` | `1719f64e4` | 11-22 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-steep-slew-response` | `7a322d2cc` | 11-25 | 2 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-sweep-stereo` | `099aaa4fd` | 11-22 | 1 | **in the chain**: its one commit ported as `8f0502e06`. Sweep stereo centre offset per band, controls 853-855 |
| `shaper-three-band-split` | `42c4135ad` | 11-22 | 0 | already on the base (landed) |
| `shaper-time` | `72cd82815` | 11-19 | 2 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-time-block-cost` | `31df6e895` | 11-25 | 51 | **in the chain**: the top of the DSP stack, merged at this head |
| `shaper-time-current-1125` | `5594c282a` | 11-25 | 4 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-time-locate-regression` | `9944e00a2` | 11-25 | 6 | **in the chain**: its one test commit picked as `824c5b65b`; the rest of it was on the base or in the stack |
| `shaper-trigger-current-1125` | `58cb2115b` | 11-25 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-trim` | `69422cef2` | 11-22 | 24 | superseded: an older generation of the panel stack (22 November base); `shaper-curve-edit` is its port to the 25 November base, same panel file size |
| `shaper-width-current-1125` | `932753575` | 11-25 | 1 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |
| `shaper-width-follower` | `a1556885d` | 11-22 | 4 | in the chain: an ancestor of the DSP stack `shaper-time-block-cost` |

## Step 3: the list scored again

On main: 21 has, 24 part, 130 missing, 3 out. On the chain at `e96e6fdaf` (round 2): **43 has, 48 part,
84 missing, 3 out**; at `2bc49936e` (round 1) it was 39 has, 48 part, 88 missing, 3 out. The four Crush rows
are "built to the branch, not yet measured against the model" on the gathered device. Round 1's words:
On the chain at `2bc49936e`: **39 has, 48 part, 88 missing,
3 out**. A bold state changed. The score is read from the controls and code in the chain; nothing was measured
against the model in this round. "held on" names a clones branch that is not in the chain yet and carries the
row, in whole or in part: it is not counted.

| # | What the model does | On main | On the chain | Note, or the clones branch that holds it |
|---|---|---|---|---|
| | **Meet the Shapers, pp. 9–28** | | | |
| 1 | The parameter slider shifts the whole modulation curve vertically while retaining its shape until it hits an endpoint. | missing | missing |  |
| 2 | The editor shows which parameter is currently modulated by the curve or follower. | missing | missing |  |
| 3 | The contour reads from a buffer of recent audio to stutter, reverse, slow or scratch playback. | missing | **has** | Time stage, controls 416-463 |
| 4 | Its vertical time range can follow the LFO length or use fixed musical or millisecond ranges. | missing | **has** | Time Range: Follow, 1/2/4/8 bars, 2.5/10/20 ms |
| 5 | It can jump instantly at hard curve steps or fade the jump to suppress clicks. | missing | missing |  |
| 6 | Its editor shows the time offset and the line marking the current buffer’s start. | missing | missing |  |
| 7 | The contour shifts pitch by up to two octaves in either direction. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 8 | It offers Complex, Vocals, Drums and Vintage processing for different source material. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 9 | It provides three fixed grain sizes in Complex mode and a continuous grain-size setting in Vintage mode. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 10 | It can shift formants separately from pitch in Vocals and Drums modes. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 11 | It can link formants to pitch so they shift together. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 12 | Density adjusts filling of short gaps when formant and pitch shift differ. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 13 | Its pitch editor can snap the contour to semitone steps and highlight musical intervals. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 14 | It supports fine pitch edits in cents and can reset the whole-semitone offset while retaining the cents. | missing | missing | held on `adi-shaper-sources` / two-head-reader (first-generation Pitch) |
| 15 | The Drive contour controls how hard audio hits the nonlinear stage. | has | has |  |
| 16 | It offers twelve distortion styles, including clipping, asymmetry and wavefolding. | part | part | three styles plus Round Hard Clip, Power Saturation and three Folds |
| 17 | It adjusts input and output gain independently around the distortion. | has | has |  |
| 18 | Grip suppresses quiet input to emphasize loud portions of the signal. | missing | missing |  |
| 19 | Push offsets the signal before compatible distortion styles to create asymmetry. | missing | missing |  |
| 20 | Accent changes the response to modulation peaks. | missing | missing |  |
| 21 | Tone blends in low-pass or high-pass filtering as Drive increases. | missing | missing |  |
| 22 | A transfer graph shows how the selected style reshapes the input. | missing | missing |  |
| 23 | It adds a generated or sampled noise source to the input. | missing | **part** | generated White and Pink only; no sampled source |
| 24 | It provides a categorized library of noise sources. | missing | **part** | two generated sources; no library |
| 25 | It imports user audio for use as a noise source. | missing | missing |  |
| 26 | It gives noise level and pitch independently shaped contours. | missing | **has** | Noise Level and Pitch contours |
| 27 | It trims the noise level separately from the contour. | missing | **has** | Noise Trim |
| 28 | It filters the noise with independent high-pass and low-pass edges. | missing | **has** | Noise High Pass and Low Pass |
| 29 | It can make the added noise mono or audition the noise without dry input. | missing | **has** | Noise Mono and Noise Only |
| 30 | It lets the noise follow the input envelope or duck when that input gets louder. | missing | missing |  |
| 31 | It convolves audio with a selectable impulse response. | missing | **part** | ADI's own generated room; no chosen impulse response |
| 32 | It ships the maker’s categorized impulse responses. | out | out |  |
| 33 | It imports user impulse-response files. | missing | missing |  |
| 34 | Separate curves control the level fed into the reverb and the level returned from it. | missing | **has** | Room Send and Return contours |
| 35 | It delays the wet onset by a time or song division. | missing | **part** | Predelay as a time; no song division |
| 36 | It adjusts decay and can cut off the tail at a gated duration. | missing | **has** | Decay and Gate |
| 37 | It scales the impulse-response size independently of decay. | missing | **has** | Size |
| 38 | It narrows or widens only the wet stereo image. | missing | **has** | Room Width |
| 39 | It can clear the tail when playback reaches a hard zero point in the wet contour. | missing | **has** | Clear Tails |
| 40 | It can follow or duck the input envelope to control the reverb tail. | missing | missing |  |
| 41 | Its dedicated dry/wet balance can retain full dry level while bringing in wet signal. | missing | **part** | a per-band Mix; the full-dry law is not checked |
| 42 | It filters each band with choices of clean and warm low-pass, high-pass, band-pass, notch and peak circuits. | part | part | Peak added (913-915); no warm circuits, no notch |
| 43 | It offers 6, 12 and 24 dB/octave filter responses. | part | **has** | 24 dB low-pass and high-pass added (916-918) |
| 44 | It independently shapes cutoff and resonance. | has | has |  |
| 45 | Its stereo control offsets cutoff in opposite directions for the two channels. | missing | **has** | Filter Stereo Offset (850-852) |
| 46 | Its Warm filters can add adjustable saturation inside the resonance feedback loop. | missing | missing |  |
| 47 | Its Safe Resonance switch can permit or prevent self-oscillation in compatible Warm filters. | missing | missing |  |
| 48 | Its Hard Distortion switch changes the warm feedback circuit to hard clipping. | missing | missing |  |
| 49 | It draws a filter response that follows cutoff and resonance modulation. | missing | missing |  |
| 50 | It selects a flanger, a phaser or no effect per band. | has | has |  |
| 51 | The phaser offers four all-pass stage counts. | has | has |  |
| 52 | Independent contours control center frequency and feedback. | has | has |  |
| 53 | It can flip feedback polarity to change peaks into notches or alter the flanger’s timbre. | has | has |  |
| 54 | It can shift the left and right center frequencies in opposite directions. | missing | **has** | Sweep Stereo Offset (853-855) |
| 55 | It modulates bit depth down toward one bit and can pass through at maximum. | has | has |  |
| 56 | It can dither before quantization. | has | has |  |
| 57 | It can add a DC bias before reducing bit depth. | missing | **has** | Crush Bias (865-867), round 2 |
| 58 | It independently modulates resampling rate and can pass the input at the host rate. | has | has |  |
| 59 | It can add controlled irregularity to resampling time. | missing | **has** | Crush Jitter (871-873), round 2 |
| 60 | It can low-pass the input before resampling. | missing | **has** | Crush Prefilter switch (874-876), round 2 |
| 61 | It can attenuate treble with an output shelf. | missing | **has** | Crush Treble (868-870), round 2 |
| 62 | It shapes gain between silence and unity. | has | has |  |
| 63 | It trims the gain at the output of the Volume stage. | missing | missing | held on `shaper-curve-edit` (Volume Trim) |
| 64 | It can overlay an external sidechain waveform behind the Volume contour. | missing | missing |  |
| 65 | It pans using an independent contour and stereo balance. | has | has |  |
| 66 | It can use four stereo-balance gain laws or combine the channels before panning. | part | part | four gain laws (925-927) and a stereo transfer; channel combining is not checked |
| 67 | It blends level panning and short interchannel delay. | has | has |  |
| 68 | It sets a maximum Haas delay between zero and 40 ms. | missing | **has** | Pan Delay Range 0-40 ms (922-924) |
| 69 | It draws a vectorscope to show stereo balance and correlation. | missing | missing |  |
| 70 | It shapes side gain from mono through natural width to double width. | has | has |  |
| 71 | It provides a separate width-stage output gain. | missing | missing | held on `shaper-curve-edit` (Width Gain) |
| 72 | It draws a vectorscope to show stereo width and correlation. | missing | missing |  |
| | **Shaper Selection Screen, Shaper Bar and Top Bar, pp. 29–30** | | | |
| 73 | The selection screen adds up to one instance of each available Shaper or Tool to the serial chain. | part | part | ten stages; still no add and remove |
| 74 | Quick presets initialize selected Time, Volume or Reverb stages to useful starting configurations. | missing | missing |  |
| 75 | Clicking a stage selects its controls and chart for editing. | part | part | held on `shaper-curve-edit` (panel) |
| 76 | A stage switch enables or bypasses the module without removing its settings. | part | part | Time, Noise and Room have switches too |
| 77 | It removes a stage and its effect from the rack. | missing | missing |  |
| 78 | Master Bypass returns dry audio while preserving the mix setting for reactivation. | part | part |  |
| 79 | Master Mix scales every stage’s per-band mix together. | has | has |  |
| 80 | A quick guide explains wave-editing gestures. | missing | missing |  |
| | **Preset Library, Preset Browser, Main Menu and Help Bar, pp. 30–34** | | | |
| 81 | It saves and recalls a complete custom setup through a preset library. | part | part |  |
| 82 | It lets the user step through and favorite whole-device presets. | part | part |  |
| 83 | It offers a library filter by name, category, pack, favorite and included or excluded Shaper. | part | part |  |
| 84 | It supplies ready-to-use whole-rack presets for common production tasks and sound-design styles. | missing | missing |  |
| 85 | It provides the maker’s presets and older the model’s libraries. | out | out |  |
| 86 | It syncs its preset library to download new presets and back up user presets. | missing | missing |  |
| 87 | It initializes the entire rack or resets only the selected stage. | part | part |  |
| 88 | It saves and loads a preset as a portable FXP file. | part | part |  |
| 89 | It can set and remove a user-defined startup preset. | part | part |  |
| 90 | It reports the hovered control, preset waveform name or precise point position in a help bar. | part | part |  |
| 91 | It offers display scaling and a high-contrast mode. | missing | missing |  |
| 92 | It can enable OpenGL graphics on Windows. | missing | missing |  |
| 93 | Its menu opens help and exposes update, system-information and licensing actions. | missing | missing |  |
| 94 | It can smooth retriggers to suppress clicks, adding lookahead for triggered Volume. | missing | **part** | Trigger Smoothing switch; the look-ahead is not checked |
| 95 | It reports 16 samples of latency while Drive is loaded. | has | has |  |
| 96 | It can reserve Drive latency while Drive is disabled to keep the rack’s delay consistent when switched. | part | part |  |
| | **Wave Presets and Wave Palette, pp. 35–36** | | | |
| 97 | It loads a preset waveform while preserving the other settings in the selected stage and band. | missing | missing |  |
| 98 | It offers a built-in set of waveform templates suited to each Shaper. | missing | missing |  |
| 99 | It ships the maker’s original preset waves. | out | out |  |
| 100 | It keeps user favorite waves available across instances. | missing | missing |  |
| 101 | It imports and exports a collection of favorite waves. | missing | missing |  |
| 102 | It stores up to nine waves tied to their source stage and band within the current setup. | missing | missing |  |
| 103 | It height-matches a loaded wave to the current shape when requested. | missing | missing |  |
| 104 | MIDI notes switch stored waves to their original stages and bands while note C can still retrigger. | missing | missing |  |
| 105 | It automates wave-slot selection separately for each target LFO. | missing | missing |  |
| | **Shaper Oscilloscope and LFO Wave Editor, pp. 36–41** | | | |
| 106 | An oscilloscope overlays incoming and processed audio against the current contour. | missing | missing | held on `shaper-curve-edit` (panel trace) |
| 107 | The scope can show separated stereo, overlaid stereo, peak magnitude or an automatic stage-specific display. | missing | missing |  |
| 108 | The scope temporarily displays a filtered external detector signal while relevant controls are hovered. | missing | missing |  |
| 109 | It draws freely placed points that can be added, moved and removed. | part | part | held on `shaper-curve-edit` (free points) |
| 110 | It makes each point hard, medium or soft independently, allowing hard steps and smooth arcs in the same shape. | part | part | held on `shaper-curve-edit` (corner shapes) |
| 111 | The line pen can draw, resize and repeat straight segments. | part | part | held on `shaper-curve-edit` (line pen) |
| 112 | The arc pen can draw, resize and repeat curved segments. | missing | missing | held on `shaper-curve-edit` (arc pen) |
| 113 | The S-curve pen can draw, resize and repeat segments with an inflection. | missing | missing | held on `shaper-curve-edit` (S pen) |
| 114 | Grid snap can constrain point creation and movement, including straight and triplet grids. | missing | missing | held on `shaper-curve-edit` (grid) |
| 115 | It selects all or several points and can move, scale or skew the selection. | missing | missing | held on `shaper-curve-edit` (marquee and group) |
| 116 | It deletes selected points or clears the entire shape. | missing | missing | held on `shaper-curve-edit` (panel) |
| 117 | It randomizes point positions and weights for a selection or the whole shape, and can keep the rhythm when requested. | missing | missing |  |
| 118 | It applies double-time or triple-time to the whole wave or selection. | missing | missing |  |
| 119 | It nudges the wave or selected points horizontally in grid increments or smaller steps. | missing | missing |  |
| 120 | It undoes and redoes edits independently for each stage and band. | part | part |  |
| 121 | It snaps existing points to the grid. | missing | missing |  |
| 122 | It flips a wave or selection horizontally or vertically. | missing | missing |  |
| 123 | It copies a wave between bands, stages or instances and can scale it on paste. | missing | missing |  |
| 124 | A modulation trace shows the actual processed shape after smoothing and envelope modulation. | missing | missing |  |
| | **LFO Settings and MIDI Setup, pp. 41–43** | | | |
| 125 | It synchronizes the contour to host song position and tempo. | has | has |  |
| 126 | It offers beat lengths from a 128th note to 32 bars with straight, dotted and triplet choices and quick divisions. | part | part | held on `shaper-curve-edit` (sync divisions) |
| 127 | Its free-running clock spans 0.02 Hz to 5.24 kHz and aligns to project position at playback start. | part | part |  |
| 128 | MIDI Pitch mode sets the LFO speed to the note frequency with an octave transpose control. | missing | missing |  |
| 129 | Each band can run with independent clock settings. | has | has |  |
| 130 | It links mode, length and trigger for multiple bands. | missing | missing |  |
| 131 | It progressively smooths hard LFO transitions. | part | part |  |
| 132 | An incoming MIDI note restarts the contour. | missing | **part** | the core takes a note-on; the host's MIDI route to the device is not checked |
| 133 | An audio transient in the main input restarts the contour. | missing | **part** | Volume contours only (754-759) |
| 134 | Triggered contours can loop following each restart or play once and hold their end value. | missing | **part** | Volume contours only (754-759) |
| 135 | A triggered one-shot can stop at a movable end marker inside the curve. | missing | **part** | Volume contours only (754-759) |
| 136 | It chooses host MIDI or a hardware port as input and filters to one MIDI channel or all channels. | missing | missing |  |
| | **Audio Trigger Setup, pp. 44–45** | | | |
| 137 | An external sidechain transient can restart the contour independently of the main input. | missing | **part** | the core takes a key signal; the host route is DEV-05 |
| 138 | The detector only accepts transients above an adjustable threshold. | missing | **has** | Trigger Threshold |
| 139 | It can audition the detector input after filtering. | missing | missing |  |
| 140 | It limits the transient detector to an adjustable high-pass and low-pass range. | missing | missing |  |
| 141 | It selects Drums, General or Complex transient detection. | missing | **has** | Trigger Profile: Fast, Balanced, Extended |
| 142 | It adjusts detector sensitivity independently of the input threshold. | missing | **has** | Trigger Detail |
| 143 | It advances or delays a transient trigger by up to four milliseconds. | missing | **part** | Trigger Shift; its range in ms is not checked |
| 144 | It can emit a selected MIDI note for each detected transient. | missing | missing |  |
| | **Multiband Editor and Routing and Mixing, pp. 45–46** | | | |
| 145 | Each stage independently splits the audio into one, two or three bands with adjustable crossover frequencies. | has | has |  |
| 146 | Each crossover edge can use a 6, 12 or 24 dB/octave slope. | has | has |  |
| 147 | The band editor displays the input spectrum and modulation values for the selected stage. | missing | missing | held on `shaper-curve-edit` (spectrum) |
| 148 | It solos a band and mutes the other two for audition. | missing | missing |  |
| 149 | It can reorder Shapers by dragging in the serial stage chain. | missing | **part** | the order is saved and played (716-726); no dragging |
| 150 | Each band independently blends the dry and processed signal. | has | has |  |
| | **Envelope Follower, pp. 47–54** | | | |
| 151 | A switch enables continuous amplitude-driven modulation alongside the LFO for supported stages. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 152 | Attack controls how quickly the envelope rises. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 153 | Hold keeps the detector response up for a chosen duration. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 154 | Release controls how quickly the envelope falls. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 155 | Adaptive Release shortens the recovery from brief peaks relative to sustained signal. | missing | missing |  |
| 156 | Threshold selects the input level needed for the follower to respond. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 157 | Amount sets the strength and direction of the follower’s effect on the target. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 158 | Shift offsets the envelope vertically. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 159 | Depth scales the contour’s modulation range before applying the envelope. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 160 | Add and Multiply combine the audio envelope with the drawn shape in different ways. | missing | **part** | Width (727-753) and the Crush hold rate (877-903) only |
| 161 | The detector can follow the crossover band or use freely chosen frequencies. | missing | **part** | the Crush hold follower only: split band or full stage input (904-906) |
| 162 | Free detection offers independent high-pass and low-pass cutoffs. | missing | **part** | the Crush hold follower only: detector high-pass and low-pass (907-912) |
| 163 | The follower can track an external sidechain independently of the signal being shaped. | missing | **part** | Width follower takes the key; the host route is DEV-05 |
| 164 | Solo Sidechain auditions the filtered detector signal through the output. | missing | missing |  |
| | **Tools, pp. 55–60** | | | |
| 165 | The Compressor can be placed anywhere in the shaping chain and compresses above a threshold at a selectable ratio. | missing | missing |  |
| 166 | Its attack, hold and release independently shape gain reduction. | missing | missing |  |
| 167 | Its adaptive release responds faster to transient peaks. | missing | missing |  |
| 168 | Manual or automatic makeup gain raises output after compression. | missing | missing |  |
| 169 | The Compressor can filter the detector, use an external key or audition that filtered key. | missing | missing |  |
| 170 | The Compressor can use the shared three-band editor to apply independent settings by band. | missing | missing |  |
| 171 | An Oscilloscope Tool can monitor at any position in the chain without processing audio. | missing | missing |  |
| 172 | The Oscilloscope sets its display length and can trigger from sync, MIDI or audio. | missing | missing |  |
| 173 | It vertically zooms a displayed waveform. | missing | missing |  |
| 174 | It can show left, right, overlaid or split stereo channels. | missing | missing |  |
| 175 | It overlays the external sidechain on the main signal. | missing | missing |  |
| 176 | It freezes and resumes the display while audio continues. | missing | missing |  |
| 177 | It marks above-full-scale input without clipping the audio. | missing | missing |  |
| | **Automation Target Indicators, p. 61** | | | |
| 178 | A shortcut reveals which of the visible controls are automation targets. | part | part |  |
