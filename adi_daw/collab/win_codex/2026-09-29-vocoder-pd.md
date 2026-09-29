# Vocoder — manual checklist before code

Live 12 chapter 28.42, printed pages 658–661, read from the local reference PDF. Behaviour below is written in our own words.

- Analyse the main input with band-pass filters and apply their envelopes to the carrier bank; preserve a dry/wet control.
- Offer noise (rate/density), external sidechain, self/modulator and monophonic pitch-tracked carriers, including oscillator waveform, tracking limits and coarse pitch.
- Provide carrier enhancement, unvoiced noise, sensitivity and fast/slow detection.
- Expose band count, range, bandwidth, precise/retro response, individual band attenuation, gate, level, depth, attack/release and carrier formant shift.
- Mono sums both sources; Stereo sums the modulator only; L/R retains both stereo sources.
- Measure band selectivity, envelope/gate/depth, formant shift, carrier modes, pitch hold, channel routing, block sizes 32–4096 and zero process allocations. Exercise the top-level device through LibPdEngine, checking a far bin before a peak, sidechain routing and a clean console; write only a float WAV.

Source plan: scalar adaptation of Surge XT VocoderEffect and VectorizedSVFilter at 58914e59c608ed4384ba6002e44c3465c58b2e71. Retain upstream GPL-3.0-or-later headers. Live's carrier and envelope controls wrap the Surge bank; unverified numeric choices will be consolidated in one table. Stacked on #189 for CMake discovery.
