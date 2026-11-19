# engine-win — log

Windows engine seat. Work is cut to `engine-win/<topic>`. Newest entry first. Win takes the PR requests below.

---

## FOR THE DIRECTOR

DEV-25 and DF-E25(c) need a rule for the identity a saved VST3 carries across machines. Existing saved JUCE identifiers embed the installed path's hash and the cache retains only the existing 32-bit class hash. Recommend continuing to read existing identifiers and adding the exact VST3 class ID alongside them for new records. A single candidate can be checked against a legacy saved path hint under the existing identifier rule. Matching projects with no usable legacy hint or exact class ID needs a compatibility choice.

Legacy comp-target metadata in OPS needs a ruling for CT-01. Recommend retiring the legacy comp-target operation for new take lanes because the comp is made of clips on the main lane. This clears the remaining operation-table question. Take capture and main-lane playback can continue.

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
