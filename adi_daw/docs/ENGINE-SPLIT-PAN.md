# Split Stereo Pan (N12)

Call `mixer.setPanMode {id,mode:"stereo"|"split"}` for the strip. Each split
knob calls `mixer.setPanLeft {id,pan}` or `mixer.setPanRight {id,pan}` with a
finite position in -1..+1. These snapshot-impact edits use the existing mixer
id, journal/undo and Session refresh path. Left/right ops are coalescable.
The window reads `TrackNode::panMode`, `panLeft`, `panRight` after refresh;
all three participate in structural sharing. The ordinary `pan` remains
stored and dormant in split mode; split positions remain stored in stereo mode.

Schema 1.10 adds optional `mixer_pan_settings`, keyed to `mixer_strip` with a
cascade foreign key. A missing row means stereo, left=-1, right=+1. This follows
ADR-0144's additive migration rule; no frozen mixer_strip column changes.
Read-only older files get the existing empty TEMP compatibility table and stay
unchanged on disk. Model reads and text projection expose all settings.
Unknown future mode text remains readable, is named by MixerStrips, and plays
stereo. A mode edit can be undone back to that exact unknown value.

The matrix positions the two input sources independently, then sums them into
the two outputs, in the graph's native float/double width. The default hard-left
/ hard-right pair is exactly unity; swapping positions exchanges the sources;
putting both at one side sums there. Live's split mono panners are constant
power without its ordinary stereo pan's sqrt(2) boost. Other selected pan laws
retain their formulas. Volume/mute cover every coefficient. Width and phase
remain Utility's controls (ADR-0203 d9).

All five coefficients (two direct, two cross, and additional-channel fader)
use the existing five-ms ramp, silence landing and precision handover. Both
inputs are read before either output so an in-place send cannot overwrite a
source. Post sends use the identical StripConfig; Pre sends retain their unity
source and mute path. Only the first stereo pair is panned; extra channels keep
the existing fader-only behavior. No callback allocates, locks or logs.

Track lanes `panLeft` and `panRight` use real -1..+1 units. They bind separately,
follow the existing grid/smoothing, and override independently on manual edits.
Re-Enable Automation resumes them normally. The existing pan lane is dormant
while split is selected.

First-touch inverse removes an originally absent settings row. Later inverses
update only the relevant column, preserving other controls and future columns.
Internal `_erase` / `_restore` fields carry absence and unknown-mode restoration;
the window does not construct them. Full track-subtree deletion capture remains
N37, whose child set must include this new cascade table.

The tests are native headless engine/store checks. Real Live comparison and
native UI/hardware parity are not claimed. This branch is independent of N25's
marker and N33's clip schema branches; win must reconcile their minor numbers
and migration histories in merge order before publishing the combined format.
