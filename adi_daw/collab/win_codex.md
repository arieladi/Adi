## 2026-09-28 — MIDI Learn ops (ADR-0195 d3), delegated by win

`controller.bind` and `controller.unbind` write project `controller_maps` through
the existing journal transaction. Bind captures an entire previous row, so
relearning the same id undoes in one step. Null is the absent-row inverse;
unbind captures every nullable and protocol-specific column. The store schema
and other store readers are unchanged; storage helpers live in controller_maps.*.
Claims cover these helpers, the catalogue, docs and tests.

Learn receives an explicit Remote/Focus Dial/takeover snapshot, resolves the
lane's owner and param_ref, and rejects the reserved CC before making an op.
Only the resolved binding enters the journal: no Focus Dial identity or settings
read during replay. Relative detection recognizes 1/127 and 63..65 clusters
(with neutral values); other/mixed or neutral-only observations are absolute.
A single relative code is necessarily ambiguous with a stationary absolute
knob; callers may supply a longer observation window. Absolute uses the passed
takeover mode; relative uses jump.

For mac's step 7: call learnController, commit the returned request, then read
controller bindings. Feed the current incoming port policy to dispatchController;
Focus receives its CC before the Remote role or any project binding. Use
controllerBindingShadowed for the binding list. Changing the live reservation
immediately changes that status without rewriting project data. These are
message-thread seams; the caller still owns parameter gestures/Live override
and takeover. No UI button or hardware driver is added here.

The focused suite passes 44 checks: Learn guards, detection, complete-row undo,
rebind, null fields, invalid payload rollback, settings-independent replay,
imported shadowed bindings and live reservation changes. Replay corpus now
contains bind, replacement and unbind, including full undo/redo and reopen.
Planting a wrong-CC reservation comparison failed three Learn/dispatch checks;
restoring it passed all 44. MSVC /WX passes. The full harness measured
5689 checks across 60 suites, all tests and validators passing. Its sole
failure was the old README count; corrected it to the measured total and
verified the headline against the captured suite results.

## 2026-09-28 — Auto Gain Stage: merge main after #166

Merged main 74990b1, retaining both log histories and removing the merged Color Bass claims. MSVC /WX and the complete harness passed 5668 checks across 60 suites, with all validators clean. README records that measured total. The intermittent Pd crash is handed to the analyser session; this update adds no workaround.

---

# win_codex — log

The Codex app on the director's Windows 11 PC (MSVC / x64), full access, in its own worktree.
Only the `win_codex` agent writes to this file. Newest entry at the top.

Onboarding and first mission: `collab/prompts/2026-09-27-win-codex-mission1.md` (ADR-0192).

---

## 2026-09-28 - connection retries stacked on #167

Branched codex/drone-connection-retry from the refreshed #167 head. Only an
incomplete /api/generate transport failure can retry, at most twice after 2 s
then 5 s. HTTP errors, JSON errors and post-body errors are excluded. Each
worker keeps its running claim and the final failure retains its original type.
Six loopback-server tests pass, including four concurrent workers; all six
parallel-worker tests still pass. No live drone directory or endpoint touched.
This PR targets codex/drone-parallel; retarget to main after #167 merges.

---

## 2026-09-28 - drone main refresh after Windows runner timeout


## 2026-09-28 - mission 3, Auto Gain Stage Session

Task E delegated by win. Claimed session, new gain-stage/input-gain files and
their tests. Session::autoGainStage renders an isolated session from the Store,
using the same loader, clip renderer and MIDI source, with ChainInput taps.
No driver is opened and the live transport/plugins are not processed. Track
mute/solo and strip automation are monitoring choices; clip gain/fades/mutes
and instrument automation remain. Groups/returns are not staged. Missing media,
unresolved devices, unsupported playback, nonfinite audio and stale undo heads
refuse the write. One OpJournal batch of mixer.setInputGain ops is one undo.

Found a necessary engine gap: inputGainDb was only read/projected, never applied
to audio. Session now inserts a persistent, atomic-target input-gain node before
audio inserts and after an instrument. Existing zero-gain graphs gain no node;
a node survives later zeroing for retired graph safety. Edits ramp for 5 ms.
ChainInput taps remain before this gain, so repeated staging sets an absolute
gain rather than accumulating it. No schema or op catalog change was needed.

Metering: 400 ms windows / 100 ms hops, absolute -70 and relative -10 gates.
RMS averages stereo channel energies; LUFS sums K-weighted energies with the
-0.691 calibration. Reuses existing K filters and TruePeakMeter, flushing its
interpolator. Mixxx's report pointed to libebur128; constants were verified in
https://github.com/jiixyj/libebur128/blob/master/ebur128/ebur128.c (no copied code).
Short selections pad to 400 ms; longer ones use complete windows. Default range
ends at the last unmuted clip, explicit ranges are allowed, at most one day.
Ceiling limits the gain before the write; silent tracks retain their setting.
This is a message-thread/offline API; menu wiring is not part of this PR.

MSVC /WX and full harness: 5632 checks across 59 suites, validators clean.
Three additional instrument-placement/stale-head checks pass in the final
focused suite, bringing the combined measured total to 5635/59.
The new suite has 25 checks: exact default RMS, LUFS calibration, peak ceiling,
MIDI-driven instrument render, silence gating, clip gain, block independence,
repeat determinism, atomic media failure and one undo. A nonlinear insert
proves gain placement: planting unity multiplication caused its check to fail;
restoring the gain passed. No live drone directory or audio device was used.

---

## 2026-09-28 - Windows Pd crash investigation

CI MSVC 19.51 faults in the console test; local MSVC 19.44 passes with Pd ON
in Ninja, a fresh Visual Studio project build, a fresh AddressSanitizer build,
and 80 repeated suite runs. Added a Windows native-stack diagnostic to expose
the CI fault site rather than guessing at the new sample members. Merged main
while preserving both log histories; MSVC /WX and the combined test run pass
5643 checks across 59 suites plus validators. This is
diagnostic evidence gathering, not a claimed fix.

---

## 2026-09-28 - mission 3, Task B: color-bass Pd devices

On codex/colorbass-pd from main after #150 merged. Both stereo top-level
canvases use adc~/dac~, declare fixed-id parameters, and answer the intrinsic
latency query with zero. Both externals self-register through ADI_PD_BUILTIN.
The real LibPdEngine tests found that the OBJECT target also needs its objects
propagated to final executables: a static archive discards unreferenced setup
initializers. They also exposed Win64 dsp_add's int/t_int varargs mismatch;
the wrappers use typed dsp_addv arguments. CMake now recreates the cached
pthreads4w target on a second configure rather than dropping its definition.

Review points 3-5: State crossfades two banks for 10 ms; allpass fractions stay
in [0.5,1.5), coefficients interpolate during the transition, and Color/Decay
copy ringing history rather than inject cold-start discontinuities. The State
sine step is bounded by the measured steady step after settling;
Color sweep steps are below 0.015. Color Cab normalizes the actual truncated
FIR for 20 Hz..20 kHz pink power, measured within 0.006 dB on three gamma values.

Win approved the narrow mac-owned pd_engine.hpp/.cpp claim, recorded as
"delegated by win, mac analyser's file" and to be removed on merge. LibPdEngine
owns PdSampleSlots and exposes message-thread bind/publish/collect calls.
Prepared Color Cab data derives from PdSampleBuffer (virtual destruction comes
from Sequenced), so the existing #153 publisher is the only handoff. A segment
acquires each slot once for both channels. Decoding and kernel construction
remain off audio. The host helper retains source hash/options/profile. The
patch exposes realtime Mix; Size/gamma/smoothing/pitch use off-thread host
rebuild/publication. Panel/drop-tile and state serialization remain UI work.

MSVC /WX and the actual top-level Pd renders pass, with a clean console hook,
a far bin at the floor before checking peaks, predicted FIR response, sample
replacement while blocks render, and zero allocations on the audio thread
(C++ new counters plus the MSVC Debug CRT hook, including Pd malloc/realloc).
The full measured total is 5613 checks across 58 suites with validators clean.
All rendering was into memory; no sound device or live drone folder was used.
The first CI build caught a GCC name collision between Bank::voices and the
outer voice-count constant. Renamed the bank member to resonators; no behavior
or test-count change.

---

## 2026-09-28 — mission 3, Dynamic EQ main refresh

Merged main after the color-bass DSP and analyser taps landed, preserving both
branches' log entries. README now records the measured combined total: 5610
checks across 58 headless suites; all validators pass. MSVC /WX build passed.
The approved Dynamic EQ implementation is unchanged. Win owns the PR merge.

---

## 2026-09-28 — Dynamic EQ PR integration after #162

Main advanced to include mac's top-level Pd canvas ruling while PR #163 was
being opened. Merged it into codex/dynamic-eq; only README's check count
conflicted. Kept the measured 5216/53 headless total: #162 changes optional
Pd-engine tests, not the headless suites. Main's ADR and other text are
preserved. No changes to either old color-bass or analyser branch.

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

Validation after merging current main through #153 (bbdfc4e): MSVC /WX passes;
the analyser suite passes **30 checks**; the complete test_all.sh run passes
**5126 checks across 52 headless suites**, with all validators/layout checks
clean. README records that measured count. The pointer publication tests include
nonzero instrument arrival (96 samples) before a 32-sample insert, as well as
closing/reopening while the render thread runs. The PR's own head must pass CI.

## 2026-09-28 — #150 refreshed through transport and the Pd sample host

Merged main through #157 (31b85b1), then through #151/#152/#153 (bbdfc4e),
as win requested. Both agents' log entries are preserved; only this log was
edited. Main's full decision text remains unchanged, followed by unchanged
ADR-0193; both 0193 and 0194 reservations remain used. Validator counts 194
ADRs. The full combined run measures **5278 checks across 53 headless suites**
(including DSP, transport, and the new pure sample-host suite), all passing.
The initial harness exit only flagged the old README total, now corrected
against that run. All validators/layout checks and MSVC /WX pass. The diff
against main remains our eleven DSP/test/doc files. No DSP changes in this
refresh; the new head needs its own CI result before win merges.

## 2026-09-28 — #150 refreshed after #147 and #148

At win's request, merged origin/main (`deff97e`) into codex/colorbass-dsp.
Only README and the appended decisions conflicted. Retained main's complete
decision text, including ADR-0179, and appended the unchanged ADR-0193; checked
the bytes and ADR-0183/0192/0193 order. validate_schema.py now counts 193 ADRs.
README reflects that and the measured **5092 checks across 51 headless suites**.
MSVC /WX and the full Git Bash test_all.sh run pass, all validators clean.
The diff against main is still only our eleven DSP/test/doc files. No DSP or
host changes were added; review points 3–5 remain for PR 2 with tests.
The replacement head requires its own CI verification before win merges #150.

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

## 2026-09-27 — #150 unstacked after #146 merged

#146 merged at 19:01:53 UTC. Merged origin/main at d66313c into
codex/colorbass-dsp in the isolated worktree. Resolved the two documentation
conflicts by retaining main's complete decision text (including its corrected
ADR-0192 d5) and appending our unchanged ADR-0193. Verified the full main text
is a prefix of the resulting file; ADR-0183, ADR-0192, ADR-0193 occur in that
order. validate_schema.py passes and counts 192 ADRs, matching README.

Local verification after the merge: tools/build.bat werror passed; Git Bash
tools/test_all.sh build passed all 5063 checks across 51 suites and all five
validators. Test and ADR counts therefore remain 5063/51 and 192.

The diff against main now contains only the eleven DSP implementation, test,
CMake, README, reservation and own-log files; none of #146's prompt, FEATURES,
or win-log additions remain in the PR diff. No production DSP changes in this
merge; points 3–5 remain follow-up work. Prior head 496dbca passed all 19 jobs;
that result does not count for the new merge head, whose CI must pass separately.

---

## 2026-09-27 — #150 review: portable unsigned test lists; next-work tests recorded

win's review: https://github.com/arieladi/Adi/pull/150#issuecomment-5856937945.
Three range-for lists inferred initializer_list<int>, then converted each item
into size_t. Replaced them with explicitly typed std::array<size_t, N> values,
with direct <array> includes. Test values and check counts are unchanged.
MSVC /WX rebuild passed; Color Cab's 46 and Chord Comb's 136 checks pass.
The actual regression gate is the GCC, Clang and Apple Clang hardened CI legs
on the newly pushed head; Windows alone does not prove this warning fixed.

#146 is still open at this entry. Once it merges, merge origin/main into this
worktree, retain ADR-0183, ADR-0192, ADR-0193 in order and run validate_schema.py
for README's ADR count. Do not merge #150; win owns its review/merge.

For PR 2 or a follow-up, each with a test (win's points 3–5):
- State switches: old/new banks run together and crossfade about 10 ms; a
  sustained-input switch must not exceed the steady-state maximum sample step.
- Allpass delay: use N=floor(delay-0.5), fraction in [0.5,1.5), smooth retuning;
  test a Color sweep for clicks as well as pitch/decay and allocation regressions.
- Color Cab level: make-up gain that preserves pink-noise RMS; test wet versus
  dry within +/-1 dB. Revisit coefficient validation with any gain staging change.

Read mac's #149 registration contract: declare
ADI_PD_BUILTIN(adi.combchord~, adi_combchord_tilde_setup) beside the external's
setup function and add its source to ADI_PD_BUILTIN_SOURCES. The registry is
called once per process before opening devices, and OBJECT-library linking
keeps registrars from being discarded. No src/juce edits are needed.

Every branch operation is in Adi-wt/codex-colorbass. I have never run checkout
or switch in the shared Adi checkout; the first status read there already
reported main. This records observed command history, not an attribution for
who moved that checkout.

---

## 2026-09-27 — phase 1: Chord Comb and Color Cab cores (ADR-0193)

Branch `codex/colorbass-dsp`, in `C:/Users/Adi/Documents/GitHub/Adi-wt/codex-colorbass`.
The main checkout's branch was never moved. Its ignored dependencies are shared
by a junction. No other agent, drone or chat was messaged; coordination is here
and in the PR. #146 supplied the assignment, claims and ADR-0193 reservation.
Its branch predates #138; I merged it into the current-main worktree, preserving
both ADR-0183 and ADR-0192 unchanged when resolving the append conflict.

**Built:** six parallel tuned combs, eight stored fractional-MIDI chords,
Saw/Square, per-note T60, in-loop Color with phase correction, allpass fractional
delay, Mix and output trim. A mono core per channel. Color Cab's pure builder
averages the whole sample spectrum, applies gamma, fractional-octave smoothing,
pitch shift and cepstral factorization, then windows/truncates to 64–1024 taps.
Its zero-latency direct FIR crossfades over 20 ms; a busy fade refuses another
kernel so the caller can retain/retry the newest request. No host/UI/Pd changes.

**Shared files:** CMake adds exactly two core sources and two test targets.
README updates the Windows/Pd-off check count and the ADR count. The 0193
reservation is now used; the device claims remain until win merges the PR.
Surge XT Combulator was read as the GPL-3.0-or-later reference; no code copied,
no new dependency. DSP formulas and their sources are in the cores and ADR.

**Measurements:**
- Comb pitch: worst 0.659777 cents over 96 cases, 40–1500 Hz, 44.1/48/96 kHz,
  both modes and endpoint Colors. The integer-delay Square fixture's even
  harmonics are -47.296 dB relative to its fundamental; the octave-low test
  rejects the original negative-feedback mistake.
- Uncolored T60 at 40 and 1500 Hz, both modes/three rates: 1.000000–1.000186 s
  for a 1 s setting. Both modes/endpoint Colors survive 60 s full-scale noise
  then 60 s silence at maximum 20 s Decay and 96 kHz with no NaN/Inf.
- Color Cab: Size 256, two-formant noisy fixture, gamma .5, smoothing .5 octave:
  maximum target error .237824 dB over 200–16000 Hz. Energy centroid .684820
  samples versus 512 in the separately constructed same-magnitude linear-phase
  response; 99.999934% energy in the first half. Formant-shift errors for
  -12/+7/+12 semitones: .125239%/.001410%/.001654%.
- Kernel-swap sine steps at 44.1/48/96 kHz: .035355873/.032350320/.016244881;
  derived bounds .044465962/.040858416/.020439997. Exact rounded-20-ms duration,
  initialized shared history, busy-request refusal and sample-zero FIR checked.
- Both allocate zero times in measured process calls, and produce identical
  samples at block sizes 32–4096. Eight distinct stored chords each match the
  average of six independent voices. Builder deterministic; sample scaling
  leaves it invariant; changing the file tail changes the response.

**Tests are not comments:** five altered source copies compiled with MSVC /W4
/WX and optimized, then failed: no Square half-delay (38 failures), no Color
phase compensation (38), half the decay exponent (12), abrupt FIR swap (2),
no positive-cepstrum doubling (1). Plants live only in ignored build/, and no
production source was changed for the plants. Baseline suites have 136 and 46
checks respectively. tools/build.bat werror passed. The final Git Bash
tools/test_all.sh build run passed: 5063 checks across 51 suites, all five
validators clean. README records 5063/51 for Pd off;
other platforms' optional Pd suites add checks to this configuration-specific
count (pre-existing test_all.sh cannot express both totals).

**What the mission needed clarified (ADR-0193):**
- Color adds damping; Decay is the unfiltered T60. Boosting loop feedback to
  cancel low-pass loss can exceed unity at DC. The stable implementation keeps
  unity-DC Color and does not promise equal T60 at every colored partial.
- A finite 256-tap filter cannot guarantee +/-1 dB for arbitrary sharp targets.
  The stated bound is measured on the named fixture. Windowing/truncation is
  an approximation to the minimum-phase factor; the test proves energy
  concentration, not a universal root-location theorem.
- First-order allpass interpolation is exact at the fundamental, dispersive
  above it. Square's exact odd-harmonic check deliberately isolates polarity
  with an integer delay. State/Mode changes reset tails and can click.
- Product question for win/director: are these documented Color/Decay and
  instantaneous State/Mode semantics the desired musical controls? No stronger
  claim is made before listening. Shipped names still belong to the director.

**Next dependency:** win reviews/merges phase 1 before phase 2 starts. mac owns
pthreads4w/Windows libpd, the built-ins registration hook and the host-side
sample slot/worker publication. The cores' setKernel is serialized with process;
it is not itself a cross-thread handoff. Phase 2 must consume host-published
values safely, and drive raw libpd against these same cores. I have not touched
mac's files or started phase 2 before the merge gate.
