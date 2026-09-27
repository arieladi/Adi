# Acceptance tests

Status key:

- **MATH ✅** — verified numerically outside Max by the test suite. The formula
  is right; whether Max is wired to use it is still unproven.
- **STRUCT ✅** — enforced by `test/validate.py` against the generated patcher.
- **LIVE ⏳** — needs the device running in Live. Not yet done.
- **KNOWN GAP** — will not pass as built; see `DEVIATIONS.md`.

Run first:

```bash
python3 test/validate.py && node test/test_spectrum.js && node test/test_weighting.js
```

---

**1. 1 kHz sine at −18 dBFS peaks exactly on the top gridline, on the 1k
vertical gridline.** — MATH ✅ / LIVE ⏳
`test_spectrum.js` reads −18.0000 dB for a −18 dBFS sine on a bin centre with
all three windows, and the log mapping round-trips 1000 Hz exactly. In Live:
set Slope 0 (the default 4.5 dB/oct lifts 1 kHz by 0 dB, so it also works at the
default) and Range Hi −18. Note a tone landing between bins scallops down by up
to 1.42 dB with Hann — put the generator on a bin centre for an exact reading.

**2. Full-scale sine reads 0 dBFS with slope 0 and Range Hi 0; Align 0 dB holds
across every block size.** — MATH ✅ / LIVE ⏳
Verified at 1024/2048/4096/8192/16384, all reading −0.0000 dB.

**3. White noise with slope 0 renders flat; pink noise with slope 3.0 renders
flat.** — MATH ✅ (slope term) / LIVE ⏳
The slope formula is verified to pivot at 1 kHz and give exactly N dB/octave.
The flatness of noise itself is a property of the analyser and needs Live.

**4. Sine shows Dynamic ≈ 3.0 dB; square wave ≈ 0 dB.** — LIVE ⏳
This is the test that catches metering scaling errors. Peak−RMS of a sine is
3.01 dB. Use Weighting Off, and give the RMS integration time (default 300 ms)
a few seconds to settle before reading.

**5. Peak Hold and RMS Hold bars decay after exactly 3 seconds of lower
signal.** — LIVE ⏳
The hold is a latch-and-drop: a new maximum restarts the timer, and when the
timer expires the bar drops to the current value. Peak Hold time is adjustable
(default 3000 ms).

**6. Correlation +1 for mono, −1 for a polarity-flipped duplicate, ~0 for
uncorrelated noise.** — LIVE ⏳

**7. Weighting = K changes RMS and Dynamic but leaves Peak untouched.** —
STRUCT ✅ / LIVE ⏳
Structurally guaranteed: the `biquad~` cascade sits between the channel source
and the RMS square only. `peakamp~` and the peak bar tap the source directly,
upstream of the filters. `test_weighting.js` verifies the filter responses
themselves against IEC 61672 and BS.1770.

**8. Null test: device output against the dry path cancels completely, in every
mode and at every setting.** — STRUCT ✅ / LIVE ⏳
`validate.py` asserts that `plugin~` outlet 0 → `plugout~` inlet 0 and outlet 1 →
inlet 1 are the only connections into `plugout~`, and fails the build otherwise.
There is no gain, filter or bypass anywhere in the path to make this
setting-dependent.

**9. Scrolling the device out of view drops CPU by an order of magnitude.** —
LIVE ⏳, partial
Implemented as a paint-liveness heuristic (see `DEVIATIONS.md`) because no Live
API reports chain-strip visibility. When it engages, the redraw clock drops to
2 Hz and the whole Jitter reduction and meter snapshot chain stops with it. If
Max paints off-screen objects anyway the gate never engages and CPU stays at the
normal figure — it cannot fail in the harmful direction. Selecting a different
track is the case most likely to work.

**10. 44.1 k → 96 k keeps the 1 kHz sine on the 1k gridline and does not shift
meter calibration.** — MATH ✅ / LIVE ⏳
Bin width, ballistic coefficients, weighting filters and LUFS windows are all
recomputed from the running sample rate, driven by `dspstate~` (which re-reports
whenever DSP restarts) and `adstatus sr` at load.

**11. Duplicating the device 20 times produces no console errors.** — LIVE ⏳
Every global name is `#0`-derived: the analysis matrices are `#0_specA/B`, and
the same id is passed to `avc.engine.js` as an argument so its `messnamed`
targets and its `pfft~ … args` match. The `pfft~` subpatcher learns both names
through `pfft~`'s documented `args` keyword.

**12. Max-hold survives stop; clears on play; clears on plot click; accumulates
with the transport stopped; does not clear at a loop boundary.** — LIVE ⏳,
one exception
All as specified except re-pressing Play *during* playback, which does not
reset — see `DEVIATIONS.md`.

**13. Compact width shows the minimal meter block; switching back to Normal
immediately shows correct accumulated values.** — STRUCT ✅ / LIVE ⏳
Guaranteed by construction: width only affects `avc.specui.js`'s drawing. Every
measurement — Dynamic, correlation, balance, crest, LUFS — is computed in MSP
regardless of what is drawn, and the full 20-value meter list is sent every
frame at every width.

**14. Cursor readout: 440 Hz → A3 at 0 cents; 1 kHz → B4 at +21 cents; the top
gridline reads Range Hi; fast mouse movement does not drop frames.** —
MATH ✅ / LIVE ⏳
`test_spectrum.js` asserts A3 +0 and B4 +21 using the shipped `noteFor()`.
Octaves follow LIVE's numbering (middle C = C3), not scientific pitch
notation (middle C = C4), because the reading is compared against Live's
own piano roll rather than against a textbook.
Frame stability is by construction: `onidle` only stores the cursor position and
the next scheduled frame draws it, so mouse movement never triggers a repaint.

---

## Extra checks worth running in Live

- **Window sidelobes.** Feed a 1 kHz full-scale sine, set Range −140/0, slope 0.
  Hann's first sidelobe should sit near −31 dB, Blackman near −58, Hi-Res near
  −93. If all three look identical, the window convolution is not being applied.
- **Console silence.** Open the Max console with the device idle for a minute.
  Anything printed in normal operation is a bug.
- **Block-size change under audio.** Changing block size rebuilds the `pfft~`.
  Audio must not glitch — only the analysis should momentarily restart.
