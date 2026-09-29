# Chorus-Ensemble - checklist before code

Live 12 section 28.8, printed pp.551-554, read first.

- Chorus: one/two taps, Auto or fixed delay; Ensemble: three evenly spaced phases; Vibrato: pure modulated delay, sine-to-triangle Shape, stereo Offset, with feedback and mix bypassed.
- High-pass 20..2000 Hz keeps low frequencies out of modulation. Width scales wet side: 0 mono, 100 unity, 200 double.
- Rate/Amount, signed Feedback via Invert, Output, Warmth and Dry/Wet.
- Measure fixed delays, modulation sidebands and vibrato pitch excursion, ensemble phase spacing, bass preservation, feedback inversion, width and gain. Test block sizes 32..4096, zero allocations, real-Pd audio/floor/console; WAV files only.

Source read: Surge 58914e59c608ed4384ba6002e44c3465c58b2e71, ChorusEffectImpl.h and BBDEnsembleEffect.cpp. Adapt the modulated circular read/feedback topology and ensemble t1/t2/t3 phase relationship with retained GPL notice. Use a scalar cubic digital delay instead of Surge's sinc/BBD circuit, and Live's tap count and controls rather than Surge's interface. ADI numeric mappings are provisional, unverified against Live.
