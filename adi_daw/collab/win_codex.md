# win_codex — log

The Codex app on the director's Windows 11 PC (MSVC / x64), full access, in its own worktree.
Only the `win_codex` agent writes to this file. Newest entry at the top.

Onboarding and first mission: `collab/prompts/2026-09-27-win-codex-mission1.md` (ADR-0192).

---

## 2026-09-28 — mission 2, Task C: Dynamic EQ CLAP and graph gestures

Win moved Task C ahead of color-bass PR 2 while #150/#159 await his merges.
Created the isolated `dynamic-eq` worktree and `codex/dynamic-eq` from main.
Neither existing branch was changed or pushed. No scheduled follow-up was made.

Direct adaptation of ZL Audio/ZLEqualizer, AGPLv3, pinned
`3468a3ac85f5c1f9d16083acbee5b1339984d53b` (the existing upstream CLAP baseline).
The reference checkout is newer (`64f8364`); the drone reports helped locate
code, but all parameter limits were checked in the pinned
`source/zlp/zlp_definitions.hpp`. Our copied PluginProcessor files keep the
original headers. They rename the class, replace editor construction and make
ZL's existing output double-to-float conversion explicit for /W4 /WX. DSP,
state, sidechain, smoothing and filter structures remain the upstream code,
compiled from the fetched source without patching it. The plug-in has its own
AGPL license, ADI identity and separate build; adi_core links none of this DSP.

Read the locally held Pro-Q 3 manual's printed pages 9, 10 and 15 and ADR-0195
again. Gestures.hpp is pure C++, with one or more tests per table row. The GUI
uses it for drag/wheel/click/create and handles host gesture lifetimes only
for changed parameters. Alt-creation has no axis lock; subsequent node drags
do. Wheel edits during a drag rebase its current values. Dynamic range maps
to ZL's target gain, preserving the endpoint for the linked wheel gesture,
including at a bound. The main view has no ZL panel dials. Double-click has
numeric band fields and an All parameters tab; right-click has band actions.
The graph explicitly identifies its curve as a static minimum-phase response,
not a live dynamic/other-phase analyser. No Pro-Q text or images were copied.

ADI choices, tested and documented: Shift scales movement by 0.1; Q wheel is
one quarter-octave/detent, gain/range wheel is 1 dB; a new dynamic band's target
starts at unity. Graph scale is 10 Hz..30 kHz (response stops at Nyquist),
+/-30 dB, with a 3-pixel drag threshold. These are our choices, not unverified
numbers from the drone or claims of matching another plug-in's sensitivity.

Validation: the standalone gesture suite passes 30 checks. Processor tests
pass 90 checks: every ZL shape at 44.1/48/96 kHz through an independently
compiled original-processor oracle; actual sample and measured magnitude
errors are both zero. Dynamic envelopes in Minimum/SVF/Parallel, with internal
and external sidechain, also have zero sample difference and demonstrably
attenuate above threshold. Range mapping, source bounds, state restore, GUI
mouse routing, curve magnitude and zero ordinary operator-new allocations in
warmed-up Minimum-mode process are covered. The CLAP ABI smoke test loads the
actual binary, checks its identity, 609 parameters and ports, activates,
processes and tears down with no audio device; unity error is zero.

The editor was rendered offscreen to a PNG and inspected. Native macOS/Linux
plug-in builds and interactive comparison against the commercial plug-in
remain review items; this does not claim them as passed. The new pure gesture
test is wired into normal headless CI under adi_warnings. Shared CMake change
is only that test target; README's check count is from the actual fresh run.
The starting main README said 5317/53; its existing binaries contribute
5186/52, so adding 30/1 gives 5216 checks across 53 suites (not 5347/54).
Every suite and validator passed; the first harness run failed only the stale
headline comparison, which was then corrected. Own processor/editor sources
also build with /W4 /WX; upstream oracle warnings are not suppressed by editing
upstream code. Build/test instructions and explicit limitations are in
plugins/dynamic-eq/README.md.

Local products: `C:/Users/Adi/adi-build/dynamic-eq/`, CLAP under
`adi_dynamic_eq_artefacts/Release/CLAP/ADI Dynamic EQ.clap`. Never installed or
played. Win reviews/merges this PR. Task B remains next, gated on #150 merged.

---


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
