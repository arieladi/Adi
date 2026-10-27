# engine — log

The engine lane (ADR-0207, `docs/LANES.md` §3): Claude Max, Opus 5.5. This session runs in a
Claude Code cloud container on Linux (GCC and Clang; no MSVC here, so `/WX` is win's leg), one
branch per item cut from `origin/main`, `engine/<topic>`. Only the engine lane writes to this file.
Newest entry at the top.

**Why this log has its own branch and PR (`engine/log`).** Every item PR prepending an entry here
would make each merge conflict the next open PR on this file, the failure `collab/README.md` rule
3 exists to prevent. So the item PRs never touch this file; this one does, and win merges it
whenever convenient. Each entry names its PR, so nothing is lost if this merges late.

---

## 2026-10-26, night — LANES §3 item 1f: a VST3 that takes `kSample64` is set up at 64 and handed double (ADR-0207 d2)

**PR:** [arieladi/Adi#237](https://github.com/arieladi/Adi/pull/237) (`engine/vst3-64`), draft, stacked on #236. CI green on all 19 legs, the JUCE legs' new 64-bit render and its failing twin included.

**What.**
- The graph tells each node, just before it prepares it, the width it will be handed (`Node::willBeHanded`). A VST3 fixes its sample size in `setupProcessing`, at activation, so it has to know then.
- `Vst3Device` says `processesDouble()` when the processor takes `kSample64`. At 64 it is set up at 64 and handed `channelBuffers64`; at 32 every VST3 is set up at 32.
- A width change alone is a reactivation, under #236's gate.
- Either graph width works at either setup, because a rebuild's last graph still plays the device.
- **Pre-existing, fixed:** the raw path's error branch silenced from the block's start, not the segment's offset.
- `adi_play --precision 32|64` and `--require-double`. CI renders the fixture at 64 with the gate, and proves the gate fails on the fixture variant that refuses 64.

**Checks.**
- `adi_precision_tests` 96 (+4), `adi_device_model_tests` 67 (+3), `adi_vst3_probe` 42 new fixture checks.
- The fixture now computes in double and writes at the setup width, so a 64-bit render holds no float sample.
- Sixteen plants, all caught.
- `test_all.sh` 8003 checks across 95 suites. GCC and Clang `-Werror` clean.

**Found, not fixed (pre-existing):** the router forgets its sounding notes when a device reactivates (`router_.reset()` in `Vst3Device::prepare`), so their note-offs never reach the plug-in. A plug-in that kept voices through `setActive(false)` would hang them. Fix: send the router's sounding notes their offs before a reactivation, as `switchTo` does.

---

## 2026-10-26, night — #236, second push: what the review of the reactivation gate found

**PR:** [arieladi/Adi#236](https://github.com/arieladi/Adi/pull/236), second commit (`07443ec`). CI green on all 19 legs. A two-lens review, each finding put to a skeptic, confirmed seven distinct defects.

**Fixed.**
- **The gate covered `process` alone.** `Graph::runNode` asks each node its tail every block. The old graph of a live rebuild called CLAP `tail.get` while the plug-in was deactivated or inside `activate`: 18 545 such calls over 20 rounds of the review's TSan probe, and a race. Now:
  - CLAP's tail query passes the gate and answers infinite while it is closed;
  - VST3's tail is read on the message thread, at construction and after each activation.
- **The graph asked the tail twice a block**, and finite then infinite overflowed `tail + reach`. It asks once.
- **`Vst3Device::prepare` never reached an active plug-in on a format change**, because JUCE's own early return skipped it (pre-existing). It releases first.
- **Every VST3 activation leaked an `IAudioProcessor` reference** (pre-existing).
- **`~ClapDevice` stopped a plug-in whose start had failed** (pre-existing).
- **The first push's test of a call made during a reactivation could not fail.** It now holds the reactivation in `stop_processing`, and plant P2 fails the suite rather than only TSan.
- **A gate that never opens hung the suite.** A watchdog and a ctest `TIMEOUT` now end it.

**Also.** The fixture VST3 builds on Linux (`Contents/<machine>-linux`), so the VST3 half runs here. Its hooks:
- a held process call;
- a strict block size;
- a live-object count;
- a count of setup calls made under a process call.

**Checks.**
- `adi_clap_tests` 486 (+13), `adi_graph_tests` 195 (+3), 14 new probe checks. Nine plants, all caught.
- `test_all.sh` 7996. GCC and Clang `-Werror` clean. TSan 0.

**Found, not fixed (pre-existing).** A plug-in's own `restartComponent(kReloadComponent | kIoChanged)` makes JUCE deactivate and reactivate it on the message thread, outside `prepare`. That runs under a raw process call that takes neither JUCE's private lock nor the gate: the same use-after-free class. After `kIoChanged` nothing rebuilds the graph either. Fix, as its own PR:
- wrap JUCE's component handler, so a restart is marshalled to the message thread under the gate's `Hold`;
- have `kIoChanged` move a shape epoch.

---

## 2026-10-26, evening — LANES §3 item 1f-a: a plug-in reactivated during a live rebuild is held off the audio thread

**PR:** [arieladi/Adi#236](https://github.com/arieladi/Adi/pull/236) (`engine/device-gate`), draft, stacked on #233. It fixes the race #233's review found and this log recorded.

**What.** `ReactivationGate` (`device_model.hpp`), one per device. `prepare`, after its early return, and `release` close it and wait for the call in flight. A call made meanwhile passes its audio through. Both sides are sequentially consistent. An unchanged `prepare` never takes it. `ClapDevice` and `Vst3Device` both use it.

**Checks.** `adi_clap_tests` 473 (+15). TSan on the review's probe: 26 races without the gate, 0 with it. `test_all.sh` 7980.

**Found, not fixed (pre-existing):** `PdDevice::prepare` has no early return. Every rebuild re-inits libpd on an instance the last graph may still be processing. It goes with the Pd tier's PR, which rewrites that `prepare`.

---

## 2026-10-26, night — #232, second push: a sleeping mix keeps the silence it drained, and drains what is still due

**PR:** [arieladi/Adi#232](https://github.com/arieladi/Adi/pull/232), second commit. The two-lens adversarial review of the first push found:
- **a regression in it (confirmed):** a consumer falling asleep forgot all of its lines' history, the silence it had just drained included. A glide asked for while it slept, a plug-in's latency switched while the project is stopped, was then held after the wake, and the onset was lost or faded in. The code before #232 was sample-exact there.
- **two pre-existing holes (confirmed, byte-identical before #232):**
  - a glide up during a drain was cut and then replayed at the next wake;
  - a rebuild in the middle of a drain did the same, through `GraphHost::rebuild`.

**The fix.** Each line counts how much of its newest history is known silence (`DelayLine::quiet_`).
- A sleep forgets all but that.
- A node stays awake while any line has audio due or a glide pending. Run in the silence, a glide lands there, and the wake plays at the new alignment from its first sample, which beats the code before #232 too.
- A sleeping node reads one graph-wide retap counter per block instead of its lines (`Slot::dozeGlides`).

**Checks.**
- `adi_retap_tests` 316 checks (+44); `adi_host_tests` 106 (+6). 18 + 2 of them fail on the first push.
- Nineteen plants, seventeen caught. The two that survive are resets kept on purpose (stated in the PR).
- Cost, by cachegrind over the whole block benchmark: +0.13% instructions in `Graph::runNode`. Without the retap counter it was +0.82%.
- `test_all.sh` 8096 checks across 95 suites. GCC and Clang `-Werror` clean. TSan: 0 races, two runs.

---

## 2026-10-26, evening — LANES §3 item 1e: a CLAP whose ports take 64 bits is handed 64 bits (ADR-0207 d2)

**PR:** [arieladi/Adi#233](https://github.com/arieladi/Adi/pull/233) (`engine/clap-64`), draft, stacked on #229.

**What.**
- `ClapDevice::processesDouble()` is true when every audio port declares `CLAP_AUDIO_PORT_SUPPORTS_64BITS` and there is an output. At 64 such a plug-in is handed `data64` on every port, `data32` null. Anything else is processed at 32 between conversions.
- The flags are read only while the plug-in is deactivated. **Pre-existing, fixed:** `prepare` scanned the ports just after `activate`, against `audio-ports.h`; the scan now runs before it.
- An activation that finds the answer moved moves `shapeEpoch`, so the graph is built once more without reactivating anything.
- **Also pre-existing, fixed:** `steady_time` did not advance after `CLAP_PROCESS_ERROR`.

**Checks.**
- `adi_clap_tests` 458 checks (+52); 23 plants, every one caught.
- `test_all.sh` 7965 checks across 95 suites. GCC and Clang `-Werror` clean.
- A three-lens adversarial review of the first push confirmed five checks that could not fail, and the missing rebuild after a moved answer. All are fixed in the second push.

**Found, not fixed (pre-existing):**
- **`ClapDevice::prepare`'s full path races the old graph.** A rebuild prepares the new graph while the old one still renders the same `Node*`s (ADR-0089 d4). When `prepare` must reactivate a plug-in (a port rescan, a new rate), it stops and deactivates it and reallocates its buses on the message thread while the audio thread may be inside `process`. The reviewers reproduced a heap-use-after-free under ASan and data races under TSan. Fix: never re-prepare a device a live graph still runs. Either defer the reactivation until the old graph is retired, or gate `process` (pass through while a reactivation is in progress, with an acknowledgement from the audio thread). The same applies to `Vst3Device::prepare`, whose `prepareToPlay` reactivates.
- **Default buses a plug-in never declared.** A CLAP with no audio-ports extension, or with no output port, is handed one stereo bus each way. `audio-ports.h` says such a plug-in has no ports, and `process.h` says the buffers must match the declared count.
- **`Vst3Device::process`'s error path ignores the segment offset**: it zeroes `out[c][0..frames)` instead of from `blockOffset`. To be fixed in the VST3 64-bit PR, which rewrites that branch.

---

## 2026-10-26, evening — LANES §3 item 1d: a glide away from zero reads the edge's real past (ADR-0079 d2)

**PR:** [arieladi/Adi#232](https://github.com/arieladi/Adi/pull/232) (`engine/zero-edge-history`), draft, stacked on #231 (it uses the history count #231 added).

**What.** The defect #231 logged. An edge at zero delay never wrote its ring (the fast path), so a glide away from zero read the edge's audio from the last time it had a delay. The same went for an edge whose source or consumer slept, and for a rebuild's handover of such a line. **Two designs were measured.** Keeping the ring written costs +33 to +116% on the 8-track fixture and up to +301% on 64 tracks: every ring is 64 KB at the default headroom, and writing one per zero-delay edge per block leaves the cache. So the chosen design costs one store: `DelayLine::forget()` on the fast paths, the sleeping-source skip, and a node's sleep. A glide away from zero waits, held, while the slow path writes the edge's real past, then crossfades into it. That is stale for the length of the delay, never a ghost (ADR-0085). A handover carries only what the old line really holds. Benchmark: nothing outside the noise.

**How it was checked.** `adi_retap_tests` 272 checks (+54), at 32 and 64, both sum paths, blocks of 256 and 64, sleeping sources and consumers, the handover. Five plants each fail them. `test_all.sh` 8046 checks across 95 suites. GCC and Clang `-Werror` clean. **Changed:** `testAZeroEdgeCanStillBeGivenADelay` lets the glide from 0 wait one block.

**Next.** The plug-in hosts at 64: CLAP first (a device says `processesDouble()` when every port declares `CLAP_AUDIO_PORT_SUPPORTS_64BITS`; double buses beside the float ones; `data64`), then VST3 `kSample64` on the JUCE-on legs, then the Pd tier and the native devices. Then LANES item 2, sends and returns: an 11-PR plan is drafted, and its questions for win will be in its first PR.

---

## 2026-10-26, evening — #231, the second review round

**PR:** [arieladi/Adi#231](https://github.com/arieladi/Adi/pull/231), third commit. A second adversarial review, of the first round's fixes alone, found three real gaps, now fixed with their tests. 1) An adopted line counted its whole ring as history, so a glide after a rebuild could still read slots prepared as zeros. It now counts what it was handed, and a line adopted mid-grow takes the ring being grown when that holds more. 2) A ringless edge refused a grow was rebuilt only if nothing else grew. `escalateLatency` now reports what it could not settle: a ringless edge asks for a rebuild whatever else grew. 3) An edge whose grow in flight is too small leaves the next poll pending, which collects the old ring and grows it again rather than rebuilding (pre-existing). `adi_retap_tests` 218 checks (+17); 9 fail on the previous commit; six plants each fail them. TSan with a scope tap: 0 races. #231's CI was green on all 19 legs at the previous commit.

---

## 2026-10-26, afternoon — LANES §3 item 1c: the ring grow on a live graph (ADR-0085), a pre-existing race

**PR:** [arieladi/Adi#231](https://github.com/arieladi/Adi/pull/231) (`engine/delayline-grow-race`), draft, stacked on #229 (the `DelayLine` it fixes is the two-width one #229 made). #229's CI was green on 18 of 19 legs when this went up, both MSVC legs included, the macOS universal leg still running.

**What.** The race #229's reviews found. `Graph::accumulate` round-tripped each edge's cursors in every grow state, and the message thread writes two of them when it offers a ring and when it collects one. So an offer landing mid-edge lost its prime count, and the edge swapped at once and played zeros for most of its new delay. Three more races came with it: the state loaded per channel, `beginGlide` reading what the audio thread writes, and escalation reading the capacity before checking for a grow. Now there is one snapshot per edge, and the incoming ring's cursors are written back only in Priming and Fading. `beginGlide` checks a message-thread-only limit. A glide's end keeps a newer request pending; an offer supersedes a stale glide; a glide during a grow waits for the swap. No glide reads past the ring the audio thread holds.

**How it was checked.** A ThreadSanitizer harness (not committed; CI has no TSan leg): 7 distinct races over 194 grows before, 0 after. `adi_retap_tests` 175 checks (+36), deterministic: the interleaving run on one thread at the point a second thread could put it. Seven plants each fail them, and two graph-level plants are caught by TSan. `test_all.sh` 7949 checks across 95 suites. GCC and Clang `-Werror`: clean. A multi-lens adversarial review is running; what survives goes into #231 before it leaves draft.

**Found, not fixed there (pre-existing, item 1d, next):** an edge at zero delay takes `accumulate`'s fast path and never writes its ring (every edge has one at the default headroom of 8192). A later glide to a delay reads the ring as it was when the edge last had one: measured, the glide block's sample 127 is 6592 where the true value lies between 11776 and 11904, audio from about 210 ms earlier. With blocks shorter than the new delay, the block after the glide plays stale samples at full weight. ADR-0079 d2 ("both taps read the same history") is what the fast path broke. A handover adopting history from such a line inherits the same stale or zero history. Fix: the fast path keeps the ring written; a sleeping source's edge zeroes it until it holds only zeros, then costs nothing (ADR-0102 d5's skip stays). It will be benchmarked.

**Next.** 1d, then the plug-in hosts at 64 (CLAP 64-bit ports, VST3 `kSample64` through JUCE), the Pd tier, then LANES item 2 (sends and returns; its design workflow is finishing).

---

## 2026-10-26, later — LANES §3 item 1, second PR: the Processing Precision switch, the graph and the engine's nodes in double (ADR-0207 d2)

**PR:** [arieladi/Adi#229](https://github.com/arieladi/Adi/pull/229) (`engine/precision-64`), stacked on #227 (its two commits are in the branch; the diff narrows when #227 merges). #227 is green on every leg (19 checks, MSVC and both JUCE-on legs included) and ready for review.

**What.** `audio.processingPrecision`, 32 (default, Live's scheme) or 64 (Cubase's), behind the `AudioIo`/`NodeIo` seam: `Node::processesDouble()` (false by default, so a node that says nothing is handed float and converted either side at 64, Cubase's rule for a plug-in that cannot take 64); the graph's buffers, sums, delay lines (either width), suspension, taps and output at both widths; the engine's own nodes in double (sum, gain, junction, strip, input gain, console halves, clip playback, the MIDI source, the placeholder device); `Session::setPrecision` as a rebuild that carries every compensated edge's history across the widths; the registry key; `adi_block_benchmark --precision`. At 32 every float path is the same arithmetic; the suite that pins those bytes is unchanged. Plug-in hosts and the Pd tier stay float: their own PRs.

**How it was checked.** `adi_precision_tests` 92 checks (+57); every 64-bit claim planted and watched failing (16 plants, all caught). Two adversarial review workflows over the diff (lenses: 32-bit byte identity, 64-bit correctness, real-time safety, warnings, test quality, ADR conformance; each finding put to skeptics told to refute it). What they found and this PR fixes: history dropped at a precision switch; `out64` not written by the no-graph path, the fade and `SilenceProcessor`; the gain-stage analysis ignoring the precision; a strip's other-width ramp resuming a stale gain after 32→64→32 (a muted track leaking 5 ms); three of my checks that could have passed with their defect, and a double grow path no test drove; three comments that claimed more than the code. Cost of 64 measured: nothing up to 128 frames; from 256 up, +19 to +68% on the 8-track all-active fixture and +33 to +118% on 64 all-active tracks.

**Found, not fixed here (pre-existing, its own PR next):** `DelayLine::offer` (message thread, during a ring grow on a live graph) writes `inWrite_` and `primeRemaining_`, which the audio thread round-trips through `cursors()`/`setCursors()` in `Graph::accumulate` on every edge; `collectRing` writes `inWrite_` too. A grow that lands between the audio thread's read and its write-back can lose the prime count, and the edge then plays zeros for most of its new delay (about 170 ms at a 9000-sample tap). `run()` also loads the grow state per channel, so one edge's channels can see different states in one call. Fix: load the state once per edge, and have the cursors round-trip only in the states the audio thread owns.

**Next.** That race (item 1c), then the plug-in hosts at 64 (CLAP 64-bit ports, then VST3 `kSample64` through JUCE, proved on the JUCE-on legs), then the Pd tier; then LANES item 2, sends and returns, whose design is being worked out now (questions for win will be in its PR: Live keeps Pre/Post as one switch per return, the format keeps `pre_fader` per routing row).

---

## 2026-10-26 — LANES §3 item 1, first PR: 64-bit summing at every mix point (ADR-0207 d2)

**PR:** [arieladi/Adi#227](https://github.com/arieladi/Adi/pull/227) (`engine/double-summing`), ready for win's review.

**What.** Live processes in 32-bit float and sums every mix point in 64-bit (Audio Fact Sheet
38.2.4); ADI's graph summed in float. Now a node's main and sidechain sums (`Graph::mix`), the
overlapping clips of one track and a clip's mono fold accumulate in double and convert to float
once. A mix point with at most one live input keeps the float path, which gives the same bytes,
so chains and the sleeping tracks of ADR-0102 d5 pay nothing; `GraphStats::wideMixes` counts the
double sums.

**The test.** `adi_precision_tests` (new suite, 33 checks): ADR-0205 d6's statement of the Fact
Sheet case (pairs of 24-bit files and their inverted copies, summed many times, cancel to zero)
at every mix point, each beside the same sum in float, which must not cancel: 64 tracks at the
master; inside a group; through compensation delay lines; on the sidechain bus; through a real
project's 24-bit WAV clip tracks, with and without per-pair volume and pan; 16 overlapping clips on
one track; a six-channel fold. On `main` 11 of the then 24 checks fail (residue 2.26e-6 at the
master, about −113 dBFS); three plants (graph sums float, clip sum float, fold float) each fail.

**Numbers.** `test_all.sh` 7853 checks across 95 suites (main 7820 across 94). GCC and Clang
`-Werror` with the hardened library: clean. The benchmark cost is in the PR, measured pinned to one core (unpinned runs here swing ±35%).

**For win.** The Live 12 manual text is not in a cloud checkout (`reference/` is not fetched by
`--build-only`), so the test follows ADR-0205 d6's description of 38.2.4; worth one look against
the manual's own wording. `docs/FEATURES.md` row 85 still says "the graph sums in float today".

**Next.** LANES item 1, second part: the 64-bit end-to-end mode behind the Processing Precision
switch (`audio.processingPrecision`, 32 by default), starting with the seam and the graph.
