# Utility - manual checklist before code

Live 12 section 28.40, printed pages 655-656, read from the local manual.

- Independent input polarity switches, followed by Left/Right/Swap/Stereo routing.
- Width scales side content, with mono at zero; Mid/Side mode selects toward all-mid or all-side. Left/Right routing bypasses these controls.
- Mono sums channels. Bass Mono removes stereo bass below a 50-500 Hz crossover; audition isolates that bass.
- Gain reaches true silence and +35 dB; Balance attenuates the opposite channel; Mute silences at this device's position; DC rejects steady offset.
- Test each behavior, complementary bass reconstruction, sample-identical blocks 32-4096, zero allocations, real LibPdEngine far-bin-first tone and clean console; float WAV only.
- Record all provisional numeric values together; no claim of Live filter topology or parameter curve parity.

Implementation choice: native MIT DSP. A complementary two-pole low-pass split provides Bass Mono without altering the unaffected mid channel. Mid/Side balance keeps the retained component at unity, rather than adding gain. Gain's -120 dB endpoint encodes negative infinity in finite Pd metadata. Five-millisecond Gain/Balance ramps are ADI defaults.
