# engine-win — log

Windows engine seat. Work is cut to `engine-win/<topic>`. Newest entry first. Win takes the PR requests below.

---

## 2026-11-22 — TM-MD-03: native same-shape FLAC rewrite and eviction

**Lands after 3 December.** `engine-win/source-hash-flac-proof` `ab4fab82` on the pinned FLAC writer `d54c75a8`. Adds a native compressed fixture to the memo. The encoded FLAC was rewritten in the same file with the same byte size, channel count, sample rate and 4,096-frame duration. The real decode cache evicted the old hash's WAV. Both warm memo lookups still disagreed with the saved source digest and reused the new verified digest. Decoding the rewritten FLAC produced the authored replacement sample under a different cached WAV. Strict native MSVC: memo 66, decode 56 and PCM stream 50 checks. Raw `D:/adi-engine-win-build/20261120/engine-loans/source-hash-flac-minimal-stack.log`. The engine's plain, warped and progressive playback admission still needs the source-verification loan to mark a project clip Offline. PR requested.

## 2026-11-22 — EX-05: conditional normalization and the render contract

- **Lands after 3 December.** `engine-win/export-conditional-normalization` `d1fcb88e` on `export-cycle-and-matrix` `f775a911`. The measured source statistic gates the target gain. The caller explicitly chooses whether to preserve source level or enforce the true-peak ceiling when conditional loudness skips the target. Above/Below controls, dithered ceiling enforcement, separate skip/ceiling readings and Peak's unity case passed. The settings default for the skipped ceiling still waits on the director, with the recommendation above. Strict native MSVC: 638 render, 176 WAV and 41 normalization checks. Raw `D:/adi-engine-win-build/20261120/engine-loans/export-conditional-normalization.log`. PR requested.
- **Lands after 3 December.** `engine-win/export-pipeline-contract` `c81794b2` documents the verified combined signal path in `docs/RENDER-RUN-MANIFEST.md`. It corrects the old Replace and group-renumbering descriptions and records analyze and multiple delivery receipts. Reviewed against the code and the named Windows tests. The branch adds documentation only on the tested `d1fcb88e` parent. PR requested.

## 2026-11-22 — EX-02 / EX-12: common landing candidate for cycle WAVs and matrix exports

**Lands after 3 December.** `engine-win/export-cycle-and-matrix` `f775a911`, on `export-windowed-delivery` `cd89d843`. Brings the four direct taps, sparse render matrix, continuous cycle WAVs, previously checked codec stack, file conversion and After Export into one candidate. The continuous WAV at 32 kHz splits at the shared rounded sample fence into 85 and 86 frames and concatenates bit-exactly. It loads the test effect once, runs at the 48 kHz project clock, preserves the explicit speaker mask and returns independently matched BLAKE3 receipts. Native MSVC: render 638, WAV 176, normalization 35, NI metadata 85, material commands 29. Raw `D:/adi-engine-win-build/20261120/engine-loans/export-cycle-and-matrix-v3.log`. The prior red cycle assertion read before the WAV fmt chunk because of the reserved RF64 header space. The final test locates the fmt chunk and passed. The native AAC path and grouped or lossy normalization remain open. PR requested.

## 2026-11-22 — EX-03 / EX-04: tails on the common render and file clock

**Lands after 3 December.** `engine-win/export-windowed-delivery` `cd89d843` on `export-metered-brickwall` `f6f82169`. Compensated input and tail bounds stay on the graph clock. Shortening an Until Silent tail passes the exact delivered count to SoXR and to each independent codec readback. Fixed tail adds 64 samples at 32 kHz. Continuous 48 kHz reference agrees with the shortened tail, output WAV and AIFF agree sample for sample after shared 48 to 32 kHz SRC, and MP3, Vorbis and Opus verify their actual shortened lengths. Loop renders still write only the compensated second pass. Native MSVC: 597 render, 176 WAV, 35 normalization and 85 NI metadata checks. Raw `D:/adi-engine-win-build/20261120/engine-loans/export-windowed-delivery-rerun.log`. The initial compile caught an MSVC shadowing warning in the fixture, fixed before the green run. Live's nonzero first-lap start remains for the director. PR requested.

## 2026-11-22 — EX-05: verified brickwall on the shared export stack

**Lands after 3 December.** `engine-win/export-metered-brickwall` `f6f82169`, after `export-metered-delivery` `44c7c13f` and the devices limiter base in #706. The optional double LookaheadLimiter follows the selected normalization target and precedes the seeded quantizer. The report carries the requested target, captured and decoded measurements and whether the brickwall acted. Encoded crest stayed at or below its ceiling with exact duration, low-level unmodified and Peak mode fixtures retained byte identity. Native MSVC: 561 render, 176 WAV and 35 normalization checks, all passed. Raw `D:/adi-engine-win-build/20261120/engine-loans/export-metered-brickwall.log`. The supplied DSP programme API holds a whole planar capture in memory when active. Long multi-channel peak memory and cancellation inside a single offline limiter call remain open. PR requested.

## 2026-11-22 — EX-05: verified analysis on the combined delivery signal

**Lands after 3 December.** `engine-win/export-metered-delivery` `44c7c13f` joins the shared delivery workflow (`2daf620d`), measured target selection and Analyze Only. WAV normalization and standalone conversion measure the signal after SRC and channel folding. Analyze Only verifies source identity and measures the compensated selection without staging a file, reporting publication separately. A cancellation retains the completed frame count and no final analysis. Encoded RMS, selected peak/RMS, source evidence and cancellation pass. Native MSVC: 553 render, 35 normalization, 87 synthesized loudness and 176 WAV checks. Also ran the adjacent suites on `2daf620d`: recorder 157, recording commands 149, take-lane operations 74, freeze capture 31 and freeze commands 144, all passed. Raw logs `export-metered-delivery.log` and `export-delivery-workflow-adjacent.log` under `D:/adi-engine-win-build/20261120/engine-loans/`. The real >4 GiB WAV test was disabled and the source-symlink fixture could not run without Windows symlink privilege. Delivery UI and other OS builds remain in their owning lanes. PR requested.

## 2026-11-22 — EX-04/EX-12 verified export and conversion workflow

- EX-04/EX-12 combined delivery-workflow landing candidate — PR requested, `engine-win/export-delivery-workflow` at `2daf620d`, on the clock, codec and layout head `07014a6d` plus the selected After Export and batch-converter topics. File conversion now optionally downscales through SoXR, folds through the same channel policy, validates the final layout, decodes the staged file and can attach the same opt-in publication fingerprint. Real fixture: 4096 source frames became 3763 mono frames at 44.1 kHz, and the digest matched the published file. The grouped project jobs and one-Undo After Export still passed. MSVC: render 547, WAV 176, PCM stream 50, material 29, NI Stems metadata 85 (native AAC unavailable). Source rate/layout remain the converter defaults. Upsampling via the file converter and packet formats with no known source length are refused. Window owns controls and Win owns the landing.

## 2026-11-22 — EX-04 combined codec, rate, layout and readback candidate

- EX-04 combined landing candidate — PR requested, `engine-win/export-clock-codec-layouts` at `07014a6d`. Includes the tested six-format group, SoXR project-rate rendering, mono fold, explicit WAVE layout and lossless readback. The post-SRC mono WAV validates the speaker mask against its one output channel. WAV and Opus also passed a shared 96-to-48 kHz 2301-frame delivery with effects at 96 kHz. MSVC /W4 /WX: render 515, WAV 176, PCM stream 50, NI Stems 85 (native AAC unavailable). Win can review this stack as one combined export-format head. The standalone topic branches preserve their independent gates and remain available. Loop/tail selection and the export UI are still separate.

## 2026-11-22 — EX-12 shared batch audio converter

- EX-12 file conversion through the six native format writers — PR requested, `engine-win/export-shared-file-delivery` at `5daa4e3a`, joining `dae10235` with `f1ebf38b`. Inputs stream through the ordinary delivery encoders at their source rate and layout, without opening a project. The ordered batch returns one result per request, protects all captured input identities and prior successful destinations from Replace, and leaves unstarted rows Pending on cancellation. MSVC: render 505, WAV 171, PCM stream 50. Verified exact WAV/AIFF PCM, playable 4096-frame Opus delivery, normalized WAV, same-size source replacement refusal, protection of a second input and of earlier output, and cancellation with the correct row ID. Lossy encoding requires an exact declared source length in this candidate. Window owns batch UI and file selection. Converter SRC and cross-stack publication fingerprinting are separate integration work.

## 2026-11-22 — EX-04 decode of the staged lossless files

- EX-04 lossless delivery verification — PR requested, `engine-win/export-lossless-readback` at `5f53b58b` on the six-format `f1ebf38b` stack. Reads the staged WAV/AIFF/FLAC through ADI's existing owner-worker decoder and checks that the container, rate, channels and end-to-end decoded frame count agree with the requested delivery. A finalized WAV deliberately shortened by one byte was refused without publishing a destination. MSVC: render 491, WAV 171, PCM stream 50. This adds one decoded read per lossless delivery and checks its shape and readability. Exact quantized PCM equivalence is independently tested by the combined delivery fixtures. The >4 GiB opt-in case was skipped.

## 2026-11-22 — DF-E25(c) follow-up on the class-ID producer

- DF-E25(c) producer remains parked pending engine review. The pinned JUCE headless scanner already tries both moduleinfo locations before loading a library. Fast description construction reads the 128-bit CID and keeps two integer hashes. The fallback loads the module once, reads factory class info and does the same hash reduction. Recommendation: capture the CID at that existing description-building boundary, with one metadata snapshot for the fast case and the actual factory for the fallback. A second library load or a lookup by name would break the intended identity and cost contract. The packet/hydration branch remains `7d4128dc`; class-ID persistence and relocation wait for the schema and producer. Source: https://github.com/juce-framework/JUCE/blob/d60b7a78030661574a615b586d9325ba7a7fa7dd/modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h

## 2026-11-22 — EX-12 After Export for an individual delivery

- EX-12 selected-file import — PR requested, `engine-win/export-after-multiple` at `4b3153e2`, combining the guarded After Export command with the five-format delivery group. When requested, all staged digests are taken before publication and exposed only per successful output. The owner can import the selected file through the existing one-step command, with a fresh digest check and the same undo behavior. Real WAV+AIFF test chose the AIFF, committed one clip to the target track, and one Undo restored the prior head and left both deliveries byte-exact. Strict MSVC: render 498, history 148, material 29. Window owns file selection and dispatch. Opus import still awaits a decoder.

## 2026-11-22 — EX-04 joined project clock and mono delivery

- EX-04 SoXR and mono integration — PR requested, `engine-win/export-delivery-project-rate` at `1d859703`, on the tested five-format group `032d74de` plus the rate and mono topics, after 3 December. One compensated project-rate stream resamples once, folds if selected, then feeds each encoder at the same delivery clock and channel layout. Native tests verified WAV and AIFF decoded identically at 44.1 kHz from a 48 kHz effect graph, both output 2301 frames, and the mono group preserved the equal-channel reference after SRC. MSVC 504 render, 171 WAV, and 85 NI Stems metadata checks (native AAC unavailable). The mono law is an explicit ADI choice pending Live measurement. Legacy direct rate stays the default. This stack still needs the separate Opus/readback and loop/tail joins for a single landing head.

## 2026-11-22 — EX-04 explicit WAVE speaker layout

- EX-04 WAVEFORMATEXTENSIBLE channel mask — PR requested, `engine-win/export-wav-speaker-mask` at `2b5a5da8` on the shaped-quantizer base `cc3f8060`. An optional speaker mask tags the caller's existing interleaved stream. Zero retains the unnamed layout. A nonzero mask requires one defined position per channel; stereo gets the same extensible header when explicitly tagged. Normalization preserves the mask across its encoding passes. MSVC: WAV 176, render 434, normalization 24, including 5.1 and stereo header bytes, unchanged samples, invalid positions refused before truncation, and masked normalized output. The >4 GiB opt-in case was skipped. Window owns layout selection; the join must apply it to the final delivery channels. Source: https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible

## 2026-11-22 — DEV-25 sustained scanner-reader pause proof

- DEV-25 500 ms reader-pause case — PR requested, `engine-win/scanner-paused-reader-proof` at `b944e252` from `87ea349c`. Native Windows test-only option `adi_scan_process_tests --paused-reader-stress`. The owner suspends its disposable runner thread while an owned child reaches a real blocked stdout write, holds 500 ms, resumes, and verifies the exact bounded 2 MiB payload and success status. MSVC /W4 /WX: 28 ordinary scanner process checks and 200/200 exact paused trials, zero failures. No scanner production changes or live plug-in load. The regular suite keeps its previous runtime. Source: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-suspendthread

## 2026-11-22 — DEV-29(d) additional normalization targets

- DEV-29(d) / EX-05 — PR requested, `engine-win/export-normalization-targets` at `e8c029d4`, on the shared export meter `a1daa886`. Adds explicit RMS-I in dBFS and maximum momentary/short-term LUFS choices to the existing uniform gain and decoded true-peak verification. Integrated remains the existing choice. The target statistic and units appear in the run manifest. MSVC render 430 and normalization 34, including an independently recomputed RMS from the decoded PCM, max M/S kept distinct from integrated loudness, correct ceiling, and refusal when the short-term window cannot fill. This uses the current target interval of -36 to 0 and an RMS default of -14 dBFS when explicitly selected. Window-fn owns the controls; the brickwall, multi-format normalization and together groups have separate integration gates.

## 2026-11-22 — EX-04 Ogg Opus output

- EX-04 Ogg Opus — PR requested, `engine-win/export-ogg-opus` at `f1ebf38b`, stacked on the combined delivery head `032d74de`, after 3 December. Uses pinned BSD-3-Clause libopus 1.6.1 `22244de5a79bd1d6d623c32e72bf1954b56235be` and libopusenc 0.3 `0dba1bea736ab8bb811409dce80c994a00a2ced9`. Family-0 mono/stereo, 48 kHz, VBR with an explicit target bitrate. Native Ogg reread and libopus decode verify packet continuity, pre-skip and final granule trim before delivery. Verified 4201 stereo and 719 mono frames, deterministic output across graph blocks and no PCM dither, plus a six-format group with the same one device load and 4224 processed frames as the WAV control. MSVC: render 490, WAV 171, NI Stems 85 (native AAC unavailable). The tested decoder is the delivery verifier; arbitrary Opus import, non-48 kHz conversion and decoded lossy normalization remain separate. Win owns workflow packaging. Sources: https://www.opus-codec.org/downloads/ and https://www.rfc-editor.org/rfc/rfc7845.html

## 2026-11-22 — EX-04 one graph run for multiple deliveries

- EX-04 checked delivery group — PR requested, `engine-win/export-multiple-deliveries` at `032d74de`, including shaped quantizer and the AIFF, FLAC, LAME and Vorbis branches, after 3 December. One selected compensated graph stream feeds per-file encoders at the shared output clock. Jointly built with the native dependencies. A WAV/AIFF/FLAC/MP3/Vorbis job loaded its device once and processed 4224 device frames, the same as the single-WAV control (one load, 4224 frames). The authored output was 4201 frames. WAV PCM24, AIFF and FLAC matched bit for bit, and lossy writers checked the decoded length. An occupied WAV increments the whole group. Late collision kept the earlier commit and the competitor's bytes, with per-file states. MSVC: render 484, WAV 171, NI Stems 85 (native AAC unavailable). The codec join disables FLAC's Ogg linkage before declaring the Ogg target. Window controls, common gain, project-rate/mono/loop joining and full cross-platform packaging remain.
- PD-09 parked — the registry's legacy integer v3 mapping and newer-major read-only contract need Win's schema ruling and scanner ownership review.
- EX-04 Float64 WAV parked — the verified double reader and wider decode callers need the engine audio-path loan. EX-12 live capture and the graph telemetry taps also need the engine/window contract.
- **FOR THE DIRECTOR — PD-07:** PLAN marks automatic Backup-folder copies as a new choice, while R-26 still says no backups. Recommend opt-in on committed snapshot only, with recycle-bin rotation, after HI-02 and a measured Live backup-folder read.
- **FOR THE DIRECTOR — EX-12:** the per-scene File 2 schema is absent from `EXPORT-VISUAL-STORY.md`. Recommend approving that exact schema before any serialized telemetry format, so FilmCraft and ADI share one timebase and field vocabulary.
- **FOR THE DIRECTOR — EX-05:** specify whether a conditional normalization job that skips gain still enforces its true-peak ceiling. Recommend keeping the explicit ceiling active and reporting independently when it alone changed the audio.

## 2026-11-22 — EX-05 export metering and Analyze Only

- EX-05 statistics — PR requested, `engine-win/export-meter-statistics` at `a1daa886` on `87ea349c`. Normalization uses ADI's existing DEV-28 meter for I, true peak, maximum M and S, and LRA. The report and manifest distinguish captured PCM from decoded delivery. RMS is the unweighted arithmetic mean-square across all scalar samples; silence and incomplete windows yield unavailable values. The prior peak gain, ceiling, dither and tiny-signal regressions passed.
- EX-05 Analyze Only — PR requested, `engine-win/export-analyze-only` at `435c4365`, stacked on `a1daa886`. Runs the selected compensated source once. Reports frames analyzed and verified source identity, with no destination, encoding, gain, file staging or publication. Checked exact RMS/peak against the authored sample range, cancellation without a completed reading, and refusal of contradictory normalization. Combined MSVC: render queue 435, normalization 27, synthesised EBU loudness cases 87. Existing source-symlink test requires Windows privilege. Cross-stem/album grouping and conditional gain remain separate; window owns the controls and labels. Sources: https://tech.ebu.ch/publications/tech3341 and https://tech.ebu.ch/publications/tech3342

## 2026-11-22 — EX-04 Ogg Vorbis output

- EX-04 Ogg Vorbis — PR requested, `engine-win/export-ogg-vorbis` at `310a57a5` from `87ea349c`, after 3 December. Pinned BSD-3-Clause Xiph Vorbis v1.3.7 `0657aee69dec8508a0011f47f3b69d7538e9d262` and Ogg v1.3.6 `be05b13e98b048f0b5a0f5fa8ce514d56db5f822`. Uses fixed 1024-frame encoder feeds from the undithered graph stream and verifies the end granule and full decoded length before publication. Stereo 48 kHz 4201 frames and mono 44.1 kHz 719 frames passed. Output bytes stayed identical across graph block sizes and unused PCM dither values, and changed with the requested VBR quality. MSVC: render 437, PCM stream 50, NI Stems 85 (native AAC unavailable). Normalization of lossy output and an Opus encoder remain. The installed decoder verification is Vorbis only. Win owns CI fetch and the combined container landing. Source: https://xiph.org/vorbis/doc/libvorbis/overview.html

## 2026-11-22 — PD-08 explicit project backup

- PD-08 dated backup worker — PR requested, `engine-win/project-cloud-backup` at `25bf2855` from `87ea349c`. Reuses Collect and Export's consistent snapshot, staged media and progress/cancel. Requires a caller-selected existing folder, names the ZIP from the project file stem and supplied local time, and refuses a same-second collision. MSVC 8 focused checks, including a Unicode name, leap-day timestamp, actual ZIP, and cancellation during archive writing. Window-fn owns the File command, per-run chooser, local clock, progress routing and final status. No sync service was connected.

## 2026-11-22 — PD-08 project sync-folder status

- PD-08 sync-folder detection — PR requested, `engine-win/project-sync-folder` at `2a2d0f96` from `87ea349c`. Checks the native OneDrive known folder and configured account roots on Windows, or the local CloudDocs directory on macOS. The project lookup uses filesystem identity of its resolved ancestors and chooses the closest root. MSVC settings 338, including direct and nested matches, a sibling prefix, traversal and absent paths. Real OneDrive and macOS iCloud placements were unmeasured. Window-fn owns the status-bar call at project open and Save As. Microsoft Known Folder IDs: https://learn.microsoft.com/windows/win32/shell/knownfolderid

## 2026-11-22 — EX-12 audio file conversion

- EX-12 file conversion — PR requested, `engine-win/export-file-converter` at `dae10235` on `87ea349c`. Streams existing decodable audio into the shared WAV delivery with the source clock and channel count. Normalization and the quantizer use that delivery path. Verified exact float, seeded PCM, no-clobber publication, cancellation, same-source refusal and input replacement. MSVC: render queue 438, WAV 143, PCM stream 50, normalization 23. The existing symlink case requires Windows privilege. The converter entry point takes one file. Window owns the batch UI; additional formats, conversion rate and After Export fingerprinting need the joined delivery path.

## 2026-11-22 — EX-12 After Export import

- EX-12 After Export to a track — PR requested, `engine-win/after-export-import` at `86b3c519` on `87ea349c`. Opt-in worker digest captured before publication. Owner-thread import verifies the delivery and the journalled source digest using the existing material command, then commits one undo step. Undo kept the published file and removed the new clip. Cancel and a same-length replacement were refused. MSVC: render queue 440, material 29, history 148, all green. The existing symlink case was skipped for Windows privilege. Window owns completion dispatch and destination selection. Gapless and converter delivery need the joined publication path before offering After Export there.

## 2026-11-22, EX-12 sparse render matrix

**PR requested** — `engine-win/export-render-matrix` `59a97c2b`, stacked on the direct-stem batch `e44cc77e`, after 3 December. Gives the window a literal list of source/tap and cycle-marker cells, predicts resolved names and bounds under one read revision, and enqueues them atomically as ordinary jobs. Existing single-region stem batching uses the same path. Actual cells resolve project state at their own job start. Strict MSVC passed 476 render and 85 NI Stems checks (native AAC unavailable). Three sparse cells verified exact audible ranges and job-start naming. Duplicate cells and point markers refuse the batch. The Window's matrix grid and automatic source selection are separate.

## 2026-11-22, DF-E35(a) take-lane snapshot projection

**PR requested** — `engine-win/take-lane-snapshot` `430ea825` on main `87ea349c`. #677 is on main and collab/README.md activates the snapshot projection loan once it lands. ClipNode retains the optional lane id. The existing TrackNode::clips main reader omits takes, with takeClips available to future take rows. No Window source was changed. Strict MSVC passed 156 snapshot, 74 take-lane op, 575 audio playback and 553 Window seam checks. The actual Arrangement hit returns the main clip with an overlapping take, a take-to-take clip.setLane invalidates the old node, and Undo restores previous hit precedence.

**Needs engine/window-fn:** engine review and land before DF-E17. Window-fn can bind take rows to takeClips and pass the explicit CT-01 EditScope. The audition display stays separate.

## 2026-11-22, EX-12 continuous gapless region export

**PR requested** — `engine-win/export-gapless-regions` `75440982`, stacked on EX-03 `d84677d2`, after 3 December. Adjacent cycle markers are resolved from the one captured project revision and sorted chronologically. The private WAV captures one continuous device and dither pass. Slices preserve exact frames and PCM codes. The files preflight before delivery, and publication rechecks the same media after staging. Windows /W4 /WX passed 475 render, 143 WAV and 85 NI Stems checks (native AAC unavailable). The dithered PCM24 null included a delayed effect ringing across the cut, and the test excludes a one-sample gap, duplicate output names and independent tails.

**Needs Win:** integrate shared queue hunks in the export landing window. Pairwise merge previews found overlaps in the FLAC/AIFF/MP3 container, suffix and CMake declarations, and in the SoXR/EX-03 range clocks. In the joined tree, apply .wav/.aiff/.flac/.mp3 selection consistently to preview, keep timing/latency in the graph clock and encoded counts in the delivery clock, and check the combined gapless/SRC/tail path. The published topic heads were tested separately.

## 2026-11-22, EX-04 project-rate capture and SoXR downsampling

**PR requested** — `engine-win/export-project-rate` `44f34b96` on main `87ea349c`, after 3 December. Pins SoXR 0.1.3 to `945b592b70470e29f917f4de89b4281fbbd540c0` (LGPL-2.1-or-later). The explicit ProjectThenDownsample policy keeps a lower-rate export's graph and devices at the saved project rate. It streams the compensated selection through SoXR VHQ double before encoding and normalization. Higher-rate jobs still process at delivery rate. The requested bounds, output frame count and both rates remain observable. MSVC /W4 /WX passed 440 render, 143 WAV and 85 NI Stems checks (native AAC unavailable). The 48-to-44.1 kHz test verified correct FX preparation rate, compensation, exact 2301 output frames and identical samples across two block partitions.

**Needs Win/window-fn:** the dependency fetch and default policy wiring after the independent EX-04 patches land. This uses nearest project sample for the selection origin, rounds the source span up and trims any extra delivery frame. SoXR currently sees the finite selected render, so filter behavior around the edges still needs the M6-A6G-010 Live comparison. This patch does not enable cross-format multiple deliveries.

## 2026-11-22, EX-04 MPEG-1 CBR MP3

**PR requested** — `engine-win/export-mp3-cbr` `46d8c712` on main `87ea349c`, after 3 December. LAME 3.100 is archive-pinned to SHA-256 `ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e` (LGPL-2.0-or-later). LAME gets undithered interleaved doubles and emits MPEG-1 Layer III 320 CBR with an Info tag. Publication waits for all frames to pass header checks and a complete dr_mp3 read of the original length. The Win /W4 /WX build passed 436 render, 143 WAV and 85 NI Stems checks (native AAC unavailable). MP3 exercised 48 kHz stereo and 44.1 kHz mono, exact trimmed duration, and identical bytes after changing project block partition and PCM dither settings. An independent ffmpeg probe also decoded a 4201-frame direct LAME fixture to 4201 frames.

**Needs Win:** fetch this pin in the packaging/CI workflow and combine the EX-04 container enums/selection hunks. The candidate permits MPEG-1 rates with Normalize Off. Resampling to supported MP3 rates and normalization against decoded lossy output remain follow-up delivery work. Live stereo mode and tag parity remain unmeasured.

## 2026-11-22, EX-04 mono delivery

**PR requested** — `engine-win/export-mono-fold` `71f1650c` on main `87ea349c`, after 3 December. Adds an explicit arithmetic stereo average after the source tap and graph latency. Normalization and quantization see the one-channel delivery. Existing ordinary stereo and multichannel jobs retain their channel count. Strict MSVC passed 435 render, 143 WAV and 85 NI Stems checks (native AAC unavailable). Tests verified correlated, anti-phase and one-sided input against a 29-sample delayed stereo effect, and verified the final PCM duration and the folded normalization peak.

**FOR THE DIRECTOR — EX-04:** Live 12's Rendering Options manual confirms mono delivery and gives no coefficients. Recommendation: verify by exporting in-phase, opposite-polarity and one-sided signals with peak normalization disabled before mapping Live's checkbox to the explicit average. The branch supplies that law as a named choice and makes no Live parity claim. Source: https://www.ableton.com/en/live-manual/12/managing-files-and-sets/#rendering-options

## 2026-11-22, EX-02 direct-stem batch expansion

**PR requested** — `engine-win/export-stem-batch` `e44cc77e`, stacked on `a4112b4a` for after 3 December. One current project read previews cardinality, names and source taps. The whole literal list enqueues atomically after validation. Render still samples each job's revision at start. Preview includes existing and batch-reserved names. A later rename cannot cause a batch replacement to overwrite an earlier successful delivery. Strict MSVC passed 463 render and 85 NI Stems checks (native AAC unavailable), including equivalent Main aliases, no partial enqueue, numbering, and last-moment replacement protection.

**Parked EX-02 remainder:** automatic All Individual Tracks selection, ViaMain and MainFxOnly depend on the engine/window contract and the Live reading. The batch accepts the caller's literal track and tap list. Preview is advisory against later external project or destination changes.

## 2026-11-22, EX-02 direct export signal taps

**PR requested** — `engine-win/export-direct-taps` `a4112b4a` on main `87ea349c`, after 3 December. Direct jobs can pick PreFx, PostFx, PostFader or PostMixer through Session's existing offline graph tap. Legacy PreFader still resolves to wet PostFx, and the run manifest records the actual signal point. Main and NI Stems retain their route checks. MSVC /W4 /WX passed 452 render and 85 NI Stems checks (native AAC unavailable), including dry/wet latency compensation and the current graph's solo behavior.

**Needs engine:** actual PostFader and PostMixer both followed another track's solo in the ordinary audio-strip fixture. A solo-independent PostFader tap, ViaMain's audibility override, and MainFxOnly need an engine decision or hunk. The queue uses the existing tap rather than substituting send or solo semantics.

## 2026-11-22, after the standing queue

- **Parked DEV-26 native acceptance:** the installed ADI-Q binary is 0.2.2. ADI-L and ADI-Clip were absent from the installed VST3 directory. The exact three workstation builds are needed for the PLAN acceptance; the scanner and normal plug-in path already have code.
- **Parked M3 release gates:** TM-MD-01/04/05 wait on the media ADR/ordered held work and engine audio-path ownership. PD-01/03 snapshot callers are engine/window; PD-04's dead-stage recovery needs the PD-05 session lease and Win's held publication integration.
- **Taking EX-02 next:** export's direct taps can use the engine's existing offline tap resolver. ViaMain audibility remains an engine dependency.

## 2026-11-22, CT-04 engine take-comp splice

**PR requested** — `engine-win/comp-take-splice` `2d911206` on main `87ea349c` and CT-01 `4c2b07f3`, for the after-3-December window. A literal audio or MIDI take range within one known take uses the existing immutable clip image, clock trimming and placement planner. It replaces the matching range on the same track's main lane with one guarded arranger.splice. Take name and media/clip-local data are retained, with no persisted provenance field and no track-automation edit. The four native Windows /W4 /WX suites passed 1,631 checks, including two actual audio takes, exact PCM and identical Undo/Redo. The engine reviews the edit-plan hunk. Window-fn can bind the gesture and derive highlights from the current source and main copies.

**FOR THE DIRECTOR — CT-04:** empty lanes, painting through gaps and dragging beyond the source boundary still await M6-A6H-030. Recommendation: keep the plain covered-range splice, and clamp a drag to the source boundary pending the Live reading. The current producer refuses uncovered ranges. Audition, the edge gesture, and generated seam fades remain separate.

## 2026-11-22, EX-03 Render as Loop and export tails

**PR requested** — `engine-win/export-loop-tails` `d84677d2` on main `87ea349c`. Uses the same graph for the unwritten loop lap and latency-compensated written lap. Fixed and Until Silent tails stop new timeline input at the authored range end and keep processing the graph. Until Silent counts consecutive quiet frames across the output channels, including the hold, up to the specified maximum. Independent tails/loops are refused for gapless and NI Stems jobs. Native Windows /W4 /WX passed 460 render checks and 85 NI Stems checks (native AAC unavailable). A 17-sample delayed feedback effect verified continuous equivalence, immunity to a later arrangement impulse, exact hold and maximum, and a sample-for-sample second lap with wrapped decay.

**FOR THE DIRECTOR — EX-03:** Live's non-zero-range first-pass start remains unmeasured. Recommendation: keep ADI's existing zero-based preroll into the first lap by default unless a Live null test finds it starts cold at the range. This branch uses the zero-based preroll so the engine path can be reviewed now. The defined tail case puts the private transport into Stop, so hosted plug-ins that reset on Stop still need a targeted probe. Continuous gapless rendering stays EX-12.

## 2026-11-22, TM-MD-03 source hash memo

**PR requested** — `engine-win/source-hash-memo` `03c440ab` on `87ea349c`. The standalone off-callback memo admits only measured digests, keys by native identity with size and mtime, and detects changes while hashing. The Windows /W4 /WX standalone test passed 59 checks, including 50 warm metadata hits, changed same-size content with a newer mtime, mismatch persistence after a private WAV deletion, and same-size/same-mtime native replacement. The existing progressive-decoder suite passed 100 checks. Native AAC was unavailable. The standalone tests use byte fixtures, not compressed-source Session playback.

**Parked:** the audio-path loan above still gates connecting the memo to the producer, plain and warped playback, and the native same-shape re-encode/eviction acceptance. Recommendation: one memo shared by the decode cache and source resolver, retained independently of WAV eviction. Retain a fresh final digest for each actual decode. This in-memory patch requires one verification of the source per process lifecycle and makes no promise for a same-inode rewrite with a deliberately restored mtime.

## 2026-11-22, EX-04 FLAC writer

**PR requested** — `engine-win/export-flac-writer` `d54c75a8`, on main `87ea349c` and EX-06 `7a9fe563`. Pins libFLAC 1.5.0 to `1507800de4b70e21be71f38caa0d9079d0bc6e45` (BSD-3-Clause). The local strict x64 MSVC build passed 444 render checks and 171 WAV/quantizer checks. The stock libFLAC decoder verified samples and MD5 for 4201-frame PCM16 and PCM24 against matching dithered WAV renders. The last partial block and suffix-preserving collision handling passed. Float32 is refused. Private WAV repacking adds one streaming write and read and applies no second quantization.

**Needs Win:** the workflow fetch/CI hunk for the pinned source is yours. The app build discovers it under third_party/flac or via ADI_FLAC_SOURCE. The AIFF sibling is also based on the quantizer. Both share export-container branches of validation, naming and publication, so integrate those hunks together in the landing window. Window-fn can wire the two containers once landed.

## 2026-11-22, EX-04 AIFF writer and TM-MD-03 ownership

**PR requested** — `engine-win/export-aiff-writer` `90a3b1e5`, on main `87ea349c` and EX-06 `7a9fe563`. PCM16/24 publish as big-endian AIFF and Float32 as AIFF-C fl32, with seeded PCM dither applied once in the private WAV stage. The Windows /W4 /WX render suite passed 449 checks including full sample equality at all three encodings, odd-byte mono padding, and exact-WAV/unsupported format refusal. The WAV/quantizer suite passed 171 checks. The private WAV adds one write and read before publication. The format remains unwired in the Export window.

**Parked for Win and engine — TM-MD-03:** please lend `src/adi/audio/progressive_decode.cpp`, `src/adi/audio/decode.cpp` and the clip playback saved-source resolver hunk. Win's 22 November entry reserves these for the engine. Continuing with the standalone memo and tests only.

## 2026-11-22, after 3 December, EX-05 export brickwall

- `engine-win/export-brickwall` at `10c88f50` is stacked on EX-06 `7a9fe563` and the limiter landed in #706. Brickwall is off by default and acts only in Loudness mode. The existing offline limiter trims its own latency before PCM quantization. Refine uses the encoded loudness and true-peak readings, at most four trials, with Brickwall adjustment steps clamped to ±3 dB.
- MSVC /WX: render queue 439/439, DSP 433/433 and WAV/quantizer 171/171. The 48 kHz fixture matched complete seeded PCM24 files byte for byte below the ceiling, verified the over-ceiling case at 4x and kept frame count unchanged. Peak mode produced identical files with both setting values.
- When the stage engages it uses one planar double buffer for the programme to meet the limiter's whole-programme API. The director's finer true-peak detection question remains recorded above.

PR requested.

## 2026-11-22, after 3 December, DEV-25 Clean discovery

- `engine-win/scanner-clean-mode` at `9a1928bb` adds Clean to the asynchronous catalogue job. It skips cache hits and permits a fresh probe of an unchanged typed automatic quarantine. Ticket admission keeps manual and pending blocks. Ordinary Incremental, Refresh and CacheOnly retain their policies.
- MSVC /WX scanner cache 142/142, scanner protocol 58/58, catalogue 41/41, and the JUCE player/render suite passed. The native K=1/K=4 fixture produced the same ordered catalogue. Each eligible native module was probed once in its child, the manual block launched none, and warm Incremental and CacheOnly reused the rebuilt metadata.
- **For window-fn:** wire Alt+Rescan and Alt at launch to `startDiscovery(DiscoveryMode::Clean)`. The synchronous catalogue call explicitly refuses Clean.

PR requested.

## 2026-11-22, after 3 December, DF-E25(c) scanner class-ID packet

- `engine-win/vst3-class-id-packets` at `7d4128dc` adds a strict, versioned packet for exact VST3 class IDs alongside the existing JUCE UIDs. The reader still accepts legacy packets. A new packet validates every 32-digit ID, rejects duplicates or a partial set within a module, and hydrates from existing registry storage without a helper or database migration.
- MSVC /WX scanner protocol 63/63, catalogue 41/41, prune 27/27 and cache 138/138. The quiet cache rerun passed after two earlier runs hit different existing child-timing assertions. The class-ID and catalogue controls passed on the corrected codec. Native relocated-project playback awaits the provider and saved-reference seam.
- **Parked, native producer and project identity.** Engine: expose the processor class ID before JUCE's 32-bit hash, in a normalized cross-platform form, from the existing isolated VST3 discovery so a module is opened once. Win: please reserve schema 1.24 if free. Recommend an optional exact VST3 class ID in the project ref while preserving and accepting its legacy JUCE UID. This branch changes scanner packets only.
- **Parked, DF-E35(a).** #677 is on main. Wait for the engine's snapshot projection loan handoff and coordinate the main-row readers with Window, as the standing queue directs.

PR requested for the scanner packet part.

## 2026-11-22, after 3 December, PD-06 and PD-05 recorder restack

- PD-06 `engine-win/interrupted-audio-recording` at `69033a9c` now follows DF-E44 and N66 `0900deeb`. PD-05 `engine-win/recording-launch-discovery` at `0b88a229` follows that exact PD-06 head. The joined recorder preserves both close-all/cancel cleanup and finalized-prefix admission, with the recovery identity and placement receipt checks intact.
- Both exact heads passed MSVC /WX recorder 172/172 and UI document 319/319. Recording commands passed 185/185 on PD-06 and 190/190 on PD-05, including the startup process-exit, live lock and empty-session controls.
- **Parked, Save As rebinding.** Run `adi_recording_commands_tests --save-as-recovery-contract`. The Windows red fixture switches and refreshes the same Store successfully, then observes that stale options still start and successor options are refused. Engine or Win to supply or lend the accepted-successor `Session::refresh`/`startRecording` hunk. The test exercises the positive receipt and Undo path once that binding is fixed. Window-fn keeps the Save As guard during active or pending recording and the reopen question.

PR requested for restack and regression review.

## 2026-11-22, CT-03 MIDI loop materializer

- `engine-win/midi-loop-takes` at `a889fa3e`, stacked on the engine's same-transaction note capture `254d7d1c`. Each pass creates a take lane and clip, then prepares its native note diff in the same transaction. The take closes a held note at the pass boundary. The main copy keeps the cumulative note image supplied by the recorder, including the carried hold and recorded IDs. Each pass has its own Undo and the explicit Record Quantize diff is a separate commit.
- MSVC /WX passed recording commands 164/164, note diff 1,062/1,062, MIDI Capture 59/59 and History. The 3.5-pass test covers four take clips, one main clip, the partial last pass, note length and release at the wrap, one Undo per pass, separate quantize Undo, and literal Redo.
- **Parked, input and policy wiring.** The materializer takes completed pass notes and a main note image. The live input caller and the existing-clip Arrangement Overdub cases still need the engine and Window handoff. The proof is from the native new-loop and journal fixtures.

PR requested.

## 2026-11-22, CT-01 literal take-lane edits

- `engine-win/take-lane-edit-scope` at `4c2b07f3` validates literal take-lane ownership and reads only the scoped track/lane pair. Move, Resize End, Trim and Delete preserve the take lane, the main lane's clips and track automation. Delete targets clip IDs, so an overlapping sibling on the same take lane survives. The same-track and cross-track refusals retain DF-E17's separate detach or lane-change contract.
- MSVC /WX passed five suites. Edit Plan 680/680, Clip Edit 420/420 and Take Lane Ops 74/74, plus Ops and History. Ops and schema validators passed. The new take checks cover each edit and a single Undo, overlapping main and other take lanes, and mis-scoped Delete.
- Engine review requested for the lent EditScope changes. Window-fn can pass the selected take lane when it wires the take controls.

PR requested.

## 2026-11-22, MX-09 audio take placement offset

- `engine-win/recording-placement-offset` at `01abcaa3` adds a signed, explicit sample offset to linear take placement. Capture timing, file bytes and Undo grouping retain their existing values. An offset of zero keeps existing callers' behavior. An excessive positive offset puts the first captured sample at song zero. Overflows are refused before staging.
- MSVC /WX recording commands 162/162. An input latency of 256 samples, output latency of 384 and driver error of 1 ms at 48 kHz move a 128-frame take from sample 800 to 112 with exactly the same PCM. Zero, signed corrections and song zero passed. In WTS-01 2a the zero-offset caller still captured all 384,000 samples exactly.
- **Parked, driver wiring.** Engine to expose input/output latency to the recording owner, and window-fn to pass the sum with the signed driver error converted at the captured rate. The explicit offset is available now for linear placement. Loop-pass boundary compensation remains a separate recording policy.

PR requested.

## 2026-11-22, engine-win register audit on main `87ea349c`

| Register row | Landed proof | Regression in the landed tree |
|---|---|---|
| DF-E19 (a) | #666 `71ef466a` | `test_render_queue.cpp` `publicationResults` and `runManifests` keep the published result and durability warning. |
| DF-E19 (b) | #646 `629a9f3d` | `test_collect_export.cpp` `publication` checks private verified-copy staging, ownership and cleanup on collision or failure. |
| DF-E19 (c) | #646 `d661f704` | `test_collect_export.cpp` `publication` checks the AtomicOnly refusal path and the explicit verified-copy path. The production callers state their policies. |
| DF-E19 (d) | #666 `96ac4186` | `test_render_queue.cpp` `finalNamesAndCaseCollisions` verifies the complete name and suffix byte limit. |
| DF-E20 (c) | #686 `262a2606` and the joined catalogue | `test_play_render.cpp` `catalogueContracts` checks no discovery on a known VST3 lookup. `test_scan_cache.cpp` `reuseAndChanges` checks that unchanged probes reuse validated metadata. |
| DF-E25 (a) | #686 `3bfd0d5c` | `test_plugin_scanner.cpp` `reservedCandidateExits`, `helperStartFailures`, `hostAndFailurePolicy` distinguish helper failures, candidate exits and durable blocking. |
| DF-E25 (b) | #646 `629a9f3d`, with #686 regression `bcf82f20` | `test_project_replace.cpp` `publicationRetries` checks that a verified copy returns native identity and a directory-sync warning when durability is unconfirmed. |
| DF-E28 (a)–(c) | #686 `1abfcb3c` | `test_decode_space.cpp` tests an impossible FLAC total before reserve, and `test_decode_cache_consistency.cpp` covers oversize admission, malformed hints and published cancellation. |
| DF-E31 (a)–(d) | #686 `3bfd0d5c`, `0f7289b2` and the joined scanner service | The saved-path and deleted-copy cases in `scanner_discovery.hpp` cover catalogue misses and relocated CLAP modules. `test_plugin_scanner.cpp` covers child exits on both sides of candidate start. `test_scan_cache.cpp` `waitsForRegistryWriter` covers write admission. N68 separately covers project hints after the asynchronous service has taken ownership. |
| DF-E32 (a) | #686 `d90479e4` and `bcf82f20` | `test_project_replace.cpp` `windowsPublicationRetries` includes protected-stage sharing failures, retry exhaustion and no-clobber controls. |
| DF-E32 (b) | #686 `d90479e4` and `bcf82f20` | `test_recording_commands.cpp` `crossVolumePublication` verifies a distinct-volume capture, exact PCM, project-side atomic publication and Undo/Redo. |
| DF-E32 (c) | #686 single-WAV and publication stack | `test_render_queue.cpp` `finalNamesAndCaseCollisions` and `singleWavDestination` cover device stems with a spaced extension and the final filename refusal. |
| DF-E32 (d) | #686 `bcf82f20` | `test_render_queue.cpp` `runManifests` preserves synthetic high file-ID and volume bits. `mediaIdentities` compares both halves against Windows FileIdInfo on NTFS; the volume fixture uses NTFS. |
| DF-E33 (a), (b) | #686 `a99448c4` | `test_settings.cpp` `testPortableSettingsPublication` and `testBundlePublication` exercise unsupported native operations, intact existing archives and a competing destination. |
| DF-E34 (a), changed dimensions | #686 `d4ecedb2` | `test_progressive_decode.cpp` `changedSavedSourceDimensions` reports mismatch before and after eviction. `damagedSavedCacheDimensions` still diagnoses a damaged cache and `savedHashesOnSessionOpen` preserves the warm zero-digest gate. |
| DF-E34 (b), (c) | #686 `1abfcb3c` | `test_decode_space.cpp` `impossible` preserves existing cache bytes and order with no admissible space. `test_decode_cache_consistency.cpp` `extremeStamp` covers the maximum timestamp. |
| DF-E34 (d) | #686 `d4ecedb2`, after #661 | `test_analysis_cache_trim.cpp` `unreadable`, `undeletable`, `cancellation` and `changedDuringInventory` cover inventory and removal with retained refusals. |
| DF-E35 (b) | #694 `759014e9` | `test_freeze_commands.cpp` `mainLaneExtentMatrix` covers the eight Freeze and Bounce cases, including overlap and group scope. |
| DF-E58 | #686 `274f81f5` | `test_audio_recording.cpp` `stopDrainsLastShortBlock` forces the final accepted block to race with Stop and checks the final WAV. |

**Still open** — DF-E25 (c) class ID, DF-E34 (a) same-shape external edits (TM-MD-03), and DF-E35 (a) snapshot lane projection and window readers. #677 is on main; the snapshot and window handoff still needs its owner coordination. This is the log-only audit and requests the register update from Win.

## 2026-11-22, N66 stacked after DF-E44

- `engine-win/recording-failed-prefix` at `0900deeb`, based on #710 and stacked on DF-E44 `0ed2c355`. Land DF-E44 first, then N66. The prefix receipt is set after the worker has reported completion and only if neither the write loop nor any sink close reported an exception. DF-E44 still closes every sink, resets the pending job before discarding and keeps any directory that `std::error_code` reports it could not remove.
- MSVC /WX and `ADI_ACCEPTANCE=ON`: recorder 172/172 (the DF-E44 170 plus two close-after-discontinuity checks), recording commands 149/149, document 319/319, and WTS-01 2a matched all 384,000 captured samples. Playback step 2b still reproduces the known first-block failure on this independent stack.
- **For window-fn:** stack DFE44-DISCARD on the joined N66 head. Keep its successful interrupted take and one-Undo receipt; surface cleanup refusal and the retained directory in the failure path.

PR requested.

## 2026-11-22, DF-E64 review follow-up

- `engine-win/play-first-block` at `01b1e723` includes current-main base `ac07be74` and changes only the lent clip source and the focused test since the review. Strict `primeAt` for a stopped transport still needs no pages. The live clip callback uses a private playing snapshot for stopped prefetch; the driver's song clock holds pending Play.
- MSVC /WX with `ADI_ACCEPTANCE=ON`: clip candidate readiness 95/95 (90 existing, five held-reader controls), window state 115/115, WTS-01 2b matched all 384,000 frames with zero underruns. In this run four simulated callbacks elapsed before the first song frame. The later WTS fader sequence exhausted the prior harness's 32 unpaced simulated callbacks and did not finish. No acceptance sources changed in this follow-up.
- **Parked, review and integration:** #710 creates a dry-merge conflict only in window-fn's `window_state.hpp`. Window-fn should reconcile and review that hunk; Engine should supply or lend the Session hunk. Win's harness needs to allow actual worker time or a bounded test-controlled readiness release for later steps, while retaining the silent hold and exact first frame checks. The focused test holds and releases the real reader independently of that harness.

PR requested.

## 2026-11-22, standing queue dependencies before 3 December

- **Parked, DF-E64 integration.** Engine or Win: please supply or lend the `Session::RenderBlock` Play-path hunk. Window-fn: please review the published `window_state.hpp` and `project_document.cpp` hunks. Win owns the WTS-01 harness; please capture from the first playing callback while asserting silence and an unchanged song position during the hold. The focused delayed-reader test uses no owner-side prime.
- **Needs, MX-09.** Win's latest ownership entry confirms the recording half. Engine: please expose the driver's input and output latency in samples to the recording owner. I will build and verify the explicit placement offset.
- **Needs, CT-01.** I will keep DF-E17's same-track lane retention and explicit cross-track detach contract. Engine review of the EditScope hunks requested.

## FOR THE DIRECTOR

- **MX-09.** When latency correction would put the take before song zero, should the pre-zero part become a left handle? Recommend keeping all recorded PCM and placing its first frame at zero until pre-zero placement behavior is agreed.

- **CT-01 and CT-02.** Should a take audition replace the main track, persist, or create Undo? What is Live's exact new-lane name, and where does recorded pre-roll go with loop and punch? Recommend transient exclusive audition without an Undo entry, and keeping pre-roll outside loop-pass clips until a reference capture settles placement.
- **CT-04.** How does comping treat an empty take-lane interval and a selection that extends beyond the take? Recommend preserving existing main audio outside the selected source material until the measured behavior is settled.
- **EX-03.** Where does Live start the unwritten pass of Render as Loop? Recommend using the selected range start for the loop pass and handling earlier priming separately, pending the reference.
- **EX-05.** Should export brickwall detection go beyond BS.1770 4x? Recommend keeping 4x as the compatibility meter and using finer detection only if an oversampled acceptance test still finds a ceiling miss.

## 2026-11-22, project saved plug-ins and the #642 gate

- **PR requested** — N68, `engine-win/project-plugin-saved-path` at `43787b7b`. After an adopted Incremental or Refresh job, a project can probe an installed VST3 or CLAP named by its saved absolute path. The request uses the isolated helper and respects quarantine. Explicit CacheOnly and in-flight jobs suppress new probes. A miss without a saved path keeps the adopted Browser catalogue and starts no scan. In the Windows JUCE `adi_play_render_tests` suite, real VST3 and CLAP fixtures outside the search roots loaded after adoption. The CLAP quarantine and missing-hint controls passed. The Windows desktop application built with N62 and N68 together.
- **PR requested, existing #642** — `68c92bbc` already removes `rankable()`. All eight statistics self-checks and 14 performance unittests passed. A dry merge with `0ea10b8e` succeeded. The GitHub macOS GUI-shell job is recorded as cancelled. The other published checks passed.

## 2026-11-22, VST3 export and the Browser's scan cache

- **PR requested** — N62, `engine-win/export-host-owner` at `6b78d94c`. The render session creates, prepares, restores, services and destroys plug-ins on the host owner. The worker renders blocks and writes the file. Quit and Cancel can service pending owner calls through teardown. WTS-01 7d in the JUCE acceptance build rendered 192,000 frames including the saved note from a real VST3 fixture, and 7e passed. The render queue passed 437 checks, including state, thread affinity and cancellation.
- **PR requested** — DF-E62 (a) and (d), `engine-win/scanner-cache-ownership` at `c6bdb514`. A successful fresh probe without a reliable stamp retires the old cached packet in the ticket transaction. The prune reader waits through a short competing registry write. This is the Browser prerequisite.
- **PR requested** — DF-E62 (b), (c) and (e), `engine-win/scanner-cache-paths` at `1d6dd030`, stacked on `c6bdb514`. Retire the spelling of a case-renamed module only when its canonical replacement was seen. Strip trailing root and dangling-target separators. Include extensionless executables in a mixed-case macOS bundle layout. The pinned Windows scanner cache and prune suites passed 141 and 29 checks, respectively. The scanner protocol and catalogue suites passed on the (a)/(d) candidate.
- **PR requested** — N66, `engine-win/recording-failed-prefix` at `0747821d`. A discontinuity, overflow or other input interruption can attach the prefix only after all sinks finish and the normal project-media checks verify their WAV dimensions. The window reports the cause and places the kept prefix in one undo step. The recording, recording commands and document suites all passed. An I/O failure without a verified finalized prefix is still refused. This candidate is independent on current main. The older DF-E44 and PD-06/PD-05 branches overlap recorder completion and need reconciliation if taken in the same window.
- The previously withdrawn DF-E51, DF-E45 and DF-E54 candidates need no follow-up PR. The current engine fixes passed the carried zone-change and FIFO checks, 44.1 kHz Beat 2 and bar boundaries plus the stored-nanosecond round trips, and Collect and Export after Cut or Undo.

## 2026-11-22 — DF-E64 Play starts with the recorded take

- PR requested `engine-win/play-first-block` (`c7f3d9cc`). Engine review (loan). Read ahead at the stopped or located playhead, and keep Play pending until the graph running the first block admits its source pages. Stop cancels pending Play, a later Locate retargets it, and failed media stays silent without holding unrelated clips. Offline admission remains strict.
- Windows MSVC Debug, `ADI_ACCEPTANCE=ON`, pinned SQLiteCpp, warnings as errors: WTS-01 2b before the fix lost 256 frames of the 384,000-frame take. The fixed run waited one 256-frame callback after a cold Locate/Play, then matched every take sample with zero underruns. The acceptance driver checks that the held callback stays silent at the locate position and measures from actual transport start. Full headless WTS-01 and the clip playback, render slices and window state tests passed (4/4).

## 2026-11-20, release a failed recorder before cancellation (DF-E44)

**PR requested** — `engine-win/recorder-failure-recovery` at `0ed2c355e2c71087ccd896c65e5c30fa40e5a6fa`, on main `917b22c0`. Every created sink gets a close attempt. The worker releases all sinks before reporting completion, even if one close throws. The owner clears the pending job before discarding. A removal refusal is reported, with the directory retained for retry. Engine seat review requested before the Window Record entry point.

**MEASURED.** On Windows the baseline failed 11 assertions with two real WAV sinks and an injected disk failure after both first writes. Both callback acknowledgement paths previously threw and kept Record pending. The exact submitted branch passed all 170 focused recorder checks under strict MSVC x64 `/W4 /WX`, including a real Windows delete-sharing refusal, cleanup retry and starting a second capture. This focused build used the existing local dependency cache. The landing window's pinned SQLiteCpp ABI matrix is already complete under #686.

## 2026-11-20, interrupted recording placement for PD-06

**PR requested** — `engine-win/interrupted-audio-recording` at `db039e655ac2dd9ee39c2eab7d5563e9479a0d92`, after DF-E44 `0ed2c355` and CT-02 audio loop reuse `d3e53de4`, on main `917b22c0`. Core partial for PD-06. Record can publish the versioned plan before callback admission and the actual first timeline sample before writing PCM. An exclusive marker protects live recording and placement. Recovery verifies the project and native file identities, repairs private RIFF/RF64 copies of the shared complete prefix and reconstructs loop passes. Then it uses the ordinary Stop commands and groups recovered edits into one Undo.

**MEASURED.** Strict MSVC x64 `/W4 /WX` passed 572 checks in four suites. A separate process exited during an actual recording, bypassing Stop and destructors. A fresh project owner recovered and placed its 128 PCM frames and could Undo. The gate also covered stale RIFF and RF64 lengths, odd PCM24 padding, exact sample and original-file preservation, read-only and deleted-track refusal, byte-identical replacement-file refusal, loop Stop geometry, commit failure/retry and reopening after Undo without duplicate placement.

**For engine and Window.** Document-side Record calls `recording::configureRecovery`, then the existing Session start. PD-05 supplies the launch candidate folder and project, and the Window reopen question can name the returned targets before placement. The candidate uses a project-local, transaction-bound `adi_meta` receipt keyed by recording ID, also written on ordinary Stop and preserved across Undo. This uses the existing journal hook with no new schema number or op. Please review the receipt namespace and the four-line Session check. The launch discovery and reopen question still need their PD-05 and Window work. The asynchronous writer's flush and ring do not establish a one-block power-cut bound. The engine has not been tested by an actual power cut. The Windows repair adapter accepts ordinary drive-rooted paths. UNC and extended device paths are refused.

## 2026-11-20, shaped WAV dither and the recording/export integration gate

**PR requested** — `engine-win/export-quantizer` at `7a9fe563d15facfec00abd778e749ee205d6ce43`, on main `917b22c0`. The common PCM16/24 quantizer uses the pinned SoX Lipshitz, improved E-weighted and Shibata tables for Shaped 1, 2 and 3. It keeps per-channel histories, a seeded stream across output partitions and a conservative full-scale margin when shaping is active. Direct WAV and normalized WAV both use it once. The run manifest records requested and effective modes. Unsupported shaping rates use ordinary TPDF and Float32 bypasses the quantizer.

**MEASURED.** The exact export branch passed 868 strict MSVC x64 checks over five suites. The five supported mode/rate pairs tested at 44.1 and 48 kHz reduced the 1–5 kHz band by 14.9–21.6 dB relative to TPDF on the fixed-seed silence fixture and increased the band above 15 kHz. Neither PCM depth saturated on the full-scale shaped fixtures. Seed/partition determinism, exact fallback, Float32 bits and one quantization in normalization passed.

**MEASURED, combined.** The exact dry merge of the PD-06 and EX-06 branches passed 1,140 strict MSVC x64 `/W4 /WX` checks in seven suites using the landed SQLiteCpp 3.4.0 pin. Schema and ops validators passed. The separate DF-E44-only run with the pin passed 300 checks in three suites. The optional physical >4 GiB WAV fixture was not run. Native NI AAC and a symlink fixture were unavailable. Live's Pow-r spectra were not run.

**For Window.** Keep the existing saved keys `pow-r1`, `pow-r2` and `pow-r3` and show Shaped 1, 2 and 3. Map them to the corresponding enum values in the Export page. The held job JSON and preset codec will need to read and write those modes when its queue resumes. Main has a WAV writer. AIFF and FLAC can use the same integer-code API when their writers exist. Please have engine review the shared WAV and normalization integration.

**d7 refresh.** The #686 SQLiteCpp gate is closed. #691 already wires F9 for linear audio, so DF-E44 serves that reachable path now. Window's F9 still refuses loop record and does not call the new recovery configuration. The earlier CT-02 and DF-E35(b) candidates can land. DF-E35(a) still waits on #677 and CT-03 waits on the engine's new-clip note producer. The N58 owner-worker API is on main. Window's interim `hold()` joins on the message thread and should be replaced by the existing start/poll/adopt flow with catalogue hits kept available. Hosted EX-11 still needs the shared lifecycle contract. No new deferred ID was used.

## 2026-11-20, keep live MIDI releases paired (DF-E51 a and b)

**PR requested** — `engine-win/mpe-held-notes` at `fc2d0a7b1c68a3c46851ebf33499c13d8730a9f8`, on main `b56b1348`. Engine seat review requested for the two lent parser files. A fixed shared pool carries a FIFO for each channel/key in both zone modes. Changing the zone preserves held IDs, and per-channel expression can return to an earlier still-held note. The output router receives the original IDs through its existing exact-ID path.

**MEASURED.** The original code failed seven regression assertions including both zone-change directions. The fixed strict pinned Windows gate passed 223 checks across the input, output, native MPE and retrospective memory suites. It includes capacity refusal, slot reuse and no callback allocations or file I/O.

## 2026-11-20, Capture MIDI clock, journal target validation and #642

**PR requested** — `engine-win/midi-capture-clock` at `f26730fb82ff7b7984485820c0c2d9f66ed2f981`, DF-E45. Engine seat review requested. Capture keeps its origin tick, picks the nearest representable tick for each exact driver-sample offset and checks the sample round trip. The shared map snaps rounding dust near integral ticks and keeps the floor for genuine fractional times, with non-finite and overflow guards. Baseline failed six checks. The corrected cases include 110 BPM beat 2, 128 BPM bar 2, nanosecond round trips at 90/110/140 BPM and the preceding side of a tempo edge.

**PR requested** — `engine-win/project-check-journal-targets` at `55f20c8c3e6fde6ed1171f70b7cd5ab68d137e40`, DF-E54 a. Engine seat review requested. Live-row reference validation skips journal subjects and keeps structural journal checks. The test removes a clip through Cut or Undo and still produces an archive. Baseline reproduced four false errors/refusals. The two exact candidates merge cleanly. Their combined strict Windows gate on the SQLiteCpp pin passed 663 checks in five suites, covering Capture, transport, clip edits, the checker and Collect and Export.

**PR requested, existing #642** — verified `engine-win/acceptance-ci` at `68c92bbcb60c1c0da9b3267a0c8d616be3aefd8a`. Its stats file has no `rankable()` and its per-app statistics self-test passed all eight checks. A dry merge on `b56b1348` leaves the evidence and statistics files byte-identical to that head. The historical JSON key is a within-app frequency gate. No cross-DAW ranking or CPU measurement was made, and no additional source change is needed for this condition.

## 2026-11-20, startup recording discovery (PD-05)

**PR requested** — `engine-win/recording-launch-discovery` at `879a5d15dfb5abed5b7560d2fd126ac07c9c30d1`, after PD-06 `db039e65`, based on main `b56b1348`. Before accepting audio for an opted-in recording, the engine registers it under a native per-process session lock in application data. The startup worker can call `recording::discoverInterrupted()` without any project or take path. It ignores live sessions, incomplete metadata, takes without complete audio, and IDs with placement receipts. It verifies the recorded project and returns the target IDs for the reopen question. Discovery leaves the original files intact.

**MEASURED.** The exact branch passed 434 strict Windows checks across the recorder, recorded placement and take-lane suites. One real process exited during recording and its take was found without the source directory being furnished to the finder. An additional process exited after registration but before the first input and was skipped. Tests also exclude a live session and a committed take whose original remains after Undo. Schema and ops validators passed.

**For Window and engine.** The startup consumer calls the finder on a worker and skips that call when Alt is down. The chosen project and directory then go to the existing PD-06 prepare/commit flow. The desktop startup hook and named question remain Window work. This is the recording-discovery slice of PD-05. Project-wide session_lock heartbeats, stale-owner handling, OpenReport and clean-close or backup metadata still need the Store lifecycle work. No new deferred ID was used.

## 2026-11-20, integration check for the three loans and startup discovery

**MEASURED.** The exact requested tips `fc2d0a7b`, `f26730fb`, `55f20c8c` and `879a5d15` merge cleanly on `b56b1348`. Their combined tree passed 593 checks in five strict MSVC x64 `/W4 /WX` suites with the SQLiteCpp 3.4.0 pin. The gate exercised live MIDI parsing, Capture MIDI, actual archive publication, audio recording and process-exit recording discovery. The review and landing requests stay on the four original branch tips.

## 2026-11-19, order the MIDI settings before the graph-worker preference

**PR requested** — `engine-win/graph-worker-setting` at `35a035acaf21b95da53e47923b217e945de1905d`, after Window-fn's MIDI settings `6cec1b83` and current main `88e2d94e`. The merge retains both Settings sections. The existing engine application wiring can merge the resolved settings tip. The worker preference stays Auto at granted buffers of 1,024 samples and above, serial below the threshold or for an unknown buffer, with explicit Off.

**MEASURED.** With the portable settings and single-WAV stack also present, strict MSVC x64 `/W4 /WX` passed 317 settings checks and 244 project-document checks. The public candidate merges to the exact tested tree. Hosted worker hardware remeasurement and the engine's desktop wiring are separate.

**Remaining owner gates.** DF-E35 a needs the snapshot loan after #677, CT-03 needs the engine's absent-stream note producer, EX-11 hosted rendering needs the engine and Window lifecycle contract, and the existing SQLiteCpp request needs Win's ABI matrix. No further engine-win finding in the current d7 queue lacks a candidate.

## 2026-11-19, order portable settings before the single WAV follow-up

**PR requested** — `engine-win/single-wav-export` at `1da021ae72b764729ba0da9aa6914a67772765d6`, after portable settings `0e6c6663`, file publication `b4485df6` and current main `88e2d94e`. Land the settings and publication families first. The merge preserves both the guarded existing-file binding for explicit WAV Replace and the separate portable settings policy. The Window-facing Job fields are unchanged.

**MEASURED.** The exact combined tree passed 1,842 strict MSVC x64 `/W4 /WX` checks across ten suites, plus 54 checks with recording capture and project folders on separate volumes. Symlink cases lacked privilege and the optional NI encoder was unavailable. Dry merges also passed with the scanner, decode, MIDI settings, Freeze and Bounce extents, and audio loop reuse branches.

## 2026-11-19, worker measurement kit and independent owner reviews

**PR requested** — `engine-win/worker-measurement-kit` at `3ac1b3ff9b2cba017d6915c685c324a7ce400f3d`, after `engine-win/acceptance-ci` at `68c92bbc` and current main `88e2d94e`. This retains the corrected per-app statistics and the original worker fixture, runner and analyser. The requested kit diff relative to the acceptance base contains only its six fixture and measurement files.

**MEASURED.** Regenerated and verified the fixture manifest, including 66 audio files, seven edge and latency fixtures, eight MIDI fixtures and event references, 16 tuning fixtures and ten Grid specifications. Both statistics self-tests passed. Five strict MSVC x64 `/W4 /WX` acceptance suites passed, including the three module self-tests, the oracle and 197,018 hosted checks. These exercise the fixtures and host path. The physical worker remeasurement remains separate.

**For Win, DF-E35 b.** The engine reviewed the exact `759014e9` extent branch. GCC 14 reproduced the eight failures on main, and the final 303 checks passed on GCC 14 and Clang 19. Its extra local Bounce commit and Undo probe also passed. See `adi_daw/collab/engine.md` on `engine/log` at `d4b142b6`.

**For Window-fn, N58.** The merge audit found shared `ui_main.cpp` blocks in the Window scanner and Device MIDI branches. Use the N58 service as the startup, project-load and Rescan path and keep Window's MIDI factory and deferred refresh when combining the Settings callback. The helper-failure candidate's typed outcome and the MIDI registry branch merge cleanly together.

## 2026-11-19, hosted export lifecycle needed for EX-11

**For engine, Window-fn and Win.** Window-fn's current Export page already dispatches instance creation to the message thread. The private render Session currently sets offline mode, restores state, services host callbacks and destroys devices on the render worker. A loader-only dispatch leaves those calls on the worker.

The in-process contract needs one private render Store and Session with owner-thread construction, state restoration, servicing and destruction. Processing runs on the render worker between those owner calls. Engine should confirm the Session handoff or supply a narrow lifecycle seam. Window-fn should keep the owner dispatch available through required teardown. The current message-thread Stop joins the render worker, which would deadlock if that worker were awaiting an owner call. An acceptance fixture should cover VST3 and CLAP, thread identities, saved-state PCM and cancellation during a queued owner call.

The existing `engine-win/single-wav-export` branch at `87959193` supports the literal Save filename and identity-bound explicit Replace. Window-fn can use these for an overwrite confirmation while the hosted lifecycle work proceeds.

## 2026-11-19, typed scanner helper startup failure for Window-fn

**PR requested** — `engine-win/scanner-helper-start-state` at `677741228631fadb15166086584a6b8ba6d264f7`, based on main `88e2d94e` through scanner `a5a12cf7` and N58 `bd021052`.

**For Window-fn, DF-W34.** Read `scan::State::HelperCouldNotStart` from `DiscoveryResult::report.failures` after adoption. A failure before candidate admission has an empty path. A candidate-time launch refusal can include its path, with the typed state naming the helper as the cause. The previous accepted catalogue stays available. N58 already provides the owner-worker startup service, CacheOnly/Incremental, explicit Refresh, default worker limits and typed VST3 instrument metadata. The Browser's adoption and presentation remain with Window-fn.

**MEASURED.** The native fixtures covered missing deployment, a helper removed after scanner open and the OS rejecting an invalid executable. The refusal leaves no candidate strike, failed-cache entry or pending ticket. The prior catalogue remained usable and the restored helper reused it. All ten scanner/player suites passed under strict MSVC x64 `/W4 /WX` and the Windows app linked. Typed propagation in the host files is confined to their scanner entry points.

## 2026-11-19, decode-cache findings on current main (DF-E28 and DF-E34)

**PR requested** — `engine-win/decode-space-admission` at `296c87b136a7f7aa86bb5db49f8693a952562d6f`, based on main `88e2d94e`. This review tip reconciles the previously requested decode admission and accounting work with the saved-source and analysis-trim fixes. It includes the #664 source stack and the pending `60fa903d` change. Use this tip for the combined review.

Strict MSVC x64 `/W4 /WX` passed 857 checks in eight suites on the exact stack. The tests cover saved-source mismatch, warm-source reuse, trim refusal and continuation, space admission and real playback. Existing native AAC and symlink cases lacked codec support and privilege. The earlier same-shape saved-source limitation remains in the DF-E34 handoff.

## 2026-11-19, automatic audio take-lane reuse (CT-02)

**PR requested** — `engine-win/audio-loop-lane-reuse` at `d3e53de4b8407ba6c8e69f09cca8e6ad9fc12a69`, on main `88e2d94e` and after audio loop materialization at `b4907bcc`.

The engine picks the first eligible visible take lane on each track. Every existing clip must end by that pass's capture start. Each existing lane is used at most once per capture. The owner-supplied new IDs are fallbacks for passes without an eligible lane. New lanes keep pass order, and existing clips and lane identity survive. First-visible selection is an ADI decision while the director's reading of Live's tie case remains open.

**MEASURED.** The pre-fix recording suite failed 10 of 149 assertions. The fixed strict MSVC x64 `/W4 /WX` gate passed 11,063 checks in five suites, including sample-exact tick and nanosecond endpoints, future material refusal, mixed reuse and fallback, Undo/Redo and rollback. The ops and schema validators passed. The loop-recording checklist records the director's mid-pass Stop, per-pass Undo and MIDI Overdub Off readings, with the ADI desktop and CT-03 gates identified.

## 2026-11-19, recording recovery and dither rulings (ADR-0250)

The director approved recovery of interrupted recordings as one Stop-equivalent Undo step with a recording ID to prevent duplicate placement. Shaped 1, 2 and 3 are SoX Lipshitz, improved E-weighted and Shibata. PD-06 and EX-06 remain in their ordered M5 slots.

## 2026-11-19, MIDI port name and manual-arm default for Window-fn

**PR requested** — `engine-win/window-fn-recording-settings` at `6cec1b83236a7262536d9b36036d8c85fb73f316`, based on main `bedee5ab`.

**For Window-fn.** Store a last known text name in each directional record under `midi.ports`. The Device row `midi.port.name` supplies validation and an empty default. Existing records without names remain valid. `record.autoArmOnSelect` defaults to `none`, as in the Settings Catalogue and the current ruling. The earlier instrument/MIDI default is superseded. Explicitly saved choices still load. Window-fn owns matching changing identifiers by name and applying the arm preference.

**MEASURED.** Five assertions failed with the previous settings registry. Strict Windows x64 MSVC `/W4 /WX` passed all 193 checks after the change, including independent input/output records, invalid type and scope refusal and exact reload of saved values.

## 2026-11-19, Freeze and Bounce in Place read audible extents (DF-E35 b)

**PR requested** — `engine-win/take-lane-render-extent` at `759014e9e7d79fce4d2cbe609042bbd3b4e4e6de`, based on main `bedee5ab`. The engine seat reviews its two lent extent hunks. Freeze uses the timeline clip accessor for track and group scope. Bounce in Place queries main-lane extents. Take clips survive its replacement. The time-base and overflow checks stay in use.

**MEASURED.** The repository regression runs eight cases across tick and nanosecond time, both track hierarchies, and presence of a later take. It exercises both commands, checks exact pre- and post-FX Freeze capture PCM, and verifies that preparation leaves project rows unchanged. All eight readiness assertions for the four take cases failed on main and passed with the two fixes. Strict Windows x64 MSVC `/W4 /WX` passed 303 checks across four suites. The longer capture also verified silence where a stored take overlapped the captured range.

## 2026-11-19, native DEV-23 application policy gate

**MEASURED for engine and Win.** Exact `engine/dev23-app-workers` head `3dd8c6056e13e6e2a89c5173a054360fd2768683` passed the settings and document suites under strict MSVC x64 `/W4 /WX` with Pd. The 447 checks include 257 native document checks. The headless driver fixtures exercise the actual worker dispatch at the granted-buffer boundary, serial Off, reconfiguration and matching PCM. Hosted worker admission still follows #677.

## 2026-11-19, audio loop takes with one Undo per pass (CT-02)

**PR requested** — `engine-win/audio-loop-materialization` at `b4907bcc33e29f61105f9d95a33521c92a7cd5ff`, main base `bedee5ab`. Land after the engine grouped-Undo contract `a0a42ebd`, publication `b4485df6`, Stop fix `274f81f5` and recording rollback `b89d58ee`. The materialization work is the final commit on that stack.

**MEASURED.** Real Session fixtures completed 3½ passes into one continuous WAV, four ordered take clips and the main comp. The last partial pass replaced its own interval, preserving the preceding pass's tail. One Undo removed the fourth take and restored the third pass's PCM. The read-only observer saw the preceding database until the complete commit. A failure in the last pass rolled back every pass. Both graph precisions, pre-existing main audio, multi-target deduplication, the saved histories and the identity cleanup regression passed.

Strict Windows x64 MSVC `/W4 /WX` passed six suites with 11,527 checks, including 124 in the recording commands suite. The last test revision reran the recording suite and exercised real distinct capture and project volumes with 174 checks, including the four-take case in both precisions. Ops and schema validators passed.

**For Window.** Once the parents land, pass the completed loop result from `Session::recording()` and one media ID per target with one lane and clip ID per pass to `recording::prepareLoopCommit`. The shared `recording::commit` publishes one transaction. The new take block is above the existing lanes, and its order follows the director's recorded pass order. Existing lane reuse, punch and pre-roll policy, latency compensation and desktop wiring remain open. #677 still gates DF-E35's snapshot loan. The MIDI tap is available on the engine branch and CT-03's note creation seam is requested below.

## 2026-11-19, settings safety on current main

**PR requested** — `engine-win/settings-portable` at `0e6c6663d9756bf05b804be7fb4b3b33677987b0`, on main `bedee5ab`. This is the single DF-E33 review family for #673 and #672. Strict MSVC x64 `/W4 /WX` passed 533 checks across settings, project replacement, project files and collection. Optional symlink fixture cases lacked privilege.

## 2026-11-19, scanner and N58 on current main

**PR requested** — `engine-win/scanner-dfe31` at `a5a12cf7c4d7ed0e94e287000378958025275967`, on main `bedee5ab`, then `engine-win/scanner-service` at `bd021052ad154f2a1864346dfa42c6e8d7136720`. The exact stacked tree passed ten scanner and player suites under strict MSVC x64 `/W4 /WX` and linked the Windows desktop app. The async fixture again exercised startup, adoption, invalidation, cancellation, stale work and warm catalogue reuse.

**For Window, #676.** As read at `259a2b90`, the worker holds `loaderMutex_` during `loader_.scan()` and the project device loader takes that same mutex. Instantiating a known device on the message thread can therefore wait for the whole scan. The N58 service above runs discovery on its own worker and adopts typed catalogue data on the owner. The existing N58 handoff gives the startup, Rescan and project-open entry points. Desktop integration remains with Window.

## 2026-11-19, retain later files when a recording transaction rolls back

**PR requested** — `engine-win/recording-rollback-identity` at `b89d58eec89780aa9b951488605e81b0f7893e22`, after `engine-win/file-publication-reliability` at `b4485df6`. Recording now uses the shared identity-bound rollback. It records only files this attempt published, never deletes a later file at the same name, reports retained paths in the failure and leaves the private source available for fresh preparation. Hosts without atomic identity-bound removal retain and report the file.

**MEASURED.** A real SQLite COMMIT veto moved the published recording aside and put a later file at its path. Three baseline assertions failed because the later file was deleted and the error omitted its path. Strict Windows x64 `/W4 /WX` passed all 56 checks after the fix. Both identities and their bytes survived the conflict, project rows rolled back, normal owned Windows cleanup passed, and the capture was recoverable.

**For engine, CT-03.** The published MIDI tap supplies the driver clock. For new take clips the note producer needs an owned absent-stream snapshot for a clip that will be created earlier in the same transaction. Please provide or lend a small `captureNotesForNewClip` preparation API using the existing typed default row and `prepareDiff` validation. Current `captureNotes` requires a live clip row, and the Capture operation is a different path. The director's reading covers the four live overdub/loop cases and separate Record Quantize Undo. It also shows that the main clip can carry a held note across passes while the take closes it at its pass boundary.

## 2026-11-19, publication and single WAV follow landed #666

**PR requested** — `engine-win/file-publication-reliability` at `b4485df6b7c6caa511e6274b08ee62d802205c16`, on main `bedee5ab`. #666 already landed the queue manifest, folded collisions and final suffix budget. The reconciliation retains them and adds bounded stage retries, project-volume recording stages and spaced Windows device-stem escaping. Post-publication directory sync is carried through both exclusive rename and verified copy. Strict MSVC x64 `/W4 /WX` passed 980 checks across eight suites, plus 54 on actual separate capture and project volumes.

**PR requested** — `engine-win/single-wav-export` at `87959193cd9032a4970fdcb84f33774447ce800e` after that publication branch. This is a follow-up to #666. It keeps the literal WAV filename and explicit Replace binding, the widened native file ID and the existing export media verification. Strict MSVC x64 `/W4 /WX` passed 1,540 checks across nine suites, plus 54 on separate volumes. Native NI encoding remained unavailable. Window still owns the Export page entry point and its host-thread servicing.

## 2026-11-19, retain the final recorded block at Stop

**PR requested** — `engine-win/audio-recording-finalization` at `274f81f54cde8ac7d292e5f3c1434fada1f4e86b`, based on main `bedee5ab`. The writer rechecks the published end of the ring after acquiring Stop on the empty path. It drains frames accepted before callback quiescence before closing each WAV.

**MEASURED.** A controlled Windows schedule parked the writer after it observed an empty ring, accepted eight samples, acknowledged Stop and released the writer. The previous code reported success with a zero-frame WAV in all three runs. With the fix all three WAVs contained the eight exact Float32 samples. Strict MSVC x64 `/W4 /WX` passed 273 checks across the recorder, recording commands and take-lane suites. The public short-recording regression covers both callback acknowledgement and a stopped driver. The deterministic schedule used private instrumentation. Win can add the register row requested by the engine.

**Take contracts.** Engine supplied CT-02 grouped Undo at `a0a42ebd` and CT-03's driver MIDI tap at `ae4cebf4` on its branches. Both are pending on main. CT-02 materialization can now proceed against the grouped contract. #677 remains open and still gates the DF-E35 snapshot loan.

## 2026-11-19, combined decode-cache gate (DF-E28 and DF-E34)

**MEASURED.** The requested `engine-win/decode-space-admission` at `1abfcb3c` and `engine-win/progressive-saved-source-mismatch` at `d4ecedb2` merge cleanly. Their combined tree `74f1751979ec67581a20130c478aaf8f123dea74` passed 853 checks across eight suites in strict MSVC x64 `/W4 /WX`. The progressive suite includes 50 warm sources opened through Session without hashing. The space-admission and trim regressions also passed. The existing native AAC and symlink cases lacked codec support and privilege, respectively. Review stays on the two existing branch requests.

## 2026-11-19, take-lane regressions reproduced (DF-E35)

**MEASURED.** Strict Windows x64 native tests reproduced the main-row overlap fault on main `12aab48a` source. The snapshot omitted the lane and reused the edited clip and track nodes after a lane-only change. Arrangement geometry returned the silent take instead of the overlapping main-lane clip. The targeted probe had 20 checks and 10 expected failures. Controls verified that the playback accessor already excludes takes.

The whole-track Freeze and Bounce in Place preparation defect is also confirmed at their native entry points. A render containing 0–6 seconds of exact main-lane PCM and one second of silence was refused after adding a silent take from 8 to 14 seconds. Four take cases reproduced the refusal across time bases and freeze scopes. The main-only controls prepared normally. An isolated two-file candidate made all eight cases prepare, with identical PCM hashes and unchanged project rows. It used the timeline accessor for Freeze and the main-lane filter for the Bounce extent query.

**Waiting.** #677 is still open and the snapshot loan begins after it lands. The Window owns main-row geometry and paint filtering. The probes and range candidate are available as private evidence. CT-02 and CT-03 remain on the engine's transaction and input-clock contracts.

## 2026-11-19, SQLiteCpp pin ready for ABI validation (ADR-0248 d7)

**PR requested for validation** — `engine-win/sqlitecpp-pin` at `e9af6572e607e62a224c33ecfe5711ea5c4ce041`, on main `12aab48a`. This branch changes only the dependency pin in `fetch_external.sh`. Win can carry the workflow hunk from `8a8f9a00` before running ADR-0024's full ABI matrix. The matrix is still required for landing.

The pinned source is SQLiteCpp 3.4.0 at `df0a28ca9733f687ab7ce77f070c0df84368a45e`. The rebuilt Windows executable reports SQLite 3.53.4. Bash syntax and strict MSVC x64 `/W4 /WX` passed, with 824 checks across eight storage and registry suites. Optional symlink cases lacked privilege. No workflow change on the requested branch.

## 2026-11-19, single-WAV export and full Windows file identity (EX-07, DF-E19, DF-E32 d)

**PR requested** — `engine-win/single-wav-export` at `77438d2e552fcf42eb46060641cc8835d368cde4`. Replaces the #666 export head after the publication reliability family at `c36e200d`. The branch carries win's `eee05d0e`, retains its folded collision and media verification, and preserves the closed-stage publication retries.

**For Window EX-11.** Set `Job.directory` and `Job.exactWavFilename` from the Save dialog, with the complete WAV filename. Use Fail by default and Replace after explicit overwrite confirmation. The renderer keeps the chosen literal spelling, checks the complete UTF-8 name before staging and records the chosen and delivered paths. Replace binds the existing file and checks identity, content and the folded collision set again before publication. The native runner, manifest and tests are ready. The Window still owns the Export page and the engine and Window own message-thread host preparation and servicing.

File identity now uses the complete Windows volume and file identifier when available. The legacy fallback is limited to unsupported queries and refuses sentinel indices. The export deduplication key, manifest, lease and rollback share the widened representation. Actual NTFS aliases and separate volumes passed. Controlled fixtures exercise different high halves. Sentinel rejection is implemented. A legacy redirector and physical ReFS remain untested. [Windows FILE_ID_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info)

Strict MSVC x64 `/W4 /WX`: nine suites and 1,515 checks, plus 54 in the separate-volume recording check. The literal-name fixture was rerun with a portable parent-alias assertion. Native NI encoding and optional link cases lacked support on this test host.

## 2026-11-19, Windows device names in the publication family (DF-E32 c)

**PR requested** — use `engine-win/file-publication-reliability` at `c36e200dddb8f4fee47f3f26ae325805a3418e6c` for DF-E32 a/b/c. This fast-forward adds the final 200-byte filename check and escapes device names when ASCII spaces occur before a dot. The generated name keeps those spaces and its original case. Ordinary nearby names and leading spaces keep their spelling.

All ten pre-fix regression assertions failed as expected. Strict Windows x64 `/W4 /WX` render-queue and NI Stems suites passed 321 checks after the fix. The native NI codec was unavailable, so the NI assertions were metadata coverage. The separate full file-identity change follows with the single-WAV export branch.

## 2026-11-19, decode cache admission and accounting (DF-E28 and DF-E34 b/c)

**PR requested** — `engine-win/decode-space-admission` at `1abfcb3cfa4a8dfc0c2bcab71a9b1374b0623843` on main `12aab48a`. It carries the six-path fix from `60fa903d`. A clean trial merge with the saved-source topic at `d4ecedb2` was checked. Win can use this branch for the DF-E28/DF-E34 b/c family.

Synchronous and progressive decoding share the same space preflight. It refuses an estimate beyond the cache cap or estimated reachable disk headroom before eviction. Existing pins, progressive reservations, LRU and the fresh free-space check remain in use. Logical file lengths estimate reclamation, with known allocation, sparse-file and hardlink differences. The index skips malformed hints, saturates the last-use clock, and accounts for a published file before a late cancellation. A shared FLAC check rejects frame counts physically impossible for the opened file length. The genuine compact fixture decoded 131,070 exact zero samples from a 68-byte FLAC. [RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html#section-4.1)

Strict Windows x64 `/W4 /WX` gate passed 748 checks in six suites. The new 45-check suite had 23 failures against the preceding code. Native AAC progress coverage was skipped. No other new case was skipped.

## 2026-11-19, internal graph-worker preference (DEV-23)

**PR requested** — `engine-win/graph-worker-setting` at `240dc0fd3bb6d6904887c8867cab2e40e63614c8`, on main `12aab48a`.

**For engine.** Read `settings::kGraphWorkersKey` from AppSettings and pass the value and the device's granted buffer to `settings::graphWorkersEnabled` when configuring the graph. The internal `auto` value enables workers at 1,024 samples and above, and `off` explicitly keeps serial processing. Unknown or smaller granted sizes stay serial. The registry row is outside Preferences and Find. The app's graph wiring is the engine's part of ADR-0248 d6.

MSVC x64 `/W4 /WX`: 421 checks in four suites, including the threshold boundary, persistence, invalid value refusal and Settings visibility. Some optional symlink fixtures could not run. The prior DF-E23 measurements are at 2,048 and 256 samples. The 1,024 decision is policy and its predicate is unit-tested.

## 2026-11-19, Windows publication and cross-volume recording (DF-E32 a/b)

**PR requested** — `engine-win/file-publication-reliability` at `d90479e48fe6165546e2f85eb91af944917af547` on main `12aab48a`. Closed-stage sync and exclusive rename share seven Windows retries for native errors 5, 32 and 33. Both verify the stage identity and ordinary file type before each attempt. One successful flush covers the rename retries. AtomicOnly keeps its existing exclusive-publication policy.

Recording copies the captured bytes into an owned `.adi-media-*` stage beneath the project's audio folder. The same-volume publication then commits one undo step. The actual two-volume case reproduced the cross-volume refusal before the fix. After the fix, the native move control still refused a direct cross-volume rename, and the recording attached, played byte-exact PCM through Session, and survived Undo and Redo.

Final MSVC x64 `/W4 /WX` build ran eight suites and 802 checks, plus 54 checks on the separate physical-volume run. Two optional link fixtures lacked symlink privilege. POSIX execution is pending. The retry rule is in ENGINE-PUBLICATION-RETRIES.md for win to carry beside DF-E19 and EX-07 in PLAN.

The SQLiteCpp 3.4.0 pin is already on `engine-win/sqlitecpp-wal-reset-fix`, with its Windows receipt. The ABI matrix remains pending and win owns the workflow hunk. DF-E35 is waiting for #677 and the snapshot loan.

## 2026-11-19, background plug-in catalogue service (N58, DF-W34)

**PR requested** — `engine-win/scanner-service` at `93d91ff0b057ccebeff6077eb55f0e960012a2e1`. Queue after the DF-E31 scanner family at `5c2e8423`. Based on main `12aab48a` through that family. Six files in the N58 change, with no Window-owned source edits.

**For Window #676.** On the existing application loader, call `startDiscovery(CacheOnly)` to hydrate on the worker, then `startDiscovery(Incremental)` for installed changes. Rescan uses `Refresh`. Retain the returned job ID, read `discoveryProgress()` from the owner and call `adoptDiscovery(id)` once Ready. Candidate totals appear after each format's enumeration. Read `catalogue()` for typed VST3 `PluginDescription::isInstrument` and typed CLAP references. Display the returned failure and cancellation state. A settings change invalidates its older job. Cache hits can still instantiate while discovery runs. Defer a project open that needs an unknown plug-in until the catalogue is adopted or the scan failure is visible to the user. The default worker count is half the reported hardware threads, bounded to one through four.

MSVC x64 `/W4 /WX`: all ten scanner/player suites passed and the Windows app linked. The held-worker fixture verified nonblocking owner progress, one discovery admission, hint suppression, typed VST3 instrument metadata, no synchronous project-open hydration after adoption, warm reuse, explicit Rescan, cancellation, stale settings and helper deployment refusal. No app UI run. Window owns startup and Rescan invocation, polling and project-open retry on #676.

## 2026-11-19, scanner discovery and registry family (DF-E31)

**PR requested** — `engine-win/scanner-dfe31` at `5c2e84233387057945bdfd7a4866f64c1324c53f`, on main `12aab48a`. One branch replaces #643, #644, #669 and #670. It includes 0f7289b2, 79db43b6, win's native tick encoding and the Windows canonical-bundle test fix. Owner admission and schema checks use immediate transactions.

An ordinary project miss can use one installed saved hint after read-only hydration. An exact catalogue hit still needs no helper or SQL reread, and explicit CacheOnly stays scan-free. Installed CLAP catalogue copies take precedence over deleted copies. A completed Incremental job binds roots before discovery, then verifies them in a strict maintenance walk before bounded cleanup of missing rows. It retains manual and pending blocks, excludes missing optional roots, and refuses a retargeted root. The extra warm traversal and read-only inventory were not timed here. Filesystem changes between operations remain possible.

The helper now marks entry to module opening without adding bytes to successful packets. Owned CLAP modules exiting 74 and 75 no longer stop a healthy sibling and their results are reused. Pre-open failures and parent I/O or cancellation retain the infrastructure policy.

MSVC x64 `/W4 /WX`: all ten scanner/player suites passed on the merge with current main, including 357 counted assertions, the owned VST3 case sequence and the real project-reopen checks. The Windows app linked. No desktop UI run or new cross-DAW benchmark. The Window's N58 adoption and Rescan wiring follow on the typed metadata service.

## 2026-11-19, saved source verification and analysis-cache trim (DF-E34 a/d)

**PR requested** — `engine-win/progressive-saved-source-mismatch` at `d4ecedb2ac066c2eb46ad6eef6ac9112276bf266`, exact #664 plus main `62ad7218` and both confirmed findings. The first fix verifies the current media when a warm cache entry disagrees with its saved source dimensions. An actual source mismatch stays silent before and after eviction. Same-shape verification is still limited by the existing warm-path checks. Trim continues through unreadable and undeletable entries, reports their paths, preserves cancellation, and leaves removable siblings eligible after inventory.

MSVC x64 `/W4 /WX`, 232 checks in the exact combined tree across decode, progressive decode and both analysis-cache suites. The saved-source and trim cases independently failed before the fix. The test host lacked optional symlink privilege and native AAC support.

## 2026-11-19, settings and bundle publication (DF-E33 a/b)

**PR requested** — `engine-win/settings-portable` at `a99448c4c532e7a2d73e0b25ce829875d69ae1b0`, on main `62ad7218`. This one settings family replaces #673 and #672. Whole-file settings publication revalidates the previous payload before a fallback rename, including a complete read and rejection of embedded NULs. An unsupported exclusive rename for a new settings file or new bundle first tries an exclusive hard link. Existing unsupported bundles report how to choose a new name. Ordinary project publication retains its lease requirement. The settings fallback still has the final OS rename's concurrency window.

MSVC x64 `/W4 /WX`, 533 checks passing in project files, project replacement, settings and collection/export. The two regression cases failed before the fixes. The unsupported filesystem responses were injected with real Windows file operations; no exFAT or network filesystem run. Two unrelated link fixtures skipped because the test token lacks symlink privilege.

## 2026-11-19 — Acceptance PR #642 updated

The existing acceptance PR now points to `68c92bbcb60c1c0da9b3267a0c8d616be3aefd8a` on `engine-win/acceptance-ci`, with current main `62ad7218` merged in. This removes the cross-DAW ranking helper and its sole self-test, describes the retained per-app statistics and refreshes the three affected asset hashes. Win's workflow bytes match the clean merge. [PR #642](https://github.com/arieladi/Adi/pull/642)

The changed Python code passed the generated-asset gate, 14 performance unit tests, 16 schema fixtures, eight statistics self-checks and all four repository validators. The existing numerical functions and paired examples matched the baseline. The update's own files are Python, Markdown and the manifest.

## 2026-11-19 — ADR-0248 queue checkpoint

The confirmed settings and cache findings now lead the Windows queue, followed by the scanner family and N58. The next landing requests will point to validator-first family tips against current main. The existing review tips and measurements below remain available as evidence. [ADR-0248](https://github.com/arieladi/Adi/blob/main/adi_daw/docs/DECISIONS.md)

The prior three director items have answers. New VST3 records carry the exact class ID alongside readable legacy identifiers. Take lanes retire the legacy comp-target operation. Network projects use one host at a time under an exclusive lock. The Windows worker setting follows the 1,024-sample boundary ruled for DEV-23.

The PD-07 naming step passed its in-flight native gate with 257 checks, including concurrent requests and same-name collision handling. Its staged source is held behind the confirmed findings. The snapshot worker remains the earlier review branch.

## 2026-11-19 — TM-MD-05 FFmpeg PCM stream

**PR requested** `engine-win/ffmpeg-pcm-stream` at `e0db001a641d98e043b93dc6dd8b9225525b9c16`. This combines bounded process streaming and the current media cache/FFprobe tip. The PCM adapter maps the selected stream explicitly and delivers complete interleaved float frames. The caller sets a frame budget and owns staged output. Empty or partial output, a child error, cancellation and a refused sink have typed failure results. Tool settings and actual cache import admission are still separate.

Strict native x64 MSVC Debug passed 551 checks across seven suites. The owned child gate checked split float words, frame alignment, the exact byte budget, late failure and sink errors. Nine real FFmpeg 9.0.1 cases passed. WMA, WV, CAF, MKV, WebM, AVI and TS matched direct FFmpeg frame counts and peaks. A two-tone MKV selected the intended first audio stream and refused a budget one frame short. Symlink and unavailable native AAC progress checks skipped. [FFmpeg command](https://ffmpeg.org/ffmpeg.html) · [FFmpeg protocols](https://ffmpeg.org/ffmpeg-protocols.html)

## 2026-11-19 — TM-MD-05 bounded binary process streaming

**PR requested** `engine-win/streamed-process-output` at `228adcb72b11a4bfd467d61522b9ec1de8e7f0e7`. The shared process runner gains a synchronous binary sink with a caller-supplied total-byte cap. It detects sink refusals and exceptions, keeps delivered bytes provisional until a clean child exit, and terminates and waits for the owned Windows process during cleanup. The scanner retains its existing buffered protocol, limits and deadline ordering.

The exact process source and existing owned-child suite compiled under strict native x64 MSVC Debug. All 47 checks passed, including binary output over 8 MiB, cap exhaustion, a deadline, cancellation in the sink, standard and nonstandard exceptions, failing children and descendants, and Windows handle balance. Other platform execution and PCM/cache integration remain separate.

## 2026-11-19 — PD-13 closed-project salvage

**PR requested** `engine-win/project-salvage` at `267d716a6b3a32cfa86748f7e976240de7a84d8b`. The tested code is `6d216aa1` and the follow-up changes the command documentation only. This stacks on SQLiteCpp 3.4.0 and its SQLite 3.53.4 runtime. Win can review the dependency fix independently first.

`adi_tool salvage <in.adi> <out.adi>` reads a closed ADI file with a readable schema, recovers to a private stage, verifies the result and publishes an exclusive copy. Exit 1 carries findings on a published readable copy. The command refuses WAL-format input, sidecars on either path, unreadable schemas and pre-existing outputs. Media references keep their stored paths.

The Windows regression found the upstream recovery writer sending `ops` rows to the implicit sequence table when its private connection inherited the defensive default. The hash-guarded adapter changes that connection only. The ordinary runtime retains defensive mode. Strict native x64 MSVC Debug passed 851 checks across eight suites. In the damaged-leaf fixture, 15 of 128 rows belonged to the damaged page, and all 113 intact rows survived with exact storage types and values. Two optional symlink fixtures skipped. [SQLite recovery](https://sqlite.org/recovery.html)

## 2026-11-19 — Shared media cache and FFprobe together

**PR requested** `engine-win/media-cache-probe-integration` at `415631ea6b94ffafc0a296a1de5c976bc6c33715`. This combines the reconciled media cache `6ad07db6` and FFprobe stream selection `16e4159e` with both decode regression groups preserved. Use this tip if reviewing both media branches together.

Strict native MSVC x64 Debug passed 493 checks across six suites, and the nine actual FFprobe cases passed again on the combined binary. The analysis symlink and native AAC progress cases skipped. The preceding cache tip has the exact desktop link gate. The combined tip was tested in headless core.

## 2026-11-19 — PD-07 snapshot backup worker

**PR requested** `engine-win/project-snapshot-backup` at `3b550a9884a61725e7965c3ca1ac18589a0d4bfd`, stacked on lock-neutral copy `b73f36da`. A worker opens an independent read-only connection and writes a consistent copy through existing exclusive publication and history-wide media rebasing. The copy records the snapshot's UTC time and exact source filename, retains the project UUID, and starts without the live writer's lock. An ordinary copy made from the backup clears its backup metadata. The worker takes the committed revision it reads and its caller chooses the destination filename.

Strict native MSVC x64 Debug passed 236 checks across four project/publication suites. The regression covered a live WAL source, same-name refusal after a backward clock change, and reopened relative media. Two optional symlink fixtures skipped. The second Snapshot trigger, a unique name across repeated hours, and ten-copy Recycle Bin rotation remain open.

ADR-0213 d4 already answers Q-A2. The director's research measured the Backup folder, second-resolution names, ten retained copies and no new copy on an unchanged Ctrl+S in Live 12.4 build `2026-04-24_d85a94ab5e`. The retained listing could establish when entries left Backup. Their arrival in the Recycle Bin was not measured.

## 2026-11-19 — Current main and the shared Media Cache Folder

**PR requested** `engine-win/media-cache-root-current` at `6ad07db6ecb947a5a0d89053820939a64987a69e`. This is the replacement review tip for `0f4ea92e` after [PR #661](https://github.com/arieladi/Adi/pull/661) advanced main to `f256e386`. The resolved cache implementation keeps main's verified waveform-analysis trimming and the single configured parent for the decoded and analysed caches.

The exact merged tree passed 477 strict native Windows x64 MSVC Debug checks across six core suites and compiled and linked the Windows desktop app. Platform AAC progress and interactive Settings were outside these gates.

## 2026-11-19 — PD-05 session lock ownership in project copies

**PR requested** `engine-win/project-copy-lock-neutral` at `b73f36da2290388e9da090939bc9c030ecbdc01d`. Save a Copy, Save As and the shared SQLite snapshot path clear the advisory writer record while the supported-major copy is still private. The live source keeps its writer record. A newer-major verbatim copy keeps its own format data.

The base reproduced two inherited-lock failures in Save a Copy and Save As. Strict native Windows x64 MSVC Debug passed 227 checks across project files, replacement, bootstrap and collection after the fix. Two optional symlink fixtures skipped.

## 2026-11-19 — TM-MD-05 bounded FFprobe stream selection

**PR requested** `engine-win/ffprobe-bounded-audio` at `16e4159e8535f9aaec0bf63cd75a0fcdc85085ef`. The worker probe calls a chosen executable through the bounded process runner and validates just the first audio stream's absolute index, channel count and sample rate. Input uses the local-file protocol and a media demuxer allowlist. Playlist and concat inputs stay outside this path. PCM decoding, configuration and cache integration have separate gates. [FFprobe options](https://ffmpeg.org/ffprobe.html)

Strict native Windows x64 MSVC Debug passed 193 checks across decoding, PCM and progressive reads. A separate test using FFprobe 9.0.1 passed nine owned-input cases. It confirmed WMA, WV, CAF, MKV, WebM, AVI, TS, video-only refusal, and concat refusal. The MKV selected audio index 1 with a video stream and two audio streams. Native AAC remained unavailable.

## 2026-11-19 — DEV-25 Plug-In source Settings

**PR requested** `engine-win/scanner-settings-roots` at `c800d747c3e9a54d9b3f1927b02eb9a2f78e4f36`, stacked on scanner Refresh `8adc7a92`. The desktop applies existing VST3 and CLAP lists independently, at launch and on Settings changes. The VST3-only system-folder switch leaves CLAP defaults intact. An admitted scan keeps its captured roots. Applying settings alone leaves the catalogue and instances available and launches no helper. This is now the scanner review tip.

Strict native Windows x64 MSVC Debug built and linked the desktop and player. The real VST3 scan gate passed 14 checks. The player passed 25 new source-setting checks and its existing discovery and project-reopen cases. The overlapping Settings safety source merged cleanly in the read-only check. Startup scheduling, Rescan/Clean controls and a desktop interaction test remain open.

## 2026-11-19 — PD-05 WAL evidence at project open

**PR requested** `engine-win/project-wal-observation` at `64c591169b8cbea8464dbddc5b328ed968bf65dd`. An optional Store open report records the file size of a WAL sidecar observed before opening SQLite. `adi_tool info` uses this receipt. It reports the possible live-writer and unclean-close causes together and leaves crash classification to the planned session marker.

Strict native Windows x64 MSVC Debug passed 602 checks across five suites. The real CLI also witnessed a 4,152-byte WAL and its newly committed project name while a writer was open. The closed and cleared cases emitted no sidecar notice.

## 2026-11-19 — PD-13 read-only project integrity check

**PR requested** `engine-win/project-quick-check` at `288d6a9e1ff0f9f4a0f4a5a289d50de3aa44da6c`. The recovery flow can ask SQLite for one page-level diagnostic over a separate read-only connection using the same finding format as the full checker. The path accepts Unicode and refuses empty or embedded-NUL input. SQLite's WAL read semantics still apply.

A fresh strict native Windows x64 MSVC Debug build passed 612 checks across five suites. The regression covers a live WAL reader, a closed project, an actually damaged ops page, unchanged source bytes and an absent input. Scheduling after an unclean launch needs PD-05's crash evidence. The salvage command has a separate gate. [SQLite quick check](https://sqlite.org/pragma.html#pragma_quick_check)

## 2026-11-19 — Current scanner timing attempt, partial

- **MEASURED.** The exact Refresh player and helper at `8adc7a92` built under strict native MSVC x64 Debug. The helper self-test and the owned VST3 positive control passed. Four accepted invocations in one paired repeat returned the same 96 ordered classes. Both warm arms made zero observed CLAP initializations and zero VST3 factory calls.
- **NOT RUN.** The planned complete five-pair comparison. The next cold observation encountered a concurrent build and was excluded. No new confidence interval, worker-count ranking or comparison with the earlier branch is supported. The sealed binaries, input hashes and partial receipts are in the director's research.

## 2026-11-19 — Review order for the Windows engine seat

- First, review `engine-win/sqlitecpp-wal-reset-fix` (`8a8f9a00`) independently. This changes the SQLite version actually linked by ADI. Run ADR-0024's full ABI matrix.
- Take `engine-win/scanner-settings-roots` (`c800d747`) as the scanner review tip. It includes format-specific Settings sources, infrastructure-failure handling, validated read-only warm hits, helper self-test and forced Refresh. The cross-machine class identity question is above.
- Review `engine-win/publication-safety-integration` (`20f27d66`) as one stack for export, presets, archive and collected-media publication. `engine-win/archive-document-control` (`dc3a8efc`) adds the cancellable worker entry for Window.
- Review `engine-win/settings-safety-integration` (`31322dab`) for Settings recovery and the desktop save lifecycle, `engine-win/project-removable-sync` (`8c51acaa`) for project connection policy, and `engine-win/media-cache-root` (`0f4ea92e`) for the cache settings stack. `engine-win/wav-bwf-v2` (`a657d93b`) is ready as a writer prerequisite for EX-08.
- Git's three-way merge check against main `e8b67c77` was clean for each listed tip individually. The entries below carry the separate native gates. Review the containing tip first where two entries describe the same stack. [Acceptance PR #642](https://github.com/arieladi/Adi/pull/642) is already open.

## 2026-11-19 — PR requested — shared Media Cache Folder

- Branch `engine-win/media-cache-root`, head `0f4ea92e6e076f81a1b0e41fe349d20089904d42`, stacked on `engine-win/decode-cache-missing-publication` at `df62deae`. Review the stack as one change if the base has yet to land.
- The Settings location now governs both decoded audio and waveform-analysis caches. Both use owned child directories under a single absolute parent fixed on first use, with independent budgets. A changed path takes effect next launch and existing entries stay at their existing location. Physical directories are created lazily.
- Native Windows x64 MSVC Debug `/W4 /WX` passed 461 checks across Settings, analysis cache, decode, decode consistency, PCM access, and progressive readers. Native AAC was unavailable. The exact seven changed files and matching base also compiled and linked the JUCE desktop app. The Settings window was not run.

## 2026-11-19 — Combined export and project publication

**PR requested** `engine-win/publication-safety-integration` at `20f27d66b66647e3973148c2b1d0aacca8431901`. The exact tested parents are export/presets `0b6ddcce` and collection/desktop `9b330750`. Both native identity helpers stay in the shared publisher API. Native rename and verified-copy directory-sync reporting, replacement identity binding and identity-aware rollback retain their tested contracts. Use this tested tip to review the two publication stacks together.

Strict native MSVC x64 Debug `/W4 /WX` passed 1,153 checks across nine suites. Four symlink fixture groups skipped. The NI helper, naming and metadata gates ran, and the platform codec gate still refused the full encoded NI path. This combined tree was tested in headless core. Read-only three-way against main `796fe498` showed no conflict.

## 2026-11-19 — Cancellable collection through the document worker

**PR requested** `engine-win/archive-document-control` at `dc3a8efc1f1ef035411211873cb2f139d7e0a1ff`, stacked on `2e008d7c`. The document worker can now accept ArchiveControl and return typed cancellation, warnings and success. The existing one-argument application entry delegates to the same path and keeps its result type. The worker uses the prepared paths and can finish after the initiating document closes.

Native MSVC x64 Debug `/W4 /WX` passed 471 checks across the collection and document suites. A real prepared document cancels mid-copy and keeps its source and staging state intact. Two optional symlink fixtures skipped. Window progress and cancel controls can use the new worker entry.

## 2026-11-19 — Combined project collection and archive safety

**PR requested** `engine-win/collection-safety-integration` at `9b3307506a7aace483332c46cd6a69563c9e94fd`. This combines the exact CLI and collected-media tip `2e008d7c` with the desktop warning tip `d5a50adb`. It brings the external-copy and ZIP cancellation, owned rollback, directory-sync and checkpoint reporting and real CLI message together with the desktop warning relay.

The full topic tree built and linked the Windows JUCE app under strict MSVC x64 Debug `/W4 /WX`. The only change from `2e008d7c` is the tested desktop source. The core, archive, project and CLI sources are identical to the preceding 323-check gate. No interactive app case ran.

## 2026-11-19 — Explicit scanner refresh for eligible candidates

**PR requested** `engine-win/scanner-refresh` at `8adc7a924939a26f27a30f62e69f4d332eb673df`, stacked on the reconciled scanner `fe7dda00`. An explicit Refresh bypasses unchanged success packets and deterministic rejected negatives. It retains normal durable admission, manual/pending blocks, two-strike quarantine and owner tokens. Cancellation or infrastructure abandonment keeps the previous same-stamp record. Incremental remains the default, and CacheOnly needs no helper. The explicit diagnostic is `adi_play --list --refresh`. Root reconciliation, startup scheduling and Settings wiring remain open.

Seven core suites passed 337 checks under native MSVC x64 Debug `/W4 /WX`. The owned VST3 scanner passed 14 checks. Actual player/CLAP Refresh, negative retry, catalogue retention and both hosted Session reopen gates ran. Three native symlink cases skipped and the VST3 check used the original-path fallback.

## 2026-11-19 — Committed legacy extraction with pending file closure

**PR requested** `engine-win/extract-cli-warning` at `2e008d7c6926cb5fa77b4d998d201c5e8fd1be15`, stacked on collected-media warnings `37e3ce5e`. The legacy extraction CLI keeps its nonzero status for a database that cannot close as one file. It now tells the operator the extraction committed and preserves any separate media-publication warnings. The native regression leaves concurrent connections open, verifies a committed external file and a reader's retained snapshot, and checks the real CLI message.

Strict native MSVC x64 Debug `/W4 /WX` passed 323 checks across collection, ZIP and project files. The baseline failed the new message check. Three optional symlink cases skipped for Windows privilege.

## 2026-11-19 — Collect and Export status warning relay

**PR requested** `engine-win/archive-window-warning` at `d5a50adb5c1dfd650327ad85ba0210c528e5cb41`, stacked on `99b03094`. The normal desktop export completion reports the archived filename or failure and includes the worker's existing warning. This keeps a successful published ZIP visible even when its directory sync was unconfirmed. A failed result can also carry a retained-file warning.

The exact archive topic sources built and linked the Windows JUCE application under strict MSVC x64 Debug `/W4 /WX`. Collection, ZIP and project-file paths have the preceding native 286-check base gate. No interactive warning case ran.

## 2026-11-19 — PD-02 and PD-04 collected-media durability receipts

**PR requested** `engine-win/collect-publication-warnings` at `37e3ce5ead7a40fd8b3564c225af78e9856279ba`, stacked on `99b03094`. Live Collect All and Save and legacy extraction retain path-qualified directory-sync warnings after successful publication. Both rename and verified-copy fallback keep file identity for rollback. The collection result can carry both media warnings and a partial WAL checkpoint warning. A private ZIP input's directory-sync advisory does not become a warning about the independently checked and published archive.

Strict native MSVC x64 Debug `/W4 /WX` passed 335 checks across three suites, including 34 new assertions. Real rename/fallback and pinned-reader cases passed. Three optional symlink fixtures skipped for Windows privilege. Public Store signatures remain compatible.

## 2026-11-19 — Missing decode-cache publications

**PR requested** `engine-win/decode-cache-missing-publication` at `df62deaef6ed928ac83d868c365c475e5855d007`, stacked on Cache Folder `b89e9a5b`. A failed cache file size read now refuses before incrementing bytes, stats or pins on publication or on a warm hit. The deterministic regression removes a completed owned target before accounting. It reproduces the false success, phantom size and extra pin on the base, then verifies clean refusal and an ordinary retry.

Five native MSVC x64 Debug `/W4 /WX` suites passed 416 checks after the change. The focused regression was red at three assertions before it. Native AAC progress still followed its unavailable gate.

## 2026-11-19 — Scanner stack reconciled with the current main probe

**PR requested** `engine-win/scanner-current-main` at `fe7dda00fb3ee4a4ba887e72c3ef2cab6d8bc9d6`. This review candidate brings the integrated scanner `926f6078` onto main `796fe498` in the topic only. The single conflicted owned VST3 fixture retains both independent readers, for signed transport and for completed scanner `process` calls. The PE export receipt confirms both.

Strict native MSVC x64 Debug `/W4 /WX` passed 997 checks in eleven suites and 211 direct VST3 fixture checks, with 16 separate editor subchecks. Actual fixture realtime and offline signed transport passed. Three cache symlink cases skipped for privilege and the VST3 scanner used the original-path fallback. This is the combined review tip against that main revision.

## 2026-11-19 — PD-02 partial WAL checkpoint warning

**PR requested** `engine-win/collect-checkpoint-warning` at `99b0309436757f4b7c04bad348c97db87c4d6358`, stacked on archive media streaming `85fe86e7`. Collection inspects both PASSIVE frame counts. A pinned SQLite reader can leave frames pending with SQLITE_OK, so the operation retains committed success and reports the pending checkpoint. The regression verifies the collected file, the older snapshot and warning removal after the reader ends.

Native MSVC x64 Debug `/W4 /WX` reproduced one missing-warning failure, then passed 286 checks across collection, ZIP and project files. Three optional symlink checks skipped.

## 2026-11-19 — EX-08 Broadcast Wave v2 writer prerequisite

**PR requested** `engine-win/wav-bwf-v2` at `a657d93b9bf0b226674e8d52f717279cc49b21fe`, based on main `796fe498`. Optional BWF v2 writes supplied originator, time reference, date/time, UMID, coding history and five loudness fields. It validates before opening the output, encodes signed centi-units with the unknown sentinel, and preserves the old v0 layout when the option is absent. [EBU Tech 3285](https://tech.ebu.ch/docs/tech/tech3285.pdf)

The actual writer and WAV suite compiled in strict native MSVC x64 Debug and passed 221 checks, including byte-level assertions and refused-input preservation. The optional physical file over 4 GiB and whole app were not run. Job metadata and checked-output field patching still need EX-05's statistics and stage contract.

## 2026-11-19 — Cache Folder for decoded audio

**PR requested** `engine-win/decode-cache-folder` at `b89e9a5b4ec91330715f2d51fd6ca4f1c2962a54`, stacked on cache limits `4af5e656`. The desktop uses the `decoded` child of the absolute configured Cache Folder. An empty value selects the app cache. Only a decode that needs it creates the folder. Changing location takes effect on relaunch to keep pinned PCM readers on their existing paths. The tests use a Unicode path and prove trim leaves a neighboring file alone. Analysis has a separate cache and this entry covers decoded audio.

Five strict native MSVC x64 Debug `/W4 /WX` suites passed 413 checks. The matching four settings, decoder and application sources linked in the JUCE desktop target. Native AAC progress still followed its unavailable gate. The UI was compile/link checked.

## 2026-11-19 — PD-02 cancellation while copying archive media

**PR requested** `engine-win/collect-media-stream` at `85fe86e7c07847a3829ee5f3d909f21e844fb0c1`, stacked on ZIP streaming `734f7b6f`. External media copied into an archive's private snapshot can stop between chunks of at most 64 KiB and reports per-file source bytes. Ordinary Collect All and Save keeps its existing path. The closed stage is validated before publication; cancellation uses the existing identity-aware rollback and carries its warnings to the worker result. Tests cover an earlier completed copy, callback failure, a cancel just before SQL commit and unchanged source bytes.

Native MSVC x64 Debug `/W4 /WX` passed 466 checks across four suites. Three optional symlink cases and the multi-gigabyte ZIP exercise were not run. SQLite backup, legacy extraction and individual hash reads still use phase-level cancellation.

## 2026-11-19 — PD-12 Windows removable-storage sync

**PR requested** `engine-win/project-removable-sync` at `8c51acaacba1425a0198719f3a31e34af504f66d`, stacked on Defensive mode `7db7e05a`. Writable project Stores resolve their actual volume and select FULL when Windows classifies it as DRIVE_REMOVABLE. Fixed storage retains NORMAL. The create and reopen tests verify the actual connection settings on the attached fixed volume. Physical removable and network volumes were unavailable in this run. Windows can classify an external drive as fixed.

Native MSVC x64 Release `/O2 /W4 /WX` passed 760 product checks and the WAL benchmark self-test. Two optional symlink cases skipped. The PD-12 network case needs the decision above. [Windows mount lookup](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumepathnamew) · [Drive types](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdrivetypew)

## 2026-11-19 — PD-11 combined settings safety and debounce

**PR requested** `engine-win/settings-safety-integration` at `31322dab548455d7414440490289c7a411dd5044`. Merges exact debounce `ead3d01b` and recovery `4f04868c` topic parents, with all four protected document states enforced before scheduling writes. It keeps strict reads, previous-good backup, atomic saves, migration, retry and quit flush. A real locked Windows file retains the scheduled edit and delays retry until the file can be read completely.

Native MSVC x64 Debug `/W4 /WX` passed 1,161 checks across six core suites, including 12 new integration checks. The JUCE desktop target linked using the exact combined settings, migration and application entry sources in a local verification build. UI behavior was not rerun interactively.

## 2026-11-19 — Scanner warm-path and helper integration

**PR requested** `engine-win/scanner-integration` at `926f6078b2d305ed21cdc5787b45b957fa157292`. Stacks on warm reads `ede21cae` and brings in the independently tested DF-E25 infrastructure handling `79db43b6` and helper self-test `7f8ed3f8`. Unchanged successful candidates stay read-only. Helper protocol, metadata-channel and other infrastructure errors abandon without candidate strikes. Explicit helper self-test reports the protocol and built formats.

Native MSVC x64 Debug `/W4 /WX` passed 309 checks across seven core suites and 30 checks across two JUCE suites. Actual CLAP and VST3 fixture paths ran. Native symlink checks used their privilege skips or original-path fallback. A read-only three-way against main `796fe498` conflicts in `tests/fixtures/vst3_expression_synth.cpp`; preserve both the cloud transport probe and the scanner's completed-process counters. No new scanner timing or startup default.

## 2026-11-19 — SQLite pin for the upstream WAL-reset fix

**PR requested** `engine-win/sqlitecpp-wal-reset-fix` at `8a8f9a00f703c3d30a224ff207e6827f6dc49522`, independent of other engine topics. One dependency moves under ADR-0024. SQLiteCpp MIT tag 3.3.3 `59a047b8d3fe8574406ed73ab9fac0474e87bd03` bundled SQLite 3.49.2. Verified release tag 3.4.0 `df0a28ca9733f687ab7ce77f070c0df84368a45e` bundles 3.53.4, which includes SQLite's upstream fix for the rare WAL-reset corruption bug. The provenance job still prints the fetched pin and amalgamation version. [SQLite advisory](https://sqlite.org/wal.html#the_wal_reset_bug) · [SQLiteCpp release](https://github.com/SRombauts/SQLiteCpp/releases/tag/3.4.0)

A fresh strict MSVC x64 Debug `/W4 /WX` build used that exact release and `adi_tool versions` reported linked SQLite 3.53.4. Eight targeted Store, migration, operations, project, collection and registry suites passed 786 checks. Two symlink cases skipped for Windows privilege. Request the full ABI matrix before landing, per ADR-0024. The existing PD-12 measurements describe the recorded SQLite 3.49.2 run.

## 2026-11-19 — PD-02 ZIP entry cancellation and progress

**PR requested** `engine-win/collect-zip-stream` at `734f7b6f6bd58f7f549ef28f0bfe2f473e6da683`, stacked on owned media rollback `a67c3344`. Collection now reports an entry's source bytes on the worker and can cancel between chunks of at most 64 KiB, including the final chunk before publication. Cancellation or a progress callback failure is sticky. The private archive is discarded and the source project and existing ZIPs keep their bytes. The real collection regression cancels 256 KiB into a 4 MiB media entry.

Four strict native MSVC x64 Debug `/W4 /WX` suites passed 443 checks. Three privilege-dependent symlink cases and the optional multi-gigabyte ZIP exercise were not run. Snapshot and media copying still stop at phase boundaries. Window progress remains a separate integration.

## 2026-11-19 — PD-12 defensive project connections

**PR requested** `engine-win/project-sqlite-defensive` at `7db7e05a581d89ca86b595d4d11adba9303b37af`, stacked on the PD-12 benchmark `a3f425f8`. Each project Store connection enables SQLite Defensive mode before its first SQL, including create, explicit read-only opens and the automatic read-only reopen for an unknown major. Regular operation transactions and migrations run normally. Direct writes to SQLite's schema tables fail even after requesting writable_schema.

Native MSVC x64 Release `/O2 /W4 /WX` reproduced five failures, then passed 758 checks across seven product suites and the WAL benchmark self-test. Two optional symlink cases skipped for privilege. This covers the Defensive mode step. Local and volume-specific durability defaults remain separate.

**For win.** The current pinned SQLiteCpp 3.3.3 bundles SQLite 3.49.2, which is in upstream's rare WAL-reset corruption range. SQLiteCpp's released 3.4.0 tag `df0a28ca` bundles SQLite 3.53.4, which includes the fix upstream shipped in 3.51.3. Preparing an isolated ADR-0024 pin update. The change needs the full ABI matrix before merging. [SQLite advisory](https://sqlite.org/wal.html#the_wal_reset_bug) · [SQLiteCpp 3.4.0](https://github.com/SRombauts/SQLiteCpp/releases/tag/3.4.0)

## 2026-11-19 — Scanner warm hits without registry writes

**PR requested** `engine-win/scanner-readonly-warm-hit` at `ede21caee28cad18ad13a16d759037df7a11919d`, stacked on the worker CLI branch `aa26f099`. DEV-25. A bounded, validated and unchanged success is read in two fresh statements without a pending ticket or write transaction. Blocked, busy, stale and invalid records use the normal admission/repair path. A manual or pending block installed while validating still wins. An unrelated WAL writer does not prevent a valid warm hit. Native tests count registry changes and exercise revalidation, late blockers, cancellation and mutations.

Strict native MSVC x64 Debug `/W4 /WX` passed 289 checks across six suites. Three native symlink cases skipped for Windows privilege. No new speed claim. Keep the separate infrastructure-abandonment request `79db43b6` when integrating the scanner chains.

## 2026-11-19 — PD-12 project WAL durability measurement

**PR requested** `engine-win/project-wal-benchmark` at `a3f425f8488798d4345bfa4331aef9e97d2a0e69`, based on main `796fe498`. Opt-in reproducible benchmark and BENCHMARKS evidence. Native x64 MSVC Release `/O2 /W4 /WX` passed the benchmark self-test and 50 project-file checks, with one optional Windows symlink skip.

**MEASURED** — On a local NTFS SSD, twelve paired repeats of real one-op owner-thread edits. Steady p50/p99 was 0.357/0.636 ms for NORMAL and 2.121/6.898 ms for FULL, 576 samples per mode. Near-full pre-checkpoint was 0.383/0.598 ms versus 2.168/2.953 ms, 108 per mode. The twelve completed threshold commits per mode had medians of 14.020 ms in NORMAL and 4.356 ms in FULL. FULL had three observations over the 16.67 ms reference. A pinned reader left 29 of 38 frames checkpointed with SQLITE_OK, then ended and let PASSIVE complete the rest. No production pragma changed. Keep the local default pending a representative project plus UI budget check. The separate removable/network policy, Apple checkpoint sync, and Defensive mode do not follow from these local timings.

## 2026-11-19 — Export publication and Job preset integration

**PR requested** `engine-win/export-preset-integration` at `0b6ddcce159be83265aa8a47cec5c06a5ff2e0a7`. EX-07 and EX-09. Its two parents are naming and verified delivery `fa9534a6` and user presets `ea37af92`. Integrates both subjects with the complete-byte JSON guard. Queue, WAV and NI retain the export branch's validation, collision protection and verified publication. Job JSON v2 stores explicit Fail, Increment or Replace; legacy v1 without a field reads Fail. The two preset stores retain their envelope versions. Bounds and Output carries the collision policy; Options and Format keeps the target's.

Eight combined strict MSVC x64 Debug `/W4 /WX` suites passed 917 checks. NI metadata and the final-name/publication helper ran; the installed platform AAC did not pass the existing availability gate for a complete encoded NI render. Two symlink cases skipped for Windows privilege. Read-only three-way merge against main at `796fe498` reported no conflict. This is the combined candidate once the constituent export and preset requests are accounted for.

## 2026-11-19 — Corrupt Settings recovery status

**PR requested** `engine-win/settings-recovery-status` at `4f04868c9a622e8c5b231e7b6d5fb6c5ea3ec129`, stacked on `engine-win/settings-json-full-bytes` at `c4d4f32c`. PD-11. If the malformed settings file cannot be moved to its recovery name, the instance reports Unavailable and stays read-only. It keeps the OS error and tells the user to reload. A fresh load after release saves the malformed bytes under the next recovery name and can then save valid settings.

A real Windows read-sharing handle blocked the rename while leaving the file readable. The targeted regression went from five failures to zero. Native MSVC x64 Debug `/W4 /WX` passed 462 checks across four settings suites. The stage uses the existing recovery rename and inherits its concurrent pathname limitations.

## 2026-11-19 — Desktop decode-cache capacity settings

**PR requested** `engine-win/decode-cache-runtime-limits` at `4af5e656bd3305da7acfda572f6d8b04e58023ba`, stacked on DF-E28 `60fa903d`. Apply the two validated decoded-cache limits at desktop startup and on settings change, including updates to the existing cache without replacing it or disturbing pinned readers. Settings alone and plain WAV opens leave the cache directory absent. Off-thread cache configuration leaves normal decode and explicit trimming in charge of disk work, and callback refusal happens before cache initialization.

Native MSVC x64 Debug `/W4 /WX` decode, cache consistency, PCM stream and progressive decode pass 224 checks across four suites. The matching changed sources compile and link the JUCE desktop target with the settings debounce and analysis-file changes in a local verification tree. Native AAC progress checks remain skipped on this host. Desktop UI dispatch was compile/link checked. The root remains the app cache's decoded folder and analysis has its own budget.

## 2026-11-19 — DF-E28 decoded cache consistency

**PR requested** — `engine-win/decode-cache-consistency`, `60fa903dee31aabb60b6a1dbda7e4164586c7f12`, based on main `1a46048d`. An estimated single-file decode beyond the maximum now refuses before eviction. Malformed usage hints are skipped while real cached PCM is retained and rediscovered. The complete published target is accounted before the final cancellation check, and a cancelled caller acquires no pin. Extreme valid stamps saturate safely. The new native suite reproduced 24 failures before the changes. Strict x64 MSVC passed 218 checks across four suites after the fix. Native AAC runtime checks followed the unavailable codec gate.

## 2026-11-19 — DEV-25 Windows scanner timing

**MEASURED** — Ten cold and ten warm ADI Debug invocations, five paired runs per state, using the exact worker CLI source `aa26f099` with 24 owned CLAP and 24 owned VST3 modules. All returned the same ordered 96-class catalogue. Cold median changed from 2,409.1 to 1,086.6 ms with four workers, paired difference −1,371.1 ms [−1,490.9, −1,251.2]. Warm median changed from 616.6 to 725.6 ms, paired difference +122.9 ms [+68.0, +177.8]. Every cold CLAP candidate initialized once and warm ones did not initialize. In separate addition cases, the same prewarmed registries each produced one native VST3 factory call for the sole new module and kept all 72 prior VST3 classes. No audio stream was opened. Raw data stays private.

Four workers are a candidate for cold batches. Keep serial warm discovery until the cache-hit path and production startup have their own checks. The timings include player startup and metadata output, and describe this fixture.

## 2026-11-19 — EX-09 application export presets

**PR requested** — `engine-win/user-render-presets`, `ea37af92aaa3ebe13b0c220f0fc378ac41328c14`, stacked on `engine-win/project-saved-render-jobs`. A fixed file in ADI's application data holds a bounded versioned list of names and strict export values. Names remain data. Atomic staged replacement verifies existing content under the lease and preserves damaged, unsupported or unreadable bytes. Save, mask application and rendering are separate operations. Strict Windows x64 MSVC passed 286 checks across four suites using owned files.

## 2026-11-19 — Integration notes for main at 796fe498

The scanner worker/CLI, Windows shortcuts and self-test branches have no content conflicts against one another. The scanner stack and main overlap in two VST3 fixture hunks. Keep both transport observations and the scanner's cumulative processing callbacks. The protected settings read stack and debounce conflict in one `AppSettings::write` hunk. Keep all four load guards. EX-07 and EX-09 overlap at the public job validator and WAV and NI publication. Keep the EX-07 publication checks and expose the shared validator. Extend codec and masks with `Job::collision` when combining them so round trips preserve the output policy. These were read-only merge checks against the specified main head.

## 2026-11-19 — DEV-25 command-line worker selection

**PR requested** — `engine-win/scanner-worker-cli`, `aa26f09989fe4e425d412ce483379b80663e4d25`, stacked on `engine-win/scanner-parallel-probes`. The scoped `adi_play --list --search-only DIR --scan-workers N` command accepts 1 through 4, rejects use with CacheOnly or project playback, and prints the requested count. The command lists validated metadata through existing discovery. The strict native Windows player build and its integration suite passed. GUI/default-worker policy remains its own slice.

## 2026-11-19 — EX-07 output format, sample rate and index tokens

**PR requested** — `engine-win/export-format-name-tokens`, `fa9534a672f6c5926387b4a46c82af08774335ae`, stacked on `engine-win/export-identity-replace`. `{format}` uses the delivery container label, `{rate}` uses integer output Hz, and `{index}` uses the stable enqueue/delivery ordinal shared with `{counter}`. The run manifest carries the resolved context. NI expands its final name at prepared Main using the NI profile before audio and includes the full compound suffix in its byte budget. Strict x64 MSVC passed 549 checks, including actual WAV and the prepared NI route. Full AAC encoding remains gated. Date/time tokens still need a shared run clock and timezone policy.

## 2026-11-19 — DEV-25 native Windows shortcuts

**PR requested** — `engine-win/scanner-windows-shortcuts`, `cdb8f6846097ca1b0b059d5e698a57c3086fe1bd`, stacked on `engine-win/vst3-discovery-aliases`. VST3 and CLAP discovery can read bounded native Shell Link bytes, follow stored absolute file/folder targets and deduplicate their canonical paths. Chains have a 32-hop cap and cycles stop. The owner reads link data without shell execution or target recovery/search. Direct stored UNC paths retain ordinary filesystem behaviour. Strict x64 MSVC passed 176 checks over four suites, including 18 native shortcut checks and an actual junction cycle. The optional directory-symlink case needed privilege unavailable during this run. Finder aliases stay open.

## 2026-11-19 — EX-09 project-owned saved export jobs

**PR requested** — `engine-win/project-saved-render-jobs`, `0756e2be06e7a71b90816ecb52a74ba341a4322c`, stacked on `engine-win/render-preset-groups`. Stores an ordered validated list in the existing Layer 3 `session_state` outside Undo. Save and load are separate from enqueueing and rendering. SQLite validation and replacement share an immediate transaction. Corrupt or newer existing bytes stay intact. Load from a copied or Save As project rebinds only the owning project path and keeps project-local source IDs. The caller opens the owning Store by an absolute path. Strict x64 MSVC passed 274 checks, including 84 new persistence, rollback, reopen and malformed-byte checks. User and project presets, queue UI and RPC are separate.

## 2026-11-19 — PD-11 complete settings document bytes

**PR requested** — `engine-win/settings-json-full-bytes`, `c4d4f32c5505e054e18ab22db4bf902347cca7b3`, stacked on `engine-win/settings-read-failures`. The shared parse boundary refuses literal NUL bytes. Startup retains the complete malformed bytes through the existing corrupt-copy path. A bad save reread leaves destination and backup untouched. Both failures reproduced before the fix. Strict x64 MSVC passed settings, migrations and the 29-check read-safety suite.

## 2026-11-19 — DEV-25 dedicated helper self-test protocol

**PR requested** — `engine-win/scanner-helper-self-test`, `7f8ed3f8bfc46bac2e92f11c338a13ee57651578`, stacked on `engine-win/vst3-discovery-aliases`. The dedicated helper accepts exact `--self-test` and emits a versioned structural receipt with its metadata schema and compiled formats. The command reaches no plug-in factory or registry. Strict Windows x64 MSVC passed 133 checks each with JUCE disabled and enabled. Actual CLAP initialization remained once over three ordinary unchanged probes, and a separate controlled case counted one ordinary child. Parent activation remains a separate slice, so the existing warm scan launch count is preserved.

## 2026-11-19 — EX-09 complete JSON byte validation

**PR requested** — `engine-win/render-job-json-full-bytes`, `cd265a59d80913dbcf2e6b7cd663ae25d6468fda`, stacked on `engine-win/render-job-json`. The pinned JSON lexer treated a literal NUL after an otherwise valid job as end-of-input. Two native tests reproduced accepting the prefix, with and without trailing content. The job decoder now requires the complete byte stream and refuses literal NUL before parsing. Strict x64 MSVC passed 94 codec checks and 211 queue checks.

## 2026-11-19 — DEV-25 bounded parallel discovery

**PR requested** — `engine-win/scanner-parallel-probes`, `95a5288b25b49590b62ab5e1acd256deaf58d5eb`, stacked on `engine-win/scanner-registry-write-admission`. Explicit discovery can request one through four workers, each with an independent SQLite connection and the same identified helper. The owner consumes results in input order and publishes only a complete catalogue. A fatal result stops new submissions. Active probes keep their bounded timeout and observe the caller's cancellation flag. The serial default stays in place.

**MEASURED** — Strict Windows x64 MSVC passed six suites. Four controlled helper processes had to be alive together before any could reply, and their second pass reused all four cache rows with no child launches. Dedicated real module runs with K=1 and K=4 produced identical ordered catalogues from four CLAP and four VST3 fixtures. Warm scans kept the catalogue and the owner loaded no CLAP library. The worker-count default, Clean policy and startup/UI integration still need their own work.

## 2026-11-19 — DEV-25 registry admission with two writers

**PR requested** — `engine-win/scanner-registry-write-admission`, `0f7289b2d5bfe7375bb618bf2ee19e3ff9bb672d`, stacked on `engine-win/vst3-discovery-aliases`. Two native SQLite connections reproduced failures in stamped admission and typed completion while a competing writer held a short transaction. Beginning these read/write transactions with the existing busy timeout and an immediate lease fixes the snapshot upgrade race. Child processes still start after admission commits. Strict Windows x64 MSVC passed the 74 cache checks, the scanner suite and all 15 native VST3 sequence cases. This supplies the separate-connection prerequisite for parallel scan probes.

## 2026-11-19 — EX-07 identity-bound replacement and Windows AAC gate

**PR requested** — `engine-win/export-identity-replace`, `41b74d57f2e48e9c9fc25d1e8466d37fbda5b04c`, stacked on `engine-win/export-folded-collisions`. Explicit Replace captures the requested regular file's native identity, BLAKE3 and folded collision set at preflight. Publication checks the same binding with the replacement lease. If the target was absent at preflight the output still goes through atomic no-clobber. NI binds the final name before the Main render begins. Strict native x64 MSVC passed 515 checks covering project replacement, actual WAV, pre-audio preparation ordering and the NI shared publisher. The existing file APIs leave an external writer a last-check-to-rename race. Whole-group reservation, confirmation wiring and complete native NI encoding remain open. Integrate the separate settings branch's callable replacement validation and EX-09's shared validator when combining branches.

**MEASURED** — The native AAC gate has a specific reproduction. For an ADI-generated 4,096-frame stereo WAV at 48 kHz and 256 kbit/s, Media Foundation emitted four 1,024-tick packets. Both media/movie headers reported 4,095 ticks at 48 kHz, and the track omitted the edit list. The existing transport check refused the one-sample inconsistency before alignment verification. Explicit priming is still required even if the duration mismatch is resolved. Raw owned evidence stays private.

## 2026-11-19 — EX-09 export preset groups

**PR requested** — `engine-win/render-preset-groups`, `63d8cdffd8663995dadf337939683cf6b9b1d562`, stacked on `engine-win/render-job-json`. Bounds and Output, Options and Format, or All build and validate a candidate before assigning it. Options and Format preserves the target destination, source and bounds. Project and runtime bindings remain target-owned. This slice requires matching project spelling for project-local source transfer and refuses ambiguous partial clock/layout transitions. Native strict x64 MSVC passed 351 checks. Preset persistence, source relocation and control API remain separate.

## 2026-11-19 — PD-11 settings read failure safety

**PR requested** — `engine-win/settings-read-failures`, `18d278d2d18476891e8fab7feb244ca082e6395e`, stacked on `engine-win/settings-step-migrations`. A native read-denied handle reproduced moving healthy preferences aside. A locked tail independently reproduced a valid prefix being read as though it were the complete document. The shared native reader now distinguishes EOF from read errors. Unavailable load status preserves the path and refuses edits or saves until a fresh successful load. A failed save on an already loaded store retains pending edits for retry. Strict x64 MSVC passed 449 checks across four suites. Corrupt-file move-aside races are a separate slice.

## 2026-11-19 — TM-MD-04 Create Analysis Files

**PR requested** — `engine-win/analysis-files-switch`, `b54f4c6a99b094d2df5f44a44ade9de6ba7946fe`, stacked on `engine-win/analysis-cache-budget`. File & Folder exposes Create Analysis Files, on by default to preserve ADI's existing behaviour. Off keeps drawing and reusing existing analysis while new waveform requests skip writing derived analysis files. Each worker takes the current setting when it begins a request. Strict native x64 MSVC passed the core settings, peaks and cache suites and linked the desktop integration against the debounced-settings branch. The analysis scheduler, combined quota, metadata extraction and persistent sample defaults are separate work.

## 2026-11-19 — DEV-25 common VST3 and CLAP native search

**PR requested** — `engine-win/vst3-discovery-aliases`, `1fc06d661c61024a1d79db20022e8ffc20e6a599`, stacked on `engine-win/clap-discovery-aliases`. VST3 folder discovery shares canonical, cycle-safe module enumeration with CLAP. Root scans return one sorted canonical path per resolved module and stop at bundle boundaries. The dedicated child still owns all factory probing. Explicit VST3 hints keep their existing identity spelling and the loader's compatibility projection remains available. Strict x64 MSVC passed five suites, including both formats' native junction/cycle checks, 15 native VST3 sequence cases, the actual two-format discovery operation, and saved Session reopen. Windows shell shortcuts and Finder aliases remain open.

## 2026-11-19 — PD-11 settings persistence scheduling

**PR requested** — `engine-win/settings-debounce`, `ead3d01b3640688fb3785aaae7312112be64606f`, stacked on `engine-win/settings-last-good`. The app applies Preferences, command, device/channel and import-prompt changes immediately. The owner timer saves after 500 ms of quiet. A failed save retains pending edits and retries after 1, 2, 4, 8, 16 and then 30 seconds. An edit cannot bypass backoff. Normal quit flushes pending edits and reports a final failure; the shutdown path covers direct app exit. Existing explicit save remains available. The separate change log still writes on edit. A foreign file refuses in-memory changes as well as publication. Strict x64 MSVC built the desktop app and passed 226 settings and 2,746 UI shell checks, including real backups and reopen after the UI's scheduled write. Merge the adjacent load-status guard additions from the schema migration branch when combining these slices.

## 2026-11-19 — EX-09 native render job JSON

**PR requested** — `engine-win/render-job-json`, `3e6b6352670808ac5ec944fc91c141a772824565`, fresh main base `315b9433`. The strict versioned codec records the persistent Job fields on that base, keeps native UTF-8 paths and exact integer values, and rejects unknown fields, duplicate keys, unsupported enums and live-only objects. It shares the pure validator with queue admission and rendering. Native strict x64 MSVC passed 364 checks across the codec, render queue and NI metadata suites. The NI AAC codec was unavailable in that build. EX-07's unmerged collision field must join the codec and its tests before persisted jobs can use it. User and project preset stores, field-group application and queue API work remain open.

## 2026-11-19 — TM-MD-05 native Opus feasibility

A private strict x64 MSVC diagnostic decoded a newly generated Ogg Opus file directly through libopusfile. Eight checks passed, including a 312-sample pre-skip, 9,600 declared and decoded 48 kHz frames, bounded reads, and aligned finite PCM. The inputs were the pinned JUCE 9.0.5 distribution's Xiph sources. This establishes feasibility for one owned mono file. ADI still needs the product decoder, chained and multichannel policy, and its error/cancellation tests.

The headless core cannot depend on a full JUCE checkout. The current JUCE build also defines the embedded Ogg, Vorbis and Opus symbols. Recommend one pinned standalone Xiph provider shared by core and JUCE, with the present JUCE readers and writers preserved and both build configurations gated. The existing license policy permits the BSD intake. No source import or decoder change was made in this slice.

## 2026-11-19 — DEV-25 native CLAP directory aliases

**PR requested** — `engine-win/clap-discovery-aliases`, `e9b3fefa823bd26f05466556ad4b873b804bc2b1`, stacked on `engine-win/scanner-fatal-diagnostic`. CLAP folder discovery resolves native junctions and symbolic links, walks each resolved directory once, deduplicates module paths, and never descends inside a plug-in bundle. Two native Windows junctions to the same vendor folder plus a cycle back into the search root reproduced a failure on the original finder. The new finder sees one canonical module through the alias-only and overlapping roots. Strict x64 MSVC passed scanner (45 checks), CLAP (503 checks) and all 15 native VST3 sequence cases with the new junction test. This leaves shell shortcuts, Finder aliases and VST3 root traversal for their own work.

## 2026-11-19 — PD-11 settings schema steps

**PR requested** — `engine-win/settings-step-migrations`, `e9aa9fa2c31f0a45c6d0d082f249dba8f55ae998`, stacked on `engine-win/settings-last-good`. The pure runner validates the complete ordered route before executing any step, keeps unknown fields, and returns no partial migration. Startup reads through the same runner as the save reread. The original disk bytes stay in the previous-good backup when a later save publishes a migrated document. A newer or foreign file stays protected. Schema 1 has no registered steps. A file without a schema still reads as schema 1. A well-formed older numbered schema without a route remains in place and gives a read-only migration diagnostic. Strict x64 MSVC passed 423 checks across the settings, migration and transport settings suites. Window still owns presentation of the load status.

## 2026-11-19 — DF-E23 native Windows hardware gate at 256 and 2,048 samples

**PR requested** — `engine-win/dfe23-hardware-diagnostics`, `685fcf8fbf64ff9f91275b9abec7d6928ab3bee7`, based on cloud worker implementation `d96c97d7731fac88ea93350897b65fcac29fe2e4`. Opt-in device CLI diagnostics verify the actual driver grant, worker allocation and callback timing. The callback writes preallocated rows and the CLI saves them after the device closes. Strict native optimized x64 MSVC build and `adi_play_render_tests` passed. The diagnostics and scanner discovery branches overlap in the default-roots constructor and playback CLI; retain one scoped root option.

**PR requested** — `engine-win/worker-measurement-kit`, `d07944a2094f67795135d00e8abd9f2d2e4be254`, stacked on `engine-win/acceptance-ci`. Adds the ADI-original fixture construction, native execution witness, controlled process sampler and paired statistics. All 87 asset checksums and 14 synthetic statistics checks pass. The kit contains only project-owned source and specifications. The director's research supplied the acceptance topology and buffer sizes.

**MEASURED** — 52/52 accepted physical ASIO arms at 48 kHz, including 12 stress arms and 40 lighter arms. Real native VST3 traces verified 64 distinct instances, six processing threads for five workers on the independent topology, and one on the serial control. The first 6,000 offline frames of each topology were bit-identical between zero and five requested workers. The lighter independent graph at 2,048 samples had median run p99 of 28.179 to 6.507 ms and paired change −22.262 ms [−23.572, −20.953], with +25.277 points of process CPU [22.985, 27.569] on a one-logical-CPU scale. At 256, 3.834 to 1.050 ms, paired change −1.836 ms [−4.389, +0.718]. One five-worker arm at 256 reached 62.914 ms and all 22 over-budget callbacks for that mode. Worker-off had one over-budget callback across its five matched arms. Serial requests allocated zero workers, as the serial graph exposes no independent hosted levels. The driver granted 48 kHz and 2,048 samples again at close.

**Decision** — keep the existing opt-in default. The high-buffer improvement is measured. The low-buffer tail, worker MMCSS verification, remaining buffer grants and complete ADR-0229 gate stay open. The driver supplied no native overload counter. These timing rows do not establish acoustic continuity or a comparative DAW ranking. Raw evidence stays off the repository.

## 2026-11-19 · Scanner timestamp range, discovery diagnostics and folded delivery names

**PR requested** — `engine-win/scanner-time-counts` at `b93ee50b2ed2916fb9b6a631e8eee86e03a194d6`, based on win's #643 head `302b1126`. Title **Encode scanner file timestamps without range loss**.

**For win:** the same patch applies cleanly to the exact #644 head `b1e44cbf`. It serializes raw native ticks without converting epochs or truncating wide signed values. The previous change failed the independent original Windows stamp check. Three strict MSVC scanner suites passed 170 checks after the correction, and a native Clang signed-128-bit check passed all 13. Apple runtime remains for CI.

**PR requested** — `engine-win/scanner-fatal-diagnostic` at `1a9a973978c60d6514917a1d759a6e5f33c93f94`, stacked on `engine-win/scanner-discovery-operation` at `b923be7e`. Title **Report the failure that stopped scanner discovery**.

DEV-25. An earlier rejected candidate no longer masks the later deployment failure or cancellation at the top of the report. Controlled child cases reproduced both wrong headlines and passed after correction. Existing real catalogue, Session and PCM gates passed under strict x64 MSVC.

**PR requested** — `engine-win/export-folded-collisions` at `2a41b6d91397c6fe17a67889192cd5955bad4864`, stacked on `engine-win/ni-delivery-media` at `150a1845`. Title **Use portable folded names for export collision checks**.

EX-07. Fail and Increment compare complete names with the pinned Unicode fold and recheck before native exclusive publication. Spelling and the final byte limit stay fixed. Real Windows files verified a folded expansion the native namespace treats as distinct. Two strict x64 MSVC suites passed 434 checks. An external fold-only arrival after the last comparison remains possible on such a filesystem. Native no-clobber is preserved. Replace and group coordination remain separate.

DF-E23 hardware preparation has started against the exact cloud implementation `d96c97d7731fac88ea93350897b65fcac29fe2e4`. Native buffer-grant and measured results will follow independently of the scanner and export changes.

## 2026-11-19 · Saved decode hashes and analysed cache budget

**For win before #643 and #644 land.** Their Apple compile fix casts the native file-clock epoch to signed nanoseconds. MSVC's file clock uses 100 ns ticks from 1601. A valid current Windows timestamp exceeds INT64_MAX when multiplied by 100. Keep raw native tick counts and encode wide integral counts without narrowing. Windows is preparing a correction for both PRs.

**PR requested** — `engine-win/decode-saved-hash` at `d8ad15411b917a6d28807d05500c55908440350d`, stacked on `engine-win/retire-queued-decodes` at `e9c61025`. Title **Reuse completed saved-hash audio without hashing the source again**.

TM-MD-02. A plain clip with a valid saved digest and matching source dimensions pins the completed decode and plays its saved PCM. A cold miss verifies the source before sharing or starting a job. A real Session opened 50 distinct warm FLAC files with zero whole-file source digests and matched its first block exactly. Three strict x64 MSVC suites passed 619 checks. A same-shape replacement at the source path can still play the saved cached audio. Filesystem relink and Offline diagnostics are separate. Native AAC remained skipped.

**PR requested** — `engine-win/analysis-cache-budget` at `473cf168be848818be5eacce754a5fa9cbcd407b`, based on `a0a31168`. Title **Evict verified analysis cache entries to an explicit byte budget**.

TM-MD-04. The owner-worker endpoint inventories valid direct-child analysis entries and removes the least recently used records to meet a caller-supplied byte budget. Already returned reads and library sample defaults survive. Busy entries report partial work. Three strict x64 MSVC suites passed 145 checks, including the native busy-file control. Native symlink coverage, global quota wiring and cross-process maintenance remain open.

## 2026-11-19 · Explicit scanner discovery and public-media rollback

**PR requested** — `engine-win/scanner-discovery-operation` at `b923be7e5cd85eff026ca5c4f8530bd4e81acd52`, stacked on `engine-win/session-catalogue-reopen` at `ecdf7cfd`. Title **Expose explicit incremental and cache-only scanner discovery**.

DEV-25. An explicit Incremental or CacheOnly operation returns immutable metadata and a typed result. Failed and cancelled refreshes leave the prior accepted snapshot available. Actual headless listing uses the operation, with confined-root and cache-only modes. Real module tests covered cold/warm/add, a bad candidate with healthy siblings, unavailable helper, damaged metadata, the legacy policy and cache-only reads with both helper and modules absent. The strict x64 MSVC hosting suite passed. K workers, Clean, startup scheduling, expiry, and window integration remain separate.

**PR requested** — `engine-win/owned-media-rollback` at `a67c3344ff48287b9f1e89afca642a2e643c9ea7`, stacked on `engine-win/collect-archive-publication` at `4a1d533e`. Title **Restrict media rollback to the file that was published**.

PD-02 and PD-04. Two real SQL rollback tests reproduced deletion of a later replacement in collection and extraction. The corrected Windows path validates and deletes through the same native file handle. Later replacements and busy files survive, and collection reports retained paths. On POSIX these two callers retain and report public rollback files when identity-bound removal is unavailable. Three strict x64 MSVC suites passed 231 checks. POSIX and privileged symlink cases remain unverified.

## 2026-11-19 · NI delivery media verification

**PR requested** — `engine-win/ni-delivery-media` at `150a1845f8157f65a5df40a6025e1a0e74af81ab`, stacked on `engine-win/export-stage-cleanup` at `d32f30ca`. Title **Verify NI input media through final delivery**.

EX-01. NI retains the Main render's input identity baseline, requires the supplied stems to match it, and rechecks the baseline after container assembly. Windows tests used real WAV render receipts and the same final publisher to reject a same-size external rewrite during the delivery and a stem that read different bytes. Three strict x64 MSVC suites passed 452 checks. The native AAC pipeline remains unverified on this installation. Plug-in assets and snapshot isolation remain outside this check.

## 2026-11-19 · Render cleanup and settings bundle import

**PR requested** — `engine-win/export-stage-cleanup` at `d32f30caf62629db4867be4e10011704d70c794d`, stacked on `engine-win/export-collision-modes` at `cbbc4202`. Title **Report private render stage cleanup failures separately**.

EX-07. WAV and NI report the private stage path and native error separately from delivery and publication status. Actual Windows sharing refusals left a private folder and preserved both a cancelled job's absence of output and a completed WAV's exact PCM. The run manifest records the cleanup result. Three strict x64 MSVC suites passed 445 checks. Native AAC and optional source symlink cases were skipped.

**PR requested** — `engine-win/settings-bundle-schema` at `72d2d37912003a8cb96a60537c703ef11a33eec4`, stacked on `engine-win/settings-bundle-safe` at `675f79a4`. Title **Check bundle manifest and settings schemas before import**.

PD-11. The manifest and settings document must use a supported integer schema and agree on the application before applying any value or appending to the change log. Valid exported v1 bundles retain settings, role resolution and presets. Seventy refusal cases preserved input bytes and target state. Three strict x64 MSVC suites passed 344 checks. Preset schema and migration remain separate.

## 2026-11-19 · Persisted hosted Session reopen

**PR requested** — `engine-win/session-catalogue-reopen` at `ecdf7cfd994a80de023de5229e62368093c3451b`, stacked on `engine-win/scanner-case-sequence` at `2ebd09ee`. Title **Verify saved VST3 and CLAP Sessions reopen without scanning**.

DEV-25. Both real plug-in formats were captured through Session-bound ParamOps and the journal, then restored into fresh owners and a fresh Store after removing their private scanner helper. State bytes matched and the first PCM after three seeks was exact. A VST3 process counter independently established that its passthrough blocks reached the actual processor. Three strict x64 MSVC suites passed. This is a headless project reopen in one process. Native platform CI and full app startup discovery need separate verification.

## 2026-11-19 · One-step verified media relink

**PR requested** — `engine-win/relink-contract` at `a8c9ce4a2db228c5b81b85e4b8064c94dfed1c86`, based on `a0a31168`. Title **Relink verified media batches in one undoable transaction**.

TM-MD-03. Every selected replacement must match the saved BLAKE3 before the relink batch commits. An unreadable, mismatched or duplicate target refuses all changes. Existing single relink uses the same path. A forced second application failure rolled back the first. Two actual WAVs produced exact Session PCM through relink and reopen, with one Undo and Redo for the pair. Three strict x64 MSVC suites passed 218 checks, with optional symlink cases skipped. Automatic discovery, ordinary playback verification and different-content Replace are separate.

## 2026-11-19 · Settings bundle publication

**PR requested** — `engine-win/settings-bundle-safe` at `675f79a4559911cd68423dd9bebbb653f287c5f6`, based on `a0a31168`. Title **Publish settings bundles from private closed archives**.

PD-04 and PD-11. Bundle export writes and closes the ZIP in a private sibling directory, then uses the existing replacing publisher. Existing ZIP bytes survive failed staging, sync, rename and open-destination checks. The old fixed staging tree and unrelated siblings keep their bytes. A post-publication durability warning keeps the result successful. Three strict x64 MSVC suites passed 271 checks. Bundle import schema validation is a separate step.

## 2026-11-19 · Collection archive publication and cancellation

**PR requested** — `engine-win/collect-archive-publication` at `4a1d533e7e2bab270d4a29cb633281b28d5d0a9f`, stacked on `engine-win/publication-policy` at `d661f704`. Title **Add archive export cancellation before atomic delivery**.

PD-02. The archive worker accepts cooperative cancellation at phase and entry boundaries, including after the archive closes and before exclusive delivery. Tests cancel at four boundaries and verify a late-arriving ZIP stays byte-identical. The successful publication warning reaches the Store, the document worker result and the CLI. Four strict x64 MSVC suites passed 446 checks. Window controls and progress display remain separate. Cancellation does not interrupt a single file copy or ZIP entry, and it finishes once atomic publication starts.

**For the cloud engine seat.** PD-03 needs the all-device snapshot completion contract. Current session-bound `ParamOps::snapshot` returns zero for both an unchanged device and a capture that could not proceed. The document should wait for a definitive capture-and-commit result before taking or copying a snapshot. This also gates the plug-in state step of PD-01. Daily snapshots still follow Q-A.

## 2026-11-19 · The scanner acceptance sequence and last-good settings

**PR requested** — `engine-win/scanner-case-sequence` at `2ebd09eeaa282b3b7842538b631008711b441566`, stacked on `engine-win/host-catalogue-hydration` at `6793c40a`. Title **Run the sixteen scanner cases through ADI and native fixtures**.

DEV-25. All 16 original cases passed on Windows. Cases 1–15 use the ADI helper and its own working native VST3 fixture. Cold and two unchanged warm calls used one child in total. Real size, timestamp, class/version changes, repeated timeouts, slash/case aliases and a directory junction were exercised. Case 11 records the intentional helper exit as an exit. Case 10 measures an automatic block after timeouts. Case 16 uses real CLAP processor state, with discovery, instantiation, accepted restore and three calls producing 84 exact stereo frames checked separately. The strict x64 MSVC gate passed three suites. Full Session reopen and production readiness reporting are separate.

**For the cloud engine seat.** The hosted lifecycle has tests at each state. For DEV-25's production display or export of readiness, we need the owner-side instantiation and state-acceptance transitions and a callback-safe first-process indication after the host actually processes the instance. The scanner will retain metadata-discovered as its own state. For CT-03, Arrangement MIDI still needs the driver event tap and its record epoch and track mapping. Existing retrospective MIDI memory resets on backwards time and has no Session input binding. A loop recorder needs a bounded handoff and a held-note continuation rule separate from audible playback events.

**PR requested** — `engine-win/settings-last-good` at `5ce8343534452686a626f7b728f150bbb78db43d`, stacked on `engine-win/settings-atomic-save` at `acba2e53`. Title **Merge settings hand edits and retain the previous good document**.

PD-11. Save rereads the current document and overlays the keys explicitly edited in this settings instance. Other stored keys and top-level fields survive. The prior valid bytes are written to `settings.json.bak` under the destination's replacement lease. A failed backup refuses the main write and retains pending edits for retry. Late changes detected during the lease check refuse the save. Five strict x64 MSVC suites passed 570 checks, with optional symlink cases skipped where Windows privilege was unavailable. Debounce, migration and full crash recovery are subsequent steps.

## 2026-11-19 · Single-export collision policies

**PR requested** — `engine-win/export-collision-modes` at `cbbc4202300ad1d4e5291da3ee5e392ab87cc4ac`, stacked on `engine-win/export-media-identity` at `e5461f1e`. Title **Resolve single-export Fail and Increment collisions at preflight**.

EX-07. Default Fail and explicit Increment choose one final delivery name. Increment inserts the counter before the complete suffix. The shared exclusive publisher refuses late arrivals, leaving those bytes intact. WAV and NI select the policy for the outer delivery only. Five strict x64 MSVC suites passed 1,025 checks. The tests exercised Windows directory-native case behavior and the actual WAV publisher. NI's shared path was exercised with staged bytes and full AAC encoding remains gated. Replace, portable folded batch sets, group reservation and UI confirmation remain open.

## 2026-11-19 · Atomic settings saves

**PR requested** — `engine-win/settings-atomic-save` at `acba2e53ee58d55fc5714b542f7fad5ed5728640`, stacked on `engine-win/settings-reset-overrides` at `783ee27a`. Title **Publish settings through a private atomic stage**.

PD-04 and PD-11. The settings store uses the shared replacing publisher after writing and closing its own sibling stage. An unrelated old .tmp file survives, and a newer, foreign or incomplete document present at save time keeps its bytes. Normal completion and refusal both clean the stage. Two strict x64 MSVC suites passed, totaling 378 checks. The last-good backup and hand-edit adoption are the next settings step.

## 2026-11-19 · Hosted CLAP acceptance, catalogue hydration, capture, export identity and settings

**PR requested** — `engine-win/clap-transport-gate` at `63c1fb2fdc0056f7394013510963657092f1d180`, stacked on `engine-win/probe-portability` at `882bf20a`. Title **Exercise CLAP transport behind a reporting VST3 delay**.

DF-E21. Against the exact cloud implementation `3c9961f861f3828240b197a6eb3bdb147e5a4d15`, the opt-in ADI host gate passed all 20 cases and 390,265 assertions under strict x64 MSVC. An actual CLAP observer downstream of the 1,024- and 4,800-sample VST3 delays checked the callback position, pre-roll, seek, PDC off, own loop wrap, offline mode and exact stereo PCM. All 20 cases failed on the baseline without the transport changes. CLAP tempo and beat-field assertions and a CLAP-format delay stay open.

**PR requested** — `engine-win/host-catalogue-hydration` at `6793c40ae07eb8c99e545334382fc99f95d34dc2`, stacked on the scanner catalogue read and catalogue-first branches. Title **Hydrate the hosted plug-in catalogue before opening projects**.

DEV-25. Fresh loader instances resolve the known VST3 and CLAP fixtures without a helper. A stored hint can rebind only to the same canonical module. A hydrated snapshot that lacks a class returns NeedsDiscovery without walking the roots. The genuine v1/v2 registry path retains legacy compatibility and migrates on discovery. Broken, foreign and newer snapshots fail closed. Five strict x64 MSVC suites passed. This covered native fixture instantiation and compiled GUI and CLI wiring. The full scan case sequence and an explicit first-processed-audio check are separate.

**PR requested** — `engine-win/audio-loop-pass-capture` at `d644644e2782f1aea13fa6148cd07c39d1ee0cba`, stacked on `engine-win/take-lane-operations` at `dc65a5f7`. Title **Capture audio loop boundaries alongside a single recording file**.

CT-02 capture step. Session writes one file per target and a preallocated, bounded pass log for an opt-in loop recording. Strict x64 MSVC passed 491 checks across five suites. Coverage includes 3½ loop lengths in both precision paths, count-in, partial edge passes, and refusal on seek or parameter changes that invalidate the capture. The linear commit route refuses loop captures so the recorded passes are not flattened.

**For the cloud engine seat.** CT-02 lane publication needs one durable store transaction that can write distinct Undo groups for each pass. The current OpJournal commit gives one Undo group to an entire batch. Please provide or agree the history contract before Windows wires loop pass publication. Capture and the stored-file lifetime can advance independently.

**PR requested** — `engine-win/export-media-identity` at `e5461f1edf031beb98ba301c6870ff06afb0abf3`, stacked on `engine-win/export-revision-report` at `56799a95`. Title **Bind WAV runs to the external media they actually read**.

EX-01. Selected external media comes from the immutable playback generation. It is fingerprinted at start and verified before final delivery. Two hardlinks share one content read and retain their individual reference paths. Tests cover a same-size rewrite and no output on detected change. Strict x64 MSVC passed 995 checks across five suites. The NI inner stage compiles with the guard. NI-wide identity, native AAC, plug-in assets and filesystem snapshot behavior still need validation.

**PR requested** — `engine-win/settings-newer-read-only` at `bb41a4799b8a2674d45f5023c8d0d1aab8c6bb1c`, based on `a0a31168`. Title **Keep settings from a newer schema read-only**.

PD-11. A newer document refuses mutation, reset and save even if its values have a shape this build cannot read. The file and its change log keep their bytes. Two strict x64 MSVC suites passed, including 368 settings checks.

**PR requested** — `engine-win/settings-reset-overrides` at `783ee27a091e4b0dc3a96d714de60b0e068e45ed`, stacked on `engine-win/settings-newer-read-only` at `bb41a479`. Title **Remove stored overrides when settings follow their defaults**.

PD-11. Reset removes overrides so the registry remains the default source. Page reset preserves other pages and unknown keys. Two strict x64 MSVC suites passed, including 372 settings checks.

## 2026-11-19 · Take-lane journal operations

**PR requested** — `engine-win/take-lane-operations` at `dc65a5f72c432cd0939a395c387d0e6b1ef47867`, stacked on `engine-win/timeline-main-lane` at `8c088dcbfce9a2264290d58022dfee171843fbbb`. Title **Add undoable take lane operations**.

CT-01. Create an empty `Lane` at the top, delete, reorder, rename, mute metadata and attach a clip to a same-track take lane or back to main. Delete captures clips and their child streams, aliases, source paths and previous order. It explicitly removes the take clips before deleting the lane. Undo and the late-failure rollback preserve captured data. Cross-track moves of a lane-attached clip require an explicit detach/move/attach batch. Strict x64 MSVC passed 1,892 checks across the lane, operation, schema, clip/time editing, placement and playback gates. Legacy comp-target behavior, audition and lane-aware producers remain open. CT-02 pass capture is in progress.

## 2026-11-19 · Catalogue reads, scanner infrastructure and export revision evidence

**PR requested** — `engine-win/scanner-catalogue-read` at `6b51e34f1a4d3c1ac437ce66279264b381f4b3f1`, stacked on `engine-win/scanner-failure-cache` at `cab873e2bef86e9660b5abe0cf405521bf4c8389`. Title **Read validated plug-in catalogue snapshots without a helper**.

DEV-25. A read-only SQLite transaction captures bounded stored descriptors without opening the scanner, inserting pending rows or walking plug-in folders. Pending, failed and blocked entries are excluded. A broken or over-limit snapshot is refused as a whole. The core validates the common packet and leaves VST3 format validation for the host. Strict x64 MSVC passed 250 checks across six suites, including reads after removing the fixture helper. Host hydration is the next step.

**PR requested** — `engine-win/scanner-infra-failures` at `79db43b6fc0d90a41e23b1806135873e223478e0`, stacked on `engine-win/scanner-failure-cache` at `cab873e2bef86e9660b5abe0cf405521bf4c8389`. Title **Keep scanner infrastructure failures out of candidate quarantine**.

DF-E25(a). Parent I/O failure and the reserved helper exits for malformed requests, unavailable format support and metadata-channel setup or delivery abandon the attempt. Exit 74 identifies pre-load channel setup and exit 75 identifies delivery after enumeration. Real child-mode tests show each infrastructure result can be retried unchanged without strikes. Strict x64 MSVC passed five scanner tests. DF-E25(b) is included in `publication-owned-copy`, where verified copy reports identity, directory-sync durability and warnings.

**PR requested** — `engine-win/export-revision-report` at `56799a95760d92b93094d149789844f6946a8087`, stacked on `engine-win/export-publication-report` at `71ef466a93afcfd979598371a4c7a1f556c853d2`. Title **Record the rendered project revision and resolved range**.

EX-01. The actual journal head, resolved track, range, sample rate and name inputs come from the render session's SQLite snapshot. Tests cover external edits during loading, job-start revisions, changed regions and current head after Undo. Strict x64 MSVC passed 488 checks across four suites. The NI same-backup check was compiled and awaits native AAC runtime coverage. Media identity at start and before publication is a subsequent step.

**PR requested** — `engine-win/retire-queued-decodes` at `e9c610254c96ee51bf11701d5c367c6afc380367`, base `main` at `a0a31168`. Title **Retire progressive decode jobs when their last queued lease is dropped**.

DF-E3 and TM-MD-02. The queue holds weak references before work starts. Dropping the last public lease removes its private stage and the next admission can reclaim the slot and reserved bytes while both workers are busy. A remaining public lease keeps the work alive. Strict x64 MSVC passed the clip playback, decode and progressive decode suites. Native AAC progress remains skipped.

## 2026-11-19 · Hosting probe portability and DF-E21 on the implementation

**PR requested** — `engine-win/probe-portability` at `882bf20ac5cac00b817335e9eec3c70e37544b12`, stacked on `engine-win/acceptance-ci` at `4c25144068552da347f5bfe23dbeb3fa89d89e0a`. Title **Run the diagnostics on native hosting platforms**.

Both hosting CI legs now build the four diagnostics and run the three loaded-module tests. The port includes native macOS bundles and protected POSIX mappings. Strict Windows x64 MSVC passed CLAP 399, VST3 777 and timing 430,303 checks, plus the DF-E21 oracle. The 86-entry fixture manifest verified. Native macOS and Linux runs still need those environments. The host-level DF-E21 gate remains Windows-only.

**DF-E21 verified for the cloud seat.** The exact implementation `engine/df-e21-plugin-transport` at `3c9961f861f3828240b197a6eb3bdb147e5a4d15` was combined locally with the fixture branches above. With `ADI_ENABLE_DF_E21_ACCEPTANCE=ON`, the strict x64 MSVC VST3 acceptance passed all twelve cases, 197,018 checks, at the reported 1,024 and 4,800 sample delays. That includes negative start, seek, tempo, the device's own short-loop boundary, PDC off, and exact stereo PCM in offline export. The implementation's CLAP and parameter automation suites, hosted render and oracle also passed. The reporting-delay chain is VST3. The full CLAP behind-latency acceptance, meter and event extensions remain in the test README. DF-E23's hosted worker comparison still awaits the worker implementation.

## 2026-11-19 · Scanner refusal cache, known VST3 loading and default take playback

**PR requested** — `engine-win/scanner-failure-cache` at `cab873e2bef86e9660b5abe0cf405521bf4c8389`, stacked on `engine-win/scanner-success-cache` at `f8fa0efaa8dd154e143e393d66342cb7382bc875`. Title **Remember stamped scanner refusals and retry aliased modules**.

DEV-25. An unchanged rejection, including a zero-class CLAP module, keeps a failed result with zero strikes and no quarantine entry. The second crash or timeout blocks an unchanged module. A reliably changed stamp retires the automatic failure while preserving manual and legacy blocks. Explicit retry resolves slash, case and directory-junction aliases. The Windows junction was created and exercised without privilege changes. Strict x64 MSVC passed 224 checks across five suites. Symbolic-link cases requiring unavailable privilege were skipped.

**PR requested** — `engine-win/catalogue-first` at `262a26066ee54e1bed23712213c6d7d6828844ec`, stacked on `engine-win/scanner-success-cache` at `f8fa0efaa8dd154e143e393d66342cb7382bc875`. Title **Reuse the VST3 catalogue before probing a saved path**.

DF-E20(c), known VST3 lookups. The Windows test instantiated a known fixture repeatedly even after removing its private scanner helper, including a request with no hint. An unknown class at a known module returned missing. The hosted render and VST3 scanner suites passed. The VST3 callback test measures the empty-result overhead of MSVC's Debug containers separately and still requires zero additional scanner allocations and zero callback I/O. Unknown modules retain their legacy discovery path until startup catalogue hydration and service land.

**PR requested** — `engine-win/export-publication-report` at `71ef466a93afcfd979598371a4c7a1f556c853d2`, stacked on `engine-win/export-final-names` at `96ac4186d6cfeff80664ec8a761e40bcd3d05b23`. Title **Keep export publication results and serialize run outcomes**.

DF-E19(a). WAV and NI use one AtomicOnly commit helper and preserve the publisher's warning, fallback and durability fields independently of Completed. The caller can serialize the frozen run membership and outcomes as JSON, including removed and unstarted jobs, without a sidecar write. Strict x64 MSVC passed 469 checks across four suites. Native AAC integration remains unavailable. The report retains requested inputs and actual output and normalization. EX-01 revision and media identity and EX-07 batch collision handling still need their own steps. When combining with `publication-owned-copy`, keep the sync hook at both publication result sites in the shared file publisher.

**PR requested** — `engine-win/timeline-main-lane` at `8c088dcbfce9a2264290d58022dfee171843fbbb`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Select the arrangement main lane for default playback**.

First CT-01 engine step. Audio, MIDI, controllers, clip envelopes, gain staging and freeze playback readers use the shared main-lane selection. Stored take clips remain available to editors. Tests check exact main audio with an overlapping take, silence after removing the main clip, only the main MIDI note and expression, and release after refresh. Strict x64 MSVC passed 1,138 checks across six suites. Lane journal operations, audition, edit scopes and loop recording are subsequent steps.

## 2026-11-19 · Publication policy and the take-lane readings

**PR requested** — `engine-win/publication-policy` at `d661f704cc423434dc80770e6a14f81598e3f857`, stacked on `engine-win/publication-owned-copy` at `629a9f3de471b6d38857c25342f8962221178380`. Title **Set publication policies at their callers**.

DF-E19(c). The EX-07 detail records the policy by caller. User deliveries and authoritative project media require exclusive rename. Completed decode caches and collected media may use verified copy with an exclusive final link. A refused rename in AtomicOnly mode starts no copy and leaves the source intact. Twelve strict x64 MSVC Debug suites passed through export, collect, project files, decode, recording, stems, freeze, bounce and the library. Native AAC coverage was skipped by the existing codec gate.

**For win to relay into CT-01 to CT-03.** The director's Live 12.4 readings already contain the following. The available original Sets were read again without changing them.

- New empty take lanes use the name `Lane` and insert at the top. The insert is undoable. Show Take Lanes is outside undo and the Set stores the folded flag on each track. Saved tracks with folded and unfolded states were observed. A controlled two-populated-track toggle and reopen were not run.
- Lane audition greyed the main lane. The contemporaneous reading records unchanged Undo wording and a saved on-state. The final retained Set has audition off after cleanup. An independent audio isolation test was not run.
- The main lane retains the take clip's name on its copied pieces. In the linked comp, a different take replaced the center piece while the two outside pieces kept their original take name. The audio loop recording used one file for four takes, including the incomplete final pass. One Undo removed the last pass and restored the previous comp.
- Arrangement MIDI Overdub merged the old notes with each pass while on, accumulating them through loops. With it off the recorded range was replaced. Undo was per take pass and Record Quantization was separately undoable. The held note ended at the boundary in the first lane and could continue into the following cycle in the cumulative overdub clip.

No new Live playback was run. The computer-use runtime is unavailable to this seat. The original readings establish the lane and journal behavior above. The narrower audio and two-track reopen checks remain open.

## 2026-11-19 · Hosted acceptance gate and owned publication staging

**PR requested** — `engine-win/acceptance-ci` at `4c25144068552da347f5bfe23dbeb3fa89d89e0a`, stacked on `engine-win/acceptance-assets` at `a60371a84436bcbb4c5cb50bdacc8e62abd94343`. Title **Run research fixtures and gate ADI hosted transport acceptance**.

Windows hosting CI builds VST3, CLAP, reporting-delay and scanner diagnostics. The strict x64 MSVC build passed all three module self-tests, the DF-E21 oracle and existing hosted render test. The Python job verified the 82-input SHA-256 manifest, reproduced 66 signals, seven edge and latency inputs, eight MIDI files and their events, and checked 16 tuning inputs and ten Grid specs. The remaining hosting leg is being ported separately.

**For the cloud engine seat** — after taking this branch, configure Windows hosting CI with `-DADI_ENABLE_DF_E21_ACCEPTANCE=ON` on the DF-E21 implementation. `adi_df_e21_host_acceptance` uses the real scanner, Session, VST3 host and render runner. Both real hosted delays report 1024 or 4800 samples after preparation. The deliberate main baseline fails all twelve cases at missing VST3 ProcessContext. Later timing assertions need the transport implementation. The default oracle remains green. CLAP and the other full-contract additions are listed in the test README. DF-E23 awaits the cloud worker implementation before the ADI recording-buffer comparison.

**PR requested** — `engine-win/publication-owned-copy` at `629a9f3de471b6d38857c25342f8962221178380`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Publish verified copies without destination cleanup races**.

DF-E19(b). The fallback streams and confirms a privately staged copy, then claims the final name with an exclusive hard link. Failure cleanup never unlinks the public destination. The copy returns its file identity and directory sync result. Strict x64 MSVC passed the collect/export and two project publication suites. The late-collision and injected-failure checks both retain the other writer's bytes and the source stage. A filesystem with neither the permitted exclusive rename nor hard links now refuses publication. Per-caller policy and job/run reporting are separate steps.

## 2026-11-19 · Scanner success cache and complete export names

**PR requested** — `engine-win/scanner-success-cache` at `f8fa0efaa8dd154e143e393d66342cb7382bc875`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Cache validated plug-in discovery results**.

DEV-25 and DF-E20(c), first scanner step. Registry v3 keeps a whole validated positive packet keyed to format, canonical path, file size, modification time, relevant bundle files, protocol and helper build. Three unchanged probes launch one owned child. Changed size with preserved mtime, changed mtime, changed helper, invalid cache, interrupted completion and legacy v2 migration are covered. An unavailable fingerprint still permits isolated discovery and reports that it could not cache. Strict x64 MSVC Debug passed all five scanner, cache, process, quarantine and registry suites. Negative outcomes, the rest of the retry policy, catalogue-first loading and readiness phases are separate work. The sixteen-case acceptance is still open.

**PR requested** — `engine-win/export-final-names` at `96ac4186d6cfeff80664ec8a761e40bcd3d05b23`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Check complete export filenames against the byte limit**.

DF-E19(d) and EX-07. WAV and NI Stems validate the final 200-byte UTF-8 budget including the full format suffix and reserved-name prefix. Strict x64 MSVC Debug passed 245 queue checks and 61 NI metadata checks. Native WAV publication accepted 199 and 200 bytes and refused 201. The case-only collision left the first file intact on the Windows test filesystem. The NI suffix boundary passed its codec-independent check. The native AAC codec was unavailable for actual NI boundary renders. Cross-filesystem case folding and the full EX-07 collision-policy choices remain open.

## 2026-11-19 · Acceptance assets published

**PR requested** — `engine-win/acceptance-assets` at `a60371a84436bcbb4c5cb50bdacc8e62abd94343`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Add the director's research acceptance assets**.

All 78 files match the validated v2 patch's Git blobs. Changes are limited to `adi_daw/tests/research_intake/`. The strict x64 MSVC Debug build (`ADI_WERROR=ON`, `/W4 /WX`) and CTest passed `adi_store_tests`, `adi_plugin_scanner_tests` and `adi_scan_quarantine_tests`. Pinned Python dependencies reproduced 66 signal files, seven clip and latency files and eight MIDI fixtures. The verifier checked their hashes and events, 16 tuning fixtures and ten Grid specifications. Fourteen sampler/statistics unit tests passed. The data checks do not close product parity or the DF-E21 acceptance gate.

**In progress** — `engine-win/acceptance-ci` is stacked on this branch. It wires the generators and probe builds into CI and provides an explicit gate for the cloud engine seat's DF-E21 work.
