# devices — log

The devices lane (ADR-0208; `docs/LANES.md` §4b). Newest entry first.
Device branches start at `origin/main`; win reviews and merges. This log has
its own `devices/log` branch, following `collab/engine.md`, so independent
device PRs do not conflict on one shared log file.

## FOR THE DIRECTOR

**The Live readings — the one list (ADR-0248 d8, d14).** For the director's own Claude session on
the PC with Live 12.4, in one sitting, from win's prompt. This seat does not drive Live. Each item
is one device, one question. Live 12.4 at 48 kHz, a fresh Set, the device dragged from the browser
(its default), nothing else on the track unless said. Bring back the statement (value, conditions,
method); Live's files and renders stay in the director's research.

1. **Saturator, High Quality at Dry/Wet 50 % (DF-E37 a; blocks #659).** Default Saturator (HQ on),
   Dry/Wet 50 %: render an impulse of 0.5, 100 ms. The first 64 samples' values, and the level of a
   10, 15 and 18 kHz sine through it.
2. **Shifter after a pitch move (DF-E37 b; blocks #660).** Pitch mode, Window 80 ms, Dry/Wet 100 %:
   automate Coarse to +7 st for 40 ms then back to 0, then an impulse of 0.5 one second later.
   Where the impulse comes out and at what level (one peak at 1,920, or two of half?).
3. **Simpler's loop through the release (DF-E37 d).** Classic, Loop on, a 2 s sample with Loop
   Length 5 % (100 ms), Release 1000 ms: a note held 1 s then released. Does the output keep
   looping and fade over the second, or stop at the loop's end?
4. **Arpeggiator style order (DEV-19).** A MIDI clip holding C3 E3 G3 B3 for one bar; Clip View,
   Transform, Arpeggiate, Rate 1/16, Steps 0, Gate 50 %: for each of the 18 styles the 16 pitches
   in order. Then C3 E3 G3 B3 D4 for Converge, Diverge, Con & Diverge, the Pinky and Thumb styles;
   and Up with Steps 1, Distance +12.
5. **Simpler's voices, velocity, envelope and filter (DEV-21).** The Voices chooser's list. Classic,
   a 1 kHz sine sample, Vel > Vol 35 %: peak level at velocities 1, 32, 64, 100, 127. Attack 100 ms
   at slope 0, +1, −1: level at 25, 50, 75 ms. Filter on, Clean, LP 12, 1 kHz, Resonance 0 % and
   100 %: noise through it, the level at 500 Hz, 1 kHz, 2 kHz.
6. **Default values not in Live's preset library (DEV-22).** A fresh Redux, Echo, Resonators,
   Multiband Dynamics and Vocoder: every control's value as shown. (Echo's dry impulse is 0.300 and
   Resonators' 0.500 at their defaults, which no Dry/Wet law explains without these values.)
7. **Saturator's Width, Curve and Period (DEV-22).** Waveshaper mode, Drive 20 dB, a 100 Hz sine at
   −6 dBFS: Width 0 / 0.3 / 1, then Curve 0 / 0.05 / 1, then Period 0 / 0.5 / 1, the others at
   default; for each render the first five harmonics' levels.
8. **Shifter's dry path and sweep phase (DEV-22).** Pitch +7 st, Dry/Wet 50 %, an impulse: is the dry
   impulse at 0 or delayed with the wet? Then the first 2,000 samples of the sweep render at +7 st.
9. **Delay with Feedback 0 (DEV-22).** The default Delay with Feedback 0: the six-tone multitone
   (71, 223, 997, 3001, 7001, 11003 Hz) render, to settle its filter's exact shape.
10. **Chorus-Ensemble's Delay Time list (DEV-22).** The chooser's six entries in order.
11. **Envelope Follower and Shaper (DEV-07).** Envelope Follower mapped to a Utility Gain, Rise and
    Fall 0 then default, a 1 kHz burst of 100 ms: the gain over time. Shaper at its default shape,
    1 Hz, mapped the same way: one cycle.
12. **AU05, automation moved or deleted with its material (DF-E38 a; the engine's).** The value
    left inside a source interval a Move or Delete vacates (the held value, the lane default, or a
    new join); how a paste onto another track maps device and send lanes with no counterpart; a
    move across tempo steps between ticks and samples.
13. **Crop (DF-E38 b; the engine's).** Whether the selection is read in source or arrangement
    coordinates; whether Crop keeps the clip's span and moves the source window, or trims the span;
    what becomes of a note crossing the edge (its length and expression).
15. **Reverb's and Hybrid Reverb's controls of another shape (DEV-22).** A fresh Reverb, as shown:
    Spin's Amount; the input filter's Lo Cut and Hi Cut (the band's frequency and width, or its two
    edges); Diffusion and Scale; the Density chooser's entries and which is selected; the Size
    Smoothing chooser's entries and which is selected. A fresh Hybrid Reverb, as shown: Routing,
    Predelay, the synced predelay value, Decay and Size.
16. **Redux's 64 samples (DEV-22).** A fresh Redux, an impulse of 0.5: where it comes out and the
    first 256 output samples. The research's table says Live reports 64; this shows what the 64
    buys (a filter's shape), before ADI copies the delay.
17. **Compressor, the queue's first new effect (DEV-22).** A fresh Compressor: every control as shown
    (Threshold, Ratio, Attack, Release, Auto Release, Output, Makeup, Knee, Lookahead, the
    Peak/RMS/Expand mode, Lin/Log envelope, Dry/Wet). At those defaults: a 1 kHz sine in 6 dB steps
    from -48 to 0 dBFS, 500 ms each, for the static curve; a 1 kHz sine at -6 dBFS switched on for
    300 ms after silence, then off, for attack and release; an impulse of 0.5 for the delay.
14. **(Max, not Live; optional, DEV-16.)** A `[line~]` ramp 0 → 1 over 100 ms from a loadbang: the
    value of its first sample. Two boxes at the same x fed from one outlet: which fires first.

(The Shifter question is answered — Live's way, ADR-0247 d9 — and struck.)

## 2026-11-19 — for win: DEV-22's built devices are at Live's defaults where Live states them; new effects before 3 December?

The lane's ADR-0248 d8 order is done as far as it can go today. (1) DF-E37 and (3) DF-E36 are in
PRs, with AR18 under ASan. (2) OneShot is loadable; the Sampler waits on your word on its sample
path (entry further down: option 1 recommended). DF-E20 (d) and (e) are in a PR. DF-E39 waits for
DEV-17 and DEV-18. (4) DEV-22: every built device whose default Live ships now takes it (Reverb's
and EQ Eight's today). The rest of DEV-22's built devices wait on readings 6 to 10, 15 and 16.

**Question.** DEV-22's row next asks for the queued effects in the manual's order, Compressor first.
None of them is in the session of ADR-0248 d2. The Mix View seat owns the mixer's compressor
functions (DF-M28), so a Live Compressor device would sit beside its Clean compressor. Neither a
default preset nor a render exists for Live's Compressor yet (reading 17).

**Recommendation:** hold new effects until after 3 December, or until reading 17 is in. Meanwhile:
- the Sampler on option 1, the moment you agree it (it is in the session: a keyboard into an
  instrument);
- `devices/pd-exact-position` once `clones/shaper-rollup` lands;
- review hunks for the clones seat (FloatSignals, the count lines).

**What it blocks:** only the order of this lane's next new device. Without your word, this seat
builds Compressor's DSP core and tests (no catalogue entry, no claim of parity), so the device is
ready when reading 17 arrives.

## 2026-11-19 — DEV-22: EQ Eight opens at Live 12.4's default curve

**PR requested:** head `devices/eq-eight-defaults` (0009be42, one commit on bedee5ab), base `main`.
Live 12.4's EQ Eight browser default comes from its default preset in the Core Library, read in
the director's research. It differs from ADI's in 24 stored values; the type numbers follow
Live's menu order, which ADI's menu already has.
- **Curve A.** Band 1 is a low shelf at 30 Hz, bands 2 and 3 bells at 200 Hz and 1 kHz, band 4 a
  high shelf at 5 kHz, all on. Bands 5 to 8 are off, at 100 Hz, 10 kHz and 5 kHz, then a 12 dB high
  cut at 18 kHz.
- **Curve B (L/R and M/S).** Bands 3 to 6 are on, as bells at 100, 500, 2,000 and 10,000 Hz. Bands 1,
  2, 7 and 8 are off: a 12 dB low cut at 40 Hz, a low shelf at 200 Hz, a high shelf at 5 kHz and a
  12 dB high cut at 18 kHz.
- **Already matching:** every gain 0 dB and every Q 0.7071, so the default response stays
  bit-exact unity (tested). Also Stereo, Edit A, Adaptive Q on, Scale 100 %, Output 0 dB and HQ off.
- **Saved projects:** a project saved earlier keeps its own values (ADR-0188 d3). Only a fresh EQ
  Eight opens differently.

The native core and the canvas's `[adi.param]` defaults take the same table; `EqEight::value(n)`
lets a test read a default. The test is first: native and canvas defaults against Live's table,
24 wrong before in each. The Pd suite's peak measurement now sets band 1's type itself.
`adi_eq_eight_tests` 45, `adi_eq_eight_pd_tests` 100; CTest 246/246 (MSVC Debug, /W4 /WX).

Live's Core Library has default presets for ten audio effects. ADI has now taken Auto Filter's,
Chorus-Ensemble's, Delay's, EQ Eight's, Phaser-Flanger's and Reverb's (as far as their controls
allow), Saturator's (on #659; Width, Curve and Period wait on reading 7), and Hybrid Reverb's Dry/Wet. Limiter and Roar are not ADI built-ins. The
devices with no default preset (Compressor, Echo, Redux, Resonators, Multiband Dynamics, Vocoder,
and the queue's new effects) take their defaults from reading 6 and its like.

## 2026-11-19 — DEV-22: Reverb's other defaults, where the control means what Live's does

**PR requested:** head `devices/reverb-defaults` (ee00241c, one commit stacked on `devices/reverb-equal-power`,
69c75a50; review that first), base `main`. The rest of Live 12.4's Reverb browser default, read
from the device's own default state in the director's research. A value is taken only where ADI's
control has Live's unit and range:
- Lo Cut on, Spin on at 0.30 Hz, the high shelf on at 4.5 kHz and 0.70, the low shelf's (off)
  values 90 Hz and 0.75, Chorus on at 0.02 and 0.02 Hz, Predelay 2.5 ms, Decay 1.2 s, Stereo 100.
- Unchanged and confirmed: Hi Cut off, the low shelf off, Size 100, Freeze off, Flat and Cut on,
  Reflect and Diffuse at 0 dB, Dry/Wet 55 %.
- Left at ADI's values: Spin's amount (Live's 17.5 does not fit ADI's 0–5 ms), the input band's two
  frequencies (Live stores a centre of 830 Hz and a width of 5.85), Diffusion and Scale (Live's
  all-pass gain 0.6 and size 0.4), Density (Live's index 1 of a chooser whose entries are not in
  the state). Hybrid Reverb's Routing, Predelay, Decay and Size are in the same position. All of
  them are reading 15 under FOR THE DIRECTOR.

The test is first: `test_reverb.cpp` checks each default against Live's stored value (14 failures before
the fix). The canvas's `[adi.param]` defaults match the native ones. The mechanism tests (Predelay,
Freeze and Cut, Flat) start from the plain tank, with Lo Cut, Spin, Chorus and the high shelf off,
as they did before. The Pd suite's far-bin floor (1e-7) turns Spin and Chorus off: their modulated
delays interpolate above that floor, which is expected, not a fault. `adi_reverb_tests` 51,
`adi_reverb_pd_tests` 43; CTest 245/245 (MSVC Debug, /W4 /WX; the branch's base predates one suite).

## 2026-11-19 — DF-E20 (d) and (e); the clones seat's three items and the count lines

**PR requested:** head `devices/df-e20` (720c4c33, one commit on 12aab48a), base `main`. Two parts of
DF-E20 are this lane's (ADR-0248 d8).

- **(d), the two header comments.** `dsp/spectrum.hpp` now says a full-scale sine reads 0.00 when
  it is *centred on a bin*. Between bins the nearest bin reads lower by the window's scalloping
  loss, the most half a bin off: −1.42 dB for Hann, −3.92 dB with no window. A new test
  (`adi_spectrum_tests`, 107 checks) asserts both at N=1024. It is a comment, so there is no failing
  test first. The new test's figures are what the header now says: Hann −1.42 within 0.01 dB; no
  window −3.909 and −3.936 on the two sides, within 0.02 dB, because the image at −f leaks into
  each side. The other half, `dsp/true_peak.hpp`'s "always at least 176.4 kHz" (it was 128 kHz at
  32 kHz), is corrected on `devices/true-peak` (PR requested earlier), whose factor rule makes the
  comment true: 6x at 32 kHz, 8x at 22.05 kHz. To keep the two from conflicting, it is not
  repeated here.
- **(e), confirmed as built, by design (ADR-0011, ADR-0095).** A patch *device* whose patch failed to
  open is a wire. It passes both channels through sample for sample, at once, in awkward block
  sizes, before and after a prepare at 48 and 44.1 kHz. It reports 0 latency, which is the truth: a
  wire delays nothing, and a nonzero figure would shift every other track against a delay that is
  not there. `openError()` says why it failed. The engine-level test existed; the new
  `testAFailedPatchDeviceIsAWireWithNoDelay` holds the device level (`adi_pd_engine_tests` 641
  checks on main's base). Whether the Devices panel shows `openError()` is the window lane's (no
  change here).
- **Numbers.** CTest 246/246 (MSVC Debug, /W4 /WX).

**AR18 and the whole suite under ASan.** Beyond the Pd engine: every one of the 246 CTest suites was built and run under MSVC ASan the same way. 245 pass. No suite reported a memory error (ASan aborts the suite on the first). The one failure is `adi_changeset_tests`'s the

**The clones seat (ADR-0248 d8).**
- **Count lines.** `main` has 18 entries, 17 available. `clones/shaper-rollup` (ADI-Shaper) and
  `devices/oneshot-instrument` (OneShot as an instrument) each make it 19 and 18. Whichever lands
  second makes it **20 and 19**: `builtinCatalog().size()==20` and `available==19` in
  `test_builtin_devices.cpp`. That branch keeps both branches' `BuiltinInfo` fields after `midi`:
  `instrument` from devices; `usesTransportPosition` and `resetMissingParameters` from clones. The
  entries and `builtinCanvas`'s array stay in the same order, with ADI-Shaper's `adiShaper` and
  OneShot's `oneShot` each at its own entry's index. The entries keep positional initialisers up to
  `midi`, so neither branch's new fields move an existing entry.
- **One pitch kernel: a reason, not a branch.** Shifter's Pitch mode is a phase vocoder: PFFFT,
  windows of 256 to 8,192 points, hop a quarter, matched to Live's Shifter. `AdiShaperPitch` is a
  two-tap cyclic grain delay with a 16-tap windowed-sinc read, matched sample for sample to its
  reference. They share no stage. One kernel would break one of the two matches, so each keeps its
  own. If ADI later wants a time-domain pitch primitive (Beat Repeat's pitch decay, a Grid object),
  it should start from `AdiShaperPitch`.
- **Beat Repeat's history and `AdiShaperTime`.** Yes in principle: one two-channel history ring
  (write, an integer-age read, the oldest readable frame), separate storage per instance, and each
  caller keeping its own slices and phase. It is decided when Beat Repeat starts (DEV-22, after
  3 December), if the three readings the clones seat lists agree. Nothing to number now. The Beat
  Repeat readings join the Live list then, not before, because the list is for the sitting before
  3 December.
- **FloatSignals.** Offered. `adi::device::FloatSignals` (used by `utility~`, `vocoder~`,
  `shifter~`, `multiband_dynamics~`) gives a compiled external float views of Pd's signals in either
  Pd precision, with no allocation in `perform`. `adi_shaper~` may use it as those do; the devices
  lane will review that hunk.
- **"Editable Pd timing cases" (the clones log, 19 November).** Agreed: from the seven fields
  `[adi.transport]` sends (playing, bpm, meter, bar, beat, ticks in the quarter), an editable patch
  cannot recover the quarter. QN 0 and QN 1 in 2/2 send the same fields, and after a 4/4 bar then
  3/4, bar 2 reads as QN 3, where it is QN 4. The answer is a reason and a branch to follow. The fix
  is an optional eighth field, the exact quarter-note position of the first input sample of each Pd
  tick. Existing `[unpack]`s of seven keep working. It needs the graph's exact position per segment,
  which reaches devices through `wantsDeviceTransport` on `clones/shaper-rollup`, not on main. So
  `devices/pd-exact-position` starts when the rollup lands. Its tests are the clones seat's cases:
  QN 0/1 in 2/2, the meter change, a segment at a nonzero buffer offset, upstream latency, and a
  partial tick across a seek and a stop, at awkward block sizes through the engine's adapter.

## 2026-11-19 — DF-E36: a patch cannot close itself, quit, open help, move the search path or write files; AR18 holds under ASan

**PR requested:** head `devices/df-e36` (351917f7, two commits on 12aab48a), base `main`. The lane's
third item (ADR-0248 d8). Each part a failing test first; then the fix in the DEV-09 overlays of
`juce/pd_loader_fix.cmake` (each with its needle check), `juce/pd_engine.*`, `juce/pd_device.hpp`
and `juce/pd_declarations.cpp`.

- **(a) The canvas freed twice.** A `g_editor.c` overlay refuses `menuclose` to a root canvas (force
  0, 1, no argument) and force 2 to any canvas, by name ("ADI refused [menuclose N] (DEV-09)"); force
  0 or 1 to a subpatch, which headless only hides it, stays. `open()` and `close()` also check the
  patch is still in Pd's canvas list before `getdollarzero` and `closefile`. Tests: force 0, 1, 2, no
  argument, the loadbang-only form (inside `libpd_openfile`) and a `[namecanvas]` alias. The unfixed
  suite crashed (access violation) on the first case.
- **The row's "consider menusave and menusaveas":** a real hole, so closed. A patch could write its
  own text to any name in any folder (`[; pd-x.pd savetofile name dir(`: the unfixed test wrote
  `leaked.pd` into another temporary folder), overwrite its own file with what it made of itself at
  run time (`menusave`), or write its data (`write`). A `g_readwrite.c` overlay refuses all three by
  name. `menusaveas` only asks a GUI, of which there is none. Objects that write files (`[text
  define]`, `[array define]`, `[soundfiler]`, `writesf~`) remain the PD4 deny set win owes an ADI
  line for.
- **(b) `menuclose 3`.** Refused in the same overlay; the test runs a second engine whose audio must keep
  flowing, with `pd-A.pd` and `pd-sub` as targets. Plant (force 3
  let through): 3 failures.
- **(c) `help-intro` and `done-popup 2`.** An `s_path.c` overlay of `open_via_helppath` refuses every
  help patch ("ADI refused a help patch: NAME (DEV-09)"); ADI calls it from nowhere, and
  `libpd_openfile` and abstraction loading do not pass there. The test plants `intro-help.pd` and
  `intro.pd` whose `[print leaked]` must not print.
- **(d) The print queue.** Refusals have their own 16-slot MPSC queue, which a flood of `[print]` cannot
  fill; a full console queue writes "[N pieces dropped]" where the gap is. `PdDevice::pumpMainThread`
  now drains the console through a new `PdPatchEngine::pumpConsole()`; pumped every 10 blocks, a
  `[metro 1]` flood drops nothing. AR8's audio-thread allocation count stays at 0. Plant (refusal queue
  off): 1 failure.
- **(e) The search path.** A `g_canvas.c` overlay refuses `declare -path`, `-stdpath`, `-lib` and
  `-stdlib` by name, from the box, the file's `#X declare` record and a run-time message alike; the
  engine's own `libpd_add_to_search_path` is untouched. The static check now reads `#X declare`
  records as `[declare]` boxes. Plant (help and declare overlays off): 2 failures.
- **Numbers.** `adi_pd_engine_tests` 660 checks pass; CTest 246/246 (MSVC Debug, /W4 /WX).

**AR18 under ASan.** The whole Pd engine suite (660 checks) was built with MSVC 14.44's
`/fsanitize=address`. That is a Debug build with `/Zi /Ob0 /Od`, without `/RTC1`, which ASan does not
take; `ASAN_OPTIONS=detect_leaks=0`. It runs clean. With the `#A` unbind in `pd_free` planted out, the
AR18 check fails, and ASan reports the heap use-after-free the overlay exists for: `pd_findbyclass`
(m_pd.c) from `garray_free` (g_array.c:618), on a canvas freed by `glist_clear` from the test's
`clear` message. Restored, it runs clean again. For win: no CI leg runs ASan on Windows. One would
take the two flags above. This is a request only, with no `.github` edit from this lane.

**Next.** DF-E20 (d), the two header comments, and (e), whether a patch device whose patch failed to
open passes audio and reports no latency, now that DF-E36 is in. Then DEV-22, one branch each.

## 2026-11-19 — the Sampler's loadability needs one decision on its sample path (for win)

OneShot plays from a MIDI clip now (entry below). The Sampler is not a canvas: its zones (up to
32, each a sample with key, velocity and select ranges and fades) live in its "component" state,
and #414's slot ops validate a slot against a canvas's `[adi.sample]` declarations
(`sample_slots.cpp`'s `checkedDevice`), so no product call can give it a sample today.
**Two ways, recommendation first:** (1) the Sampler's catalogue entry declares N sample slots
(say 32) through a small canonical declaration text, the slot ops fill them, and each filled slot
is a zone with the full key/velocity range until a zone op (the engine's) edits its ranges —
loadable and audible at once, ranges later; (2) a zone op from the start (engine), the slot ops
untouched. Either way the native Sampler is dispatched directly by `makeBuiltinDevice`. This seat
takes (1) on your word; until then DF-E36 goes first, as ADR-0248 d8's order allows.

## 2026-11-19 — DEV-21: OneShot is a loadable instrument, so a MIDI clip sounds

**`devices/oneshot-instrument` (`0d309d4d`), stacked on #656 (`devices/oneshot-live-params`),
on win's loan of `builtin_catalog.cpp` and `makeBuiltinDevice`'s instrument dispatch.**

- Catalogue: `BuiltinInfo::instrument`; OneShot (`org.adidaw.builtin.oneshot`, Instruments, no
  audio in, MIDI in). **19 entries** with it; **the clones seat's ADI-Shaper makes 20** — whichever
  of us lands second updates the array sizes and `test_builtin_devices`' counts (19→20 entries,
  18→19 available). Clones: please confirm in your log.
- The insert writes subtype `instrument` (`builtin_insert.cpp`); `BuiltinDevice` reports
  `EventFlow::Consume` for an instrument.
- OneShot's canonical canvas is embedded with a native factory (`native_devices.cpp`): canvas
  parameter N → core index N−1 in its units, notes at their block-relative frames, sample from
  slot 1 (#414's slot ops), idempotent prepare.
- **Beyond the named loan, for win's review:** `builtin_samples.cpp` gains OneShot's sample
  (source kept, stereo from the first two channels, at the source's rate, restored through
  `prepareOneShotSample`) — the slot path a OneShot sample needs; `builtin_insert.cpp`'s subtype
  line. Both are small and named in the commit.
- **Test, through the product's calls:** insert on a MIDI track (subtype instrument), assign a
  1 kHz sine to slot 1 (one undo step), ordinary Session, a MIDI clip's C3: sound from frame 1 at
  **0.1256** = 0.5 × OneShot's default −12 dB (Live's). Plant: not marked instrument → 3
  failures. Builtin devices 110; CTest **245/245**.

**PR requested** — title "OneShot is a loadable instrument, so a MIDI clip sounds (DEV-21)";
head `devices/oneshot-instrument` at `0d309d4d`; base `devices/oneshot-live-params` (#656) until it
lands, then `main`. Body: the commit message.

**Next:** the Sampler's loadability — its zones are not a canvas, so it needs its own sample path
(a zone list built from slot assignments); then DF-E36.

## 2026-11-19 — DF-E37 (d) and (f): the looped release; an idempotent OneShot prepare — DF-E37 done

**Both on #656's branch, `devices/oneshot-live-params` (`c8363f0b`, `14181dfb`).**

- **(d) Failing first:** a 2 s 100 Hz sample, Loop Length 5 % (100 ms), Release 1000 ms, note-off
  2.5 s in: 200–300 ms after note-off, rms **0** (stopped at the loop's end). **Fix:** a Classic
  voice with Loop on loops through its release (crossfade included) and ends at zero. Now rms
  0.2123 (expected 0.2121), exact zero after the release, no step above 1.2× the tone's slope;
  Loop off still ends at End; a Sampler zone sounds through a release longer than its loop.
  Live's manual covers Sampler; Simpler is reading 3.
- **(f) Confirmed** (the review's "to confirm"): a same-format `prepare` cut the note. **Fix:**
  early return at an unchanged rate and block size (ADR-0090 d5); `release()` makes the next
  prepare fresh; the Pd external's DSP restart calls `resetVoices()`; the Sampler releases each
  layer before its own fresh prepare (a plant without that fails). OneShot.md updated.
- OneShot 324, Sampler 39; CTest **245/245**. Plants: held-only loop → OneShot and Sampler fail;
  no layer release → Sampler fails.

**DF-E37 is done:** (a) on #659, (b) on #660, (c) on `devices/builtin-real-values` (lands first),
(d) (e) (f) on #656. #656's head is now `14181dfb`.

**Next (ADR-0248 d8 (2)):** OneShot and the Sampler as loadable instruments, on win's loan of
`builtin_catalog.cpp` and `makeBuiltinDevice`'s instrument dispatch; the clones seat adds
ADI-Shaper's entry to the same file, so the count lines are agreed in both logs.

## 2026-11-19 — DF-E37 (e): the Sampler's zone fades are a gain, not a scaled velocity

**On #656's branch, `devices/oneshot-live-params` (`892830ac`).**

- **Failing first, at the default Vel > Vol 35 %:** a linear select fade at w 0.5 played 0.825 of
  the zone; a constant-power one 0.897; a key crossfade's midpoint summed +2 dB.
- **Fix:** the weight is queued on the layer's OneShot (`queueNoteGain`, one per forwarded note-on,
  in order) and latched as the voice's own gain; velocity is left as played. Kept: the velocity
  zone on the unscaled velocity, the selector latch, the laws (ADI's own, labelled), the skip at
  w ≤ 0, no allocation (fixed queue).
- Now 0.5 / 0.707 / 2 × 0.707; velocity 0.5 still plays at 1 − .35 + .35 × .5. Plant: scaled
  velocity → 3 failures. A test-order detail: the Sampler's saved state differs before and after a
  prepare (re-derived zone values); the test saves after a render as it always did — noted, not
  changed here. Sampler 35; CTest **245/245**.

**PR request #656 stands** with this commit (head now `892830ac`); with (c) it waits only for
`devices/builtin-real-values` to land first.

## 2026-11-19 — DF-E37 (c): a built-in's saved values survive a range change

**`devices/builtin-real-values` (`2a4a5044`), from `main`, on win's loan of `builtin_host.cpp`'s
value mapping.** It must land before #656, #659 and #660 (their new ranges).

- The state (still `version: 1`) now also carries `real:{id:value}` and `units:{id:unit}`. On load
  the real value wins while its unit is unchanged: clamped, rounded/snapped, turned back into a
  position in the one `device.loadState`. A unit change falls back to the position. Positions-only
  states keep their positions (pre-release loss, written in ENGINE-BUILTINS.md). The mapping is
  now `juce/builtin_values.*`, tested under two declarations.
- `setParam` honours `value.real` on lin/int/menu/toggle curves only. **For the engine:** the
  parameter mirror's `real` is a linear reading of the position (`param_ops.cpp`'s
  `fillRequest`), wrong on log and exponent curves; honouring it would have set Frequency 1000 Hz
  to about 12.4 kHz on every reopen. So on those curves the position is taken — which means a
  mirror row pushed after a state load (`Session::restoreState` pushes rows whose position
  differs) still moves a log parameter when its range changed. **Request:** compute the mirror's
  `real` with the parameter's curve (the device's own reading, e.g. `getParam(id).real` after the
  set), or compare by real in `restoreState`; then `setParam` can honour real on every curve.
- Tests: Output −12 dB, Frequency 1000 Hz, Window 40 ms come back exactly under the new ranges;
  menu stays; unit change falls back; clamp; refusal leaves nothing to apply; per-curve `setParam`.
  Plant: real ignored → 5 failures. 16 checks; CTest **246/246**.

**PR requested** — title "A built-in's saved values survive a range change (DF-E37 c)"; head
`devices/builtin-real-values` at `2a4a5044`; base `main`. Body: the commit message.

## 2026-11-19 — DF-E37 (b): Shifter returns to a clean unison after any pitch move

**On #660's branch, `devices/shifter-latency` (`7c5d8158`).**

- **Failing first:** +7 st for 1,926 samples and back, then an impulse of 0.5: four nonzero
  samples, half copies around 960 and 2880 (the review's case) — a comb for good.
- **Fix:** at unison with the taps' phase away from rest (0 or 0.5), a 20 ms crossfade
  (`Pitch::kRestSeconds`) from the two taps where they are to the clean Window/2 read, then the
  phase rests; a move away from unison cancels it. No glide, no jump.
- Now: the impulse once, 1920 late, exactly 0.5; a 100 Hz tone through the return steps at most
  1.04× its own steepest slope. Kept: `liveUnison` (240/1920/8400), latency 0, pitch within 0.04
  cents, the +7 st figures, no allocation. Live's own return is reading 2 in the list.
- Plant: no return → fails. Shifter 47; CTest **242/242**.

**PR request #660 stands** with this commit (head now `7c5d8158`). The Shifter question is struck
(answered: Live's way, ADR-0247 d9).

## 2026-11-19 — DF-E37 (a): Saturator HQ no longer comb-filters at a partial Dry/Wet

**On #659's branch, `devices/saturator-latency` (`e539f7dc`).**

- **Failing first:** at 10/50/90 % Dry/Wet the response from 1 to 18 kHz against the 100 %-wet
  one: 1.50 / **12.96** / 1.94 dB off (a 3-sample dry against the 4× minimum-phase wet).
- **Fix:** for 0 < Dry/Wet < 100 % with HQ on, the dry is upsampled by the same filter beside the
  wet and mixed before the down-sampling filter: one phase response for both. 0 % keeps the exact
  3-sample dry (alignment test unchanged); 100 % has no dry. Kept: 3 samples reported, unity DC,
  alias suppression (0.00044), native/Pd parity, no allocation.
- Now **0.000 dB** from the 100 %-wet response at every mix. The resampler's own roll-off (0.9 dB
  at 18 kHz) is reported, not hidden; reading 1 in the list settles whether Live's is the same.
- Plant: the old dry → 3 failures. Saturator 40; CTest **242/242** (that branch's base). The test
  file's comment named a machine; it no longer does.

**PR request #659 stands** with this commit (head now `e539f7dc`).

## 2026-11-19, evening — ADR-0248 read: this lane's order from here

The steering (win's entry of this evening) is this lane's queue now, in its order: (1) DF-E37, each
item a failing test then the fix — (a) Saturator HQ's notch at partial Dry/Wet before #659, (b)
Shifter's lasting comb before #660, (c) saved values under new ranges on win's loan of
`builtin_host.cpp`'s value mapping, (d) the looped release, (e) the Sampler's fades and velocity,
(f) OneShot's idempotent prepare; (2) OneShot and the Sampler as loadable instruments on win's loan
of `builtin_catalog.cpp` and `makeBuiltinDevice`; (3) DF-E36; (4) DEV-22 where it stands (Auto
Filter, Chorus-Ensemble, Delay, the reverbs and Phaser-Flanger branches above are its last
pieces). The Live readings are now one list under FOR THE DIRECTOR, with the engine's DF-E38.

**Already done, on branches:** DF-E20 (d) — both header comments — is on `devices/true-peak`
(`b5ee5aa6`): `true_peak.hpp` now gives the Annex's rule (6x at 32 kHz, so 192 kHz; never below
176.4 kHz from 22.05 kHz up) and `spectrum.hpp` says a sine half a bin off reads −1.42 dB at 1,024
points. When `devices/true-peak` and the engine's f28eacd0 meet, the double path keeps the
non-finite guard. **DEV-16:** steps 3 and 4 (`[line~]`; the director's device's most-used boxes)
were pushed before the steering arrived (`10fb3b4d`, `607d3d5a` on `devices/amxd-translator`);
DEV-16 stops there until DEV-10 exists. Win may take the PR at step 2 (`fe9fa8af`) and leave the
two later commits on the branch, or take all four.

## 2026-11-19 — DEV-22: Phaser-Flanger notches when all wet, and starts at Live 12.4's default

**`devices/phaser-flanger-defaults` (`837700a1`), from `main`.**

- **Reading:** Live's default is 100 % wet (its preset), and its render notches (997 Hz −8.5 dB,
  averaged over the LFO). ADI's phaser wet was the bare all-pass chain — unit magnitude, so at
  100 % it would only shift phase; ADI's notches came from its 50 % mix. **A real defect, found by
  the default:** moving ADI to Live's 100 % without this would have silenced the effect.
- **ADI now:** the wet is the classic sum, (input + chain)/2; defaults from Live's preset (2 notches
  at 400 Hz, Spread 35 %, Flanger 2.5 ms, LFO 0.149 Hz triangle at 75 %, duty 10 %, LFO 2 at 0.5 Hz,
  envelope on 1 ms / 120 ms, Safe Bass 80 Hz, 100 % wet) and ranges where they mean the same.
  Live's 42 notches are not claimed (ADI's core has 8 stages).
- Tests: comparable defaults equal Live's; LFO held, the default dips 40.8 dB between 200–800 Hz;
  the single-notch oracles restated for the classic sum. The 8.5 dB is not compared (LFO phase at
  capture unknown). Plant: bare all-pass wet → 3 failures. Phaser 30, Pd 41; CTest **245/245**.

**PR requested** — title "Phaser-Flanger notches when all wet, and starts at Live 12.4's default
(DEV-22)"; head `devices/phaser-flanger-defaults` at `837700a1`; base `main`. Body: the commit
message.

## 2026-11-19 — DEV-22: Live's Dry/Wet is sin/cos, confirmed at a second point; both reverbs follow

**`devices/reverb-equal-power` (`69c75a50`), from `main`.**

- **Reading (DV-09):** Hybrid Reverb's dry impulse is 0.7071 at its default 50 %; **Reverb's is
  0.64945 at its default 55 %** = cos(0.55·π/2) — which settles the curve (the square-root
  equal-power law gives 0.6708). With Chorus-Ensemble and Delay, four Live 12 devices measured, one
  law.
- **ADI now:** Reverb and Hybrid Reverb crossfade sin/cos (were linear), exact at the ends; Reverb's
  Dry/Wet default 55 % (Live's preset).
- Tests: Reverb's first sample 0.64945 at its default; Hybrid Reverb's dry 0.70711 (IR silent at
  its start). Plant: linear → fails. Reverb 28, Hybrid Reverb 51; CTest **245/245**.

**PR requested** — title "Reverb and Hybrid Reverb mix sin/cos as Live 12.4 measures; Reverb's
default 55 % (DEV-22)"; head `devices/reverb-equal-power` at `69c75a50`; base `main`. Body: the
commit message.

**Not explained by it:** Echo's dry is 0.300 and Resonators' 0.500 at their defaults; their default
Dry/Wet values are not in Live's preset library (the corpus Set deleted each device after
rendering), so they wait for a reading (recipe 5's kind: one look at a fresh device).

## 2026-11-19 — DEV-22: Delay's loop filter is 12 dB/oct, and its default is Live 12.4's

**`devices/delay-defaults` (`92511657`), from `main`.** Third of the corpus devices.

- **Reading (DV-09 default renders, 48 kHz, 120 BPM):** first output sample 0.7071 of an impulse
  (dry, equal-power at 50 %). Six tones (dB/°): 71 −8.53/−27.7, 223 −11.16/+26.9, 997 +2.58/+44.1,
  3001 −9.60/−37.9, 7001 −11.39/−24.5, 11003 −3.23/−52.3.
- **Method:** with Live's default (3/16 = 18 000 samples, Feedback 0.5, Dry/Wet 0.5), each tone
  was solved for the loop filter F under three topologies (filter in the loop, in the feedback
  only, after the loop) and two mix laws. Only in-loop + equal-power gives a clean filter: ~0 dB in
  band, +79° and −2.1 dB at 71 Hz — a second-order high-pass at 1 kHz/2⁴ = 62.5 Hz to 0.05 dB and
  0.5° (a one-pole gives +41°), and a second-order low-pass near 16 kHz.
- **ADI now:** Butterworth TPT high-pass and low-pass in the loop at f·2^(∓W/2) (was one-poles);
  defaults from Live's preset — synced 3/16 (right stores 4 behind Link), 375 ms free, Feedback
  50 %, Filter on, 1 kHz (50..18000 Hz), Width 8 oct, LFO 0.5 Hz triangle, Dry/Wet equal-power.
- Test: first sample 0.70711; six tones measured over the **capture's own signal and window**
  (the probe is six −30 dBFS sines from phase 0; the ratio is taken 1–3 s after onset): within
  **0.30 dB / 1.9°** (bound 0.35 dB / 2°). An exact model of that window reproduces ADI's deviation
  to the fourth decimal, so the rest is the filter's exact shape: a free fit of both corners and
  dampings reaches 0.08 dB at best and identifies no cleaner design. (A second commit, `82e994bb`
  on the same branch, corrects the first's "unsettled capture" explanation.) Oracles that assumed
  the old defaults state their mechanics. Plant: no high-pass → 2.7 dB / 71° off. Delay 93, Pd 44;
  CTest **245/245**.

**PR requested** — title "Delay's loop filter is 12 dB/oct and its default is Live 12.4's
(DEV-22)"; head `devices/delay-defaults` at `82e994bb`; base `main`. Body: the commit message.

**Reading that would settle the decimals:** the six tones with Feedback 0 (the filter alone, once).

## 2026-11-19 — DEV-22: Chorus-Ensemble mixes equal-power, as Live 12.4 measures

**`devices/chorus-ensemble-defaults` (`b285030e`), from `main`.** Second of the corpus devices.

- **Reading (DV-09, default render, impulse 0.5 L / −0.25 R):** the first output sample is
  0.7071 of the input on both sides; the response sums to 1.4211. At Live's default Dry/Wet 50 %
  that is the dry path at an equal-power crossfade. ADI's was linear (0.5).
- **ADI now:** Dry/Wet as cos/sin, exact at both ends, Vibrato all wet; Live's defaults and
  ranges where the control means the same (High-Pass 50 Hz, Rate 0.1..15 Hz at 0.9, Feedback
  0..99 %, Output to +6 dB). Only the 50 % point is measured — the curve between is the common
  equal-power law, not read. Not mapped: Live's Delay Time chooser (6 entries, default index 3;
  its list is a reading). The six tones are LFO-phase dependent and are not compared.
- Tests: first sample 0.70711 (Live's float gain differs by 1.1e-6), sum 1.4143 vs 1.4211; the Pd
  in-phase oracle now √2. Plant: linear law → 2 failures. Chorus-Ensemble 27, Pd 26; CTest
  **245/245**.

**PR requested** — title "Chorus-Ensemble mixes equal-power as Live 12.4 measures, with Live's
defaults (DEV-22)"; head `devices/chorus-ensemble-defaults` at `b285030e`; base `main`. Body: the
commit message.

**A question this raises (win, or the next reading):** if Live's Dry/Wet is equal-power in its
newer devices, the same may hold for Delay (its impulse also starts at 0.7071 at 50 %) — checked
next with Delay's own numbers, not assumed.

## 2026-11-19 — DEV-22: Auto Filter's default is Live 12.4's and nulls its measured response

**`devices/auto-filter-defaults` (`6521366e`), from `main`.** First of the corpus devices in the
manual's order (the DV-09 entry further down), from readings already taken — no new Live.

- **Reading (DV-09, the research's default renders, 48 kHz):** Auto Filter's browser default (LP,
  24 dB, 10 kHz, Resonance 0, Clean; values from Live's own default preset, read privately) gives
  71/223/997/3001/7001/11003 Hz at −0.0006/−0.0063/−0.1255/−1.1305/−6.0077/−14.5236 dB, phase
  −1.39/−4.36/−19.47/−58.15/−130.93/+164.70°; impulse peak +2 samples at 0.2942.
- **The fit:** two cascaded TPT state-variable low-passes, k = 2 (Q 0.5 each), cutoff prewarped
  — to four decimals. That is ADI's Clean core already; ADI's default was 1 kHz, 29 %, 12 dB.
- **ADI now:** Live's defaults, and Live's ranges where the control means the same — Frequency
  20..20000 Hz (10 kHz), Resonance 0, Slope 24 dB, envelope Attack 1 ms (to 100 ms) and Release
  250 ms, LFO 0.1..60 Hz, Steps 2..64, sidechain EQ 30..15000 Hz at 200 Hz, Q 0.1..12, ±15 dB.
  Not claimed: Live 12's separate 12/24 and 6–48 Morph slopes, LFO time modes, other circuits,
  the Resonance law beyond 0 (only k = 2 at 0 is measured).
- Test: six tones within **0.00024 dB / 0.0014°**, impulse peak +2 at 0.2942. Plant: the old
  12 dB default → 3 failures. Auto Filter 166 checks; CTest **245/245**.

**PR requested** — title "Auto Filter's default is Live 12.4's, and nulls its measured response
(DEV-22)"; head `devices/auto-filter-defaults` at `6521366e`; base `main`. Body: the commit message.

**Next:** the same for Chorus-Ensemble, Delay, Echo and on down the corpus list.

## 2026-11-19 — the readings this lane waits on, as recipes (FOR THE DIRECTOR item 2)

One device, one question each, Live 12.4 at 48 kHz, a fresh Set, the device from the browser (no
preset), nothing else on the track. What comes back here is the statement (value, conditions,
method); files stay in the director's research.

1. **Arpeggiator style order (DEV-19, XC-25).** A MIDI clip with C3 E3 G3 B3 held one bar; Clip
   View → Transform → Arpeggiate, Rate 1/16, Steps 0, Gate 50 %. For each of the 18 styles, the
   16 note pitches in order (or the Set saved and its notes read). Also, with C3 E3 G3 B3 D4 (five
   notes), the Converge/Diverge/Con & Diverge/Pinky/Thumb styles; and Up with Steps 1, Distance
   +12, to see whether the transposed copy follows the whole pattern.
2. **Simpler voices and velocity (DEV-21).** The Voices chooser's full list. Then Classic mode, a
   1 kHz sine sample, Vel > Vol 35 %: render one note at each velocity 1, 32, 64, 100, 127 and
   read the peak level of each.
3. **Simpler envelope slope (DEV-21).** Attack 100 ms, slope 0 and slope +1 and −1: render one
   note of a constant-level sample and read the level at 25, 50 and 75 ms; Decay 100 ms to
   sustain −24 dB the same way.
4. **Simpler filter (DEV-21).** Filter on, Clean, LP 12, cutoff 22 kHz then 1 kHz, Resonance 0 %
   and 100 %: white noise through it, the magnitude at 500 Hz, 1 kHz, 2 kHz (a spectrum read).
5. **Redux defaults (DEV-22).** A fresh Redux: every control's value as shown.
6. **Saturator's Width, Curve, Period (DEV-22).** Waveshaper mode, Drive 20 dB, a 100 Hz sine at
   −6 dBFS: render with Width 0 / 0.3 / 1 (others default), then Curve 0 / 0.05 / 1, then Period
   0 / 0.5 / 1; nine renders, each read as its first five harmonic levels.
7. **Shifter's dry path and sweep phase (DEV-22).** Pitch mode +7 st, Dry/Wet 50 %, an impulse:
   is the dry impulse at 0 or delayed with the wet? Then the sweep render that DV-06 used, at
   +7 st, its first 2,000 samples.
8. **Envelope Follower and Shaper (DEV-07, DEV-16).** Envelope Follower mapped to a Utility
   Gain, Rise 0, Fall 0 then defaults, a 1 kHz burst of 100 ms: the mapped value's curve (Utility
   output level over time). Shaper: its default shape at 1 Hz mapped the same way, one cycle.
9. **Max's own `line~` start and `[t]` tie order (DEV-16; Max, not Live).** A `line~` ramp
   0 → 1 over 100 ms from a loadbang: the value at the first sample. Two boxes at the same x fed
   from one outlet: which fires first.

## 2026-11-19 — DEV-16, step 4: what the director's own device uses most

**`devices/amxd-translator` (`607d3d5a`), one more commit on the same PR request.** Chosen by the
box counts of his `AVC Spectrum Meter.amxd` (the entry below on step 3).

- `[prepend]`/`[append]` (list then `[list trim]`, so the first word is the selector, as Max);
  `[f]`, `[i]`, `[delay]`, `[abs~]`; `[!- n]` as (−x) + n, int unless n is a float; `[gate n]` as
  a prefix and a `[route]`; `[selector~ n]` as one `[*~]` per input, summed; `[slide~]` as its own
  recursion in `[fexpr~]`; **`[biquad~]`**: Max's a0 a1 a2 b1 b2 subtract the feedback, Pd's
  fb1 fb2 ff1 ff2 ff3 add it — arguments reordered, a coefficient list reordered and negated on the
  way in (a control source into its left inlet takes that route, a signal goes straight in);
  `live.text` (toggle), `live.menu`, `live.tab` as parameters.
- Played in libpd: `[prepend set]` 3 → "set 3"; `[i]` 2.9 → 2; `[!- 10]` 3.5 → 7, `[!- 10.]` →
  6.5; `[gate 2]` passes 5 out of outlet 2; `[selector~ 2 1]` passes input 1 exactly; `[slide~ 10
  10]` on a step is 1 − 0.9^(n+1) sample for sample; Max's `biquad~` "1 0 0 −0.5 0" starts at 1,
  settles at 2. Plant: feedback not negated → 0.667, fails. 60 checks; CTest **247/247**.

**PR requested** — as step 1's entry (head `devices/amxd-translator`, now `607d3d5a`).

## 2026-11-19 — DEV-10: the three starter templates, proved

**`devices/pd-templates` (`bc3a84de`), stacked on `devices/pd-object-walk`.** M7's "Pd templates"
(the lane table) — DEV-10's "three starter templates".

- `pd/templates/`: **Audio Effect** (`adc~ 1 2` → Gain in dB, smoothed 20 ms → `dac~ 1 2`),
  **Instrument** (`notein` → `[poly 1 1]` → one sine voice, 5 ms rise to the velocity, Release
  fall), **MIDI Effect** (`notein` → Transpose → `clip 0 127` → `noteout`). Vanilla plus one
  `[adi.param]` each, with a comment on where the ports are and what to replace.
- Tests: each parses with no problem, walks clean (PD4) against the running build, opens with
  nothing in the console; the audio effect is the input bit for bit at 0 dB (64 late) and plays
  −6.000 dB when the host sends `$0-adi-1`; the instrument plays A4 at 0.25 peak at full velocity
  and is silent after Release. Plant: a cyclone object → walk and console both fail. 18 checks;
  CTest **247/247**.

**PR requested** — title "DEV-10's three starter templates, proved to parse, walk, open and
play"; head `devices/pd-templates` at `bc3a84de`; base `devices/pd-object-walk` until it lands,
then `main`. Body: the commit message.

**For the engine/window (DEV-10):** offering them is the new-device flow's (identity
`org.adidaw.pd.<kind>`, the 'patch' role with the template's text). The MIDI effect's output is
proved once DEV-14's `midiOut` (`devices/pd-midi-out`) lands.

## 2026-11-19 — DEV-16, step 3: `[line~]`'s chained segments

**`devices/amxd-translator` (`10fb3b4d`), one more commit on the same PR request.**

- Max's `[line~]` plays a list of (target, ms) pairs one after another; Pd's `[vline~]` reads a
  third number as a delay, so a Max list sent straight to it is wrong. `[line~]` becomes a
  `[pd max-line~]` that walks the pairs and schedules each as "target ms delay" with the delay
  accumulated; the first goes with delay 0, which in `[vline~]` cancels what was still scheduled,
  as a new list does in Max. Its ramp-time inlet, end bang and an initial value are refused.
- Played in libpd: "0 0 1 100 0 200" → 0, 0.5, 1, 0.5, 0 at 0, 50, 100, 200, 300 ms within 1e-3.
  `[vline~]` leads the expected ramp by one sample; Max's own `line~` timing is a reading to take
  (Max, not Live). Plant: no accumulated delay → 3 failures. 45 checks; CTest **247/247**.

**PR requested** — as step 1's entry (head `devices/amxd-translator`, now `10fb3b4d`).

**The one real device in reach** is the director's own, in this repository: `adi-m4l-analyzer`'s
`AVC Spectrum Meter.amxd` (plain `ptch`, 541 boxes). It is an analyser on Jitter, `js` and the Live
Object Model (`jit.*`, `js`, `jsui`, `live.object`/`path`/`observer`), so it is the AI path's case,
not the translator's. Its most used boxes set the order for later steps: `prepend` (63), `sel`
(14), `live.menu` (14), `snapshot~` (13), `expr` (12), `biquad~` (10), `live.text` (10), `atodb`,
`change`, `route`, `slide~`, `gate`, `maximum`, `average~`, `selector~`, `delay`, `!-`. Several
need Max's own behaviour read first (int or float in `sel`/`change`, `atodb` at 0, `biquad~`'s
coefficient order) — Max, not Live.

## 2026-11-19 — DEV-16, step 2: Max's integers and subpatchers, made explicit

**`devices/amxd-translator` (`fe9fa8af`), one commit on top of step 1** — the same PR request now
carries both steps.

- Each Max box now maps to a fragment (Pd boxes, their inner connections, where the Max inlets and
  outlets landed), so a Max/Pd difference is written into the patch instead of refused.
- **Integers**: `[+ 1]`, `[/ 2]`, `[% 3]`, comparisons, `[min]`/`[max]`/`[abs]` with an int or no
  argument, `[pack]`/`[unpack]` (default "0 0", and `i` slots), `[trigger]`'s `i`, the number box —
  each int inlet through `[i]` first; integer division and int outlets through `[i]` after. Float
  forms unchanged; float modulo refused.
- **Subpatchers**: `[p name]` → `[pd name]`, recursively; ports keep Max's left-to-right order and
  are `[inlet~]`/`[outlet~]` where a signal crosses; a refusal inside is named by path
  (`p helper/obj-1`).
- Played in libpd: `[+ 1]` fed 2.7 → 3; `[/ 2]` fed 7 → 3; `[> 0]` fed 0.5 → 0; `[pack]` fed 2.9
  → 2 0; `[unpack]` fed 3.7 4.2 → 4 then 3; `[t i f]` fed 2.5 → 2.5 then 2; a signal `[p half]`
  plays × 0.5 exactly; a control `[p inc]` fed 1.5 → 2. Plant: int inlets bypassing `[i]` → 3
  failures. 38 checks; CTest **247/247**.

**PR requested** — as step 1's entry (head `devices/amxd-translator`, now `fe9fa8af`).

**Still to do in DEV-16:** `line~` as segments, bpatchers, MIDI devices (after DEV-14/18), the AI
path for the rest, Suite's modulators (after DEV-07), parity against Live (readings).

## 2026-11-19 — DEV-16, step 1: the `.amxd` translator — reader, pre-flight, the exact subset

**`devices/amxd-translator` (`70c63694`), stacked on `devices/pd-object-walk`.** P2, taken now
because every M-row left in this lane waits on a reading, an ADR or another seat. It needs DEV-10
to become a device; the translation itself needs nobody.

- `translateAmxd(bytes)` (`src/juce/pd_amxd.*`, no libpd): reads the `ampf`/`meta`/`ptch`
  container; Live's encrypted (`ciph`) devices and frozen (`mx@c`) devices are not read. Audio
  effects only in this step.
- **The pre-flight is the translation table**: a Max box translates only where Pd behaves the
  same — signal arithmetic (scalar or signal right inlet chosen from what feeds it), `cycle~`
  (not its phase inlet: an offset in Max, a reset in Pd), `phasor~`, `sig~`, `plugin~`/`plugout~`
  → `adc~`/`dac~`, `loadbang`, `live.thisdevice`'s load bang, `metro` in ms, `trigger` b/f/l/s,
  float arithmetic only (Max's `[+ 1]` is integer), `send`/`receive` (`---x` → `$0-x`), `print`,
  messages (escaped), flonums, comments. `live.dial`/`live.numbox`/`live.slider` → `[adi.param]`
  with range, default, unit and curve. Everything else is refused, box by box, with its reason;
  the result must then pass PD4's walk.
- **Max's right-to-left order made explicit**: each control fan-out goes through `[t a ...]`, its
  right outlet to the rightmost box. Equal x: lower box first — an assumption; Max's tie rule is a
  reading to take (Max, not Live; not run here).
- Tests (every device written by the test): container cases; a gain device and a `live.dial`
  device translate, walk clean against the running build, open in libpd and play input × 0.5 and
  × 0.25 **exactly** at the reported 64 samples; a fan-out prints right before left in Pd. Plant:
  no trigger → 3 failures, one heard at run time (Pd alone fires left first). 25 checks; CTest
  **247/247**.

**PR requested** — title "The .amxd translator, step 1: reader, pre-flight and the exact subset
(DEV-16)"; head `devices/amxd-translator` at `70c63694`; base `devices/pd-object-walk` until it
lands, then `main`. Body: the commit message.

**Still to do in DEV-16:** step 2 (integer arithmetic made explicit with `[i]`, `pack`/`unpack`,
subpatchers as `pd` canvases with `inlet~`/`inlet` chosen by what crosses them, `line~` as
segments); MIDI devices after DEV-14/DEV-18; the AI path for the rest (Propose tier, engine);
Suite's LFO/Envelope Follower/Shaper after DEV-07. The parity test against Live needs readings.

## 2026-11-19 — PD4: the allowed-object walk, with the vocabulary read from the running build

**`devices/pd-object-walk` (`2d949a65`), from `main`.** PD4 serves DEV-09, DEV-10, DEV-11 and
DEV-16; it is this lane's piece of DEV-10 (M7) that needs nobody else.

- `walkPdObjects(patchText, abstractions, vocabulary)` (`src/juce/pd_object_walk.*`, no libpd,
  every ABI): names each box this build cannot make by file (the patch or an abstraction),
  subpatch path, class and object-box number (counted as `parsePdDeclarations` counts). Subpatches
  walked in place; stored abstractions (DEV-10's 'abstractions' role), named as `[voice]` or by
  `[clone]`'s argument (Pd 0.56's own argument rule), walked once each. Refused: unknown class,
  a `$` class name (only known when Pd runs), an abstraction containing itself, nesting past 64,
  a canvas that does not close. `describe()` gives one line per refusal for the device's
  problem list.
- `PdRuntime::creatableClasses()` (`pd_vocabulary.cpp`) reads `pd_objectmaker`'s creators under
  instance 0's lock: the method table **plus the bang/float/symbol/list slots**, where
  `class_doaddmethod` files those selectors (read in m_class.c; the first version, table only,
  refused `Resonators.pd` for its `[list]` boxes — the test caught it). 329 names, ADI's
  compiled-in externals included; no list kept by hand.
- One tokeniser: `pdRecords` is the declaration parse's scanner made public; both read it.
- Tests: walk cases on a hand-made vocabulary; the build has vanilla's objects and ADI's
  externals and no cyclone/zexy; **all 27 shipped patches walk clean**. Walk 77; declarations 76
  unchanged; CTest **246/246**.

**PR requested** — title "The allowed-object walk, with the vocabulary read from the running
build (PD4)"; head `devices/pd-object-walk` at `2d949a65`; base `main`. Body: the commit message.

**For win (a ruling, not the director's unless you say so):** the vocabulary is the build's, so
it includes vanilla's `[netsend]`, `[netreceive]`, `[oscparse]`, `[file]`, `[textfile]`,
`[soundfiler]` (its `write`), `[writesf~]` and `[pdcontrol]`. A user patch (DEV-10) or a
translated `.amxd` (DEV-16) can therefore open sockets and write files. Recommendation: the walk
takes a deny set from one ADR line — network and file-writing classes refused in user patches
unless the user allows them per device — and the fence stays the build's for natives (ADR-0188
d8). Blocks nothing now: the walk has no caller until DEV-10's `loadState`.

**Next:** DEV-10's other devices piece, the three starter templates, waits on its identity and
state roles (the engine's). Then DEV-16's `.amxd` reader on top of this walk (P2, after DEV-10).

## 2026-11-19 — taking Live 12.4: Arpeggiate's style order (DEV-19 / XC-25), MIDI only

One device, one question: the step order each of the 18 Arpeggiator/Arpeggiate styles gives for a
held 4-note chord (and 5 notes where the middle is ambiguous), with Steps/Distance for transposed
repeats. engine-win's log checked first: its current work is the scanner and export, no ASIO run.
No audio is rendered; the reading stays in the director's research, the measured orders come here.

**Not taken:** control of Live was declined on this machine when asked. Nothing was opened or
changed. The 18 style orders stay a reading to take (Converge, Diverge, Con & Diverge, the Pinky and
Thumb styles, and the endpoints of UpDown vs Up & Down are not defined by the manual); the
ordering function waits for it, and DEV-19 waits for DEV-17/18 anyway.

## 2026-11-19 — DEV-28, step 3: the meter agrees with libebur128 to 0.0000 LU; Main's CPU cost

**`devices/loudness-oracle` (`94cfc664`), stacked on `devices/true-peak`.**

- `tests/test_loudness_oracle.cpp`: pink noise, a speech-like signal with pauses, a 20 Hz–20 kHz
  sweep and 5.1 pink noise (BS.1770 surround weights, LFE out) through `dsp::LoudnessMeter` and
  libebur128 1.2.6, at 44.1, 48 and 96 kHz. Integrated, max M, max S and the last M and S agree to
  **0.0000 LU** in every case; the suite holds them to 0.01 (the plan's gate is 0.1).
- LRA: libebur128 hops its short-term window at **1 Hz** (3 s every second; read in its source),
  Tech 3342 at 10 Hz. ADI's own short-term readings taken on the 1 Hz grid and ranked libebur128's
  way agree to 0.0000; ADI's 10 Hz LRA differs from libebur128's by up to 0.45 LU (the sweep), both
  correct for their grid. Held to 0.01 and 0.5. True peak, two interpolators: worst 0.11 dB, held
  to 0.2.
- Test-only: built only when `third_party/libebur128` is fetched, never linked into the app;
  `ADI_REQUIRE_LOUDNESS_ORACLE=ON` makes its absence a configure error. Plant: the relative gate
  at −9.5 → 3 failures. Oracle **96** checks.
- **Main's CPU cost** (the gate asks for it), `/O2`, one thread, 512-frame blocks, stereo, on an
  8-core desktop CPU: 48 kHz **0.33 % of one core** (35.6 µs a
  block), of which true peak 0.29 %; 96 kHz 0.41 %. `readings()` 0.01 µs. libebur128 doing the same
  (I, LRA, TP, histogram mode): 0.46 % and 0.71 %. Method: 120 s of noise, best of 5.

**PR requested** — title "The loudness meter against libebur128, test-only (DEV-28, MX-27)"; head
`devices/loudness-oracle` at `94cfc664`; base `devices/true-peak` until the stack lands, then
`main`. Body: the commit message plus the CPU paragraph above.

**For win — the pin (LD3 says you write the row, I propose it):** `jiixyj/libebur128 libebur128
MIT v1.2.6 67b33abe1558160ed76ada1322329b0e9e058b02`, role `build` so CI's `--build-only` fetch
runs the oracle, plus its `EXTERNAL-CODE.md` row (test-only, never shipped). Until then the suite
runs where it is fetched. ffmpeg's `ebur128` (LD6) stays optional, Linux, behind
`ADI_REQUIRE_ORACLES`; not done here.

## 2026-11-19 — DEV-29 (c): the true-peak brickwall in `LookaheadLimiter`

**`devices/limiter-true-peak` (`d9f8e4d0`), stacked on `devices/true-peak`** (it uses the meter's
interpolator).

- `setTruePeak(bool)`, **off by default**: the detector is `TruePeakMeter`'s own interpolator
  (now `TruePeakMeter::design`/`factorFor`, so a limited signal reads at the ceiling on the
  meter), upsample-and-hold, and each audio sample's target is the ceiling over the largest
  interpolated value within 2 samples of it. Latency = lookahead + `truePeakDelay()` (8 + 2 at
  every oversampled rate, 0 from 176.4 kHz where the Annex oversamples by 1), reported, whole
  samples. `process()` for double as well as float; the delay line is double either way.
- Measured, on the quarter-rate sine at 45° (+6 dBTP), 11 kHz + 997 Hz, hard-gated quarter-rate
  bursts, +12 dB noise and +12 dB doublets, at 44.1/48/96 kHz: every sample at or under the
  ceiling exactly; true peak over the ceiling under 0.001 dB with a lookahead of 64 or more
  (worst 0.0003, noise) and under 0.05 dB with none (worst 0.025, noise). Without the 2-sample
  spread the noise residue is 0.28 dB, so the spread stays. The old sample-peak mode leaves
  +2.96 dB on the quarter-rate sine, asserted.
- Below the ceiling in true peak: the input, exactly `latencySamples()` late, bit for bit, float
  and double. Plant: detection back to sample peak -> 47 failures. DSP **262** (was 151); CTest
  **243/243**.

**PR requested** — title "A true-peak brickwall in LookaheadLimiter, off by default (DEV-29 c,
EX-05 LD5)"; head `devices/limiter-true-peak` at `d9f8e4d0`; base `devices/true-peak` until the
stack lands, then `main`. Body: the commit message.

**For the engine (DEV-29 c's other half):** the refine loop (≤ 4 trials, ±3 dB steps, 0.02 LU
plateau) inside `NormalizationCapture`, with the encoded true-peak check after dither as the final
gate, is yours; turn the brickwall on with `setTruePeak(true)` and report `latencySamples()`.

## 2026-11-19 — DEV-14, step 1: a patch's MIDI output comes back on its frame; the host's reset bang

**`devices/pd-midi-out` (`b046391d`), stacked on `devices/pd-block-quantum`.**

- libpd's per-instance MIDI-out hooks fill a fixed array inside the locked tick; each message is
  stamped with where its tick's audio comes out plus Pd's logical time within the tick, and
  `process()` hands out those due in its segment (`LibPdEngine::midiOut()`, block-relative
  frames). MIDI 1 by construction. Overflow (512 a call) is flagged for the host to release
  everything, and counted.
- `hostReset()`: a bang to `$0-adi-reset` before the next tick; `PdDevice::prepare` sends it;
  `PdDevice::hostReset()` is there for stop, bypass and panic.
- Tests (AR9's timing half): `[notein] -> [+ 12] -> [noteout]` at the input's frames; 64 later
  through the one-block adapter; `[pipe 101]` exactly +4,848, 48 samples into a tick; 600 at once
  -> 512 back, flagged, 88 counted; the reset arrives. Plant: no logical offset fails the pipe
  case. Pd engine **648**; CTest **242/242**.

**PR requested** — title "A patch's MIDI output comes back on its frame; the host's reset bang
(DEV-14 step 1)"; head `devices/pd-midi-out` at `b046391d`; base `devices/pd-block-quantum` until
the stack lands, then `main`. Body: the commit message.

**What DEV-14 still needs from others:** win's ADR (R4, the row says one comes first: kinds,
Consume, `[clone]` voices with the MPE member channel as the voice index, precision); the engine's
DEV-17 (decode `midiOut()` under the device's note prefix, unique ids) and DEV-18 (MIDI-effect
patches in the transform phase); the engine calling `PdDevice::hostReset()` on stop, bypass and
panic. AR9's unique-id half and AR10 (voices, Consume) wait for those.

**Next:** DEV-10 and DEV-15 both wait on the engine and on ADRs (DEV-15: "an ADR answers the gap
review's eight decisions before any Grid-style work"). So queue item 8, this lane's other rows in
milestone order; first the ones that need nobody else.

## 2026-11-19 — DEV-13: user patches add no delay on the 64-sample grid, one reported block otherwise

**`devices/pd-block-quantum` (`83b40061`), stacked on `devices/pd-double` (the DEV-09 stack).**

- `LibPdEngine::BlockMode::Automatic`, to the director's Q-K2 answer: whole Pd ticks with no
  added delay while the buffer (at prepare) and every segment are multiples of 64, each event at
  the start of its tick; a buffer that is not a multiple runs one reported, compensated block;
  a segment inside a tick arriving later falls back to that block once (counted; the adapter's
  latency epoch moves, and `PdDevice::latencyEpoch` now includes it, so the graph re-compensates;
  never silenced). `segmentQuantum()`: 64 for Pd in WholeBlocks/Automatic, 1 otherwise.
- Tests: the segment contract (buffers 64/128/256, events at 1, 63, 64, 65, 127, kept on the
  grid) is an exact null with no delay; 511 is the input 64 late from the start; AR5's 1 + 255 on
  the grid nulls, inside the tick falls back once and runs continuous at 64. Plant: Automatic as
  Buffered fails all four. Pd engine **641**; CTest **242/242**.
- The research's own pd-segment-contract specification (tests/research_intake) is not on main
  yet; when engine-win's PR lands, its numbers become this test's data.

**PR requested** — title "User patches add no delay on the 64-sample grid, one reported block
otherwise (DEV-13, Q-K2)"; head `devices/pd-block-quantum` at `83b40061`; base `devices/pd-double`
until the DEV-09 stack lands, then `main`. Body: the commit message.

**Request to the engine (DEV-13's other half, the graph and `pd_patch_host`):** (1) when a
device's `segmentQuantum()` is above 1 (`PdDevice::segmentQuantum()`; a `DeviceInstance` virtual
is the engine's call), keep that node's block split only at multiples of it, and hand each
segment the events falling inside it (they reach Pd at the tick's start, as now); (2) construct
user patches with `BlockMode::Automatic`. Until then a user patch in Automatic still never goes
silent: the first off-grid split costs one counted fallback to the reported block.

**Next:** DEV-14 (Pd note output, voices and instrument kinds).

## 2026-11-19 — DEV-28, step 2: true peak on every channel, from double too; DF-E20 (d) closed

**`devices/true-peak` (`b5ee5aa6`), stacked on `devices/loudness-meter`.**

- `TruePeakMeter::prepare(rate, channels)`: any channel count (default 8, as before); the
  loudness meter's true peak now covers every channel. A double-input path, reading exactly
  what the float path reads for float-exact input.
- **DF-E20 (d), both halves, in this lane's files** (the register names the engine seat; the
  files are `src/adi/dsp`): the oversampling factor is now the smallest whole number reaching
  176.4 kHz (6× at 32 kHz, 8× at 22.05 kHz; 44.1-192 kHz unchanged), so `true_peak.hpp`'s
  promise holds and is tested; `spectrum.hpp` now says a full-scale sine reads 0.00 when centred
  on a bin and less between bins by the window's scalloping loss. **win:** DF-E20 (d) can move to
  Done with this PR.
- Auto Gain Stage: `dsp::AutoGain` is ADR-0166's per-effect auto gain (a 1 s window by design),
  not an integrated reading, and stays. The Auto Gain Stage's LUFS mode is
  `src/adi/engine/gain_stage.cpp` (engine): **request to the engine** to read it from
  `dsp::LoudnessMeter` with N13 and export normalization.
- dsp **162**, loudness **62**; plant (the old factor rule) fails the new checks; CTest **243/243**.

**PR requested** — title "True peak on any channel count, from double too, and at least
176.4 kHz at every rate (DEV-28, DF-E20 d)"; head `devices/true-peak` at `b5ee5aa6`; base
`devices/loudness-meter` until it lands, then `main`. Body: the commit message.

**Next:** queue item 7, the Pd tier for the Grid: DEV-13 (a patch's delay when blocks do not
divide by 64), on the DEV-09 stack.

## 2026-11-19 — DEV-28, step 1: the one loudness meter, proved against Tech 3341/3342 and a brute-force reference

**`devices/loudness-meter` (`a3d4dfac`), from `origin/main`.** New: `src/adi/dsp/loudness.*`,
`cmake/devices/loudness.cmake`, `tests/test_loudness.cpp`.

- **The meter** follows the row's contract: K-weighting (AutoGain's BS.1770 prototypes at any
  rate); 10 ms sub-blocks (boundaries at round(k·rate/100)); M over 40, S over 300, both and
  their maxima every sub-block, -inf until filled; a 400 ms gating block and a 10 Hz
  short-term value every 100 ms above -70 LUFS; histograms as **Fenwick trees** over 0.01 LU
  bins (-70..+20 LUFS) with counts and exact energy sums, so I and LRA are recomputed every
  100 ms in O(log n) and nothing walks the bins on the audio thread; LRA by Tech 3342's MATLAB
  rule; run and pause (no window holding paused audio feeds I, LRA or the maxima after
  resuming); true peak via `TruePeakMeter` (≤ 8 channels); float and double input; explicit
  channel weights.
- **Proof (62 checks):** EBU Tech 3341 (2023) cases 1-6 (48 kHz; 1 and 5 also at 44.1 and
  96), 9, 12, 10, 13, 11, 14, 15-23 (20-23 synthesised at 4 fs, low-passed, decimated at
  offsets 0-3); Tech 3342 (2023) cases 1-4 (48 kHz; 1 also at 44.1 and 96). Cases 7, 8 (3341)
  and 5, 6 (3342) are the EBU's programme files and are not used. A brute-force reference in
  the test (every gating block, both gates, sorting) agrees within 0.02 LU for I and LRA.
  Block sizes 1 to 4096 give bit-identical readings; process/readings/reset allocate nothing.
  Plants: no relative gate fails 3341 cases 3 and 4; a 5th percentile fails 3342 case 4.
- The test takes 56 s alone in MSVC Debug (74 s in the parallel run); most of it is the
  100-second cases and the reference. MSVC `/W4 /WX` clean; CTest **243/243**.
- The tables were read from the EBU's own Tech 3341 v4 and 3342 v4 PDFs (public); nothing of
  them is in the repository beyond the case numbers and levels the test synthesises.

**PR requested** — title "The one loudness meter, proved against EBU Tech 3341/3342 and a
brute-force reference (DEV-28 step 1)"; head `devices/loudness-meter` at `a3d4dfac`; base
`main`. Body: the commit message.

**Requests (DEV-28's other halves; not this lane's files):**
- **win:** pin **libebur128** in `tools/fetch_external.sh` (test-only, MIT) and its
  EXTERNAL-CODE row, so the meter can be checked against it at 44.1/48/96 kHz (LD3). Its LRA
  (1 Hz hop) is not used as an oracle, per the row.
- **engine:** move N13 (`src/adi/engine/main_loudness.*`, the Main meter) and export
  normalization onto `dsp::LoudnessMeter` (one integrated-LUFS implementation, XC-03). Its
  `readings()` is a plain snapshot: read it on the thread that processes, or publish it.
- This lane next: Auto Gain Stage's LUFS mode onto the meter (`src/adi/dsp/auto_gain.*`,
  ours), and the true-peak meter's double path and more than 8 channels.

## 2026-11-19 — DEV-22: what the research's default-preset corpus says next (no branch)

The research's DV-09 corpus (16 of Live 12.4's effects at their browser defaults, compensated
offline renders of a sweep, an impulse of 0.5 / -0.25 and six tones at 71, 223, 997, 3001,
7001 and 11003 Hz; analysed in the director's research on 14 November) gives acceptance numbers
beyond the latency table. Read privately; the statements:

- **Saturator** (High Quality on, Analog Clip, drive 0): the compensated impulse response is
  causal from 3 samples before the peak (so Live's reported 3 is its true peak delay), peak
  0.569 of the input, 99 % of the energy within 25 samples after the peak, above 1e-8 for 169
  samples after it, DC gain 1.0000. ADI's new minimum-phase filter (`devices/saturator-
  latency`): causal, peak 4 samples in (one later than Live), 0.669 of the input, energy99 at
  +7 after its peak, tail 25 samples. Live's resampler is sharper and longer (an IIR, by its
  tail). **Next step for Saturator:** fit the High Quality filter to that response (peak at 3,
  0.569, energy99 +25, tail ~170) and add those four numbers to its test.
- **Redux** (browser default): a real 64-sample delay (the compensated impulse peaks at 0,
  0.760 of the input, 99 % of the energy within 3 samples), DC gain ~0, and on the six tones
  gain -0.022 / -0.003 / -0.009 / -0.081 / -0.445 / -1.115 dB with phase +4.24 / +1.95 / +3.28 /
  +9.10 / +21.04 / +33.03 degrees: a ~5.3 Hz first-order DC blocker plus a ~0.4-sample advance
  against the 64 and a gentle top-end roll-off; nothing above -124 dBFS outside the tones.
  ADI's Redux default (48 kHz rate, 16 bits, no filters, no delay) is far from that. Live
  ships no Redux default preset, and no research set holds one; the ranges are readable from
  Live's own Redux presets. **Reading to take:** Redux's default control values (one look at
  a fresh Redux in Live 12.4). Then one branch: Live's ranges, defaults, the 64-sample path,
  and the six-tone numbers as the test.
- Overdrive and Saturator above, and Auto Filter, Chorus-Ensemble, Delay, Echo, Hybrid Reverb,
  Multiband Dynamics, Phaser-Flanger, Resonators, Reverb and Vocoder each have the same three
  default renders in the corpus; EQ Eight and Utility null exactly against dry. These are the
  first parity numbers for each built device, one branch each, in the manual's order.

**Next:** queue item 6, DEV-28, the one loudness meter (M4's critical path); DEV-22 resumes
after it, device by device, from the corpus above.

## 2026-11-19 — DEV-22: Overdrive shapes at 4× and reports Live's 5 samples

**`devices/overdrive-latency` (`7d89a70c`), from `origin/main`.**

- **Reading:** DV-08, Overdrive's default preset: "Latency: 5 samples (0.104 ms)", the same
  switched off. Live ships no default-preset file for Overdrive, so its ranges stay a
  reading to take.
- **ADI before:** the OJD shaper at the base rate (aliasing), reporting 0.
- **ADI now:** the shaper at 4× (polyphase interpolation, decimation at phase 0) through a
  185-tap minimum-phase filter: flat to 20 kHz, ≥ 61 dB down past 0.135 of the 4× rate,
  5.02 samples of low-frequency delay. Reports 5; the dry path waits 5. A driven 7 kHz tone's
  folded fifth harmonic: -25 dB before, -83 dB now (plant: base-rate shaping fails the check).
  Overdrive **21** checks, Overdrive Pd **17**, native/Pd parity unchanged; CTest **242/242**.

**PR requested** — title "Overdrive shapes at 4x and reports Live 12.4's 5 samples (DEV-22)";
head `devices/overdrive-latency` at `7d89a70c`; base `main`. Body: the commit message.

## 2026-11-19 — DEV-22: Saturator reports Live's 3 samples; defaults as Live's preset

**`devices/saturator-latency` (`870c0e2f`), from `origin/main`.**

- **Readings:** the latency table (DV-08: Status Bar text of each default preset, 48 kHz):
  Saturator "Latency: 3 samples (0.062 ms)", the same when switched off. Live 12.4's shipped
  default Saturator preset, read from its file: Oversampling (High Quality) on, Color on,
  Bass Shaper threshold -50 dB, Waveshaper Drive 1 (100 %); PreDrive -36..36, BaseDrive (Amt
  Lo) -36..36, ColorDepth (Amt Hi) -24..24, ColorFrequency 30..18500 Hz at 1000, ColorWidth
  0..1 at 0.3, PostDrive (Output) -36..0, Dry/Wet 1; Waveshaper Lin 0.5, Curve 0.05, Damp 0,
  Period 0, Depth 0 (all 0..1).
- **ADI before:** High Quality off by default; with it, two 65-tap linear-phase passes at 4×,
  16 samples delayed and reported (more delay than Live, against ADR-0203 d4).
- **ADI now:** the same filter made minimum phase (same magnitude within 1e-6 through the
  passband; DC group delay 3.56 samples for both passes), reporting Live's 3, dry delayed 3;
  the 0.56-sample remainder is written in the code. Alias suppression unchanged. Defaults:
  High Quality on, Color on, Bass Shaper -50 dB, Waveshaper Drive 100 %. Ranges matched where
  the control means the same: Amt Lo ±36 dB, Frequency 30..18500 Hz, Output -36..0 dB.
- **Left for a reading:** Width, Curve and Period are on Live's 0..1 scales (defaults 0.3,
  0.05, 0); ADI's are octaves and its own units. Mapping them needs a render each.
- Saturator **37**, Saturator Pd **30**; native/Pd parity unchanged; MSVC `/W4 /WX` clean;
  CTest **242/242**. Plant: reporting 16 fails both latency checks.
- **Merge note for win:** `tests/test_saturator_pd.cpp` is also touched by DEV-09 step 3
  (`devices/pd-audio-thread` adds a `block()` before reading the HQ latency). Whichever lands
  second takes both: the `block()` line and `== 3`.

**PR requested** — title "Saturator reports Live 12.4's 3 samples, defaults as Live's preset
(DEV-22)"; head `devices/saturator-latency` at `870c0e2f`; base `main`. Body: the commit message.

**Next:** Overdrive (0 against Live's 5), then Redux (0 against 64), then the Limiter's lookahead
against 64 / 128 / 256.

## 2026-11-19 — DEV-22: Shifter's Pitch mode as Live 12.4 measures it

**`devices/shifter-latency` (`5bd1ec48`), from `origin/main`.** Files: `src/adi/dsp/shifter.*`,
`pd/devices/Shifter.pd`, the two Shifter tests.

- **The reading (DV-06, relayed in RESEARCH-INTAKE 6.2; private notes read again):**
  Shifter's default preset, Pitch, Dry/Wet 100 %, a 0.5 impulse, 48 kHz offline export.
  Status Bar latency 0 at Window 10, 80 and 350 ms; at 0 st the output is the impulse
  alone, 240 / 1920 / 8400 samples late (half the Window), compensation on or off; Window
  10..350 ms, default 80. At +7 st: onset +16 / +187 / +848, peak +337 (0.196) / +1469
  (0.329) / +6454 (0.423), energy centre +263.5 / +1894.5 / +7043.4.
- **ADI before:** a phase vocoder on a power-of-two window, the whole window reported as
  latency, Window 5..170 ms at 40.
- **ADI now:** a rotating-tap delay line (two taps sweeping 0..Window half a cycle apart,
  sin² crossfades summing to one, resting at unison with one tap at Window/2). Unison is
  Live's exactly: a clean copy 240 / 1920 / 8400 samples late; `latency()` 0; Window
  10..350 ms at 80. Pitch accuracy under 0.04 cents from -12 to +12 st.
- **Away from unison:** with a free sweep phase the same model put Live's +7 st onset and
  peak within 1..5 samples at 10 and 80 ms (peak values 0.204 and 0.32 against 0.196 and
  0.329). Live's phase reference is not in the export, so ADI's +7 st numbers are printed
  by the test (onset +54 / +695 / +855 with ADI's rest phase), not claimed.
- Plant: another rest phase fails all three unison checks. MSVC `/W4 /WX` clean; CTest
  **242/242**.
- **Readings still to take (one question each):** Dry/Wet below 100 % (is Live's dry path
  delayed with the wet? ADI's is not); the sweep's phase reference (to claim the shape at
  +7 st).

**PR requested** — title "Shifter's Pitch mode as Live 12.4 measures it: no reported
latency, half a Window late (DEV-22)"; head `devices/shifter-latency` at `5bd1ec48`; base
`main`. Body: the commit message. A question on it is under FOR THE DIRECTOR.

**The rest of RESEARCH-INTAKE 6.2's latency table against what is built:** EQ Eight HQ
16 = Live's 16. **Saturator** reports 16 with High Quality, Live 3: ADI adds more delay
than Live (against ADR-0203 d4), next. **Overdrive** 0 against Live's 5, **Redux** 0
against 64, **Limiter** lookahead to check against 64 / 128 / 256 (+6 with True Peak):
each its own branch after. Compressor, Glue, Gate, Corpus, Erosion, Vinyl, Spectral Time
and Spectral Resonator are not built; their numbers wait for them.

## 2026-11-19 — DEV-21, step 1: OneShot's controls take Live 12.4's ranges, units and defaults

**`devices/oneshot-live-params` (`95a4fe0b`), stacked on `devices/sampler`** (which stands
on #278).

**The reading.** Live 12.4 (build of 24 April 2026) ships a default Simpler preset in its
Core Library; read from its files on this machine, no Live session run. The values, as
statements: Volume -36..36 dB, default -12; Attack 0.1 ms..20 s, 0.1 ms; Decay 1 ms..60 s,
600 ms; Sustain a level 0.000316..1 (shown -70..0 dB), 1 (0 dB); Release 1 ms..60 s, 50 ms;
One-shot Fade In and Fade Out 0..2000 ms, 0.1 ms each; Loop on, Snap on, Retrigger on;
Filter on, Low-pass, 24 dB, 30 Hz..22 kHz at 22 kHz, Resonance 0..1.25 at 0, Drive 0..24 dB;
LFO on, sine, 0.01..30 Hz at 1 Hz, free-running; Vel > Vol 0.35; Transpose ±48 st; Detune
±50 cents; Pan, Spread 0..100 %, Glide (off, 50 ms), pitch-bend range 5 (MPE 48); Voices
the sixth entry of its chooser. The file stays private, the numbers are in the test.

**What changed.** OneShot's matching controls now carry those ranges, units and defaults;
Vel > Vol is new (35 %). The canvas declares the same and a test pins all 28 values.
The oracles test mechanics, so they switch off what they do not test and model Live's
shortest attack and release where they compare samples (an exact 0.1 ms ramp, spectral
windows after it, one more block where a 1 ms release overlaps). Sampler keeps Snap off per
zone and exposes Vel > Vol. OneShot **295**, OneShot Pd **38**, Sampler **31**; MSVC `/W4 /WX`
clean; CTest **245/245**. Plant: the old Volume range fails the Live check and the dB oracle.

**Not claimed (the laws are still ADI's):** linear envelope segments (Live's have slopes,
default decay and release slope 1), the Vel > Vol blend, Resonance-to-Q, the filter circuits
and Morph, Pan, Spread, Glide, Drive (not built). **Readings still to take from Live 12.4,
one question each, before the sound is claimed:** the Voices chooser's list; the velocity
law at 35 %; the envelope slope law; the Clean filter's response at 22 kHz and at a
resonance. Each needs a render from Live; they are this lane's next use of Live and will be
announced here first.

**PR requested** — title "OneShot's controls take Live 12.4's ranges, units and defaults
(DEV-21 step 1)"; head `devices/oneshot-live-params` at `95a4fe0b`; base `devices/sampler`
until #278 and the Sampler PR land, then `main`. Body: the commit message.

**Requests to the engine (DEV-21's other half; LANES: `src/juce/*host*` and the catalogue
are not this lane's).** (1) `builtin_catalog`: entries for `adi.oneshot` and `adi.sampler`
with kind `instrument` (`EventFlow::Consume`), available. (2) `makeBuiltinDevice`: native
dispatch of both to `device::OneShot` / `device::Sampler` (no Pd adapter, ADR-0203 d4).
(3) Their samples through #414's slot ops (the `sample:1` role). The device side is ready
on the stack above; tests inject the loader meanwhile.

**Next:** DEV-19 (item 4) waits on the engine's DEV-17/DEV-18 (see #207 above), so item 5,
DEV-22: first the Shifter's latency against the reading (Live reports 0 and delays by half
its window), then the latency table of RESEARCH-INTAKE 6.2 across the built effects.

## 2026-11-19 — DEV-09, step 5: the Pd tier built with `PD_FLOATSIZE=64`, every suite passing (AR17); DEV-09 complete

**`devices/pd-double` (`d4568d8e`), stacked on `devices/pd-hash-a`.**

- `ADI_PD_FLOATSIZE_64` (off by default, in `pd_loader_fix.cmake`) puts
  `PD_FLOATSIZE=64` PUBLIC on `libpd_static`, so everything that includes Pd agrees on
  the layout.
- **Built that way, 19 of 242 suites failed.** Every ADI external handed Pd's signal
  vectors to its float core as `float*`, which at 64 are doubles. `FloatSignals`
  (`src/adi/pd_builtins/float_signals.hpp`) gives an external float views of its
  signals, converting around its perform routine only when `t_sample` is double (at 32 a
  view is the signal itself and nothing is added); applied to all 18 externals, their
  perform routines untouched. Plus one clamp of a `t_floatarg` against float literals.
- The loader tests now also plant every name Pd itself tries: a 64-bit Pd looks for
  deken names (`.windows-amd64-64.dll`) and never a bare `.dll`.
- Native against real-Pd renders: at 64, AutoFilter and EQ Eight differ by **2^-25**
  (one float ulp at 0.5; the canvas computes a parameter in double before the float core
  rounds it). At 64 one float ulp at full scale is allowed; at 32 the check stays
  bit-exact.
- Result: **both configurations, MSVC `/W4 /WX` clean, CTest 242/242.** A new check pins
  `sizeof(t_float)`/`sizeof(t_sample)` to `PD_FLOATSIZE`. Pd devices keep
  `processesDouble()` false, which stays true of them (the engine adapter hands Pd float
  buffers); a double path end to end would be its own change.
- When #278 lands, OneShot's external (`oneshot_tilde.cpp`) takes `FloatSignals` the same
  way; it is on DEV-21's list.

**PR requested** — title "Pd: a PD_FLOATSIZE=64 build of the Pd tier, every suite passing
in it (DEV-09 step 5, AR17)"; head `devices/pd-double` at `d4568d8e`; base
`devices/pd-hash-a` until it lands, then `main`. Body: the commit message.
**Request to win (CI is yours):** a CI leg configuring with `-DADI_PD_FLOATSIZE_64=ON`
(one Linux leg would do), so the 64-bit Pd tier stays green.

**DEV-09 as a whole**, five PRs in one stack, each naming its base:
`devices/pd-teardown` → `pd-fence` → `pd-audio-thread` → `pd-hash-a` → `pd-double`.
Its contract items: native loading closed at the source (step 2); quit, exit, open and
path changes fenced (2); one trylock tick per Pd tick (3); printing through a queue (3);
SPSC sends (3); arrays read without the lock-leaking path (3); the `#A` overlay (4); the
latency binding unbound on close (1); a `PD_FLOATSIZE=64` build (5). Tests AR7, AR8, AR17,
AR18 and #173's 40-cycle stress are in `test_pd_engine.cpp`. Not done, as the row defers
them: `[pd~]` out-of-process patches; a file and network policy for shared patches.
GCC and Clang have not compiled these branches here (no local GCC or Clang on this
machine); CI's legs are the first to.

**Next:** queue item 3, DEV-21, stacked on `devices/sampler`: OneShot's parameters to
Live 12.4's own ranges, units and defaults, read from the default preset Live ships.

## 2026-11-19 — DEV-09, step 4: `#A` unbound when its owner is freed (AR18, PD2)

**`devices/pd-hash-a` (`fcf0cc75`), stacked on `devices/pd-audio-thread`.**

- **Reproduced first.** A file load saves and restores `#A` around itself, so opening a
  whole patch was never the danger. An abstraction made **at run time** binds `#A` when
  it finishes loading (`canvas_popabstraction`) and nothing unbinds it; cleared, it
  leaves `#A` pointing at freed memory, which the next array or `[text define]` free
  dereferences. The test makes a table and an abstraction by dynamic patching in one
  instance, clears the abstraction, and checks `#A` is bound to nothing: it failed before
  the overlay, passes after. (The MSVC debug heap does not trap that read; ASan would.)
- **The fix** is plugdata-pd `bdb30eedee` ("Correct fix for pd bindlist crash (detected
  by ASAN)", Pd's own BSD-3 licence), as an overlay on the pinned `m_pd.c`: `pd_free`
  unbinds whatever it frees from `#A` first. Attribution is in the overlay's comment; win,
  if `docs/EXTERNAL-CODE.md` should name it too, that file is yours.

`adi_pd_engine_tests` **633**; MSVC `/W4 /WX` whole tree clean; CTest **242/242**.

**PR requested** — title "Pd: unbind #A when its owner is freed (DEV-09 step 4, AR18)";
head `devices/pd-hash-a` at `fcf0cc75`; base `devices/pd-audio-thread` until it lands,
then `main`. Body: the commit message.

**Machine note.** To check new code under clang's `-Wconversion` locally I started a
winget install of LLVM; it raised a Windows elevation prompt that nobody could answer,
so I stopped winget. A UAC prompt for LLVM's installer may still be on the desktop: it is
safe to decline. CI's GCC and Clang legs stay the check for those warnings.

**Next:** DEV-09 step 5, the `PD_FLOATSIZE=64` build for the 64-bit mode (AR17).

## 2026-11-19 — DEV-09, step 3: the audio thread never waits for Pd (AR8)

**`devices/pd-audio-thread` (`b86243f3`), stacked on `devices/pd-fence`.** Files: this
lane's `src/juce/pd_engine.*` and the Pd tests.

- **One `sys_trylock` per Pd tick** replaces `libpd_process_float`, which took
  `sys_lock` (blocking) and ran `sys_pollgui` on the audio thread on every tick. A busy
  lock skips the tick (silence, counted as `skippedTicks`). Measured: a message-thread
  holder of the lock for 50 ms; the slowest 64-frame segment stayed under 10 ms and the
  ticks were counted. Plant (waiting for the lock): 61 ms in one segment, nothing skipped.
- **Every delivery inside that lock**, just before the tick: `sendFloat` from the message
  thread through a single-producer ring (its name resolved under the lock), parameters
  latest-value-wins, transport and routed MIDI (kept over a segment that holds no whole
  tick). A parameter therefore reaches Pd in the next locked tick, still before its block
  (ADR-0188 d3). Two device tests read a latency straight after a parameter without
  rendering; they now render one block first (EQ Eight, Saturator).
- **Printing through a queue:** the print hook pushes into a bounded multi-producer queue
  of fixed slots (no lock, no allocation); the message thread joins the pieces. `[print]`
  on a 1 ms `[metro]`: **0** CRT allocations on the audio thread over ~400 lines (MSVC debug
  heap hook). Plant (the old string append): 3.
- **Arrays read without the lock-leaking path:** `libpd_read_array` returns -2 with the
  instance still locked when a table is shorter than asked. A published table that
  shrinks then hung the instance; the plant trips the test's 10 s watchdog. The tick now
  reads under its own trylock with a bounds check; `readArray` always unlocks.
- `gensym` in `bindParameters`/`bindArrays` now under the lock; `InstanceLock` is the
  message-thread holder an attached editor will use (DEV-11).
- Not serviced any more: `sys_pollgui` (sockets, GUI) never runs. `[netreceive]`-style
  objects therefore get no socket polling; the plan defers a file and network policy for
  shared patches (DEV-09 "Reject or defer"), and it is noted here so nobody is surprised.

`adi_pd_engine_tests` **631** checks. MSVC `/W4 /WX` whole tree clean; CTest **242/242**.

**PR requested** — title "Pd: the audio thread never waits for Pd: trylock tick, queued
deliveries, lock-free print (DEV-09 step 3, AR8)"; head `devices/pd-audio-thread` at
`b86243f3`; base `devices/pd-fence` until it lands, then `main`. Body: the commit message.

**Next:** DEV-09 step 4, the `#A` overlay (plugdata-pd `bdb30eed`, BSD-3 like Pd) with a
dangling-binding test, then step 5, the `PD_FLOATSIZE=64` build.

## 2026-11-19 — DEV-09, step 2: native code refused at Pd's own loader; `[; pd quit]` and kin fenced (AR7)

**`devices/pd-fence` (`b3adb315`), stacked on `devices/pd-teardown`** (both change
`pd_loader_fix.cmake`).

- **The bypass was real, and is now measured.** The engine's guards checked the patch
  folder, the search paths and `[declare]`. A patch naming an object by absolute path,
  by `../` path, or creating one from a message box at run time reached Pd's loader with
  none of them, and Pd called `LoadLibrary` on the planted file. Now Pd's one native
  loader (`sys_do_load_lib_from_file`) refuses every library and names it on the
  console ("ADI refused native code: <dir>/<file>"); the external-scheduler path too.
- **`[; pd quit 7]` ended the process with code 7** (the plant shows it: the test binary
  exits 7 at that section). `quit`, `exit`, `verifyquit`, `open`, `menunew`,
  `add-to-path`, `path-dialog`, `startup-dialog`, load/save/forget preferences, the
  audio and MIDI device dialogs and `fast-forward` are each one refusal naming the
  message. ADI opens patches with `libpd_openfile`, never `open`.
- Tests (AR7): 19 new checks; `adi_pd_engine_tests` **620**. Plants: without the loader
  refusal 6 checks fail (each of the three ways reached `LoadLibrary`); without the
  `quit` fence the binary exits 7. Restored: 0 failures.

MSVC `/W4 /WX`, the whole tree: clean. CTest **242 of 242**. (This build tree now links
with `/INCREMENTAL:NO`: MSVC 19.44's incremental linker failed with LNK1000 on a relink;
a local setting, nothing in the repository.)

**PR requested** — title "Pd: native code refused at Pd's loader; [; pd quit/exit/open/
paths] fenced (DEV-09 step 2, AR7)"; head `devices/pd-fence` at `b3adb315`; base
`devices/pd-teardown` (the step-1 PR) until it lands, then `main`. Body: the commit
message.

**Next:** DEV-09 step 3, the audio thread: one `sys_trylock` per Pd tick (skip and count,
never wait), every delivery inside it, no `sys_pollgui` there, the print hook through a
queue, arrays read without libpd's lock-leaking path (AR8).

## 2026-11-19 — DEV-09, step 1: a closed Pd engine frees everything it allocated

**`devices/pd-teardown` (`a7a95c58`), from `origin/main` `ae09f638`.** Files: this
lane's `src/juce/pd_engine.*`, `src/juce/pd_loader_fix.cmake`, `tests/test_pd_engine.cpp`.

- #173's unbind re-applied (MF4): `libpd_bind` returns the receive object; it is now
  kept and `libpd_unbind`ed while the instance is alive; the transport symbol is
  dropped with the instance.
- #173's forty-cycle stress is back, with a measurement: under the MSVC debug heap the
  live block count across 40 cycles of two engines (the printing one closed first)
  must not grow. It found more than #173 knew. After the unbind, **36 blocks per cycle**
  were still left, traced by allocation number and call stack to pinned Pd itself:
  `pdinstance_free` frees neither the `t_pdinstance` (392 bytes), nor the names of its
  twelve built-in symbols, nor its two 512-byte sound buffers, nor its path list; and
  `[inlet~]`/`[outlet~]` never free their reblocker array (72 bytes each, for every
  signal inlet and outlet of every patch or abstraction closed). Each is freed by an
  overlay in `pd_loader_fix.cmake`; the multi-line needles are matched after CRLF is
  folded to LF, since a Windows checkout of the pinned tree is CRLF.
- Result: **0** blocks over the 40 cycles. Plant: skipping only the unbind gives
  **80** (one per engine) and fails the check; restored, it passes.

MSVC 19.44 Debug `/W4 /WX`, the whole tree: clean. CTest **242 of 242** at `-j 6`;
`adi_pd_engine_tests` 601 checks. The leak count is measured on MSVC only (the debug
heap); elsewhere the cycles run as a stress.

**PR requested** — title "Pd: a closed engine frees all it allocated (DEV-09 step 1,
MF4)"; head `devices/pd-teardown` at `a7a95c58`; base `main`. Body: the commit
message, and "DEV-09 step 1 of 5; the native-loading and global-message fence, the
trylock tick with its print queue, the #A overlay and the 64-bit build follow as
their own PRs. Files taken under LANES §1 (the devices lane's `src/juce/pd_*`)."

**Next:** DEV-09 step 2, the native loader refused at the source and `[; pd quit /
exit / open / paths / preferences]` refused (AR7).

## 2026-11-19 — Queue item 1: #278 brought to main, #208 ported onto it, #207 not landable

**#278 (`devices/oneshot`, now `3892833a`).** `origin/main` (`ae09f638`) merged in
without a conflict. MSVC 19.44 Debug, `ADI_WERROR=ON` (/W4 /WX), the whole tree:
clean. CTest: 243 of 244 suites pass at `-j 8`; `adi_oneshot_tests` **197** and
`adi_oneshot_pd_tests` **38**, as on 28 October. The one failure was
`adi_scan_process_tests` ("timeout terminates child descendants...") under load; it
passes alone (3.5 s) and is the scanner's, not this branch's. The four validators
pass. Its draft gates stand as written on 28 October (Live parity, instrument
dispatch, the Pd instrument's Consume metadata); those are DEV-21's, item 3.

**PR requested** — mark #278 ready for review and land it: head `devices/oneshot`
at `3892833a`, base `main`. Body unchanged, plus: "Brought to main `ae09f638` on
19 November; MSVC /W4 /WX clean; 197 + 38 checks."

**#208 → `devices/sampler` (`a5500cc3`), stacked on `devices/oneshot`.** #208's
Sampler, ported onto #278 instead of the stale #207 stack (PLAN MF2). Changes from
#208: no Session hook (internal-instrument dispatch is DEV-21's with the engine;
the test injects the loader as OneShot's does); no `EventOwner` (a #207 type not on
main); prepare idempotent (ADR-0090 d5), `release()` makes the next one full; stepped
controls store whole steps; the check that failed on Apple clang in #208's CI
("modulation preserves base value", an exact `== 0` on `-90 + n*102`, which a fused
multiply-add does not give) now compares within 1e-9. `adi_sampler_tests` **31**
checks (#208's 28 and three new), OneShot's 197 unchanged. Plants: re-preparing at
the same format and unrounded steps fail the two new checks (2 of 31); restored, 0.

**PR requested** — title "Sampler on OneShot's voices, ported from #208 (DEV-21)";
head `devices/sampler` at `a5500cc3`; base `devices/oneshot` (#278) until #278
lands, then `main`. Body: the commit message. Then close #208 as superseded by it.

**#207 (`codex/midi-effects`) should not land, and nothing of it can land from this
lane.** Measured against main: a trial merge conflicts in 30-odd files, the window's
UI among them (its base is the stale 30 September stack). Its fourteen kernels compile
only against its own engine changes: `EventType::Control = 5` (main has
`Controller = 5`), `MappedValue`/`MappedMod`, `Event::owner` with `EventOwner` on every
`Node`, and `transformsEvents()`/`transformEvents()` added to `DeviceInstance` and run
from `Graph`. The plan rejects exactly those (DEV-18: "#207's EventType::Control = 5
and its per-event EventOwner*"; DEV-17: "#207's nextId * 1024") and gives the
transform phase and note identity to the engine (DEV-18, DEV-17). So the kernels are
ported (DEV-19, MF1: from `7f421f4d` with its listed fixes) once the engine's
transform phase is on main, onto that interface; this lane designs no second
transform path meanwhile. **Request to win:** close #207 as "to be ported (DEV-19 on
DEV-17/DEV-18)", keeping the branch; LANES §4b item 4 already says so.

**Queue:** item 2, DEV-09, next. Its first step (the latency binding unbound on close,
MF4) is on `devices/pd-unbind`, building.

## 2026-11-19 — The lane is taken up again (ADR-0208; ADR-0246 d5, in #641)

A Claude work seat in the director's research (Windows, MSVC 19.44,
Live 12.4 installed) now holds this lane. Its own clone and worktrees are apart
from the research folders and from the engine-win seat. Branches `devices/<name>`
from `origin/main`; this log stays on `devices/log`, newest first.

**The loan ends with this entry** (LANES §1, XC-46): `src/adi/dsp/loudness` and
`src/adi/dsp/limiter` come back to this lane. Nothing was built under the loan
(`origin/main` at `ae09f638` has no `src/adi/dsp/loudness`; `engine/main-loudness`
is the engine's own Main meter wiring and is left to it).

**Queue, in the order of the prompt of 19 November:** (1) the three unlanded
PRs #278, #207, #208; (2) DEV-09; (3) DEV-21; (4) DEV-19; (5) DEV-22; (6) DEV-28
with MX-27; (7) DEV-13, DEV-14, DEV-10, DEV-15; (8) the other rows of this lane
in milestone order.

**Half done, from 28 October:** #278 (draft, `devices/oneshot`); the Compressor's
Live 11.2.7 probes (46 tracks, the director's Mac, private). Taking #278 now.

**Live:** not taken today. When this seat takes Live it says so here first and
checks `collab/engine-win.md` on `engine-win/log`.

## 2026-10-28 — OneShot device recovery open as draft #278

**[PR #278](https://github.com/arieladi/Adi/pull/278)**, `devices/oneshot`,
commit `0161d33`, is based directly on main `a57d47a`. Ten owned files recover
#206's native sampler and #207's independent ParamMod behavior, then add a
compiled-in Pd external, forty-control canvas and immutable prepared-sample
bridge. No engine, host, UI, plugin, workflow or global CMake files changed.
No merge performed.

Sample copying/analysis happens off-thread. Processing borrows prepared data
under the existing sample-slot read scope, with no reference ownership changes;
replacement/clear generations reset voices. The Pd MIDI fallback has a fixed
512-event queue, counted overflow with voice reset, and a repeated-key policy
that releases the previous channel/key identity before replacing it. This
prevents an orphaned loop with Retrigger disabled. Its timing and MIDI 1.0
precision limits remain explicit; the native core uses exact original events.

Validation: dependency pins verified with `fetch_external.sh --build-only`;
strict Debug Ninja/Clang build, `ADI_WERROR=ON`, libc++ Debug hardening.
**8,414 checks / 97 suites**, all four validators and spec layout pass.
Affected suites: native **197**, real Pd **38**. Four compiled source plants
(ignored ParamMod, ignored generation change, omitted repeated-key release,
missing prepared sample) fail **8/2/2/6** checks; sources restored before the
final full build and suite. New OneShot targets have no warnings. The unchanged
`adi_tool` target emits the inherited Apple linker duplicate `libadi_core.a`
warning; this is not suppressed or claimed fixed. GCC/MSVC CI is pending.

Remaining draft gates: measured Live parity and inherited feature/default
gaps; production discovery/import and preparation hook; Pd instrument Consume
metadata; native direct dispatch after the adapter base/instrument integration.
The fallback reports its current 64-sample adapter tick plus zero intrinsic
sampler delay. Generic same-rate host prepare does not reset an already running
Pd graph: explicit `$0-oneshot-reset` or a published empty sample is required
until the owner adds lifecycle notification. Native prepare resets voices.
All these limitations are documented in `pd/devices/OneShot.md`.

Worktree: `work/active.nosync/oneshot`. Evidence in `adi_daw/build-strict/`:
`fetch-pins-final.log`, `build-oneshot.log`, `build-final.log`,
`test-all-final.log`, `oneshot-source-plants.json` and per-plant logs.
Generated synthetic test WAVs are retained under `build-strict/test-renders/`.
Queue continues with the Sampler recovery feasibility audit and Compressor
measurements; no action from Adi is pending.

## 2026-10-28 — Compressor static and dynamics batches measured; longer probes next

The original three isolated Sets have now produced **46 measured tracks** on
Live **11.2.7 (2022-12-08_2509d781e7)**. Calibration passes 61 checks, static
passes 97 integrity checks across 15 tracks, and dynamics passes 139 across
22 tracks. Each batch has captured controls and export settings; every raw
reference matches the original stereo float32 input at zero offset. Static
renders are 918,000 frames; calibration/dynamics are 390,000, all at 48 kHz.

The dynamics parameter verifier initially treated Live's `1.00 s` display as
milliseconds. Corrected unit conversion and display precision, rejected 1 ms
and 1.01 s failure plants, and recomputed all 630 checks from the unchanged
original API records into `dynamics-parameters-v2.json`. The failed original
capture is retained with its hash; this was a verifier bug, not a Live access
or settings problem. Both earlier captures still validate.

Independent calibration review confirms the 4:1 hard-knee operating point:
-6 dBFS input becomes -16.5000039 dBFS at threshold -20 dB, about 3.8e-6 dB
from the documented relationship. A candidate Peak/Lin amplitude detector
using the displayed attack/release times matches held-out timing, sine and
stereo probes within 4.70e-7 absolute audio amplitude. This is evidence for a
model candidate, not a shipped Compressor or full parity claim. RMS transients
are not explained by the same detector; transferring it would be incorrect.

Longer probes are required by the actual curves: RMS/Lin 100/1000 ms has not
settled after a one-second high hold, and Log 100/1000 ms has not recovered
after two seconds. A separate extended Set is being prepared with six-second
holds, ten-second nonzero release tails, two high levels and short/long Auto
Release excitation. The existing Sets, manifests, inputs and renders remain
unchanged. Local analysis and provenance: `work/local.nosync/live11-probe/evidence/`.
No action from Adi is pending.

## 2026-10-28 — Live access working; Compressor calibration measured on Live 11.2.7

Live is available and working. The earlier “access unresolved” note described
an automation targeting problem, not a missing Live installation or a request
for Adi to configure it. Binding the app by bundle identifier resolved control.
Preserved the current Set as `untitled_preserved_2026-10-28.als` in its own
Project, then loaded the isolated `compressor_calibration` Set. About Live
confirmed **11.2.7, build 2022-12-08_2509d781e7**. No input from Adi is pending.

Captured all nine tracks through the local parameter bridge and verified every
Compressor control, mixer unity, centered pan, stopped transport, and restored
focus. Exported All Individual Tracks, Return/Master effects off, start 1.1.1,
length 4.0.1 (8.125 s), stereo WAV, 48 kHz, 32 bit, no dither, normalization or
loop rendering. The WAV parser confirms actual IEEE float32 and 390,000 frames.
**61 calibration checks pass**: both no-device references match their inputs
exactly at zero offset; bypass, three unity-ratio lookaheads, dry-only and fixed
+6 dB controls meet their declared tolerances. The active Compressor step has
finite positive output and measured attenuation. This validates the measurement
path, not full Compressor parity or internal lookahead latency (offline PDC).

Local evidence is under `work/local.nosync/live11-probe/`: actual WAVs in
`out/compressor_calibration/`, parameter capture, validation and export/version
provenance in `evidence/calibration-{parameters,validation,export-settings}.json`.
Source/Set/manifest/output hashes are retained. Seven synthetic validator
failure plants were rejected before accepting the actual renders. The earlier
preparation README and manifests intentionally retain their preparation-time
status; these dated evidence records supersede their no-render checkpoint.
Static and dynamics response batches are next. Live 12 manual evidence remains
separate from these Live 11 measurements.

## 2026-10-28 — Reconciled CI and later work; OneShot recovery resumes

Verified the exact current heads through GitHub: **all 19 checks pass** for
[#250](https://github.com/arieladi/Adi/pull/250) (`d730ad5`), draft
[#253](https://github.com/arieladi/Adi/pull/253) (`234a816`), and
[#259](https://github.com/arieladi/Adi/pull/259) (`1f47b21`). This includes
GCC, Clang, MSVC, strict warning/hardening jobs and both JUCE jobs. Main remains
`a57d47a`; none of these changes has been merged.

The preceding checkpoint omitted two later completed changes:

- Hybrid Reverb now uses PFFFT through `RealFft` in `spectrum.*`, with packed
  DC/Nyquist handling, normalized inverse and preallocated aligned scratch.
  Full strict local validation: **8,304 checks / 97 suites**. Affected suites:
  103 FFT, 50 native Hybrid and 11 Pd checks. Four compiled source plants
  (DC/Nyquist, inverse scale, phase, omitted partition) fail 13/7/13/10 checks;
  restoration passes. The sparse `1e-7` oracle remains; dense full-tail tests
  have an explicit amplitude-scaled float-roundoff budget. The Live parity
  gaps remain; the handwritten FFT debt is resolved.
- The existing ADI limiter retained the previous rate's release coefficient
  across prepare. #259 retains seconds and recomputes the coefficient,
  including values set before first prepare. Strict local validation:
  **8,189 checks / 95 suites**; the original behavior and compiled plant fail
  eight new checks. This is not a Live Limiter implementation or parity claim.

Recovery re-audit: #206–#208 production integration prerequisites remain.
However, OneShot's native DSP plus the independent ParamMod additions can be
recovered within this lane; an explicit test loader can prove playback without
editing Session. Work starts on `devices/oneshot` in a fresh
`work/active.nosync/oneshot` worktree. A Pd sample bridge must publish immutable
prepared data, not call allocating sample analysis from perform. Pd instrument
Consume/event-flow metadata and production discovery remain integration gates.

Additional local measurement preparation was also reconciled: Live **11.2.7**
API controls for Compressor, Glue and Gate are in `work/live11-evidence/`,
explicitly not fresh defaults or audio renders. Three Compressor probe Sets
(calibration/static/dynamics), 46 tracks and seven synthetic input WAVs exist
under `work/local.nosync/live11-probe`. The prior MBD Set was saved in that
folder before calibration was opened; the old preparation README predates
those actions. No rendered outputs exist yet. Capture/validation scripts are
being completed, and current Live/export state must be verified before accepting
any measurement. Current Preferences observation: 48 kHz, 512 samples, no input,
MacBook Air Speakers output; no preferences changed in this resumed turn.

Filesystem correction: iCloud has replaced the earlier `work/pd-zero-delay`
and `work/hybrid-reverb` symlinks with duplicate directories. Do not assume
those aliases still identify the validated checkout; current Git heads and
remote CI are authoritative. Preserve duplicates until their state is audited;
new worktrees are created directly inside `work/active.nosync/`.

## 2026-10-27 — Checkpoint: two device PRs open; no merges

- **[#250](https://github.com/arieladi/Adi/pull/250)** — `devices/pd-zero-delay`,
  `d730ad5f628eb50803424b9e48dbec484f87d8bf`: 17 canonical native devices,
  explicit whole-64-sample user-patch mode, Redux alias fix. Strict local
  Clang build, **8,543 checks / 97 suites**, validators and compiled plants
  pass. CI is pending; GCC/MSVC and JUCE matrix results are not yet claimed.
- **Draft [#253](https://github.com/arieladi/Adi/pull/253)** —
  `devices/hybrid-reverb`, `f1562a89d7f4c2dd58b3158579f9954c9dd807db`:
  eight device-owned files recovered independently from #209. Strict local
  Clang build and **8,207 checks / 97 suites** pass. The draft retains the
  documented Live parity gaps and handwritten-FFT debt; it is not a finished
  Live clone. CI is pending.
- **#206/#207/#208** remain blocked at engine/UI ownership boundaries, as
  detailed below. No forbidden stack changes were imported and no PR merged.
- Compressor and Glue Compressor manual audits are recorded below. New Live
  renders/default measurements remain open; Gate is the next device audit.

Resume from these commits. Check CI on the exact PR heads first and fix any
owned failure. For Hybrid Reverb, replace its handwritten FFT with PFFFT via
the existing DSP wrapper boundary, prove transform packing/normalization and
convolution tolerance with failure plants, then rerun the affected checks.
Integrate native dispatch when the adapter base is available on main. Preserve
the remaining adapter remainders and Live measurement gates below.

Local worktrees and their cached builds are under
`work/local.nosync/{pd-zero-delay,hybrid-reverb,devices-log}`, reached through
stable `work/<name>` symlinks. Build directories: `adi_daw/build` for the
adapter and `adi_daw/build-strict` for Hybrid Reverb. Generated WAVs were moved
to `work/local.nosync/test-renders/`; they are synthetic ADI test outputs,
not Live measurements. Mutation source has been restored. The parallel
workers stopped at the workspace credit limit; this is a clean checkpoint,
not a device blocker. Continue the queue when resumed.

## 2026-10-27 — Hybrid Reverb recovery from #209 — full local validation green, Live parity open

Branch `devices/hybrid-reverb`, based on `origin/main` `a57d47a581157fed3256efb8c5bf40102e98645f`; independent worktree. Recovered the eight device-owned files from #209 (`codex/hybrid-reverb` at `7f421f4`): the native convolution/Surge Reverb1 core, compiled-in Pd external, `Hybrid Reverb.pd`, host-side sample preparation helper, CMake module and two test suites. No OneShot/Sampler/MIDI ancestry, engine/graph/host/UI changes, or historical checklist documents were carried. The original #209 remains the provenance and incomplete-scope record; this recovery does not claim completed Live parity.

Reference read: local Ableton Live 12 manual, §28.23, printed/PDF pp.590–596. This is manual evidence, not a Live render measurement. The director's Mac runs Live 11; no output measured from Live 11 or Live 12 was supplied or created here. Existing parameter ranges/defaults/curves remain ADI values unverified against Live. Missing parity includes the five Live algorithm identities, four-band EQ/Pre Algo, Vintage, Bass Mono, independently retained time/sync predelay feedback, IR browser/editor and host-preparation controls on the panel. Existing engineering limits remain 256-sample native convolution latency, 262144-frame IR cap, independent stereo convolution and partition-boundary replacement without a dual-tail crossfade.

Tests use an independent direct time-domain convolution oracle with absolute tolerance `1e-7`, deterministic synthetic inputs, block-size invariance, stereo/routing/tempo checks, allocation auditing, immutable IR publication and float-WAV round trips. Added negative controls proving the convolution oracle rejects silence, bypass, a 10% gain error and a one-sample timing shift. These are synthetic oracle plants, not substitutes for a version-labelled measured Live render gate.

Known architecture debt carried from #209: `hybrid_reverb.cpp` implements a handwritten double FFT, while the current `spectrum.hpp`/dependency policy says PFFFT is the sole FFT and hides its API in the DSP wrapper. This recovery preserves the existing implementation; replacing it through the shared FFT wrapper is still required. The source retains existing notices and reuses the already present Surge adaptation in `live_reverb.cpp`.

Validation completed so far: build-only dependency pins verified in a separate COW copy; Debug Ninja configure with Apple Clang, `ADI_WERROR=ON` and libc++ Debug hardening; `tools/validate_pd.py` passes. The isolated native core also compiled under strict Clang C++20 Debug/O0 hardening and passed **17 checks, 0 failures**, including the four oracle plants. Full strict CMake build passed. `tools/test_all.sh build-strict` passed **8,207 checks across 97 suites**, all validators and the spec layout check; the Hybrid Pd suite passes **11 checks**. `test_all.sh` is the full suite runtime pass; a duplicate CTest runtime pass is not needed. Recovery commit `f1562a8` is on `devices/hybrid-reverb`, draft PR **[#253](https://github.com/arieladi/Adi/pull/253)**. No merge performed. GCC/MSVC results remain pending CI. Canonical native dispatch integration awaits the adapter base (#250).

## 2026-10-27 — Glue Compressor evidence gate and Live probe availability

Read Live 12 manual §28.21, printed pages 586–588. Confirmed ratio-dependent
knee, two-timescale Auto Release, Range limiting the available compression
(0 dB gives no compression), fixed Soft Clip with nominal -0.5 dB maximum,
2× oversampling and the documented possibility of >0 dB peaks when oversampled,
plus external sidechain gain/blend/EQ/listen. The manual does not give the
complete parameter/default table, numerical knee/ballistics or waveshaper.
No Glue Compressor implementation or version-labelled render fixture is on
main. Keep its parity gate open rather than assigning an unmeasured SSL-like
algorithm; Gate is the next missing-device audit.

The existing `collab/mac/live-probe/` kit is the behavioral measurement route:
it generated the earlier Live **11.2.7** Multiband Dynamics evidence. Its default
`~/adi-live-probe` output directory is absent on this machine. Read-only access
to the local AdiVST WebSocket bridge works and currently identifies Multiband
Dynamics. The handshake reports Live version `?`, so it cannot label a new
measurement. No device was loaded or changed, and no render was made. A new
probe must use an isolated Set, verify the actual Live version and export
settings, and preserve the user's existing work. The kit's driver blindly
assumes remembered export settings; inspect those before running it.

## 2026-10-27 — Compressor evidence gate; next missing effect audit

Read the local Live 12 manual §28.9–28.9.2, printed pages 555–559.
Confirmed behavior includes threshold/ratio transfer, unity ratio, Dry/Wet
endpoints, Knee spanning threshold ± the displayed knee amount (the manual's
10 dB knee / -20 dB threshold example spans -30 to -10 dB), Peak/RMS/Expand,
Lin/Log, Auto Release, 0/1/10 ms Lookahead, sidechain gain/blend/EQ/listen and
Makeup being unavailable with an external sidechain. This is Live 12 manual
evidence only, not a Live 11 or Live 12 measurement.

Main has no Compressor core/external/canvas or version-labelled Live render
fixture. Existing `rmsc` is ring-modulation ducking, not Compressor; generic
limiter and multiband dynamics code does not establish Compressor's behavior.
The manual does not quantify all ranges/defaults, detector ballistics, RMS
integration, Log/Auto release, stereo linking or makeup law. Do not substitute
remembered algorithms and call it Live parity. Compressor's measured parity
gate is therefore open under LANES §5; continue with the next device audit
while the adapter and Hybrid Reverb full suites run.

## 2026-10-27 — Zero-delay adapter: full local validation green

PR **[#250](https://github.com/arieladi/Adi/pull/250)**, head `d730ad5`, is open
from `devices/pd-zero-delay`; never merged. CI is pending.

The branch now passes **102 adapter checks**, **260 native/Pd
comparison checks** and **68 Redux checks** under Apple Clang Debug with
`ADI_WERROR=ON` and libc++ Debug hardening. All 17 canonical native renders
match their real-Pd renders exactly, allowing only IEEE `+0`/`-0` equivalence
because Pd's output summing clears signed zero. A one-ulp oracle plant is
rejected. These are ADI-to-ADI comparisons, not Live measurements.

The comparison exposed a real Redux crossed-buffer defect: Pd may recycle the
right inlet vector for the left outlet. Capture both samples before writing
either output. The standalone original-code plant fails the new regression;
max error in the reproducer changes from 0.375 to zero after the fix.

Three compiled adapter source mutations also fail the test suite: restoring a
fixed 64-sample prime (8 failed checks), halving parameter values (6 failed checks), and dropping
the process block offset (4 failed checks). Every mutation was restored; the restored adapter
passes all 102 checks. An earlier plant attempt timed out on filesystem reads
and is not counted as a successful negative control.

The native allocation audit covers processing, tempo/MIDI/sample handoff and
now parameter sends. Static review found no allocating, locking or logging
path after preparation in the 17 cores. The full strict build passed, and `tools/test_all.sh build` passed **8,543
checks across 97 suites**, every validator and the spec layout check.
`tools/validate_pd.py` also passed separately. No GCC/MSVC result is claimed
before CI. The inherited macOS
linker reports a duplicate `libadi_core.a` link from unchanged shared CMake.

Build environment note: files in the new Documents checkout repeatedly
stalled on iCloud-backed reads. Isolated worktrees now live under
`work/local.nosync/`, with stable symlinks at their original paths. Restored
unchanged tracked files from HEAD, preserving all device edits. No user
checkout or other lane's build was changed.

## 2026-10-27 — Recovery audit: OneShot, MIDI effects and Sampler blocked at lane boundaries

Audited PRs #206–#209 against main `a57d47a`. The old stack is actually
#205 → OneShot → MIDI effects → Sampler → Hybrid Reverb; rebasing the entire
stack would import UI and graph work owned by other lanes.

- **#206 OneShot:** its implementation `072c813` modifies Session and UI
  sample-import code in addition to the DSP/device. Main lacks the UI ancestry.
  Native device files can be recovered, but a full working rebase needs the
  window/engine owners' integration. No Pd patch/external exists in this PR.
- **#207 MIDI effects:** `c130f0e` requires event ownership, mapped/control
  events, graph event transformation, Session factories, and host interfaces
  absent from main. These are engine/host prerequisites; this lane will not
  transplant or edit them. Warning fixes appear only later in #209.
- **#208 Sampler:** `b982fda` depends on both preceding devices and changes
  Session. No Pd patch/external exists. The old stack's macOS failure is its
  `getParam("volume").real == 0` modulation assertion: compare the unchanged
  stored/base value before and after modulation, rather than an assumed exact
  floating-point reconstruction of zero, when this item can be recovered.
- **#209 Hybrid Reverb:** its eight device files are independent of those
  prerequisites and are being recovered onto `devices/hybrid-reverb`. The
  unrelated graph/MIDI/Sampler changes in that PR are excluded.

These existing tests use generated signals/event oracles. None supplies a
version-labelled measured Live render fixture, and the old checklists mark
parameter ranges/defaults and substantial device features unverified. Recovery
and synthetic test success must not be described as Live parity.

## 2026-10-27 — Zero-delay adapter: implementation and verification in progress

Branch `devices/pd-zero-delay`, based on main `a57d47a`.

- Canonical built-ins execute the existing native cores directly, selected by
  complete compiled canvas and resolved helper contents, never by filename.
  Modified canvases/helpers retain Pd execution. Original patches remain
  openable and the existing Pd suites explicitly exercise that path.
- A user patch can select `BlockMode::WholeBlocks` before preparation. Every
  segment must contain a whole number of Pd's 64-sample ticks; the mode returns
  same-frame output without priming. Violations are counted and silenced;
  latency is never silently changed during playback. Oversized valid calls
  are tiled through preallocated scratch instead of dropping samples.
- **Remainder for win/engine:** `maxFrames % 64 == 0` does not establish that
  guarantee: event splitting can produce 100 + 412 from a 512-frame block.
  Arbitrary user-patch segments retain the stable, reported 64-sample adapter.
  Enabling whole-block mode in a caller requires its scheduling guarantee;
  this lane does not change the graph. A streaming one-tick adapter could
  potentially reduce the conservative 64 to 63, but that is not this change.
- **Remaining legacy canvases:** `pd/adi-eq8.pd`, `pd/adi-limiter.pd`,
  `pd/adi-rmsc.pd` and the analyser remain on Pd. They are vanilla programs,
  not the canonical compiled-external canvases covered by the native dispatch;
  substituting similarly named DSP without a wiring/null proof would change
  their sound or array publication.
- **Inherited Pd runtime constraint:** pinned `z_libpd.c` takes `sys_lock`
  inside both process APIs and array reads; `sched_tick` also calls the locked
  message queue. Arbitrary user-patch callbacks can allocate or print. The
  native path bypasses these calls. Whole-block scheduling removes buffering
  delay, but is not a claim that arbitrary vanilla Pd is realtime-safe.
  Removing those runtime locks without preserving Pd clocks, message queues
  and concurrent instance lifetime is separate work, not an unlocked call
  substituted here.

Full verification and PR links are recorded in the checkpoint above.

## 2026-10-27 — Lane bootstrap and source locations

Read the standing prompt and five-lane LANES/ADR-0208 text from
`origin/win/lanes-5` (#238), since they are not yet on main. Read the requested
ADRs, device contract, FEATURES table and EQEight patch/core/external/module/
tests. `pd/README.md` is absent on both branches; the available document is
`pd/devices/README.md`.

The extracted `reference/DOCS/_text/Ableton_live12/` directory is absent on
this Mac. A local Live 12 manual PDF is available under the existing checkout's
`adi_daw/References/live12-manual-en.pdf`; the original is left untouched.
All future measurements must say Live 11 or Live 12. No Live measurement has
been made or inferred in this bootstrap/adapter work.
