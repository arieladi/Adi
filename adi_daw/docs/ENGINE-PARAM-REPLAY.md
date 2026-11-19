# Live parameter replay (N47)

Successful `device.setParam` and `device.loadState` edits now reach existing
plug-in instances through the Store's applied-batch feed. This joins the
historical inverse contract of ADR-0124 d4 to the live parameter owner. It uses
the existing `Session::load` / `refresh`, `ParamOps::attachSession`, and owner
drain calls. It adds no opcode, saved field, schema version or window undo path.

## The committed batch

`adi/applied_batches.hpp` exposes an immutable `AppliedBatch`. It records the
actual handler type and payload, original operation type, sequence, transaction
identity and execution direction. A normal commit preserves request order.
Undo preserves inverse execution order; the dispatched type can be the paired
inverse operation. Redo preserves forward order. Branch switches and snapshot
reverts retain the complete rewind and replay legs in one notice.

The collector runs inside the real mutation transaction, before compaction can
prune operation rows. The complete delivery value is prepared before COMMIT;
publication follows COMMIT. A failed handler, inverse, commit hook, validation
or SQL transaction publishes nothing. Scratch previews and temporary checkpoint
replay publish nothing. A same-head branch metadata switch can publish an empty
batch. Replay-base replacement is a distinct reset notice, with no invented
operation sequence.

Each notice owns every state blob referenced by an actual non-clearing
`device.loadState` packet. It also owns complete typed final parameter rows for
each state-touched device, including an explicit empty set. Device definitions
and final presence are captured for every parameter/state target. Unknown SQL
columns retain their storage classes and values. No notice borrows a statement,
SQLite BLOB or decoder buffer.

## Ordered delivery and reconciliation

The Session retains the Store's revocable connection identity.
`ParamOps::attachSession` subscribes once. Its callback queues the immutable
notice; it calls no plug-in. The ordinary `drain(nowMs)` first secures newly
accepted capture input, then retries pending delivery on the owner thread,
using that clock for the existing echo guards. HI-01's retained gesture owner
keeps newer input audible across older receipts. `uiEvent` uses the same path. A direct
owner may also call `retryApplied(nowMs)` and inspect `appliedDeliveryStatus()`.
A Session without ParamOps has no parameter delivery sink.

The queue preserves transaction order. Each entry has a cursor through the
actual dispatched operations, followed by a separate cursor through the final
parameter projections. A refused parameter or state delivery retains the
failed cursor; its suffix and later batches wait. A retry does not repeat an
acknowledged prefix. Equal values and cleared rows are successful bookkeeping
results, distinct from a missing attachment or a plug-in refusal.

First-touch Undo restores the opening value and then processes the opener's
clear. The clear removes first-touch bookkeeping; it does not invent a default
parameter value. Redo restores row-presence bookkeeping even when the plug-in
already holds an equal value. Consequently the next gesture knows whether it
needs an opener.

Row-presence acknowledgement comes from the successful Store receipt. The
existing public `applied` call remains a runtime-only adapter; a bare payload
does not establish that a parameter row exists in a Store.

An own successful submission needs acknowledgement without replaying an
opener or reloading an already heard preset. ParamOps stamps an opaque transient
producer proof on its complete emitted interval for one device. Session-bound
capture offers one owned gesture per transaction; later accepted input has its
own ordering identity and cannot be overwritten by an older acknowledgment.
Standalone legacy capture may still group multiple completed edits. The journal carries that proof
only on the corresponding normal-commit receipt; it is never part of the
payload or saved history. The parameter owner recognises its private witness
and checks the complete unchanged group, source and attachment before skipping
redundant live delivery. An unknown or altered proof does not authorise a skip.
Undo, Redo and branch/revert receipts always carry no producer proof.

Final parameter reconciliation matters after a state chunk. Suppose a chunk
contains cutoff `.9`, a later parameter row overrides it to `.35`, and a preset
edit changes both to `.6`. Undo dispatches the row inverse `.35`, then the old
chunk `.9`. Replaying only those calls would leave the wrong audible value.
The receipt's final parameter projection restores `.35` after the chunk, just
as reopening the committed project does. This projection belongs to the same
transaction; a delayed retry never substitutes a newer SQL state.

For a backend with pending parameter changes, the effective state hash is
acknowledged after those changes settle, while the state epoch and expected
parameter values still agree. A state signal that arrives before that quiet
observation remains eligible for preset capture. The generic device API cannot
prove that such a signal is an old echo rather than a new opaque preset; replay
does not suppress a possible new preset to make that ambiguous case disappear.

## Source and instance lifetime

`AppliedSource` identifies one live Store connection. It does not keep the Store
or database alive. Closing the Store invalidates it before database teardown;
a successful Save As replaces it even though the Store object's address stays
the same. Failed Save As preserves the existing source. The replacement token
proves its immediate predecessor through `isReplacementOf`; reopening a file,
equal filenames and an unrelated Store do not supply that proof.

A healthy consumer with an empty receipt queue and no open gesture, callback
reservation or capture fault may follow that exact Save As
replacement before any new-domain commit. It keeps the same instances, rings
and capture state while rotating its subscription. If pending work was discarded,
a notification was missed, or an observer failed, refreshing the same Session
does not clear invalidation: a fresh Session must load the new source. Final
SQL rows cannot reconstruct an erased opener's missing audible value. A window
can avoid this recovery path by finishing committed delivery through
`retryApplied` before Save As and handling any refusal before replacing the file.
Refreshing the same Session from an unrelated Store is never this continuation.

Subscriptions are move-only revocation tokens; the bus holds weak observers.
Revoking during delivery suppresses calls that have not started. A subscription
added during delivery starts with the next batch. A throwing observer is faulted
and reported through its token while other observers continue. It cannot turn
a durable SQL success into a failure result. Same-Store journal/history mutation,
close and Save As are refused during delivery; reads and subscription changes
remain available. Arbitrary writes through raw `db()` are outside this contract.

Runtime receipts also bind the Session and actual device attachment lifetime.
A numeric device ID or equal device-definition row alone cannot establish that
a later plug-in instance is the intended target. Existing attachments use their
instance identity. An unrealised target requires a matching realised source
sequence or an explicit invalidation/resynchronisation result. Old receipts
must not silently cross a source or device replacement. Session-bound attachment
lifetime checks do not change the outlive-or-detach rule for standalone device
attachments.

Delivery status distinguishes pending work, a required attachment, a refusal
and invalidation from an empty queue. It describes the live state; it does not
change the result of the SQL transaction that produced the receipt. Missing
notifications or a faulted subscription require explicit recovery rather than
silent continuation across a gap.

## Producer and window boundaries

The existing ProjectDocument timer and parameter input path provide the drain
calls; no window source change is required for successful replay. Direct
Session/ParamOps owners must continue to drain or explicitly retry. Live delivery
is owner work, outside the audio callback.

A first-ever raw parameter packet whose inverse only clears an absent row
contains no previous audible value. Replay cannot reconstruct that value.
Producers that need first-touch Undo must capture the opener and final edit
together, as ParamOps already does, or edit an existing recorded row.

HI-01's [common gesture owner](ENGINE-GESTURES.md) retains session-bound input
when a caller drops an offer or SQL refuses it. Its transient typed capability
binds the exact offered vector and atomic state resources; explicit retry or
cancel remains available after failure. Raw unrelated packets and standalone
legacy capture do not acquire that ownership merely by carrying an N47 proof.
Measured Live interaction parity remains a separate reading.
