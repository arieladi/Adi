# EQ Eight — checklist before DSP (mission 8)

Working title, director names the device. Live 12 §28.15, printed pp.577–579:
- Eight independently enabled bands, cascaded; stacking identical bands strengthens filtering.
- Eight responses: 48/12 dB low cut, low shelf, peak, notch, high shelf, 12/48 dB high cut.
- Stereo shares a curve; L/R and M/S have independent curves and an edit selector.
- Frequency, gain and Q controls; gain is inapplicable to cuts/notch. Adaptive Q tightens as gain increases.
- Scale changes gains only; global Gain changes output level; Audition isolates a band.
- Optional 2x processing, with latency reported to the host.
- Analyzer/graph gestures belong to the device panel/analyser; this PR supplies compiled DSP and declarative top-level patch, not a second GUI.

Source: Surge ParametricEQ3BandEffect.cpp (58914e59c608ed4384ba6002e44c3465c58b2e71) uses sst-filters BiquadFilter::coeff_peakEQ/coeff_orfanidisEQ, e92d93a92beabde03fa4ab767b285fa21c6608d6. Adapt its scalar coefficients/cascade, retain GPL header. Extend three bands to eight, add cookbook shelves, use four cascaded second-order Butterworth sections for 48 dB cuts. Live behaviour wins on types/channel modes; Surge supplies the peak design, not a proprietary Live-match claim.

All numeric control mappings below are **ADI, unverified against Live**; the manual verifies the eight response choices, channel modes and 2x switch. One measurement table:

| Live control | ADI range | Default | Unit | Curve |
|---|---|---|---|---|
| Per-band Activator | off/on | bands 1–4 on; 5–8 off | boolean | discrete |
| Per-band Filter Type | eight manual responses | Peak | enum | discrete |
| Per-band Freq | 10–22000 | 80,200,500,1000,2000,4000,8000,16000 | Hz | logarithmic |
| Per-band Gain | −15–15 | 0 | dB | linear |
| Per-band Q | 0.1–18 | 0.70710678 | ratio | logarithmic |
| Mode | Stereo/LR/MS | Stereo | enum | discrete |
| Edit | first/second channel | first | enum | discrete |
| Adaptive Q | off/on | on | boolean | discrete |
| Scale | −200–200 | 100 | percent | linear |
| Gain | −24–24 | 0 | dB | linear |
| Audition | off, band 1–8 | off | enum | discrete |
| Oversampling | off/on | off | boolean | discrete |

Tests: centre gains/shelves/notch, 12/48 dB slopes, band summation/disable, L/R and M/S separation, Scale and Adaptive Q, HQ latency, all block sizes 32–4096 identical, allocations zero; LibPdEngine floor before boosted peak, clean console and WAV only.

Implementation supplies stable parameter IDs for both A/B curves; Edit is panel selection state, not a remapping of automation IDs. Peak coefficients retain the Surge Orfanidis design; Q maps to octave bandwidth. Scalar smoothing is per sample, independent of host block size. Oversampling is a 33-tap half-band interpolation/decimation pair, with measured 16-frame latency. Native MSVC /WX tests measure low-cut slopes 12.035/48.168 dB per octave and high-cut 12.102/49.380 in the selected octave (bilinear warping explains the high-frequency difference). The real Pd +6 dB peak is 0.798104934 from a 0.4 sine; far bin below 1e-12, zero allocations, clean console. Panel graph/analyser rendering is explicitly not supplied by this DSP patch.
