# Color-bass devices (working names)

Open `Chord Comb.pd` or `Color Cab.pd` as a **top-level** `LibPdEngine` patch,
with two inputs and two outputs. Add the parent `pd/` search path for
`adi.param`. These canvases use `adc~` / `dac~` (ADR-0197).
Both externals are built into ADI; no native libraries are loaded from disk.

Chord Comb exposes State, Mode, Decay, Color, Mix and Output. Its eight default
chords are initialized in the patch. A `chord <0..7> <six fractional MIDI notes>`
message edits a stored chord; State selects it with a 10 ms bank crossfade.
Color/Decay preserve the ringing history. Requests during a fade coalesce.

Color Cab declares sample slot 1 and Mix parameter 2. The host calls
`bindSamples(parsePdDeclarations(patchText))` with audio stopped. Decode the
source into a `PdSampleBuffer` at the engine's processing rate, then call
`prepareColorCabSample(buffer, options)` **off the audio thread**. Options are
Size (64..1024), gamma (0..1), smoothing in octaves (0..2) and pitch in semitones
(-24..24). Rebuild with new options off-thread, then call
`engine.publishSample(1, std::move(prepared))` on the message thread.
The prepared buffer retains its source BLAKE3, options, and derived magnitude
profile alongside the FIR. A state serializer can retain that profile when
media is unavailable; this PR does not add the panel/drop tile or serialization.
Design controls deliberately use this host API, not audio-thread Pd messages.
The canvas exposes only the realtime Mix control.

Call `collectSamples()` on the message-thread timer. It uses #153's existing
`PdSampleSlots` / `SnapshotPublisher` retirement protocol. Each slot is acquired
once per engine segment and shared by both channel externals. No sample data
travels in Pd messages and no kernel is built or freed by `process`.
Missing/unprepared or wrong-rate slots leave the previous kernel playing
(identity initially). Correct-rate replacements crossfade over 20 ms.

`test_colorbass_pd.cpp` opens these actual canvases, checks the console hook,
checks an empty far bin before the sine peak, measures the predicted FIR
response, and publishes samples while another thread renders. No sound device
is opened. `test_combchord.cpp` and `test_colorcab.cpp` cover the DSP transitions,
fractional tuning, coefficient sweeps, pink-power make-up and allocation counts.
