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

The prepared R3 path runs offline, including study and exact-duration
processing. Its PCM is already aligned at output frame zero. Preparation and
destruction belong off the audio thread.

src/adi/audio/warp_map.hpp defines AWRP's 16-byte frame/tick records and
checked canonical maps (SPEC 6.4). Readers accept wider future records through
the generic stream stride and leave the original bytes untouched. A producer
requiring new unknown record fields must refuse a rewrite. The shared
detectTransients reader in audio/transients.hpp supplies source-relative onset
frames to Beats and Auto-Warp analysis; it uses bounded windows,
has no Store side effects and refuses on the callback.

Engine owners can pass `WarpTimingSettings` to `requestWarp` to use shared
AWTM timing for automatic Beats renders. The worker verifies the original
source hash and looks up the exact settings before checking prepared PCM.
It projects cached positions into the original source window before playback
rate conversion. A matching empty window uses the slice origin and anchor
positions. Missing or incompatible timing uses the incumbent detector.
Current ClipPlayback uses `requestWarp(plan)` while the timing rollout is
gated. The settings overload resolves AWTM for automatic Beats only.

Prepared PCM keys for shared timing include the canonical onset positions.
A new cache-aware request rechecks timing after the preceding request has
started that lookup, so a later AWTM publication can refresh a prior fallback.
Existing task holders retain their completed PCM.

Auto-Warp (audio/auto_warp.hpp) fits an onset grid, optionally follows tempo
changes and places bar anchors in the supplied meter. From First Beat trims
the source window to the first detected attack on the incumbent path. A
selected shared grid trims to its first measured downbeat. Sample Start
retains the original offset. A supplied source BPM provides a straight clock
for short or non-percussive material. Without enough reliable timing evidence
the producer refuses with NoTempo. The captured tempo hint selects between
half-time and double-time candidates when grid confidence leaves ambiguity.

clip_edit::prepareImportAudio connects this analysis to a captured AWRP blob,
source window and musical clip length. It performs no project writes until
commitMaterial. Replay/redo uses the captured map, never another analysis.
Copy-in publication allows an identical regular owned file retained by undo;
it never replaces a different file or removes an existing file on SQL failure.
Cancellation is checked again at commit, including deduplicated imports.

The prepared ClipPlayback path hands private PCM to the ordinary paged reader.
engine/warp_projection.hpp projects
source musical ticks through the project tempo integral, adding interior
tempo junctions. It handles trimmed source clocks before project zero by
extending the initial tempo. Source offsets, loop bounds and end markers use
that musical clock and take precedence over earlier fixed nanosecond pins.
This path needs a new prepared render for a tempo change.
The formant_shift row value is an extra semitone shift (ratio 2^(value/12));
the default is zero. Matching Pitch Editor edits are applied in source coordinates
before Warp. Streaming Re-Pitch reads the corrected native-rate window on the
same marker and tempo clock as a plain clip, including during A/B comparison.
Prepared renders use the same directed mapping when the page reader reverses
the prepared window.
Channel aliases remain unsupported.

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

## Streaming warp playback (TM-WT-04)

The Arrangement's native Beats, Texture and Re-Pitch players read the original
source window through the bounded PCM page cache. The compiled Warp index uses
the shared TM-TT-01 step, linear and Bézier tempo integral to map each output
frame to fractional AWRP ticks. Each loop iteration uses the tempo at its own
position. Tempo edits and Link changes use the existing Session generation
handover. On an authored tempo edit during playback, the driver carries on with
the previous graph until the new graph has the compensated R3 pages at its
current position, including around transport loops. Beats onset analysis stays
off the callback and is reusable across tempo changes. The native players
prepare bandlimited sample-rate kernels from the pinned converter off the
callback.

Supported Tones, Complex and Complex Pro clips use a bounded worker-side R3
voice at steady tempo and through tempo changes. It supplies
`getPreferredStartPad()` and discards `getStartDelay()` output frames. The
worker primes each tile with bounded history. Steady tempo uses 8,192-frame
tiles on the clip's source clock. Variable tempo uses 16,384-frame tiles on
the arranged output clock and pads the source with zeros at the placed end.
The fixed tile origins keep the output identical when a cold seek meets
cached pages. Stereo R3 uses mid/side to preserve the stereo relationship
through ratio changes. A 512-frame overlap aligns adjacent waveforms within
a 64-frame search window. The worker keeps two tiles and bounded source
pages and reclaims idle scratch.

R3 reset can allocate, so the audio callback reads published PCM pages. The
track source delays its clips by the declared R3 latency, and the graph
compensates parallel paths. Export removes that preroll from the requested
timeline range. The streaming R3 path supports time ratios and effective pitch
scales from 0.5 to 2. The prepared path still serves unsupported configurations,
with a clip diagnostic when a ramp falls outside the streaming domain.

`WarpClipPlayer::launchAt` projects a stored Session slot from its quantized
launch tick and source phase. Resident and paged Beats, Texture and Re-Pitch
use the effective clock from the persisted slot graph. The scheduler's Legato
forecast warms the upcoming source pages. A cold page miss emits silence and
counts an underrun while the worker fills the request.

A scene that mixes native and fixed sources keeps the effective clock for
native Warp and uses a flat prepared BPM for the fixed sources. Continued
native audio keeps its run and source phase while the fixed tracks stop or
launch new material. A mixed scene validates the continued clip and primes
the full raw source window when it needs all phases of a paged clip.

Supported fixed-clock R3 slots use aligned worker PCM. The track reports the
maximum source delay across its Arrangement clips and Session media banks.
The driver delays Session PCM and its Arrangement replacement mask together.
A scheduled R3 Legato tempo scene can switch when both banks have primed the
target's full bounded playback window. A larger candidate that cannot supply
that phase leaves the current scene and clock playing.
An external tempo update that would drop a running worker R3 slot keeps the
active graph. The driver rechecks the slot at adoption if a launch settles
after the owner prepared the candidate.

The stretcher seat report records the measurements for playback, export and
scheduled scenes, along with the outstanding Live reference readings.
