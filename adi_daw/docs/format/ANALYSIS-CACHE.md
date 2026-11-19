# ADIA analysis cache envelope, version 1

This is optional application cache data, not an ADI project stream or a user
library setting. One file per source hash, `.adia`, under the owned `analysed`
folder. All integers are unsigned little-endian. File size is 168 bytes through
32 MiB, including the header. There is no compression or recursive encoding.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `ADIA` |
| 4 | 2 | envelope version, 1 |
| 6 | 2 | header size, 168 |
| 8 | 64 | lowercase ASCII BLAKE3 of the source media bytes |
| 72 | 4 | source sample rate, 1..768000 |
| 76 | 4 | channels, 1..64 |
| 80 | 8 | source frames, 0..INT64_MAX |
| 88 | 4 | section count, 0..32 |
| 92 | 4 | exact body byte count, file size minus 168 |
| 96 | 64 | lowercase ASCII BLAKE3 of the complete body |
| 160 | 8 | reserved, zero |

Each body section has a 16-byte header followed by exactly its payload bytes:

| Relative offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | unique ASCII tag, uppercase `A`..`Z` |
| 4 | 4 | nonzero algorithm/section version |
| 8 | 4 | algorithm configuration identifier |
| 12 | 4 | payload length |
| 16 | length | opaque payload |

The producer writes tags in lexicographic order; a reader accepts any unique
order. The body digest is verified before copying section payloads. A bounded
first pass rejects duplicate tags, impossible sizes, unknown envelope version,
nonzero reserved data, truncated records and trailing bytes before section
allocation. A file read also matches the complete expected source identity and
dimensions. The digest is an accidental-corruption check, not authentication
against a hostile cache owner. An atomic replacement can race a metadata probe;
a size mismatch produces a cache miss, never a partial successful record.

`APKS`, version 1/configuration 0, embeds the existing APKS waveform codec
including its own source identity/dimensions. The consumer validates that codec
and matches those fields to the media pool. Other tags remain opaque and survive
ordinary same-process updates. An inconsistent or invalid optional envelope can
be rebuilt; user Warp/clip settings must never be stored here.
