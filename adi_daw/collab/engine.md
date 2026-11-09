# engine — log

The engine lane (ADR-0207, `docs/LANES.md` §3): this seat is Codex in a Linux cloud environment
(GCC 14 and Clang 19; no MSVC here, so `/WX` is win's leg), one branch per item,
`engine/<topic>`, cut from `origin/engine/log` as directed on 8 November. Only the engine lane writes to this file.
Newest entry at the top.

**Why this log has its own branch and PR (`engine/log`).** Every item PR prepending an entry here
would make each merge conflict the next open PR on this file, the failure `collab/README.md` rule
3 exists to prevent. So the item PRs never touch this file; this one does, and win merges it
whenever convenient. Each entry names its PR, so nothing is lost if this merges late.

---

## 2026-11-09 — MI-01 captured note diff gated; taking MI-02

**PR requested:** head `engine/note-apply-diff` at **`71949e5406ab327abf245a89b4818fc1ffb88f48`**, base `engine/log` (**`815a4f5a5328ec3691c0010de23136a00e2b1b79`**). Title: **Apply captured MIDI note differences with one exact stream write**. Branch pushed; win opens and merges the PR and runs MSVC. Independent branch cut from refreshed origin/engine/log, with no pending ED-01/ED-03, Link, N29 or N17 source merged. No feature-branch engine-log edits.

**Window/producer handoff / PR body:** `note_diff.hpp` supplies immutable owner capture, pure full-record preparation and one expected-head `note.applyDiff` commit. The canonical row ID, whole-stream BLAKE3, exact old records and complete removed expression sets guard staleness independently of the journal head. Full uint64 note identities and signed SQL identities are retained. First creation uses a caller-owned literal stream ID and one captured complete default row; the op never allocates an ID. One stream INSERT/UPDATE carries all added/removed/changed notes, including a 10,000-note edit. Removal deletes every owned expression dimension in the same transaction; Undo restores exact typed rows, opaque flags/tails/trailer, original ordering and original stream absence. Native or caller-supplied future tails only; unsupported synthesis refuses. Numeric bounds and checked signed endpoints refuse before narrowing. Preview effects are literal values; changing the MI-01 request invalidates that mutable informational preview. MI-02 supplies the bound overlap preview next.

**Historical compatibility and admission:** the five P0 note editors use the same kernel while retaining descriptor names, insert/delete inverse pairs and scalar velocity coalescing. Generic borrowed `OpReplayMetadata` lets History expose recorded inverse provenance without op-specific History logic. Valid historical scalar insert/delete replay preserves the expression rows those old inverses depend on. New captured Redo recomputes original forward intent against recorded typed rows and literal creation defaults/identity; row-ID pressure or a contradictory forward cannot silently change them. INTEGER and REAL guards differ; NaN, affinity drift, changed canonical/expression sets, and extra/missing trigger writes refuse and roll back. Post-apply total row changes must equal the planned delta. No schema/ANOT layout change; OPS adds one P0 op (211 total: 71 P0, 84 P1, 53 P2, 3 P3).

**Signed origins:** the Clip View reader retains valid notes starting before relative zero. Native playback admits signed starts within its retained ±2^50 tick bound, chases a crossing tail with its original end, and skips notes already ended without silencing the clip. Forty actual Session fixtures cover both widths, regular/irregular partitions, source offsets, explicit End, loop prefix/repeats, touching boundaries and both arithmetic-domain endpoints; callback allocation/file I/O and dropped-note checks stay zero. This preserves the existing playback bound; it does not claim the entire int64 storage range is playable.

**Full gate:** full GCC14 Debug/Werror/Pd-on/JUCE-off build, zero warnings; `test_all.sh` **16,560 checks / 190 suites**, every row and sum audited, zero failures, all four validators and binary layout clean. The independent baseline was 15,685/188: **875 added checks / two suites**. New note diff **362**, compatibility **271**; native MIDI **412** (240 new), Clip View **30**, replay **69**. The new note-diff and compatibility suites cover 72 successful nonempty edits across three clips with an independent raw-byte model, Undo-all digest and reopen/Redo; real SQLite write hooks, full IDs, future layouts/columns/defaults, pure preparation after Store close, coherent WAL capture, independent stale guards, empty expression sets, all five old editors, literal historical logs/orphans/negative stream allocation, coalescing, typed values and late rollback. The full run initially exposed a stale equal-time pitch-order assumption in the Clip View fixture; changing it to a genuinely earlier move exposed the signed-origin reader/consumer restrictions corrected above. The preserved failed run is not counted as the final gate.

**Compiler/adversarial gate:** twenty strict GCC/Clang translation units (seven production and thirteen tests), zero warnings; thirteen Clang suites pass **1,642 checks**, zero failures, against the validated GCC cold archives. Sixteen isolated compiled plants trigger **207 ordinary named assertion failures**, no crash or exception counted. Plants cover whole-stream and old-record guards, future tails/flags, full-width expression identity/removal, original absence, historical inverse pairing/expression behavior, captured Redo intent/creation identity, SQL storage classes, actual stream-write counts and both signed-origin consumers. All 680 frozen inputs, core, production/test objects and baseline binaries retain their hashes. Artifacts: `/workspace/adi-cloud/note-diff-focused-tests.json`, `note-diff-complete-build-final.log`, `note-diff-complete-gate.log`, `note-diff-complete-gate-audit.json`, `note-diff-review/`. Final GCC core, settings archive and thirteen focused binaries are preserved with hashes at `/workspace/adi-cloud/note-diff-release-71949e54/manifest.json`. No whole-project Clang, sanitizer, MSVC/macOS, hardware or measured Live parity claim; README remains win-owned.

**Merge handoff and next:** combine native MIDI test dispatch additions additively with N17/Link, retaining Link's stored-identity/continuity changes alongside this bounded signed-start admission. This closes MI-01's mutation kernel, not note overlap, gesture lifetime, recording/overdub, curve transforms or the window editor. ED-04 still needs the two-level pending ED-01→ED-03 source stack; do not add a third level. **Next immediately: MI-02**, from fresh origin/engine/log with only this exact gated MI-01 prerequisite if still absent. Its versioned key-only interval resolver will expose every truncation/removal before one commit; prior literal note operations keep their replay semantics. Continue in this same turn without waiting for PR creation or CI.

---

## 2026-11-09 — ED-03 material planner gated; native document latency measured; taking MI-01

**PR requested:** head `engine/edit-scope-placement` at **`c58de5be2ad7b9f86510a7ee6178ca4a30189b5f`**, base `engine/log` (**`a3b4001e6e3031ed3a09c153eeccda73674fbc10`**). Title: **Add captured material scope and shared clear-before-place planning**. Branch pushed; win opens and merges the PR and runs MSVC. Created from refreshed origin/engine/log, then merged the sole gated prerequisite, ED-01 `engine/range-survivor-identity` at `84c43f15eba955fe45c6ef481bf7fe79dc5ce556`; merge ED-01 first. No engine-log edits are included. This is the material foundation of ED-03, not closure of its producer, automation or window/drop gates.

**Window handoff / PR body:** `edit_plan.hpp` supplies owned literal `EditScope`, captured policy and descriptions, prepared group moves and anchored-start end resizing. Capture every source first, clear the union of every same-lane destination with relocated originals excluded, then place all incoming material in one expected-head `arranger.splice`. Swaps retain both identities; gaps between destinations survive; sole or leftmost survivors keep original root/child IDs and later fragments allocate only the identities actually needed. The pure interval resolver has no Store/SQL/settings access; image capture, source-clock transforms and typed row planning remain in the owner adapter. Ordinary clip/range paste, duplicate, cut and delete share the material planner. Dedicated whole-Set Time commands retain their existing all-lane path. Settings are captured once by the `adi_settings` bridge; core receives owned values. No schema or opcode change.

**Material and admission:** physical-nanosecond resolution and directed interior tick conversion prevent rounded touching from hiding real overlap, while native outer endpoints, pinned source phase, loops, source-end markers, authored/inherited fades, opaque children, aliases and typed future columns survive Undo/Redo. Description is bound to the captured source/head and semantic op prefix; actor/label stamping and explicitly described appended requests remain possible. Main-lane/frozen/policy boundaries refuse by name. Default Lock-off edits with affected global track, recursively owned device or send automation explicitly refuse until AU-05; explicit Lock keeps those rows fixed. Unsupported modes do not silently become a different edit. No media, bounce, recording, MIDI Capture, stem or raw-window-producer migration; no AU-05/AT-10, left-trim/Crop, ripple or Live parity closure.

**Correctness gate:** full GCC14 Debug/Werror/PD-on/JUCE-off build with zero warnings; `test_all.sh` **27,554 checks / 191 suites**, every row and sum audited, all four validators and binary layout clean. ED-01 base was 15,933/188. New pure placement **10,566**, owner planning **610**, native playback **445** checks. The native owner gate commits 40 actual nonempty edits across three tracks, independently checks physical overlap after every commit, undoes all to the exact typed rows and initial digest, then reopens and replays. Pure algebra has a separate seeded occupancy/permutation oracle. Thirty-six native Session fixtures cover both widths, 44.1/48 kHz source rates, tick/ns placement, Warp, tempo changes, source offsets, loops, sparse victims, inherited split fades and active end fades; callback allocation/file-I/O checks remain zero. Existing clip-edit, native audio, Time, row, material and MIDI suites remain green.

**Compiler and adversarial evidence:** fifteen strict GCC/Clang translation units (five production, nine tests, benchmark compile-only), zero warnings. Nine Clang suites pass **12,942 checks**, no failures, using validated GCC cold archives. Nine isolated compiled plants trigger **185 ordinary named assertion failures**, no crashes or uncaught exceptions: alias detachment (2), unlocked automation admission (13), description payload binding (1), outside-mask filtering (8), actual owner relocation exclusion (95), pinned source phase (8), sole survivor identity (11), pure moved-ID removal reporting (26) and union gaps (21). The moved-ID reporting plant is not claimed as the actual swap test; the owner plant covers that boundary independently. Final benchmark CLI amendment changed only its tool TU, recompiled strictly with both compilers; production/test inputs and 23 unaffected Clang artifacts stayed hash-identical. No whole-project Clang, sanitizer, MSVC/macOS or hardware claim.

**Measured native document path:** Sixteen individual observations (one fresh and one primed per width/path/member-count), Linux GCC14 Debug, 100 material tracks plus Main, 200 starting clips, 100 synthetic native devices, 48 kHz/256 frames. Prepared capture took **6.12–8.37 ms for one clip** and **38.99–43.86 ms for eight**. Whole document synchronisation took **21,570.67–27,208.28 ms** across the raw and prepared paths; owner totals were 21,647.99–27,215.12 ms. All edits retain 100/100 devices with no loader call, one rebuild and one publication, zero ordinary callback allocations/file I/O and no source/decode read errors. The run nevertheless records **2,763 callback execution-budget exceedances / 70,438 blocks** and **290,560 missing source frames** across new generations. These are process-duration and per-source counters, not hardware xruns. Aggregate nonzero output after adoption does not measure edited-source readiness. Raw moves retain victims while prepared placement removes them, so these are the same intent with different row effects; no distribution, controlled hardware, Release or window-drop claim. Static inspection finds 66 fresh full-source Re-Pitch renders per raw rebuild (6,336,000 stereo output frames), with no prepared-result reuse; this is a profiling lead, not measured timing attribution. ADR-0217 d5 actual window/drop validation remains open. Raw JSON SHA256 `a412656d9be19803d551871db6d6be0bb8babf6ac75bfc7b0b374787930da02e`.

**Reproduction and merge notes:** code measured at `00c0a9aabcd0ec8363fb9e7405a1c3453da00a23`; the final commit adds only the report/raw observations. `ENGINE-EDIT-PLACEMENT.md` describes the API; `ENGINE-EDIT-PLACEMENT-MEASURE.md` contains the sixteen individual observations, provenance and limits, with full raw JSON. Local evidence: `/workspace/adi-cloud/placement-review/summary.json`, `placement-full-gate-audit.json`, `placement-complete-build.log`, `placement-complete-gate.log`, `placement-measure/run-bounded/`, and `placement-measure/static-synchronise-audit.md`. The measured core SHA256 is `015955505711fea2300b1457c463d8bc4b2dfdc2b1dd0c946d2f026379d49e54`. Preserve N17 and ED-01 test dispatch additions additively when their independent branches meet; the existing opaque-copy fixture now chooses Lock explicitly and free destination space. No README/FEATURES counts changed.

**Blocked next item — ED-04:** origin/engine/log is still `a3b4001e`; ED-01 and ED-03 are two pending source levels. ED-04 explicitly depends on ED-03 (PLAN's ED-04 card), so another dependent branch would exceed this seat's two-level limit. No waiting for PRs or CI: take independent **MI-01, `note.applyDiff`**, immediately on `engine/note-apply-diff` cut from fresh origin/engine/log. Preserve full-stride note bytes, flags and expression rows; one stream write per diff, stale-stream guard, exact inverse and historical P0 inverse dispatch. Revisit ED-04 once its base lands. The measured synchronisation workload and actual window/drop validation remain explicit follow-ups, not evidence that the latency gate passed.

---

## 2026-11-09 — ED-01 range survivors and lane scope gated; taking ED-03

**PR requested:** head `engine/range-survivor-identity` at **`84c43f15eba955fe45c6ef481bf7fe79dc5ce556`**, base `engine/log` (**`ade4ea0c2def33126d9e79b3d367a16ecf71759d`**). Title: **Preserve range survivor identities and distinguish material from Time scope**. Branch pushed; win opens and merges the PR and runs MSVC. This finishes ED-01's remaining engine range-kernel work after N17's two-point split. Cut directly from origin/engine/log; no pending N17 source or log edits are included in this feature branch.

**Window handoff / PR body:** existing material-range preparation APIs now select and clear only main-lane clips (`lane_id IS NULL`). Copy/Cut/Delete/Paste/Duplicate ignore independent take material, including unsupported/nonfinite take metadata. Shared cut math trims a sole left or right survivor in place, preserving original root/material-child IDs, external aliases, source phase, loop and fade domains; a two-survivor erase keeps the left ID and allocates exactly one independent right piece. Neither an unused tail nor a discarded middle consumes IDs. Captured `arranger.splice`, expected-head commits and public signatures stay unchanged. The dedicated whole-Set Copy/Cut/Delete/Insert Time paths explicitly retain all-track/all-lane scope; Range's insertTime option copies main-lane material while still inserting time across the Set. Delete Time relocates the staged trimmed root and source-clock rows rather than reloading old values. ENGINE-EDIT-COMMANDS and header comments specify the boundaries.

**Gate:** full GCC14 Debug/Werror/PD-on/JUCE-off build, zero warnings, and `test_all.sh` **15,933 checks / 188 suites**, every row/sum audited, zero failures, all four validators and binary layout clean. Independent source baseline **15,685/188**; this item adds **248** checks: clip edits **296** (124 new), native audio **375** (124 new). Time edits **413**, row edits **36**, native MIDI **172** and material commands **29** remain green. Both-clock interval cases cover disjoint/touching/full/prefix/suffix/interior erasures, exact typed Undo/Redo, opaque child/parent/alias preservation, pinned-zero and absent clocks across a tempo change, main-lane versus whole-Set scope, exhausted IDs and the single remaining ID. Twelve actual PCM cases at both widths and source rates prove material deletion only silences its selected span, Time deletion shifts the original suffix by precisely the removed duration, and original source/loop/fade phase plus identities survive one Undo/Redo. Callback allocation and file-I/O audits remain zero.

**Compiler, adversarial and compatibility evidence:** strict GCC and Clang19 compile clip_commands.cpp plus all six focused test units with actual flags and Wshadow/Werror, zero warnings. Clang passes **1,321 checks**, zero failures, against validated GCC cold dependencies. Eight isolated compiled plants trigger **52 ordinary assertion failures**: sole-tail recreation (15), lost source offset (24, actual PCM), omitted main-lane filter (5), writer-only all-lane scope (2), Time-copy main-only scope (2), Time-erase main-only scope (2), discarded sole-prefix clone (1), and discarded interior allocation (1). No crash/exception counted; source/archive/baseline hashes stay unchanged. Supplementary external combined production code uses this refactor with the exact pushed N17 overload/header and preserved N17 validator/test objects: **271 core + 357 audio + 208 MIDI = 836 checks**, zero failures. This verifies the pending two-cut behavior without stacking its implementation into this branch. Evidence: `/workspace/adi-cloud/ed01-review/summary.json`, `range-survivor-focused-tests.log`, `range-survivor-complete-build.log`, `range-survivor-build-recovered.log`, `range-survivor-complete-gate.log`.

**Merge handoff and limits:** when combining this and N17, retain both branches' core/audio test dispatch calls additively; the compatibility run used N17's preserved test objects, not a merged dispatcher. Complete deletion still captures/detaches external aliases for referential integrity, including a take alias outside selection; independent take rows remain protected. Existing pasted-Time material still lands on the main lane; this does not add take-lane clipboard placement, audition, EditScope, global automation-follow, ripple or frozen-track policy. Exhaustion fixtures exhaust root and child IDs together, not child-only limits independently. No whole-project Clang, sanitizer, MSVC/macOS execution or measured Live parity claim; README remains win-owned.

**Next immediately: ED-03**, the shared EditScope/placement planner, with captured scope and descriptions, clear-before-place algebra, group moves and anchored end resizing, then the rest of M2 in the director's order. Cut the next branch from refreshed origin/engine/log, never main; explicitly name any gated range-kernel prerequisite rather than claiming its code is already merged. Continue in this same turn without waiting for PR creation or CI.

---

## 2026-11-09 — N17 two-point split gated; taking the remaining ED-01 range kernel

**PR requested:** head `engine/clip-two-point-split` at **`bf51597e23826b744cfc4bc6147ec406736fbfa4`**, base `engine/log` (**`a09d67cd80d59ca2b469a4681fb98ad25a4c973a`**). Title: **Split one clip at two points in one undo step**. Branch pushed; win opens and merges the PR and runs MSVC. This closes N17's remaining engine request; the window owns its row and bindings. Independent of the pending N29 and Link source changes, cut directly from origin/engine/log.

**Window handoff / PR body:** call `clip_edit::prepareSplit(store, sourceId, middleId, rightId, begin, end, referenceRate)` on the Store owner, then `commitPrepared` once. Both cuts are made in memory from one coherent captured image with one shared child-ID allocator, returning one `clip.split` request. The left piece keeps sourceId, the selected interior is middleId, and the right piece uses rightId. The caller can append `clip.setMute` for middleId so both cuts and only the selected span's mute share one expected-head check, transaction and Undo. Endpoints use one explicit input clock and must remain ordered and strictly inside the stored clip after conversion; identities must be distinct and unused. One-cut preparation remains available; ordinary zero/negative caller IDs accepted by the existing schema/history remain supported. Split/join validation accepts one or two distinct new pieces, keeps their track/kind/time base and the existing ownership/inverse guards. OPS and ENGINE-EDIT-COMMANDS document the extension; no schema, new opcode or window change.

**Material and atomicity:** source media, loop phase, pinned unwarped clocks and original fade domains stay intact; new edges create no fades. Opaque notes/expression/automation data, full-width stored identities, future typed BLOB/NULL columns and automation-parent links survive both clones and captured Undo/Redo. Original child IDs and external aliases stay with the original. Reversed/edge/outside/clock-collapsed selections, occupied/repeated IDs, foreign stores and stale null/non-null heads refuse; a bad appended op rolls back the entire edit. This is the bounded two-cut producer, not the later EditScope/overlap planner or arbitrary joining.

**Gate:** full GCC14 Debug/Werror/PD-on/JUCE-off build, zero warnings, and `test_all.sh` **15,926 checks / 188 suites**, every row/sum audited, zero failures and all four validators/layout clean. Added **241 checks**: core clip edits **271** (99 new), native audio **357** (106 new), native MIDI **208** (36 new); Time edits **413** and row edits **36** remain green. Eight unwarped source-rate/width/loop variants plus two Warp variants compare exact audible PCM before the split and after split/one Undo/one Redo, with nonzero source/placement offsets, cuts inside active fades and a tempo junction. Native MIDI checks explicit original/split schedules, boundary off-before-chase ordering, overlapping same-key voice ownership, tuning, exclusive endpoints and retained ANOT identities in both widths and regular/irregular callback partitions. Callback allocation and file-I/O audits remain zero. Core fixtures cover all four stored/input-clock combinations, read-only preparation, exact typed rows, optional-row absence, appended mute, atomic refusals and malformed captured packets.

**Compiler/adversarial gate:** strict GCC and Clang19 compile both changed production units and five focused test units with actual flags plus Wshadow/Werror, zero warnings. Clang passes **1,285 checks**, zero failures, using validated GCC cold dependencies. Six isolated compiled plants trigger **76 ordinary assertion failures**: omitted right image (17), lost right source phase (20, actual native PCM), omitted middle children (16), two-piece time-base validation bypass (4, otherwise SQL-valid alternate-clock rows), unshared child allocator (11), and omitted expected head only in the new overload (8). All exit normally with named test failures; no crash or exception counted. Source/core/baseline object/binary hashes remain unchanged.. Artifacts: `/workspace/adi-cloud/n17-review/`, `n17-preflight.log`, `n17-core-tests.log`, `n17-complete-build.log`, `n17-complete-build-final.log`, `n17-complete-gate.log`. No whole-project Clang, sanitizer, MSVC/macOS execution or measured Live parity claim. New native MIDI split cases use constant tempo without clip looping; crossing notes release/chase at independent clip boundaries, with no new tie behavior promised. README counts remain win-owned.

**Next immediately:** the remaining ED-01 range kernel, on `engine/range-survivor-identity` cut from refreshed origin/engine/log: keep the original root/child identities when only a tail survives, and constrain ordinary material-range reads/clears to the main lane while preserving dedicated whole-Set Time edits across all lanes. Then ED-03's EditScope/placement planner and the rest of M2 in the director's order. Continue in this same turn without waiting for PR creation or CI.

---

## 2026-11-09 — N29 checkpoint timeline gated; taking N17

**PR requested:** head `engine/history-checkpoint-timeline` at **`57fe7aa8bde039b7b261dcc278fd72c8500ffcb2`**, base `engine/log` (**`36fcfdef6a7736a5740181c44c39c7ebe903277c`**). Title: **Expose the History checkpoint base without a false undo step**. Branch pushed; win opens and merges the PR and runs MSVC. This closes N29's remaining engine integration; the window owns striking its needs row. Independent of the pending Link consumer, cut directly from origin/engine/log.

**Window handoff / PR body:** `History::readTimeline()` now returns `Timeline::baseSeq`, zero before compaction and the retained structural root's sequence afterwards. It reads the checkpoint metadata in the same read transaction, validates the retained root, and omits that root from public `steps`. The window can indicate that older steps were compacted without offering an impossible Undo. The base stays in internal ancestry, so branch heads and named/automatic snapshot anchors exactly at the base remain visible with their stored sequences; `headSeq` and a first visible step's parent may equal the base. At that head no visible step is applied and the current redo line remains usable. Broken checkpoint metadata returns an explicit error and no partial timeline. No schema, operation, navigation or retention-policy change; ENGINE-HISTORY-LIST.md and ENGINE-HISTORY-RETENTION.md document the read contract.

**Gate:** full GCC14 Debug/Werror/PD-on/JUCE-off build, zero warnings; `test_all.sh` **15,733 checks / 188 suites**, every row/sum audited, zero failures, all four validators and binary layout clean. This adds **48** assertions to the existing History list suite (**77**); compaction **49**, history **95**, snapshots **65** and policy **38** remain green. Real commits/compaction exercise base advancement, whole transactions, exact-base snapshots and fork navigation, Undo/Redo, held values, read-only reopen, zero SQL changes, opaque checkpoint/payload bytes and eight malformed metadata cases. Strict GCC and Clang19 compile history_list.cpp and all five focused test units with actual flags plus Wshadow/Werror, zero warnings; Clang passes **324 checks**, zero failures, against the validated GCC archive.

**Adversarial evidence and limits:** four isolated compiled production plants trigger **36 ordinary assertion failures**: visible structural root (8), zeroed public base (14), lost exact-base snapshot (2), severed internal base ancestry (12). Each exits normally with test failure; no crash or exception counted. Source/core/baseline object/binary hashes stay unchanged. Evidence: `/workspace/adi-cloud/n29-review/summary.json`, `n29-complete-build.log`, `n29-complete-gate.log`. The environment briefly reported starting during the full run, then returned ready with the same files and original test process; its final log is the completed original run. No whole-project Clang, sanitizer, MSVC/macOS execution or measured Live parity claim. HI-02's capped retention/stored-state changes and HI-03's moveTo remain separate queue items; README remains win-owned.

**Next immediately: N17**, the two-point split of one clip as one captured operation, with caller-known middle identity so the window can append its mute in the same transaction. New branch `engine/clip-two-point-split` from the refreshed origin/engine/log, never main. Then the M2 edit kernel in the director's order; continue in this same turn without waiting for PR creation or CI.

---

## 2026-11-08 — Native Link playback consumer gated; taking N29

**PR requested:** head `engine/link-playback` at **`228f7b300c8563149e0b102a6d08f6d8fd49dab6`**, base `engine/log` (**`5ab36bcfb5b01693c9ead28331933a21e507c49f`**). Title: **Connect Link timing to native playback**. Branch pushed; win opens and merges the PR and runs MSVC. This completes the dated Link-consumer exception in LANES §3 item 1. No main changes or GitHub API calls; this log is not part of the feature branch.

**Window/driver handoff / PR body:** `LinkPlayback` joins the SDK adapter to the real native `Session`. The single driver calls `process(io, buffer, command)` with physical output latency only; the consumer pins one graph, adds its compensation once, captures one SDK state, and splits native rendering at quantized start/stop offsets. The driver carries `Result::preparation` through its owner mailbox; the serialized document owner calls `prepare(store, packet, nowMicros)`. `setLaunchTick` chooses the next start position. Results expose waiting, start/stop offsets and phase error. Start Stop Sync remains independently optional, newer Play cancels an obsolete scheduled Stop, and late starts chase the corresponding musical position. Private performed tempo maps preserve elapsed sample time and authored project/history; device instances remain loaded. The driver never constructs graphs or reads Store/files. `ENGINE-LINK-PLAYBACK.md` specifies ownership, admission, clocks and bounds. No window or JUCE driver wiring is included.

**Coherence and continuity:** one physical callback retains its graph, clock, audio source generation, strip/send/Reference automation and device-parameter event-source pointer/lifetime through every slice, even across two concurrent owner publications and collection. Stored note identity, exact authored contents and actual instrument path govern voice adoption; this covers XC-25's held-note identity requirement. Accepted delayed MIDI events retain remaining arrival and source ownership through multiple delay hops. Fan-out releases keep ownership until all relevant delayed copies complete; partial rejection retries, dead-end Stop paths cannot hold voices, and an incomplete or canceled on/off pair cannot resurrect a voice. Source readiness is checked immediately before launch, with owner priming at the current position and loop head and a worker sequence guard against stale eviction. Phase correction adjusts private effective tempo by at most 0.5 percent; fractional loop beats do not accumulate false drift. Backward seeks and loops repair old performed-history segments at their exact musical boundary. Linear input recording stays sample-continuous.

**Full gate:** GCC 14 Debug/Werror, PD on, JUCE off; final complete build and `tools/test_all.sh` **16,095 checks / 193 suites**, every row and sum inspected, zero failures, all four validators and binary-layout check clean. Base was **15,685 / 188**; this item adds **410 checks / 5 suites**. Focused native suites: render slices **94**, MIDI continuity **109**, page readiness **41**, consumer **139**, immutable strip bindings **24**, SDK **38**; existing external tempo **96**, paired publication **39**, Main loudness **44** and MIDI controllers **90** also pass. Actual two-participant SDK runs, executed exclusively, pass **50 SDK + 147 native-consumer checks**, including real peer tempo preparation and captured start/stop timestamps reaching PCM and MIDI. Both output widths, a nonzero graph delay, distant ramp PCM against independent native Warp, recording, delayed owner preparation, repeated loops, seeks, opt-out, retained device ownership and unchanged project/history are exercised. The empty-callback meter reset remains supported; the direct-source controller fixture now acknowledges delivered ons as the real graph does, with its assertions unchanged.

**Compiler, sanitizer and adversarial gate:** strict GCC and Clang 19 compile **11 changed production units + 10 focused test units** with actual flags plus `-Wshadow -Werror`, zero warnings. Clang, ASan/UBSan and TSAN each pass **714 checks**, zero failures or sanitizer reports. Sanitizers instrument those units/tests against validated uninstrumented GCC cold dependencies; no whole-program coverage claim. Matching nothrow hooks complete test allocation probes without disabling leak/mismatch checks. Debian Clang TSAN uses its packaged shared runtime to preserve those hooks; the static-runtime duplicate-symbol attempt is not counted as a runtime gate. An overlapping network test attempt was disclosed and superseded by an exclusive **177-check TSAN Link rerun** and the exclusive GCC peer runs above. **34 isolated compiled fault plants** produce ordinary assertion failures (**238 expected failed assertions**), no crash/exception counted: 11 render/publication/source-lifetime plants, 13 MIDI identity/queue/fan-out plants, 9 consumer timing/intent/preparation plants and the deterministic stale-page-worker plant. Review artifacts are under `/workspace/adi-cloud/link-review/`, `render-slices-plants/`, `midi-plants/`, `link-consumer-plants/` and `clip-readiness-review/`; final GCC log `link-complete-gate.log`. The final core and ten GCC binaries are archived with hashes at `/workspace/adi-cloud/link-release-228f7b30/`.

**Limits and next:** performed history caps at 4096 boundaries; stale preparations refuse after two seconds, and a failed candidate retains the last valid graph. The bounded fan-out queue scan has no saturated-large-graph timing benchmark here. No hardware latency, measured Live parity, whole Clang build or MSVC/macOS execution is claimed. Link remains on the merged 3.1.5 pin; the explicit `Link-4.1.1`, Link Audio off re-pin is the separate queued XC-31/M0 item in the director's order. README counts remain win-owned. **Next immediately: N29**, expose the retained checkpoint base through the read-only timeline and omit its structural root from undo steps while preserving exact-base branch/snapshot anchors; then N17's two-point split and M2. Continue in this same turn, without waiting for PR creation or CI.

---

## 2026-11-05 — Context checkpoint: native Link playback consumer in flight, seat continues

**Continue this item; no stop or PR-ready claim.** Worktree `/workspace/wt-engine-link-playback`, branch `engine/link-playback`, clean merge base **`bac6a0d96f3070d8b7d02ed6ca81258e80f48a74`** combines three independently gated prerequisites: SDK `6c3d1b25`, external-tempo preparation `dffe13a9` and paired graph/clock publication `0f705682`. It stands at depth two above engine/log, never main. Current integration changes remain local/uncommitted until their full gate; no integration push yet.

**Concrete WIP already building/running:** GraphHost/Session scoped RenderBlock APIs retain one publication through multiple physical-buffer slices. A per-acquisition serial invalidates old handles even if the graph pointer repeats; expired calls zero both widths without dereferencing retired state. Session's block owns CallbackScope through acquisition, queries its actually pinned spec/tempo/tick/sample/latency, and advances native transport only for processed parts. Stack-only AudioIoSlice offsets both output widths, input channels and streamTimeSamples with bounds/overflow/null-channel checks. Session owner setExternalTempo updates through ordinary refresh while retaining device instances; new setExternalTempoAt preserves performed tempo before a musical boundary, replaces its future and keeps elapsed sample time continuous. Private history caps at 4096 boundaries and never enters the project. Integer tick conversion has its ordinary sub-sample quantization: the 100→200 BPM test permits two ticks while requiring the exact same sample frame. The Link SDK frame now queries a peer-aware transport launch on a copy of the same captured state, without committing a phase change; launchBeat is an explicit query in LinkBuffer.

**Actual consumer:** new engine/link_playback.hpp/.cpp owns no threads/Store on callback. One driver combines physical output latency with the pinned graph's compensation, captures the actual SDK frame, retains native graph identity across launch/stop slices, honors independent Start Stop Sync, returns owned preparation/status packets, and leaves graph construction to owner prepare(). Owner packets have a stale-time guard; constant reset serves a start while performed-boundary updates serve live tempo changes. The public processCaptured path is a consumer of an already captured frame (used by controlled native timing tests); process() is the actual SDK path and needs the next real-network test. No UI wiring, whole-output resampling or completed continuous Live-follow claim.

**Evidence so far, not a final gate:** GCC Werror core and focused native tests pass. `adi_render_slices_tests` **31** proves actual concurrent owner publication between 17/47-frame slices, stable old graph/clock/format through both parts, new format next callback, expired handle refusal, retained **actual one device instance/load**, bounds/pointer/stream-clock handling, callback zero allocation/free/file I/O, and a real armed input recording across slicing plus a 100→200 BPM performed boundary: **all 128 stereo input frames** are bit-exact in the actual WAV with continuous project placement. The fixture initially omitted record arm, and a too-strict inverse-tick equality was corrected to native tick quantization; neither old failure is a gate. Merged prerequisite suites passed **39 paired + 96 external + 35 SDK** before the latest additions; newer basic source query/core builds also pass slices31 + SDK35. New `adi_link_playback_tests` **26** uses controlled captured frames with actual Session Warp audio, native MIDI/instrument, both widths, a frame-17 start, frame-23 stop of the following buffer, exact audible/on/off boundaries, callback metrics, opt-out, no project/history changes, owner preparation/device preservation, stale owner rejection and late start/seek/chase. This is meaningful native signal/event evidence but **not a substitute for an actual SDK-peer consumer test**. Latest logs `/tmp/link-playback-native-build.log`, native.log, history-test3.log, coordinator-build.log, coordinator-slices.log, coordinator-sdk.log, prerequisites.log; source integration has no Clang/fault/TSAN/full gate yet. CMake adds render_slices and link_playback tests; no source under UI/DSP/plugins was edited.

**Next immediately:** extend test_link_playback.cpp with optional --peers actual SDK→native consumer integration, run outside the restricted no-route sandbox as already approved for SDK tests (default test_all must not silently enable networking). Poll two real participants, establish tempo, prepare owner generation, trigger real peer start/stop, and compare actual native event/PCM boundary against that exact frame's launch time with graph+hardware latency. Then add actual held-note tempo-change tests. Current MidiClips increments generation on every rebuild and flushes/re-chases all voices; preserve active voice identity across a true clock-only rebuild, with authored identity validation, bounded owner-built lookup and proper off/controller retry semantics, rather than hiding that retrigger. Do not simply resample the whole mix; fixed unwarped clocks are independent. Stable-rate drift/phase correction, disabling Link back to authored tempo, seek/loop and candidate page readiness before live publication still need explicit behavior/tests. Current consumer reports phaseErrorFrames and avoids a seek for two-frame tick rounding; it does not yet implement full continuous drift correction. Keep that limit visible until corrected. A clock change must preserve linear recording time: use a performed tempo event at the current musical boundary rather than resetting the map from zero and jumping elapsed sample time. Update/reset history on a new start appropriately. Finish native signal/lifecycle gates, then full GCC/Werror/test_all/validators, focused Clang, normal-failure fault plants and relevant sanitizer stress; push/log/next. D11 latency switches and remaining brief follow; model weights/Drive handover remain separately named dependencies. This checkpoint is only a log entry. Continue in the same turn through compaction, never end to report or ask.

---

## 2026-11-05 — Paired graph/transport publication gated; taking native Link playback integration

**PR requested:** head `engine/transport-generation` at **`0f7056827cb41a5763ea19c3e3237e51ce59f7a2`**, base `engine/log` (`ec9a4a7b`). Title: **Publish transport metadata with the graph that actually renders it**. Independent of SDK/tempo preparation; final integration can combine all three prerequisites at depth two. Win opens the PR; main remains untouched.

**Window/host handoff / PR body:** GraphHost's optional owned `GraphTransportState` is part of the PublishedGraph itself. One acquired audio publication adopts history, announces its identity, invokes that graph's beginBlock exactly once, then passes its matching metadata to every actual node. There is no independent clock acquisition that can race to a different generation. Failed graph preparation publishes neither graph nor clock. DeviceHost's RebuildSpec forwards the context through initial and shape/coalescer builds. `renderedTransportState()` is single-audio-reader inspection after process and before the next process, protected by the actual last-rendered publication; future owner publications cannot change that value early. Omitted state retains the caller's original AudioIo transport. Graph destruction precedes its retained clock's destruction, and owner reclamation retires both together, including unheard/skipped generations. Callback access is raw-pointer only: no shared_ptr copy/release, allocation/free, SQL or I/O. The provider's beginBlock has the normal noexcept/no-lock/no-I/O callback contract. Session prepares its TimingView inside this graph context, removes its separate clock publisher/collector, and uses the actually rendered state for node BPM, loop application and metronome/post-graph consumers. A safe type query accepts a caller-installed foreign graph context without interpreting it as native metronome storage. ENGINE-TRANSPORT-GENERATION.md documents ownership/lifetime/thread boundaries. No persisted setting, schema/opcode or UI change; no Link network/tempo/start-stop implementation claimed yet.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **14,119 checks / 168 suites**, every row/sum inspected, zero failures, validators/layout clean. Integrated base 14,080/167; this item adds **39**. Actual graph nodes/PCM at both widths prove context identity, exactly one begin, multiple unheard publications, last-rendered accessor after future publications, old-reader protection, exact owner retirement, refusal retaining the old complete pair, legacy caller metadata and safe foreign context. Real DeviceHost shape-epoch/coalescer rebuilding retains the clock. Native Session render runs concurrently with **200 actual Store tempo edits/refreshes per width**, and explicitly proves both tagged source generations were heard, every source's real node BPM matches its actual graph and callback allocation/free/file I/O remains zero. Existing host **124**, transport-info **131**, transport-settings/click **49**, Session **214**, cue **96** passed a focused preflight and remain green in the final full gate. Focused Clang19 compiles all three production CPP units (host, device_host, Session) and final test with actual conversion/Wshadow flags, and passes **39** against validated GCC cold dependencies. Four actual GCC Ninja Wshadow checks pass. Initial 35/37-check baselines are superseded by the final 39 including nonvacuous dual-generation observation and deterministic last-rendered identity.

**Adversarial/concurrency evidence:** **9 compiled plants**, normal assertion failures: graph context not retained (19), DeviceHost forwarding missing (4), native Session attachment missing (2), clock applied after DSP (13), accessor takes newest owner publication rather than last-rendered graph (2), accessor loses rendered context (9), clock begun twice (4), native clock BPM shifted (2), and legacy caller metadata discarded (2). GCC TSAN instruments all three changed production units and the complete 39-check native suite, including concurrent real Session refresh/render: **39 checks, zero reports**, strict warning flags retained without compile-warning demotion or runtime suppression. Cold dependencies/remaining engine are not claimed wholly instrumented. Review `/workspace/engine-review/transport-generation-review`, script transport-generation-review.py; logs `/tmp/transport-generation-full-build.log`, full-gate.log, focused3.log, legacy.log, clang.log, shadow.log, plants.log, tsan.log (transport-generation- prefix each). No whole Clang/MSVC/macOS/hardware/measured Live claim; README remains win-owned.

**Next immediately / integration workspace:** `/workspace/wt-engine-link-playback`, branch `engine/link-playback`, starts at `engine/log` (`37e22468`). It already cleanly merges the actual SDK prerequisite `6c3d1b25` and external-tempo prerequisite `dffe13a9`; current merge head `7ebb7e11`. Merge this paired-publication prerequisite next, then build the actual native transport/playback consumer. No integration gate/ready claim yet. Required direction: pinned graph generation across sub-buffer processing for exact quantum launch/stop; derive first audible time from Link's monotonic clock plus physical output latency; immutable actual prepared clock drives Warp/MIDI/automation and loop conversion; owner preparation/rebuild never occurs on callback and preserves device instances. AudioIo currently has raw audio pointers/frame count/streamTimeSamples and no MIDI-event span, so sub-buffer pointer/count/stream-clock handling needs actual source/recording validation. A callback must not acquire a different graph halfway through a split buffer. Current Session::spec() is owner inspection only; actual callback preparation identity must come from the pinned graph clock, not a racing owner field. No whole-output resampling shortcut: unwarped fixed source clocks have independent semantics.

**Known integration work to prove, not hide:** MidiClips currently increments its generation on every rebuild and releases/re-chases held notes on that transition; do not claim seamless Live held-note behavior without an actual held-note tempo-change test and the necessary correction. Rebuilding audio source readers also needs ready pages for the desired musical position before publication, with the old valid graph retained during owner preparation. SDK beat/phase magnitudes are local while quantum phase is shared, transport intent may arrive for a past/future timestamp, and ordinary host timestamp/rate drift must be distinguished from an explicit locate. Preserve native monitoring/input/recording semantics across any slice. Finish and gate concrete native launch/stop/follow behavior, then D11 Keep Monitoring Latency/Reduced Latency/Delay Compensation switches and the rest of the brief. Existing model-weight/Drive handover dependencies remain named open, not a seat stop. Continue this same turn through compaction; this is a gate/log checkpoint, not an end.

---

## 2026-11-05 — Coherent native external-tempo preparation gated; taking paired graph/transport publication

**PR requested:** head `engine/external-tempo` at **`dffe13a90fe2593e9a8ff9e3fe10b00807ee1cee`**, base `engine/log` (`361f2807`). Title: **Prepare coherent external tempo generations without changing authored project clocks**. Independent of the actual Link SDK adapter (`engine/link-session`), designed to merge as a separate prerequisite rather than stacking three deep. Win opens the PR; main remains untouched.

**Window/owner handoff / PR body:** an owner-selected `SessionSpec::externalTempoBpm` prepares a constant-tempo performance generation without changing authored tempo automation, native rows, history or the raw `Session::model()` projection. Its private playback model supplies the same tempo to audio/Warp preparation, MIDI placement, automation compilation, instrument TransportInfo and saved musical-loop conversion. Driver rate re-preparation and refresh retain that selection even when authored tempo events change. Omitting it keeps normal authored-tempo behavior. Finite 20..999 BPM and performanceState=true are required; accidental offline use refuses, leaving a failed preflight available for valid retry. Absolute-time fingerprinted frozen caches explicitly require unfreezing rather than silently playing the wrong musical timing. Session load now refuses callback entry before diagnostics, allocation/free, SQL or I/O. ENGINE-EXTERNAL-TEMPO.md specifies that the option belongs to an immutable prepared generation, not a callback tempo mutation or complete continuous Link implementation. No UI/schema/opcode change, automatic Link network/rebuild queue or whole-output resampling.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **14,176 checks / 168 suites**, all rows/sum inspected, zero failures, validators/layout clean. Integrated base 14,080/167; this native integration adds **96**. Actual Session Re-Pitch PCM plus fader automation match an otherwise identical native project authored at 60 BPM while the source preserves 120/240 BPM events. Both widths and 44.1/48 kHz prove one-second placement, two-second Warp duration, exact native note-on at 1.25 seconds/off at 1.75, instrument transport BPM past the authored change and ready callback PCM. Actual driver changes 44.1↔48 kHz preserve those note boundaries and audio. Refresh after authored BPM changes to 333 still matches the constant-tempo reference; a native musical-loop op and subsequent refresh wrap at the external tempo's exact sample window and repeat matching audio. Digest, SQLite total_changes and journal count prove preparation/playback leave project state untouched. Invalid values/offline use and later valid retry are checked. A native SQLite statement trace plus new/delete/file-I/O counters prove callback load refusal and unchanged diagnostics. An actual Freeze capture/prepare/commit installs a valid fingerprinted cache, then external load refuses without changing it. Initial fixture compile error (HashResult rather than its checked .hex) and missing NOT NULL fixture hash were corrected before the 96-check gate; neither is counted as product success. Focused Clang19 compiles Session production and the final workflow test with actual conversion/Wshadow flags and passes **96** against validated GCC cold dependencies. Both actual GCC Ninja Wshadow checks pass.

**Adversarial evidence:** **10 compiled plants**, normal assertion failures only: clock display/loop uses authored tempo (12), playback override removed (32), MIDI alone uses authored tempo (12), automation alone uses authored tempo (16), Warp alone uses authored tempo (20), private override also mutates raw model (4), stale frozen audio admitted (1), offline authority admitted (1), 1000 BPM admitted beyond the chosen contract (2), and callback load admitted (1). These separate consumer mutants prove actual timing/audio/event consumption rather than a DTO or one BPM field. Review `/workspace/engine-review/external-tempo-review`, script external-tempo-review.py; logs `/tmp/external-tempo-full-build.log`, full-gate.log, focused4.log, clang.log, shadow.log and plants.log (external-tempo- prefix on each). No new thread ownership or sanitizer claim here; existing live graph/transport tests remain in the full gate. No whole Clang/MSVC/macOS/hardware/measured Live claim; README remains win-owned.

**Next immediately / necessary live-follow publication fix:** static prepared generation coherence does not alone make a live clock switch atomic. Existing Session separately publishes the prepared graph and TimingView; an intervening audio callback could acquire one generation's musical resources and another's metadata. Before native Link following, bind the clock to the graph's own publication. Independent worktree `/workspace/wt-engine-transport-generation`, branch `engine/transport-generation`, base `engine/log` (`ec9a4a7b`), already has local WIP: a graph-owned `GraphTransportState`, optional owner context passed through DeviceHost's RebuildSpec, and GraphHost's one acquired publication invokes its matching beginBlock before nodes. Session builds a shared TimingView into that graph context, removes the separate clock publisher, and uses the actually rendered context for metronome/post-graph consumers. Context retires with the graph off callback; no shared_ptr operations occur on the callback. Core build is in progress, **not gated/ready**. Write real graph/clock identity, failed publication, skipped generations/lifetime, coalescer and concurrent Session tests, preserving legacy host/transport/click behavior and zero callback allocation/free/locks/I/O; then gate/push/log it. This is an independent prerequisite so final Link playback can combine SDK, tempo generation and paired publication at depth two, preserving device instances and generation lifetimes rather than changing only a displayed BPM. Then implement quantized native transport scheduling/following and continue D11 monitoring/delay switches and the remaining brief. Earlier model-weight/Drive handover blockers remain named dependencies, not a seat stop. Continue the same turn through compaction; this gate/log entry is not a stop.

---

## 2026-11-05 — Actual Link SDK adapter gated; taking coherent external-tempo playback generations

**PR requested:** head `engine/link-session` at **`6c3d1b254f9beb2eee6e4fa26ae3817a86594bf4`**, base `engine/log` (`0fbc1a7a`). Title: **Integrate pinned Ableton Link SDK through an audited audio driver boundary**. Independent of sample settings/analysed/progressive branches. Win opens the PR; no main checkout/edit or PR merge by this seat.

**Window/driver handoff / PR body:** `engine::LinkSession::create(initialTempo, error)` owns an actual SDK participant, whose private implementation exposes no third-party headers. Networking starts off; owner `enable(true)` explicitly opens it, while `enableStartStopSync(true)` separately selects transport sharing. A single audio driver calls `process(LinkBuffer, LinkCommand)` to capture one SDK audio snapshot, optionally edit tempo/transport/requested beat, observe it at callback-clock time plus physical output latency, then commit through the SDK audio API. Returned owned `LinkFrame` carries actual beat/phase/tempo/peers, enabled/start-stop flags, transport intent/change time, audible time and a peer-aware requested launch timestamp. Invalid bounds refuse the whole command before any SDK mutation; owner construction/configuration refuses callback use without diagnostic mutation. No blocking app-state API is used. The owner retains the participant through its last driver callback and retires it off callback. ENGINE-LINK.md gives the handoff and the distinction between future launch and immediate play intent.

**Dependency and bounds:** official upstream tag **Link-3.1.5 / 45931b8bd2a7066f1950f2a0beab81a0c51f28fe**, LICENSE.md explicitly GPL-2.0-or-later; its Asio gitlink **231cb29bab30f82712fcd54faaea42424cc6e710**, Boost-1.0 license read. fetch_external.sh --build-only successfully verifies all pins and fetches the exact submodule. CMake requires both header trees, privately links the SDK target and keeps dependency headers SYSTEM. EXTERNAL-CODE.md moves Link from reference-only to compiled dependency and corrects the old one-submodule description. No third-party source patch, Link Audio, PTP/sample-clock implementation or UI/schema/opcode change. Adapter bounds: finite 20..999 BPM, zero tempo command meaning leave alone, quantum 1/1024..256, finite beat requests +/-1e9, callback time +/-2^50 us and nonnegative output latency up to ten seconds. SDK transport intent is separate from scheduled launch; its musical timeline keeps advancing while stopped. Beat magnitudes can differ between peers while phase synchronizes. Network BPM uses integer microseconds-per-beat precision.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **14,115 checks / 168 suites**, all rows/sum inspected, zero failures and all validators/layout clean. The refreshed integrated topic base contributes 14,080/167; this adapter adds **35**. Default native tests use the actual SDK with networking off: invalid construction/commands, real monotonic clock, first-audible timestamp, tempo continuity, stopped timeline, negative phase, owner/driver boundaries and 10,000 actual capture/edit/commit callbacks with zero allocation/free/file I/O/direct linked pthread mutex lock or trylock. Linux lock wrapping audits calls in linked code, not hidden locks inside shared system libraries. Focused Clang19 compiles production and final test with actual conversion/Wshadow flags and passes **35** against validated GCC cold dependencies; both actual GCC Ninja Wshadow checks pass.

**Actual network evidence:** final GCC and focused Clang executables each pass **47 checks** with `--peers`, explicitly enabling two actual participants: real multicast discovery (one peer each), shared tempo and phase at the same audible timestamp, future quantum launch with retained running shared phase, Start Stop Sync delivery, opt-out independence, and 20,000 enabled-peer captures with zero audited allocation/free/file I/O/direct mutex calls. The ordinary restricted sandbox exposes only an inactive loopback/no routes and fails discovery (37 checks/1 failure); it is not counted as a product success or silently skipped. Automatic review allowed the local peer test outside that restriction, where discovery works. The first unrestricted test's exact-BPM tolerance failed because SDK wire tempo is quantized to integer us/beat; corrected to 0.001 BPM, final GCC/Clang both pass 47. Logs `/tmp/link-session-peers.log`, peers-final.log, gcc-peers-final.log and clang-peers.log retain the distinction (all with link-session- prefix). No physical network/hardware or measured Live claim.

**Adversarial/concurrency evidence:** **10 compiled plants**, normal assertion failures: blocking application capture (4), application commit (1), output latency omitted/doubled (1 each), audio commit omitted (3), unsupported quantum admitted (2), invalid transport enum admitted (2), callback construction admitted (1), callback configuration admitted (1), and shared phase rudely forced at launch (1, actual two-peer 47-check executable). Review `/workspace/engine-review/link-session-review`, scripts link-session-review.py/link-session-tsan.py; logs `/tmp/link-session-plants.log`, plant-phase.log, clang.log, shadow.log, full-build.log, full-gate.log and fetch.log. Focused GCC TSAN instruments the adapter **and SDK header implementation**, 30,000 actual audio commands concurrent with owner start-stop/network-off configuration: **2 checks, zero reports**, 32 actual owner configuration passes. GCC initially refuses compilation at Werror because its TSAN cannot model Asio atomic_thread_fence; only the external sanitizer harness demotes `-Wtsan` to a retained warning (`-Wno-error=tsan`), without changing production flags/vendor sources or suppressing runtime reports. `/tmp/link-session-tsan2.log` retains those unsupported-fence warnings and the successful stress. This is limited observational TSAN evidence, not proof of Asio fence protocols or whole-SDK race freedom. Original compile failure is not a passed test. No whole Clang/MSVC/macOS/hardware claim; README remains win-owned.

**Next immediately / no false D11 completion:** the adapter alone does not change native Session tempo, prepared Warp/MIDI/automation, transport scheduling or settings. New independent worktree `/workspace/wt-engine-external-tempo`, branch `engine/external-tempo`, base `engine/log` (`361f2807`), prepares the required coherent generation without depending on this SDK PR. Current local WIP adds optional owner-selected SessionSpec externalTempoBpm, preserves raw authored model/store rows, replaces only the private playback model's tempo, and builds TimingView from that same playback tempo. Existing Warp, MIDI and automation already compile from that private owned model, so use the same tempo instead of replacing only the displayed BPM. Offline use/invalid bounds refuse at load; existing frozen absolute-time caches explicitly need unfreezing rather than silently playing stale rendered timing. Core build is in progress, tests not yet written/gated; no ready claim. Finish native playback/clock/loop/automation tests at both widths/rates, refresh/prepare/project immutability, failure and callback boundaries, then gate/push/log it. Combine the independent SDK and coherent-tempo prerequisites for native quantized transport/playback integration at depth two; then D11 monitoring/delay switches and the remaining brief. Model weights and Drive clone handover remain their recorded open dependencies, not a seat stop. Continue in this same turn through compaction; a checkpoint is a log entry, not a reason to end.

---

## 2026-11-05 — Native sample Save/Recall gated; taking the Link SDK audio-driver adapter

**PR requested:** head `engine/sample-defaults` at **`02bf7226`**, base `engine/sample-settings` (`28f6e2ab`). Title: **Save native sample defaults and recall them through material imports**. Explicit dependency on the persistent authored-settings library, depth two; independent of analysed/progressive decode branches. Win opens the PR and merges the prerequisite first; main remains untouched.

**Window/worker handoff / PR body:** on the serialized Store owner, `clip_edit::captureSampleDefaults(store, clipId, referenceRate)` captures an actual audio clip and complete source identity/dimensions in one read transaction. `saveSampleDefaults(..., libraryIndex, error)` Saves under its native BLAKE3 without changing project rows/history, even from a read-only project. It reuses `copyClip` source/fade freezing, retains future AWRP bytes exactly, and folds a legacy unwarped musical End into fixed source time without extending an earlier pin. The explicit four-argument `prepareImportAudio(..., libraryIndex)` Recalls authored settings by the freshly imported/deduplicated source hash, requires exact source dimensions and refuses unsupported/invalid data instead of silently discarding user choices. Authored Warp-off overrides automatic Warp analysis. Missing defaults retain ordinary import; the existing three-argument API deliberately consults no library. Existing native clip/media operations carry owned settings, so library closure/changes after preparation cannot affect commit, undo/redo, CBOR replay or Session playback. Requested placement/ids/track survive; intrinsic source/loop extent defines the initial rectangle. Normal stale-head/source-store checks, staged copy publication, transaction rollback/retry and one-group history remain native material responsibilities. ENGINE-SAMPLE-DEFAULTS.md gives the callable contract. No UI, schema or opcode change.

**Source clocks and boundaries:** unwarped offset/loop/End/fade pins keep their source times at another tempo; legacy handle coordinates are re-expressed for the destination tempo, and Ns-only loops receive usable fallback handles. Warp projects authored musical clocks at the destination tempo. Signature codec bounds now match the existing native schema (numerator 1..255, power-of-two denominator 1..128), correcting the prerequisite's overly narrow 64/64 limit. Direct aliases require existing alias resolution; clip/device automation refuses as nonportable; unknown native audio/playback/fade/property fields refuse rather than drop authored controls. Name/color/placement/local scale context are project choices. Save requires an existing complete native media identity but can work offline; it does not physically rehash an unavailable source. Import retains actual source verification. The application owns its library/lifetime; no UI/application-lifecycle wiring or measured Live parity is claimed.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **13,804 checks / 159 suites**, all rows and sum inspected, zero failures and all validators/layout clean. This integration adds **65**; inherited sample-settings 53, library 48 and material commands 29 remain green. Actual native stores exercise read-only Save, cache cleanup/library reopen, library closure before commit, source trims, native signature extremes, two tempos, 44.1/48 kHz and both precisions, curved fade PCM equality, Ns-only loop repetition, fixed source End, future Warp bytes and Re-Pitch at half frequency/double duration. Actual history undo/redo and journal CBOR replay work with no library. Late SQL COMMIT failure rolls back clip/media/ops, retires only the newly published copy and permits the same captured import to retry; source and persistent settings survive. Deduplicated existing-media Recall and opaque future annotation preservation are tested. Callback Save/capture/import refuses with zero allocation/free/file I/O and unchanged caller diagnostic; actual Session render callbacks are audited too. Focused Clang19 compiles all three changed production CPP units plus the final workflow test with actual conversion/Wshadow flags and passes **65** against validated GCC cold dependencies. Four actual GCC Ninja Wshadow checks pass, final test repeated.

**Adversarial evidence:** **11 compiled plants**, each normal assertion failures: capture callback (1), Save callback (1), import callback (1), source dimensions (1), unknown source fields (1), future AWRP normalized/lost (1), End source clock shifted (3), source start discarded (11), gain discarded (7), import consumer skips Recall entirely (15), unsupported authored version silently ignored (1). The first no-consumer run escaped through a missing-row fixture exception; the fixture now asserts missing signature/map attachments safely and the final plant fails normally. That initial exception is not counted. Review `/workspace/engine-review/sample-defaults-review`, script sample-defaults-review.py; logs `/tmp/sample-defaults-full-build.log`, full-gate.log, final-focused.log, clang.log, clang-final-test.log, shadow.log, shadow-final-test.log, plants.log and plants-last.log (all with sample-defaults- prefix). No new cross-thread ownership was introduced here; the prior library stress evidence and SQLite-WAL caveat remain as recorded, without a new blanket TSAN claim. No whole Clang/MSVC/macOS/hardware/Live claim; README remains win-owned.

**Remote topic integration / next immediately:** win advanced remote engine/log to `0fbc1a7a` while this gate ran, bringing its main/stack merge and the corrected second-issue Linux prompt. Fast-forwarded only the engine/log worktree; no main checkout or direct main edit. The next independent worktree `/workspace/wt-engine-link-session`, branch `engine/link-session`, starts at that topic head. Official upstream Link-3.1.5 resolves to `45931b8bd2a7066f1950f2a0beab81a0c51f28fe`; LICENSE.md explicitly GPL-2.0-or-later, and its Asio gitlink is `231cb29bab30f82712fcd54faaea42424cc6e710`. Both fetched; the SDK is not copied from reference and Link Audio remains outside scope. Local WIP adds a private SDK adapter and reproducible fetch/CMake pin: networking defaults off, owner enable/start-stop controls, one audio capture/edit/observe/commit snapshot at first audible buffer time, bounded commands and returned beat/phase/transport/quantized launch timestamp. Core build and actual-SDK tests are in progress, **not gated or PR-ready**. Finish the SDK adapter gate, then its native transport integration and remaining D11 latency switches before advancing the brief. Clock sync is not PTP/audio streaming or project collaboration. Model weights and actual Drive clone handover remain specifically open, not a reason to stop this seat. Gate, push, log, next continues in this same turn through compaction.

---

## 2026-11-05 — Authored sample-settings library gated; taking native Save/Recall/import integration

**PR requested:** head `engine/sample-settings` at **`28f6e2abdb1537a0a9182dd92e0c5d6339bde1f7`**, base `engine/log` (`de697f8e`). Title: **Persist authored sample Warp and clip defaults in the native library**. Independent of analysed/progressive branches; existing library SQLite version remains 1, no project schema/opcode change. Win opens the PR; main remains untouched.

**Window/worker handoff / PR body:** the application-scoped persistent `library::Index` now offers owned `sampleSettings(hash)`, `saveSampleSettings(hash, settings, error)` and explicit `clearSampleSettings(hash, error)`. Source hash is the identity, never path, mounted volume or a file-index row. Authored source/window, Warp mode/enable and original AWRP bytes, pitch/formant/gain/channel/reverse, mute/loop/content/end clocks, musical/ns fades/curves/tensions and time signature live alongside tags/BPM/rating in the existing annotations JSON. No cache cleanup path touches that data database. Native AWRP coverage/order validates against the source window, and readable future record tails/version/trailer survive **byte-for-byte** rather than being re-encoded from known fields. Unsupported existing settings refuse overwrite; unrelated tag updates and export preserve their opaque JSON. Explicit clear removes only that hash's defaults. Save/import/clear use immediate read/merge/write transactions, preventing concurrent annotation updates from being silently lost; identical saves perform no persistent update. No UI or audio-renderer change is claimed from storage.

**Exchange/bounds:** legacy-only export stays `{version:1, media:...}`; records containing sample_settings export version 2. Import accepts both, and authored settings require envelope 2. Codec version 1 has 34 exact fields, strict integers/booleans/finite numeric values, source-window bounds and canonical nullable clocks. Original marker bytes use lowercase hex (32 MiB raw maximum); annotation size is bounded to twice that plus 64 KiB. A bounded SAX pass checks depth 16, 16 million events, 256-byte keys, strings and 256 MiB documents **before** DOM parsing, rejecting duplicate/escaped-duplicate keys. A bad later row rolls back earlier real changes; future data never silently drops. Stored excessive/deep data refuses editing without deletion. New settings/exchange callbacks refuse before allocation/free/SQL/I/O/locking or diagnostic mutation. ENGINE-SAMPLE-SETTINGS.md specifies ownership and limits. Existing library indexing/lifecycle keep their separate worker contract; database version/application-id guards remain unchanged.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **13,739 checks / 158 suites**, all rows/sum inspected, zero failures, validators/layout clean. This independent item adds **53**; legacy library remains **48**. Settings round trips cover all nondefault controls and exact doubles; native text exchange/reopen/path-independent copy, unknown version/fields, future native bytes, unsigned overflow/float coercion, actual out-of-source window, malformed Warp coverage, source-clock pairs, nonfinite values, idempotent no-write data_version, callback metrics, future-data tag merges/refused overwrite/explicit clear, late SQL trigger failure/retry, and invalid later imports with an **actually changed earlier rating** are exercised. A deep stored future-metadata field that the typed loader otherwise accepts proves the SAX depth guard independently of the exchange envelope check. Focused Clang19 compiles both production CPP units and both tests with actual conversion/Wshadow flags and passes **53 + 48** against validated GCC cold dependencies. Four actual GCC Ninja Wshadow checks pass; a pre-existing legacy lambda's stop parameter was renamed token to remove its real shadow warning. Final focused tests also assert callback refusal throws nothing; initial fault-harness compile/escaping-exception failures were not counted as caught plants.

**Adversarial evidence:** **17 compiled plants**, normal assertion failures only: encode/decode/validate callback guards (1 each), native load callback (1), save admission plus redundant validator refusal (1), clear callback (2), export/import callback (1 each), future authored overwrite (2), future AWRP normalization losing bytes (1), source window past EOF (1), integer coercion (3), unknown settings fields discarded (1), depth guard (1), duplicate JSON (2), import transaction removed (4), path-shaped hash admitted (1). Review `/workspace/engine-review/sample-settings-review`, scripts sample-settings-review.py/sample-settings-tsan.py; logs `/tmp/sample-settings-plants-complete.log`, `/tmp/sample-settings-clang-final.log`, `/tmp/sample-settings-clang-legacy.log`, `/tmp/sample-settings-shadow-final.log`, `/tmp/sample-settings-gate-*`, `/tmp/sample-settings-final-*`.

**Concurrency evidence/limit explicitly:** one application Index under GCC TSAN, instrumenting the new codec/Index with concurrent settings/tag/read threads: **2 checks, zero reports**, final authored values/tags and reopen preserved. The separate **two-connection functional stress** also passes 2, retaining all 32 tag updates and final settings. The first two-connections-in-one-process TSAN run reports the exact upstream-documented WAL memcpy pair walIndexWriteHdr/walIndexTryHdr in library.sqlite-shm, not an ADI object: bundled sqlite3.c **tag-20200519-1** explicitly describes this false-positive double-read/barrier protocol (its no_sanitize_thread annotation is Clang-only). The original report `/tmp/sample-settings-tsan.log` is retained. No suppression, vendor patch or SQLite race-free claim was introduced: `/tmp/sample-settings-tsan-one-index.log` is the app's one-Index serialized C++ scope; `/tmp/sample-settings-two-connections.log` is functional native SQLite stress. No whole Clang/MSVC/macOS/hardware/Live parity claim; README remains win-owned.

**Next immediately — storage is not a completed Save workflow:** worktree `/workspace/wt-engine-sample-defaults`, branch `engine/sample-defaults`, based on this feature at `28f6e2ab` (depth two). Connect an actual native clip to authored capture/Save and an actual new import to Recall, with correct source identity/window, immutable captured settings, normal native transaction/undo/CBOR replay and actual Session playback at both widths, including another tempo/rate and native cache cleanup/reopen. Reuse `copyClip`'s existing source-clock/fade freezing instead of inventing tempo conversion: clip_commands.cpp freezeAudio/freezeWarpFades already pin legacy source domains and retain native future bytes. Keep placement/project identities/device automation out of sample defaults, preserve unsupported user data and document boundaries; no UI edit. Then Link and the remaining brief queue. Model weights/Drive backend handover remain specifically open from prior entries. This is a gate/log checkpoint, not a stop: continue the same turn through compaction.

---

## 2026-11-05 — Central analysed cache gated; taking persistent sample Warp/clip defaults

**PR requested:** head `engine/analysed-cache` at **`90e71ff2a4e1d79a59d1f638fd5be6c4a77e00a8`**, base `engine/log` (`71b080b5`). Title: **Keep per-sample waveform analysis in the shared analysed folder**. Independent of PCM/AAC/progressive playback and the scanner/stem stacks. Win opens the PR; main stays untouched.

**Window/worker handoff / PR body:** `media::AnalysisCache` now holds one `cache/analysed/<source BLAKE3>.adia` per sample, with bounded versioned/configured named sections in that single file. The unchanged normal `buildMediaPeaks` API uses the process folder; its new overload accepts an explicit cache or nullptr to disable central persistence. Legacy pool BLOBs remain readable/opaque and are never overwritten; new waveform results persist centrally without pool/op/history writes. Matching content/dimensions can draw a read-only project's missing source from the central APKS record. New building still hashes/resolves/reads/rechecks the source. Existing valid cached resolution can be reused, as with the pool cache. Missing, malformed or unwritable optional cache is rebuildable; even serialization allocation failure after computing the bins preserves the successful waveform. A stopped persistence request remains cancelled. Portable ADI_HOME/default-folder routing and the old API callback boundary are tested, rather than claiming integration from only a custom-cache DTO.

**Format/publication/cleanup:** explicit LE ADIA v1, 168-byte header, exact source hash/rate/channels/frames and body length/digest; 0..32 unique four-letter sections, 32 MiB total file limit, bounded first pass before payload copies, no recursive encoding. APKS embeds the existing validated peak codec; unknown sections survive same-process updates. All callback entry points refuse before allocation/free/SQL/file I/O/locking and preserve caller diagnostics. A writer reserves an owner-only private directory, writes/flushes/closes a complete record, then `media::replaceCacheFile` atomically replaces on the same volume through the existing platform adapter, with no partial-copy fallback. Failed/cancelled private files retire without touching previous complete data. Removal accepts one lowercase hash and removes only its actual regular .adia file; no source, sidecar, library or project removal. Empty/unavailable app-data roots never become relative cache writes. ENGINE-ANALYSED-CACHE.md and docs/format/ANALYSIS-CACHE.md specify the contract. No project schema/opcode change and no UI edits.

**Gate:** final GCC14 Debug/Werror/PD-on/JUCE-off build and `test_all.sh` **13,729 checks / 158 suites**, all rows and sum inspected, zero failures, validators/layout clean. This independent branch adds **43** to win's integrated 13,686-check base; legacy peaks remain **33**. Tests cover exact bytes and round trips, truncated/oversized/reserved/version/digest/duplicate records, identity/dimension mismatch, unknown section retention, corrupt-record rebuilding, pre-cancellation, owner-only staging retirement, one file per sample, cleanup isolation, read-only/missing-source waveform reuse, no native-history/pool writes, no adjacent .asd file, controlled cache-serialization bad_alloc with retained bins/recovery, and zero callback allocation/free/actual I/O, including the legacy wrapper before singleton creation. Focused Clang19 compiles the three changed production CPP units and both test units with actual conversion/Wshadow flags: **43 + 33** pass against validated GCC cold dependencies. Five actual GCC Ninja Wshadow checks pass, with the final new test repeated. No whole Clang/MSVC/macOS/hardware/Live claim; README remains win-owned.

**Adversarial evidence:** **15 compiled final-source plants**, each a normal assertion failure (no crash/exception counted): encode callback (1), decode callback (1), read callback (1), write callback (1), remove callback (1), payload digest (2), duplicate section (1), source identity (1), unknown section retention (1), reserved header (1), read cancellation (1), write cancellation including its redundant lookup checkpoint (1), waveform consumer ignoring shared cache (1), serialization failure treated as mandatory (1), and legacy-wrapper callback before default initialization (1). Focused GCC TSAN instruments cache, peak consumer, hash and publication: **2 checks, zero races**; two cache instances/two writers/two readers preserve complete records, both final sections and per-sample cleanup. Review artifacts `/workspace/engine-review/analysis-review`, scripts analysis-review.py/analysis-tsan.py; logs `/tmp/analysis-final-*`, `/tmp/analysis-clang-final.log`, `/tmp/analysis-clang-legacy.log`, `/tmp/analysis-plants-final.log`, `/tmp/analysis-tsan2.log`, `/tmp/analysis-shadow*.log`. Initial 41-check full gate is superseded by the 43-check default-route gate. Syntax-only preflights and a corrected test-harness formatting compile failure are not counted as passed runtime checks.

**Boundaries, not deferred user data:** this is rebuildable analysis only. Authoritative Warp/clip defaults must never enter it. Same-process operations serialize; concurrent applications atomically replace complete snapshots and may cause optional sections to need rebuilding (no cross-process merge transaction claim). No automatic analysis LRU/orphan reaper, cached transient algorithm or saved sample-settings implementation is claimed. Cache body digests/expected identities are corruption checks for an app-owned cache, not hostile-owner authentication. POSIX native replacement runs here; Windows/macOS remain unrun.

**Next immediately / local preparation:** persistent per-source Warp/clip settings in the native library, outside cache cleanup. Worktree `/workspace/wt-engine-sample-settings`, branch `engine/sample-settings`, base `engine/log` (`de697f8e`). Unreferenced local WIP `library/sample_settings.hpp/.cpp` starts the typed pure codec (source/window, sound/loop/fade/signature defaults and exact AWRP bytes including readable future tails); it is not yet compiled/gated or connected to Index/native Save/Recall. Keep existing tags/BPM/rating, native transaction identity, future-data preservation, atomic imports/reopen and JSON exchange; do not substitute cache data for authored settings or claim project save/recall from the DTO. Then wire actual native clip capture/import and take Link/rest of Phase D/brief. Existing stem-weight/Drive handover blockers remain named open and do not stop this seat. Gate, push, log, next continues through compaction in this same turn.

---

## 2026-11-05 — Progressive native compressed playback gated; taking the analysed folder

**PR requested:** head `engine/progressive-decode` at **`f0e3188f5d777c4507f3f00a20555cfd3297617b`**, base `engine/pcm-decode-stream` (`506f4274`), independent `engine/aac-import` (`26d7a64c`) merged as `e41d0b4d`. Title: **Play compressed clips from bounded progressive decode prefixes**. Both independent prerequisites must merge first; this integration is depth two. Win opens the PR; main remains untouched.

**Window/worker handoff / PR body:** plain compressed clips with a cheap exact nonempty container length now load and play a flushed PCM prefix before full conversion. The normal unwarped native material import captures header dimensions and source identity without warming a full decode; existing media import overloads keep complete validation. `inspectPcm` retains unknown length rather than guessing a headerless MP3 duration. `ProgressiveDecoder` owns two codec workers by default (configurable 1..8), bounds running plus queued jobs at 32, coalesces live leases by source byte hash across path aliases, and permits fresh retry of a terminal job while old generations retain its diagnostics. `SessionSpec::progressiveDecoder` optionally selects an app-owned service/cache with lifetime through retired generations; null uses the process service. `ClipPlayback::decodeProblems()` gives the owner actual asynchronous error text, separately from construction problems. No Store enters jobs or the callback. No project schema, opcode, blob or UI edit.

**PCM/cache/lifecycle contract:** native bounded owner-thread PCM reads supply RIFF/AIFF/FLAC/MP3/Ogg; native AAC sink delivery supplies supported mono/stereo AAC-LC M4A/MP4/ADTS. The old AAC WAV API wraps the sink and preserves priming/tail semantics. Each job owns an exclusive private prefix directory under the shared decoded cache. WavWriter flushes each complete block/header before the ready-frame release. Prefix readers open/read/close under the same state mutex as writes, publication and writer finalization **including native-error/cancellation unwinding**; no persistent prefix reader can block a Windows rename. The final source hash pass runs outside that lock so available pages remain usable during it. Page requests beyond available PCM/context defer without publishing zero pages or incrementing errors; terminal errors report once and prime returns false promptly. The callback still only requests/consumes ready pages and never locks/allocates/decodes/does disk I/O. Admission accounts full expected PCM, queued reservations, cache budget and minimum disk headroom. Only a closed complete decode with unchanged final source identity enters the public hash path through no-clobber publication; a real concurrent winner must have exact dimensions, and existing bytes are never overwritten. The last public lease returns its completed cache pin synchronously; pending last leases request cancellation, and worker state cleans its private files. ENGINE-PROGRESSIVE-DECODE.md specifies bounds, ownership and cache trust.

**Gate:** final GCC14 Debug/Werror/PD-on/JUCE-off build and `test_all.sh` **13,868 checks / 160 suites**, all rows and sum inspected, zero failures, validators/layout clean. Both prerequisites contribute their prior 50 and 39 checks; this integration adds **93**. Actual Vorbis and native FFmpeg61 AAC Sessions play exact nonzero PCM at both widths while held incomplete and before the actual target is published; the independent complete reference is a separate cache. 44.1 kHz conversion, real right context, nonzero source offsets and repeated loop seeks match full WAV playback before completion. Tests cover coalescing, worker/admission bounds, source mutation, cancellation/retry, disk limits, malformed/concurrent publication, complete pin lifetime, owner diagnostics and zero callback allocation/free/I/O. Legacy clip playback passes all **251**, including its pin assertion moved to the actual completion boundary; AAC import 39, PCM stream 50, MP4 AAC 48 and NI Stems 43 remain green. Focused Clang19 compiles ten production CPP units (both FFmpeg ABIs included) plus the new test with actual conversion/Wshadow flags and passes all **93** against validated GCC cold dependencies. Ten actual GCC Ninja Wshadow syntax checks pass, with changed units repeated after the final edits.

**Adversarial evidence:** **16 compiled final-source plants** each fail by normal assertion (no crash/exception counted): read callback (1), pending range (9), partial frames (1), final source hash (2), completed pin (2), last pin release (2), last-owner cancellation (1), job capacity (1), worker count (4), coalescing (5), disk budget (1), import secretly full-decoding (4), pending treated as error (4), silent pending page (4), diagnostic callback guard (1), terminal retry (4). Focused GCC TSAN instruments the job/cache, PCM CPP+C codec implementations, WAV I/O, hash and publication: **3 checks, zero races**, eight jobs/sixteen readers with live cache maintenance, then eight cancellations with concurrent real prefix reads and no torn-header read failures. The first added cancellation harness accidentally reused complete targets because setLimits alone does not trim; its timeout is not a product failure or passed check. Adding explicit trim forces fresh jobs; the final stress passes. Review artifacts `/workspace/engine-review/progressive-review`, scripts progressive-review.py/progressive-tsan.py, final logs `/tmp/progressive-retire-*`, `/tmp/progressive-clang-retire.log`, `/tmp/progressive-plants-retire.log`, `/tmp/progressive-tsan-retire2.log`. The earlier full pin assertion failure and pre-unwind-fix passing run are superseded by this final gate.

**Limits kept explicit:** Warp still needs complete offline preparation; unknown exact header length retains the complete path. Jobs require stable closed sources; header-only import can commit a damaged later payload, whose decode then reports failure. Native Windows/macOS rewrap and codec execution remain unrun here; FFmpeg60 is compile-checked only. No whole Clang/MSVC/macOS/hardware/Live parity claim. The resource cap is explicit; cache crash leftovers are not a new startup-reaper claim. App-owned complete cache dimensions are validated, not authenticated against malicious same-shape PCM. README remains win-owned.

**Next immediately / already preparing:** Phase D 10's one-per-sample central `analysed` folder, followed by persistent library Warp/clip settings that cache cleanup never deletes. Independent worktree `/workspace/wt-engine-analysed-cache`, branch `engine/analysed-cache`, base `engine/log` (`71b080b5`): local WIP has bounded versioned/digested non-recursive analysis sections, atomic optional-cache replacement, normal waveform-consumer integration and isolated tests. It has syntax preflight only, not a gate/ready claim; configure/build/test it next after reclaiming only obsolete generated prerequisite builds. Then take Link and the rest of the brief. Stem model weight access and the actual Drive clone handover remain the earlier named blockers for those claims, not a seat stop. Gate, push, log, next continues in this same turn through compaction.

---

## 2026-11-05 — Context checkpoint: progressive decode integration in flight, seat continues

**Continue this item; no stop or PR-ready claim.** Worktree `/workspace/wt-engine-progressive-decode`, branch `engine/progressive-decode`, clean prerequisite merge `e41d0b4d` combines independent PCM `506f4274` and AAC `26d7a64c` (depth two, no main). The current integration edits are local pending their final gate: bounded two-worker/32-job hash-coalesced decode service, private flushed prefixes and complete no-clobber cache publication, native AAC PCM sink plus retained WAV wrapper, unwarped header-only native import, and actual ClipPlayback/Session page consumption before full conversion. Warp and unknown exact header length retain full preparation; no UI/schema/opcode edits. ENGINE-PROGRESSIVE-DECODE.md states ownership, source stability, cancellation, disk/pin/queue limits and unsupported claims.

**Evidence so far, not the final gate:** focused GCC initially 89 clean checks; current focused Clang19 production units/test pass **91** with actual conversion/Wshadow flags against validated GCC cold dependencies. Real Vorbis and native FFmpeg61 AAC Sessions play exact PCM at 32/64 while held incomplete; separate full-cache reference never warms the actual target. 44.1 kHz conversion/right context/source offset/loop seeks match complete WAV. GCC TSAN instrumented job/cache/decoder C+CPP/WAV/hash/publication stress: **2 checks, zero races**, eight jobs/sixteen readers/live cache maintenance. Ten actual GCC Ninja Wshadow syntax checks pass. Compiled plants are still running under `/workspace/engine-review/progressive-review.py`; artifacts/logs `/workspace/engine-review/progressive-review` and `/tmp/progressive-*`.

**Gate correction in progress:** initial full build succeeds; its test_all run identifies the legacy pin assertion immediately after load (the load is now intentionally asynchronous). Update it to assert the pin after complete publication. Also release completed pins synchronously with the last public lease rather than the worker's brief bookkeeping reference; permit fresh retry while an older generation retains a terminal cancelled/failed lease. Those latest changes are already in the 91-check Clang/TSAN evidence but need the final rebuilt GCC full gate. Never claim the older 89-check full-run pin failure as green. Wait for external fault-variant links to finish before relinking their owning Ninja core archive; then rebuild and run all rows/validators. Win opens PRs after the gate/push entry. Next take the analysed folder/library Warp data, Link and the remaining brief queue; model weight access/Drive backend handover remain named open from prior entries. This checkpoint is a log entry, not a seat stop: continue in the same turn through compaction.

---

## 2026-11-04 — Native PCM pull streams gated; taking progressive decode integration

**PR requested:** head `engine/pcm-decode-stream` at **`506f427418b00c8f52ade37d7c159a30520cc5e7`**, base `engine/log` (`58ed0af4`). Title: **Expose bounded owner-worker PCM decoding streams**. Independent of `engine/aac-import`; no feature prerequisite. Win opens the PR; main stays untouched.

**Worker/window handoff / PR body:** `audio::openPcmStream` returns native metadata plus a worker-owned pull decoder, not a cache path or async job. Immutable format/rate/channels and optional exact frame count can be copied into job state. Read caller-owned interleaved float spans of 1..65536 whole frames; explicit Frames/End/Cancelled/InvalidBuffer/AudioThread/WrongOwner/Failed statuses, counted actual frames and owner-only error diagnostics. Same opening worker reads/destroys; callback/foreign/invalid/pre-cancelled requests preserve buffers, position and diagnostics, with no allocation/free or I/O on the callback refusal. Cancellation is between blocks, native initialization may predecode an initial frame, sources must remain stable. Read through End to verify a declared total: overrun/short source or reported codec error is terminal even if a file is repaired later. Unknown lengths retain the native decoder's EOF-integrity limits, not an invented duration. MP3 without a length header is left unknown and not fully scanned on open; the existing complete-cache path retains its prior pre-scan policy and agrees on actual PCM. No project rows/opcodes/schema, background threads, full decoded PCM allocation or playback-completion claim is introduced.

**Vorbis / scope:** RIFF/AIFF/FLAC/MP3/Ogg Vorbis/Ogg FLAC use the existing native decoders. Vorbis now opens an owned native FILE (wide on Windows) and stb's checked file section, instead of retaining the whole compressed file in a C++ vector; its existing 2 GiB seek cap remains. stb owns the cursor, ADI closes the file after decoder teardown, including failed opens. The C wrapper enables stdio and selects the vendor's secure CRT branch on Windows; no third-party source edited. Legacy complete-cache Vorbis uses the same streaming backend. New AAC import is deliberately absent from this independent branch; its streaming integration is next. ENGINE-PCM-STREAM.md and synthetic fixture provenance/hash README document contracts/limits. No UI/device/plug-in tier edits.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and final test_all.sh **13,736 checks / 158 suites**, every row/sum inspected, zero failures, validators/layout clean; new suite **50**, legacy decoder **56**. Actual 1/7/31/4096/65536-frame pulls across eleven existing formats/rates, guarded spans, complete PCM equality to canonical cache, independent original 16/24-bit PCM, metadata/EOF/frame counters, cancellation and correct resumption, another-worker and callback zero allocation/free/file-I/O refusal, known-empty WAV, above-unity/tiny/signed-zero float bits, terminal truncation/repair, Unicode Vorbis, unknown-length no-Xing MP3 with independently authored channel frequencies/amplitudes, complete-cache equality, larger Vorbis real prefix/no whole-compressed C++ allocation, and actual Ogg FLAC bit identity are exercised. New long Vorbis/no-Xing/Ogg FLAC fixtures are synthetic, independently encoded by FFmpeg 7.1.5-0+deb13u1, checked in; no runtime CLI dependency. Focused Clang19 compiles both production C++ and the C codec implementations plus test, with actual target flags (C implementations remain outside the vendor warning wall as before); C++/test warning/conversion/Wshadow clean, all **50** pass against GCC cold dependencies. Both actual GCC Ninja Wshadow checks pass. No whole Clang/MSVC/macOS/JUCE/hardware/Live claim; README remains win-owned.

**Adversarial/concurrent gate:** **14** compiled normal-failure plants: read callback (2), open callback (1), wrong owner (3), block bound (4), whole frames (6), empty buffer (3), block cancellation (4), open cancellation (1), declared-short EOF (2), terminal failure (1), no-Xing pre-scan (2), dimension bound (1), wrong frame counter (28), restored whole-Vorbis retention (1). No crash/exception counted, validated sources/binaries untouched. Focused **TSAN**, production C++ wrapper AND C codec implementations instrumented with its harness, passes **3** checks with no races: concurrent owner reads preserve every one of 409600 PCM frames while another worker is refused 1000+ times; an actual FIFO refuses before a blocking open; 300 malformed native Vorbis opens leave file descriptors unchanged. External scripts/logs: /workspace/engine-review/pcm-stream-review.py, pcm-stream-tsan.py / pcm-stream-boundary.cpp, /tmp/pcm-stream-*. The environment was reselected starting→ready with this uncommitted work/gates intact, and work continued. Reclaimed only clean/pushed automation/lane/shape/recording/write-policy generated builds (sources/logs retained) for the next cold build; about 18 GiB free. No seat stop.

**Next immediately:** combine this independent prerequisite and AAC `26d7a64c` on `engine/progressive-decode`; two independent PR prerequisites at depth two, no three-deep stack. First expose native AAC PCM chunks without thread-local writer hooks; retain existing encode/decode wrappers and exact priming/duration/mono semantics. Add shared bounded background decode jobs/readiness and safe flushed prefix reads/private file lifetime/completion publication; ClipPlayback must obtain prefix pages on its worker rather than synchronously converting the full compressed source in its constructor. Fast metadata capture can use known exact source dimensions; name the fallback for unknown totals/offline Warp rather than guess durations. No codec/SQL/allocation/lock/file access in the callback; validate real Session samples before decode completion, seek/resampler context, error/cancel/last-owner lifetimes and concurrent/TSAN behavior. Keep existing cache cancellation/hash/publication guarantees when merging AAC. Win merges the two prerequisites first; no waiting on CI or main. Then analysed folder/library Warp data, Link and the rest of the brief, with model-weight access and Drive handoff still the named open claims. Gate, push, log, next continues through compaction in this same turn; this is a context checkpoint, not an ending.

---

## 2026-11-04 — Ordinary native AAC-LC import gated; taking streaming PCM and progressive playback

**PR requested:** head `engine/aac-import` at **`26d7a64ca6c60724bf4f3150dcf8fbcb46cd43cd`**, base `engine/log` (`9bcbee27`, no stem-result feature dependency). Title: **Import native mono/stereo AAC-LC through the normal audio pipeline**. Win opens the PR; main remains untouched.

**Window/worker handoff / PR body:** the existing `clip_edit::prepareImportAudio` / `commitMaterial` flow now captures content-sniffed mono/stereo AAC-LC M4A/MP4 or raw ADTS, records exact decoded dimensions and source BLAKE3, and creates the ordinary media+playing clip as one undo action. Copy-in retains original compressed bytes, Warp defaults off; no schema/opcode change. `DecodeCache::playable(source, stop_token)` and a trailing-token prepareImport overload retain the original APIs and add cancellation through hashing, PCM blocks, native AAC packets/draining and publication checks. Material import forwards its token and uses fixed-buffer/exact-size cancellable copy-in. The owner checkpoint reports progress/requests cancellation outside the cache mutex; do not reenter that cache or a Store whose capture is in progress. Media/material callback refusal now precedes allocation/free, SQL/file I/O and clearing caller-owned requests; the legacy callback test now correctly expects its old request to be preserved.

**Transport/native split:** `aacConfig` reads supported mono/stereo LC without changing NI's stereo `aacSampleRate`, `readAacMovie` or aligned writer/export contract. `readAacAudioMovie` accepts differing sibling AAC configs/rates/durations; ordinary import chooses the first audio track in file order, does not mix tracks or add a selector UI. A movie with no audio has typed no-audio status and generic media capture retains its opaque MP4 with null audio dimensions, without claiming video playback. `readAdts` bounds one million fixed-config single-access-unit LC frames and validates header/layer/sync/extents; CRC bytes are stripped but not checksum-verified. ADTS retains all coded samples with priming zero because authored gapless duration is absent. ISO-BMFF preserves exact edit-list priming/duration and the existing coarse-clock end rule. Five audio tracks, one million packets/track, 64 MiB moov, 1 MiB/packet/NI metadata; mono/stereo LC/1024-frame mode up to 65535 Hz. Fragmented/encrypted/complex edits, HE/SBR, unsupported layouts/frame/rate modes and ID3-prefixed ADTS are not claimed. Linux dynamically uses libavcodec60/61 with matching avutil58/59; native Windows/macOS adapters use mono/stereo PCM layouts and a private single-track rewrap (`writeAacAudioMovie`) without relaxing NI export. Native availability retains its existing verified-codec probe, unavailable codecs have a distinct error. No transcoder/Python runtime is invoked; the CLI is fixture-generation only.

**Cache integrity/publication:** validate complete cached hits before pinning; never consume an incomplete target or overwrite/delete it. Recheck the source hash after conversion and refuse a changed final hash. Sources must stay closed/stable; endpoint hashes are not an atomic snapshot against an in-place writer changing and restoring bytes. Publish the private WAV without replacing a concurrent winner, whose complete dimensions must match. Cancellation/other failures cannot be hidden by an appearing cache file. Remove only owned private artifacts; an already published valid cache file can remain for later reuse. Reserve decoded space before AAC conversion, check size multiplication overflow, retain existing LRU/pins/free-space policy. ENGINE-AAC-IMPORT.md / OPS and synthetic fixture provenance/hash README document the accepted scope.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **13,725 checks / 158 suites**, every row and sum inspected, zero failures, all validators/layout clean. New suite **39**, legacy decode **56**, MP4/AAC **48**, NI Stems **43**, media ops **71** pass; this branch adds 39 to win's integrated 13,686 baseline, not thousands of newly written tests. New synthetic mono 44.1 kHz and stereo 48 kHz M4A/ADTS were independently encoded/muxed/decoded by FFmpeg 7.1.5-0+deb13u1; compressed bytes and float32 reference WAVs are committed, no runtime/test CLI dependency. Authored vs coded frame counts, priming, channel order/PCM, wrong extensions/source-hash reuse/restart, six malformed ADTS classes plus later sync/config failure, native material import/owned compressed bytes/one undo/redo/reopen, actual Session playback at 32/64, live cancellation at the first private-file checkpoint before full decoding, token propagation/no premature cache publication, source mutation, disk refusal, concurrent winner byte preservation, invalid cached targets, opaque non-audio containers and callback zero allocation/free/file I/O are exercised. Current GCC and focused Clang19 suites each pass all 39; six production units plus test compiled with actual warning/conversion/Wshadow flags and linked against validated GCC cold dependencies, both FFmpeg ABI shims compiled. All eight actual GCC Ninja Wshadow checks pass; final transport check repeated after correcting Clang's signed-shift warning. Native libavcodec61/avutil59 was exercised; ABI60/58 was compiled, not runtime-exercised. Windows/macOS adapter sources were reviewed and test paths use UTF-8 conversion/wide ADI_HOME; those platform executions/whole Clang/JUCE/UI/hardware/Live parity remain unrun. README stays win-owned.

**Adversarial evidence:** 15 compiled normal-failure plants in the actual 39-check suite: lost priming (4), ADTS profile (1), truncated extent (1), changed config (1), relaxed NI stereo (1), lost final source hash (1), missing native checkpoint (1), lost disk reservation (1), overwriting concurrent winner (1), invalid cache hit (1), dropped media token (1), wrong format label (1), each media/material callback guard (1/1), FFmpeg mono layout (6). Separate production-linked probes add **4** passing CRC-header/native byte-identity and exact-one-million/one-extra packet boundary checks, plus **2** passing controlled unavailable-backend checks; removing only codec admission makes the latter fail normally (1). Thus **16** compiled plants, all explicit FAIL/exit1, no exception/crash counted, no validated source/binary replacement. External scripts/logs: /workspace/engine-review/aac-import-review.py, aac-import-boundary.py/.cpp and /tmp/aac-import-*.

**Environment correction / no hidden failed gate:** the expanded integrated build is ~7 GiB. The first full run filled the workspace and hit the cache's real 2 GiB free-space floor; that invalidated run was stopped/preserved as /tmp/aac-import-disk-full-all.log, not counted as a product gate. Removed only reproducible generated builds from already clean/pushed scanner-process/quarantine, graph-worker, stem-result, MIDI memory/capture-input/capture worktrees; sources/commits/benchmarks/review logs remain, reconfigure to rerun those old reviews. Kept scanner build for its external headless VST3 archive dependencies. About 14 GiB headroom remains. General AAC reaches the default cache for NI container metadata, so these cloud gates explicitly use **ADI_HOME=/workspace/.adi-cloud-test-data** (documented portable override, not HOME) because the sandbox cannot create /home/agent/.cache/adi; new suite isolates its own appdata. The rerun and final post-warning-fix gate above pass. No stop here.

**Next immediately / Phase D 10:** a file must play before decoding finishes. Current media capture calls blocking playable(), and ClipPlayback's constructor also calls it before starting its page worker; AAC support does not claim to close that gap. First expose a bounded owner-worker PCM pull stream/fast metadata primitive from the existing native RIFF/AIFF/FLAC/MP3/Ogg decoders, independently of this AAC branch so the later integration can have two independent prerequisites at depth two (not a three-deep stack). Then combine it with this AAC branch for native AAC streaming and shared bounded progressive jobs: flushed immutable prefix/readiness, no partial cache hit/publication, worker-only reads, source lifetime/cancellation/error state, seek/resampler context, actual Session output before completion, and race/underrun/TSAN evidence. No thread-local writer hooks or synchronous full decode on the callback/message thread. Warp's offline preparation is a separate constraint; name any remaining claim. Then the analysed folder/library Warp data, Link and the rest of the brief. Model weight access and native Drive/cascade handoff remain the named open blockers above. Gate, push, log, next continues in this same turn through compaction; this is a context checkpoint, not a stop.

---

## 2026-11-04 — Whole Loudness Drive cascade handoff still blocked; taking general AAC/M4A import

**Named item blocker, not a seat stop:** Phase D 7/7a's whole Loudness Drive must engage actual per-channel/group ADI-L stages and the master's full-mode end (ADR-0204 d1). This checkout still contains no ADI-Q/ADI-L clone header/target, supported DeviceInstance adapter, accepted measurement/handover artifact, stable parameter/state ids, response query or limiter light/full-mode contract; the earlier N16 handoff request remains unanswered here. Only the generic Live-class limiter exists in the device tier, which this lane does not change or substitute for the director's clone. InputGainNode and Auto Gain Stage already provide pre-insert gain after the instrument, but neither is a completed saved VCA/cascade control. Inspection also confirms tracks.role's native migration is still absent here (N38's handlers deliberately refuse that older format), relevant to a saved reference bypass. No new schema/control, slot engagement or complete Drive claim was made from the empty inspection branch. Win/clones must publish that actual backend contract; this is the existing handover dependency, not a director approval question.

**Queue continuation at once:** 7b's installer/UI tiers belong to win/window; 7c/7d export jobs/LUFS/NI Stems and 8/9 freeze/bounce/history/save have their prior pushed gates and remain taken. At Phase D 10, audio/decode.hpp's SourceFormat and decode.cpp's decoder dispatch still recognize only RIFF/AIFF/FLAC/MP3/Ogg, while the independent NI Stems path already has native MP4/AAC/platform codecs. General AAC/M4A import is therefore the next concrete engine gap. Wire sniffed content and the existing native MP4/AAC path into the normal import/cache/playback pipeline, with bounded decoding, cache identity/publication/error/cancellation and real fixture evidence; then take progressive decode playback, the analysed folder/library Warp data, Link and remaining brief work. Keep model weight access and the Drive handoff named open, rather than claiming either from a DTO or substituting unrelated DSP. Gate, push, log, next continues in this same turn through compaction; no stop here.

---

## 2026-11-04 — Native N-stem result import gated; model weights blocked, taking Loudness Drive

**PR requested:** head `engine/stem-results` at **`75d9a0b5`**, base `engine/log` (`c830b634`, including win's earlier integration history). Title: **Import completed N-stem results as one native action**. No scanner or inference dependency. Win opens the PR; main remains untouched.

**Window/worker handoff / PR body:** `media::prepareSeparatedStems` captures a closed producer's named, hash-authenticated WAVs on the serialized document owner worker; `commitSeparatedStems` creates owned content-addressed audio, one group and N child audio tracks/clips in one **Separate stems** undo action. Supply source media/hash/frame window, exact service/model/version/request and optional weights hash (mandatory for builtin), free allocated identities, placement, generation, explicit timestamp and advisory selection. Source and results retain exact rate/window length and mono/stereo samples; all clips start together, Warp off, source offset zero. Tick end rounds upward to cover the final sample. N is resource-bounded 1..1024, not hardcoded to four. Exact request and canonical model/version/weights/source-window metadata enter existing agent_requests in the same SQL transaction; actor_detail carries provenance, native CBOR replay needs no inference or result folder. Previously registered identical media is relinked to owned pool audio through native ops and undo restores its prior paths. No project opcode/schema change. ENGINE-SEPARATED-STEMS.md/OPS document API and ownership.

**Boundary/failure policy:** strict bounded UTF-8/no NUL, lowercase 64-hex hashes, real absolute result directory and real basename files, exact finite WAV dimensions, free/distinct identities, unlocked group parent, aggregate byte preflight (8 GiB default, caller ceiling 1 TiB). Fixed 64 KiB copy buffers enforce captured byte counts; captured and final publication hashes must match. Stop/checkpoint loops support cancellation; callback entry refuses before allocation/SQL/file I/O. Private exclusively reserved staging is owner-lifetime RAII; producer files can disappear after capture. Same still-open Store/client, document path/generation and journal head only; caller head/veto policy stays effective. SQL failure rolls back music/head/audit and can retry, retaining unreferenced content-addressed published files for GC rather than deleting another possible owner's asset. Existing destination must be a real matching regular file and is never overwritten. Equal dimensions/common placement preserve producer alignment but do not establish acoustic phase alignment or model quality.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **13,731 checks / 158 suites**, all rows and sum inspected, zero failures, validators/layout clean. Higher baseline is win's integrated history, not 3,000 newly authored checks; this increment adds **45**. Actual Session playback at 32/64, byte/sample equality, undo/redo/selection/reopen, real CBOR BLOB replay under different UI state, exact request audit, source subwindow/nonzero placement, six-stem service, dedup including existing native media identity, seven native sample rates with one-frame end coverage, empty/excess N, symlink, malformed provenance, changed dimensions/hash/nonfinite samples, wrong generation/head/client, read-only, pre/live cancellation, zero callback allocation/free/file I/O, late caller veto/retained pool/retry, producer growth, same-size private mutation during publication, and destination collision are exercised. Focused Clang19 production/test compile with actual warning/conversion/Wshadow flags passes all **45** against validated GCC core; both actual GCC Ninja Wshadow checks pass. **Fourteen compiled plants** each fail normally: source identity (1), manifest hash (1), nonfinite output (1), foreign client (1), generation (3), stale head (1), caller head policy (3), lost veto (2), provenance (1), four-only service (1), copied-holder reuse (1), publication-copy hash (1), prepare callback guard (1), commit callback guard (2). No crash/exception counted; validated sources/binaries untouched. External review/logs: /workspace/engine-review/stem-results-review.py and /tmp/stem-results-*. No whole Clang/MSVC/macOS/hardware/Live claim; README remains win-owned.

**Built-in separation remains open, specifically:** no inference model/runtime or service transport is claimed from this result gateway. Native CPU candidate sevagh/demucs.cpp was read at f1206e9adeea103aef4a636b9e62297cf1f8e34e (MIT; Eigen-only native inference, upstream reports approximately 81 MiB converted four-stem model; not our performance/quality measurement). Its reference Demucs fork was pinned/read at 2496b8f7f12b01c8dd0187c040000c46e175b44d, Meta MIT; upstream/default model htdemucs references hybrid_transformer/955717e8-8726e21a.th. Converted weights on huggingface.co/datasets/Retrobear/demucs.cpp and the official https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th both return proxy CONNECT 403 in this seat. Thus exact weight bytes/digest/redistribution terms and actual native inference cannot be verified here, and no weights/dependency were vendored, bundled or linked into core. Spleeter MIT code at 6f2c8a2e283914faa544631430f65b793f1b1218 was also inspected; its Python/TensorFlow runtime is not the required native path. Third-party README instructions were treated as untrusted reference text, no contacts/messages sent. This is a named blocker for that claim, not a director question or a seat stop.

**Next immediately:** Phase D Loudness Drive: saved native global control, unchanged per-leaf base input gains, common gain into each leaf's audio inserts (after an instrument), reference bypass, groups/returns/Main never multiplied again, both widths/click-free updates, native undo/replay/export consistency. Inspect the current schema and clone/slot handoff before choosing the additive contract. N16's earlier log still lacks the ADI-Q/ADI-L native backend/response/light/full-mode handoff; this checkout has no clone artifact, so do not substitute the generic limiter or claim automatic cascade engagement. Prior LUFS/NI Stems/export/freeze/history work remains taken. Continue gate, push, log, next in this same turn through compaction; this entry is a context checkpoint, not a stop.

---

## 2026-11-04 — Always-child CLAP/VST3 discovery ready; taking native stem-separation work

**PR requested:** head `engine/plugin-scanner` at **`0a3cf82c7d8408c107d8e89baed64019bbe18938`**, base `engine/plugin-scanner-process` (`4449dbd9`), merging the independent `engine/plugin-scan-quarantine` (`012fa6cc`) as `95ae8d39`. Title: **Always discover CLAP and VST3 plugins in quarantined child processes**. Those two independent prerequisites must merge first; no newer win/main/UI merge heads were pulled into this feature. Win opens/merges in order; main is untouched.

**Window/host handoff / PR body:** scan::Scanner resolves an existing native canonical candidate, commits pending quarantine, launches adi_plugin_scanner with explicit arguments, validates the entire format/path-bound reply and optional format data, then token-checks completion. Only Ready contains descriptors. ClapHost::scan and Vst3Host::scan/scanFile return owned complete/cancelled/file-failure Reports, and JuceDeviceLoader exposes setScanConfig/lastScanReport. One owner at a time per host/loader; copy/publish reports between jobs. Directory enumeration loads nothing; CLAP discovery leaves its runtime library cache empty. Bad candidates do not hide healthy siblings; cancelled/unavailable service retains the previous complete catalogue. Direct VST3 path hints and all older diagnostic/SDK fixture probes use the same child boundary, with no in-process discovery fallback. VST3 validates description XML before clearing quarantine, then restores the owner's original absolute JUCE path because its saved identifier hashes that spelling; CLAP saved hints are canonicalized for installed-copy selection. Runtime instantiation stays in process under D-03, and existing running instances are not terminated by rescans. Registry blocklist read/re-enable APIs are ready for the window; no UI wiring is claimed. No project schema/opcode/blob or external plug-in ABI change; rebuild native adapters for the C++ scan signatures.

**Wire/failure contract:** schema-1 UTF-8 JSON has exact typed fields, matching canonical path/format and unique keys/descriptor ids. Unknown/missing/future/duplicate (including escaped key spellings), NUL/invalid UTF-8, truncated/trailing data or invalid later descriptors refuse whole. Bounds: 8 MiB packet, 65,536 descriptors and catalogue entries, 65,536 raw string bytes, depth eight and 4,194,304 preflight string/container/separator tokens. Child CLAP strings are bounded before copy and feature lists cap at 1,024/65,536 bytes. Reserved metadata channel precedes library constructor/init/factory/deinit/unload; incidental stdout/stderr is discarded, and CLAP unload completes before metadata publication. Crashes/timeouts/nonzero/oversized/bad metadata retain named blocks; caller cancellation/helper launch/unsupported deployment abandons its own marker. Stale/failed completion publishes nothing. Resource exceptions retain pending quarantine. VST3 XML refuses DTD/entity declarations, nested content and inconsistent path/id/common fields; only the helper target compiles its discovery factory probe. This is crash containment, not a privilege sandbox.

**Packaging / parser findings:** CMake builds CLAP helper everywhere, adds headless VST3 with JUCE, and always copies the matching helper beside each discovery consumer (probe/player/render/app, including MacOS directory), even when the consumer does not relink. Explicit helper path remains available; unavailable never falls back. Version-2 shared registry means older suite applications must be updated together. The full 65,536-row test exposed nlohmann 3.12's quadratic DOM-callback array scan; a streaming duplicate-key pass plus ordinary DOM parsing fixes it without lowering the supported count. Final focused 42-check run takes about six seconds on this guest; no universal parent latency claim. ENGINE-PLUGIN-SCANNER.md, the updated foundation docs and OPS specify the contract.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,085 checks / 109 suites**, all rows/sum inspected, zero failures and clean validators/layout. New scanner **42**, legacy CLAP **486** and both foundation suites pass. Real controlled CLAP libraries prove multiple exact descriptors/UTF-8 paths, child-only PID records, noisy output separation, static/init/factory/descriptor/deinit/unload crashes, hangs, bad/duplicate/oversized fields, quarantine/re-enable, alias admission, late owner block, format rejection, cancellation and catalogue retention. Valid 65,536 descriptors and an over-count packet fitting the byte limit exercise both boundaries; callback guards have zero allocations/file I/O. Focused Clang19 builds the changed scanner/CLAP/child/test/fixture units and passes all 42 against validated GCC cold dependencies. A pinned JUCE modules-only/headless Linux build runs the production VST3 child/client/validator against the existing three-class SDK fixture: **14** pass under GCC and focused Clang (GCC JUCE cold archive), including XML refusal, retained generations, native alias UID spelling and callback guards. This avoids unavailable X11/ALSA development packages; it is not a whole JUCE/UI build. GCC and Clang strict Wshadow syntax passes for Vst3Host, JuceDeviceLoader and the updated SDK probe; seven actual default Ninja-command Wshadow checks pass including the CLAP diagnostic probe. Its controlled static-constructor-crash smoke exits normally with code 1 and a durable blocked row, never entering runtime creation. The exact copy-target fragment runs with a separate headless consumer: default sibling lookup succeeds; replacing only its generated helper copy with a stale fixture is repaired on the next build without consumer relink, identical source SHA256. No Windows/macOS execution, hardware or measured Live parity claim; README remains win-owned.

**Defect evidence:** **31 compiled plants** fail normally, no exception/crash counted and no validated source/binary replacement: format (1), path (4), schema (1), duplicate keys (1), duplicate ids (1), over-count (1), nonzero accepted (3), ignored format validation (1), lost failure quarantine (5), ignored completion (1), noncanonical admission (1), five scanner/protocol callback guards (1 each), lost metadata channel (1), parent library open (5), lost catalogue retention (3), three CLAP callback guards (1 each), VST3 DTD/nested/path/name acceptance (1 each), valid-prefix publication (4), lost saved UID spelling (1), failed VST3 publication (1), and two VST3 callback guards (1 each). Review scripts/logs remain under /workspace/engine-review and /tmp/plugin-scanner-*. The temporary stale-copy test used only a controlled generated consumer copy, not the validated helper.

**Next immediately — Phase D item 6:** stem separation built in/offline, native with no Python runtime, its own process; four stems/model in installer, N-stem external services. Read ADR-0203 d8/d25 and the Master Reference's service contract before choosing the model/runtime. Verify actual code AND weight licensing under the two refusals, native CPU support, exact model identity/digest, bounded audio/job/result ownership and cancellation; do not claim a bundled model or service implementation from a DTO. Installer/UI remain their lanes. Then Loudness Drive over leaf input gains/cascade and the rest of the brief, preserving prior export/NI Stems/freeze/history work rather than duplicating it. This is a gate/log checkpoint, not a stop: continue in this same turn through compaction until the seat's actual usage/context limit.

---

## 2026-11-04 — Durable scan quarantine ready; taking always-child discovery

**PR requested:** head `engine/plugin-scan-quarantine` at **`012fa6cca36e4c8d42dac150ef33e9de51537aab`**, base `engine/log` (`ecd72baf`, before win's concurrent stack merge). Title: **Persist plugin scan quarantine with guarded attempt completion**. Independent of the process-runner branch; integration will consume both independent prerequisites rather than stacking them. Win opens the PR; main is untouched.

**Window/scanner handoff / PR body:** PluginRegistry version **2** transactionally migrates version 1, retaining routes/times and ADIP identity. Owner beginScan(format, absolute UTF-8 canonical-path key, explicit Unix microseconds) commits pending quarantine before launch; an existing pending/blocked key refuses, and persistence failure means no scan. Pending survives close/death/reopen without automatic expiry, preserving the distinction between interrupted and proven failed. finishScan accepts only the same still-open connection's opaque ticket and exact unrecycled AUTOINCREMENT pending identity: success/abandonment remove their own marker, failure stores a bounded named block. Cancellation/helper launch failures should abandon; plug-in crashes/timeouts/bad metadata must fail. Explicit blockScan invalidates an active completion, unblockScan re-enables idempotently; late successes/failures cannot clear/poison replacement attempts. Other files/connections/reopened instances reject foreign/expired tickets. scanBlocks returns sorted owned format/path/state/reason/time for a visible blocklist; window draws/re-enables through these APIs. All registry methods now refuse callbacks before SQL/allocation/diagnostic mutation. One owner per connection; concurrent suite connections use existing WAL/busy timeout. ENGINE-SCAN-QUARANTINE.md/OPS document it. This is app data, no project schema/opcode/blob change; old version-1 suite applications refuse the upgraded registry and should be updated together.

**Bounds/identity:** clap/vst3 only; exact absolute UTF-8 path keys <=65,536 bytes, nonempty failure reasons <=4,096 bytes, no NUL, nonnegative explicit time. Scanner resolves aliases before admission; registry never guesses case/Unicode equivalence or touches the filesystem for a key. At most 65,536 shared entries; existing blocks remain editable/re-enableable at capacity. Whole-list reads default to 4,096 rows, allow 1..65,536, cap aggregate text at 16 MiB and preflight stored types/column lengths before copying. Invalid/later/excess data refuses whole, not a successful prefix. Ticket payload copies precede the committed marker; post-commit ticket return moves without allocation. Completion/SQL failure preserves quarantine and rejects metadata.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,016 checks / 107 suites**, every row/sum inspected, zero failures, validators/layout clean. New quarantine suite **45** and legacy registry **35** pass. Actual native migration/rollback, preserved routes/timestamps, cross-connection committed visibility, all completion states, namespace separation, re-enable/replacement, copied/foreign/expired/stale tickets, concurrent four-owner admission (exactly one winner), malformed input, callback zero allocation/file I/O, insert/update/delete trigger failures, full 65,536-entry capacity, read row/text bounds and invalid later stored row are exercised. Focused Clang19 production/test compile with actual warning/conversion/Wshadow flags and all 45 pass against validated GCC core; both actual GCC Ninja Wshadow checks pass. **Sixteen** compiled plants fail normally: lost connection identity (3), accepted older attempt (7), recycled id (7), wrong failure state (1), close clearing pending (1), over-capacity (2), partial read (1), invalid UTF-8 (4), plus individually removed callback guards for all eight entry points (1/1/1/1/3/3/3/1). No crash/exception counted and validated sources/binaries untouched. External real child `_Exit` bypasses all DB destructors, leaves committed WAL, a second begin refuses, and actual API reopen→explicit re-enable→new successful completion passes. No whole Clang/MSVC/macOS/JUCE/hardware claim; README remains win-owned.

**Next at once:** integrate bounded/versioned, duplicate-free, path/format-bound metadata and a dedicated child helper; CLAP parent enumeration must leave libraryCount zero, with real controlled shared-library fixtures for constructor/init/factory crashes/hangs/noisy output and invalid data. VST3 must include the loader's direct path-hint discovery and never fall back in process. Package helper beside the app or provide explicit path; fail closed when helper/registry unavailable. Pinned JUCE has a modules-only/headless VST3 path: use that for a focused Linux child build if feasible without inventing a whole JUCE/UI/hardware gate. After scanner completion continue stem separation, Loudness Drive and the remaining brief. Gate, push, log, next persists in this same turn through compaction; no stop here.

---

## 2026-11-04 — Scanner process boundary ready; taking durable quarantine and discovery

**PR requested:** head `engine/plugin-scanner-process` at **`4449dbd98a20fed6224026ccc7ebac55ceb4a65f`**, base `engine/log` (`ecd72baf`, before win's concurrent stack merge). Title: **Add bounded child process execution for isolated plugin discovery**. No feature dependencies. Win opens the PR; main remains untouched.

**Host/window handoff / PR body:** owner workers can call scan::runProcess with an absolute executable, explicit UTF-8 argument vector and bounded ProcessOptions. No shell/PATH lookup/global cwd or signal handler. Raw bounded stdout is drained during execution; stdin/stderr use the null device. Result distinguishes invalid/callback/launch failure, normal exit including nonzero, crash, timeout, cancellation, output limit and I/O failure; only normal zero exit is success. Defaults 30 seconds/8 MiB; accepted 1..300000 ms, 1 byte..8 MiB, 256 arguments/128 KiB argv budget, no embedded NUL. Windows native paths/strict UTF-8 conversion/CRT quoting use a suspended launch assigned to a kill-on-close job before resume, with explicit inherited handles. POSIX uses posix_spawn with its own group, nonblocking reader, bounded drain batches and WNOWAIT to retain leader identity until group kill/reap. Timeout/cancel/failure and normal exit clean descendants without waiting for inherited-writer EOF. RAII retains cleanup on allocation exceptions; POSIX callers must not ignore SIGCHLD/steal its child status. Callback refusal precedes allocation/OS calls. This is crash containment rather than a malicious-code privilege sandbox. ENGINE-SCAN-PROCESS.md and OPS document ownership/bounds/platform constraints. No project schema/opcode/blob change.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **9,998 checks / 107 suites**, every row and sum inspected, zero failures, validators/layout clean. New suite **27** uses actual child crash/hang/flood, binary NUL/large stdout, stderr, stdin EOF, exact empty/space/quote/backslash/metacharacter/newline/UTF-8 arguments, Unicode executable paths, closed standard descriptors, limits, pre/live cancellation, timeout and normal-exit descendants, 24 repeated and 24 concurrent launches, direct child reaping and callback zero allocation/file I/O. Focused Clang19 production/test compile with actual warning/conversion/Wshadow flags and all 27 pass against validated GCC core; both actual GCC Ninja Wshadow checks pass. Seven compiled plants fail normally: callback guard (2), output bound (2), group cleanup (2), empty argument loss (1), stdout byte loss (11), crash classification (1), late deadline (2). No crash/exception counted as a plant success, no validated source/binary replacement. Windows/macOS execution/whole Clang/JUCE/hardware remain unrun; README stays win-owned.

**Next immediately / remaining scanner claim:** this foundation deliberately leaves existing CLAP/VST3 scans unchanged until integration. Add durable pre-launch quarantine and explicit re-enable in the shared plug-in registry, token-checked completion so stale jobs cannot clear new blocks; then a bounded/versioned path-bound metadata helper, CLAP parent discovery without loading, and VST3 including its separate path-hint discovery call. No in-process fallback. Master Reference R-17 confirms visible disabled list/re-enable; pending attempts surviving parent death must stay quarantined. JUCE 9.0.2 was fetched read-only and its tag verified at pinned 72782788ce18c2d4d760b28e0921d6ffc6431102: VST3 directory search only inspects files; findAllTypesForFile is the child boundary. Linux lacks X11/ALSA development packages at present, so no JUCE compilation claim. Continue gate, push, log, next through compaction in this same turn; this is not a stop.

---

## 2026-11-04 — Persistent graph workers gated; taking the out-of-process scanner

**PR requested:** head `engine/graph-workers` at **`f6e8533b4ae679ce80c5c576ae29a57953a48f38`**, base `engine/log` (`2d73325a`). Title: **Run independent graph nodes on persistent audio workers**. Independent of the scale/automation stacks. Win opens the PR; main is untouched.

**Driver/host handoff / PR body:** Graph, GraphHost and Session expose setParallelRendering(workers, minimumFrames, minimumNodes, initializer, context). Zero workers remains the default. Thresholds are explicit; at most sixteen persistent owner-created workers. Optional driver initialization runs once per worker outside CallbackScope before borrowed nodes are prepared; failure joins partial resources and preserves the old published graph/audio. Its context must survive later rebuilds until policy replacement/destruction. Unknown/device nodes stay on the callback by default; only SumNode/GainNode/MixNode currently opt in, and duplicate node pointers never run on workers. Custom native adapters must rebuild against the added Node capability virtual. No external plug-in ABI or project format changes. Owner-created participant scratch and fixed consumer order preserve routing, sidechain, PDC, bypass, events, suspension, tap windows and counters in both precisions. Callback dispatch/completion uses bounded partition storage and atomics; worker waits occur outside CallbackScope, with no callback mutex/semaphore wait. Completed-bit recovery prevents duplicate node execution after a later worker wait failure. Per-job FP mode, including x86 FTZ/DAZ, follows the caller and restores the worker. Driver priority/affinity remains explicit; no universal device/plugin readiness claim. ENGINE-GRAPH-WORKERS.md and OPS describe ownership and failures.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,063 checks / 107 suites**, every row/sum inspected, all validators/layout pass. New suite **92** covers primitive 1/3/16-worker generations, actual overlap, small-block serial policy, affinity/duplicates, byte-identical float/double rendering at 44.1–768 kHz and 32–4096 frames, real Store/Session realization and swaps, failed initialization before borrowed-node prepare, caller FP changes, zero callback/worker-job allocation/free/file I/O, scope counters and release. Both precisions also run 1,000 callbacks with up to 500 concurrent owner latency retaps. Focused Clang19 builds changed graph/worker/host/session/test units and runs all 92 against validated GCC core; GCC ThreadSanitizer instruments those units/test and passes all 92 without reports or suppressions. Actual Ninja warning/Wshadow checks pass for six affected units, with the final test repeated; Release benchmark syntax also passes. Nine compiled plants fail normally (affinity, small-block switch, sleeping levels, counters, sum order, scope, FP, partition loss, late setup); no crashes counted. Separate injected post/wait failures retain exact once-per-node audio, and removing completed-bit retention fails normally. Full Release benchmark and its self-test pass. No whole Clang/MSVC/macOS/JUCE/hardware/Live parity claim; README remains win-owned.

**Measured policy:** nine committed artifacts retain 256 raw rows plus environment/source fingerprints: fixed existing benchmark projects, both precisions, 32–4096 frames, 1,000 measured callbacks after 32 warmups, four-vCPU AMD EPYC cloud guest pinned to guest CPUs 0–3. Focused old hot-render baseline is exactly 2d73325a; current serial, forced-three-worker and explicit candidate profile are separate runs. Forced workers substantially hurt 32/64-frame tails (13/3 budget misses per precision); do not enable them there. Candidate minimumFrames=4096/minimumNodes=64 dispatches only active 64-track 4096-frame levels and skips sleeping levels. That case's p50/p99 improves from serial 748.5/1703.5 to 614.2/910.8 microseconds (float), and 2150.7/3948.2 to 1002.1/1505.2 (double). Small-block serial p50 medians versus old renderer are 0.991/1.010; raw ranges/tails remain available. Candidate totals have zero/one budget misses; the double miss is a 32-frame row with no pool, reflecting guest scheduling noise. No event drops/rejects. This is one guest's tested candidate, not a universal automatic profile or hardware xrun claim.

**Concurrent log integration:** win advanced remote engine/log with its existing engine-stack/UI merge history while this gate was running. Merged that remote topic head 89c9a784 into the log branch without conflicts or rewriting history; no main checkout, UI source edit or PR merge was performed by this seat. Feature scanner work remains independently based on ecd72baf.

**Next at once:** Phase D plug-in scanning always out of process (R-17/D-03). Inspect actual CLAP/VST3 enumeration and helper launch paths before choosing the shared bounded subprocess/protocol contract; parent enumeration must never load a plug-in. Cover crash, timeout, malformed/oversized output, cancellation and path identity using controlled fixtures, with precise JUCE/Windows handoff. Then stem separation, Loudness Drive and remaining brief queue. Gate, push, log, next continues in this same turn through compaction; this checkpoint is not a stop.

---

## 2026-11-04 — Per-key tuning timeline ready; taking multi-threaded graph processing

**PR requested:** head `engine/scale-timeline` at **`0c80f1d640306f8b07517fca61900a34224dfc0f`**, base `engine/log` (`9b84a9e9`). Title: **Expose immutable tuning changes at exact timeline ticks**. Explicit dependency: `engine/scale-context` (`18966b17`) and N32. Independent of the writer/Scala branches; cross-branch writer integration was tested separately. Win opens/merges in order; main is untouched.

**Window/device handoff / PR body:** `engine::readScaleTimeline(store)` reads global key events/references/memberships and referenced definitions in one coherent owner transaction, preserving an outer transaction. `compileScaleTimeline` accepts explicit owner values. Held immutable generations expose atTicks, events and nextChangeAfterTicks: inclusive exact int64 key boundaries, next strictly later boundary, C major/12-TET before the first event (including pre-roll), no floating-clock narrowing or tick-plus-one overflow. Queries are bounded binary searches without allocation, SQL, locks or I/O; keep/retire their generation on the owner. Event views retain key IDs and exact positions; ScaleContext pitch queries still require explicit absolute tonic. Clip-local scales remain separate overrides through N32/N33. No automatic note retuning, sample-time conversion, device publication, Session hook or UI/DSP changes are claimed. ENGINE-SCALE-TIMELINE.md and OPS §9.5 specify the handoff. No schema/opcode/binary-layout change.

**Bounds/error policy:** at most 65,536 events/1,024 supplied definitions; 1,048,576 compiled degree rows including fallback, 1,048,576 memberships, 8 MiB authored text. Repeated definitions and names count at every event before allocating compiled copies. Reader accumulation is independently bounded; individual explicit definitions still cap at 65,536 degrees. Global positions/IDs are ordered/unique/nonnegative/positive; local flags refuse. Orphan/missing/invalid used data, even a bad later tuning after a good prefix, refuses the whole map; retain the prior valid generation on rebuild failure. Unreferenced contents do not affect the map. Reads/compilation refuse callback use before SQL or copying.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,067 checks / 109 suites**, every row/sum inspected, zero failures, all validators/layout pass. New timeline suite **28** covers exact 0/inclusive/strict-next boundaries, adjacent ticks beyond 2^53, INT64 min/max, 24/53-EDO changes within a continuing clip, author data/source lifetime, moved holders, 11 malformed cases, count/repeated degree/text preflight, full 65,536-event map, caller transaction preservation, SQL immutability, clip-local/global distinction, malformed note independence, orphan/missing/later-corruption refusal and retained good generation. 300,000 callback queries plus refusal paths have zero allocation/file-I/O; callback SQL trace is zero. Clang19 changed module/test compile with full warning/conversion/Wshadow flags and all 28 pass against validated GCC core; two actual Ninja-command Wshadow checks pass. Seven compiled plants fail normally: previous-at-boundary (10), shifted clock (7), wrong default membership (1), local accepted globally (1), missing repeated-degree budget (1), missing repeated-text budget (1), accepted valid prefix (2). No crash/exception counted. Separate executable links the actual timeline module with pushed native tuning writer: **10** real Store checks pass for key edits, per-key non-octave context, retained generation, whole-action undo/redo, binary replay under different UI state/digest and reopen. No whole Clang/MSVC/JUCE/hardware/Live parity claim; README stays win-owned.

**Next immediately — Phase D graph threading:** ADR-0056's same-level output independence does not yet make runNode concurrently safe: Graph owns shared mix/sidechain/compensation scratch, input-idle flags and render counters. Isolate render state per executing participant before any worker concurrency. Preserve fixed consumer sum order, event routing/splitting/PDC/tails/bypass and both widths; provider thread ownership needs explicit treatment (do not assume every external/device node is safe). Then persistent owner-created workers, callback no allocation/lock/logging, completion barriers, small-block serial switch from measured data, determinism and real concurrency/TSAN/bench evidence per ADR-0102 d4. No pool or Live performance readiness is claimed from reverse-order tests. Graph work can branch independently from engine/log without scale dependencies. After that take the out-of-process scanner, stem separation, Loudness Drive and remaining brief queue. Standing order continues: gate, push, log, next in this turn through compaction; this is a checkpoint, not a stop.

---

## 2026-11-04 — Bounded Scala input reaches native tunings; taking per-key timeline context

**PR requested:** head `engine/scala-import` at **`5aa71c08d931b8bf9d0ba79dcbaed8930948d620`**, base `engine/log` (`d1a6ebaf`). Title: **Parse bounded Scala scale input into exact tuning definitions**. Explicit dependency: `engine/scale-context` (`18966b17`) and N32. Parser code is independent of `engine/tuning-edits`; the integration was also tested against that separate pushed writer. Win opens/merges in order; main remains untouched.

**Window/device handoff / PR body:** `engine::parseScala(bytes,id,displayName)` owns an explicit TuningDefinition without files/SQL, identity reservation or project writes. It preserves description spacing/bytes and explicit name, marks source scala, inserts unison degree zero, stores intervals 1..n-1 as double cents and moves the final interval into the actual repeat period (including tritaves). Ratio operands are positive decimal integers up to 2^53-1, exact in double; log1p retains near-unison ratios. Numeric-line annotations are ignored instead of invented note labels. The owner can compile/review this definition, then use tunings::definitionPayload with the writer's create/replace and key assignment in one native action. The logged resolved double values replay without file access or new logarithms. Native note pitch still uses the existing explicitly bounded key/float-offset representation; no automatic retuning.

**Accepted grammar / review limits:** optional initial UTF-8 BOM, LF/CRLF/CR, optional final newline; first non-comment physical line is the description (empty allowed), then blank/comment lines are skipped. Count and interval tokens stop at ASCII whitespace or !. A token containing a decimal point is finite decimal cents (plus/exponent supported); otherwise a/b or integer a/1. Strictly rising positive intervals, exactly the declared count, no extra trailing content. Bounds: 8 MiB input, 65,536 bytes per physical line/name, 1..65,536 degrees. NUL/overflow/invalid/ambiguous data refuses a whole definition with a physical-line diagnostic, never a partial scale. Callback guard precedes copying/allocation. ENGINE-SCALA-IMPORT.md and OPS §9.5 specify these choices. No schema/opcode/binary-layout change. The official Scala page remains proxy403, so accepted-grammar fixtures are evidence, not a full external Scala/Live importer parity claim. KBM/reference frequency, filename/encoding inference and UI chooser wiring remain separate.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,064 checks / 109 suites**, all rows/sum inspected, zero failures, every validator/layout pass. New parser suite **25** covers ratio/decimal/full-double precision, non-octave period, near-unison, description/source ownership, comments/empty descriptions/BOM/line endings, 23 malformed packets, preflight budgets/line diagnostics, the full 65,536-degree case, actual native ANOT E-minus-fifty-cent insertion with one-action undo/redo, and callback zero allocations/file I/O. Clang19 changed parser/test compile with full warning/conversion/Wshadow flags and all 25 pass against validated GCC core; two actual Ninja-command Wshadow checks pass. Seven compiled plants fail normally: wrong period (4), narrowed cents (4), lost description (2), lost display name (1), ignored trailing content (1), missing byte preflight (1), missing degree-count preflight (1). No crash/exception counted. A separate cross-branch executable links the actual new parser to the pushed tuning writer and passes **16** real Store checks: parsed definition→native tuning/key/clip commit→effective context→one undo/redo→binary log replay under different UI state→identical SQL digest/projection→reopen. No merged-stack readiness or whole Clang/MSVC/JUCE/hardware/manual parity claim. README remains win-owned.

**Seat maintenance:** reclaimed only generated build directories of three already-gated, pushed and clean items (clip-envelopes/send-automation/audio-memory), retaining source commits, logs and review evidence; about 7.6 GiB freed. Current feature builds and root checkout remain clean. This changes no source or branch history.

**Next at once:** immutable per-key ScaleTimeline over exact int64 arrangement ticks, coherent owner read, default 12-TET before first event, bounded referenced definitions/memberships, next-change boundaries and allocation-free callback lookup with retained generation lifetimes. Clip-local scales remain explicit overrides of the global timeline. Then take Phase D multithreaded graph processing, scanner out of process, stem separation/Loudness Drive/Link/scope in brief order; keep open Capture format/inference and measured parity claims named, not fabricated or used to stop independent work. Standing order remains gate, push, log, next in this same turn through app compaction; this checkpoint is not an ending.

---

## 2026-11-04 — Native tuning/key-scale edits replay exactly; taking Scala import

**PR requested:** head `engine/tuning-edits` at **`e88a91744941298240830e3cf4c74b64215005f9`**, base `engine/log` (`6cf70734`). Title: **Add native tuning definitions and per-key scale edits**. Explicit dependency: `engine/scale-context` (`18966b17`) and its N32 prerequisite; no newer win/main/UI merge heads. Win opens/merges in dependency order; main stays untouched.

**Window/device handoff / PR body:** six native owner operations now implement tuning.create/delete/replace, project.insertKeyEvent/removeKeyEvent/setKeyScale. Explicit positive int64 identities, integer arrangement ticks and complete authored values are carried in payloads; inserts never upsert. Definitions preserve double cent offsets, sparse authored degree IDs, non-octave periods, names/description and provenance. Replacement validates every referencing key's membership before writing; deletion refuses a referenced tuning. Scale assignment preserves position/identity and can explicitly detach to implicit 12-TET or choose empty membership. Twelve-bit compatibility masks are enforced on writes even for tuned keys; tuning-aware reads still ignore that mask. Definitions cap at 65,536 degrees, 65,536 bytes/text and 8 MiB total text. Unknown nested fields, missing/noninteger/overflowed IDs, nonfinite/invalid rows and missing/duplicate selected degrees refuse. Complete inverse capture plus the journal transaction handles undo/redo/reopen and late SQL rollback. Handler/inverse callback guards run before SQL; payload construction and the journal remain owner APIs. Async callers still supply their normal head/document/veto policy. Notes are not implicitly retuned.

**Projection and format:** typed rows and text projection now cover all five existing scale tables. Tuning definitions expose full-double period/degrees and escaped author text; key events expose exact position/root/identifier/mask, membership and content-labelled references. Row IDs never appear; degree IDs remain authored coordinates. Same-named definitions, changed relational identities and shuffled rows retain identical projection. Missing tuning targets are explicit unresolved references. ENGINE-TUNING-EDITS.md, OPS §9.1 and TEXT-PROJECTION document it. No schema/binary-layout change. The catalogue is **176** (four new rows; the key insert/remove rows already existed), P2 +4; win reconciles independent opcode stacks. No README edit. Scala file parsing, editor/device production wiring, presets/KBM/reference/bypass/Retune Set On Loading remain separate work.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,072 checks / 109 suites**, every row and sum inspected, zero failures, all validators/layout pass. New native suite **38** covers real native edit→inherited clip context at two different tunings, sparse Scala/tritave definition, held-context lifetime, exact double values, one action undo/redo/reopen, complete definition/key inverses, referenced delete/invalidating replacement refusal, late trigger failure after old rows cleared, transaction rollback, 23 malformed packets, int64 ID/ticks beyond 2^53, empty memberships and callback SQL refusal. Actual binary op log exercises all six handlers under different selection/playhead/grid, matching both SQL digest and full text; whole-log rewind/redo succeeds. Clang19 compiles changed ops/typed-reader/projector/test units, syntax-checks catalogue, then runs all 38 against validated GCC core; not whole Clang CI. Five actual Ninja-command Wshadow checks pass (test repeated after final additions). Seven compiled plants fail normally: narrowed cents (3), lost names (3), invalidated membership (1), incomplete tuning inverse (3), wrong root (1), incomplete key inverse (1), unknown nested fields (1). No crash/exception counted. MSVC/JUCE/hardware/measured Live parity unrun.

**Next at once:** Scala .scl owner parsing into explicit bounded tuning values, moving the last interval to the repeat period and inserting degree-zero unison as ADR-0178 requires; keep ratio conversion double, source/description exact and no KBM/absolute-root guesses. The official Scala format page returned proxy403 today, so report accepted grammar and tested fixtures precisely, not external parity. Parser can depend on context independently of the writer PR, avoiding an unnecessary write-stack dependency. Then immutable per-key timeline context, multithreaded graph/scanner/stem separation/Loudness Drive/Link/scope as the brief orders. Capture persistence/inference questions remain named in earlier entries, not a seat stop. Gate, push, log, next continues in this turn through app compaction; this is a checkpoint, not a stop.

---

## 2026-11-04 — Immutable Scale Mode/tuning context ready; taking native scale edits

**PR requested:** head `engine/scale-context` at **`18966b17bae5c03d5bb4c3e727930d2419483248`**, base `engine/log` (`b304506`). Title: **Expose immutable clip scale and named tuning context**. Explicit dependency: N32 clip-details code commit `93f2e35` (merged as `e1400b3c`), not its newer win/main merge head. N33's optional table is read through N32's existing policy; no new N33 dependency. Win opens/merges in order; this Linux seat leaves main untouched.

**Window/device handoff / PR body:** `engine::readClipScaleContext(store,clip)` reads effective clip-local/inherited scale and referenced tuning rows in one owner snapshot without selecting/decoding note blobs. Missing/bad definitions and ambiguous inherited linear/ramp clocks refuse whole rather than silently falling back. N32 `readClipDetails` gains default-true includeNotes and participates in a caller-owned transaction without committing it. `compileScaleContext` accepts explicit values, retaining immutable degree IDs, cent offsets, authored names, period, provenance and description. Twelve-bit masks apply only to implicit 12-TET; explicit tunings use degree membership and ignore the mask (ADR-0178). Empty membership, sparse degree IDs and non-octave periods remain exact. Hold contexts on the owner for graph-generation lifetimes; do not retire their final shared data on the callback.

**Consumer contract:** bounded allocation/SQL/lock/I/O-free includesDegree/nearest/match/contains queries require an explicit absolute tonic in cents. Nearest scale-member ties choose the lower pitch; literal highlighting does not quantize notes. Owner-only grid projects an inclusive ruler/Fold list with author names or numeric degree fallback, bounded to 65,536 rows and no partial list on failure. Optional native MIDI-key/float-offset encoding reports actual precision error (default maximum 0.0001 cents), not an arbitrary-double storage claim. Full definitions cap at 65,536 degrees; pitch bounds +/-1e9 cents and repeat bounds +/-2^50 protect conversions. No schema/new opcode or playback retuning; editor/device automatic wiring, preset parity, import, KBM/reference pitch, bypass and Retune Set On Loading are not claimed. ENGINE-SCALE-CONTEXT.md and OPS §9.5 document the API and limits.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,039 checks / 108 suites**, every result row and summed count inspected, zero failures, all validators/layout pass. New context suite **39** and N32 **29** pass. Tests cover 24-EDO Rast/E half-flat, 53-EDO precision, sparse degrees/tritave period, masks/empty Fold, explicit tonic, negative periods/tolerance/ties, malformed rows/references, bounded grid, retained immutable data, real per-key SQL changes/local override/linear timing, caller read transactions, no note reads with an invalid blob, callback SQL refusal and 128,000 realtime query pairs with zero callback allocations/file I/O. Clang19 compiles both changed production units and the new test with full warning/conversion/Wshadow flags and runs all 39 against the validated GCC core (not whole Clang CI). Three actual Ninja-command Wshadow checks pass. Seven compiled plants fail normally: wrong tuned membership (5), lost authored names (3), wrong period (2), lost tonic (4), wrong tie (1), invalid degree order (2), missing referenced-degree check (1). No crash/exception counted; MSVC/JUCE/hardware/measured Live parity unrun. README remains win-owned.

**Next immediately:** native scale-aware edits over ADR-0178's existing tables: explicit-ID tuning create/replace/delete and key-event insert/remove/scale assignment, complete inverse capture, cross-row validation, coherent text projection and binary replay under different UI state. Then Scala import and immutable per-key timeline context, followed by the remaining Phase D queue. Capture precision/inference questions in the prior entry remain open for those claims; they do not stop independent work. Standing order: gate, push, log, next in the same turn through app compaction; this is a checkpoint, not an ending.

---

## 2026-11-04 — Native MIDI-1/CC Capture plays and replays; continuing Scale Mode/tuning support

**PR requested:** head `engine/midi-capture` at **`bc385f6387c954ec0179b630f2f6dbfc240bd05a`**, base `engine/log` (`7fa6f30`). Title: **Capture MIDI notes and controllers in one native stream operation**. Explicit dependency: `engine/midi-capture-input` (`5dc751a`) and its declared memory/controller dependencies. Independent of audio Capture/automation stacks. Win opens and merges in dependency order; main remains untouched by this Linux seat.

**Window/host handoff / PR body:** `midi_capture.hpp` adds owner-side capture::prepareMidi(store,snapshot,MidiSpec,error) and commitMidi. Supply a new clip ID, project tick position and explicit driver-sample range containing the complete retained input, name and held-note closing policy. A coherent read checks the unlocked/unfrozen regular target and step-tempo map; every placed boundary must roundtrip its exact source-relative sample. Notes keep uint64 source IDs and source-quantized MIDI-1 onset/release words; arbitrary note doubles and unsupported expressions refuse rather than narrow. CC values remain double. Held notes may close at the explicit boundary with release zero. Truncation, serial/time gaps, unknown/reused ownership, mismatched releases, zero-duration notes, bad targets, ramps and unrepresentable clocks refuse without writes. Commit requires the same still-open Store/client and captured head (including NULL), retains transaction veto, and refuses callbacks before SQL/allocation. Caller document generation remains necessary across async work.

**Native operation:** one `capture.createMidi` carries explicit MIDI clip and stream IDs plus validated sorted v1 ANOT/ACTL binary images. It writes each stream once, with a 1,048,576 aggregate note/point limit and 2,048 CC-address limit. Unknown/future/truncated images, invalid fields/reserved flags, duplicate/occupied IDs/addresses and late SQL failure roll back everything. Its inverse is clip.delete: one undo restores original clip/stream absence; redo/replay consumes only captured values. This branch adds one opcode (OPS catalogue **174**, from this base's 173), no schema or binary-layout change. Win reconciles that count with the independent automation opcode stack; no README edit. OPS §9.5 documents the payload/recovery/limits.

**Playback limits, not a full-Live completion claim:** native CC first-value chase, ordinary holds, owned pedal releases and cross-stream ordering stay as the existing playback contract. Raw input memory keeps exact serial order; committed native clips do not promise that cross-stream serial order or unknown controller state before a first point. Host/physical-input/UI Capture wiring remains caller work. Full arbitrary-double note/expression persistence and automatic stopped-transport Live phrase/tempo/loop inference remain open.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,215 checks / 111 suites**, every row/sum inspected, zero failures, all validators/layout pass. New native suite **41** covers actual MpeParser→memory→prepared op→Session audio in both precisions, held-end closure, source >2^53 clocks, uint64 extremes, all 44.1–768 kHz rates and step-tempo crossing, full-double CC, one undo/redo/reopen, binary-log replay under different selection/playhead/grid, coherent outer reads, NULL-head/reopen guards, callback SQL/allocation guards, 18 malformed native packets and frozen targets. A **10,000-note** Capture writes its stream exactly once (actual SQL trace), with complete one-step undo/redo. Clang19 compiles changed Capture/test translation units and runs all 41 against the validated core; changed catalogue also passes Clang syntax/Wshadow. Three actual Ninja-command Wshadow checks pass. Eight compiled plants fail normally: tolerated velocity narrowing (1), missing held end (5), removed exact sample guard (1), narrowed CC (4), reopened-document guard (1), missing head guard (1), accepted future layout (1), zero duration accepted (1). No crash/exception counted. MSVC/JUCE/hardware and measured Live parity remain unrun.

**Capture review block / next independent Phase D item:** full MPE persistence needs the format contract asked of win above; ANOT/AEXP cannot truthfully store all captured doubles as they stand. Exact automatic Live Capture inference also lacks authoritative fixtures/defaults: the repository's Master Reference preview says tempo and loop length are guessed when stopped but supplies no inference algorithm; official Ableton manual access is still proxy-blocked, not parity evidence. Win: provide/approve the persistence contract and stopped/playing Capture fixtures (phrase boundary, inferred tempo, bar/loop rounding). These are named blockers for those remaining Capture claims, not a seat stop or fabricated algorithm. Native MIDI-1/CC and audio Capture APIs are ready for integration in the meantime.

**Take next at once:** Phase D Scale Mode/tunings engine support. Reuse already-pushed N32 clip-details and N33 clip-properties context rather than duplicating their storage/ops. Read ADR-0178 and the Master Reference's §5.1 (extracted read-only at /tmp/adi-master-preview.txt), inspect exact feature commits rather than their newer win/main merge heads, and expose immutable tuning/scale-degree context, membership/folding and named degree data needed by the note editor/devices. No UI/device DSP edits, no guessed non-octave root mapping, and no premature full-Live claim. Then multi-threading/scanner/stem separation/Loudness Drive/Link/scope as the brief orders. Standing order remains gate, push, log, next in this same turn, including through context compaction; this is a checkpoint, not an ending.

---

## 2026-11-04 — Chord releases and ordinary CC now reach Capture; taking native MIDI clip preparation

**PR requested:** head `engine/midi-capture-input` at **`5dc751a285bbe7fc7925ec0b41985b2f05faf1dd`**, base `engine/log` (`09b0cd5`). Title: **Preserve plain MIDI chord releases and ordinary input controllers**. Explicit dependency: `engine/midi-memory` (`273cf7a`) and its declared typed-controller dependencies. Win opens/merges in order; this Linux seat leaves main untouched.

**Window/host handoff / PR body:** MpeParser now keeps a non-member channel's identities per key, so ordinary distinct-key chords release earlier keys correctly instead of emitting unknown IDs and leaving native-router voices alive. MPE members retain the existing single-note expression ownership. Reset clears the map without recycling IDs; unmatched releases remain zero. Same-key retrigger deliberately retains the former newest-identity release policy, not an invented FIFO rule. Every ordinary CC now emits normalized typed Controller input on its original zero-based address, including zone-master controls and non-timbre member controls. Member CC74/106 keeps its exact per-note 14-bit timbre path; outside a member these stay ordinary addressed CC. Other high-resolution CC pairs preserve both addressed components. Consumed status-bit data bytes refuse before indexing/state/normalization. Sink refusal remains counted/nonblocking. Actual parser→router and parser→Capture memory paths work without callback allocations/I/O. No schema/opcode/UI/device-production change; OPS §9.5 records scope and channel conventions.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,174 checks / 110 suites**, all rows/sum inspected, zero failures, all validators/layout pass. New input suite **21**, legacy MPE **72** and memory **32** pass in the full gate; focused Clang19 input **21** and legacy MPE **72** pass with full warning/conversion/Wshadow flags. Actual Ninja-command Wshadow passes for changed parser and new suite. Tests cover all 128 keys in reverse release order, channel isolation, velocity-zero off/reset/retrigger, no native-router stuck/unknown chord voices, exact packet clocks/IDs into Capture, all 128 CCs through the actual router, high-resolution pairs, MPE timbre/pedals/master controls, bad data and delivery rejection. Callback allocation/file-I/O guards remain zero. Six compiled plants fail normally: channel-only releases (5), missing ordinary CC (9), lost member timbre (2), wrong CC channel (5), invalid value-byte accepted (2), stale per-key reset state (1). MSVC/hardware/manual parity unrun.

**Next immediately:** owner-side native MIDI Capture from the exact snapshot. Use a coherent Store read and captured client/head guards, explicit new clip identity/placement, typed note identity matching and exact sample→project-step-tempo roundtrips. Existing native `note.insert` preserves uint64 IDs, and normalized source-quantized MIDI-1 velocity can roundtrip through ANOT exactly; reject arbitrary doubles that cannot rather than silently quantizing them. `controllers::writeRequest` can capture full-double CC points in addressed streams. Create the clip, write explicit-ID CC streams before note.insert's auto-created notes row so IDs cannot collide, then insert notes, all as one OpJournal transaction. Close held notes only at an explicit Capture boundary; refuse ambiguous/unmatched ownership/truncated input instead of inventing it. Raw transcript remains exact; native playback retains its existing cross-stream ordering. Expression persistence/format review and automatic Live phrase/tempo/loop inference remain open, not a claim of full Live Capture completion. The format question in the prior entry is still for win; independent native MIDI-1/CC preparation proceeds now, then Scale Mode and the brief's queue. Context checkpoint is a log entry, not a stop; keep taking the next item in this same turn.

---

## 2026-11-04 — Exact retrospective MIDI memory ready; fixing its live input prerequisite

**PR requested:** head `engine/midi-memory` at **`273cf7ae7e0b33fb77869f1890e7966098b2a561`**, base `engine/log` (`ffe4df1`). Title: **Retain exact typed MIDI input for retrospective Capture**. Explicit dependency: controller playback code commit `d7d7c0` and its controller-editing dependency, not the newer remote branch's win/main merge stack. Independent of the audio-memory/automation stacks. Win opens and merges in order; main remains untouched by this seat.

**Window/host handoff / PR body:** `engine/midi_memory.hpp` supplies one preallocated rolling typed-event memory per explicit input/target track and sample rate, with an explicitly declared note-channel convention. One producer serializes gates, live Event pushes at integer driver timestamps and exclusive block-end advance (including silent blocks); one owner snapshots concurrently. Armed OR monitored input listens independently of transport play/record. Forward rests preserve history; on/off, backward clocks, malformed musical input and pinned-slot drops start new epochs. Notes, expression and controllers retain double values, uint64 identities, original frames, >2^53 times and coincident serial order without MIDI re-encoding. Controller addresses remain 0..15/0..127; MpeParser note channels are explicitly One-based, clip notes Zero-based. Unassigned releases remain unresolved. Addressed parameter/modulation events are ignored without erasing musical history. Snapshots return immutable bounded history, current end clock and explicit prefixDropped notice after capacity eviction, or an empty racing-overwrite refusal. Producer never waits/allocates/locks/logs/does I/O; stats name copied/dropped/ignored/invalid events. Default 65,536 events, bounded 2..1,048,576. This is exact input memory plus the actual MPE-parser handoff, not host/UI wiring, a complete phrase, inferred tempo/loop or native clip persistence. No schema/new opcode/device-production change; OPS §9.5 documents it.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,153 checks / 109 suites**, every result row and sum inspected, zero failures, validators/layout all pass. New suite **32** passes GCC14 and Clang19 with the full warning/conversion/Wshadow flags; actual Ninja-command Wshadow checks pass for both new translation units. GCC ThreadSanitizer completes all 32 with no report (40,000-event producer, 821 successful owner snapshots / 45,381 attempts / 107 named pinned drops in that run). Callback allocation/file-I/O guards stay zero. Covers actual MPE input packet/frame/identity path, 14-bit pitch beyond float, integer/high-ID extremes, channels, rests/gates/restarts, ring eviction/immutability, malformed input and concurrent wraps/drops. Seven compiled plants fail normally: narrowed value (5), narrowed clock (3), ignored monitor (6), lost note identity (6), lost CC address (3), missing prefix notice (1), spliced clock (1). No crash/exception counted; MSVC/hardware/manual parity unrun.

**Next immediately:** finish Capture's live-input prerequisite before owner note composition. Current MpeParser has only one key/identity per channel even without an MPE zone: an ordinary same-channel chord loses earlier note-off identities. It also drops most ordinary CCs, despite the typed Controller path already playing. Fix plain-note per-key identity tracking and ordinary CC delivery while preserving member-channel MPE 74/106 timbre resolution. Validate consumed MIDI data bytes before array indexing; keep unmatched releases explicit and reset IDs safely. This is a concrete input defect, not a reason to stop the seat. Then compose native Capture notes/CCs, address the storage precision limit and Live tempo/loop inference, then Scale Mode and the rest of Phase D.

**Win format question / independent work continues:** exact input memory is intentionally wider than ANOT's velocity bytes and AEXP's float values. Full arbitrary-double/MPE Capture cannot silently use those fields as exact storage. A compatible versioned record extension (e.g. authoritative double tails with legacy prefixes) or another explicit persistence contract needs format review; no schema/version or lossy conversion has been invented here. Basic source-quantized MIDI-1 values may be persisted only with a demonstrated exact source roundtrip. The input parser/phrase work can proceed now. Context checkpoint is this log entry, not a turn ending; user standing order remains gate, push, log, next, continuing through app compaction.

---

## 2026-11-04 — Capture memory makes playing undoable clips; taking retrospective MIDI

**PR requested:** head `engine/audio-memory-capture` at **`c21e180f8e28a5ec23dd17ddf32f064c8068a560`**, base `engine/log` (`22215fa`). Title: **Materialize retrospective audio memory as an undoable playing clip**. Explicit dependencies: `engine/audio-memory` (`0f91906`) and N20 `engine/audio-input-recording` code commit `0768096`. Only that code commit was merged, not the newer branch head containing win's main/unrelated-lane merge. Win opens and merges in dependency order; main remains untouched by this seat.

**Window/host handoff / PR body:** Take an immutable input snapshot, then call `recording::captureMemory(store,snapshot,track,timelineStartSample,workFolder)` on its document owner. The helper resolves the project's source identity and creates a private Float32 WAV via AudioRecordingResult::fromMemory. Placement is explicit at the snapshot's rate, independently of its integer driver stream clock (which may exceed 2^53). Use existing recording::prepareCommit/commit with explicit ClipIds/names: the existing publication/dedup/rollback/retry path now makes a real playing Capture clip in one native undo step. Source bits, rate and frame count are preserved without resampling, normalization or dither; malformed/empty metadata, over-30-second/256-MiB snapshots and invalid timeline extents refuse before file reservation. Materialization failures clean only their own staging. Stale/failed commits retain private PCM for fresh preparation; Undo/Redo/reopen retains actual heard audio. InvalidCapture names invalid memory metadata/placement. No schema/new opcode/UI/device-production changes.

**Connection guard / integration:** prepareCommit and commit now require the same still-open Store pointer/client plus the captured head (including NULL). A reopened or different connection must prepare afresh, even for the same filename/head. A worker connection must commit there or prepare again on the owner; keep the originating window document generation across async work. This intentionally tightens the former path-only prepared-edit guard. Source-project identity is still checked. Capture/helper preparation refuses callbacks before database/file access; SQL tracing proves the database guard. Automatic UI Capture placement, physical input configuration/callback hookup and multi-input policy remain caller integration, not invented from stream timestamps. The RAM input API and owner materialization are concrete engine paths, not a claim that the existing app Capture button is wired.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,133 checks / 110 suites**, all result rows and sum inspected, zero failures, every validator/layout pass. New bridge suite **32** covers actual immutable input→WAV→native transaction→Session playback at both precisions, exact placement/duration, undo/redo/reopen, all 44.1–768 kHz source rates, float mantissa/NaN/signed-zero bits, reopen/head rejection, late rollback/retry, dedup, bad metadata, staging collision and actual SQL/file callback guards. Clang19 bridge/memory/raw recorder/recording commands/Session **5/5** pass (32/35/59/36/214 checks). Three translation units pass Wshadow. Six compiled plants fail normally: PCM narrowing (5), shifted placement (4), missing input identity (1), accepted empty memory (1), reopened Store guard removed (4), callback SQL guard removed (1). MSVC/hardware/UI/manual parity remain unrun; no README count edit.

**Next immediately / context checkpoint, not a stop:** retrospective MIDI Capture. The existing typed Event must retain double values, note identities, channels, controller addresses and exact integer timestamps; use the already-pushed controller event contract rather than another raw-MIDI encoding. `engine/midi-controller-playback` code commit `d7d7c032db351c98e251c86b6e3d5d639f7f7cb1` has been fetched and is available; current remote head `f17a5c7` contains later win/main merges, so inspect/declare the exact dependency rather than merging unrelated lanes. Begin bounded race-safe rolling typed-event memory, then owner phrase/note/CC preparation and Live-style tempo/loop inference with explicit parity limits. Existing ANOT velocity bytes and AEXP float values are a real persistence precision constraint: do not silently narrow arbitrary captured doubles or pretend those fields already preserve them. Log any needed format decision and continue independent work. After Capture: Scale Mode, multi-threading, scanner, stem separation, Loudness Drive, Link and scope in the brief's order. User's standing order is gate, push, log, next in this same turn; context compaction is not a reason to stop.

---

## 2026-11-04 — Capture audio RAM memory ready; taking undoable materialization

**PR requested:** head `engine/audio-memory` at **`0f919061ce5826829669fef52726110548f2b699`**, base `engine/log` (`8c8048a`). Title: **Keep thirty seconds of race-safe retrospective input audio**. Independent of the automation stack. Win opens the PR; no main changes/merges by this Linux seat.

**Window/host handoff / PR body:** `engine/audio_memory.hpp` supplies one preallocated AudioMemory per explicit physical input, default 30 seconds, listening while armed OR monitored. Queue gate changes and planar float input blocks to exactly one producer, with the driver's explicit integer stream-sample clock independent of the playhead. Listening on/off clears logical history; a gap, reset or rejected/pinned segment starts a new epoch, so the owner captures only a contiguous suffix. Irregular callbacks and callbacks larger than the ring retain exactly the latest configured duration. Source float bits (including signed zero/NaN payloads/above-unity) survive; no internal double graph bus is narrowed. Null channels explicitly mean digital silence; a hardware adapter must validate its selected input mapping. `snapshot(output,error)` runs on one owner, returns immutable interleaved samples plus input/epoch/clock/format, or an explicit empty refusal on racing overwrite/in-flight chunks. Producer never waits for the reader: bounded lock-free chunk ownership drops/counts pinned segments. Stats expose copied/dropped/ignored/invalid input. Constructor bounds identity/format/duration and caps RAM at 256 MiB/input. The default 48 kHz stereo sample payload is 11.52 MB plus metadata/spare chunk. Existing owner Float32 WAV I/O preserves the snapshot. No schema/opcode/UI/device production changes; this does not yet attach physical inputs or materialize a clip.

**Gate:** full GCC14 Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,006 checks / 107 suites**, every result row and sum inspected, zero failures, validators/layout pass. The lower count versus automation is the deliberately independent engine/log code base, not removed tests. New memory suite **35** passes GCC and Clang19 with Wshadow; actual Ninja-command Wshadow checks also pass for both changed translation units. GCC ThreadSanitizer completes all 35 checks with no report (concurrent 4,000-block producer and owner snapshots, 423 successful snapshots / 6,096 attempts in that run). Callback allocation/file-I/O guards stay zero. Tests cover exact default retention, all 44.1–768 kHz rates, multichannel and one-frame rings, multiple wraps/callback, gates, discontinuities/restarts, >2^53 timestamps, integer overflow, budgets, source bits, owner WAV roundtrip, independent inputs and immutable snapshots. Six compiled plants fail normally: ignored monitor (8), shortened memory (8), lost channel (11), narrowed clock (1), spliced gap (2), missing retention bound (5). No crash/exception counted. MSVC, hardware adapters and measured Live parity remain unrun here.

**Take next immediately:** bridge a snapshot to the already-pushed N20 recording take/import path so Capture creates an actual playing clip in one undo step. Reuse `engine/audio-input-recording`'s code commit `0768096` (same recording sources as current remote head `3d53172`; the later head merges main and unrelated lanes, so do not pull that whole stack into this item). Its AudioRecordingResult owns the private take and recording::prepareCommit/commit already handles publication, dedup, exact sample-boundary musical placement, rollback/retry and undo. Add a memory-origin take factory and owner helper rather than duplicating asset/journal machinery. Declare audio-memory and audio-input-recording dependencies explicitly. Then MIDI retrospective Capture, Scale Mode and the rest of Phase D, without ending at this context checkpoint. Full hardware/UI Capture wiring and Live tempo inference remain explicit work, not completed by this buffer API.

---

## 2026-11-04 — Send levels automate like faders; taking Capture audio memory

**PR requested:** head `engine/send-automation` at **`8e6d4ff11833490f7180052c55df8d20ed46ec7e`**, base `engine/log` (`c28b547`). Title: **Play automated send levels through fader and recording policy**. Explicit dependency: `engine/clip-envelopes` (`7ca44d0`) and its declared automation dependencies. Win opens and merges in dependency order; this Linux seat never touches main.

**Window/host handoff / PR body:** Existing native lane/point APIs can address a track-origin send with owner_kind=routing, owner_id=its routing row ID, param_ref=gain_db, value_domain=real (dB). Session refresh binds arrangement and source-track absolute clip envelopes to the send's existing second strip stage, preserving +24 dB ceiling, silence threshold, control cadence, five-millisecond ramps and both graph precisions. Enable Send/solo stay gates; source mute, Pre/Post fader/automation and track-delay/PDC stay in the first stage. Graph holders retain both programs independently. Ordinary routing.setGain edits override only that send lane, even when a track shares its numeric ID; Re-Enable resumes it. Send lanes join the existing Arm/write/commit/refresh/finish policy without permanent recorded overrides. Unsupported domains/parameters/non-send targets are named. No schema/new opcode/UI/device-production change; timestamp producers still need the prior explicit coordinator handoff.

**Solo policy:** Reachability conservatively includes an enabled send if its arrangement/default or any clip point can rise above silence anywhere in its program. Actual audio follows its current curve. This avoids callback topology/PDC changes, but can keep an arbitrary destination reachable in silent portions of an otherwise audible send. Wholly silent automation does not open it. This explicit policy is not a measured Live parity claim.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,266 checks / 113 suites**, every result row and sum inspected, zero failures, all validators/layout green. New send suite **45** checks covers real Store→Session float/double playback, ceiling, source mute, Pre/Post, seek/clip fallback, disabled sends, equal track/routing IDs, ordinary override and timestamped captured-data commit/resumption, solo reachability, target refusals, independent graph lifetimes, PDC and callback no-allocation. Clang19 send/mixer/Session/clip/write-policy **5/5** pass. Five translation units pass Wshadow. Six compiled plants fail normally: absent ceiling (2), absent binding (16), stored-knob solo reachability (1), wrong clip source (3), missing manual override (2), missing send write recognition (4). MSVC/JUCE, physical inputs/plugins and measured Live parity remain unrun here. README count remains win-owned.

**Next immediately:** Phase D Capture, starting its default 30-second RAM audio memory per armed or monitored input (ADR-0132 d9), then owner-side materialization and MIDI retrospective capture, before Scale Mode and the rest of the brief. Keep input identity/clock explicit, exact source samples, bounded producer work and memory, no callback allocation/lock/file I/O, and a race-safe owner snapshot. Do not confuse this retrospective buffer with transport recording or claim hardware/UI hookup from an engine API. Capture can branch independently from engine/log; it does not need the automation dependency stack. Standing order remains gate, push, log, next in this same turn; no stop at this checkpoint.

---

## 2026-11-04 — Absolute clip envelopes play; taking automated send levels now

**PR requested:** head `engine/clip-envelopes` at **`7ca44d02c1227560d9b49ea785b107a6aee0673d`**, base `engine/log` (`5aa99b0`). Title: **Play absolute clip automation envelopes through native lanes**. Explicit dependency: `engine/automation-write-policy` (`875c820`) and the preceding automation stack, integrated only into this item worktree. Win opens/merges; main is untouched.

**Window/host handoff / PR body:** Existing track/device lanes now play clip-relative automation_data as immutable absolute-value overlays while a clip's half-open placement is active, falling back to the arrangement stream/default. Latest audible start then clip ID owns overlaps; an underlying clip resumes its ongoing phase. Musical/linear placement, tick/ns envelopes, source offsets, loop intros/wraps, native curves through step tempos and full-precision values are compiled off-thread. Device emission sees rounded clip/point/wrap boundaries between cadence ticks, including downward and half-tie rounding; strips retain their existing grid/smoothing and both widths. Native values/curve phases are retained rather than renormalizing trimmed segments. No schema/opcode/UI changes. OPS §9.6 documents absolute semantics, bounds and limits. Stands on automation-write-policy.

**Gate:** full GCC14 Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,221 checks / 112 suites**, all rows and sum inspected, zero failures, validators/layout clean. New suite **48** checks includes 44.1–768 kHz, nonlinear curves through step tempos, offsets, loop prefixes/phases, overlap fallback/order, exact rounded events, transport wraps/seeks, float/double strip renders, callback allocation guards, Store→native edit→model→engine→undo, and malformed/unit/budget refusals. Clang19 Werror clip/arrangement/parameter-automation/mixer/Session **5/5** pass. Four changed/new translation units pass Wshadow; the existing arrangement test changes only its missing-clip diagnostic expectation. Eight separately compiled plants fail normally: reversed overlap (2), ignored offset (4), ignored loop (6), linearized curves (5), raw rather than rounded point timing (1), closed clip-end interval (9), unknown clock accepted (1), missing event boundaries (3). Validated sources/binaries were never replaced by plants. MSVC, real host/hardware and measured Live parity remain unrun here.

**Limits, explicitly not hidden blockers:** absolute values use the existing lane domain; a relative-modulation mode is not invented. Clip-owned gain/modulation and aliases remain named unsupported, while addressed clip streams on track/device lanes play. Invalid streams refuse a whole overlay, preserving valid neighbors and arrangement fallback; empty/muted clips do not mask it. Preparation caps per-lane points/boundaries at 1,048,576 and placements at 16,384 with a repeat budget; sub-sample loops are explicitly refused. Ramp tempos use the existing reported step fallback. Native loops on ns envelopes map musical phase through the source-relative tempo clock. No measured Live parity is claimed from those implementation rules.

**Standing order:** the director explicitly ruled that a context checkpoint is a log entry, not a stop. This seat resumes from the previous entry, gates/pushes/logs this item, and takes the next one without ending the turn for a report, CI, or queue length. No new director gate blocks independent work.

**Continuing immediately:** automate send levels like faders. Current SendNode already has a second StripNode for its own level; reuse that stage, lifetime retention, source mute/pre/post/solo semantics and PDC. Add routing-owned gain_db lane binding, retain the send ceiling, and include sends in ordinary manual override and recording-write policy without colliding with track IDs. Then resume Capture/Scale Mode/multi-threading/scanner/stem separation/Loudness Drive/Link/scope work in the brief's order; host/UI timestamp/recording coordination and Live parity remain explicit handoffs, not fabricated completions.

---

## 2026-11-04 — Session automation-write policy ready; context checkpoint and clip envelopes next

**PR requested:** head `engine/automation-write-policy` at **`875c820181ea505b0cfe88ae9bceb9472fe6f64b`**, base `engine/log` (`8299371`). Title: **Coordinate Session automation writing and manual overrides**. Explicit dependency: `engine/automation-recording` (`34c2bd2`) and its shape/point/lifecycle dependencies. Win opens and merges in that order; no main changes or PR merges by this seat.

**Window/host handoff / PR body:** Session now exposes owner-side Arm/record state and `beginAutomationWrite` / `finishAutomationWrite`. A recognized active write suspends its strip/device automation source independently of ordinary manual overrides; its parameter/fader journal changes do not light a permanent override. Gate-off prevents new writes but keeps the prior write suspended while captured points are pending. The owner closes/drains the producer, commits the prepared recording, refreshes Session, then finishes with follow=true to resume the new curve. Follow=false explicitly retains a normal manual override on capture/commit failure. Re-Enable does not cancel an active write; deleted/disabled lanes prune stale write state. Rebuild failure retains the requested policy for retry, like existing Re-Enable behavior; false is not readiness. No schema/new opcode/UI/host callback production change. Stands on automation-recording.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,173 checks / 111 suites**, all 111 result rows and their sum inspected, zero failures, every validator/layout check passes. Policy **31** checks proves the real Store→timestamped capture→native commit→Session refresh→resumed parameter-event path, both gate checks, pending stop, manual fallback, ordinary record-off behavior, strip lanes and deletion/undo cleanup. Clang19 Werror Session/parameter-automation/recorder/policy **4/4** pass. Changed Session translation unit and new suite pass Wshadow. Six compiled plants fail normally: ignored Arm (1), ignored record (1), missing suspension (4), permanent recorded override (2), premature stop cleanup (2), missing manual fallback (4). No plants changed validated sources/binaries. Windows/MSVC, real hardware/plugin timestamps and measured Live parity are still unrun here.

**Seat checkpoint:** four increments completed in sequence, each gated and logged before taking the next: lane lifecycle `a0c22af`, prepared shape insertion `3f35b87`, timestamped capture `34c2bd2`, Session write policy `875c820`. Earlier point editing remains `1c37d05`. The main checkout and every completed item checkout are clean. Win opens the PRs; Git HTTPS pushes work. No automatic publication or new-task restoration is claimed.

**Take next immediately:** clip-envelope playback in Phase D item 1, then automated send levels, then Capture/Scale Mode/multi-threading/scanner/stem separation/Loudness Drive/Link/scope engine work as the brief orders it. AutomationProgram currently reports every clip-scoped automation_data row as not played and accepts only track/device owners; that refusal is the concrete next code site. Read the format's clip-relative lane/data contract and clip placement/offset/loop model before extending it. Preserve arrangement fallback, overlap ownership, loop/seek behavior, all six curves, sample compensation and callback no-allocation guarantees. Do not silently guess relative modulation semantics or claim exact Live geometry from the prototype templates. Official-manual access is still proxy-blocked; its domains are saved in the environment draft but not applied/published here. Independent engine preparation continues.

**Remaining integration, not a readiness claim:** window/host producers must map target/value/time units correctly, queue controls to the single producer, delimit loops/seeks, and invoke the commit/refresh/finish sequence. Session policy and capture are validated APIs, not automatic wiring of every existing host callback or the UI Arm button. Keep the prepared document/client/head guards and supply the UI's document generation across async work. Full Live preset geometry/defaults need authoritative manual/fixture comparison; no parity signoff is invented. New item branches should declare this explicit dependency stack until win reports it merged.

---

## 2026-11-04 — Automation Arm capture/commit foundation ready; continuing Session write policy

**PR requested:** head `engine/automation-recording` at **`34c2bd28e8ff00cc4ae0d2a84c2ad4da4c8a349b`**, base `engine/log` (`ba1232a`). Title: **Add Automation Arm and timestamped recording capture**. Explicit dependency: `engine/automation-shapes` (`3f35b87`) and its point/lifecycle dependencies. Win opens and merges in dependency order; main is untouched.

**Window/host handoff / PR body:** `engine/automation_recording.hpp` supplies one bounded SPSC recorder per explicit lane/data/clip target. Arm defaults on, transport recording off; gates affect automation capture rather than normal ParamOps. Exactly one producer serializes controls and manual Begin/Value/End events with explicit lane-unit timestamps; UI controls must be queued to that producer. Disarm/stop close the prior gesture using the last held value; seek/loop boundaries must be delimited explicitly. Producer calls allocate/lock/log nothing and reach neither Store nor plugins. Overflow, invalid input and consumer point-budget exhaustion discard the entire gesture, counted instead of committing a partial curve. `prepareRecordedAutomation` resolves fresh IDs through the generalized one-revision `prepareAutomationWrite`; commit retains document/client/head guards and one exact undo. No schema/new opcode/UI/host callback production change. Existing callbacks/Arm button are not automatically wired, and gesture-final ParamEditCapture values are not treated as sample timing. Stands on automation-shapes.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,142 checks / 110 suites**, all validators/layout checks pass. Recorder **36** and refactored shapes **50** pass. Clang19 Werror core plus recorder/shapes/points/lifecycle/ops/replay **6/6** pass. Three changed translation units pass GCC Wshadow. Recorder checks include allocation guards, 1000 gestures across real producer/consumer threads, bounded overflow recovery, gates, exact timestamps above 2^53, stop endpoints, data-unit flags, malformed input, and native commit/undo/redo. Six independently compiled plants fail normally: ignored Arm (5), ignored record state (6), narrowed value (3), lost timestamps (8), partial overflow recording (2), ignored taint (2). No crash or timeout is counted as a plant success. MSVC, real host/driver timestamps and Live parity remain unverified here.

**Continuing immediately:** Session's write policy. While an armed recording gesture is active, its lane must be suspended without lighting the ordinary manual-override state; manual parameter journal changes during that write must not permanently override the lane. Only after the captured data commits and refreshes should the owner resume its automation. This is still Automation Arm integration, before clip envelopes/send playback; do not claim the full host/Session path is finished from capture alone. Then continue Phase D in order. The director requested more than one item per seat; lifecycle, shape insertion and capture have each passed and been logged before taking the next one.

---

## 2026-11-04 — Prepared automation shape insertion ready; taking Automation Arm now

**PR requested:** head `engine/automation-shapes` at **`3f35b877aa8049a224f660dedf25926a9a2dc2e5`**, base `engine/log` (`5b2f102`). Title: **Add prepared automation shape insertion**. Explicit dependencies: `engine/automation-edits` (`1c37d05`) and `engine/automation-lanes` (`a0c22af`), integrated only on this item branch, with their catalogue counts reconciled to 174. Win opens/merges the dependency PRs before this one; no main merge or history rewrite was performed.

**Window handoff / PR body:** `automation_shapes.hpp` supplies bounded generation for sine, triangle, saw/inverse, square, exponential/logarithmic rise/fall and configurable ADSR, plus `prepareShapeInsertion`/`commitShapeInsertion`. Preparation resolves fresh explicit point IDs and an inclusive-range delete/write batch under one SQLite read snapshot without writes/reservations. Commit guards the original open document/client and exact history head, including NULL. Existing outside records remain; resulting point interpolation can change sound across unanchored boundaries. Native vertices/curve descriptors preserve triangle/ramp geometry; sine uses requested sampling. Integer quotient/remainder preserves times beyond 2^53. Templates, defaults and boundaries are documented as implementation behavior awaiting Live comparison; blocked official-manual access is not a measured parity claim. No schema, new opcode, UI, device or callback change. Stands on the two automation dependencies above.

**Gate:** GCC14 full Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,106 checks / 109 suites**, all individual results and sum inspected; all validators/layout checks pass. Clang19 Werror core plus shape/point/lifecycle/automation/ops/replay tests **6/6** pass; final strengthened shape suite **50** passes under both compilers. New shape source and suite pass Wshadow; integrated catalogue was already covered by the lifecycle/points gates and compiled under both compilers. Six separately compiled plants fail normally: narrowed int64 time (1), missing NULL-head guard (4), missing document guard (2), retained selected points (2), mixed snapshot reads (2), ignored lane bounds (1). A deterministic second connection commits between head and metadata reads and proves one-revision preparation plus stale refusal. Final sources and validated binaries are unchanged by plants. MSVC/remote CI/Live visual and render parity remain unrun here.

**Continuing immediately:** Automation Arm and timestamped gesture recording into the same native lane/point operations. Existing ParamEditCapture only records gesture-final values at drain time; it cannot be relabelled sample-accurate automation capture. Add a bounded producer/consumer capture with explicit lane-unit timestamps, recording/arm gates and atomic owner-side commits, preserving normal parameter edits while disarmed. Then clip envelopes, send levels and the rest of Phase D. Exact Live preset geometry/defaults remain a validation follow-up when the manual/fixture prerequisite is available; no need to ask the director before independent recording work.

---

## 2026-11-04 — Automation lane lifecycle ready; taking shape insertion immediately

**PR requested:** head `engine/automation-lanes` at **`a0c22af586208c8b2cb1ffa68c3312e0d41c5dc4`**, base `engine/log` (`29a2c7f`). Title: **Add undoable automation lane lifecycle**. Independent of `engine/automation-edits`, which is already pushed. Win opens the PR and merges; this Linux seat does neither merge nor claim MSVC/remote CI success.

**Window handoff / PR body:** `automation_lanes.hpp` now supplies `createLane(LaneSpec)`, delete, clear, enable, visibility and default-value requests for the document owner to commit through OpJournal. Creation carries every default explicitly and validates the existing polymorphic owner, identity, time base, value domain and bounds. Clear empties all arrangement/clip streams while retaining row/clip identities, future header version/stride/flags and trailers; any unreadable stream rolls the entire operation back. Delete undo restores every stored scalar and every stream byte, including legacy metadata and malformed blobs. Captured `automation.restoreLane` inverses preserve original absence. No schema, UI, device-production or audio-callback change. OPS §9.6 documents the native lifecycle. This branch adds one opcode to its base catalogue; combine its count deliberately with automation-edits' restoreData when both are integrated.

**Gate:** full GCC14 Debug/Werror/PD-on/JUCE-off build and test_all.sh **10,010 checks / 107 suites**, all validators and binary-layout checks pass. Lifecycle **39** checks covers explicit replay, exact undo/redo, metadata bounds and owners, populated cascade restoration, future streams, clip slots, malformed later-stream rollback and caller-hook rollback. Clang19 Debug/Werror core/lifecycle/ops/replay build, **3/3** focused suites pass. All three changed translation units pass GCC Wshadow. Four separately compiled plants fail normally: missing owner validation (5), lost restored streams (4), normalized future header (1), narrowed double default (1). Validated binaries and checkout sources were never replaced by plants. Main is untouched; item and log use separate worktrees. Windows/MSVC and Live parity remain win's acceptance legs.

**Prompt/network evidence:** the requested second-issue `collab/prompts/2026-11-04-engine-lane-codex.md` does not exist in fetched origin/main or origin/engine/log; the director's direct correction supplies the Linux-seat/win-opens-PR rules. No fabricated prompt read is claimed. The official Ableton automation manual request returned proxy CONNECT 403. The environment draft now adds ableton.com/www.ableton.com while preserving existing domains/presets; persistence is confirmed, runtime application/publication is not. Exact Live preset/parity evidence remains unverified; this does not block independent insertion/recording mechanisms.

**Continuing now:** shape insertion on the pushed point/lifecycle APIs, then Automation Arm, clip envelopes and automated sends, in Phase D order. Earlier needs remain taken by pushed feature branches. Do not end the seat after this item; the director explicitly requested continued work through the context/usage boundary.

---

## 2026-11-04 — Native automation point edits ready; continue Phase D

**PR requested** — head `engine/automation-edits` at **`1c37d0563c16580e19510c61631c538dbc71519f`**, base **`engine/log`** at `f3fe7c5c37ea01c450e1878a217b455da66fcfb8`. Title: **Add undoable automation point editing**. The tested branch is pushed; win/window opens the PR, win merges. GitHub GraphQL returned Forbidden, so no opened PR or remote CI success is claimed.

**PR body / window handoff:** The document owner can submit `automation::writePoints`, `deletePoints`, `movePoints`, and `setCurve` through `OpJournal` for an existing arrangement or clip lane. Requests explicitly address the automation_data row, lane, clip (null for arrangement), and full uint64 point IDs. Writes upsert; moves use absolute int64 time/double value; curves use the existing six formulas and finite -1..1 tension. All-or-nothing selection validation, stable coincident-point order, flags/reserved bytes, future record suffixes and trailers are retained. Captured `automation.restoreData` restores exact bytes or original absence. Inserting unknown newer-format fields is refused. OPS §9.6 documents the payloads and limits. No schema, UI, device production or callback changes. This is the native point-editing prerequisite, not completion of shape presets or all Phase D. Stacked on `engine/log` at the base above.

**Validation:** GCC14 full Debug/Werror/PD-on/JUCE-off build and `tools/test_all.sh`: **10,017 checks / 107 suites**, every result and the sum inspected, all validators and binary layout checks clean. New edits suite **46** checks includes deterministic replay, exact undo/redo, finite/range/refusal checks, full-width IDs and values, future-layout reordering/deletion, clip-relative nanosecond flags, owner/data mismatch, engine compilation/evaluation, and final commit-hook rollback. Clang19 Debug/Werror builds the core and passes edits **46**, automation **39**, ops **130**, replay **63**. All three changed translation units pass GCC Wshadow. Five independently compiled fault plants fail normally: value narrowing (2), ignored curves (1), dropped trailer (2), ignored address (4), undo absence retained as an empty row (3). The final plant variants do not edit checkout sources or replace the validated binaries. MSVC, JUCE host/GUI, seven-ABI CI and measured Live parity remain unverified in this Linux seat; win owns merge acceptance. No README count edit.

**Seat and base discrepancy:** The uploaded 4 November work-seat instructions describe Windows and say `engine/log` contains the rebuilt #382–#421 feature stack. This actual machine is Linux; fetched `origin/engine/log` is `f3fe7c5`, carrying the newest handoff but not those feature implementations (including the controller source APIs). The required base was preserved, and this increment has no dependency on absent feature code or schema minors. Win should reconcile the stack base and OPS catalogue count with the existing feature branches during integration; no reset, force push or duplication of those features was performed. The main checkout remains untouched, with code and log in separate worktrees.

**Network:** Existing Git HTTPS authentication works for reads and push. The draft now adds `api.github.com` to the existing network policy, preserving all presets; saving is confirmed but runtime application/publication is not. No token was requested or exposed. The fallback PR title/body is recorded here and a local copy is `/workspace/engine-review/automation-edits-pr.md`. Clang was extracted under `/workspace/adi-clang` from checksum-verified Debian packages obtained through its signed official HTTPS archive; no verification was disabled.

**Queue:** Continue Phase D item 1. Implement native lane creation/deletion and lane enable/visibility/default/clear operations with exact inverses, then Live shape insertion on these point APIs, Automation Arm, clip-envelope playback and automated send levels. Point editing currently requires an already existing lane; it does not introduce a UI SQL path or claim those follow-ups are built. Earlier needs remain taken by the existing feature branches. Capture, Scale Mode, multi-threading, scanner, stem separation, Loudness Drive, Link and scope engine work remain later in the standing queue. No LANES §5 question blocks continuation.

---


## 2026-11-03 — Controller lanes play through the graph and hosts; on to Phase D automation

**PR requested** — head `engine/midi-controller-playback` at **`d7d7c032db351c98e251c86b6e3d5d639f7f7cb1`**, base **`engine/midi-controller-editing`** (`c16d1cd`, pending PR above main). Title: **Play MIDI controller lanes through compensated graphs and hosts**. Depth two, explicitly depends on controller editing/read APIs. Win/window opens; win merges. GitHub API remains inaccessible here.

**PR body:** Compiles addressed controller data into immutable per-track timelines, covering musical/linear clip placement, offsets, loops, overlap precedence and all six curve shapes/tensions through step tempos. The MIDI source chases full-precision values before notes, emits on a global 64-sample cadence plus exact boundaries, and retains owned CC64/66/69 releases through stop/seek/wrap/mute/deletion/rebuild, graph compensation and overflow. Delivery tokens route acknowledgements only to the matching source by binary search; retries retain ownership until release delivery. Controller pressure preserves note space and rotates addresses. The event stays double through the graph and mapped VST3 parameters; CLAP narrows at its advertised MIDI boundary, alongside native notes. Hosts count unsupported controllers. No schema, new op, DSP or UI production changes. OPS §9.5 documents timing and host limits.

**Window handoff:** the previous PR's committed `cc.write`/`cc.delete` data now plays after normal `Session::refresh`; no extra UI playback path or Store reads from callbacks. Empty lanes send nothing; ordinary CC holds after placement ends, owned pedal releases do not keep cancelling unrelated live input after acknowledgement. Invalid lanes, aliases and unsupported tempo ramps report preparation problems. Latest clip start wins a shared address, with clip ID as tie breaker. `MidiClips::droppedControllers()` counts deferred/refused updates. CLAP ports must advertise MIDI/MIDI-MPE for ordinary CC; unsupported ports are counted. VST3 uses IMidiMapping where the existing host can reach IEditController; JUCE's unexposed separate controller remains an explicitly counted unsupported route. Do not claim every VST3 accepts CC. Existing Pd/native MpeRouter consumers receive the new control route without touching device production.

**Validation:** full GCC14 Debug/Werror/no-JUCE/PD-on build and `test_all.sh` **10,121 checks / 108 suites**, every C++ result and sum inspected; all validators green. Focused GCC/Clang: new playback **90**, MIDI clips **167**, CLAP **490**, MPE output **91**, graph **200**; Clang controller editing **54**, engine **119**, ops **130**, replay **63** also pass. Real Session commit→model→source→instrument, cascade deletion, both graph precisions, 44.1–768 kHz, irregular callback sizes, three pedals across five transitions, same-source rejection retry, pending compensation, empty/full lists and rotating controller pressure are covered. Callback allocation/file-I/O guards stay zero. Seven changed translation units pass direct Wshadow compilation. Fourteen separately compiled fault plants fail normally: omitted curves, wrong tempo phase, reused loop offset, reversed overlap owner, narrowed values, unowned hold-2, skipped pedal release, missing note reserve, callback-local cadence, false rejection acknowledgement, lost channel, truncated MIDI rounding, absent CLAP MIDI wire and reused source namespace. No crash counted. MSVC/JUCE host build remains CI's leg; README check totals remain win-owned.

**Queue:** refreshed main/window/devices refs; main remains `9be60a8`, window/log `49b597d`, devices/log `28cf3b2`; no new engine need. Earlier table items already have pushed implementations/PR requests; no duplicate work on unstruck rows. Phase C note transforms and controller lanes are delivered. Continue Phase D item 1: undoable automation edits needed by shape insertion, then Automation Arm, clip envelopes and send levels. No LANES §5 question blocks. This continuation chains directly to the next item.


## 2026-11-03 — Controller-lane editing and immutable data ready; playback follows

**PR requested** — head `engine/midi-controller-editing` at **`c16d1cdb7551de2615fe2571dbb36bbca18307fa`**, base **`main`** (`9be60a8` at cut). Title: **Add undoable MIDI controller lanes and snapshot data**. Independent of note transforms/daily/compaction. Win/window opens; win merges. The environment still has no GitHub API.

**PR body:** Adds native cc.write/cc.delete plus captured cc.restoreStream on the format's existing addressed event_streams/ACTL rows. Explicit row/channel/controller/point identities, full normalized double values and curve/tension/flags survive writing and exact undo/redo. Point deletion preserves unrelated records, including future tails and trailers; an unknown wider layout refuses invented new records. Invalid/unbound streams remain visible with errors. ClipNode.controllers and rows::Model.controllers carry the typed data, raw bytes and errors; every field participates in sharing. SnapshotBuilder now reads controllers and clip/project metadata under one SQLite snapshot. No schema, UI, DSP or callback change. This PR supplies editing/read APIs; prepared controller playback and host delivery follow in the next branch.

**Window handoff:** use `controllers::prepareWrite(Store&,clip,channel,controller,points)` on the document owner. Zero point IDs request fresh explicit IDs; it locates an existing slot or allocates a noncolliding event-stream ID and returns the actual IDs/request. Preparation writes/reserves nothing and respects a caller-owned read transaction. `commitPrepared` guards the captured head (including NULL), file/open-client identity; retain document generation across async work. The window needs no SQL access. Existing lane/point IDs are in `ClipNode.controllers` / `controllers::readLanes`; channel 0..15/controller 0..127 are storage addresses. `writeRequest`/`deleteRequest` also build explicit OpRequests for batching. Values remain normalized double, with MIDI narrowing deferred to the host. Deleting a lane's last point leaves its empty addressed row; undo of first creation restores absence. OPS §9.5 gives payloads, curves and error behavior. Playback is still pending this companion increment; do not present an unbuilt host route as sounding.

**Validation:** final full GCC14 Debug/Werror/no-JUCE/PD-on build/runner **10,025 checks / 107 suites**, every C++ result and sum inspected; all five validators pass. GCC/Clang controller **54**, engine **119**, ops **130**, replay **63**, MIDI playback **165**, project view **14** pass. Direct Wshadow builds of catalogue, controller code, snapshot, row adapter and suite pass. Fifteen compiled plants all fail normally: missing prepared head/document guards, invalid channels, narrowed uint64 point IDs/values, detached tails, partial missing-ID deletion, wrong clip slot, omitted controller sharing, mixed controller/clip revisions, hidden malformed lanes, missing row-model data, absence restored as an empty row, preparation writing metadata, and invented future record fields. No crash counted. A deterministic second-connection edit between queries proves one-revision snapshots. Producer tests include fresh root/NULL guards, retry refusal, same-file reopen invalidation and no reservation writes. README counts remain win-owned.

**Integration:** independent OPS.md additions from note transforms and this branch each add one inverse opcode; combine the table/count deliberately when both merge. Existing freeze's catch-all source fingerprint traverses FK-owned event_streams rows, so CC changes remain source material; combined branches still need their integration tests. Controller playback must consume the new immutable row data, preserve full value precision to the host boundary, chase/loop/seek/reset held pedals safely and keep callbacks allocation/lock/log free. Continuing that now on one branch above this one (depth two), with no director question.


## 2026-11-03 — Phase C native quantize/transpose ready; controller-lane editing next

**PR requested** — head `engine/midi-note-transforms` at **`cd40539b7bf2733919ef80ac8c4ae9629d5a07e7`**, base **`main`** (cut from `9be60a8`). Title: **Add native MIDI note quantize and transpose**. No unmerged dependency. Win/window opens the PR; win merges. API remains unavailable here.

**PR body:** Completes Phase C's missing quantize/transpose native editing operations with explicit full-width note IDs and numeric settings. Quantize supports starts, ends, strength, swing and grid origin with exact int64 arithmetic, including beyond 2^53 and overflowing start-minus-origin differences. Start-only keeps duration; collapsed independently snapped ends keep one tick; overflow refuses the entire edit. Transpose shifts the listed pitches and refuses the whole selection if any would exceed 0–127. Both preserve unrelated note fields and expression identities. Quantize's compact CBOR-binary inverse restores the exact original stream, including coincident-note order, future record tails, flags/version and trailer. No schema, UI, DSP or callback change.

**Window handoff:** `notes::quantizeRequest(clip, ids, QuantizeSettings)` and `transposeRequest(clip, ids, semitones)` from `note_transforms.hpp` return native OpRequests; commit one, or a batch for a multi-clip gesture. Strength/swing are integer hundredths of a percent (10000 = 100.00%); factories capture every grid/origin/start/end setting into the payload. UI selection bits/playhead/grid are never read. Native `note.restoreNotes` is the inverse's exact stream packet and cannot replace the note identity set. OPS §9.5 documents the fields and refusal rules. This extends N17's separately requested material/fade/Auto-Warp APIs; it does not repeat those branches. Controller lanes are still the next Phase C increment. Tie rounding, one-tick collapsed notes and transpose range refusal are explicit implementation behavior; no measured Live parity claim.

**Validation:** full GCC14 Debug/Werror/no-JUCE/PD-on build plus `test_all.sh`: **10,007 checks / 107 suites, all five validators clean**; every C++ result and sum inspected. Final GCC/Clang new suite **36**, ops **130**, replay **63**, MIDI playback **165** pass. Direct Wshadow builds of catalogue, transforms and the suite pass. Nine compiled plants fail normally (1/1/1/2/1/1/2/1/2 checks): restored-ID loss, ignored strength/swing, narrowed tick phase, detached record tails, changed start-only duration, ignored selection, omitted transpose bounds, normalized inverse layout. Source bytes restored exactly after reordering and after undo/redo; valid-empty restore cannot orphan expression identities. Clang's test-initializer braces warning was fixed before final validation. Two unregistered prior-history executables were archived before the full run; no stale results counted. README counts remain win-owned.

**Continuing:** native `cc.write`/`cc.delete`, addressed controller-stream reads and snapshot data. The schema already has event_streams.channel/controller, so no invented controller table is needed. After editing, connect prepared controller events to the graph/hosts as the next increment; the engine EventType currently carries notes/expression but no ordinary control events. No director gate blocks.


## 2026-11-03 — Active undo caps and replay checkpoints ready; chaining into remaining Phase C MIDI edits

**PR requested** — head `engine/history-compaction` at **`3c1b8acd6e8e8aa65acfad85254a1f292b83db52`**, base **`engine/history-retention`** (`75909f7`, daily snapshots requested immediately below). Title: **Compact undo history into replayable project checkpoints**. This is the second and final stack level. Win/window opens; win merges daily retention first. The feature is pushed; no actual PR URL is available in this API-blocked seat.

**PR body:** Implements ADR-0203 d13's active undo cap of 2,000 transactions or 1,000 decimal MB, with optional smaller project budgets. Each native journal commit atomically checkpoints and prunes a complete oldest prefix before its final caller hook. Schema 1.10 adds one typed-CBOR project-state checkpoint at the retained root; undo stops there, while redo, named snapshots and reachable branches remain valid. All existing snapshots, branch heads/forks and unuploaded collaboration seqs protect their points; protections can leave the journal above budget and are reported. The newest whole transaction and legacy embedded media remain protected. Checkpoint failure or final hook rejection rolls back the edit and compaction together. Replay restores the checkpoint into an empty-journal target, preserving UI metadata, then replays only source ops after baseSeq. No UI, DSP, audio-callback or new opcode change.

**Window/sync handoff:** `History::setUndoRetentionPolicy(UndoRetentionPolicy)` persists the project policy and compacts; `compactUndo()` reports remaining transaction/byte counts, baseSeq, removedOps and protectedHistory. Normal journal commits enforce it automatically. Collaboration must be enabled before its edits, and uploadedThroughSeq means ALL local seqs through that point were uploaded; missing ack protects everything. Transfer any existing checkpoint along with a new collaboration session's log. N29's undo-list adapter must exclude the retained root, as `nextUndo()` already does; integrate its sibling after both land. Replay/export-log consumers hold a source read transaction across `restoreReplayBase(source,target)` and retained-log reads and skip the root's already-folded forward op. Metadata/snapshot callers remain as the daily handoff below. Details: ENGINE-HISTORY-RETENTION and format/SPEC §8.3.

**Validation:** final full GCC14 Debug/Werror/no-JUCE/PD-on build and runner: **10,028 checks / 108 suites**, every C++ result and sum inspected, all zero failures. CI/op/PD/layout validators pass. **Sole failure: schema validator's win-owned README count, 51 tables versus 52**; all schema invariants and frozen 1.0–1.9 migrations to 1.10 pass. Win updates the count and reconciles the independent schema-1.10 siblings during integration; no README edit here. GCC/Clang compaction **49**, daily **38**, history **95**, snapshots **65**, replay **63**, migration **162**, text projection **210**, integrity **43**, collect/export **63** pass. Direct Wshadow builds of history, ops, compaction, integrity and the new suite pass. **Eleven compiled plants all fail normally** (2/2/2/2/2/1/1/1/2/1/2 checks): ignored stored policy, wrong boundary state, bytes ignored, named/unuploaded/fork protections removed, partial-batch cut, inverse-less root offered, NULL changed to TEXT, late caller hook outside transaction, restore without transaction. No crash counted. Tests read the first committed batch boundary directly, without invoking maintenance that could repair a planted partial cut; branch protection is also tested after switching to the older line. A legacy extraction compatibility failure from the first broad run is fixed and covered by collect/export; aborted/earlier runs are not final evidence.

**Boundaries:** the byte budget covers active journal payload/inverse/selection, UTF-8 metadata and fixed per-row accounting, not the whole file/checkpoint/content pool. Checkpoint capture can cost project size plus retained-history rewind on the owner; never call from audio. Snapshots remain usable and thus can protect history above cap until explicit removal/automatic snapshot-budget eviction. Legacy embedded rows keep their replay log until extraction, since current triggers forbid restoring those old embedded rows. No claim of measured Live parity for numeric retention UI.

**Queue:** history caps and daily snapshots are now delivered, with N3/N30 separately delivered earlier. Continuing Phase C's missing quantize/transpose/controller editing; tempo analysis/Auto-Warp and the material/fade/Time APIs are already in `engine/phase-c-editing` and are not being redone. Latest fetch main **9be60a8** includes win's N14/N4/N20 store/registry through #360 and the mixer timeout #376; the engine recording half remains in its earlier branch. Window log/needs table is unchanged (`49b597d`), no new engine rows. No LANES §5 gate blocks.


## 2026-11-03 — Daily history snapshots and persistent-snapshot budgets ready; undo compaction next

**PR requested** — head `engine/history-retention` at **`75909f71439bf0afad0d408da42629e214004079`**, base `main` (cut from `61eb28d`). Title: **Add daily snapshots and bounded snapshot retention**. GitHub API remains unavailable in this seat; win/window opens the PR and win merges. No other feature dependency.

**PR body:** Implements ADR-0203 d13's daily automatic snapshot and 10,000 local / 100,000 collaboration persistent-snapshot limits. Opening an unchanged document creates nothing; the owner service records changed days and refreshes that day's automatic point after completed edits, undo or redo, including after reopen/timezone changes. Renaming an automatic point pins it as named. Retention removes only oldest automatic points; all-named limits report a block. Eviction, insertion/update and persistent day markers commit together, with rollback and same-head retry on failure. Explicit snapshot removal is metadata, like rename. No schema, opcode, UI or audio-thread change.

**Window handoff:** construct one `DailyHistorySnapshots(Store&, HistorySnapshotPolicy)` before accepting edits on New/Open/Save As; call `poll(nowUtcMicros, utcOffsetMinutes)` after completed edit gestures/undo/redo and before close. Recreate the service when replacing the Store. Poll returns Unchanged/Created/Updated/Blocked/Failed and ID/name/error; there is no timer or clock read. `History::takeSnapshot` accepts the same optional budget policy, `renameSnapshot` promotes automatic points, and `removeSnapshot` explicitly frees a slot. See `docs/ENGINE-HISTORY-RETENTION.md`. Persistent snapshot metadata adds no journal operations. These APIs do not replace the separate N3/N30 file commands.

**Validation:** full GCC14 Debug/Werror/no-JUCE/PD-on build and `test_all.sh`: **9,963 checks / 107 suites**, every C++ result and sum inspected; all five validators pass. GCC14 and Clang19 daily-policy **38**, snapshots **65**, history **95** pass. Direct shadow-warning builds of both changed production units and the new suite pass. Eight compiled plants fail normally (3/5/6/12/1/1/5/1 checks); no crashes counted: named eviction, failed promotion, phantom open, duplicate day, lost pending retry, negative-date floor, budget off-by-one and eviction without a transaction. Full runner was rebuilt after removing unregistered freeze executables; an earlier stale run was stopped and is not validation evidence. README counts remain win-owned.

**Queue:** this completes daily snapshots and persistent snapshot budgets, **not** active undo's 2,000 transactions / 1,000 MB compaction. Continuing that independent increment now. It must store a replayable checkpoint, preserve named snapshots/reachable branch points and refuse to compact unuploaded collaboration edits. Freeze Group's final request remains `c7fecf74` below; live-key Freeze All still needs the host key-routing capability described there. No LANES §5 question blocks.


## 2026-11-03 — Freeze Group added; final track-freeze head supersedes the earlier request

**PR requested / update the earlier request:** engine/track-freeze ->
**engine/track-delete-undo**, head **c7fecf74daebd9e059b4226c413c3e61f8f24c71**,
title **Add track and group freeze caches with native undo**. This is the SAME
freeze topic, still two deep on N37 (945f8d0); no new third-level stack. The
ordinary checkpoint 2f9333c below is superseded by this head. GitHub API remains
unavailable; win/window opens the PR and relays native CI / MSVC /WX.

**Final body:** implement ordinary whole-track freeze and single whole-group
freeze with verified dry/wet cached audio, preserved original clips/device state,
explicit tail, one native undo step and recoverable no-clobber publication. The
group mode caches the summed bus and root inserts, parks every descendant reader
and device, and removes child audio connections from the realized graph. Original
routing rows are retained. Group fader/pan/sends/parent console remain live. Child
audio routes to outside the group stop until unfreeze: an explicit capture option
acknowledges that tradeoff. Descendant arm switches are saved/disarmed/restored
with cache scope; undo/redo/reopen and N37 track-delete undo preserve them. A root
deletion retains surviving children's current disarmed state; its undo restores
the frozen group exactly. Graph filtering follows original planning and remaps
indices, preserving explicit routing rather than creating default outputs.

**Window API addition:** set FreezeCaptureSpec.wholeGroups=true and select
non-overlapping group roots; acknowledgeChildAudioRoutes=true only after the
user chooses the child-route tradeoff. Then use the same generation-guarded
capture/prepareFreeze/commitFreeze/unfreeze calls from the prior entry. All actual
contributing child device identities are captured as well as the root's.
Snapshot children expose frozenByGroup, effective frozen=true and
freezeMode=group-member; the owning root's mode is group. Look up the owner to
find its cache and unfreeze the whole group. Every field is in same(), including
an ownership change between nested groups with otherwise unchanged child data.
The native guard rejects edits to baked child controls and rearming while
allowing the live root controls. Cache fallback resolution now feeds the actual
playback reader, with size/time invalidation before reusing a verified location.
New capture jobs do not copy/open parked group originals, so other tracks can
still be frozen with those originals and plug-ins offline. See
**docs/ENGINE-FREEZE.md** for the final contract and supported boundaries.

**Boundaries remain explicit:** capture requires Solo off for a whole group,
no external audio/key inputs into its subtree, baked VCA controls inside it,
and no already frozen children. A master in the scope or MIDI forwarded without
its included instrument is refused. Nested ordinary groups work. Freeze All
Channels with independently adjustable child balances/live keys and live-prefix
modes are still subsequent work, not claimed closed here. Float32 cache encoding
is an explicit precision boundary; no bit-exact 64-bit null claim. No UI, device
production, DSP or README edits.

**Format:** additive schema **1.11**, frozen_groups with captured descendant IDs
and original arm flags, plus group.setFreezeScope and its symmetric captured
inverse. The 1.10 cache schema is frozen separately and left intact; migration
adds an object, not columns to an already numbered schema. Generic N37 capture
retains the new dependent BLOB without a special undo path. Update the earlier
PR body's schema note: **win's README table count must become 53**, not 52, for
the strict schema gate; reconcile sibling schema numbering when combining PRs.

**Verified:** full GCC **10,166 checks / 111 suites**, every C++ result and sum
inspected and passing. CI/OPS/PD/layout validators pass; the sole schema validator
failure is win's README **51 versus 53 tables**. Migration/canonical/schema
checks otherwise pass. GCC + Clang: group **65**, ordinary native **79**, capture
**27**, fingerprint **30**, Session **214**, graph host **124**, device host
**60**, engine/snapshot **128**, track-delete **38**, migration **175**, text
projection **213**, all pass. Actual compensated PCM at both processing widths,
analog bus taps without double processing, missing original files/plug-ins,
actual pruned child/send paths, saved arm restoration, late SQL rollback and
same-job retry, fallback playback, and independent capture with group originals
offline are exercised. A direct shadow compile caught a nested lambda's row
name; renamed it, then rebuilt/retested group/native with GCC and Clang. Nine
final shadow compiles pass. Ten separately compiled plants fail normally:
child-fader fast guard (4), child arm restore (16), child prune (3), owner sharing
(1), cached dry bus tap (1), fallback (2), parked original input copying (2), Solo
(1), external key (1), plan-transform dispatch (3). No crash counts as a plant.
The earlier ordinary increment's 11 shadow checks/nine plants also remain valid.

**Next:** continue Freeze All Channels/live-key handling, then the remaining
LANES queue. No director gate.


## 2026-11-03 — Whole-track freeze caches, original-state retention and native undo

**PR requested:** engine/track-freeze -> **engine/track-delete-undo**, head
**2f9333c089a6fc45015853cb29e42e12e06e93dd**, title **Add whole-track freeze
caches with native undo**. Two deep: requires N37's generic dependent-row
track-delete/undo capture (945f8d0). Win merges that first. GitHub API remains
unavailable here; please open and relay native CI / MSVC /WX.

**Body / behavior:** a frozen flag previously left the original readers and DSP
running. This increment captures verified private Pre FX/Post FX WAVs from one
WAL-inclusive snapshot and one multistream Session pass, publishes immutable
owned media, and commits cache metadata/disarm flags in one native undo step.
Original clips, parameters and opaque state remain intact. Playback skips parked
plug-ins and original media; fader/pan/width/phase/delay/mute/solo/outgoing sends
and the parent console remain live. The cached files play Warp off, with the
chosen cache encoding (Float32 default, an encoding boundary at either graph
width). Warm unfreeze restores fresh instances from preserved state so stale
voices/history cannot resume. Cache topology requires a stopped callback or
driver release; the owner reads an atomic driver observation, never Transport.

**Window calls:** flush edit capture; on the rendering connection/host owner,
call render::captureFreezeAudio with tracks, endSample from zero through ALL
material plus the explicit tail, session format and documentGeneration. The
private snapshot also pins/hash-verifies active audio assets so replacement of
an external source during progress cannot contaminate later blocks. On the
writable document owner, prepareFreeze(store, capture, currentGeneration,
firstMediaId, importedUtc), then commitFreeze(store, edit, currentGeneration).
Both preparation and publication check generation; filename, history head and
causal source fingerprints are guarded too. unfreeze(store, trackIds) is another
native undo step, with an installed-identity callback for captured actual
format/UID/vendor/version; without it only stored source identity is checked.
Explicit acknowledgement handles externally changed state. Snapshot TrackNode
now exposes frozen, freezeMediaId and freezeMode, all in same(). Mode is wet,
unverified legacy, or empty. Original clips stay visible. Details:
docs/ENGINE-FREEZE.md.

**Scope still queued:** this is ordinary whole-track wet freeze, including a
batch of independent tracks. It refuses partial ranges, incoming audio/sidechain
keys, groups, locked/already frozen tracks and forwarded MIDI without its own
instrument. Freeze Group, All Channels with independent balance/live keys and
live source-prefix caches are subsequent increments; this does not close all of
ADR-0203 d16. A static file cannot respond to a live key or child fader. On
combining N20, stop/finish recording before freeze/disarm; attempted recording
material edits into a frozen source are refused by the native guard.

**Persistence/recovery:** additive schema 1.10, frozen_renders; frozen 1.9 schema
and full migration/read-only coverage. track.setFreeze has captured before/after
states, strict shape checks and a symmetric inverse. Native commits/history and
Session loading enforce causal state/arm/media constraints. Known live controls
use a previously verified memo keyed by total_changes, data_version and both
schema versions; opaque hooks, source-table triggers and unknown edits force
validation. No SQL apply accesses files or loads devices. Publication moves a
SEPARATE COPY, retaining original private renders for same-job retry after late
SQL failure. Failure removes only files this attempt created; reused/colliding
public files remain untouched. Track-delete undo preserves cache provenance.

**Verified:** final GCC full runner **10,085 checks / 110 suites**, every C++
result inspected and passing. CI/OPS/PD/layout validators pass. The sole schema
validator issue is **README says 51 tables, schema has 52**: win owns that edit;
please update it before the strict CI gate. README test totals are also win's.
Older-minor migrations and canonical/schema validation pass. The first full run
found the old migration fixture's unconditional 48 kHz expectation and the
text projection's literal 1.9 header/missing coverage row; fixed and rerun full.
GCC + Clang focused: native freeze **79**, capture **27**, fingerprint **30**,
Session **214**, track-delete **38**, snapshot/engine **128**, replay **64**,
migration **162**, text projection **210**, all pass. Eleven direct shadow
compiles pass. Nine separately compiled plants fail normally (no crash): lost
private source, lost generation, missing source guard, unprotected input gain,
hardlinked input revision, dry for wet, stale instances resumed, parked plug-ins
loaded, omitted snapshot sharing field. Actual dry/wet PCM at both widths,
zero callback allocation/file I/O, no original DSP frames, missing original
files/plug-ins, corruption, native edit refusal, late SQL retry, no-clobber,
undo/redo/reopen, generation and late cancellation covered. No UI/device/DSP or
README edits. Schema numbering must be reconciled with sibling 1.10/1.11 PRs
when win combines them; no third-level stack was created.

**Next:** continue remaining freeze modes and the LANES queue; no director gate.


## 2026-11-03 — Dedicated Bounce to New Track / whole-chain Bounce in Place

**PR requested:** engine/bounce-commands -> **engine/phase-c-editing**, head
**98387836e5d6d1e98519e555442c50470e3591e1**, title **Add dedicated track
bounce commands and retain recoverable renders**. Two deep: requires phase-C's
complete typed clip/range capture and Warp-capable playback; win merges N17
(engine/phase-c-editing 19d1fc6) first, then this. No freeze PR is stacked on it.
GitHub API remains unavailable here; please open and relay native CI / MSVC /WX.

**Body / what the window can call:** bounce_commands.hpp adds
prepareBounceToNewTrack with explicit source/new-track/clip/media identities,
render range/format/session, optional new Main-route id, name, muteOriginal and
documentGeneration. It captures a private WAL-inclusive project, renders the
source's direct post-fader wet output and prepares a plain audio sibling after
the source, preserving parent, palette color and explicit Main destination.
Later siblings shift in the same undo step; the clip is named and placed
sample-exact, Warp off. muteOriginal defaults true and mutes the ENTIRE original
track. prepareBounceInPlace takes a whole regular-track [0,end) range (including
the caller's explicit tail), captures its pre-fader/post-FX output, replaces
all clips, parks enabled inserts bypassed, resets baked input gain and disarms
an armed source while retaining live fader/pan/width/delay/routing. commitTrackBounce
checks the caller's current document generation; the private material edit also
checks the actual project connection and captured head. Track/order/route/media/
clip/source-state operations share ONE undo step. ENGINE-BOUNCE.md supplies the
API/thread/range contracts and scope; no new opcode, schema, UI or device-tier
code. OPS.md describes the existing native op composition.

In-place playback does not run parked device DSP a second time; bypass retains
its reported delay and the existing compensated dry line (ADR-0203 d4). The
original params/state/blob rows remain for Undo. New Track is a direct output
render: it does not clone sends, return effects or sidechain routes and does
not claim to replace every parallel path. Whole-chain In Place refuses partial
extents, a group, locked/frozen/missing source and live incoming main/send audio
that would double a baked mix. The existing lower-level range-bounce producer
remains available for material edits. Group freeze and the separate cached tap
streams of Freeze All Channels remain next; no freeze parity closure is claimed.
A chosen narrowing file encoding carries its own quantization tolerance, and
live graph transition fades are not claimed bit-identical to offline replay.

**Recovery fix:** the phase-C material publisher previously MOVED its only staged
render before SQL COMMIT. A deferred SQL fault then removed the owned destination
and lost the retry source. It now publishes a separate verified private COPY;
the SAME prepared bounce/import survives late rollback and retries without
rerendering. Copy-in also rehomes a reused valid external media identity into
owned audio with a native relink; Undo restores its still-existing old path.
Uncopied external sources are rehashed before commit, and changing one refuses
the whole transaction without deleting it. Private snapshot SQLite paths now
use UTF-8 rather than the Windows locale conversion; all dedicated bounce
fixtures run inside a project directory with Chinese/Hebrew characters.

**Validation:** GCC14 Debug/Werror/PD ON/JUCE OFF full runner **11062 checks /
116 suites, ALL C++ PASS, every binary summary inspected**. The only gate
failure is the N17 base's existing **README says 51 tables / schema has 54**;
all schema/migration checks otherwise pass (version 1.11), and CI/ops/Pd/layout
validators pass. README counts are win's; please update on main when merging
the N17 schema. No README edit by this lane. GCC and Clang19 dedicated bounce
**69** and material **29** pass, including final Unicode-path fixtures.
Actual native playback compares wet output in BOTH widths through a parent
and a different explicit Main destination, with source FX/gain/pan. Checks prove
parking with zero source-DSP calls, Warp off, metadata/order/color/routing,
source mute/disarm and retained live fader/pan, exact full-state Undo/Redo/reopen,
generation/head/completed guards, partial/incoming refusal, late deferred SQL
rollback and SAME-job retry, external source preservation and copy ownership.
Four changed translation units pass extra shadow checking. Seven independently
compiled defect plants fail: moved-only recovery source (1), unchecked external
file (1), ignored copy-in choice (1), wrong Main route (4), unparked chain (8),
baked fader applied twice (2), missing document-generation guard (4). Sources
were not mutated during the proofs; no timeout increase or duplicated mixer tests.

**Next:** track freeze, Freeze All Channels and Freeze Group with fingerprints,
then history caps/daily snapshots. Continue chaining; no director gate.

---


## 2026-11-03 — NI Stems: bounded MP4, real AAC workers, queue export and one-step group import

**PR requested:** engine/ni-stems -> **main**, head
**341c0eeae06494f52e3145b289ecfbe07ab74fe9**, title
**Add NI Stems transport, export jobs and captured group import**.
Independent of the unmerged N13/normalization stack; no third-level branch.
GitHub API remains unavailable here; win/window please open and relay native
Windows/macOS CI, including MSVC /WX. No new director gate.

**Body / what the window can call:** set Job.niStems to one to four direct wet
Source/name/#RRGGBB values plus a per-track AAC bit rate (256000 default), use
stereo 44.1/48-kHz Session settings, and enqueue through the existing Queue.
Main post-fader is the premixed master; output is .stem.mp4 with four stem slots
(unused slots contain real silence). Queue identity/retry/naming/region/cancel
and no-clobber publication retain their contracts; progress spans five streams,
while Result.framesWritten is one audible stream's frame count. To import,
prepareStemImport captures a private original and decodes only stems, then
commitStemImport verifies project connection, document generation and journal
head and creates the group/tracks/Warp-off clips/media as ONE undo step.
Allocate explicit ids, placement ticks, optional parent/index and optional
Live palette indices 0..69; NI RGB stays in the retained manifest, never in
tracks.color. containerMedia() identifies the untouched owned original after
reopen. Enabled NI mastering compressor/limiter is a named refusal unless the
caller explicitly chooses raw stems; manifest().masteringEnabled still reports
the unapplied processing. ENGINE-NI-STEMS.md documents the worker/thread,
resource, codec, recovery and capability contracts. No UI, DSP-tier, schema or
native-opcode changes; OPS.md records the existing native batch composition.

The original ISO-BMFF reader/writer bounds boxes, tables, descriptors and actual
mdat packet addresses, supports co64/extended media, preserves raw NI metadata
at moov/udta/stem and separates encoder priming, audible duration and EOF padding.
Five AAC-LC tracks use one codec/rate/config. Unsupported fragmentation,
encryption, HE-AAC, composition offsets, complex edits and inconsistent tracks
refuse by name. Packet data streams from disk. Native platform AAC is independent
of JUCE; Linux dynamically loads codec60/util58 or codec61/util59 through two
major-matched public-header shims. FFmpeg headers are sparse, pinned by official
tag AND verified commit; no FFmpeg implementation is compiled or CLI invoked.
The ordinary compensated WAV renderer is reused from one SQLite Backup revision
(including WAL and device blobs), with verified absolute media paths, so live
project edits cannot alter later stem renders. Direct wet stems need not sum
back through nonlinear Main processing; generated mastering DSP is disabled.

Native import retains an original private copy until commit and moves only a
separate publication copy. A late SQL failure removes newly published assets,
preserves other files and leaves the SAME prepared job retryable. In-batch/pool
content dedup works even for the identical silent slots. Imported pool assets
remain through Undo for Redo; the original container is retained byte for byte,
including unknown metadata. No new persistent group-to-container association
is claimed. Worker cancellation/service runs between bounded operations;
publication inside the native transaction checks stop without pumping the UI.

**Known capability / follow-up, not a parity closure:** native Mac/Windows code
needs its CI runners. Some Media Foundation versions omit explicit sample-exact
priming/duration: the real native availability probe then returns false and both
export/import refuse instead of guessing a delay. The missing Windows codec
capability remains open. This branch's NI jobs use normalization Off. **When
combining with engine/export-normalization, keep NI normalization Off/refuse an
enabled mode until post-AAC loudness/true-peak verification is implemented**;
verifying a pre-AAC WAV cannot certify the delivered AAC ceiling. The two Job
additions are adjacent; retain both fields and the queue's format dispatch when
merging. NI LUFS integration follows the N13/normalization merge; no unsafe
three-deep stack. This is not a Traktor certification or NI mastering-DSP clone.

**Validation:** GCC14 Debug/Werror/PD ON/JUCE OFF full test_all **9976 checks
across 108 suites, ALL PASS, every binary result inspected; validators clean**,
and validate_schema.py separately PASS (51 tables, version 1.9). Final GCC and
Clang19 MP4/AAC **48**, NI **43**, ordinary queue **196** pass; the last private
codec fixture names were made distinct per rate and MP4 tests rerun on both.
Actual AAC decode proves exact frame count and zero-sample waveform alignment
at 44.1 and 48 kHz. Structural fixtures cover malformed offsets/counts/clocks,
unsupported codecs/edits, future boxes, and real sparse >4-GiB co64 addresses
without reading/allocating gigabytes. Import actually plays through native
groups in BOTH processing widths; tests cover source-project edits during
render, silent slots, labels/palette separation, master exclusion, native
Undo/Redo/reopen, stale generation/head, enabled mastering's explicit choice,
late SQL rollback, same-job retry and preserved external destination collisions.

Ten changed translation units pass the extra shadow check. Seven independently
compiled defect plants fail: lost priming (7), packet outside mdat accepted (1),
enabled SBR accepted (1), live project replacing captured revision (1), premixed
master imported as a stem (2), moved-only recovery source (1), missing head
guard (1). Original sources were not mutated. Independent FFmpeg reads all five
ADI-generated AAC streams at each rate, exact edit durations and zero-sample
alignment; its raw PCM command retains final packet padding (ADI trims it by
container duration). Mixxx's independent five-second sine container remuxes with
all five streams at 5.000 s and preserved 1600-frame priming. No external source
or audio fixture was vendored; references and header pins are attributed.

**Next:** freeze and bounce (LANES 3.4); then history caps/daily snapshots and
remaining brief work. Continue chaining; the window's newer needs still win.

---







## 2026-11-03 — Export queue: Peak and LUFS targets from one captured render

**PR requested:** engine/export-normalization -> **engine/main-loudness**,
head **6b02b28df5cbcd0bdf9b59a5f44aebd0ba2e4146**, title **Normalize captured exports to peak or LUFS targets**.
**Dependency / two deep:** stands on N13's engine/main-loudness (0e4c3cc);
win merges N13 first, then this. No third branch is stacked on these.

**Body / what the window can call:** Job.normalization captures Off (default),
Peak or Loudness, targetLufs (-14 default, -36..0), truePeakCeilingDb (-1 default,
-12..0) and optional explicit channelWeights (0..2). Map the existing Export
settings into each job; Queue add/update/snapshot keeps those values, and the
runner reads no ambient settings. Off retains the original direct writer path.
Peak targets 0 dB sample peak. Loudness uses the selected cropped source's
gated integrated LUFS and true peak, retaining the actual encoded measurements
in Result.normalization: optional source/output metrics, appliedGainDb,
ceilingLimited and encodingPasses. A ceiling-limited target reports its achieved
LUFS. ENGINE-EXPORT-NORMALIZATION.md and ENGINE-EXPORT-QUEUE.md describe the
API, units, channel roles, failure behavior and remaining parity work.

One continuous project/device render captures private **double PCM**; gain,
encoding and verification use that capture, never another plug-in run. It
preserves a 64-bit graph until final quantization, excludes preroll from analysis,
and includes intentional compensated-range silence. The existing N13 meter
now accepts the renderer's full 8--768-kHz range. Loudness gain is corrected
against the encoded result when a boost changes absolute-gate membership.
The worker also verifies the encoded WAV's true peak after quantization/dither;
bounded re-encoding lowers gain if needed, restarting the same dither seed.
FIR-tail padding extends neither the file nor its integrated-loudness window.
Tiny Peak sources use a ratio without an overflowing reciprocal. No schema,
native-opcode, UI or device-tier changes. Legacy export-test member shadowing
was renamed for the strict warning pass.

**Scope:** uniform gain normalization; a ceiling can make the LUFS target
unreachable, which is reported. This does not claim a brickwall limiter, RMS
targeting or linked-batch normalization (the rest of ADR-0205 d9). Unmeasurable/
short Loudness sources fail with a reason to select Peak or Off; silent Peak
retains unity. Channel roles are explicit/full-range by default, never guessed.
This uses N13's bounded 0.1-LU gates and reconstructed-peak design; no certified
or measured-Live claim. Cancellation/host servicing continues during conversion.
A job stays Running until no-clobber publication, including encoding after its
last rendered progress frame. NI Stems is next; no director gate.

**Validation:** GCC14 Debug/Werror/PD ON/JUCE OFF full test_all **9895 checks
across 108 suites, ALL PASS, validators clean**, every binary result inspected.
Counts reflect this N13 stack's base, which predates newer window-only tests.
GCC and Clang19 normalization **23**, render queue **210**, meter **44** pass;
the variable-only shadow cleanup was rebuilt/retested on both after the full run.
Tests decode actual WAVs for LUFS at 44.1/48/96/768 kHz, dithered PCM intersample
ceiling, gate-membership correction, channel-weight propagation, wide PCM24
half-LSB precision, tiny gain, exact one-pass dither, short/invalid/cancel cases.
Actual cropped compensated exports prove excluded preroll, both widths, one
loader/continuous device render, pending job edits, cancellation during encoding
and late destination collision preservation. Five changed source/test shadow
checks pass. Compiled plants catch narrowed capture (2 failures), ignored encoded
ceiling (1), missing gate correction (1), wrong dither stream (1), missing worker
service/cancellation (1), extra plug-in rendering (2), lost 768-kHz support (1).
Original sources were never changed during those proofs.

---


## 2026-11-03 — N20 engine backend: actual audio input recording and captured takes

**PR requested:** engine/audio-input-recording -> main, head **0768096ec8fbfc09cb4d24b2c2389ea856bda8ab**,
title **Record audio inputs and attach completed takes as one undo step**.

**Body for the PR / what the window can call:** Session::startRecording takes
AudioRecordingSpec (current driver rate/max block, work folder, bounded ring,
encoding, armed destination tracks and ordered AudioIo input-channel indices).
The callback captures actual input into preallocated SPSC buffers; one worker
writes all target WAVs. session.recording().stop() and owner poll() return the
finished pass after strict callback acknowledgment and finalization; poll(true)
requires an already stopped device. Cancellation discards only owned work
after quiescence, including a failed partial take. Result carries rate, actual
start sample/frame count, per-track files/channels and resolved source-project
identity. Keep the originating window document generation. Then call
recording::prepareCommit(store, result, ClipIds[]) and recording::commit: all
media import/relink + clip create/attach requests become one native undo step.
Both boundaries must round-trip the current musical clock exactly at the
capture rate. No schema/new opcode, UI or device-tier changes. See
ENGINE-AUDIO-RECORDING.md and OPS.md for lifetime, recovery and integration.

Publication stages a **separate copy**: publishFile moves its source, so moving
the only take and then rolling SQL back would destroy the recording. The late
SQL regression found that bug during development; the final implementation
retains the original for a fresh prepare/retry, cleans only new publications,
and safely reuses identical files retained by Undo. Existing pooled content
is relocated to a verified owned recording asset; if Undo needs a prior path
inside the private source directory, that directory stays. The command manages
cleanup; callers must not blindly discard a retained history source. Simultaneous
identical takes deduplicate within the captured batch, before any media row
exists. Changed source bytes, stale head, another project, locked/missing target
or a differing destination file refuses without losing the private take.
Undo/Redo/reopen restores actual heard PCM without another recording pass.

**Scope / remaining N20:** this is continuous linear raw-input recording,
not the whole device configuration. Win/window still owns the device-scoped
channel-config home/format and opening/enabling hardware inputs. The adapter
must resolve device channels into the supplied callback indices. Monitoring,
hardware/manual latency calibration, punch/loop takes, MIDI recording,
quantization playback and Capture memory remain separate runtime work; N31's
already-pushed preferences did not implement those. A seek/loop wrap, missing
input, callback-size mismatch, bounded overflow or writer failure is explicit,
never substituted silence or an incorrectly placed clip. No measured-Live claim.
Next queue item: export LUFS/peak targeting (LANES 3.3); N16 native Q/L contracts
and N38's role format remain external, with no new director gate.

**Validation:** GCC14 Debug/Werror/PD ON/JUCE OFF full test_all **9980 checks
across 108 suites, ALL PASS, validators clean**. Every binary result inspected.
Clang19 input recorder **59**, recording commands **36**, Session **214** pass.
Tests read actual WAVs at 44.1/48 kHz, many small queue wraps, nonadjacent/shared
channel order, partial blocks, deterministic bounded overflow, stopped-device
reclamation, cancellation, seeks/interior loops and worker I/O failure; actual
Session records in either graph width with zero callback allocations/file I/O.
Recorded audio is heard at its original exact sample range; two-track one-undo
dedup, exact Undo/Redo/reopen, changed bytes/head/project, late SQL cleanup/retry,
old asset reuse and external/private prior-history paths pass. Five changed
source/test shadow checks pass. Compiled plants fail as intended: wrong channel
2, lost final samples 12, lost project identity 1, lost head 2, publication of
the only take 6, deletion of a reused asset 1. No source mutation during proof.

---


## 2026-11-03 — N17 continuation: the Warp button on an existing clean import

**PR requested: update the existing N17 PR**, engine/phase-c-editing -> main,
head **19d1fc6** (after 619ccd3), title **Add captured clip, range and whole-Set
editing actions**. Add to its body: the window can now call
clip_edit::prepareSetWarp(store, clipId, WarpOptions, referenceRate) from
warp_commands.hpp on an owner/worker connection, keep the document generation,
then commitPrepared. Enable detects source timing and captures trim, bar map,
mode and musical length on the same clip identity; disable captures the actual
physical source duration and pins, instead of leaving a stretched extent with
only its flag switched. One native captured arranger.splice undo step; apply,
undo and redo never analyze files. Exact head, cancellation and a commit-time
BLAKE3 guard protect the captured source. A future marker layout refuses rewrite.
The full API/limits are in ENGINE-WARP.md, ENGINE-EDIT-COMMANDS.md and OPS.md.

This producer is for the director's **clean import** (d7): an edited source
offset/loop, alias, End marker, reverse/pitch or trimmed extent gives a named
refusal. All six mode selections capture; arbitrary edited-clip Warp toggling
and individual catalogue marker setters are separate. Material split/copy of
Warp audio already works from the previous increment. No schema, version or
new opcode change here; no UI/device code. The requested clip/range/Time,
fades, silence and audio-import/bounce actions are available together in this
one N17 branch. Video import still refuses honestly; the video clip/window
backend is P2 under ADR-0206 d4, rather than a silent audio attachment.
Next actionable window work is N20's engine input-recording backend; win's
per-device channel configuration is still external. N16's native Q/L contracts
and N38's role format remain with their respective lanes; no director gate.

**Validation:** full GCC14 Debug/Werror/PD ON/JUCE OFF test_all **10988 checks
across 115 C++ suites**, every result inspected and passing. Only the known
win-owned README **51 vs 54 tables** fails; all other schema/frozen migrations,
ops, Pd, CI and binary-layout validators pass. GCC and Clang19 Warp actions
31, Warp playback/edit 125, Warp import 33 pass. Actual heard attacks move onto
the project grid; Warp off returns physical speed/duration; exact undo/redo
restores the heard PCM. Tests cover every mode selection, missing/edited
source refusals, late/pre cancellation, stale journal head, changed source
bytes and future map preservation. Strict shadow passes on producer/test.
Compiled plants catch wrong off duration (1 failure), missing file guard (1),
missing expected head (3), future marker rewrite (1). Latest fetch remains
main 0cadd72, window/log 49b597d; no new need or CI relay arrived.

---


## 2026-11-03 — N17 continuation: warped material edits retain musical fades

**PR requested: update the same N17 PR**, engine/phase-c-editing -> main,
head **619ccd351049f6eeb8f64e49871d6af1dd6cc6ff** (after 318e4a1). Add to its
body: the window's existing clip/range/Time copy, split and edit producers now
accept attached Warp audio. Split retains the original fade curve through
both pieces, including overlapping fades, repeated splits and unrolled source
loops, without inserting an internal edge fade. A subsequent fade handle edit
clears just its chosen side's retained domain; undo restores the full typed
row and future bytes. No source-window or AWRP rewrite on these material edits.
Aliases still give their prior named refusal. N17 stays open: existing-clip
Warp actions continue next, followed by the remaining import work.

**Schema 1.11** adds optional clip_warp_fade_domains with paired nullable
in/out begin/end ticks on the unrolled source musical clock. Schema 1.10 is
frozen exactly; migration/read-only TEMP projection adds an empty table,
never inventing fades for old clips. Warp playback converts these domains
through the tempo integral; unwarped playback retains its separate nanosecond
clocks. Model Clip and snapshot ClipNode expose fadeClockTicks (four optional
coordinates, in begin/end then out begin/end); every coordinate participates
in sharing and the text projection. Typed image/FK capture includes the new
child for delete, copy, split and range/Time edits, preserving future columns.
No UI/device files or new opcodes. SPEC/OPS and engine API docs updated.

**Validation:** GCC14 Debug/Werror/PD ON/JUCE OFF full test_all **10957 checks
across 114 C++ suites**, every binary summary inspected, all passing. The sole
validator failure remains win's README count (**51 vs 54 tables**); all other
schema/frozen migration, ops, Pd, CI and binary-layout checks pass. GCC and
Clang19: Warp playback/edit 125, ordinary clip edits 172, whole-Set Time 413,
migrations 186, text projection 214, all passing. Heard PCM compares exactly
in both widths across overlapping fades/loop/repeated splits and a linear
split across a tempo junction; exact undo/redo/future bytes and partial-range
edges pass. Each of the four coordinates changes structural sharing. Nine
changed-source/test shadow checks pass. Compiled plants catch missing musical
clock (6 failures), missing capture (4), missing sharing comparisons (4), and
missing handle reset (2). Actual callback allocation/file I/O guards stay zero.

---


## 2026-11-03 — N17 continuation: Auto-Warp import and actual project playback

**PR requested: update the existing N17 request**, engine/phase-c-editing ->
main, head **318e4a1324f7500dc75d0b65720a9c94f06122a2** (after ee646f4),
title **Add captured clip, range and whole-Set editing actions**. One N17 PR,
no new stack. Add to its body: the window can now call prepareImportAudio with
warp=true and captured mode, meter, tempo hint/optional source BPM,
Detect Tempo Changes, From First Beat/Sample Start and cancellation. Analysis
captures source trim, musical duration and canonical AWRP bar markers in the
same media+clip undo group; an indeterminate beat refuses with a source-tempo
reason. Replay and redo use literal captured bytes. ClipPlayback now plays
all six modes through off-callback private prepared PCM and the paged reader,
including project-tempo junctions within a bar, source-rate conversion,
source offsets and musical loops/end markers. Old unwarped nanosecond pins do
not override the Warp clock. Bounce forces Warp off (d16). ENGINE-WARP.md and
ENGINE-EDIT-COMMANDS.md document the APIs, lifetime and provisional analysis.

Source-grid onset detection precedes native file-rate conversion, so sinc
pre-ringing cannot move detected attacks. Reimport after undo safely reuses
an identical regular owned asset; a failed SQL transaction never deletes
that existing file. Different content and symlinks still refuse. Cancellation
is checked at commit even when media is deduplicated/no publication is needed.
No schema version/table/opcode changes in this increment. No UI/device code.
N17 remains open: Warp-aware split/fade retention and existing-clip Warp
commands continue next, then the remaining import work. Native-mode sound and
analysis are generated-signal tested, not a claim of measured Live parity.

**Validation:** GCC14 Debug/Werror/PD ON/JUCE OFF full test_all: **10882 checks
across 114 C++ suites**, every binary result inspected, all passing. Sole
validator failure is the already logged win-owned README **51 vs 53 tables**;
other schema/frozen migrations, ops, Pd, CI and binary-layout checks pass.
Clang19 focused: Auto-Warp 43, Warp import 33, Warp playback 76, worker warp
91, material actions 24, ordinary playback 247; all pass. Actual callback
allocation/file-I/O guards remain zero. Eight changed-source/test shadow
checks pass. Compiled plants catch ignored project tempo (4 failures), a stale
unwarped source offset (2), deletion of a reused pre-existing asset (1), and
lost first-beat trim (6). Exact undo/redo/reopen, source attacks heard on the
48-kHz grid from 44.1-kHz files, late SQL failure and stale-head guards pass.
Latest fetch: main 0cadd72, window/log 49b597d; no new needs or CI relay.

---


## 2026-11-02 — N17 continuation: six prepared warp engines, before project integration

**Existing N17 PR request updated, no second PR/stack:** engine/phase-c-editing
-> main, head **ee646f4ac82e6132b20c5ddfc5540654144ba9ad** (after db2ec7a).
The previous captured editing/whole-Set Time/material actions remain on this
same branch. Add to its body: audio/warp.hpp now provides an off-callback worker
that prepares private, aligned Float32 PCM for all six ADR-0188 d2 modes:
native transient-slice Beats, native granular Texture, libsamplerate Re-Pitch,
and Rubber Band R3 Tones/Complex/Complex Pro. Typed plans capture source/output
anchors and controls; shared onset detection and canonical AWRP marker reading/
writing are in audio/transients.hpp and audio/warp_map.hpp. This increment is
the worker backend, not yet the live project handoff: Warp import and existing
ClipPlayback still give their prior named refusals. Auto-Warp tempo/bar
analysis, its edit producer and actual playback integration continue next.
N17 stays open; the window continues using the preceding completed editing
APIs and must not store a private prepared file as a media-pool asset path.
ENGINE-WARP.md describes lifetime, bounds, units and integration status.

Rubber Band is pinned **v4.0.0 / 1d95888bec3ae0a17c0c4af791810d5a63f6bc35**,
and fetch_external.sh --build-only verifies it. ADR-0061's required exact-commit
"version 2 ... or ... any later version" header finding is recorded in
EXTERNAL-CODE.md. Upstream single-file build is unmodified, with vendor warnings
separate from strict ADI code; Apple uses Accelerate, Windows its portable
configuration. R3 gets interior key frames only (0->0 would divide by zero),
exact source/output duration and file-rate-aware pitch/formant correction.
Complex Pro's preserved envelope is verified with harmonic excitation, not
just a pure sine that has no unshifted envelope after transposition. Re-Pitch
keeps continuous sinc-best state and explicit anchor rate changes, with pitch
tied to tape speed. Native modes share stereo timing/jitter; typed controls
select transient loop/envelope and seeded grains. Native parameters/sound
remain unmeasured against Live; the approved R3 Tones/Envelope parity gaps and
provisional native defaults are documented. No measured-Live claim.

The private PCM owner has a private constructor/owned directory, immutable
file/metadata and deleted copy; only a newly reserved staging directory can
be removed by its final off-thread owner, after readers close. Raw WAV reads
never construct/pin the decoding cache accidentally. Cancellation cleans up
private outputs, and callback calls refuse before allocation/I/O. AWRP v1 is
the already-declared stream's 16-byte i64 source-window frame/tick pair; full
64-bit/future-stride reads retain the original opaque bytes. No SQL schema
version/table/opcode change in this increment. Prepared offline PCM is aligned
and adds zero live stretcher latency; future live processing must declare PDC.

**Validation:** final GCC14 Debug/Werror/PD ON/JUCE OFF test_all has **10730
checks / 111 C++ suites, all passing**, every binary result checked. Sole
failed validator is the known win-owned **README 51 vs 53 tables** in this
N17 schema; win updates that count on integration. All other schema/frozen
migrations, ops/Pd/CI/layout validators pass. GCC + Clang19 warp **91** pass:
all engines, exact rates/lengths, actual pitch/spectral-envelope/impulse-anchor
checks across 44.1/48-kHz sources, transient-loop directions, stereo coherence,
seed repeatability, codec golden/future/full64, cancellation/lifetime and real
callback allocation/I/O guards. Strict GCC shadow passes for four sources.
Compiled defects caught: R3 origin division (8), lost formants (6), ignored
tape rate (4), ignored grain seed (1), wrong future stride (1), unrefined
onsets (1). No MSVC/remote CI claim. API remains unavailable; win/window updates
the one existing request. Continuing Auto-Warp/project integration and then
video, with no LANES section 5 gate blocking implementation.

---



## 2026-11-02 — N17 continuation: the whole-Set Time clipboard now acts

**Existing N17 PR request updated, no new PR/stack:** engine/phase-c-editing
-> main, head **db2ec7a943710566ce3381b19760fc7157cceaa2** (after ceeacbc).
**Title:** Add captured editing, whole-Set time commands and material actions (N17)
**Body / window handoff:** clip_commands.hpp now supplies TimeCopy/TimeCut and
copyTime, prepareCutTime, preparePasteTime and prepareDuplicateTime, beside the
previous Delete Time, Insert Silence, range/clip edits, fades and import/bounce
APIs on this same branch. The new clipboard includes every track's clipped
material and global automation, tempo/meter/key carry and interior events,
tuned-key bindings/composite degree overrides, locators/cycles and clipped
arranger sections. Paste inserts the source's integrated physical duration
and musical length, places its context, then restores the destination's maps
and automation at the far join. It works at zero, with captured replacement/
restoration of root map bindings. Duplicate pastes at the source end; Cut
captures its value and deletion under one read snapshot. Commit the one
arranger.splice packet through commitPrepared with its expected head and the
window's document generation. No UI production change. Old material-only
insertTime overloads remain material-only; use the new named APIs for the
global timeline. ENGINE-EDIT-COMMANDS.md and OPS.md document calls and limits.

Unchanged outside envelopes retain their point identities/flags/curve domains
and stream trailer; pasted points/ranges receive fresh captured IDs. Empty
source envelopes copy their default; missing targets, insertion before a first
or after a last point, coincident joins and nonlinear subdomains retain the
proper values rather than inventing ramps. New target data retains captured
source future columns/trailer, while existing target metadata stays intact.
Media content, referenced tuning system/degrees, full lane contract, project
filename, captured track kinds and reference render rate are checked before
paste. Current scope is the same project/track ids and reference rate, with no
implicit cross-project mapping; a changed lane contract/rate needs a fresh copy.
Existing arranger playlist references follow insertion but copied sections do
not duplicate the playback playlist. There is no new schema/opcode. Also fixes
material-only paste reusing a synthetic ID allocated while trimming the
clipboard: both range paste paths now reserve all copied root identities.

**Validation:** final GCC14 Debug/Werror/PD ON/JUCE OFF full runner **10639
checks / 110 C++ suites, every suite passes** (all 110 result lines verified).
The sole failed validator remains **README 51 vs actual schema 53 tables**,
win-owned and already logged for this N17 PR; win fixes that count on integration.
All other schema/frozen migration, ops/Pd/CI/layout validators pass. GCC +
Clang19 Time edits **413**, clip edits **172**, actual playback **247**, row edits
**36**; Clang replay **63**. Tests include whole-state undo/redo/reopen, empty
envelopes, guarded lane/tuning identity, stale head and late SQL rollback. Heard
44.1/48-kHz resampled/faded/looped sources in both precisions copy exactly:
400 source frames are inserted where the destination tempo alone would yield
100, including the following clip's physical shift and heard undo/redo.
Strict GCC shadow checks pass for four changed sources. Compiled defects caught:
target-tempo gap (8), lost destination context (1), reused clipboard id (32),
changed lane contract (1), lost new-stream trailer (1), material-only timeline
(21). No MSVC/remote CI/measured-Live claim; API blocked, win/window updates or
opens the one requested N17 PR. Main0cadd72/window49b597d unchanged at last fetch.

**N17 continues:** global Time commands are no longer an open engine gap.
Auto-Warp import/detection and warped playback are next, then video attachment;
the existing named refusals are still accurate until those backends land.
ADR-0188 d2's approved engine split governs: native Beats/Texture, libsamplerate
Re-Pitch, Rubber Band R3 for Tones/Complex/Complex Pro. No director gate blocks
implementation; continue the queue. General alias playback/consolidation remains
separate existing engine work rather than a claim of this clipboard increment.

---



## 2026-11-02 — N17 continuation: Delete Time preserves the whole Set

**Existing PR request updated, not a second N17 PR:** engine/phase-c-editing
-> main, head **ceeacbc2b143c87667faa7adbd6adf3556d426a9** (after 53556ae).
**Title:** Add captured range edits, fades, time deletion and import/bounce actions (N17)
**Body / window handoff:** The existing clip_commands.hpp entry points now also
include prepareDeleteTime(Store&, beginTicks, endTicks, referenceRate). It
removes the selected musical time across every track as one guarded
arranger.splice undo step, trimming crossing clips and moving remaining tick/ns
clips. Linear objects use the old tempo map's integrated physical duration.
Unwarped audio source/loop/fade clocks stay pinned before movement; playback
matches the original with only the selected samples removed, including
resampling, loops and both native precisions. Global automation preserves its
original curve domains on both sides of the join, with coincident boundary
guards, original surviving 64-bit identities/flags/trailer, and fresh captured
boundary IDs. End-context tempo, meter and key events survive at the join;
removed key tunings and every composite degree row are captured for undo.
Locators/cycles in either clock, arranger sections and their playlist references,
declared project end and saved loop follow the cut. The window uses
commitPrepared with the captured expected head and its own document generation.
No UI production change; ENGINE-EDIT-COMMANDS.md and OPS.md describe the API.
Existing material, fades, import/bounce and Insert Silence APIs remain in this
same PR, as detailed in the original handoff below.

The row-delta wire format keeps integer ids for every old single-key packet;
composite key_map_degrees uses {key_map_id,degree}. FK guards group all columns
of a composite reference. Full typed NULL/empty/future BLOB data is preserved,
and parent restoration works with arbitrary packet order. Existing Insert
Silence also keeps future fields when splitting an already retained curve range.
No new schema/opcode beyond N17's existing 1.10 additions. Fully deleting the
saved loop provisionally disables it and retains its old positive bracket length
at the join; this specific Live behavior remains unmeasured, and is documented.

**Validation:** final GCC14 Debug/Werror/PD ON/JUCE OFF full runner has **10423
checks across 110 C++ suites, all passing**; every binary result line was checked.
The ONLY failed validator is the already reported win-owned README table count,
**51 vs 53** for this N17 schema. Win updates that count during integration;
README was not edited. All other schema/frozen migrations, ops/Pd/CI/layout
validators pass. GCC + Clang19 time-edit **245**, captured rows **36**, clip edits
**172**, actual playback **199**; Clang replay **63**. Strict GCC shadow checks
pass on seven changed sources. Compiled defects caught: wrong join value (4),
unmoved right pieces (8), lost arranger parent ordering (1), partial composite
FK matching (2). Tests include exact whole-state undo/redo/reopen, late SQL
rollback, stale head, future tuning/arranger bytes, all six curves with positive/
negative tension in both clocks and repeated insert/delete, boundary clusters,
and exact heard 44.1/48-kHz resampled sources, both widths. No MSVC/remote CI/
Live-measured claim. Main remains 0cadd72; window/log49b597d adds no new need/CI.

**N17 remains open:** full global Copy/Cut/Paste/Duplicate Time is next (the
existing insertTime flag still pastes material only), then Auto-Warp detection/
warp playback and video attachment/general alias/consolidation work. A normalized
automation clipboard draft is already in progress locally. This is not a
director gate; continuing the queue as instructed. GitHub API remains blocked;
win/window updates or opens the existing requested PR, without a director relay.

---



## 2026-11-02 — N42 one persistent Pre/Post choice on each return

**PR requested:** engine/return-send-tap -> main, head **bf83f66**.
**Title:** Persist one Pre/Post send choice per return
**Body:** N42: return.setSendTap {id, pre} persists a return-owned choice even
with no sends and changes all incoming send taps in one undo step. Ordinary new
routing.connect sends inherit an explicit return choice; a conflicting new
per-edge setter refuses (same-value old callers still work). TrackNode and
rows::Track expose optional returnSendTap {preFader, explicitChoice, legacyMixed};
all fields participate in sharing. Snapshot sends and actual engine routing use
that explicit policy. Legacy absent choices keep their raw per-edge audio,
including mixed values, until the first explicit return choice. The new-send
producer prepareSendToReturn/commitSendToReturn captures uniform legacy Pre as
literal requests and guards file/head; the window keeps its document/generation
check and caller-reserved ids. The inverse guards expected choice/flags/edge set,
restores exact absence and mixed flags, and updates only tap columns, preserving
future metadata. routing.disconnect now captures/restores its full typed row,
including channel-map/future BLOBs, gain/pan/order/enable/origin, bypassing current
policy on inverse reconnect. Historical six-field inverses and scalar apply
semantics remain supported. The window replaces its per-edge batch with the
return op and uses the producer/DTO for new sends; no UI production change.
ENGINE-RETURN-SEND-TAPS.md gives calls and compatibility details.

**Format:** 1.10 adds only return_send_taps (tracks FK, cascade); frozen 1.9 is
byte-for-byte the base schema. Migration creates an empty table and changes no
old audio. Read-only older projects use an empty TEMP stand-in. Text projection
inlines stored sendTap pre/post (including explicit Post), retains raw edge taps,
and covers the new table. N37's generic track inverse discovers this FK child
when win combines the branches; no duplicate track-delete patch/own merge.
Independent 1.10 schema branches N25/N17/N31 still need win's ordered version/
merge handling; this is a standalone branch from main, not a deeper stack.

**Validation:** final full run has all **107 C++ suites / 9968 checks passing**
on GCC 14 Debug/Werror, PD ON/JUCE OFF. test_all.sh's ONLY final failure is
validate_schema.py's README count: **README 51 tables vs actual 52**. **Win must
update its owned count on this PR (or while integrating schema branches) to make
that check/CI green.** README was not edited. All schema shape/frozen upgrade
checks 1.0 through 1.9 -> 1.10, ops/Pd/CI/layout validators pass. GCC + Clang 19
return taps **68**, migration **162**, projection **209**, catalog **106**, replay
**63**, engine **119**. The existing N11 sharing test now accounts for a return's
derived tap changing when its last incoming send moves away; unrelated tracks
remain shared. Actual heard Post .25 vs Pre .375 with deliberately mismatched raw
rows passes in both precisions, alongside zero-send persistence, inheritance,
mixed legacy/CBOR/reopen, typed reconnect, future bytes, conflict rollback and
UI-independent literal replay/undo/redo. Strict GCC shadow checks pass for eight
changed sources; compiled defects caught: stale control sharing (1), lost choice
(28), ignored engine policy (5), lost route image (1), stale restore (1). No
MSVC/remote CI claim. API still CONNECT 403; win/window opens above, with the
README-only failure visible. Continuing own remaining needs, no director gate.

---


## 2026-11-02 — N41 New includes A Reverb and B Delay

**PR requested:** engine/default-returns -> engine/library-devices, head **8e4c6a5**.
**Title:** Seed default Reverb and Delay returns with wet-only effects
**Body:** N41: Store::createNewProject creates two MIDI/two audio tracks, A Reverb,
B Delay and Main, in that root order. Main keeps id 1, regular tracks 2 to 5,
returns 6/7; each has a mixer strip. All four regular tracks have enabled Post
sends to both returns at the -150 dB minus-infinity floor; no return feedback
edges are added. The returns contain compiled native Reverb and Delay identities,
chains/devices and saved 100-percent-wet Mix controls (canonical ids 31/25),
with the other native controls at their defaults. All rows are initialized in
one root transaction, with no ops or UI undo protection. The first edit remains
undoable and New survives snapshot/fork/revert/reopen. Window callers keep the
same factory and model/snapshot APIs; new tracks reserve ids above existing
tracks. No schema/opcode/UI production change. ENGINE-DEFAULT-RETURNS.md describes
the root and loader. Missing-native builds keep normal placeholder identities.
**Stack:** stands on N15 engine/library-devices (71d7921), depth two, for actual
native factory loading; win merges N15 first. N31's independent New overload can
retain its recording preferences on the four regular tracks (2 to 5) when win
combines the factory edits; no own merge/rebase was performed. Native preset/
render measurements remain the devices/parity lane, not a claim of measured
Live sound from this initializer.

**Validation:** GCC 14 Debug/Werror/PD ON/JUCE OFF test_all.sh **9992 / 107**,
all validators clean. GCC + Clang 19 bootstrap **30**, built-ins **99**, catalog
**106**, Session **214**. The actual Session loads both native effects on the
correct chains with Mix=100, renders initial silence, and preserves root data
through undo/redo/fork/snapshot/reopen. Strict GCC shadow and PD-off syntax pass
for initializer/test sources. Compiled defects caught: wrong return kind (2
failures), audible default send (1), dry return (2). No full PD-off runtime,
MSVC or remote CI claim. API remains CONNECT 403; win/window opens above.
Continuing N42 return-owned persistent Pre/Post; no LANES section 5 gate.

---


## 2026-11-02 — N40 source windows and waveform data for the window

**PR requested:** engine/snapshot-audio-peaks -> main, head **4a64819**.
**Title:** Expose audio sources and worker waveform peaks
**Body:** N40: ClipNode.audio holds optional AudioClipSource with media id, full
64-bit source-rate start/length, Warp mode/nullable marker bytes, pitch/formant,
reverse/channel mode, and optional pool hash/rate/channels/frames/missing flag.
Every field participates in sharing; missing attachments/pool rows remain
explicit, old frames immutable. Rebuilds read coherent metadata only, without
filesystem/decode work or pool peaks BLOBs. The window can use media/peaks.hpp:
readMediaPeaks reads an optional cache, buildMediaPeaks uses a worker-local
read-only Store and the playback DecodeCache/WavReader path to stream typed
per-channel min/max. Content identity/dimensions, bounds, short tail bins,
nonfinite-to-zero drawing and finite overs are handled. The optional versioned
APKS v1 LE cache is bounded to one million bin/channel pairs; oversized BLOBs
are rejected via SQLite length/CASE before materialization. Unknown cache bytes
are preserved; workers never write the project/cache/op log/history. A valid
cache can draw with missing original audio, without claiming audio availability.
All allocating/SQL/file APIs refuse audio-thread misuse first. The worker result
must match the current document/generation/media hash before painting; the window
owns drawing and integration. ENGINE-SNAPSHOT-AUDIO.md and format/PEAKS.md give
the call, source-domain mapping and cache format. No schema/opcode/UI production
change. Raw source bins do not bake gain/fades/reverse/Warp or claim exact warped
rendering/Live visual parity. Cancellation is checked per hash/read and around
DecodeCache; a compressed decode call itself is not interruptible.

**Validation:** GCC 14 Debug/Werror/PD ON/JUCE OFF test_all.sh **9944 / 108**,
all validators clean. GCC + Clang 19 snapshot audio **26**, peaks **33**, engine
**119**, decode **56**, media ops **71**. Actual stereo Float32 source and partial
bin extrema, FLAC-vs-PCM exact bins, fixed LE cache bytes, read-only cached missing
source, legacy empty mono pool row, malformed/oversized cache, identity changes,
cancel-before-SQL and callback SQL/file-I/O refusal pass. Strict GCC shadow checks
pass for four sources; compiled defects caught: stale audio sharing (22 failures),
swapped channels (2), ignored cache identity (1), late pre-cancel (1). No MSVC or
remote CI claim. API still CONNECT 403; win/window opens above. Continuing N41.

---


## 2026-11-02 — N39 all routing identities in the snapshot

**PR requested:** engine/snapshot-routing-ids -> main, head **dd62a03**.
**Title:** Expose highest routing identity across every connection kind
**Body:** N39: Snapshot.maxRoutingId observes the highest positive routing row id
across main/send/sidechain/VCA/cue, disabled rows and hardware endpoints. It is
zero with no positive ids and preserves full signed 64-bit width including
INT64_MAX. Endpoint numbers never enter the maximum. The window can use one past
this value for its new send after guarding overflow, reserving ids for its
pending batch and checking document/head freshness at commit. It is an
observation, not a concurrent allocator; routing.connect still receives its id
from the literal payload. A root metadata change shares unchanged send/track
nodes and keeps old frames immutable. A borrowed/deferred read transaction
(the same seam as N27/N38) keeps the maximum coherent with the frame's send list
under a concurrent WAL writer. No SQL write/schema/opcode/UI production change.
ENGINE-SNAPSHOT-ROUTING-IDS.md gives the window's call and guard requirements.

**Validation:** GCC 14 Debug/Werror/PD ON/JUCE OFF test_all.sh **9901 / 107**,
all validators clean. GCC + Clang 19 routing identities **16**, engine **119**,
catalog **106**. Strict GCC shadow checks pass. Compiled defects caught:
send-filtered maximum (12 failures), 32-bit truncation (2), incoherent WAL read
(1). No MSVC/remote CI claim; API still CONNECT 403, win/window opens above.
Continuing N40 source windows and off-thread waveform peaks; no director gate.

---


## 2026-11-02 — N38 engine role reader/op supplied; win's format remains

**PR requested:** engine/track-roles -> main, head **58ca080**.
**Title:** Add native track role snapshot and undoable op seam
**Body:** N38's engine part: TrackNode.role is optional native role text, included
in structural sharing. A missing tracks.role column reads no assignment; unknown
future text remains visible. track.setRole {id, role?} accepts kick/bass/vocal/
reference, with omission/null clearing. Positive ids and mixable track kinds are
validated; a second reference refuses, so a caller explicitly clears the old id
then assigns the new one in one batch. The symmetric inverse preserves SQL NULL;
an unknown stored role refuses overwrite rather than creating a lossy inverse.
GraphRebuild makes N23 re-read the native reference role once both branches land.
Snapshot construction borrows a caller transaction or a deferred read snapshot
(the same seam as N27). No UI production change or format DDL/version change.
OPS.md documents the new op and its unavailable-format refusal.

**N38 is still open for win:** origin/main has no tracks.role. This PR never
invents/migrates that win-owned field and the op refuses atomically until its
native format exists. Win supplies the nullable column, allowed role values,
format version/migration, and can enforce reference uniqueness in the format.
The ALTER in this suite is a synthetic reader/op fixture, not a migration or a
claim that the native role is already persisted on today's schema. N23 already
consumes the native role and supplies exclusive cue/reference audio; this branch
does not duplicate it. No director question is needed; continuing N39.

**Validation:** GCC 14 Debug/Werror/PD ON/JUCE OFF full **9916 / 107**, all
validators clean. GCC + Clang 19 role **31**, engine **119**, catalog **106**,
replay **63**. Native role fixture, unavailable legacy schema, reference move,
nullable undo/redo, reopen, and literal replay with different UI selections pass.
Strict GCC shadow checks pass for three sources; compiled defects catch missing
role sharing (5 failures) and lost null inverse (2). No remote CI/MSVC claim.
GitHub API remains CONNECT 403 after environment restart; win/window opens above.

---


## 2026-11-02 — N37 Delete Track and Ungroup restore their contents

**PR requested:** engine/track-delete-undo -> main, head **945f8d0**.
**Title:** Restore complete track subtrees and routing on undo
**Body:** N37: existing track.delete now captures the complete causal row image:
track descendants, all columns/SQLite types, clips/streams/lanes/mixer/summing,
nested racks, parameters, panels, expression routes and state hashes. It removes
routing of every internal kind and owned automation/controller bindings, while
keeping equal numeric hardware ids distinct. Surviving rows changed by FK SET
NULL (aliases, lane membership, VCA links) have exact before/after images.
track.create's paired inverse accepts optional versioned contents; ordinary
create and old inverses still work. Restoration checks identities/current rows,
uses a savepoint with temporary FK deferral, then verifies the complete causal
image and FK integrity. Collisions, unrelated packet rows and changed surviving
rows refuse atomically. Future keyed FK children, composite keys and future
BLOB/NULL columns survive. Media_files, shared state_blobs and plugin_refs stay
pooled; weak remarks, session views and unknown scoped extensions remain.
No window call changes: its existing Delete/Ungroup batches get a full undo.
No schema/opcode change; OPS.md section 6.3 documents the extended inverse.
Affected user triggers, generated/unkeyed tables, key-changing SET NULL and SET
DEFAULT effects currently refuse explicitly rather than silently losing data.
The typed row_capture helpers match N15/N17 for merge compatibility; new FK child
tables (such as N31 recording choices) are discovered automatically.

**Validation:** GCC 14 Debug/Werror/PD ON/JUCE OFF test_all.sh **9923 / 107**,
all validators clean. GCC + Clang 19: track deletion **38**, catalog **106**,
history **95**, replay **63**. Full metadata digest matches across repeated
undo/redo, reopen and Ungroup; late batch failure and restoration conflicts leave
no partial changes. Strict GCC shadow checks pass for four changed sources.
Isolated compiled defects caught: lost future cascades (2 failures), lost external
restore (3), unrelated restore injection (1). No MSVC/remote CI claim. API still
CONNECT 403; win/window opens this exact PR. Continuing N38/N39, no director gate.

---


## 2026-11-02 — N27 remarks in the immutable snapshot

**PR requested:** engine/snapshot-remarks -> main, head **06c372f**.
**Title:** Expose remarks and coherent anchor ownership in snapshots
**Body:** N27: Snapshot.remarks holds shared const RemarkNode values with all nine
stored fields (id, targetKind, targetId, nullable paramId, author, actorDetail,
text, createdUtc, resolved), plus optional trackId and anchorMissing. The window
can filter trackId for row pips or use the raw target address for Info View.
Clip ownership and nested device/rack ownership resolve once per rebuild;
parameters need no plugin_params mirror row. Deleted, missing or cyclic anchors
retain their metadata as unresolved remarks. Every raw and derived field takes
part in sharing, including owner changes with unchanged remark text. The builder
borrows an existing caller transaction or starts a deferred read transaction so
a concurrent WAL writer cannot mix ownership and annotation states. With no
remarks it skips device ownership reads. No schema/opcode or UI production change.
ENGINE-SNAPSHOT-REMARKS.md describes the interface; existing remark ops edit it.

**Validation:** GCC 14 Debug/Werror/PD ON/JUCE OFF test_all.sh **9914 checks / 107
suites**, all validators clean. GCC and Clang 19: snapshot remarks **29**, engine
**119**, existing remark ops **52**. Strict GCC shadow checks pass. Isolated
compiled defects are caught: missing coherent read (1 failure), text-only sharing
(14), lost author field (2). No MSVC or remote CI claim. GitHub API remains
CONNECT 403; win/window opens from the exact branch above. Then continuing N37.

---


## 2026-11-02 — N24 engine backend: alignment switch, Track Delay suspended

**PR requested:** engine/delay-compensation-switch -> main, head **5af5a8e**.
**Title:** Add the delay compensation switch backend and suspend Track Delay
**Body:** N24's engine side: SessionSpec.delayCompensation (default true) and
Session::setDelayCompensation(bool) accept the app choice. A changed choice
publishes a new graph without reloading devices; the same value is a no-op.
Graph/GraphHost expose the same build/publication policy. Off means zero main
and sidechain alignment rings/headroom, including retap and escalation paths.
Intrinsic latency, physical MIDI forwarding delay and delayed dry bypass remain;
bypass delay can still grow independently. Scope input stamps show heard global
time off; diagnostic latency remains maximum physical path latency, not a claim
that unaligned paths coincide. MixerStrips suspends Track Delay with the same
flag, including sends/feedback, without changing SQL/model values. Re-enabling
uses the latest saved value, even after edits/refreshes while off. No schema/opcode,
registry or UI production changes. ENGINE-DELAY-COMPENSATION.md gives the call.
**Win/window part remains:** add/feed the app's Options/registry choice (proposed
audio.delayCompensation=true) into SessionSpec/setter. This PR supplies the
backend; it does not falsely claim the app row/window control has landed.

**Validation:** GCC14 Debug/Werror full test_all PASS **9996 checks / 107 suites**,
all validators clean. New111-check suite passes GCC and Clang19 at both widths:
heard main/sidechain impulses, exact MIDI frames, scope clocks, physical bypass,
latency retap/growth, signed Track Delay off/on/latest edit, preserved SQL,
no-op choice, zero callback allocation/IO. Clang graph200/retap334/bypass107/
Session214/mixer337(including sends)/precision108 pass. Strict shadow syntax
passes changed engine sources/tests. Final compiled plants: alignment left on18
failures, retap re-enabled6, bypass delay lost2, Track Delay leaked6, escalation
still considering disabled rings6. Restored tests pass. The final audit fixed
that escalation path explicitly; its new test grows only the physical dry ring,
without spurious ringless alignment failures. No remote CI/MSVC/hardware/Live
parity timing claim; README count remains win's. Next **N27** (snapshot remarks).

---


## 2026-11-02 — N10: Learn a control directly, with no automation lane

**PR requested:** engine/controller-targets -> main, head **2e61936**.
**Title:** Learn MIDI controller targets without creating automation lanes
**Body:** N10: window calls learnControllerTarget(bindingId, kind, targetId,
paramRef, CC, observedValues, policy, error) for a mixer control without an
automation lane. It returns a captured controller.bind request, creates no
lane and reads no project or app policy. Existing learnController resolves a
lane's owner/param_ref and calls that same producer. Both apply Remote/Focus
reservation, source/channel/CC checks, absolute/relative detection and takeover.
Direct addresses require a known target kind, positive IDs and nonempty param;
commit also checks the actual target still exists. No schema/opcode/payload
change. The asynchronous UI retains its document-generation/expected-head
guards. OPS.md describes the call and captured-policy/replay behavior.

**Validation:** GCC14 Debug/Werror full test_all PASS **9897/106**, all validators
clean. GCC+Clang19 controller56/catalogue106/replay63 pass; controller source/test
shadow/Werror syntax passes. New cases cover no lane or writes, exact Pan target,
equivalence to lane Learn, raw-binding equality on replay despite later policy,
undo to absence, relative takeover, Focus/Remote/input errors and atomic refusal
of a missing target. Compiled plants: lost target1 failure, ignored reservation2,
wrong relative takeover2; restored tests pass. No remote CI/MSVC/hardware claim.
Next **N24**, Delay Compensation and Track Delay disabled with it, then the rest.

---


## 2026-11-02 — N31: per-track recording choices, snapshot and root inheritance

**PR requested:** engine/track-recording-options -> main, head **11786be**.
**Title:** Persist per-track Rec-Q, Play-Q and monitoring-latency choices
**Body:** N31: TrackNode.recording carries recordEnabled/recordGrid,
playEnabled/playGrid/playForgivenessMs and keepMonitoringLatency, all compared
for sharing. track.setRecordQuantize {id,on,grid?}, track.setPlayQuantize
{id,on,grid?,forgivenessMs?}, and track.setKeepMonitoringLatency {id,on} are
undoable; track.restoreRecording restores the complete typed prior optional
row, including future BLOBs and exact absence. Grid choices are 1/4,1/8,1/16,1/32,
disabling retains the grid, explicit forgiveness is 0..1000ms (default zero).
Missing policy defaults to Rec-Q on 1/16, Play-Q off, Keep Monitoring Latency on.
Window appends trackRecordingDefaults(newId, record.quantize choice, keep)
to a new-track batch. For New, createNewProject(path,error,choice,keep) captures
the app defaults for all four initial tracks at the history root, no extra undo;
the old overload keeps fixed defaults. Invalid choices create no file. Callers
capture settings values, never a handler. Row-model and text projections expose
all policy values and omit fixed defaults. Additive schema1.10 adds only
track_recording_options and freezes exact1.9; no existing native DDL changes.
Win must reconcile this additive minor with the other unmerged format PRs.
ENGINE-TRACK-RECORDING.md gives the window call path. N31 supplies policy/ops;
recording writer, post-take quantize and live Play-Q pending-list scheduling
remain separate engine work, not claimed implemented by these switches.
N37's full track inverse must include the new cascade child as well.

**Validation:** all **107 C++ suites / 9936 checks pass** under GCC14 Debug/Werror.
The full runner fails **only** validate_schema's win-owned README table count,
51 versus52; CI/OPS/Pd/layout validators and frozen1.0..1.9 migrations pass.
README unchanged. GCC+Clang19 focused recording33, text projection209,
Session214/replay66/bootstrap24 pass; migrate162/catalogue106/engine119 pass.
Strict shadow syntax covers changed sources/tests (migration fixture uses its
normal target history-path define). Plants against the final33-check suite:
restore-defaults-instead-of-absence2 failures, ignore-policy-sharing4, interpret
future monitoring-op fields1. Restored tests pass. Migration fixture now stores
48k explicitly, so its preservation assertion still tests the same property
when the predecessor is1.9 with a44.1k default. Full coverage manifest includes
the new table, no ignored projection gap. No remote CI/MSVC/hardware/parity claim.

Environment refresh retained files and active compilers. Reusable start_skill
was updated with the current workflow/ownership/queue, preserving install and
network settings. API still fails CONNECT403 after restart despite saved allow
rule and a GH_TOKEN binding name; no credential values inspected. Continue the
push/log PR-request route. Next **N10**, then N24/N27 and the remaining needs.

---


## 2026-11-02 — MSVC mixer timeout: separate group/send console sweeps

**PR requested:** engine/mixer-test-runtime -> main, head **834d26f**.
**Title:** Separate group console sweeps from mixer tests for MSVC Debug
**Body:** Window reports #350's MS STL Debug mixer ctest exceeding its existing
600-second limit. That suite still combines ordinary mixer checks and the group
console/send-level sweeps, after master-console tests were split earlier. Add
adi_group_summing_tests and dispatch the existing two heavy functions there;
ordinary mixer and master-console binaries remain separate. No render length,
assertion, oracle, production code or timeout changes. All functions remain
referenced in the common dispatcher for strict unused-function diagnostics.
Current main's mixer337 + master98 = **435 checks**; the split is mixer233 +
group/send104 + master98 = **435**. (The old hand-off's 428 predates current main.)
The window has no new interface to consume; this is the engine's CI repair.

**Validation:** GCC14 Debug/Werror full test_all PASS **9885 checks / 107 suites**,
all validators/migrations/layout clean. Clang19/Werror all3 suites pass the same
435 checks. Local GCC elapsed mixer2.47s/group1.73s/master1.14s; Clang
2.71s/1.76s/0.97s, concurrent local work present. GCC shadow/Werror syntax passes.
No MSVC/remote CI timing claim; please let the existing seven-ABI CI validate
its checked-iterator workload. Timeout stays unchanged. No extra tests or plants
for this test-dispatch-only change; the original complete renders are the oracle.
README counts remain win's. Next **N31**, then continue the needs queue.

---


## 2026-11-02 — N28: undoable Color Cab sample assignment and missing-file snapshot

**PR requested:** engine/sample-slots -> engine/library-devices, head **34259fe**.
**Title:** Assign built-in sample slots with undo and missing-file snapshots
**Body:** Depends on N15 (71d7921), depth two. Window calls prepareSampleAssignment
(store, device, declared slot, file, engineRate, importedUtc, optionalDecodeCache)
then commitSampleAssignment; prepareSampleClear clears it. One undo group captures
media import/relink, an absent slot's explicit empty baseline and device.loadState
hashes. The existing ParamOps.appliedState path handles live forward/inverse
notifications; Session restores on reopen. Color Cab slot1 (Impulse) uses the
existing public preparation SDK and lock-free PdSampleSlots, no DSP/UI production
changes. State contains the media hash, derived profile and mono source backup
for another engine rate with the original absent. Snapshot.sampleSlots carries
device/slot/name/UID, state/media hashes, optional media ID, empty/missing/derived
flags and problems; immutable metadata is cached by state hash, small pool and
locator presence refresh per rebuild, and every field participates in sharing.
No project schema or opcode addition. Oversized/nonfinite/wrong-slot state
refuses before publication. Immutable content follows the existing writeBlobs
convention: later journal failure may retain an unreferenced blob only; expected
head is checked inside the journal too. Hybrid's missing native backend remains
unavailable. ENGINE-SAMPLE-SLOTS.md gives the exact window integration and limits.

**Validation:** GCC14 Debug/Werror full test_all PASS **10014 checks / 108 suites**,
all validators/migrations/layout clean. GCC + Clang19/Werror focused sample28,
built-ins99, ParamOps177, engine119 and Session214 pass. After final review's
undo label and pre-copy blob-size guard, affected sample28/engine119 pass on
both compilers. GCC shadow syntax including disabled-backend paths passes.
Sample tests exercise real audio, live undo/redo, missing original, 44.1k restore,
malformed-state atomic refusal, cached UID/pool changes, and zero callback
allocation/file IO. Compiled plants: missing undo baseline4 failures, skipped
live publication3, ignored missing-state sharing3; restored tests pass. No
remote CI/MSVC/JUCE-runtime/hardware/Live parity claim; README remains win's.

**Refreshed queue:** explicitly fetched window/log (032498f; the remote's normal
fetch spec covers main and engine branches only). New **N42** is queued: return-owned
Pre/Post with persistence even without sends. New **N43** matches the already-pushed
N23 engine/cue-reference (1466c85): Snapshot.cue.volumeDb/cueMode,
cue.setVolume {db} (-150..+6), cue.setMode {on}, readCueSettings and the real cue
path. Stored in session_state cue.controls, performance ops rather than undo
edits; window refreshes ProjectView/Session as documented. N23's PR request
remains the one for these controls; no duplicate backend/PR is needed.

Window also reports #350's MSVC mixer timeout (same job passed #351). Repair that
CI bottleneck next by isolating existing group-console/send-level sweeps in a
third binary, keeping assertions and renders intact and the timeout unchanged;
then **N31** and the remaining needs. No LANES5 director gate blocks this queue.

---


## 2026-11-02 — N15: catalogue, native built-in project loader and one-step insertion

**PR requested:** engine/library-devices -> main, head **71d7921**.
**Title:** Expose the built-in device catalogue and project loader
**Body:** N15: the browser can read builtinCatalog() (UID/category/folder/name/availability),
then prepareBuiltinInsert(store, uid, track, beforeDevice) and commitBuiltinInsert
for one undo step creating/reusing the internal reference and chain. Preparation
is read-only, captures all IDs/typed reference defaults and expected head, and
normalizes legacy tied/large ordinals with exact inverses. The native project
loader and JuceDeviceLoader now host the 17 existing canonical effects, with
real parameter descriptors, atomic intent, transport/MIDI, latency and CBOR
state; unknown fields survive save. Uses the existing device factory, no DSP or
UI production changes. Existing app-scoped library::Index already supplies file
browsing/pending hashes/preset files; ENGINE-BUILTINS.md documents that seam and
the installed plug-in registry separately. No schema addition. New invertible
pluginRef.register/unregister ops capture complete typed rows, including future
columns/BLOBs, and have replay corpus coverage. Hybrid Reverb remains listed
unavailable because its native factory entry does not exist; builds without PD
retain the catalogue and placeholders. Sample assignment is N28, next.

**Validation:** GCC14 Debug/Werror full test_all PASS, **9986 checks / 107 suites**,
all validators/migrations/layout clean. Clang19/Werror built-ins99, catalogue106,
replay65, library48 and Session214 pass. GCC shadow/Werror syntax covers new
sources/tests and PD-disabled branches; GCC + Clang JUCE loader syntax passes
against JUCE9.0.2 (linked GUI development libraries unavailable). Compiled plants
removing parameter delivery, expected-head validation and ordering each cause
one failure; restored tests pass. Callback allocation checks cover all17.
No remote CI/MSVC, linked JUCE, hardware or Live parity claim. README counts are
win's. Window can consume the interfaces above; Hybrid/instrument backends and
parity remain the devices lane's needs, not falsely advertised as available.

---


## 2026-11-02 — N17/N18 clipboard identity follow-ups; N15 precedes N28 as its loader dependency

**Existing PR requests updated, no new stacks:**
- engine/device-copy: **3638fc4** after 31fe98e, to main.
- engine/phase-c-editing: **53556ae** after f6e9613, to main.

Old clipboards are values, so an undo/new history branch can outlive the row
an ID originally named. N18 now verifies the copied plug-in's format/UID/shell
identity and the remote endpoint's host/port/protocol before paste. Cosmetic
labels and a relocated binary remain compatible. N17 clip/range clipboards
retain each attached media's content identity and refuse an ID reused for
another file. Hashed media can move or change missing status; legacy unhashed
rows retain their locator/dimensions. No peaks or media bytes are copied into
the clipboard. These checks run inside preparation's read snapshot; commit
retains the existing expected-head guard.

N18: **test_all PASS, 9920/107, validators clean**; Clang device-copy35,
device-ops139, catalogue106, replay63, device-model71 pass. A compiled bypass
of the identity check causes 2 failures, restored checks pass. N17: **all
109 C++ suites pass, 10124 checks**; full runner still fails only README51
versus schema53. Clang clip-edit172, playback159, row-edits22, material24,
catalogue106, replay63 pass; a compiled bypass causes 3 clipboard failures.
No remote CI/MSVC/JUCE/hardware/Live parity claims. The N17 scope gaps remain
open as recorded in its original entry.

**N28 dependency found:** the sample handoff/preparation SDK already exists:
LibPdEngine::bindSamples/publishSample, PdSampleSlots and the public Color Cab
and Hybrid Reverb prepared-sample helpers. The project's JuceDeviceLoader,
however, still explicitly refuses format=internal ("internal devices are not
built yet"), and PdDevice exposes neither parameters/state nor a project
loader. A sample op alone would therefore create stored data with no usable
project device to receive it. This is the same host/catalogue insertion seam
required by **N15**. Bringing N15 forward to supply that dependency, then return
to N28, N31 and the remaining needs. Native DSP implementations and canonical
canvas bytes remain the devices lane's; use their existing public factory/SDK,
do not alter DSP or UI. No director gate or permission question is involved.

---



## 2026-11-02 — N18 ready; continuing N20/N28/N31

**PR requested:** **engine/device-copy**, head **31fe98e**, to **main**
(base **0e20848**). Title:
**Expose stored device copies and prepare ordered paste batches (N18)**.

PR body / window handoff: call adi::readDeviceCopy(Store&,id) on the Store's
owner after flushing any pending host edit capture that should be included.
The value contains the registered device.remove inverse as a device.insert
payload, the referenced plug-in's metadata and a hasNestedChains flag. It reads
stored params, state hashes (not their bytes), panel order, expression route,
name/preset/bypass/missing/always-process, nullable rack kind and remote host.
Read-only inspection is supported; no op or live plug-in read happens. For
paste, call prepareDevicePaste(Store&,copy,newId,chain,beforeDeviceOptional)
then commitDevicePaste. A null selection appends. The batch captures explicit
moves in current (ord,id) order, handles ties and large ordinals, inserts
immediately before the selection, and undoes every original ordinal exactly.
The captured expected head includes a null root. Keep document generation
with asynchronous results. Same-project refs/state are reused; cross-project
pool import and nested rack images are not supplied and return named refusals.
The reader still exposes a rack's root metadata. The window can now implement
ordinary device Cut/Copy/Paste/Duplicate without reading SQLite itself.

The existing insert/remove pair now retains native rack_kind/remote_host_id,
through optional nullable rackKind/remoteHost payload keys. Inferred-int
narrowing of order/latency/parameter flags is fixed. Values outside SQLite's
signed 64-bit range refuse; late parameter failures roll back the new root and
all preceding order moves. No schema bump, host processing or UI code change.

GCC Debug/Werror: **test_all PASS, 9915 checks across 107 suites, validators
clean**. Clang Werror: device-copy30, device-ops139, catalogue106, replay63,
device-model71 pass (409); new reader/test and changed catalogue also pass
-Wshadow/-Werror syntax checks. Compiled plants fail: ignored before-position1,
old flag replay4, lost expected head1; restored checks pass. Remote CI, MSVC,
linked JUCE, hardware and measured Live behavior unobserved. Win/window opens;
win merges.

**N20 ownership/contract still open:** main 0e20848 still declares
audio.channelConfig as Device-scope empty Text (settings/registry.cpp154);
AppSettings::set still refuses Device values (settings/store.cpp104). Its
device-ID/channel-pair/name format and store belong to win; opening physical
inputs is in the window's app code. Session's AudioIo already accepts inputs,
but this is not a recording-writer implementation. Recording integration stays
open pending the published channel configuration seam; no project-field
workaround or registry edit has been invented. This is not a director gate.
Continuing to **N28** sample assignment, then **N31** and the remaining needs.

Main now includes the director's actual first-window remarks in
collab/director.md (#355): R1/R6 wait on N26 (green and pushed at 04f3290);
N40 supplies waveform data. The first window has been opened. The UI remarks
remain the window's; this lane does not edit those files.

---



## 2026-11-02 — N26 retained curve domains green; moving to N18

**Existing PR request updated:** engine/snapshot-automation is now
**04f3290** (after 6566d8e), still to main. Keep its one N26 PR.

Window handoff: AutomationPointNode now carries optional curveSlice
{begin,end,sourceFirst,sourceLast} for its outgoing segment. When N17 is present,
convert those four fields to engine::CurveSlice and draw with curveSliceValue;
refitting the stored kind/tension between the new endpoints changes its shape.
The envelope retains curveRanges with each range's identity, storage ordinal,
owner and values. All fields participate in sharing, including metadata-only
changes with byte-identical AAUT. Invalid owners, missing/duplicate ordinals,
nonfinite values and bad intervals refuse the entire envelope with a problem;
diagnostic metadata stays readable. Old retained frames remain unchanged.
The optional SQL seam reads N17's table when present and adds no schema or
branch dependency. Clip envelope playback is still the separate phase D item.

GCC Debug/Werror: **test_all PASS, 9901 checks across 107 suites, validators
clean**. Clang Werror: snapshot87, automation39, Session214, engine119 pass;
reader/test also pass -Wshadow/-Werror syntax checks. A compiled defect that
compares only the number of retained ranges causes 11 failures; restored checks
pass. Remote CI/MSVC/JUCE unobserved. README's measured check count stays win's
(advisory here; no table mismatch). Continuing directly to **N18**; no director
gate or permission question has been reached.

---



## 2026-11-02 — N33 follow-up for N17's pinned source clocks

**Existing PR request updated:** engine/clip-properties is now **7496a7d**
(after 9b50180), still based on engine/markers-locators. Keep its one N33 PR;
no merge, rebase, force or additional stack dependency.

Start/End/Loop property edits now honor a present N17 clip_playback_details
table: Start clears the offset and retained fade domains, End clears the
source limit, and a loop window edit clears both physical endpoints. A
switch-only edit keeps its dormant window. Their inverse restores the full
typed details row (or absence), including future BLOB columns. Clip deletion
also includes the optional playback/range child tables and remains able to
restore old captured payloads. No N17 schema or C++ dependency was added;
the standalone suite supplies the optional SQL seam.

Window uses the same N33 ops. Win retains N17's stronger clip-image/FK guard
when combining both branches; this follow-up supplies the property-clock
invalidation and typed inverse. It prevents a successful field edit from
leaving playback on an older split-time override.

GCC Debug/Werror: **10071 checks across 108 C++ suites pass**. Full runner
still fails only README's 51-vs-53 table count; other validators pass.
Clang Werror: properties95, playback89, catalogue106, migration175 pass.
A compiled lost-invalidation plant produces 3 failures, restored checks pass.
Remote CI/MSVC/JUCE unobserved. Continuing with N26's retained-curve snapshot
compatibility and then N18; the N17 remainder stays recorded above.

---



## 2026-11-02 — N17 captured editing and material actions; N17 stays open for the rest of Time and Warp/video

**PR requested** (ADR-0210 d4): **engine/phase-c-editing**, head
**f6e9613ee03affe8ac522de8555d93ee6ed8f789**, to **main** (cut from dff5b41;
main fetched as add5102 during this work). Title:
**Add captured range edits, fades, Insert Silence and import/bounce actions (N17)**.

PR body / window handoff: use src/adi/clip_commands.hpp for copyClip,
prepareCutClip, preparePaste, prepareDuplicate, prepareSplit, copyRange,
prepareCutRange, prepareDeleteRange, preparePasteRange, prepareDuplicateRange
and prepareInsertSilence. Pass explicit ticks/ns and the actual
SessionSpec.sampleRate. Cut captures clipboard/deletion under one read
snapshot. A range edit splits crossing clips and retains outside material.
Paste maps selected tracks explicitly; a different destination clears the
source comp lane. Commit through commitPrepared so the captured expected
journal head, including a null root, is checked in the transaction. Keep the
document generation with asynchronous results. IDs and typed row values,
including native opaque children, future BLOB columns and external alias
links, are captured before apply. Undo/redo neither allocate IDs nor evaluate
defaults. Uncaptured FK effects and generated columns refuse by name.

clip.setFades edits both handles in one step, with lengths/curve and optional
continuous tension; no automatic internal edge fades. Snapshots/projection
carry all fade fields and optional source/fade clocks, each in sharing.
Source windows stay unchanged, including SRC phase; tick/ns placements retain
their declared domain. Splitting through either active fade preserves its
original envelope at both widths. Insert Silence shifts clips, maps,
locators/cycles, arranger sections, project end and the saved transport loop
(including exact JSON undo/future fields). Initial map events at zero stay.
Global automation holds at the gap and retains original nonlinear curve
domains on both sides, including repeated cuts. Original full-width point IDs,
flags, tension and trailer bytes remain; new boundary IDs are captured.
Direct blob replacement refuses data with retained curve ranges.

Use src/adi/material_commands.hpp for ImportOptions and BounceOptions:
prepareImportAudio/prepareBounce return a MaterialEdit; commitMaterial
publishes owned files without clobbering immediately before SQL COMMIT.
Import captures dimensions/hash, deduplicates, attaches a playing clip and
honors explicit copy-in/edge-fade choices, in one undo step. Defaults off.
Bounce captures a private SQLite/WAL backup, renders an independent Session
through the existing source/tap/range/encoding runner, then attaches the result.
An explicit replace selection gives Bounce in Place. Host thread contracts
remain the caller's. Cancellation, stale head, target collision and late SQL
failure leave no partial clip/media/publication. Undo retains media for redo;
reusing an already committed result cannot remove its successful file.
See docs/ENGINE-EDIT-COMMANDS.md for the exact interface and limits.

**N17 is not struck.** This PR's insertTime flag inserts musical time then
pastes/duplicates the captured material; its clipboard includes clip envelopes,
not a full global tempo/meter/key/automation clipboard. Full Copy/Cut/Delete
Time remains engine work. Auto-Warp tempo/bar-marker detection and the warp
renderer, playable video attachment, arbitrary join/consolidation and alias
playback are not supplied. Warped-source edits, aliases requiring split, future
AAUT strides requiring unknown boundary fields, Warp-on-import and video
return explicit refusals rather than writing an unplayable audio clip. Clip
envelopes are preserved, not newly played. No Live parity or backend completion
is claimed for those gaps. Keep them queued while the usable API reaches the
window; continue the oldest independent needs instead of making it wait.

**Win integration:** schema 1.10 adds clip_playback_details and
automation_segment_ranges; frozen 1.9 is byte-identical to the base. Standalone
has **53 tables**. Reconcile the minor/migration numbering with N25 marker1.10,
N33 clip1.11 and N12 pan1.10 when merging. A tree containing just all four
additions has 56 tables. Do not leave an already-current file missing a sibling
table. README counts stay win's. The N33 optional-clock invalidation and N26
curve-domain snapshot compatibility follow-ups are next, on their existing
branches. At merge, retain N17's complete clip image/FK guard, the N33 properties
ops, and the combined End marker/source-clock reader.

Validation: GCC14 Debug/Werror, libpd on/JUCE off: **all 109 C++ suites pass,
10119 checks**. Full test_all is **not green** only because validate_schema
reports README's 51 tables versus this branch's 53. Every other validator
passes, including the OPS count, layout, frozen history and migration/additivity.
Clang19 Werror: clip-edit167, playback159, row-edits22, material24,
automation39, curves48, catalogue106, migration162, projection212 (939 checks).
All 11 new C++ sources additionally pass Clang -Wshadow/-Werror syntax checks.
Compiled plants fail: refitted automation8, lost fade domains6, lost expected
head1, leaked publication1; restored checks pass. Renders cover SRC at 44.1/48 kHz,
loop phase, tempo changes, partial fades at native32/64, exact PCM before/after
split, Insert Silence and undo/redo. Import/bounce checks cover deferred SQL
commit failure cleanup, cancellation, mutation during render, the next clip-ID
collision in Bounce in Place, repeat commit and cross-track lane ownership.
Remote CI, MSVC, linked JUCE, hardware and measured Live behavior are unobserved.
Win/window opens this PR and win merges. Continuing through the queue.

---



## 2026-11-02 — N12 ready; continuing to N17

**PR requested** (ADR-0210 d4): `engine/split-stereo-pan` at **`c3ab3a9`** →
`main` (base `dff5b41`). Title: **Add persistent Split Stereo Pan with independent panners (N12)**.

PR body / window handoff: commit `mixer.setPanMode {id,mode:"stereo"|"split"}`,
`mixer.setPanLeft {id,pan}` and `mixer.setPanRight {id,pan}` (finite -1..+1),
then refresh Session and Snapshot. `id` is the existing mixer strip's track id.
TrackNode now exposes panMode/panLeft/panRight, all in same(); other tracks and
unchanged settings stay shared. Both split positions persist while in stereo;
the original pan persists while dormant in split. Left/right edits are
coalescable. Defaults are stereo, left -1/right +1. Independent real-unit track
lanes panLeft/panRight bind, override separately and use Re-Enable normally.

The native-width matrix independently positions each source. Default hard-L/R
is exact unity; a swap exchanges sources, either side can sum both. Live's split
mono panners use constant power without its ordinary stereo sqrt(2) boost;
other selected laws retain their formulas. Fader/mute, five-ms coefficient
ramps, silence landing, precision handover and source Post sends include the
crossfeeds. Both inputs are captured before either output for in-place safety.
Pre sends retain unity and mute; extra channels retain fader-only behavior.
No callback allocation/lock/log. Width/phase remain Utility's placement.

Schema **1.10**, frozen 1.9: additive optional mixer_pan_settings keyed to
mixer_strip, no altered frozen core columns. Fresh/migrated/read-only TEMP
reads and projection expose it. First-touch inverse restores row absence;
later inverses update only their column, preserving dormant controls and
future fields. Unknown future mode reads/names as stereo and can be restored
verbatim by undo. OPS and `docs/ENGINE-SPLIT-PAN.md` define the seam. Full
track deletion capture is still **N37**; add this cascade table to its child set.

**Win merge work:** this is independent of N25's marker1.10 and N33's clip1.11.
Reconcile minor numbers/migration history in merge order before publishing the
combined format; do not leave an already-current file missing a new table.
Its standalone schema has **52 tables**, so README line57's 51 must become 52
for this branch's CI; the combined N25/N33/N12 tree has 54. README stays win's.

Validation: GCC Debug/Werror, libpd on/JUCE off, **all 107 test binaries pass,
9898 checks**. test_all is **not green** solely for the current schema validator's
README 51-vs-52 table count. OPS catalogue totals/version validator metadata are
updated and revalidated; all other validators pass. All migrations/additivity
checks pass. Six Clang Werror suites pass: split69, mixer337, engine119,
Session214, migrate162, projection209. Covers native32/64 matrix, exact unity /
swap / collapse / center, in-place reads, extra/mono channels, fader/mute,
Post/Pre sends, segment invariance, ramp/width handoff, independent automation,
ops/undo/redo/future columns/future mode, all snapshot fields, projection,
reopen/refusal, read-only unchanged schema and writable migration. Four compiled
regressions fail: missing crossfeed21, overwritten source19, missing mode
sharing1, missing first-touch absence2. Remote CI/MSVC/JUCE and measured Live
parity unobserved. Win/window opens; win merges. Continuing to **N17**, phase-C
editing ops and prompt actions. No director gate reached.


## 2026-11-02 — N8 ready; continuing to N12

**PR requested** (ADR-0210 d4): `engine/plugin-editors` at **`ac17ea1`** →
`main` (base `dff5b41`). Title: **Expose VST3 and CLAP editors with safe floating host content (N8)**.

PR body / window handoff: include `juce/editor_host_component.hpp`, compile
`editor_host_component.cpp` in the JUCE window target, and call
`createEditorComponent(device,error)`. Move its result into FloatingHost::openView,
using the component's own size. Call PluginEditorComponent::frame() on that
window's existing clock. The bridge owns the editor lease; duplicate wrappers
are refused. The exact VST3 editor uses JUCE 9's createEditorAndMakeActive,
constrainer/resize notifications and editorBeingDeleted. CLAP Hosted mode
creates and parents a native child inside the floating host, exposes size and
resize hints, adjusts host sizes, and respects physical/logical API pixels.
Report bridge.error() if native parenting/show fails. No editor is inline.

`DeviceInstance::createEditor(EditorMode,error)` also exposes a shared protocol
lease directly. Close is idempotent; removing a device closes its GUI before
its processor/library is destroyed, leaving retained handles safe and invalid.
CLAP's host GUI callbacks use fixed atomic mailboxes; the existing DeviceHost
message-thread pump or editor.poll() drains them. No callback GUI work,
allocation, lock or new timer. closed(true) receives its required destroy
acknowledgment without hiding a disconnected window. Failed create, dimensions,
show/hide/parent and resize are reported honestly. Native floating CLAP mode
also supports title/transient owner and window-owned geometry (0/0 if not exposed).

**Win review, not a director gate:** N8 asks for component content on the newer
FloatingHost seam (ADR-0180); ADR-0076 specifically describes CLAP create(true).
Both modes are implemented explicitly. Choose native Floating if retaining that
older route; it needs no second JUCE wrapper window. Hosted is the N8 component
route and never means an inline strip GUI. No ADR changed. Details and exact
ownership/frame contract: `docs/ENGINE-PLUGIN-EDITORS.md`. No schema/op change.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9862 checks / 107 suites**,
validators clean. Five Clang Werror suites pass: editor48, CLAP486, device44,
model71, host60. The protocol fake covers creation rollback, size/mode/API
refusal, shared leases, floating/hosted parent distinction, visibility/refusal,
resize hints, worker notifications, no resize echo, closed acknowledgment,
reopen and destruction order/retained handles. Three compiled regressions fail:
late device GUI close (2), ignored plugin close (10), show before parent (2).
Pinned JUCE 9.0.2 headers: GCC and Clang syntax checks with Werror/Wall/Wextra/
conversion/sign-conversion/shadow pass for the hosts, bridge and JUCE fixture.
The existing JUCE probe now includes 16 ownership/constrainer/component checks
for its macOS/Windows jobs. They are **not run locally**: a linked JUCE Linux
configuration failed for missing GUI development libraries (freetype etc.).
Native real-plugin drawing/parenting, monitor/DPI and measured Live parity,
MSVC and remote CI remain unobserved. Win/window opens; win merges.
Continuing to **N12**, Split Stereo Pan. No director gate reached.


## 2026-11-02 — N7 ready; continuing to N8

**PR requested** (ADR-0210 d4): `engine/ui-param-capture` at **`7d5e1f7`** →
`main` (base `dff5b41`). Title: **Capture device strip parameter gestures as undoable edits (N7)**.

PR body / window handoff: call `ParamOps::uiEvent(dev,param,kind,norm,nowMs,out)`
on the owner/message thread, using Begin/Value/End and the timer drain's clock.
Call `attachSession(session)` after load/refresh. It supplies a separate UI
capture per device, retaining the plug-in's single-producer ring. Accepted
values sound immediately; End emits one final edit and its first-touch opener.
Commit every appended request as one transaction, writing pending state blobs
first just as for timer drain. The bool describes the current input; older
intervals can append requests even when it returns false. Devices already hold
accepted values: do not replay own openers. Keyboard values close after 150ms
quiet. Removed devices, external edits and recalled state cancel stale gestures.
Backend refusal and invalid targets are reported, not recorded as accepted.

A Session attachment publishes a runtime automation override before End;
Re-Enable Automation resumes the lane. Refresh N26 with overriddenLanes() when
that set changes. CLAP control ids remain hex and automation refs decimal per
ADR-0165. As with existing graph publication, an already-running callback is
not interrupted; this is not a sample-exact manual/automation arbitration API.
The CLAP control queue is now bounded SPSC, retains backpressure and failed
callback values, exposes accepted pending intent, flushes at stop/reactivation,
and discards older intent only after successful guarded state recall. Delayed
UI echoes cannot become extra undo steps. Immediate undo supersedes accepted
intent even if a backend getter lags. No new schema/op name; OPS and
`docs/ENGINE-UI-PARAM-CAPTURE.md` describe the seam. Window wiring stays window's.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9874 checks / 107 suites**,
validators clean. Seven focused Clang suites pass: UI47, ParamOps177,
ParamEdits377, CLAP499, device automation133, Session214, automation39.
Covers long drags, keyboard intervals, independent concurrent producers,
first-touch/journal undo/redo, refusal, delayed echoes, asynchronous undo,
state recall and device retirement, live automation override/re-enable and
CLAP decimal lookup. CLAP fake additionally exercises queue overflow,
small-event-list backpressure, retry, stop/format/state boundaries and 8192
accepted values across concurrent FIFO wraps. Three compiled regressions are
caught: lost echo history (2), old-audible equality (1), missing override (1).
Callback allocation/IO guards pass. Remote CI, MSVC/JUCE and measured Live
parity unobserved. Win/window opens; win merges. Continuing to **N8**,
plug-in editors through the hosts, from current main. No director gate reached.


## 2026-11-02 — N34 ready; continuing to N7

**PR requested** (ADR-0210 d4): `engine/note-active` at **`2db02a4`** →
`main` (base `dff5b41`). Title: **Activate MIDI notes without rewriting their stream (N34)**.

PR body / window handoff: use the already documented
`note.setMute {clip,note,muted}` with `muted = !active`. `note` is N32's original
uint64 ANOT identity, not display order. Commit as a snapshot-impact edit and
refresh Session and the Clip View read. Inactive notes stay present/readable;
playback skips them. Undo/redo of this symmetric op restores the prior muted
bit, and deactivating a held note releases its voice on the next callback.
The clip must be MIDI with an existing canonical notes stream. Zero/unassigned,
absent or duplicate target ids and unreadable/unsupported-unit streams are
refused, never silently replaced with an empty stream.

The handler patches only bit0 in the original record stride: other flags and
fields, wider future tails, original header/trailing bytes, row id/extension
columns and other streams survive exactly. No sorting, blob re-encoding,
schema bump or new audio-thread work. Existing OPS entry now has its handler;
`docs/ENGINE-NOTE-ACTIVE.md` defines the call. N32 is an independent compatible
reader, not a build dependency.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9866 checks / 107 suites**,
validators clean. New flag suite **49**; four Clang Werror suites pass: flags49,
MIDI clips168, catalog106, ops130. Covers exact one-bit native/future stream
changes on first and second records, full-width id, other flags/streams/columns,
undo/redo/reopen, malformed/ambiguous target refusal, atomic batch rollback and
read-only refusal. Op-driven playback proves deactivation, undo reactivation and
held-voice release; callback allocation/IO guards pass. Three compiled plants
fail: wiping other flags (12), wrong bit (16), wrong stride (1). Remote CI,
MSVC/JUCE and measured Live parity unobserved. Win/window opens; win merges.
Continuing to **N7**, a separate window producer into ParamOps edit capture,
lifting the engine portion of `537c56d` and testing live/undo behavior, from
current main. No director gate reached.



## 2026-11-02 — N33 pushed; continuing to N34

**PR requested** (ADR-0210 d4): `engine/clip-properties` at **`9b50180`** →
**`engine/markers-locators`** (`bb846aa`, N25). Two deep; merge N25 first.
Title: **Edit and play clip markers, loop, signature and scale (N33)**.

PR body / window handoff: submit `clip.setStartMarker {id,ticks}`,
`clip.setEndMarker {id,ticks}`, `clip.setLoop {id,on,start?,length?}`,
`clip.setTimeSignature {id,num?,den?}` and
`clip.setScale {id,root?,scale?,mask?}`. All are undoable snapshot edits; refresh
ProjectView and Session, and N32's `readClipDetails` for the Clip View. Source
markers/loop use ticks in 0..2^50 even for a linear clip, preserving placement
and its units. Start pins the previous default End; End bounds non-loop audio
at the exact sample in both graph widths and releases crossing MIDI notes.
Loop uses its own window with End inactive. A malformed imported loop can still
be switched off. Signature uses numerator1..255 / power-of-two denominator1..128,
both omitted to inherit. Scale takes root0..11/nonempty id/12-bit relative mask
including tonic, all omitted to inherit. Signature/scale are metadata, not an
implicit Fit to Scale. `rows::Clip` and the text projection carry local values;
N32's optional-table reader is already compatible (independent PR).

Schema **1.11**, additive optional `clip_properties`, frozen 1.10 and migration/
read-only tests. N25 is the format dependency; N32 is not a build dependency.
Scalar inverses capture full core/property rows, update in place and retain
presence/extensions. Clip delete also captures all native streams, expressions,
audio, clip envelopes, properties, and alias links cleared by FK; restore keeps
ids/types/BLOBs and refuses collisions. Legacy inverse payloads still apply.
Arbitrary unknown child tables are not claimed. Full track deletion is still
N37; this closes the clip-level loss newly exposed by Main properties.
API/semantics: `docs/ENGINE-CLIP-PROPERTIES.md`, OPS/SPEC updated.

**Win-owned integration edit before CI/merge:** README's table count is **51**
in this branch, **53** in its schema. Please update `adi_daw/README.md:57` to
53 in this PR, after N25's count52 edit. Engine leaves that count to win.
This is `validate_schema.py`'s sole failure; every frozen minor0..10 upgrades to
1.11 and the additive-object checks pass. No validator was weakened.

Validation: GCC Debug/Werror, libpd on/JUCE off, **10061 checks / 108 suites**,
all test binaries pass. `test_all.sh` returns failure solely on that README
count; ops/pd/ci/layout validators pass. Seven Clang Werror suites pass:
properties85, audio clips89, MIDI clips167, markers126, migrate175, projection211,
media ops71. Tests include op-driven exact audio/MIDI source boundaries, double
precision, loop End inactivity, full-state undo/redo/reopen, native cascade and
alias restoration, collision rollback, extensions, invalid/batched edits and
old read-only bytes unchanged. Five compiled plants fail: missing End pin (2),
missing aliases (4), missing native children (4), no audio cut (2), no MIDI cut
(1). No remote CI, MSVC/JUCE or measured Live parity result claimed. Win/window
opens; win reviews/merges. Continuing to **N34** independently from main, using
existing documented `note.setMute` for the active flag (muted=!active).
No director gate reached.



## 2026-11-02 — N32 ready; continuing to N33

**PR requested** (ADR-0210 d4): `engine/clip-details` at **`93f2e35`** →
`main` (base `dff5b41`). Title: **Expose clip properties and exact note identities for Clip View (N32)**.

PR body / window handoff: the row allows an owner-thread read API rather than
inflating every arrangement snapshot with notes. Call
`engine::readClipDetails(store, clipId)` after journal movement, outside its
mutation transaction. The owned result has kind/name/optional palette colour,
track/lane/alias identity, original tick/ns placement/length, all common clip
fields, source start/end markers, loop switch/window, inherited signature/scale
at the clip's start (including native tuning id/degrees), and every original
ANOT note id and property, including inactive notes. Address edits with
`{clip:clipId,note:note.id}`; display order is not identity. Values remain stable
while held. Invalid streams/notes are named without a partial curve or losing
clip metadata. Legacy zero/duplicate ids stay exact with a problem; refuse
ambiguous edits rather than inventing row-index identities. Future note extents
and encoded bytes remain available. Read targets one stream, no op/playback.
A missing clip or SQL failure is explicit. See `docs/ENGINE-CLIP-DETAILS.md`.

No schema/op change here. The reader understands N33's optional additive
`clip_properties` table when present: local end marker, signature and 12-TET
scale with explicit local flags, NULL/absence means inheritance. Compatibility
is tested with a synthetic table, not claimed as a native migration. N33 supplies
writes/playback; N34 supplies note active writes. Until then Start is the stored
content offset and End derives from source offset plus visible duration, through
the step tempo map for a linear clip. Raw stored units always remain readable;
unrepresentable derivations and tempo ramps are named.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9843 checks / 107 suites**,
validators clean. New suite **29**; five Clang Werror suites pass: clip details29,
MIDI clips165, catalog106, ops130, project view14. Covers a real note move using a
returned full-width id, reorder/undo and retained values; all common properties;
NULL versus zero; inherited/local meter and scale; linear tempo-change context;
range guards; inactive/ambiguous/corrupt/future notes and read-only reopen.
Three compiled plants fail: lost identity (4), reversed active bit (1), ignored
local properties (1). Remote CI, MSVC/JUCE and measured Live parity unobserved.
Win/window opens; win reviews/merges. Continuing to **N33** from N25's
`engine/markers-locators` (two deep), so its additive format is 1.11 after N25's
1.10. N32's read API is compatible but not a build dependency. N34 follows.
No director gate reached.



## 2026-11-02 — N29 ready; continuing to N32

**PR requested** (ADR-0210 d4): `engine/history-list` at **`de77bf3`** →
`main` (base `dff5b41`). Title: **Expose complete undo transactions with branches and snapshots (N29)**.

PR body / window handoff: call `History(store).readTimeline()` on the message
thread after history moves or snapshot metadata changes, outside a journal
mutation. `steps` contains whole undo transactions newest sequence first across
all branches: txn id, first/last seq, op count, first-op label/actor/detail,
timestamp and parent; `applied` marks current-head ancestry and `onCurrentLine`
also marks Ctrl-Y's redo path. `branchIds` names stored-head ancestry, with the
current branch's redo line included, and `snapshotIds` links anchors. Root
head/branch and complete branch/snapshot metadata are included; root snapshots
remain even before any step. Values are owned so a retained frame is consistent.
One read transaction, no CBOR/inverse/state blobs decoded, no log/head mutation.
Ephemeral ops excluded; corrupt tables/cycles/partial-transaction heads return a
named error with no partial data. Existing History APIs perform the actions.
`docs/ENGINE-HISTORY-LIST.md` defines the statuses. No schema/op change.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9843 checks / 107 suites**,
validators clean. New list suite **29**; four Clang Werror suites pass:
list29, history95, snapshots65, ops130. Checks include mixed-actor batches,
first-op labels, branching/shared ancestry, undo redo-line status, branch switch,
root/automatic anchors, retained values, backwards timestamps, metadata-only
reads despite malformed payload bytes, read-only reopen and explicit corruption/
nested-transaction errors. Two separately compiled plants fail: reversed order
(2), marking undone steps applied (1). Remote CI, MSVC/JUCE and measured Live
parity unobserved. Win/window opens; win reviews/merges. Continuing to **N32**,
the message-thread clip properties/note-identity read API allowed by its row,
from main. N33 and N34 follow. No director gate reached.



## 2026-11-02 — N26 ready; continuing to N29

**PR requested** (ADR-0210 d4): `engine/snapshot-automation` at **`6566d8e`**
→ `main` (base `dff5b41`). Title: **Expose automation lanes, envelopes and runtime overrides (N26)**.

PR body / window handoff: `Snapshot::automationLanes` / `findAutomationLane(id)`
expose every stored owner/parameter/domain/unit/default/bounds/enabled/visible/
height field, with arrangement and clip envelopes identified by data id and
optional clip id. Point ids are the original uint64, times the original ticks/ns
(clip-relative for clip envelopes); value, curve, tension and flags are readable.
Invalid envelopes retain an explicit problem and no partial curve. Future AAUT
tails and corrupt encoded changes participate in sharing. After refreshing the
Session, call `ProjectView::refresh(store, session.overriddenLanes())`; repeat
after Session's existing single/all `reenableAutomation` methods. Root
`automationOverridden` lights Re-Enable; per-lane `overridden` identifies the
control. Retained frames keep their own flags. The no-set overload is a view
without a live session; overrides are not persisted or journaled. Every field
participates in sharing, with unchanged envelope/sibling reuse, and changed
bounds/time bases revalidate before reuse. `docs/ENGINE-AUTOMATION-SNAPSHOT.md`
gives the refresh contract. No schema, op or audio playback policy change.

Validation: GCC Debug/Werror, libpd on/JUCE off, **9882 checks / 107 suites**,
all validators clean. New suite **68**; five Clang Werror suites pass:
automation snapshot68, engine119, project view14, automation39, parameter
automation133. Checks cover metadata and envelope identity/sharing, units,
full-width point ids, retained override frames from a real Session, Re-Enable,
corrupt/nonfinite/refused streams, bound revalidation, future tails, deletion.
Three separately compiled plants fail: missing override (1), skipped bound
revalidation (1), ignored encoded changes (11). No remote CI, MSVC/JUCE or
measured Live parity result claimed. Win/window opens; win reviews/merges.
Continuing to **N29**, the complete transaction-based undo list with branch
membership and snapshot anchors, from current main. No director gate reached.



## 2026-11-02 — N25 pushed; continuing to N26

**PR requested** (ADR-0210 d4): `engine/markers-locators` at **`bb846aa`**
→ `main` (base `dff5b41`). Title: **Add marker locators, literal notes and prompts (N25)**.

PR body / window handoff: read `Snapshot::markers` / `findMarker(id)` for stable
identity, time base, optional tick/ns positions and cycle lengths, name, kind,
optional palette colour, note and literal prompt. Id order is not display order.
Every field participates in sharing. Use `marker.create {id,pos,time_base?,name?,kind?,length?,color?,note?,prompt?}`
(default musical position; a length defaults to cycle), `marker.delete {id}`,
`marker.move {id,pos}`, `marker.rename {id,name}`, `marker.resize {id,length}`,
`marker.setColor {id,color?}` (omit to clear), `marker.setNote {id,text}` and
`marker.setPrompt {id,text}`. Cycles need positive lengths, colour is 0..69.
All are snapshot-impact undoable edits; no graph rebuild. Text is never executed.
Schema 1.10 adds optional `marker_details`, frozen 1.9 and migration/read-only
coverage; the projection includes note/prompt and linear cycle lengths. Captured
undo retains both rows, extension columns and SQLite types; scalar inverses
update in place, avoiding cascade loss. A failed batch leaves no rows/log data.

**Win-owned integration edit before CI/merge:** `adi_daw/README.md:57` still says
**51 tables**; this additive schema has **52**. Please change that count in the
feature PR. Engine leaves the README count to win (LANES §1/standing prompt).
This is the sole `validate_schema.py` failure, not a director gate. No other
validator is failing. The schema checker proves every frozen minor upgrades to
1.10, including the named 1.9 default rebuild; no validator is weakened.

Validation: GCC Debug/Werror, libpd on/JUCE off, **9955 checks / 107 suites**,
all test binaries pass. `test_all.sh` returns failure solely for the README count
above; ops/pd/ci/layout validators pass. Marker suite **126**; five Clang Werror
suites pass: marker126, engine119, migrate162, text projection209, history95.
New coverage includes both units, literal multiline text, sharing of every
field, null colour versus zero, exact undo/redo after reopen, atomic failure,
1.9 read-only bytes unchanged, empty/nonempty BLOBs and quoted extension columns.
Two separately compiled plants fail: omitted deleted text (3), ignored note
sharing (1). No remote CI, MSVC/JUCE or measured Live parity result claimed.
Win/window opens the PR and win reviews/merges. Continuing to **N26**, lanes,
points and runtime overrides in the snapshot, independently from main.



## 2026-11-02 — N23 ready; continuing to N25

**PR requested** (ADR-0210 d4): `engine/cue-reference` at **`1466c85`** →
**`engine/transport-loop-metronome`** (`e42fd56`, N19). Two deep; merge N19 first.
Title: **Route Cue and exclusive reference past Main (N23)**.

PR body / window handoff: `Snapshot::cue` exposes mode, selected track ids,
output pair, volume, Solo in Place and reference selection/state. In Cue mode
row buttons emit `cue.setTrack {id,on}` and read `selectedTracks`, keeping stored
solo flags separate; `cue.setMode {on}` switches their behavior, and
`mixer.setSoloInPlace {on}` controls returns. Use `cue.setOutput {first}` (even,
zero-based stereo pair 0..62), `cue.setVolume {db}` (-150..+6), and
`cue.setReference {on,id?}` for the tap/hold latch. All are saved transport-scope
performance ops, audited/outside undo. Refresh ProjectView and Session. Cue is
Post FX/PFL, including muted tracks. Reference keeps its source fader/pan and
volume/pan automation, bypasses Main/parent processing and uses a complementary
5 ms Main/reference swap to both monitor and Cue Out. Other cue signals/clicks
are excluded while reference is exclusive. Shared Main/Cue pairs get reference
once; selection and volume changes ramp. Fresh reference feeds fill their delay
before Main fades, preventing a cold-start dip/click. Monitoring reuses rendered
audio: devices run once, private graph sinks align to Main/track delay, stable
edge keys preserve history, and lane ownership has its own audio publication.
Offline renders ignore Cue/audition/mode changes and preserve musical solo/mute.

**N38 integration:** no role DDL/version/migration is invented here. If
`tracks.role` exists, its native reference is excluded from Main/sends and
supplies the default audition target. `projectReferenceTrack`/`referenceFromRole`
expose that derivation; preference edits do not pin a previous role. On today's
schema an explicit id already auditions past Main; selecting an ordinary track
as a performance target alone does not alter its Main route while off. The role
format/snapshot/op need remains with win/engine as N38, not claimed closed.
The synthetic role column test proves reading only. API details:
`docs/ENGINE-CUE-MONITOR.md`; OPS and format/session contracts updated.

**Merge with N13:** meter Main after graph processing, before `cueMonitor_.process`.
The monitor mixer now hosts N19's click on Cue Out/volume.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9960 checks / 108 suites**,
validators clean. Cue suite **96**, also Clang Werror. Six other Clang suites:
mixer337, transport/settings49, host124, realize105, precision108, export197 pass.
Checks include 5 ms enter/leave/shared-pair levels, Main FX/fader bypass, PFL mute,
SIP/returns, output widths, compensation/history, cold activation, automation
values/lifetime, no callback allocation and offline isolation. Four separately
compiled plants fail: Main audio in cue (23), wrong clock (3), missing lane owner
(1), skipped cold delay wait (2). MSVC/JUCE, hardware routing and measured Live
sound/level parity remain platform gates; no remote CI result observed here.
Win opens/reviews/merges. Continuing to **N25**, marker/locator ops and snapshot,
from current main. No director gate has been reached.


## 2026-11-02 — N22 ready; continuing to N23

**PR requested** (ADR-0210 d4): `engine/snapshot-track-delay` at **`673c51c`**
→ `main` (base `dff5b41`). Title: **Expose track delay in immutable snapshots (N22)**.

PR body: The window can read `TrackNode::delaySamples`, a signed int64 stored at
`Snapshot::sampleRate` (project rate); display milliseconds as
`1000.0 * delaySamples / sampleRate`. Missing strips read zero. The field
participates in structural sharing, so delay-only edits replace that track's node
and retain siblings. Existing `mixer.setDelay {id,samples}` and its undo are the
write path; this changes no schema, op or audio processing. Raw stored values are
exposed, including values the runtime may clamp/name as a problem.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9823 checks / 106 suites**,
validators clean. Engine suite **128**, also Clang Werror; Clang project-view14
passes. Nine checks cover both signs, project-rate units, missing/removed strips,
undo, changed-node identity and unchanged sharing. A compiled plant omitting delay
from `same` fails three checks. N19-only stale test binary excluded. Remote CI,
MSVC/JUCE and Live measurements unobserved; win opens/reviews/merges.

Continuing to **N23**, cue path, Solo in Place and exclusive reference. It needs
N19's metronome routing, so the new branch will stand on `engine/transport-loop-metronome`
(two deep), named explicitly in its PR. The N38 role-format placement still needs
win's migration/ADR mechanism for changing `tracks` (ADR-0144 d3); the cue backend
will accept an explicit reference track id, so that integration does not stall it.
No director question or gate has been reached.


## 2026-11-02 — N19 ready; continuing to N22

**PR requested** (ADR-0210 d4): `engine/transport-loop-metronome` at **`e42fd56`**
→ `main` (base `dff5b41`). Title: **Save and play loop and metronome controls (N19)**.

PR body: The window commits `transport.setMetronome {on}` and
`transport.setLoop {on,start?,end?}` (musical ticks), then refreshes ProjectView
and Session. Snapshot `transport` exposes metronome/loopEnabled/loopStartTicks/
loopEndTicks; missing keys default off with a four-bar 4/4 region at zero.
They remain ephemeral/audited, outside undo: saved session metadata and op rows
commit atomically. Loop toggles retain bounds, invalid windows fail, and malformed
metadata defaults safely. The audio driver applies changed loops at a callback
boundary using the tempo map; unchanged rebuilds preserve position/revision and
sub-sample windows play one frame. Original synthesized clicks follow tempo and
signature with bar accents, stop when parked, and align to actual rendered graph
latency through loop wraps (including delays longer than a loop). Both output
widths work. Clicks bypass Main devices/fader on the current default stereo pair;
N23 adds Cue Out/volume. Export queue and gain-stage offline sessions opt out of
performance controls, preserving file renders. OPS and format/session metadata
contracts are updated; see `docs/ENGINE-TRANSPORT-PERFORMANCE.md`.

**Merge with N13:** keep `mainLoudness_.process(block)` immediately after graph
processing, before `renderMetronome`; a click is a cue signal. These independent
branches both touch Session::process. Win merges/reconciles, no engine merge.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9864 checks / 107 suites**,
all validators clean. New transport/settings suite **49**, Clang Werror also;
Clang export queue197, session214 and history-snapshots65 pass. Tests cover each
snapshot field, undo exclusion, batch rollback, reopen, live toggles, changing
tempo, 3/8 grid/accent, float/double dual outputs, delayed click/loop alignment,
one-frame loops and export isolation. Compiled plants ignore click compensation
(2 failures/49) and restore saved monitoring during export (2/197). Original click
sound/level parity with Live, MSVC/JUCE and remote CI are unmeasured here.

N3/N30/N9/N13/N19 are pushed, with PR requests in this log; no matching PR head
refs found yet via Git, API remains unavailable. Continuing to **N22** from main.
N16 integration dependency remains as below; no director gate is reached.


## 2026-11-02 — N13 ready; continuing to N19

**PR requested** (ADR-0210 d4): `engine/main-loudness` at **`0e4c3cc`** → `main`
(base `dff5b41`). Title: **Meter Main loudness and true peak in real time (N13)**.

PR body: The window can load `session.mainLoudness().readout()`'s atomic floats
`momentaryLufs`, `shortTermLufs`, `integratedLufs` and `truePeakDb` with relaxed
ordering and call `requestReset()` for the next callback. Silence/unwarmed windows
are negative infinity. Session measures its actual post-Main output at native
float/double width, without changing audio. K-weighted 400 ms and 3 s windows
update at 10 Hz; integrated uses -70 LUFS / -10 LU gates and a bounded 0.1 LU
histogram (gate quantisation, not a certified EBU claim). True peak uses the
existing Kaiser-sinc design in double, 4x/2x/sample peak by rate, holding until
reset. Same-format rebuilds retain history; new formats publish fresh DSP state
and tick reclaims retired state off audio. No callback allocation, lock, I/O or
store access. Mono/stereo weights are one; standalone API accepts explicit roles
(front1, surround1.41, LFE0). Until channel roles arrive, Session treats channels
as full-range rather than guessing LFE. See `docs/ENGINE-MAIN-LOUDNESS.md`.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9858 checks / 107 suites**,
validators clean. New suite **44**, also Clang Werror; Clang session214, mixer337,
master98 and precision108 pass. An allocation counter covers processing, gates
and reset; independent complex filter-response calibration, both loudness gates,
known intersample crests, format/reset lifetime and post-Main gain are exercised.
Two compiled plants are caught: no K-weighting (11 failures) and no interpolation
(2). N9-only stale binary excluded before discovery. MSVC/JUCE/Live measurements
and remote CI unobserved; win opens/reviews/merges.

Continuing to **N19**, metronome and loop, from main. N16 remains integration
blocked as below. No director gate has been reached.


## 2026-11-02 — N9 ready; continuing to N13

**PR requested** (ADR-0210 d4): `engine/strip-meters` at **`fa09375`** → `main`
(base `dff5b41`). Title: **Publish scalar post-fader meters for every strip (N9)**.

PR body: The window can read `session.strips().stripFor(trackId)->meter()` through
a const Session and load `peak`/`rms` with relaxed ordering. Both are lock-free
linear floats per persistent strip, including groups and Main; peak is the largest
channel magnitude and RMS the mean across channel samples. No scope subscription,
SQL read, op or snapshot rebuild is needed. Values cover the current callback
(prefix while a callback is segmented), reset on its next offset-zero segment,
and clear when suspended or the driver stops. UI ballistics/holds stay with the
window. Float/double audio is measured at its native width, after gain/pan/mute,
without changing output samples. Nonfinite/oversized device output cannot put
NaN/Inf into the readout. Rebuild preparation preserves live values and meter
addresses. The earlier #205 StripMeter seam is retained, extended for current
64-bit and segmented graph processing; no UI/device-tier production edits.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9842 checks / 107 suites**,
validators clean. New meter suite **28**, also Clang Werror; broader Clang mixer
337, master98 and precision108 pass. A separately compiled plant resetting every
segment fails the two split-callback checks. Stale N3-only binaries were removed
from test discovery before this independent-main run. No MSVC/JUCE/Live parity or
remote CI success claimed; win opens/reviews/merges.

N3/N30 and N9 are pushed. Continuing to **N13**, Main LUFS/true peak, from main.


## 2026-11-02 — N30 ready; continuing to N9

**PR requested** (API unavailable; ADR-0210 d4). Head `engine/collect-project` at
`3722d72`, base **`engine/save-project`** (N3, `7cc1ffc`). Two deep; merge N3 first.
Title: **Collect current and history media into portable projects (N30)**.

PR body: The File menu can call `Store::collectAllAndSave()` and
`Store::collectAndExport(zip)` off the audio thread, alongside N3's `saveCopy` and
`saveAs`. Collection verifies and copies one file per hash into local `audio/`,
then saves relative paths in the current pool and both sides of captured media
ops on every history branch, including unlinked assets. Refresh the view/session
after in-place collection. Undo, redo and branch changes retain their media when
the folder moves. No musical edit or new history step is added. Matching existing
files are reused, different contents/symlinks refused, and copy/SQL failures remove
only newly published files and roll back all path changes. The ZIP exporter now
uses the same collector on a private SQLite backup and preserves its live source;
read-only ZIP sources and legacy extraction remain supported. SPEC §3.8 documents
the location-maintenance contract. This branch depends on N3.

Validation: GCC Debug/Werror, libpd on/JUCE off: **9897 checks / 107 suites**,
all validators clean. Collection suite **102**, project-files **44**, both pass
Clang Werror. Tests cover inactive branches, undo-only assets, relink inverses,
physical folder moves, existing-target verification, missing-media and SQL
rollback, read-only/export restrictions and source preservation. Corrected N3's
branch-selection fixture to select the abandoned branch and assert its relink path.
Two separately compiled plants were caught: skipped captured paths (9 failures)
and omitted failed-file cleanup (2). Local MSVC/JUCE/Live measurements unrun;
remote CI not observable here. Win opens/reviews/merges.

N16 remains integration-blocked as recorded below. N3/N30 are pushed; continuing
with **N9**, from current main, without waiting for PR opening.

## 2026-11-02 — N3 Save As ready; chaining into N30

### PR requested — N3

**Title:** Save project copies and continue in Save As (N3)
**Head:** `engine/save-project`, `7cc1ffc`; **base:** `main` at `dff5b41`.

`Store::saveCopy(path)` snapshots committed WAL data into a private stage, relocates media references in rows AND captured media import/relink history on all branches/inverses, folds the copy's WAL and publishes without replacing an existing file. It leaves the source unchanged and supports missing media. `Store::saveAs(path)` then switches the same Store object to the new writable database: existing OpJournal/History references keep working, writer identity/name/UUID/history/session state survive, and future edits reach only the new file. Store filename I/O now passes UTF-8 to SQLite, including Windows Unicode paths. No schema/op or UI implementation changes; SPEC §3.7 documents file-maintenance relocation and failure semantics.

**For the window:** call `document.store().saveAs(selectedPath)` on the Store owner thread after finishing SQL work; require `result.ok`, then refresh ProjectView/Session and the displayed filename. The Store object itself stays put, so OpSubmitter/History need no rebind. `saveCopy` is available for the Copy menu without switching. Calls refuse an existing destination and uncommitted transactions; Save As also refuses live SQL statements. A failed switch reports that the completed copy is retained. A held old reader can produce a nonfatal cleanup warning. An unknown newer major can only be copied verbatim, preserving read-only policy.

**Validation:** GCC Debug/Werror, libpd on/JUCE off: **9858 checks / 107 suites**, all validators clean; project-files44. Four focused Clang Werror suites pass. Actual relocated playback, live WAL, continued journal/history edits, source preservation, Unicode filename, inactive branches, relink inverses, unlink captures through two relocations, missing media, read-only/future-major copies and failure cleanup are tested. Two separately compiled plants (skip captured path relocation; switch path without switching database) each fail2 checks. UI/device/clone production untouched.

Next N30 will reuse this copy/location machinery for Collect All and Save and improve the existing ZIP export's captured-history portability. N16 remains integration-blocked as recorded above; this is not a turn stop or director question.

---

## 2026-11-02 — chaining the window needs; N16 integration dependency

The director explicitly ordered continuous chaining until the needs table is empty or a LANES §5 question blocks. Fetched main `dff5b41` and current window/log: N1 (#306), N2 (#307), N5 (#308), N11 (#309), N35 (#316), N36 (#317) and N6 (#299) are struck/merged. N37–N41 are added and will be included. API remains Forbidden; PR requests continue through this log without the director relaying.

**N16 integration blocked, not closed.** The checkout contains no ADI-Q/ADI-L native processing/response API or handover artifact, and no clone log naming one. A persisted switch alone would claim processing that is unavailable; its engaged EQ response and 5ms bypass/delay behavior need a real backend contract. For win/clones: publish the native core header/target or the supported DeviceInstance adapter, its stable parameter/state ids, EQ response query and limiter light/full mode contract. The engine's slot integration is queued against that contract. This is not a director question or a reason to stop: proceeding with N3/N30, then the following needs. The existing DeviceInstance seam is the intended adapter boundary; no clone or device production source is edited.

---

## 2026-11-01 — N11 snapshot sends ready; four concrete PR requests

### PR requested — N11

**Title:** Expose shared send-route snapshots for mixer controls (N11)
**Head:** `engine/snapshot-sends`, `5ab8833`; **base:** `main` at `6fa069a`. Independent of N1/N2 and N5.

`Snapshot::sends` contains shared immutable `SendRoute` rows: `id`, `sourceKind`, `sourceId`, `destinationKind`, `destinationId`, `order`, double `gainDb`/`pan`, `preFader`, `enabled`. Every field participates in `same()`. Disabled rows and sends into ordinary tracks/groups remain represented; endpoint kinds let the window identify unsupported rows too. Stable ordering is by routing id, not display/return order. Only changed sends are replaced; unchanged sends, tracks, clips and tempo remain shared. All capture happens off the audio thread during the normal snapshot rebuild. No schema or op change.

**For the window:** read `ProjectView::current()->sends` with the frame's ordinary snapshot reference. Filter track endpoints and match source/destination ids to tracks/returns to draw the send controls. Use the routing id with existing `routing.setGain`, `setPan`, `setEnabled`, `setPreFader` ops. `preFader=true` is Post FX before the strip; false is Post Fader before parent console encoding. A per-return selector can submit its routing-row changes as one batch. This supplies N11's missing send rows without a project-file read or another provider.

**Validation:** GCC Debug/Werror, real libpd on/JUCE off: **9690 checks / 105 suites**, all validators clean; engine119. Clang engine/project-view suites pass. The snapshot tests independently edit all nine control/endpoint fields, verify row identity/insertion/removal/filtering, disabled sends, 64-bit ids, double gain, old-snapshot immutability and exact sharing. Each of nine compiled omitted-comparison plants fails its regression.

### Current PR-request heads (not claimed merged)

| Need | Branch | Head | Requested base |
|---|---|---|---|
| N1 | `engine/project-defaults` | `8394df5` | `main` |
| N2 | `engine/project-bootstrap` | `9d7a23c` | `engine/project-defaults` (N1 first) |
| N5 | `engine/import-audio-clip` | `0bf9eb5` | `main` |
| N11 | `engine/snapshot-sends` | `5ab8833` | `main` |

The N1/N2/N5 entries below contain their complete PR titles, bodies and window instructions. N2's latest commit changes only its new test file's SPDX line; its behavior/check counts remain the validated `8daf5f0` implementation. API access still returns Forbidden; win/window open these requests under ADR-0210 d4 and relay CI. No message or approval is requested from the director. No feature PR has been merged or force-pushed by this seat.

Next is N16's three dormant slots (ADI-Q, ADI-L and sidechain) with snapshot state and ops; then N3/N30 in the window's specified order. The later export LUFS/NI work no longer takes priority over that table. The director's replacement prompt is saved in the cloud handoff; no UI, device or clone production files were changed in these four items.

---

## 2026-11-01 — N5 import-to-playing-clip ready

### PR requested — N5

**Title:** Prepare audio imports and attach playing clips atomically (N5)
**Head:** `engine/import-audio-clip`, `0bf9eb5`; **base:** `main` at `6fa069a`. Independent of N1/N2, not a third stack level.

`media::prepareImport` captures a file without committing project rows/history. Recognized audio supplies source rate/channels/decoded frames, duration, format and native WAV bit depth; generic non-audio retains nullable metadata. It checks the content hash again after capture, releases temporary cache pins, and reports known corrupt audio. Reused media ids still return `result.audio`; stale/missing paths yield a captured relink to the matching file. Existing reused rows keep their old stored metadata, while the caller receives fresh dimensions.

`clip.attachAudio {id,media,frames}` / `clip.detachAudio {id}` are lifted from `3feee62`, with source bounds checked. Attach creates an unwarped source window; detach refuses edited audio rather than losing it. Import + `clip.create` + attach can commit as one transaction; failure rolls back all three and one Undo removes them together. Deleting an imported clip also now captures its complete audio attachment in inverse `clip.create`'s optional `audio` object, including empty/NULL warp bytes, source edits and pitch/channel properties. Neither handler reads files. OPS.md documents the two new P0 ops (172 total) and captured payloads; no schema/UI production change.

**For the window:** call `prepareImport` off the audio thread and require successful `result.audio`; append its request only if `request.opType` is nonempty (it can be a relink for an existing id). Use `result.mediaId` and `result.audio->frames` in `clip.attachAudio`, explicit placement/length in `clip.create`, and submit the entire list through one OpSubmitter batch. Omit the empty request for healthy deduplication. An optional DecodeCache pointer supports the caller's configured worker cache. This replaces the N5 drop stub; playback is exercised in the engine test, not inferred from inserted rows.

**Validation:** GCC Debug/Werror, libpd on/JUCE off: **9704 checks / 105 suites**, all validators clean; media71. Four focused Clang Werror suites pass. Tests cover actual playback, single-step undo/redo with source removed, deletion/undo of all audio properties, decode metadata/pin release, moved deduplication, rejection paths and transaction rollback. Four compiled plants fail: wrong metadata (1), early commit (9), lost attachment (2), oversized window (3). Remote MSVC/seven-ABI CI remains external; API is Forbidden, so this is the ADR-0210 d4 PR request. Next: N11 snapshot sends, on a new main-based branch.

---

## 2026-11-01 — N2 bootstrap ready; N1 migration hardened

### PR requested — N2

**Title:** Bootstrap New projects at the history root (N2)
**Head:** `engine/project-bootstrap`, `8daf5f0`; **base:** `engine/project-defaults` at `8394df5`. Depends on N1's project row/schema1.9; this is the second and last level of this stack. Please merge N1 first.

`Store::createNewProject(path, error)` creates the project row, Main (id1), two MIDI/two audio tracks (ids2–5), all five mixer strips, 120 BPM and4/4 maps. All initialized state is the history root: zero ops, no Undo/Redo, persistent across reopening, undo, redo, branching and reverting to a snapshot named before any edit. The default track counts follow ADR-0209 §10 d10; CTZ is not selected. `adi_tool create` also uses the new factory. No new schema/op and no UI production edit.

**For the window:** use `Store::createNewProject` for New in place of `Store::create`; remove its subsequent Main `track.create` bootstrap and its special `protectRoot(kNewProjectLabel)` workaround. Keep normal protection/error handling for actual edits. Read the root's five tracks and rate through the ordinary ProjectView snapshot; first user edits undo normally. `Store::create` remains row-only, `createEmpty` schema-only, for callers supplying another layout.

**Validation:** GCC Debug/Werror, real libpd on/JUCE off: **9732 checks / 106 suites**, all validators clean; bootstrap24, store56, migration149. Six focused Clang Werror suites pass. The CLI creates schema1.9 with five tracks and zero ops. Compiled wrong-Main and bootstrap-as-an-op plants fail5 and7 checks. The official Live manual fetch is denied in this environment and no local copy exists: the four-track default comes from the director's explicit d10 ruling; no measured parity claim is made for the template names/order.

**N1 correction:** its latest head is now `8394df5`, also contained in N2. Incoming project foreign keys are matched case-insensitively, as SQLite matches table identifiers. The mixed-case extension regression fails without this guard. This final combined full run includes that correction; old stored rates and extension data remain protected. The earlier N1 title/body still applies with this updated head.

**PR requested via ADR-0210 d4:** GitHub API access is still Forbidden. Win/window can open these two branches directly; the director need not carry a message. Next independent branch starts from main for N5, the import-to-playing-clip path.

---

## 2026-11-01 — ADR-0210 adopted; N1 new-project metadata ready

The director's standing prompt of 1 November replaces the old order: the window's needs table is now the head of the engine queue. Read the updated LANES and ADR-0209/0210 on `win/adr-0209-ruled`, and the needs table on `window/log`. `origin/main` at `6fa069a` contains the merges of #297 through #302, including N6; none of those PRs remains queued here. New work starts from main, with at most two dependent branches. UI and device production files remain their lanes' work.

### PR requested — N1

**Title:** Initialize new-project metadata at 44.1 kHz (N1)
**Head:** `engine/project-defaults`, `b47b6ec`; **base:** `main` at `6fa069a`.

`Store::create(path, error)` now creates the singleton project row (`id=1`, `Untitled`, 44100 Hz) before any undoable edit. `project.setName` immediately changes it, undo returns to its initial name, and snapshot reads report 44100. Snapshot's missing-row fallback also uses 44100. Schema1.9 implements ADR-0203 d18's new default; migration from every frozen minor preserves stored project values, restores indexes/triggers, leaves read-only files untouched, and rolls back unsafe extension copies. The 1.8 schema is frozen byte-for-byte. The validator allows only this named default change. Converters/low-level fixtures that supply their own rows use `Store::createEmpty`; fixture calls were mechanically updated without UI production changes.

**For the window:** existing `Store::create` calls now get the project row and correct rate automatically; `project.setName` needs no workaround. The Main-track bootstrap is N2, next, so retain that workaround until N2's opt-in factory is available. No new op or UI implementation is included in N1.

**Validation:** CI-style build-only dependency fetch; GCC Debug `ADI_WERROR=ON`, real libpd on/JUCE off: **9708 checks / 105 suites**, all validators clean. Store56, migration149; seven focused Clang Werror suites pass. Compiled missing-row and wrong-rate plants both fail the new tests. Frozen1.8 SHA matches main's original schema. MSVC/seven-ABI CI remains external. API retry still returns Forbidden; ADR-0210 d4's PR-request path is used, without a director relay.

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
