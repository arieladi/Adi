# Architecture

## Signal flow

```
plugin~ ─────────────────────────────────────────────────────────► plugout~
   │                                                  (the entire audio path)
   ├─► +~ ─► *~0.5 ──► Mid / Sum
   ├─► -~ ─► *~0.5 ──► Side
   │
   ├─► selector~ srcA ─► pfft~ avc.fftanalysis  ─► jit.poke~ ─► #0_specA
   ├─► selector~ srcB ─► pfft~ avc.fftanalysis  ─► jit.poke~ ─► #0_specB
   │      (created at runtime by avc.engine.js; one per engine)
   │
   ├─► selector~ mtrL ─► peakamp~ / abs~+slide~ / biquad~×3 ─► *~ ─► slide~ ─► snapshot~
   ├─► selector~ mtrR ─► (same)
   └─► *~ (LR, L², R²) ─► slide~ ─► snapshot~ ─► correlation, balance
```

Nothing is inserted anywhere. `plugout~` has exactly two inputs, both from
`plugin~`, and `test/validate.py` fails the build if that is ever untrue.

## Why these choices

**`jsui` + `mgraphics`, not `jit.gl.sketch` into a `jit.pwindow`.** Twenty
instances of a GL-backed device means twenty GL contexts plus per-frame texture
readback, which is both heavy and historically fragile inside Live — and Push 3
standalone makes it worse. `jsui` is CPU-drawn and antialiased, gives mouse
hover, drag and right-click natively (needed for the cursor readout), and
renders text without any extra machinery. The cost is that drawing is
single-threaded on the main thread, which is why the drawing layer never sees a
raw bin: it receives one already-reduced list per curve per frame, at most 1024
points, and loops only over those.

**Ballistics live in the signal domain, inside `pfft~`.** At 4096/overlap-4 and
44.1 kHz the FFT produces ~43 frames per second, but the display runs at 30 —
and at 1024/overlap-4 it produces 172. Running the ballistics at UI rate would
drop 30–80% of frames and RT Max would miss peaks outright. `vectral~` applies a
one-pole per *bin* across frames, so every frame is seen. All five analysis
types are computed simultaneously into five matrix planes, so switching primary
or secondary type is instant and never restarts an accumulation.

| plane | type | how |
|---|---|---|
| 0 | RT Avg | `sqrt` of one-pole-averaged power |
| 1 | RT Max | `vectral~ slide 1 D` — instant attack, Avg-Time fall |
| 2 | Max | same with an effectively infinite fall; `clear` resets it |
| 3 | Avg | `frameaccum~(m²)` ÷ frame count |
| 4 | RT Sigma | `sqrt(E[m²] − E[m]²)`, same time constant as RT Avg |

**Cost is independent of block size.** The per-bin work is proportional to
sample-rate × overlap ÷ 2 bins per second — 88.2k bins/s at 44.1 kHz overlap 4,
whatever the FFT size. Larger blocks cost more per frame but produce
proportionally fewer frames.

**The reduction visits each bin exactly once.** `avc.specreduce.genjit` walks the
bins belonging to each display column and takes their max, so the whole pass is
O(nbins), not O(width × nbins). A plain interpolated resample would alias badly:
at 16384/44.1 kHz one pixel near 20 kHz spans ~95 bins, and a tone falling
between sample points would flicker or vanish. Where a column is narrower than
one bin — the low end at small block sizes — it interpolates instead, which is
what keeps the bottom octaves smooth. GenExpr's `for` loop makes the variable
bin count expressible in one compiled pass.

## Level calibration

A real sine of amplitude A on a bin centre gives `|X| = A·N·CG/2`, where CG is
the window's coherent gain (= a₀ for any cosine-sum window). So:

```
amplitude = 2·|X| / (N · CG)     <- sent to the DSP as one gain, `gcal`
dB_raw    = 20·log10(amplitude)   (DC gets no ×2)
dB_shown  = dB_raw + align + slope · log2(f / 1000)
```

Verified exact at every block size and every window in `test/test_spectrum.js`.

## Where to change things

| To change | Edit |
|---|---|
| Window kernels, coherent gain, Hi-Res choice | `avc.engine.js` → `WINDOWS` |
| Avg-Time → one-pole conversion | `avc.engine.js` → `slideFor()` |
| Level calibration | `avc.engine.js` → `pushWindow()` |
| Log mapping, per-pixel aggregation, slope, smoothing | `build/gen_patch.py` → `CODE` |
| Colours, layout, grid, readout, meter drawing | `avc.specui.js` (all colours in `COL`) |
| Meter ballistics, A/C/K filters, bias modes | `avc.meters.js` |
| The FFT DSP itself | `build/fft_analysis.py` |
| Anything structural in the device | `build/device.py` |

Re-run the generator after editing any `build/*.py`; the `.amxd`, `.maxpat`,
`.maxpat` subpatcher and `.genjit` are all generated and will overwrite manual
edits. The three `.js` files are hand-written and are *not* generated.

## Message contracts

`avc.specui.js` receives `c0`–`c3` (one dB list per curve), `meters` (20
floats), and one setter per display parameter. It sends back `plotw` (so Max
sizes the reduction to the actual plot width), `reset`, `pin`, and `idle`.

`avc.engine.js` receives configuration and `plotw`; it owns the two `pfft~`
objects and pushes window kernels, calibration, ballistic coefficients and the
gen parameters. `avc.meters.js` receives configuration and sends back filter
coefficients, ballistic coefficients, the hold time, the bias offset and the
meter scale span.

`test/validate.py` checks these contracts across files: every message the patch
sends to a `js`/`jsui` object must have a matching handler, and every varname
the JS looks up must exist in the patch.
