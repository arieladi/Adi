# Authored sample settings in the native library

`library::Index` stores authored audio-sample defaults in its existing persistent
`data/library.sqlite` annotations, keyed by the source file's BLAKE3. No source
path, mounted volume or file-index row is required. These are user data; never
remove the library database as decoding/analysis-cache cleanup. There is no
project schema, opcode or library database-version migration.

`sampleSettings(hash)` returns owned data or Missing/Realtime/Invalid/Unsupported/
Resource/Database. `saveSampleSettings(hash, settings, error)` and
`clearSampleSettings(hash, error)` operate on the one hash's authored defaults.
They preserve tags, BPM, rating and unrelated annotation fields. Saving an
identical object performs no persistent update. Save, clear and metadata import
use immediate SQLite transactions, including read/merge/write, so another
connection's metadata changes cannot silently disappear. Native write failure
rolls back; the caller may retry the same settings. Explicit clear removes only
the defaults property, including a version this reader cannot edit.

The typed record contains original rate/channels/frames, the chosen source frame
window, Warp enable/mode and optional raw AWRP, transpose/formant/gain, channel
mode/reverse, clip mute, loop/content offsets, source-end pins, musical and
nanosecond fade clocks/curves/tensions, and optional time signature. Source
window/order/type/range validation precedes storage. Native AWRP validates its
coverage against the source window. The exact original bytes are retained,
including readable future record tails, versions and trailers; saving never
rebuilds a user's marker map from its known prefix fields. An unsupported
existing sample-settings version/shape cannot be overwritten by a current save.
Tag updates and export preserve that opaque authored object.

Placement, project identities, selection and device automation are not portable
sample defaults. This storage component does not capture/apply project clips,
modify their history, render audio or implement missing renderer controls. A
native Save/Recall/import consumer must explicitly bind these values to an
actual source of matching identity/dimensions and use normal native operations
for musical changes. That integration is the next engine item; this record is
not a claim that a user-facing Save button is wired.

Each annotation remains a JSON object with tags/BPM/rating and an optional
`sample_settings` object. Its current codec version is 1, with 34 exact fields;
integers are not coerced from floats/strings, doubles must be finite, optional
clocks use null and authored AWRP uses lowercase hex. The raw map is bounded to
32 MiB; the complete annotation is at most twice that plus 64 KiB. Excessive
existing data refuses editing instead of being dropped. The original library
SQLite application id and version remain unchanged; older tag writers already
preserve unknown annotation fields when merging existing rows.

Metadata export uses the existing `{version, media}` envelope: version 1 for
legacy-only metadata, version 2 when authored defaults are present. Import reads
both versions; a sample-settings property requires version 2. JSON is bounded
to 256 MiB, depth 16 and 16 million SAX events, with 256-byte keys and bounded
strings. Duplicate keys, including escaped spellings, refuse in the preflight
before ordinary DOM parsing. A bad later row rolls back every earlier change.
Unknown/future authored sample settings refuse replacement; users can still
export their stored object and merge unrelated legacy metadata. Export above
the document limit fails without modifying the database.

New settings and exchange entry points are owner-worker operations. Callback
entry refuses before allocation/free/SQLite/I/O/locking or caller diagnostic
mutation. Library lifecycle/indexing operations retain their existing separate
owner-worker contract. Tests use isolated native databases; no user library is
modified during verification. Actual compiler, mutation, full-suite and sanitizer
evidence is recorded in the engine log after the gate.
