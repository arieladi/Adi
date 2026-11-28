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
and matches those fields to the media pool.

`PNOT`, version 1/configuration 0, contains monophonic pitch notes measured from
the equal-weight average of the source channels. The payload begins with a
64-bit note count. Each note is six 64-bit words in order: start frame, exclusive
end frame, median pitch, fitted drift, RMS vibrato and curve point count. A
point is a 64-bit source frame followed by the measured pitch. Pitch, drift and
vibrato are little-endian IEEE 754 binary64 values in cents, with absolute
MIDI cents for pitch (A4 is 6900). Notes are ordered, disjoint and bounded by
the envelope's source frame count. Each has at least one point and its
points are strictly increasing inside the note. A source with no voiced notes
has a zero count. The payload is limited to 16 MiB.

An optional `BND1` trailer follows the final note. The four ASCII bytes are
followed by a little-endian 64-bit hint count, then that many records. Each
record has a little-endian 64-bit source frame and a one-byte reason mask.
Bits 0, 1 and 2 mark an aperiodic transition, a rapid pitch change and competing
periodicity, respectively. At least one reason is set and the remaining bits
are zero. Frames increase strictly and may equal the source frame count for an
exclusive end boundary. An absent trailer means the boundaries were not
assessed. A zero count means the assessment found no hints. Hints are evidence
for reviewing boundaries and leave the note geometry and identities intact.

`ALIB` version 2 carries library descriptors, optional gated LUFS, type,
tempo and key with provenance and key strength, plus bounded read-only file
tags. Configuration 1 identifies the expanded tag reader. Like APKS, the
payload repeats source dimensions and checks them against the envelope.

`AWTM` version 2 carries a derived Warp analysis in the 44,100 Hz source
clock. Configuration packs the meter numerator, denominator and transient
sensitivity. The 56-byte payload header repeats the source dimensions,
counts the four point series, and carries the median tempo, an optional
source quarter-note tempo cue and the musical ticks per detected pulse.
Its series are ordered onsets (frame, normalized strength, local peak dBFS),
beat frames, downbeat indices, and tempo regions (first beat index and BPM).
Positions use 64-bit frames, indices use 32 bits and values use IEEE binary32
for onset measurements and binary64 for BPM. Tempo zero is the missing value.
The cache checks configuration, dimensions, size, ordering, and value bounds
before returning data. Authored AWRP remains in the project edit.

Other tags stay opaque and survive ordinary same-process updates. An invalid
optional envelope or pitch payload can be rebuilt. Authored clip edits belong
in project rows.
