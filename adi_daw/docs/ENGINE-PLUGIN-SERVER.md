# Plug-in server protocol study

DF-E92 and DEV-27, 27 November 2026. Implement the client in ADI against a
pinned AudioGridder v13 subset. Use the stock server for the first VST3
interoperability checks. The ADI server fork needs a versioned extension for
native note expression and full plug-in identity.

This study follows the upstream client, server and JUCE dependency at
`76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f`, where the wire version is 13.
The `release_1_2_0` tag (`22654e2b9d7d23548e3f65f8d363f662f6c20d0d`) uses
the same audio and network handshake layouts. These findings describe those
sources. Interoperability still needs a live client/server run for each
supported platform and build. [Upstream revision][upstream] · [Release tag][release] · [Wire declarations][handshake]

## Stock connection and messages

1. The client opens a TCP connection to the configured master endpoint
   (upstream defaults to 55056 plus server ID). The request supplies protocol
   version, input/output/sidechain channel counts, sample rate, block size,
   precision, client ID, flags and an active-channel mask. The response gives
   the version, sandbox/local-mode flags and a worker port. The master
   connection closes. The stock server checks that the request version is at
   least 13. An ADI client should accept only response versions it understands.
   Supported local configurations can use Unix-domain sockets for the same
   exchange. [Defaults][defaults] · [Client handshake][client-handshake] ·
   [Server handshake][server-handshake] · [Worker allocation][worker-listener]
2. The client opens the worker's request/reply, server event, audio and screen
   sockets in that order. In a stock remote session, a headless client can
   establish the screen socket and
   leave it idle. The worker sends its plug-in catalogue on the request socket
   once the connections are set up. [Client worker setup][client-worker] ·
   [Worker accept order][worker-accept]
3. A command frame starts with native integers for message type and length.
   The types use fixed native fields, strings, JSON or MessagePack depending
   on the command. String and binary payloads also carry an inner length. The
   stock catalogue is MessagePack. Add Plug-in takes JSON with a server
   catalogue ID, initial settings, layout and optional multi-mono channel
   mask. Success returns JSON with latency and is followed by preset and
   parameter descriptions. State get/set uses the processor's chain index and
   one text value containing JUCE-encoded memory blocks separated by pipes.
   Validate both lengths and decode the JUCE encoding explicitly. The stock
   set-state handler has no success reply. [Command framing][command-frame] ·
   [Payload encoding][payloads] · [Add result][add-result] ·
   [Catalogue][catalogue] · [State messages][state-messages] ·
   [State encoding][state-encoding] · [JUCE memory blocks][juce-memory-block]
4. An audio request carries dimensions, requested output size, precision,
   event count and a trace UUID, followed by planar float32 or float64 PCM,
   MIDI records and the raw JUCE `AudioPlayHead::PositionInfo` object. Each
   MIDI record carries a sample offset, byte count and MIDI bytes. Requested
   dimensions let an instrument send MIDI with zero audio input channels.
   The reply contains dimensions, MIDI count and plug-in chain latency in
   samples, then planar PCM and MIDI records. The worker processes on request
   with the client's playhead position and returns the requested precision.
   These are native object representations with ABI-dependent sizes, padding
   and byte order. Check compatibility with the pinned layout and write each
   supported field explicitly at the boundary. [Audio framing][audio-frame] ·
   [Server audio worker][audio-worker] · [Pinned JUCE playhead layout][juce-position]

The stock server scans VST3, with VST2, AU and LV2 support conditional on
build, platform and configuration. Its catalogue's `id2` contains the format
and JUCE's integer `uniqueId` in hex. Treat it as a server lookup token. The
ADI fork must advertise the full VST3 class ID or CLAP ID so ADI can use the
same project identity for local and remote instances as ADR-0261 d22 requires.
The fork's two formats are VST3 and CLAP. [Stock format scan][formats] ·
[Catalogue identity fields][catalogue-id] · [Stock ID derivation][stock-id]

## Events and offline semantics

Sample-offset MIDI allows MIDI note messages, pressure, channel pitch bend
and MPE messages where supported by the receiver. A MIDI channel/key and
quantized controller value cannot encode CLAP's independent note IDs,
port IDs and double-precision per-note expression. This matters even at the
same channel and pitch when two notes overlap and need independent expression.
The stock audio path passes a JUCE MIDI buffer. Its separate parameter
messages carry a parameter index, channel and float value without a block
sample offset or note ID. The stock wire has no native CLAP note, note
expression, parameter modulation or other typed process event. [Audio MIDI encoding][audio-frame] ·
[Server audio worker][audio-worker] · [Stock parameter messages][parameters]

Extend the fork through explicit capability negotiation. Send a bounded,
length-delimited list in each direction with event type, sample offset and
stable fields for that type. Preserve CLAP's note ID, port, channel, key,
expression or parameter ID and double precision where applicable. Preserve
event order at the same sample, carry variable data as bytes, and construct
native events and any pointers locally. Keep MIDI as a negotiated dialect.
A first round-trip test should send overlapping same-key notes with distinct
IDs and different fractional tuning at known offsets, then compare the
received events and generated audio to the local CLAP host. [ADI's local CLAP event mapping][adi-clap]

Stock v13 processes a request on arrival and has no offline render-mode field.
ADI can wait for an ordered reply on the offline render thread. This alone
cannot promise the plug-in's format-specific offline mode or quality. The fork
will need to negotiate offline preparation and format-specific render mode.
[Handshake fields][handshake] · [Audio framing][audio-frame] · [Server processing][audio-worker]

## Authentication, encryption and permission

Stock v13 provides no cryptographic server authentication, client
authentication, encryption or message integrity beyond ordinary TCP delivery.
The client supplies its own ID. The server uses that value for logging and
sandbox naming. The worker accepts the expected sockets by arrival order.
Discovery's mDNS UUID and labels are advertised metadata. Use discovery to
suggest addresses and the ledger to authorize contact. A discovery identity
or an ADI ledger grant cannot authenticate the peer on the wire. Optional
local sockets rely on OS isolation. [Server accept and ID][server-handshake] ·
[Sandbox naming][sandbox-id] · [Worker accept order][worker-accept] ·
[Socket I/O][socket-io] · [Discovery advertisement][discovery]

Use stock TCP on a deliberately trusted LAN, or within a tunnel the user
configured. Keep discovery off by default and require the local project/server
grant before connecting or sending audio, MIDI, transport or plug-in state.
Received projects, copies and pasted devices carry only the requested
location. Treat all incoming frame sizes, dimensions, event counts, strings
and state blobs as untrusted. Set bounds before allocating, enforce deadlines
off the audio thread and keep each connection's failure local to its device.
[ADI decisions, ADR-0053 and ADR-0261 d20–d23][decisions]

## Client choice and device contract

Write the ADI client against the pinned wire behavior. Keep framing and
transport behind an internal connection and test it against stock releases.
The upstream client brings a JUCE plug-in wrapper with screen and tray code,
and its audio streamer can issue direct socket I/O without buffering or wait
for a buffered reply. ADI's callback contract needs preallocated bounded
handoffs to a dedicated I/O thread and a fixed one-block delivery deadline.
A small client can own those semantics directly. [Stock connection setup][client-worker] ·
[Unbuffered send][streamer-unbuffered] · [Buffered receive][streamer-buffered]

Upstream's own source is MIT and requires its copyright and permission notice
to accompany copies or substantial portions. This resolves the GPLv3
parenthetical in ADR-0053 for upstream's own source. Its JUCE 7 fork is a
separate dependency, with GPLv3 or commercial terms for the full library and
ISC terms for designated modules. The build also pulls in FFmpeg, WebP and
Boost. Before distributing the server fork, audit the pinned dependency
notices and built configuration with win, and keep upstream attribution in the
licence materials. The ADI binary needs no additional upstream client
dependency for a client written against the wire. [Upstream MIT licence][licence] ·
[Pinned JUCE licence][juce-licence] · [Upstream build dependencies][build] ·
[ADI external code policy][external]

Use the existing `DeviceInstance` and `remote_host_id` placement. Load one
remote processor per ADI device so state, latency and failure belong to that
device. The live callback submits the input block and consumes the previous
block's reply. Report the prepared block length plus plug-in latency through the
device's existing latency hook. A lost deadline produces silence for that
device and increments its counter. Discard stale responses before accepting
another block. When loading an unavailable server, try the same plug-in
locally, then preserve state in a placeholder. An offline render using a
remote device waits within its render deadline and fails by device name when
the server fails. When state capture cannot complete, keep the last stored
bytes and mark the device stale, as SC-430 specifies. [Device interface][device] ·
[Project schema][schema] · [Delay compensation][pdc] · [Export engine][export] ·
[DEV-27][plan]

## Engine increment

The session owner injects a granted server loader with `withRemoteDevices`.
The project's device row selects a requested server. The loader reads the
current endpoint and project identity, then asks the application for a
paired destination and the AGT-03 plug-in-hosting lease. The ledger scope
includes the application, project UUID, connection and server fingerprint.
At issuance the authorizer borrows the open project Store to derive its
machine-local file binding. A copy with the same project UUID needs its own
grant. The default sharing copy gets a new UUID and disables its server
rows. A disabled row is refused before the endpoint is resolved or a grant
is requested. The lease retains app-owned state after issuance.
The worker checks the lease before setup, processing and state exchange.
Raw stock lookup tokens cannot serve as project plug-in identities.
An unavailable server falls back to the installed plug-in for live
playback, or the existing placeholder when both are unavailable.
The VST3 loader takes a full-class-ID lookup against the validated local
catalogue. A successful fallback retains the same class ID for its
project and runtime identity. Old local projects still use the existing
catalogue identifier path. Offline playback requires the authored server.

`RemoteDevice` uses two preallocated single-producer, single-consumer rings.
The I/O worker owns connection setup, processing and teardown. The device
gathers graph segments into the prepared quantum and reports one quantum
plus plug-in latency. The callback checks for the previous sequence and
counts a missed deadline when that reply is absent. A device that falls
behind drops stale replies. When a variable-size device callback crosses a
quantum boundary, it can leave less than one full period for the just-submitted
request. The callback applies the same deadline rule in that case.

WAV export, freeze and the diagnostic player's isolated renderer wait for
bounded worker exchanges. A failed exchange names the plug-in and stops the
render before publishing output. A saved state that the backend cannot
restore also stops offline rendering, leaving the stored bytes available.

State capture and recall run on the same worker with a 500 ms owner deadline.
The stock transport bounds and decodes the server's text encoding, sends
state sets in request order and reads back after a set. The stock command
carries no positive set acknowledgement. One silent, event-free exchange
without transport lets the plug-in settle accepted state before the first
musical block. The exchange uses the original deadline. The official server's ADI Test
Plain fixture returned a changed 193-byte state exactly and played after
recall at peak 0.0712499991. The engine's save receipt names stale devices
whose capture failed, preserves their last stored blobs and continues
capturing the other devices. The window's save and quit policy uses this
receipt as part of SC-430.

The stock prototype covers explicit IPv4 endpoints, mono or stereo float32
PCM, ordinary MIDI input and transport. The native loopback protocol
negotiates activation, offline mode and state. Its typed block codec carries
sample offsets and double-precision expression with independent note and
port identities. The client exposes decoded reply events to its caller.
The release server will need peer authentication, encrypted transport and
a maintained hosting target. The fixture TCP run exercises stock framing.
A second loopback test serves an actual CLAP fixture from another
process, using a fixture lookup token. ADI's compensated WAV export matched
all 1,732 samples of the same local CLAP render exactly (peak 0.37109375).
Disconnecting after seven replies failed export by device name and discarded
the staged audio.

The native peer can advertise parameter read and set support during
activation. The client reconstructs ordinary CLAP parameter IDs from the
SDK IDs in a bounded manifest and publishes only acknowledged normalized
values. The device keeps the same descriptor slots when a parameter is
removed or returns after a reconnect. The real CLAP host regression
applies a manual gain value and a normalized automation event to two held
voices. With a seven-sample fixture DSP delay, automation at frame 40
changes its PCM at frame 47. The remote device reports 71 samples and
matches 256 compensated samples exactly.

Stock v13 reports JUCE parameter indices. A paired catalogue supplies an
explicit index-to-project-ID map for the full VST3 class before the client
exposes those controls. The worker reads each bound value and reads back
after setting. These controls are configurable and have no
sample-accurate automation capability. The official server returned
0.625 for an acknowledged Drive set on the ADI Test Plain fixture.

### Surge XT acceptance

The official Surge XT 1.3.4 Linux plug-in ran in two comparisons. Each
comparison rendered 500 stereo blocks of 64 frames at 48 kHz and aligned
the remote output by the 64 samples reported by ADI.

| Host path | Aligned float32 samples | Peak | Maximum absolute difference |
| --- | ---: | ---: | ---: |
| Stock server VST3 and local VST3 | 64,000 | 0.253958732 | 0 |
| Native CLAP server fixture and local ADI CLAP | 64,000 | 0.504983 | 0 |

The stock run enabled oscillator retrigger on both hosts and verified
the switched values before playing A4. The official Linux server used
the scratch scheduler fallback described below. During state recall Surge
queues the patch, applies it in its first DSP block and stops voices.
With musical input in that block it can clear a newly delivered note.
With the stock worker's silent exchange, an authored 67,061-byte state
loaded through Session and the remote plug-in matched all 64,000 aligned
samples from local `adi_play`. A note at the first musical block reached
peak 0.253958732. Removing only the silent exchange for the same project
produced 64,000 zero samples.

The stock comparison also ran across separate veth network namespaces
with the server at `198.18.0.2:55056`. The first block was silent,
and all 64,000 compensated samples again matched the local reference
bit for bit. Setup took 1.238 s. Across 501 audio exchanges, RTT
p50/p95/p99/max was 74.092/1,723.52/5,800.62/26,054.5 µs.
Thirty exchanges exceeded the 1,333.333 µs period. A second run produced
the same PCM and exceeded the period on 37 of 501 exchanges. The runs
used offline waits, stock server logging and the scheduler fallback.

The native run drove identical authored MIDI through two ADI Sessions,
with the remote CLAP loaded in a separate process. Two overlapping notes
on the same key reached the server as distinct runtime IDs at offsets
0 and 32 with exact tuning of +0.125 and −0.375 semitones. A direct
native fixture also measured a 0.62385 peak PCM difference from
neutral expression and preserved finer fractional values bit for bit.
The native test protocol is scoped to loopback. A separate native state
test captured a nondefault 50,208-byte Surge state and restored it in
a freshly prepared server process. Its state bytes and all 64,000
compensated output samples were identical across both instances and
the local reference. [Official Surge XT release][surge-release]

### Discovery and cached catalogue

The application network owner can enable `PluginServerDiscovery` from
`sync.audioGridderDiscovery`, which defaults to off. A disabled browse opens
no socket. An enabled IPv4 browse sends one `_audiogridder._tcp.local` query
to `224.0.0.251:5353` and reads unicast replies on the same ephemeral UDP
socket. It deduplicates by advertised UUID and address, returns the reply
TTL, and closes the socket after the bounded collection window. The default
window is 750 ms. A portable loopback fixture verified silence while disabled,
one fixed query when enabled, malformed and wrong-source reply rejection,
and one result from duplicate replies in a 250 ms window.
[Discovery engine seam][discovery-engine] · [Loopback fixture][discovery-fixture]

The production multicast query also found the official AudioGridder 1.2.0
Linux server on an isolated virtual Ethernet pair. It returned the server
at `10.254.42.1:55056` with a 10 s TTL in a 753.233 ms browse. The network
namespace had no physical LAN access. [Official Linux archive][linux-release]

`PluginServerCatalogue` stores one atomic metadata snapshot per locally
paired server fingerprint in the application's cache directory. A network
refresh requires a ledger grant for the project, paired server and exact
destination before it queries. The observed mDNS UUID has no authority.
The stock catalogue query reads the unfiltered list and closes without
activating a plug-in. A stock short token enters the cache only when an
application-owned resolver supplies a separate full VST3 class ID. Other
authorized sources can supply full VST3 class IDs and case-sensitive CLAP IDs.
Cached reads work offline and carry no permission to connect. The stock
fixture put one mapped row from two eligible wire entries into the cache.
The official Linux server returned four VST3 entries, and the authorized
refresh and readback cached the one mapped class in 18.5103 ms without the
scratch scheduling fallback.
[Discovery engine seam][discovery-engine] · [Stock metadata transport][wire-transport]

## Measuring the link and deciding when PTP is needed

Measure RTT with a monotonic clock on the sending endpoint. Subtract measured
server turnaround to estimate the combined transport time. Half that interval
is a one-way estimate under the assumption of equal delays. A synchronized
sender and receiver can measure each direction separately with four stamps
(client send, server receive, server send, client receive). Record warm-up,
packet size, sample count, median, p95, p99, maximum, loss, misses against the
block deadline and clock basis. A stock audio request's trace UUID and
optional JUCE host callback time do not supply paired synchronized transport
stamps. [Probe source][probe] · [Wire request][audio-frame] ·
[JUCE host callback field][juce-position]

Measure the stock processing exchange separately so the deadline test
includes TCP, scheduling and plug-in work. Compare loaded full-block
turnaround and misses at the target sample rate to `blockFrames / sampleRate`.
At 48 kHz the deadline is 1.333 ms for 64 frames and 85.333 ms for 4096.
If replies miss that deadline, the short-block target needs lower measured
transport, scheduling or processing delay, or a longer block. The server is
request-driven and PTP does not remove ADI's pipeline block. Use a shared clock
when RTT and endpoint turnaround leave directional uncertainty larger than
the slack available to diagnose deadline misses. Repeat with synchronized
stamps and report clock offset uncertainty alongside directional jitter. For
a future free-running audio peer, measure sample phase drift over a
representative run and compare it with that mode's synchronization tolerance.
[ADR-0107 d5][decisions] · [Server processing][audio-worker]

### Stock server interoperability on Linux

A same-machine trial used the official AudioGridder 1.2.0 Linux x86-64
binary on IPv4 loopback in a private network namespace and Xvfb display.
The release ZIP SHA-256 was
`3b29d13d2a2bbb4e5ab8ac22832562f4410da17b64617844a18c038ca1253922`.
The server scanned the owned ADI Test Plain VST3 fixture as `VST3-8a7afd7a`.
The ADI native transport started it and sent an A4 note on followed by
note off for the final four blocks. The output from 500 stereo blocks at
48 kHz and 64 frames was bit-identical to all 64,000 float32 samples produced
by the same VST3 bundle loaded locally. Peak was 0.0712499991 (−22.944 dBFS),
with zero reported plug-in latency and a tail peak of zero. An ADI Session
playing the project MIDI through the authorized server loader confirmed
64 samples of reported pipeline delay and a silent first block. After
compensation it produced the same 64,000 float32 samples, bit for bit.
[Official Linux archive][linux-release] · [ADI test VST3][stock-fixture] ·
[ADI v13 transport][wire-transport]

The unprivileged container denies real-time scheduling. With the release
executable alone, the 64-frame Plain trial and 128-frame MPE trial reached
VST3 preparation and hit the 15 s setup deadline before the stock audio
worker started. A scratch-only loader interposer retried the stock worker's
rejected real-time `pthread_create` as `SCHED_OTHER` and logged `EPERM`
followed by success. The PCM comparison and timing samples use that fallback.
The direct transport trial bounded setup at 15 s and each exchange at 1 s.
Two sequential runs measured p50/p95/p99/max RTTs of
27.171/65.339/162.015/2,074.101 µs and 23.495/27.873/50.877/233.492 µs.
The first run exceeded the 1,333.333 µs budget at least once. This measures
TCP, scheduling and plug-in processing on one machine. A physical-link
and two-host PTP measurement are still owed.
[Stock worker launch][worker-accept] · [Stock audio worker][audio-worker]

### UDP loopback measurement

The probe ran between separate Python 3.13.5 processes over IPv4 loopback on
Linux 6.18.57. Each run sent 1,000 echoes with 1 ms between exchanges, 512
payload bytes (557 bytes including the probe header), a 100 ms receive
timeout and a 1,333.333 µs RTT budget. Each run received all 1,000 replies
and included all of them in the results.
The probe uses nearest-rank percentiles and reports jitter as the absolute
change between consecutive successful samples. [Probe source][probe]

| Clock mode | RTT p50 / p95 / p99 / max (µs) | RTT jitter p95 (µs) | Budget misses |
| --- | ---: | ---: | ---: |
| Unsynchronized | 37.787 / 62.525 / 104.579 / 1,529.083 | 33.411 | 1 |
| Shared system clock | 36.045 / 98.930 / 911.995 / 6,124.483 | 73.811 | 9 |
| Shared kernel TAI clock | 32.830 / 60.122 / 145.982 / 6,640.306 | 46.150 | 6 |

The unsynchronized run's symmetric one-way estimate had a median of 17.4865 µs
and p95 jitter of 16.3845 µs. The shared system clock measured outbound and
inbound medians of 20.702 and 11.988 µs, with p95 jitter of 36.795 and
34.782 µs. The same-host TAI clock measured medians of 19.440 and 11.097 µs,
with p95 jitter of 23.215 and 23.825 µs. Both shared-clock runs accepted all
directional samples. The runs demonstrate application-level UDP timing over
one kernel. A two-host PTP and physical-link measurement remains open.

Run these commands from the DAW directory in separate terminals. For the other
rows, pass `unsynchronized` or `tai` to both processes.

```sh
/usr/bin/python3 tools/plugin_server_latency.py serve --bind 127.0.0.1 --port 9027 --clock shared-system
/usr/bin/python3 tools/plugin_server_latency.py run 127.0.0.1 9027 --count 1000 --payload-bytes 512 \
  --deadline-us 1333.333 --clock shared-system
```

### Isolated link between two endpoints

A second set of 1,000-echo runs used two network namespaces with a veth pair,
IPv4 addresses `198.18.0.1/30` and `198.18.0.2/30`, and no default routes.
Both endpoints ran on one Linux kernel. Each run sent 512 payload bytes per
echo and measured against the 1,333.333 µs block budget.

| Clock | RTT p50 / p95 / p99 (µs) | Lost / budget misses | Forward / return p50 (µs) |
| --- | ---: | ---: | ---: |
| Monotonic RTT | 24.697 / 47.011 / 104.578 | 0 / 1 | — |
| Shared system clock | 85.312 / 148.775 / 375.382 | 0 / 6 | 43.382 / 36.871 |
| Shared kernel TAI | 82.919 / 139.106 / 279.754 | 0 / 4 | 43.398 / 35.795 |

The shared-clock directional jitter p95 was 48.775 / 50.737 µs on the
system clock and 37.488 / 40.743 µs on TAI. These are application timestamps
including scheduling. The VM exposes a KVM virtual PTP clock in sysfs.
A physical PTP trial needs independently clocked machines, PHC access
and a measured bound on clock offset. For a free-running audio peer, a
relative clock error of `ppm` over `seconds` at 48 kHz drifts
`48000 * seconds * ppm / 1e6` samples. Compare this against the mode's phase
tolerance. [Probe source][probe]

### Live playback through a stalled and disconnected TCP link

An independent v13 peer held reply four for roughly 30 ms, resumed audio,
then closed the connection after ten replies. Three runs drove the real ADI
graph for 160 callbacks at 48 kHz and 64 frames across the veth pair. In each
run seven output blocks contained the exact expected wet and dry samples
and 152 contained the dry track alone. The remote device counted 152
missed deadlines in each run, resumed after the stall and named the closed
connection. Its late replies never appeared in a later output block.
The callback allocation audit measured zero. A separate network trace
recorded zero network syscalls on the callback thread and 66 on its worker.

The portable TCP regression uses explicit peer barriers and an ephemeral
loopback port. It checks dry isolation, exact sequence correspondence,
resumption, counted misses and a named failure on close. The test allows
the host scheduler to change the number of missed deadlines.

## For the director

**A. Stock server support in the release.** Keep stock compatibility for the
first trusted-LAN interoperability checks. For the released server connection,
I recommend pairing with the ADI server and using an authenticated encrypted
transport for every session channel. The release choice is whether to expose
stock TCP as an explicitly selected trusted-LAN mode as well.

[upstream]: https://github.com/apohl79/audiogridder/tree/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f
[release]: https://github.com/apohl79/audiogridder/tree/22654e2b9d7d23548e3f65f8d363f662f6c20d0d
[linux-release]: https://github.com/apohl79/audiogridder/releases/download/release_1_2_0/AudioGridder_1.2.0-Linux.zip
[handshake]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Common/Source/Message.hpp#L89-L155
[defaults]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Common/Source/Defaults.hpp#L15-L17
[client-handshake]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Plugin/Source/Client.cpp#L344-L389
[server-handshake]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Server.cpp#L1197-L1276
[worker-listener]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Server.cpp#L1565-L1599
[client-worker]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Plugin/Source/Client.cpp#L394-L459
[worker-accept]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Worker.cpp#L74-L129
[command-frame]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Common/Source/Message.hpp#L930-L1026
[payloads]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Common/Source/Message.hpp#L469-L588
[add-result]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Worker.cpp#L249-L345
[catalogue]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Worker.cpp#L587-L674
[state-messages]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Worker.cpp#L459-L489
[state-encoding]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Processor.cpp#L1181-L1224
[juce-memory-block]: https://github.com/apohl79/JUCE/blob/05ca58e0c451b858ec0e224ffb1db178f0442b54/modules/juce_core/memory/juce_MemoryBlock.cpp#L353-L405
[audio-frame]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Common/Source/Message.hpp#L160-L421
[audio-worker]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/AudioWorker.cpp#L71-L151
[juce-position]: https://github.com/apohl79/JUCE/blob/05ca58e0c451b858ec0e224ffb1db178f0442b54/modules/juce_audio_basics/audio_play_head/juce_AudioPlayHead.h#L317-L502
[formats]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Server.cpp#L844-L871
[catalogue-id]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Processor.hpp#L39-L47
[stock-id]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Processor.cpp#L69-L79
[parameters]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Common/Source/Message.hpp#L838-L855
[sandbox-id]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/Server.cpp#L1278-L1296
[socket-io]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Common/Source/Message.cpp#L24-L123
[discovery]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Server/Source/ServiceResponder.cpp#L118-L138
[streamer-unbuffered]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Plugin/Source/AudioStreamer.hpp#L216-L245
[streamer-buffered]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/Plugin/Source/AudioStreamer.hpp#L267-L300
[licence]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/COPYING
[juce-licence]: https://github.com/apohl79/JUCE/blob/05ca58e0c451b858ec0e224ffb1db178f0442b54/LICENSE.md
[build]: https://github.com/apohl79/audiogridder/blob/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f/CMakeLists.txt#L74-L88
[adi-clap]: ../src/juce/clap_host.hpp
[decisions]: DECISIONS.md
[external]: EXTERNAL-CODE.md
[device]: ../src/juce/device_model.hpp
[schema]: format/schema.sql
[pdc]: ENGINE-DELAY-COMPENSATION.md
[export]: ENGINE-EXPORT-QUEUE.md
[plan]: PLAN.md#dev-27-audiogridder-remote-devices
[probe]: ../tools/plugin_server_latency.py
[stock-fixture]: ../tests/fixtures/vst3_expression_synth.cpp
[surge-release]: https://github.com/surge-synthesizer/releases-xt/releases/tag/1.3.4
[wire-transport]: ../src/juce/audiogridder_transport.cpp
[discovery-engine]: ../src/juce/net_plugin_server.hpp
[discovery-fixture]: ../tests/run_plugin_server_discovery.py
