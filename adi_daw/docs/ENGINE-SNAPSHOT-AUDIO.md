# Audio sources and waveform peaks (N40)

ClipNode.audio is optional AudioClipSource. It contains the audio_clips media id,
sourceStartFrames and sourceLengthFrames at the source file's own rate, plus its
warp flag/mode/nullable marker bytes, transpose, formant shift, reverse and channel
mode. mediaInfo carries the pool content hash, nullable rate/channels/total frames
and stored missing flag; it is absent when a legacy attachment has no pool row.
A stored missing flag is not a fresh file probe. All raw and pool identity fields
participate in structural sharing. Old snapshots stay immutable. Each rebuild
reads coherent metadata in one borrowed/deferred SQLite transaction; it performs
no filesystem lookup or decode and never copies the pool peaks into the snapshot.

For drawing, media/peaks.hpp supplies typed MediaPeaks and three owner/worker APIs:

- readMediaPeaks(store, mediaId) reads the optional pool cache only, with no file
  access. It validates the version, bounds, content hash and stored dimensions.
- buildMediaPeaks(store, mediaId, maxBinsPerChannel, stopToken, decodeCache) first
  uses a valid pool cache, otherwise resolves and hashes the original, uses the
  same DecodeCache/WavReader path as playback, and streams per-channel min/max.
  A short final bin is kept; nonfinite samples become zero only in this drawing
  result, while finite overs are retained. Memory is bounded to one million
  bin/channel pairs plus a 4096-frame read block. The maximum applies to newly
  generated data; a valid existing cache can have another resolution.
- encodePeaks/decodePeaks implement the optional APKS v1 cache documented in
  format/PEAKS.md. The worker does not write it, change pool rows, create ops or
  move history. Unknown cache bytes remain untouched even if rebuilding fails.

Use a separate worker-local Store::open(path, error, true) connection when the
window owns the live Store. Do not share a Store connection between UI and worker
threads. A completion must still match the current document/generation and media
contentHash before painting; memoize the typed result by content identity. A valid
cache can draw even when the original file is missing, without implying the audio
is available. Pool caches never override authoritative media or source windows.

Peak bins are bin-major then channel; each spans framesPerBin source frames.
Intersect them with sourceStartFrames/sourceLengthFrames, then apply the clip's
view transforms. They describe the original source, without baking project-rate
resampling, clip gain/fades, reverse or Warp. The snapshot provides Warp metadata;
this branch makes no claim of exact warped-timeline rendering or Live visual
parity. The window owns its drawing and integration.

Every allocating/SQL/I/O entry point refuses audio::onAudioThread first. Result
problems are explicit; callback refusal has empty detail/data. encodePeaks returns
empty on callback misuse and throws for invalid data off callback. Cooperative
cancellation is checked before work, during hashing, and between streamed reads.
DecodeCache's compressed decode call itself cannot be interrupted; cancellation
is checked immediately after it. The source hash is checked again after reading,
so an ordinary source-file change discards the result. No DB read transaction is
held across source hashing/decoding.
