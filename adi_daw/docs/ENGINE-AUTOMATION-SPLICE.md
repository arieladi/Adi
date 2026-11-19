# Guarded automation replacement foundation (AU-05)

`automation::planEnvelopeSplice` is a pure owner-side value planner. It replaces
one explicit half-open interval with an explicit incoming envelope and keeps
the old rendered curve outside that interval. This is the envelope foundation
for ADR-0217 d3. The material planner binding follows in a separate slice;
ordinary material editing still uses its existing unlocked-automation refusal.
There is no new opcode, persistence format, menu or window-side point batching.

The owner supplies `SpliceKnot` values in the lane's native clock together with
their absolute sample positions from one captured tempo map and Session rate.
`SpliceBounds` carries both coordinates for each edge. The incoming envelope
includes both exact edges; its terminal value describes the incoming End and
has no outgoing range. It is already translated into the destination clock.
The foundation performs no time conversion, owner/parameter mapping, setting
lookup or clipboard capture. It cannot choose a move, copy or clear policy.

For an old segment from 0 to 100, replacing 25 to 75 retains the old prefix and
suffix, adds the old incoming guard at 25, inserts the explicit incoming points,
and restores the old outgoing curve at 75. Coincident order matters: incoming
owns Begin, and the old envelope owns End. Existing points exactly at End,
including multiple step points, survive in their original order. Before the
first point the old first value is held; after the last, the old last value is
held. An empty old envelope gets its explicit lane default on both outside
sides. Replacing that lane never changes its default to the incoming value.

Nonlinear outside pieces retain `engine::CurveSlice` subdomains instead of
fitting a different curve between new endpoints. Existing subdomains remain
relative to their original source curve. A native interval that projects to
zero rendered length needs no outgoing range; surviving coincident points
still determine the outgoing value. Both sample and native ordering, including
consistency at the two cuts, are checked. All values and supported curve data
must be finite; unknown point flags/reserved fields and unrepresentable curve
progress refuse explicitly. Native and rendered spans must both be positive.
The rendered precision bound is 2^50 samples; the default point budget is 2^20,
and callers cannot disable the hard 2^23 cap. Failure returns an error and no
partial geometry, leaving both input envelopes unchanged.

Every returned knot and domain owns its values. `pointOrigin` identifies an
unchanged original ordinal, an incoming ordinal, or a new boundary. Original
and incoming uint64 IDs remain literal values. Guards use ID zero with Guard
provenance, which distinguishes them from unchanged legacy ID zero. Persistent
point/range IDs are allocated by the owner when forming the single captured
transaction. `rangeOrigin` identifies the old or incoming outgoing segment from
which a domain derives. Two outside pieces can cite one old range: its row ID
may survive on only one piece, with the other allocated through the shared
allocator. Preserve the complete old row, including unknown columns, when
using that provenance.

The owner adapter must validate the AAUT header/version/stride and retain its
header/trailer and typed row information before decoding. This value API does
not represent unknown record tails and cannot authorize synthesizing them.
It also does not acquire a Store, database transaction, IDs, transport, files,
callbacks or settings. Its vectors allocate on the owner thread; the function
is not an audio callback API. The existing renderer evaluates the resulting
program without new callback work.

The suite compares every sampled value outside each replacement against the
original engine `valueAt` program and every inside value against the incoming
program. It covers all six curve families and five tensions, existing ranges,
coincident End steps, quantized cuts, empty/held lanes, full-width IDs and
near-limit clocks, invalid inputs and a fixed 120-span sequence. These are ADI
regressions, not Live measurements. Full AU-05/AT-10 acceptance, track/device/send
scope mapping and the planner's atomic move/copy/delete integration remain the
next binding work.
