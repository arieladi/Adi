# Phaser-Flanger - checklist before code

Read Live 12 section 28.29, pp.612-615 and Surge/SST Phaser.h, Flanger.h, BiquadFilter::coeff_APF.

- Phaser allpass notches with Center/Spread/Blend; Flanger unipolar delay motion; Doubler bipolar longer delays.
- Main LFO: ten waveforms, Hz or transport tempo sync, Phase/Spin, Duty. Second triangle LFO with independent sync and Mix.
- Envelope amount/attack/release; Amount; signed feedback; Safe Bass 5..3000 Hz; Output/Warmth/DryWet.
- Measured phaser notch and unity allpass magnitude, flanger comb notch and delay, bipolar doubler delay, synced modulation equivalence, blocks 32..4096, no allocation, real-Pd floor before tone and clean console.

Adapt Surge's serial second-order allpass/feedback recurrence and Flanger's interpolated comb read/feedback. Retain GPL notices. Use Live's modes and modulation routing; omit Surge's extra comb voices/arpeggiator. Scalar per-sample modulation avoids a fixed upstream host-block assumption. Provisional values will be in one table.
