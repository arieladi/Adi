# win_codex — log

The Codex app on the director's Windows 11 PC (MSVC / x64), full access, in its own worktree.
Only the `win_codex` agent writes to this file. Newest entry at the top.

Onboarding and first mission: `collab/prompts/2026-09-27-win-codex-mission1.md` (ADR-0192).

---

## 2026-09-28 — A2: per-track analyser tap points

Branched codex/analyser-taps from main after #157 merged (31b85b1), reusing
our idle transport worktree. Read #155's exact tap list and win's translation
of the whole channel strip versus StripNode. Taps are keyed by (track, point),
with PostFader the unchanged default. PreFader captures StripNode input before
its gain; ChainInput captures Graph's compensated summed input at the first
insert, or the first effect after the instrument (cached EventFlow::Consume).
Without an effect after it, ChainInput falls back to the strip input.

Consumer calls: openScope(trackId, seconds, ScopePoint::ChainInput) for
spectrum.pre; openScope(trackId, seconds, ScopePoint::PreFader) for spectrum.post
and plain spectrum. ScopeTap::read remains unchanged. One second means 48000
stereo frames at 48 kHz. A repeated key retains its original ring length.
closeScope(trackId, point) detaches, while Session retains the allocation until
audio has stopped; one already-started callback may finish its write.

Graph writes ChainInput at its slot's actual arrival, excluding that node's
own latency. PreFader uses strip arrival; only PostFader adds strip latency.
Device implementations are untouched. Each off tap costs one relaxed pointer
load, no copy; the non-null path uses an acquire fence paired with the setter's
release, so a newly allocated ring is safely published on ARM as well as x86.
Suspended nodes also write silence to open taps: Graph notifies the strip via a
scope-only silence hook without running its DSP. ScopeTap::write accepts null
left audio for that allocation-free silence path. FFT/banding stays off audio.

The new test covers exact insert gain, no-insert and instrument-only fallbacks,
fader separation, real 32/96-sample delay lines aligned by heard stamps, strip
latency applying only post-fader, zero writes after closing, 40 open/close
cycles at each point while rendering (including first allocation), silent
suspension, and zero callback allocations/file I/O. Also applied win's deferred
#157 nit: session transport uses textproj::kPPQ/kWhole. No other textproj or
tempo-builder change. CMake adds only the new test target.

## 2026-09-27 — mission 2, Task A: transport at the NodeIo boundary

Win delegated Task A and approved the narrow plumbing expansion on this chat
(claims also recorded on #154). `AudioIo` carries one nullable
`const TransportInfo*`; Session fills the value on its callback stack, copies
the caller's io, and supplies the pointer. Graph forwards it to every segment's
NodeIo. The runNode declaration/calls change only to carry that pointer.
Standalone graphs receive null. `host.cpp`, device drivers, SilenceProcessor,
the tempo builders, and mac's `textproj.*` are untouched. CMake adds one test
target beside the session suite.

Time signatures are **not in Snapshot**: `rows::readModel` reads
`time_signature_map` into `rows::Model::signatures`, sorted by tick position and
id. Session rebuild copies those rows and builds the existing `TempoMap` as
MidiClips does. The immutable view is published only after a successful rebuild,
read through `SnapshotPublisher::AudioRead` once per callback, and reclaimed by
`collect` in the message-thread tick. Process reads neither model_ nor spec_.
Samples convert through `TempoMap::secondsToTicks`; BPM is its step value,
including when a stored event requests a ramp. The bar walk counts a shortened
bar before a meter change and defaults to 4/4 from zero, matching
`renderPosition(metersOf(...))` without allocating a string on the audio thread.

Consumer agreement for mac's #152: `playing`, `bpm`, `timeSigNumerator`,
`timeSigDenominator`, and one-based `bar`/`beat` map directly to the existing
`LibPdEngine::Transport` fields. `ticksInQuarter` is absolute musical ticks
modulo 5,765,760 (SPEC 4.2), **not** ticks within the denominator-defined beat;
it remains exact when converted to Pd float. `timelineSample` is an additional
signed 64-bit field for the first timeline sample of the callback. All segments
see that same block-start value; consumers must check null and not retain the
pointer. No Pd host changes are part of this PR.

MSVC /WX and the new suite pass: **131 checks**, including projection agreement
at every fixture meter change and the adjacent samples (including a missing
initial meter and a shortened bar), step BPM across a tempo change, the
quarter/beat distinction in 3/8, stopped/playing, identical start values for
32/64/128/256/512/1024/2048/4096 frames, loop wraps at every size, null standalone
transport and the identical pointer across split segments. Twenty alternating
tempo edits while a render thread processes callbacks all arrive as coherent
views. Callback allocation and file-I/O counters both remain zero.

The full local run completed all **5012 checks across 50 suites** and all five
validators/layout checks. Its only initial failure was the stale README total
(4805/48 on main); the README now records the measured total, including this
suite. No ADR is added or renumbered.

Before CI could start on #157, main advanced through #147/#148 and the README
conflicted. Merged main (`deff97e`) in this worktree; the engine change is
unchanged. MSVC /WX and all 131 transport checks pass after the merge. The CLAP
suite now has 390 checks (25 more). The full post-merge run measured **5041
checks across 50 suites**, all passing, with all validators/layout checks clean;
README records that measured total for this headless configuration. Main's
Pd/JUCE CI tiers have additional suites.
