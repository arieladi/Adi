# Clip View read model (N32)

On the store's owner/message thread call
`engine::readClipDetails(store, clipId)` after a committed edit, undo/redo or
history navigation. Read outside a journal mutation transaction. The result
owns its values and keeps a consistent database read view; it has no store or
stream handles. A missing clip or SQL failure returns `error` and no partial
clip. Only the selected clip's note stream is read.

The returned `clip` includes kind/name/optional palette colour, track/lane/alias
ids, original tick/ns placement and length, mute/gain, fades and their curves,
content offset, loop start/length and enabled switch. NULL stays distinct from
zero. `startMarkerTicks` is the stored content offset. Until a local end marker
exists, `endMarkerTicks` derives from offset plus visible length; a linear clip's
length is converted through the step tempo map. Unrepresentable derived times
and unsupported tempo ramps are named in `propertiesProblem`, while raw stored
positions remain available.

Signature and scale inherit at the arrangement start, defaulting to 4/4 and
C major when no earlier project event exists. Native project tuning id/degrees
are retained. The reader also understands the additive `clip_properties` table
of N33 when present: local end marker, signature and 12-TET scale overrides have
explicit `local` flags. Absence/NULL means inheritance. This PR adds no table or
write op; N33 supplies local property edits and playback integration.

`notes` includes inactive notes, with `id` equal to the original ANOT `note_id`:
use `{clip: clipId, note: note.id}` for the note ops. Display order is not
identity and can change after an edit. Every note exposes start/duration ticks,
key, attack/release velocity, channel, all flags, probability, tuning and active
(`NoteFlags::Muted` clear). Times are source-relative. Alias metadata and this
clip's own stream are reported as stored; the reader does not invent alias
playback or silently address another clip.

An invalid stream or note is named in `notesProblem` with no partial note list;
other clip properties remain readable. Legacy zero/duplicate ids remain exact
and produce a problem alongside their geometry: the window must refuse an
ambiguous edit, not substitute the row index. Known prefixes of wider future
records can be drawn; `notesRecordSize` and `encodedNotes` retain their version
extent and original bytes. No op, history move or playback work is triggered.
