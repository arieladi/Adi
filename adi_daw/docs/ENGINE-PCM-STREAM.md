# Owner-worker PCM pull streams

`audio/pcm_stream.hpp` exposes the existing native decoders as a synchronous
pull stream. It does not create jobs, threads, project rows, cache files or a
progressive playback path. Those consumers can now open a decoder, copy its
immutable metadata, and request interleaved float32 blocks before the rest of
the file has decoded. This branch is independent of the AAC import branch;
its native profile is RIFF/AIFF/FLAC/MP3/Ogg, including Ogg FLAC through the
existing dispatch. AAC streaming belongs to the later integration.

`openPcmStream` runs on a worker, accepts a regular file (including a symlink to
one), and returns explicit Ready, Cancelled, AudioThread or Failed status. It
opens native codec metadata and validates 1..64 channels / 1..768000 Hz. The
optional frame count is exact when cheaply available; absent means unknown,
not a duration estimate. MP3 without a Xing/Info length header is not fully
scanned/decoded to discover its length. Native initialization may still read
headers/end pages or predecode an initial codec frame. The legacy complete-cache
path retains its exact-length pre-scan policy; this API introduces no new
normalization or trim.

Read and final destruction belong on the opening worker. `read` accepts a
caller-owned span containing 1..65536 whole frames and reports Frames, End,
Cancelled, InvalidBuffer, AudioThread, WrongOwner or Failed. No returned frame
count exceeds the span. Empty/partial/over-bound spans, pre-cancelled blocks,
callbacks and another worker leave its buffer, decoder position and diagnostic
state unchanged. These refusals do not poison the stream. Cancellation is
between bounded blocks; a native read already in progress is not interrupted.
A stopped request can be retried with an active token. Open checks a token
before I/O and again after native metadata initialization.

Read until explicit End, even after reaching a declared length. Overrun/short
EOF relative to known metadata and reported codec errors fail terminally; a
later file repair cannot resume that same failed decoder. Reopen explicitly.
Unknown totals are counted as they stream and retain the decoder's own EOF/
integrity limits: missing metadata cannot prove every truncated codec tail.
`framesRead`, `problem` and borrowed `error` are owner-only observations. Metadata
is immutable after open; copy it into a job/snapshot rather than exposing the
mutable decoder to the callback. Caller-owned chunks can feed a bounded job
queue or writer. Sources must remain closed/stable; this API does not hash or
snapshot them. Input lifetimes, content authentication and publication belong
to the caller's job/cache layer.

Vorbis now uses stb_vorbis's native FILE section API instead of retaining the
whole compressed file in a vector. ADI opens that FILE wide on Windows and
natively on POSIX; stb owns its cursor, ADI owns final closure. The section is
bounded by the checked file extent and preserves the existing 2 GiB seek limit.
Its C implementation enables the file API and chooses the vendor's secure CRT
branch on Windows; no third-party source was edited. The original complete
DecodeCache path also uses this streaming Vorbis backend. RIFF/AIFF/FLAC/MP3
continue to use audited C++ native file callbacks. Factory/read callback guards
precede allocation/free, file I/O and mutable state; valid decoding never runs
on the audio callback. This primitive does not move ClipPlayback's existing
synchronous setup or implement ready-prefix pages.

The suite checks variable 1/7/31/4096/65536-frame chunks, guards outside spans,
complete PCM equality to the existing cache across eleven fixtures, independent
16/24-bit lossless originals, source dimensions and EOF, owner/callback/stop
refusal followed by correct resumption, terminal source truncation/repair,
known-empty WAV, float32 bit preservation, Unicode Vorbis paths, no-Xing unknown
length and authored frequency/amplitude, and absence of the old whole-compressed
C++ allocation on a longer Vorbis fixture. See the engine log for actual platform,
compiler, sanitizer and mutation evidence. Progressive cache jobs and actual
Session playback before completion remain open until their integration gate.
