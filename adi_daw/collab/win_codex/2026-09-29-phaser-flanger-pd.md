# Phaser-Flanger - checklist before code

Read Live 12 section 28.29, pp.612-615 and Surge/SST Phaser.h, Flanger.h, BiquadFilter::coeff_APF.

- Phaser allpass notches with Center/Spread/Blend; Flanger unipolar delay motion; Doubler bipolar longer delays.
- Main LFO: ten waveforms, Hz or transport tempo sync, Phase/Spin, Duty. Second triangle LFO with independent sync and Mix.
- Envelope amount/attack/release; Amount; signed feedback; Safe Bass 5..3000 Hz; Output/Warmth/DryWet.
- Measured phaser notch and unity allpass magnitude, flanger comb notch and delay, bipolar doubler delay, synced modulation equivalence, blocks 32..4096, no allocation, real-Pd floor before tone and clean console.

Adapt Surge's serial second-order allpass/feedback recurrence and Flanger's interpolated comb read/feedback. Retain GPL notices. Use Live's modes and modulation routing; omit Surge's extra comb voices/arpeggiator. Scalar per-sample modulation avoids a fixed upstream host-block assumption. Provisional values will be in one table.

## Single provisional values table

All ADI ranges/defaults/units/curves below are **unverified against Live**, except Safe Bass endpoints, Phase/Duty/LFO Mix semantics stated in the manual. Beats are quarter-note counts.

| Control | Range | Default | Unit | Curve / choices |
|---|---|---|---|---|
| Mode | 0 to 2 | 0 | - | menu Phaser Flanger Doubler |
| Notches | 1 to 8 | 4 | - | int  |
| Center | 20 to 18000 | 1000 | Hz | log  |
| Spread | 0 to 100 | 50 | % | lin  |
| Blend | 0 to 1 | 0 | - | lin  |
| Time | 0.1 to 100 | 3 | ms | log  |
| Sync | 0 to 1 | 0 | - | int  |
| Rate | 0.01 to 20 | 0.5 | Hz | log  |
| Beats | 0.0625 to 16 | 4 | qn | log  |
| Wave | 0 to 9 | 1 | - | menu Sine Triangle Analog Triangle8 Triangle16 SawUp SawDown Rectangle Random SampleHold |
| StereoMode | 0 to 1 | 0 | - | menu Phase Spin |
| Phase | 0 to 360 | 180 | deg | lin  |
| Spin | -50 to 50 | 0 | % | lin  |
| Duty | -100 to 100 | 0 | % | lin  |
| Lfo2Mix | 0 to 100 | 0 | % | lin  |
| Sync2 | 0 to 1 | 0 | - | int  |
| Rate2 | 0.01 to 20 | 0.2 | Hz | log  |
| Beats2 | 0.0625 to 16 | 8 | qn | log  |
| Amount | 0 to 100 | 50 | % | lin  |
| Feedback | 0 to 95 | 0 | % | lin  |
| Invert | 0 to 1 | 0 | - | int  |
| EnvOn | 0 to 1 | 0 | - | int  |
| EnvAmount | -100 to 100 | 0 | % | lin  |
| Attack | 0.1 to 1000 | 10 | ms | log  |
| Release | 1 to 5000 | 200 | ms | log  |
| SafeBassOn | 0 to 1 | 0 | - | int  |
| SafeBass | 5 to 3000 | 120 | Hz | log  |
| Output | -60 to 12 | 0 | dB | lin  |
| Warmth | 0 to 100 | 0 | % | lin  |
| Mix | 0 to 100 | 50 | % | lin  |
| Center modulation | -2..2 | Amount-dependent | oct | linear LFO scaling; unverified |
| Spread Q | 0.5..4.5 | 2.5 | Q | linear; modulation limited 0.2..8; unverified |
| Flanger depth | 0..90 | Amount-dependent | % of Time | unipolar; unverified |
| Doubler depth | -80..80 | Amount-dependent | % of Time | bipolar; unverified |
| Warmth cutoff | 18000..3000 | 18000 | Hz | linear; unverified |

## Validation and choices

MSVC /WX: 29 DSP and 40 real-Pd checks pass. One allpass gives unity wet magnitude and a dry/wet center notch at 1 kHz. A 2 ms Flanger produces the 250 Hz comb notch; Doubler gives exactly 960 samples at 20 ms/48 kHz; inverted 50% feedback gives a -0.5 repeat. Both LFO tempo modes equal their Hz equivalents at 90 BPM. Safe Bass, envelope movement, all ten waveforms at duty endpoint, 32..4096 partition identity and no allocations pass. LibPdEngine makes the expected -6 dB delayed tone (0.200474897), after a quiet far bin, with clean console and routed parameters; float WAV only.

The allpass coefficients and feedback recurrence follow Surge/SST; the modulation frequency/Q mapping and single Flanger comb are ADI adaptations for Live controls. Warmth uses a tanh and low-pass rather than Surge tone voicing. Sync follows BPM, without claiming transport-reset LFO phase. Random modulation is deterministic after prepare. No visual UI or exact Live sonic parity claimed; top-level Pd patch supplied.
