# Snapshot remarks (N27)

Snapshot.remarks is a vector of shared const RemarkNode values, sorted by
persistent id rather than display order. Each keeps id, targetKind, targetId,
optional paramId, author, actorDetail, text, createdUtc and resolved exactly as
stored. Nullable paramId remains distinct from an empty string. The reader does
not derive author provenance from the op actor or query a live plug-in.

trackId resolves the anchor's owning track once per rebuild. Clip anchors follow
clips.track_id; devices follow their chain and nested rack parents. A parameter
need not have a plugin_params mirror row before its first touch (ADR-0057).
anchorMissing is true when the anchor or its owning track/container cannot be
resolved. These remarks remain in the vector, including dangling and cyclic
anchors, with all metadata intact. A hierarchy change alone can change the
resolved owner and invalidate that remark's shared node. Every raw and derived
field participates in equality; unchanged remarks share their previous nodes.

The window can filter trackId for a row's remark pip, or use the raw target
address for the selected object/parameter in Info View. Orphans can remain at
the project level. resolved is the user's annotation state, independent of
anchorMissing. Existing remark.add/edit/resolve/remove and undo/redo already
supply edits; this branch changes no schema or opcode and no UI production code.

SnapshotBuilder reads one coherent SQLite state using a deferred read transaction
when the caller has none. It borrows an existing read/write transaction without
nesting, committing or rolling it back. Thus a concurrent WAL writer cannot
produce text from one edit and an owner from another. The read transaction ends
before publication; no DB handle, lock, query or ownership lookup reaches the
audio callback. With no remarks the builder avoids reading device ownership.
