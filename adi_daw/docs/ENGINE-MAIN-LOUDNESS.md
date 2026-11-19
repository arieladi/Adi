# Main's live loudness readout (N13)

The window reads `session.mainLoudness().readout()` with independent relaxed
loads of `momentaryLufs`, `shortTermLufs`, `integratedLufs` and `truePeakDb`.
Values are dB/LUFS; silence and an unwarmed window are negative infinity.
`session.mainLoudness().requestReset()` queues a reset for the next callback.
These readings are live scalars, outside the snapshot and the op journal.

Session measures its final Main output after the fader, summing and graph fade.
It prefers `AudioIo::out64` when present; regular audio devices supply float.
Measurement never changes samples. Momentary uses 400 ms, short-term 3 s,
updated every 100 ms. K-weighting reuses `dsp::AutoGain`'s rate-specific shelf
and high-pass coefficients. Integrated measurement uses 400 ms windows at
100 ms hops, an absolute -70 LUFS gate and a relative -10 LU gate. Its bounded
0.1 LU histogram stores actual sums/counts per bin, with at most 0.1 LU gate
quantisation, rather than retaining an ever-growing list of windows.

True peak holds the largest reconstructed or original sample magnitude. It
uses the existing `dsp::TruePeakMeter` Kaiser-sinc design, evaluated in double:
4x below 88.2 kHz, 2x below 176.4 kHz and sample peak above. Original samples
always count. Filters/history are retained between callbacks and reset together
with windows and gates. The visual meter does not add audio latency.

An ordinary graph rebuild or same-format prepare retains history. A different
sample rate/channel layout publishes fresh DSP state; its old state is reclaimed
by `Session::tick` after audio has moved past it. Device stop queues a reset.
No callback allocates, locks, logs or accesses the store.

Mono/stereo use BS.1770 channel weights of one. The standalone `MainLoudness`
API accepts explicit channel weights: front 1, surround 1.41, LFE 0. Until the
engine has named channel roles, Session treats all its output channels as
full-range; it does not infer an LFE from an ordinal. Nonfinite samples count
as silence in the meter, and extreme wide samples are bounded in measurement
only, preserving finite filter state. The audible output remains untouched.

Headless checks compare measured tones against an independent complex filter
response, exercise both gates and graph widths, known intersample crests,
format/reset lifetime, and actual post-Main-fader Session output. These are
mathematical/runtime checks, not a claim of certified EBU or measured Live parity.

The standalone measurement format also accepts 8--768 kHz, matching offline
export. The export normalizer uses an independent instance for its selected
range and encoded result; it does not read a live Session's accumulated meter.
