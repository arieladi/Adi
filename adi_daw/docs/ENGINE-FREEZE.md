# Whole-track freeze backend

This first increment freezes whole regular audio, MIDI or instrument tracks,
including their explicit tail, with separate dry (Pre FX) and wet (Post FX)
files. Original clips, device order, enabled switches, parameters and opaque
state remain intact. Input gain and inserts are baked; fader, pan, width, phase,
track delay, mute, solo, outgoing sends and parent console processing stay live.
The cached files play with Warp off and adapt to the playback sample rate.
Float32 is the default cache encoding, including in a 64-bit graph: the file is
an encoding boundary, so this does not promise a bit-exact 64-bit null.

The producer refuses partial-track ranges, locked/already frozen tracks,
incoming audio or sidechain edges, and MIDI material forwarded without its own
instrument. Freeze All Channels with live keys/group balance and live instrument-prefix
caches remain subsequent increments. Whole-group mode is described below. A static wet file cannot
respond to a changed key or independently changed child balances. The lower
capture/fingerprint helpers do not enable those native modes.

## Window integration

Flush the current edit capture before starting the job.
Run render::captureFreezeAudio on a connection owned by the rendering worker
(a read-only source connection is sufficient). Set FreezeCaptureSpec.tracks,
endSample (exclusive, from zero through all material plus the chosen tail),
session, and documentGeneration. The caller supplies its tail preference; the
worker does not infer it from a plug-in's reported tail. Capture uses one
WAL-inclusive database backup, private verified copies of active audio assets,
one Session and one processing pass for all dry/wet streams. The live Session
is never rendered. Original-file changes during progress cannot alter the
private input generation. Progress/service/cancellation run off the callback;
private copying checks cancellation every 64 KiB.

On the writable document owner, call freeze::prepareFreeze(store, capture,
currentGeneration, firstMediaId, importedUtc), then commitFreeze(store, edit,
currentGeneration). Reserve the explicit media-ID range for at most two files
per participant. Retain the job/document identity across the worker handoff.
The preparation and commit check generation, resolved SQLite filename and
captured history head. Preparation also compares the current causal state to
the state actually rendered, even if a direct SQL edit did not move history.
The private edit cannot be modified by the UI. Stop the transport before
applying the resulting cache topology to the live Session, and wait for a
stopped callback (or driver release). Session reads an atomic driver observation
for this guard; the document owner never reads Transport concurrently. A warm
unfreeze creates fresh instances from preserved state, keeping old instances
retired for graph lifetime safety so stale voices and delay history cannot resume.

unfreeze(store, trackIds) is another one-step native command. It checks source
fingerprints and accepts an installed-identity callback; compare the captured
ordered IDs/format/UID/vendor/version against the actual installed devices.
If state or installed versions changed externally, report it before allowing
an explicit acknowledgeChanged unfreeze. Without that callback, only stored
source identity is verified. No device is loaded by the native operation.
Unfreeze retains cached media for history and restores the original arm switch.

Snapshot TrackNode exposes frozen, freezeMediaId and freezeMode; every field
participates in structural sharing. Mode is wet, unverified for a legacy flag
without provenance, or empty. It describes stored metadata; successful Session
loading validates the cache. Original clips remain visible in the snapshot.
Disable edits to baked source material while frozen, offering Unfreeze first.

## Persistence and failures

Schema 1.17 adds frozen_renders. Each row holds pre/post media IDs, frames,
sample rate/channels, source fingerprint, installed identity CBOR and original
arm state. Older minors migrate additively; read-only older projects get an
empty temporary table. The new track.setFreeze operation carries captured
before/after states and swaps them for its inverse. Media import/relink and all
selected track states form one transaction/undo step. Track-delete undo also
preserves this new dependent row through the generic N37 capture.

Native commits, history changes and Session loading validate causal source
fingerprints. Live scalar controls use a guarded fast path after an already
verified state; direct SQL, another connection's writes, unknown operations,
commit hooks and source-table triggers force validation again. SQL apply never
reads files or loads a host. Cache hashes are verified off-thread when opened;
parked original audio files and plug-ins are not opened. Legacy unverified
freeze flags refuse playback until explicitly unfrozen.

Publication never replaces a destination. It publishes a separate private copy,
keeping the original renders for recovery. If SQL COMMIT fails after files were
published, only files created by that attempt are removed; the same prepared
job can retry. A prior pooled file is reused only after a full hash match and
is never removed on failure. Last-owner cleanup removes only the job's private
directory, after its readers and worker Session have closed.

## Freeze Group

Set wholeGroups=true and select non-overlapping group roots in
FreezeCaptureSpec. This records each complete group bus once, including its
child balances, internal sends/keys, console halves and group inserts. Children
keep their original clips/state in the file but their sources, devices and
audio routes are removed from the realized graph. The group fader, pan, sends
and parent console remain live. Child audio routes to outside the group also
stop until unfreeze: show this warning and set acknowledgeChildAudioRoutes=true
only when the user chooses that operation. The saved routing rows are never
rewritten. Source changes inside the parked subtree require unfreezing first.

Capture requires Solo off, no externally driven audio/sidechain inputs into the
subtree, and no independently frozen children. Baked VCA controls must be within
the group. MIDI forwarded out of the group without an included instrument is
refused. These boundaries prevent a static cache from silently replacing a
partial or live-input mix; live-input modes remain follow-up work. Empty and
nested ordinary groups are valid. A master track cannot belong to the scope.

Use the same prepareFreeze/commitFreeze/unfreeze calls. Schema 1.18 adds
frozen_groups with captured descendant IDs and original arm switches, and
group.setFreezeScope records them beside the root cache in the same transaction.
Undo, redo, reopen and generic track-delete undo retain that metadata. Unfreeze
restores child arm switches. A root deletion retains the surviving children's
current disarmed state; undo restores the entire frozen group.

Snapshot children expose frozenByGroup (zero when absent), frozen=true and
freezeMode=group-member. The active group root has mode group. The child's own
freezeMediaId stays empty; look up its owner to find the group cache and unfreeze that owning group. Every field
participates in structural sharing, including ownership changes between nested
groups. Parked child readers are filtered before constructing audio/MIDI
sources; missing original media and plug-ins therefore do not block playback.
The original graph is planned first, then its nodes/edges are filtered and
indices remapped, preserving explicit routing rather than inventing defaults.

Verified cache resolutions are pinned in the private playback model, including
original-hint/collected-media fallbacks. Later opens check file size/modification
time and reverify changed files; unchanged immutable cache files avoid hashing
the full audio on each live fader rebuild. Serialized hints and the UI model
are preserved. New captures exclude parked group originals from input copying,
so unrelated tracks can still be frozen with those originals offline.
