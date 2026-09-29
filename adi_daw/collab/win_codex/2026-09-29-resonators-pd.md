# Resonators — manual checklist before code

Read Live 12 chapter 28.31, printed pages 617–618, from the local reference. Behaviour in our own words:

- Five parallel tuned resonators, each independently enabled and gained; switching I off leaves II–V working.
- A shared input filter selects low-pass, band-pass, high-pass or notch and cutoff.
- Two resonation characters, pitch-dependent decay with a constant-time option, and Color for brightness.
- I processes both input channels; II/IV take left and III/V right. Width acts on the wet stereo voices; output gain does not alter the dry endpoint.
- Root note and fine tuning, with independent relative pitch/fine offsets for II–V; support explicitly supplied alternate tuning frequencies without ambient settings in the DSP.
- Measure pitch in cents, relative/fine tuning, decay time and Const, routing, mode/color/filter behaviour, exact 32–4096 block equality and zero process allocations. Real-Pd proof checks a floor bin before a peak, clean console and a file-only render.

Source plan: Surge XT CombulatorEffect's parallel tuned combs, with the scalar equivalent of its sst-filters COMBquad feedback/read/write recurrence and sst-basic-blocks softclip polynomial. Surge's separate ResonatorEffect is three band-pass filters, not the string/comb structure needed here (ADR-0192 explains this distinction). Read the actual pinned source before choosing coefficients. Keep copied GPL headers and identify differences and unverified values in the PR. Stack on #189.
