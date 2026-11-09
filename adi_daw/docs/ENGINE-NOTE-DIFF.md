# Captured note differences (MI-01)

`adi/note_diff.hpp` provides one captured edit of a MIDI clip's notes. Capture
on the Store owner, prepare from owned immutable values, then commit once on
the owner. The result is one `note.applyDiff` request, one stream write and one
Undo step, irrespective of the number of affected notes. Several clip requests
may be submitted together through the existing journal/document batching path.

This is the note mutation kernel. MI-02's overlap resolver, gesture lifetime,
recording/overdub producers and expression-curve editing remain separate work.
Nothing reads selection, grid, playhead or settings implicitly. These helpers
allocate; Store access and all preparation stay outside the audio callback.

## Capture and prepare

`captureNotes(store, clipId, optionalCreationId)` reads the canonical notes row,
all its complete note-expression rows and the journal head in one coherent
read transaction. It reuses a caller transaction when present. `Snapshot` owns
immutable bytes and typed rows; its views remain valid while that value lives,
including after the Store is closed. Copies share const capture storage.

An existing canonical row keeps its exact signed SQL identity; a supplied ID
must agree. A missing row remains distinct from an empty ANOT. Reading an
absent snapshot needs no creation ID, but adding a first note requires the
caller to provide an unused literal stream ID. Capture then evaluates defaults
once in an isolated copy of the table and owns the complete resulting row.
Unknown defaults are never reevaluated during apply or Undo. Unsupported table
extensions refuse capture explicitly.

`Record::bytes` is the sole record representation. `values` decodes the known
prefix, `withValues` validates changes and retains the exact opaque tail, and
`makeRecord` creates a native record or a wider one with a caller-owned tail.
Fields are checked before narrowing; signed negative starts and nonzero uint64
note IDs across their full range are supported. Note ID is not a row index.
The Clip View read model also retains valid negative note origins; it does not
discard the note list merely because a record begins before relative tick zero.
Native playback admits these starts within its existing bounded tick domain:
a note crossing the visible source boundary is chased with its original end,
while a note ending at or before that boundary remains silent. The playback
bound is still narrower than the checked storage representation.

A `Diff` contains added full records, removed old full records, and changed
old/new pairs. A changed pair keeps its identity. Use a changed pair, rather
than remove-plus-add, to edit an existing identity. Duplicate or conflicting
entries refuse. Removed records capture every expression dimension, including
unknown/plugin dimensions; old and new streams retain unknown headers, tails
and trailers within the supported musical-stream admission.

`prepareDiff(snapshot, diff)` is pure: no Store, SQL, file, default evaluation
or persistent-ID allocation. It validates the entire diff, derives complete
final bytes and returns a prepared value with source-file/head guards. An empty
effective diff produces no requests. Its `effects` are literal validated
changes suitable for a preview; they cease to describe the operation if a
caller subsequently changes its requests.

For a native-width addition, the call sequence is:

```cpp
auto snapshot = adi::notes::captureNotes(store, clipId, streamId);
if (!snapshot.ok) throw std::runtime_error(snapshot.error);

adi::notes::NoteValues note;
note.id = callerAllocatedNoteId;
note.start = startTicks;
note.duration = durationTicks;
note.key = 60;
note.velocity = 100;
note.releaseVelocity = 64;
std::string error;
auto record = adi::notes::makeRecord(note, {}, error);
if (!record) throw std::runtime_error(error);

adi::notes::Diff diff;
diff.added.push_back(*record);
auto prepared = adi::notes::prepareDiff(snapshot, diff);
if (!prepared.ok) throw std::runtime_error(prepared.error);
auto committed = adi::notes::commitDiff(store, prepared);
```

The caller handles a failed commit through `committed.ok` and `committed.error`.
A future-width stream needs an explicitly supplied full tail for additions; this
native-width
example deliberately cannot invent it. The producer chooses the stream and
note identities; the new operation does not allocate them during replay.

## Commit, staleness and exact inverse

`commitDiff` checks the source-file identity and passes the captured head,
including an actual null head, to `OpJournal`. A document caller may instead
submit the owned requests and options through its normal commit path. Keep its
document-generation guard when returning work from another thread.

The forward handler independently checks the canonical row ID and BLAKE3 of
the entire original stream. Full old record bytes and complete affected
expression sets are preconditions too, including an expected empty set. These
guards catch direct SQL or another WAL connection changing the material without
advancing the journal head. No stale diff is silently rebased.

Apply updates the existing stream once, or inserts one literal captured row
for an absent stream. It removes expression rows for removed notes in the same
transaction. Moving, resizing or changing velocity retains their note-relative
expression bytes; this kernel does not crop or stretch curves. Unrelated and
orphan expression identities are not repaired incidentally.

Captured inverse packets restore complete typed rows, exact original ordering,
flags and opaque bytes, including whether the stream existed. Their fixed
stream/expression slots are validated against their clip, identities and current
rows before mutation. They cannot become a general arbitrary-table row editor.
`note_capture` is an inverse-only packet, rejected by forward submission schema.
Expected row guards distinguish SQLite storage classes, including INTEGER from
REAL. Actual rows are checked again after applying the delta; affinity coercion
or trigger changes roll back with the journal transaction before an inconsistent
inverse can be recorded. The actual row-change count must equal the planned
delta too, so hidden extra writes cannot satisfy the one-write contract merely
by leaving the same final bytes.

The five historical P0 op names use this kernel while retaining their current
inverse dispatch names. Insert/delete remain paired. Move and resize capture
exact inverse state; velocity keeps its scalar inverse and coalescable behavior.
Valid old scalar inverses still replay. Earlier history that never captured
lost flags cannot retroactively recover them; new deletion inverses do capture
them and all expression rows. Existing `note.restoreNotes` keeps its original
same-identity-set restriction.

History passes borrowed original-operation and inverse bytes to the handler,
with replay direction. Verified old scalar insert/delete replay preserves the
expression rows its old inverse depends on, including repeated Undo/Redo.
Historical first creation follows SQLite's old MAX+1 behavior even for negative
stream IDs; an exhausted positive maximum refuses because the old random
allocation was never recorded. New captured Redo restores the literal
recorded stream identity and defaults and verifies the original forward intent
against the capture. Unrelated row-ID pressure cannot silently change that
identity; a contradictory forward payload refuses.

The full wire grammar, numeric ranges, future-layout boundaries and native-only
legacy first-creation compatibility are specified in [OPS §9.5](OPS.md#95-notes-and-expression).
