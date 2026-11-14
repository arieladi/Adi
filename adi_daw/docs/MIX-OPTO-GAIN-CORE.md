# Fixed Opto gain-envelope component

`adi_mix_opto_gain` is an unregistered framework-neutral key-to-linear-gain
component. It is not a stereo audio processor, native device, route provider,
controller or complete compressor product. A caller selects the finite key lane
and owns linking, EQ, programme multiplication, channels and source lifetime.

Ratio4, knee0 and range60dB are fixed. Threshold and typed normalized Attack and
Release values are prepared controls. A normalized value is not a displayed
millisecond time: the time law couples Attack and Release. The two double
envelope histories, attack table, nonlinear release branch and float detector/
gain boundaries are preserved. Controls do not change while running.

Preparation accepts44.1..96kHz and1..4096 maximum samples per span. The internal
rate rounds to float. Selected verification rates do not establish every rate
and control combination. Nearest rounding is required during setup and must
remain stable while processing; the caller retains its FTZ/DAZ policy.

The immutable exponential table initializes only during stopped nearest-rounding
preparation. Each instance retains its address and owns one scalar state
allocation. Callbacks never reach the table's static initialization guard.
Fixed coefficients are prepared; the varying attack coefficient cache uses the
exact float sample count. Rendering stays in a precise implementation unit.
There is no per-sample heap, virtual dispatch, worker, lock or library pow/log.
The actual varying coefficient exponential remains when its cached key changes.
These structural properties are not a measured CPU percentage.

Both processing entries check equal lengths, the prepared maximum, alignment,
representable byte ranges and partial overlap before processing. Valid complete
storage remains the caller's precondition. Exact key/gain alias is supported.
`processChecked` rejects a nonfinite sample anywhere in the requested span before
changing any gain, history or cache, so corrected input may retry.
`processTrustedFinite` omits only that scan: its producer guarantees finite
samples. Empty matching spans are a no-op. Consecutive spans retain continuous
state independent of partitions; no physical callback context is inferred.

Prepare stages a replacement, preserving the entire previous stream on failure.
Stopped reset clears histories/cache while retaining controls and the shared
table. This is an authored fresh-state lifecycle contract, not a host preset or
native reset/restore format. One serialized owner is required.

This fixed seam does not admit other ratios, knees, ranges, Auto controls, moving
parameters, quality, Character, Hold/lookahead, audible monitors or style changes.
Native sampled stereo comparisons do not imply every caller's channel/routing
composition is equivalent. Generated safety/lifecycle checks and private sound
comparisons remain distinct. Reference observations and fixtures stay outside
public sources and tests.
