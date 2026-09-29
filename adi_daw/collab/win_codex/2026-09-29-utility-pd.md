# Utility - manual checklist before code

Live 12 section 28.40, printed pages 655-656, read from the local manual.

- Independent input polarity switches, followed by Left/Right/Swap/Stereo routing.
- Width scales side content, with mono at zero; Mid/Side mode selects toward all-mid or all-side. Left/Right routing bypasses these controls.
- Mono sums channels. Bass Mono removes stereo bass below a 50-500 Hz crossover; audition isolates that bass.
- Gain reaches true silence and +35 dB; Balance attenuates the opposite channel; Mute silences at this device's position; DC rejects steady offset.
- Test each behavior, complementary bass reconstruction, sample-identical blocks 32-4096, zero allocations, real LibPdEngine far-bin-first tone and clean console; float WAV only.
- Record all provisional numeric values together; no claim of Live filter topology or parameter curve parity.

Implementation choice: native MIT DSP. A complementary two-pole low-pass split provides Bass Mono without altering the unaffected mid channel. Mid/Side balance keeps the retained component at unity, rather than adding gain. Gain's -120 dB endpoint encodes negative infinity in finite Pd metadata. Five-millisecond Gain/Balance ramps are ADI defaults.

## Single numeric parity table

ADI ranges/defaults/units/curves below are unverified against Live except Bass Frequency endpoints and Gain maximum, which are printed in the manual. MidSide uses negative for M and positive for S. Gain -120 encodes true silence.

| Control | Range | Default | Unit | Curve / choices |
|---|---|---|---|---|
| PhaseL | 0 to 1 | 0 | - | int  |
| PhaseR | 0 to 1 | 0 | - | int  |
| Channel | 0 to 3 | 3 | - | menu Left Right Swap Stereo |
| Width | 0 to 400 | 100 | % | lin  |
| MidSideMode | 0 to 1 | 0 | - | int  |
| MidSide | -100 to 100 | 0 | % | lin  |
| Mono | 0 to 1 | 0 | - | int  |
| BassMono | 0 to 1 | 0 | - | int  |
| BassFreq | 50 to 500 | 120 | Hz | log  |
| BassAudition | 0 to 1 | 0 | - | int  |
| Gain | -120 to 35 | 0 | dB | lin  |
| Balance | -100 to 100 | 0 | % | lin  |
| Mute | 0 to 1 | 0 | - | int  |
| DC | 0 to 1 | 0 | - | int  |
| Gain/Balance smoothing | 5 | 5 | ms | exponential time constant; unverified |
| DC cutoff | 5 | 5 | Hz | one-pole; unverified |
| Bass split | 2 | 2 | poles | cascaded low-pass and complementary residual; unverified |

## Validation

MSVC /WX: 42 native DSP checks and 24 real-Pd checks pass. Both 48/96 kHz cover DC removal, bass/treble separation and complementary mid reconstruction. All channel modes, polarity, Width and M/S extrema, gain, balance and mute are tested. Blocks 32..4096 agree exactly; no process allocations. Real-Pd -6 dB gain produces a 0.200474897 tone from 0.4 input, after a far bin at the floor; all controls route and console is clean. Float WAV only, build/utility-pd-render.wav. Gain smoothing is exponential with exact silence once within 1e-12, not a claim of Live parameter-ramp parity. No README count edited.
