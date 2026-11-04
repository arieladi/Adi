# Daily snapshots and snapshot budgets

DailyHistorySnapshots is one document-owner service per open writable project.
Construct it before accepting edits, then call poll(nowUtcMicros,
utcOffsetMinutes) after a completed edit gesture, undo or redo and before close.
Create a fresh service when New/Open/Save As replaces the Store. The caller
supplies time and local offset; there is no clock read or autosave timer here.
Every edit is still committed immediately through the native journal.

Opening an unchanged project creates nothing. On the first observed change of
a local calendar day, the service creates one automatic snapshot; subsequent
changes refresh that day's automatic point to the latest head. Its name follows
the project and supplied time. A persistent date-to-ID marker avoids duplicate
automatic entries after reopen or a timezone/clock move. Undo and redo count as
project changes; view-only state does not. Poll returns Unchanged, Created,
Updated, Blocked or Failed, with the snapshot ID/name or error. A failed/blocked
write keeps pending work for retry after the cause is resolved.

Renaming an automatic snapshot makes it named and pins it. A later daily update
creates a new automatic point and preserves the promoted name. The automatic
point follows the day's work; use Take Snapshot or rename to retain a particular
intermediate point. Snapshot metadata never appends an undo operation.

History::takeSnapshot now enforces the same budget as the daily service:
10,000 persistent snapshots locally, 100,000 in collaboration mode. Pass
HistorySnapshotPolicy for collaboration or an optional smaller project budget;
values above the appropriate maximum are refused. At the limit the oldest
automatic point is removed, ordered by creation time then ID. Named points are
never removed automatically. If named points fill the budget, creation reports
Blocked/failure. History::removeSnapshot is the explicit user action for freeing
a slot; it is metadata, like rename, and does not create an undo step.

Retention and insertion/update form one SQLite transaction. If the new write
fails, evicted automatic points and daily markers return with the rollback.
All helpers refuse read-only writes. No schema or new opcode is needed: the
existing history_snapshots and adi_meta tables hold this metadata. The date keys
are history.daily.<local-day-number>; UI/app settings remain outside the file.

This increment bounds persistent snapshot metadata. Active undo's separate
2,000-transaction / 1,000 MB compaction policy remains the next increment. It must
preserve named points, reachable branches and unuploaded collaboration ops and
record a replayable baseline before removing any log prefix. Save As/Copy and
Collect are the separate N3/N30 APIs; this service adds no file-save command.
