# Arrangement editing API (N17)

The window can call the owner-thread preparation functions in
[clip_commands.hpp](../src/adi/clip_commands.hpp) and
[material_commands.hpp](../src/adi/material_commands.hpp). They capture complete
typed rows and explicit IDs without writing project rows. Commit through
commitPrepared/commitMaterial, which preserve the captured journal head.
Keep the document generation with an asynchronous result and discard it if the
window has switched/replaced its document, even if its filename is unchanged.

TimePoint declares ticks (base 0) or nanoseconds (base 1). Never pass a sample
index in either field. Pass the actual SessionSpec.sampleRate as referenceRate;
preparation uses the renderer's tempo conversion and rounds absolute boundaries
once. An unwarped source keeps its source window and resampler phase; source,
loop endpoints and retained fade domains are pinned in nanoseconds before
movement. Future rate changes follow those physical clocks. MIDI material
retains its tick-relative offset, loop window, note IDs and opaque streams.

Available entry points:

- copyClip, preparePaste, prepareDuplicate and prepareSplit. The caller supplies
  the new clip ID; child IDs are allocated during capture and stored in the op.
  Clip/range clipboards retain media content identities. Paste refuses a pool
  ID reused for another file after history changes. Hashed media can relocate
  without changing identity; legacy unhashed rows retain locator/dimensions.
  Peaks and media bytes are not copied into the clipboard.
- copyRange and prepareDeleteRange trim every intersecting clip on selected
  tracks. prepareCutRange returns a clipboard and deletion captured in one read
  snapshot; prepareCutClip does the same for one clip. Commit the prepared edit.
  Copy is a value, never an op.
- preparePasteRange replaces its destination span, preserving outside pieces.
  Supply one destination track for each source track, in the same order.
  Destination comp lanes are cleared because they belong to another track.
- prepareDuplicateRange places an independent copy at the selection's end.
- prepareInsertSilence shifts the Set, including tick/ns clips, maps, locators,
  spanning cycle markers, the saved transport loop, arranger sections, declared project end and global
  automation. Crossing clips split. Initial tempo/meter/key events at zero stay
  at zero. The inserted duration for linear objects uses the tempo immediately
  before the insertion; an event exactly there moves after the gap.
- preparePasteRange/prepareDuplicateRange with insertTime=true first insert
  musical time across the Set, then place the captured **material**. They are
  one undo step. The clipboard includes clip envelopes, not global automation
  or tempo/meter/key events. Full Copy/Cut/Delete Time with a global timeline
  clipboard remains open; callers must not label these material-only APIs as
  a complete Copy Time implementation.
- clip.setFades edits either/both fade handles in one op. Length is ticks;
  tension is nullable [-1,1]. NULL selects legacy arithmetic. The snapshot
  carries lengths, curve kinds, tensions and retained fade domains. The window
  can use engine/clip_fade.hpp and engine/curves.hpp to draw the same curve.

Insert Silence retains original nonlinear global automation curves through
automation_segment_ranges, including after repeated insertions. Existing point
IDs (all 64 bits), flags, tension and stream trailer bytes remain unchanged;
new boundary points have fresh captured IDs. Global point ordering remains by
raw time, including coincident points. Future record strides requiring unknown
boundary fields refuse. Clip envelopes are copied unchanged; their playback
is still the separate phase D engine item.

Split/copy of an attached warped clip and split of aliases return named
refusals: the main renderer does not play those sources yet. Splitting through
a fade retains the original envelope on both pieces, with no new internal
edge fade. A subsequent handle edit replaces that side's retained domain;
undo restores it exactly. The captured clip.join op restores its split; this
API does not promise arbitrary source joining or consolidation.

ImportOptions controls copying into the project and optional edge fades; both
default off. A clean audio import probes sample rate/channels/frames, hashes
and deduplicates media, attaches a playing clip and pins the source clock, all
as one undo step. Copy-in stages and verifies the file, then publishes without
overwriting immediately before SQL commit. Failure rolls back rows and removes
only a file this attempt published. Undo retains the owned media file for redo.
Warp=true refuses before mutation because Auto-Warp detection/bar markers and
the warp renderer are not built. Video likewise has no playable attachment
backend; it is not accepted as a silent audio clip.

prepareBounce captures a private SQLite backup including the live WAL, renders
through the existing independent offline runner, then returns a prepared
import. Its source/range/tap/encoding come from render::Job. An explicit replace
selection gives Bounce in Place; otherwise the new clip is additive. It never
processes the live Session. Keep host loaders and serviceHost on the thread
their plug-ins require. Cancellation, a changed journal head, file collision
or failed SQL commit leave no clip or published file. Completed media remains
available for undo/redo. This is a synchronous owner-thread API: use a separate
worker-owned project connection (and a host-compatible thread/process), then
post the value back for commit on the live project's owner. Do not run it on
the audio callback or block the window while rendering.
