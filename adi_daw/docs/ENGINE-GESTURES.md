# Gesture preview and commit (HI-01 / N44)

ADR-0216 gives window, surface and plug-in producers one lifetime: begin,
update with absolute input, then commit once or cancel. `adi::GestureSession`
is the public alias for `adi::gesture::Session`; it is distinct from the
playback `adi::engine::Session`. The API lives in `adi/gesture_session.hpp`.
The concrete playback adapter lives in `adi/gesture_presentation.hpp` and the
existing-operation producers in `adi/gesture_producers.hpp`.

## Window and control owners

Create an `EnginePresentation` from the live `ProjectView` and playback Session,
then attach a `GestureCoordinator` to the Store. Begin with a fixed `Spec`
(strategy, actor, label and optional selection) and an owned producer. Check
every returned status. Each update transfers an owned absolute input. Keep the
returned gesture until commit or completed cancellation; do not commit the
preview's requests separately.

The origin is captured once. An update derives requests from that origin, the
latest absolute input and the current committed base. A preview is never the
next update's origin. Source identity, journal head, complete typed rows and
selected sets bind the evaluation. A foreign commit causes owner-thread
reapplication on its new base; a conflicting target suspends the preview with
a reason. Source notifications enqueue work only. Call `drain(nowMs)` on the
coordinator outside notification dispatch, using the existing owner clock.
EnginePresentation also contributes a complete typed presentation-material and
schema image guard. It is checked after heavy preparation before publication
and inside the real commit transaction, including unrelated same-head changes.

Commit runs the actual handlers once in one `OpJournal` transaction, with one
frozen label and inverses captured from the committed state being replaced.
A validated semantic no-op creates no transaction. `CommitOutcome::durable`
records SQL success separately from presentation status. A later runtime
failure must never cause the already committed requests to be submitted again.
Cancel clears only this owner's preview, preserves newer committed edits and
writes no operations or state blobs. A refused cancel remains `Cancelling`;
`retryCancel` advances restoration and cannot reinstate the cancelled intent.

An abandoned public handle requests cancellation. The source coordinator and
playback runtime retain unfinished restoration and its diagnostic receipt;
they do not keep SQLite alive. ProjectView and Session endpoints are revocable,
and device claims bind actual instances, not merely numeric IDs. A detached
endpoint is an explicit presentation outcome.

## Native preview strategies

Live values cover track volume, pan and split pan, existing input gain,
send gain/pan and existing summing drive. Activation and solo producers capture
clicked/selected tracks and explicit exclusive/additive policy. Repeated latch
updates toggle from the captured origin. Main is excluded by the existing
track-switch rules. Native controls and ProjectView follow the preview before
commit. Scalar updates preserve automation overrides; cancel restores the
committed automation behavior, while committed manual edits retain the normal
Re-Enable Automation semantics.

Structural evaluation runs the real operation handlers in a scratch transaction.
Capture copies its snapshot, model, clip data and settings, then the transaction
is explicitly rolled back. Only after successful rollback may readers, source
workers and graph preparation run. Publication admits the complete owned
generation again. Reused devices must retain matching format and shape and
are not prepared a second time. Each graph retains its owned control and
automation bindings; the active generation installs its controls before each
processing block. Retired ownership is collected off the callback. The first
committed-base adoption may prepare a baseline
generation; subsequent scalar value changes use controls.

Concrete structural producers cover `clip.setLoop`, an existing tempo event,
an explicitly absent tempo-event position and explicit moves of existing
automation points. Tempo updates preserve the event's ID, position, curve and
tension while changing BPM. The caller supplies the captured event position
for a Tap Tempo run. Automation edits address the captured point-ID set and
delegate to the existing codec and handler. They are explicit edits, not an
AU-03 recording pass.

Unsupported topology/format changes, frozen structural material and controls
without a native consumer return a named refusal. In particular this API does
not add width DSP or introduce a missing input-gain control during preview.
Disjoint scalar owners can coexist; a structural preview and another active
scalar preview currently refuse composition rather than publish a partial view.
Placement and note-editor clients must use their respective planner/owned-diff
producers when integrating; these adapters do not duplicate those kernels.

## Plug-in capture and the existing document path

Session-bound `ParamOps` is a client of the same coordinator. Its callback rings,
state signals and UI producer retain accepted input through submission failure.
Begin reserves the source before input can become visible. Sources without an
End use the existing quiet detector to finish a gesture; the quiet interval
does not merge committed transactions or perform Undo and recommit.

`drain` and `uiEvent` offer at most one completed gesture in an output vector.
Completed gestures keep FIFO order; an unfinished gesture on a disjoint scalar
endpoint may stay open while another endpoint completes. An earlier completed
offer or refusal still blocks later completed input, and overlapping targets
wait. The existing document sequence
(`writeBlobs`, then ordinary `OpJournal::commit` through its submitter) remains
usable: session-bound `writeBlobs` validates and stages resources with zero SQL
writes. Each offered request carries a private, transient
`capturedTransaction`. The journal admits the exact whole vector and installs
its blobs inside the same guarded transaction as inverse/apply. Partial,
mixed, altered, superseded or already durable offers refuse. The capability is
absent from payloads, saved history and History replay. Consistent actor stamps
from the existing submitter remain allowed. Sealed offers refuse a
`CommitOptions::beforeCommit` hook; ordinary unsealed commits retain their
existing hook behavior.

Inspect `pendingGestures()` after a refusal. Use `commitPending` or
`retryPendingCommit` for an explicit retry, `cancelPending` or
`retryPendingCancel` for restoration, and `pumpGestures` for owner progress.
An unsubmitted offer may be offered again; a refused submission waits for an
explicit decision. Newer accepted input cannot be overwritten by an older
gesture's cancellation or receipt. State bytes and first-touch openers stay
owned until the outcome is known. Standalone ParamOps retains its earlier
unsealed capture contract.

Capture is settled before older N47 receipts can overwrite newly accepted
runtime input. Receipt replay remains ordered and refuses unsupported or lost
instance/source continuity. A state epoch alone cannot prove prior opaque
bytes: ambiguous or concurrent unreported mutation exposes a capture fault or
reconciliation requirement. Actual host state loads mute their own signals.
Callback bindings own their sink, device identity and source fence throughout
an announced callback; detach retires them on the owner side. Callback paths
do not allocate, lock, access SQL or wait for owner progress.

## Navigation and source replacement

Open gestures, undrained callback reservations and unresolved capture faults
refuse Undo, Redo, revert and branch/material switches, including same-head
shortcuts. HI-03's future moveTo API must use this same admission fence.
Ordinary foreign commits remain available and trigger
rebase. Snapshot naming/renaming/removal and retention maintenance that leave
the current material/source intact remain available.

Explicit close and Save As use the same source admission. Input accepted while
a copy is being made prevents final source replacement: the copy may be saved,
but the original project stays active and the result says so. A successful
replacement finalizes the old source atomically and physically closes its old
database. Forced Store destruction invalidates the source without waiting for
a gesture. Runtime restoration retains no Store pointer. Clean N47 consumers
may follow the proven immediate Save As replacement; a gap or lost capture
requires explicit recovery.

[OPS §6.4](OPS.md#64-gestures-preview-then-one-committed-action) records
ADR-0216's gesture contract and retires the old time-window merge. This API adds
no automation recording, opcode, project schema or ANOT layout. The tempo
handler's backward-compatible optional fields are documented in OPS §9.1.
M-LIVE-UNDO remains NOT RUN until the measurement seat records Live 12.4's
interaction labels; native tests do not substitute for that reading.
