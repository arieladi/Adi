# Dedicated track bounce commands

`adi/bounce_commands.hpp` supplies document-owner/worker producers for ADR-0203
d16. They build on the phase-C material renderer and complete typed clip/range
capture. They do not process a live driver Session. Keep the Store on its owner
thread, honor the device loader's thread contract, and retain the current
document generation with the job. Both commands capture the actual project
connection filename and journal head; commit refuses another connection/file,
replacement document generation, or an intervening native edit.

`prepareBounceToNewTrack(Store&, TrackBounceOptions, loader, control)` captures
the selected regular track or group, renders its direct post-fader wet output,
and prepares a plain audio sibling. Supply explicit source/new track, clip,
media and, when the source has a user Main route, new routing identities.
The new sibling follows the source; later siblings shift together in the same
undo step. It inherits the parent and palette color and retains the source's
explicit Main destination. Name defaults to the source plus ` (Bounce)` and
also names the new clip. `muteOriginal` defaults true and mutes the ENTIRE
original track; callers wanting it to remain live must set false. This is a
direct output render, not an isolated mix through Main. Sends, return effects
and sidechain connections are not cloned. It does not promise to replace every
parallel route of a source. The output clip starts at the job's begin sample,
converted to nanoseconds with a checked sample round trip, and plays Warp off.

`prepareBounceInPlace(Store&, InPlaceBounceOptions, loader, control)` renders a
whole regular track before its fader, replaces all its clips and parks its
enabled inserts bypassed. Their state, params and blobs remain intact. Baked
input gain resets to zero, and an armed source disarms. The fader, pan, width,
track delay and output routing remain live. Device bypass keeps latency under
ADR-0203 d4; its graph delay line remains compensated and source device DSP is
not called. One Undo restores the full original material, enabled state, input
gain and arm flag. This is the whole-chain command: begin must be sample zero,
and the explicit end must include every clip and the caller's chosen tail.
It refuses a partial range, a group, locked/frozen/missing track, or live
incoming main/send audio that would otherwise be added to its already baked
mix. The lower-level `prepareBounce` range producer still supplies material
edits; it does not silently disable a whole chain outside a selected range.
Baked material changes what an external Pre FX tap sees; this command is not
Freeze All Channels' promise to preserve separate dry tap streams.

`TrackBounce::ready()/error()` describes preparation. `commitTrackBounce`
requires the caller's current document generation and commits every track,
ordering, route, material and source-state operation as ONE native undo step.
The prepared value retains private files until successful commit or destruction.
The same prepared job may retry a publication or late SQL failure; a completed
job refuses another commit. Existing owned destinations are reused only after
full content verification and are never removed by a failed attempt.

The job supplies an explicit range, integer rate, precision, block size and WAV
encoding/dither. Neither command reads ambient settings. The caller extends
the end for a desired tail and passes the appropriate prompt choices; Warp is
forced off. A selected lossy PCM bit depth or float32 cache has that encoding's
quantization tolerance, even when the render graph runs at 64 bits. This does
not claim binary identity through a narrowing file encoding or a live graph's
transition fade. Freeze, Freeze All Channels, Freeze Group, freeze fingerprints
and their separate source streams remain the next engine item.

Material publication retains its only private source and moves a separate
publication copy. Deferred SQL COMMIT failure can then roll back all rows,
remove only the just-created owned asset, and retry without rerendering. Copy-in
also rehomes a reused valid external media identity into project-owned audio;
Undo restores its still-existing old path. External files left uncopied are
verified again before SQL commit, so changing one after capture refuses the
edit without deleting it. Successful pool files survive Undo for Redo/history.

Tests audition actual outputs through a parent and a different explicit Main
route in both processing widths, with source effects, gain and pan. They cover
source parking without a second effect pass, Warp off, exact native undo/redo
and reopen, hierarchy/order/color, document/head guards, same-job retry after a
deferred FK fault, and preservation of external sources. No schema or opcode
is added: existing native track/mixer/device/media/clip operations form the
transaction, as described in OPS.md.
