# Optional media_files.peaks cache: APKS v1

This is a regenerable drawing cache in the existing nullable BLOB, never audio
or editable musical content. It introduces no table, column or schema version.
NULL means absent; unknown versions/encodings remain opaque and preserved. A
reader may regenerate a separate result from authoritative media without writing
this field. Source content identity and any stored pool dimensions must match.

All integers and IEEE float32 values are explicitly little-endian, without native
struct padding. The header is exactly 80 bytes:

| Offset | Bytes | Meaning |
|---|---|---|
| 0 | 4 | ASCII APKS |
| 4 | 2 | Version, 1 |
| 6 | 2 | Header size, 80 |
| 8 | 4 | Source sample rate, 1 to 768000 |
| 12 | 4 | Channels, 1 to 64 |
| 16 | 8 | Source frames, at most INT64_MAX |
| 24 | 8 | Frames per bin, at least 1 |
| 32 | 8 | Bins per channel, ceil(frames / framesPerBin) |
| 40 | 32 | Source BLAKE3 bytes (pool hash is its lowercase 64-digit hex) |
| 72 | 8 | Reserved, zero in v1 |

Payload order is bin, then channel, each an 8-byte pair: minimum float32 followed
by maximum float32. The total bin/channel pair count is at most 1048576; total
bytes must equal 80 + 8 * binsPerChannel * channels, with no trailing bytes.
Every pair must be finite and minimum <= maximum. An empty source has zero bins.
A builder maps nonfinite source samples to zero for drawing and preserves finite
values above unity. A short final bin covers only remaining source frames.

The reader checks header size/version/reserved bytes, arithmetic bounds, exact
payload length and all ranges before publishing a result. The pool reader uses
SQLite length/CASE to reject an oversized BLOB without materializing it. No
cache data enters the audio callback. Missing files, malformed caches and unknown
versions cannot change or replace original media.
