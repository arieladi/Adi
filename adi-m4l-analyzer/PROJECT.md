# adi-m4l-analyzer — the Max for Live spectrum meter

The analyser that ADR-0183's native one is ported from. It runs today, in
Ableton, as `AVC Spectrum Meter.amxd`, and it is kept here because **the
measurement in it is worth more than the code around it**.

Imported 2026-09-27 from `AVC-Spectrum-Meter` (70 commits, history left in that
repo). Nothing here is built by `adi_daw`'s CMake; it is a Max for Live device
and a Python generator, and it is kept as a working reference and a thing that
may be improved on its own terms.

## What it is

A Voxengo SPAN-style analyser and a Brainworx bx_meter-style meter in Live's
155 px chain strip, at 960 px wide. Six screens: **SPN** the analyser, **OSC** a
DJ-style RGB waveform, **OS2** a plain oscilloscope, **RME** third-octave bands
with a period-locked scope, **SPG** a spectrogram, **SET** the settings.

**It never touches audio.** `plugin~` goes straight to `plugout~` and the
signal is tapped in parallel for analysis only, in every mode and at every
setting. `test/validate.py` asserts that connection and fails if it is ever
broken.

## Layout

    build/     Python generators. device.py emits the .amxd; the patchers are
               generated, never hand-edited, because a 500-object patcher
               edited by hand cannot be reviewed.
    device/    the generated device and the hand-written JavaScript
               (avc.specui.js draws everything; avc.engine.js is the analysis
               control layer; avc.meters.js the metering maths).
    test/      run_all.sh -- structural validation, spectrum maths, weighting.
    docs/      ACCEPTANCE-TESTS.md, DEVIATIONS.md and the rest.
    backup/    a standalone copy from before the DJ-scope work, kept because it
               was the last known-good during a bad run.
    install.sh copies the device into Live's User Library. It REFUSES while
               Live is running, and that refusal is the point: copying these
               files under a live device segfaulted Live once, inside Max's
               file watcher.

## What is worth porting, and what is not

**The JavaScript drawing layer is throwaway.** ADR-0183 d7 says so and it is
still true: the native analyser draws in C++ against published arrays.

**The measurement is not.** These were made correct here, usually the hard way,
and would be re-derived wrongly from first principles:

| What | Where | Why it was hard |
|---|---|---|
| Coherent-gain calibration | `avc.engine.js` `windowCG`, `test_spectrum.js` §1 | A full-scale sine reads 0.00 dBFS at every block size and window. The window is applied by `fftin~`, not by convolving the spectrum — that convolution was wrong for weeks and only a skirt measurement showed it. |
| The 4.5 dB/octave slope | `gen_patch.py`, `test_spectrum.js` §3 | Pivots at exactly 1 kHz. Read off SPAN's own editor. |
| RMS ballistics | `avc.meters.js` | Matched to bx_meter: −3.6 against −3.6. `average~` has no float method, which is why a "300 ms" window silently ran at 4.35 s. |
| Correlation, balance, goniometer | `avc.meters.js`, `device.py` | L and R must come from one sample; two `jit.spill` off one outlet race, and mono then draws the same ball as wide. |
| The period detector | `avc.specui.js` `detectPeriod` | Autocorrelation, and wrong four ways before it was right — see below. |
| Third-octave bands | `avc.specui.js` `BAND_F` | Thirty ISO bands, which is what DIGICheck shows. |

## Findings that cost time here and would cost it again

- **`jit.gen` resamples any input whose dims differ from the output's**, which
  truncated the spectrum at 1.5 kHz and put full-scale spikes on it. Every
  matrix has to be sized to the bin count, not the plot width.
- **Max `expr` has no ternary, and exponent notation (`1e-20`) does not work**
  in an object box. It killed correlation, balance and both LUFS meters.
- **A single outlet feeding two destinations has undefined order.** Four
  separate bugs here, including the goniometer showing no stereo at all.
- **Live's CPU meter measures audio processing and excludes interface
  redrawing.** Two different costs were conflated for days: a macOS
  microstackshot profile put `jsui_paint` at 52% of the process while the
  device's MSP had zero samples in it.
- **Pitch detection**: normalise over the overlapping region, take the first
  local *maximum* (not the global one — that is the octave error), and compute
  one lag past the search limit or a low note's peak is never testable.
- **Note names follow Live's octave numbering** (middle C is C3), not
  scientific pitch notation. Agreeing with a textbook while disagreeing with
  the host by an octave is the worse failure.
- **Rekordbox's waveform scrolls at a constant rate and never sweeps**, and its
  bar markers give the rate independently. Its *future* half is not prediction
  — it analysed the file before playback. A live analyser on a master bus has
  no future to draw, which is why the native one will not have one either.

## Its own rules

- No third-party externals. Vanilla Max/MSP/Jitter and JS only.
- The Max Console stays near-silent in normal operation.
- `test/validate.py` holds the structural rules, and every rule in it was
  checked by planting the fault it catches. Several exist because the fault
  they catch is **silent**: a control whose value nothing reads, a `jit.spill`
  whose length disagrees with its matrix, a `live.path` outlet that answers
  only `goto`/`bang`/`getid`.

## Relationship to the native analyser

ADR-0183 in `adi_daw/docs/DECISIONS.md`. The native one is a Pd DSP tier with a
C++ UI; this one is Max and JavaScript. They share the maths and the acceptance
numbers and nothing else.
