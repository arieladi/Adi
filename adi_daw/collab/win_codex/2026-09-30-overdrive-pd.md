# Overdrive — checklist before DSP (mission 8)

Live 12 §28.27, printed p.608, read from the local manual. Our own wording:
- Band-pass frequency and bandwidth shape the signal before distortion.
- Drive zero is still capable of distortion; increasing Drive adds it.
- Tone controls high-frequency energy after the nonlinear stage.
- Low Dynamics compresses more and raises level; high Dynamics preserves more input dynamics.
- Dry/Wet blends original and processed signals; zero is exact dry.

ADR-0187 selects BYOD waveshapers. Adapt the scalar OJD transfer from BYOD/src/processors/drive/waveshaper/SurgeWaveshapers.cpp at 1cf22b6ac802b9dc33cfc9f8dd6af5b3c3e40bc9, retaining its Surge GPL-3 header. Constants checked directly in that source, not taken from the drone. Native band-pass, post tone and RMS envelope/VCA surround it; this is an ADI interpretation of Live's behaviour, not a claim of identical proprietary transfer functions.

Every range/default/unit/curve below is **ADI, unverified against Live**, except the manual explicitly discusses 0% Drive and 100% wet. One table for director measurement:

| Live control | ADI range | Default | Unit | Curve |
|---|---|---|---|---|
| Filter Frequency | 50–12000 | 1000 | Hz | logarithmic |
| Filter Bandwidth | 0.1–6 | 3 | octaves | logarithmic |
| Drive | 0–100 | 50 | percent | linear control; exponential internal gain |
| Tone | 0–100 | 50 | percent | linear control; logarithmic cutoff |
| Dynamics | 0–100 | 50 | percent | linear RMS gain blend |
| Dry/Wet | 0–100 | 100 | percent | linear |

Tests planned: pre-band selectivity, zero-drive harmonics, drive increase, tone roll-off, Dynamics input/output ratios, exact dry; block sizes 32–4096 identical; core/Pd allocation audit; LibPdEngine far-bin floor before audible fundamental/harmonics, clean console; render only to WAV.

Implemented six smoothed controls, native constant-peak band-pass, BYOD OJD scalar knees (original copyright header retained), post low-pass/DC removal, RMS-preserving gain blend. Native MSVC /WX: 17 core and 17 LibPdEngine checks pass. Measured loud/quiet fundamental ratio is 1.186670 at low Dynamics versus 9.706549 at high Dynamics for a 10:1 input ratio. Real Pd fundamental 1.268425870, far bin below 1e-12; clean console and zero process allocations. Float WAV artifact stays in the build folder.
