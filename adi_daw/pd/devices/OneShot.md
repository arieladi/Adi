# OneShot recovery

`OneShot.pd` is a top-level instrument canvas with stereo `dac~` outputs and
the compiled-in `adi.oneshot~` external. Open it through `LibPdEngine` with two
inputs and two outputs and add the parent `pd/` search path. Its controls
declare the native parameter names, units, ranges and defaults, in the native
order: the first forty-one are the ones every project saved before SC-482
holds, and each control added since comes after them, never renumbered. A
state that lacks a control gives it its default, and every added default plays
what OneShot played before the control existed (`test_oneshot.cpp`,
`legacyState`).

SC-482 added, beside Live 12's Simpler: a filter envelope (attack, decay,
sustain, release and an amount of +-72 st on the cutoff) and a pitch envelope
(on/off, the same four times and an amount of +-48 st), both linear like the
amp envelope, from 0 at note-on, released with the note in every mode. The
filter's two circuit choosers, as Live's: Clean, OSR, MS2, SMP and PRD for
low- and high-pass, Clean and OSR for band-pass and notch; Drive (0..24 dB) on
every circuit but Clean, for low-, high- and band-pass; and the cutoff's
velocity amount (100 % opens it four octaves at full velocity, ADI's span) and
key amount (100 % moves it an octave with each octave of pitch from C3). The
circuits are ADI's saturations around Clean's state-variable stage, linear for a
quiet signal; Live's own circuits are readings still to take. The LFO's Attack
(its depth fades in linearly from note-on), Retrigger (on, the default: each
note starts its LFO at Offset, 0..360 degrees; off: at the phase of a
free-running LFO that runs through silence too) and LFO < Key (100 %: an octave
above C3 runs the LFO twice as fast). Glide (Off, Glide, Portamento) and Glide
time: Glide is mono and legato, a note played over a held one takes that voice,
its envelopes and its place in the sample, and slides to its own pitch;
Portamento is polyphonic, each new voice sliding from the pitch of the newest
note still sounding. Neither glides from silence, and Slice mode never glides.
Whether Live's legato Glide restarts the sample is a reading still to take.

Since DEV-21 step 1 the ranges, units and defaults of the controls OneShot
shares with Simpler are Live 12.4's own, read from the default Simpler preset
that build ships (Volume -36..36 dB at -12; Attack 0.1 ms to 20 s at 0.1 ms;
Decay and Release 1 ms to 60 s at 600 ms and 50 ms; Sustain -70..0 dB at 0 dB;
Fade In and Out 0..2000 ms at 0.1 ms; Loop, Snap, Filter and LFO on; Filter
30 Hz..22 kHz at 22 kHz; Resonance 0..125 %; Voices 6; Vel > Vol 35 %);
`test_oneshot.cpp` pins them. The LAWS behind them are still ADI's and are not
claimed as Live's: the linear attack, decay and release (Live has slopes), the
Vel > Vol blend, the Resonance-to-Q curve, the filter (its Morph type is not
built; its circuits are ADI's, see below), and the Voices chooser's list (a
reading to take).

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
MIDI state without allocating. The native core's own `prepare()` resets voices
only when the rate or block size changes, or after `release()` (ADR-0090 d5,
DF-E37 f); the Pd external's DSP restart resets them explicitly.

Tests cover independent synthetic gain/pitch oracles, event timing,
state/file round trips, immutable replacement and clearing, sample publication
during rendering, real-Pd wiring and allocation/deallocation checks. Failure
plants prove the synthetic oracles reject wrong gain and timing. No measured
Live 11 or Live 12 output fixture is included. Warp, proprietary filter models,
waveform/zone editing and the other original #206 parity gaps remain open.
