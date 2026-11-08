# General AAC-LC import

The ordinary `audio::DecodeCache`, `media::prepareImport` and
`clip_edit::prepareImportAudio` path accepts supported AAC-LC in ISO-BMFF
M4A/MP4 and raw ADTS. Recognition uses file bytes, including when the extension
is wrong. Decoding stays on the owner worker, into source-BLAKE3-keyed float32
WAVs at the original rate and mono/stereo layout. Playback uses the existing
paged PCM reader; it performs no AAC decoding or file access in the callback.
This increment completes the ordinary decode-before-play path for the accepted
profile. Progressive playback before decoding finishes remains separate work.

General M4A parsing uses `readAacAudioMovie`, distinct from NI Stems'
`readAacMovie`. The general reader accepts mono/stereo LC and sibling AAC tracks
with different configs/rates/durations. Ordinary audio import chooses the first
audio track in file order; it does not mix tracks or implement a track-selection
UI. Non-audio handlers are skipped. A movie with no audio reports `decode.no_audio`;
generic media capture can retain that opaque MP4 with null audio dimensions,
while Import Audio still refuses to claim video playback. Unsupported audio
codecs/profiles fail explicitly, rather than becoming a silent media row.

ISO-BMFF retains the existing bounds: five audio tracks, one million packets per
track, 64 MiB movie metadata, 1 MiB per packet and 1 MiB NI metadata. Packet
references must remain within media extents. Fragmented/encrypted streams,
composition offsets, complex/non-unit/empty edits, nonzero sample-entry versions,
HE-AAC/SBR and unsupported channel/frame/rate modes refuse. The accepted LC
config is mono/stereo, standard 1,024-sample frames and rates up to 65,535 Hz;
NI export remains its original stereo 44.1/48 kHz profile. An authored single
edit preserves exact priming/duration with the existing coarse-clock end rule.
The decoder must produce the complete declared sample window.

ADTS accepts MPEG-2/4 LC, fixed mono/stereo config, a standard supported sample
rate and one access unit per frame. It bounds packets to one million, validates
sync/layer/header/frame extents and strips seven- or nine-byte transport headers.
CRC fields are skipped, not checksum-verified. A configuration change, multiple
raw blocks or trailing truncation refuses the whole file. Raw ADTS carries no
authored gapless end: priming is zero and all coded frames, including any encoder
padding, are retained. No guessed delay or silence trim is applied. ID3-prefixed
ADTS and additional AAC profiles are not claimed by this accepted subset.

The platform adapters decode mono/stereo layouts. Linux uses dynamically loaded
libavcodec 60/61 with matching libavutil 58/59; Windows uses Media Foundation and
macOS Core Audio. Native Windows/macOS decoding privately rewraps a single track
through `writeAacAudioMovie`, which supports mono without relaxing NI Stems'
aligned-stereo writer or export contract. The existing native availability probe
is retained; unavailable/unverified codec versions report `decode.codec_unavailable`.
No external transcoder executable or Python runtime is invoked or linked.

The original `playable(source)` API remains, with a cancellable overload taking
`std::stop_token`. Tokens are checked during source hashing, PCM blocks, AAC
packets/draining and before publication. The owner checkpoint hook runs outside
the cache mutex and must not reenter the cache. Through media/material capture,
it must not reenter that Store either. It can report progress and request
cancellation. MP4 metadata parsing is bounded but is not incrementally cancellable.
`media::prepareImport` retains its old API and adds a token overload; ordinary
material import forwards its token. Copy-in uses fixed 64 KiB buffers with stop
and exact-size checks. Cancellation leaves no native request or project rows.

Callers must provide a closed, stable source during decoding. Before publication,
the source is hashed again; a changed final hash refuses its former cache key.
These endpoint checks are not an atomic snapshot of a file being written in place.
The private WAV is published
without replacing an existing target. A concurrent winner must be a real complete
WAV with the expected dimensions; its bytes remain untouched. Cached hits are
validated before pinning, rejecting incomplete/corrupt targets without overwriting
or deleting them. Cancellation and other decode failures cannot be hidden by a
concurrently appearing target. Only this attempt's private files are removed on
failure. Already published valid cache audio may remain available for later use.
Existing LRU/size/free-space limits and pins continue to apply; decoded size is
reserved before AAC decoding and multiplication overflow refuses.

Native media rows record content-based `m4a`/`aac`, original compressed-source
hash and exact decoded dimensions. Copy-in retains the original compressed bytes;
cache PCM is regenerable. The ordinary media+clip batch remains one undo action,
Warp off by default; no project opcode or schema change is introduced. Media and
material capture refuse callback calls before allocation, clearing caller values,
SQL or file I/O. DecodeCache itself remains the existing throwing worker API;
its callback refusal is before file I/O.

`tests/fixtures/aac_import` contains synthetic files made by an independent
FFmpeg CLI encoder/muxer and independently decoded reference WAVs. Tests compare
authored timing, coded ADTS timing, layout, channel order, PCM and real Session
playback at both precisions; cache identity/restart, native import undo/redo/reopen,
live packet cancellation, source mutation, concurrent publication, malformed
ADTS and opaque non-audio containers are exercised. Platform execution claims
belong to the actual gate log, not this API description.
