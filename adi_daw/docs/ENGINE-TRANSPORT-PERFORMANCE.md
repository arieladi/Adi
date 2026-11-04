# Saved loop and metronome (N19)

The window commits `transport.setMetronome {on}` and
`transport.setLoop {on, start?, end?}`. Bounds are musical ticks, not samples.
Refresh ProjectView and Session after success. `Snapshot::transport` exposes
`metronome`, `loopEnabled`, `loopStartTicks`, `loopEndTicks`; `hasLoop` distinguishes
saved settings from a driver-controlled transport that has no saved loop yet.
These are session metadata, saved in the project but outside undo. Toggling off
retains the region. Initial settings are off, with a four-bar 4/4 window at zero.
Invalid windows fail transactionally; malformed saved metadata uses defaults.

Session converts ticks through the same tempo map as clip playback at the
session rate. The driver takes a changed loop configuration before processing
its next callback; an ordinary rebuild does not seek or restart it. A valid
window smaller than one sample plays as one frame. Existing direct driver loop
commands work when no saved loop owns the region.

The original synthesized click follows tempo and the current signature's beat
unit, accenting the first beat of a bar. It plays only while running. It is mixed
into the default stereo cue pair after Main's devices/fader. Cue Out/volume and reference
exclusion now use the N23 monitor mixer; see `ENGINE-CUE-MONITOR.md`. Both float/double output pointers are
supported. The click's heard timeline follows the output latency of the graph
that actually rendered, including loop wraps and a delay longer than one loop;
post-start/seek history is not invented. Sound/level parity against Live is
unmeasured; the initial click uses 1760/880 Hz and a 4 ms exponential envelope.

`SessionSpec::performanceState=false` excludes saved loop/click from isolated
analysis/render sessions. The export queue forces this mode, and gain staging
uses it too. A monitoring choice never changes a file render. Callers implementing
other offline render paths must choose this mode explicitly.

When merging with N13, keep Main loudness measurement immediately after
`graph_.process`, before `cueMonitor_.process`: the click is a cue signal. No audio
callback reads SQL, allocates, locks or logs. Settings and maps are copied into
the existing immutable TimingView publication; mutable driver state stays on the
driver thread.
