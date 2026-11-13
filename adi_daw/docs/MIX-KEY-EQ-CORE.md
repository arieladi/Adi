# Fixed key EQ component

`adi_mix_key_eq` is an unregistered, framework-neutral, fixed base-rate stereo key
processor. It does not enable a native compressor/de-esser/gate function or change
the existing plug-in engines. Its output is the filtered key, not limited or
compressed programme audio.

Preparation builds ordered root batches and analog sections from physical band
frequency, gain, Q, shape and slope. It applies the explicit float boundaries,
first-section Q floors, section-specific extrema/prominence, anchor selection
and response-matching policy before using the common precise DFI stages. The
ordinary Bell, shelves, cuts, Notch, Band Pass and Tilt paths are supported.
Flat Tilt, All Pass and the separate brickwall-cut path are explicitly refused.
Non-cut shapes retain their bounded slope-clamping law.

Each prepared bank has up to six ordered bands and fixed section capacity. The
stereo path forms Side as `(R-L)*0.5`, Mid as `Side+L`, then applies each band's
Stereo/Mid/Side placement. Inverse conversion and optional mono audition retain
their float boundaries. Near-zero gain-bearing bands use the full-cascade dry
blend and the signed small-gain prototype floor. A reusable dry buffer is
allocated only when that prepared blend needs it.

Controls and layout are fixed until stopped prepare. Unsupported configurations
and invalid process calls are refused before writes or history advancement.
Failed preparation retains the complete previous configuration and state.
Reset clears recursive histories; no serialized-state restore or running
enable/topology transition contract is implied. Other layouts, quality conversion,
moving controls and compressor gain remain separate implementation gates.

The caller owns mutable planar key buffers separately from programme audio and
serializes lifecycle calls. In-place M/S conversion and one precise span call per
section/channel/segment avoid extra always-allocated scratch or per-sample
external dispatch. Coefficients are built only in preparation; processing has no
allocation, locks, atomics or worker. Paired-state/SIMD optimization and full graph
CPU comparisons require later numerical and timing gates.

Generated tests cover configuration boundaries, bounded section counts, strong
failed-prepare behavior, invalid buffer calls, split blocks, reset/reprepare,
placement, audition, near-zero blending and zero callback allocation/free.
Their authored multiband/routing mechanics are not additional native parity
evidence. Private reference fixtures and tools remain outside public sources.
