# Saturator - checklist before code

Read Live 12 section 28.34, printed pp.631-634; Surge DistortionEffect.cpp and SST waveshapers Saturators.h/Trigonometrics.h.

- Eight named curves, including Digital hard ceiling, Analog linear low-level region, Bass threshold -50..0 dB and Sinoid folding.
- Drive, post clip Off/Soft/Hard, Output and Dry/Wet.
- Color: pre-emphasis and inverse post-emphasis, low amount plus high amount/frequency/width.
- Custom Waveshaper drive/curve/depth/linear/damp/period; Hi-Quality reduces aliasing; Pre-DC removes input offset.
- Measure harmonic generation, linear regions, clipping ceilings, fold reversal, color small-signal cancellation, HQ alias reduction/latency, block identity and allocation-free processing; real-Pd floor then signal, clean console, float WAV only.

Adapt Surge/SST's rational saturator and hard-clip kernels and Distortion's pre-shaper/post-filter architecture, preserving GPL notices. The other Live-named curves are explicitly ADI approximations, not Live proprietary formula claims. HQ uses a scalar 4x FIR path, preallocated, with reported latency and aligned dry signal. Numeric assumptions go in one table.
