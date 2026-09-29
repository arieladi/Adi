# Chorus-Ensemble - checklist before code

Live 12 section 28.8, printed pp.551-554, read first.

- Chorus: one/two taps, Auto or fixed delay; Ensemble: three evenly spaced phases; Vibrato: pure modulated delay, sine-to-triangle Shape, stereo Offset, with feedback and mix bypassed.
- High-pass 20..2000 Hz keeps low frequencies out of modulation. Width scales wet side: 0 mono, 100 unity, 200 double.
- Rate/Amount, signed Feedback via Invert, Output, Warmth and Dry/Wet.
- Measure fixed delays, modulation sidebands and vibrato pitch excursion, ensemble phase spacing, bass preservation, feedback inversion, width and gain. Test block sizes 32..4096, zero allocations, real-Pd audio/floor/console; WAV files only.

Source read: Surge 58914e59c608ed4384ba6002e44c3465c58b2e71, ChorusEffectImpl.h and BBDEnsembleEffect.cpp. Adapt the modulated circular read/feedback topology and ensemble t1/t2/t3 phase relationship with retained GPL notice. Use a scalar cubic digital delay instead of Surge's sinc/BBD circuit, and Live's tap count and controls rather than Surge's interface. ADI numeric mappings are provisional, unverified against Live.

## Single provisional values table

All ADI ranges/defaults/units/curves are **unverified against Live** unless the manual explicitly gives the endpoint (high-pass 20..2000 Hz, Width 0/100/200%, Offset 180 degrees, Shape 0/100%). UI chooser presentation is pending the host device view.

| Live control / patch name | Range | Default | Unit | Curve / choices |
|---|---|---|---|---|
| Mode | 0 to 2 | 0 | - | menu Chorus Ensemble Vibrato |
| HighPassOn | 0 to 1 | 0 | - | int  |
| HighPass | 20 to 2000 | 100 | Hz | log  |
| Width | 0 to 200 | 100 | % | lin  |
| DelayAuto | 0 to 1 | 1 | - | int  |
| Time | 1 to 40 | 10 | ms | log  |
| Taps | 1 to 2 | 2 | - | int  |
| Offset | 0 to 180 | 0 | deg | lin  |
| Shape | 0 to 100 | 0 | % | lin  |
| Rate | 0.01 to 20 | 1 | Hz | log  |
| Amount | 0 to 100 | 50 | % | lin  |
| Feedback | 0 to 95 | 0 | % | lin  |
| Invert | 0 to 1 | 0 | - | int  |
| Output | -60 to 12 | 0 | dB | lin  |
| Warmth | 0 to 100 | 0 | % | lin  |
| Mix | 0 to 100 | 50 | % | lin  |
| Auto delay | 2..20 | Amount-dependent | ms | 2+18*Amount; unverified |
| Chorus / Vibrato depth | 0..4 / 0..8 | Amount-dependent | ms | capped at 90% of delay; unverified |
| Stereo phase (Chorus/Ensemble) | 90 | 90 | degrees | fixed; unverified |
| Warmth cutoff | 18000..3000 | 18000 | Hz | linear with Warmth; unverified |

## Validation and differences

MSVC /WX: 24 DSP checks and 26 LibPdEngine checks pass. Fixed 10 ms impulse, signed feedback, two-tap odd-sideband cancellation and single-tap sidebands are measured. Vibrato at 2 Hz, 1.6 ms depth measures 979.894309..1020.105512 Hz on a 1 kHz input, matching the predicted +/-20.1 Hz. Ensemble differs from Chorus; high-pass protects bass; Warmth adds harmonics. Blocks 32..4096 agree exactly; zero allocations. Real-Pd stable delayed tone at -6 dB is 0.200474897 after a floor-bin check, with every parameter routed and clean console. Float WAV only.

Surge supplies the tap/feedback topology and evenly spaced ensemble phase pattern, adapted to separate stereo rings. Cubic interpolation, Live-style one/two/three taps, the input bass bypass and tanh/one-pole Warmth are ADI choices; Surge sinc interpolation, BBD circuitry and its saturation/filter voicing are not reproduced. No claim of sonic identity to Live.
