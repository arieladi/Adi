# Undo History read model (N29)

On the message thread call `History(store).readTimeline()` after a commit,
undo/redo, branch switch, or history snapshot metadata change. It is a value
snapshot: an existing window frame keeps its strings and flags after the next
edit. Refresh outside an active journal transaction. A failed read returns an
explicit `error` and no partial history; it never presents a broken table or
parent cycle as an empty valid list.

`steps` is newest journal sequence first across every branch. One row is one
complete undo transaction, with txn id, first/last sequence, op count, the first
edit op's label/actor/detail, timestamp and parent sequence. Wall-clock changes
do not reorder the list. Ephemeral ops are omitted and multi-op transactions
are never split into misleading separate undo actions.

`applied` identifies the current head's ancestors. `onCurrentLine` additionally
includes undone transactions on the highest-child line Ctrl-Y follows. Branch
ids name each stored branch head's ancestry; the current branch also includes
its redo line because its stored head moves backwards on undo. Shared-prefix
steps can belong to several branches. All edit transactions remain visible,
including a redo tail beyond an inactive branch's stored head.

`branches` retains id/name/head/current/creation metadata. `snapshots`, newest
timestamp then id first, retains the named and automatic anchors, including
snapshots at the root before any step. Each step also lists its snapshot ids.
Root snapshots have no step. The root exposes the current head and branch id.

Reading uses one database read transaction for consistent metadata. It does not
read CBOR payloads, inverse data or plug-in state, append ops, or move the head.
Existing `undo`, `redo`, `switchToBranch`, `revertToSnapshot`, `takeSnapshot` and
`renameSnapshot` remain the action APIs. No schema or op vocabulary changes.
