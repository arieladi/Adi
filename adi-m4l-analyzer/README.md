# AVC Spectrum & Meter

A Max for Live **audio effect** that puts a SPAN-style FFT spectrum analyser and a
bx_meter-style Peak/RMS/Dynamic meter **inside Live's device chain strip**. No
floating window, no overlay — the inline view is the only view, drawn entirely
within the 155 px the chain allows.

**The device never touches the audio.** `plugin~` connects straight to
`plugout~`; everything else is a parallel read. There is no gain stage, no
filter in the path, no bypassable processing. A null test against the dry path
cancels in every mode and at every setting, and the build's validator enforces
that structurally (it fails the build if anything other than `plugin~` ever
feeds `plugout~`).

---

## Read this first

**Status: four bring-up rounds done. Fourteen bugs found and fixed.**

The device loads in Live and the UI draws correctly. The first run turned up
four faults that no amount of offline checking could have caught, all now fixed
and all now caught by `test/validate.py`:

1. The gen patcher was missing `"classnamespace": "jit.gen"`, so it silently
   failed to compile and `jit.gen` degraded to passing its input through —
   which painted the plot solid green at 0 dB.
2. `jit.gen` referenced its gen patcher as a bare argument; the supported form
   is the `@gen` attribute.
3. Two `expr` objects emitted floats into `select` / `selector~`, which only
   accept ints. Both channel selectors stayed at 0, so the FFT *and* the meters
   were fed silence — hence `-inf` levels and a `NaN` dynamic reading.
4. `adstatus sr` reports its numeric value on its **right** outlet, not
   outlet 0. Sample rate now comes from `dspstate~` alone.

Second run — the device drew, but only up to 3 kHz and with dead high end:

5. **Every numeric parameter was declared as Live type Int.** Live's native
   integer representation is limited to 256 values, so it silently rewrote
   every range to `mmin + 255`: Freq Hi's 3000–48000 became 3000–**3255**, and
   the spectrum could never show above 3 kHz. Integration/Release/Peak Hold
   were pinned at 257/305/355 for the same reason. All numeric parameters are
   now Float type with an Int unit style, which is what Max's documentation
   prescribes for exactly this case.
6. The `pfft~` subpatch took its frame size from `fftinfo~` on `loadbang`, but
   a `pfft~` created by scripting cannot be relied on to fire its subpatch's
   loadbang. The frame size is now passed in through `args`, and the parent
   also sends an explicit `setup`.
7. Underlay only reached a cold inlet, so toggling it did nothing until the
   channel menu was also changed.

Third run — spectrum compressed into the left 60%, levels ~20 dB low, garbage
spikes at the top end, dead meters, and several seconds of lag at playback:

8. **`jit.spill` emits exactly `listlength` values whatever the matrix holds.**
   Fixed at 1024 against a 617-cell output it read 407 values past the end of
   the matrix every frame. That squeezed the real spectrum into the left 60% of
   the plot -- so every x position drew the level of a much higher frequency,
   about 20 dB low -- and painted the out-of-bounds garbage as spikes. One bug,
   three symptoms. `listlength` now tracks the output width, and
   `test/test_spectrum.js` asserts the whole display pipeline stays aligned.
9. **Max's `expr` has no ternary operator.** Its documented set is
   `+ - * / %`, the bitwise/logical operators and comparisons -- `?:` is not
   among them, so such an object never outputs and everything downstream sits
   at its creation default forever. Five objects were affected, including the
   one that selects the metering source, which is why the meters read nothing.
   All replaced with `sel` and message boxes; the validator now rejects any
   ternary `expr`.
10. The visibility gate only re-checked every 16 bangs. Once it dropped to the
    500 ms heartbeat that is 8 seconds before it could notice the device was
    visible again -- the "stuck for 5-10 seconds at play" symptom. Recovery is
    now immediate; only going idle is rate-limited.
11. The second spectrum was drawn as a bare line, so the display read as stray
    lines rather than one spectrum. It is now a filled dark region behind the
    live curve, and the second engine uses the same green family instead of
    blue -- SPAN's lingering-buildup look.

Fourth run — spectrum still compressed and cut at ~2.7 kHz, levels still wrong,
RMS jumpy compared with bx_meter:

12. **`pfft~` was running at its default 512, not the requested 4096.** Numbers
    passed to `patcher.newdefault()` from JS did not reach it as ints, and
    `pfft~` silently fell back to its documented default. The analysis ran at
    512 while the reduction believed 4096, so the whole spectrum was compressed
    8x into the bottom 2.7 kHz and every hover read the level at 8x the
    intended frequency. The engines are now created by
    `thispatcher script newdefault` from message boxes, where Max parses the
    arguments itself and the size arrives as a genuine int. **The subpatch also
    reports the frame size it actually received, and the reduction is
    configured from that rather than from what was asked for**, so this class of
    mismatch can no longer distort the display silently -- it reports itself
    instead.
13. RMS was a one-pole. "Integration time = 20 dB fall in 300 ms" is only a
    ~33 ms time constant, which lurches on every transient. It is now a true
    sliding-window RMS (`average~ ... rms`) with the window set to the
    integration time, which is both what bx_meter shows and what RMS means.
    Release now shapes the peak bar only.
14. Channel mode defaults to **Sum**, so the display is one curve plus its Max
    hold rather than four overlaid curves. L+R overlaid is still one menu step
    away.

Fifth round -- spectrum still truncated, meters still not matching bx_meter.
The spectrum cause is still open; these are the metering faults found by
auditing every object against the Max reference documentation:

15. **`average~` has no `float` method.** Its documented methods are
    `int / absolute / bipolar / rms / signal`. The RMS window was sent as a
    float, so it was not rounded -- it was *rejected*, silently, leaving the
    object at the interval its creation argument installs: 192000 samples,
    a **4.35 s window** where 300 ms was asked for. A 4 s RMS reads several dB
    below a 300 ms RMS on programme material, by an amount that varies with
    the material -- which is exactly the error measured against bx_meter, and
    why it never looked like a constant offset. The LUFS path already coerced
    with `expr int(...)`; the RMS path did not. `test/validate.py` now fails
    the build on any control value reaching an `average~` uncoerced.
16. The clip counter tested `>=~ 0.999` against the **signed** signal, so every
    negative-going clipped sample went uncounted -- roughly half of them. It
    now tests `abs~`.
17. The clip counter emitted the **pre-increment** value: `edge~`'s bang hits
    `f`'s hot inlet, which outputs the stored number before `+ 1` writes the
    new one back. A single clipped sample reported 0.
18. **Max Crest Factor, both clip counters and both LUFS meters were computed
    in MSP 30 times a second and thrown away** -- nothing in the drawing layer
    ever read those five slots. They are now displayed. Slot 19, labelled
    "integrated LUFS", was being fed short-term LUFS a second time, so it
    showed a plausible number for a measurement the device does not make; it
    now reads as absent.
19. Correlation and balance shared one hard-coded `slide~ 22050`. That is a
    500 ms time constant at 44.1 kHz only, it never tracked the sample rate,
    and applying it to the balance pair meant the documented **3 s balance was
    actually running at 500 ms**. Both are now computed from the running rate.
20. The over indicator on the bars tested the **biased** level while the
    numeric readout tested the raw one. In K-20 the red strip lit permanently
    on normal material. An over is a property of the signal, so both now test
    the raw level.
21. The dB scale beside the bars always started at 0 even after `meterscale`
    raised the top, so in the K-System modes the labels disagreed with the bars.
22. In Link mode the dynamic bar spanned both channels while the number printed
    inside it was the left channel's alone, and the two channels were combined
    by averaging **decibels** -- the mean of -10 and -20 dB is -15.0 where the
    true combined level is -12.6. Both fixed.

**If you are reloading into an existing Live set**, the old clamped values are
stored in the set and will come back. Either drop in a fresh device instance,
or open the settings panel and pick "SPAN Default" from the preset menu.

If you have an older copy of the device open in Max, **replace it with the
regenerated one** rather than patching it by hand.

Everything else was written against the Max 8.3.1 reference documentation
bundled with Live 11 Suite, and the maths is verified numerically by the test
suite below. Treat the next run as continued bring-up —
`docs/BRINGUP.md` walks through it in checkpoint order.

`docs/DEVIATIONS.md` lists everything that differs from the brief, everything
inferred rather than known, and everything deliberately not built. Read it
before judging a behaviour as a bug.

---

## Files

```
device/                              <- everything here ships together
  AVC Spectrum Meter.amxd            the device (ready to drop into Live)
  AVC Spectrum Meter.maxpat          the same patcher as plain JSON, for pasting
  avc.fftanalysis.maxpat             pfft~ subpatcher: one analysis engine
  avc.specreduce.genjit              jit.gen: bins -> one point per pixel
  avc.specui.js                      jsui: the entire display layer
  avc.engine.js                      pfft~ lifecycle + spectrum configuration
  avc.meters.js                      A/C/K weighting design + meter ballistics

build/                               the generator; edit this, not the patchers
  maxpat.py                          patcher-JSON / .amxd writer
  device.py                          the main device
  fft_analysis.py                    the pfft~ subpatcher
  gen_patch.py                       the jit.gen reduction

test/
  validate.py                        structural + cross-file checks
  test_spectrum.js                   window, calibration, slope, cursor maths
  test_weighting.js                  A/C/K filters vs IEC 61672 / BS.1770

docs/
  ARCHITECTURE.md                    signal flow and where to change things
  DEVIATIONS.md                      deviations, inferences, and gaps
  ACCEPTANCE-TESTS.md                the 14 acceptance tests, with procedures
  BRINGUP.md                         first-run checklist
```

The patchers are **generated**. Change `build/*.py` and re-run, or your edits
will be overwritten:

```bash
cd build && python3 fft_analysis.py && python3 gen_patch.py && python3 device.py
```

## Install

1. Copy the whole contents of `device/` into one folder. Live and Max resolve
   `avc.fftanalysis.maxpat`, `avc.specreduce.genjit` and the three `.js` files
   from the folder containing the `.amxd`, so they must stay together.
2. Drag `AVC Spectrum Meter.amxd` onto an audio track.

For a portable single file, open the device in Max and use
**File → Freeze Device**; that packs the subpatcher, the gen patcher and the JS
into the `.amxd`. Freeze only after the device works — a frozen device is
awkward to debug.

If you would rather build it by hand, `docs/BRINGUP.md` has the
"New Audio Effect in Live → working device" route using the `.maxpat` JSON.

## Run the tests

```bash
python3 test/validate.py && node test/test_spectrum.js && node test/test_weighting.js
```

These check the parts that can be checked without Max: patcher structure,
cross-file message contracts, and all of the DSP maths that is expressible
outside MSP. They do not and cannot verify that Max wires it up as intended.

## Defaults

Block 4096 · Overlap 75% · Hann · Avg Time 194 ms · RT Max primary ·
second spectrum on, type Max · filled · 10 Hz–20 kHz · **−80/−13 dB** ·
slope 4.50 dB/oct pivoting at 1 kHz · smoothing off · Sum ·
**40 fps** · meters Stereo, unweighted, Float on, dBFS.

These match the reference SPAN instance's own settings, read off its Spectrum
Mode Editor and global settings rather than assumed. Two things still differ
and both are documented in `docs/DEVIATIONS.md`: SPAN's overlap is 80%, which
`pfft~` cannot express (75% and 87.5% are the neighbours), and its window is
"Dome", which is not one of the three cosine-sum windows this device can apply
by spectral convolution.

Width presets: Compact 420 / Normal 960 / Wide 1440. The spectrum always keeps
its area as the width shrinks; the meter block gives ground first and collapses
to a minimal two-bar form at Compact, while continuing to measure everything.
