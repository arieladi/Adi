# Built-in browser and loader (N15)

The app-scoped file library is already implemented in `adi/library/index.hpp`:
open `appdata::libraryIndexFile()`, scan a caller-identified volume, read
`entries()` before hashing completes, and run the cancellable low-priority
worker. It accepts samples and preset files as files; JSON metadata is keyed by
content hash. It never mutates the open project. See ADR-0147 for volume and
case rules. The existing plug-in registry remains the installed VST3/CLAP index.

For ADI devices, `builtinCatalog()` gives stable UID, category, folder, name,
input count and availability. `prepareBuiltinInsert(store, uid, trackId,
beforeDevice)` resolves or creates the internal plug-in reference and track
chain. Omit beforeDevice to append. Commit through `commitBuiltinInsert` after
checking the window's document generation; the producer also guards source
file and expected history head, including the empty root. It produces one undo
step and normalizes legacy tied/large ordinals with exact inverses. Preparation
is a read snapshot and performs no project writes. It can insert in a nested
chain when its selected device belongs to the target track.

`makeBuiltinDevice` hosts the existing canonical native factory without JUCE or
libpd patch loading. `withBuiltinDevices(externalLoader)` composes it with an
external-format loader; JuceDeviceLoader also dispatches internal rows here.
The host advertises declaration IDs, real ranges, curves, units and control
shapes, stages normalized parameter intent atomically, passes transport/MIDI,
and reports changing native latency. Callback processing allocates no memory.

The opaque `state` role is CBOR `{version:1,uid,params:{id:normalized}}`.
Unknown root/parameter entries survive a save; invalid known state refuses
before changing any parameter. Standard plugin_state/state_blobs persist this
role, and the Session's plugin_params mirror remains authoritative on restore.
Sample-slot state and assignment are N28, a follow-up on this loader.

Seventeen existing native effects are available with ADI_WITH_PD. Hybrid Reverb
is listed unavailable because it has no entry in the native factory; it is not
silently replaced. Without that backend all catalogue entries remain readable
but unavailable, and project loading retains ordinary missing-device state.
This does not certify device parity or supply instruments not yet in the
canonical factory. UI category rendering and library lifetime remain window
work; no UI production code is changed here.
