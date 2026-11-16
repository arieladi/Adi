# Addable native Clean compressor — DF-M28

Users can add **Compressor** from the device area's **+** menu, edit its Clean
controls during playback, see the real native device in the tab/STACK chain,
remove it and Undo, and reopen the project with its authored settings. It is an
internal-key, zero-latency processor over the shared Clean core; no plug-in is
hosted inside it.

| Stable ID | Control | Fresh value |
|---|---|---|
| 0 | Threshold | -18 dB |
| 1 | Ratio | 4:1 |
| 2 | Knee | 18 dB |
| 3 | Reduction Range | 60 dB |
| 4 | Attack | 0.250 ms |
| 5 | Release | 200 ms |
| 6 | Auto Release | Off |
| 7 | Auto Gain | On |

IDs are numeric automation identities, not names or visual positions. Future
controls must not repurpose them. Normalized units remain the common automation
wire domain; ratio and time controls are nonlinear. The timing display domain
is distinct from the engine's coupled envelope coefficients. Automatic Release
shows its percentage, and maximum Ratio shows Inf:1. No external-key route,
Hold, key-EQ, other style, oversampling or lookahead control is advertised by
this bounded device. These will join as separately usable capabilities.

## Audio and lifecycle

Mono/stereo and float/double I/O use the same float-signal/double-envelope engine.
A normalized-control cache avoids remapping unchanged settings on each segment.
There is no per-device worker, timer, meter service or callback allocation. The
ordinary input-copy pass turns non-finite/float-unrepresentable samples into zero
before recursive state; finite signals keep their authored processing path.
Overlapping output/partial input aliases refuse before admitting a logical core
block. Identical-format preparation preserves a running envelope; release then
prepare starts a new stream. Width changes need no distinct filter backend.

NodeIo supplies segments rather than the physical callback total. Each segment
is a checked logical Clean block; this core has no callback-cadenced hold/latency
plan to restart. Whole versus 17/47 split processing and live control changes
are checked sample for sample. A later component with callback-dependent state
must not copy this adapter assumption without the physical block contract.

All eight controls use common automation, exact-runtime gesture, undo/redo,
parameter-publication and state authority. State is versioned CBOR of authored
settings; it retains unknown envelope-independent fields, validates all values
before committing and never serializes runtime pointers or transient DSP memory.
A project stores the state itself, not a dependency on an external preset file.
Hidden views request no control metadata/value/text work. Visible unchanged text
is cached, including Release's mode dependency.

## Validation and limits

The native factory/document tests cover actual insertion, controls, automation
binding, audio, split blocks, repeated preparation, invalid buffers/state,
remove/Undo and reopen. Generated rate coverage extends through 768 kHz; this is
not full reference validation at every host rate. Software-peer component tests
cover the real native tab, eight controls, latency tooltip, a real gesture/Undo
and the default With Tools STACK row. They create no OS window or audio device.

Selected full-array audio comparisons are retained in the director's private
research folder, outside public sources/tests. New CPU timing is deferred; no
CPU parity percentage or live visual acceptance is inferred from allocation-free
callbacks or software peers. Claude performs live playback, pointer/focus and
application acceptance; win reviews/merges the slice.
