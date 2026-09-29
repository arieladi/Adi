# Auto Filter — checklist before DSP (mission 8)

Live 12 §28.2, printed pp.525–533. Our wording:
- Ten types: LP, HP, BP, notch, morph, DJ, comb, resampling, notch+LP, vowel. Conventional slopes 12/24; morph also 6/48. Morph changes LP→BP→HP, comb polarity, notch offset, or vowel normalization.
- Clean SVF plus DFM, MS2 and PRD circuit characters; Drive adds nonlinearity and circuits apply only to conventional/morph types.
- Stereo LFO: Phase or Spin, eight waveforms, shape/smoothing, free Hz/time or musical division, synced offset, steps or beat-time sample/hold.
- Envelope amount/polarity, attack/hold/release and beat-time sample/hold.
- Output Clip/Gain/Dry-Wet. Internal/external sidechain blend/gain/listen/filter; mono sidechain default.
- Tests measure spectral type behaviour, envelope and tempo-LFO sidebands, sidechain isolation, all block sizes 32–4096, zero process allocations, real Pd floor-first peak/console and file render.

Sources checked directly: Surge's sst-filters CytomicSVF.h and K35Filter.h at e92d93a92beabde03fa4ab767b285fa21c6608d6, retaining headers. Scalar TPT/Sallen-Key states replace SIMD. PRD uses the same source's VintageLadders::Huov state equations, standard tanh/exp replacing approximations. These are Surge-derived circuit interpretations, not Ableton circuit identity. DFM is an ADI driven feedback SVF. Native comb/resampling/vowel and modulation surround these cores. The host chooses the external sidechain source/tap; patch adc~ 3/4 consumes its already-routed stereo bus.

All following ranges/defaults/units/curves are **ADI, unverified against Live**. Choices and described behaviours are manual-backed, not measured proprietary responses. Stable separate Hz/time and beat controls allow the generic Pd parameter contract to keep units fixed.

| Live control | ADI range/choices | Default | Unit | Curve |
|---|---|---|---|---|
| Filter Type | ten manual types | LP | enum | discrete |
| Slope | 6/12/24/48 (6/48 only morph) | 12 | dB/oct | discrete |
| Freq | 20–22000 | 1000 | Hz | log |
| Res | 0–100 | 29.2893 | percent | linear |
| Morph | 0–100 | 0 | percent | linear |
| DJ Control | −100–100 | 0 | percent | bipolar |
| Pitch | −24–24 | 0 | semitones | linear |
| Formant | a/e/i/o/u, continuous 0–4 | a | index | linear |
| Circuit | SVF/DFM/MS2/PRD | SVF | enum | discrete |
| Drive | 0–100 | 0 | percent | linear; internal exponential gain |
| LFO Amt | 0–100 | 0 | percent | linear; four-octave maximum |
| LFO Rate / Time | 0.01–40 | 1 | Hz / seconds by mode | log |
| LFO Mode | Hz/Time/Beats/Sixteenths | Hz | enum | discrete |
| LFO musical rate | 1/64–64 | 1 | beats or sixteenths | log |
| LFO Wave | Sine/Triangle/Saw/Square/RampUp/RampDown/Wander/SH | Sine | enum | discrete |
| LFO Morph / Smooth | 0–100 | 50 | percent | linear |
| LFO Stereo Mode | Phase/Spin | Phase | enum | discrete |
| LFO Phase | 0–360 | 0 | degrees | linear |
| LFO Spin | 0–100 | 0 | percent | linear |
| LFO Phase Offset | 0–360, sync modes only | 0 | degrees | linear |
| LFO Quantization | off/Steps/SH | off | enum | discrete |
| LFO Steps | 1–32 | 8 | count | integer |
| LFO SH | 1/64–16 | 0.25 | beats | log |
| Envelope | −100–100 | 0 | percent | bipolar; four-octave maximum |
| Envelope Attack | 0.1–1000 | 10 | ms | log |
| Envelope Hold | off/on | off | boolean | discrete |
| Envelope Release | 1–3000 | 100 | ms | log |
| Envelope SH | off/on | off | boolean | discrete |
| Envelope SH rate | 1/64–16 | 0.25 | beats | log |
| Clip | off/on | off | boolean | discrete |
| Output | −24–24 | 0 | dB | linear |
| Dry/Wet | 0–100 | 100 | percent | linear |
| External | off/on | off | boolean | discrete |
| SC Mix | 0–100 | 100 | percent | linear |
| SC Gain | −24–24 | 0 | dB | linear |
| SC Listen | off/on | off | boolean | discrete |
| SC Filter | off/on | off | boolean | discrete |
| SC Type | low shelf/peak/high shelf/LP/BP/HP | BP | enum | discrete |
| SC Filter Frequency | 20–22000 | 1000 | Hz | log |
| SC Filter Q | 0.1–18 | 0.70710678 | ratio | log |
| SC Shelf/Peak Gain | −24–24 | 0 | dB | linear |
| Mono Sidechain | off/on | on | boolean | discrete |

Vowel formant centres are explicitly ADI approximations, not Live measurements: a=(800,1150,2900), e=(400,1700,2600), i=(350,2000,2800), o=(450,800,2830), u=(325,700,2530) Hz. The analyzer/curve display and source/tap chooser belong to the host panel, not another renderer in this patch.

Implementation choices: native one-pole endpoints supply Morph's 6 dB slope for all circuits; the two/four-pole Surge cores supply 12/24, and cascading supplies 48. MS2/PRD retain the source's 0.3 × sample-rate upper frequency clamp. Vowel Morph interpolates the three formant weights toward equal weight. Envelope Hold preserves a trigger for the configured attack duration. The LFO follows transport BPM, with deterministic phase measured from DSP preparation; it does not seek phase to the host's absolute bar position. GUI spectrum drawing/source selection remain host-panel work.

Validation: MSVC /WX, 163 DSP checks and 54 LibPdEngine checks passed. All ten types are bit-identical at block sizes 32–4096; waveform/quantization timing also survives block changes. Circuit slope progression, Spin→Phase/zero restoration, negative envelope and Hold are tested. The 120 BPM one-beat LFO produces 998/1002 Hz sidebands around a 1 kHz input; changing to 60 BPM moves them to 999/1001. Resampling 5 kHz at 8 kHz folds to 3 kHz. Real Pd's fundamental is 0.187911732 with its far bin below 1e-7 checked first, zero process allocations, clean console, all 42 parameters routed, and channels 3/4 proven to receive the external sidechain. Audio is written only to auto_filter_pd_render.wav by the test.
