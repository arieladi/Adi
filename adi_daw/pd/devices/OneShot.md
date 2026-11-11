# OneShot recovery

`OneShot.pd` is a top-level instrument canvas with stereo `dac~` outputs and
the compiled-in `adi.oneshot~` external. Open it through `LibPdEngine` with two
inputs and two outputs and add the parent `pd/` search path. Its forty controls
declare the recovered native parameter names, units, ranges and defaults.
These are ADI values from #206, **unverified against Live**. This recovery adds
no new claim about Simpler parity or fresh Live defaults.

Bind parameters and sample declarations with audio stopped. Decode sample slot
1 off-thread, then call `prepareOneShotSample()` before publishing the result
through `LibPdEngine::publishSample()`. Preparation copies, validates and
analyzes the audio off-thread; the published buffer also retains original
audio and its media hash. Call `collectSamples()` on the message thread.
Use `clearOneShotSample(1)` to publish a clear with a new generation.
Missing, cleared or unprepared samples play silence. A replacement ends old
voices at its first processing boundary; no sample ownership changes in the
audio callback. Each core uses one publication source until `prepare()`.

The Pd fallback reconstructs note identity from MIDI channel and key, uses
7-bit velocity and the host's ±48-semitone MPE bend range, and applies events
at Pd ticks. A repeated key on the same channel releases its previous identity
before starting the new note; MIDI cannot recover overlapping original note
ids in that case. A fixed 512-event queue counts overflow and clears voices
on the next tick instead of leaving a dropped release stuck. The external's
`drops` message reports that count from its third outlet. Tempo comes from
`adi.transport`. The native core retains original typed events, note ids,
double expression values and exact frame offsets.

OneShot reports no intrinsic latency. On current main, the generic Pd adapter
adds its reported 64-sample tick. Native zero-delay dispatch awaits the adapter
base and instrument integration. Production Session discovery, UI import,
automatic invocation of the prepared-sample helper, and Pd instrument
`EventFlow::Consume` metadata remain integration work outside this recovery.
Tests inject Session's existing public loader explicitly; they do not prove
automatic device discovery. The generic Pd device currently forwards notes.
Its same-rate `prepare()` resets MIDI routing without restarting an already
running Pd graph, so it does not automatically reset this external's held
voices. Until the host integrates lifecycle notification, send a float to
`$0-oneshot-reset` (via `sendFloat("oneshot-reset", 0)`) or publish an empty
sample slot before resuming. The explicit reset clears voices and the bounded
MIDI state without allocating. The native core's own `prepare()` resets voices.

Tests cover independent synthetic gain/pitch oracles, event timing,
state/file round trips, immutable replacement and clearing, sample publication
during rendering, real-Pd wiring and allocation/deallocation checks. Failure
plants prove the synthetic oracles reject wrong gain and timing. No measured
Live 11 or Live 12 output fixture is included. Warp, proprietary filter models,
waveform/zone editing and the other original #206 parity gaps remain open.
