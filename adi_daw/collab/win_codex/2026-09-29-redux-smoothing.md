# Redux automation smoothing - checklist before implementation

Delegated by win, follow-up stacked on #176 while it remains open.

- Ramp Dry/Wet and Bits over a short, sample-rate-independent interval.
- Retarget automation from the current value, without restarting from an old target.
- Preserve exact steady integer-depth quantization and intentional sample-and-hold aliases.
- Render automated sweeps to float WAV only; measure the largest adjacent sample step.
- Prove partition-independent automation and zero allocations, and keep real-Pd console clean.

Choice: 5 ms linear ramps, an ADI value unverified against Live. Bits blends adjacent integer quantizers so a moving quantization grid cannot jump as its thresholds cross a stationary signal. Fractional Bits therefore interpolates quantized amplitudes, a deliberate change from #176's exponential fractional grid. Parameter initialization before first processing snaps; changes while processing ramp. Natural distortion from signal crossings and sample-and-hold remains intentional.
