# Automation in the immutable project view (N26)

The window reads `Snapshot::automationLanes`, ordered by lane id, or
`findAutomationLane(id)`. Each lane exposes its owner kind/id, parameter ref and
name, time base, value domain and unit, default and optional bounds, enabled,
visible and height fields. These are stored facts, independent of whether the
audio engine can play that owner or parameter today.

`lane.envelopes` contains `automation_data` rows ordered by their stable data id.
A missing `clipId` means arrangement automation; a present one is a clip envelope
whose times are relative to that clip. Each point has its original uint64 id,
time, value, tension, curve and flags. Time is ticks for lane time base 0 and
nanoseconds for 1. The snapshot does not place points in samples or change their
identities. Empty lanes retain their metadata/default value.

An unreadable or invalid envelope is retained with an explicit `problem` and
empty points, never a plausible partial curve. Checks follow playback's units,
ordering, finite values, bounds, curves and reserved bytes. Wider future AAUT
records retain their known prefix. Original encoded bytes participate in
structural sharing, so edits to an unknown tail or corrupt data remain visible.
Unchanged envelopes share even when their lane's name or override changes;
changed time bases or bounds revalidate the stream before reuse.

Overrides are session state (ADR-0162). On the message thread, first refresh the
Session after an edit, then call
`view.refresh(store, session.overriddenLanes())`. Do the same after
`session.reenableAutomation()` or its single-lane overload. The resulting root's
`automationOverridden` lights Re-Enable Automation; `lane.overridden` marks each
affected lane. A frame holding the previous snapshot keeps its previous flags.
The ordinary `refresh(store)` / `SnapshotBuilder::fromStore(store)` overload
represents a view without a live session and has no overrides. No runtime flag
is written into the project or journal.

All reads, decoding and node allocation happen on the calling message thread.
Every exposed field affects sharing; unchanged lanes, envelopes, tracks and
tempo retain their nodes. This adds no schema or op and changes no playback
policy. The window owns forwarding the live Session state at its refresh seam.

N17 compatibility: when automation_segment_ranges is present, each outgoing
AutomationPointNode has an optional curveSlice with begin/end progress and
sourceFirst/sourceLast. Use N17's engine::curveSliceValue for drawing: convert
these four values to its CurveSlice, then evaluate with the stored curve,
tension and endpoints. Refitting the curve between the new endpoints changes
its shape. The snapshot also retains curveRanges with identity/ordinal/owner;
all fields participate in sharing, including range-only changes with identical
AAUT bytes. Invalid owners, duplicate/missing ordinals, nonfinite values and
bad intervals empty the entire envelope with an explicit problem. This is an
optional SQL seam, with no dependency or schema bump on this branch. Clip
envelope data remains readable here; this does not add its playback.
