# held-engine-win: the engine-win seat's held work, gathered into one chain

**This chain lands only when the director says so.** Nobody opens a pull request for it before then.

- Chain: `held/engine-win-chain` (on origin). Log: this file, on `held/held-engine-win-log`.
- BASE: `origin/land/basics-1128c` while it is not yet in origin/main; origin/main once it is
  (`git merge-base --is-ancestor origin/land/basics-1128c origin/main`). Merge the newest BASE into the chain
  before each push.
- Build and test: the build PC's slot 16 through `bash /d/adibuild-tools/slot.sh run 16 origin/held/engine-win-chain <core|ui|all>`.
- Mission: gather the finished, never-landed work of the engine-win seat (branches `origin/engine-win/*` not merged
  into origin/main) into one proved chain. Survey, chain, proof, account.

## State (newest first)

- 2026-11-28, round 3: **all 18 taken stacks are on the chain** (17 merged + project-snapshot-backup taken by
  another route). (13) export-profile-validation `abad9cd1` (decisions in its commit message and the chain
  table), (14) source-hash-flac-proof `a43678f6` (no conflicts). BASE unchanged (origin/main `629a4834`).
  Validators pass at `abad9cd1`.
  **Round 1's check reported** (on `c8a41862`, merges 1-5): `CORE CONFIGURE FAILED`:
  `cmake/sqlite_recover.cmake:13 Missing pinned sqlite-recover; run tools/fetch_external.sh --build-only`.
  project-salvage (5) added a pinned sparse clone of `sqlite/sqlite` (three `ext/recover` files) to
  `fetch_external.sh`; the build PC's third-party step did not produce it. See Proof for what was done next.

- 2026-11-28, round 2, end: **BASE is now origin/main** (land/basics-1128c merged in #838). Chain
  `held/engine-win-chain` at `77224fc8` holds 16 of the 18 taken stacks. Round 2 merged, in order: BASE
  (origin/main at `629a4834`) `5579e020`, (9) ffmpeg-pcm-stream `7f9328ab`, (11)
  publication-safety-integration `d6df55e2`, archive-document-control `cff34908`, render-job-json-full-bytes
  `190bb7e5` (a no-op: its NUL guard came with the publication stack), (12) clap-transport-gate `df3f8e2d`,
  (15) comp-take-splice `2968450d`, (16) scanner-clean-mode `f0a41333`, (17b) project-sync-folder `77224fc8`
  (16 of 18 counts project-snapshot-backup as taken by another route). All five validators
  pass at `77224fc8`. No merge dropped a test.
  **Left for the next round, in this order:** (13) export-profile-validation, then (14) source-hash-flac-proof
  (it carries export-flac-writer and export-quantizer from (13)'s stack, so it goes after it). Then the full
  checks: core, then all.
  **Nothing is built yet.** Slot 16 was still "waiting for a build place" with round 1's check on `c8a41862`
  (merges 1-5) from 16:02 to the end of round 2; `slot.sh run` refuses ("BUSY") until it ends. Next session:
  `bash /d/adibuild-tools/slot.sh wait 16` until it reports, read it, then
  `bash /d/adibuild-tools/slot.sh run 16 origin/held/engine-win-chain core`.
- How to do (13) export-profile-validation (31 commits; merge attempted in round 2 and aborted to stay inside
  the round, nothing of it is on the chain). Nine files collide. The decisions already made for the six small
  ones, to redo quickly:
  - `CMakeLists.txt`: both hunks both sides (BASE `aiff_file.cpp` + seat `quantizer.cpp`; BASE's four job/preset
    sources + seat `after_export.cpp`).
  - `wav_file.hpp`: both includes (`<array>`, `quantizer.hpp`); BASE's BWF v2 block **without** its
    `enum class WavDither` line (the seat moved WavDither to `quantizer.hpp`, first three values unchanged),
    plus the seat's `validWavSpeakerMask`.
  - `wav_file.cpp`: constructor both (BASE's `validateV2` loudness, seat's `quantizer_.emplace`); write path the
    seat's `quantizer_->quantize`. BASE's `quantizePcm` stays for `wavSampleAtDepth` (check that the
    quantizer's undithered rounding equals it: clamp to +-2, round, saturate).
  - `normalization.hpp/.cpp`: both enum groups; `finish(..., service, advanceFrames, speakerMask)` in that
    order (BASE's queue passes `advanceWork` sixth; the seat's call sites in queue.cpp must pass
    `{}`/`advanceWork` before the mask); `final.process` becomes `final.append` (the seat's measurement API);
    the seat's literal `pass + 1 < 4` reads `maxLoudnessEncodingPasses`.
  - `test_wav_file.cpp`: one main with BASE's `metadataV2`, `invalidMetadataV2` and the seat's `shapedDither`,
    `speakerMask`.
  - `queue.hpp`: all four hunks both sides (BASE `container`/`exactPcmFilename` + the seat's deliveries, rate
    policy, analysis-only, tail, loop, gapless; `validateJob` + `DeliveryResult`; `onHost`/`wavProgress` +
    `fingerprintDelivery`; the seat's `renderWavPrepared(..., const PlanRange& = {})` + BASE's
    `prepareNiOutput`).
  - `queue.cpp` (17 hunks) and `test_render_queue.cpp` (4) are the real work. The seat split validation into
    `validate(job, plannedRange)` + public `validateDeliveryProfile`; BASE (with the publication stack) made the
    old one the public `validateJob` used by `job_json.cpp`, `job_preset.cpp`, `project_jobs.cpp`. Keep
    `validateJob(const Job&)` as the public entry that calls the seat's `validate(j, false)`, and carry BASE's
    AIFF container and literal-filename rules into `validateDeliveryProfile`.
  - **Two AIFF writers meet here**: BASE's `audio/aiff_file.*` (`PcmContainer::Aiff`, `writeAiffFromWav`,
    `validateAiffSize`, from the save dialog) and the seat's `render/aiff_writer.*` (`ExportContainer::Aiff`
    in a delivery). Keep both to build, refuse `container != Wav` together with non-empty `deliveries` or a
    non-WAV `encoding.container`, and log it for the seat to fold its delivery onto BASE's writer.

- 2026-11-28, round 1, end: chain `held/engine-win-chain` at `7bc8ed71` holds 11 of the 17 taken stacks
  (table below). **Next, in this order:** (9) ffmpeg-pcm-stream, (11) publication-safety-integration, then
  archive-document-control and render-job-json-full-bytes, (12) clap-transport-gate, (13)
  export-profile-validation, (14) source-hash-flac-proof, (15) comp-take-splice, (16) scanner-clean-mode,
  (17) project-sync-folder. Then the full checks (core, then all). The first core check (slot 16, on
  `c8a41862`, the first five merges) was still waiting for a build place when this round ended: read it with
  `bash /d/adibuild-tools/slot.sh wait 16` before starting another, then check the newer head.
- How the merges were done, for the next session: `git merge --no-ff origin/engine-win/<top>`, then
  `git checkout --conflict=zdiff3 <files>` to see the base of each hunk. Most hunks are two additions at one
  place (keep both); the rest are BASE rewrites of the same code since 19 November, where the seat's change
  is re-applied onto BASE's new form. Each merge commit's message names its conflicts and how they went.
- Survey change: **project-snapshot-backup and project-copy-lock-neutral are already on main by another
  route**: SC-157 `940ac322` (basics, 27 Nov) built project backups (`writeProjectBackup`, `projectBackups`,
  `restoreProjectBackup`, automatic backups) and copies clear `session_lock`. The seat's older
  `writeSnapshotBackup` API would be a second path to the same thing, so it is left out (merge aborted).
- Watch: project-sync-folder (PD-08 status) detects OneDrive/iCloud roots; BASE now has `synchronizedProject`
  in `store.cpp` (ADR-0258 d7) for the journal policy. Read both before merging; if they duplicate the folder
  test, have the seat's status call use BASE's function.
- 2026-11-28, round 1: survey done (below). Chain started; see "Chain" for what is merged and what is next.

## Survey (2026-11-28, round 1)

How it was made: `git branch -r --no-merged origin/main` filtered to `origin/engine-win/*` (104 branches; the
seat's log branch `engine-win/log` is left out). For each: its head; which other engine-win branches contain it
(`merge-base --is-ancestor`); `git cherry` and a three-way `merge-tree` against BASE; and the share of its added
lines (from its merge-base with main) that are already present in BASE ("On BASE"). The seat's own log
(`adi_daw/collab/engine-win.md` on `engine-win/log`) and win's queue of 22 November
(`adi_daw/collab/prompts/2026-11-22-engine-win-queue.md`) gave the order, the holds and the withdrawals.

What the survey found:

- 103 work branches. 18 tops of stacks are **to take** (they hold 85 of the branches). 15 are **already on main**
  (or contained in a taken branch by an equal commit). 3 were **withdrawn by the seat**. Nothing is unclear.
- Almost everything to take was held by win's landing windows of 19 and 20 November (#686, #694: "held by the
  plan or the critique"; ADR-0248 d7 holds EX-08, EX-09, PD-11, PD-12 until after 3 December), or was built
  after 22 November on branches marked "lands after 3 December". It is finished work with its own native
  MSVC gates in the seat's log; none of it was refused on its merits.
- The large conflicts against BASE come from three places: the export queue (`render/queue.*`,
  `normalization.*`, `wav_file.*`) that the 19 November publication stack and the 22 November export stack
  both rewrote; the media cache, whose decode-cache base (#686's decode-space-admission) landed in a different
  form; and `ui_main.cpp`, which every lane has changed since.

| Branch | Head | Top of stack | On BASE | State |
|---|---|---|---|---|
| after-export-import | 86b3c519 | export-profile-validation | 0% | contained in export-profile-validation |
| analysis-files-switch | b54f4c6a | itself | 6% | to take |
| archive-document-control | dc3a8efc | itself | 2% | to take |
| archive-window-warning | d5a50adb | publication-safety-integration | 2% | contained in publication-safety-integration |
| catalogue-first | 6b90d48c | scanner-fatal-diagnostic | 100% | already on main: superseded by scanner-dfe31/scanner-service in #686 (PR #644 closed) |
| clap-transport-gate | dce7dffd | itself | 1% | to take (PR #663, open, held since 19 Nov) |
| collect-checkpoint-warning | 99b03094 | archive-document-control, publication-safety-integration | 2% | contained in both |
| collect-media-stream | 85fe86e7 | archive-document-control, publication-safety-integration | 2% | contained in both |
| collect-publication-warnings | 37e3ce5e | archive-document-control, publication-safety-integration | 2% | contained in both |
| collect-zip-stream | 734f7b6f | archive-document-control, publication-safety-integration | 2% | contained in both |
| collection-safety-integration | 9b330750 | publication-safety-integration | 2% | contained in publication-safety-integration |
| comp-take-splice | 2d911206 | itself | 3% | to take |
| decode-cache-consistency | 60fa903d | ffmpeg-pcm-stream | 85% | contained in ffmpeg-pcm-stream (its fix landed in #686 in another form) |
| decode-cache-folder | b89e9a5b | ffmpeg-pcm-stream | 55% | contained in ffmpeg-pcm-stream |
| decode-cache-missing-publication | df62deae | ffmpeg-pcm-stream | 50% | contained in ffmpeg-pcm-stream |
| decode-cache-runtime-limits | 4af5e656 | ffmpeg-pcm-stream | 65% | contained in ffmpeg-pcm-stream |
| dfe23-hardware-diagnostics | 5ebb976f | itself | 96% | already on main: carried by hosted-workers-current-main (merged); PR #665 closed |
| export-after-multiple | 4b3153e2 | export-profile-validation | 1% | contained in export-profile-validation |
| export-aiff-writer | 90a3b1e5 | export-profile-validation | 0% | contained in export-profile-validation |
| export-analyze-only | 435c4365 | export-profile-validation | 3% | contained in export-profile-validation |
| export-brickwall | 10c88f50 | export-profile-validation | 0% | contained in export-profile-validation |
| export-clock-codec-layouts | 07014a6d | export-profile-validation | 1% | contained in export-profile-validation |
| export-conditional-normalization | d1fcb88e | export-profile-validation | 2% | contained in export-profile-validation |
| export-cycle-and-matrix | f775a911 | export-profile-validation | 2% | contained in export-profile-validation |
| export-delivery-project-rate | 1d859703 | export-profile-validation | 1% | contained in export-profile-validation |
| export-delivery-workflow | 2daf620d | export-profile-validation | 1% | contained in export-profile-validation |
| export-direct-taps | a4112b4a | export-profile-validation | 2% | contained in export-profile-validation |
| export-file-converter | dae10235 | export-profile-validation | 8% | contained in export-profile-validation |
| export-flac-writer | d54c75a8 | export-profile-validation, source-hash-flac-proof | 0% | contained in both |
| export-format-name-tokens | fa9534a6 | publication-safety-integration | 68% | contained in publication-safety-integration |
| export-gapless-regions | 75440982 | export-profile-validation | 4% | contained in export-profile-validation |
| export-identity-replace | 41b74d57 | publication-safety-integration | 98% | contained in publication-safety-integration (its base #666 is on main) |
| export-loop-tails | d84677d2 | export-profile-validation | 5% | contained in export-profile-validation |
| export-lossless-readback | 5f53b58b | export-profile-validation | 1% | contained in export-profile-validation |
| export-meter-statistics | a1daa886 | export-profile-validation | 1% | contained in export-profile-validation |
| export-metered-brickwall | f6f82169 | export-profile-validation | 1% | contained in export-profile-validation |
| export-metered-delivery | 44c7c13f | export-profile-validation | 1% | contained in export-profile-validation |
| export-mono-fold | 71f1650c | export-profile-validation | 0% | contained in export-profile-validation |
| export-mp3-cbr | 46d8c712 | export-profile-validation | 1% | contained in export-profile-validation |
| export-multiple-deliveries | 032d74de | export-profile-validation | 1% | contained in export-profile-validation |
| export-normalization-targets | e8c029d4 | export-profile-validation | 0% | contained in export-profile-validation |
| export-ogg-opus | f1ebf38b | export-profile-validation | 1% | contained in export-profile-validation |
| export-ogg-vorbis | 310a57a5 | export-profile-validation | 1% | contained in export-profile-validation |
| export-pipeline-contract | c81794b2 | export-profile-validation | 2% | contained in export-profile-validation |
| export-preset-integration | 0b6ddcce | publication-safety-integration | 49% | contained in publication-safety-integration |
| export-profile-validation | 2b3b7e5c | itself | 2% | to take (top of the 22 Nov export stack, 31 commits) |
| export-project-rate | 44f34b96 | export-profile-validation | 1% | contained in export-profile-validation |
| export-quantizer | 7a9fe563 | export-profile-validation, source-hash-flac-proof | 0% | contained in both |
| export-render-matrix | 59a97c2b | export-profile-validation | 7% | contained in export-profile-validation |
| export-shared-file-delivery | 5daa4e3a | export-profile-validation | 1% | contained in export-profile-validation |
| export-slice-readback | 91b24281 | export-profile-validation | 2% | contained in export-profile-validation |
| export-stem-batch | e44cc77e | export-profile-validation | 8% | contained in export-profile-validation |
| export-wav-speaker-mask | 2b5a5da8 | export-profile-validation | 0% | contained in export-profile-validation |
| export-windowed-delivery | cd89d843 | export-profile-validation | 1% | contained in export-profile-validation |
| extract-cli-warning | 2e008d7c | archive-document-control, publication-safety-integration | 2% | contained in both |
| ffmpeg-pcm-stream | e0db001a | itself | 21% | to take (top of the media-cache stack) |
| ffprobe-bounded-audio | 16e4159e | ffmpeg-pcm-stream | 0% | contained in ffmpeg-pcm-stream |
| interrupted-audio-recording | 69033a9c | recording-launch-discovery | 95% | already on main by another route: rewritten as SC-250 `92f8757c` (basics); only its PD-06/PD-05 section of `docs/ENGINE-AUDIO-LOOP-CAPTURE.md` was not carried |
| media-cache-probe-integration | 415631ea | ffmpeg-pcm-stream | 33% | contained in ffmpeg-pcm-stream |
| media-cache-root | 0f4ea92e | ffmpeg-pcm-stream | 47% | contained in ffmpeg-pcm-stream |
| media-cache-root-current | 6ad07db6 | ffmpeg-pcm-stream | 47% | contained in ffmpeg-pcm-stream |
| midi-capture-clock | f26730fb | itself | 11% | abandoned by its seat: withdrawn (DF-E45 fixed on main by the engine) |
| mpe-held-notes | fc2d0a7b | itself | 2% | abandoned by its seat: withdrawn (DF-E51 fixed on main by the engine) |
| probe-portability | 882bf20a | clap-transport-gate | 1% | contained in clap-transport-gate |
| project-check-journal-targets | 55f20c8c | itself | 0% | abandoned by its seat: withdrawn (DF-E54 fixed on main by the engine) |
| project-cloud-backup | 25bf2855 | itself | 0% | to take |
| project-copy-lock-neutral | b73f36da | project-snapshot-backup | 0% | contained in project-snapshot-backup |
| project-quick-check | 288d6a9e | itself | 5% | to take |
| project-removable-sync | 8c51acaa | itself | 0% | to take |
| project-salvage | 267d716a | itself | 1% | to take |
| project-saved-render-jobs | 0756e2be | publication-safety-integration | 0% | contained in publication-safety-integration |
| project-snapshot-backup | 3b550a98 | itself | 3% | to take |
| project-sqlite-defensive | 7db7e05a | project-removable-sync | 0% | contained in project-removable-sync |
| project-sync-folder | 2a2d0f96 | itself | 0% | to take |
| project-wal-benchmark | a3f425f8 | project-removable-sync | 0% | contained in project-removable-sync |
| project-wal-observation | 64c59116 | itself | 0% | to take |
| publication-safety-integration | 20f27d66 | itself | 46% | to take (top of the 19 Nov EX-07/EX-09/PD-02/PD-04 stack) |
| recording-launch-discovery | 0b88a229 | itself | 94% | already on main by another route: SC-250 `92f8757c`; doc section not carried |
| render-job-json | 3e6b6352 | publication-safety-integration, render-job-json-full-bytes | 0% | contained in both |
| render-job-json-full-bytes | cd265a59 | itself | 0% | to take (sibling of the publication stack; its NUL guard is not in it) |
| render-preset-groups | 63d8cdff | publication-safety-integration | 0% | contained in publication-safety-integration |
| scanner-clean-mode | 9a1928bb | itself | 12% | to take |
| scanner-failure-cache | 5137c430 | scanner-fatal-diagnostic | 100% | already on main: superseded by scanner-dfe31 in #686 (PR #643 closed) |
| scanner-fatal-diagnostic | c1394a8f | itself | 94% | already on main: superseded by scanner-dfe31 in #686 (PR #669 closed); the missing lines are the older forms it replaced |
| scanner-helper-self-test | 7f8ed3f8 | itself | 100% | already on main: carried by scanner-integration into #686 |
| scanner-infra-failures | ceec2a51 | itself | 100% | already on main: superseded by scanner-dfe31 in #686 (PR #670 closed) |
| scanner-paused-reader-proof | b944e252 | itself | 0% | to take |
| scanner-success-cache | 578bca35 | scanner-fatal-diagnostic | 100% | already on main: superseded by scanner-dfe31 in #686 |
| scanner-time-counts | 2d77b0f3 | itself | 100% | already on main: superseded by scanner-dfe31 in #686 (PR #671 closed) |
| scanner-windows-shortcuts | cdb8f684 | itself | 0% | to take |
| settings-debounce | ead3d01b | settings-safety-integration | 4% | contained in settings-safety-integration |
| settings-json-full-bytes | c4d4f32c | settings-safety-integration | 2% | contained in settings-safety-integration |
| settings-read-failures | 18d278d2 | settings-safety-integration | 3% | contained in settings-safety-integration |
| settings-recovery-status | 4f04868c | settings-safety-integration | 2% | contained in settings-safety-integration |
| settings-safety-integration | 31322dab | itself | 2% | to take |
| settings-step-migrations | e9aa9fa2 | settings-safety-integration | 0% | contained in settings-safety-integration |
| source-hash-flac-proof | ab4fab82 | itself | 0% | to take |
| source-hash-memo | 03c440ab | itself | 0% | contained: the same change is `b0190e21` inside source-hash-flac-proof |
| sqlitecpp-wal-reset-fix | 8a8f9a00 | project-salvage | 100% | already on main: SQLiteCpp 3.4.0 landed as sqlitecpp-pin in #686 (its merge is a no-op) |
| streamed-process-output | 228adcb7 | ffmpeg-pcm-stream | 7% | contained in ffmpeg-pcm-stream |
| user-render-presets | ea37af92 | publication-safety-integration | 0% | contained in publication-safety-integration |
| vst3-class-id-packets | 7d4128dc | itself | 0% | to take |
| wav-bwf-v2 | a657d93b | itself | 1% | to take |

"On BASE" is the share of the branch's added lines (from its merge-base with origin/main) already present in
BASE's copy of the same file. A high share on a contained branch means its parent landed in another form.

## Chain: the order of merging

Oldest stack first, by the date the seat requested it. 19 November: (1) settings-safety-integration (PD-11),
(2) project-removable-sync (PD-12), (3) project-wal-observation (PD-05), (4) project-quick-check (PD-13),
(5) project-salvage (PD-13), (6) project-snapshot-backup (PD-07), (7) wav-bwf-v2 (EX-08), (8) analysis-files-switch
(TM-MD-04), (9) ffmpeg-pcm-stream (TM-MD-05, Cache Folder), (10) scanner-windows-shortcuts (DEV-25),
(11) publication-safety-integration, then archive-document-control and render-job-json-full-bytes (EX-07, EX-09,
PD-02, PD-04), (12) clap-transport-gate (#663). 22 November: (13) export-profile-validation (EX-02 to EX-12),
(14) source-hash-flac-proof (TM-MD-03), (15) comp-take-splice (CT-04), (16) scanner-clean-mode,
scanner-paused-reader-proof, vst3-class-id-packets (DEV-25, DF-E25 c), (17) project-cloud-backup and
project-sync-folder (PD-08).

| # | Stack merged | Origin head | Chain head after | Conflicts and how they were resolved |
|---|---|---|---|---|
| 1 | settings-safety-integration (PD-11) | 31322dab | e41a287d | `store.hpp` both type groups; `store.cpp` BASE's FILE reader kept, NUL refusal kept before the step migration; `import_choices.hpp` stop-asking (BASE) with the scheduled save (seat); `ui_main.cpp` preferences flushed before BASE's quit flow (a failed flush warns, then continues it), debounce tick before the no-document return, device grant uses `saveSoon` |
| 2 | project-removable-sync (PD-12; carries wal-benchmark, sqlite-defensive) | 8c51acaa | b1f08ddd | `store.cpp` BASE's sync/network rollback-journal policy (ADR-0258 d7) kept; the removable FULL applies on the local WAL path only; `SPEC.md` likewise; `test_project_files` both tests run |
| 3 | project-wal-observation (PD-05) | 64c59116 | 5b20a730 | `Store::open` keeps BASE's `openWithLease`/`openForHeadlessApply`; `OpenReport*` threaded through `openWithLease` |
| 4 | project-quick-check (PD-13) | 288d6a9e | 2533522f | `test_check.cpp` both new tests and both includes |
| 5 | project-salvage (PD-13; carries the SQLiteCpp pin already on main) | 267d716a | c8a41862 | `adi_tool` links `adi_control` and `adi_salvage`; `EXTERNAL-CODE.md` and usage text both rows; `fetch_external.sh` landed 3.4.0 pin plus sqlite-recover |
| - | project-snapshot-backup | 3b550a98 | (not merged) | already on main by another route (SC-157); see State |
| 7 | wav-bwf-v2 (EX-08) | a657d93b | 95cf10ca | `wav_file` BASE's `wavSampleAtDepth`/`quantizePcm` and the BWF v2 validators side by side |
| 8 | analysis-files-switch (TM-MD-04) | b54f4c6a | eb66274d | `buildMediaPeaks(…, PeakCacheResolution, AnalysisFileWrites)`: the switch goes after BASE's resolution (two test calls name `AnyValid`); the waveform worker reads it per request; Settings row beside BASE's in-place Sample Editor default; `ui_main` applies it at install and on Settings change |
| 10 | scanner-windows-shortcuts (DEV-25) | cdb8f684 | 05e46552 | `findCandidates` keeps BASE's denied-location diagnostics and walk; `.lnk` roots and entries followed inside it (Windows only) |
| 16a | scanner-paused-reader-proof (DEV-25) | b944e252 | d2a70fba | none |
| 16b | vst3-class-id-packets (DF-E25 c) | 7d4128dc | 7207a3e0 | one include line each side |
| 17a | project-cloud-backup (PD-08) | 25bf2855 | 7bc8ed71 | none |
| - | BASE: origin/main (land/basics-1128c landed in #838) | 629a4834 | 5579e020 | none |
| 9 | ffmpeg-pcm-stream (TM-MD-05, Cache Folder; carries the decode-cache, FFprobe and streamed-output stack) | e0db001a | 7f9328ab | `CMakeLists` BASE's decode-space tests kept; `decode.cpp` both includes, BASE's `admitSpace` (it holds the seat's over-maximum refusal); `ui_main` Cache Folder read before AppCommands, BASE's `refreshCacheLimits` the one place limits apply, now through `setDefaultDecodeCacheLimits` (no cache built at launch), the seat's duplicate `applyDecodeCacheLimits` dropped; consistency test BASE's form + the seat's `missingPublishedFile`; doc both |
| 11 | publication-safety-integration (EX-07, EX-09, PD-02, PD-04) | 20f27d66 | d6df55e2 | `publish_file.*` BASE throughout (the seat's side was the older form); `queue.*` BASE's AIFF/literal-name validation now in the public `validateJob`, `{format}` token = container (`wav`, `aiff`, `ni_stems_mp4`; new helper `formatToken`, since BASE added AIFF after the tokens), the seat's `prepareNiOutput`; `collect_export` + `collection_detail.hpp` BASE's `keepUndoHistory` threaded into the seat's hooked `detail::collectExport`; `project_files` BASE's resolve-first collision-safe naming with the seat's cancellation, copy progress, hooks and warnings carried into `publishCollectedMedia`; `ui_main` BASE's worker; tests BASE's canonical-parent checks + the seat's `deliveryNameTokens`; `job_json.cpp` refuses an AIFF or literal-filename job (version 2 has no field for them) |
| 11b | archive-document-control (EX-09) | dc3a8efc | cff34908 | `project_document` two entries: `runCollectAndExport(job)` -> StoreFileResult, `(job, control)` -> media::FileResult with `cancelled`, BASE's body; `ui_main` converts for its promise |
| 11c | render-job-json-full-bytes | cd265a59 | 190bb7e5 | none; no content (equal commit already in 11) |
| 12 | clap-transport-gate (#663, carries probe-portability) | dce7dffd | df3f8e2d | `ci.yml` BASE's VST3 session step, then the seat's all-platform research self-tests and the Windows transport gate; `MANIFEST.json` both groups, 92 entries checked against committed bytes |
| 15 | comp-take-splice (CT-04) | 2d911206 | 2968450d | `edit_plan.cpp` BASE's `prepareDelete(clips)` and the seat's `prepareCompTake` both; test both includes |
| 16c | scanner-clean-mode (DEV-25) | 9a1928bb | f0a41333 | loader: the seat's Clean cache mode with BASE's SC-319 block lifting, Clean lifts as Refresh does; `test_scan_cache` both tests |
| 17b | project-sync-folder (PD-08) | 2a2d0f96 | 77224fc8 | one include line each side; read against BASE's `synchronizedProject`: no shared code, but two detectors (see For the director) |
| 13 | export-profile-validation (EX-02..EX-12, 31 commits) | 2b3b7e5c | abad9cd1 | nine files. `wav_file` main's BWF v2 + the seat's quantizer/speaker mask (WavDither now in `quantizer.hpp`); `normalization` `finish(..., service, advanceFrames, speakerMask)`; `queue.hpp` every hunk both; `queue.cpp` the seat's `validate(job, plannedRange)` + `validateDeliveryProfile`, main's public `validateJob` calls it; main's AIFF/literal-name rules moved into `validateDeliveryProfile` (an AIFF or literal-name job is one WAV-encoded PCM delivery; gapless refuses it); `{format}` = the job's container, per delivery for the seat's multi-delivery; `renderWavPrepared` carries main's `onHost` session, `prime(10 s, stop)`, AIFF-from-staged-WAV and `wavProgress` onto the seat's rate/tail/loop/multi-delivery runner; the seat's new `serviceHost` calls go through the host owner; `Queue::run` main's start token + the seat's stem-batch reservation; `job_json` v2 refuses the seat's new options (and the shaped dithers it would have written as "rectangular"); tests: all of both, the seat's `aiffExports` renamed `aiffDeliveryExports`, and its `explicitDirectTaps`/`directStemBatch` (defined, never called on its branch) now run |
| 14 | source-hash-flac-proof (TM-MD-03) | ab4fab82 | a43678f6 | none |

All five validators pass at `7bc8ed71`. No merge dropped a test.

## Proof

- Core check started on slot 16 at `c8a41862` (merges 1 to 5); it had not got a build place when round 1
  ended. Nothing is proved yet: the merges were resolved by reading, not built.
- Round 2: the same check was still waiting for a build place at the end of round 2 (third-party sources
  fetched 16:02, then "waiting for a build place"); a new run is refused while it waits. Still nothing proved.
  The places most likely to break on the first build, to read first: `queue.cpp` `validateJob` /
  `formatToken` (merge 11), `project_files.cpp` `publishCollectedMedia`'s new parameters (11),
  `project_document` overloads (11b), `ui_main.cpp` `refreshCacheLimits` (9).
- Round 3: round 1's check on `c8a41862` reported at 17:12: **core configure failed**, sqlite-recover missing
  (see State). A core check on `abad9cd1` (merges 1-13) was started at once to see whether the build PC's
  third-party fetch picks the pin up on a second run.

## Account (so far)

What the chain adds, once proved (one line per thing a user would notice):

- Preferences are saved half a second after the last change, retried if the file is locked, and flushed on
  quit; a damaged or unreadable settings file is kept aside instead of being overwritten (PD-11).
- A project on a removable drive syncs each edit to disk; project files refuse writes to SQLite's own
  schema tables (PD-12).
- `adi_tool info` says when a project had a write-ahead log left beside it at open (PD-05).
- A project can be checked page by page without opening it for writing, and `adi_tool salvage` recovers
  the readable rows of a damaged closed project into a new file (PD-13).
- WAV export can write Broadcast Wave v2 metadata (originator, time reference, loudness fields) (EX-08,
  engine side; no control yet).
- File & Folder gets Live's "Create Analysis Files" switch (TM-MD-04).
- The plug-in search follows Windows shortcuts to plug-in folders and files (DEV-25).
- The scanner packet can carry exact VST3 class IDs (DF-E25 c, packet half; the producer is parked).
- A dated ZIP backup of the project into a chosen folder, engine side (PD-08; the File command is window-fn's).
- Settings > File & Folder > Cache Folder now works: decoded audio and waveform analysis go under the chosen
  folder from the next launch; the decode limits apply without building the cache at launch (TM-MD-05).
- Audio that only FFmpeg can read is bounded by FFprobe first and decoded as a stream (TM-MD-05).
- Export names can use `{format}`, `{rate}` and `{index}` (EX-07).
- Export presets apply by field group, user export presets are kept in ADI's data, and saved export jobs
  travel with the project (EX-07, EX-09, engine side).
- Collect and Export can be cancelled inside a large file, shows copy progress, and reports a pending
  checkpoint and media durability warnings in the status bar (PD-02, PD-04).
- Comp Take: a range of a take lane copied to the main lane as one undo step (CT-04, engine side).
- A Clean plug-in scan that re-probes everything except plug-ins the user blocked (DEV-25, engine side).
- ADI can tell which sync folder (OneDrive, iCloud Drive) holds a project (PD-08, engine side; no status yet).
- Not a user feature: the research probes are built and self-tested on every platform in CI (#663).

Left out, and why: see the survey's "already on main" and "abandoned" rows, and project-snapshot-backup
(SC-157 did it). Not yet merged (next round, not left out): export-profile-validation and
source-hash-flac-proof.

## For the director

- **Create Analysis Files is a new Settings row** (Live has it, File & Folder). It is built in today's look
  as a plain switch. Recommendation: accept it as Live's row; no picture needed beyond the existing page.
- **Two backup paths.** SC-157 (on main) made Live's automatic Backup folder; the seat's PD-08 branch adds an
  explicit dated ZIP backup to a chosen folder. Recommendation: keep the ZIP one only as the engine half of
  a later File command, after the director rules whether ADI offers it beside Live's Backup folder (Live has
  no such command: it is an addition).

- **Two AIFF exports.** Main has AIFF from the save dialog (`PcmContainer::Aiff`); the seat's held export
  stack has its own AIFF writer for its multi-format deliveries. Recommendation: one writer, main's; the
  seat folds its delivery onto it before the export stack lands. Nothing for him to choose unless he wants
  the seat's multi-delivery export at all before 3 December's plan rows are read.
- **Two sync-folder detectors.** Main's journal policy (ADR-0258 d7) and the seat's PD-08 status detect
  OneDrive/iCloud separately. Recommendation: the seat makes both use one root list before the status is
  shown; no ruling needed.
- **Clean scan and Live.** The seat's Clean scan is Live's "deep rescan" (Alt with Rescan). It has no
  control yet. Recommendation: wire it to Alt+Rescan, Live's gesture, when the Plug-Ins page is next touched.

Beyond these, the seat's own open questions (its log's "FOR THE DIRECTOR": MX-09, CT-01/CT-02, CT-04,
EX-03, EX-04, EX-05, PD-07, EX-12), which this chain does not answer.
