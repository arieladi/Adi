# Owner allocation of persistent SQL row IDs

`adi::RowIds` in `adi/row_ids.hpp` is the shared owner-side allocation context.
Create one against the current Store database under the producer's read snapshot,
pass it by reference for the whole prepared batch, and call `next(table)` for each
new row. `reserve(table, id)` includes identities explicitly supplied by an intent
before later rows are allocated. Contexts cannot be copied. They capture one
schema/maximum per table and retain unpublished allocations in memory.

The default rule is the existing positive append rule: start above both zero and
the current native signed integer maximum, honor every explicit reservation, and
refuse at int64MAX before addition. No ID is converted through double. Tables
need one `INTEGER PRIMARY KEY` named `id`; complete/composite keys, other key
names/types, missing tables and embedded NUL refuse. Metadata values are bound
and literal table names are quoted. A quoted real table name remains usable.

`RowIdExhaustion::FirstPositiveGap` retains the existing controller-lane producer's
special exhaustion behavior. Only when the append ceiling is occupied does it
scan the positive keys for the first gap, skipping explicit and unpublished
reservations too. Other producers keep refusing exhaustion. Choosing gap fallback
once does not change a later call's default rule. New IDs always stay positive;
legacy negative-only tables do not generate negative IDs.

Allocation/reservation reads but never writes SQL, journal, settings or media.
It is refused on the audio callback, including a cached namespace. This is not a
durable or concurrent reservation service: use one owner context and snapshot;
existing captured row/absence and journal-head checks admit publication. IDs are
literal in prepared operations and their captured inverses. Undo/Redo does not
consult the allocator to generate replacements. No schema or format version is
introduced; distributed ID spaces remain the later collaboration design.

Current engine producers use this implementation: `clip_edit::CopyIds` (retaining
its existing supported-child checks), built-in ref/chain/device insertion,
sample-slot media import, controller stream creation, MIDI Capture's complete
controller/notes batch, and new note-stream creation. The historical scalar note
replay branch deliberately keeps its old MAX+1 behavior, including negative/zero
IDs, for compatibility with the recorded inverse. Snapshot's max-routing field
is a read-only capture and keeps that role. Full-width identities inside binary
streams remain their separate declared stream namespace.

Window handoff: replace the two local SQL MAX+1 sites in
`ui/project_document.cpp` with this owner API, retaining a context across each
batch. Those window sources remain window-owned and are not edited here. This
engine slice supplies the common implementation; it does not claim that binding
or the future distributed allocator is complete.
