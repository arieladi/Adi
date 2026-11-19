# Routing identities in snapshots (N39)

Snapshot.maxRoutingId is the highest positive id in the entire routing table,
including main, send, sidechain, VCA and cue rows, disabled rows and hardware
endpoints. Endpoint numbers are not routing row identities. It is zero when no
positive routing id exists, and retains signed 64-bit width, including INT64_MAX.
Each rebuild copies the current maximum into the root; unchanged send/track
nodes remain shared, and older published snapshots retain their old maximum.

For a new send the window can use maxRoutingId + 1 after checking that the maximum
is below INT64_MAX. Reserve further ids within the same pending batch. This value
is a snapshot observation, not an allocator: guard the document generation/head
when committing a prepared batch and handle a collision from another writer.
The engine never reads UI selection or invents an id inside routing.connect.

The maximum and send list come from one SQLite read snapshot, borrowing an
existing caller transaction when present. A concurrent WAL writer is seen in
full on the next rebuild, so a frame cannot contain a send above its maximum.
