# Prepared warp audio (N17)

The worker API in src/adi/audio/warp.hpp renders a source window to a private,
aligned Float32 WAV. WarpRenderPlan supplies the source/file-rate frame window,
session rate and explicit source/output-frame anchors. The first anchor is
0 -> 0 and the last covers the complete source and output. All coordinates
are captured values; this layer reads no Store, settings, plug-in or live Session.

The six engines follow ADR-0188 d2:

| Mode | Worker |
|---|---|
| Beats | Native transient slice player; Off, Forward and PingPong tail loops and transient-envelope decay |
| Tones | Rubber Band R3; grain sizes up to 50 ms select its short window, larger sizes its standard window |
| Texture | Native Hann grains at fourfold overlap; shared stereo grain jitter from a captured seed |
| Re-Pitch | libsamplerate sinc-best, with a continuous state and explicit rate changes at each anchor |
| Complex | Rubber Band R3 |
| Complex Pro | R3 with independently preserved/scaled formants |

R3's coarse Tones window choice and missing Complex Pro Envelope control are
the approved parity gaps in ADR-0188. The current native Beats tail starts
after a 10-ms attack; Texture defaults to 65-ms grains and 25-percent jitter.
Native controls and sound still need comparison with Live. Generated tests
check frame placement, spectra, loop direction, stereo coherence and seeded
repeatability; they do not establish measured Live sound.

The source rate may be 8--768 kHz and the output 44.1--768 kHz, up to 64
channels. Source/output windows are limited to 2^31-1 frames on every ABI.
Re-Pitch segment ratios must be within libsamplerate's supported range; its
pitch follows tape speed, so independent transpose requests refuse. Native
Beats/Texture first use sinc-best file-rate conversion when needed. R3's
pitch and formant corrections account for file-rate conversion separately.
Non-finite input samples become zero before DSP.

prepareWarp is synchronous worker/owner-thread work. Cancellation is checked
between bounded reads/process blocks and output drains. Calling it on the
audio callback returns WarpError::Realtime before allocation or I/O. Keep the
returned shared_ptr<const PreparedWarp> alive while its file is read, then
close readers and release it off the callback. The final owner removes only
the private directory created for that preparation. The type cannot be
constructed or copied by callers, and its file/metadata cannot be changed.
This file is temporary playback data, never a media-pool asset path.

R3 runs offline, including study and exact-duration processing. Its prepared
PCM is already aligned at output frame zero, so a paged reader of it adds
zero live stretcher latency. A future live stretcher needs its own declared
PDC delay. Preparation and destruction belong off the audio thread.

src/adi/audio/warp_map.hpp defines AWRP's 16-byte frame/tick records and
checked canonical maps (SPEC 6.4). Readers accept wider future records through
the generic stream stride and leave the original bytes untouched. A producer
requiring new unknown record fields must refuse a rewrite. The shared
detectTransients reader in audio/transients.hpp supplies source-relative onset
frames to Beats and Auto-Warp analysis; it uses bounded windows,
has no Store side effects and refuses on the callback.

Auto-Warp (audio/auto_warp.hpp) fits an onset grid, optionally follows tempo
changes and places bar anchors in the supplied meter. From First Beat trims
the source window to the first detected attack; Sample Start retains its
original offset. A supplied source BPM provides a straight clock for short
or non-percussive material. Without enough reliable timing evidence the
producer refuses with NoTempo. Grid confidence does not resolve half/double
tempo ambiguity; the captured tempo hint selects between those candidates.

clip_edit::prepareImportAudio connects this analysis to a captured AWRP blob,
source window and musical clip length. It performs no project writes until
commitMaterial. Replay/redo uses the captured map, never another analysis.
Copy-in publication allows an identical regular owned file retained by undo;
it never replaces a different file or removes an existing file on SQL failure.
Cancellation is checked again at commit, including deduplicated imports.

ClipPlayback prepares enabled Warp clips off the callback and hands their
private PCM to the ordinary paged reader. engine/warp_projection.hpp projects
source musical ticks through the project tempo integral, adding interior
tempo junctions. It handles trimmed source clocks before project zero by
extending the initial tempo. Source offsets, loop bounds and end markers use
that musical clock; fixed nanosecond pins from earlier unwarped playback do
not override it. A tempo change requires a new prepared playback generation.
The formant_shift row value is an extra semitone shift (ratio 2^(value/12));
the default is zero. Reverse and channel aliases remain unsupported here.

Split, copy and range/Time material edits retain warped fade shapes on the
unrolled source musical clock (clip_warp_fade_domains, schema 1.13), including
repeated splits inside overlapping fades and source loops. Handle edits replace
only the selected side; undo restores its full typed metadata.

clip_edit::prepareSetWarp (warp_commands.hpp) supplies the Warp button for a
clean, full-source existing import (ADR-0203 d7). Enabling captures detected
trim, AWRP, mode and musical length on the same clip identity. Disabling pins
the actual physical source clock and restores its unstretched duration; it
does not leave a stretched placement with only its flag changed. Both directions
retain the rest of the full typed rows and undo exactly. Expected journal head,
document generation, cancellation and a commit-time file hash protect analysis.
An unknown future marker layout refuses a rewrite. A clip already in the
requested state, or with edited source offsets, loop, End marker, reverse,
pitch or trimmed extent, currently refuses this clean-import producer. Use the
separate material APIs for split/copy; this API does not promise arbitrary
edited-clip Warp toggling or individual catalogue marker setters.

Import and playback connect all six modes;
these tests establish generated-signal behavior, not measured Live parity.
