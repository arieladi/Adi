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

## For the director

**A. Stock server support in the release.** Keep stock compatibility for the
first trusted-LAN interoperability checks. For the released server connection,
I recommend pairing with the ADI server and using an authenticated encrypted
transport for every session channel. The release choice is whether to expose
stock TCP as an explicitly selected trusted-LAN mode as well.

[upstream]: https://github.com/apohl79/audiogridder/tree/76dd9a006a86110b9245bc44f7a6c78f3d6a5c9f
[release]: https://github.com/apohl79/audiogridder/tree/22654e2b9d7d23548e3f65f8d363f662f6c20d0d
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
