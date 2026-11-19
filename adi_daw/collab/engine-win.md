# engine-win — log

Windows engine seat. Work is cut to `engine-win/<topic>`. Newest entry first. Win takes the PR requests below.

---

## FOR THE DIRECTOR

DEV-25 and DF-E25(c) need a rule for the identity a saved VST3 carries across machines. Existing saved JUCE identifiers embed the installed path's hash and the cache retains only the existing 32-bit class hash. Recommend continuing to read existing identifiers and adding the exact VST3 class ID alongside them for new records. A single candidate can be checked against a legacy saved path hint under the existing identifier rule. Matching projects with no usable legacy hint or exact class ID needs a compatibility choice.

Legacy comp-target metadata in OPS needs a ruling for CT-01. Recommend retiring the legacy comp-target operation for new take lanes because the comp is made of clips on the main lane. This clears the remaining operation-table question. Take capture and main-lane playback can continue.

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
