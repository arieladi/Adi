# Multiband Dynamics: Live's device, measured and rebuilt to the bit

The director's instruction (2026-10-03): a Pd device that copies Ableton's
Multiband Dynamics and does exactly what Live's does, one to one. Done by
**mac** on that direct instruction, inside win_codex's Pd-device area, with a
claim row in `collab/README.md`.

Nothing here is read off the manual. Live 11.2.7 Suite on this Mac rendered
about 650 probe signals through its own Multiband Dynamics, at 44.1, 48 and
96 kHz (`live-probe/`), and every claim below is a measurement of those renders.
The C++ core replays the same probes and is held to Live's output sample by
sample: **447 of 468 static probes and 17 of 23 switch-automation probes are
identical to Live's to the last bit.** The rest are named under "Not yet the
bit" with their size.

## The signal path

**2x oversampling, at any host rate.** A polyphase IIR half-band: HIIR's design
(de Soras), 8 coefficients, transition 0.01, first-order allpass sections
`y = (x - y1) * a + x1` in float. The upsampler's even path makes the first
sample, the odd path the second; the downsampler pairs each first sample with
the previous second one. Zero latency. Bit-exact at 44.1, 48 and 96 kHz.

**Crossovers at twice the host rate.** Linkwitz-Riley 4th order as two identical
RBJ-cookbook Butterworth sections computed entirely in float, run as direct
form I (`y = b0x; y += b1x1; y += b2x2; y -= a1y1; y -= a2y2`), Q = **0.707**
(the -0.0026 dB dip Live shows at every crossover is `20 log10(2 * 0.707^2)`),
`w = 2 pi f (1/fs)` in float. The split frequency is held as a float log10:
`f = powf(10, log10f(value))`.
- low = AP(fH) after LP4(fL); high = HP4(fH) after AP(fL), each AP one biquad;
- mid = g * LP4(fH) after HP4(fL), g = 1 - 10^((-24 dB/oct * log2(fH/fL)) * 0.05),
  so the bands do not quite sum to an allpass, exactly as Live's do;
- low split off: low = LP(fL), mid = HP(fL); high split off: mid = LP(fH), high =
  HP(fH); both off: one band; low split at or above the high split: both sit at
  the high split and g = 0 silences the mid band.

Live's float artefacts come with it. At a 30 Hz split Live's low band has a DC
gain of 0.971 at 44.1 kHz, 0.960 at 48 kHz and 1.137 at 96 kHz: float rounding
of biquad poles near z = 1. The core reproduces all three to the bit, and Live
renders with denormals flushed, so the core flushes them too.

**Detector.** Per band, stereo-linked, on the band after its Input gain. Peak
reads `(|L| + |R|) / 2` (opposite-polarity channels compress fully); RMS reads
`(L^2 + R^2) / 2`. It starts at 0.01 in its own units and updates as
`(c * env) + ((1 - c) * d)` with `c * env` rounded on its own; attack if
`d > env`. `c = float(exp(ln(1e-4) / (ms * Time * 2 * rate / 1000)))`, exact to
the last bit, with no clamp from 0.01 ms to 50 s.

**Gain computer, in log2 units.** Live's own fast log2 and 2^x: continuous
least-squares cubics on the float's mantissa. Thresholds are
`float(T) * (1 / 6.02f)`: Live's dB per octave is 6.02, not 6.0206. Above:
`r_a (L - T_a)`; below: `r_b (T_b - L)`. Soft knee: a quadratic over 20 dB from
the knee's start, `r * (t * t * 0.1505f)`. The two sides add even when the
thresholds cross; the sum is capped at exactly 6 octaves (64x, +36.12 dB). Live
stores r, not a ratio: Above r shows as `1 : 1/(1+r)`, Below r as `1 : 1/(1-r)`.
RMS mode takes its amplitude from the ARM reciprocal-square-root and reciprocal
estimates with one Newton step each (Live runs on NEON here): emulated portably,
checked against the M1's own instructions on every positive float.

**Bands and output.** Band out = `((band * in) * G) * out`; Master after the
downsampler. Gain knobs are `powf(10, dB * 0.05f)`: the product, not dB/20,
and the platform's powf. An inactive band passes raw. Solo plays only soloed
bands; soloing a band whose split is off is silence.

**Sidechain.** Trigger = `dry * input + (wet * gain) * sidechain` at the host
rate, before the resampler; `wet = sin(m pi/2)` of the float mix,
`dry = sqrtf(1 - wet^2)` (at 99.9 % that is 0.00158221, not cos's 0.00157080).
The trigger is band-split like the input. S/C Gain below -69.7 dB is off: Live
is silent at -69.9 dB and on at -69.5 dB. **Listen** plays whenever it is on,
S/C on or off: Master x the sum of input gain x split trigger over the bands
that compress (inactive bands add nothing, output gains are not applied).

**Switches mid-stream.** Peak/RMS carries the envelope over (sqrt or square), so
nothing moves. S/C On moves the trigger over 1.5 ms (72 samples at 48 kHz) of an
accumulated float smoothstep `t*t*3 - t*t*t*2`; a switch inside a ramp waits for
it. The sidechain path stops while S/C is off and resumes where it stopped.
Band activators and the knee switch act at their own sample.

**Live's own defaults** (a fresh device): splits 120 Hz and 2.50 kHz, Soft Knee
on, RMS, thresholds -20/-60 dB, ratios 1:1, attack 50/10/5 ms and release
300/200/100 ms (low/mid/high), S/C off at 0 dB, 100 %.

## What belongs to the host, not the device

Measured, and left to ADI's host because Live's host does them, not the device:
- **Device On**: a 1 ms raised-cosine crossfade at the switch's sample; while
  off the device is not processed; switching back on clears detectors and
  crossovers but keeps the resampler. `resumeAfterBypass()` gives the host that
  reset; the fade's last ulp (37 of 48 samples exact) is unresolved.
- **Automation**: Live turns a jump into a centred ~4 ms S-curve. The core
  carries a causal twin (two 94-sample boxcars at 48 kHz, the director kept it)
  so a jump from ADI's host sounds as it does in Live. Some automation points
  land one sample late in Live; the rule is not known.
- **Clip edges and silence**: Live fades every clip over ~4 ms and zeroes a
  track's output once its tail has decayed below about -140 dBFS.

## Not yet the bit

- **S/C Gain's last ulp.** Live multiplies by a float one or two ulps from the
  stored gain for some values (2.0 acts as 1.99999976). Twenty probes null at
  -128 to -178 dB instead of exactly. Battery `mbd_scg` (162 probes) is built to
  pin the mapping and waits for a render.
- **Platform libm.** Live's numbers come from macOS. The crossovers, knobs and
  frequency mapping use the platform's float sin/cos/pow/log as Live's do, so
  on another libm they can round an ulp differently. At a 30 Hz split that moves
  the low band audibly, as it would for Live itself on Windows. The unit test
  checks bit-exactness on macOS and a tolerance elsewhere.

## The patch and the tests

`pd/devices/MultibandDynamics.pd`: `[adc~ 1 2 3 4]` (3/4 the sidechain) into
`[adi.multiband_dynamics~]` into `[dac~ 1 2]`, and 43 `[adi.param]`s in Live's
order and names: Live's 37 automatable parameters (Device On is the host's) and
the six it keeps unautomated (Low/High split buttons, three Solos, S/C Listen).
Ranges, defaults and curves are Live's; the ratio parameters carry Live's r.

`tests/test_multiband_dynamics.cpp` (86 checks) holds the core to Live's own
samples. `live-probe/plant.py` plants 20 defects, one per finding: 19 are
caught; the 20th is the same behaviour (soloing a band whose split is off
silences it either way). `tests/test_multiband_dynamics_pd.cpp` (53 checks)
opens the real patch in libpd; removing the sidechain wiring or one
`[adi.param]` makes it fail.

Agents did much of the measuring (filters, gain computer, detector, routing,
switch automation), each on a private copy and proven against the renders
before it was merged.
