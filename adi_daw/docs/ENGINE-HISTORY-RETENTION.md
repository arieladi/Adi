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
removedOps and protectedHistory. It triggers above a hard limit and removes
whole oldest transactions towards 90% low water (at least one), preserving the
newest whole transaction. Upload acknowledgement and legacy embedded media may
leave history above budget. Daily automatic snapshots never block compaction;
pruned automatic points and their daily markers are removed together.

Named snapshots and dead ends survive beyond the undo window as verified,
row-deduplicated states. Schema 1.20 stores typed rows and manifests by exact
CBOR BLAKE3 identity; schema 1.21 maps internal structural roots to manifests.
An old boundary can become a `history.root` hash reference; a named original
origin may need a synthetic metadata sequence. Roots are hidden from Undo/Redo,
`recent` and timeline steps. Their stored branch/snapshot DTOs expose
`storedStateHash`; existing IDs and names remain stable. An unheld redo/dead-end
tip gets a system branch before compaction removes its old bridge. Neither
named points nor forks pin the stepwise prefix. The active budget excludes all
metadata roots, state objects, content/media pools and SQLite overhead.

Capture temporarily moves project state under a savepoint and rolls back. All
required states are captured and actually restored/validated before any graph
rewrite or pruning. A recoverable failure rolls back only retention; normal edit
commit succeeds with `CommitResult::retentionWarning`. Explicit compaction rolls
back its entire metadata transaction. No partial graph or hash objects escape.
All work stays on the document owner, never on the audio callback.

Same-baseline navigation keeps existing inverse/forward applied batches.
Cross-baseline navigation requires a full runtime reload: default
`revertToSnapshot`/`switchToBranch` returns a failure with
`sessionReloadRequired`, before changing state. Window must stop/fence the live
Session, call with `History::StoredRestore::SessionReloadPrepared`, then reload
the Document/Session before audio resumes. Successful transition publishes an
empty `ReplayBaseReset`, invalidating old ParamOps; that event alone does not
rehydrate plugins. UI/session metadata is preserved. The immutable state-blob pool
is merged with collision checks, keeping resources from other branches available.

Replay consumers hold a source read transaction across baseline and retained-log
reads. `restoreReplayBase(source, emptyJournalTarget)` installs the current line's
verified baseline, preserves target UI metadata and returns `retainedSeqs` in
oldest-first order. Replay exactly that list, rather than a numeric seq suffix or
all recent ops. It excludes independent roots, inactive branches and the redo
tail beyond the current head. Target plugin resources include retained-log blobs.
A nonempty/read-only target or corrupt/incompatible content fails atomically.
`restoreStoredSnapshot(source, snapshotId, emptyTarget)` opens an archived named
point without changing the source, including a read-only source. It installs the
verified stored root and needs no retained replay; a snapshot still in stepwise
history uses ordinary project copy/navigation and is refused by this helper.

Save As/Copy, Collect and Export visit all distinct stored root states, including
media absent from the current pool and retained ops. Writes refresh row hashes,
root metadata payloads and the current legacy compatibility image together;
unchanged states share immutable content. Older read-only files retain their
legacy checkpoint path. The legacy full-state compatibility envelope is marked
with schema 1.21 after normalization so older readers refuse newer semantics.
