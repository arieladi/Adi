# Vocoder — manual checklist before code

Live 12 chapter 28.42, printed pages 658–661, read from the local reference PDF. Behaviour below is written in our own words.

- Analyse the main input with band-pass filters and apply their envelopes to the carrier bank; preserve a dry/wet control.
- Offer noise (rate/density), external sidechain, self/modulator and monophonic pitch-tracked carriers, including oscillator waveform, tracking limits and coarse pitch.
- Provide carrier enhancement, unvoiced noise, sensitivity and fast/slow detection.
- Expose band count, range, bandwidth, precise/retro response, individual band attenuation, gate, level, depth, attack/release and carrier formant shift.
- Mono sums both sources; Stereo sums the modulator only; L/R retains both stereo sources.
- Measure band selectivity, envelope/gate/depth, formant shift, carrier modes, pitch hold, channel routing, block sizes 32–4096 and zero process allocations. Exercise the top-level device through LibPdEngine, checking a far bin before a peak, sidechain routing and a clean console; write only a float WAV.

Source plan: scalar adaptation of Surge XT VocoderEffect and VectorizedSVFilter at 58914e59c608ed4384ba6002e44c3465c58b2e71. Retain upstream GPL-3.0-or-later headers. Live's carrier and envelope controls wrap the Surge bank; unverified numeric choices will be consolidated in one table. Stacked on #189 for CMake discovery.

Implementation: the GPL Surge two-stage state-variable band-pass recurrence, logarithmic bank and squared/gated envelope driving each carrier filter are adapted directly. Headers are preserved. The scalar bank takes two substeps per sample for stability with Live-style broad bands; it is not a bit-exact Surge effect. ADI adds carrier generation, independently timed attack/release, depth, per-band attenuation, Retro tilt and formant control. Autocorrelation tracks monophonic pitch and holds the last reliable estimate through silence; this is an ADI detector, not Live's proprietary algorithm. Enhancement uses a bounded per-band normalizer; unvoiced detection uses a high-frequency energy ratio. Both are ADI interpretations of the manual's behaviour.

External carrier uses existing NodeIo.sidechain -> Pd adc~ channels 3/4 -> the built-in's third/fourth signal inlets. Main input is the modulator. The top-level canvas uses adc~ 1 2 3 4 and dac~ 1 2. Host code is untouched. The consuming device instance must declare four input channels and route its sidechain; absent carrier input is silence. The 20-band maximum follows Surge's fixed bank capacity, not an asserted Live limit. Numeric Live parity remains unverified.

All values below are **ADI values, unverified against Live**, including defaults, units and curves. Menu entries are in index order. The chapter explicitly describes Depth 0/100/200%, sensitivity endpoints and mono/stereo/LR semantics; numerical time/frequency/gain settings still need the director's Live session.

| Live control / ADI name | ADI range or choices | Default | Unit | Curve |
|---|---|---|---|---|
| Carrier | Noise External Modulator PitchTracking | 0 | - | menu |
| NoiseRate | 100 to 48000 | 48000 | Hz | log |
| NoiseDensity | 0 to 100 | 100 | % | lin |
| PitchLow | 40 to 1000 | 80 | Hz | log |
| PitchHigh | 100 to 4000 | 1200 | Hz | log |
| Waveform | Saw Pulse50 Pulse25 Pulse12 | 0 | - | menu |
| Pitch | -48 to 48 | 0 | st | lin |
| Enhance | 0 to 1 | 0 | - | int |
| Unvoiced | 0 to 100 | 0 | % | lin |
| Sensitivity | 0 to 100 | 50 | % | lin |
| Fast | 0 to 1 | 1 | - | int |
| Bands | 4 8 12 16 20 | 4 | - | menu |
| Low | 40 to 4000 | 80 | Hz | log |
| High | 1000 to 20000 | 12000 | Hz | log |
| Bandwidth | 10 to 200 | 100 | % | lin |
| Retro | 0 to 1 | 0 | - | int |
| Gate | -120 to 0 | -96 | dB | lin |
| Level | -60 to 24 | 0 | dB | lin |
| Depth | 0 to 200 | 100 | % | lin |
| Attack | 0.1 to 1000 | 5 | ms | log |
| Release | 1 to 5000 | 100 | ms | log |
| Channels | Mono Stereo LR | 1 | - | menu |
| Formant | -24 to 24 | 0 | st | lin |
| Mix | 0 to 100 | 100 | % | lin |
| Band1 | -60 to 0 | 0 | dB | lin |
| Band2 | -60 to 0 | 0 | dB | lin |
| Band3 | -60 to 0 | 0 | dB | lin |
| Band4 | -60 to 0 | 0 | dB | lin |
| Band5 | -60 to 0 | 0 | dB | lin |
| Band6 | -60 to 0 | 0 | dB | lin |
| Band7 | -60 to 0 | 0 | dB | lin |
| Band8 | -60 to 0 | 0 | dB | lin |
| Band9 | -60 to 0 | 0 | dB | lin |
| Band10 | -60 to 0 | 0 | dB | lin |
| Band11 | -60 to 0 | 0 | dB | lin |
| Band12 | -60 to 0 | 0 | dB | lin |
| Band13 | -60 to 0 | 0 | dB | lin |
| Band14 | -60 to 0 | 0 | dB | lin |
| Band15 | -60 to 0 | 0 | dB | lin |
| Band16 | -60 to 0 | 0 | dB | lin |
| Band17 | -60 to 0 | 0 | dB | lin |
| Band18 | -60 to 0 | 0 | dB | lin |
| Band19 | -60 to 0 | 0 | dB | lin |
| Band20 | -60 to 0 | 0 | dB | lin |
| Internal bank law | Q=1/(sqrt(r)-1/sqrt(r)) * 100/BW, bounded 0.707–100; r=adjacent frequency ratio | Precise | ratio | Retro multiplies Q by sqrt(f/low), gain by its square root |
| Internal stability | two substeps; band centres bounded 10 Hz–0.2*sample rate | always | Hz | Surge spread 0.4/Q, reciprocal damping |
| Internal pitch detector | 1024 samples at <=~12 kHz; 512-sample comparison every 128 decimated samples; correlation >0.85 | holds 220 Hz until acquired | Hz | parabolic lag interpolation; one-pole input cutoff 1.5*High |
| Oscillator widths | 50, 25, 12.5 | saw | % | naive oscillator; aliases at high pitches, not Live parity |
| Internal enhancement | power follower 0.999, gain capped 8, epsilon 1e-6 | off | ratio | inverse RMS |
| Unvoiced detector | 2 kHz split; 5/50 ms fast/slow | fast | ms | squared-energy comparison and one-pole switching |
| Band floor / output | -60 dB band endpoint mutes; upstream envelope power cap 6 and output factor 4 | all bands 0 dB | dB | linear wet/dry |

Validation: MSVC /WX core and real-Pd suites pass; Clang frontend conversion diagnostics checked as well. Core checks cover selectivity, octave formant move, gate/depth, every carrier, envelope timing, pitch hold/reacquisition and oscillator tuning, bandwidth, level, routing, 8–192 kHz stability, exact partition equality at 32–4096 and zero process allocations. The real-Pd suite checks the floor before the audible carrier peak, all 44 parameter routes, missing-sidechain silence, zero allocations and a clean console, then writes vocoder-pd-render.wav. A scalar reference impulse also verifies the pinned Surge recurrence with the documented two substeps.

Final native result: 44 core checks and 57 real-Pd checks passed. Fault plant: forcing the bank output to zero caused 20 core failures and the Pd audible-peak failure; restored code passed both suites again. The far bin alone therefore cannot certify silence as a working device.
