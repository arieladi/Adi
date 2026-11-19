# Device sample assignment (N28)

This branch depends on engine/library-devices (N15); merge that first.

The window calls prepareSampleAssignment(store, deviceId, declaredSlotId, file,
engineRate, importedUtc, optionalDecodeCache) off the callback, checks its own
document generation, then calls commitSampleAssignment. Color Cab declares
slot 1, Impulse. The producer refuses unavailable backends and undeclared slots.
A supplied decoding cache supports the existing non-WAV import formats and its
configured limits. Without one the file must already be a supported WAV.
Preparation imports by media content hash, decodes, resamples and calls the
existing public Color Cab preparation SDK. No DSP production code changes.

The resulting journal batch is one undo step. Only captured media fields and
state hashes enter it. A first assignment captures an explicit empty baseline
before its new state, so applying undo through the existing ParamOps.appliedState
path also clears the live slot before its absent row is restored. Later
assignments replace the existing state directly. Clear uses prepareSampleClear.
Notify the live device through ParamOps.appliedState for each device.loadState
payload, as for existing state edits; notify inverse payloads in undo order and
forward payloads on redo. Reopening a Session restores the same state normally.

The sample:<declared-id> plugin_state role holds a content-addressed CBOR blob.
It contains kind=adi.builtin.sample, version=1, UID, slot, empty, and for occupied
slots mediaHash, sourceRate, source_f32le (mono float32 little-endian), and the
derived rate/kernel/target. The original-rate mono backup permits preparation
at another engine rate when the original file is absent. The derived profile
is restored directly at its saved rate. Unknown CBOR fields survive a save.
The backup is limited to 8M input frames and 8M resampled frames; oversized,
nonfinite, corrupt or unsupported state fails before publication. The source
hash is verified against the captured import to catch changes during decode.

The host publishes prepared samples through PdSampleSlots, retaining the old
buffer until the audio reader has advanced. It never decodes, designs, allocates
or opens a file on the callback. Changing the engine rate prepares all samples
before changing the live device. An empty slot publishes an identity kernel.

Snapshot.sampleSlots exposes deviceId, slot, name, UID, stateHash, mediaHash,
optional mediaId, empty, missing, hasDerivedState and problem. Immutable metadata
is decoded only when its state hash changes (or an earlier read failed). Each
rebuild refreshes pool identity and the existing locator's presence off the
callback; missing also respects the pool's explicit missing flag. It does not
rehash a whole file on every edit. A present file at a stale locator can still
fail the ordinary content verification at the next import/relink. All fields
participate in structural sharing, and old frames remain immutable. The window
can use this snapshot rather than opening the project file itself.

State blobs are immutable content published before the journal, following the
existing ParamOps.writeBlobs convention. Stale preparation refuses before
publication; a race or later journal failure can retain an unreferenced content
blob. The expected head is also checked inside the journal transaction, so no
partial media/state references or undo step survive a failed commit. Source file
and document-generation guards apply as for other asynchronous producers.

Hybrid Reverb remains unavailable through the native factory; this does not
claim its convolution backend or any unbuilt instrument is supplied. Color Cab
is the current public sample-slot consumer.
