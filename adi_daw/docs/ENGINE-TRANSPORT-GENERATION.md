# Transport metadata belongs to the rendered graph

`GraphHost::rebuild` accepts an optional owned
`shared_ptr<const GraphTransportState>` after the existing plan transform.
It publishes that state inside the same `PublishedGraph` as the prepared
resources. Failed builds publish neither part. The audio reader acquires one
graph, performs its usual history handover, announces it, and invokes that
graph's `beginBlock()` exactly once before any node runs. Its returned
`TransportInfo` belongs to that actual graph, rather than a separately published
clock that might advance between the two acquisitions.

The owner constructs immutable clock parameters and retires them with the
graph. A clock may own private single-audio-reader scratch for the returned
metadata. `beginBlock` must be noexcept and perform no allocation, deallocation,
locking or I/O; the metadata must live through the complete block. The audio
path does not copy or release shared_ptrs. Graph destruction precedes clock
destruction. Skipped generations retain and retire their clock without applying
it; the existing strict reader-epoch reclamation remains unchanged.

`renderedTransportState()` is for the single audio reader after processing and
before its next call. It names the graph that actually ran, protected by that
same publication. A newer owner publication cannot change this pointer until
audio adopts a newer graph. It is not an owner/UI accessor or a separately
retained handle. Omitting the optional state preserves the caller's original
`AudioIo::transport` metadata, so standalone/legacy hosts retain their contract.

`DeviceHost::RebuildSpec::transportState` forwards this ownership on initial
build and latency/shape-coalescer rebuilds. Session now prepares its TimingView
into that graph-owned state, applies that generation's musical loop at the
block boundary and derives node transport metadata from it. It removes the
separate TimingView publisher. Metronome/post-graph consumers use the clock
from the actual rendered graph; they cannot accidentally pick a future owner
generation. A safe type query permits a caller-installed foreign graph context
without interpreting its payload as native metronome state. No project schema,
opcode, persisted setting or UI change is involved.

This fixes publication identity before live external-clock following. It does
not connect a Link participant, change Session tempo, schedule a future quantum,
queue owner preparation or bypass monitoring latency. The independent Link SDK
adapter and external-tempo preparation can be combined with this primitive in
the next native playback integration. The existing Session/GraphHost owner and
single-reader thread contracts still apply; nothing permits concurrent owner
writers or callback-side graph preparation.

Tests exercise actual graph node metadata and PCM at both widths, exactly one
clock invocation, multiple unheard publications, failed rebuild, old-reader
protection, owner retirement and legacy caller metadata. DeviceHost's real shape
coalescer retains the clock on rebuild. Native Session refresh/render stress
actually observes both tagged source generations and rejects any mismatch
between the rendered source and its BPM; callback allocation/free/file-I/O
metrics stay zero. Existing host, transport-info, loop, metronome and cue suites
remain part of the gate. Compiler, fault and sanitizer evidence is recorded in
the engine log; this is not a measured Live or unrun platform claim.
