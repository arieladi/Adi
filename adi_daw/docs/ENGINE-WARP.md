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
frames to Beats and the upcoming Auto-Warp analysis; it uses bounded windows,
has no Store side effects and refuses on the callback.

At this increment these worker APIs and marker validation are implemented.
Auto-Warp tempo/bar analysis, its project edit producer and the ClipPlayback
handoff are the next integration steps. The import producer continues to
report that Warp import is unavailable until that complete path is connected.
