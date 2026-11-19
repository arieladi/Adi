# Native Link playback

`engine::LinkPlayback` connects the existing Link SDK adapter to `Session`.
The window/driver integration owns one participant, one consumer and the
serialized document owner. This API does not open networking: the owner opts in
with `LinkSession::enable`, and independently selects Start Stop Sync.

## Threads and clocks

Call `process(io, buffer, command)` on the single audio driver. Supply Link-domain
monotonic callback time and **physical output latency only**. The consumer pins
the graph first, adds that graph's compensation once, and captures one SDK audio
state. A launch query uses a copy of that state; it does not force the peer phase.
`processCaptured` is the controlled-frame test seam into the same native consumer.

One physical callback retains one graph, its clock, source generation and
automation bindings across all launch/stop slices. Device-parameter event sources
and their owners are captured during graph preparation; later bindings first take
effect with the next acquired graph. Both output widths, input
pointers and `streamTimeSamples` advance together. Expired render handles refuse
without reading reclaimed state. No callback constructs graphs, reads SQL/files,
allocates, frees, locks or copies owning reference counts.

Return `Result::preparation` by value through the application's driver-to-owner
mailbox. The serialized owner calls `prepare(store, packet, nowMicros)` in the
same Link clock domain. Replaced, consumed and more-than-two-second-old packets
refuse. The owner prepares Warp/read-ahead and immutable automation, then admits
the packet before changing shared bindings. An intent arriving after admission
is a subsequent request. A failed or stale candidate leaves the last valid graph
available. The result's `waiting` flag distinguishes preparation/quantized waiting
from completed playback.

## Musical time

Peer tempo changes prepare a private effective map used by the native audio,
Warp, MIDI, automation and metronome builders. They do not write tempo ops or
change the project digest. A performed boundary retains elapsed sample time
before that boundary and replaces its future. Disabling Link restores authored
tempo from a performed boundary. New starts reset the performance history.

The map retains at most 4096 boundaries. Exceeding this resource bound refuses
preparation with a Session error; it does not silently discard earlier timing.
A new start resets this private history. Frozen renders also retain the existing
external-tempo refusal until the owner unfreezes them.

The driver compares the tempo at the actual pinned position as well as the
selected external BPM. After a backward seek or return to an earlier loop
segment, repair replaces the future from the exact seek/loop boundary. The old
valid generation can continue while the owner prepares that change. Retimed
sample bounds of an unchanged musical loop preserve transport identity; an
authored loop edit remains a discontinuity.

Phase tracking counts fractional performed beats across loop wraps. A deliberate
locate establishes a new local position. Errors of up to two frames are treated
as timestamp/tick quantization. Larger errors request a bounded rate correction
toward Link over a two-second horizon, limited to 0.5 percent and 20–999 BPM.
One drift preparation remains valid while the owner works; real tempo/mode
changes supersede it. Corrections are throttled after acknowledgement. The
consumer does not resample the mixed output or change unwarped source rate.

## Starts, stops and readiness

`setLaunchTick` selects the musical start position. A stopped local Play and an
opted-in peer start wait for the captured launch timestamp and prepared clock.
A later launch query for the same intent updates its pending boundary. Late
starts chase the corresponding musical position. Play while already playing is
idempotent and cancels an older scheduled Stop. Opting out cancels pending peer
intentions while retaining local transport control and shared tempo.

Before a playing slice, the pinned source generation must admit its actual
mandatory pages. An unavailable distant position requests owner preparation
with the intended launch tick. A page-readiness result is used immediately with
the same request target. Admission retains actual fixed-slot page reservations,
so even a worker's temporary claim cannot make an admitted page unavailable.
Reservations survive repeated loop visits and processing parts; superseding
requests, failed admission, owner timeout and stopped rendering release them.
Ordinary rendering drops obsolete reservations before requesting its next range,
so a full admitted cache cannot starve later playback. The worker still rejects
stale request snapshots. Owner preparation covers the current position and loop
head within the fixed page budget and retains its prepared pages until adoption.
It never reads mutable driver Transport state directly.

Stored note identity, authored content and the actual instrument route decide
whether a held voice survives a clock-only rebuild. Matching voices retain their
runtime IDs; real note edits, instrument replacement, seeks and stops retain
release/chase behavior. Accepted delayed MIDI events carry their remaining arrival
time and live-source ownership through graph replacement. Controller identity is
retained across unheard intermediate generations as well as audible ones.
For fan-out, a release keeps its ownership until every accepted delayed copy
reaches an instrument or output. A retained onset also validates its adopted
voice before delivery, so a canceled or incomplete on/off pair cannot resurrect
a released voice. This uses bounded queue scans; saturated large fan-out graphs
have no callback-time benchmark in this gate.

## Verification scope

The native suites cover actual PCM and instrument events in both precisions,
input recording, graph/clock/automation publication, held and delayed MIDI,
source readiness, stale preparation and callback resource guards. The optional
`adi_link_playback_tests --peers` path uses two actual SDK participants; ordinary
test runs keep networking off.

This engine API does not wire the JUCE device driver or window, establish hardware
latency, prove Live parity, or enable Link Audio. Windows/MSVC and the director's
hardware/Live readings remain separate validation legs. The SDK re-pin is the
separate queued XC-31 item.
