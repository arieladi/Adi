# Hybrid Reverb — checklist before code

Read Live 12 Audio Effect Reference pp.590–596. Own wording; mission explicitly chooses convolution plus Surge reverb rather than copying Live's five proprietary algorithms.

- Convolution and algorithmic reverb combine in Serial, Parallel, Algorithm-only and Convolution-only routes; Blend weights their contributions.
- Send changes only the wet excitation; predelay can use milliseconds or note duration at host tempo.
- Import an actual time-domain impulse, retaining stereo channels; prepare its attack/decay/time-scale off audio, then publish through [adi.sample]'s existing host mechanism.
- Use the existing GPL-3 Surge Reverb1 adaptation for the algorithmic engine, with decay, room size, algorithm delay and freeze/input-freeze controls.
- Dry/Wet and stereo width act at the output. Compensate dry/algorithm timing for the convolution's fixed intrinsic latency and report it to the host.
- Exact convolution oracle, routing, latency, all block sizes 32–4096, allocation audit; LibPdEngine top-level patch, clean console, floor-bin-before-peak proof, sample replacement while rendering; float WAV only.

Scope choice: implement the requested two-engine core. Live's five algorithm identities, four-band EQ, Vintage and Bass Mono are follow-up parity controls, not names applied to a different algorithm. IR attack/decay/size are host preparation options; rebuilding a long kernel on the audio thread is forbidden. All unverified control values get one ADI table.

## Implementation and validation

Original ADI 256-sample partitioned FFT convolution retains the impulse's phase and stereo channels. The derived immutable kernel is prepared off audio with `prepareHybridReverbSample`, then sent through existing `LibPdEngine::publishSample(1, ...)`; no second publication mechanism. The wrapper reads it only within PdSampleBlock's lifetime. `adi.hybridreverb~` is compiled in; `Hybrid Reverb.pd` is a top-level adc~/dac~ canvas, with existing adi.param, adi.sample and adi.transport declarations.

The algorithm reuses `live_reverb.cpp`, the GPL-3 Surge/SST Reverb1 adaptation (upstream headers retained there). Serial runs convolution into this algorithm; Parallel and either single-engine mode have aligned timing. Intrinsic latency is 256 samples; Pd's adapter separately adds its existing 64. Predelay and wet Send precede both engines; wet feedback is bounded, Dry/Wet and Blend have 5 ms smoothing. Algorithm Freeze In controls whether new audio can excite the frozen tail.

MSVC /WX: 13 core checks, 11 LibPdEngine checks, 158 Pd-engine regression checks. Direct time-domain convolution oracle spans several partitions; all integer block sizes 32–4096 yield identical output. Exact dry latency, free/synced predelay, four-route endpoints, stereo IR and allocation audit pass. Pd tests decode an actual impulse WAV on the host, prepare/publish it, check a far bin at the floor before comparing the expected response, read the output float WAV back exactly, and replace IRs while rendering with no allocations and a clean console. CI/Pd validators pass.

Earlier stack CI identified `-Wdangling-else` and `-Wmisleading-indentation` in MIDI graph additions. Explicit braces and formatting are fixed HERE at the stack top, without pushing #207/#208 or intermediate UI branches. These are control-flow clarification only. The Linux build also found an indirect `<cmath>` dependency in the modulation test; the explicit include is carried on this top.

Limits: maximum IR 262144 frames (reject, never truncate); independent stereo channel convolution, not four-channel true-stereo; replacing an IR changes the kernel at the next convolution partition, not a dual-kernel tail crossfade. IR attack/decay/size are host-side preparation options. Live's named algorithms, EQ/Vintage/Bass Mono remain explicit parity extensions; this PR does not mislabel Surge as those algorithms.

## Unverified against Live — one table

Every range/default/unit/curve below is **ADI, unverified against Live**. The 256-sample partition, 262144-frame cap and 5 ms smoothing are engineering choices, not Live measurements.

| Live control / ADI option | ADI range | ADI default | Unit / curve |
|---|---|---|---|
| Routing | Serial / Parallel / Algorithm / Convolution | Serial | discrete |
| Blend | 0..100 | 50 | % / linear |
| Send | -60..12 | 0 | dB / linear control, exponential gain |
| Predelay | 0..2000 | 0 | ms / linear |
| Predelay Sync | Off / On | Off | discrete |
| Synced duration | 1/64..4 | 1/4 | quarter notes / linear |
| Predelay Feedback | 0..0.95 | 0 | ratio / linear |
| Decay | 0.1..20 | 1.5 | seconds / logarithmic control |
| Size | 10..400 | 100 | % / linear |
| Algorithm Delay | 0..250 | 0 | ms / linear |
| Freeze | Off / On | Off | discrete |
| Freeze In | Off / On | Off | discrete |
| Stereo | 0..200 | 100 | % / linear side gain |
| Dry/Wet | 0..100 | 50 | % / linear amplitude blend |
| IR Attack (host preparation) | 0..20 | 0 | seconds / linear envelope |
| IR Decay (host preparation) | 0 disabled, otherwise 0..60 | 0 | seconds to -60 dB / exponential envelope |
| IR Size (host preparation) | 0.25..4 | 1 | time scale / linear interpolation |
