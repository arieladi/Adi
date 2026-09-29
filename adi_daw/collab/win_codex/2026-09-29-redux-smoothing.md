# Redux automation smoothing - checklist before implementation

Delegated by win, follow-up stacked on #176 while it remains open.

- Ramp Dry/Wet and Bits over a short, sample-rate-independent interval.
- Retarget automation from the current value, without restarting from an old target.
- Preserve exact steady integer-depth quantization and intentional sample-and-hold aliases.
- Render automated sweeps to float WAV only; measure the largest adjacent sample step.
- Prove partition-independent automation and zero allocations, and keep real-Pd console clean.

Choice: 5 ms linear ramps, an ADI value unverified against Live. Bits blends adjacent integer quantizers so a moving quantization grid cannot jump as its thresholds cross a stationary signal. Fractional Bits therefore interpolates quantized amplitudes, a deliberate change from #176's exponential fractional grid. Parameter initialization before first processing snaps; changes while processing ramp. Natural distortion from signal crossings and sample-and-hold remains intentional.

## Implemented and validated

Dry/Wet and Bits each ramp for 5 ms, once per stereo frame. Repeated identical targets do not restart a ramp; changed targets start from the current interpolated value. Prepare clears ramp state and adopts the retained targets, so initial loading is deterministic. Integer Bits still uses the original mid-rise quantizer, Shape and DC Shift; fractional Bits blends its two neighboring integer outputs after inverse companding. This is an ADI automation choice, unverified against Live; no claim that Redux's deliberate signal quantization is itself click-free.

Windows MSVC /WX: 66 DSP checks and 24 LibPdEngine checks pass; schema and Pd validators pass. Automated events every 137 samples are sample-identical at blocks 32..4096 with zero allocations. The biased-sine sweep's largest adjacent step is 0.001253992. Full-range Bits on constant input measures 0.023958355 at 48 kHz and 0.011979192 at 96 kHz; Dry/Wet is below 0.001. Host-driven real-Pd automation measures 0.021451056, allocates nothing and leaves the console clean. Existing alias/far-bin-first tests still pass.

Fault plant: replacing the 5 ms ramp with one sample produces 7 core failures and 1 real-Pd failure. The core sweep jumps 0.155492157 and real-Pd jumps 0.126803130. Restoring the implementation returns all 90 checks to green. Float WAV artifacts are build/redux-automation-render.wav and build/redux-pd-automation-render.wav; no audio device was opened.

#176 is still open, so this PR is stacked on codex/redux-pd at 96a4685e05486a2fa85f9e8ad310845d2404a5fd. Retarget to main after that merge. README counts and existing PR branches are untouched.
