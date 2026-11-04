# Cue, Solo in Place and exclusive reference (N23)

The window commits these performance ops, then refreshes ProjectView and Session:

| Control | Op / payload | Snapshot read |
|---|---|---|
| Solo / Cue | `cue.setMode {on}` | `cue.cueMode` |
| Cue button | `cue.setTrack {id,on}` | `cue.selectedTracks` (sorted ids) |
| Cue Out | `cue.setOutput {first}` | `cue.firstOutputChannel` |
| Cue Volume | `cue.setVolume {db}` | `cue.volumeDb` |
| Solo in Place | `mixer.setSoloInPlace {on}` | `cue.soloInPlace` |
| Reference tap/hold latch | `cue.setReference {on,id?}` | `cue.referenceOn`, `cue.referenceTrack` |

In Solo mode the row's existing `track.setSolo`/`soloed` path remains. In Cue mode
its button emits `cue.setTrack` and reads `selectedTracks`; it does not overwrite
a track's stored solo flag. Main's live monitor ignores those stored solo gates
while Cue mode is selected; switching back restores them. Solo in Place controls
whether a soloed track's returns stay heard. These controls are transport-scoped,
ephemeral audit rows and saved session metadata, outside undo. Session defaults
are Solo, Solo in Place on, no cued tracks, Cue Volume 0 dB and stereo pair 0.

Cue is Post FX / pre-fader listening, ignoring a track's Main mute/fader. Cue
selection changes use 5 ms ramps. Reference audition retains its source's fader,
pan and volume/pan automation, ignoring Main mute and mute automation. Engaging
it makes a complementary 5 ms swap from Main to reference; releasing retains the
reference source through the ramp back. While engaged it replaces other Cue
signals, including the metronome. Cue Volume (-150..+6 dB, ramped) applies to
both cue and reference. Main's own effects/fader never touch the reference path.

`first` is a zero-based even stereo-pair index, 0..62: 0 is outputs 1/2, 2 is
3/4. The graph remains stereo when the hardware exposes more channels. Normal
Cue goes to that pair; exclusive reference goes to monitor outputs 1/2 and Cue
Out. With a shared pair each signal is added once. Unavailable external channels
receive no fallback onto a different pair; the window can compare the selected
pair against its device's enabled outputs.

The monitor feeds reuse track audio already rendered by the graph. Each has a
private audio-only gain and a sink whose sidechain clock is anchored to Main's
output. This aligns cue with compensation and track delay while reusing the
existing device processing. Stable edge keys carry cue delay history through
rebuilds. Reference automation uses its own publication: lane ownership survives
failed/skipped rebuilds and is reclaimed only after that audio reader moves.
A newly created reference feed fills its compensation delay before the Main
fade starts, preserving the project until aligned reference audio is ready.
All allocation, graph work, JSON/SQL and reclamation remain off audio.

The role format is N38, owned by win. When a `tracks.role` column is present,
`readCueSettings` finds its native reference and exposes `projectReferenceTrack`.
That role excludes its own output/sends from Main and supplies the default
reference target. A zero saved preference follows the role even after unrelated
Cue edits; `referenceFromRole` distinguishes that from an explicit audition id.
No role DDL/version/migration is introduced by this PR. On the current schema an
explicit id already provides exclusive audition; choosing an ordinary track as a
performance target alone does not change its Main route while audition is off.
The N38 window integration can use the native role once its format/snapshot op
lands. The synthetic role column in this suite tests reading only, not migration.

Offline export/analysis uses `SessionSpec::performanceState=false`: no cue,
reference swap or click, and normal stored solo/mute behavior. A native project
reference role remains excluded from Main. Cue selections never turn into file
render solo flags. Explicit audition preferences alone do not alter file audio.

N13 merge order: meter Main immediately after `graph_.process(block)`, before the
cue monitor mixes reference/headphones/clicks. Mathematical renders, state and
lifetime tests are headless; measured Live sound/level parity and actual interface
routing still belong to the platform/parity gate.
