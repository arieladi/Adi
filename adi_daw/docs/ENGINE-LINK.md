# Link SDK audio-driver boundary

`engine::LinkSession` owns one actual Ableton Link SDK participant. Create and
retire it on the application owner; retain it through the last audio callback.
The private implementation hides all SDK types from engine consumers. The
official SDK is pinned to **Link-4.1.1**, commit
`5ba907ae027a8c2ec8824463e37296dcefc5e9be`; its Asio gitlink is
`8806a6803cde7054c3049d3666d3ec36786568c5`. GPL-2.0-or-later and Boost-1.0
licenses were read at those commits. `fetch_external.sh --build-only` fetches
both, and CMake requires the new Asio root include tree (1.38.2). The SDK
configuration is scoped so its C++17 default cannot downgrade ADI's C++20
targets. This is musical synchronization,
separate from PTP sample-clock synchronization, audio streaming or collaboration.

Link Audio remains off. This translation unit uses `ableton::Link`, never
`LinkAudio.hpp` or an audio-sharing participant. Compile-time macro/type guards
require the SDK's musical-only controller. The SDK and Asio stay private to the
adapter; no new audio-sharing API or setting is exposed.

Networking starts **disabled**. Only explicit owner `enable(true)` joins the
network. Start/stop sharing is separately off until `enableStartStopSync(true)`;
participants can share tempo/phase while retaining independent transport intent.
Neither owner control nor participant construction is a callback operation:
misuse refuses before allocation, configuration or diagnostic mutation. Never
retire a participant from the audio callback. The SDK owns and joins its own
discovery/dispatch threads; application code does not install SDK-thread UI
callbacks or call the SDK's blocking application-state APIs.

One audio driver calls `process(LinkBuffer, LinkCommand)` once per buffer. Supply
`callbackMicros` in **Link's monotonic clock domain** (`clockMicros()`), plus the
physical first-output latency in microseconds. The adapter captures one SDK
audio state, applies optional tempo/transport/beat requests, observes that same
state at the first audible timestamp, then commits it through the SDK audio API.
There is no wall-clock conversion, SQL, graph rebuild or per-buffer memory
management here. Tempo/phase/beat and transport intent describe the same captured
generation. A frame is returned by value and must be handed to the application's
usual driver/publication path, not retained as a live SDK state.

Commands are explicit and consumed by that call; the adapter creates no UI-to-
driver queue. Tempo zero leaves shared tempo alone; an explicit tempo must be
finite and within the SDK's 20..999 BPM range. Quantum is 1/1024..256 beats, beat
requests are finite within +/-1e9, callback timestamps within +/-2^50 us, and
nonnegative output latency at most ten seconds. The whole command validates
before capture or any mutation; invalid input returns `valid=false` and retains
the previous actual SDK state. These are adapter representability bounds, not
claims of a particular audio device's supported latency.

`requestBeat` uses the SDK's peer-aware request API. Alone, the requested beat
maps immediately; with peers it maps to the next matching shared quantum phase.
`LinkFrame::launchMicros` reports that scheduled beat time. It does **not** force
the shared phase. `requestedPlaying` is transport **intent**, with
`transportChangeMicros`, not permission to start samples early at a pending
quantized launch. The caller must schedule the reported launch/stop correctly.
The Link musical timeline continues while transport is stopped. Beat magnitude
can differ between participants; their phase and tempo synchronize. Network
tempo is quantized to integer microseconds per beat, so peer comparison needs
that precision rather than exact floating-point BPM equality.

The native performed-history consumer now integrates these frames with Session
transport, prepared audio/MIDI and effective tempo; see
[ENGINE-LINK-PLAYBACK.md](ENGINE-LINK-PLAYBACK.md) for publication, source readiness,
continuity and held-note ownership. This SDK re-pin preserves that consumer.
UI settings/device-clock integration and the director's LinkHut/Live latency
measurement remain their stated work; passing the wrapper is not measured
D11/UI/Live parity.

The default native test never enables networking and uses the actual SDK to
exercise clocks, negative phase, tempo continuity, transport independence,
atomic refusal and 10,000 capture/edit/commit callbacks. The optional
`adi_link_session_tests --peers` explicitly opens two actual participants for
discovery, shared tempo/phase, quantized launch, opt-out start/stop and 20,000
enabled-peer captures. It needs network access and does not silently skip failed
discovery. Callback new/delete and file-I/O metrics are audited; Linux additionally
wraps direct pthread mutex lock/trylock calls linked into the test. This audit
does not inspect private locks inside shared system libraries. The engine log
records the exact compiler, network environment, fault and sanitizer evidence.
