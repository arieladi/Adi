# Main Clip Properties (N33)

The window submits these undoable snapshot-impact edits, then refreshes its
project and Session. N32's `engine::readClipDetails` reads the resulting values;
that separate reader is compatible with this format and is not a build dependency.

| Op | Payload |
|---|---|
| `clip.setStartMarker` | `{id,ticks}` |
| `clip.setEndMarker` | `{id,ticks}` |
| `clip.setLoop` | `{id,on,start?,length?}` |
| `clip.setTimeSignature` | `{id,num?,den?}` |
| `clip.setScale` | `{id,root?,scale?,mask?}` |

Source markers and the loop window use musical ticks, including on a linear
clip; arrangement position/length retain their original units and values.
Supported tick values are 0..2^50. Start precedes End. Changing Start pins the
old default End first, so it does not move End. With no stored end marker, End
derives from content offset plus visible duration, converted through the step
tempo map for a linear clip. Unsupported tempo-ramp derivation is refused.

While Loop is off, audio ends at the source End boundary and MIDI releases a
crossing held note there. Notes before Start are chased when they overlap it;
later notes beyond End are not played. The timeline placement remains fixed.
While Loop is on, its own source window controls repeats and End is inactive.
An enabled loop needs a nonnegative start and positive length. Disable can be
used to stop a malformed imported loop without replacing its window first.
Existing audio source-window bounds and named playback problems still apply.

Signature takes both numerator (1..255) and power-of-two denominator (1..128),
or neither to inherit the project context. Scale takes all of root (0..11), a
nonempty id and root-relative 12-bit mask including the tonic, or none to
inherit. These local Live scales are 12-TET; inherited project tunings remain
available through N32. Selecting signature/scale changes metadata, without
moving existing notes or implicitly applying Fit to Scale.

Schema 1.11 adds optional `clip_properties` after schema 1.10's marker details.
NULL means inherited/default; empty rows are permitted. Stored local values are
also in `rows::Clip` and the text projection. New edits capture only the clip
row and optional property row, update in place for undo, preserve unknown
columns and avoid cascading other clip content. Batch failure rolls back rows
and journal entries together.

`clip.delete` captures the full clip, native event streams, note expressions,
audio attachment, clip envelopes, property row, and external aliases whose
foreign keys would clear. Undo restores their ids, bytes, types and extension
columns, refuses collisions, and remains replayable after reopen. It does not
claim to capture arbitrary unknown child tables. Older minimal captured create
payloads remain accepted. No callback allocates, locks or reads the store.
