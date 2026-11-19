# Captured automation replacement storage

`automation::prepareEnvelopeSplice(Store&, laneId, begin, end, replacement,
actualSessionSampleRate, pointBudget)` adapts the pure AU05 envelope splice to
one global lane. It prepares one existing `arranger.splice`; submit its returned
requests intact through the ordinary journal/submitter. Preparation is read-only
and belongs on the Store owner thread. A refused submission needs preparation
again. Actor stamps remain supported by the captured transaction path.

Incoming points use absolute native positions in the destination lane's declared
clock, include both boundaries, and have no outgoing range on the terminal End.
Optional CurveSlices are owned values. The adapter projects the old and incoming
points and both cuts through the same captured tempo map and actual sample rate;
callers do not supply sample projections. Incoming point IDs are ignored and
allocated afresh, even if they collide with old full-width IDs. The replacement
is half open: incoming wins at Begin, the original target wins at End.

Outside original point IDs, including uint64MAX and legacy zero, survive
literally. New guards and copied incoming points use distinct positive IDs,
skipping every ID in the original stream, including removed points. SQL row IDs
use the existing `clip_edit::CopyIds` owner allocator. One old nonlinear range
split into two retains its complete typed row and ID on the first surviving
piece; the other gets a fresh row ID and the same opaque columns. New incoming
ranges use captured table defaults. Point ordinals and progress domains follow
the pure plan; zero rendered-width ranges may disappear. No curve is flattened.

Only AAUT version 1, 32-byte records, known header/record flags and a clock that
matches the lane are editable. Future versions/strides, malformed values,
unsupported flags, unsorted points, invalid ranges and exhausted budgets/IDs
refuse without a partial packet or any SQL write. The original header flags,
opaque trailer and complete data/range SQL columns survive. Clip-scoped streams
remain unchanged; a clip-owned lane is refused. An absent global stream is
created with the lane's old default represented by the outside guards; Undo
restores absence. Input/default/curve/clock precision and point budgets retain
the pure kernel's limits; rate must be finite, positive and at most 768 kHz.

The sealed packet captures the live Store connection and undo head, the complete
lane row, every data row for that lane, every range row for the global stream,
the complete tempo map, and absence at each newly allocated SQL identity.
Ordinary journal commit checks these inside its transaction before mutation.
This detects same-head lane/clock/default edits, stream/range insertion or
removal, tempo changes and identity reuse. It does not stale unrelated track
names. Exact packet admission rejects tampering, mixed/partial packets, source
replacement and reuse after a durable commit. A SQL failure rolls back points,
ranges and journal rows together; History Undo/Redo uses captured native typed
images and needs neither the original clipboard nor the transient token.

This is the storage prerequisite for AU05. It selects no material move/copy/
delete policy, track/device/send mapping, source clear-hole values or tempo
translation rule, reads no Lock Envelopes setting, and is not yet bound to the
material planner. Existing unlocked relevant-automation admission still refuses.
It introduces no opcode, schema, UI binding, media rewrite or callback work.
Live measurement parity remains pending the PLAN readings.
