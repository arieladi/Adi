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

- copyClip, preparePaste and prepareDuplicate. The caller supplies the new
  clip ID; child IDs are allocated during capture and stored in the op.
  Clip/range clipboards retain media content identities. Paste refuses a pool
  ID reused for another file after history changes. Hashed media can relocate
  without changing identity; legacy unhashed rows retain locator/dimensions.
  Peaks and media bytes are not copied into the clipboard.
- prepareSplit has one-cut and two-cut overloads. One cut takes sourceId,
  rightId and TimePoint; two cuts take sourceId, middleId, rightId, begin and
  end, followed by referenceRate. Both endpoints use the same explicit input
  clock and must remain ordered and strictly inside the stored clip after
  conversion. The left piece keeps sourceId; the other supplied IDs must be
  distinct and unused. The two-cut overload prepares both cuts in memory from
  one captured image and one read snapshot, returning one clip.split request.
  The middle piece is exactly the selected span. To deactivate that span,
  append clip.setMute for middleId to the prepared requests and commit once;
  both cuts and the mute share one expected-head check, transaction and Undo.
  A boundary on an existing edge uses the one-cut or no-split path instead.
- copyRange and prepareDeleteRange select intersecting main-lane clips
  (lane_id IS NULL) on the selected tracks. Take-lane clips are not selected.
  prepareCutRange returns a clipboard and deletion captured in one read
  snapshot; prepareCutClip does the same for one clip. Commit the prepared edit.
  Copy is a value, never an op.
- preparePasteRange replaces its main-lane destination span, preserving
  outside pieces and independent take-lane clips. Supply one destination track
  for each source track, in the same order. Copied material lands on the main
  lane; source take-lane bindings are not destination bindings.
- prepareDuplicateRange copies selected main-lane material to the selection's end.
- prepareInsertSilence shifts the Set, including tick/ns clips on every lane,
  maps, locators, spanning cycle markers, the saved transport loop, arranger
  sections, declared project end and global automation. Crossing clips split. Initial tempo/meter/key events at zero stay
  at zero. The inserted duration for linear objects uses the tempo immediately
  before the insertion; an event exactly there moves after the gap.
- prepareDeleteTime removes a musical span across every track and lane.
  Linear objects
  move by the original integrated tempo duration. Crossing clips are trimmed;
  source and fade clocks remain pinned. The join keeps the old end's tempo,
  meter and key, including its tuning and degree overrides. Locators, cycles,
  arranger references, declared end and the saved loop follow the cut. A loop
  wholly removed is disabled, with its old positive bracket length at the join;
  this particular Live behavior has not yet been measured.
- preparePasteRange/prepareDuplicateRange with insertTime=true first insert
  musical time across the Set, then place the captured **material**. They are
  one undo step. The selected material is main-lane only; insertion still
  shifts existing clips on every lane throughout the Set. The clipboard includes
  clip envelopes, not global automation or tempo/meter/key events. These remain
  material-only APIs.
- copyTime, prepareCutTime, preparePasteTime and prepareDuplicateTime operate on
  the whole Set, including its global timeline. TimeCopy captures clipped
  material from every lane on every track, including hidden and grouped tracks,
  each map's carry/interior events, tuned-key bindings/degree overrides,
  locators/cycles, clipped arranger sections and every global automation lane.
  Cut captures the clipboard and deletion under one read snapshot; Duplicate
  pastes at the source selection's end. Paste inserts the copied musical span
  with its original integrated physical duration, then restores the target's
  tempo/meter/key and automation at the far join. It also works at tick zero.
  Existing arranger playlist references follow the insertion; copied sections
  do not duplicate that playback playlist. Existing clips shifted by Time
  commands retain their lane bindings. This does not add take-lane clipboard
  placement: preparePasteTime still places copied material on the main lane.
  These clipboards currently target the same project and captured track ids;
  missing tracks or changed kinds refuse. Media and tuning content identities,
  the full automation lane contract and the reference render rate are checked.
  A changed lane contract or render rate requires a fresh copy. Cross-project
  lane/track mapping is not implicit. All native ids/defaults are captured in
  the one resulting arranger.splice packet; unchanged outside automation keeps
  its point ids/flags/curve domains/trailer, while pasted points get fresh ids.
- clip.setFades edits either/both fade handles in one op. Length is ticks;
  tension is nullable [-1,1]. NULL selects legacy arithmetic. The snapshot
  carries lengths, curve kinds, tensions and retained fade domains. The window
  can use engine/clip_fade.hpp and engine/curves.hpp to draw the same curve.

Range removal preserves a sole surviving piece's clip ID and existing material
child IDs. A surviving tail is trimmed in place, retaining source/loop/fade clocks,
opaque note/expression bytes and external aliases to the original clip. If both
sides survive, the left keeps the original ID and the right gets a captured new
identity. A sole prefix is also trimmed in place; no discarded piece consumes
clip or child IDs. This also applies before Delete Time relocates the retained pieces.
Undo restores complete original rows, including previously absent clock rows.
Complete deletion still captures and detaches external aliases for referential
integrity; undo restores those links, including aliases outside the selected lane.
Main-lane material scope follows PLAN ED-01 and ADR-0225; whole-Set Time scope
continues to follow PLAN ED-02.

Insert Silence, Delete Time and the whole-Set clipboard retain nonlinear global
automation curves through automation_segment_ranges, including after repeated
edits. Delete Time uses coincident boundary guards to preserve the two original
values at the join. Existing point
IDs (all 64 bits), flags, tension and stream trailer bytes remain unchanged;
new boundary points have fresh captured IDs. Global point ordering remains by
raw time, including coincident points. Future record strides requiring unknown
boundary fields refuse. Clip envelopes are copied unchanged; their playback
is still the separate phase D engine item.

Split/copy also supports attached Warp audio. Its retained fade domains use
source musical ticks (schema 1.13); unwarped audio retains nanoseconds. The
source map and window stay unchanged, and a new placement follows its tempo.
Aliases still return a named refusal. Splitting through
a fade retains the original envelope on every piece, with no new internal
edge fade. Two cuts retain the same source, loop and fade domains on all three
pieces; only their visible spans and source-relative offsets change. A subsequent
handle edit replaces that side's retained domain;
undo restores it exactly. The captured clip.join op restores its split; this
API does not promise arbitrary source joining or consolidation.

ImportOptions controls copying into the project and optional edge fades; both
default off. A clean audio import probes sample rate/channels/frames, hashes
and deduplicates media, attaches a playing clip and pins the source clock, all
as one undo step. Copy-in stages and verifies the file, then publishes without
overwriting immediately before SQL commit. Failure rolls back rows and removes
only a file this attempt published. Undo retains the owned media file for redo.
Warp=true analyzes source onsets, captures a tempo/bar map and creates a
musical clip played by the prepared warp backend (ENGINE-WARP.md). ImportOptions
captures the six-mode selection, tempo hint/optional source BPM, meter,
tempo-change detection, From First Beat versus Sample Start and cancellation.
An indeterminate beat refuses before project mutation; provide a source BPM
for short or non-percussive material. Bounce always creates Warp-off audio,
regardless of the import preference (ADR-0203 d16).
For an existing clean import, prepareSetWarp in warp_commands.hpp analyzes
and captures the Warp button's action. Enabling keeps its clip identity and
captures a musical source length; disabling restores physical duration and
source pins. Edited source offsets, loops, aliases, End markers, reverse,
pitch and trimmed extents give named refusals in this producer. Keep the
document generation and commit with commitPrepared; redo never analyzes.
Video still has no playable attachment
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
