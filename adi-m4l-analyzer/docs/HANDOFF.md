# Handoff: match AVC Spectrum Meter to SPAN and bx_meter

Paste this whole file as the opening prompt of a new session.

---

## What this is

`~/Documents/GitHub/AVC-Spectrum-Meter` is a Max for Live audio effect that puts
a SPAN-style FFT analyser and a bx_meter-style Peak/RMS/Dynamic meter inside
Live's 155 px chain strip. The patchers are **generated**: edit `build/*.py` and
re-run, never edit `device/*.maxpat` or the `.amxd`. The three `.js` files in
`device/` are hand-written.

```bash
cd build && python3 fft_analysis.py && python3 gen_patch.py && python3 device.py
cd .. && python3 test/validate.py && node test/test_spectrum.js && node test/test_weighting.js
# install:
cp device/"AVC Spectrum Meter.amxd" device/avc.*.maxpat device/avc.*.genjit device/avc.*.js \
   ~/Music/Ableton/User\ Library/Presets/Audio\ Effects/Max\ Audio\ Effect/
```

The device never touches audio: `plugin~ -> plugout~` is the entire path and
`validate.py` fails the build if anything else ever feeds `plugout~`. Keep it
that way.

## THE RULE FOR THIS WORK

**Measure. Do not infer.** Every single bug that actually mattered in this
project was found by measuring, and every wrong turn came from reasoning about
what Max "should" do. Five rounds were lost to plausible-sounding theories.

You have screen access — this works and is the most valuable tool here:

```bash
screencapture -x -t png /tmp/shot.png          # full screen
screencapture -x -t png -R x,y,w,h /tmp/r.png  # region, in POINTS; ~200 ms per frame
```

Python + PIL are installed. The established method: capture a burst, find each
plot's pixel bounds and its dB calibration from its own gridlines, extract the
curve as the topmost run of N consecutive fill-coloured pixels per column, and
convert to dB. Two calibration recipes that worked (RE-DERIVE THEM, the display
resolution and window positions change between sessions):

- SPAN: gridlines are RGB(60,60,60); two of them 12 dB apart give px/dB.
- Ours: gridlines are RGB(74,74,74), every 6 dB; range is `rangeHi..rangeLo`.
- Separate the live curve from the max-hold by requiring a RUN of bright pixels
  — the max curve's outline stroke is as bright as the live curve's fill, so a
  brightness threshold alone picks the wrong layer. This cost a whole round.

Max's own reference documentation is on this machine and is authoritative:
`/Applications/Ableton Live 11 Suite.app/Contents/App-Resources/Max/Max.app/Contents/Resources/C74/docs/`
(`refpages/**/*.maxref.xml`, `vignettes/`). Strip tags with Python. When a
refpage is ambiguous, disassemble the external in `.../C74/externals/` — that is
how the `average~` bug was proven.

## Reference settings, read off the running plug-ins

SPAN 3.23 Spectrum Mode Editor: Window **DOME**, Type **RT MAX**, 2nd Type
**MAX**, Block **4096**, Overlap **80.0**, Avg Time **194**, Filled, 2nd
Spectrum on, Anti-Alias on, Align 0 dB on, Offset off, Smooth off,
Freq **10.0 – 20.0k**, Range **-80.0 / -13.0**, Slope **4.50**.
SPAN global: **40 fps**, Density off, Large Cursor Readouts off.

Ours matches all of that except: overlap (pfft~ needs a power of two, so 80% is
unreachable — 75% is hop 1024, 87.5% is hop 512, SPAN's 80% is hop 819) and the
window (see below).

## What is VERIFIED and must not be re-litigated

Measured, not assumed:

- **Frequency mapping is correct.** The spectrum spans the full width and the
  curve tracks SPAN's shape.
- **Levels agree at 50 Hz within ~2 dB**: our max-hold -25.7 vs SPAN -23.1,
  our live -30.3 vs SPAN -28.8.
- **Both max-hold curves are true static infinite holds** (sd 0.00 over 9.3 s).
- **The display is not redraw-limited**: across 44 consecutive captures 207 ms
  apart our curve was never once identical to the previous frame.
- **Meters match bx_meter**: Peak -0.1 vs -0.1, RMS -3.6/-3.7 vs -3.6/-3.6.
  (Dynamic reads ~1 dB high: ours 3.1/3.3 vs bx 2.1/2.1 — still open, minor.)
- Checked against the documentation and correct: the Hann kernel
  `[0.5,-0.25,0,0]`, the 7-tap convolution tap indexing, `vectral~` receiving
  `slide 1 <fall>` (two floats, instant attack), `planemap` usage, `slideFor`
  working in hop units, and every parameter default.

## THE OPEN PROBLEM

Three symptoms that are probably ONE cause:

1. **Our live curve moves half as much as SPAN's.** At 50 Hz over 9.3 s: ours
   sd 1.44 dB / range 5.8; SPAN sd 2.90 / range 10.9. SPAN's series pulses
   visibly with the kick (`-31 -30 -29 -29 -30 -24 -29 -24 -29 -23`); ours
   wanders with no structure. The user cannot tell when the kick hits, and
   cannot see the bass arriving an eighth later. This is the headline complaint.
2. **Our high end is far too hot.** User-read values: at 20 kHz SPAN live -72 /
   max -52, ours live -48 / max -43. At 10 Hz SPAN -60/-55, ours -49/-43. At
   14 kHz the two are close (max: SPAN -32, ours -28) and the user says 14 kHz
   is the ONE band where our timing looks right.
3. **Our comb notches between 100 Hz and 1 kHz are much shallower than SPAN's.**

A hot high end + filled-in notches + compressed dynamics is the classic
signature of **spectral leakage**. Leakage raises a floor that tracks total
signal energy, which would explain all three at once, including why the level
error grows toward the band edges and why the dynamics are squashed.

Leading hypotheses, in order:

- **H1 — the window is not actually being applied in Max.** The maths is proven
  in `test/test_spectrum.js` to 1e-15, but that tests the JS kernel, NOT that
  the patch applies it. The FFT is taken rectangular (`fftin~ 1 square`) and
  Hann applied afterwards as a 7-tap convolution across bins in
  `build/fft_analysis.py`. If the coefficients never arrive, or a sign or tap
  index is wrong in the PATCH, the effective window is rectangular — whose
  sidelobes fall at only 6 dB/octave and would produce exactly these symptoms.
  **Test:** play a single sine and look at the skirt.
- **H2 — SPAN's "Dome" window is narrower than Hann.** Hann's mainlobe is
  4 bins; a sine window's is 3 and would cut deeper notches. Voxengo does not
  document Dome's definition anywhere I could find. NOTE: this device applies
  the window by spectral convolution, which is exact only for a cosine-sum
  window; a sine window is not one, so it cannot be added as another kernel —
  it would need the time-domain path and one `pfft~` per window.
- **H3 — the per-column MAX aggregation** in `build/gen_patch.py` inflates and
  freezes the high end (a column at 20 kHz spans ~23 bins at 4096; the max of 23
  held peaks barely moves). Below ~874 Hz a column spans less than one bin and
  the code interpolates instead, so this cannot explain the LOW-end dynamics.

## RUN THESE EXPERIMENTS FIRST, ON CONTROLLED SIGNALS

Stop testing on music. The user has SPAN, bx_meter and our device on the Live
master and will cooperate. Ask them to load a test-tone source (Operator, or an
audio file) and run:

1. **1 kHz sine at -18 dBFS, on a bin centre (use 1033.59 Hz = bin 96 at 44.1k
   with 4096).** Compare the SKIRT of the peak in SPAN vs ours: how many dB down
   at ±2, ±5, ±20, ±100 bins. This is the decisive test for H1/H2. A rectangular
   window gives a first sidelobe at -13 dB falling ~6 dB/octave; Hann gives -31
   dB falling ~18 dB/octave. It will also directly reveal Dome's mainlobe width.
2. **White noise at -20 dBFS.** Both analysers should render flat with slope 0.
   Compare absolute level and, critically, the frame-to-frame variance at several
   frequencies — that isolates the ballistics from the spectral content.
3. **A single click / impulse every 2 s.** Watch the decay of the RT Max curve
   in both. This measures the actual fall time against the nominal 194 ms.
4. Only then go back to the kick-and-bass loop.

## Max/MSP traps already paid for — do not rediscover these

- Live's **Int parameters hold only 256 values**; it silently rewrites `mmax` to
  `mmin+255`. All numeric params are Float type with Int unit style.
- Max's **`expr` has no ternary operator**; such an object never outputs.
- **Exponent notation (`1e-20`) is undocumented for object-box arguments.** Four
  `expr` objects containing it produced no output at all — correlation, balance
  and both LUFS meters were dead. `validate.py` now rejects it.
- **`jit.spill` emits exactly `listlength` values** whatever the matrix holds.
- **`jit.gen` RESAMPLES any input whose dimensions differ from the output's**,
  and `samplepix` past the edge WRAPS. This was the 1.6 kHz truncation and the
  two full-scale bands at 6.6 and 13.3 kHz. Every matrix in the reduction chain
  must be the same width; `test_spectrum.js` test 7 now enforces it.
- **`average~` has no `float` method** (methods: int/absolute/bipolar/rms/signal)
  and silently ignores an interval longer than its creation argument, which is
  also its initial value. A float window is rejected, not rounded.
- **`vectral~`**: vector size is a creation arg defaulting to 512 but there IS a
  `size` message; `slide` takes TWO floats (up, down).
- **One outlet fanning to several destinations has undefined order.** This made
  the correlation `expr` fire before its inputs arrived, pinning it to ±1.
- `pfft~` must be created by `thispatcher script newdefault` from a message box;
  numbers from JS `newdefault()` did not arrive as ints and it fell back to 512.
- A `.genjit` needs `"classnamespace": "jit.gen"`; `jit.gen` needs `@gen`.
- `mgraphics` has `pattern_create_linear` / `add_color_stop_rgba` / `set_source`.

## Agents

Two multi-agent runs (6 agents each) failed entirely on account session limits
and returned nothing, twice, burning ~400k tokens. **Verify agents actually work
before depending on them** — spawn one trivial agent first. If they run, the
highest-value fan-out is: one agent per hypothesis above, each required to prove
its claim with pixel measurements or the Max documentation, plus an adversarial
verifier per finding. If they fail again, do the work inline — it is tractable.

## Deliverable

The user wants our analyser and meter to be visually and behaviourally
indistinguishable from SPAN and bx_meter side by side on the same signal. Judge
every change by re-measuring against them, not by reasoning about it. Read
`README.md` (numbered history of 22 fixed bugs) and `docs/DEVIATIONS.md` before
starting.
