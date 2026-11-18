# Atomic navigation to a retained history point

`History::moveTo(optionalSeq, nowUtc = 0, StoredRestore::Refuse)` restores a
retained whole-edit boundary in one SQLite transaction. The window can submit
`TimelineStep.step.lastSeq` and refresh once from the resulting applied batch.
No window binding is included here. It replaces a loop of individual Undo/Redo
calls with the existing common-ancestor inverse/forward path. No op or schema is
added and the journal's original payloads/inverses remain literal.

Targets must exist, be non-ephemeral, and end their whole original transaction.
A structural `history.root` needs its actual stored-baseline metadata. The current
head must also be a valid boundary; a corrupt interior head refuses before any
rewind. NULL means the original pre-edit state. Once compaction materializes an
origin root, NULL canonicalizes to that retained address; it never writes a null
head below a checkpoint. An unavailable original state refuses.

The source's ordinary owner/navigation admission applies for the entire call,
including no-op targets. Pending producer input, an active gesture/source change,
read-only or callback access and reentrant applied-event mutation refuse. With
admission satisfied, an unchanged target writes no SQL rows and publishes no
batch. A late handler, inverse, head-write or SQL failure rolls all document,
branch and head changes back; `opsApplied` returns zero and no receipt escapes.

A successful same-baseline move emits one `AppliedBatchKind::HistoryMove` after
COMMIT: inverses newest first, then original forwards oldest first. It counts all
executed operations in `Result.opsApplied`; `txnId` is zero because several
original transactions can be traversed. It does not append a synthetic journal
transaction. Existing ParamOps consumes the owned receipt on the ordinary owner
drain, in FIFO order; source notification itself only enqueues it. Live device
state loads are tested on the same actual attached instance, without a Session
reload or journal echo.

Leaving a line keeps its future tip, including work already undone at the current
head. If the destination's existing redo path keeps the same tip, or a noncurrent
branch already holds it, no branch is added. Otherwise one system branch named
`before history move` holds the tip. `created_utc` is the caller's `nowUtc`; zero
is allowed when none was supplied. No ambient clock is read. Repeated navigation
does not duplicate already held tips. Ordinary Undo/Redo and timeline reads keep
their existing highest-child redo rule.

Different stored baselines retain HI02's runtime boundary. The default returns
`sessionReloadRequired` and refuses before changing SQL. The document owner must
stop/fence its old Session, explicitly call with
`StoredRestore::SessionReloadPrepared`, and reload the document/Session before
resuming audio. Success emits one empty `ReplayBaseReset`, invalidating old
ParamOps delivery, and restores only the target baseline and retained ancestry.
This API does not fabricate incremental operations for a full-state transition.

Tests cover whole compound steps, exact inverse/forward order, one transaction
and post-COMMIT receipt, no-op, cross-branch/common-ancestor moves, preserved
unheld and undone tips, real late SQL rollback, missing inverses, invalid targets
and current heads, ephemeral sequences, readonly/callback/reentrant refusal,
producer reservations, stored-root/origin transitions and actual runtime state
delivery. Live UI/measurement parity remains the window/measurement work.
