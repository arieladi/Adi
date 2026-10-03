# Multiband Dynamics: Live's device, measured and rebuilt

The director's instruction (2026-10-03): a Pd device that copies Ableton's
Multiband Dynamics and does exactly what Live's does, one to one. Done by
**mac** on that direct instruction, inside win_codex's Pd-device area, with a
claim row in `collab/README.md`.

Unlike the earlier Live-parity devices, nothing here is read off the manual.
Live 11.2.7 Suite on this Mac rendered 300-odd probe signals through its own
Multiband Dynamics (`live-probe/`), and every claim below is a measurement of
those renders. The C++ core replays the same probes and is held to Live's
output sample by sample (`live-probe/nulltest.py`).

## What Live's device is

**2x oversampling, everywhere.** A polyphase IIR half-band: HIIR's design
(de Soras) with 8 coefficients and a 0.01 transition band, first-order
allpass sections `y = (x - y1) * a + x1` in float. The upsampler's even path
makes the first sample, the odd path the second; the downsampler pairs each
first sample with the previous second one. Reproduced **bit for bit**: the
single-band impulse response nulls to exactly zero, the resampler's own DC
gain of 0.9999974 included. Zero latency.

**Crossovers at 96 kHz.** Linkwitz-Riley 4th order as two identical
Butterworth sections, bilinear with prewarping, Q = **0.707**: the -0.0026 dB
dip Live shows at every crossover is exactly `20 log10(2 * 0.707^2)`. Phase
says how the bands are formed:

- low = LP(fL) x AP(fH), high = HP(fH) x AP(fL),
- mid = HP(fL)LP(fH) - LP(fL)HP(fH), so the three always sum to AP(fL)AP(fH).

Low split off: low = LP(fL), mid = HP(fL). High split off: mid = LP(fH), high
= HP(fH). Both off: one band. With the low split at or above the high split,
both sit at the high split and the mid band is exactly silent.

**Detector.** Per band, stereo-linked, on the band after its Input gain:
Peak reads `(|L| + |R|) / 2` (opposite-polarity channels compress fully),
RMS reads `(L^2 + R^2) / 2`. A one-pole follower with separate attack and
release coefficients, chosen by whether the input is above the envelope.
Attack and release reach -80 dB (1e-4) of the way in the set time, scaled by
Time Scaling, per oversampled sample.

**Gain computer.** Above: `r_a (L - T_a)` past the threshold. Below:
`r_b (T_b - L)` under it. Live stores r, not a ratio: Above r in [-1, 1] shows
as `1 : 1/(1+r)`, Below r in [-3, 1] as `1 : 1/(1-r)`. Soft knee: a quadratic
over 20 dB centred on each threshold. The two sides simply add, even with
crossed thresholds or overlapping knees. Amount multiplies r. The dynamic gain
stops at **64x (+36.12 dB)** however it is asked for more; Input and Output
gains sit outside that cap; there is no floor.

**Bands and output.** Band out = band x Input x dynamic gain x Output. An
inactive band passes raw, ignoring its gains. Solo plays only soloed bands; a
soloed inactive band plays raw, and soloing a band whose split is off is
silence. Bands sum, then Master Output.

**Sidechain.** The trigger is `cos(Mix * pi/2) x input + sin(Mix * pi/2) x
(S/C Gain x sidechain)`, mixed at the host rate before the resampler
(bit-exact), then split into bands like the input, so a 5 kHz sidechain ducks
only the high band. Listen plays the trigger.

**Live's own defaults** (a fresh device, read from Live): splits 120 Hz and
2.50 kHz, Soft Knee on, RMS, thresholds -20/-60 dB, ratios 1:1, attack
50/10/5 ms and release 300/200/100 ms (low/mid/high), S/C off at 0 dB, 100 %.

**Not the device.** Live fades every clip edge over 192 samples, Fade
setting or not; and its automation turns a jump into a centred ~4 ms S-curve
before the device sees it. The core carries a causal twin of that S-curve so a
parameter jump from ADI's host sounds as it does in Live; it is two 94-sample
boxcars at 48 kHz.

## The patch

`pd/devices/MultibandDynamics.pd`: `[adc~ 1 2 3 4]` (3/4 the sidechain) into
`[adi.multiband_dynamics~]` into `[dac~ 1 2]`, and 43 `[adi.param]`s in Live's
own order and names: Live's 37 automatable parameters (Device On is the
host's) and the six it keeps in the set without automating (Low/High split
buttons, three Solos, S/C Listen). Ranges, defaults and curves are Live's:
crossovers, attack, release and Time Scaling are log10 in Live and `log` here;
the ratio parameters carry Live's r.
