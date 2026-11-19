# Native sample Save and Recall

On the serialized project owner, `clip_edit::captureSampleDefaults(store, clipId,
referenceRate)` reads an actual audio clip and its media identity in one native
read transaction. `saveSampleDefaults(..., libraryIndex, error)` persists that
owned capture under the source BLAKE3 in the application library. A read-only
project can Save: neither call changes project rows, operations or history.
The reference rate is the heard session rate used by the existing `copyClip`
source-clock/fade freezing. Legacy unwarped musical End markers are folded into
fixed source End time; existing earlier source End pins remain authoritative.
Warp maps retain their original binary bytes, including readable future layouts.
Time signatures accept the native numerator 1..255 and power-of-two denominator
1..128, matching `clip_properties` rather than imposing a narrower library limit.

The explicit four-argument `prepareImportAudio(store, options, referenceRate,
libraryIndex)` probes and hashes the real imported source, then reads its authored
settings. Its sample rate, channel count and total frames must match. Missing
settings use normal import. Invalid, unsupported or mismatched authored data
refuse the preparation without project writes; they never silently become a
default import. Supplying authored defaults overrides automatic Warp analysis,
including an explicit saved Warp-off choice. The established three-argument
overload imports without consulting a library.

The new clip keeps the requested identity, track and placement. Its source
window, Warp/pitch/formant/reverse/channel controls, gain/mute, loop, source End,
fade curves/tensions/clocks and signature come from the saved settings. The
initial arrangement extent runs from content offset to the source End or next
loop boundary; the old arrangement rectangle, name, color and local scale
context are project choices. Extending a recalled loop remains a separate edit.
Unwarped source clocks stay fixed across destination tempos; musical Warp clocks
project into the destination tempo. Pinned fade clocks remain authoritative and
legacy handle coordinates are expressed in the destination tempo. Ns-only loops
receive usable legacy musical handle coordinates for subsequent native copying.

Preparation owns all captured values in ordinary native media/import and clip
image operations. No library pointer survives into commit, history, replay,
graph rebuild or playback. Changing/closing the library after preparation does
not change the result. The normal expected-head/source-store checks, private
copy publication, transaction, rollback/retry, one-group undo and CBOR replay
remain the material command's existing responsibilities. No new schema, opcode
or project format is introduced. Callback capture/Save/import refuses before
allocation, freeing, SQLite, file I/O or caller diagnostic mutation. Applying an
image is an internal owner-only helper, not a callback operation.

Capture requires complete positive source metadata and a lowercase content hash
already belonging to the project media row. It can Save an offline source with
that metadata; it does not re-read the physical source on Save. Import always
validates its actual source through the existing material path. Audio aliases
still need the existing alias-resolution workflow; direct capture refuses them.
Project/device clip automation is not portable and refuses capture. Unknown
native audio/playback/fade/property fields also refuse instead of being dropped.
Placement, project identities, selection, name/color and project scale context
are not sample defaults. Authoritative data lives in `data/library.sqlite`,
separate from optional analysis/decoded cache cleanup; see
[ENGINE-SAMPLE-SETTINGS.md](ENGINE-SAMPLE-SETTINGS.md) for persistence and exchange
bounds. This is an engine API handoff; no UI button or application-library
lifecycle wiring is added here.

The native workflow suite exercises actual stores, read-only Save, library
cleanup/reopen and closure before commit, both playback precisions at 44.1/48
kHz, a different destination tempo, curved fades, Ns-only loops, End markers,
future Warp bytes and audible Re-Pitch duration/frequency. Actual native history
and CBOR replay work without a library. Late COMMIT failure retires an owned
published copy and allows the same captured import to retry without losing its
source or saved data. Unsupported future annotations remain byte-for-byte intact.
Compiler/platform and adversarial evidence belongs in the engine log; these
tests make no measured Live or unrun platform claim.
