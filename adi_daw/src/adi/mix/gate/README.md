# Fixed Classic gate component

`adi_mix_gate` is a framework-free, stereo, base-rate Classic downward gate. It
has zero latency and no native device registration. Clean, Vocal, Guitar, Upward
and Ducking return `UnsupportedConfiguration`; their names do not select an
approximation using the Classic engine.

Settings use physical units:

| Control | Accepted range |
|---|---|
| Threshold | −60 to 0 dB |
| Ratio | 1 to 100 |
| Range | 0 to 100 dB |
| Attack | 0 to 1 second |
| Release | 0 to 5 seconds, with a 5 ms effective floor |
| Hold | 0 to 250 ms |
| Knee | 0 to 30 dB |
| Left/right key mix | −1 selects left, 0 takes maximum, +1 selects right |

Each key channel has independent envelope and inclusive peak-hold histories.
Each output combines those two envelopes with its own mix, then applies the
hysteretic expansion detector and two gain-envelope stages to its original
programme channel. Hold affects both peak retention and release smoothing;
Knee also changes the effective expansion ratio. These controls are not generic
substitutes for another dynamics component's controls.

All settings are fixed for a prepared stream. Any change supplied to `process`
or `restore` is refused before output, history or segment position changes.
There is no moving-control or normalized controller admission API. The physical
Attack/Release conversion helpers are stateless; Settings defaults are useful
component defaults, not a device factory preset.

`prepare` creates a fresh stream and offers a strong failure guarantee.
`reprepare` accepts the same float-rounded rate and layout, retains settings,
and clears DSP history; changing capacity is allowed. `reset` clears all
envelope, hold and detector histories to the Classic silence baseline. An
identical settings restore retains history. Preparation, reset and destruction
require stopped audio; processing has one serialized owner.

Two distinct main planes are required. An explicit external key has two complete
planes, or zero planes for temporary silence on that same route. It never falls
back to the programme. Mono, partial/null planes and partial overlaps are
refused. Exact key/main aliases, including crossed channels, are supported.
Samples must be finite and within float's finite range. Double I/O rounds to
float on entry and widens on output; envelope history uses double precision.

Physical callbacks use `beginBlock` followed by contiguous segments whose
pointers address the physical block start. Segments retain the physical block's
16-sample adaptive-control cadence. Changing physical block partitions can
change that cadence; arbitrary block-size bit identity is not promised.
Contexts are invalidated by reset and successful preparation. IO precision is
fixed until reset/preparation. Invalid calls preserve the current segment cursor.

The accepted rate range is 44.1–96 kHz, with float-rounded internal rate.
Validation covers generated signals at 44.1, 48 and 96 kHz; it does not establish
all intermediate rates or all control combinations. Tests cover hold expiry,
hysteresis, stereo key choices, physical-block segmentation, state reset and
continuation, failed preparation, alias refusal, both IO precisions and zero
callback allocation/deallocation. No CPU or live-host performance claim is made.
Round-to-nearest must remain fixed through preparation and processing.
Preparation refuses other rounding modes before initializing the shared
immutable lookup table. The caller owns FTZ/DAZ policy; the core does not change
the environment. Its detector floor does not clear the double envelope
histories. There is no fitted silence cutoff, and CPU behavior during very long
silent tails remains unmeasured in the eventual adapter environment.

The component has fixed unity levels, center pan, normal wet output and neutral
key filters. It does not expose oversampling, lookahead, audition, M/S, key EQ,
dry mixing, bypass, mono or dynamic configuration. A future adapter must reject
those unsupported capabilities instead of silently ignoring them.
