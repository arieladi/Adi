# Fixed Wide de-esser component

`adi_mix_deesser` is a framework-free, stereo, zero-latency processing component.
It is not registered as a native device and does not enable a Mix View function.
It uses the common precise matched-section solver and Direct Form I runtime.

The prepared graph includes fourth-order high-pass and low-pass key filters,
their coupled narrow-band compensation section, an inclusive 5 ms peak hold,
logarithmic reduction, a 50 Hz smoothing section and a range-dependent envelope.
Single Vocal adds two filtered classifier branches, inclusive 3 ms holds and two
Schmitt states per channel. Filter and envelope history is double; each signal
section publishes float. Double input/output deliberately uses this same float
engine. Ordinary finite double samples within float's range round on entry.

Settings are fixed until fresh preparation. Both Allround and Single Vocal
support internal/external stereo key, normal output, filtered-key audition and
removed-signal audition. Input gain/pan precedes programme and internal key;
external key bypasses it. Output gain/pan applies to normal output only. The
link control is normalized: 0 is independent, 0.5 is 100% linked and 1 is 200%.
Allround also supports selective Mid/Side linking above 100%. Single Vocal above
100% is explicitly unsupported in this tier.

Other units are physical: threshold/range in dB, cutoffs in Hz, linear gain and
bipolar balance pan. The present admission is threshold `[-48,0)` dB, positive
range up to 24 dB, approximately 2..20 kHz cutoffs, finite linear gains 0..64 and
pan -1..1. Invalid arguments are distinguished from unsupported capabilities.
The normalized threshold helper refuses the unclosed lower piecewise region.
Conversion helpers are stateless; controller deadbands and host setter history
are not part of these helpers. Singular/unsupported coefficient designs fail
preparation without replacing the previous stream.

Accepted sample rates are 44.1..96 kHz; coefficients use the float-rounded rate.
Generated checks exercise 44.1, 48 and 96 kHz, plus equal-rounded fractional
rates. This does not establish reference equality at every accepted rate or
cutoff. Split Band, oversampling, lookahead, bypass transitions, moving controls,
mono layout and changed-state restore are not implemented or silently ignored.

`prepare` starts a completely fresh stream, including cleared classifier states.
`reset` and same-rate `reprepare` clear filters, holds and envelopes but retain
the two classifier Schmitt states per channel. Reprepare may change block
capacity; a different rate/layout needs fresh prepare. Identical settings restore
retains all history. Every failed preparation preserves the complete old stream.

One serialized audio owner supplies each physical block followed by contiguous
segments. Main pointers address the physical block start, not the segment start.
All settings, context, layout and pointer-span checks precede writes or history
advancement. Refused segments can be corrected and retried at the same offset.
Two complete main planes and either zero or two complete key planes are required.
An absent external key is temporary silence on the same route; it still runs the
active graph and preserves tails. Exact and crossed key/main aliases are safe
because both key samples are read before either main output is written. Partial
overlaps are refused. Source identity and channel-layout migration are host
lifecycle responsibilities, not inferred from pointer values.

Preparation owns allocations and coefficient construction. Callbacks allocate
and free nothing, create no worker/timer and use no SDK or lock. The fixed plan
is calculated once and copied to independent channel histories; no detector or
history is shared merely because current samples happen to match. The current
sample loop is scalar, with the shared precise section boundary. This is no CPU
parity or whole-product completion claim.

The generated test target covers queue expiry/reset, control conversion,
physical-block and segment invariance, float/double I/O, key aliasing, explicit
silence, monitor routing, failed allocation continuation, lifecycle/refusal and
callback allocation/free counts. It contains no external recordings or private
reference data. Standalone build: configure this directory with CMake, build
`adi_mix_deesser_tests`, then run CTest.
