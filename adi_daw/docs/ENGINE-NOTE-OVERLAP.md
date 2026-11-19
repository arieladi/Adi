# Captured note overlap (MI-02)

`adi/note_overlap.hpp` provides pure preparation of a same-key note edit with
its complete overlap effects. Capture notes on the Store owner with
`captureNotes`, prepare from that owned immutable snapshot with
`prepareOverlapDiff`, inspect the preview, then submit once with
`commitOverlapDiff`. The result is zero requests for a semantic no-op or one
versioned `note.applyDiff`, one canonical stream write and one Undo step.
Several clips may use the existing document/journal batch path with their
captured requests and head guard. Store access, preparation and submission are
owner work, outside the audio callback.

The adopted basis is [PLAN MI-02](PLAN.md), ADR-0203 d1/d7, and the repository's
manual-derived A-M03 record in [Ableton 12 research](daws/ableton-12.md).
Version 1 makes the table below explicit. It is a documentary implementation,
not a measured Live case matrix. The current official manual could not be
retrieved in this cloud environment because its enforced network allowlist
rejected ableton.com; no Live application was run. Boundary/channel/curve
parity remains open without blocking this stated rule.

## The version-1 rule

Intervals are positive, checked, signed integer ticks and half-open: `[a,b)`
includes its start and excludes its end. Equality and overlap use exact integer
comparisons, including positions above 2^53. No floating-point conversion or
implicit grid choice enters this resolver.

For original victim `[a,b)` and final incoming driver `[c,d)`:

| Relationship | Condition | Effect on victim |
|---|---|---|
| Disjoint or touching | `b <= c` or `d <= a` | Preserve it. |
| Equal | `a == c && b == d` | Remove it. |
| Incoming covers victim | `c <= a && b <= d` | Remove it. |
| Incoming strictly inside | `a < c && d < b` | Keep `[a,c)` only. |
| Incoming starts inside | `a < c < b <= d` | Keep `[a,c)` only. |
| Incoming ends inside | `c <= a < d < b` | Remove it. |

The classification uses this order, so equal and shared-edge cases have one
label. There is no right-hand fragment, merge-policy selector or new survivor
identity. Incoming notes retain their caller-supplied identities; shortened
victims retain their original identity and start. Full record bytes remain the
material authority. Flags, unknown tails, channel, cents and unrelated fields
survive exactly, except for requested field changes folded into a survivor.

Collisions are partitioned only by the stored integer key within one clip's
canonical notes stream. Different MIDI channels, tuning cents and inactive
flags do not partition or exclude notes. Other clips and auxiliary addressed
streams are outside this edit.

## One batch, with explicit effects

Additions and changes to start, duration or key are incoming drivers. Velocity,
channel, cents and flag changes alone do not normalize historical overlaps.
Pure removals introduce no drivers.

Every final driver is staged before resolving any victim. All driver original
identities and explicit removals are excluded from the victim set. Selected
notes can therefore swap places or reverse their final order without erasing
one another midway through iteration. A copy has a new identity; its original
remains an ordinary eligible victim. Same-key final drivers must be disjoint;
intersecting drivers return `IncomingOverlap` with a deterministic conflicting
pair. The implementation does not invent selection, array or ID precedence.

For each original victim, any driver covering its original start removes it.
Otherwise the earliest driver start strictly inside its original interval
shortens its end. All drivers are considered against that original interval;
input-array order cannot change the result. For example, victim `[0,100)` with
drivers `[20,30)` and `[50,60)` becomes `[0,20)`, while victim `[25,100)` with
those drivers is removed. Gaps do not create right-hand fragments.

The resolver first validates the complete requested diff, including exact old
bytes, stride and opaque tails. A malformed requested change cannot become
valid merely because overlap subsequently removes that note. A non-geometric
change to a surviving victim is combined with its shortened duration; a removed
victim exposes the removal alongside the original intent. Unrelated existing
victim/victim overlaps remain untouched.

Normalized intent and effective diffs use baseline record order for removals
and changes, and ascending full-width uint64 identity for additions. Existing
baseline record order is retained; when SortedByTime requires sorting, it is a
stable start-only sort. Header, trailer, complete record bytes and exact inverse
behavior come from the MI-01 kernel.

Per-key disjoint-driver indices avoid a complete stream scan per incoming note.
Every original victim/driver overlap counts toward the fixed version-1 limit
of 100000 diagnostic pairs, including later drivers whose effect is superseded
by an earlier truncation. Pair 100001 returns `CollisionLimitExceeded`, with no
requests or partial effective result. A successful preview always contains all
its collisions and induced effects. Changing this replay-relevant bound requires
a new rule version.

## Preview and submission

`PreparedOverlap` has private const shared backing. Its const accessors expose:

- the exact source `Snapshot` and captured `expectedHead`, where null means an
  actual empty head;
- normalized `intent`, complete `effects`, and zero or one exact `requests`;
- `collisions`, `sideEffects`, and the conflicting incoming pair on refusal;
- status and an error suitable for the caller's normal refusal path.

Each side effect contains the original full record, its final full record or
removal, and the determining driver identity. It is an original-to-final
projection; requested non-geometric changes can appear in the final record.
Retained expressions stay opaque and unchanged. Removed notes carry their
complete expression sets in the request. A caller can present the proposed
shortenings/removals before submitting the same immutable value.

For example, an insertion uses a literal note identity and full record:

```cpp
auto source = adi::notes::captureNotes(store, clipId, callerOwnedStreamId);
if (!source.ok) throw std::runtime_error(source.error);

adi::notes::NoteValues note;
note.id = callerOwnedNoteId;
note.start = startTicks;
note.duration = durationTicks;
note.key = 60;
note.velocity = 100;
note.releaseVelocity = 64;
std::string error;
auto record = adi::notes::makeRecord(note, {}, error);
if (!record) throw std::runtime_error(error);

adi::notes::Diff intent;
intent.added.push_back(*record);
auto prepared = adi::notes::prepareOverlapDiff(source, intent);
if (!prepared.ok()) throw std::runtime_error(prepared.error());
// Inspect prepared.sideEffects() before submitting this same value.
auto result = adi::notes::commitOverlapDiff(store, prepared);
```

The caller handles `result.ok` and `result.error`. A missing stream needs its
previously captured literal creation ID/default row; an existing stream's ID is
authoritative. Wider future records require explicit caller-owned tails. No
persistent identities or SQL defaults are allocated/evaluated during preparation.
Copies retain their owned capture and preview after the Store is closed.

The committer checks source-file identity and constructs a mandatory expected
head guard, including a null head and an empty request list. Submission metadata
can supply actor, actor detail, label and advisory first-request `selBefore`.
It cannot replace payload, operation, target, resolver version, source or head.
Selection metadata never influences resolution. A document submitter may copy
and stamp the captured requests through its normal batch path, retaining the
captured head and document-generation guards.

Copying a request and changing its material creates a separate raw submission;
this preview does not describe that altered value. In particular, stripping the
entire optional `overlap` envelope can leave a valid literal MI-01 request.
Immutable preparation binds the preview used by the new producer, rather than
promising that arbitrary raw packets cannot express other edits.

## Transaction and replay

[OPS §9.5](OPS.md#95-notes-and-expression) specifies the closed versioned wire
shape. Outer arrays contain the complete effective diff; inner arrays contain
intent. The handler resolves that intent again in the existing journal
transaction and compares all computed effects. Permuting valid arrays is
harmless; duplicate IDs, extra outer no-op changes, omitted side effects and
contradictory intent/effects refuse before writes.

The canonical row identity, whole original ANOT hash, exact old records and
removed expression sets retain their MI-01 guards. The envelope additionally
guards the complete expression set of every retained changed note, including
an expected empty set. Missing/duplicate/extra identities and changed row bytes,
identities or SQLite storage classes refuse even if ANOT and the head did not
change. Unrelated expression identities stay outside these staleness guards.

The final planned stream is reused by the owner adapter. One ANOT INSERT/UPDATE
and expression removals share the transaction. Existing post-apply typed-row,
canonical-row, complete expression-set and actual row-change-count checks remain
in force; affinity drift, hidden trigger writes or late SQL failure roll back.
No retained expression row is rewritten by overlap resolution.

Undo uses the existing exact `note_capture` inverse, restoring original order,
rows, absence and historical overlaps without resolving them. Redo recomputes
the stored version and must agree with the captured material delta. It refuses
a contradictory forward payload; another valid intent producing exactly the
same material is not distinguished by that inverse. Existing MI-01 packets
without the envelope and the five historical scalar editor entry points keep
their literal behavior. A later rule correction gets another explicit version.

## Remaining producer work

A producer can supply checked positive final spans in reversed order; the
resolver retains their identities and admits them when final drivers are
disjoint. This does not implement a complete crossing-stretch tool. Intentional
stretch must scale expression, and negative-scale curve mapping is separate
work. Overlap end-shortening keeps opaque note-relative expression bytes,
including points beyond a shortened end; no measured Live storage claim is made.

Simultaneously overlapping incoming-note precedence remains a named refusal.
Gesture lifetime, the window editor, preview-note audition, MIDI recording and
curve-editing producers remain their own items. There is no UI change or claim
of completed Live parity in this kernel.
