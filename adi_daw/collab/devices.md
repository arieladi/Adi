# devices — log

The devices lane (ADR-0208; `docs/LANES.md` §4b). Newest entry first.
Device branches start at `origin/main`; win reviews and merges. This log has
its own `devices/log` branch, following `collab/engine.md`, so independent
device PRs do not conflict on one shared log file.

## 2026-10-28 — Live access working; Compressor calibration measured on Live 11.2.7

Live is available and working. The earlier “access unresolved” note described
an automation targeting problem, not a missing Live installation or a request
for Adi to configure it. Binding the app by bundle identifier resolved control.
Preserved the current Set as `untitled_preserved_2026-10-28.als` in its own
Project, then loaded the isolated `compressor_calibration` Set. About Live
confirmed **11.2.7, build 2022-12-08_2509d781e7**. No input from Adi is pending.

Captured all nine tracks through the local parameter bridge and verified every
Compressor control, mixer unity, centered pan, stopped transport, and restored
focus. Exported All Individual Tracks, Return/Master effects off, start 1.1.1,
length 4.0.1 (8.125 s), stereo WAV, 48 kHz, 32 bit, no dither, normalization or
loop rendering. The WAV parser confirms actual IEEE float32 and 390,000 frames.
**61 calibration checks pass**: both no-device references match their inputs
exactly at zero offset; bypass, three unity-ratio lookaheads, dry-only and fixed
+6 dB controls meet their declared tolerances. The active Compressor step has
finite positive output and measured attenuation. This validates the measurement
path, not full Compressor parity or internal lookahead latency (offline PDC).

Local evidence is under `work/local.nosync/live11-probe/`: actual WAVs in
`out/compressor_calibration/`, parameter capture, validation and export/version
provenance in `evidence/calibration-{parameters,validation,export-settings}.json`.
Source/Set/manifest/output hashes are retained. Seven synthetic validator
failure plants were rejected before accepting the actual renders. The earlier
preparation README and manifests intentionally retain their preparation-time
status; these dated evidence records supersede their no-render checkpoint.
Static and dynamics response batches are next. Live 12 manual evidence remains
separate from these Live 11 measurements.

## 2026-10-28 — Reconciled CI and later work; OneShot recovery resumes

Verified the exact current heads through GitHub: **all 19 checks pass** for
[#250](https://github.com/arieladi/Adi/pull/250) (`d730ad5`), draft
[#253](https://github.com/arieladi/Adi/pull/253) (`234a816`), and
[#259](https://github.com/arieladi/Adi/pull/259) (`1f47b21`). This includes
GCC, Clang, MSVC, strict warning/hardening jobs and both JUCE jobs. Main remains
`a57d47a`; none of these changes has been merged.

The preceding checkpoint omitted two later completed changes:

- Hybrid Reverb now uses PFFFT through `RealFft` in `spectrum.*`, with packed
  DC/Nyquist handling, normalized inverse and preallocated aligned scratch.
  Full strict local validation: **8,304 checks / 97 suites**. Affected suites:
  103 FFT, 50 native Hybrid and 11 Pd checks. Four compiled source plants
  (DC/Nyquist, inverse scale, phase, omitted partition) fail 13/7/13/10 checks;
  restoration passes. The sparse `1e-7` oracle remains; dense full-tail tests
  have an explicit amplitude-scaled float-roundoff budget. The Live parity
  gaps remain; the handwritten FFT debt is resolved.
- The existing ADI limiter retained the previous rate's release coefficient
  across prepare. #259 retains seconds and recomputes the coefficient,
  including values set before first prepare. Strict local validation:
  **8,189 checks / 95 suites**; the original behavior and compiled plant fail
  eight new checks. This is not a Live Limiter implementation or parity claim.

Recovery re-audit: #206–#208 production integration prerequisites remain.
However, OneShot's native DSP plus the independent ParamMod additions can be
recovered within this lane; an explicit test loader can prove playback without
editing Session. Work starts on `devices/oneshot` in a fresh
`work/active.nosync/oneshot` worktree. A Pd sample bridge must publish immutable
prepared data, not call allocating sample analysis from perform. Pd instrument
Consume/event-flow metadata and production discovery remain integration gates.

Additional local measurement preparation was also reconciled: Live **11.2.7**
API controls for Compressor, Glue and Gate are in `work/live11-evidence/`,
explicitly not fresh defaults or audio renders. Three Compressor probe Sets
(calibration/static/dynamics), 46 tracks and seven synthetic input WAVs exist
under `work/local.nosync/live11-probe`. The prior MBD Set was saved in that
folder before calibration was opened; the old preparation README predates
those actions. No rendered outputs exist yet. Capture/validation scripts are
being completed, and current Live/export state must be verified before accepting
any measurement. Current Preferences observation: 48 kHz, 512 samples, no input,
MacBook Air Speakers output; no preferences changed in this resumed turn.

Filesystem correction: iCloud has replaced the earlier `work/pd-zero-delay`
and `work/hybrid-reverb` symlinks with duplicate directories. Do not assume
those aliases still identify the validated checkout; current Git heads and
remote CI are authoritative. Preserve duplicates until their state is audited;
new worktrees are created directly inside `work/active.nosync/`.

## 2026-10-27 — Checkpoint: two device PRs open; no merges

- **[#250](https://github.com/arieladi/Adi/pull/250)** — `devices/pd-zero-delay`,
  `d730ad5f628eb50803424b9e48dbec484f87d8bf`: 17 canonical native devices,
  explicit whole-64-sample user-patch mode, Redux alias fix. Strict local
  Clang build, **8,543 checks / 97 suites**, validators and compiled plants
  pass. CI is pending; GCC/MSVC and JUCE matrix results are not yet claimed.
- **Draft [#253](https://github.com/arieladi/Adi/pull/253)** —
  `devices/hybrid-reverb`, `f1562a89d7f4c2dd58b3158579f9954c9dd807db`:
  eight device-owned files recovered independently from #209. Strict local
  Clang build and **8,207 checks / 97 suites** pass. The draft retains the
  documented Live parity gaps and handwritten-FFT debt; it is not a finished
  Live clone. CI is pending.
- **#206/#207/#208** remain blocked at engine/UI ownership boundaries, as
  detailed below. No forbidden stack changes were imported and no PR merged.
- Compressor and Glue Compressor manual audits are recorded below. New Live
  renders/default measurements remain open; Gate is the next device audit.

Resume from these commits. Check CI on the exact PR heads first and fix any
owned failure. For Hybrid Reverb, replace its handwritten FFT with PFFFT via
the existing DSP wrapper boundary, prove transform packing/normalization and
convolution tolerance with failure plants, then rerun the affected checks.
Integrate native dispatch when the adapter base is available on main. Preserve
the remaining adapter remainders and Live measurement gates below.

Local worktrees and their cached builds are under
`work/local.nosync/{pd-zero-delay,hybrid-reverb,devices-log}`, reached through
stable `work/<name>` symlinks. Build directories: `adi_daw/build` for the
adapter and `adi_daw/build-strict` for Hybrid Reverb. Generated WAVs were moved
to `work/local.nosync/test-renders/`; they are synthetic ADI test outputs,
not Live measurements. Mutation source has been restored. The parallel
workers stopped at the workspace credit limit; this is a clean checkpoint,
not a device blocker. Continue the queue when resumed.

## 2026-10-27 — Hybrid Reverb recovery from #209 — full local validation green, Live parity open

Branch `devices/hybrid-reverb`, based on `origin/main` `a57d47a581157fed3256efb8c5bf40102e98645f`; independent worktree. Recovered the eight device-owned files from #209 (`codex/hybrid-reverb` at `7f421f4`): the native convolution/Surge Reverb1 core, compiled-in Pd external, `Hybrid Reverb.pd`, host-side sample preparation helper, CMake module and two test suites. No OneShot/Sampler/MIDI ancestry, engine/graph/host/UI changes, or historical checklist documents were carried. The original #209 remains the provenance and incomplete-scope record; this recovery does not claim completed Live parity.

Reference read: local Ableton Live 12 manual, §28.23, printed/PDF pp.590–596. This is manual evidence, not a Live render measurement. The director's Mac runs Live 11; no output measured from Live 11 or Live 12 was supplied or created here. Existing parameter ranges/defaults/curves remain ADI values unverified against Live. Missing parity includes the five Live algorithm identities, four-band EQ/Pre Algo, Vintage, Bass Mono, independently retained time/sync predelay feedback, IR browser/editor and host-preparation controls on the panel. Existing engineering limits remain 256-sample native convolution latency, 262144-frame IR cap, independent stereo convolution and partition-boundary replacement without a dual-tail crossfade.

Tests use an independent direct time-domain convolution oracle with absolute tolerance `1e-7`, deterministic synthetic inputs, block-size invariance, stereo/routing/tempo checks, allocation auditing, immutable IR publication and float-WAV round trips. Added negative controls proving the convolution oracle rejects silence, bypass, a 10% gain error and a one-sample timing shift. These are synthetic oracle plants, not substitutes for a version-labelled measured Live render gate.

Known architecture debt carried from #209: `hybrid_reverb.cpp` implements a handwritten double FFT, while the current `spectrum.hpp`/dependency policy says PFFFT is the sole FFT and hides its API in the DSP wrapper. This recovery preserves the existing implementation; replacing it through the shared FFT wrapper is still required. The source retains existing notices and reuses the already present Surge adaptation in `live_reverb.cpp`.

Validation completed so far: build-only dependency pins verified in a separate COW copy; Debug Ninja configure with Apple Clang, `ADI_WERROR=ON` and libc++ Debug hardening; `tools/validate_pd.py` passes. The isolated native core also compiled under strict Clang C++20 Debug/O0 hardening and passed **17 checks, 0 failures**, including the four oracle plants. Full strict CMake build passed. `tools/test_all.sh build-strict` passed **8,207 checks across 97 suites**, all validators and the spec layout check; the Hybrid Pd suite passes **11 checks**. `test_all.sh` is the full suite runtime pass; a duplicate CTest runtime pass is not needed. Recovery commit `f1562a8` is on `devices/hybrid-reverb`, draft PR **[#253](https://github.com/arieladi/Adi/pull/253)**. No merge performed. GCC/MSVC results remain pending CI. Canonical native dispatch integration awaits the adapter base (#250).

## 2026-10-27 — Glue Compressor evidence gate and Live probe availability

Read Live 12 manual §28.21, printed pages 586–588. Confirmed ratio-dependent
knee, two-timescale Auto Release, Range limiting the available compression
(0 dB gives no compression), fixed Soft Clip with nominal -0.5 dB maximum,
2× oversampling and the documented possibility of >0 dB peaks when oversampled,
plus external sidechain gain/blend/EQ/listen. The manual does not give the
complete parameter/default table, numerical knee/ballistics or waveshaper.
No Glue Compressor implementation or version-labelled render fixture is on
main. Keep its parity gate open rather than assigning an unmeasured SSL-like
algorithm; Gate is the next missing-device audit.

The existing `collab/mac/live-probe/` kit is the behavioral measurement route:
it generated the earlier Live **11.2.7** Multiband Dynamics evidence. Its default
`~/adi-live-probe` output directory is absent on this machine. Read-only access
to the local AdiVST WebSocket bridge works and currently identifies Multiband
Dynamics. The handshake reports Live version `?`, so it cannot label a new
measurement. No device was loaded or changed, and no render was made. A new
probe must use an isolated Set, verify the actual Live version and export
settings, and preserve the user's existing work. The kit's driver blindly
assumes remembered export settings; inspect those before running it.

## 2026-10-27 — Compressor evidence gate; next missing effect audit

Read the local Live 12 manual §28.9–28.9.2, printed pages 555–559.
Confirmed behavior includes threshold/ratio transfer, unity ratio, Dry/Wet
endpoints, Knee spanning threshold ± the displayed knee amount (the manual's
10 dB knee / -20 dB threshold example spans -30 to -10 dB), Peak/RMS/Expand,
Lin/Log, Auto Release, 0/1/10 ms Lookahead, sidechain gain/blend/EQ/listen and
Makeup being unavailable with an external sidechain. This is Live 12 manual
evidence only, not a Live 11 or Live 12 measurement.

Main has no Compressor core/external/canvas or version-labelled Live render
fixture. Existing `rmsc` is ring-modulation ducking, not Compressor; generic
limiter and multiband dynamics code does not establish Compressor's behavior.
The manual does not quantify all ranges/defaults, detector ballistics, RMS
integration, Log/Auto release, stereo linking or makeup law. Do not substitute
remembered algorithms and call it Live parity. Compressor's measured parity
gate is therefore open under LANES §5; continue with the next device audit
while the adapter and Hybrid Reverb full suites run.

## 2026-10-27 — Zero-delay adapter: full local validation green

PR **[#250](https://github.com/arieladi/Adi/pull/250)**, head `d730ad5`, is open
from `devices/pd-zero-delay`; never merged. CI is pending.

The branch now passes **102 adapter checks**, **260 native/Pd
comparison checks** and **68 Redux checks** under Apple Clang Debug with
`ADI_WERROR=ON` and libc++ Debug hardening. All 17 canonical native renders
match their real-Pd renders exactly, allowing only IEEE `+0`/`-0` equivalence
because Pd's output summing clears signed zero. A one-ulp oracle plant is
rejected. These are ADI-to-ADI comparisons, not Live measurements.

The comparison exposed a real Redux crossed-buffer defect: Pd may recycle the
right inlet vector for the left outlet. Capture both samples before writing
either output. The standalone original-code plant fails the new regression;
max error in the reproducer changes from 0.375 to zero after the fix.

Three compiled adapter source mutations also fail the test suite: restoring a
fixed 64-sample prime (8 failed checks), halving parameter values (6 failed checks), and dropping
the process block offset (4 failed checks). Every mutation was restored; the restored adapter
passes all 102 checks. An earlier plant attempt timed out on filesystem reads
and is not counted as a successful negative control.

The native allocation audit covers processing, tempo/MIDI/sample handoff and
now parameter sends. Static review found no allocating, locking or logging
path after preparation in the 17 cores. The full strict build passed, and `tools/test_all.sh build` passed **8,543
checks across 97 suites**, every validator and the spec layout check.
`tools/validate_pd.py` also passed separately. No GCC/MSVC result is claimed
before CI. The inherited macOS
linker reports a duplicate `libadi_core.a` link from unchanged shared CMake.

Build environment note: files in the new Documents checkout repeatedly
stalled on iCloud-backed reads. Isolated worktrees now live under
`work/local.nosync/`, with stable symlinks at their original paths. Restored
unchanged tracked files from HEAD, preserving all device edits. No user
checkout or other lane's build was changed.

## 2026-10-27 — Recovery audit: OneShot, MIDI effects and Sampler blocked at lane boundaries

Audited PRs #206–#209 against main `a57d47a`. The old stack is actually
#205 → OneShot → MIDI effects → Sampler → Hybrid Reverb; rebasing the entire
stack would import UI and graph work owned by other lanes.

- **#206 OneShot:** its implementation `072c813` modifies Session and UI
  sample-import code in addition to the DSP/device. Main lacks the UI ancestry.
  Native device files can be recovered, but a full working rebase needs the
  window/engine owners' integration. No Pd patch/external exists in this PR.
- **#207 MIDI effects:** `c130f0e` requires event ownership, mapped/control
  events, graph event transformation, Session factories, and host interfaces
  absent from main. These are engine/host prerequisites; this lane will not
  transplant or edit them. Warning fixes appear only later in #209.
- **#208 Sampler:** `b982fda` depends on both preceding devices and changes
  Session. No Pd patch/external exists. The old stack's macOS failure is its
  `getParam("volume").real == 0` modulation assertion: compare the unchanged
  stored/base value before and after modulation, rather than an assumed exact
  floating-point reconstruction of zero, when this item can be recovered.
- **#209 Hybrid Reverb:** its eight device files are independent of those
  prerequisites and are being recovered onto `devices/hybrid-reverb`. The
  unrelated graph/MIDI/Sampler changes in that PR are excluded.

These existing tests use generated signals/event oracles. None supplies a
version-labelled measured Live render fixture, and the old checklists mark
parameter ranges/defaults and substantial device features unverified. Recovery
and synthetic test success must not be described as Live parity.

## 2026-10-27 — Zero-delay adapter: implementation and verification in progress

Branch `devices/pd-zero-delay`, based on main `a57d47a`.

- Canonical built-ins execute the existing native cores directly, selected by
  complete compiled canvas and resolved helper contents, never by filename.
  Modified canvases/helpers retain Pd execution. Original patches remain
  openable and the existing Pd suites explicitly exercise that path.
- A user patch can select `BlockMode::WholeBlocks` before preparation. Every
  segment must contain a whole number of Pd's 64-sample ticks; the mode returns
  same-frame output without priming. Violations are counted and silenced;
  latency is never silently changed during playback. Oversized valid calls
  are tiled through preallocated scratch instead of dropping samples.
- **Remainder for win/engine:** `maxFrames % 64 == 0` does not establish that
  guarantee: event splitting can produce 100 + 412 from a 512-frame block.
  Arbitrary user-patch segments retain the stable, reported 64-sample adapter.
  Enabling whole-block mode in a caller requires its scheduling guarantee;
  this lane does not change the graph. A streaming one-tick adapter could
  potentially reduce the conservative 64 to 63, but that is not this change.
- **Remaining legacy canvases:** `pd/adi-eq8.pd`, `pd/adi-limiter.pd`,
  `pd/adi-rmsc.pd` and the analyser remain on Pd. They are vanilla programs,
  not the canonical compiled-external canvases covered by the native dispatch;
  substituting similarly named DSP without a wiring/null proof would change
  their sound or array publication.
- **Inherited Pd runtime constraint:** pinned `z_libpd.c` takes `sys_lock`
  inside both process APIs and array reads; `sched_tick` also calls the locked
  message queue. Arbitrary user-patch callbacks can allocate or print. The
  native path bypasses these calls. Whole-block scheduling removes buffering
  delay, but is not a claim that arbitrary vanilla Pd is realtime-safe.
  Removing those runtime locks without preserving Pd clocks, message queues
  and concurrent instance lifetime is separate work, not an unlocked call
  substituted here.

Full verification and PR links are recorded in the checkpoint above.

## 2026-10-27 — Lane bootstrap and source locations

Read the standing prompt and five-lane LANES/ADR-0208 text from
`origin/win/lanes-5` (#238), since they are not yet on main. Read the requested
ADRs, device contract, FEATURES table and EQEight patch/core/external/module/
tests. `pd/README.md` is absent on both branches; the available document is
`pd/devices/README.md`.

The extracted `reference/DOCS/_text/Ableton_live12/` directory is absent on
this Mac. A local Live 12 manual PDF is available under the existing checkout's
`adi_daw/References/live12-manual-en.pdf`; the original is left untouched.
All future measurements must say Live 11 or Live 12. No Live measurement has
been made or inferred in this bootstrap/adapter work.
