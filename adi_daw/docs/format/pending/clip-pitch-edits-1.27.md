# Clip pitch edits, staged project schema

Win reserved minor 1.27 and `clip.setPitchEdits` for the pitch seat. The strict
table in [clip-pitch-edits-1.27.sql](clip-pitch-edits-1.27.sql) is exercised
in disposable project fixtures. Current project creation and migration still use
1.23. The predecessor minors must land in order, or win can assign the next
minor in the reservation table. [Reservations](../../../collab/RESERVATIONS.md)

`clip_pitch_edits` has one row per edited clip. The row binds the complete
authored edit set to the source content hash, original frame count and sample
rate, and the analysis version and configuration. Edits use the original
source-frame clock. The payload preserves their source note ordinals and
original bounds when the analysis cache evicts its rebuildable data. Each
edit carries its destination bounds and literal control values. The CBOR
encoding is bounded to 16 MiB. Mute has its own boolean and uses zero for the
stored finite gain. Empty edit sets are represented by row absence.

Clipboard copies and split pieces retain the complete source-bound edit set
under their own clip identities. Each piece uses its usual playback window and
offset after applying the edits. Deleting a clip captures the row for Undo.
Detaching or replacing a source leaves the row dormant and preserves it for
Undo. Only an attachment and analysis matching the recorded source identity
and note boundaries can use the row for rendering. Project digests and history
checkpoints capture the table automatically.

To activate, fold the DDL into `schema.sql` after the reserved predecessors.
Freeze the actual predecessor, add the new object to `Store::migrationSteps`,
and update the schema minor, `SPEC.md` and schema validator version together.
Check fresh creation, migration, read-only older files and replay with the
native format gates. Add the table to the text projection coverage manifest
when the native schema includes it and record the projection contract in
`TEXT-PROJECTION.md`.
