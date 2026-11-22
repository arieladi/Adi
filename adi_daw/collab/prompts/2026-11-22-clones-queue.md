# clones — standing queue, 22 November 2026

This is the whole queue to work until your usage limit. It replaces every earlier list win gave this seat.

## How to work

- Fetch first (`git fetch origin`) and read the top entry of `adi_daw/collab/win.md` on main and every log entry
  addressed to you. Rows of `adi_daw/docs/DEFERRED.md` that name the clones seat come before new items (ADR-0248 d1).
- Work the items top to bottom. One branch per item, `clones/<topic>`, cut from `origin/main` unless the item says
  to stack.
- Test first where the item has a test. Build and test under MSVC /W4 /WX, in both Pd precisions where Pd is touched.
- Push, write the entry in your own log (`adi_daw/collab/clones.md` on `clones/log`), end it with "PR requested", and
  start the next item in the same turn.
- An item blocked on win, another seat or a decision: park it with one line in your log and take the next.
- A question for the director goes under "FOR THE DIRECTOR" at the top of your log with your recommendation. Then
  continue.
- Stay in your own paths (`collab/README.md`: `adi_shaper*` wherever they sit, and your log). Needs in other seats'
  paths go in your log's needs table.
- Public text names no other maker's product or vendor beyond those the ADRs name, no machine and no private path.
- When this list is empty, take the next unbuilt row of your lane in `docs/PLAN.md` in milestone order. Work for
  after 3 December goes on branches marked "lands after 3 December". Never stop because "nothing names this lane".
  Stop only at your usage limit or a broken environment.

## Queue

### Before 3 December (ADR-0248 d1, d2: fix before new)

1. **LOG-OWN: your 20 November entries in your own log**
   Re-enter the three "PR requested" entries of 20 November (DF-E59 ff0c9a15, DF-E60 f8ef8af0,
   shaper-panel-audio-reconcile e37c8d4e) in `clones.md`. Commit ee69a8c7 and its neighbours put them in
   `collab/mixview.md` on `mixview/log`.
   Done when `git show origin/clones/log:adi_daw/collab/clones.md` shows all three with those heads. Do not edit
   `mixview.md`; its clean-up is a need (below).

2. **LOG-SCRUB: a scrub commit on clones/log**
   New commit, no rewrite. Remove other makers' product and vendor names and manual links from FOR THE DIRECTOR and
   from the entries (#708's Held list: the section still names the source product). Correct "shaper-only" to
   "every built-in" for the revision-counter scan.
   Done when a public-repo scan of `origin/main..origin/clones/log` finds no product, vendor, machine or private-path
   name. Win lands the log after this.

3. **PANEL-LINEAR: rebuild the panel stack as linear commits (rebuild only)**
   Rebuild reconcile > rack-editor > clock-grid > trim as linear commits, no merges, on `origin/land/clones-1122`,
   under new branch names. Files: `adi_shaper*` (DSP, `juce/adi_shaper_panel*`, `cmake/devices/adi_shaper*.cmake`,
   `pd/devices/ADI-Shaper.pd`, the shaper tests) and the two loaned sites in `ui_main.cpp` and `ui_devices.cpp`.
   Done when `git log --merges land/clones-1122..<new top>` is empty (8 merges today); the ten device suites pass in
   both Pd precisions; the six ADI-Shaper suites pass under /WX; the 46-check rack/editor replay passes.
   Log the new heads, but do not request landing: landing waits on win's decision about #708's hold (ADR-0248 d11
   sheet, d2), not on the linear commits alone.

4. **PD-EXACT-REVIEW: review devices/pd-exact-position**
   The devices lane asked on 20 November for your review of its two hunks in `src/adi/pd_builtins/adi_shaper_tilde.cpp`
   and `adi_shaper_transport.hpp`. The branch is in #706, which lands before #708, so do this early.
   Done when a `clones.md` entry agrees or asks for named changes, and `test_adi_shaper_pd` passes at PD_FLOATSIZE 32
   and 64 on `land/clones-1122` plus the linear stack. Review only; no edits to that branch.

5. **DEV-31-SESSION-GATE: the owed beat-gate test in a real Session**
   DF-E29 is on main (5737c91c), so DEV-31's done-test "its beat gate in a real Session" is now due. New suite
   `tests/test_adi_shaper_session.cpp`, registered in `cmake/devices/adi_shaper.cmake`. From main.
   Done when the suite passes on main with ADI-Shaper's beat sync behind upstream compensation, and fails when the
   device's beat source is broken on purpose.

6. **CURVE-EDIT-P99: continuous point edits raise p99** (blocked: win's decision on #708's hold; skip to the next)
   Your 19 November measurement: edits every 256-frame callback took p99 from 117.5 to 127.1 us and deadline misses
   from 9 to 15 in 30,720. Files: `src/adi/dsp/adi_shaper*`, `tests/test_adi_shaper.cpp`. Stacks on the PANEL-LINEAR
   top.
   Done when the same edited condition gives p99 and misses at or below the uncached evaluator's, with audio
   byte-identical.

7. **DRIVE-4X-P99: four moving Drives under the deadline**
   Four instances of moving Drive at 256 frames measured p99 7.911 ms against a 5.333 ms deadline. Files:
   `src/adi/dsp/adi_shaper_drive.*`, `tests/test_adi_shaper_drive.cpp`.
   Done when the four-track test (nine trials of 8,192 callbacks) shows p99 below the deadline, with Drive's
   measured-response tests and its 16-sample latency unchanged.

8. **PAN-EDGE: Pan's switching transient at the hard endpoints**
   The compensated residual is 0.00907 L / 0.00498 R on the alternating hard Pan setting. Files:
   `src/adi/dsp/adi_shaper.cpp`, `tests/test_adi_shaper.cpp`.
   Done when a new edge oracle test shows the whole-capture compensated RMS residual below 1e-4.

9. **SCOPE-CONTRACT: one OScope 2 and one PsyScope Pro contract for the Mix View seat**
   ADR-0248 d11, ADR-0247 d5. One consolidated `clones.md` entry per scope: measured laws, acceptance cases and an
   explicit open list. Items 10 to 12 then update it.
   Done when the Mix View seat can cite one entry per scope and every number traces to a reproducible private fixture.

10. **PSY-OFFSET: PsyScope's per-source and global offset**
    Bounds, wheel and fine step sizes, and the sign under a nonzero setting. Measurement only; the result goes in
    `clones.md`.
    Done when a table gives offset against displayed pulse shift at three or more nonzero settings, in both units.
    Needs the reference plug-in run interactively. If you cannot, hand the steps to win for the director's idle time
    (with the pen test) and take the next.

11. **OSC-ANALYSERS: the rest of the OScope and PsyScope calibration**
    Correlation integration, goniometer scale, analyser passband, the collision threshold check (-60 dBFS / 24 dB),
    loudness histories, display refresh. Measurement only.
    Done when each has a measured value from a sine or noise fixture, replacing "still lack independent calibration".
    Same fallback as item 10.

12. **PSY-PHASE: PsyScope's phase bars**
    At 0, 90 and 180 degrees in Layers, Sum and single-source: window, averaging, thresholds, refresh. Verify the L/R
    operand switch.
    Done when bar values along one beat are logged for each case. Same fallback as item 10.

### After 3 December (branches marked "lands after 3 December")

The DEV-31 stage ports are cut from `origin/main` (the landed rollup), not from the held panel stack. Port the code
from the old `clones/adi-shaper-*` branches; append parameter IDs; an old state must reopen unchanged.

13. **TWO-HEAD-READER: the shared two-head delay reader**
    Agreed with the devices lane on 19 November (its step 1): you write it, the devices lane reviews it, then the
    Shifter takes it after #660. Files: `src/adi/dsp/two_head_reader.hpp` and its test. DSP and cross-tests only.
    Done when the cross-tests reproduce #660's Shifter and the cyclic Pitch bit for bit (3,127,711 comparisons) and
    neither device changes sound.

14. **DEV-31-PITCH: the cyclic Pitch stage** (blocked: TWO-HEAD-READER's review by the devices lane; skip to the next)
    1-100 ms window, +/-24 st, on the shared reader, from `clones/adi-shaper-sources`. Files:
    `src/adi/dsp/adi_shaper_pitch.*`, `adi_shaper.*`, `adi_shaper_tilde.cpp`, `ADI-Shaper.pd`,
    `tests/test_adi_shaper_pitch.cpp`.
    Done when unison peaks land on the measured frame at three rates, native and Pd agree at both precisions, CPU is
    recorded at 256 and 2,048, and an old state reopens unchanged.

15. **DEV-31-NOISE: the Noise stage**
    White at 44.1 kHz interpolated; ADI's own procedural pink, no source samples. Files:
    `src/adi/dsp/adi_shaper_noise.*`, the integration files, `tests/test_adi_shaper_noise.cpp`.
    Done when the noise contract holds (RMS 0.0223-0.0224, 95% energy at 21.2-21.3 kHz, L/R correlation about 0,
    mono samples equal) with Pd parity.

16. **DEV-31-REVERB: the Reverb stage**
    ADI's own room response, no factory IRs (PLAN DEV-31). Files: `src/adi/dsp/adi_shaper_reverb.*`, the
    integration files, `tests/test_adi_shaper_reverb.cpp`.
    Done when the control-law tests pass (send, return, width, gate, decay, size, predelay) with Pd parity, and CPU is
    recorded separately.

17. **DEV-31-FOLLOWER: the Width band's envelope follower**
    Add/Multiply, amount, depth, offset, threshold, A/H/R. Files: `src/adi/dsp/adi_shaper*`,
    `tests/test_adi_shaper.cpp`.
    Done when the Add law holds within 0.000048 side gain, the attack (100u^3 ms) and hold (500u^2 ms) tables match,
    and the stereo error is at most 5.7e-7.

18. **DEV-31-AUDIO-RETRIG: Volume contour audio retrigger**
    Looping and one-shot, internal detector, reported latency 672 / 1,152 frames at 48 kHz. Files:
    `src/adi/dsp/adi_shaper*`, `tests/test_adi_shaper.cpp`.
    Done when the burst fixture restarts at 14,671 / 54,171 / 82,071, the quiet burst does not, and the reported
    latency equals the measured delay in all 12 rate/algorithm cases.

19. **DEV-31-KEY-MIDI-CORE: external-key Width detection and MIDI note retrigger, shaper side**
    Unit-tested without host wiring. Files: `src/adi/dsp/adi_shaper*`, `tests/test_adi_shaper.cpp`.
    Done when the external-key plateau table (0.5 to 1.3 side gain) and note-on restarts at exact samples pass in unit
    tests. Host wiring is a need from the devices lane (below).

20. **DEV-31-ORDER: recallable stage order**
    One rank per stage, IDs appended, ties broken by identity. Cut from main with the stages that exist; each later
    stage appends its rank. Files: `src/adi/dsp/adi_shaper.*`, `adi_shaper_tilde.cpp`, `ADI-Shaper.pd`,
    `tests/test_adi_shaper.cpp`.
    Done when 4,096 sampled permutations pass, the Volume/Drive order predictions hold both ways, and an old state
    restores the legacy order.

21. **PRESET-ATTRIB: attribute the preset error stage by stage**
    After each stage port, re-run the private preset fixtures and attribute the 81 worse programs per stage. Summary
    only in `clones.md`.
    Done when a per-stage error report shows the count below 0.001 RMS rising from 37. Starts after the first port.

## Waiting on win

- #708 `land/clones-1122`: DF-E59 (ff0c9a15) and DF-E60 (f8ef8af0). Done; do not redo.
- The panel stack: `clones/shaper-panel-audio-reconcile-20261120` (e37c8d4e), `clones/shaper-rack-editor`
  (4b0c6036), `clones/shaper-clock-grid` (80e2ad08), `clones/shaper-trim` (69422cef, top). Held by #708 until win
  decides whether ADR-0248 d11's sheet-before-a-curve-editor hold is lifted. Item 3 rebuilds it linearly; panel tabs
  for new stages wait on the same decision.
- `clones/log` (7609e7e2): lands after item 2's scrub commit.
- Superseded and contained in the trim top: `shaper-pens`, `shaper-panel`, `shaper-corners`,
  `shaper-point-selection`, `shaper-curve-cpu`, `shaper-time`. The old `clones/adi-shaper-*` branches are superseded by
  the landed rollup; they are only sources for items 13 to 20.
- The pen-stroke readings for Line, Arc and S, taken by win in the director's idle time, and any steps handed over
  under items 10 to 12.

## Needs from other seats

- devices: expose ADI-Shaper's per-instance waveform and meter feed in `builtin_host.cpp` (win's, lent to devices)
  and `native_devices.cpp`, so the stock rack shows the live traces and meters.
- devices: the native Time wrapper must reset on a locate to the same position, through the transport revision (you
  hold the failing regression).
- devices: review TWO-HEAD-READER (item 13); the Shifter takes it after #660.
- devices / win, after 3 December: ADI-Shaper's 4-input declaration and MIDI flag in `builtin_catalog.cpp`, the native
  factory's note-on and key planes in `native_devices.cpp`, and an editable Pd `notein`, for item 19's host side.
- window-fn (its owner-review item): review the two loaned sites on the panel stack (`ui_main.cpp` editor factory,
  `ui_devices.cpp` wrench checks).
- window-fn (its SHELL-BROWSER-FIXTURE item, first in its queue): fix the `test_ui_shell.cpp` `browserDevices`
  fixture (Hybrid Reverb assertion, restore track 4, guard the empty strip). It crashes without ADI-Shaper too. Do
  not fix it yourself.
- mixview / win: remove your 20 November entries from `collab/mixview.md` (ee69a8c7 and the DF-E59/DF-E60 entries)
  once item 1 is in your log.

## For the director

- Is your OScope 2 the Full edition? It decides whether its multi-instance Sum can be calibrated.
- DF-E43: which third-party devices from your own racks to clone, and under what names (after ADI-Shaper and the
  scope contracts).
