# basics-analysers — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/analysers.md` on
`basics/analysers-log`; your work is one chain branch, `basics/analysers-chain`, from origin/main.

**Why.** The director looked at ADI's analysers in the real window and was not pleased: "a 90s spectrum
that shows nothing". Before ADI he had built an analyser as a Max for Live device, measured side by side
against the tools he wants ADI's to match, and he was pleased with it. It is in this repository:
`adi-m4l-analyzer/` at the repository's root. Nothing in ADI's analysers has drawn on it. His word of
27 November: port what it measured.

**What to read.**
- `adi-m4l-analyzer/PROJECT.md` (what is worth porting and what is not), `README.md` (22 faults found
  and why), `docs/ARCHITECTURE.md`, `docs/ACCEPTANCE-TESTS.md`, `docs/DEVIATIONS.md`.
- Its maths: `device/avc.meters.js` (weighting filters, ballistics), `device/avc.engine.js` (windows,
  calibration, averaging), `device/avc.specui.js` (the period detector, the bands, the scopes), and its
  tests `test/test_spectrum.js`, `test/test_weighting.js`.
- ADI's side: `src/juce/ui_analysis.*`, `src/juce/ui_mix_analysers.cpp`, `src/adi/ui/shared_analysis*`,
  the loudness meter under `src/adi/mix/`. Decisions: ADR-0183, ADR-0244 d4 to d7, d16 and d18, ADR-0251,
  ADR-0257 d12. Register: DF-W22.

**What ports and what does not.** The device is Max and JavaScript, drawn inside Live: none of its code
is copied. Its measurement is: the settings, the formulas, the ballistics, the tests and the numbers it
reached against its references. Port those into ADI's C++ and prove each with the same test.

**ADI's rules that hold here.** An analyser hears and never speaks: nothing of it is in the audio path.
The audio thread allocates nothing and takes no lock. Analysis runs only while something shows it.
Everything is drawn by ADI: no other maker's bitmap, logo or font. A mode's label is ADI's own word,
never another product's name.

## Queue, in this order

1. **The measurement core, with its tests.** Port `test_spectrum.js` and `test_weighting.js` as C++
   suites and make ADI pass them: a full-scale sine reads 0.00 dBFS at every block size and window (the
   window's coherent gain); the display slope of 4.5 dB per octave pivots at exactly 1 kHz; the A, C and
   K weighting filters meet their standards' tolerances at every sample rate ADI runs at.
2. **The spectrum, as he liked it.** The device's default settings are its reference's own, read off
   that tool: block 4096, averaging 194 ms, 10 Hz to 20 kHz, −80 to −13 dB, 40 frames a second. A live
   curve over a filled, darker held maximum. The curve types (real-time average, real-time maximum,
   held maximum, average) are computed on every analysis frame, never at the display's rate, so that no
   peak is missed. One value per display column: the maximum of the bins a column covers, and an
   interpolation where a column is narrower than a bin. A read-out under the pointer: frequency, note
   (Live's octave numbering: middle C is C3) and level. This is the "Filled Spectrum" of the Info box's
   switch and of the Analysers page.
3. **The meter, with its reference's layout and readings (ADR-0244 d16).** Peak, RMS and Dynamic bars.
   RMS is a true sliding window of the integration time (a one-pole lurches on every transient); release
   shapes the peak bar only; peak hold; the K scales; the weightings of item 1; correlation over 500 ms
   and balance over 3 s, both from the running sample rate; clip counters on the absolute signal; crest
   factor. Two channels are combined in power, never by averaging decibels. Its documented agreement is
   the target: peak and RMS within 0.1 dB of what the device reads on the same signal.
4. **The stereo picture (a goniometer)** on the Phase page: left and right taken from the same sample,
   so that mono draws a line and not a ball.
5. **Third-octave bands with a period-locked scope.** Thirty ISO bands. The scope locks to the period
   found by autocorrelation: normalise over the overlapping region, take the first local maximum (the
   global one is the octave error), and compute one lag past the search limit.
6. **The colour waveform:** the scope with its low, middle and high bands in three colours, scrolling at
   a constant rate. It has no future half: a live analyser has no future to draw.

## For the director

Put here, with a recommendation and a picture's measurements, anything that changes what a user sees
beyond the list above: for example whether the Info box's switch gains the third-octave bands or the
stereo picture, and what each new mode is called.

## Paths

The analyser files belong to the Mix View seat. By the director's word of 27 November they are lent to
this queue until that seat next runs; say in your report what you changed in them, for it to read.
