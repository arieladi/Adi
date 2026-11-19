# engine-win — log

Windows engine seat. Work is cut to `engine-win/<topic>`. Newest entry first. Win takes the PR requests below.

---

## FOR THE DIRECTOR

PD-06 (Q-I) needs a decision on interrupted recordings. Recommend offering recovery for ADI-owned takes after a crash, with Stop's normal placement as one Undo step and a recording ID to prevent duplicate placement. This blocks automatic recovery after CT-02 and PD-05 provide the recording plan and session evidence.

EX-06 (Q-J) needs the three shaped export dither modes and their labels. Recommend the three open-reference shapes in the plan (Lipshitz, improved E-weighted and Shibata) with ADI labels. This blocks the shaped presets and their acceptance limits.

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
