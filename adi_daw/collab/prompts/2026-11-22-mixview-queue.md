# mixview — standing queue, 22 November 2026

This is the whole queue from win, to work until your usage limit. It replaces every shorter list you had.

## How to work

- Fetch main first, and read the top entry of `collab/win.md` and the Mix View rows of `docs/DEFERRED.md`.
- Work the items top to bottom. One branch per item (`mixview/<topic>`), cut from origin/main unless the item says to stack.
- Write the test first where the item names one. Build on MSVC (/W4 /WX) and run the named tests.
- Push, log the entry in `collab/mixview.md` on `mixview/log`, end it with "PR requested", and start the next item in the same turn.
- An item blocked on win, another seat or a decision gets one line in your log; take the next item.
- A question for the director goes under "FOR THE DIRECTOR" at the top of your log with your recommendation. Then continue.
- Stay in your own paths (`collab/README.md`). Needs in other seats' paths go in your log's needs table.
- Until 3 December, ADR-0248 d2 holds: the usable session first. Work meant for after that date goes on branches marked "lands after 3 December".
- When this list is empty, take the next unbuilt Mix View row of `docs/PLAN.md` in milestone order. Never stop because "nothing names this lane". Stop only at your usage limit or a broken environment.

## Queue

### Before 3 December

1. **STANDING-land: answer reviews, keep the joined branch current** (an event item: it fires when win posts findings or lands)
   Answer every finding win posts on your open PRs or requested branches, on those branches.
   After each landing window, merge main into `mixview/review-cumulative-20261122`, rerun the gates, log any conflicts.
   Once now: remove the machine storage detail from your 22 November log entry (ADR-0248 d10).
   Test: the joined branch merges main cleanly; its CTests and the 906 Mix View / 329 Arrangement / 7 Appearance component checks pass.

2. **DF-M62: sign-compare under libstdc++ 12**
   `std::bit_width` returns `size_t` there. Loop as `std::size_t` with a `static_cast`, as `RealTransform.h:57` already does.
   Files: `src/adi/mix/limiter/Dsp/FloatRealTransform.h:61`.
   Test: full tree with `-DADI_WERROR=ON` on GCC 12.2; CI Linux and macOS legs green; MSVC /W4 /WX unchanged; limiter suites unchanged.
   This fix is yours. If you have no GCC 12, fix it by inspection and ask the gemini seat in your needs table to confirm the build.

3. **strict-sweep: the warnings MSVC misses**
   Compile every Mix View translation unit on the cumulative tip with clang-cl or clang-tidy, using CI's set (`-Wconversion`, `-Wsign-compare`, `-Wmisleading-indentation`, `-Wunused-function`). Fix every hit before win's landing CI finds it.
   Files: `src/adi/mix/**`; `src/juce/ui_mix*`, `ui_stack*`, `ui_analysis*`, `ui_scope_classic*`, `mix_*_host*`; in `src/adi/ui/`: `shared_analysis.*`, `analysis_comparison.*`, `stack_model.*`, `parameter_feed.*`, `focus_scope.hpp`.
   Test: zero warnings under the CI set on those files; CTests unchanged.

4. **DF-M2: the decision guide**
   Add every open Mix View choice still unruled: Match Apply, the Analysis table's "note" column, the timed-CPU question, and the DF-M44 key policy as built.
   The guide is private, not in the repo.
   Done when each question under "For the director" below has a guide entry with options and a recommendation.

5. **DF-M13-rest: Focus FIR, ten nodes with an external key**
   At 192 kHz / 512, the ten-node external-key worst callback is still about 348k counted ops (fe18068e spread only the internal-key work). Spread this work as well.
   Files: `src/adi/mix/focus/Core.cpp`, `Transform.hpp`; `tests/test_mix_focus.cpp`. Stack on `mixview/focus-fir-work-dfm13-20261122`.
   Test: a fail-first work-limit assertion for ten nodes with an external key; byte-identical audio and latency; zero callback allocations. The 2.667 ms wall-time gate waits on the CPU question.

6. **DF-M17-parity: high-rate parity of four cores**
   Compare de-esser, gate, opto and clean-key output with their source VST3 builds at 176.4, 192, 384 and 768 kHz. Admission is tested; parity is not.
   Files: `src/adi/mix/compressor/**`, `filters/**` and their tests, on the #558/#573 heads with the fixes their reviews asked for.
   Test: saved-output comparisons within the established peak/RMS gates, logged per core.

7. **DF-M34-limiter: a Limiter that sleeps without losing state**
   Skipping silence must advance the 128-sample convolver phase and the delayed audio. Keep dither-on output and pending configuration awake. Keep the Adaptive cold-history residual excluded until it is exact.
   Files: `src/adi/mix/limit_chain/**`, `limiter/**`; `src/juce/mix_function_host.*`; `tests/test_mix_limit_chain.cpp`, `test_mix_limiter.cpp`. Stack after `mixview/neutral-eq-suspension-dfm34-20261122`.
   Test: real Graph gate; the Limiter suspends on a silent channel and wakes on an edit or programme with delayed output byte-exact; the 600-check cold-idle matrix holds; an idle Limiter costs 0.

8. **DF-M34-eq: finite tails for stateful EQ modes**
   Zero Phase with lookahead and the spectral bands get a finite tail and a wake path that neither re-enters prepare nor allocates.
   Files: `src/adi/mix/eq/**`; `src/juce/mix_eq_host.*`; `tests/test_mix_eq_host.cpp`. Same stack, after item 7.
   Test: a fail-first host check on the inherited infinite tail; sample-exact wake at the first resumed block; offline intent keeps it running; zero callback allocations.

9. **DF-M36-tests: the compressor host's hold test**
   bf221a35 changed three hosts but tested only the EQ. Add the deterministic no-op-prepare hold test for the compressor host, as 8542a7d3 did for the EQ.
   Files: `tests/test_mix_compressor_host.cpp`. The `builtin_host` hunk and its test are win's (see "Needs").
   Test: a graph rebuild during playback through an unchanged format passes no unprocessed segment; the test fails on the order before bf221a35.

### After 3 December (branches marked "lands after 3 December")

10. **DF-M51: the "+" at the end of every chain**
    Check what the insertion picker on your branches already covers, then finish the row: Mix View tools (those already in the chain grey), a device or plug-in, and a third-party plug-in as a chip whose click opens its editor over the Mix View (ADR-0244 d13, d14).
    Files: `src/juce/ui_stack*`, `ui_stack_picker.*`; `src/adi/ui/stack_model.*`. Stack on the DF-M50 lens branch (`mixview/stack-cards-clean-20261119` line).
    Test: software-peer checks for add, grey state, chip click and one undo step per add.

11. **DF-M52: "Sounding now"**
    A filter that shows the channels sounding when it is chosen, kept as a snapshot until refreshed or another filter is chosen (ADR-0244 d15).
    Files: `src/juce/ui_mix*`; `src/adi/ui/stack_model.*`. After item 10.
    Test: fixtures with known sounding channels; the list stays fixed while signals change; refresh updates it.

12. **DF-M54-scope-offset: polarity flip and visual offsets in Focus Scope**
    A per-layer polarity view flip, and per-source visual offsets in samples or ms (10-sample and 1-sample steps). Preview only: playback and stored timing do not change (PLAN MX-13).
    Files: `src/juce/ui_mix_focus.*`; `src/adi/ui/focus_scope.hpp`; `tests/test_ui_mix_functions.cpp`. On top of `mixview/focus-scope-channels-20261122`.
    Test: a flipped layer cancels against its pair; an offset moves edges by the stated samples; audio and project unchanged.

13. **DF-M54-phase-bars: Focus Scope phase bars**
    PsyScope's operand rules: the first two sources' mono in Layers/Stack, a source's L/R in single or Sum view.
    Files: `src/juce/ui_mix_focus.*`; `src/adi/ui/shared_analysis.*`. After item 12.
    Test: in-phase reads +1, inverted reads -1, the operand switches by mode; no new tap or FFT.

14. **DF-M54-delta-spectrum: residual spectrum for Difference**
    Built from the existing aligned samples and the shared FFT plan.
    Files: `src/adi/ui/shared_analysis.*`; `src/juce/ui_mix_focus.*`. After `mixview/focus-scope-difference-20261122`.
    Test: identical members give an empty residual; a known band difference shows in place; no extra tap.

15. **DF-M55-analysis-table: the Analysis table's columns**
    Loudness per channel (today only Main's M LUFS) and a "what masks it" column from the existing masking bands. The "note" column waits on the director.
    Files: `src/juce/ui_mix_analysers.cpp`; `src/adi/ui/shared_analysis.*`.
    Test: software-peer checks against fixtures with a known masker; no work while the page is hidden (ADR-0237).

16. **MX-12-producer: strip analysis in the shared broker**
    One 96-point spectrum every 50 ms with 300 ms smoothing, and one 40-80-column min/max scope per visible strip lease; none for a hidden strip.
    Files: `src/adi/ui/shared_analysis.*`; `tests/test_shared_analysis.cpp`. The Mixer consumer is the window's.
    Test: N visible strips give N taps and hidden ones zero; one FFT per tap and resolution (MX-30).

17. **DF-M28-compressor-styles: Opto and Filtered Clean as addable functions** (blocked: the row is "on the director's go", and its order puts presets (DF-M23) and keys (DF-M11) first; skip to the next)
    Make the reviewed Opto and Filtered Clean (key EQ) styles addable native compressor functions, one undo step each.
    Files: `src/adi/mix/compressor/**`, `filters/**`; `src/juce/mix_compressor_host.*`, `ui_stack_picker.*`. Stack on #555/#558-#573.
    Test: add, remove and Undo through the Stack "+"; saved-output parity kept; zero callback allocations.

18. **DF-M28-gate-deesser: Gate and De-esser as addable functions** (blocked: as item 17; skip to the next)
    Internal key first.
    Files: the gate and de-esser cores under `src/adi/mix/**`; `src/juce/mix_*_host.*`. After item 17.
    Test: the 162 saved outputs stay byte-identical once registered; add/Undo gates pass.

19. **DF-M3-sound: null comparison against the source VST3s**
    Offline, every registered Mix View function against the VST3 it comes from, including the Clean make-up ramp for DF-M35.
    Files: tests and the offline harness for `src/adi/mix/**`.
    Test: per-function peak/RMS within the gates, logged. Sound only; CPU waits on the director's question.

20. **DF-M55-pictures: each analyser look beside its source**
    Real-size pictures of the VISU spectrogram, the SPAN / BluPrint / Live Spectrum looks, the Classic Scope and Compare, each beside its source.
    Private pictures only, nothing in the repo. Log one sheet per look for win to send.

21. **DF-M53-pictures: the Main meter beside bx_meter**
    Same signal on both: Balance, Correlation, Peak/Dynamic/Rms, holds, Float, weighting.
    Private pictures only. Log the pictures and a reading table for win to send.

22. **DEV-22-spectrum: the shared analyser as Live's Spectrum device**
    A shared-analysis lease API for a device on a track: device output tap, block size, channel, average, max hold. The canvas is the devices lane's.
    Files: `src/adi/ui/shared_analysis.*`, `spectrum_slope.hpp`.
    Test: a device-tap lease returns Live-ordered bins; zero work when its panel is closed.

## Waiting on win

Do not redo these. Answer review findings on them (item 1).

- PRs: #548, #555, #558, #560-#567, #570, #572, #573, #674.
- Branches: `mixview/cumulative-20261122` (05fedb1d), `device-identity-dfm60a-20261122` (6febf651), `focus-match-locate-20261122` (2aec0323), `device-key-shape-dfm60b-20261122` (b418df8f), `neutral-eq-suspension-dfm34-20261122` (d8194c80), `focus-fir-work-dfm13-20261122` (fe18068e), `focus-lazy-storage-20261122` (80eacfe8), `spectrogram-clear-writer-20261122` (18281178), `spectrum-realtime-maximum-20261122` (9d8c214d), `focus-scope-trace-20261122` (6bbe3a26), `focus-scope-mid-20261122` (4af8219b), `spectrum-cumulative-maximum-current-20261122` (3e1f7d31), `focus-align-measure-20261122` (6637f2ae), `focus-scope-difference-20261122` (d850105e), `focus-scope-channels-20261122` (950709f9), `review-cumulative-20261122` (joined, 950709f9), `offline-suspension-regression-20261119` (test-only, for the engine). All under `mixview/`.
- The rows these carry are not queued: DF-M4, DF-M10, M15, M16, M18, M19, M21, M24-M26, M29-M31, M37-M41, M61 (a-c). DF-W27 closes with the stack-top PR.
- Win also owes: the live retest of cumulative 950709f9 (DF-M1) and the DF-M35 listening check; closing the rows already fixed on main through #651/#685 (DF-M14, DF-M43-M46, DF-M56-M59, DF-M60 (c)).

## Needs from other seats

- **window:** an acknowledgement of non-Locate transport changes (Stop/Play between UI frames), so Align Apply can publish one Track Delay edit (ADR-0247 d8, DF-M54, MX-13 Fix Delay).
- **engine:** one Engine-to-monitor delivery of the selected native stage's latency-aligned residual for Hear Removed; **window** binds it to the current device identity (ADR-0247 d8).
- **engine:** the realised per-block key source identity for Carve and Focus IIR; **window:** device-page loans to expose native Carve/Focus (DF-M11, DF-M54 Unmask).
- **engine:** the generic offline graph suspension exclusion (ADR-0043 d5; test branch `mixview/offline-suspension-regression-20261119`) and Auto Gain Stage offline intent (DF-M34).
- **engine-win:** Project Open's cold CLAP load still scans synchronously from the real loader (DF-M42 remainder, next to N68).
- **engine and window-fn (its owner-review item):** review the DF-M60 (a)/(b) loan hunks on `device-identity-dfm60a-20261122` and `device-key-shape-dfm60b-20261122`, and the `window_state.hpp` drain on `focus-match-locate-20261122`.
- **win:** the DF-M23 preset ADR (storage, format, placement; ADR-0240 d3, the #584 proposal). Presets stay unstartable until it exists.
- **win:** the `builtin_host.cpp` hunk of bf221a35 and its no-op-prepare hold test (DF-M36; win's path).
- **clones:** measure VISU's native column cadence; the grouped spectrogram history stays private until then.
- **devices:** the Spectrum device canvas (DEV-22) that item 22 feeds.
- **window:** the Mixer strip consumer for item 16 (`ui_mixer.*` is the window's).
- **gemini:** confirm DF-M62 on GCC 12.

## For the director

- May the seat run timed CPU passes, and when? This gates DF-M13's 2.667 ms wall time, DF-M3's CPU half and the MX-30/MX-36 budgets.
- Focus Match Apply (an addition; Live has none): what does applying a match write — bands of the channel's native EQ, how many, one undo step?
- Analysis table: what should the "note" column say?
- DF-M28: is the 15 November order (compressor, presets, keys, then the other styles, gate, de-esser) your go for items 17 and 18, before presets and keys exist?
- DF-M47 and DF-M48 (the Mix View taking the window; its look and panels) wait on your Mix View sheet (DF-W20).
