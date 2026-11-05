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

The following compaction increment adds `UndoRetentionPolicy` (2,000
transactions / 1,000 decimal MB by default, optionally lower),
`History::setUndoRetentionPolicy` and `compactUndo`. Setting policy compacts in
the same metadata transaction; normal journal commits enforce the stored policy
before the final commit hook. Enable collaboration before receiving its edits,
and set `uploadedThroughSeq` only after every local seq through that point has
been uploaded. Missing acknowledgement protects the whole log. A new
collaboration session must transfer any existing checkpoint with its log.

Compaction reports remaining transaction/byte counts, retained baseSeq,
removedOps and protectedHistory. Named snapshots, other existing snapshots,
branch heads and forks can keep history above budget. The newest whole
transaction and legacy embedded-media rows are also protected. Nothing deletes
these protections to meet a numeric limit. Daily automatic points remain usable
until the snapshot-budget/user removal path removes them.

Schema 1.19's single history_checkpoint row contains typed project state at the
new root. Taking it temporarily rewinds under a SQLite savepoint, captures state,
then rolls back that savepoint; only the checkpoint and old prefix pruning are
committed. Keep all of this on the document owner, never on the audio callback.
Checkpoint building can be proportional to project content and retained history.
The byte limit is active journal storage, not total file size or content pools;
see format/SPEC §8.3 for the exact accounting and retained-root contract.

Replay consumers must hold a source read transaction while reading base and log.
`History::restoreReplayBase(source, emptyJournalTarget)` preserves target UI
metadata and returns baseSeq; replay source ops strictly after that seq. Domain
rows include binary content and retain their SQL types. Source media-file paths
still use the usual resolver; this is not Collect/Export. Failed restore rolls
back all target writes. A nonempty/read-only target is refused. History's undo
menu stops at the retained root, while redo and protected snapshot/branch
navigation retain their existing semantics. The retained root must likewise be
excluded from the window's list of undoable steps (N29 integration).

Save As/Copy and Collect remain the separate N3/N30 APIs. Neither service adds
a file-save command, changes UI code, nor appends snapshot/compaction opcodes.
