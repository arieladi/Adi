# Optional BWF v2 in WavWriter (EX-08 prerequisite)

`WavMetadata::bwfV2` opts into version 2. Leaving it absent preserves the old
four-field aggregate, v0 chunk bytes, text truncation/raw-byte behavior and iXML
placement. Existing no-metadata writers are unchanged. This is source
compatibility, not a promise of unchanged C++ structure size or binary ABI.

The optional structure supplies originator reference, date/time, opaque 64-byte
UMID, coding history and five independent optional loudness values. Description,
originator and the unsigned 64-bit sample time reference stay in `WavMetadata`.
The caller supplies every value; the writer generates no identifier, clock,
measurement or process-history assertion. Empty text and an absent UMID remain
zero-filled. UMID semantics and originator-reference uniqueness belong to the
caller; this layer does not validate a SMPTE identifier or allocate an R99 USID.

The byte contract follows [EBU Tech 3285 v2, May 2011, §§2.3–2.4,
pp.9–13](https://tech.ebu.ch/docs/tech/tech3285.pdf). The fixed payload is 602
bytes, with variable coding history following it. Version is 2 at offset 346;
UMID occupies 348–411, five little-endian signed centi-unit values start at
412, and bytes 422–601 are zero. Missing loudness values use `0x7fff`.
Supplied values are rounded to nearest with ties away from zero after scaling
by 100. Four inputs permit −99.99 through 99.99; loudness range permits
0 through 99.99. Values use LUFS, LU, dBTP, LUFS and LUFS respectively.

All v2 validation completes before opening/truncating the output:

- ASCII text fits its fixed width and has no embedded NUL; oversized/non-ASCII
  v2 text is refused rather than silently truncated.
- Supplied dates/times have the fixed widths, decimal components and valid
  calendar/time ranges. EBU's permitted separator freedom is retained.
- Nonempty coding history consists of CR/LF-terminated ASCII rows. Its chunk
  size is checked; the RIFF alignment byte is outside that size. This checks
  transport syntax, not whether the stated processing actually happened.
- Supplied loudness is finite and within the stated physical ranges; there is
  no saturation and no conversion of invalid measurements into “unknown”.

[EBU R98, Appendix 1](https://tech.ebu.ch/docs/r/r098.pdf) describes recommended
coding-history row contents. [EBU R99, §2](https://tech.ebu.ch/files/live/sites/tech/files/shared/r/r099.pdf)
describes a recommended reference-generation scheme. Neither generator is
implemented here. Time reference is written exactly as supplied, low word then
high word; timeline-to-sample conversion remains the caller's job.

Metadata is captured by the constructor. Later edits to the caller's structure
do not patch the file. RIFF/RF64 promotion, iXML padding and actual audio samples
continue through the existing writer and reader.

Tests compare the full fixed prefix/history against an independent byte oracle,
including signed EBU examples, half ties, every unknown field, exact-width text,
reserved bytes, both history parities and RF64 promotion. They verify legacy v0
bytes/offsets, unchanged samples, invalid-value refusal without touching an
existing file, and refusal without creating a new file.

This is the writer prerequisite only. EX-08's `Job.metadata`, injected clock and
seeded R99 reference generation, markers/time-map ownership, non-WAV tags and
the post-check metadata update remain deferred. The export hook must follow
EX-05's chosen checked-output statistics and stage order; no current render is
claimed to emit these fields automatically.
