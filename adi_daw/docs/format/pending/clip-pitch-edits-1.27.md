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
stored finite gain. An empty edit set with automatic note geometry has no row.

The ordinary CBOR record is the array of edit rows. Authored geometry wraps
that array under `rows` alongside `segmentation` with `nextId` and `segments`.
Each segment has a stable ID and a half-open range in the original source
clock. The detector's measurements rebuild the authored segments' contours.
An authored span may include unvoiced audio when it retains at least one
measured voiced point.
An explicitly empty geometry retains its row and suppresses the analyzed
segments while retaining the source signal for playback.

Clipboard copies and split pieces retain the complete source-bound edit set
under their own clip identities. Each piece uses its usual playback window and
offset after applying the edits. Deleting a clip captures the row for Undo.
Detaching or replacing a source leaves the row dormant and preserves it for
Undo. Only an attachment and analysis matching the recorded source identity
and note boundaries can use the row for rendering. Project digests and history
checkpoints capture the table automatically.

`clip_pitch_extractions` marks a MIDI clip created by pitch extraction. The
row is owned by the output clip and remains valid after the audio source is
deleted or replaced. Extraction to an existing track uses these rows to
find earlier main-lane outputs. Undo restores the removed clips and markers
together. Manual copies are independent MIDI clips.

To activate, fold the DDL into `schema.sql` after the reserved predecessors.
Freeze the actual predecessor, add the new tables to `Store::migrationSteps`,
and update the schema minor, `SPEC.md` and schema validator version together.
Check fresh creation, migration, read-only older files and replay with the
native format gates. Add both tables to the text projection coverage manifest
when the native schema includes them. The staged fixtures already check the
clip-owned projection, including dormant edits and the pre-pitch read-only
case. [Projection contract](../TEXT-PROJECTION.md#81-authored-audio-pitch)
