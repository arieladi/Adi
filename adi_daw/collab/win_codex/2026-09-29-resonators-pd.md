# Resonators — manual checklist before code

Read Live 12 chapter 28.31, printed pages 617–618, from the local reference. Behaviour in our own words:

- Five parallel tuned resonators, each independently enabled and gained; switching I off leaves II–V working.
- A shared input filter selects low-pass, band-pass, high-pass or notch and cutoff.
- Two resonation characters, pitch-dependent decay with a constant-time option, and Color for brightness.
- I processes both input channels; II/IV take left and III/V right. Width acts on the wet stereo voices; output gain does not alter the dry endpoint.
- Root note and fine tuning, with independent relative pitch/fine offsets for II–V; support explicitly supplied alternate tuning frequencies without ambient settings in the DSP.
- Measure pitch in cents, relative/fine tuning, decay time and Const, routing, mode/color/filter behaviour, exact 32–4096 block equality and zero process allocations. Real-Pd proof checks a floor bin before a peak, clean console and a file-only render.

Source plan: Surge XT CombulatorEffect's parallel tuned combs, with the scalar equivalent of its sst-filters COMBquad feedback/read/write recurrence and sst-basic-blocks softclip polynomial. Surge's separate ResonatorEffect is three band-pass filters, not the string/comb structure needed here (ADR-0192 explains this distinction). Read the actual pinned source before choosing coefficients. Keep copied GPL headers and identify differences and unverified values in the PR. Stack on #189.

Implementation and source boundary: Surge XT CombulatorEffect at 58914e59c608ed4384ba6002e44c3465c58b2e71 supplies the parallel tuned-comb design. Its actual comb is sst-filters COMBquad_SSE2 (e92d93a92beabde03fa4ab767b285fa21c6608d6), whose delayed read, feedback addition, soft-clipped write and wet output are adapted as scalar lanes. The softclip polynomial comes from sst-basic-blocks Clippers.h (a32b8aec14d661e415bb676bb2e2a0a4da4efc96). Original GPL headers remain intact in the copied/adapted files. The native test compares integer-period echoes with that recurrence, not a claim of complete Surge output parity.

ADI differences: five voices instead of Surge's three, Live's fixed L/R routing instead of pan controls, cubic interpolation instead of the SIMD sinc table, a decay-time feedback law instead of Surge's feedback knob, and a one-pole Color filter with phase compensation in the delay length. Mode A uses positive feedback; B uses negative feedback at half the period, retaining the selected fundamental with an odd-harmonic character. Both are ADI interpretations of the manual's qualitative modes. No oversampling or Surge noise injection is retained. These choices keep the callback preallocated and its output independent of block boundaries. A disabled voice skips its comb processing and retains its state for reactivation.

Tuning boundary: the core accepts five explicit resolved frequencies atomically via tuning(array), and the compiled-in external accepts `tuning f1 f2 f3 f4 f5` (`tuning` with no arguments clears it). The top-level patch exposes `$0-tuning_hz`. This supports alternate frequency sets without reading ambient project state. **Automatic project tuning/active-clip scale following and a scale-degree UI are not connected in this PR:** the Pd host has no published tuning/scale seam. The standard device uses MIDI-number root/relative semitones; the future host connection must resolve scale/tuning pitches before sending them. No claim is made that Live's displayed octave labels match ADI's MIDI-number convention.

All rows below are **ADI values, unverified against Live**, including ranges/defaults/units/curves. The manual verifies five voices, four input-filter types, the ±24-semitone relative range, independent switches/gains, and the described routing/Const/Width behaviours. It also specifies ±21 scale degrees when scale-aware; that host/UI mode remains the explicit boundary above.

| Live control / ADI name | ADI range or choices | Default | Unit | Curve |
|---|---|---|---|---|
| FilterOn | 0 to 1 | 0 | - | int |
| FilterType | LowPass BandPass HighPass Notch | 0 | - | menu |
| Frequency | 20 to 20000 | 10000 | Hz | log |
| Mode | A B | 0 | - | menu |
| Decay | 0.05 to 20 | 2 | s | log |
| Const | 0 to 1 | 0 | - | int |
| Color | 0 to 100 | 75 | % | lin |
| Note | 0 to 72 | 48 | MIDI | int |
| Fine1 | -100 to 100 | 0 | ct | lin |
| On1 | 0 to 1 | 1 | - | int |
| Gain1 | -60 to 12 | 0 | dB | lin |
| On2 | 0 to 1 | 1 | - | int |
| Pitch2 | -24 to 24 | 7 | st | int |
| Fine2 | -100 to 100 | 0 | ct | lin |
| Gain2 | -60 to 12 | -6 | dB | lin |
| On3 | 0 to 1 | 1 | - | int |
| Pitch3 | -24 to 24 | 12 | st | int |
| Fine3 | -100 to 100 | 0 | ct | lin |
| Gain3 | -60 to 12 | -6 | dB | lin |
| On4 | 0 to 1 | 1 | - | int |
| Pitch4 | -24 to 24 | 19 | st | int |
| Fine4 | -100 to 100 | 0 | ct | lin |
| Gain4 | -60 to 12 | -6 | dB | lin |
| On5 | 0 to 1 | 1 | - | int |
| Pitch5 | -24 to 24 | 24 | st | int |
| Fine5 | -100 to 100 | 0 | ct | lin |
| Gain5 | -60 to 12 | -6 | dB | lin |
| Width | 0 to 200 | 100 | % | lin |
| Gain | -60 to 24 | 0 | dB | lin |
| Mix | 0 to 100 | 50 | % | lin |
| Note mapping | MIDI 0–72; A4=69=440 Hz; explicit tuning overrides five base frequencies | Note 48 | Hz | 12 equal semitones/octave plus individual fine cents |
| Explicit tuning | 2–20000 Hz per voice, validated as one array; actual frequency bounded by 0.2*sample rate | disabled | Hz | absolute frequencies, then individual fine tuning |
| Decay law | T=Decay*sqrt(440/f), or T=Decay with Const | pitch-dependent | s | feedback=0.001^(period/(sampleRate*T)); small-signal RT60 |
| Mode A/B | positive full-period / negative half-period | A | samples | Color phase delay subtracted; delay minimum 2 samples |
| Color law | one-pole 200–20000 Hz, bypass at 100%; cutoff <=0.45*sample rate | 75 | Hz | exponential cutoff; higher harmonics decay faster |
| Input filter | RBJ two-pole, Q=1/sqrt(2), cutoff <=0.45*sample rate | bypass | ratio | low/band/high/notch |
| Band gain floor / mix | -60 dB is exact mute; Width affects II–V only | all voices on | dB/% | linear wet/dry, side gain Width/100 |
| Internal bounds | prepare supports 8–384 kHz; buffers cover 2 Hz; tails below 1e-30 cleared | 48 kHz fallback | Hz | fixed preallocation |
| Surge feedback saturation | clamp ±1.5, x-(4/27)x^3 | always | amplitude | copied softclip polynomial; RT60 shortens for large signals |

Validation: MSVC /WX and 64-bit Clang frontend conversion checks pass. 51 core checks and 47 real-Pd checks pass. The rendered impulse peak measures 440.0000 Hz (0 cents), with one-second small-signal decay -60.00004 dB. Additional renders prove pitch-dependent and constant-time decay at 240/480 Hz within 0.2 dB, fine/relative frequency laws, routing, filter modes, Color, alternate tuning, block equality 32–4096, supported-rate extremes and zero allocations. The top-level Pd test checks the far bin first (below 1e-7), then a nonzero/non-bypass peak (0.000080349 for a 0.0001 input), routes all 30 controls, verifies zero allocations and a clean console, and writes resonators-pd-render.wav. Forcing the comb output to zero caused 20 core failures and the real-Pd sound assertion to fail; restored code passed both suites again.
