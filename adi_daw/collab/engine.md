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

## 2026-10-30 — MSVC CI repair, direct stems and PCM export

The window opened #301 (`engine/export-stems` → `engine/export-queue`) at a4e814a. The window reports #297 all green; #299 has 18/19 green with macOS still running. #298's Windows MSVC Debug mixer timed out at the existing 600-second limit; its other 94 tests passed. Those reports describe the pre-repair heads; new CI still needs relaying because `api.github.com/graphql` remains Forbidden here. Win merges.

### #298 CI repair

`f76e36b4cb9bc600b6d7d1e6502a49fa9fce406d` splits master32/master64/nested-console checks into `adi_master_summing_tests`, built from the same fixtures and test source. The remaining mixer has 330 checks; the new master suite 98: all 428 preserved, with identical render lengths and no higher timeout. Both binaries pass GCC and Clang Werror (approximately 3.4s and 1s here; these are Linux timings, not an MSVC claim). `quietRun` replaces the inner `run` that triggered C4456. The ordinary shared runner keeps all functions referenced under strict Clang, without warning suppression. Full GCC validation at this head: **8623 checks / 96 suites**, all validators clean.

The repair was merged (no rebase or force push) into the export stack and all heads were pushed:

| Branch / PR | Current head |
|---|---|
| `engine/master-summing`, #298 | `f76e36b4cb9bc600b6d7d1e6502a49fa9fce406d` |
| `engine/export-queue`, #300 | `f9b86f494dbb7799bf2c6685ffdbc64764d5ee88` |
| `engine/export-stems`, #301 | `f5fdf47655d05fdfc344f37c824def33bf51b93a` |
| `engine/export-pcm`, PR requested against `engine/export-stems` | `186ae70ce1265aab141df147ff22df2b2d72ada8` |

### #301: direct wet stems and one cycle-marker range

Feature commit `a4e814a`: graph output capture preserves 64 channels and 32/64 precision without graph mutation, extra processing, allocations or master-output changes. Session resolves direct pre/post-fader source endpoints; post-fader is before the parent's console encoder and pre-fader remains after the source's own effects. The full graph still runs, retaining returns and sidechain dependencies. Cropping uses the selected endpoint's cached signed latency. Jobs can resolve a current cycle marker's name and tick/ns bounds at render start; absolute endpoints round independently. New `{region}` and source-aware `{track}`/`{group}` names are documented.

Before the inherited CI repair: GCC 8814 checks/97 suites, export 158, capture 33, validators clean. Clang export/capture/graph/mixer/session/precision 6/6. Tests decode actual source/group/return/multichannel WAVs, console boundaries, pending region edits, return echoes, signed delay, full-width double/float capture, segmented callbacks, sidechains and stale output. This is not dry/isolated-through-master processing or a continuous gapless region batch.

### PCM WAV and dither

Feature commit `39271d0d037b1288c892d41ea0597d7ff545a488` on `engine/export-pcm`, based on `engine/export-stems`: PCM16/PCM24/Float32 jobs, none/triangular/rectangular dither. **New jobs now default to PCM24/triangular, matching the existing registry; callers wanting the initial runner's Float32 must select it explicitly.** Standalone WavWriter retains its old no-dither default and float bit behavior. No registry/schema changes.

Double audio stays double through final integer quantization. Quantization rounds signed ties away from zero and saturates; seeded noise is applied once in destination LSBs. Generator state continues across channels and writes; a stable enqueue ordinal distinguishes job streams, while retries reproduce samples. Float32 never dithers. Unsupported enum values fail rather than selecting a fallback; Pow-r/noise shaping is not implemented.

Before the inherited CI repair: GCC 8871/97, export 196, WAV 143, validators clean. Clang WAV/export/decode/media/clip-playback 5/5. Three compiled defect plants all fail: premature float narrowing 2 checks, removed dither 8, per-write reseeding 4. Seeds, stereo independence, quantized noise statistics and callback-partition identity are covered.

**Final combined validation at `186ae70`: GCC Debug/Werror, libpd on/JUCE off, 8871 checks across 98 suites, all validators clean.** Export 196, capture 33, WAV 143, mixer 330 and master 98. Clang Werror mixer/master/WAV/export/capture/decode/media/clip-playback: 8/8. These are local results; the new MSVC/seven-ABI CI results remain unverified here.

### Continuing item 4

LUFS normalization remains next, with NI Stems export/import, dry/isolated master routing, persistence and window integration still open. Region batch/matrix/After Export are P2 in ADR-0205 d9; no gapless claim has been made. No later item or the whole engine queue is claimed complete.

Read-only planning found usable K-weighting coefficients (`AutoGain::kShelf/kHighPass`) and private stereo integrated-loudness code in gain_stage.cpp. Export must not call Session::autoGainStage: it commits input-gain operations and alters analysis routing. A post-render pass should measure and normalize a private full-width spool from one Session/snapshot; re-rendering plugins or using a Float32 spool can change the result. Existing TruePeakMeter is float-only/eight-channel and LookaheadLimiter is sample-peak, so neither alone satisfies the required full-width true-peak ceiling. Multichannel LUFS needs explicit channel-role weights.

There is no production AAC/NI container path yet. Platform codec adapters, independently produced NI fixtures, stream identity/priming/padding handling, and atomic project-owned media import are needed. Those are independent engine implementation tasks, not a new director approval gate. Win/window own their UI integration and final merge; the engine continues its queue.

---

## 2026-10-30 — window opened N6/export PRs and retargeted item 3

The window lane reports [#299](https://github.com/arieladi/Adi/pull/299) (`engine/snapshot-n6` → `main`) and [#300](https://github.com/arieladi/Adi/pull/300) (`engine/export-queue` → `engine/master-summing`) opened. #297 and #298 now target `engine/return-loop`. CI is running on all four according to the window; final success is not yet reported. Codex attached #299/#300 and retried the API: still Forbidden. Win retains merge ownership. Next local work continues item 4's full-width source capture and stem exports on a branch above #300.

---

## 2026-10-30 — item 3 integrated with sends; N6 ready; first export runner ready

Codex acted on the window lane's 30 October handoff. `engine/log` was fast-forwarded to `eb2241d` and pushed; `engine/log-cloud-takeover` was then deleted locally and remotely. This entry is on `engine/log`, preserving #240 as the log's one PR. No feature PR was merged, rebased or force-pushed.

### Item 3: #297 and #298

Both branches now contain `engine/return-loop` (`9f9fbc6`) through merge commits:

| PR / branch | New head | GCC full suite, Debug/Werror, libpd on, JUCE off | Focused Clang Werror |
|---|---|---|---|
| [#297](https://github.com/arieladi/Adi/pull/297), `engine/bypass-delay` | `bc9ecf33c7ed46aa0427035cb7d427b3b113a16c` | **8575 checks / 96 suites**, validators clean | bypass, graph, host, mixer, plan, realize, retap, precision: 8/8 |
| [#298](https://github.com/arieladi/Adi/pull/298), `engine/master-summing` | `fcfaecb6fa2cfd3c3b885de3216dbeb028392caf` | **8623 checks / 95 suites**, validators clean | graph, host, mixer, plan, realize, retap, precision: 7/7; final mixer 428 checks |

The bypass merge preserved both sets of virtuals/comments and exposed an auto-merge defect: dry-history adoption had landed inside the helper newly called for both plan and chain edges, so it copied/counted each device twice. An exact-count regression failed in all four handover cases before the fix. Adoption now runs once, and counts only `DelayLine::adoptHistory` successes. A new combined return-loop test disables a 100-sample device at frame 768 while an echo is inside its history, rebuilds, and optionally changes precision. Echoes remain at 200/812/1424/2036, feedbackTrip stays 612, and device DSP stops. Final suite counts: bypass107, graph200, host124, mixer272, precision104.

The master merge retained per-send console halves and all incoming tests. Added checks render sends into a group through a summed master at both precisions, and preserve distinct group-send/master-output flavours and history when master summing is disabled. Direct sends into the master remain refused by the existing planner. Mixer: **428 checks** under GCC and Clang.

**For win/director:** retarget both PRs to `engine/return-loop`, and relay/watch CI for these new merge heads, including seven ABIs and MSVC `/WX`. The window's earlier green results predate these commits. `gh pr view` still fails with API Forbidden in this environment; no current remote CI success is claimed. The director must publish the already-saved environment settings with `api.github.com` allowed before Codex can handle API operations itself.

### N6: snapshot metadata for the window

Ready branch [`engine/snapshot-n6`](https://github.com/arieladi/Adi/tree/engine/snapshot-n6), **`98f2275501c587ef7912ce52a0a56f5023e86f81`**, based on main. Please open its PR against `main` (title: “Expose hierarchy and timeline metadata in engine snapshots”).

- `TrackNode`: `parent` (int64, 0 at top), `indexInParent` (int64), `kind`, `color` (optional int64; NULL and zero remain different). Every field participates in `same()`.
- `ClipNode::name` is read and compared by `same()`.
- `Snapshot::meters` keeps the old step-7 `vector<textproj::Meter>` API: SQL `pos_ticks` maps to `start_ticks`, with numerator/denominator. The root is rebuilt every time; meter edits retain unchanged track/clip/tempo nodes. An implicit 4/4 at zero matches text projection and preserves bar numbering when the first stored signature is later.
- The flat `(index_in_parent,id)` traversal is explicitly documented as not being display order.
- **8222 checks / 95 suites**, validators clean; snapshot/engine suite **99 checks**. Clang engine/project-view suites pass. Independent edits test each field, old snapshot immutability, nullable/wide color, meter sorting/insertion/deletion, and unchanged-node sharing.

**Window handoff:** after win merges N6, remove the fallback project-file reads described in STEP-7-PLAN §2.2 and use these snapshot fields. N6 is ready for review, not yet claimed landed. No UI files were changed.

### Item 4: first export increment

Ready branch [`engine/export-queue`](https://github.com/arieladi/Adi/tree/engine/export-queue), **`8a64ad636dce3b9255926edf47df0bce7310efe2`**, based on `engine/master-summing` at `fcfaecb`. Please open its PR against `engine/master-summing` (title: “Add export jobs and isolated master WAV rendering”).

`adi/render/queue.hpp` adds application-only jobs, immutable status snapshots, pending edits/removal, explicit retry, cancellation, and retained results. Each job loads a fresh read-only Store/Session/device set under one SQLite snapshot at render start. `rows::readModel` now respects a caller-owned transaction; a concurrent committed-edit regression proves that audio and filename metadata come from one revision.

The first runner streams the master into Float32 WAV, preserving 32/64 graph selection until encoding. It prerolls from zero, crops the heard range with latency compensation, bounds buffers by block/channel count, checks reads/underruns/nonfinite values, and rejects any latency/topology report, including a final-block report or a shorter branch whose change leaves total master latency unchanged. Brace names use the registry form; `{counter}` is the stable enqueue ordinal so removing results never reuses it. Cancellation/failure removes private staging only; publication never overwrites. Host message-thread contracts remain the caller's responsibility; plugin offline-quality negotiation is not added.

**8708 checks / 96 suites**, all validators clean. New export suite: **85 checks**, decoding actual WAV files for ranges/PDC, fresh state, precision, 48→44.1 kHz conversion, concurrent edits, callback mutation, queue lifecycle, cancellation, racing destinations, failures and moving latency. Clang export/plan/session suites pass. The API and limitations are in `docs/ENGINE-EXPORT-QUEUE.md`; prepared PR bodies also remain in `/workspace/adi-review` in the cloud workspace.

**Item 4 is not complete.** Next increments: full-width track/group/region capture and wet/dry/master-path selection, normalization/LUFS, dither and other encoders/bit depths, NI Stems export/import, job persistence and window wiring. The current API explicitly writes Float32; it does not silently claim the registry's PCM24/triangular defaults. Freeze/bounce, history/save and lazy Q/L slots remain after item 4. Existing decisions authorize independent implementation; no new approval gate is inferred from the missing UI.

---

## 2026-10-29 — cloud takeover: LANES §3 item 3 implemented, integration pending

The director asked Codex to take over the engine lane queue while setting up its cloud environment. The latest `engine/log` handoff through #249 was read before selecting work; the sends stack was not recreated. The newer five-lane ownership in `win/lanes-5` was checked: the window and Pd/native device work remain with their lanes.

**Bypass retains delay:** [`engine/bypass-delay`](https://github.com/arieladi/Adi/tree/engine/bypass-delay), commit `e981850e75d581bfa2e3f8559927d8c3832bc538`, based on main `b773bea`.
- Graph-owned dry lines preserve latency without device DSP, retain active-period history, drain compensated input, and follow latency retaps/growth.
- History crosses graph rebuilds and precision changes; double bypass stays double even for a float-only device. Zero-delay active devices take the existing forget/fast-path approach.
- Bypass/always-process flags are atomic. The graph's sampled bypass decision selects DSP once per block.
- GCC 14.2 `ADI_WERROR=ON`: `test_all.sh` **8287 checks across 96 suites**, all validators pass. New bypass suite: **107 checks**.
- Clang 19.1.7 `ADI_WERROR=ON`: device-model, bypass, graph, host and precision suites pass (5/5).
- Five plants caught: zero bypass latency, unprimed dry history, DSP during bypass, narrowed double bypass, omitted handover. Originals restored and the 107 checks rerun green.
- Standalone `DeviceNode::process` retains its old pass-through helper behavior; graph execution owns the delayed bypass contract.

**Master console summing:** [`engine/master-summing`](https://github.com/arieladi/Adi/tree/engine/master-summing), commit `22b307d8745f1c9677aab989d72b73acf475805a`, independently based on `b773bea`.
- Existing summing operations and graph admission accept master beside group, per ADR-0203 d14. Other track kinds remain refused.
- Existing channel/buss halves, undo, drive, flavours and state preservation are reused. Nested summed groups retain both their own buss and the master's channel half.
- GCC `test_all.sh`: **8296 checks across 95 suites**, validators clean. GCC and Clang mixer suites: **221 checks** each, including all eight flavours at 32/64-bit graph settings and a nested render.
- Two plants caught: rejecting master operations and omitting master console processing. Restored suite green.
- The 64-bit mixer checks validate graph compatibility and rendered output, not a new claim about each console core's internal precision.

**Integration state.** Both branches are pushed; neither is merged. No PR was created and no remote CI result is claimed: the cloud egress proxy returns CONNECT 403 for `api.github.com`. Git HTTPS reads and pushes work with existing platform authentication. The required domain was added to the environment draft alongside tested install/start instructions; the user must review/save settings and publish. Windows/MSVC, the seven-ABI CI gate and any director parity acceptance remain unrun here. This log change has its own branch so item branches do not collide in this file.

**Environment.** The isolated checkout is `/workspace/Adi`; no worktree was created. The baseline main build passed 8179 checks across 95 suites before coding. CMake 3.31.10/Ninja 1.13.0 are installed under `/workspace/adi-tools`; Clang 19.1.7 was extracted from packages verified through Debian's signed index into `/workspace/adi-clang`. The headless workflow includes real libpd and CLAP tests. JUCE GUI/VST3 and hardware audio were not installed or validated. Generated WAV test renders were preserved outside the checkout.

**Next, still unimplemented: LANES item 4, the export queue.** Existing decisions authorize the work; lack of an ADR number or UI is not a reason to wait. Start with jobs plus a bounded offline master-WAV runner, strict registry-brace naming, isolated Session/device instances, heard-timeline range alignment, cancellation and no-overwrite publication. Reuse `audio::WavWriter` and `media::publishFile`. Subsequent pieces are full-width track/stem capture, normalization, encoders/dither, and NI Stems import/export. `ScopeTap` is float visualization storage and must not silently become a double/multichannel export sink.

Items 5–7 (freeze/bounce, history caps/save operations, channel slots) also remain. They have independent authorized implementation work: freeze should reuse the offline pre-fader renderer and preserve live mixer controls; history must preserve named snapshots, branch anchors and unuploaded collaborative ops; Q/L slots should be lazy and use an injected factory rather than guessed clone identifiers. Actual clone DSP and UI handoffs are external dependencies. No claim is made that the whole engine queue is finished.

---

## 2026-10-27, evening — LANES §3 item 2, sends and returns: Live's feedback loop

**[arieladi/Adi#249](https://github.com/arieladi/Adi/pull/249) (`engine/return-loop`), stacked on #247: a return's send into itself plays (ADR-0203 d3).** With it, item 2's planner and loop work is done; what remains of item 2 waits on win's questions (PRs 8 to 11).
- What the return sends itself comes back into its junction one largest block (D) later, at the send's gain each trip. A ring per row sits between a writer at the end of the send and a reader at the head of the return, outside the graph's order.
- The loop is not compensated. The trip is D plus the return's chain latency up to the tap. A sender's latency does not move it, and neither does the return's own track delay. `Session::feedbackTrip` reports it.
- A loop comes back into play empty. The swap decides this on the audio thread, against the graph that really played last.
- The echoes in flight cross a rebuild, a precision change and a new block size. A new sample rate or a device stop empties the loop.
- A loop that runs away is held at +120 dBFS. A NaN is written as silence. Below −600 dB the writer writes zero, so a decaying loop reaches true silence and its return sleeps (without it, a loop between −6 and 0 dB never gets below a subnormal).
- `test_all.sh` 8460. 27 plants, each caught. A five-lens critique ran before the code. It found the writer's input edge playing the track delay, which made the trip depend on the senders. It also found the missing ceiling on a runaway loop, and two retap checks that could not fail. All three were fixed before the PR.

**For win, adding to question 8 (the loop):**
- D is the graph's largest block, offline too, until AWAITING 7(f) measures Live's figure.
- Should the return's own track delay lengthen its loop? It is pinned at no.
- The +120 dBFS ceiling and the −600 dB floor are ADI's guards. Live's behaviour is not measured.

**Next:** LANES item 3: a bypassed device keeps its delay (ADR-0203 d4), then the master summed (d14). The Pd fixed block now belongs to the devices lane.

---

## 2026-10-27, late afternoon — LANES §3 item 2, sends and returns: a return's own sends

**[arieladi/Adi#247](https://github.com/arieladi/Adi/pull/247) (`engine/return-sends`), stacked on #245: a return's own sends play (ADR-0203 d3).**
- A return's send is planned like any track's. Enable Send is the row's switch, so a send switched off is still planned and turning it on retimes nothing.
- A return can send into another return. Solo follows through #245's closures, unchanged.
- A return's send back into a track that feeds it is a loop, refused like every loop.
- Two returns feeding each other (R1 into R2 and R2 into R1) is a ring. `docs/AWAITING.md` row 7(f) asks whether Live allows one. Until it is measured, the later row loses, with its own message.
- `test_all.sh` 8396. Six plants, each caught.

**Next:** PR 7b, Live's feedback loop, a return feeding itself.

---

## 2026-10-27, afternoon — LANES §3 item 2, sends and returns: solo across sends

**[arieladi/Adi#245](https://github.com/arieladi/Adi/pull/245) (`engine/sends-solo`), stacked on #244: a soloed return hears its senders (ADR-0203 d3).**
- A Post send plays its source's strip itself, in its node's first stage, and leaves where a Pre send does. The strip keeps its solo, so a sender soloed away is silent while its send plays. No node comes or goes with a send or a solo.
- `hearing()` gives one gate per track and one per send. A soloed return hears a group's send (the group's children open into the group alone) and a send's send; a defeated vocal plays into its defeated reverb; a soloed VCA no longer silences the project.
- The plan's SoloGate node was not built. A three-lens critique confirmed it would leak a soloed-away track's dry for one callback on arrival, and drop a delayed track's dry for 300 samples when its first send came.
- `test_all.sh` 8381. Twenty plants, each caught.

**For win, adding to question 7:**
- Should Solo in Place open a non-return track a soloed track sends to, its own audio included? It is pinned at yes.
- Keep every return under Solo in Place, or only those sent to?
- Should a defeated track inside a closed group have its reverb play without its dry?

**ADR-0172 d3** says sends follow the track delay "because they leave from the same tail". They still follow it, through the send's first stage. The ADR text is win's to amend.

**Next:** PR 7, Live's return loop (a return feeding itself). PRs 8 to 11 wait on questions 5, 4, 1 and 7, and 9, so after PR 7 the lane moves to LANES item 3. (PR 7 was split in two: #247 is its first half.)

---

## 2026-10-27, afternoon — sends and returns: the review round of #242 and #243

An adversarial review, two lenses with a skeptic each, of #242 and #243. Every confirmed defect is fixed, with a plant per fix, and #244 is merged forward and re-checked.

**[arieladi/Adi#242](https://github.com/arieladi/Adi/pull/242), `7f8c41e`.** `test_all.sh` 8275.
- Asleep, a strip lands its gain. A send pulled down in a gap between clips used to play its old level for 5 ms on the next sound, and so did a track muted in a gap (pre-existing).
- A send's level has a top, +24 dB, as a guard until Live's is read (AWAITING 7(a)). +inf or 800 dB used to put infinity into the master.
- `StripNode::prepare` no longer races the audio thread. ThreadSanitizer: one race before, none after.

**[arieladi/Adi#243](https://github.com/arieladi/Adi/pull/243), `b0f72ca`.** `test_all.sh` 8327.
- A send into a summing group gets a channel half of its own. It used to play +5.49 dB at console9, because the buss decoded what nothing had encoded.
- Solo in Place opens a track that is not a return only through a send that carries something.
- A send row with id 0 is refused; it used to play as a plain route at 0 dB.
- A self-send that is not a return's is refused as a loop.
- A send back in the plan starts at its level.

**[arieladi/Adi#244](https://github.com/arieladi/Adi/pull/244), merge `20eff41`.** `test_all.sh` 8339.

**Found, not fixed (pre-existing, outside these PRs):**
- A strip's own volume has the same hole a send's had: `volume_db` of +inf, or `mixer.setVolume` at 1000 dB, plays as infinity. The fix is a fader top, which is AWAITING's too.
- A track that leaves the plan and comes back ramps its strip from the last gain it played, as a send did. That is rare: a delete undone.
- `StripNode::setTransport` writes a plain pointer each sync while the audio thread reads it. The value never changes, so the race is benign, but it is formally a race.

**For win, with question 7 (Solo in Place):** with a send ON from a soloed track into an audio track (ADI's addition), that track's own audio is heard too, as it is through a main route into it. Should Solo in Place open only what arrives by the send? That needs the plan's per-send gates (PR 6).

---

## 2026-10-27, midday — LANES §3 item 2, sends and returns: Pre sends play

**[arieladi/Adi#244](https://github.com/arieladi/Adi/pull/244) (`engine/sends-pre`), stacked on #243: Pre sends play, before the fader (ADR-0203 d3).**
- A Pre row leaves after the effects and before the strip, so the source's fader, pan and track delay do not reach it.
- Its own switch takes the source's mute and solo, and it reads the source's mute lane. Its lane is kept alive with every graph that plays it, as a strip's lanes are.
- The send's own pan is now tested through the session.
- Six plants, each caught on its own. `test_all.sh` 8313.

**Question for win, the tenth:** does Live delay a Pre send by the source's Track Delay? Here the delay belongs to the strip, so a Pre send leaves before it. Changing the answer touches one place (where the tap is) and flips one test.

---

## 2026-10-27, morning — LANES §3 item 2, sends and returns: the plan's first three PRs, and what they need from win

The plan is in six steps after these: Pre sends and send pan, solo across sends, Live's return loop, sidechain keys reaching devices, Live's tap points on routes, the return switches and Sends Only, Cue. Each is its own PR.

**[arieladi/Adi#241](https://github.com/arieladi/Adi/pull/241) (`engine/chain-history`): an edge inside a track keeps its ring history across a rebuild (ADR-0092).**
- Before, only the plan's edges between tracks were carried. Every edit (a fader move included) dropped the samples of any delay played inside a track:
  - a summed child's track delay, on the edge from its strip to its channel half;
  - a clip track compensated against a later input, on its clips' edge into its junction.
- Edges are keyed by where they leave. A clip reader, new at every rebuild, names itself. A plan edge also carries what its wire is: a console half's output, so toggling summing never plays driven history as the plain track's. That was a +12 dB burst, pre-existing.
- Two pushes. The second fixed four review findings: real clips were never matched, a replaced channel half dropped the edge, the summing toggle burst, and the carry count.
- `test_all.sh` 8225.

**[arieladi/Adi#242](https://github.com/arieladi/Adi/pull/242) (`engine/send-groundwork`), stacked on #241: a send can be realised.**
- `EventFlow::Stop`: a send carries no notes, holds none awake and acknowledges no note-off.
- `PlannedEdge` gains a routing id and a `Tap`. `realize` wires tap → `SendNode` → the destination's junction, and both edges keep their history.
- A caller's node is placed once.
- `SendNode` is a strip's gain stage: bit for bit at 0 dB at either width.
- `test_all.sh` 8269.

**[arieladi/Adi#243](https://github.com/arieladi/Adi/pull/243) (`engine/sends-play`), stacked on #242: sends and returns play.** ADR-0203 d3 reverses ADR-0072.
- Post sends into returns or any track, planned after the main edges.
- A send switched off is planned anyway, at gate 0, so Enable Send never retimes.
- Refused per row, named: a loop (the later row loses), the master, a VCA. Named as not yet: Pre sends, a return's own sends, Live's return loop.
- Live's Solo in Place keeps a soloed track's returns audible.
- `test_all.sh` 8301.

**Not yet, and why it waits:** a soloed RETURN does not yet hear its senders. Solo silences them in their strips, before the send's tap. That needs the plan's solo gate after the tap, step 6.

**Questions for win** (the format and the ops). The engine plays rows as they are, so nothing here blocks the next engine PRs:
1. **A new send's level.** Live starts a send fully down (ADR-0203 d3, measured by d26). `connect` writes `gain_db` 0, so a send made by the ops is born at 0 dB, and -inf is refused (`ops.cpp`).
   - Should SPEC 6.7 make -150 dB the floor?
   - Should `connect` take level, pan, pre and enabled?
   - Should a disconnect's undo restore the whole row?
   - Should `setEnabled` be a GraphRebuild op?
2. **The send's pan.** Its range, its law, and whether it sits on top of the source's pan. Today a Post send taps after the strip's pan, and its own pan is Live's law.
3. **Pre/Post is one switch per return in Live**, and the format has `pre_fader` per row. Does it get a per-return home, or are the rows written as a batch? Which wins on a row that disagrees?
4. **Enable Send / Enable All Sends** for a return's own sends: where does the switch live in the format?
5. **A send into a summing group**: does it get a channel half of its own? Today it enters the sum bare.
6. **Sends Only** (ADR-0203 d3, the fourth state of ADR-0065's rule): a disabled user main row, or a field?
7. **Solo in Place**: a project or an app setting? It is always on today, as Live's default.
8. **The sidechain endpoints for devices** (`src_kind`/`dst_kind` 'device'). Sidechain keys reaching a compressor need them.
9. **An ADR to amend** ADR-0092 d1 and ADR-0163 d4/d6 for sends and their solo, and to correct SPEC 6.9 (line 622) and FEATURES.md row 57, which say input gain is stored and not applied; the session plays it.

---

## 2026-10-27, early — LANES §3 item 1g, the Pd tier at `PD_FLOATSIZE=64` and the native devices in double: its files are the devices lane's since ADR-0208, so it is handed over with what the survey found

**Not started as a PR; blocked by ownership, not by work.** ADR-0208 d3 (win's #238) gives the devices lane `pd/**`, `src/adi/dsp/**`, `src/adi/pd_builtins/**` and `src/juce/pd_*`, and says a need across a line is logged, not edited. Building the Pd tier in double changes all of them together: flipping `PD_FLOATSIZE` without the externals and the cores breaks every Pd device. The engine side of it is one CMake line, and that line must land with the rest. So the item goes to win to place, and the engine lane takes item 2.

**What a four-agent survey measured** (libpd 0.16.1, Pd 0.56-5; scratch builds at both widths, nothing in the tree changed):
- **libpd's CMake has no float-size option.** `PD_FLOATSIZE=64` has to be defined by ADI, PUBLIC on `libpd_static` and restated PUBLIC on `adi_core`, beside `PDINSTANCE`. A mismatch is refused for externals (`class_new64`) but silent for `pd_engine.cpp`, which builds atoms with `SETFLOAT` and calls `pd_float`.
- **It is one width per process.** Two libpds in one binary link silently to whichever archive comes first. A dual build needs a renamed copy of the whole archive (`llvm-objcopy --redefine-syms` worked on Linux; Mach-O and COFF were tried on toy objects only), and the externals, `PdBuiltins` and `LibPdEngine` compiled once per width. So a double Pd computes in double at the default 32 too, converted at the device's seam. Vanilla Pd measured the same CPU in double (64 `biquad~`: 0.739 s against 0.727 s for 60 s of audio).
- **At 64 every built-in device breaks without a word.** All 17 externals cast Pd's `t_sample*` to `float*` (70 casts, through `t_int`, so no warning) into cores that take `float*`. With the flag on, 15 compile clean under `-Werror` and produce wrong audio (`adi.utility~`: 3898 of 7872 samples wrong; AutoFilter peaks at NaN). All 16 device suites fail. Two fail to build: `combchord_tilde.cpp:35` (`std::clamp` of a `t_floatarg` against `float` bounds) and `colorcab_tilde.cpp:23` (`-Wfloat-conversion`). Both one-line fixes are no-ops at 32.
- **With a conversion at each external's edge, all 16 device suites pass at 64.** 14 of the 15 render WAVs are bit-identical to 32. AutoFilter differs by at most 2.98e-8, from two patch defaults a float cannot hold. Every golden, exact and tolerance check survives.
- **`LibPdEngine` itself compiles unchanged at 64**, and its float path stays exact against a double libpd. Going double end to end means:
  - double interleave buffers with `libpd_process_double`, or `libpd_process_raw_double`, which is planar like the graph;
  - templated push and pop for `in64`/`out64`;
  - five `sizeof(float)` byte counts made generic;
  - `PdDevice::processesDouble()`.
- **`test_pd_engine.cpp`'s planted-loader check** (`d8`) fails at 64, because Pd at 64 looks only for deken names (`*.linux-amd64-64.so`). The plant needs the deken name.

**The order the survey recommends** (for whichever lane win gives it):
1. The two compile fixes.
2. A guard so no perform routine can cast a signal vector to `float*`: a typed accessor or a `static_assert` on `sizeof(t_sample)`.
3. Each core in double, templated on the sample type: "the native devices in double", and a per-core numeric review, not a mechanical edit.
4. `LibPdEngine` and `PdDevice` double.
5. The define, in one place that reaches every translation unit.
6. The 20 Pd suites re-run, expecting last-bit render deltas.

**For win (the policy, not a gate):** libpd's width is a build property, and the Audio page's switch is a runtime one. One libpd built at 64 means Pd computes in double at both settings. ADR-0207 d2's "building the Pd tier in double" reads that way, but at 32 it is not bit-identical to today.

**Found, not fixed (pre-existing, device tier):**
- After a sample-rate change, published arrays (the analysers' taps) publish at the old rate. `bindArrays` computes their timing at bind time, and `LibPdEngine::prepare` never recomputes it: 60 Hz instead of 30 at 96 kHz.
- libpd's `libpd_read_array`/`libpd_write_array` return -2 still holding `sys_lock` on a range failure. ADI is safe only because it checks `libpd_arraysize` at bind time.
- The Pd suites' `TempDir` is not unique per process, so parallel runs can collide.

---

## 2026-10-27, early — LANES §3 item 1g-a: a Pd device left alone by an unchanged rebuild, and held off the audio thread when it is not

**PR:** [arieladi/Adi#239](https://github.com/arieladi/Adi/pull/239) (`engine/pd-prepare`), draft, on #236 (merged). It fixes the defect this log recorded under #236. CI green on the legs finished at writing.

**What.**
- `PdDevice::prepare` returns at once on an unchanged rate and block (ADR-0090). Before, every rebuild anywhere in a project re-initialised every Pd device: a block of audio lost, and every sounding note forgotten, its note-off never sent.
- The rest runs under #236's gate. A rebuild prepares the next graph while the last one plays the same device (ADR-0089 d4), so the message thread was reassigning the adapter's buffers under the audio thread.
- `release()` marks it unprepared, so a restart at the same format starts clean.

**Checks.**
- `adi_pd_tests` 78 (+16), on a fake engine whose `prepare` and `process` can be held.
- `adi_pd_engine_tests` 600 (+2), through real libpd: across the `prepare` a rebuild makes, `adi-proof.pd` comes back unchanged. Without the fix, 64 samples are wrong.
- Six plants, all caught. `test_all.sh` 8014. GCC and Clang `-Werror` clean. TSan 0.

**Ownership:** it edits `src/juce/pd_device.*` and two Pd suites, which ADR-0208 gives the devices lane for the zero-delay adapter. It predates ADR-0208 and fixes the device contract (ADR-0090) that the adapter's work starts from. Win may merge it as it stands, or hand it over.

---

## 2026-10-27, early — #237, second push: what the review of the VST3 at 64 found

**PR:** [arieladi/Adi#237](https://github.com/arieladi/Adi/pull/237), commits `25e040c` (the fixes) and `7dacaed` (main merged in; one conflict in `Graph::prepare`, both sides kept). A two-lens review, each finding put to a skeptic, confirmed three defects. All were introduced by #237; the first was also in the 32-bit step before it.

**Fixed.**
- **The VST3 render steps could not hear a silent plug-in.** The demo project's audio clip plays at -6 dBFS from 0 to 1.5 s, and `--require-peak` takes the whole render's peak, so the clip met it alone. `--require-double` reads what the host set up, not what it played. Each step now also renders from 2 s, where only the VST3 sounds, at 32 and at 64. Plain's negative run must exit 2 with its own line.
- **The probe's 64-bit section read only the left channel, and both channels held the same signal.** It now records and pre-fills both, and feeds each its own noise.
- **`adi_play` counted "handed double" from what a device says now**, not from what the graph decided. `DeviceNode` now remembers the width it was told, and the count needs both.

**Checks.**
- `adi_device_model_tests` 70 (+3).
- Eight plants, all caught. Three are silences that the old step command lines passed; one of them, a session-path silence at 64, is invisible to the probe.
- `test_all.sh` 8189 across 95 suites, merged with main. GCC and Clang `-Werror` clean.

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
