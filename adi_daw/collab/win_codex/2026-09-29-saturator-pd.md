# Saturator - checklist before code

Read Live 12 section 28.34, printed pp.631-634; Surge DistortionEffect.cpp and SST waveshapers Saturators.h/Trigonometrics.h.

- Eight named curves, including Digital hard ceiling, Analog linear low-level region, Bass threshold -50..0 dB and Sinoid folding.
- Drive, post clip Off/Soft/Hard, Output and Dry/Wet.
- Color: pre-emphasis and inverse post-emphasis, low amount plus high amount/frequency/width.
- Custom Waveshaper drive/curve/depth/linear/damp/period; Hi-Quality reduces aliasing; Pre-DC removes input offset.
- Measure harmonic generation, linear regions, clipping ceilings, fold reversal, color small-signal cancellation, HQ alias reduction/latency, block identity and allocation-free processing; real-Pd floor then signal, clean console, float WAV only.

Adapt Surge/SST's rational saturator and hard-clip kernels and Distortion's pre-shaper/post-filter architecture, preserving GPL notices. The other Live-named curves are explicitly ADI approximations, not Live proprietary formula claims. HQ uses a scalar 4x FIR path, preallocated, with reported latency and aligned dry signal. Numeric assumptions go in one table.

## Single provisional values table

All ADI ranges/defaults/units/curves below are **unverified against Live**, except the manual Bass Threshold -50..0 dB endpoints and percentage-control semantics.

| Control | Range | Default | Unit | Curve / choices |
|---|---|---|---|---|
| CurveType | 0 to 7 | 0 | - | menu AnalogClip SoftSine BassShaper MediumCurve HardCurve SinoidFold DigitalClip Waveshaper |
| Drive | -36 to 36 | 0 | dB | lin  |
| BassThreshold | -50 to 0 | -12 | dB | lin  |
| PostClip | 0 to 2 | 0 | - | menu Off Soft Hard |
| ColorOn | 0 to 1 | 0 | - | int  |
| AmtLo | -24 to 24 | 0 | dB | lin  |
| AmtHi | -24 to 24 | 0 | dB | lin  |
| Frequency | 20 to 18000 | 1000 | Hz | log  |
| Width | 0.1 to 4 | 1 | oct | log  |
| Output | -60 to 0 | 0 | dB | lin  |
| Mix | 0 to 100 | 100 | % | lin  |
| ShaperDrive | 0 to 100 | 0 | % | lin  |
| Curve | -100 to 100 | 0 | % | lin  |
| Depth | 0 to 100 | 0 | % | lin  |
| Linear | 0 to 100 | 50 | % | lin  |
| Damp | 0 to 100 | 0 | % | lin  |
| Period | 0.1 to 20 | 1 | - | log  |
| HiQuality | 0 to 1 | 0 | - | int  |
| PreDC | 0 to 1 | 0 | - | int  |
| Analog knee | 0.5 | 0.5 | amplitude | linear then exponential toward unity; unverified |
| Color low corner | 200 | 200 | Hz | first-order shelf; unverified |
| Pre-DC corner | 5 | 5 | Hz | one-pole; unverified |
| HQ | 4x / 65 taps | off | factor / taps | Hann-windowed sinc, 0.1125 cycles per oversampled sample; unverified |

## Validation and parity bounds

MSVC /WX: 31 DSP and 30 real-Pd checks pass. Tests cover Analog/Bass linear regions, hard ceiling and harmonics, fold reversal, all custom shape controls, inverse Color cancellation in the linear region, post-clip ceiling after Color, and Pre-DC. HQ reduces the 15 kHz alias from an 11 kHz tone from 0.401454058 to 0.000450308 (>58 dB), while retaining the wanted tone. Its dry path is aligned at 16 samples and the actual Pd latency outlet reports that value. HQ blocks 32..4096 agree and allocate nothing. The real clipped tone fundamental is 0.593321328, matching the analytic clipped-sine response after the far bin is verified at floor. Console clean, all controls routed, float WAV only.

Surge CLIP and rational TANH are adapted directly; Analog/Bass exponential knees, other Live-named curve formulas, custom shaping laws, Color shelf/peak filters and the 4x FIR design are ADI implementations. Color post filters invert the pre filters, in reverse order. Post Clip limits the blended output before Output gain, including Color amplification. HiQuality changes latency; the external reports it whenever parameters change. No claim of exact Live sonic parity, UI curve rendering, or parameter smoothing.
