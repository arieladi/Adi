# Key EQ component

`adi_mix_key_eq` is an unregistered, framework-neutral, base-rate stereo key
processor. It does not enable a native compressor/de-esser/gate function or change
the existing plug-in engines. Its output is the filtered key, not limited or
compressed programme audio.

Preparation builds ordered root batches and analog sections from physical band
frequency, gain, Q, shape and slope. It applies the explicit float boundaries,
first-section Q floors, section-specific extrema/prominence, anchor selection
and response-matching policy before using the common precise DFI stages. The
ordinary Bell, shelves, cuts, Notch, Band Pass and Tilt paths are supported.
Low Cut is high-pass; High Cut is low-pass.
All Pass uses its separate mirrored-root and float-clamped bilinear conversion;
it retains the same ordered precise DFI state and float section boundaries.
Flat Tilt and the separate brickwall-cut path are explicitly refused.
Non-cut shapes retain their bounded slope-clamping law.

Each prepared bank has up to six ordered bands and fixed section capacity. The
stereo path forms Side as `(R-L)*0.5`, Mid as `Side+L`, then applies each band's
Stereo/Mid/Side placement. Inverse conversion and optional mono audition retain
their float boundaries. Near-zero gain-bearing bands use the full-cascade dry
blend and the signed small-gain prototype floor. A reusable dry buffer is
allocated only when that prepared blend needs it.

The default controls and layout are fixed until stopped prepare. Unsupported configurations
and invalid process calls are refused before writes or history advancement.
Failed preparation retains the complete previous configuration and state.
Reset clears recursive histories; no serialized-state restore or running
enable/topology transition contract is implied. Other layouts, quality conversion,
moving topology and compressor gain remain separate implementation gates.

The caller supplies valid aligned arrays of finite float audio, owns mutable planar key buffers separately from programme audio and
serializes lifecycle calls. In-place M/S conversion and one precise span call per
section/channel/segment avoid extra always-allocated scratch or per-sample
external dispatch. The default graph builds coefficients only in preparation; processing has no
allocation, locks, atomics or worker. Paired-state/SIMD optimization and full graph
CPU comparisons require later numerical and timing gates.
An all-bands-off prepared bank still performs its defined M/S roundtrip; it is
distinct from a facade's true disabled bypass.

Generated tests cover configuration boundaries, bounded section counts, strong
failed-prepare behavior, invalid buffer calls, split blocks, reset/reprepare,
placement, audition, near-zero blending and zero callback allocation/free.
Their authored multiband/routing mechanics are not additional native parity
evidence. Private reference fixtures and tools remain outside public sources.

## Optional moving lane

`Preparation::movingLane` opts into FrequencyLog2, GainDb or Q. This tier requires
exactly one enabled band at index zero, Stereo placement, immutable shape/slope
and audition, base rate and a maximum callback of 4096 samples. Gain movement
requires a gain-bearing shape. Unsupported settings refuse before replacing
the old stream. The default fixed bank retains its six-band path and allocates
no moving state, coefficient plan or moving blend scratch.

`mapTarget` maps a unit normalized value into an exact float DSP coordinate:
log2 Hz, dB or linear Q. It rejects invalid input without changing the output.
This utility is not a host controller, typed-value parser or state format.
Frequency stays in log coordinates, including its float endpoints, through
coefficient design. Stopped preparation starts from the physical Band values;
its initial frequency coordinate is float(log2(physical Hz)). An admitted mapped
target is already in the DSP coordinate and is not round-tripped through typed
Hz, parameter formatting or a host readback. Targets within float epsilon of the admitted target are
ignored without drifting that target. A larger change continues from the current
float publication, using a float slope with a double sample accumulator and
float callback reanchoring. Interruption does not jump to the previous target.

`beginBlock(BlockRequest, BlockContext&)` admits targets and lifecycle actions
only at physical callback boundaries. There is no nonzero-offset target API.
The subsequent context-based `process` accepts arbitrary contiguous segments,
with pointers to the physical block start; it preserves that callback's 4/32
coefficient checkpoints. A new callback cannot replace an unfinished one.
Invalid, stale, foreign, gapped or overlapping-buffer calls retain the cursor
and signal histories, so the caller may correct and retry. Valid moving spans
must contain at least one sample. All sample planes must be complete and finite.

Coefficient events use prepared bounded storage: at most ceil(maxBlock/4)+1
event records, each referencing the actual prepared SOS count (at most eight).
The callback first stages a small control/prototype cache and the required
coefficient records; design failure leaves admitted controls and audio histories
unchanged. Rendering borrows those records, publishes coefficients while
preserving double DFI histories, and uses precise span loops rather than
cross-unit per-sample calls. Frequency-only redesign retains the analog root
batches. Constant controls skip plan scanning, coefficient work and cache copying.
Adaptive coefficient design is bounded **on the callback**; it is not delegated
to a worker or advertised as off-audio. No callback allocation/free, waiting,
locks or shared epoch atomics are used.

Running boundary Reset and ReprepareSame complete the pending ramp at its target
and clear signal histories. RestoreTarget completes it at the supplied recalled
target (or the existing target) while preserving histories. All three invalidate
the coefficient epoch; no native opaque-state parser or arbitrary configuration
migration is implied. Stopped `reset()` also invalidates outstanding contexts.
Running generations are local to the instance; the stopped preparation identity
separates contexts belonging to different streams.

Generated checks cover target mapping/refusal, prototype-cache equivalence,
interruption, fixed/constant equivalence, arbitrary segments, lifecycle and
resource/continuation guarantees. Their generated coverage does not establish
all shape/rate/control combinations or a whole compressor performance claim.
