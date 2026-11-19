# A note's active flag (N34)

The window calls the existing documented op
`note.setMute {clip,note,muted}` with `muted = !active`. `note` is the exact
uint64 id read by N32's `readClipDetails`, not the note's display order.
After commit or undo/redo refresh the Session and the Clip View read model.
Inactive notes remain in the store and in N32's note list; playback omits them.
Deactivating a held note releases it when the new playback generation arrives.

This is a snapshot-impact edit with symmetric undo of the prior muted bit.
The clip must be MIDI and its canonical ANOT stream must already exist.
Unassigned zero, absent and duplicate target ids are refused, as are unreadable
streams and unsupported time/unit flags. No missing stream is manufactured.

Only `NoteFlags::Muted` (bit0) changes, in the original record stride. Every
other flag and field, future record tail, original header/trailing byte,
stream row id/extension and other stream is retained. No sorting or ordinary
NoteRecord-vector re-encoding occurs. All database work stays on the owner
thread in the journal transaction; this adds no schema or audio-thread work.
