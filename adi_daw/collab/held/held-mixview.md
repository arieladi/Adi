# Held Mix View work, gathered into one chain

**This chain lands only when the director says so.** Nothing here is a pull request. The chain is
`held/mixview-chain`; this log lives on `held/held-mixview-log`.

**The mission.** Since late November the Mix View seat kept building while landing was held ("lands after
3 December"). Its finished work sits on `origin/mixview/*`, unmerged. This log gathers it into one chain on
top of the newest landing stack, proves the chain builds and passes as one program, and says what it adds.
The Mix View is the director's own area: the chain changes no look and no behaviour of it; it only joins.

**How to go on (for the next session).** Read this file top to bottom. The survey is done (step 1). The
state of steps 2-4 is in "Progress" below, newest last. BASE is `origin/land/basics-1128c` until it is an
ancestor of `origin/main`, then `origin/main`. Merge the newest BASE into the chain before each push.
Builds and tests run only through `bash /d/adibuild-tools/slot.sh` (slot 13).

## Step 1: the survey (28 November 2026)

Method. `git branch -r --no-merged origin/main`, kept `origin/mixview/*` (94 branches besides
`mixview/log`). For each: its head; every other mixview branch that contains it (`git merge-base
--is-ancestor`); `git cherry origin/main` (is any commit already on main by patch); and `git cherry` against
the stack top (is anything outside the top). The seat's own log (`mixview/log`, `collab/mixview.md`) gives
the order and calls each of these "PR requested, lands after 3 December".

What it found.
- **One long stack.** 90 of the branches are one linear stack whose top is
  `mixview/mx30-parked-realtime-burst-lands-after-2026-12-03` (`b87b53a82`, 99 own commits, none on main
  by patch). Every other stack branch, including the before-date ones (`df-m34-*`, `df-m65-*`, `df-m69-*`)
  and the seat's review joins (`review-scope-*`, `review-nov25-main-*`), is an ancestor of it. Taking the
  top takes them all.
- `mixview/review-cumulative-20261122`: its five own commits are in the top by patch; only its merges of
  an older main differ. Contained.
- `mixview/matched-filters-20261113` (closed PR #558): the matched filter section, its doc and its tests
  are on main already. The branch is one stale merge commit that only conflicts. Already on main.
- `mixview/param-replacement-receipt-regression-20261119`: its one test is on main already, reworded
  (`tests/test_param_live_replay.cpp`, "Replacement probe"). Already on main.
- `mixview/offline-suspension-regression-20261119`: two test-only commits on
  `tests/test_mix_render_intent.cpp` (a normal-quality offline session still invokes a zero-tail device on
  a silent block). Not on main. To take.
- Out of the mission but seen: `origin/land/mixview-1122` and `origin/land/mixview-1123` are win's old
  landing stacks; what they hold beyond main is other seats' work (engine, window, clones), none of it
  the Mix View seat's. Not touched here.

To take, in order: `offline-suspension-regression-20261119` (oldest), then the top
`mx30-parked-realtime-burst-lands-after-2026-12-03`.

A trial merge (`git merge-tree`) of the top into BASE conflicts in ten files: `src/adi/ui/shared_analysis.*`,
`src/juce/ui_analysis.*`, `src/juce/ui_mix_analysers.cpp`, `src/juce/ui_mix_channels.cpp` and the tests
`test_mix_focus.cpp`, `test_mix_pipeline_document.cpp`, `test_shared_analysis.cpp`,
`test_ui_shared_analysis.cpp`. The cause on BASE's side is mainly the "Analysers 1-6" series (27 November:
measurement core, spectrum, meter, goniometer, third-octave bands, colour waveform) and SC-112 (the Info
box's own analyser), which reworked the same analyser files after the stack last merged main.

| # | Branch | Head | Top of which stack | State |
|---|---|---|---|---|
| 1 | `mixview/matched-filters-20261113` | `3c767304a` | own top (one merge commit, PR #558 closed) | already on main (MatchedSection.* and its tests are on main; the stale merge only conflicts) |
| 2 | `mixview/offline-suspension-regression-20261119` | `13ed31756` | own top (2 test-only commits) | to take |
| 3 | `mixview/param-replacement-receipt-regression-20261119` | `eb41c1210` | own top (1 test-only commit) | already on main (the same test, reworded, is in tests/test_param_live_replay.cpp on main) |
| 4 | `mixview/focus-scope-preview-lands-after-2026-12-03` | `ac054b105` | `mx30-parked-realtime-burst` stack | contained in the top |
| 5 | `mixview/focus-scope-phase-lands-after-2026-12-03` | `7531cc42b` | `mx30-parked-realtime-burst` stack | contained in the top |
| 6 | `mixview/mx12-strip-producer-lands-after-2026-12-03` | `430c73111` | `mx30-parked-realtime-burst` stack | contained in the top |
| 7 | `mixview/dev22-spectrum-lands-after-2026-12-03` | `57f628c7e` | `mx30-parked-realtime-burst` stack | contained in the top |
| 8 | `mixview/focus-scope-delta-spectrum-lands-after-2026-12-03` | `3b057fe9d` | `mx30-parked-realtime-burst` stack | contained in the top |
| 9 | `mixview/mx30-strip-audio-lands-after-2026-12-03` | `74cd68d22` | `mx30-parked-realtime-burst` stack | contained in the top |
| 10 | `mixview/df-m55-spectrum-layouts-lands-after-2026-12-03` | `0619d04fc` | `mx30-parked-realtime-burst` stack | contained in the top |
| 11 | `mixview/df-m55-info-spectrum-scale-lands-after-2026-12-03` | `ffeebdcb0` | `mx30-parked-realtime-burst` stack | contained in the top |
| 12 | `mixview/df-m55-spectrum-cursor-lands-after-2026-12-03` | `e4db45806` | `mx30-parked-realtime-burst` stack | contained in the top |
| 13 | `mixview/mx30-strip-idle-lands-after-2026-12-03` | `8e03d995a` | `mx30-parked-realtime-burst` stack | contained in the top |
| 14 | `mixview/focus-align-noise-lands-after-2026-12-03` | `6c13c217f` | `mx30-parked-realtime-burst` stack | contained in the top |
| 15 | `mixview/mx13-device-tap-stamps-lands-after-2026-12-03` | `e0f7030ca` | `mx30-parked-realtime-burst` stack | contained in the top |
| 16 | `mixview/mx36-stack-demand-lands-after-2026-12-03` | `d9c18ca6b` | `mx30-parked-realtime-burst` stack | contained in the top |
| 17 | `mixview/phase-bands-lands-after-2026-12-03` | `084138bcc` | `mx30-parked-realtime-burst` stack | contained in the top |
| 18 | `mixview/mx36-folded-browser-search-lands-after-2026-12-03` | `37115afc2` | `mx30-parked-realtime-burst` stack | contained in the top |
| 19 | `mixview/mx21-pinned-spectrum-lands-after-2026-12-03` | `03d3bdaa3` | `mx30-parked-realtime-burst` stack | contained in the top |
| 20 | `mixview/df-m55-main-plr-lands-after-2026-12-03` | `ad7cd6b84` | `mx30-parked-realtime-burst` stack | contained in the top |
| 21 | `mixview/mx13-full-rate-align-lands-after-2026-12-03` | `910fbc138` | `mx30-parked-realtime-burst` stack | contained in the top |
| 22 | `mixview/mx21-heard-pinned-spectrum-lands-after-2026-12-03` | `d1c965d19` | `mx30-parked-realtime-burst` stack | contained in the top |
| 23 | `mixview/mx21-staggered-spectrum-lands-after-2026-12-03` | `e1dbedf95` | `mx30-parked-realtime-burst` stack | contained in the top |
| 24 | `mixview/mx21-overview-spectrum-profile-lands-after-2026-12-03` | `a6ef94c95` | `mx30-parked-realtime-burst` stack | contained in the top |
| 25 | `mixview/mx21-spectrum-statistics-lands-after-2026-12-03` | `ef540f239` | `mx30-parked-realtime-burst` stack | contained in the top |
| 26 | `mixview/mx21-wide-spectrum-cache-lands-after-2026-12-03` | `96b95c700` | `mx30-parked-realtime-burst` stack | contained in the top |
| 27 | `mixview/mx21-extended-log-grid-lands-after-2026-12-03` | `9b3efbdef` | `mx30-parked-realtime-burst` stack | contained in the top |
| 28 | `mixview/mx21-pinned-spectrum-realtime-average-lands-after-2026-12-03` | `7e4af7358` | `mx30-parked-realtime-burst` stack | contained in the top |
| 29 | `mixview/mx21-spectrum-cumulative-average-lands-after-2026-12-03` | `4f353e655` | `mx30-parked-realtime-burst` stack | contained in the top |
| 30 | `mixview/mx21-spectrum-two-second-balance-lands-after-2026-12-03` | `31643cb0b` | `mx30-parked-realtime-burst` stack | contained in the top |
| 31 | `mixview/mx21-spectrum-realtime-sigma-lands-after-2026-12-03` | `137caa1f9` | `mx30-parked-realtime-burst` stack | contained in the top |
| 32 | `mixview/mx21-spectrum-parked-balance-lands-after-2026-12-03` | `c444e88f1` | `mx30-parked-realtime-burst` stack | contained in the top |
| 33 | `mixview/mx21-spectrum-parked-rt-average-lands-after-2026-12-03` | `99e9f5ef6` | `mx30-parked-realtime-burst` stack | contained in the top |
| 34 | `mixview/mx21-spectrum-crest-lands-after-2026-12-03` | `f93fee581` | `mx30-parked-realtime-burst` stack | contained in the top |
| 35 | `mixview/mx30-ordinary-window-hop-phase-lands-after-2026-12-03` | `b97920435` | `mx30-parked-realtime-burst` stack | contained in the top |
| 36 | `mixview/mx21-spectrum-sample-statistics-lands-after-2026-12-03` | `9c29f0ddb` | `mx30-parked-realtime-burst` stack | contained in the top |
| 37 | `mixview/mx21-spectrum-statistics-reset-lands-after-2026-12-03` | `537eef816` | `mx30-parked-realtime-burst` stack | contained in the top |
| 38 | `mixview/mx30-parked-realtime-hop-lands-after-2026-12-03` | `b54719d3a` | `mx30-parked-realtime-burst` stack | contained in the top |
| 39 | `mixview/mx21-spectrum-true-peak-lands-after-2026-12-03` | `eeae6f132` | `mx30-parked-realtime-burst` stack | contained in the top |
| 40 | `mixview/mx30-parked-phase-bands-lands-after-2026-12-03` | `5d86afa90` | `mx30-parked-realtime-burst` stack | contained in the top |
| 41 | `mixview/review-scope-lands-after-2026-12-03` | `5d86afa90` | `mx30-parked-realtime-burst` stack | contained in the top |
| 42 | `mixview/mx21-phase-band-spatial-lands-after-2026-12-03` | `5a931f0bc` | `mx30-parked-realtime-burst` stack | contained in the top |
| 43 | `mixview/mx13-focus-scope-cursor-lands-after-2026-12-03` | `672b97bd9` | `mx30-parked-realtime-burst` stack | contained in the top |
| 44 | `mixview/mx13-full-bar-after-locate-lands-after-2026-12-03` | `e00acfe6d` | `mx30-parked-realtime-burst` stack | contained in the top |
| 45 | `mixview/mx21-spectrum-cursor-scales-lands-after-2026-12-03` | `cebc5e3c4` | `mx30-parked-realtime-burst` stack | contained in the top |
| 46 | `mixview/mx13-focus-scope-region-lands-after-2026-12-03` | `756ab68b8` | `mx30-parked-realtime-burst` stack | contained in the top |
| 47 | `mixview/mx30-parked-spectrogram-hop-lands-after-2026-12-03` | `7fa1d1cb8` | `mx30-parked-realtime-burst` stack | contained in the top |
| 48 | `mixview/mx13-focus-scope-freeze-lands-after-2026-12-03` | `8243e08d9` | `mx30-parked-realtime-burst` stack | contained in the top |
| 49 | `mixview/mx21-spectrogram-frequency-scales-lands-after-2026-12-03` | `a31c571e3` | `mx30-parked-realtime-burst` stack | contained in the top |
| 50 | `mixview/mx21-spectrogram-cursor-lands-after-2026-12-03` | `d6eff615e` | `mx30-parked-realtime-burst` stack | contained in the top |
| 51 | `mixview/mx21-spectrogram-png-lands-after-2026-12-03` | `44236030e` | `mx30-parked-realtime-burst` stack | contained in the top |
| 52 | `mixview/mx21-spectrogram-levels-lands-after-2026-12-03` | `85813f15d` | `mx30-parked-realtime-burst` stack | contained in the top |
| 53 | `mixview/mx21-spectrogram-ruler-lands-after-2026-12-03` | `578e321f2` | `mx30-parked-realtime-burst` stack | contained in the top |
| 54 | `mixview/mx13-difference-rms-lands-after-2026-12-03` | `a6b79cad5` | `mx30-parked-realtime-burst` stack | contained in the top |
| 55 | `mixview/mx13-captured-true-peak-lands-after-2026-12-03` | `b272da1cd` | `mx30-parked-realtime-burst` stack | contained in the top |
| 56 | `mixview/mx13-musical-window-lands-after-2026-12-03` | `5c88abf07` | `mx30-parked-realtime-burst` stack | contained in the top |
| 57 | `mixview/mx13-scope-vertical-zoom-lands-after-2026-12-03` | `087992d94` | `mx30-parked-realtime-burst` stack | contained in the top |
| 58 | `mixview/mx13-scope-cursor-grid-edge-lands-after-2026-12-03` | `523f1bdba` | `mx30-parked-realtime-burst` stack | contained in the top |
| 59 | `mixview/df-m34-skipped-audio-slices` | `0deaa3d0e` | `mx30-parked-realtime-burst` stack | contained in the top |
| 60 | `mixview/mx13-scope-musical-grid-lands-after-2026-12-03` | `66f69b6fd` | `mx30-parked-realtime-burst` stack | contained in the top |
| 61 | `mixview/df-m34-limiter-clock` | `1db80412c` | `mx30-parked-realtime-burst` stack | contained in the top |
| 62 | `mixview/mx13-scope-vertical-pan-lands-after-2026-12-03` | `39a52b470` | `mx30-parked-realtime-burst` stack | contained in the top |
| 63 | `mixview/df-m34-eq-clock` | `d4a705aa9` | `mx30-parked-realtime-burst` stack | contained in the top |
| 64 | `mixview/df-m65-clip-input-step-pin` | `3ad269c4f` | `mx30-parked-realtime-burst` stack | contained in the top |
| 65 | `mixview/df-m69-focus-allocation` | `29e4e6b4f` | `mx30-parked-realtime-burst` stack | contained in the top |
| 66 | `mixview/mx13-scope-dense-grid-lands-after-2026-12-03` | `c984222a6` | `mx30-parked-realtime-burst` stack | contained in the top |
| 67 | `mixview/mx13-scope-seek-fence-lands-after-2026-12-03` | `792cf7e64` | `mx30-parked-realtime-burst` stack | contained in the top |
| 68 | `mixview/review-nov25-main-lands-after-2026-12-03` | `a21269b42` | `mx30-parked-realtime-burst` stack | contained in the top |
| 69 | `mixview/mx13-scope-sixteenth-note-lands-after-2026-12-03` | `c8168ec83` | `mx30-parked-realtime-burst` stack | contained in the top |
| 70 | `mixview/mx13-scope-long-windows-lands-after-2026-12-03` | `749f2dc85` | `mx30-parked-realtime-burst` stack | contained in the top |
| 71 | `mixview/mx13-scope-time-viewport-lands-after-2026-12-03` | `d4bedd0fa` | `mx30-parked-realtime-burst` stack | contained in the top |
| 72 | `mixview/mx13-scope-fixed-time-lands-after-2026-12-03` | `0964711f4` | `mx30-parked-realtime-burst` stack | contained in the top |
| 73 | `mixview/mx13-scope-stop-hold-lands-after-2026-12-03` | `1c6e4747e` | `mx30-parked-realtime-burst` stack | contained in the top |
| 74 | `mixview/mx21-spectrogram-gamma-lands-after-2026-12-03` | `5a2384d69` | `mx30-parked-realtime-burst` stack | contained in the top |
| 75 | `mixview/mx13-scope-one-layer-lands-after-2026-12-03` | `339cf8259` | `mx30-parked-realtime-burst` stack | contained in the top |
| 76 | `mixview/mx13-scope-layer-channels-lands-after-2026-12-03` | `832dfcabb` | `mx30-parked-realtime-burst` stack | contained in the top |
| 77 | `mixview/mx21-spectrogram-heard-grid-lands-after-2026-12-03` | `b04acc606` | `mx30-parked-realtime-burst` stack | contained in the top |
| 78 | `mixview/mx21-spectrogram-matched-columns-lands-after-2026-12-03` | `1854d60dc` | `mx30-parked-realtime-burst` stack | contained in the top |
| 79 | `mixview/mx21-spectrogram-comparison-demand-lands-after-2026-12-03` | `767187cd1` | `mx30-parked-realtime-burst` stack | contained in the top |
| 80 | `mixview/mx21-spectrogram-delta-view-lands-after-2026-12-03` | `4c71e15a1` | `mx30-parked-realtime-burst` stack | contained in the top |
| 81 | `mixview/mx21-spectrogram-delta-cursor-lands-after-2026-12-03` | `62e733e14` | `mx30-parked-realtime-burst` stack | contained in the top |
| 82 | `mixview/mx21-spectrogram-delta-png-lands-after-2026-12-03` | `833d19674` | `mx30-parked-realtime-burst` stack | contained in the top |
| 83 | `mixview/mx21-spectrogram-delta-ruler-lands-after-2026-12-03` | `170c3f543` | `mx30-parked-realtime-burst` stack | contained in the top |
| 84 | `mixview/mx21-spectrogram-delta-scales-lands-after-2026-12-03` | `acd4234be` | `mx30-parked-realtime-burst` stack | contained in the top |
| 85 | `mixview/mx21-spectrogram-delta-sweep-lands-after-2026-12-03` | `baf5f4360` | `mx30-parked-realtime-burst` stack | contained in the top |
| 86 | `mixview/mx21-compare-retained-paths-lands-after-2026-12-03` | `8cb89d83e` | `mx30-parked-realtime-burst` stack | contained in the top |
| 87 | `mixview/mx21-spectrum-block-size-lands-after-2026-12-03` | `a41a259b9` | `mx30-parked-realtime-burst` stack | contained in the top |
| 88 | `mixview/mx21-spectrum-overlap-lands-after-2026-12-03` | `9c08ae51e` | `mx30-parked-realtime-burst` stack | contained in the top |
| 89 | `mixview/mx21-spectrum-smoothing-lands-after-2026-12-03` | `4b642b9b2` | `mx30-parked-realtime-burst` stack | contained in the top |
| 90 | `mixview/mx30-park-cumulative-average-lands-after-2026-12-03` | `52bb11779` | `mx30-parked-realtime-burst` stack | contained in the top |
| 91 | `mixview/mx30-parked-burst-fence-lands-after-2026-12-03` | `7301a65a8` | `mx30-parked-realtime-burst` stack | contained in the top |
| 92 | `mixview/mx30-parked-scope-average-lands-after-2026-12-03` | `3167cecd4` | `mx30-parked-realtime-burst` stack | contained in the top |
| 93 | `mixview/mx30-parked-realtime-burst-lands-after-2026-12-03` | `b87b53a82` | **the top** of the one long stack | to take (carries every row below it) |
| 94 | `mixview/review-cumulative-20261122` | `678bfbe14` | own top (review join of main 73e26e4aa) | contained in the top by content (its 5 commits are in the top stack; only its own main merges differ) |

No branch is "abandoned by its seat" or "unclear": the seat's log requests every stack row.

## Progress

- 28 Nov, round 1: survey written and pushed.
- 28 Nov, round 1: `held/mixview-chain` created from BASE `origin/land/basics-1128c` (`3543a9a74`'s
  successor on the landing stack) and pushed. Two merges:
  1. `offline-suspension-regression-20261119`: one include line collided; both kept.
  2. the top `mx30-parked-realtime-burst-lands-after-2026-12-03`: ten files, 70 hunks. How each was
     resolved is in the merge commit's message. The parts a reader should know:
     - **Output bits.** Both sides had taken the same `SharedAnalysis::Output` bits (256, 512, 1024) for
       different outputs. The landed ones (FilledSpectrum, PeriodScope, ColourWaveform) keep their values;
       the seat's six (Strip, DeviceBlockAverage, PhaseBands, CumulativeAverage, RealtimeSigma,
       PhaseBandDetail) moved to 2048..65536. Nothing stores these bits, so no file format changes.
     - **Mode enum.** The landed modes keep their numbers; the seat's two new modes follow them.
     - **Renamed helper.** Each side renamed `Domain::ensureSpectrum` (to `prepareOutputs` and to
       `ensureOutputPlans`). Both stay: `prepareOutputs` builds the period capture and calls
       `ensureOutputPlans`.
     - **The Spectrum page (the one real collision).** On 27 November the director's analysers queue made
       the Analysers page's Spectrum the Filled Spectrum with four traces (Realtime Average, Realtime
       Maximum, Held Maximum, Average). Before that, while held, the seat had grown the same page's trace
       box into six line traces (Realtime, Realtime Average, Cumulative Average, Realtime Sigma, Realtime
       Maximum, Cumulative Maximum) with Stack pins. The two cannot share one box: the names collide and
       each side's tests count the box's items. The chain keeps the director's newer Spectrum page as it
       landed; the seat's line traces stay in `SharedSpectrumView` (and its Info and detached analyser
       modes) but are not offered on that page, and the pin control shows only where it can pin. The
       seat's page tests that select a line trace from that box are expected to fail for this reason; the
       check will name them. This is question A below.
     - **Phase.** The landed goniometer trail draws inside the seat's phase-band layout (bands below, the
       stereo picture above).
     - **Channel browser.** The landed "name as the header shows it" (walk-B B3) is also what the seat's
       folded search matches.
- 28 Nov, round 1: validators pass; chain pushed at the stack merge (`d24d19bb`); check `all` in slot 13
  failed to compile one file: `shared_analysis.cpp(914)`, MSVC C4458, the seat's strip code declared a
  local `period` that now hides the landed period-scope member `Domain::period`. Renamed `stripPeriod`.
- 28 Nov, round 1: reading the seat's tests showed the A2 resolution above would fail the whole of the
  seat's pinned-spectrum suite in `test_ui_mix_functions.cpp` (about a hundred checks), because that page
  no longer had its line traces. Changed the chain to **A1**: a page "Line Spectrum" appended after
  Waveform carries the seat's six line traces and pins exactly as it built them; the Spectrum page is the
  Filled Spectrum, unchanged. The one trace selector lists the page's own traces and each page keeps its
  choice. The seat's tests now address `Page::LineSpectrum` (only that name changed in them), and the
  landed page-count check counts the new page. Commit `held-mixview: the seat's line traces on a Line
  Spectrum page beside the Filled Spectrum`.
- 28 Nov, round 1: `origin/land/basics-1128c` landed on main (#838), so **BASE is now `origin/main`**;
  merged it into the chain (documentation only). Second check `all` started in slot 13.
- 28 Nov, round 1: **second check, `all`, at `9e4eb807`: core and ui both build** (MSVC Debug, /WX).
  Core: 286 of 291 pass. UI (the four window suites): shell, seams and window_state pass; document fails
  one check. The failures, one by one:
  - `adi_ui_shared_analysis_tests` (`tests/test_shared_analysis.cpp`), 2 of 1434: "a stamped broadband
    copy 137 samples late suggests an advance of 2.8541667 ms without allocations" and "unequal physical
    heads resolve the same exact heard interval without allocation or tap mutation". **The seat's own**:
    its full-rate Align (MX-13, `src/adi/ui/focus_lag.hpp`) held its wide capture in an empty
    `std::vector`, which in an MSVC Debug build already allocates (the seat proved it in Release). The fix
    is plain and is on the chain: a `unique_ptr` made only when the capture outgrows the local array.
  - `adi_mix_render_intent_tests`, 2 of 25: "the real export session invokes a zero-tail device on a
    silent block" and "the normal-quality offline session invokes a zero-tail device on a silent block".
    **The seat's own and deliberate**: `offline-suspension-regression-20261119` is a reproduction the seat
    wrote for the Engine seat (its log, 19 Nov: "demonstrates the ADR-0043 d5 gap ... not a landing
    candidate by itself"). `Graph::runNode` suspends a zero-tail device on silence whatever the render
    context. The fix is the Engine seat's (Graph needs an offline flag apart from export quality), not
    plain, so it is left; see question B. My survey's "to take" for this branch was too quick.
  - `adi_agent_log_tests` (three checks on child processes writing one timeline), `adi_link_playback_tests`
    (two "prepares off callback" checks), `adi_library_tests` (timeout at 120 s) and `adi_ui_document_tests`
    ("opening yields to the owner between slow plug-in loads"): the chain changes none of their code; all
    four are timing or process checks on a build PC shared by about twenty sessions. Rerun in the next
    check to be sure.
  - **Not run here:** `adi_ui_shared_spectrum_tests` (`tests/test_ui_shared_analysis.cpp`) and
    `adi_ui_mix_functions_tests` (`tests/test_ui_mix_functions.cpp`). `slot.sh` builds and runs only the four
    window suites, and these two are the suites the merge touched most (the Spectrum pages, pins, Phase,
    Filled Spectrum). They are proved only by CI's `ui.yml` (Linux, no display) or a build that names
    them. This is the chain's largest unproved part.
- 28 Nov, round 1: focus lag fix pushed; third check `core` with the five failing suites as filter started.

## Step 4: the account

What the chain adds to ADI, as a user would notice it (all in the Mix View; nothing outside it changes
what a user sees):
- **Line Spectrum page** (new entry at the end of the Analysers page list): the seat's line spectrum with
  six traces (Realtime, Realtime Average, Cumulative Average, Realtime Sigma, Realtime Maximum, Cumulative
  Maximum), Stack pins overlaid on the primary, block size, overlap and display smoothing choices, and a
  statistics strip (integrated RMS, held crest, held sample peak and clip counts, optional true peak, two
  second stereo balance, Reset).
- **Spectrum layouts** (Wide, Detail, Extended, Device) followed by the Info box and Compare; a spectrum
  cursor with frequency, pitch and level on every scale, the Device Spectrum included.
- **Spectrogram**: five frequency scales, a frequency ruler, floor/ceiling and contrast, a cursor reading,
  PNG export of the displayed history, and a Pre/Post comparison (signed change between a channel's input
  and output, with its own Sweep, ruler, scales, cursor and PNG).
- **Phase**: per-band correlation, width and pan under the stereo picture.
- **Main analysis**: peak-to-loudness ratio (PLR).
- **Mixer strips**: a small shared spectrum and scope summary per visible strip, idle when not shown.
- **Focus Scope**: capture by beat, bar, two or four bars, 1/16 note or 85/170/340 ms; the beat and bar
  grid at heard positions; time zoom and pan, vertical zoom and pan; a cursor and a measured region; Freeze
  and a held picture on Stop; independent layer channels; polarity and timing previews; Difference
  spectrum, Difference RMS and captured true peak; full-rate Align at high sample rates.
- **Channel browser**: search folds accents and case.
- **Native EQ and Limiter**: their clocks keep time while the graph suspends them in silence (DF-M34), so
  a wake does not jump.
- Underneath, many "parked" analysis paths: stopped or silent sources stop costing worker passes and
  resume on the same hop grid.

Left out, and why:
- `mixview/param-replacement-receipt-regression-20261119`, `mixview/matched-filters-20261113`: already on
  main by another route.
- `mixview/review-cumulative-20261122`: its five commits are in the stack top; its own merges of an older
  main add nothing.
- Nothing of the stack was left out. The two red render-intent checks are carried, not fixed (question B).

What the director must decide before the chain lands: questions A and B below, and the picture of the Line
Spectrum page (ADR-0243). The seat's own open questions (its log's "FOR THE DIRECTOR") are unchanged by
this chain.

## For the director

**A. The Analysers Spectrum page.** Your 27 November analysers queue made it the Filled Spectrum (four
traces). The Mix View seat, held since 22 November, had made the same page a line spectrum with six traces
(including Realtime Sigma and Cumulative Average) and Stack pins overlaid. Only one can be that page.
- A1: the Filled Spectrum stays the page; the seat's line traces move to their own page beside it (a new
  page entry, its picture shown to you first).
- A2: the Filled Spectrum stays the page; the line traces stay out of the Analysers page (they remain in
  the Info box and the detached analyser).
- A3: the seat's line traces become the page again, and the Filled Spectrum moves elsewhere.
Recommendation: A1. It keeps your newer word and all of the seat's measured work. The chain is built as
A1 now (page label "Line Spectrum", the last entry of the page list, today's look for its controls); its
picture is owed to you before it lands (ADR-0243). A2 would leave about a hundred of the seat's checks
failing. One check fails under every choice: the seat's "the All spectrum tile uses the selected layout"
(DF-M55), because your All page now shows the Filled Spectrum tile, which has one fixed grid. Reply "A1",
"A2" or "A3".

**B. The seat's red reproduction for the Engine.** `offline-suspension-regression-20261119` adds two checks
that fail until the Engine stops suspending zero-tail devices in export and in normal-quality offline
renders (ADR-0043 d5).
- B1: keep the two checks in the chain; it lands only after the Engine's fix joins it.
- B2: take the reproduction out of this chain (a revert of its merge) and hand it to the Engine seat's queue.
Recommendation: B2. The Mix View work should not wait on an Engine fix, and the seat itself called the
branch "not a landing candidate by itself". Reply "B1" or "B2".
