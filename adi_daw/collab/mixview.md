# Mix View seat

The Focus entry and unsolo rules are confirmed by ADR-0248. ADR-0247 keeps the four channel tag fields, assigns Removed to the selected recipient and stage, and makes Align Apply one timing-offset edit. The clones seat is measuring the two additional scope references.

### 2026-11-19 — Default-active VST3 key adapter review

**For the Engine adapter owner.** The Windows Release owned-fixture gate now runs 107 checks. It reproduces the four dormant-key failures and four distinct shape failures on a VST3 whose auxiliary is already active. The SDK keeps two auxiliary channels while toggling the key intent on and off. Each toggle currently invalidates the graph's preparation stamp, and the following same-format prepare reactivates the SDK. The two native adapter decisions use raw `keyRequested_`; actual activation uses `defaultKeyEnabled_ || keyRequested_`. Please use the effective activation for the preparation stamp and prepare guard. DF-M60 (b) will then ask for intent changes without needless re-preparation. The fixture also covers programme continuity, the existing reader, and the graph identity across a duplicate route receipt. No adapter change is part of the Mix View branch.

The reopened dormant bus can take its stored intent inside the existing `Session::resolveNewRows` loan before the first rebuild. The route-edit path must record a successful fallback on the actual applied receipt, so the presentation coordinator does not rebuild it a second time.

The private Capture Locate regression has seven expected failures in 117 Release checks. The Carve stage audition control has two expected failures in 126 Release checks across 400,384 compared samples. Both are held behind the current owner and landing order.

### 2026-11-19 — Locate the repeated Windows Debug shell crash on #651

**PR requested.** The test-only checkpoint commit `5500f46598e2544c35fea0bcd797ffc8452291dc` is on the existing #651 branch. The first Windows Debug job `135741209179` and its retry `135761332025` failed with a segmentation fault on the same synthetic merge `d1e1f98b1a5d8674b7d50fa883de3838d02cab47`, after identical software render files. Neither log identified the failing fixture. A local MSVC syntax check with optimization off passed. The new hosted run records entry and return around the remaining fixtures and around the last rendered Arrangement fixture. Production sources are unchanged. If CTest omits the process exception on the next run, please collect the executable's exit status or dump on the hosted Windows runner. The existing Release gates remain 753 Mix View checks and four audio suites at `7391bbff`.

Read the new Win entry at `88e2d94e`. The Window loan covers the generation-keyed detach loop in `ProjectDocument::synchronise` and the transport Locate revision at command drain with a pass-through accessor. The detach worker and tests are staged in DF-M60's order, and the transport change follows DF-M60. Both request the Window lane's review.

### #573 Windows compiler compatibility

The failed Opto Windows job stopped in inherited `param_ops.cpp` at MSVC C4864 before reaching the Opto tests. The existing #573 branch now carries main's exact compiler fix from `85c5ab47`, preserving its author. The standalone Windows x64 Release Opto module at `8f310b710fde45be754379469fa4315230b9dcb9` passed 309 checks with zero callback allocations and frees. The hosted workflow will validate the inherited translation unit on its newer compiler.

**PR requested.** Re-review the existing #573 draft at `8f310b71` after its parent stack. The Opto runtime stays unregistered at this stage.

### DF-M60 expanded identity and dormant-key red gates

On the current review stack, the expanded Windows Release document gate ran 118 checks with 12 expected failures in Paste, Duplicate and incompatible legacy replacement. Both Browser cases passed. The owned VST3 gate ran 90 checks with four expected failures. It now distinguishes the first post-FX samples from Pre FX, preserves the complete programme comparison, and reproduces Undo and reopen. The fixtures are staged privately for the cumulative correction once #651 lands. The existing phase and monitor paths are outside these fixtures.

### DF-M25 detached analyser returns to its source in Analysers

The detached analyser's Mix View action opens the matching Analysers page with the displayed channel. It respects the preferred docked or separate workspace and selects a valid fallback if the old source disappeared. Stack keeps its filter and group when opened later. The action adds no project edit or Undo entry.

The fixture reproduced seven failures at `e38090b5`. Exact Windows x64 Release `a64cd847a1893af880c35c7c7e01cf26e3813b6e` passed 900 Mix View component checks and the three shared-analysis and Mix Functions CTests. The retest brief covers Phase, the same-channel return, excluded Stack groups, two solos, the detached workspace and the removed-source fallback. ZIP SHA-256 `46cb64c9ffaac7b397ccd186faac55b5afc7d4b0709c983cca1da56f782357d9`; ADI.exe SHA-256 `01c047d26ab19ba8f68c6344615fb4bb35bba81d7b648570c46878b3781594c9`.

**PR requested.** Include `mixview/detached-analysis-return-20261119` after Main Meter Link. The Window review is limited to the existing return callback.

### DF-M53 Main meter Link mode

The Main meter offers Link with one combined Dynamic reading and a central range bar. Peak, RMS and holds still read the individual L/R channels. The independent Link interval starts from fresh samples and shares the active physical tap. It uses the existing weighting and reset controls. The linked calculation runs only for the Link profile. The monitor control remains independent.

The measurement stub failed five intended checks. The UI stub failed five more out of 428. Exact Windows x64 Release `db9a9bb78e824fe0510212d77ee1c98cdf659953` passed 893 Mix View shell checks and all four spectrum, shared analysis, stereo meter and Mix Functions suites. The component checks cover shared tap, separate interval, untouched Stereo readings, no monitor command, and zero meter demand when hidden. The retest brief covers the reference reading and visible behavior. ZIP SHA-256 `4deb027238a940cbe55016af35b46f0b9b1b27e1dee4cc7440c00ee8dede8677`; ADI.exe SHA-256 `ff4470581bdc7d4cea7ab484469f38cd62d33dad7e8a8a44865270d566a42f1f`.

**PR requested.** Include `mixview/main-meter-link-20261119` after the spectrogram history optimization. The #651 Windows rerun remains a separate gate. Its failed job checked synthetic merge `d1e1f98b` over `bedee5ab`, whose own hosted Windows Debug shell passed.

### #651 Windows Debug shell gate

The hosted Windows Debug shell test segfaulted after its first nine software renders at `7391bbff`. The other 26 tests in that job passed, and the macOS and Linux shell gates passed. The job produced no stack or dump. Both the previous #651 UI head and the Arrangement parent passed the Windows shell. The exact Windows job is rerunning. The local 753-check Release Mix View gate covers a smaller path. The next diagnostic will mark the end of the live-parity fixture and the following shell helpers if the crash repeats.

### DF-M55 unchanged spectrogram history

A spectrogram that already shows the current publication skips the clear-prefix lookup. A new publication finds the first serial after a local clear by binary search. The source-driven comparison covered 49,923 ordered and gapped histories, with up to 128 comparisons on the earlier unchanged path and zero now. The Release software-peer gate at `e17abc0adc8939c09916f78b462b685d2aa5fa1c` passed 422 checks, including the retained history, pixels, palette and freeze regressions. These are operation counts and image checks. Live timing remains with Claude.

**PR requested.** Include `mixview/spectrogram-raster-idle-20261119` in the cumulative review after the one-bar Scope delivery.

### #548 Windows rerun

`eb5c68b8aa1b26524eb39b3a48d85d5261190d18` now has 21 successful hosted checks and a clean merge tree with current main. The exact Windows rerun passed the Pd timing fixture on the same source. Win can re-review this head for its existing dependency order while #651 remains the cumulative landing gate.

**PR requested.** Existing #548, with the serial EQ and latency/precision fixes already logged.

### DF-M25: return to Analysers with the displayed channel

The current register specifies the Analysers tab. The earlier `a2e7e931` slice deliberately restored a Stack row and its test pinned that choice. ADR-0237 d9 also asks to keep the displayed channel. Both fit by opening Analysers on that channel and the matching analyser page, while preserving the user's Stack scope and group filters for a later explicit switch. Request the `showAnalyzerWindow` return callback in `ui_floating.cpp` for this correction only. The existing safe owner, preferred docked or detached workspace and missing-source fallback can stay in that route.

### #651 reconciled with the Arrangement and CI updates

`7391bbff3204b7ead9e6a09cf34a985875824ee5` merges main `af13cf5a`. Win's validator-generated operation totals replace the previous prose total. Both operation rows survive, the validator calculates 213, and the schema and CI validators pass. The merged `.github` tree is identical to main. Exact Windows x64 Release passed 753 Mix View component checks and all four Carve, compressor, compressor host and EQ host CTests. The later export-only main update also has a clean merge tree with this head.

**PR requested.** Re-review the existing #651 at `7391bbff`. The hosted matrix is now running on this head.

### Existing review heads: #548 and #558

#558 at `fa3a1d3f` now has completed green hosted checks. It is available for win's existing dependency order. #579 `51b0aec5` and #580 `63a59729` remain clean. #548 at `eb5c68b8` passed 245 of 246 tests in its only failed job. The sole failure was the Pd lock-hold timing fixture, which observed 18.091 ms on its slowest segment against the 10 ms bound. That PR does not change the Pd engine or its fixture. Retrying that same hosted Windows job once distinguishes a repeatable failure from a single runner outlier. #651 remains first for the cumulative release.

### DF-M60(b): route activation and reopen scope for Engine review

The dormant auxiliary fixture now sends distinct Pre FX and Post FX samples through the real SDK callback and retains a complete programme comparison. It also checks that receipt drain does not activate the bus twice. The new fixture is staged for the first cumulative run. Request the one owner-side `Session::load` synchronization before its first rebuild for stored enabled keys. The active edit stays in the existing `refreshDeviceKeys` loan. Retaps can preserve preparation. The VST3 adapter currently changes its preparation epoch for a requested-bit change even if the physical auxiliary bus was already open. Engine owns that adapter distinction. It is needed to prove the unchanged-shape case for default-active VST3s.

For DF-M60(a), an older capture reader held across a same-ID incompatible legacy replacement can also delay registration of the new binding. Treat safe retirement of that in-flight producer as a ParamOps owner follow-up. The existing current-capture regression is independent and still uses the narrow Document detach-loop request above.

### DF-M60(a): precise Window loan request for retiring capture

The existing paste loan covers the new row identity. Legacy incompatible replacement at the same device ID also exposes a separate capture-lifetime problem: `ProjectDocument::synchronise` detaches retained retirees by number on each later publication, including the current device's capture at that number. Request a narrow loan for that detach loop only. `ParamOps::attachSession` already compares the current generation when it updates the binding. The extra gate will hold a replacement's gesture through an unrelated track rename, reject all three commands from the previous generation and check that the replacement still commits with one Undo. Production work remains queued after #651 lands.

### #651: Carve fixture keeps both renders under Clang

Hosted hardened Clang rejected the non-finite-input fixture's boolean bitwise operator. Both processing calls now run separately before their results are combined. The targeted Clang diagnostic and exact Windows x64 Release Carve suite pass (431 checks). Production files are identical to the previous head. Author branch head `9b87852b`, hosted checks rerunning.

**PR requested.** Same #651, with win retaining the landing order.

### 19 November — #651 local Windows gates on the reconciled head

Exact `811702260b8cb0b26462f8c943e416e327b1a17d` rebuilt under Windows x64 Release and passed `adi_mix_eq_host_tests`, `adi_mix_compressor_tests` and `adi_mix_compressor_host_tests`. These cover the previously advertised EQ latency handover and the compressor's paired non-finite-input fixture. The hosted rerun remains the landing gate.

**PR requested.** The same #651 head has the local operation/schema/CI validator and three hosted-component prerequisites ready for re-review.

### 19 November — #651 merge validator

The hosted validator for `c21e1170` saw 213 operation rows and read 212 in the headline. Each parent passes on its own. The synthetic merge combines #651's existing `routing.setKeyTap` and main's existing `lane.restore` from #662, so the shared headline also needs 86 P1 rows instead of 85. The same two failures reproduced locally after merging main.

Merged `12aab48a` into the existing #651 branch and corrected those two totals in `811702260b8cb0b26462f8c943e416e327b1a17d`. The operation, schema upgrade and CI validators passed. The only merge-resolution edit was the headline count. Win's #679 can apply its longer-term count removal independently.

**PR requested.** Re-review #651 at `81170226`. Hosted jobs are rerunning on this head.

### 19 November — #651 hardened Clang fixture fix

The hardened clang job rejected the compressor fixture's two bitwise boolean expressions under `-Wbitwise-instead-of-logical`. It stopped before CTest. The local Clang diagnostic reproduced both failures on the exact fixture. The revised test executes the subject and reference renders separately and then checks both statuses and the output. This preserves the comparison after a failed render.

`c21e1170a8c87cabe047827b8826279fd5bde473` is pushed to the existing `mixview/part-a-and-review-20261119` branch. The same Clang check passed after the change. Windows x64 Release compressor tests passed 111 checks, compared 425,984 samples and recorded zero audio allocations or frees. This is a fixture-only change. Hosted checks and the additional local host regressions are pending.

**PR requested.** Re-review the existing #651 head after hosted checks settle.

### 19 November — DF-M54 one musical bar in Focus Scope

Focus Scope now projects a complete heard bar from the actual device rate, project tempo and meter grid. The existing short preview covers the wait for a complete bar. Group columns are streamed from the existing short physical taps. The projection adds signed stereo samples before reducing them to extrema, so opposite polarity cancels. It retains early and late transients after the earlier audio has rolled out of both tap rings. The completed bar uses one grouped reader and zero FFT plans. Stop, hiding and full clipping release its demand. Timing and routing are unchanged.

Fixture-only `6bdc4ded` reproduced two failures at 48 kHz: the old view did not cover the complete 96,000 samples and lost the early transient. Exact Windows x64 Release `16e845eec9e0d31f794ed5063a5f7ea4d68e01fd` passed that component gate, bar grid and grouped reader gates, 893 Mix View shell component checks and all three targeted CTests. The retest brief covers tempo and signature changes, cancellation, seek and loop, and the visible return from the short preview. Live comparison and CPU timing remain open. ZIP SHA-256 `fcb143cbc2eef90f12dbe14b09cdd64275b40f550251af541e9e3212593d4a02`; ADI.exe SHA-256 `06b1eb6649d6055f7c8010b546e141048f3ccfcf85cdf7a5f1a00ebd78b0638e`.

**PR requested.** Include `mixview/focus-one-bar-20261119` after the Compare held-reference correction in the cumulative review.

### 19 November — DF-M55 retain Hold B through an invalid A selection

A captured B stays held if A is unavailable or temporarily points to B. Compare releases both analysis subscriptions while there is no valid pair, then subscribes only to A and waits for fresh input when a valid A returns. B's bins and reference metadata stay identical. Replacing, removing or clearing B still drops the hold.

Fixture-only `f1910297` failed four checks for the hold lifetime, inactive demand and fresh A pairing. Exact Windows x64 Release `3774e736a04249384982bc599e8764e3be125da8` passed those checks, 893 Mix View shell checks and three targeted CTests. ZIP SHA-256 `a141eae0d3bead48dca011290a16a02564e72de45f4e666cb7ccd827db685ba3`; ADI.exe SHA-256 `c10c5d289f32f17fd7c86eacb0d0656282676227ba38cca2e1611aa8920050a3`. The retest brief covers equal-source, clear, replace and remove.

**PR requested.** Include `mixview/held-reference-invalid-primary-20261119` after the exact Scope paths in the cumulative review.

### 19 November — DF-M54 exact flat Scope path reduction

The stroke builder now preserves the exact flat trace with one line instead of one vertex per column. It preserves visible endpoints, vertical extrema and every nonflat move, including very small changes. Cancellation stays on the same centerline. This uses fixed-size temporary state inside the existing projection, with no new analysis work, hidden state or audio processing.

Fixture-only `2ed17ffd` failed both path-size checks. Exact Windows x64 Release `e5176331d85b4cf782383b4203df77fbc10d8829` passed the existing software paint checks, the exact geometry oracle, 893 Mix View shell checks and three targeted CTests. ADI.exe SHA-256 `296036d732275411fbf3dd1771f53177b4d686b70b73e0d260426e83213c918d`. This internal change leaves the plot geometry and audio intact. A measured CPU gain on music remains open.

**PR requested.** Include `mixview/focus-flat-path-gate-20261119` after the Focus over-range indication in the cumulative review.

### 19 November — DF-M54 Focus Scope over-range indication

An aligned stereo sum above full scale now flags Sum over 0 dBFS in the existing Scope title, including when the numeric readout is absent in a narrow panel. Exactly full scale leaves the normal title. The plot scale remains fixed, so member traces do not move when the sum exceeds it. A valid cancelled sum clears the indication; the data path uses the peaks from the existing signed sum calculation.

Fixture-only `c72bbd39` failed the positive-left, negative-right and narrow readout checks. Exact Windows x64 Release `d6b67334eff7b3da9c3e7c7505b469a1e00be759` passed those cases plus exact unity and cancellation. It passed 893 Mix View shell component checks and three targeted CTests. ZIP SHA-256 `cd013c0874c5540261acb3886a1f683d70d4dd8abe8cd1df4a773bdf8a36cef0`; ADI.exe SHA-256 `cde8d37d53defed20ecbaf004d4b89ed2a15607236535035e8816b4aa8a70f59`. The retest brief covers threshold, channel, polarity, narrow layout and unchanged playback.

**PR requested.** Include `mixview/focus-scope-headroom-20261119` after Stack tool search in the cumulative review.

### 19 November — DF-M50/DF-M49 Stack browser searches tool names

The left Search channels and tools field now matches the names of devices and native tools on each channel, as well as the channel name. Existing group and Sounding now membership still applies. Choosing a result reveals the channel by stable identity. Typing keeps the selection and adds no Undo step. The search reads passive chain metadata on a changed query or project view and retains the matches across unchanged frames.

Fixture-only `ff664290` failed four checks covering tool matching, selection, group recovery and refresh on an update. Exact Windows x64 Release `c854f7f69c187ce469a246d42a9dd9dbaf8df2cc` passed 893 Mix View component checks and three targeted CTests. ZIP SHA-256 `3a77504f79434264103aa61a277393b2e6fc66ee4390a1daf5eda97e98798d47`; ADI.exe SHA-256 `3314e976f98929297365e9ab8b32b43135d4b4fd6d09918ab24aeb7ffd1e817e`. Claude's brief covers shortcut, matching, filter precedence, selection and rename/remove/Undo.

**PR requested.** Include `mixview/stack-tool-search-20261119` after the Focus Scope sum in the cumulative review.

### 19 November — Window loan requested for Focus Match seek validation

The Learn Match repro distinguishes queued Locate, acknowledged Locate with only old capture, and a fresh publication from just one source. The existing window input can show the same position before and after a queued same-position Locate, while both analysis publications still describe the old audio. Position and the analyzer generation cannot identify that gap.

For Win and the Window seat: please lend the command-drain block in `src/adi/ui/window_state.hpp` and one pass-through accessor in `src/juce/ui_shell.hpp`. The narrow change publishes the applied Locate revision before acknowledging the command, with no added work in empty callback drains. This blocks the stale-capture guard for Learn Match only. The two-track numeric candidate and view work stay separate from the transport guard.

### 19 November — DF-M54 Focus Scope signed stereo sum

Focus Scope draws a labeled stereo Sum overlay and keeps the individual member traces. It sums raw aligned samples before each display column is calculated, and derives the existing peak and RMS from the same scan. Antiphase cancels in that scan. The raw reading retains peaks above unity and the overlay uses the same fixed plot limits as the member traces. A missing or incompatible window suppresses the sum. The existing path owns taps and subscriptions; audio processing and routing are unchanged.

Fixture-only `e2c398e2` failed both software-paint checks for addition and cancellation. Exact Windows x64 Release `0504da1d84592ee1c674467234a0ecd01a265f9f` passed those checks, the numerical gate for time alignment and above-unity extrema, 877 Mix View shell checks and three targeted CTests. ZIP SHA-256 `57e924c2580df12fd6b52db7c3ceb88c9cc3df7826de551f1fa92487c251c00b`; ADI.exe SHA-256 `c26c865f8bdbdff8ab3582da02e7b67af7420d1b6c94e75ebf73e8045317a461`. The retest brief covers both polarities, seek and loop and the original kick-and-bass material. Live paint cost and the timing-correction feature are separate follow-ups.

**PR requested.** Include `mixview/focus-scope-sum-20261119` after the Main meter visibility gate.

### 19 November — DF-M53 hold Main meter demand to the visible plot

Moving the meter or one of its ancestors fully outside the visible plot now releases analysis immediately and restores Stereo when an audition was active. Partial clipping retains the existing reader and audition. Bringing the plot back waits for fresh data and leaves the monitor in Stereo. Normal meter and audio paths are unchanged.

The new test at `c0f452cd` reproduced the stale lease and active audition before the next frame. Exact Windows x64 Release `013f041a3cd3b10bb5f49846a31f9b9811844d6d` passed 877 Mix View checks and three targeted CTests, including own-move and ancestor-move clipping. ZIP SHA-256 `3a4051d32ae0eaad6e0c136766291c8cf97c87f10d0211f41b2aac82b4d9add3`; ADI.exe SHA-256 `7379f8e3f42c67856ddb1d0ebf1fefac7a212c37704e687e73cebb2229862d23`. The retest brief sets out the audience-visible audition result, optional geometry check and peak/hold regression.

**PR requested.** Include `mixview/main-meter-clipping-20261119` after the Compare visibility gate in the cumulative review.

### 19 November — DF-M55 clipped Compare gate

A partly clipped Compare keeps its subscriptions while the plot remains visible. Moving the plot entirely outside an ancestor now has an exact component gate: both leases go away, further audio reads and transforms stay zero while clipped, and the restored plot waits for fresh publications from both sources. The gate adds coverage to the existing visibility behavior.

Author branch `mixview/classic-compare-gates-meter-20261119` at `68d489e433acf0349e004fb2d17f78a0ac76036c`. Exact Release passed 877 Mix View component checks and three targeted CTests. This commit is test-only.

**PR requested.** Include the coverage after the Classic scope guard in the cumulative review.

### 19 November — DF-M55 Classic scope idle paint

Classic now skips the plot repaint when its visible raster geometry is unchanged. The visible scope still accepts samples so that the first edge after parked silence, as well as a seek, reaches the trigger and display. Hidden Classic releases its existing subscription. The progressive and Sync cases passed with a software peer, alongside the shared analysis and scope engine tests.

Author branch `mixview/classic-compare-gates-meter-20261119` at `ca59b24b24151012c5c220def192faaddfc7d29c`. Exact Release passed 877 Mix View component checks and three targeted CTests. This internal repaint change keeps the display appearance and audio processing unchanged.

**PR requested.** Include this commit after calibrated Compare in the cumulative review.

### 19 November — DF-M55 calibrated Compare scales

Compare now shows Wide, Detail, Extended and Device on the same axes for A and B. Its scale is independent of ordinary Spectrum. Both sides use the existing calibrated signal data; Hold B survives scale changes, and Resume and Clear wait for fresh measurements. The tests cover shared analysis and absence of demand when hidden.

Author branch `mixview/compare-calibrated-meter-20261119` at `10ea9bbbc07ac290fe9f68af66c9f271169853c3`. Windows x64 Release passed 877 Mix View component checks and both targeted CTests. ZIP SHA-256 `9e78b154c82a74707beac723eda8f5a58bf1069bcbedb782836626654fbf549e`; ADI.exe SHA-256 `ba5c901139e3909d54997a416ddbe21778fb4d7821675594e062f6248e8937f5`. The retest brief covers A/B calibration, Hold, source changes and scales. Desktop comparison and live CPU are pending.

**PR requested.** Include this commit after Device spectrum in the cumulative review.

### 19 November — DF-M55 Device spectrum

Device adds a 10 Hz to 24 kHz spectrum with a 120 dB range, a top frequency ruler and a left dBFS ruler. It uses the existing calibrated analysis and draws through the actual Nyquist limit. Freeze, averaging and hidden analysis demand use the existing paths.

Author branch `mixview/live-device-spectrum-meter-20261119` at `ef9881e861e32c33af54aa3e1dfb8be30a54af6f`. Exact Windows x64 Release passed 877 Mix View component checks and both targeted CTests. The review ZIP SHA-256 is `447726faff25aca34f9e7aca7141db5ce8a0c41ae6821491209b5c56e1669c2c`; ADI.exe is `2bff6c776873c57d225046d8fba113bcba53a29b01222c18ee204055b7ee1504`. The retest brief covers scale, calibration, Freeze and hidden demand. Desktop comparison and live CPU are pending.

**PR requested.** Include this commit in the cumulative review after #651.

## 19 November 2026 — DF-M55 Extended Spectrum scale

**PR requested for the cumulative Mix View head.** `mixview/bluprint-extended-meter-20261119` is pushed at `863c0199ea998496954d7eca18094e63cba59f01`. Extended adds a filled log-frequency display from 10 Hz to 30 kHz and 0 to −100 dBFS, in 10 dB divisions. The trace stops at the source's Nyquist frequency. Calibrated bins, FFT, time and tap are shared with the earlier scales. Freeze and a detached panel use the existing demand and capture rules. The two selected Release suites and 877 software-peer checks passed.

- Release ZIP `ADI-MixView-Spectrum-Extended-863c0199ea-win64-Release.zip`, SHA-256 `b8cf1618e8b3bdecbb62960049a0e2380f223e886440f16bd62053efdda1ce01`.
- ADI.exe SHA-256 `6a4715b0cc340d6deee0f7b82dba0a165088055f4660efa2662b186bfd3a65e1`, scanner `2aa2e222e5b3d9c900aa480cbd17f0e3f610211e166afda14c5e2e4fc8f1a992`, corresponding source `b12f4b51de0ac744a253f7a8041e5cffb7b5eebd7df4f5ba9d2fbe7eefbb34c8`.
- Claude brief `ADI-Q-MixView-Spectrum-Extended-863c0199-CLAUDE-RETEST.txt`. Reference profiles, Sync and whole-track averaging remain separate.

## 19 November 2026 — DF-M55 Spectrogram Sweep

**PR requested for the cumulative Mix View head.** `mixview/visu-sweep-meter-20261119` is pushed at `81505fb1ca02a4d5b574e0d7fa34c0259d01c3fa`, after Detail. Sweep shows the existing 128-position spectrogram history in place as its write position moves and wraps. Scroll remains available. Freeze, reset and source changes keep their distinct history behavior. Both views share the existing hop, tap and history. Headless tests cover the in-place Sweep, immutable shared history and hidden work. Both selected suites and 877 software-peer shell checks passed.

- Release ZIP `ADI-MixView-Spectrogram-Sweep-81505fb1ca-win64-Release.zip`, SHA-256 `0b2d0ac90692245a36ed8c602d204f1a8962822f067a81b6c528eb44acbeea6e`.
- ADI.exe SHA-256 `c9af47b909b174b6dd2e8abc88ada5f2e5c0505aed0ac63a39ad40cb24e6bca8`, scanner `2aa2e222e5b3d9c900aa480cbd17f0e3f610211e166afda14c5e2e4fc8f1a992`, corresponding source `75f5297f576868ff9fe03fc9c573ab488a563bdd50644e87d563e2abbff60c02`.
- Claude brief `ADI-Q-MixView-Spectrogram-Sweep-81505fb1-CLAUDE-RETEST.txt`. Reference fast and slow capture cadence remain separate work.

## 19 November 2026 — DF-M55 Detail Spectrum scale

**PR requested for the cumulative Mix View head.** The reviewed design's Detail spectrum slice is pushed at `mixview/span-detail-meter-20261119`, `3ced515d3d120d565f8fa3e23adbeb7e91583385`. Include after the meter and width gates.

The ADI Detail view shows 20 Hz to 20 kHz and −18 to −78 dBFS in 6 dB divisions, with valid curve coverage bounded by the source's Nyquist frequency. Wide and Detail keep the calibrated source, FFT, slope and measurement. Freeze retains the capture. A detached view starts with the parent scale and can choose its own. Shared-analysis and software-peer checks cover source and window sharing, the axis and slope, stopped or hidden work, and the legacy Overview restore. Both selected suites and 877 shell checks passed.

- Release ZIP `ADI-MixView-Spectrum-Detail-3ced515d3d-win64-Release.zip`, SHA-256 `a3dd15de0d379fd11a4250236fb67e37c6b9c35482cf5b24a4b2368eef9c258d`.
- ADI.exe SHA-256 `6efdec5c9a6dee5e3890fd2b5c4f1dad1ac306b8510b8cd873cb9697855357c5`, scanner `2aa2e222e5b3d9c900aa480cbd17f0e3f610211e166afda14c5e2e4fc8f1a992`, corresponding source `04aebeb80a3dfcd0f94d5e5b1345e4b56481c4666e5acd19f3169c6e7b52fadf`.
- Claude brief `ADI-Q-MixView-Spectrum-Detail-3ced515d-CLAUDE-RETEST.txt`. The side-by-side reference timing and numeric readings remain live checks.

## 19 November 2026 — DF-M60(d) auxiliary-width panel publication

**PR requested for the same cumulative Mix View head.** The tested width-only regression is pushed on `mixview/windows-meter-continuity-20261119` at `3c4dec725885d03a708d152f9d4cdd9faa050210`. The existing stack already compares both auxiliary channel count and processing issue. The new gate isolates a width change on the same runtime and checks zero hidden parameter text or value queries.

The local counterfactual using the former width comparator failed both change directions. The exact stack passed the feed, document, Mix View functions and 877 shell checks. These are test-only changes relative to the meter Release, which remains the current executable review package.

## 19 November 2026 — DF-M61 Main meter continuity and legacy Overview

**PR requested for the cumulative Mix View head after #651 lands.** The verified meter slice is pushed at `mixview/windows-meter-continuity-20261119`, `167e256c314322051b7c7257ba7ad5baeb3b35a0`. Include it in the single integration request. No separate pull request is needed.

The Main meter now preserves its interval through loop wraps and Play/Stop. A seek, source or format replacement, overrun and explicit Reset retain their existing fresh-interval behavior. TP and momentary LUFS follow the existing Main loudness service independently of the stereo publication. Changes to the undisplayed loudness values leave the readout paint idle. The historic peak remains while envelopes are still decaying after a signal. The exact visible post-signal cost is still a live gate. The pristine and freshly reset parked paths retain their zero-worker behavior.

Saved Overview ID 0 now opens Analysers → Spectrum without rewriting the stored ID. Subsequent subpage changes remain available. The optional dock returns beside Stack and the retired Spectrum surface has no demand. The CTests reproduced the continuity/readout failures before the fixes. The shell regression reproduced six old-tab failures. A diagnostic showed that a fixture had fed Main and the visible Info spectrum from one synthetic source; separating their taps verified that the two Main Spectrum views share one transform and one publication.

The exact Release passed six targeted CTests, 877 Mix View software-peer checks, and seven appearance checks. The approved design specifies Dark as default, and `53403555` matches it. The no-profile and Settings alias gates passed.

- Release ZIP `ADI-MixView-Main-Meter-Continuity-167e256c31-win64-Release.zip`, SHA-256 `bba42c413656e199b726459aac95da3cc4fcfb45722dfbf8fd81f2a2cc0ef390`.
- ADI.exe SHA-256 `4e2f189615556b26c69cca8f663258c7f3afbd286c4df8285b03b17122e4f615`, scanner `91fc524d9ab19730c162d64308dd005446a7c8b858c502c53dd4cba2f56906b9`, corresponding source `9c188b08d695a6816d5afad6b4ada93de964df4f456b5875c66566567cc4b8cc`.
- Claude brief `ADI-Q-MixView-Main-Meter-Continuity-167e256c-CLAUDE-RETEST.txt`.

In the rest of the older range, #552 is on main, as are #551, #553, #554, #556 and #557 via #578. #549 and #550 are merged work from other lanes. DF-M34's wet Limiter stays open because dither and latency changes can still produce legitimate work during a silent input.

## 19 November 2026 — Device routing and identity reproductions on the cumulative branch

The local #651 integration `0c6e1985` takes the reviewed VST3 and CLAP host sources as-is and keeps the serial EQ provider. Its Release passes the EQ, serial-backend and existing unchanged-route suites. The serial graph version of the priming fixture keeps the promised EQ wet while the host grows its compensation rings.

At fixture head `4fe8c665`, DF-M60(a) reproduced nine failures across Paste, Duplicate and explicit legacy ID replay. Browser insertion already passed with the picker’s retained-ID floor. DF-M60(b) reproduced four failures in the owned dormant-auxiliary VST3 through actual project routing and audio callbacks, including first audio after reopen. DF-M60(d)'s hidden, unchanged-runtime width-publication test passes on the cumulative branch, which already compares both sidechain width and processing issue. The queued #651 hosted checks are running. The loaned production points remain untouched before it lands. The independent Main meter gates are building now.

## 19 November 2026 — DF-M60(c) Windows Release review

The exact #651 head `8575a967` produced the Windows x64 Release review build. It passed the nine targeted CTests and all 753 software-peer shell checks on this branch. The separate cumulative assembly is local while its dependencies land. It retains the serial EQ provider and the reviewed engine-owned VST3 and CLAP sources.

- Release ZIP `ADI-MixView-EQ-Promised-Latency-8575a9676d-win64-Release.zip`, SHA-256 `427353b63458af62e6d7c90fefe5b87ffa56efaa30782b9faf3a7c537d84fb48`.
- ADI.exe SHA-256 `571004d5ee2c1d523da35f8d5ebd9cce93d7b1250612c535170b85d573352de1`, scanner `20f9683fb41adf370ee95bc785bc1a84dd085f6d7ba6c4e737524ca3a96bdeb1`, corresponding source `4c2a9f7d94b39706bb416ef4a33f5c1102a9bb08eb25830d1b08e549d213bb79`.
- Claude brief `ADI-Q-MixView-EQ-Promised-Latency-8575a967-CLAUDE-RETEST.txt`.

## 19 November 2026 — DF-M60(c), EQ latency reservation on #651

**PR requested.** #651's existing branch `mixview/part-a-and-review-20261119` is advanced from `5bad5728` to `8575a9676dc03e429c7851850ac8b4a6ae238984` and ready for re-review. The standalone EQ reserves the latency it advertises. Undo during ring growth keeps the old graph fed until the originally promised plan receives its matching physical block, then offers the return. The serial owner's explicit hold, withdrawal of an unread offer and same-physical-block pinning remain intact. One atomic wins either withdrawal or reservation and the callback takes no lock or allocation.

The fixture-only head `5f90143f` reproduced four failures. The new head passed all 130 standalone EQ checks, including the precisely timed 384-sample pulse and the actual graph ring-growth reversal. All eight targeted Release CTests passed. The Windows review bundle is building from this same head. DF-M60(a) and (b) have separately staged tests and are still open under the authorized post-landing loans.

## 19 November 2026 — Landing sequence following ADR-0248

The current landing windows can take #579 at `51b0aec5`, then #580 at `63a59729`. Their exact heads have successful checks and include the denormal and retained-disabled-row repairs. #651 follows when its advertised EQ latency regression passes on the exact branch. Follow it with one cumulative Mix View stack for the Windows session. #674 is superseded by that stack when it has the meter fixes.

The earlier analyser work in #559, #563, #565, #568, #569 and #571 is already on main through #578. #548 at `eb5c68b8` contains the precision and offline-admission fixes with targeted Release evidence. Its hosted exact-head checks are pending, so keep it in review ahead of the serial native integration. #558 at `fa3a1d3f` has the matched-filter repair and strict Release evidence. Its hosted exact-head checks are also pending.

Hold #555, #560, #561, #562, #564, #566, #567, #570, #572 and #573 outside the first Windows session stack. Their checks ran on older bases or are pending. After #558, the dependency order is #560 → #562, #561 → #564 → #567 → #570 → #573, and #566 → #572. #555 follows its compressor base. The current confirmed fixes need to be carried and gated in their eventual integration.

**For engine review after #651.** A default-active VST3 auxiliary bus appears to change its preparation epoch when key intent changes even though the bus layout stays identical. DF-M60(b) must use the key-activation loan in Session and keep unchanged layouts gapless. Please confirm whether the host can expose effective layout/intent without a refresh for a default-active bus. The dormant-bus fixture is independent of this clarification.

## 19 November 2026 — DF-M42 device status feedback

**PR requested.** `mixview/device-success-status-dfm42-20261119` at `c7b5af03baab7f9f702445d5959b63b532a84762`. Successful built-in insertion, plug-in insertion and device paste clear earlier refusals. A refused built-in insertion keeps its own message. The two targeted Release suites and 870 software-peer shell checks passed, including the actual chain-strip paste and real device-key store callbacks.

- Release ZIP `ADI-MixView-Device-Success-Status-c7b5af03ba-win64-Release.zip`, SHA-256 `54406740fcde25e4128fa1457f6f19c3affb75db42196d74fae1419221830040`.
- ADI.exe SHA-256 `014730a893316ca3f43a0f2002a34d57e09bd4f80d9ca6db85c0fff6d6b0f416`, scanner `454c0490d65e8d4267ea5f09656ecf61ba0034aaa79552e9aefc6435619fffab`, corresponding source `acc6cf4f4ec18e616508cfa36e51ff16ae3b420022493fc2a829b15a42055fd0`.
- Claude brief `ADI-Q-MixView-Device-Success-Status-c7b5af03-CLAUDE-RETEST.txt`.

Following ADR-0248, the next landing gate is the promised EQ latency repair on #651. The additional analyzer features wait behind the confirmed review findings.

## 19 November 2026 — DF-M39 stale Add menu feedback

**PR requested.** `mixview/add-refused-dfm39-20261119` at `b8c0c5e932ea0b20a66c0f873b379a40abe7be51` after DF-M38's tested ancestry. A stale toolbar, row picker or Device View Add choice says “This Add menu is out of date. Reopen it and try again.” The targeted component tests cover a real intervening rename, stale filtered selection, dismissal, the fresh action, and the absence of stale edits and Undo. Release also passed 870 shell checks.

- ZIP `outputs/ADI-MixView-Stale-Add-Status-b8c0c5e932-win64-Release.zip`, SHA-256 `cb2a4b031a3642b416304f8e9fcb3725c79f1d292104dbf38ed146145b57ddd8`.
- ADI.exe SHA-256 `0fafcf58bcfda7b3df3e56bd5c86311dc359bedd2c3e0e85ce69f9c095e46741`, scanner `454c0490d65e8d4267ea5f09656ecf61ba0034aaa79552e9aefc6435619fffab`, corresponding source `25e405a40fb6d5d24e83bf8d2a15f585f49ee4cb8c7c9759d0054a0c50e59834`.
- Claude brief `outputs/ADI-Q-MixView-Stale-Add-Status-b8c0c5e9-CLAUDE-RETEST.txt`.

DF-M40 and DF-M41 already passed the 17 November live route/panel retest on the published native-serial ancestry. The retained/unavailable key case was separately checked live in that round and has a device-key route test. These still await their merge into main.

### DF-M37 — Clean callback recovery

**PR requested.** `mixview/clean-callback-recovery-20261119` at `a2ff97e5065c4ca5e0c7000100dc22a23a5158b4`.

The float and double adapters preserve dry input at the callback's actual offset before preparation and after release. They fill silence when there is no input. A refused or incomplete checked block resets the core after the dry copy so the next segment can recover. The live prepared path still uses its existing capacity check.

The regression at `ec8819af` failed the four float/double callback cases. The exact Release passed all 104 native Clean host checks with zero callback allocations or frees, the three targeted CTest suites, and 870 shell checks. Release ADI.exe SHA-256 `069f4f64f19608d64e9fafc8181a63c262f4b5ed00d5fdfb8303083291bec75d`. Evidence is in the private seat archive. This defensive path has no new review package.

### DF-M38 — Independent Clean and real Stack control gates

**PR requested.** `mixview/clean-envelope-gates-20261119` at `50c42c77cbef978adadb28f5d5bcf45d57171892`.

The Release gates now test analytic attack and release landmarks, a settled quiet signal and re-trigger, and a separately calculated Auto Gain value. The authored-chain fixture checks recovered ordering, settings, and project reopen. The Stack fixture drives the actual folded dot and checks authored membership across Undo. The compressor suite passed 141 checks over 425,984 compared samples with zero callback allocations or frees. All three targeted CTest suites and all 870 Mix View shell checks passed. This is a test-only branch.

## 19 November 2026 — DF-M36 unchanged-format prepare keeps audio admitted

**PR requested.** `mixview/native-noop-prepare-20261119` is pushed at `bf221a35`. Exact-format and sufficient-capacity prepares return before taking the reactivation Hold in native EQ, Clean, and the built-in host. The EQ still updates the advertised read-only latency. Real format changes retain the guarded prepare path.

The fixture-only `8542a7d3` failed the reentrant audio-admission check. Release passed 875 EQ host checks and both float/double duplicate-prepare renders were sample-identical. It also passed 870 software-peer checks and five CTests, including the serial EQ and live device-callback key regressions. Evidence is in the private evidence.

- ZIP `outputs/ADI-MixView-Native-Noop-Prepare-bf221a35c8-win64-Release.zip`, SHA-256 `eeba7fa76b736a678db26f5f9ac5efb851071881566fadc6db4dc346d1131d30`.
- ADI.exe SHA-256 `3c33dff3798aee9df47007b7ed7327eb54c9312e0da01151d7a0ada26ee43035`, scanner SHA-256 `3b5c90a5f8c4168ed31abdf87167174e991bb166a8b236b3e1524844a69dd0c3`, corresponding source SHA-256 `fbb69f386c4fb19b29de2e604a3aea0c10a37e9328732ce87151b552d002666b`.
- Claude brief `outputs/ADI-Q-MixView-Native-Noop-Prepare-bf221a35-CLAUDE-RETEST.txt`.

## 19 November 2026 — DF-M35 Clean make-up and wake Release

**PR requested.** `mixview/clean-makeup-wake-20261119` is pushed at `36fc0109`. Ordinary make-up edits use a continuous linear-amplitude ramp, including reversals. The steady loop has no ramp branch. Explicit Auto Gain toggles and reset use the current target. The first valid wake segment adopts current authored settings before resetting state, so a parked Threshold edit is present on its first sample.

The test-only `21bab98b` failed precisely the first-sample wake case. Release passed 98 Clean host checks with zero callback allocations or frees, all three selected CTests, and 870 software-peer checks. The exact Release core also passed all nine private matched stereo replay pairs. The matched scripts, hypotheses and numerical evidence stay in the private evidence; the app build gates are in the private evidence.

- ZIP `outputs/ADI-MixView-Clean-Makeup-36fc010926-win64-Release.zip`, SHA-256 `33fc1b2415a51af40781a86cce862f5753acc99fb01a8cb6ce9b34ce57e0b103`.
- ADI.exe SHA-256 `8dabc29abcbebcb11e264d5d44b159304a16a24f56dac801d9c430d7844d3d90`, scanner SHA-256 `8ad088dc002b43e06c5ae20f8e62ab3a7e1eeb66818ff8cc30e248ae12944492`, corresponding source SHA-256 `bf5bec6b1132a84b9707e8da75738550a4a37842b1d6f2966b4f64dc00b1ed69`.
- Claude brief `outputs/ADI-Q-MixView-Clean-Makeup-36fc0109-CLAUDE-RETEST.txt`.

## 19 November 2026 — DF-M34 native Clean silence and wake

**PR requested.** `mixview/clean-suspension-20261119` is pushed at `6a71f323` after the native serial EQ assembly. Clean runs a bounded 48-second quiet release, clears its output once, and resets on the audio owner before the first real sample. The Release passed 870 software-peer checks, three CTests and 93 Clean host checks with zero callback allocations and frees. A short silent gap matches uninterrupted processing sample for sample. A rested node wakes at the fresh waveform and matches the continuously running fixture within 1e-7 peak.

DF-M34's EQ and Limiter parts stay open. EQ's only current full reset re-enters prepare and can allocate. Both need a wet-quiet wake path for structural requests that preserves the pinned PDC epoch. Limiter dither also generates genuine output on silent input, so that mode needs continuous processing. The exact source audit is in the private evidence.

- ZIP `outputs/ADI-MixView-Clean-Suspension-6a71f323ba-win64-Release.zip`, SHA-256 `ea3fe1c4f55b6b93038741cdf5689c78bf7c2188d77b3c358319a9a9606ae011`.
- ADI.exe SHA-256 `67388c85a87e66cfd5595eb8ab6af64325f9d0bc440e8b335c3a235b0736d71b`, scanner SHA-256 `d4a9a59b59a3f7c0831367e5906b3a2674508a1f470e1e2864e16322a4ac54d8`, corresponding source SHA-256 `58468169c093f0c2221f15a0f9181088cc38c061314136d442dc15b73640db9d`.
- Claude brief `outputs/ADI-Q-MixView-Clean-Suspension-6a71f323-CLAUDE-RETEST.txt`. Raw evidence is in the private evidence.

## 19 November 2026 — DF-M29–31, native serial EQ Release and #548 re-review

**PR requested.** #548 `mixview/eq-serial-native-20261113` is fast-forwarded from `770bf1b0` to `eb5c68b8` for re-review. Its merge tree is clean against current main. The cumulative `mixview/native-serial-eq-assembly-20261119` at `f7fab268` is pushed and includes the key route and paired EQ surfaces after DF-M26. It preserves active EQ through float → double → float at the same stream format, renders the requested plan deterministically offline, and tells users to reopen the project after a parked failure. Rolling real-time plan publication still uses the existing safe owner/ready-plan path.

The Release passed 870 software-peer checks and eight targeted CTests. Native serial EQ passed 72 checks with 248,832 compared sample pairs and no audio allocations or frees. Device keys passed 200 checks; a real DeviceCore fixture reduced the recipient's 1 kHz programme by over 10 dB, Undo restored the unrouted level, Redo restored reduction and a silent routed key settled to the same level as the absent route. All logs are in the private evidence. DF-M32 and DF-M33 remain covered by the current #579 and #580 heads.

- ZIP `outputs/ADI-MixView-Native-Serial-EQ-f7fab268b5-win64-Release.zip`, SHA-256 `7a1443f4e3f50e8b7baafef5672bd4e807cfd3ef47adb69537dc40e14d2892f4`.
- ADI.exe SHA-256 `10806afff72afd47e76aec2a44f8e7b7c33650ee24d60884209d4c19f96efbfa`, scanner SHA-256 `042ee50ce5987e841d065814b67bea611da2a2d6211b67b970c3cbdd2b55b2dd`, corresponding source SHA-256 `2e81b79a4e2e10430c230efe3227e8adae82d62196bca7ccaf0ecb89d5f71ac3`.
- Claude brief `outputs/ADI-Q-MixView-Native-Serial-EQ-f7fab268-CLAUDE-RETEST.txt` includes the passing EQ setup, three key taps and the Limiter control's detector-threshold effect.

## 19 November 2026 — DF-M26 actual Stack chains without reserved slots

**PR requested.** `mixview/no-reserved-slots-1119` is pushed at `6679713e` after DF-M25. Snapshot projection builds real device-chain items directly, with no previous reserve-and-clear for hidden EQ, Ducking or Limiter slots. Active device IDs, retained gestures, channel hierarchy and order still use the actual chain. Model consumers can still supply their own explicit generic items.

The red fixture `2c00f17d` failed the first-refresh allocation check with pre-reserved real storage. The Release passed all 632 model checks and 870 software-peer checks. Release ADI.exe SHA-256 `da522e8b85246753ecfe0f7e55816093191ad4f1b562c93c19fba9d64eadb4de`. This is an internal projection repair and uses the previous visual review brief. Logs are in the private evidence. DF-M27 remains closed by ADR-0240.

## 19 November 2026 — DF-M25 return from the detached analyser

**PR requested.** `mixview/detached-analyser-return-1119` is pushed at `a2e7e931` after DF-M24. The analyser's Mix View action opens Stack on the channel whose data is displayed, even when normal entry with two solos would open Focus. It retains the analyser and its current mode and keeps explicit Stack group filters. A missing source falls back to a valid current channel. The action writes no project Undo step.

The fixture-only `55f87833` failed all three return cases. The Release passed 870 software-peer checks and both targeted CTests. Private evidence is in the private evidence.

- ZIP `outputs/ADI-MixView-Detached-Analyser-Return-a2e7e93193-win64-Release.zip`, SHA-256 `f6af7dd9c4c1feb4e02ab98dd5401c49408db45d7d66945bd4adbdfbe2d0eb35`.
- ADI.exe SHA-256 `2c82ce097fa1520f00f84fe4949bb7ac4a862a68f1486e115cef0a47e0a9b75e`, scanner SHA-256 `2a369abb2af66c7b3c08ff36fb5c3ced882c9fef7d993b51fd682553b4c9a38f`, corresponding source SHA-256 `e8fa123a25376323c664d2bd57cdc118597f4af1e58a8296d4800255be56c26e`.
- Claude brief `outputs/ADI-Q-MixView-Detached-Analyser-Return-a2e7e931-CLAUDE-RETEST.txt`.

## 19 November 2026 — DF-M28 Classic Gate key contract

**PR requested.** Existing #572 `mixview/g-complete-request-20261114` is fast-forwarded to `7d3bd050` for re-review, after #566. The added core gate proves that CompleteRequests can retain a single prepared state for internal programme aliases, routed silence and real external keys, including a retarget and removal without reprepare. Fixed internal detection and programme aliases are bit-identical at 44.1/48/96/192 kHz in float and double. The Release suite passed 371 checks and compared 2,670,592 samples with zero callback allocations and frees. The readme now states the generated host-rate admission through 768 kHz.

The future native adapter will take realized key identity from the callback route and feed this existing API. The test introduces no registration or new DSP, so there is no app package. Gate and de-esser registration still follow the preset and key sequence in DF-M28. Evidence stays in the private evidence.

## 19 November 2026 — DF-M24 Compare, Average and triggered Scope Release

**PR requested.** Land the complete `mixview/triggered-scope-surface-20261119` at `95a67a31` after DF-M19. This tested head includes the broker and Compare/Average slices. Their pushed `bf22f99d` and `44ba613f` heads mark review subdivisions; the final fixture fixes are in `95a67a31`. Compare chooses A and B explicitly, fences live transport disagreements and can retain B while releasing its live demand. The one-second exponential Average shares the raw spectrum's transform. Dedicated and detached Scope use the licensed s(M)exoscope face and controls and the continuous broker capture. All keeps its compact latest-window Scope. The same source shares a physical tap, and hiding or freezing releases the view's live lease.

Release passed 857 software-peer checks and the four CTests for shared spectrum presentation, analysis, comparison and triggered scope. Initial fixtures exposed the test stack reserve, reading pixels before the graphics scope finished, and an insufficiently primed exponential average. The rerun uses heap-owned root fixtures, reads completed software rasters, and primes the average before testing its slow response to a level drop. The product commit's audio path was unchanged by those fixture fixes. The live brief includes source timing, stop and hide transitions, and reference comparisons.

- ZIP `outputs/ADI-MixView-Compare-Average-Scope-95a67a3158-win64-Release.zip`, SHA-256 `8e756cae4565c02423cbf5b8bad0a5fc4c015643f1071c66193872761d6a9e85`.
- ADI.exe SHA-256 `28346c75d034833f9f31b2778777a95ae83cca5d24949f068776a28e244ee18a`, scanner SHA-256 `2a369abb2af66c7b3c08ff36fb5c3ced882c9fef7d993b51fd682553b4c9a38f`, corresponding source SHA-256 `d448247596584e514e566cd16f7a1ece609bb0fcc822edd1d420ef2a8c9e93a1`.
- Claude brief `outputs/ADI-Q-MixView-Compare-Average-Scope-95a67a31-CLAUDE-RETEST.txt`. Raw build and gate evidence stays in the private evidence.

## 19 November 2026 — DF-M19 spectrum slope and adjacent rows

**PR requested.** `mixview/slope-fanout-dfm19-20261119` is pushed at `7b5daa0c` after `c425fc49`. Overview, the attached dock and the detail spectrum consume the same stored slope. The detached pane remains independent after opening. A fake-peer fixture proved that one slope change adds zero taps, reads, worker passes, transforms and publications. The next hop does one transform and all three projections still share the immutable window. The first fixture exceeded the Windows test executable's stack reserve; heap-owning the fixture pane fixed that without changing the product or its linker stack. Release passed 857 software-peer checks and three selected CTests.

- Release ZIP `outputs/ADI-MixView-Spectrum-Slope-7b5daa0cd8-win64-Release.zip`, SHA-256 `c9449b283e05a10aa86f708cce99c52eefa5b1f31be27b7ed4b534ce31939b60`.
- ADI.exe SHA-256 `b4e2bcf4d70d018964d9395f1fa06cecbe2979e7aab62a9379fbf772362d2724`, scanner SHA-256 `f923bcfee3ad5ffce0a163e330f9ddc125d835403cd1da7cd183cb584861c8a7`. Corresponding source SHA-256 `38b9e220c3d704e5e911ae7b400fafda28d838047abd9e3f7399e9b8f4c115ae`. Claude brief `outputs/ADI-Q-MixView-Spectrum-Slope-7b5daa0c-CLAUDE-RETEST.txt`.
- DF-M21/22 are already exercised in this cumulative build through #580 and B3. Remove/recover/Undo for a native activator, ordinary bypass, and plus-to-add on the chosen actual chain retain their existing gates. There is no duplicate branch. DF-M23 is parked on Win's storage, file-format and chain-placement ADR. The director's replacement decision is settled in ADR-0240 and the private preset prototype stays off the review branch.
- Strict Release follow-up on DF-M57 and #555 exact Hold ancestry `ce8690d0`: compressor (158), Clean key (236), Carve and Focus all pass. The active-Hold case checks fallout and reset under finite and invalid input. This is additive gate evidence for the existing M57 and Hold branches.

## 19 November 2026 — DF-M16–18, strict Release repro of dynamics and key filters

**PR requested.** The existing #558 and #573 author branches were fast-forwarded to `fa3a1d3f` and `4a5478ea`. Land #558 first; re-review these exact heads. #573 follows #570. Independently assembled source `95844115` passed all seven Release CTests with warnings-as-errors (2,659 checks, zero callback/process allocations and frees). This includes the matched sections, compressor, de-esser, gate, key EQ, Clean key, and Opto. The Opto test includes its fast-math translation unit. The private evidence directory is the private records.

- DF-M16 rejects or zeroes non-finite input at the existing input boundary according to each core's contract. The separate #555 Hold integration still gets its own gate.
- DF-M17 tests 176.4, 192, 384, and 768 kHz admission for the affected cores. The prior native comparison does not establish parity at all those rates.
- DF-M18 preserves the filter policy. In 16 whole-array native comparisons at 44.1/48/96 kHz the high-order near-Nyquist behavior passed the 1e-6 peak and 1e-7 RMS gates. The reference also reaches about −10.42 dB on the 20 kHz BandPass at 44.1 kHz. No production coefficient rewrite and no new CPU percentage. Unregistered components receive no separate review app.

## 19 November 2026 — DF-M15, native limiter latency while bypassed

**PR requested.** `mixview/limiter-bypass-repair-dfm15-20261119` at `c425fc49`, after `b0f1dcc2`. A bypassed native Limiter can finish a latency transition against the exact compensation plan pinned by the graph. The graph keeps the compensated dry signal audible and the last audible reduction reading intact. An admitted child fade can finish while a newer offer waits for its own PDC. Private scratch work ends when that transition settles.

The earlier `f8c503d3` backend failed the three child-fade assertions. The Release passed 1,093 limiter-chain checks, 76 document and direct-graph checks, the graph and Mix View function suites, and 854 Mix View software-peer checks. The direct fixture holds one real prepared Full or Lite device and verifies the old plan holds the claim and the new pinned plan releases it. The document fixture separately verifies dry output and Undo/Redo with a saved Gain row.

For Win's engine register: a first raw `device.setParam` with no previous row yields a null inverse. Undo removed that row but left the running Gain at the edited value in the initial fixture. That failure is separate from a latency claim. The final limiter test seeds an explicit Gain value to exercise the documented value-to-value Undo path. Archived the original and corrected logs privately.

Review `ADI-MixView-Limiter-Bypass-20261119-c425fc49b1-win64-Release`.
- App SHA-256 `7f4200f2680b170c8f19801a371cbb2f8883d61a278bb145c2cb622f5865d0d3`
- ZIP SHA-256 `f2014b2b80b725fccea4028497c73da28296c2e3e2609557c7d3a9bafabd3f49`
- Scanner SHA-256 `f923bcfee3ad5ffce0a163e330f9ddc125d835403cd1da7cd183cb584861c8a7`
- Brief `ADI-Q-MixView-Limiter-Bypass-20261119-c425fc49-CLAUDE-RETEST.txt`


## 19 November 2026 — landing checkpoint for the held PRs

Ready for Win to land in this order: #579 `51b0aec5`, then #580 `63a59729`. Each published head has 21/21 hosted checks. The histories merge cleanly against the tested main pin `4e11edd9`, and both are in the gated Main Integration Release. The quiet-envelope and legacy off-device recovery checks are already in those heads. Keep later tail, prepare and control follow-ups separate.

#555 `2a39ffb1` has 21/21 and merges cleanly against that pin. Its Hold path is the base for the subsequent shared compressor sanitation. Use the reconciled DF-M57 candidate when integrating both paths. For the reviewed filter stack, take #558 first, then the #560 → #562, #561 → #564 → #567, and #566 → #572 branches. #570 follows #567 and the Hold work. #573 follows #570. #558 needs the test-registration reconciliation and #573 needs its module-target fix before approval of their new exact heads. #559, #563, #565, #568, #569 and #571 all have their reviewed commits in main.

#548 is held for its reconciled serial EQ and actual routed-key gates. The new private candidate preserves both media-width and deterministic offline gates and exercises one serial EQ key through the document's callback path. Native Carve and Focus IIR have separate candidate children. They are queued for builds after the older rows. No private preset work was imported.

DF-M15's fail-first build is active next. Main `1a46048d` adds register rows for other seats and leaves the Mix View rows unchanged.


## 19 November 2026 — DF-M13, schedule Focus FIR work across each hop

**PR requested.** `mixview/focus-fir-pacing-20261119` at `b0f1dcc2`, after `ead20df4`. The negative gate at `8c67dd2f` reproduced two scheduling failures at 192 kHz / 512 with one and ten enabled nodes. The paced version passed all 424 Focus assertions and the four adjoining key, EQ chain, backend and pipeline suites. The Focus oracle reads 3,639,758 compared samples with zero callback allocations or frees. At 192 kHz the reference comparison is exact in float and within `1.12e-16` peak in double.

Per-hop work follows the actually active node masks and lands within the fixed source deadline. The calculated peak work at 192 kHz / 512 drops from 348,672 to 53,257 counted operations for one node. Ten nodes still require up to 348,196. These are work counts. The 2.667 ms wall-time gate remains open for a quiet matched run.

Release test SHA-256 `cfa6b0826515b4928869f607a2181459f3752cb667b2f3357b83d343f65ed938`. Red and green logs are sealed in the private the private evidence archive folder. The Release app hash is still `8cbf49097b2d0adb707405f82c2fc9cf91da73ae367096904bf0fffbbbe59b16`. The serial Focus path is not exposed in this app. The live FIR brief can be used when that path is available.


## 19 November 2026 — DF-M11 and main integration

**PR requested.** `mixview/stack-main-integration-20261119` at `ead20df4`, after Stack persistence `8bb87bc3`, brings in main `4e11edd9`. The integration retains the schema 1.22 device-key path and its prepared PDC handover, combines it with arriving-audio transport for hosted plug-ins, and preserves the shared control sources, appearance settings and analyser refresh. This is the current forward base for subsequent Mix View work.

Release gates: 854 software-peer checks and all 12 targeted gates, including actual DeviceBridge, device keys, native EQ pipeline, VST3 key and CLAP fixtures, graph processing, window state, settings and shared analysis. The older 17 November sidechain retest accepted the routing fix. The review package lets Claude verify the combined build and an audible CLAP key. Win and the engine seat still own route/schema and host review. DF-M12 is settled in the decision guide: Focus recalls consistent settings without hidden edit history. DF-M14 stays covered by the earlier deadline-miss regression.

Review `ADI-MixView-Main-Integration-20261119-ead20df440-win64-Release`.
- App SHA-256 `8cbf49097b2d0adb707405f82c2fc9cf91da73ae367096904bf0fffbbbe59b16`
- ZIP SHA-256 `6d96f249472e62382eef512cb27f6517fcdc5e7adbcb949337a364046c7787ae`
- Scanner SHA-256 `5a297bf023a757a1f2ad3a3facad1999da9ff4e164d88219b37a87517dc14c24`
- Brief `ADI-Q-MixView-Main-Integration-20261119-ead20df4-CLAUDE-RETEST.txt`


## 19 November 2026 — DF-M10, Stack state across project reopen

**PR requested.** `mixview/stack-view-state-20261119` at `8bb87bc3`, stacked on `2691319d`. Each window remembers search and base row height. Pins belong to the project and updates from separate windows preserve each other's choices. A restored channel keeps its pin after Undo. Presentation preferences create no project operation or Undo entry.

The Windows x64 Release passed 854 software-peer checks and all three window-state, Stack-model and Mix View function suites. The legacy fixture now supplies the established `docked` field so the gate can isolate invalid optional Stack fields; its other window geometry survives.

Review `ADI-MixView-Stack-View-State-20261119-8bb87bc3ce-win64-Release`.
- App SHA-256 `796425c81692e74a62bc8d527af38b579ec93525570a8b9c15c8fffd4816cb72`
- ZIP SHA-256 `abd55388f2cae064ee0718f1e257c362b50fab42dd23e566832f030e7ed3d939`
- Scanner SHA-256 `f04c1fa640f537360825bc6907d3da4e051c9c788736a94d52bba7979ef5213e`
- Brief `ADI-Q-MixView-Stack-View-State-20261119-8bb87bc3-CLAUDE-RETEST.txt`


## 19 November 2026 — DF-M4, Main analysis after device format changes

**PR requested.** `mixview/device-analyzer-rate-regression-20261119` at `2691319d`, after Appearance `e44487b8`. The new headless DeviceBridge gate passed 208 checks in the Mix View functions suite. It changes the sample rate from 48 to 44.1 kHz at 256 samples, then stops and starts with the same configuration. Main Scope, Spectrogram and the stereo meter keep their leases and use one physical tap. Old results are invalidated before the first new window, and the retained frozen snapshots stay intact.

The gate exercises the format-change refresh already present in the review stack. It changes only tests, so the Appearance package remains the review app. Changing the system device and verifying the live picture stay with Claude.


## 19 November 2026 — Part B step 8, Appearance

**PR requested.** `mixview/appearance-review-20261119` at `e44487b8`, stacked after `2102d4e9`. Settings exposes Dark, Mid Grey, Light and Mix View, with Dark and Compact as the defaults and Open as the other density. Existing `os` theme values resolve to Dark. An open Mix View applies the theme and its density roles when any held edit releases. A frozen spectrogram keeps its columns and source state when recoloured. The default swatch reflects the theme when the palette menu next opens.

Release gates: 836 Mix View software-peer checks and all six targeted settings, window-state, analysis and Mix View suites. The explicit appearance gate uses the software peer. Win owns the registry landing; this candidate uses `lookfeel.theme` and `lookfeel.density`. Please reconcile those entries with the central registry change once.

Review `ADI-MixView-Appearance-20261119-e44487b89d-win64-Release`.
- App SHA-256 `6fc806fef48e14544d1212ee507d2b9a2b5f0420f662573382c6e7996ee48c94`
- ZIP SHA-256 `430c85b202722e1ced233b22f462290e87300c03cdd59fba09529bde53a29a20`
- Scanner SHA-256 `c5d9521fec525afe6639daeb56c9fe53af6ef756e1081b7de95155c105706a9f`
- Brief `ADI-Q-MixView-Appearance-20261119-e44487b8-CLAUDE-RETEST.txt`

Register intake after fetching main `4e11edd9`: DF-M1 remains with Claude. DF-M2 is recorded in the private `DECISION-GUIDE.md`. DF-M3 has a private matched-acceptance sheet and a regenerated plan with 23 profiles, four blanks and three repetitions. New quiet timings remain deferred. Next is the real DeviceBridge format-change gate for DF-M4.


## 19 November 2026 — Part B step 7, Focus workspace

**PR requested.** `mixview/focus-review-20261119` at `2102d4e9`, stacked after `f79a0dc4`. Two or more solos open Focus from Arrangement. Unsolo removes the member and promotes the first remaining solo if Main left. Focus keeps stable member identities and uses their live chains. Hear A, B, C and All use track solos and leaving keeps them. Together and Scope use shared, sample-stamped views, with the live stereo sum restricted to valid common windows.

When a loop repeats the same audio timestamp, individual traces continue. The combined scope and mask wait for an observed stop and fresh windows after restart. This is a conservative safeguard until the taps carry a shared loop iteration. Closing or hiding the hosting window releases only Focus's leases. Native Unmask apply and Match remain separate work. The Removed and Align actions wait on the two recorded director choices.

Release gates: 834 Mix View software-peer checks and six targeted suites (masking, shared analysis, shared spectrum, Stack model, window state and Mix View functions). The tests cover entry and unsolo policy, detach and reparent release, identical-time summing, cancellation, seek and loop boundaries. See the brief for live layout and comparison.

Review `ADI-MixView-Focus-Workspace-20261119-2102d4e967-win64-Release`.
- App SHA-256 `2ae7bdc9b6e1b61796accaa1b61184d7428e7f279eff35543a5e796c5151bf49`
- ZIP SHA-256 `ea01a929a2e32aadffdfd08c7272f695f7fe0c09e635520cd9ee2825c212cac8`
- Scanner SHA-256 `96ea249cb7dfe3038fc099986e2e808ec3171386223976ab01f469f03a6478e6`
- Brief `ADI-Q-MixView-Focus-Workspace-20261119-2102d4e9-CLAUDE-RETEST.txt`


## 19 November 2026 — Part B step 6, Analysers page and shared meter data

**PR requested.** `mixview/analysers-review-20261119` at `f79a0dc4`, stacked after `3603a1c5`. All and individual Spectrum, Spectrogram, Scope, Phase and Loudness views use Main, the selected channel or a fixed stable ID. The Analysis table subscribes to metering only and reads clipping counts from the shared meter calculation. Source choices refresh on actual project view changes, including rename and removal at the initial generation. Hiding the table or its window releases its meter leases. The Mix View Info preview reads the shared spectrum and spectrogram presentation settings through its own display lease.

Release gates: 834 Mix View software-peer checks and all three targeted suites (shared analysis, shared spectrum and Mix View functions). The new regressions cover source refresh at generation zero, meter-only clip counts and the visible Info preview. Live side-by-side checks are in the brief.

Review `ADI-MixView-Analysers-20261119-f79a0dc4b6-win64-Release`.
- App SHA-256 `1912ad49e54db1a7cd92b7513f211ba5c89e583fc8f5986ff81201ff7c4f9fe8`
- ZIP SHA-256 `01ad7a7da832543824faf6577e9ae8d7f0a0890d403a71da30782f6d6e506866`
- Scanner SHA-256 `04d6e4b6dd166bb977c0b2cc2a6256626f88d6ed8fa3112c13ddf0570a1f4855`
- Brief `ADI-Q-MixView-Analysers-20261119-f79a0dc4-CLAUDE-RETEST.txt`


## 19 November 2026 — design step 5 Main meter Release

**PR requested.** Author branch `mixview/main-meter-review-20261119`, head `3603a1c5`, after Sounding Now `0016e5b0`. The right-hand Main meter reads the shared post-fader feed with balance, correlation, Peak, Dynamic and RMS, holds, weighting, scale, Float and Reset. L/R/M/S and Flat/A/C/K Filter Listen are temporary physical Main monitor modes. The headless audio tests verify that the Main tap, export and Cue stay independent. Closing or hiding the owner restores Stereo.

Five Release suites and 833 software-peer checks passed. The new idle regression failed on the unparked baseline and now records no extra worker passes, cursor reads or publications for an empty meter across 1,100 silent blocks. It verifies live monitoring wakes on the first audible block, peak decay still works and a same-position seek takes a fresh epoch.

Package `ADI-MixView-Main-Meter-20261119-3603a1c5df-win64-Release.zip`. ZIP SHA-256 `d9fc1b730cb81a6825fb3dbc0e1a4e71d479501d6d7e6b94cf54f6cb55686a20`. ADI.exe SHA-256 `8adf332e9b598ab3e33aa92b0e27aaae079d54985101fbe00a1c4297404d82cc`. Scanner SHA-256 `53f00cde88f4a3e69a3bb1c324df87b927d3642d69ff4599aa51052fe583dc62`. Brief `ADI-Q-MixView-Main-Meter-20261119-3603a1c5-CLAUDE-RETEST.txt`. Exact reference readouts, hardware routing and live CPU await Claude.

## 19 November 2026 — design step 4 Sounding Now Release

**PR requested.** Author branch `mixview/sounding-now-review-20261119`, head `0016e5b0`, after row picker `d1913d09`. Sounding Now snapshots the channel IDs with a post-fader reading at or above −60 dBFS. I used that threshold to match the visible meter floor. The list and Stack keep that snapshot through level changes and metadata updates until Refresh or a new filter. The filter changes neither playback nor Undo and adds no audio taps.

Two targeted Release suites and 828 software-peer component checks passed. Package `ADI-MixView-Sounding-Now-20261119-0016e5b0b4-win64-Release.zip`. ZIP SHA-256 `25d67fca5065959020ffa2f174752f649d25dbf98a011891e1ec9529392f70e3`. ADI.exe SHA-256 `e9da12cbf1c5eeb52b4bae7b7911ffd339ba15f2885668b7d307c4d855f2cee5`. Scanner SHA-256 `6a0becb071631d746a4bc0b98fb3c7e51dacac02d849b2d9a3df532488bc4e0a`. Brief `ADI-Q-MixView-Sounding-Now-20261119-0016e5b0-CLAUDE-RETEST.txt`. The next slice is the permanent Main meter and monitor-only audition.

## 19 November 2026 — design step 3 row picker Release

**PR requested.** Author branch `mixview/stack-picker-clean-20261119`, head `d1913d09`, after Stack cards `3d4904ae`. Each chain has a searchable add control with existing native functions shown as selected and unavailable for duplicate insertion. It uses the common device edit for compatible built-ins, VST3 and CLAP and opens external editors from named chips. A late picker cannot redirect a choice to another row. New inserts reserve past retained runtime IDs, including after removing the highest native function.

Four targeted Release suites and 817 software-peer checks passed. The headless document gate opens a fake external runtime, verifies its path, parameters and stable identity, and exercises one Undo and Redo. Package `ADI-MixView-Row-Picker-20261119-d1913d09cc-win64-Release.zip`. ZIP SHA-256 `f369b8ec9034fc30423cd27037e2010f1f7b3869fda84361e41fbd2c5a8bf0d6`. ADI.exe SHA-256 `28bb46efa00ed00a4ebb73dd519c052e6e7e5817838f3b5809eba27922953fcb`. Scanner SHA-256 `6a0becb071631d746a4bc0b98fb3c7e51dacac02d849b2d9a3df532488bc4e0a`. Brief `ADI-Q-MixView-Row-Picker-20261119-d1913d09-CLAUDE-RETEST.txt`. Installed VST3/CLAP editor behavior and scanning stay in the live gate.

## 19 November 2026 — design step 2 Stack lens and cards Release

**PR requested.** Author branch `mixview/stack-cards-clean-20261119`, head `3d4904ae`, after workspace `460ecdbf`. The Stack uses the sheet's continuous row lens and a Rows control. Channel names, colour, peak/RMS, Mute and Solo stay at the left. Native cards show the EQ curve, the visible device's post-EQ spectrum, a paired bell drag with one Undo, and real, demand-gated Limiter reduction. Compact folded pills can activate, unfold and remove the exact device through existing authority. The default Stack shows all channels. Third-party named chips and per-row insertion are the next design step.

Nine targeted Release suites and 788 software-peer checks passed. The limiter retirement gate exercises revoke, Undo on the original runtime with a fresh telemetry lease, Redo and replacement. The package is `ADI-MixView-Stack-Cards-20261119-3d4904ae4e-win64-Release.zip`. ZIP SHA-256 `98b0c89d4df6f9352c4390d100040bde14c500f6af607c2f99e56d14eaa98501`. ADI.exe SHA-256 `79e7db9693fc555734c33cd6fc678f13221636ddffa405b312b7c122fbff1a92`. Scanner SHA-256 `f02b53217c98a9a9c88d7efeb83eaf4fc5d9dce7a6df35082f7371a441181a66`. Brief `ADI-Q-MixView-Stack-Cards-20261119-3d4904ae-CLAUDE-RETEST.txt`. Live geometry, drag feel and stopped CPU still await Claude. The Limiter history reports actual callbacks. Other dynamics-card displays still need their specific contracts.

## 19 November 2026 — design step 1 workspace Release

**PR requested.** Author branch `mixview/workspace-20261119`, head `460ecdbf`, review after `01c0bc1e`. Arrangement and Mix View switch the whole body through one command. The song strip reads the existing timeline and seeks via the transport owner. The searchable project channel browser and stable group filter own the left column with a demand-driven Info preview. Opening Analysers retains the source and Stack selection. A docked Mix View releases the hidden Device View's parameter demand. Detach, minimize, restore and re-dock have separate headless checks.

Two standard suites passed and the Mix View software-peer gate passed 782 checks. Package `ADI-MixView-Workspace-20261119-460ecdbffb-win64-Release.zip`. ZIP SHA-256 `c7e575f4843c34b8be157475d307e43e66f733c0ed72ccdb4bf8d5dfa2a8684f`. ADI.exe SHA-256 `6ad2fc3f771d94b22851f6c7cca36b9d9ed0b6f518f67cd895010fe19a245b6c`. Scanner SHA-256 `b0d5935dd14e9321472e8922973399c23df331a569115538d75bc405d5fa66f4`. Brief `ADI-Q-MixView-Workspace-20261119-460ecdbf-CLAUDE-RETEST.txt`. Focus, right meter and the full Analysers page have separate slices.

Fetched new main `a0a31168` after #641. The Stack lens and native cards candidate incorporates it and is compiling. It includes the expanded Limiter's measured reduction through a lease held only while its display is visible. The prepared insertion-picker and Sounding Now slices follow it.

## 19 November 2026 — engine review fixes in Release

**PR requested.** Author branch `mixview/part-a-and-review-20261119`, head `01c0bc1e`, review after Part A `f353e8f1`. DF-M56 preserves aliased keys through Clip and clears reset routing state. DF-M57 keeps invalid input out of the dynamics detectors. DF-M58 recovers a missed Focus FIR deadline and withdraws superseded EQ and Focus plans. DF-M59 keeps spectrogram hops when the history is full, parks a blocked reader retry and keeps loudness maxima consistent with their readings.

The fail-first regressions were exercised on the private integration baseline. Eleven targeted suites pass on this clean cumulative x64 Release, including the actual limiter and serial key paths. Package `ADI-MixView-Engine-Review-Fixes-20261119-01c0bc1e9e-win64-Release.zip`. ZIP SHA-256 `5f414f470a3ed55bc69b744f65c20a504adf738a04458d5780f91e627ac1238a`. ADI.exe SHA-256 `2409bb0c136c974277a3a60ed69ae185ea7018023d446b8f155769ca8f2b41c7`. Scanner SHA-256 `e79f5aec7127876143afffb11d4e72948900b29478f405ee83dcdc2639b4d82d`. Brief `ADI-Q-MixView-Engine-Review-Fixes-20261119-01c0bc1e-CLAUDE-RETEST.txt`.

Up next is the song strip and the full Mix View workspace. The Stack lens, cards and insertion picker are prepared on separate review slices. In the held PR queue, local repairs for #548, #558 and #573 are staged and still require exact-head gates. The #548 serial EQ key follow-up is needed to retain DF-M44 after merging the serial adapter.

## 19 November 2026 — Part A Release and paired EQ evidence

**PR requested.** Author branch `mixview/part-a-20261119`, head `f353e8f1`, based on `ae09f638` plus #579/#580 and the idle-recovery branch. Review after #579, #580 and idle recovery are available on main. DF-M43 allocates new native function IDs above retained runtimes, DF-M44 makes a missing external EQ key silent, DF-M45 preserves compensated programme through a prepared key-route edit, and DF-M46 persists settled reported latency as derived project state. The folded pill belongs to the new Stack; an installed CLAP's audible key remains a live case.

Ten targeted Release suites passed, covering device-key route lifecycle and VST3 aux planes, native EQ, controls, parameter captures, derived latency and stopped analysis. Schema 1.22 and all upgrades from 1.0–1.21 passed, as did the ops validator. The package is `ADI-MixView-Part-A-20261119-f353e8f1c2-win64-Release.zip`. ZIP SHA-256 `e176278a7b9d85fe73220c7162918ac08eb8f9a6817f703b2c6f8c919d9909c3`. ADI.exe SHA-256 `a2a98641ac787b42a4dd58fbf1004f13286d33efb345d6592410c19b3c90a66e`. Scanner SHA-256 `d9cce59ea6bffc082e0662976a0f75ec7d9238e925ee9faaba7d305ce46a6118`. Brief `ADI-Q-MixView-Part-A-20261119-f353e8f1-CLAUDE-RETEST.txt`.

The paired EQ gesture has a private integration package, source `34c7a7aa`. Four Release suites and 795 Mix View software-peer checks passed. Preview and rollback deliver the pair in one settings update. One Undo restores both values, with normal host replay at the next owner tick. A click at float readback writes no history, and a refused journal write restores both values. ZIP SHA-256 `b181d101aece511ae83e6b8f79ca737fdeb502790d90ddd01d5830c112428411`. ADI.exe SHA-256 `8ae46cde0296b1864be6535b2583ca8cf6891ba2622e08e51c7ae2570c7c911a`. Brief `ADI-Q-MixView-Paired-EQ-20261119-34c7a7aa-CLAUDE-RETEST.txt`. Source review comes with the clean Stack card branch. Continue with standalone engine review fixes before the design stages.

## 19 November 2026 — standalone idle recovery Release

**PR requested.** Base `ae09f638`, author branch `mixview/idle-recovery-20261119`, head `edf89820`. This branch is directly reviewable against main. It retains equivalent control snapshots, parks settled silent analysis behind its demand and transport/source checks, and updates spectrogram images in software memory. Analyst workers remain demand-driven. The headless counter gate checks that settled, certified idle does not create new analysis hops or publications. Three targeted Release suites passed (parameter feed, shared analysis and shared spectrum).

The separate Windows x64 Release review package is `ADI-MixView-Idle-Recovery-Main-20261119-edf89820e0-win64-Release.zip`. ZIP SHA-256 `74fba57f30163fceaa6b6b58799313f47f0e789c67f74bd52d245bb8d3a7a718`. ADI.exe SHA-256 `a254d0a144fed2e0d4b4a1518ad5cc8d877a47a2f526475c589571d9c71135cb`. Scanner SHA-256 `a68810def49b7a7bb85a1c51122c838dc86df07ca70e43a4e8c7a99bf5d3a07a`. Claude brief `ADI-Q-MixView-Idle-Recovery-Main-20261119-edf89820-CLAUDE-RETEST.txt`. Use a compatible main-schema project. The stopped-project responsiveness verdict remains with Claude.

The Part A device-key candidate is now in Release build and headless gates. Its routing prerequisite is the prepared native host/owner work in #579 and #580.

## 19 November 2026 — Release handoffs and clean publication queue

The 18 November responsiveness and review-fix packages are available for Claude. Source `4e5c6dfd` now also has a sealed x64 Release package for Focus entry and unsolo, the Stack EQ spectrum and A/C/K meter weighting. The build passed 795 software-peer checks. ZIP SHA-256 `6ed5362d1f3252e7465d9f5402d760ba43501db6cf73d22e971487d3a9c227e7`. ADI.exe SHA-256 `49fe04c88c0792566d4b47cd39282eb36193af07fc0a3a72bb29205727d5fb8f`. Brief `ADI-Q-MixView-Focus-Policy-20261119-4e5c6dfd-CLAUDE-RETEST.txt`. Claude's original responsiveness gate is still the first live item.

The integration build has the private Mix Set prototype. Source publishing starts from clean main and ports the tested owned changes in review order. The storage/format/placement ADR still belongs to Win under DF-M23. DF-M43 and the paired EQ gesture are being prepared as separate author slices. The four-job Release build cap applies to this seat.



## 19 November 2026 — Focus entry and unsolo confirmed

`4e5c6dfd` opens Focus on two or more soloed channels from Arrangement, and opens Stack with zero or one. An explicit Focus unsolo removes the member. When Main leaves, the first remaining soloed member becomes Main. Leaving Focus preserves existing solos. Windows Release rebuilt and passed 795 software-peer checks. Claude's existing package remains `a15680be`.

## 18 November 2026 evening — DF-M56–59 and approved appearance in Release review

Windows x64 Release review commit `a15680bee316d8fd046fd42f276949e6d9a43bf1` includes the idle repairs and Part A fixes plus the fail-first regressions for aliased limiter keys and reset, non-finite dynamics inputs, overdue Focus FIR work and reverted latency plans, full-budget spectrogram history, and limiter loudness maxima. It uses the shared appearance tables for the Mix View, with Dark and Compact as defaults. No routing cables were added. Grouped returns will use ordinary rows when the engine publishes them.

All 18 targeted Release suites passed. The Mix View software-peer fixture passed 786 checks. Native-window and audible-device verdicts remain with Claude. The private review ledger carries the archive and exact retest. Archive SHA-256 `28f3fd14b8bfec7702476f19d057ba40dc073d6f4ce182f341fcab1cf74674ba`. ADI.exe SHA-256 `045058e522ae9b451b6da8af21d6d002aad66b60f75259b8fe46f6034d4be3ab`. This candidate leaves the open PR landing order below intact.

The next source slice is `086eadb0`. Expanded EQ readouts follow the band, and visible Stack cards share their post-device analysis with compatible subscribers. The Main meter has passive A/C/K weighting. The individual response, shared-demand, software-rendering, geometry and release checks passed. Claude still has the sealed `a15680be` package. Focus entry and unsolo decisions are outstanding under DF-W27.

## 18 November 2026 evening — Release review after stopped-project overload

The first live round on fd0eb9f2 was blocked on Arrangement. The x64 Release package is 6e3fe84c. It coalesces certified parked-silent analysis, retains equivalent parameter publications and keeps spectrogram raster edits in software memory. Six Release suites and 785 software-peer checks passed. The stopped 48 kHz / 480-sample project probe measured 10.575 ms median Debug and 2.119 ms Release. That probe excluded the physical driver, windowing, vendor editors and analyzers. Claude owns the live responsiveness gate and the reopened DF-M43–46 cases.

**Landing.** Win can re-review #579 at 51b0aec5, then #580 at 63a59729. Both exact heads have green checks and that merge sequence is clean against main 313d1c98. #555 is independently green and its base is already on main. Land it after those two with both foundation document changes retained. #558 needs its CMake conflict resolved before its independently green child sequences #560 → #562, #561 → #564 → #567 and #566 → #572. #570 follows #555 and #567. #548 needs refreshed checks and conflict resolution even though its branch has M29–31 fixes. #573 is held for the Opto test target and CI failure. #559, #563, #565, #568, #569 and #571 are already on main.

The release archive, hashes, measurements, retest and detailed PR audit are in the private Mix View seat ledger. This entry supplies a tested build and the landing sequence. The remaining DF-M56–59 findings will have separate regression evidence.

## 16 November 2026 — private presets/device-key candidate sealed, headless gates green

Combined source `5aa82cc6cc0931eb08d2dd53864440344e092fbb` is clean and builds under strict MSVC x64 Debug/Release, JUCE 9.0.5. Private native-preset and device-key author slices remain separate from public merge; exact corresponding source is bundled for review. No new PR or self-merge. #579/#580 review corrections remain on their existing pushed branches.

**Proof:** final Debug ten suites 1,139 checks/0; explicit Mix View software/fake-peer shell 753/0; six optimized Release suites 436/0. Schema 1.22/ops validators pass, including historical schema upgrades. Key graph callback allocation/free guards 0/0; native Clean 0/0. Owned VST3 fixture checks complete stereo arrays, double-only low bits, segment bounds and unchanged primary in-place pointers with auxiliary delivery/removal. Restoring the main SDK in-place fast path avoids imposing an unnecessary plug-in-side copy. Scanner rejection in Release is literal 0 allocation; MSVC checked-Debug's empty returned-vector proxy overhead is measured separately (3/3, no additional rejection work). These are not live acceptance or CPU timing.

**Behavior:** whole native Mix Set replacement and exact single-function recall use one sealed common Undo transaction; ordinary plug-ins remain. Device key source/tap/None use actual channel taps and compensated device endpoints, declared CLAP/VST3 auxiliaries and owner-only activation. Stale source menus refuse. None can recover an unavailable stored key without offering fictitious new inputs or being blocked by another unavailable key. Independent device copy deliberately strips the identity-bound removal image, so cloning/Undo never steals or duplicates the original key. Native processing algorithm/coefficient tree is unchanged from reviewed base `63a59729`; no per-device idle timer/worker/poll was added. Clean C remains internal-key-only.

**Private review delivery:** the private records and `outputs/ADI-Q-MixView-Presets-Keys-20261116-CLAUDE-RETEST.txt`. ZIP `ADI-MixView-Presets-Keys-20261116-5aa82cc6-win64-Release.zip`, SHA-256 `abaa69a1af84068d3aa93db85cec5e8c9231916c47efbc1433bf99a4045764e8`; app `ac3067982ee397178873ce176da38131e9c03de696f021768c782050526ba2ce`. New portable folder/profile, matching source archive, licenses and manifest verified; normal install/original projects untouched, app not launched by this seat. Live desktop/editor/host comparisons stay with Claude; CPU benchmarking remains explicitly deferred by the director. Full cross-platform CI remains separate; independent #548 EQ repair is not included.

**Win's pending policy:** ADR-0240 d3 still assigns preset home/encoding/placement among ordinary plug-ins to win's ADR from the already posted #584 proposal. This package labels that carrier/placement as private review, not a shipped public format. Whole-set replacement/one Undo is settled and implemented; no director question was reopened. Public carrier rollout waits for that ADR and ordinary code review.

## 16 November 2026 — preset proofs green; real device-key owner/host path under combined validation

DF-M23 author `mixview/native-presets-20261116` at `69f03e20` remains private pending ADR-0240 d3's storage/encoding/ordinary-device placement ADR. Five targeted suites passed and the Mix View software-peer shell passed 753/0 before route integration. This is not a released file format. The complete proposal is already on #584; the director's replace-whole-set / one Undo policy is implemented and is not reopened.

DF-M11 author `mixview/device-key-20261116` (private validation tree) now includes schema 1.22, durable tap choices, causal device/source deletion and Undo, actual device-target graph keys, prepared VST3 main/aux bus tables and declared CLAP port roles. Owner menus use source channel + Pre FX/Post FX/Post Mixer or None with exact runtime guards. Replacement/removal is one sealed transaction, with same-head material guards. Candidate topology admission uses the same Graph topological sort without preparing/touching SDK state before commit. Clean C remains internal-key-only. Channels without keys gain no key DSP node/worker/poll.

The first data/graph slice plus presets passed nine targeted CTests, including every historical schema upgrade through 1.22. Extended host/owner/component tests and strict combined app compilation are running single-job from an isolated frozen tree. Two strict-build issues (duplicate shape-epoch override and a shadowed menu variable) were corrected; no final readiness, live or CPU timing claim yet. Owned fixture observations use the exact bundle module, not a different build copy. No native desktop/host/editor launch, capture/input or new CPU benchmark; Adi's benchmark deferral remains in force. No new PR or self-merge.

## 16 November 2026 — reviewed repairs green; presets under component validation; DF-M11 starts

#579 is repaired at `51b0aec5`, now based on main; #580 is repaired at `63a59729`. Same branches pushed for re-review. Exact #580 Debug app and four integration suites pass; 753/0 fake-peer Mix View checks pass. No self-merge or local desktop/host launch. Long-silence numerical/allocation checks are not CPU timing.

DF-M23 private implementation now covers bounded native-only carriers, atomic file publication, actual-loader preflight, full-set replacement and isolated function recall through one sealed common batch. The first 39 model/file checks and the actual native EQ/L document suite pass, including first-touch Undo/Redo, invalid-state refusal and file-independent project recall. It remains private pending win's storage/format/placement ADR (#584 proposal comment); no format is declared shipped. Candidate carrier is versioned JSON `adi.native-presets` v1, function/mixset scopes, stable native UID, compatibility `enabled`, normalized authored controls and hex state-role bytes. Matching functions retain identity/manual placement, missing use the existing root tail, omitted natives disappear; ordinary plug-ins remain. Additive owner-only `DeviceInstance::stateRecallReady()` allows the native L adapter to refuse compound recall before commit while an earlier configuration is pending; no callback poll. This small cross-lane API hunk is claimed for review.

DF-M11 / DEV-05 / MX-24 is next per the director now #512 merged. Claim schema minor **1.22** (`routing_key_taps`, absent = Post Mixer), routing-tap ops/cascade Undo, planner/realizer/session device-key endpoints and the VST3/CLAP first auxiliary-bus plumbing/tests. N-bus remains deferred. Win: please keep N38 tracks.role on the subsequent minor or coordinate a combined bump before authoring; no edits to main's dirty checkout. This work uses compensated graph edges, not analyzer taps. Native Clean remains internal-key until its external-key engine adapter is separately enabled. CPU timing remains deferred; sound preservation and callback allocation tests continue, live testing exclusively Claude.

## 16 November 2026 — preset continuation and immediate review repairs

The director explicitly authorizes Mix Set/single-function presets next, then device keys now that #512 merged. Current main is `a1f1ad38` (ADR-0240 records the replacement ruling; #552 and #581 have landed). Before layering the preset UI, repair the merge blockers on existing #579/#580. #579 is rebased onto current main and adds scoped callback denormal protection plus a generated180-second quiet regression: native84/0, callback allocation/free0/0 already passes; full rebuilt app/integration gate is still completing. #580 will recover already-disabled native records with setEnabled(true), preserving old projects, control-client and Bounce compatibility; disabling an active native function remains remove/Undo. Actual folded-dot and stronger authored-membership tests are in preparation. No sound-law rewrite, native capture or new CPU timing.

Claim new engine-owned preset model/preparation paths `src/adi/mix_presets.*`, focused tests and necessary document/control integration. Whole replacement uses ONE common journal transaction; single-function load isolates its exact function; host/state preflight precedes durable writes. Parser/file catalogue work remains separate from audio. The concrete storage/encoding/placement proposal is on #584 for win's ADR; unapproved file/placement policy is not shipped by this claim. Old Clean93921caa review package remains immutable while these reviewed fixes are tested.

DF-M11 follows: please retain/confirm schema minor1.22 ownership for the assigned device key routes while N38's tracks.role work proceeds. This seat will audit current route/planner/realizer APIs first, not use analyzer taps as audio keys. No device-key route implementation or new preset binary is claimed yet.

## 16 November 2026 — director settles Mix Set replacement; #512 fence lifted

**DF-M23, the director's exact ruling:** “Replace the whole native function set, as one undo step. To change one function and keep the rest, the user loads that function’s own preset.” Ordinary plug-ins remain untouched. This supersedes the earlier merge recommendation and pending merge-versus-replace question. A Mix Set replaces the complete native set; a single-function preset is the partial-load path. One Undo must recover the former set/settings/order, and Redo the complete replacement. Preflight/refusal must not partially mutate the channel; use the common prepared operation/undo authority.

The private decision guide and storage/application proposal are updated. Win: please incorporate this settled behavior into the DF-M23 storage/file-format ADR, including placement around ordinary plug-ins. Proposed writable home remains `appdata::pathsFor(App::Daw).config / "Presets" / "MixView"`, with function and channel-set folders. No public format or preset implementation is declared delivered by this log.

Fetched main is now `c7ef0363`: #512 merged as `729892e2`, so the earlier operation-file fence is lifted. DF-M11 can proceed after auditing current routing/schema ownership. This does not change the sealed Clean/#580 package or its hashes. Native audio/CPU contracts and Claude-only live testing remain unchanged.

## 16 November 2026 — optimized Clean review package sealed; idle metering fixed

The optimized application built from #58093921caa is sealed with matching source/notices and a concise Claude brief. App SHA-256 `bf9a059156af0c0300a33778da2227da620d63d3eb89724d57a3cfab1c657107`, ZIP SHA-256 `aee6b0c6b6c3bb9e85d1ba005132148e58575515ed996971561eaa4cd707fc6e`. Four Debug integration suites,753/0 fake-peer Mix View checks and five Release integration suites pass; the compressor library is byte-identical before/after #580. No install replaced and no application, scanner, OS window or audio device launched. Package/brief/receipt are recorded in the director's current implementation ledger.

DF-M20 is #581 `1d6eab72`, directly on main. Recording-off now skips unused loudness filter/history work; active/paused metering semantics and audio processing remain. Five strict Debug integration suites/four validators pass.194560 stereo frames remain byte-identical against the always-recording engine; a causal previous-source test fails the demanded-history resume check and the candidate passes5/0. This is an unused-work removal proof, not a new CPU percentage; timing remains deferred. The sealed Clean package deliberately excludes this independent later branch.

The preset merge/replace question is now pending with the director. DF-M11 routing still needs win to clear the #512 operation-file fence. No route/schema/preset-deletion implementation is inferred. All supplied PRs await review/merge; live tests trail with Claude.

## 16 November 2026 — Clean and Mix View controls supplied; next efficiency follow-up

#579 `e782005b` is ready for review, not draft. Its optimized application and five selected Release suites pass. #580 `93921caa`, based on #579, supplies DF-M21/22: + Function inside STACK targets its selected channel independently of Device View; retained menu ownership/selection/snapshot guards refuse stale actions; native switches share remove/Undo while ordinary plug-ins keep bypass. Four strict Debug integration suites pass and the explicit fake-peer shell passes753/0, including unchanged-frame zero allocation. Optimized UI-overlay packaging is underway. No local OS window, audio device, desktop input/capture or new CPU timing.

DF-M23 proposed storage is `appdata::pathsFor(App::Daw).config / "Presets" / "MixView"`, with function and channel-set folders, bounded versioned files and atomic common Undo application. Projects keep applied authored state independently of the file. No transient DSP/runtime/analyzer state or numeric external-key routing IDs belong in presets. Merge-versus-replace remains an explicit director choice; win writes the ADR before a public schema. No deletion policy is assumed.

DF-M11 remains blocked on the #512 fence: the route schema/operation work needs `ops*`/changesets. Please clear that ownership boundary before implementation; no fenced edits have been made. While it waits, claim the independent DF-M20 limiter idle-loudness path (`mix/limiter/DspEngine.cpp`, header contract and focused limiter tests) to skip unused measurement when recording is off, preserving active metering and output. New CPU timing remains deferred; show causal state/work tests and exact audio, not a speedup percentage. Source authoring is isolated from the frozen Clean package build.

## 16 November 2026 — repaired core contracts; reachable Clean supplied

DF-M16/17/18 fixes are pushed to the existing #558/#560/#561/#564/#566/#567/#570/#572/#573 branches and their dependents. The filter primitive now targets main independently; DS/G base and extended tiers are separately verified. Thirteen strict Debug component suites pass. DS112 saved renders and both Gate25-case sets retain byte-identical output; sixteen new key-EQ edge comparisons pass unchanged strict gates. The high-order edge response is shared by the measured reference, so no decramping/redesign was substituted. Higher host rates through768kHz are functionally admitted without claiming reference parity at unmeasured rates. No CPU timing was collected.

DF-M28 is draft #579, `e782005b`, based on #552 (two slices from main): an addable internal-key Clean native device with eight working controls, numeric automation IDs, canonical defaults/display times, state, common gestures/Undo, real tab/STACK membership and zero latency. Strict Debug app build, four integration suites, native80 checks with callback allocation/free0/0, software-peer51 checks and fake-peer-only Mix View753 checks pass, plus all four validators. Additional bounded Clean default/auto-control audio comparisons pass strict gates. Optimized packaging is underway on this frozen source; live acceptance stays with Claude and win reviews/merges.

The DF-M23 preset storage/application proposal is delivered privately for win's ADR, keeping merge-versus-replace unresolved rather than deleting by assumption. While the frozen optimized build runs, claim the subsequent ordered DF-M21/22 presentation paths (`ui_stack.*`, `ui_devices.*` and focused software-peer tests): one native remove/Undo meaning and shared explicit-target add menus inside Mix View. Use existing prepared operations; fenced operation/schema files remain untouched. No preset schema or destructive preset-load policy is implemented by this claim.

Director answers still awaiting ADR coverage: With Tools default, optional right dock, independent Clip→Limiter; separate EQ/Carve/Focus pages in that independent audio order with manual Apply; consistent Focus recall; Still ordinary; Versatile seed retained. No new multi-channel bulk-edit law is inferred.

## 16 November 2026 — dynamics repair ownership and reachable Clean device

The director explicitly requests continuous work through DF-M16/17/18 and then the addable internal-key Clean compressor DF-M28; stop only for a required decision or attention-worthy issue. Claim the existing `mix/filters`, `key_eq`, `deesser`, `gate`, `opto`, `clean_key` and necessary `compressor` source/test/rate-admission paths for those repairs. Preserve finite-input numerical paths and all saved-output gates; add nonfinite recovery and high-rate coverage without claiming reference parity at unmeasured rates. After repair proof, the Clean provider owns its new native adapter/common menu/state/undo integration, with no external keys or missing-style controls. Fenced operation files remain untouched. Local integration combines held component tips solely for verification; it is not a new public PR or a claim those fixes have landed. Existing PR fixes will be carried at their owning boundaries. CPU timing remains deferred; live tests stay with Claude.

## 15 November 2026 — native EQ review repairs pushed

#548 `770bf1b0` now targets main and addresses DF-M29/30/31. Width handoff forces stopped prepare only on actual precision changes. Export-created EQs use a serialized Offline owner without a worker, completing available configuration before physical-block admission; live processing retains its threaded parked owner and allocation-free path. Recovery text names remove/Undo instead of a nonexistent reload command. Strict Debug build, seven integration suites, 667 explicit fake-peer Mix View checks and four validators pass; the extended native suite passes72 checks,248,832 sample pairs and live callback allocation/free0/0. Fresh offline whole/split replays are exact with different Main-pump schedules. This is component verification, not a full app/export live check or new CPU claim. The PR includes a short acceptance brief; win re-reviews/merges.

#578 has since landed at main `ac5050bd`. Next work remains DF-M16/17/18 before registering the internal-key Clean compressor DF-M28; no new inaccessible-core refinement is substituted for that device. No other core repair or new native compressor is claimed in this checkpoint. No new PR, self-merge, fenced edit or live desktop test.

## 15 November 2026 — reachable devices next; native EQ review first

Director answers awaiting ADR coverage: With tools is the default channel scope; the analyzer right dock is optional. Clip and Limiter have independent enables in Clip → Limiter order. EQ/Carve/Focus use separate pages, independent enables in EQ → Carve → Focus order, and suggestions require explicit application. Focus recall uses current settings rather than hidden edit history. Still remains an ordinary plug-in. Versatile retains its per-instance seed for repeatable recall. The reference sketch is a starting point, not an all-expanded layout requirement. Multi-channel relative/absolute edit behavior is not settled.

Following the continuation brief, repair held #548 on current main first: DF-M29 precision handoff, DF-M30 deterministic offline plan service, and DF-M31 actionable failure recovery text. Claim `src/juce/mix_eq_host.cpp`, `mix_function_host.cpp`, `src/adi/mix/eq/Backend.*`, `src/adi/mix/eq_chain/Backend.*`, their focused tests/docs. Offline-only serialized owner work must never enter the live callback path. No fenced operation or schema edit. Next are DF-M16/17/18, then the addable internal-key Clean compressor DF-M28 before further inaccessible refinements. #578 has not landed at the fetched main pin `58b2d096`; do not fork another analyzer feature stack ahead of it. Live acceptance stays with the director/Claude; win reviews and merges.

## 14 November 2026 — review repairs complete on the existing stacks

The director's repair wave is pushed to the existing PRs; no new PR or self-merge. #541 `cc4b59da` is ready for win's re-review and its complete hosted core/UI matrix passes. Latency-changing controls are configuration-only, limiters share an event-driven parked owner, and claimed structural conflicts refuse before values change. The same open PR carries the follow-ups to merged #537/#540: exact new-gesture cleanup on an older refused commit, stale/refused Delete/Backspace guards, audio-applied seek revisions, bounded stopped tap storage, and the offline gain-staging consumer correction. The engine-owned paths remain subject to win and the returning engine seat's review; no fenced operations, schema or routing change was added.

#542 `8eae2b1c` gates heavy parameter publication by visible demand and caches native EQ metadata; unsupported Carve/Focus tabs stay absent. #543 `50632cc2` corrects mono audition/continuity with frozen stereo preservation checks. #544 `b2f7b071` counts actual successful enqueue operations. #545 `6ccdd061` distributes FIR work across source samples and uses the actual sample rate, with strict output/deadline/allocation checks. Its separate phase transform is not included in the FIR quota; this is not a full callback CPU-parity claim. #546 `c8772063` tests real parked-owner activity, including causal no-wait and timed-poll failures, and retains cached page selection through hidden demand.

All 31 dependent existing heads are rebased and pushed, preserving the originally published follow-up fixes and the two explicit dependency merges. Reentrant publication getters are guarded on #554. The final analyzer test fixture now proves the With tools default excludes generic plug-ins without requesting their parameters, then chooses All channels for the main/floating visibility matrix; the production default is unchanged. Final strict Debug validation: native-EQ end #548 `6fb411a9`, 2,034 checks; analyzer end #574 `4c1c39c6`, 2,961 checks (13 selected suites plus 753 explicit null-native-peer Mix View checks). All four repository validators pass; selected source hashes are stable across validation. The full native shell test path was not run.

Later exact-head hosted CI still trails in its queue and remains a merge gate. Original drafts #555/#573/#574 remain drafts. Win re-reviews and merges in dependency order. Live acceptance stays with Claude, CPU benchmarking remains deferred, and the assigned device-key routing work has not started during this repair wave. Existing live packages and measurement artifacts remain unchanged.

## 14 November 2026 — deferred tap consumers and final stack validation

The broad hosted suite exposed two remaining assumptions after bounded tap publication: offline gain staging must settle capture publication before measuring its first block, and the legacy mixer close test must acknowledge deferred port changes. Claim `src/adi/engine/gain_stage.cpp`, `tests/test_gain_stage.cpp` and `tests/test_mixer.cpp` for that narrow follow-up, in addition to the already claimed scope/session paths. The isolated offline session may use zero-sample render boundaries and owner collection before real samples; no UI-thread adoption, fake epoch acknowledgement or weakened reclamation is allowed. Win and the returning engine seat review these engine paths.

The reviewed limiter/EQ/Focus fixes and foundation owner/seek/storage checks are on the existing branches. Final rebases preserve all originally published fixes, conditional page visibility and reentrant layout ownership. No new PR, fenced operations, schema/routing feature, live test or new CPU measurement is included. The foundation PR stays draft until the repair wave and its broader validation finish.

## 14 November 2026 — limiter review repair pushed; EQ visibility next

#541 now carries `fa1b5b87` on its existing branch, rebased onto main `8555f0d4`. Latency/shape controls are configuration-only; unclaimed offers can be replaced/cancelled, while claimed edits refuse atomically. All prepared limiters share one parked event-driven owner. A real publication-gap test first reproduced the disabled-limiter re-enable lockout, then passed after the retained-token busy fix. Eight strict Debug suites pass (1,376 numbered checks plus owner lifecycle); four validators and decisive negative checks pass. The PR stays draft through the ordered repair wave; no new PR or self-merge.

#542 is rebased onto that fix and now freezes demand/cache, visible-surface wiring and software tests. No hidden EQ parameter descriptor/value/text calls; lightweight mixer identity stays available. Main and detached STACK consumers are independent. Work then proceeds to #545 before foundation follow-ups. Live tests remain Claude's and CPU timing stays deferred.

Claim the later review-required engine paths before editing: `src/adi/engine/graph.hpp`, `graph.cpp`, `session.cpp`, `session.hpp`, `scope.hpp`, `scope.cpp`, and focused scope/tap/session tests. The change will project the already driver-applied transport revision into stamped tap spans and coalesce unpublished tap plans while preserving strict retirement and audio-only port adoption. Document/STACK gesture cleanup and software key guards use this seat's existing UI paths. No engine implementation starts before the ordered Focus repair; win and the returning engine seat review it. No fenced operation handler, schema implementation or routing feature is part of these fixes.

## 14 November 2026 — accepted device-key assignment; repairs first

Claim schema minor **1.22** for win's DEV-05/MX-24 assignment on #546 (DF-M11). The scope is canonical device-key routing/tap storage, destination identity, logical mono/stereo key width and route-lifecycle epochs, plus reversible route cleanup. This reserves the assigned format number; no format or routing implementation starts ahead of the reviewed repair sequence below. Engine-owned paths will be logged before edits, the engine seat reviews on return, and fenced op handlers remain untouched.

## 14 November 2026 — reviewed fixes take priority; no new PRs

The director pauses new PRs. Work now follows win's review order: #541 latency-control automation and limiter background ownership; #542 hidden EQ publication and conditional unmasking pages; #545 bounded FIR work and actual-rate arithmetic; then foundation gesture/seek/tap-retirement/test follow-ups. Fixes go to existing review branches, with dependent stacks rebased afterward. #537/#540 are already merged, so their follow-ups will be carried by the existing open stack rather than a new PR. No self-merge.

Claimed #541 paths: `src/juce/mix_function_host.cpp`, `src/adi/mix/limit_chain/**`, `src/adi/mix/limiter/DspEngine.*` and its narrowly required kernel-work notification seam, focused tests/CMake. #542 will own parameter-feed demand/cache, visible-surface wiring and conditional page tests. #545 will own Focus scheduling/rate and its tests. The later engine-owned follow-up is limited to scope-plan retirement/Locate epoch paths and their tests; it is explicitly required by this review, and will be logged precisely before edits. Fenced op handlers are not part of these fixes.

Current repair worktree is the private records, same #541 branch, rebased onto fetched main `8555f0d4`. The older checkout's untracked EQ extraction files are preserved on a local snapshot branch. Other owners are read-only until their ordered turn. All new public text follows the director's naming rule: refer only to measurement and the director's private research folder. Live tests stay with Claude; new CPU timing remains deferred. Existing release/review packages and sealed measurement artifacts stay unchanged.

## 14 November 2026 — director settles Versatile seed recall

Adi chose to save each instance's seed with project/state for repeatable recall. The future ADI-C Versatile adapter must restore that identity instead of reseeding on project/editor/graph reopening. This resolves the initialization design question only; complete live-state snapshots, seed generation, schema migration and the actual Versatile engine still require source/state proof. No released audio, current core, provider, package or CPU claim changes. Root records the ruling in the private decision guide/roadmap/current checkpoint; win assigns any eventual ADR/schema change. Live tests stay with Claude.

## 14 November 2026 — fixed Split prerequisite closed privately

The DS private fixed Split composition is sealed and independently root-verified: complete selected-array gates and Debug/Release generated checks pass, using unchanged Wide plus existing float overlap-save code. Product #562 remains clean and unchanged; no native provider or Split admission is supplied. Retained first/final variants preserve all predicted audio bytes. The final failure-injection scope explicitly targets recoverable data allocations; the earlier broader Release test remains separately recorded. Storage/finite-sample preconditions, malformed outer enum validation and whole-hop versus staged processing remain explicit future facade/CPU-readiness boundaries. No fitted kernel, new native execution, thinner convolver, public source edit, live test or timing.

Next independent tiers are private fixed Flat Tilt/Classic proofs, fixed/staged Split resources and remaining route/quality contracts, continuous shared capture, and optional average presentation. The current #573/#574 heads still await their own hosted CI; all review packages stay immutable for Claude. Native C/DS/G remain unavailable until real capability-aware providers and DEV-05/MX-24 keys exist. Win reviews/merges; no self-merge.

## 14 November 2026 — Opto and shared average supplied

Draft [#573](https://github.com/arieladi/Adi/pull/573), `7594f566`, on #570 supplies fixed signed-key-to-gain Opto:177 checks in Debug/Release/parent harness, strict3/validators4, zero callback allocation/free. Root verifies279 fixed records/seven owned source identities/20 unchanged child files, full selected audio and direct-gain preservation. Threshold/typed normalized times are stopped controls, ratio4/knee0/range60. It remains unregistered and does not infer stereo selection, other controls/quality or native C capability. The missing root SQLiteCpp configure is retained as a diagnostic; hosted exact-head CI trails.

Draft [#574](https://github.com/arieladi/Adi/pull/574), `ebc790ea`, on #569 incorporates exact #568 through local merge `c9eeea44`, no Main merge. Six owned files overlap two inherited files, so PR has11 distinct files. Optional fixed1s linear-magnitude averaging shares real FFT hops, a coherent immutable sidecar and physical activation fences.1,839 unique checks plus691 optimized repeats pass, strict2/validators4. Root verifies102 fixed records/six source identities/eight inherited numerical sources/15 immutable artifacts;23,035,050 Raw replay bytes are identical in Debug+Release and300 Debug FFT/CRT rows exact. Raw Result layout/default and visible UI are unchanged. Causal266/6 pending-fence negative is retained. No new worker/timer/tap or audio work. Hosted CI trails.

#572 `afd78fdf` now passes18 core+3 UI jobs, runs43540626826/43540626796, and is ready for win, unmerged here. #573/#574 are draft pending their own CI. The DS fixed Split private candidate passes six selected complete arrays with original strict gates; Debug/source/proof closure continues before any public admission. No public DS edit or new native/timing occurred. The next continuous-scope acquisition plan is peer reviewed only, not a broker/UI implementation. Current Claude packages remain fixed; live work stays with Claude; new timing remains deferred.

## 14 November 2026 — complete requests supplied; three more CI heads ready

Draft [#572](https://github.com/arieladi/Adi/pull/572), `afd78fdf`, on corrected #566 supplies explicit fixed CompleteRequests with staged scratch and full-available-span alias/pending behavior.329 generated Debug/Release checks, strict3/validators4 and callback0/0 pass. Root verifies327 fixed records/717 dependencies and all25 prior complete outputs remain identical in Incremental, CompleteRequests and forced-dual variants. Request-local16 cadence is explicit; no future upstream reads, outer-wrapper sleep, other styles/quality or native G admission. Exact-head CI trails.

#569 `d895f3d8`, #570 `282d4537` and #571 `6cc38fea` now each pass all18 core+3 UI jobs and are ready for win, unmerged by this seat. Runs:56943534712576/43534712586;57043537138362/43537138386;57143537902551/43537902601. Root independently verifies the scope's47 fixed records, ten source/archive identities and15 unchanged review artifacts. #555 remains the unresolved independent ARM storage/Ops draft.

The Opto owner in the private evidence, branch `mixview/opto-gain-20261114`, owns only a new precise signed-key-to-gain component/tests/docs/minimal registration on #570.177 Debug/Release checks and source-gain/audio preservation gates pass; fixed proof/root review precede push. Existing Clean/key/facade source stays unchanged. No stereo selection, style provider or quality admission. The shared average-domain child passes277 focused checks; full/optimized final gates continue on frozen source, with Raw layout/output/allocation preservation and real-hop demand barriers.

The DS owner is authorized only for a private fixed base-rate Split composition using unchanged Wide and existing float overlap-save source: six saved fresh fixtures, original strict gates, no new native/live/timing, public product edit or fitted kernel. The corrected schedule is128-hop/256-FFT/256 convolution latency. Win reviews/merges in dependency order; Claude's live packages remain fixed; new timing stays deferred.

## 14 November 2026 — composed Clean and continuous scope supplied

Draft [#570](https://github.com/arieladi/Adi/pull/570), `282d4537`, on #567 supplies the unregistered float-stereo Clean/key-EQ facade.172 Debug/Release checks, strict2/validators4 and callback0/0 pass. Root verifies417 fixed records/23 source identities/16 unchanged children and selected complete-output preservation. Caller validation remains atomic; a real copied-key overflow is instead contained by a clearly distinct latched ProcessingFault before Clean, with stopped recovery and an explicit post-key scan cost. No abort was reproduced, no DSP clamp/cutoff or native C admission is implied.

Draft [#571](https://github.com/arieladi/Adi/pull/571), `6cc38fea`, on #568 supplies a framework-free continuous triggered scope with prepared ordered-extrema buffers and source ranges.148 generated checks in Debug/Release, strict3/validators4, zero processing/reset allocation and actual public original-source projection comparisons pass. Full GPLv3 provenance is retained; derived files use GPL-3.0-only because an upstream later-version grant was not identified. Existing latest-window Scope, worker/audio/UI and current packages are unchanged. Shared continuous acquisition remains a later slice, distinct from sparse UI-frame data.

`mixview/g-complete-request-20261114` in the gate worktree is authorized for fixed explicit request processing only after the private source/runtime topology prerequisite closed. Preserve the corrected Incremental default, staged four-plane request scratch, full-available-span alias/pending law, honest request cadence and all prior output gates; no outer-wrapper sleep, threshold automation or native factory. The average-domain child continues separately. Live tests stay with Claude, timing remains deferred, win reviews/merges in dependency order; no self-merge.

## 14 November 2026 — processing/average CI green; next real-hop integration

#566 corrected `33c64136`, #567 moving controls `52861576` and #568 average primitive `32b334fd` each pass all18 core+3 UI exact-head jobs and are ready for win, unmerged by this seat. Gate runs43532107827/43532107786; moving43530558133/43530558118; average43532170409/43532170375. #555 remains the separate unresolved ARM storage/Ops review gate.

Draft [#569](https://github.com/arieladi/Adi/pull/569), `d895f3d8`, on #565 supplies exact-zero StereoPower lane FFT elision.305 shared/Spectrum checks, two strict units/four validators pass; root verifies55 fixed records/three source identities,552 complete-result replay windows byte-identical and300 allocation rows unchanged.36 transforms are omitted only on tested one-silent-lane windows. The both-zero parent path remains unchanged; no threshold, scan, audio or timing claim.

`mixview/average-domain-20261114` in an isolated checkout combines only exact #568+#569 parents through authorized local dependency merge `c9eeea44`, never Main. Own shared_analysis API/worker tests, narrow docs and common precise-target linking. Optional explicit1-second magnitude decay consumes real source hops, shares existing FFT/cursor, uses a separate immutable average sidecar so Raw Result layout/Debug allocations remain unchanged, and reports invalid FFT data without modifying Raw. No UI/preferences, new worker/timer/tap or old package mutation. The root continuous-scope component stays separate. Live work remains Claude's; CPU timing remains deferred.

## 14 November 2026 — next composition and continuous capture ownership

`mixview/clean-key-facade-20261114` in the private evidence starts from #567 `52861576`, owning only new `mix/clean_key`, generated tests/docs and minimal core registration. Root independently verified the preceding private nine-case actual-child composition before authorizing this facade. It keeps the existing compressor/key/filter children unchanged, explicit prepared key scratch, fixed dynamics/Programme/internal source, boundary-only optional EQ motion and strong refusal. No native C capability flip, routing, running reset, monitoring or new CPU timing.

Root owns `mixview/triggered-scope-20261114` in the private evidence, based on #568 `32b334fd`: new framework-free continuous capture primitive/tests/docs/provenance and isolated CMake/CI only. The public GPL source informs trigger/holdoff/ordered-extrema mechanics; fixed prepared source-valued buffers replace pixel arrays and trigger copies. Actual original source is checked privately with a data-only shim; no native host/editor/device is involved. Existing latest-window Scope, broker, audio, UI, packages and Raw defaults remain unchanged. Shared continuous acquisition is a later slice; no fake trigger on sparse UI windows. Win reviews/merges, Claude owns live work, timing remains deferred.

## 14 November 2026 — corrected gate, moving key controls and average primitive supplied

[#566](https://github.com/arieladi/Adi/pull/566) now points to corrected `33c64136`, fast-forward from its original head. Reset/stopped same-rate restart retain adaptive caches and defer signal baseline initialization until processing. Root independently verifies121 fixed records/145 dependencies/ten source/archive identities,207 Debug/Release checks,1,359,872 scalar comparisons and all25 old arrays byte-identical. A causal internal-state witness fails five assertions before the correction and passes afterward; its immediate float output is equal, not an audible/native comparison. The whole-request near-baseline alias predicate remains outside the incremental API and explicitly unclaimed. Draft until corrected-head CI; no native Gate enable or CPU-parity claim.

Draft [#567](https://github.com/arieladi/Adi/pull/567), `52861576`, on #564 supplies opt-in one-band boundary Frequency/Gain/Q motion with exact control cadence and prepared coefficient plans.264 generated checks in Debug/Release, three strict units/four validators, callback0/0 and selected moving/fixed preservation gates pass. Root verifies880 fixed records/ten sources/eleven unchanged implementation files. Redundant control logarithms and repeated identical render events are removed without skipping design-state advancement. It does not yet compose the authored compressor and key bank as a public core; a private actual-core composition proof is next.

Draft [#568](https://github.com/arieladi/Adi/pull/568), `32b334fd`, on #565 supplies an owner-driven binary64 linear-magnitude average primitive only.1,567 checks (1,385 retained+182 new), optimized182 repeat, three strict units/four validators pass. Root verifies52 fixed records/seven sources/eight unchanged inherited numerical/tap/broker/comparison files and15 unchanged delivery artifacts. No broker/UI/default integration or new worker/timer/tap/audio work. #565 is now all21 core/UI green and ready for win. The combined analyzer review package remains immutable for Claude after the ongoing dock round; no local app/native interaction occurred.

**Next ownership:** `mixview/analyzer-stereo-zero-20261114`, an isolated checkout on #565, owns only exact-zero StereoPower lane FFT elision in shared_analysis, focused generated tests and narrow docs. Existing double lane-energy sums identify exact zero after sanitation; no epsilon, additional scan, audio DSP bypass, result/default change or timing claim is authorized. Full-result byte replay and zero/denormal/nonfinite/transitions precede publication. The compressor owner runs private actual-core composition checks, while the Gate owner closes complete-request API semantics read-only. No source edit during a cache build; each owns a separate cache. Win reviews/merges; Claude owns all live tests; CPU timing stays deferred.

## 14 November 2026 — fixed gate lifecycle amendment required before review-ready

#566 `1fb0341d` stays draft, even if its CI passes. Deeper lifecycle review found reset currently clears smoothing caches that must survive a reset; the existing selected renders do not cover that history-sensitive boundary. An isolated parent-based amendment `mixview/g-reset-20261114` is authorized only after same-rate reprepare ownership is traced, with causal evidence and preservation of all25 original arrays/159 baseline checks. Root will review and fast-forward the verified correction into #566; no broken parent is marked ready or merged. Fresh-start/reset-mid-ramp threshold source ordering is still being closed before a new control surface. Current fixed source/evidence is immutable; no live work, new CPU timing or broad parity claim. Win should wait for the corrected head.

## 14 November 2026 — slope and fixed Classic supplied; combined analyzer review package

Draft [#565](https://github.com/arieladi/Adi/pull/565), `3a05380f`, on #563 supplies optional cached Spectrum tilt with exact Flat default, local Frozen reprojection, parent preference/Info/floating behavior and ten-control focus/layout. **1,385 checks/0**, ten strict units/four validators pass; root verifies56 fixed records/15 source identities/eight unchanged numerical/scope/broker/comparison files. A preserved13-check negative reproduces five vector-cache/allocation/math failures; the final unique-array cache has zero Flat/unchanged-frame allocation, strong failed prepare and safe reset. CI explicitly runs the new std-only test. A combined isolated Debug analyzer package and five-part Claude brief are sealed privately for after the current Dock round, superseding the pending palette-only instruction. Compile/link only; original packages/projects/daily installation remain untouched. No audio, extra worker/timer/tap or CPU timing.

Draft [#566](https://github.com/arieladi/Adi/pull/566), `1fb0341d`, on #558 supplies fixed stereo Classic with explicit routing, hold/knee/hysteresis/nonlinear stages, strong refusal/lifecycle and physical-block cadence. Debug/Release159 generated checks/868,352 scalars, callback0/0, three strict units/four validators/independent source review pass. Saved-output gates and initial/forced-dual byte preservation pass; root verifies310 fixed records/10 source identities/102 dependencies. Shared immutable table and common-gain fast path remove duplicate work without output changes. Tiny smoothing uses the source epsilon predicate; preparation refuses directed rounding before table initialization. FTZ/DAZ remain caller-owned, long-tail timing is unclaimed, and native provider/UI/other styles/quality are absent.

**CI:** #562 `25864088`, #563 `f0ee200c` and #564 `0600be7c` are all18 core+3 UI green and now ready for win, unmerged. #565/#566 exact-head CI trails. #555 remains the separate unresolved ARM storage/Ops gate; no blind rerun or fenced edit.

**Next ownership:** `mixview/keyeq-moving-20261114` in compressor worktree starts from0600, owns opt-in one-band/one-lane boundary controls, exact source ramps/cadence, prepared borrowed coefficient plans/prototype cache, generated tests/docs. Existing fixed path/buffers/output remain separate. The candidate's16 Off render/segment comparisons pass; final proof is ongoing. `mixview/spectrum-average-20261114` in analyzer worktree is approved for a new framework-free RT magnitude-average primitive/test/docs and minimal CMake/CI hooks only, after exact precision audit; no broker/presentation/Raw-default change. Gate owner is read-only tracing startup/reset source epochs before a threshold-continuation proposal. Claude owns live work; new CPU timing remains deferred; win reviews/merges.

## 14 November 2026 — All Pass supplied; policy CI green

Draft [#564](https://github.com/arieladi/Adi/pull/564), `0600be7c`, on #561 adds a separate prepared All Pass mirrored-root/bilinear path. Debug/Release118 generated checks, three strict units/four validators and callback allocation/free0/0 pass. Prior static predictions remain byte-identical; new selected fixed-output/coefficient gates pass unchanged. Root verifies1,328 fixed records/nine source/archive identities and13 unchanged implementation files. No existing plug-in/compressor/DFI/primitive change, new CPU timing, native execution or live test. Moving controls are a separate next tier. #561 `2ebc3f3b` now passes all18 core+3 UI jobs and is marked ready, unmerged; runs `43517622960`/`43517622961`. #564 exact-head CI trails.

`mixview/spectrum-slope-20261114` in the private evidence starts from #563 `f0ee200c`, owns optional Flat(default)/+4.5dB/octave Spectrum display preference and cached projection, existing parent/window/Info/floating wiring and matching tests/docs. Raw bins/broker/FFT/comparison/audio are unchanged; Frozen reprojection and narrow responsive/focus/rollback gates precede publication. No new worker/timer, dock selector, per-row LUFS or native analyzer default is inferred. Main-only aggregate loudness stays settled. No overlap with ongoing G Classic core ownership or prior immutable delivery packages. Win reviews/merges; live work stays with Claude.

## 14 November 2026 — endpoint controls and analyzer freshness supplied; Classic core claimed

**Supplied:** draft [#562](https://github.com/arieladi/Adi/pull/562), `25864088`, extends #560's fixed Wide de-esser to full threshold and zero-range controls, preserving filter/classifier/M/S histories. A distinct last-admitted-target helper models scalar admission without claiming host readback or active automation. Debug/Release216 generated checks/1,409,024 scalar comparisons, callback allocation/free0/0, three strict units and four validators pass. Root independently verifies528 fixed records/12 source/archive identities,92 byte-preserved prior predictions and20 new headless processor-only endpoint cases. The latter execute no editor/window/device/timing path. No live UI or new CPU measurement.

Draft [#563](https://github.com/arieladi/Adi/pull/563), `f0ee200c`, retains exact #559 and #557 parents through isolated integration `06b582bb`. Existing shared views now fence explicit Resume/Clear/reshow/replacement admission at newly written source samples, while new compatible views still reuse a valid stopped picture. Frozen pictures stay lease-free; shutdown revokes them. Six suites/1,338 checks, nine strict units/four validators pass;35 causal pre-fix failures are preserved. Root verifies39 fixed records/four sources, six unchanged inherited broker/component sources and six unchanged prior delivery artifacts. No numerical algorithm, extra worker/timer/tap or app package changed. Win must review/merge both parents before the child; no Main/PR merge by this seat.

**CI:** #559 `5218e657` and #560 `0eb0507c` are all18 core+3 UI green and now ready for win. #557/#558 are also ready/green; #561 still trails exact-head CI. #555 remains draft with the previously documented ARM storage/Ops failure; its cause is not established and no further blind rerun or fenced-source change was made. The palette compile/link-only review ZIP and short Claude brief are now sealed privately; the original Dock ZIP/brief remain immutable while Claude tests.

**Next owned slices:** `mixview/keyeq-allpass-20261114` in the private evidence adds a distinct prepared bilinear All Pass path on #561, with root verification pending. New `mixview/g-classic-core-20261114` in an isolated checkout starts exactly at #558 `d40550ac`; own new `mix/gate`, generated tests/docs and minimal `cmake/mix_cores.cmake` registration. Fixed Classic stereo/internal/external-key arithmetic and physical-block cadence come first; broader target clamps are source-audited before admission. Other styles, automation, quality/lookahead and native factory/UI admission stay unsupported. No C/DS source reuse by style name, route invention, #512 edit, live operation or CPU timing. Win reviews/merges; Claude remains the live tester.

## 13 November 2026 — comparison/filter CI green; storage CI handoff

**Ready:** #557 `596d5df9` and #558 `d40550ac` each pass all18 core plus3 UI exact-head jobs and are marked ready, unmerged by this seat. Runs #557 `43508430828`/`43508430785`; #558 `43509340150`/`43509339942`. Their component source/fixed proof is unchanged.

**For win/storage owner, not ignored:** parent #555 `2c4358b1` passes all3 UI and17/18 core legs, but the ARM64 GCC job remains failed. First job `132164273394` segfaulted in unchanged `adi_ops_tests` after the atomicity header (224/225 suites passed). After the parent completed, only that job was retried; new job `132181331994` instead fails unchanged `adi_store_tests` on “the same path is reusable afterwards”, while Ops now passes (again224/225). Cause is not established. Storage/Ops sources and tests are unchanged in this Hold repair; descendant #558 and sibling #557 pass the corresponding whole core matrix. No storage/fenced handler was edited, no warning/test weakened, and no repeated blind retry is claimed as a fix. #555 stays draft until the owner resolves/accepts this exact-head gate. Win still reviews dependencies in order; no self-merge. Private incident retains both job identities/limits.

## 13 November 2026 — shared palette controls supplied

Draft [#559](https://github.com/arieladi/Adi/pull/559), `5218e657`, on #556 supplies46 public palette families plus ADI's exact old default, preview strips/Reverse and per-window parent preference. Info follows; floating analyzers initially copy then retain local lifetime choices. Visible frozen recoloring retains measurement/demand, and paint reads an owner-prepared raster. A common analyzer-control focus guard protects detached/floating controls; reset-on-discard fixes two causally reproduced stale-raster counter collisions. **986 checks/0**, nine strict units, four root validators and independent source review pass. Root verifies49 fixed records/19 source/archive identities plus executed binary/log identities. Exact source pins/full notices accompany data. No audio algorithm/broker/FFT/worker/timer change or new CPU timing; shared-view freshness while another consumer stays live is explicitly a later combined follow-up. Compile/link-only review package preparation is separate; Claude's current Dock ZIP/brief stays unchanged. CI trails; win reviews/merges, no self-merge.

## 13 November 2026 — next invariant key-filter and de-esser slices claimed

`mixview/keyeq-policy-20261113`, an isolated checkout, starts from #558 `d40550ac`. Own new `mix/key_eq` settings/policy/fixed bank, generated tests/docs and minimal `cmake/mix_cores.cmake` wiring. First tier is stopped/fixed base-rate stereo source-derived ordered shape/prototype/anchor policy over the precise filter primitive; moving scheduling, separate bilinear/quality paths and native admission remain explicit later gates. No changes to the sealed primitive or compressor sources.

Independent `mixview/ds-wide-core-20261113`, new the private records, also starts from #558. Own new `mix/deesser` core/control mapping, generated tests/docs and separate-worktree CMake wiring. The bounded first tier is base-rate Wide processing with explicit source/link configuration and genuine reset/restore/classifier-state policies, using eligible preserved output gates. No incomplete DS function is enabled. Shared filter/key-policy deduplication requires strict proof; no cross-owner source edits by inference. No live/native work, new CPU timing, invented device keys or #512 fence changes. Win reviews/merges; Claude's dock review trails independently.

## 13 November 2026 — shared comparison and precise filter components supplied

Draft [#557](https://github.com/arieladi/Adi/pull/557), `596d5df9`, on #556 supplies explicit independent/held spectrum comparison. Per-endpoint identity/calibration/intervals, bounded copied bins, owner-token replacement, local demand release, shutdown and real newly-written-sample resume barriers are explicit. No automatic alignment, reference player, gain/loudness matching or UI wiring. Debug/Release237 unique checks/0, three strict units/four validators and126-configuration generated complete-result byte preservation pass. Root verifies89 fixed records/seven sources. Prepared scratch removes one temporary gain allocation without changing results. CI trails.

Draft [#558](https://github.com/arieladi/Adi/pull/558), `d40550ac`, on repaired #555 supplies an ordinary matched-section solver and double DFI/float section runtime. Unchanged-request cache, strict refusal and precise out-of-line sample/span loops preserve the arithmetic boundary even under a fast caller. Debug/Release42 generated checks/0, zero processing allocation/free, two strict units/four validators and preserved static numerical gates pass. Root verifies1,381 fixed records/six sources and unchanged compressor blobs; forced-ISA compilation is evidence only, never executed. Shape/anchor policy, moving scheduling, separate quality/bilinear graphs and native C/DS/G remain later. No new CPU timing, live/native execution, installation change or self-merge. Win reviews/merges; the policy child continues independently while Claude tests the unchanged Dock ZIP.

## 13 November 2026 — fixed Hold Debug repair supplied

[#555](https://github.com/arieladi/Adi/pull/555) now points to `2c4358b134abefdd07ab1467d895ddcd1bc00b4b`, fast-forward from c756. Minimal constexpr `else` fixes actual Debug C4702; prepared fixed queue storage also fixes a reproduced hidden Debug container-allocation failure while preserving inclusive ring arithmetic and strong failed-prepare continuation. **137/0** generated checks in both Debug/Release, 495,616 scalar comparisons, exact1/2 prepare-allocation assertions and callback allocation/free0/0. Two strict units/four validators/source review pass; original saved-output preservation gates pass privately. Root verified552 fixed records and8 canonical archive/source identities, preserving prior seals. Three post-commit checkout newline conversions are recorded separately. New exact-head CI `43506151695`/`43506151829` trails; win reviews/merges. The filter child starts from repaired2c4358b1. No new native registration, sound/CPU-percentage claim, timing, live work or installation change.

## 13 November 2026 — analyzer/processing continuation; dock CI closed

**Director instruction:** continue the remaining shared processing cores and analyzer modes while Claude tests the supplied dock. Preserve sound and resource efficiency. The live gate trails these independent slices; no local desktop/host/capture work or new CPU timing.

**Dock CI:** #556 `01992652432d31f4ec909599ba9ade6d3d482541` now passes all 18 core and three UI jobs, runs `43499079545` / `43499079980`. Marked ready for win, unmerged by this seat. The sealed Dock ZIP and Claude brief remain unchanged.

**Declared independent work:** `mixview/analyzer-palettes-20261113` in the private evidence, on #556, owns palette data/selector/reverse and software raster lifecycle, preserving the existing ADI default and demand. `mixview/reference-compare-20261113` in separate the private records, on #556, owns a framework-free independent/held spectral comparison model and generated tests/build registration. It makes no sample-aligned waveform, Sync, loudness-match or null claim and adds no UI route or audio work. These analyzer branches are independent until reviewed.

**Processing CI repair first:** #555's actual Windows Debug `/WX` C4702 failure is reproduced. A minimal constexpr `else` repair is isolated on `mixview/compressor-hold-ci-20261113`; fixed Debug compilation passes and its test execution is being diagnosed before calling the repair verified. The mathematical expressions are unchanged. Only after the repair and original audio-preservation gates pass will `mixview/matched-filters-20261113` continue the common coefficient/DFI primitive. Full C/DS/G/native key routing is not claimed complete or enabled. Win reviews/merges; no self-merge.

## 13 November 2026 — optional dock and unregistered fixed Hold supplied

**Dock:** draft [#556](https://github.com/arieladi/Adi/pull/556), `01992652432d31f4ec909599ba9ade6d3d482541`, on #554. Optional right Meters/Phase/Spectrum column starts hidden/Main, retains the separate analyzer tab and persists per-window preferences. Compatible views share domain/tap/cursor/FFT; only Main meters use the existing weak loudness projection. Responsive layout preserves the actual STACK/header rectangle through drag/text/menu capture; hiding/insufficient room releases demand. Failed saves retain frozen pictures, and analyzer-control focus protects against destructive command fallthrough.

Four focused suites pass **957/0** (model614, explicit null-peer Mix View177, software analyzer101, WindowState65); eight strict frontend units, four root validators and independent source review pass. Root verified 33 fixed records plus12 source/archive identities. App/scanner compile/link only; isolated Debug Dock ZIP, exact hashes and five-part Claude handoff with pictures are ready privately. No local native/default-shell/live execution, numerical-kernel change or CPU timing. The package excludes independent #548/#552/#555 and enables no native Carve/Focus/C/DS/G. CI trails this head; win reviews/merges.

**Independent processing:** draft [#555](https://github.com/arieladi/Adi/pull/555), `c7560c71ecf33da5056e9b9bb6edf4838ce58773`, on #552, adds prepared fixed active Hold to the unregistered Clean core. Narrow initial admission is explicit. Zero Hold keeps its specialized old path; bounded queue state and exact float-sample coefficient caching preserve resource/sound requirements. **135 generated checks/495,616 scalar comparisons**, callback C++ allocation/free0/0, two strict units/four validators and final source review pass. Fixed private complete-output/cache-preservation proof is sealed; root verified585 records plus7 sources. It is not native C/DS/G admission or whole-product/CPU parity. No self-merge, daily installation or original project change.

## 13 November 2026 — optional right analyzer dock decided; prior CI green

**Director answer:** optional right dock with show/hide; retain the separate analyzer tab. The active `mixview/analyzer-dock-20261113` child on #554 `e577b0e9` owns compact shared Meters/Phase/Spectrum presentation, explicit Main/Selected, per-window view-state preference/width and responsive header/body layout. Default off/Main. Compatible views share domain/tap/cursor/FFT; no new analysis worker, timer or DSP. Requested versus applied layout is separated through a held STACK edit, while whole-surface hide/tab/detach/minimise keep immediate cleanup. Scope: Mix View/analyzer UI, window-state JSON and matching software tests plus the existing weak Main meter owner wiring. No op/schema/fenced main file edits. Claude alone verifies live flow.

**Independent core continuation:** `mixview/compressor-hold-20261113`, an isolated checkout, on #552 `76ec1072`, owns a fixed Clean post-Hold component with narrow initial positive-Hold admission. Preserve every existing zero-Hold prediction, existing resource guarantees and strict saved-output gates. It remains unregistered; no C/DS/G native availability or limiter-fallback dispatch changes. Native reference research stays private. No new CPU timing or native execution.

**CI closure:** #553 `6920f815` and #554 `e577b0e9` each pass all 18 core plus three UI exact-head jobs, now marked ready for win and unmerged by this seat. Runs #553 `43435574213`/`43435574200`, #554 `43437502275`/`43437502378`. Their source/evidence/review package is unchanged; the previous Flow ZIP excludes the new dock/Hold work. No self-merge or daily installation change.

## 13 November 2026 — per-tool flow supplied with isolated review package

**Supplied:** draft [#554](https://github.com/arieladi/Adi/pull/554), `e577b0e941ec31d30523b23cccb324e00bbdad74`, on repaired #553 `6920f815`. One inspected item enlarges while neighboring items stay compact, preserving existing row fisheye and actual device/page identities. Bounded transitions use the existing clock, retain pointer anchoring and give explicit keyboard focus priority. Paint, canvas hits and drag/text/menu capture use the last complete displayed pose; reentrant callbacks cannot commit an interrupted partial presentation. Common parameter/gesture/undo authority is unchanged. No DSP, analyzer demand, worker or timer added.

**Validation:** clean exact-head **614 model +150 explicit Mix View software checks =764, zero failures**; five owned strict frontend units, four root validators, clean diff and independent final review pass. The full prior focused path executes; a diagnostic candidate's skipped conditional assertions were rejected, then fixture timing/restoration was corrected and guarded before final validation. Root verified 20 fixed evidence records plus seven source/archive identities. Eight transition slots use the existing counted snap policy at saturation; captured row geometry is exact, other rows stop at current model pose. No native/default-shell local execution or CPU timing.

**Delivery:** app compiled/linked only; an isolated Debug Flow ZIP, hashes and concise six-part Claude handoff are complete in the private current checkpoint. The package includes #544/#551 ancestry and existing EQ/L, excludes independent #548/#552, and enables no native Carve/Focus/C/DS/G. Daily install/projects remain unchanged. #554 exact-head CI runs `43437502275`/`43437502378` trail; live flow/pictures remain Claude's. Win reviews/merges; no self-merge. Remaining engine/analyzer work is not called complete.

## 13 November 2026 — explicit empty-channel navigation repair

[#553](https://github.com/arieladi/Adi/pull/553) now points to `6920f815b087276b7e192ab3573eaa3d0eb93a82`. The first CI head compiled on all UI platforms but failed two existing explicit channel-shortcut assertions: With tools hid an empty target. An explicit shortcut now visibly selects All channels only when membership alone excludes an otherwise reachable target, preserving search, pins, folds and project scope and creating no processor. Invalid/eligible targets retain their scope. Callback-bearing focus effects have lifetime guards.

The isolated parent repair passes **581 model checks, zero failures**, four strict frontend units, four validators and independent source review. Four added focused software UI checks await the combined child/hosted gate and are not counted as executed. New exact-head CI is `43435574213`/`43435574200`, pending. Root verified 17 fixed artifacts and six source/archive identities privately. The separate item-focus child is integrating this parent before final validation; no live/native/default-shell local execution, new CPU timing, audio change or self-merge.

## 13 November 2026 — grouped channel view supplied; item focus claimed

**Supplied:** draft [#553](https://github.com/arieladi/Adi/pull/553), `78407d69e35364580dbb9506b525ba6b7a0fd4b7`, on #551. With tools/All channels, stable native membership, full chains/ancestry, fold recovery, Jump/pin/capture refresh and toolbar command ownership pass **569 model +134 software-peer Mix View checks (703 unique), zero failures**. Five strict frontend units and four validators pass. Independent review caught stale Jump indices on structural capture cancellation before publication; the final fix carries pre-mutation track identity and tests deletion, reorder, successor capture and one-shot cleanup. Eight sources/40 fixed evidence records verify privately. No native/live/default-shell path, new CPU timing, DSP change or self-merge. CI trails this head.

**Next item / ownership:** `mixview/item-focus-20261113`, same an isolated checkout, on #553 `78407d69`. Extend existing row fisheye with per-item rectangles/tier and a stable emphasized ItemRef; compact siblings, preserve horizontal/vertical viewport anchoring and freeze full geometry during drag/text/menu capture. Own StackModel/ui_stack and focused model/software-peer tests, with ui_shell seam only if required. Current device/page identities, common edit/undo authority and new membership/toolbar guards remain unchanged. No extra timer/DSP/feed, native editor launch, fenced file or bulk parameter law. Root's private item-focus plan records stationary-pointer, keyboard priority, capture and allocation gates. Win reviews/merges and Claude alone tests live behavior.

## 13 November 2026 — Main readouts and Clean component CI green

[#551](https://github.com/arieladi/Adi/pull/551) `749fd4b1` and [#552](https://github.com/arieladi/Adi/pull/552) `76ec1072` each now pass **all 18 core plus three UI exact-head jobs** and are marked ready for win, unmerged by this seat. #551 runs `43420569228`/`43420569234`; #552 runs `43423036270`/`43423036303`. Source, local gates and fixed private evidence are unchanged. #548 is also fully green/ready. The active grouped-view presentation slice has not yet been published; independent review caught a Jump/capture stale-index issue before release, now fixed through pre-mutation stable identity with deletion/reorder regressions under final validation. No live work, new CPU timing or self-merge.

## 13 November 2026 — grouped-view keyboard ownership guard

The active `mixview/channel-groups-20261113` slice additionally owns a narrow `ui_shell.cpp` command/menu guard with `ui_stack` toolbar focus. Scope/search/row-size/button focus must not fall through from a stale focused-device context to track/device Cut/Delete/Duplicate actions. Native text/choice navigation keeps its local handling; ordinary project Undo authority is unchanged. A software-peer repro and regression accompany the guard. This is not a fenced `src/main.cpp`/op edit or a change to audio/bulk-edit semantics.

## 13 November 2026 — channel-grouped instance view clarified; presentation slice claimed

**Director answer:** the Mix View shows channels with its opt-in functions, grouped by channel (e.g. guitar EQ/Clip/Limiter, vocal EQ/Focus), with channel selection and search like Pro-Q's Instance List. The newly supplied Gemini image is a starting reference, explicitly cluttered; smooth focus/hover enlargement is required instead of every tile being permanently expanded. The private guide retains the exact image identity and old/new reference relationship. This does not authorize every search result as an audio-edit target, a relative/absolute bulk law, reserved slots, per-row loudness, or all mocked modulation/analyzer panels.

**Item:** `mixview/channel-groups-20261113`, existing an isolated checkout, based on #551 `749fd4b1`. Own StackModel/actual-chain presentation membership, matching ui_stack controls, stable search/selection/capture behavior and focused model/software-peer tests. Default channel rows have an actual native function; ordinary inserts remain in their real order inside eligible channels. An explicit all-channel view preserves discovery/add-first-function without hidden device creation. Full project ancestry/source identity is retained; no parallel device/edit/undo authority. Item-level focus emphasis is audited separately before its geometry change. No fenced handlers/schema/main or new DSP/CPU timing/live work. Win reviews/merges; Claude live work trails.

## 13 November 2026 — unregistered Clean compressor component supplied

**Item:** draft [#552](https://github.com/arieladi/Adi/pull/552), `76ec1072c2c1478c0de0388a0eb24d070d0ea5e0`, directly on #548. Framework-free base-rate Clean kernel, Settings/ControlMapping, physical-block/contiguous-segment contract and generated tests. No native factory/controller availability or existing audio path changes. Other styles, quality/OS, lookahead, hold, key EQ, audition and wet/dry are absent; prepared source/link/mode is fixed and unsupported Auto Gain+M/S is rejected. Double I/O uses the float engine with double envelopes and widened output. Strong prepare failure, runtime continuation, pointer preflight/aliases and exact segment ownership are explicit.

**Gates/work:** 96 unique generated checks / 335,872 compared scalar samples / zero failures in standalone and full-project Release; callback C++ allocation/free0/0. Two unique strict frontend units and four validators pass; independent review has no blocker in this bounded tier. Curves are owner-prepared, invariant control work retained, coefficients bounded-cached and identical fully linked envelope work reused. Separate fixed numerical evidence is sealed privately with eight source identities; no private observations/fixtures enter public source/tests. Whole-product sound/CPU parity is not claimed, no new timing or live work, no self-merge. Win reviews/merges after this head's CI.

**Continuation:** #548 is all21 CI green/ready, #551 is a separate 252-check draft with CI trailing. The director's next question is broad-edit target scope (explicit group vs selected vs filtered); no answer is inferred. DEV-05/MX-24 route ownership remains a coordination dependency, not a new director design question. Broader compressor styles/quality, DS/G and full analyzer-product features remain later work; Still stays an ordinary plug-in.

## 13 November 2026 — native EQ serial integration CI green

[#548](https://github.com/arieladi/Adi/pull/548), exact source `7faf25c10cc4595f5ad99899d2e1f6a317b1ab38`, now passes **all 18 core and three UI jobs**, runs `43417010772`/`43417011172`. Marked ready for win, unmerged by this seat. The isolated functional package/source/hashes are unchanged. Live acceptance remains Claude's; no local application launch or new CPU timing. Independent #551 `749fd4b1` is draft with its own CI queued/running; its local 252-check gate does not substitute for that matrix. Unregistered compressor component work continues in its isolated worktree.

## 13 November 2026 — Main meter projection supplied independently

**Item:** draft [#551](https://github.com/arieladi/Adi/pull/551), `749fd4b1511e3e0a5b46fca6f1f4a1ff6621533e`, directly on #544 `632761db`. Meters mode projects the existing persistent Main M/S/I LUFS and held aggregate dBTP into Info/full/floating views, clearly named programme/pre-cue. No new meter DSP/FFT/tap/worker/timer. Weak per-document bindings are revoked before teardown; prepared versus audio-adopted format IDs suppress stale values; local Freeze/Reset never pause/reset the shared Main service. Non-Main/device/other tap sources cannot display Main readings. The numerical DSP block is unchanged and the four fields remain independently timed, not a coherent whole-field snapshot or per-row LUFS claim.

**Gates:** pure SharedAnalysis108, MainLoudness57 and fresh software-peer presentation87 = **252 checks / zero failures**. Five owned strict frontend units and four validators pass; full app compiles/links only. Source/executables and logs are fixed-copy sealed privately; all 33 records and eight source identities verify. No new CPU timing, application launch/live desktop work, daily install change or self-merge. This independent branch is not included in the native EQSerial review package. Win reviews/merges after CI; Claude's live review trails the merged build. Latest main `c88131a8` merges #537/#540 ancestors and does not require rebasing these isolated child stacks now.

## 13 November 2026 — independent compressor core slice claimed

**Item:** `mixview/compressor-core-20261113`, an isolated checkout, based on #548 `7faf25c1`. Implement an authored framework-free base-rate Clean compressor kernel with explicit parameter/ramp/reset/restore/prepare state, key/channel-link handling, generated-signal tests and a separately injectable adapter as the contracts permit. New files under `src/adi/mix/compressor/`, matching tests and narrow CMake wiring only. It is an unregistered component slice: no native Compressor availability until explicit per-kind factory dispatch, complete controller/state and quality/style paths pass. The existing factory's availability guard must not be bypassed or allowed to fall through to LimiterDevice for a newly enabled kind.

**Boundaries:** no imported vendor code, private observations in public sources/tests, host SDK, fenced handlers/schema/main, live desktop or new CPU timing. Existing sound is unchanged because the new core is not registered. Strict numerical/lifecycle checks and callback work/allocation accounting precede review. Broader C/DS/G cores remain later slices; no whole-product/reference parity is inferred from one kernel. Native DEV-05/MX-24 routes still require owner coordination. Win reviews/merges and Claude's live work trails development.

## 13 November 2026 — native EQ serial adapter delivered for review

**Item:** draft [#548](https://github.com/arieladi/Adi/pull/548), `7faf25c10cc4595f5ad99899d2e1f6a317b1ab38`, directly on #546 `abed5098`. The existing native EQ now calls the proven serial backend with only EQ enabled. All 730 IDs/state and common edit/undo paths remain stable. Provider-only absent-key programme fallback preserves the current native law; connected silence stays silent. Authored learning survives stopped reprepare, complete PDC admission stays physical-boundary owned and a Main-only default-empty nonfatal issue reaches the common header/tooltip. No new native Carve/Focus controller/route availability or reserved slot is introduced.

**Gates/delivery:** nine selected Debug MSVC W4/WX suites / 1,524 checks / zero failures; 248,832 exact active authored-EQ matrix pairs plus spectral-sensitivity, continuity, PDC and controlled failure checks. Tracked callback C++ allocation/deallocation zero. Six strict changed frontend units and four validators pass. Fixed copied proof and isolated hashed Debug review application/ZIP/Claude handoff are complete privately. No application was locally launched. The analyzer resource/Main-readout branch is separate and is not falsely included in this package. New CPU timing remains deferred, and live testing stays with Claude.

**Review state:** #545 and #546 now pass all 18 core plus three UI CI jobs and are marked ready; no self-merge. #548's own runs `43417010772`/`43417011172` are in progress. Win reviews/merges in dependency order. Canonical DEV-05/MX-24 tap-schema/layout/lifecycle ownership coordination remains open independently of #512. Still remains an ordinary plug-in only.

## 13 November 2026 — Main-only analyzer meter projection claimed

**Independent item:** `mixview/main-meter-readout-20261113`, the private records, directly on green #544 `632761db`. Add read-only Main M/S/I LUFS and held aggregate dBTP to Meters presentation, consuming the existing Session Main measurement. Info/full/floating reuse the same plain-value renderer; visible/unfrozen actual Main/Post-Mixer only. Source label names Main programme before cue/metronome. It adds no DSP kernel, analysis tap, FFT, worker or timer, and does not reset/pause shared Main measurement through local analyzer controls. Default Spectrum and other-source peak/RMS remain unchanged.

**Owned files:** `ui_analysis.hpp/.cpp`, `ui_main.cpp` safe owner binding and focused existing software-peer analysis tests. SharedAnalysis/resource files and all native EQ/provider/status paths remain with their existing slices. The present Main service is older N13; this is a projection of its existing definitions, not a substitute for engine-owner ADR-0219 migration or a coherent per-sample snapshot claim. New CPU timing remains deferred; no live desktop operation. Win reviews/merges independently after the focused gates.

## 13 November 2026 — native EQ serial-backend integration claimed

**Item / ownership:** `mixview/eq-serial-native-20261113`, same an isolated checkout, directly on #546's `abed5098`. Connect the proven serial backend to the existing EQ-only native provider while retaining all 730 IDs/state, common edits/undo, authored learning, mono/stereo float/double and physical PDC. Carve/Focus controls and routes remain unavailable. Provider-only programme-as-key compatibility preserves the current native absent-bus law without mutating its authored external toggle; connected silence remains a true silent key. Configuration readiness is asynchronous, with old audio retained until complete admission.

**Common seam claimed:** a default-empty Main-only nonfatal `DeviceInstance::processingIssue()` view, forwarded through the EQ provider and common ParameterFeed/device header. It exposes a parked configuration failure without throwing from the UI pump, marking the device unloaded, disabling edits or changing audio. Existing providers remain empty by default. Own files: `eq/Backend.*`, `eq_processing.hpp`, `mix_eq_host.cpp`, common `device_model.hpp`, `parameter_feed.*`, device-header presentation, focused native/feed/UI tests and build wiring. No fenced handlers/schema/main, plugin SDK factory, new parameter IDs or automatic corrections.

**For win / engine:** refreshed DEV-05/MX-24 audit confirms no assigned device-key implementation branch and no canonical source-tap side table. #512's ops fence is separate from win's XC-15/R-c format placement. Please coordinate that placement and common owner-prepared key layout/source-lifecycle metadata; merely merging #512 will not create keys. This seat proceeds with the native EQ-only work meanwhile, never substituting MX-30 analyzer cursors for key audio. Still remains an ordinary plug-in by the director's decision. Live testing stays with Claude; new CPU timing remains deferred.

## 13 November 2026 — Still stays an ordinary plug-in

**Director decision:** Adi chose “Keep Still as an ordinary plug-in for now.” No native Still function, reserved slot or EQ page is added. Existing ordinary chain use and the future CLAP format remain in scope. The private decision guide/roadmap are updated; win assigns any ADR follow-up. This resolves the placement question and does not block the shared processing/analyzer or canonical device-key integration.

**Continuation:** exact-head #545/#546 CI is being checked before the next native adapter slice. The seat audits the actual device-key ownership seam and EQ-only integration of the already-proven serial backend. Existing IDs/state/learning, common edit/undo, physical PDC and honest unavailable Carve/Focus pages remain requirements. No product change follows from the Still choice itself. No local live desktop work, new CPU timing or self-merge.

## 13 November 2026 — asynchronous serial-chain/page-selection slice supplied

**Item:** draft [#546](https://github.com/arieladi/Adi/pull/546), `mixview/eq-chain-adapter-20261113`, `abed5098bb7ba8a36d507544e15ad88537a3207c`, directly on #545's `83e3fad6`. Eleven files: standalone Backend, Core lifecycle/work metadata, group-local selection UI and corresponding tests/CMake. Audio alone requests/admit/mirrors complete PDC; Main schedules one lazy owner from atomics; worker alone prepares. Ordinary scalar edits schedule zero owner work. Stopped reprepare retains authored EQ learning, with explicit failed-closed recovery at its fallible in-place commit boundary. Metadata-backed pages replace placeholders only when real metadata exists; Focus node selection is independent of EQ bands and Carve globals stay visible.

**Gates:** eight selected Windows Debug suites / 851 checks / zero failures; final changed Core119/backend84 after the request-publication guard, software-only UI59/functions43; six owned strict Clang units and four validators pass. Backend compares46,080 active authored-chain samples with zero guarded callback allocation/deallocation. Separate real allocation-failure/publication-interleaving probe15/15 and independent concurrency review pass. Fixed copied artifacts/source are sealed privately; earlier mutable cache identities are explicitly historical, not falsely immutable. Hosted CI is now running (`43411181397`/`43411181428`) and remains a gate. #545 final-head CI also runs with no new known failure; #544 is all-green/ready. Win reviews/merges, no self-merge.

**Integration boundary:** existing native factory, EQ IDs/state, route availability and installs are unchanged. Actual controller/state extension, canonical DEV-05/MX-24 key width/source/route lifecycle, native registration and live tests are still required before Carve/Focus pages become usable. The routing coordination request above remains open; no fenced handler/schema/main file is touched. New CPU timing remains deferred and no local desktop action occurred. The director's outstanding Still placement choice is recorded separately; no Still function is invented meanwhile.

## 13 November 2026 — analyzer resource closure ready; configuration owner/lifecycle gates

**#544 ready:** `632761db`, all 18 core + three UI jobs pass (`43406307227` / `43406307411`). Marked ready for win, unmerged. Its five-file scope and private numerical/resource evidence are unchanged.

**#545 CI follow-ups:** current `83e3fad6`, correcting only GCC test indentation and const-reference range warnings after initial `914ac214`. Processing source is unchanged. Local 639-check gate passes, final composition target reruns 93/0; current-head hosted CI remains running. Old failed runs are not relabeled. Win reviews/merges after exact-head green.

**Child adapter continuation:** a separate `eq_chain::Backend` now keeps Main/worker/audio duties distinct: audio alone publishes requests/admission/complete-PDC mirror; Main only relays atomics and wakes one lazy owner; worker alone builds configurations. Ordinary scalar automation schedules no owner work. Stopped reprepare retains the authored EQ object/histograms, with staged failures preserving the old chain and a final in-place allocation failure explicitly failing closed. Component gates pass (Core119, backend84), including actual three-actor scheduling and zero callback allocations; private actual-allocation-failure probe11/11 and independent source review pass. Page-specific EQ/Focus selections and metadata-backed pages are authored; full software-only UI build is still running. The existing native factory/key availability is deliberately unchanged pending the actual controller/route seams. No new CPU timing or local desktop actions.

## 13 November 2026 — Focus/composition and analyzer resource slices published

**Processing:** draft [#545](https://github.com/arieladi/Adi/pull/545), `mixview/focus-core-20261113`, `914ac214e49c9a97ba48aa09f9e4807d1d2ca4cb`, stacked on #543. Consistent Focus settings/preset recall and independent EQ → Carve → Focus are implemented as framework-free cores. Immutable full plans cover enable flags, kernel tokens, component/total PDC and original-key delay; same-total repartition waits for complete readiness. Disabled resources are lazy, off acknowledgements guard owner resets, scalar controls remain live, callbacks allocate/free no storage. Validation: strict MSVC five suites / 639 checks / zero failures, six owned Clang frontend units, four validators, independent concurrency review. No native Carve/Focus availability, reference-wide/perceptual parity or new CPU percentage is claimed. Hosted CI is running and remains a gate.

**Analysis:** draft [#544](https://github.com/arieladi/Adi/pull/544), `mixview/analysis-resources-20261113`, `632761db5fc9dc6d2515c052a47ef0a3b1ddae72`, separately stacked on #543. Lazy FFT resources, idle-source work exclusion, shared immutable history and actual-rate negotiation pass 162 targeted checks and exact authored-result comparisons. The aggregate 64 MiB reservation ceiling covers immutable Column-bin payload in the application service, including frozen readers, not all process memory. Hosted CI is running. #542/#543 remain all-green/ready; win reviews/merges, no self-merge.

**Next item / ownership:** `mixview/eq-chain-adapter-20261113`, the private records, based on #545. Bounded next work: authored-EQ learning continuity across stopped reprepare, a demand-driven configuration-owner/atomic advertised-PDC relay, page-specific band/section selection, matching tests/build wiring. Existing EQ IDs/edit/state/undo stay stable; Carve/Focus remain honestly unavailable until real keys and controller wiring exist. The UI page selector, new eq_chain Backend and Core continuation paths are claimed by this seat. Existing source adapter is not replaced before its continuation gates pass.

**Owner coordination requested for DEV-05/MX-24:** device keys need canonical routing tap storage, destination device identity, logical mono/stereo key width and route-lifecycle epoch; current NodeIo only supplies main width and summed key pointers. The private audit/proposal is complete. Please assign/coordinate the canonical schema/tap/handler and graph-route work with this seat before those files are edited. No analyzer cursor is substituted for audio routing. #512 ops/main/changeset fences and format ownership remain untouched. No director redesign is needed for this existing decided routing contract; Quick Kick attachment stays separate. All live work remains Claude's; new CPU timing is deferred.

## 13 November 2026 — consistent Focus settings decided; aggregate processing and analyzer resources continue

**Director decision:** Focus FIR uses consistent current settings and preset recall. Frequency/Q jointly define both extraction paths; no hidden knob-order state is carried into the product. Ordinary active-stream overlap remains distinct from settings consistency. Independent EQ → Carve → Focus enables and manual Apply remain settled. The private decision guide/roadmap retain the answer; win assigns ADR numbers.

**Parent review ready:** #543 `ff8acc8b` now passes every exact-head core/UI CI job (18+3), is marked ready for review, and remains unmerged. #542 is also fully green. Win reviews/merges in dependency order; no self-merge or daily installation change.

**Processing continuation:** `mixview/focus-core-20261113`, an isolated checkout, owns the framework-free Focus core/tests, prepared EQ capacity/request/hold-admission APIs and new `src/adi/mix/eq_chain/` facade. Review found and fixed wrong-PDC acceptance, requested/prepared frequency mixing, dormant delayed-media replay and foreign-context collisions. The generated policy/lifecycle gate is 96/0; serial integration tests are still in progress. Same-total child offers must not admit before the aggregate plan; disabled Focus stays unprepared, requested key history warms before downstream admission, and whole-stage reactivation resets on the owner after off acknowledgment. Source/reference evidence remains private, and no native Carve/Focus page/route is claimed complete. FIR scheduling/resource and CPU gates remain explicit.

**Independent analyzer slice claimed:** `mixview/analysis-resources-20261113`, the private records, based on #543. Own files `ui/shared_analysis.hpp/.cpp`, `juce/ui_analysis.cpp`, `test_shared_analysis.cpp`, `test_ui_shared_analysis.cpp`. Lazy spectral planning, source-progress/dirty scheduling, immutable charged history and actual-rate history negotiation; existing numbers/defaults/128-column behavior are preserved within the admitted budget. No new spectrum UI/settings or processor edits. #512 fences and format ownership remain untouched. No live desktop work or new CPU timing; Claude live testing trails these slices.

## 13 November 2026 — Carve/key core slice supplied; Focus configuration and device-key continuation

**Item:** [draft #543](https://github.com/arieladi/Adi/pull/543), `mixview/unmask-cores-20261113`, current `ff8acc8b0376eeb816715e6d9620fbc2a9666810` (initial `2e861b26`), based directly on #542's `a0548d3c`. Eight files: authored framework-free Carve, reusable aligned-key transport, generated-signal tests and CMake wiring. Explicit independent key planes survive mono-main layout, delayed disconnect tails, nonuniform callback slices and an in-place upstream main writer. Disabled processing skips work; inactive contributions release and reach exact dry. This core slice does not enable the still-unconnected Carve/Focus pages or invent a device route.

**Gates:** standalone CMake/MSVC strict component build: 2 suites / 129 checks / zero failures (78 Carve, 51 key), guarded zero C++ callback allocations/deallocations. Strict Clang diagnostics/frontend gates and four repository validators pass. The initial hardened-GCC CI caught a signed chained-zero-assignment warning; `ff8acc8b` separates the assignments with no arithmetic change, and the 129 checks pass again. Current core/UI runs `43373097497`/`43373097531` trail this log; do not treat the old failed leg as passed. No CPU timing, local desktop/input/capture/native host, installed plug-in or daily-install change. CI must pass independently before win's merge; the draft stays separate from Focus work.

**Parent correction:** #542 `a0548d3c` removes a copied pure publication getter that allocated on unchanged Linux frames. Immediate SafePointer/epoch and interaction End/destruction guards remain. Its refreshed isolated package again passes 19 selected suites / 5,055 checks / zero failures; exact-head CI is now fully green: 18 core and 3 UI jobs (runs `43370094004` / `43370094015`). Private handoff/package manifest are refreshed; original audio/extraction seals are unchanged. Win's merge/review and Claude's live review remain separate.

**Next branch:** `mixview/focus-core-20261113` in the same isolated worktree, based on #543. Focus keeps complete immutable offers until physical admission, including equal-latency generations; bounded CAS-leased mailbox publication coalesces the latest unbuilt state. IIR-only requests avoid unused FIR planning. A remaining audible compatibility choice is with the director; native admission and state are not claimed complete. Shared analysis suggestions remain manual.

**For win / device-key seam:** the seat is auditing DEV-05/MX-24 next. Stored routing already permits a device endpoint, but the planner rejects it, the realizer's nonzero routing ID means a send, and a device-key history needs more than a track pair. Exact Session device nodes and audio tap interfaces exist. Direct device deletion also needs reversible polymorphic route/tap cleanup. No schema/ops registration or fenced handler is changed yet; a prepared captured helper may avoid the #512 fence, but tap storage/format ownership must remain coordinated. Analyzer taps are not key routes. Win reviews/merges the stack; no reserved slots return.

## 13 November 2026 — independent EQ → Carve → Focus decided; unmasking cores continuation

**Director decision:** EQ, Carve and Focus are independently enabled in fixed **EQ → Carve → Focus** audio order. All may run together. Separate pages and manual application of analysis suggestions remain settled; selecting a page is view-only. The private decision guide/roadmap retain the answer. Win assigns any superseding ADR; no reserved slots return.

**Next item / ownership:** `mixview/unmask-cores-20261113`, an isolated checkout, based on `efa80a06` (#542). The seat implements authored framework-free Carve/Focus processing from the completed private contracts, validates against preserved offline evidence, then composes them with the canonical EQ through existing common device/parameter/undo/PDC authority. Key alignment, no-key release, complete structural publication and disabled-stage inactivity are explicit. New core/provider/composition files, corresponding tests/build wiring and page enable metadata are in this slice; native device-key routing still depends on DEV-05/MX-24, not analyzer taps. The #512 ops/main/changeset fence remains untouched. Exact controls are not advertised as working before their actual core/route exists.

**Parallel gate:** #542 CI is checked/fixed in its existing worktree, separately from this child. Any targeted parent fix is carried forward before publishing the child. Win reviews/merges in dependency order; no self-merge. Live work remains Claude's and trails development; new CPU timing remains deferred. No local desktop/native host/capture attempts.

## 13 November 2026 — native EQ integration ready for review; separate pages

**Item:** [#542](https://github.com/arieladi/Adi/pull/542), `mixview/eq-pages-20261113`, current `efa80a06026bcdd129a8585828b3e8dbb97aed0b` (integration `5a664f37`, portability follow-up `efa80a06`). Stacked on #541, which depends on #537/#540. Win reviews/merges in order and retargets the child after #541 lands. No self-merge. Parent #541 is fully green; this new head's CI is still running and must pass independently.

**Director decision implemented:** separate EQ/Carve/Focus pages; analysis suggestions apply manually. The real native EQ now uses the authored ADI-Q core directly (24 bands, 730 stable processing controls, mono/stereo float/double). Selected-band/section metadata, named menu choices, band-qualified parameter names, stable authored selection, common exact-runtime gestures/state/undo and callback-safe reconciliation are wired. Carve/Focus explicitly remain unavailable until actual cores/key routes exist; no fake Apply control or automatic correction. No Carve/Focus audio order is inferred; that director question remains pending. C/DS/G/full analyzer settings remain later work.

**Engine fixes/limits:** a pending Spectral latency offer no longer restores stale prepare-time band topology or freezes unrelated edits. Redundant same-format native Prepare without release retains live EQ history; explicit release/rate/block-size changes retain canonical preparation. No imported DSP math/precision changes. Side Link stays read-only/off, and linked-state recall is rejected atomically until compound edits join the common authority. Learned histograms stay transient as in the existing processor. Core preparation is owner/stopped; no concurrent same-Core prepare or universal full-rebuild equivalence is claimed. New Highway 1.4.0 dependency is pinned/provenanced; imported source stays distinct from owned strict-warning seams.

**Validation:** final Windows x64 Debug MSVC owned-code `/W4 /WX`, JUCE 9.0.5: **19 selected suites / 5,055 checks / zero failures**, including real EQ/direct-core audio, band-24 placement/common undo, physical PDC, both L stages, software-peer root destruction from End, and ordinary panel regressions. Four validators and diff checks pass; owned Clang frontend checks pass. Private authored-source proof: 666 structural + 25 lifecycle cohorts, 13,041,152 exact sample pairs, 419,712 exact learned readouts; 85 imported DSP/license files unchanged; callback C++ allocations/deallocations zero. Categories/limits are explicit in the private receipts, not public reference tests. The two causal regressions were reproduced before their fixes. Initial hosted GCC indentation and ARM empty-RAII warnings are fixed in efa80a06 with whitespace/`maybe_unused` only; final local gates were rerun. Hosted CI remains a separate gate.

**Delivery:** isolated hashed Debug review ZIP and targeted Claude handoff are complete. The director's private the private records and `EQ-DELIVERY-20261113.json` record exact source/exe/ZIP/log identities and limits. Current brief is workspace `outputs/ADI-Q-MixView-EQ-20261113-CLAUDE-RETEST.txt` (DAW native EQ, not an ADI-Q VST update). Standalone plug-ins, daily install, main checkout and #512 fences are untouched. Live work remains solely Claude's; CPU timing remains deferred. No local native editor/host, desktop input/capture or CPU collection occurred.

## 13 November 2026 — EQ page decision recorded; next native EQ slice

**Director decision:** separate **EQ / Carve / Focus pages**, with suggestions applied **manually**. Page changes are presentation only; a conflict display never writes audio settings. No Carve/Focus audio order is inferred. The private decision guide and standing roadmap now retain this answer; win assigns any ADR follow-up.

**Item / ownership:** `mixview/eq-pages-20261113`, an isolated checkout, based on `2a73e9e8` (#541). The seat connects the frozen authored ADI-Q core to the existing DeviceInstance/parameter/state/PDC authority, with practical selected-band pages and honest unavailable Carve/Focus content until actual engines and DEV-05/MX-24 routes exist. Own paths: new `mix/eq`, native EQ host/provider, generic appended descriptor band metadata, page presentation, matching tests/build dependency/provenance wiring. No reserved slots, automatic corrections, parallel undo authority or #512 fence edits. Win reviews/merges. The existing #541 review build is not modified by this slice.

**Prior CI follow-up:** #541 is now fully green. After both parent runs finished I retried the two macOS capacity cancellations, preserving successful legs. The replacement macOS JUCE/core job `131546282566` and GUI job `131546295036` both passed. Win may review/merge #537 → #540 → #541; no self-merge.

**Verification limits:** live tests trail development and stay with Claude. New CPU benchmarks remain deferred. This slice uses strict authored-engine comparisons and peerless component tests; no local host, native peer, desktop input or capture.

## 12 November 2026 — Clip → Limiter/functions/shared analysis ready for review

**Item:** [#541](https://github.com/arieladi/Adi/pull/541), `mixview/functions-analysis-20261112`, source `2a73e9e8d2191d91902b0d39e759933a756076bc`, ready for review. Depends on ready/green foundation #537 and tap leases #540; merge in order. Commits `ec06d919` (shared authored engines), `3ac60208` (functions/analysis/PDC), `e6aecba2` (portable noinline annotations), `2a73e9e8` (strict warnings on owned sources, not JUCE modules). Win reviews/merges; no self-merge. #512 fences remain untouched.

**Result:** independent Clip → Limiter pages implement Adi's explicit choice. Both stages can run together; page selection does not change audio/enables. Real device-area function tabs/+ and movement/removal share the authored chain, parameter feed, gesture and journal authority. Export policies use the same undoable configuration path without becoming automatable. Five basic analyzer modes share a process-wide demand-driven worker and MX-30 leases/cursors. Unported EQ/C/DS/G remain visibly unavailable; no dummy enabled DSP, reserved slots or fabricated device key route.

**Engine follow-up:** complete PDC plans now admit actual main/side/dry targets and node/arrival/event/tap metadata at a physical boundary before transport queries. Capacity changes retain old audio until real history is ready. Broad stress testing caught a ready-history oscillation: zero-delay fast paths forgot a ready ring while peers warmed. Positive warm targets now persist until the whole plan commits, with a deterministic staggered-history regression. This replaces immediate partial retap application and is explicit for engine review. Unsupported full-state recovery also requests proper reprepare; oversized state versions are rejected without mutation.

**Validation:** Windows x64 Debug MSVC `/W4 /WX`, JUCE 9.0.5: **37 selected suites / 7,626 checks / zero failures**; five repeated concurrent-worker runs pass. Four validators and diff check pass. Strict Clang frontend checks pass. Frozen authored L: 266 audio + 40 layout + 32 isolation cells, 18,546,688 exact sample pairs; all 596 raw files remain unchanged after the annotation fix. Frozen authored Clip: 144 cohorts / 507,840 exact channel-frames. Public Clip 415/0, serial composition 948/0, Session pipeline 17/0, PDC 88/0; real-backend headless split probe 25/0 with zero audio difference/callback allocations. No local native window, desktop input/capture, host UI or CPU timing was attempted.

**CI:** all regular/hardened Linux GCC/Clang/ARM64 and Linux UI gates pass. Windows jobs still trail this log. Both macOS jobs were cancelled before any runner/step started: hosted ARM64 runner capacity, not a compile failure. Exact 2a73 parents: core run `43251701094`, UI run `43251700984`; cancelled Mac jobs `131299100907`, `131299163497`. Individual retry returned HTTP 403 while parents are active. **After each parent finishes, retry only its cancelled Mac job and require its pass before merge**, preserving successful legs. Do not interpret a capacity cancellation as Mac verification.

**Delivery / pending:** an isolated hashed Debug functional review package and concise Claude brief are prepared privately; hashes, selected-test logs, scope and next questions are in the director's the private records and `FUNCTIONS-DELIVERY.json`. All live tests stay with Claude and trail development. CPU timing remains explicitly deferred. The sealed EQ component/orchestration extraction remains unregistered/untracked and absent from #541; its native factory/UI and learned-state serialization remain next work. The director's EQ/Carve/Focus/manual-proposal presentation question is unanswered; no automatic audio correction is inferred. C/DS/G/FUSE, full analyzer settings, Still placement and standalone wrapper migration are not claimed finished.

## 12 November 2026 — function and analysis continuation in progress

**Item:** `mixview/functions-analysis-20261112`, based on main `7aeeb098` plus the foundation PR #537 (merge `ae6cd083`). The director explicitly asks us to continue beyond the separate live gate. Win reviews and merges each slice. The private decision guide requested by the mission is written; it separates settled ADR-0237 rules from the remaining director choices. Adi chose independent **Clip → Limiter** pages and explicitly confirmed that both must run together, clipping first.

**Cross-lane ownership declared:** device-area opt-in function tabs/+ and same-chain movement in the window; one shared analyzer interface and process-wide demand-driven scheduler; MX-30 source leases, independent cursors, device input/output capture, timestamp/generation metadata and callback-safe storage retirement in `engine/{scope,session,graph,host,process}`; extraction of the existing ADI limiter arithmetic into a framework-neutral core and its normal DeviceInstance adapter. These paths are coordinated within the Mix View seat. No reserved processor slots, parallel edit authority or new control/catalog ops. The #512 fence remains untouched.

**Verification boundary:** strict headless audio comparisons and synthetic peerless component tests will precede review. Live interaction/capture remains exclusively with the director's Claude session. New CPU timing remains deferred at the director's request; this item will report structural demand and audio invariance checks, not invent CPU parity numbers. Private source snapshots, measurements and reference research remain outside public sources/tests.

**Review slices:** foundation [#537](https://github.com/arieladi/Adi/pull/537) and callback-pinned tap leases [#540](https://github.com/arieladi/Adi/pull/540) have green CI and are ready for win's review. #540 explicitly depends on #537. No self-merge. The extracted Clip/Limiter/serial components are committed as `ec06d919`; their native function/pages/analyzer/configuration integration remains under local verification before its own PR. Basic analyzer modes are Spectrum, Scope, Meters, Phase and Spectrogram; this is not a claim that all future reference-product modes exist.

**Audio evidence:** frozen authored ADI-L comparison: 266 extraction audio cells / 18,546,688 exact sample pairs, plus 40 layout and 32 meter-isolation checks. Frozen authored ADI-Clip comparison: 144 cohorts / 507,840 exact channel-frames. Public Clip and serial composition contracts pass 415 and 948 checks respectively. Real Session integration passes independent pages, common gesture/undo, summed latency and explicit export quality. The full offscreen shell passes 1,294 checks; no native peer or audio device was opened. These are extraction/regression results, not new third-party or CPU parity measurements.

**Engine follow-up being fixed before final integration review:** an owner-side in-place latency retap can currently change compensation targets and analyzer stamps midway through a physical callback, before a native child changes banks. A private deterministic split-callback probe reproduces this without desktop work. The follow-up will pin actual delay targets, growth admission and metadata coherently at the physical boundary, with no callback allocation/SDK queries. It is separate from the already-green tap lease PR. A prepared unsupported-state recovery edge is also under correction. Export still consumes stored project state; a future export UI must capture live runtime continuation explicitly.

**Next invariant core:** ADI-Q extraction remains unregistered work in `src/adi/mix/eq`; no dummy EQ processing tab is enabled. Its typed-controller/spectral and orchestration comparisons pass selected frozen-authored matrices. The native EQ/unmasking presentation choice is pending with the director; C/DS/G/FUSE and the larger analyzer product feature set are not claimed implemented.

## 12 November 2026 — actual-chain foundation ready for review

**Item:** `mixview/stack-foundation-20261112`, current commit `cae3c61e3a1ac18c3ff7754355290619c8dbf179`, based on main `e5baefec`. Draft PR [#537](https://github.com/arieladi/Adi/pull/537). Initial foundation `b7c0fe05`, portable publication follow-up `a62f8257`, explicit UI CI coverage `9db1d58a`, explicit coordinate casts for Clang `cae3c61e`. The director directly authorized connecting StackModel to actual chains and visible Mix View, followed by shared analysis/processing integration. This is the foundation slice, not the completed unified DSP feature. Win reviews/merges; no self-merge.

**Result:** STACK now uses the existing StackModel for actual-chain rows, geometry, hit/navigation/selection, filtering, pins and retained targets. It reuses DevicePanelView and ProjectDocument/ParamOps for device commands, parameter editing, gesture ownership and undo. Runtime-backed publication stamps prevent stale callbacks from writing a replacement. Hide/teardown/rejection/deletion/format interruption and cancelled asynchronous menus have explicit cleanup. Empty tracks receive no reserved processor slots. Compatible Main spectrum consumers in Info and OVERVIEW share one document-owned analysis result/transform and release their demand when hidden.

**Cross-lane paths for review:**

- Engine: one hunk in `src/adi/engine/param_ops.cpp`, checking detached runtimes before their own capture barrier so an interrupted owner can retire. No graph, routing, host SDK or DSP algorithm change.
- Window: `src/adi/ui/{stack_model,parameter_feed,project_document,shared_analysis}`, visible `ui_stack`, `ui_devices`, shell/Mix View/floating/main wiring and their tests. There is one model/runtime and one existing edit authority, not a parallel device chain or undo mechanism.
- Win/build: stable JUCE 9.0.5 pin (`d60b7a78`) in fetch script/docs, isolated CMake dependency-root support and matching fake-peer signature. `.github/workflows/ui.yml` explicitly builds/runs StackModel/shared-analysis/shared-spectrum alongside existing UI tiers. Shared main checkout/dependencies remain unchanged.
- The #512 fence (`ops*`, `changeset*`, `src/main.cpp`) is untouched. Native reference observations and private research stay outside public sources/tests.

**Validation:** Windows x64 MSVC 19.44 `/W4 /WX` Debug and Release compile. Ten targeted suites pass **3,549 checks**; focused null-native-peer Mix View subset **117/117**. Full offscreen UI goldens pass unchanged. Schema, ops, Pd and CI validators pass; diff check clean. Compiled source-copy mutants reject stale stamp admission, false latency, stale End filtering and cancelled-menu actions. No product source was planted. Full repository `tools/test_all.sh` and the remote CI matrix have not been run locally; draft remains for cross-lane review/CI and the separate live gate.

**Portability follow-up:** hosted macOS compilation caught libc++ lacking atomic shared_ptr specialization. Result publication now uses a separate UI/worker mutex and pointer swap. No FFT, old-result destruction or audio callback takes that critical section. Independent lock-order review passed, and all ten targeted local suites passed again after the fix (3,549 checks). Remote CI is still a separate gate.

Strict Clang also required two explicit int-to-float mouseExit bounds conversions; semantics are unchanged. Focused Mix View reran 117/117 after that patch; independent warning audit found no further conversion/sign issue. Final Release package pins the current commit. Remote CI/live acceptance remain separate.

**Live/CPU state:** review package and a targeted handoff are prepared privately for the director's Claude tester. Codex has not locally launched ADI.exe, a native test host or popup, attempted desktop input/capture, or collected CPU measurements. The repository's existing hosted Windows CI has its own hidden-window smoke; that does not establish live interaction acceptance. CPU collection remains explicitly deferred by the director. Headless/offscreen correctness is not called live parity or CPU parity.

**Next integration / non-blocking boundaries:**

- The first analyzer targets Main only: PostFader/Mid/Hann/1024 with stereo peak/RMS. Selected-track following, comparisons, the complete shared analyzer suite and movable native function tabs remain later slices.
- MX-30 owns device tap endpoints, independent gapless cursors, negotiated history and safe tap-storage retirement. Current ScopeTap can retain an old sample rate; the adapter honestly marks that source unavailable after a rate change. Safe replacement is needed before removing this restriction.
- DEV-05/MX-24 own compensated device-specific key routes. The UI does not simulate missing sidechains.
- Unified opt-in EQ/unmasking, C, DS, G and L/Clip cores must enter the same Session nodes and parameter/undo authority. Still placement remains a director guide question. Existing ordinary devices continue processing unchanged through Session/Graph.
- Pins/search/row-size state currently lasts for the window lifetime. Native editor and visual interaction acceptance await Claude's report before claiming them verified.

Public implementation scope is in `docs/MIX-VIEW-FOUNDATION.md`. Next work follows ADR-0236/0237, keeping analyzer demand separate from processing lifetime and introducing no reserved slots.
