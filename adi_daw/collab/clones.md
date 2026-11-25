# Clones lane log

## FOR THE DIRECTOR

**Scope edition.** Does the installed alignment scope 2.0.5 include multi-instance summation? I recommend measuring that view with two isolated sources once the edition is confirmed. Single-source timing and scale have independent readings.

**DF-E43.** Which additional third-party devices from your racks do you want, and what should ADI call each one? I recommend choosing one you actually use after the shaper and scope contracts. Until then the item stays parked.

Win granted the two window sites on 22 November. The window lane reviews the ADI-Shaper opener and wrench checks on `clones/shaper-rack-editor`. For the devices lane, the built-in and native wrappers still need to expose the per-instance waveform feed in `builtin_host.cpp` and `native_devices.cpp`. The native Time wrapper also needs the exact sample and transport revision to reset on a locate at the same position. We have a regression for that case.

**For Win — test the three drawing pens in the installed reference when the director is idle.** Playback of matching saved points is already verified. We need to know which points an actual stroke inserts and where the next click places its repeat. The current implementation stays in place pending your readings.

1. Open Volume alone. Use one full-range band, 100% wet, a synced quarter-note cycle and a flat curve at 50% gain. Save this as your private baseline.
2. Restore the baseline before each take. Select Line, Arc or S and confirm its toolbar icon stays selected when the pointer enters the graph. With no modifier held, drag from one quarter of the way across the graph at 25% gain to halfway across at 75% gain. Save one reloadable state per pen.
3. For the fourth take, restore the baseline and draw the same S again. Release, leave the pointer exactly there, pause briefly, then click once without moving. Save this state as **S repeat**. Perform the repeat before opening any menu or save dialog.
4. Reopen each of the four results and confirm the drawing survived. Return the four states privately to the clones seat, with the plugin version and a screenshot of each resulting curve. For the repeat, report where the second shape started and ended and whether it touched or replaced the first. If a pen stays inactive, report which tool remained selected.

The clones lane will compare the saved positions and corner weights against ADI. Its pen files are `adi_shaper_panel.cpp`, `adi_shaper_panel.hpp` and `adi_shaper_panel_tests.cpp`.

## 25 November 2026 — Three Pan laws across the private program bank

Among 873 programs, 51 use 52 bands sensitive to the gain choice and another five use full delay blend, where the gain choice is neutral. Every comparison rendered the previous route, the corrected blend and range with the previous gain, and the identical route with the new gain. The direct gain transfer and all outputs survived the final held-position optimization.

In saved order the complete route went from 70 to 71 to 73 programs below 0.001 stereo RMS. In fixed order it went from 61 to 62 to 64. The gain law itself added two passes per order, one each for 4.5 dB and linear lift, with zero threshold losses. A third pass in each order came from applying the stored blend and range. All five full-delay programs produced byte-identical results on switching gain laws.

Of the 51 sensitive programs, the gain choice alone improved 29 and worsened 22 in saved order, and improved 28 and worsened 23 in fixed order. The five treated programs among the earlier 81 split two better and three worse in both orders. The earlier 81 retain four saved-order and one fixed-order passes. All 56 fixed-order historical hashes and all 37 genuine saved-order hashes matched. Fresh saved-order comparisons reproduced all prior threshold classifications for the remaining 19 older fallback records.

## 25 November 2026 — Alternate balance laws in the registered Pan bands

`clones/shaper-pan-gain-laws` at `fb744c450` — Add three gain laws to the per-band Pan menu. Lands after 3 December. The 3 dB law remains the default. The extra endpoint choices are 4.5 dB lift, linear lift and cut only. Held positions reuse their computed tap until the position changes.

Release MSVC with warnings as errors passed all four registered suites at each Pd precision. Across 64 synchronized and free-running cases at 256 and 2,048 frames the native device and crossed editable Pd canvas produced 24,576,000 matching channel values after undoing the deliberate output swap. The default law reproduced all 3,072,000 tested values from current main. The exact four-beat core's peak difference from the reference was 0.00000255. The registered comparison reached 0.00007143 channel RMS and 0.03063 peak at sharp joins. Supplying the registered host's physical 3.999999285-quarter default to the direct core reproduced its samples exactly.

Three alternating pairs of source and ADI runs used five trials of 2,048 callbacks each per mode and block size. In menu order, ADI/reference median times at 256 frames were 12.3/36.6, 13.8/24.0 and 8.5/24.2 microseconds. Their p99 values were 23.6/59.7, 19.7/43.0 and 19.8/39.8. At 2,048 frames the median times were 101.0/202.9, 66.2/111.9 and 73.8/112.8, with p99 of 144.3/301.7, 143.5/162.9 and 149.7/179.6. At 256 frames ADI/reference misses were 2/12, 1/4 and 0/1. No run missed at 2,048 frames.

PR requested

## 25 November 2026 — Rounded clipping in Drive

This alternative appears on 46 bands in 44 of the 873 private programs, including five of the original 81 regressions. A nonzero stored Drive excursion remains in 42 programs, three of them in the earlier 81. Fifteen programs use the neutral auxiliary settings exercised here.

For normalized drive d, the measured input gain is 2 to the power 8d and the corner k = 0.85 + 0.12d. Let t be the absolute value after input gain. Through the corner the output equals the signed input. Above it the magnitude is 1 − (1−k)²/(t+1−2k), with the input sign restored. The measured level compensation after this transfer at 0%, 25%, 50%, 75% and 100% was 1, 0.331292, 0.228286, 0.194148 and 0.172630 respectively. Two small coherent tones isolate the compensation. Opposite signed steps around the corner at independent settings test the nonlinear transfer.

At 48 kHz and half drive, direct evaluation at each interpolated input gave stereo-noise RMS error of 0.0000784. Antiderivative evaluation gave 0.00854 even after phase adjustment. On the hard-clip control the antiderivative gave 0.0000761, versus 0.03440 for direct evaluation. The reference reported 16 frames of latency at 44.1, 48 and 96 kHz. On moving kick and bass, using the existing reconstruction filters and the measured gain curve gave largest channel RMS below 0.000039 at those rates. The existing twofold filter at 96 kHz has a larger residual on wideband random input. A private filter fit lowered it, with no shared filter change proposed in this feature.

For DEV-31 the clones seat takes the Drive kernel, the rack's DSP header and source, the device patch and the existing shaper, Drive and registered tests for `clones/shaper-rounded-clip`. Three appended switches, host IDs 928–930, round the existing hard clipping choice independently in each band. The existing style menu and the default hard clip retain their meanings. Lands after 3 December.

## 25 November 2026 — Pan delay range across 873 saved programs

The 0–40 ms Pan range on `clones/shaper-pan-range-1125` at `1d47fa1b2` was measured with the independent transfer selector on `clones/shaper-pan-transfer` at `16cbbf6c8`. The registered device compared the previous route, the selected route still at 2 ms, and that same route with each program's saved range. The 58 programs contain 45 standard Pan bands and all 13 previously held stereo-transfer bands. The remaining 815 retain their prior measurements.

At the 0.001 stereo RMS threshold, saved order went 63, 63, 70 of 873 across those arms. Fixed order went 57, 57, 61. All prior passes remained within tolerance. Seven saved-order gains came from four standard and three transfer bands. Four fixed-order gains came from one standard and three transfer bands. With the selected route held constant, the range edit improved 48 and worsened 10 in saved order. In fixed order it improved 42, worsened 15 and tied once.

Nine of the original 81 regressions were treated. Comparing the complete prior and new settings, eight improved and one worsened in either order. The below-threshold count within those 81 rose from three to four in saved order and stayed at one in fixed order. All 58 fixed-order historical hashes and all 40 available genuine saved-order hashes matched. For the 18 older results recorded as fixed-route fallbacks, fresh saved-order replays reproduced all 18 prior threshold classifications. Paired differences use fresh renders of each actual route.

## 25 November 2026 — Far-channel delay range in each Pan band

`clones/shaper-pan-range-1125` at `1d47fa1b2` — Set the far-channel delay range independently in each Pan band. Lands after 3 December. Three appended controls, IDs 922–924, cover 0–40 ms and default to 2 ms. History is allocated during preparation so a range change can use earlier audio immediately. The full 40 ms equals 7,680 samples at 192 kHz. Stationary contours read the lower whole sample, and moving contours interpolate.

All four DSP, Drive, registered Pd and session suites passed under Release MSVC with warnings as errors at both precisions. Tests exercised all three band routes, independently timed stereo markers, a fractional 110.25-frame read, edits during a tail, sparse recall and a crossed editable Pd output. Registered 2 ms captures at 256 and 2,048 frames reproduced the prior registered build byte for byte.

On moving kick and bass at six range settings, the largest stereo RMS error for the exact-clock DSP was 0.0000000667 across twelve 192,000-frame comparisons. The registered device retains the previously reported normalized beat-division drift. Its largest channel RMS error against the aligned reference was 0.00005084 at 2 ms and 0.00033951 at 40 ms. The reference and the registered device both reported zero latency for this fixture.

Across five trials of 2,048 callbacks, the 40 ms registered device measured median/p99 of 17.3/25.9 microseconds at 256 frames and 132.1/234.4 at 2,048. Three individual deadlines were missed at 256 and zero at 2,048. At 2 ms the prior and new 256-frame medians/p99 were 17.7/28.7 and 17.3/28.6. At 2,048 these were 137.7/245.8 and 136.3/271.7. For integration with stereo transfer, keep each sparse ID range bounded and wire each appended message to its final canvas object index.

PR requested

## 25 November 2026 — Three alternate balance laws for Pan

Three source settings occur in 23, 16 and 17 of the 873 paired programs. At the center, both channels have unity gain. With signed excursion p clamped to −1 through +1, the 4.5 dB endpoint law uses the existing 3 dB gain times its square root for each speaker. The linear-lift law uses 1+p on the left and 1−p on the right. The cut-only law limits those gains to unity. None transfers the other channel into the stereo output. Above half of the delay blend each gain moves toward unity by twice the remaining travel. The far speaker reads its existing post-gain history.

We struck each input twice with independently signed impulses across 126 conditions covering both directions, mixed wet settings and 44.1, 48 and 96 kHz. All four paths agreed within 0.000000175 peak. An independent calculation on a held contour and stereo kick and bass covered three laws, four delay blends, and blocks of 256 and 2,048. Across 24 captures of 192,000 stereo frames each, the largest channel RMS difference was 0.0000000307 and peak error was 0.00000253.

For DEV-31 I take the ADI-Shaper DSP header, core, canvas and existing DSP and registered tests for an after-date menu in each Pan band, on `clones/shaper-pan-gain-laws`. Host IDs 925–927 select the amplitude law. Sparse states retain the current law. When combined with stereo transfer, the transfer switch selects its own existing channel matrix. The 922–924 range controls remain independently mergeable.

## 25 November 2026 — Match the moving 24 dB/octave crossover

`clones/shaper-steep-slew-response` at `7a322d2cc` — Preserve the measured steep crossover histories while its cutoff moves. This repair stacks on the 32 kHz split-limit correction at `0ac2daa75`. Either stable edge can move. The three-band phase adjustment takes the sum of the actual upper branches, including their histories.

All 610,592 reference channel samples matched bit for bit at 32, 44.1, 48 and 96 kHz. All 1,144,952 comparison samples for the existing 6 and 12 dB/octave paths matched the prior build. Four registered suites passed at each Pd precision, in Release MSVC with warnings as errors. The ADI slope-change reset, non-finite recovery and high-rate stability fixtures passed. The reference reset its filter histories on even an identical state reload, leaving the original live slope-switch reset unverified.

Moving stereo kick and bass at 256 frames measured median/p99 of 35.0/57.6 microseconds before and 34.5/50.8 after. At 2,048 frames these measured 282.6/796.7 before and 264.7/412.2 after. Stationary p99 fell from 33.9 to 29.4 and from 256.2 to 206.5 microseconds. Stationary/moving deadline misses per 60,000 callbacks at 256 went from 11/49 to 16/10. Per 7,480 callbacks at 2,048 they went from 1/1 to 0/2.

PR requested

## 25 November 2026 — Continuous point edits in the audio evaluator

`clones/shaper-curve-edit` at `ef692ef7a` — Reuse compiled curve spans during point height edits. Review this two-file DSP change against `clones/shaper-panel-trim-1125` at `8698acb53`. The panel has its separate landing hold.

When only heights change, the evaluator updates those heights on the affected curves and invalidates their cached audio. Edits to knot geometry, transitions into or out of a constant contour, and coincident hard points still take the rebuild path. A committed regression compares the sample bits and the moving flag against a fresh uncached contour.

Each evaluator processed 30,720 measured callbacks at each block size on the registered device, using the same continuous edits, 48 kHz kick and bass and 120 BPM transport. At 256 frames the uncached, prior panel and repaired evaluators measured p99 of 37.9, 27.7 and 27.6 microseconds, with 2, 0 and 0 deadline misses. At 2,048 frames the corresponding p99 values were 145.9, 138.8 and 135.6 microseconds, with no misses in any arm. Across the full comparison renders all 2,621,440 stereo frames at 256 and all 20,971,520 at 2,048 matched byte for byte. The separate 192,000-frame captures matched the archived channel hashes. The Release MSVC DSP and weighted-wave tests passed with warnings as errors.

PR requested

## 25 November 2026 — Held panel stack on current main

For Win and Window. The six commits from `bd52ccf1c` are linear. The four branch tips are `clones/shaper-panel-reconcile-1125` at `18d58e2e3`, `clones/shaper-panel-rack-1125` at `0b7e432d3`, `clones/shaper-panel-grid-1125` at `ad8921489` and `clones/shaper-panel-trim-1125` at `8698acb53`. The trim tip includes Window's four fixes from `c10003c4` and the current main device and window changes. The generic editor continues to serve the device. The custom panel remains held until after 3 December and the director's agreement to Window's proposal sheet.

All 11 registered device suites passed at each Pd precision. The three panel, session and view suites passed, as did 46 real-rack replay checks. A focused private shell run passed 50 checks with the unused-variable annotation described in the separate Window handoff. The unmodified shell test target still fails its Release warnings-as-errors build. The final replay on the latest main changed only the two collaboration documents. Every patch and every source and test tree matched its exercised predecessor.

The private 1920 × 1080 captures show the same curve and 49% Volume at quarter-note sync. The stock instance still needs the per-instance waveform and meter feed. Window can use this pair when preparing the sheet.

## 25 November 2026 — Maximum delay and moving excursions in Pan

Two staggered stereo ramps measured the instantaneous distance to the far-side sample in 24 cases. Smooth and sharp contours crossed both stereo directions at four range settings and 44.1, 48 and 96 kHz. Across 2,934,336 measured positions, put v = min(1, range × absolute excursion). The measured distance in milliseconds follows 40v³ + max(0, 2v − 4v²). The largest difference was 0.00181 sample. With the range fully open, authored corners reached ±1.148 excursion but the observed delay stopped at exactly 40 ms. A steady line reads the lower whole frame, as measured separately in the 60 stationary cases on 22 November.

For the after-date Pan range control I take the ADI-Shaper DSP source and header, its device patch and its two focused DSP and registered tests. Each band will display the end-of-travel delay in milliseconds, 0 to 40, with 2 ms on a sparse recall. The existing 2 ms law has its own bit-exact audio regression.

## 25 November 2026 — Restore Width envelope detection on current main

`clones/shaper-width-current-1125` at `932753575` — Apply the Width follower to constant and moving band contours. Lands after 3 December. The 418 existing descriptor positions stay in order. Removing the active-follower guard from the constant Width path fails the three-band regression.

All 445 declarations and connections were checked. The DSP, Drive, registered and session suites passed in Release MSVC with warnings as errors at both Pd precisions. On the 192,000-frame stereo fixture, peak error was 0.00000005961 at 256 and 2,048 frames against the 0.00000057 limit. Registered Add and Multiply reproduced their earlier calibrated captures byte for byte.

On kick and bass, Add measured median/p99 of 10.2/14.1 microseconds at 256 frames and 79.1/111.4 at 2,048. Multiply measured 10.1/14.5 and 79.9/104.8. With the follower off the same processor measured 5.8/8.1 and 45.2/60.1. Each enabled mode missed one deadline in 10,240 callbacks at 256 and zero at 2,048. Latency was zero.

PR requested

## 25 November 2026 — Moving 24 dB crossover response

Follow-up to `clones/shaper-split-repair-1125`. The lower control limit is settled. Independent source renders also isolated the two stereo outputs during a moving 24 dB/octave split. Two cascaded direct-form I sections reproduce the response when the smoothed normalized frequency is rounded to binary32 before conversion to Hz, the coefficients are calculated in double then rounded to float, and each product and each left-to-right sum are rounded separately. In the upper-edge compensation path, apply the upper low and high branches to the lower band's signal and sum their actual outputs.

This model matched all 38,116 channel samples of the extreme 32 kHz sweep. It also matched the isolated 48 and 96 kHz bands and all 158,328 channel values in an independent 48 kHz three-band render with both controls moving. Peak residual in the 44.1 kHz two-band capture was 0.000000298. The prior section missed by 0.01824 peak at 32 kHz. Next tests cover live slope changes, immediate edits, long decays and processing cost in both stable and moving sections.

The clones seat takes the existing ADI-Shaper crossover DSP and its tests for `clones/shaper-steep-slew-response`, based on the measured slew repair. This is a follow-up repair. The first repair can be reviewed independently.

## 25 November 2026 — Pan transfer attribution across 873 saved programs

For the DEV-31 review of `clones/shaper-pan-transfer` at `16cbbf6c`. Lands after 3 December. At a threshold of 0.001 stereo RMS error, saved stage order improved from **60 to 63 of 873** and fixed order from **54 to 57 of 873**. In each order three programs newly crossed the threshold and none dropped out.

The comparison changed 123 bands in 114 programs. It retained the previous scores of 759 unaffected programs and the existing response in 13 bands that use a nonzero delay blend at a different delay range. Each affected saved-order program was rendered with the selector off and on, using the same incoming signal at each affected Pan. Every selector-off Pan stage reproduced the historical Pan samples. The fixed-order runs checked complete registered processor states and their reported latency.

Among the 114 affected programs, saved-order error fell in 62 and rose in 52. With fixed order it fell in 44 and rose in 70. Nine of the 81 earlier regressions contained an eligible Pan band. Of those nine, four improved and five worsened in saved order. Three improved and six worsened in fixed order. These are full-chain errors against the reference. The isolated Pan matrix and the preceding program-level errors have separate fixtures.

PR requested

## 25 November 2026 — Stereo transfer in each Pan band

`clones/shaper-pan-transfer` at `16cbbf6c` — Add per-band stereo transfer to Pan. Lands after 3 December. IDs 919–921 append three independent switches. Older projects recall the existing balance law. At zero delay blend and half excursion, half of the far input moves to the near speaker and half stays in the far speaker. The delay stores both mixed channels, so a change of direction can replay the previous near speaker in the far ear.

Across 666 measured stationary settings at 44.1, 48 and 96 kHz, separate left and right impulses reproduced all four matrix paths to within 0.000000166 gain. On the moving hard-switch kick and bass at 48 kHz, both 256- and 2,048-frame runs gave less than 0.000000015 RMS error per channel over 192,000 aligned frames. The worst sample differed by 0.00000224. The previous balance mode produced 0.07847 left and 0.07688 right RMS error on the same signal. Sparse-state recall, distinct band switches and both columns of the matrix are covered through the registered and editable Pd paths.

Release MSVC `/W4 /WX` passed all four rack, Drive, session and Pd suites at each signal precision.

PR requested

## 25 November 2026 — Stage order on the current seven-stage device

`clones/shaper-order-current-1125` at `0c16c68b` — Preserve Pan controls when recalling stage order. Lands after 3 December. Use this head for queue item 20. It incorporates the earlier order submission and preserves the 418 descriptors on main, including the three Pan delay blends. Ranks 716–722 append to the descriptor list. Ties use persistent stage identity, and sparse recall restores the legacy order over an already reordered device.

The tests verify 4,096 stage permutations against an independent oracle, Volume before and after nonlinear Drive, 16-frame latency, sparse state, and the existing Crush hold-clock and Pan behavior. The DSP, Drive, session and registered/edited Pd suites passed on this head in Release MSVC `/W4 /WX` at both Pd precisions.

PR requested

## 25 November 2026 — For Window, strict Release shell test build

The unmodified `ui_test_hygiene.hpp` at line 76 prevents the MSVC Release `/W4 /WX` build of the registered `adi_ui_shell_tests` target. The release CRT removes the report calls inside the range-for body, leaving `kind` unused. The ordinary `test_ui_shell.cpp` object and a minimal translation unit using the main-branch header both fail with C4189 promoted to an error. Window owns this header. I recommend marking that existing loop variable `[[maybe_unused]]` and rebuilding the unmodified shell suite. The clones headless dispatcher uses that one annotation in its private copy to exercise the existing shell test bodies.

## 25 November 2026 — Restore the Volume audio retrigger on current main

`clones/shaper-trigger-current-1125` at `58cb2115` — Retrigger Volume contours from stereo audio onsets. Lands after 3 December. Use this update for queue item 18. The existing three Pan descriptor positions, optimized Width path, Crush processing and effective crossover spacing remain in the current integration. The eleven trigger controls at IDs 754–764 append after the 418 existing descriptors. The patch graph check verified all 429 declarations and their physical parameter messages.

The three onset markers restarted the contour at frames 14,671, 54,171 and 82,071. The quiet burst passed without a restart. The reported delay agreed with the measured delay across the twelve rate and detector combinations, including 672 and 1,152 frames at 48 kHz. The rack, Drive, real session and registered/edited Pd suites passed in Release MSVC `/W4 /WX` at both signal precisions.

PR requested

## 25 November 2026 — Generated room stage on current main

`clones/shaper-reverb-current-1125` at `95b31475` — Restore the generated stereo room on current main. Lands after 3 December. This replaces the previous room review head. IDs 626–715 append after the 418 existing descriptor positions, including the Pan blend controls. ADI generates the room response. The control-law, tail reset, ordered reflection, session recall and registered/edited Pd tests passed in five Release MSVC `/W4 /WX` suites at each signal precision.

Five paired kick-and-bass trials at 48 kHz measured median callbacks of 45.8 and 448.5 microseconds at 256 and 2,048 frames. The room added 40.9 and 421.6 microseconds over bypass, or 0.767% and 0.988% of the respective deadlines. The worst trial p99 was 0.133 ms at 256 and 20.501 ms at 2,048. There were 4 and 13 individual overruns in 10,240 calls per size. Reported latency was zero.

PR requested

## 25 November 2026 — Calibrated Noise on current main

`clones/shaper-noise-current-1125` at `79c377a7` — Restore the calibrated Noise stage on current main. Lands after 3 December. This replaces the previous Noise review head. The first 418 generic-panel descriptors retain their positions, including all three Pan blends. Noise appends IDs 536–625. Old projects restore with Noise off. White uses the measured interpolation and Pink uses ADI's own procedural source.

At 48 kHz, 2,097,152 frames gave white-noise RMS 0.0223501 left and 0.0223214 right, stereo correlation 0.000676 and 95% energy below 21,234.375 Hz on both channels. The 256- and 2,048-frame renders were byte-identical. Five Release MSVC `/W4 /WX` suites passed at each Pd precision, including mono equality, the registered device, the edited patch and session recall.

Five paired kick-and-bass trials at each block size measured White's incremental cost over bypass at 11.90 and 96.55 microseconds, respectively. Pink added 27.3 and 219.3 microseconds. That represents 0.223% and 0.226% of the 256- and 2,048-frame deadlines for White, and 0.512% and 0.514% for Pink. Both reported zero latency.

PR requested

## 25 November 2026 — Stage identities when integrating the separate DEV-31 branches

For Win, preserve the saved identities when combining Time, Pitch, Noise and the generated room stage. Each independent port currently appends its own runtime stage after Crush. The original eleven-stage declaration assigns Time 7, Pitch 8, Noise 9 and room 10. The held curve editor uses `(stage * 3 + band) * 2 + lane`, so these stages own curve keys 42–47, 48–53, 54–59 and 60–65 respectively. The original four rank parameters are 723, 724, 725 and 726. A missing stage leaves its saved identifiers available for that stage. Reassigning them by landing order would recall someone else's curve or rank.

Keep existing descriptor positions when appending the independent controls. The landed three Pan delay blends already follow the original 415 descriptors. The combined acceptance case should reopen independent curves and ranks for all four stages with Pitch present and with Pitch absent. The seven-stage order port and the independent after-date ports can each be reviewed on their current base. Lands after 3 December.

## 25 November 2026 — Four moving Drive instances meet the callback p99 target

`clones/shaper-drive-margin-1125` at `dbc4c2cf` — Process each Drive oversampling phase group with one transfer selection. Select the nonlinear transfer once per base-rate sample, advance the mirrored delay ring once per phase group, and keep the rounded filter coefficients available in double precision. The measured impulse, transfer, alias and low-drive tests passed. Changing the starting ring position across 17 offsets at both oversampling factors preserved every output frame. Static and automated audio matched the prior kernel bit for bit in all three styles at 44.1, 48 and 96 kHz.

Four registered stereo tracks processed kick and bass at 48 kHz, 120 BPM, with a moving Drive contour and 16 frames of reported latency on each track. Nine alternating paired trials each covered 8,192 callbacks of 256 frames. The revised trial p99 values in milliseconds were **0.3370, 0.5833, 0.3092, 0.2979, 0.2932, 0.5288, 0.3041, 0.2996, 0.3158**. All were below 5.3333 ms. All four captured stereo streams matched in every pair. Across the 73,728 callbacks, the prior and revised versions had 58 and 40 individual overruns, pooled p99 0.3893 and 0.3256 ms, and median thread cycles 656,857 and 609,585.

At 2,048 frames the revised trial p99 values were 11.0068, 2.8235 and 17.6568 ms against 42.6667 ms. Median thread cycles decreased from 5,343,149 to 4,973,873. Pooled p99 increased from 12.4987 to 13.7114 ms and individual overruns rose from 8 to 19 across 24,576 callbacks. Every paired audio capture again matched. A first experiment that changed only the ring reached 12.4864 ms at p99 in one 256-frame trial and was rejected. Timing includes scheduling on the shared system.

Release MSVC `/W4 /WX` passed the Drive and registered Pd suites at both signal precisions. The final Pd64 gate also passed the complete rack and session fixtures.

PR requested

## 25 November 2026 — Repair the 32 kHz crossover slew test

`clones/shaper-split-repair-1125` at `0ac2daa7` — Apply crossover slew within the measured frequency limits. This repair is ready before 3 December and supersedes the earlier split-slew submission.

At 32 kHz, the first low output sample after a unit impulse was 0.903585625 at 95%, 96% and 100% of the lower-edge control. Inverting the first-order response gives 14,917.23934 Hz. The 94% setting gave 13,918.27556 Hz. The same plateau measured 14,917.23918 Hz at 48 kHz. Two complete 19,058-frame stereo captures made the same move with an in-range request at the limit and a request for 20 kHz. Every sample matched. Apply the physical lower-edge limit before smoothing. The 22.05 kHz upper-edge case separately tests ADI's numerical limit of 0.49 times the sample rate.

The moving Volume and optimized Width paths now use the same effective cutoffs. Removing the lower limit, bypassing slew, dropping Width's edge history and moving the numerical ceiling each made the regression fail independently. **Separate response finding.** In a 32 kHz extreme sweep, the moving 24 dB/octave section still differs from the reference by 0.01824 peak. The static spacing and slew tests do not establish moving filter response parity for that slope.

Release MSVC `/W4 /WX` passed all four suites in each Pd precision on this head. These cover the DSP, Drive, transport in a real session, and the registered and editable Pd paths.

PR requested

## 25 November 2026 — Pan stereo transfer, DEV-31

For devices, the Pan delay writer is now on main. The 22 November stereo-transfer contract identified this missing Pan mode in 127 of 873 saved programs. The clones seat takes `adi_shaper.cpp`, `adi_shaper.hpp`, `ADI-Shaper.pd` and the two ADI-Shaper rack and Pd tests for `clones/shaper-pan-transfer`. Reserve IDs 919–921 for independent low, middle and high band selectors. Old projects keep the existing balance law. The new mode will be checked with separate left and right markers, moving delay and sparse recall. Lands after 3 December.

## 25 November 2026 — Review dependencies and existing scope contracts

For Mix View, the two entries of 22 November headed “Mix View contract for the alignment scope 2.0.5” and “Mix View contract for the layer scope 1.5.03” each give the measured laws, reproducible test signals, acceptance cases and outstanding readings. Items 10–12 remain parked until the director releases the desktop for the interactive offset, analyser and phase-bar readings.

For devices, item 13 is ready for your review on `clones/two-head-reader` at `9b7cd7ca`. The original and shared-reader processors passed 3,127,711 bit-exact comparisons. Item 14, cyclic Pitch, awaits that review. Lands after 3 December.

Item 19, external-key Width and note retrigger, awaits the independent Width follower and Volume audio-retrigger ports on main. Their combined private fixture covers five side-gain plateaus and four exact note-on restarts. Lands after 3 December.

For Win, the generic panel remains the device editor on main. The custom panel awaits the director's agreement to a proposal sheet and a landing after 3 December.

## 22 November 2026 — Steep Filter, all 873 programs

`clones/shaper-filter-steep` at `5fff5d0a` — Complete the reference comparison for the two 24 dB Filter responses. **Lands after 3 December.** With saved stage order, the number below 0.001 stereo RMS error rose from 53 to 60 of 873. With fixed order it rose from 49 to 54. Every previously passing program stayed below the threshold in its respective order.

The changed response occurs on 101 bands in 100 programs. All 100 complete stereo streams changed. In saved order, 82 improved by more than 0.000001 RMS, 17 worsened and one stayed within that distance. Fixed order gave 74, 25 and one respectively. Of nine affected programs in the earlier 81, eight improved and one worsened in saved order. One crossed the threshold, from 0.00736285 to 0.000299543. The other 773 programs passed the checks in both orders.

Four of the affected programs also use the peak Filter on a different band. Another uses the Hold follower; its running detector was included in both arms. Forty-nine historical saved-order baselines were freshly rendered. They changed no previous threshold decisions. One fixed-order score was recalculated after the entire audio stream matched its archived checksum. Both old and refreshed scores remained above the threshold.

PR requested

## 22 November 2026 — Steep Filter responses in the registered device

`clones/shaper-filter-steep` at `5fff5d0a` — Add 24 dB/octave low-pass and high-pass on independent bands. **Lands after 3 December.** All 415 existing normalized values passed sparse recall. Native and edited Pd passed live response changes in all three bands, including control writes during a ringing response. The rack, filter and registered suites passed at both Pd precisions under MSVC /W4 /WX. The kernel passed 1,370,196 signed comparisons against two existing sections in series. Its signed impulse test matched the original within 0.000000206 of a unit impulse. Reversing the two sections failed both moving-response checks.

Registered stereo kick and bass at 48 kHz. Each timing pair lists 256 / 2,048 frames in µs across 30,720 callbacks at each size. RMS compares the complete stereo render with the reference. All arms reported zero latency. All block-size pairs reproduced their audio exactly, and the existing high-pass output matched the prior head byte-for-byte.

| Response | Prior → new RMS | New median | Reference median | New p99 | Reference p99 | New misses | Reference misses |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Low-pass | 0.00406 → 4.93e-08 | 29.8 / 241.8 | 69.5 / 484.6 | 53.5 / 478.9 | 141.3 / 2,064.9 | 10 / 0 | 19 / 2 |
| High-pass | 0.00181 → 7.78e-08 | 33.6 / 266.1 | 70.0 / 483.0 | 125.5 / 542.5 | 130.1 / 1,243.1 | 7 / 3 | 16 / 2 |
| Moving low-pass | 0.112 → 1.9e-06 | 62.5 / 532.1 | 111.6 / 687.9 | 133.6 / 4,705.2 | 397.3 / 1,648.7 | 13 / 7 | 66 / 0 |
| Moving high-pass | 0.0281 → 8.48e-07 | 79.1 / 505.1 | 113.0 / 684.6 | 232.0 / 777.2 | 390.4 / 1,356.5 | 28 / 1 | 71 / 3 |
| Existing high-pass | 9.2e-08 → 9.2e-08 | 28.2 / 166.5 | 65.4 / 263.5 | 133.5 / 542.3 | 172.4 / 562.5 | 23 / 0 | 22 / 0 |

For Win when merging with peak at `966700ee` and stereo offset at `830d3a12`: keep each band's three independent controls, with peak taking precedence over steep. Reindex the nine appended control/message pairs from the final canvas and connect every message to the external. The private 424-control merge passed native and crossed editable Pd at 32-bit precision, including all nine sparse controls in normalized and real units. Removing the middle band's offset, peak or steep wire failed the signed audio comparison in each case. Giving steep precedence over peak also failed it.

PR requested

## 22 November 2026 — First half-step after Filter silence

On affected clean Filter restarts, the first of the two updates in a sample behaves as if damping increases by 2. The second update uses the ordinary law. Signed low-pass, high-pass and band-pass outputs independently constrain the two updates. Across 156 per-channel recordings at 26 rate and control settings, the magnitude of the preceding band-pass sample correctly classified all 25 exceptional and 131 ordinary restarts, including 11 with exactly zero prior state.

Expressed in units of the smallest positive normal binary32 value, the exceptional measurements ranged from 1.02958 to 1.88363. The nearest ordinary nonzero value was 2.04417. An exact value of 2 was not sampled. The largest first-update damping error was 0.0000275. The current ADI filter clears this residual state at a substantially larger amplitude. These readings bound the discrepancy and leave the exact boundary open.

## 22 November 2026 — Filter response when audio resumes after silence

An isolated stereo marker followed by exact silence exposes a short restart difference in the original 12 dB Filter. In independent 48 kHz runs the peak differences between a run with the earlier marker and a run without it were 0.00116021 for low-pass and 0.00197680 for high-pass. The ADI core on main produced identical audio for each pair. Without the earlier marker its peak error was below 0.00000003. This also affects the first few resumed samples of the corresponding steep responses.

The difference reproduced at 64, 256 and 2,048 frames. Reversing the marker polarity preserved the measured difference. Sending the marker to one channel left the other channel unchanged. Replacing the intervening zeros with the same input at one hundred-thousandth amplitude reduced peak error below 0.000000291 in the four low- and high-pass cases. Bypass, disabled Filter, Volume, Width and Pan showed zero dependence on the earlier marker. Further measurements are needed to establish the Filter's internal restart condition.

## 22 November 2026 — Steep low-pass and high-pass Filter contract

For the devices lane, the clones seat takes the ADI-Shaper Filter core, existing rack, editable patch and tests on `clones/shaper-filter-steep`. **Lands after 3 December.** Reserve host parameters 916–918 for a 24 dB/octave switch on each band's 12 dB/octave low-pass and high-pass. Each switch defaults off. The saved five-way type selector keeps its original normalization. The current peak override takes precedence when both are selected. Editing an underlying selection that leaves the audible response unchanged must preserve filter history.

In separate headless reference instances, a 12 dB section at the requested resonance feeds a second 12 dB section at normalized resonance 0.2. Both receive the same cutoff at each frame. Each section finishes both nonlinear substeps before the next receives its signed output. On a 44.1 kHz moving stereo test with partial wet and off-center cutoffs, the low- and high-pass cascades matched the respective reference within 0.000000014 RMS. Reversing the sections gave 0.002317 and 0.002830 RMS. The equivalent full-wet 96 kHz moving cases matched the reference float streams exactly. A separately calculated recurrence was within 0.000000337 RMS at 96 kHz. Isolated startup impulses and the remaining filter types are under separate investigation.

## 22 November 2026 — All-program resonant peak comparison

For Win, adding the peak Filter at `966700ee` raised the count below 0.001 stereo RMS error from 51 to 53 of 873 in saved stage order and from 47 to 49 in fixed order. The same two programs crossed in both orders, and none fell out. Their new saved-order errors were 0.000004234 and 0.000001330. Fourteen saved-order baselines had to be recaptured with the previous renderer. This changed no threshold decisions.

All 50 affected programs produced different audio in both orders. In saved order, 29 improved by more than 0.000001 RMS, 18 worsened and three stayed within that distance. Fixed order gave 26, 21 and three respectively. Across the earlier 81, five improved, two worsened and 74 stayed within tolerance in each order. A band appearing in a saved program does not establish its audible contribution.

The 823 unaffected fixed-order renders matched their prior full streams exactly. In saved order, the 86 controls with an existing Filter reproduced its prior complete output on their actual upstream audio. The 737 remaining programs used the same stored settings and chain without a Filter. Source programs, conversions, per-program identifiers and audio remain private.

## 22 November 2026 — Three-run crossover comparison

For Win, all eight 48 kHz stereo captures at `ef2ca023` matched `a5560662` byte-for-byte on the registered kick and bass. Each cell below uses 30,720 timed callbacks in three fresh processes per implementation with rotated run order. Both heads and the reference reported zero latency.

| Case | Buffer in frames | Old head median µs | New head median µs | Reference median µs | New / reference p99 µs | New / reference misses |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Lower cutoff edit | 256 | 41.7 | 19.0 | 76.6 | 24.9 / 332.3 | 12 / 125 |
| Lower cutoff edit | 2,048 | 327.7 | 147.8 | 465.5 | 363.4 / 4,295.6 | 7 / 25 |
| Upper cutoff edit | 256 | 41.4 | 18.6 | 74.6 | 25.9 / 179.9 | 9 / 80 |
| Upper cutoff edit | 2,048 | 321.9 | 142.8 | 455.3 | 418.7 / 2,832.4 | 30 / 18 |
| Interrupted reversal | 256 | 41.7 | 19.1 | 75.7 | 37.4 / 114.4 | 48 / 24 |
| Interrupted reversal | 2,048 | 325.6 | 145.6 | 460.7 | 229.2 / 1,529.6 | 17 / 15 |
| Stationary | 256 | 15.7 | 9.8 | 29.0 | 16.8 / 53.5 | 17 / 6 |
| Stationary | 2,048 | 118.8 | 71.9 | 100.2 | 377.7 / 141.2 | 19 / 6 |

## 22 November 2026 — Lower the cost of moving and stationary crossovers

`clones/shaper-split-slew` at `ef2ca023` — Choose the stationary or moving renderer once per stage callback. While an edge moves, evaluate its frequency coefficient from one shared 18 KiB polynomial table. The cap and out-of-table frequencies use their analytic values. Cold setup and the transition to a settled frequency keep the original calculation.

All six suites passed in both Pd precisions under MSVC /W4 /WX. Two steep-slope stereo cases cover entering the frequency cap during an edit, settling, falling and reversing at 22.05 and 32 kHz. Reusing old coefficients failed ten sampled assertions. Lowering the cap from 0.49 to 0.48 of the sample rate failed four. A separate six-rate comparison against the direct function covered 6,041,352 sample values. All stationary values matched exactly. One value after a cutoff resumed differed by 0.0000000000291.

The first registered 48 kHz kick-and-bass comparison of this head measured a lower-edge sweep at 19.2/146.3 µs median for 256/2,048 frames, against 41.4/325.6 on `a5560662` and 78.0/467.5 in the reference. Each value covers 10,240 callbacks. The three-run comparison also measures an upper-edge sweep, an interrupted reversal, and the stationary control.

PR requested

## 22 November 2026 — Resonant peak Filter in the registered device

`clones/shaper-filter-peak` at `966700ee` — Add an independent resonant peak switch to each Filter band. **Lands after 3 December.** The 415 original host parameters keep their positions and ranges. Sparse states restore all three new switches to off, including when loaded over an active peak. Three suites passed under MSVC /W4 /WX at each Pd precision. Native and edited Pd passed live switching of every band and changes to its underlying mode. A band-pass substitution failed both new acoustic tests. The signed peak impulses matched reference readings within 0.00000204 of a unit impulse over three sample rates.

On registered 48 kHz stereo kick and bass, replacing a plain band-pass with the peak reduced RMS error from 0.187127 to 0.0000000487 at full wet, from 0.043858 to 0.0000000138 at partial wet, and from 0.122802 to 0.0000000746 with both contours moving over a quarter of a beat. All cases matched their own renders between 256 and 2,048 frames. The old band-pass control stayed byte-identical. All arms reported zero latency.

Full-wet processing took 26.5/202.2 µs median at 256/2,048 frames versus 44.4/291.9 in the reference. Candidate p99 was 62.7/1,389.3 µs, with 23/6 misses, against 133.8/2,623.1 and 21/3. Moving both contours took 57.3/456.5 µs median versus 97.5/629.7, with 48/12 versus 82/63 misses per 30,720 callbacks at each size.

For Win when merging with stereo offset at `830d3a12`: retain both sparse control ranges. Append and wire all six control/message pairs in the canvas, numbering them from their final object positions. The automatic merge retained only the first set of indexed connections. Removing the second set failed the edited-canvas audio test. The corrected 421-control integration passed with independent signed stereo offsets and live peak switches on all three bands.

PR requested

## 22 November 2026 — Resonant peak Filter response

For the devices lane, the clones seat takes the ADI-Shaper Filter core, its existing rack and Pd declarations, and its device tests on `clones/shaper-filter-peak`. **Lands after 3 December.** Reserve host parameters 913–915 for the low, middle and high band peak switches. Each defaults off. The five existing Filter selections keep their normalized values. When a switch is on its band uses the peak response. Switching it off restores the underlying selection.

The peak response adds the signed input to the 12 dB band-pass output at the same cutoff and resonance. Eighteen signed stereo impulse cases at 44.1, 48 and 96 kHz included both endpoints of both controls. The largest peak difference from that sum was below 0.0000000005 at the tested impulse amplitudes. Partial band and master mix multiply the added band-pass component. Two moving contours with a stereo offset gave 0.0000000119 RMS error at both 256 and 2,048 frames. Negating the input negated every output sample. Zero wet returned the input exactly. Changing the stored drive or safe-resonance switch produced the same output on the tested trajectory. At 48 kHz the reference reported zero latency ordinarily and 16 frames with its existing reserve enabled.

## 22 November 2026 — Moving crossover cutoffs

`clones/shaper-split-slew` at `a5560662` — Smooth live crossover frequency edits in logarithmic frequency while keeping the first configured value immediate. Three cascaded poles use the rate constant 72.02133 per second. A callback or internal 512-frame boundary may put a sufficiently settled edge to sleep when its normalized distance is at most 1/16,384. Six rack, stage and registered Pd suites passed at each Pd precision under MSVC /W4 /WX. Native and editable Pd reproduced two live edits. The landed source failed 12 of the 16 measured sweep landmarks.

At 48 kHz, stereo kick-and-bass RMS error at 256/2,048 frames fell from 0.0273462 to 0.0000000255 for a lower-edge sweep, from 0.004776 to below 0.0000000137 for an upper-edge sweep, and from 0.0299511 to 0.0000000226 for a reversal. The unedited waveform stayed byte-identical. The reference's upper sweep itself differed by 0.00000106 RMS between block sizes. The difference between the two implementations' block-size effects was 0.0000000092 RMS. All arms reported zero latency.

At 256/2,048 frames the lower sweep took 42.0/325.1 µs median versus 73.7/461.0 in the reference. Its p99 was 86.0/7,199.3 µs versus 151.4/8,158.0, with 50/65 versus 86/59 misses per 30,720 callbacks at each size. The steady 2,048-frame control took 118.2 µs versus 97.6 in the reference. For Win when combining with spacing at `34ee3fbd`, compute the two effective physical frequencies first, then pass each frequency, sample rate and cold flag to its edge. The fixed Width guards for `42c4135a` are in the preceding entry.

PR requested

## 22 November 2026 — Constant three-band Width

`clones/shaper-three-band-split` at `42c4135a` — Compute fixed band gains once per callback. Six suites passed in both Pd precisions under MSVC /W4 /WX. All six paired runs reduced the median at both tested buffer sizes. On stereo kick and bass at 48 kHz, 256/2,048 frames took 10.6/80.2 µs versus 13.4/111.0 on the previous head and 23.6/106.6 in the reference. Candidate p99 was 15.6/160.5 µs versus 58.7/385.8 in the reference. There were 8/4 candidate deadline misses and 21/3 reference misses per 61,440 callbacks at each size. Moving Width at 2,048 frames stayed near 111 µs median and its p99 increased from 366.5 to 582.7 µs.

For Win when combining the Width follower, crossover spacing and moving cutoffs: a fixed Width band must have a constant contour and an inactive follower. Compute both effective frequencies before setting their targets. Check `startChunk(sampleRate_)` on both edges separately and use the ordinary renderer if either edge moves. All 7,463,394 stereo sample values in 27 integration cases then matched the ordinary renderer exactly. Omitting both edge checks failed all 27 cases. Checking the lower edge alone failed 18, beginning at the upper-frequency edit. Omitting the follower check changed 455,949 sample values in the independent detector test.

PR requested

## 22 November 2026 — For Win, registered Hold and crossover integration

The Hold and clock integration at `cf693f3a`, `51901a1a` and `761744fb` also passed with the stereo crossover at `962af9d0` and spacing at `34ee3fbd`. All seven registered states reproduced their complete stereo audio between 256 and 2,048 frames in each arm. The independent reference captures repeated and all arms reported zero latency.

With the detector disabled, split-band stereo error fell from 0.000319995 to 0.0000000111 RMS after adding the crossover. With the detector linked to the middle band it fell from 0.00248448 to 0.00000145. The full-input cut pair fell from 0.00026754 to 0.00000335 and overlapping cuts from 0.00031769 to 0.000000692. Both unsplit controls remained byte-identical to the earlier clock integration.

The filtered pair took 57.7/464.5 µs median at 256/2,048 versus 146.8/1,147.7 µs in the reference. The corresponding p99 readings were 105.4/1,031.2 and 254.4/14,422.4 µs, with 1/0 versus 1/6 misses. The disabled 2,048-frame result varied from 26 candidate misses on the first run to one when run order was reversed. These are measurements of the named heads together.

## 22 November 2026 — Process both crossover channels together

`clones/shaper-three-band-split` at `962af9d0` — Process both channels together in the first-order crossover. The paired implementation and scalar fallback preserve the order and precision of the four filters. All 1,731,798 samples from independent stereo markers, irregular blocks, cutoff edits and slope changes matched the preceding head. The rack, stage and registered Pd suites passed in both precisions under MSVC /W4 /WX.

Seventy-two fresh registered kick-and-bass runs rotated all six orders of the old head, new head and reference. At 256/2,048 frames, three independent gains took 12.0/90.5 µs median versus 18.2/110.7 µs on the old head and 22.7/125.0 µs in the reference. The new p99 was 19.9/192.3 µs, with 8/3 deadline misses. The reference had 26/6 misses per 61,440 callbacks at each size.

Width took 13.5/112.4 µs median versus the reference's 23.6/105.8 µs. At 2,048 frames the new head used 396,784 thread cycles per callback against 414,216 in the reference, with p99 of 370.7 versus 287.5 µs and three versus two misses. The 256-frame Width case had 21 misses versus 17. Equal-size stereo error stayed at 0.000000021 for independent gains and 0.000000042 for Width. All paths reported zero latency.

PR requested

## 22 November 2026 — Preserve the measured gap between crossover frequencies

`clones/shaper-split-spacing` at `34ee3fbd` — Apply the measured half-octave spacing and single-edge limits. ADI retains the requested knob frequencies independently. The upper cutoff follows the lower when necessary and a saved state reopens with both original knob values. The existing band selector controls which bands are enabled. Five new source-response checks failed on main. All six rack, stage and registered Pd suites passed under MSVC /W4 /WX in both precisions.

At both 256 and 2,048 frames on 48 kHz stereo kick and bass, equal interior cuts improved from 0.005726 to 0.000000288 RMS and crossed cuts from 0.009493 to 0.000001663. The lower and upper endpoint settings improved from 0.014198 to 0.000000029 and from 0.0002433 to 0.0000000072. A separated pair and an ordinary two-band setting stayed byte-identical. All repeats and both block sizes produced identical audio and zero latency.

For the equal-cut case at 256/2,048 frames, ADI measured 27.8/206.9 µs median and 42.4/393.3 µs p99. The reference measured 59.3/318.0 µs median and 112.3/1,107.2 µs p99. Misses were 10/31 versus 42/36 per 30,720 callbacks at each size. The lower-endpoint case at 2,048 frames had 26 misses versus two in the reference.

The private control conversion now preserves the middle band at both coincident endpoints. In a complete regeneration, 872 of 873 states were byte-identical in all three encodings. The sole difference corrected a float-step cutoff. This change accounted for no threshold crossings.

PR requested

## 22 November 2026 — Live crossover frequency changes have three smoothing poles

The lower and upper edges use three equal one-pole smoothers in series on normalized log frequency. Per sample, the coefficient is 1 − exp(−72.02133 / sample rate). A newly opened device starts at its supplied frequency. When automation changes the target while audio is running, the first pole follows the target, the second follows the first and the third follows the second. An edit during the transition starts from the current states.

At the start of a callback and each further 512-frame slice inside it, compare the previous third-pole value with its target. If the absolute normalized difference is at most 1/16,384, emit the target and hold all three internal states. The next edit resumes from those held states. Values immediately below, at and above the threshold distinguish the inclusive comparison. The measurement host delivered its scripted edits on callback boundaries; the smoothing starts when the device receives the edit.

Two independent stereo measurement methods covered both edges, startup, reversals, irregularly placed events and six rates from 16 to 192 kHz. Across 27 in-band holdouts the largest full-stream error was 0.000000047 RMS. A 16 kHz case starting above Nyquist showed a separate coefficient-precision effect. The 47-capture private suite also distinguishes the 512-frame slice from a 1,024-frame slice at a host size of 1,501.

## 22 November 2026 — Crossover spacing and the two end positions

With the ten-octave reference control, the upper cutoff stays at least five percentage points above the lower one. This is half an octave, or a frequency ratio of √2. The lower control has priority up to 95%, about 14,917.24 Hz. At the other end the lowest upper cutoff is about 29.1352 Hz. Independent signed stereo impulses covered 24 pairs at each of 44.1, 48 and 96 kHz. At 48/48% and 48/48.001% the effective controls were 48/53%. Separate two-band recordings reproduced these results across all nine slope pairs.

Both controls at zero give a middle and high band split at 29.1352 Hz. Both at full give a low and middle band split at 14,917.24 Hz. Reloading actual saved settings at the two reachable boundaries confirmed these equivalent endpoint pairs at 256 and 2,048 frames. Removing the surviving middle band changed the full stereo signal by 0.16514 RMS in the low-edge Drive case.

ADI stores the enabled bands separately from cutoff frequency. An endpoint pair therefore maps to its two surviving bands and their measured cutoff. Of 873 saved programs, none stores the raw coincident endpoints. One saved steep split is a float step inside the minimum gap. Two Sweep settings request 20,377.55 Hz, beyond ADI's present 20 kHz control. Reducing only that edge changes their isolated stage but gives identical complete racks on this fixture. A higher Pitch edge changes its full rack by 0.0000254 RMS.

## 22 November 2026 — Correct the first-order three-band crossover

`clones/shaper-three-band-split` at `802ac60f` — Correct the first-order three-band crossover paths. The two outer bands cascade their filters in opposite orders, and the middle is their complement. The existing seven-stage rack shares this split. Isolated impulse and frequency-response tests failed 26 checks on main. Both stereo channels, live retuning and irregular edited-Pd blocks are covered. All six rack, stage and registered Pd suites passed under MSVC /W4 /WX at both precisions.

On 48 kHz kick and bass, low and middle band errors each fell from 0.002145 RMS to below 0.000000016. Three independent gains fell from 0.003218 to 0.000000021 and Width from 0.000212 to 0.000000043. Both block sizes produced the same audio. Unity, two-way and mixed-slope outputs matched main byte for byte. A separate clock measurement accounted for the remaining 0.000317 RMS on wet Crush.

At 256/2,048 frames, independent gains took 13.9/108.8 µs median versus 22.3/108.9 µs in the reference. The corrected p99 was 25.8/204.1 µs versus 42.4/356.5 µs, with 0/0 versus 3/10 misses per 30,720 callbacks. Active Width took 19.9/116.1 µs versus 23.5/105.1 µs. Its 256-frame p99 was 127.3 versus 50.8 µs and misses were 9 versus zero. All paths reported zero latency.

In the 873-program acoustic comparison, 207 saved programs selected this split and 176 used it in a represented stage. Seven additional programs crossed below 0.001 stereo RMS, with no losses at the threshold in either order. The fixed-order count rose from 40 to 47 and the measured-order count from 44 to 51. Among all 207, measured-order error decreased in 92, increased in 69 and changed by less than 0.000001 in 46. Restoring previously unmeasured signal orders accounted for none of these crossings.

PR requested

## 22 November 2026 — For Win, integration check for the three Hold reviews

Combine the Hold input at `cf693f3a`, fractional clock at `51901a1a` and full-rate capture at `761744fb`. The clock sources merge cleanly. On the rack call into Crush, qualify the constant-contour shortcut with the manual detector being disabled. A constant drawn height can still have a moving detector. A private registered Pd32 build exercised this combination through the actual device factory, external and embedded patch.

At 48 kHz, moving Multiply fell from 0.0368116 RMS error with the detector change alone to 0.00001950 with all three reviews. The held flat contour fell from 0.00041727 to 0.00000168. Disabling manual modulation on the separate unsplit controls gave 0.000000032 RMS. Both ears were byte-identical between the 256- and 2,048-frame renders. All paths reported zero latency.

Moving Multiply took 28.0/221.2 µs median and 48.9/373.5 µs p99 at 256/2,048 frames. The reference took 98.8/707.1 µs median and 182.0/1,263.1 µs p99. Neither missed a deadline in those 10,240 callbacks per size. The 2,048-frame moving-off control had 14 misses against zero in the reference. This is an integration reading on the named review heads.

## 22 November 2026 — Calibrate the Hold detector's rise and fall

`clones/shaper-hold-input` at `cf693f3a` — Use the calibrated envelope coefficients in the Hold detector. **Lands after 3 December.** Its shared coefficient implementation is byte-identical to the Width review. The independent signed-step regression failed on the preceding implementation. The rack, Crush, detector filter and registered Pd suites passed in both precisions under MSVC /W4 /WX.

All 96 registered stereo kick-and-bass captures reproduced their audio across repeat runs and 256/2,048-frame blocks at 48 kHz. Latency was zero. Error against the reference fell from 0.001356 to 0.000417 for a held flat contour, from 0.000751 to 0.000475 for full-height Add and from 0.000446 to 0.000288 for the filtered cut pair. Moving Multiply rose from 0.036258 to 0.036812. An independent detector readout on that same signal improved from 0.00000538 to 0.0000000154 RMS in normalized height. The separate full-rate capture and clock reviews affect the final output.

The held flat contour at 256/2,048 took 24.6/209.6 µs median and 48.9/427.6 µs p99, versus 100.9/706.3 and 249.4/5,515.3 µs for the reference. Misses were 3/0 versus 9/8. Moving Multiply used 32.4/246.1 µs median versus 105.7/755.1 µs; misses were 6/0 versus 76/103, per 20,480 callbacks per size. Median and p99 fell below the reference in all sixteen measured cells.

PR requested

## 22 November 2026 — Order of the two outer three-band filters

Two stereo cutoff steps distinguish filter arrangements that sound identical at fixed frequencies. Keep the opposite edge fixed and step each cutoff separately at frame 8,192 on sustained, independent left and right input. Read the instantaneous coefficients from separately rendered two-band outputs and use those measured coefficients in each candidate three-band reconstruction.

The low band passes through the lower low-pass and then the upper low-pass. The high band passes through the upper high-pass and then the lower high-pass. Subtract these two from the stage input for the middle band. Over the first 512 frames of each transition, peak prediction errors were 0.00000247 and 0.00000152. Using a fixed-cutoff algebraic shortcut with the instantaneous coefficients gave 0.000235 and 0.000150. These readings identify the order independently of the cutoff smoothing time.

Two separated cutoff pairs and all nine slope combinations at 44.1, 48 and 96 kHz provided 216 single-band and unity comparisons. Solo agreed sample-for-sample with muting the other two gain curves in every Volume case. The first-order prediction was within 0.000000075 peak; the other eight pairs followed the existing compensated serial split to within 0.00000143. The first-order path is also present underneath the active Filter, Sweep, Drive and Crush stages.

## 22 November 2026 — Attribute Hold detection in saved signal chains

Nine saved settings use manual Hold detection supported by this comparison. Two select the full stage input and both leave its cutoffs open. Seven use their own split band. An additional seven automatic-release settings have a different response and remain outside this comparison.

Each test restored the same registered controls, kick and bass, clock implementation and surrounding processors. The previous enabled and disabled renders reproduced all archived full-stream hashes in both the fixed and the measured original orders. Against the previous enabled detector, calibrating attack and release moved two original-order settings closer to the reference, six by less than 0.000001 RMS and one farther away. The fixed-order counts were three closer, five within tolerance and one farther. Eight of the nine audio files changed in each order. The largest change in original-order output was 0.005111 stereo RMS.

None of the nine crossed 0.001 RMS. The closest remains at 0.0010692 in its measured order. The overall measured-order count remains 44 of 873 below threshold, and the fixed-order count remains 40 of 873. No source audio or settings are distributed.

## 22 November 2026 — Three-band Volume follows a distinct first-order split

At 48 kHz with independent stereo impulses, place the split controls at 20% and 58%. Measure the complex response of each individual crossover with the other crossover disabled. For low and high cutoffs respectively, call those measured low-pass responses L1 and L2 and their complementary high-pass responses H1 and H2.

With both slopes at 6 dB/octave, the low band follows L1 × L2, the high follows H1 × H2, and the middle equals the input minus the two outer bands. Soloing each band in turn gave byte-identical output to setting the other two gain curves to zero. This held for both channels. The reconstructed three-band prediction was within 0.000000053 peak per unit impulse. The present low and middle paths differ from the measurement by 0.009226 peak and still cancel at unity. This explains why the earlier recombination check passed.

The 6/24, 12/6 and 12/24 dB/octave pairs instead followed the current serial split and its phase compensation at these settings. The next checks use additional rates, adjacent cutoffs and other stage types. The implementation will retain the measured two-band and steeper-slope paths.

## 22 November 2026 — Calibrate the Width detector's rise and fall

`clones/shaper-width-follower` at `a1556885` — Apply measured attack and release coefficients to Width. **Lands after 3 December.** The calibration reproduced 131 of 132 held-out coefficients exactly, with a one-float-step difference at the fastest attack at 192 kHz. The earlier implementation failed the stricter trajectory tests. The rack and registered Pd suites passed in both precisions under MSVC /W4 /WX, including long-tail regressions.

On registered stereo kick and bass, RMS error fell from 0.0000000637 to 0.0000000083 in Add and from 0.0000000360 to 0.0000000078 in Multiply. Both peaked at 0.0000000596. Off remained byte-identical to the preceding head and the default matched both the reference and the preceding head exactly. The 256- and 2,048-frame captures agreed.

At 256/2,048 frames Add took 9.2/77.6 µs median and 14.6/131.6 µs p99, versus 21.5/78.9 and 38.6/128.4 µs for the reference. Deadline misses were 1/14 versus 2/4, per 30,720 callbacks at each size. Multiply took 9.9/79.3 µs median, versus 22.0/84.2 µs; misses were 3/2 for both. The main improvement is envelope accuracy.

PR requested

## 22 November 2026 — Filter the full signal feeding a Hold detector

`clones/shaper-hold-input` at `f6cb7904` — Add high-pass and low-pass cutoffs to each full-input detector. **Lands after 3 December.** The six appended controls retain open cutoffs on older states. The rack, Crush, detector filter and registered Pd suites passed under MSVC /W4 /WX in both precisions. The embedded patch was verified byte for byte. Cutoff automation and re-entry after a neutral setting were tested against independently measured sample trajectories.

At 256 and 2,048 frames the filtered kick-and-bass comparison fell from 0.003615 to 0.000446 stereo RMS error. With overlapping cutoff controls it fell from 0.004452 to 0.000333. The previous off, open and linked renders remained byte-identical. All repeated captures agreed across block sizes and all arms reported zero latency.

At 256/2,048 frames the cut pair used 59.1/472.6 µs median and 108.2/1,894.5 µs p99. The reference used 154.2/1,173.2 µs median and 1,619.5/6,927.8 µs p99. Deadline misses were 1/6 for ADI and 93/4 for the reference, per 20,480 callbacks at each size. Of the nine eligible saved manual settings, two select the full input and both leave these cuts open.

PR requested

## 22 November 2026 — Select the detector input for each Hold band

`clones/shaper-hold-input` at `2d5e13d5` — Select the split band or the full signal entering Crush. **Lands after 3 December.** Each band's selector defaults to the linked split. A quarter-level change before Crush produced byte-identical reference audio whether applied inside the rack or to its input. Reversing the two stages produced 0.0150–0.0166 stereo RMS error. Three band probes also distinguish the full stage input from the dry track input.

The rack, Crush and registered Pd suites passed under MSVC /W4 /WX in both precisions. At 256 and 2,048 frames, selecting the full input brought stereo kick-and-bass RMS error from 0.002462 to 0.000282 in the middle band and from 0.002386 to 0.000280 in the low band. Unsplit and earlier linked renders were byte-identical. Reported latency was zero.

At 256/2,048 frames the middle-band case used 58.5/541.8 µs median, versus 140.2/1,027.7 µs for the reference. Its p99 was 130.4/6,667.0 µs, versus 437.9/13,988.8 µs. Deadline misses were 15/25 and 90/22 respectively, per 30,720 callbacks at each size. On the low band the corresponding medians were 60.0/450.4 and 139.6/1,024.7 µs.

PR requested

## 22 November 2026 — Shared manual follower coefficients and percentage conversion

At a dyadic input step, the first nonzero stereo side sample gives the attack coefficient directly. On a replenished dyadic plateau, each sample immediately following exact equality gives the release coefficient. These fixtures produced 99 training and 66 separately chosen validation readings per control across 32, 44.1, 48, 88.2, 96 and 192 kHz.

For normalized u, the fitted attack duration in milliseconds is 0.005 + 99.995u³. The release duration is 5 + 55·expm1(u·ln(5050/55)). Both share a time scale of 1.00032789 and a third-order rational approximation of the exponential, with separately rounded numerator and denominator. The two-constant model reproduced all 198 training coefficients and 131 of 132 validation coefficients exactly. One blind attack reading differed by one unit in the last place. The prior implementation reproduced 54 of the 132 validation coefficients exactly. At 48 kHz the measured 20% attack coefficient is 0.025540947914 and the measured 11% release pole is 0.999484956264.

Percentage conversion is also audible on the moving Multiply test. With the same measured coefficients and clock, division of the physical 80% amount by 100 reduced the error from 0.002518 to 0.0000195 stereo RMS compared with multiplying by a rounded 0.01. The clones seat will use these independent readings to test the common follower in Width and Hold. Earlier hearing tests and the stored parameter IDs remain the acceptance controls.

## 22 November 2026 — Free-input filter response for the Hold follower

With the full-stage detector selected, each stereo channel is filtered before rectification and the louder channel drives the envelope. Fifteen exponential gain readings at each of three rates identified two equal bilinear poles at each active edge. At normalized control 0.25, 0.5 and 0.75 the measured cutoff frequencies were 116.541, 659.255 and 3,729.310 Hz. The frequency is 20.6017 Hz times 2 raised to ten times the normalized control. The twelve signed stereo cross-checks gave 0.00000077–0.00000232 RMS. Taking magnitudes before filtering gave 0.0286–0.0898 RMS instead.

The high-pass control has priority. Its effective range is zero to 95%. The low-pass control is raised to at least five percentage points above it. The high-pass zero endpoint and low-pass 100% endpoint bypass their respective filters. Active frequencies near Nyquist are limited to 0.4999 times sample rate. At 8, 16 and 22.05 kHz the saturated low-pass cutoffs measured 3,999.19995, 7,998.40019 and 11,022.79537 Hz. In linked-band mode both stored filter settings are dormant.

The clones seat reserves IDs 907–909 for each band's high-pass cutoff and 910–912 for its low-pass cutoff on the existing Hold detector branch. Defaults are zero and 100%. Tests cover signed stereo input, both slopes, colliding cutoff settings, near-Nyquist behavior, neutral-edge recall and live changes. The full-input tap was first isolated on Width using an upstream quarter-gain stage. The registered Hold comparison additionally checks the stage order in the original before scoring its full-input route. **Lands after 3 December.**

## 22 November 2026 — Resolve constant-curve precision and the remaining measured order fallbacks

Of 3,716 historical curves, 408 in 275 programs had a single free point plus eight identical scalar heights that represent the original value more accurately. Recalling the scalars reduced the median point error from 0.00000419 to zero. Exactly 301 heights then matched. The largest remaining height error was 0.000000119. All 598 ineligible saved files remained byte-identical. At the same decoded height, the point and scalar forms produced byte-identical full streams in all 550 controls across both clocks.

Among the 275 programs, 273 source-order streams and 272 fixed-order streams changed at each clock. In source order 28 errors fell, seventeen rose and 230 stayed within 0.000001. No error crossed 0.001 at either clock or in either order because of this curve representation. Fifty-three programs contained eligible Hold contours. In their isolated mapping comparison twelve source-order errors fell, twelve rose and 29 stayed within tolerance.

The fixed-order result remains 40/873. Replaying three previously unmeasured source-order cases replaced fixed-route fallbacks and changed the corresponding source-order baseline from 43/873 to 44/873 at both clocks. Two newly measured routes came under the threshold and one rose above it. The 44/873 count holds both before and after the curve change. All 175 previously recorded source-order baselines tested in this group reproduced. These measurements use private reference states.

## 22 November 2026 — Manual per-band Crush Hold follower

`clones/shaper-hold-input` at `9bec0183` — Control each Crush hold rate from its stereo band. **Lands after 3 December.** IDs 877–903 store the nine manual controls per band. The common detector is shared with Width. The rack, Crush and registered native/Pd tests passed under MSVC /W4 /WX at both Pd precisions. Muting only the modulation supplied to the clock failed both active regressions and preserved the disabled oracle. Older active Crush projects restore all followers off. Bypass and three follower-off curves reproduced both channels exactly.

On kick and bass the fixed Add stereo error fell from 0.025408 to 0.004308. Adding positive Hold took 0.026766 to 0.001356. The full-height Add case went from 0.005361 to 0.000751. Moving Multiply improved from 0.046824 to 0.036258 and remains discrepant. All results reproduced at 256 and 2,048 frames with zero latency. A separate reference-driven clock test distinguishes the phase carry through the 1,331 unity samples of the moving fixture. That comparison also detects small differences in attack, release and amount conversion.

Positive Hold took 22.5/198.1 µs median at 256/2,048 frames in the registered device versus 100.8/704.0 µs in the reference. Its p99 was 41.4/515.8 versus 201.8/3,073.3 µs. Moving Multiply took 28.8/226.2 versus 99.1/722.7 µs median, with p99 51.8/407.6 versus 191.7/2,507.3 µs. On that moving case ADI missed zero/one deadlines and the reference seven/zero out of 30,720 at each size.

Seven of sixteen historical Hold programs match the linked manual controls. In isolated Crush audio five errors fell, one rose and one was exactly unchanged. The largest improvement was 0.156404 to 0.118404 RMS. In their seven-stage mixes one fell, four rose and two stayed within 0.000001. Two other programs require the full-stage detector source, and seven use automatic release.

For Win, use this clean review branch. The earlier `clones/shaper-crush-hold-follower` ref is superseded following a commit-subject audit. Please retire it once review has moved. The complete source tree on the review branch matches the tested tree exactly.

PR requested

## 22 November 2026 — Choose the linked band or full input for the Hold detector

Two of the sixteen stored active Hold followers select a separate detector input with both filtering edges open. Both are in split mode. Changing only the detector source altered the original isolated output by up to 0.00019030 stereo RMS. In a different state with linked routing, changing the stored cutoff limits alone left every sample unchanged. The non-default low-pass cutoff is dormant while the detector follows its band.

An upstream Volume stage at one quarter gain distinguishes the raw device input from the input reaching Crush. Isolated low, middle and high bands supplied three independent controls. Linked routing followed its processed band exactly. With both filter edges open the separate route followed the current stage input before splitting to within 0.0000000156 RMS. Reading the original device input instead was about 0.20 RMS wrong. Reading the detector's own split band was 0.03–0.04 RMS wrong.

The clones seat takes one per-band source selector in the existing Hold port. IDs 904–906 select the linked band or the full stage input. An earlier state recalls the linked band in all three slots. Acceptance compares split bands after upstream gain, verifies the same input at opened filter edges and preserves all samples of earlier linked projects. This routing can cover two additional manual historical states. Separate tests cover finite high- and low-cut settings. **Lands after 3 December.**

## 22 November 2026 — Restore side-channel inversion in Width Multiply

`clones/shaper-width-follower` at `2859eb9f` — Preserve signed side gain in Width Multiply. **Lands after 3 December.** Width now uses the common ADI-Shaper follower implementation. Negative Multiply results can reverse the stereo difference while preserving the mid. Add retains its zero floor. The regression fails on the previous head. The rack and registered native/Pd suites passed under MSVC /W4 /WX in both precisions.

On the negative-offset kick-and-bass comparison, stereo error fell from 0.00599931 to 0.0000000140 RMS at both block sizes. The largest residual was 0.000000328. All twelve registered captures from the preceding six-state Width test remained byte-identical across 2,304,000 stereo frames. Both default renders passed the input exactly. Latency remained zero.

At 256/2,048 frames the revised device took 13.0/103.2 µs median, compared with 12.5/104.2 µs on the preceding head and 73.4/493.4 µs in the reference. Revised p99 was 22.0/212.3 µs versus 145.0/4,544.7 µs in the reference. Deadline misses were 1/1 for the revision and 7/14 for the reference across 30,720 callbacks at each size.

PR requested

## 22 November 2026 — Isolate the endpoint and stochastic clock interactions

All 163 historical programs with enabled Crush were replayed from a frozen weighted device. The endpoint change alone altered 42 stereo streams in fixed order and 43 in source order. Against the original, the fixed comparisons were eleven lower in RMS, nine higher and 143 within 0.000001. Source order gave nine lower, eleven higher and 143 within tolerance.

Adding the calibrated clock to the endpoint build gave fourteen lower, ten higher and 139 within tolerance in fixed order. In source order the counts were sixteen lower, twelve higher and 135 within tolerance. Applying both to the original frozen build gave 22 lower, seventeen higher and 124 within tolerance in fixed order, and 21 lower, twenty higher and 122 within tolerance in source order. Separately rounding stochastic timing changed 34 of 39 fixed-order streams and 33 of 39 source-order streams. None of these comparisons crossed the 0.001 stereo RMS threshold. The private inventory also found 408 single-point lanes with a quantized point and an independently stored, more precise constant height. Their mapping is undergoing controlled replay.

## 22 November 2026 — Automatic release in the two input followers

Toggling automatic release alone changed all seven original Hold programs that enable it. On kick and bass the isolated stereo RMS change was 0.001205–0.015062. Twelve new comparisons of signed stereo markers selected the current-frame signal from Width as the control for Hold at 44.1, 48 and 96 kHz. Shifting the control by either one frame raised each error to at least 0.227 RMS.

The manual release setting still controls the extremes. The slow limit is approximately the selected manual time plus 250 ms, and the fast limit is about one quarter of the manual time. At 20% these measured approximately 336 ms and 21.46 ms. For a 0.625 to 0.00390625 amplitude step, the midpoint in decay rate arrived 16.92–16.99 ms after the edge across the three rates. A later four-frame pulse below the current envelope still changed the subsequent envelope by 0.00914. Increasing manual Hold delayed the audible fall while the automatic release detector continued to track the underlying input.

The repeatable test holds a half-scale peak for 500 ms and reads the next falling envelope sample each time the outer envelope returns to exactly half scale. Twice that falling sample gives the instantaneous decay coefficient. A small drop and a large drop produce different fastest rates. Pulse pairs at five and 1,024 frames reject both a fixed transition timer and the sum of isolated pulse responses. Automatic release remains outside the manual port pending a detector that also predicts these held-out pairs.

## 22 November 2026 — Width can reverse the side channel in Multiply

When a negative offset puts the normalized modulation below zero, Width's Multiply mode keeps the signed result. Add has a zero lower limit. A positive result in either mode tops out at one, corresponding to a physical side gain of two. At base 0.125, amount 0.5, depth two and minimum offset, a 0.25 input peak produced a normalized result of −0.96875 in Multiply. The physical side gain is −1.9375, which reverses the stereo difference while keeping the mid signal.

Two base heights, both modes, depth two and eight, five offsets and seven dyadic levels gave 210 original plateau readings. Applying the mode-specific lower bound reproduced all 210 readings exactly. Five separately rendered Crush Hold fixtures at 44.1, 48 and 96 kHz compared an equivalent negative modulation with the original fixed minimum-rate contour, using one constant-magnitude channel and an independent marker channel. Both channels matched at every sample. The shared detector therefore preserves the signed Multiply modulation through Width and the Hold clock limits the rate to its own physical range. The clones seat keeps these changes within the two existing ADI-Shaper topic branches.

## 22 November 2026 — Width follower holds the input peak and follows manual release

`clones/shaper-width-follower` at `4607d889` — Track sliding input peaks and continuous release in Width. **Lands after 3 December.** The detector retains the largest input in the selected window before attack/release. Zero Hold pauses its history. Positive Hold can recover an earlier peak after a live window increase. Storage is prepared outside processing and supports the full half-second through 768 kHz.

The continuous manual release fit used 99 decay readings and 45 blind readings. On the blind set 36 rounded coefficients matched exactly and the remaining nine differed by one unit in the last place. All 36 held-out signed envelope traces improved, with median RMS falling from 0.03750 to 0.00000108. Rack and registered Pd suites passed under MSVC /W4 /WX at both Pd precisions. The established long decay, sustained levels and the oldest peak at 768 kHz are covered.

On 48 kHz kick and bass, the fixed positive Hold error fell from 0.000579085 to 0.0000000911 stereo RMS. Changing Hold during playback reduced it from 0.00111789 to 0.0000001417. The widened pair went from 0.0132070 to 0.000002788. Both block sizes produced the same readings. The default passed the input exactly and reported zero latency. Six historical manual Width maps were rescored against the seven-stage branch. The largest change in their overall RMS score was below 0.00000005.

With positive Hold the previous, revised and reference devices took 9.5, 11.8 and 71.9 µs median at 256 frames, and 74.0, 104.2 and 502.0 µs at 2,048. Revised/reference p99 was 19.0/141.2 and 212.4/2,545.9 µs. Their deadline misses were 3/21 and 0/12 out of 30,720 callbacks at each size. The sliding window costs more than the prior held-envelope approximation. A new negative-offset reading produces signed Width side gain below zero outside these fixtures. Its lower bound is under measurement.

PR requested

## 22 November 2026 — Manual input modulation of the Crush Hold rate

The clones seat takes the per-band Hold follower in the ADI-Shaper DSP, Pd canvas and device tests. Controls 877–903 cover each band's enable, mode, amount, depth, offset, threshold, attack, hold and release. An older state restores all three followers off. This addition lands after 3 December. The three copies use the same manual detector law as Width.

The original takes the louder absolute channel in the band. With current envelope E, normalized base height B, amount A and depth D in {2, 4, 8}, Add applies B + D(2A−1)E and Multiply applies BDAE. Offset moves this result by twice its normalized departure from halfway, followed by limiting to [0,1]. At base 0.5, amount 0.39, depth 2 and sustained half-scale, Add yields about 0.28. At base 0.5 and amount 0.8, Multiply yields about 0.4. A signed full-band stereo fixture discriminated the current frame from either adjacent frame at three sample rates. The falling response and positive-window retention use the separate Width acceptance fixtures above.

Tests must cover both signs of Add, Multiply, offsets, threshold, different band inputs, flat and moving base contours, equal-state native and Pd renders, old-state recall, and registered kick and bass at 256 and 2,048 frames. Automatic release, other detector filter settings and soft knees need separate readings.

## 22 November 2026 — Clock precision across the weighted historical device

The fixed-order result is 40/873 before and after the clock repair. Restoring all newly measured source-order routes brings the comparison to 43/873 before and after. The earlier 42/873 total used a fixed-order fallback for one program. Its fallback RMS was 0.00143269, its old clock with the restored order was already 0.000996198, and its corrected clock with that order is 0.000996307. No program crossed 0.001 because of the clock change. The earlier group of 81 kept its one match on both routes.

Of 163 enabled programs, thirteen source-order errors fell, eleven rose and 139 were within 0.000001. On the fixed route the counts were twelve lower, nine higher and 142 within tolerance. There were 104 and 105 changed streams respectively. The largest stereo differences were 0.037869 and 0.032713 RMS. All 710 programs without active Crush reopened identically. All 873 files in the old fixed-order baseline were re-rendered and reproduced their archived hashes.

For attribution, the primary comparison preserves the existing jitter calculation on jitter-positive bands. Only zero-jitter bands receive the calibrated clock. On the 101 zero-jitter stage-isolated programs whose original audio changed with Hold, 23 errors fell, 22 rose and 56 were within tolerance. Median residual moved from 0.00119846 to 0.00119736. The integrated zero-jitter kernel reproduced the submitted native core on 11,520,000 stereo frames. The full-rate endpoint and jointly changing the jitter calculation are separate comparisons.

## 22 November 2026 — What the Width follower holds when its input falls

Independent signed stereo steps show that positive Hold stores the recent input maximum ahead of the attack/release detector. After a one-frame 0.75 transient at 48 kHz and Hold 0.035, the measured envelope rose from 0.74029 through 0.74985 to 0.749998 and 0.75 after the input had fallen. Freezing the current envelope value misses the attack. A descending ramp at 44.1 kHz matched the sliding input maximum to 0.00000006. The strict input/envelope comparison and per-sample rounding also account for the sustained plateau ripple documented in the Hold follower fixtures.

When the Hold setting changes between positive values, the new window takes effect immediately. Lengthening it may recover a peak that was outside the smaller window. At zero, the detector takes the current input and pauses its hold-history cursor. Restoring a positive value can recover pre-zero input. Samples received while Hold was zero do not enter this history. Eighteen changes in an 8,072-frame run matched at 0.0000000143 RMS with blocks of one and sixteen. The 256-frame external host delivered the change on its callback, giving 0.0000000198 RMS against the same law. Clearing the history on entering zero gave 0.0543 RMS.

The clones seat takes the owned Width follower core and its tests on the existing `clones/shaper-width-follower` review branch. **Lands after 3 December.** Keep neutral and switched-off audio unchanged, prepare storage away from audio processing, verify the previous Width acceptance cases and measure the revised active detector at both block sizes. The same detector response is required by the future Crush Hold control.

## 22 November 2026 — Faster moving hold contour with the exact calibrated output

`clones/shaper-crush-clock-precision` at `51901a1a` — Reduce the cost of moving Crush hold rates. Three small factor tables replace the exponential polynomial, and a new period is computed at its first capture. The tables add 4,736 shared bytes and no per-instance memory. Each supported rate still uses its original division. The 8,160,049 possible logarithmic inputs retained their rounded exponential. Across seventeen rates the steps and stereo samples matched in 835,584 comparisons, including the extreme rates. All 96 registered comparison captures were byte-identical to the calibrated device. The rack, Crush and registered native/edited-Pd suites passed at both Pd precisions under MSVC /W4 /WX.

Across six alternating pairs, the wide-ramp median/p99 at 256 frames fell from 25.5/78.5 to 20.9/57.7 µs, and at 2,048 from 213.6/1,399.9 to 166.8/1,016.9 µs. Misses fell from 22 to 21 and from three to zero in 61,440 callbacks per arm and size. Registered thread cycles fell 17.6% and 20.0% with six wins in six pairs at each size. The exact-height triangle fell from 26.9 to 21.4 and from 213.4 to 166.0 µs median. Its cycles fell 19.2% and 21.2%. The calibrated wide-ramp comparison measured source medians of 77.8 and 472.5 µs.

The steady fractional control measured 15.1 and 118.2 µs median, compared with 15.0 and 118.9 before this optimization. Its 256-frame misses rose from seven to nineteen, and neutral 2,048-frame misses rose from one to two. These timing changes accompany identical audio. All 873 fixed-order weighted renders have now been checked on the calibrated behavior. The score remains 40/873 and all 710 states without active Crush are identical.

PR requested

## 22 November 2026 — Keep the calibrated clock rounding on fused targets

`clones/shaper-crush-clock-precision` at `ca4ee5fa` — Keep the two measured clock operations separately rounded. This updates the timing repair at `de769084`. Five of 43 measured heights gave a different logarithmic argument if the compiler fused the height multiply and offset addition. At height 0.38999999 and 48 kHz, a late signed left marker measured +0.00388830. Fused arithmetic produced −0.01766950. A local rounded intermediate and five new signed source markers protect the result. Optimized assembly confirms distinct multiply and add operations. The numerical counterexample uses an explicitly fused calculation.

All three rack, Crush and registered suites passed under MSVC /W4 /WX at both Pd precisions. The guarded calculation also passed the five new readings and both previous extreme-rate cases. The full-height and constant-control paths keep their previous behavior. In seventeen paired moving-control checks the median guarded/prior ratios were 1.044 and 1.002 at 256 and 2,048 frames, and both interquartile ranges included one.

PR requested

## 22 November 2026 — Acceptance fixtures for the Crush Hold envelope follower

Sixteen of the 873 historical programs enable the Hold follower in an audible band. Fifteen use Add and one uses Multiply. Fifteen changed when the follower alone was disabled in an isolated Crush replay. Changes on the second bar spanned 0.0000144–0.156404 stereo RMS. All eight eligible full-chain comparisons changed, with a maximum of 0.060494 RMS. Reopening the original positive state gave exactly the same audio on every trial. Seven states enable automatic release, and one stores a different detector filter setting.

The Width and Hold reference controls can be compared without access to either detector's internals. Feed equal and opposite signed stereo markers with dyadic levels, read Width gain directly from the side output, halve it and use that value to drive the measured hold clock. All 24 tests selected the current-frame control at 44.1, 48 and 96 kHz, including Add and Multiply, two depths, and two sets of attack/hold/release controls. The worst reconstructed audio RMS was 0.000000442 in Add and 0.00001192 in Multiply. The active detector must continue operating on a flat full-height drawn curve.

A quarter-scale plateau with minimum attack and release exposed a difference in the existing Width follower. The original cycles through 0.2489609, 0.2499838, 0.2499998 and 0.25 at 48 kHz. At 96 kHz the cycle takes seven frames. A constant-level estimate misses these variations. The measured baseline and held-out downward steps will govern a shared detector. The Hold port is parked on that common response and the Width front end. The seven automatic-release settings need their own readings.

## 22 November 2026 — Repair sustained fractional sample-and-hold timing

`clones/shaper-crush-clock-precision` at `de769084` — Calibrate the sample-and-hold clock over sustained playback. Across 2,891,211 measured crossings in 129 rate/height cases, every case improved. On 39 blind cases median timing RMS fell from 0.0563432 to 0.00000117 sample and the worst case fell from 3.28477 to 0.00000484 sample. All 8,160,049 representable logarithmic arguments matched the correctly rounded exponential in an exhaustive check. The cache occupies 2,688 bytes. Main failed the sustained signed marker at 96 kHz. Rack, Crush and registered native/edited-Pd suites passed under MSVC /W4 /WX at both Pd precisions.

At 256 and 2,048 frames on registered kick and bass, stationary fractional-hold stereo RMS fell from 0.001614 to 0.0000000315. A continuously moving triangle with identical physical heights fell from 0.000584 to 0.0000000315. A wider ramp fell from 0.001398 to 0.00001019. For that ramp, the remaining one-ULP contour-height differences independently predict the residual. The full flat capture stayed byte-identical to main. All 72 timed streams passed stereo hash checks and reported zero latency.

Wide-ramp registered median/p99 at 256 frames was 26.8/76.2 µs for ADI against 77.8/256.2 µs for the source. At 2,048 the figures were 210.9/1,033.5 against 472.5/4,454.3 µs. ADI missed 17/1 deadlines at these sizes against 51/6 in the source, in 30,720 callbacks per cell. Main took 24.9/193.7 µs median. Registered thread cycles rose 6.2%/9.6% for the more accurate moving clock. The steady fractional medians were 15.9/123.9 µs, against 60.5/329.9 µs in the source.

Across all 873 seven-stage programs 128 output streams changed. Eight RMS errors fell by over 0.000001, eleven rose and 854 were within tolerance. All 710 without active Crush stayed identical and 17/873 remain below 0.001. The sample clock and full-rate latch are independent fixes. The old and corrected clock both use the same main full-rate behavior in this comparison.

PR requested

## 22 November 2026 — Fractional hold-clock calibration over sustained playback

The existing exponential approximation accumulates measurable timing drift on an unchanged hold height. Three-second stereo marker trains at 44.1, 48 and 96 kHz distinguish rounding the normalized phase after each sample addition and before subtracting one on a wrap. The physical curve is a shifted exponential through 8.82 Hz and 44,100 Hz with a 225.96 Hz offset. Form the signed logarithmic span and each intermediate height operation in binary32, round the exponential and subtract the offset, then round the sample-normalized advance. The measured stored heights and the physical normalized values entering the DSP are distinct at the final bit in some cases.

On sixty calibration cells and thirty previously unused height/rate combinations, the largest crossing RMS over three seconds was below 0.000006 sample. Across twelve separate moving stereo fixtures, including signed warm playback over a full-rate segment, the model's largest audio RMS was 0.00000173. Carrying fractional phase with the old frequency law still missed by as much as 0.01293 RMS. This calibrates clock precision separately from the moving endpoint's stored capture.

The clones seat takes the owned Crush stage and its direct tests on `clones/shaper-crush-clock-precision` for the current-stage repair. Preserve stationary full-height audio, zero latency, the reset contract and native/Pd parity. Benchmark moving controls as well as fixed controls at both callback sizes.

## 22 November 2026 — What remains after the Crush endpoint and prefilter repairs

The full-rate latch fix and callback-grid fix pass the strict rack suite together. The musical linear-curve residual has a separate cause. The returning source node is at cell 8,191 of 8,192. The eight-point native ramp returns at the following cycle's first sample. Moving only the last anchor in the measured model reduced RMS from 0.004482 to 0.000554, preserving all 752 full-height frames per cycle. On the stepped curve the signed source table's cubic read reduced RMS from 0.005834 to 0.0000258. Both block sizes gave the same source capture. Keep the audible latch fix and use these acceptance cases when the full point editor can land.

A separately isolated prefilter discrepancy comes from the Hold envelope follower. With the original follower on, changing the prefilter produced 0.118414 RMS in the isolated stage and 0.051518 in the full rack. The identical switch with only the follower disabled produced bit-identical output in both. The drawn Hold curve is at full height, with follower amount 0.39 and hold 0.52. The present saved mapping supplies that flat curve without the follower. Across the thirteen isolated source ablations, enabling the new filter lowered error in eight, tied in three and raised it in two. Seven nonzero effect pairs correlated from 0.9084 to 0.9987.

## 22 November 2026 — Per-band prefilter before the fractional Crush hold

`clones/shaper-crush-prefilter` at `f0545461` — Add the calibrated eight-pole prefilter before the Crush hold. **Lands after 3 December.** The three independent switches at IDs 874–876 default off. The filter freezes its state while switched off. At a changing cutoff all sixteen poles share one cached coefficient. The capped response, rise and fall, signed stereo, reset and single-channel nonfinite recovery passed. A missing filter, a different rounding order in the pole update, and clearing history on switch-off all failed independent regressions. The rack, Crush and registered native/edited-Pd suites passed at both Pd precisions under MSVC /W4 /WX. With the switch off, 196,608 stereo frames matched the prior binary exactly.

With the prefilter on, registered kick and bass took 31.7/177.5 µs median/p99 at 256 frames and 284.7/966.6 µs at 2,048. The reference took 49.3/85.2 and 354.3/1,981.9 µs. Both missed one deadline at 256 frames and zero at 2,048 in 10,240 callbacks per size. The largest channel RMS error was 0.000000043. With the prefilter off the ADI medians were 26.1 and 199.7 µs, and the earlier stereo recordings reopened exactly. Latency was zero throughout.

Thirteen historical programs enabled the prefilter. Ten weighted source-order outputs changed, from 0.000103 to 0.03103 RMS. Five errors fell, five rose and three were within 0.000001. The fixed-order counts were five lower, four higher and four within tolerance. Of the three prior regressions one improved, one worsened and one was unchanged. The source-order and fixed-order counts remain 42/873 and 40/873. The seven-stage comparison gave five lower, three higher and five within tolerance.

PR requested

## 22 November 2026 — Preserve the fractional Crush latch through a full-rate plateau

`clones/shaper-crush-endpoint` at `761744fb` — Retain the Crush latch phase through a full-rate plateau. This repairs the existing stage on main. In the cold and 8,192-frame silent-preroll test, the original retained 0.071371 of a cycle when it left the full-rate segment. On the first post-plateau stereo marker the old core gave −0.250000 on the left against the measured +0.110614. A moving contour at full rate emits current audio and retains the interpolated capture. A flat full-rate contour freezes the phase and keeps the latest direct sample. A capped, sub-full rate keeps the interpolation. The new source markers distinguish all three cases. The earlier hold-grid entry described the phase reset in the old ADI implementation. These readings correct the reference behavior and preserve the separate callback-grid fix.

Rack, Crush and registered native/edited-Pd passed under MSVC /W4 /WX at both precisions. All 873 seven-stage kick-and-bass programs were re-rendered. Thirty-six changed, seven errors fell by over 0.000001 RMS, three rose and 863 were within tolerance. All 710 without active Crush were identical. The count below 0.001 remained 17 in both runs. This is the direct seven-stage comparison.

At 256/2,048 frames the corrected registered device measured 16.3/126.0 µs median on the stepped contour and 24.8/192.9 µs on the linear contour. The corresponding reference medians were 74.5/453.1 µs and 79.4/469.7 µs. The corrected runs missed 0/0 and 2/0 deadlines respectively per 10,240 callbacks. On the musical stepped fixture, the larger channel RMS fell from 0.005892 to 0.005596 at 256 and from 0.005980 to 0.005519 at 2,048. On the linear fixture it rose from 0.004242 to 0.004482. Direct phase markers pass on both ears. The stationary fixture retained its exact previous waveform. All paths report zero latency.

PR requested

## 22 November 2026 — Jitter attribution over identical scoring windows

The weighted 1,505-control replay of `clones/shaper-crush-jitter` at `0b6ca57e` and the source agree on which programs respond. Across 39 pairs the same six are exactly unchanged on the scored bar and 33 change. Their source changes span 0.00000317–0.009969 RMS. ADI's authored sequence spans 0.00000202–0.021274 RMS. Eight source comparisons used the full chain and 31 used the saved Crush stage. Fresh reloads of the same source state were repeatable. Five responsive startups had settled before the scored bar.

In source order ten errors fell, twelve rose and seventeen were within 0.000001. In fixed order the counts were nine lower, fourteen higher and sixteen within tolerance. The one affected passing program stayed below 0.001 RMS, changing from 0.00040215 to 0.00046918. The pipelines retain 42/873 in source order and 40/873 in fixed order. Each error compares two specific renders. Independent stereo clock-distribution tests cover the timing behavior across draws.

## 22 November 2026 — Crush prefilter measurements and acceptance cases

Eighty-eight signed stereo transfer measurements and five independent broadband order checks put eight identical first-order low-pass poles after bit reduction and before the fractional sample hold. The cutoff tracks the nominal hold frequency every audio frame. For each pole use the bilinear gain g/(1+g), where g = tan(π times cutoff divided by sample rate). The independently fitted maximum gain was 0.999686062335968 at 44.1, 48 and 96 kHz. A 48 kHz control step measured 0.0000000368 RMS against a current-frame coefficient. Interpolating that coefficient across just two samples missed the transition by over 0.028 peak.

The off switch freezes both channels' filter memories. An entirely flat, exactly full-rate curve passes audio directly. A full-rate plateau in a moving curve runs the filters at their capped gain, including a plateau at the start of the curve. A 3,000-frame plateau at 96 kHz differed from the dry path by 0.015715 RMS. Its re-entry transient reached 3.24 against the normalized input range. The capped gain and single-precision pole histories reproduced the extreme case to 0.000000280 RMS and 0.0000226 peak. Independent kick, bass and signed markers cover ordinary rates and both sides of the cap. The port must preserve the measured transient and a disabled stage's prior sound.

The clones seat takes the owned Crush DSP, rack and Pd declarations, stage tests and registered tests on `clones/shaper-crush-prefilter`. IDs 874–876 select the prefilter per band. Existing projects restore the off state. Thirteen active programs and three of the earlier 81 regressions use it. **Lands after 3 December.**

## 22 November 2026 — Recallable stereo-shared clock jitter in Crush

`clones/shaper-crush-jitter` at `0b6ca57e` — Model per-band Crush clock jitter and stereo latch timing. **Lands after 3 December.** IDs 871–873 recall each band's amount, defaulting to zero in older projects. Two equal-seeded triangular streams advance independently for dither and timing. In the reference their draws correlated at 0.999999897 while their positions agreed. Pausing dither for 1,024 frames and resuming gave 0.000000715 RMS against the paused sequence. A full-height flat hold also paused its timing sequence while dither continued. The model uses ADI's deterministic generator. Smoothing when automating the amount remains open.

The original core failed the new timing-distribution test. Signed stereo latch tests at four rates, cap and reset cases, and registered native and edited-Pd replays passed. The rack, Crush and registered suites all passed under MSVC /W4 /WX at both Pd precisions. At 75% jitter on kick and bass, registered median/p99 was 27.3/42.4 µs at 256 and 214.6/4,904.6 µs at 2,048, against 61.7/101.2 and 362.2/11,872.6 µs. ADI missed 11 and 32 deadlines per 10,240 callbacks respectively, against 9 and 13 in the reference. Each implementation independently gave identical recordings at both block sizes. Between the two generators the signed stereo RMS difference was 0.03870/0.03756. At zero, ADI and reference each reproduced its earlier recording exactly. All four comparisons reported zero latency.

Across the 39 affected seven-stage programs, twelve RMS errors fell, eight rose, and nineteen were within 0.000001. Separate verification of the prior treble control found all 854 neutral fixed-order programs identical over 327,936,000 stereo frames. A decimal transcription in the earlier moving-resolution CPU entry is corrected to the original gate reading of 0.00000166 RMS.

PR requested

## 22 November 2026 — For Devices, the two Time locate tests and their prerequisites

The core API and its regression are on `clones/shaper-linear-reconcile` at `e8ff2ead`. In `test_adi_shaper.cpp` the buffered stereo marker survives a contiguous call with the existing revision. A revision change at the predicted sample clears it. The registered native factory case still needs the Devices-owned wrapper to forward quarter notes, exact samples and transport revision to `syncAtSample`. At the 128-frame boundary, keep the timeline at its predicted frame, change revision from 1 to 2 and require the delayed marker to disappear. Keep the edited-Pd case on its explicit 192,000-frame jump. The registered test failed before forwarding and passed after forwarding in private. Its test hunk belongs to clones and is parked for the Time stack and owner hook. The shared two-head reader is separately available for Devices review on `clones/two-head-reader` at `9b7cd7ca`.

## 22 November 2026 — Per-band Crush treble follows both curves

`clones/shaper-crush-treble` at `edc8ee1f` — Calibrate Crush treble against bit and hold contours. **Lands after 3 December.** IDs 868–870 store each band's treble independently. Old states reopen at 100%. The 350 Hz filter keeps its history at full treble so the next edit can use the preceding audio. The knob, native and edited-Pd paths all passed under MSVC /W4 /WX at both precisions. Applying the shelf after the DC filter failed the moving marker by 0.00314. Discarding its history at full treble failed a later automation marker by over 0.55. Across three rates, 196,608 neutral stereo frames matched the previous binary and 1,769,472 whole, split and irregular stereo comparisons matched exactly.

At 30% treble on registered kick and bass the median/p99 was 26.3/42.3 µs at 256 frames and 235.1/327.3 µs at 2,048. The reference took 44.6/137.8 and 251.4/1,442.7 µs. The largest channel RMS error was 0.000000063. At full treble the large-block median was 231.6 µs for ADI and 231.2 µs for the reference. ADI missed zero deadlines in 40,960 callbacks across both settings and sizes. All paths reported zero latency.

Of nineteen programs with reduced treble, eighteen changed sample output. In the other, both coupled curves were at full height throughout. In the source-order weighted comparison ten errors decreased, five increased and four stayed within 0.000001 RMS. The one earlier regression improved from 0.423969 to 0.419908. The fixed-order result was eleven lower, four higher and four within tolerance. All fifteen overlaps with previously calibrated stages reproduced both prior paths. The measured pipelines remain at 42/873 in source order and 40/873 in fixed order. On the seven-stage main comparison nine improved, five worsened and five stayed within tolerance. Separate study found a neutral fractional-hold pre-roll difference around one moving resolution edge, independent of this treble port.

PR requested

## 22 November 2026 — Faster moving Crush curves with the calibrated crossing intact

`clones/shaper-crush-resolution` at `2afb86d5` — Reuse the preceding quantizer code for moving Crush curves. This updates the current-stage repair at `77f4ec0a`. It keeps the previous integer code per ear and takes a shorter path for normalized controls. It also preserves the original rules for infinities, NaNs, finite out-of-range controls, full resolution and resets.

Compared on the same registered thread, processing cycles fell by 13.60% at 256 frames and 15.60% at 2,048 for a continuously moving bit curve. Steady controls fell by 18.89% and 21.58%, and the eight-step curve by 14.43% and 16.28%. A separate six-order comparison of registered processes measured moving-curve medians of 26.9 to 17.8 µs and 213.55 to 141.10 µs. The respective p99s fell from 41.35 to 30.50 µs and from 471.05 to 287.10 µs. Deadline misses rose from 9 to 11 and 7 to 10 per 61,440 callbacks. The original 0.00000166 worst transition RMS and the saved-program counts from `77f4ec0a` apply to the same rendered signal.

The two versions produced identical stereo outputs on all six registered musical fixtures, 18 cross-rate traces, 589,824 fixed-control frames and invalid-control recovery cases. The rack, Crush and registered native/edited-Pd suites passed under MSVC /W4 /WX at both precisions. Reported latency remains zero.

PR requested

## 22 November 2026 — Random variation of the Crush sample clock

The two ears take their signed samples at the same fractional time. The jitter control varies a cumulative clock every audio frame. Inverting the independently marked left and right inputs recovered held stereo output to 0.000000553 RMS and 0.00000412 peak. All 36 matching runs at 256 and 2,048 frames were byte-identical across three sample rates. Doubling the knob from one quarter to one half scaled the recovered random speed deviation by 3.9993 across 2,164 joint frames. Doubling from one half to full scaled it by 3.9996. The clock is independent of the dither switch.

An independent reproducible model uses a triangular draw T on [−1,1]. For normalized jitter J, the unbounded speed is approximately `1 + 10 J² T − 2.7183 J⁴`. Multiply this speed by the nominal fractional advance and clamp the resulting increment to [0,1]. On six held-out settings the interval mean missed the measured value by at most 0.00685 of one nominal period and the 5th, 50th and 95th percentiles by at most 0.0187. The exact reference random-number sequence is unknown. ADI can reproduce the measured statistics with its own deterministic sequence.

An increment clamped to one emits the current input but preserves its fractional remainder. The stored capture for the following frame uses the preceding and current inputs at the actual crossing. Across 21 high-rate conditions the phase error was 0.0014–0.0042 cycles for this rule versus 0.265–0.291 when carrying excess speed. A truly flat, unsmoothed full-height section pauses both phase and stochastic position. An equally high plateau within a moving contour continues to draw. Inserting 1,024 flat bypass frames moved later captures by exactly 1,024 frames. A fresh state restarts repeatably.

Of 1,048 stored settings, 39 use jitter in an active band, including two earlier Trim regressions. Thirty-eight of those 39 changed the stereo kick and bass in a matched switch-off check. Whole-chain comparisons were made for eight programs and isolated Crush comparisons for 31. Five full-height stored contours had an audible startup difference through smoothing, falling below 0.0000001 amplitude by 158 ms. All 31 additional settings found only in inactive stages gave a null.

The clones seat takes `adi_shaper_crush.*`, `adi_shaper.*`, `ADI-Shaper.pd` and the matching stage, rack and Pd tests on `clones/shaper-crush-jitter`. IDs 871–873 are reserved at 0–100% with a zero legacy default. **Lands after 3 December.**

## 22 November 2026 — Crush preserves physical levels when the resolution changes

`clones/shaper-crush-resolution` at `77f4ec0a` — Preserve each bit resolution across a Crush threshold crossing. This fixes the current stage on main. The old core returned 0.07724228 on the first independent stereo marker against 0.08445889 measured. Across the nine transition fixtures, the worst RMS fell from 0.032832 to 0.00000166. All 589,824 stationary stereo frames in a separate binary comparison retained their exact samples. The rack, Crush and registered native/edited-Pd suites passed under MSVC /W4 /WX at both precisions.

On the registered stereo kick and bass with a linear bit curve, the per-channel error fell from 0.002342/0.002475 to 0.000231/0.000499 at both 256 and 2,048 frames. Median callback times in the first pass were 27.0 and 223.0 µs versus 36.4 and 221.8 µs for the reference. Six additional order-balanced 2,048-frame runs gave median callback times of 208.4 µs before the fix, 225.8 µs after it and 346.3 µs for the reference. Their median p99s were 1,870.6, 1,461.2 and 4,983.1 µs. They missed 51, 61 and 128 deadlines respectively in 61,440 callbacks each. Every capture reproduced its stored stereo waveform. Full-rate transitions and fixed controls retain zero reported latency. The eight-step rack approximation has larger corner errors than the direct core comparison.

All 873 seven-stage programs were rendered through both kernels. The count below 0.001 stereo RMS remained 17. On the weighted device, 45 original programs have moving bit contours. In source stage order six errors fell, five rose and 34 stayed within 0.000001 RMS. Of five movers in the earlier 81-program regression group, three improved and two were within tolerance. All 25 cases overlapping a previous bias, Sweep or Room addition reproduced their prior centered-build input to the comparison. The 828 stationary or inactive cases preserve 42 of 873 below threshold in the source-order pipeline and 40 in weighted fixed order. Neither conditional count changed. For the eventual bias merge, keep each endpoint's physical level in both the audio calculation and the dither-only subtraction.

PR requested

## 22 November 2026 — The Crush treble control

Nineteen of the 873 rendered programs use a reduced treble setting on an audible Crush band. One is among the 81 prior regressions. The reference passes the low frequencies and attenuates the highs as bit resolution or hold rate drops. If normalized bit height is B, hold-rate height H and treble T, the high-frequency gain is T + (1−T)BH. A first-order bilinear low-pass at 350 Hz separates the low component. At full treble the band passes its existing processed signal. The shelf follows the sample hold and precedes the measured 10 Hz DC filter.

Forty-eight independent signed stereo broadband fixtures at 44.1, 48 and 96 kHz covered off-grid settings and both prefilter positions. Independently fitting the two channels gave a 349.99996–350.00010 Hz corner. The product law reproduced the low-treble outputs within 0.000000234 RMS. Eight moving tests used two independent, off-grid stepped or sloped contours at both callback sizes. The worst RMS was 0.000000073 before DC filtering. Applying the same shelf after DC filtering gave 0.0000465–0.000455 RMS. The full-resolution/full-rate endpoint is neutral for any treble setting.

The clones seat takes `adi_shaper_crush.*`, `adi_shaper.*`, `ADI-Shaper.pd` and the shaper's rack, Crush and Pd tests on `clones/shaper-crush-treble`. Public IDs 868–870 are reserved for the three treble values, with 100% as the old-state default. **Lands after 3 December.**

## 22 November 2026 — Which Room timings reproduce the reference hall

The 70.25/92.35 µs and 525.30/635.53 µs ADI/reference readings above are for the verified settings. All 16 active reference streams reproduced an independent stereo impulse convolved with the actual input, within 0.000000050 RMS per channel. An unrotated control reproduced it within 0.000000062. On an impulse, ADI and the reference first respond at one sample. The time to 95% of the response energy is 202.69 ms for ADI and 260.13 ms for the reference. At 99% the times are 341.98 and 460.35 ms.

The earlier 89.9 and 556.7 µs reference runs produced repeatable wet audio at both buffer sizes. Their captures differed from the verified hall by 0.30437 RMS left and 0.31118 right. Those older Room readings are superseded for the hall comparison. The earlier white Noise measurements have independent procedural level and exact dry checks. In the final Room comparison ADI had a lower gross median in all eight rounds at both sizes. After subtracting bypass, it had a lower median in five of eight at 256 and seven of eight at 2,048.

## 22 November 2026 — Room compared with the installed reference after optimization

On 48 kHz stereo kick and bass the registered Room at `97f9ffd7` took a gross median of 70.25 µs at 256 frames and 525.30 µs at 2,048. The corresponding reference readings were 92.35 and 635.53 µs. Subtracting bypass within the balanced runs yielded 66.55 and 501.00 µs for ADI versus 71.90 and 594.48 µs for the reference. Gross p99 was 1,008.75 and 7,602.70 µs versus 1,238.85 and 14,853.35 µs. ADI missed 71 and 9 deadlines out of 16,384 active callbacks at each size. The reference missed 52 and 37. Small-block median and p99 improved while the number of small-block misses remained higher for ADI.

Both hosts used matching controls, all 16 enabled stereo outputs matched their respective verified recordings, all 16 bypass captures matched dry and all paths reported zero latency. The sources use different room responses. An initial batch where the reference response had yet to load was excluded. The repeat allowed initialization before timing and checked the entire wet waveform for every run. These readings compare callback wall time across hosts. The earlier 27% steady-control cycle reduction compares the two ADI implementations on the same thread.

## 22 November 2026 — Stereo reflection processing for Room

`clones/shaper-reverb-port` at `97f9ffd7` — Accumulate stereo room reflections in one ordered pass. **Lands after 3 December.** This updates the existing generated Room submission. Each reflection now stores its two gains together. The processor finds the valid end of the reflection list once per frame, then accumulates both channels in the original order. Other architectures retain a scalar path. A 32-bit scalar build passed the expanded Room suite.

On the 48 kHz stereo kick and bass, processing-thread cycles fell by 27.53% at 256 frames and 27.41% at 2,048 with steady controls. With moving send, return and room controls they fell by 19.90% and 23.16%. Every active paired trial improved. Three independent registered instances received the same input block on the same thread in rotating order. The new version's median wall times were 45.05 and 599.83 µs steady, and 57.23 and 378.15 µs moving. These compare implementations of ADI's authored response.

At both buffer sizes all 162 eligible stored responses retained their stereo samples, a total of 31,104,000 frames. The independent direct suite checked 7,204,144 frames at six rates. Two new assertions catch a swapped reflection gain and a stale read when a long predelay follows a reset. Each intentional defect passed the previous suite and failed its new assertion. The rack, Room and registered native/edited-Pd suites passed under MSVC /W4 /WX at both precisions. Latency remains zero.

PR requested

## 22 November 2026 — Measured Crush transitions between bit resolutions

The current core evaluates the preceding sample on the new quantization grid. Signed stereo markers show that the reference retains each sample's own grid. At a change, take the fractional crossing weight in quantizer-code units, interpolate the two physical output levels on their respective grids, then apply the current gain compensation. Equal integer codes return the current physical level. Keep history when entering or leaving full resolution. The existing constant-resolution calculation can remain intact.

Nine independently drawn three-height curves crossed the grid between stored points at 44.1, 48, 88.2 and 96 kHz. The reference was fed signed kick and bass and unrelated stereo markers, at four block sizes including 256 and 2,048 frames. In the transition windows the current rule missed by 0.008059–0.032832 RMS. The recovered rule missed by 0.000000169–0.00000166 RMS, with 0.0000305 worst peak. A separate dithered stereo fixture distinguished the grids over 1,000 moving-code edges and verified 167 equal-code transitions. The paired-channel RMS residual fell from 0.03364–0.05973 to 0.00000174–0.00000465. Another 938,918 fixed-height samples checked the unchanged behavior.

The clones seat takes `adi_shaper_crush.cpp`, `adi_shaper_crush.hpp` and `test_adi_shaper_crush.cpp` for the current-stage timing repair on `clones/shaper-crush-resolution`. The per-band bias submission uses the same crossing calculation and will need both rules at integration.

## 22 November 2026 — Lower cost for steady white Noise

`clones/shaper-noise-port` at `d9d0e19e` — Process steady white Noise in bounded blocks. **Lands after 3 December.** This updates the existing Noise review. The single full-range, fully wet white stage with a fixed level now generates exactly the stereo sample pairs required by its resampler and filters them in blocks of up to 64 frames. The scalar path remains available for other settings. Separate channel histories preserve continuity when switching between mono, stereo, pink and white.

With the same stereo kick and bass, the registered white-stage median fell from 28.6 to 21.2 µs at 256 frames and from 224.6 to 166.8 µs at 2,048. The matching reference readings were 37.7 and 171.2 µs. Subtracting bypass in paired trials gave 17.7 and 140.2 µs for ADI, versus 18.1 and 123.5 µs for the reference. At the larger size ADI's p99 rose from 1,117 to 1,220 µs while deadline misses went from 40 to 38. Five large-block reference trial medians ranged from 131 to 174 µs. These readings establish a lower ADI median at both sizes and leave the larger-block incremental cost above the reference.

Six 192,000-frame registered stereo recordings matched the previous build byte for byte at both sizes. A separate core comparison covered 801 irregular callbacks across 44.1, 48 and 96 kHz, filter and routing changes, fresh seeds and resets. Bypass, white and pink all retained their output. The rack, Noise and registered native/edited-Pd suites passed under MSVC /W4 /WX at both precisions. Reported latency remained zero.

PR requested

## 22 November 2026 — Isolating the effect of saved Crush bias

We reopened all 43 affected programs in the reference, rendered each twice, cleared only the three bias values and rendered again. Thirty-seven repeats and archived recordings agreed within 0.000001 stereo RMS. The other six retain their individual repeat errors in the private results. A seven-stage regression of 0.049439 to 0.224996 RMS came from a program whose full reference output was exactly silent in both bias settings. Crush alone changed by 0.250202 RMS in both the original and the weighted device. Building up the source chain one stage at a time located the mute at Noise before Crush. Removing only Noise from the entire source chain restored a 0.129473 RMS difference between bias settings. Removing any one of the other enabled stages kept it silent.

The nearest remaining complete-stage comparison is Drive into Crush into Volume. The source-order device scores 0.00106906 centered and 0.00106915 with its stored bias. Removing bias from the source as well leaves 0.00106901. The reference change itself is 0.00002615 RMS and the device change is 0.00002661 RMS. The samplewise difference between those two changes is 0.00003468 RMS. This keeps the near miss open.

PR requested

## 22 November 2026 — Recallable per-band Crush quantizer bias

`clones/shaper-crush-bias` at `5f1d6c4a` — Add calibrated per-band quantizer bias to Crush. **Lands after 3 December.** IDs 865–867 store 0–100% bias independently in the three bands. Missing keys restore the previous centered response. The quantizer carries the preceding sample's bias into threshold crossings. With dither it subtracts the response to the same noise on silence, keeping a silent channel silent. Full-resolution playback stays transparent to the bias control.

The old core failed a signed stereo reading at 0.06657066 against 0.17970359. Twenty-one independent broadband comparisons at three rates gave at most 0.000000102 RMS and 0.00000516 peak error. The rack, Crush and registered native/edited-Pd suites passed under MSVC /W4 /WX at both precisions. Source and device recordings were independently identical across 256 and 2,048 frames. Centered recordings matched their archives exactly. The instantaneous bias is calibrated. The source's smoothing of an automated target remains unmeasured.

At 75% bias the registered median/p99 on stereo kick and bass was 20.7/45.4 µs at 256 frames and 201.8/293.2 µs at 2,048, versus 42.1/75.4 and 348.6/1,660.9 µs for the reference. Neither biased ADI run missed a deadline in 10,240 callbacks per size. Both reported zero latency and the largest per-channel RMS error was 0.000000163. At zero bias the registered medians were 25.3 and 165.4 µs, with one deadline miss at 256 frames.

Forty-three of 873 saved programs use a nonzero bias on an audible band. On the seven-stage main comparison seven improved, eighteen worsened and eighteen moved by at most 0.000001 RMS. One program went from 0.00003743 to 0.013589. Reopening its weighted curve preserved 0.00003744 and the audible crossing. Forcing minimum bit resolution in that program's solo Crush made the difference between zero and full bias 0.0122618 RMS in both engines. On the weighted original-stage-order pipeline fifteen errors fell, twelve rose and sixteen stayed within tolerance. All 25 overlapping prior stage renders reproduced their centered full-length files exactly. The measured pipeline retains 42 of 873 below 0.001 RMS. The weighted fixed-order experiment retains 40. These are private composite comparisons pending an integrated build.

PR requested

## 22 November 2026 — Master mix through two active stages

The existing ADI-Shaper master routing matches 105 fresh reference observations. For stationary master values of 0, 25, 50, 75 and 100%, multiplying each active band's blend by the master reproduced six million stereo frames exactly. The cases included Volume and Width in both orders, Volume and nonlinear Drive in both orders, unequal band mixes, three split slopes, both buffer sizes at 48 kHz and 256 frames at 96 kHz. The 960,000 stereo frames common to both 48 kHz block sizes were identical. Eighteen of the 873 saved programs have an intermediate master value. All eighteen use multiple stages.

With a +0.5 left-only marker at half master, Volume at quarter gain followed by mono Width returned +0.234375 left and +0.078125 right. A single dry/full-chain crossfade would return +0.28125 and +0.03125. On stereo material the final crossfade missed by 0.075509 RMS in that pairing and by 0.106619 through Volume into Drive. At zero master the active Drive retained its 16-frame delay. The 12 and 24 dB/octave crossovers retained their phase response. These readings cover fixed master settings. The current routing stands.

## 22 November 2026 — Registered white Noise and Room against the reference

On the same stereo kick and bass at 48 kHz, the registered white Noise stage on `5e9b063d` took 28.2 µs at 256 frames and 218.9 µs at 2,048. The reference took 42.3 and 174.8 µs. Subtracting each path's bypass cost within paired trials gave 24.4 and 193.9 µs for ADI and 25.9 and 127.5 µs for the reference. The larger block is a performance target.

Room on `df9f2778` took 100.6 and 813.8 µs versus 89.9 and 556.7 µs for the reference. Increases over bypass were 95.4 and 788.7 µs versus 73.1 and 510.6 µs. This compares the processing cost of ADI's own response and the reference's default hall at matching controls. The stereo outputs differ. Reported latency was zero for all sixteen stage, host, block and bypass conditions.

The figures are medians of five trial medians. Timers covered only the registered processing call. Every trial covered the same multiset of input block positions. White missed 5 of 7,500 and 6 of 9,375 enabled deadlines at the two sizes, versus 28 and 4. Room missed 19 and 21, versus 21 and 7. Sixteen of ADI's 21 large-block Room misses fell in one trial. These are wall durations under the measured workload. The two hosts lack matching processing-thread cycle readings.

## 22 November 2026 — Four-track Drive deadline audit

The original nine-trial run on `44de27f0` missed 713 of 73,728 callback deadlines, down from 1,209. The aggregate one-percent boundary therefore fell below 5.3333 ms on the repair. The earlier 4.5697 ms is the median of nine trial p99s. Three individual trials still exceeded the deadline at 6.9778, 5.9085 and 13.1699 ms.

A fresh run stored wall duration and processing-thread cycles together for each callback. The existing fix met the p99 deadline in all nine trials on four moving tracks. Pooled p99 was 0.6419 ms, with 223 misses. The longest callback took 99.035 ms while using fewer thread cycles than the pooled cycle p99. Thread elapsed time on the test system advanced in 15.625 ms steps, so the recorded data do not identify the interruption's cause.

A private variant kept a wider history at a cost of 1,040 bytes per band. It preserved all four stereo captures and the 16-frame delay. Its pooled p99 was 0.5110 ms with 143 misses, but its trial p99 improved in only three of nine pairs and median cycles in five. The existing review request stands at `44de27f0`.

PR requested

## 22 November 2026 — Driven Filter holdouts establish a remaining rate dependency

For the resonant low-pass used by 29 previous regressions, a 48 kHz model fitted only to small perturbations around positive and negative steady inputs was evaluated on 36 separate warm-start settings. All inputs and reference outputs repeated sample for sample at 256 and 2,048 frames. The comparator detected a single deliberately changed sample. Signed multitone, independent stereo broadband and kick and bass probed different parts of the response.

At high resonance and half input drive, scaling the two internal steps for sample rate reduced the 96 kHz broadband error from 0.01487 to 0.000679 RMS. The same correction raised 44.1 kHz broadband error from 0.00838 to 0.00958 RMS, with a peak miss of 0.0565. Kick and bass at the same two rates was much closer, about 0.000013 RMS. This mode remains open until its state-dependent response holds on broadband at both rates.

PR requested

## 22 November 2026 — Selectable Pan delay blend corrects the zero-blend regression

`clones/shaper-pan-edge` at `82caa7d5` — Reproduce Pan transitions at the selected delay blend. For Win, this head resolves the control check that held `5c554c59` and is ready for review. The three bands expose independent 0–100% controls at stable IDs 856–858. A project with no blend keys restores 50%, matching a fresh reference instance. Source programs that explicitly chose zero can reproduce the immediate far ear. The calibrated range on this branch is 2 ms.

The strict baseline failed eleven new core checks. Independent signed impulses covered 44.1, 48 and 96 kHz. The registered native and crossed editable Pd paths reproduced the five stationary blends, both dry paths and state recall. Both official suites passed under MSVC /W4 /WX in both Pd precisions. On a moving continuous contour the worst direct-core stereo-channel RMS error was 0.000000483. On the registered hard-switch test, every music and signed-marker capture met the 0.0001 RMS limit at both buffer sizes. The new music captures were identical across buffer sizes for each setting.

| Delay blend | Maximum registered music RMS, either channel | ADI / reference median at 256 frames | ADI / reference median at 2,048 frames |
|---|---:|---:|---:|
| 0% | 0.00001634 | 15.1 / 25.3 µs | 117.5 / 118.1 µs |
| 50% | 0.00005084 | 17.6 / 31.5 µs | 139.2 / 158.7 µs |
| 100% | 0.00001256 | 17.1 / 29.8 µs | 137.0 / 149.7 µs |

ADI missed two deadlines across 61,440 music callbacks. Both were at the 50% setting. The high-percentile times at 2,048 frames exceeded the reference at 50% and 100%. ADI reports zero latency and the source reserved 16 frames. The separate normalized-clock handoff explains the remaining gap between the direct and registered edge residuals.

PR requested

## 22 November 2026 — A stereo transfer law and selectable delay range for Pan

The most frequent missing Pan law occurs in 127 of 873 programs and ten of the earlier 81 regressions. With delay blend at zero, a +50% excursion sends half the right input to the left output while retaining half on the right. The left input stays at unity. Negative excursion exchanges the channels. At either endpoint the two inputs sum into the near speaker.

For excursion magnitude a and normalized delay blend h, let t = a × (1−max(0,2h−1)). The near output is its input plus t times the far input. The far output receives (1−t) times its input. Both complete mixed outputs enter the delay history. Only the current far speaker blends direct and delayed audio. Writing the pre-crossfeed speaker into its history gave 0.00650 RMS on a hard switch. Band and master wet controls scale the difference from dry by their product in this isolated stage.

Across 666 stationary cases at 44.1, 48 and 96 kHz, independent stereo input reconstructed the output within 0.000000163 peak. On 51 moving cases the maximum stereo RMS was 0.0000122 and peak error was 0.000185. During an overshoot the matrix uses the clamped position, while the delay uses the full signed excursion.

For a stationary contour, normalized range s and excursion a, let v = sa. The far-side delay in milliseconds follows 40v³ + max(0,2v−4v²), spanning 0–40 ms. The integer tap is the floor of the resulting sample count. All 36 grid cases and 24 independent off-grid cases matched the observed frame exactly. The selectable range and stereo transfer need the Pan delay writer from the existing repair to reach main. The active review branch covers the standard Pan law and its measured 2 ms range.

PR requested

## 22 November 2026 — Saved Sweep offsets on the historical bank

For all 116 affected programs, we loaded, saved and reopened the three offsets through a private 1,496-control registered device with weighted curves, Time and Trim. All 464 state restores and readback checks passed. On the original stage order, with Reverb held fixed where available, 85 stereo RMS errors fell, 26 rose and five stayed within 0.000001. Two programs finished below 0.001, up from one. All 33 overlapping centered Reverb paths reproduced their earlier samples exactly. Of the twelve programs with every stage represented, nine improved and three worsened.

The new crossing fell from 0.00111743 to 0.000915782 against the entire original. Pitch remained absent in this chain. When we bypassed only Pitch in the original, the remaining Sweep and Width signal matched at 0.0000000935 RMS. The existing Reverb pass also stayed below threshold. All figures use the values that survived device recall.

Keeping the 757 unaffected outputs gives **41 centered and 42 with offset among 873** on the measured original-order Reverb pipeline. A separate fixed-order registered experiment with the historical baseline moved from 39 to 40. These totals describe their respective private comparisons. The full bank still awaits a single integrated product build.

PR requested

## 22 November 2026 — Measured quantizer bias for Crush

Of the 873 paired programs, 43 have a nonzero bias on an audible Crush band, including four of the 81 previous regressions. The control spans 0–100% and defaults to zero. With normalized bias b and bit-depth contour u below full resolution, the quantization threshold shifts by 0.499b of a code interval. The old output scale, expressed per code before division by the number of levels, is 1−0.25(1−u)³. Bias subtracts a further 0.25b³(1−u)⁹. The highest resolution bypasses the quantizer.

We measured single threshold crossings and settled levels at seven bias settings and nine active resolutions, undoing the measured 10 Hz output filter in the recording. The recovered shift agreed within 0.000000162 of one code. We checked the resulting law on 21 separate stereo broadband captures including full-resolution bypass, at 44.1, 48 and 96 kHz. The largest RMS error was 0.000000102 and the largest sample error was 0.00000517.

Clones takes the ADI-Shaper Crush kernel, parameter routing, canvas and tests for the three band bias controls. **Lands after 3 December.**

PR requested

## 22 November 2026 — Measured Pan delay blend and existing review scope

Two fresh reference instances stored a 50% Pan delay blend in each inactive band, both before and after processing. The saved neutral program stored 0%. The original hard-edge comparison also stored 50% with a 2 ms maximum delay. At a leftward or rightward half excursion, an isolated far-side marker arrived 21 frames late at 48 kHz for that setting. Zero blend was immediate. With blend h between 0 and 0.5, the immediate far-side fraction followed (1−2h)³ and the complementary fraction took the delayed path. From 0.5 to 1, both channel gains moved linearly from the chosen Pan law toward unity while the far side retained the delay.

For the identical moving contour and kick and bass, the 50% reference gave 0.00002511/0.00005084 left/right stereo-channel RMS error on `5c554c59`. At explicit 0% the same code instead measured 0.00912084/0.00463888. Independent signed markers showed no outgoing-ear repeat at 0%, an attenuated repeat at 50% and a different amplitude at 100%. The comparison at both buffer sizes uses the same transport and compensates the reference's 16-frame reserve.

Clones is correcting this same review branch with three selectable delay blends in the ADI-Shaper DSP, canvas and tests. Stable IDs 856–858 are reserved for this. Review remains held pending fresh tests at all three settings. Time range and alternative balance laws have separate measurements.

## 22 November 2026 — Pan edge review condition for Win

Please hold `clones/shaper-pan-edge` at `5c554c59` for a control check. Its passing hard-endpoint fixture has the inter-channel delay blend set to 50% in the reference. The branch applies its delayed path to every moving held Pan contour. Separate signed and stereo music captures at 0%, 50% and 100% are in progress to verify the intended default and parameter routing before landing.

## 22 November 2026 — Independent stereo center offsets for Sweep

`clones/shaper-sweep-stereo` at `099aaa4f` — Add independent stereo center offsets to Sweep bands. **Lands after 3 December.** Stable IDs 853–855 expose the signed controls in the low, middle and high bands. A saved project with no offset keys restores centered stereo. The flanger uses its existing delay range. The four phaser depths reach lower and higher frequencies with spread, including the measured limit just below Nyquist.

All five new impulse cases failed on main. The largest normalized tap error fell from 0.699897 to 0.00001282. Another 30,720 stereo frames checked moving center, feedback, polarity, repeated offset changes and returning to center with a ringing tail. The rack, Sweep and registered native/edited Pd suites all passed under MSVC /W4 /WX at both signal precisions.

On stereo kick and bass with a moving eight-stage phaser, median/p99 callback duration was 27.8/44.4 µs centered and 39.7/60.2 µs at +50% spread for 256 frames. At 2,048 frames the readings were 217.8/379.6 and 249.9/376.8 µs. All 40,960 callbacks met their deadline. Processing reported zero added latency.

Switching only the measured offsets on the seven-stage 116-program comparison improved 69 errors, increased 39 and left eight within 0.000001 stereo RMS. One program crossed from 0.00111743 to 0.000956235. Of eleven earlier Trim regressions using the control, four improved, five worsened and two stayed within tolerance. These comparisons use the seven-stage main baseline.

PR requested

## 22 November 2026 — Reverb on the historical 873-program comparison

The historical bank had 39 programs below 0.001 stereo RMS, including one of the 163 programs using Reverb. We reproduced all 163 archived, four-bar stereo renders byte for byte with their corresponding registered historical builds. The comparison then replayed 162 in source order with the held weighted curves, Time, band Trim and the corrected room from `df9f2778`. Downstream clocks account for each preceding processor's latency. One program requires an unavailable band solo.

With room muted, one of 162 passed. Enabling the room raised that to three, with both gains among the 25 programs whose full chain was represented. Across those 162, 56 errors fell and 106 rose. The median change was +0.00260713 RMS. In the 16 earlier Trim regressions the errors fell for three and rose for thirteen. None of those sixteen crossed the threshold.

The two threshold crossings were 0.00344542 to 0.000454536 for Time → Reverb → Filter, and 0.00232475 to 0.000537071 for Sweep → Volume → Reverb → Width. The existing pass stayed below threshold at 0.000075677. All three repeated in fresh runs with identical stereo samples in both conditions. Carrying forward the original captures for the 710 Reverb-inactive programs and the one unported solo gives **39 muted and 41 enabled across the 873-entry comparison**. This total combines the measured stage chain with those archived captures. A single integrated device build still needs its own full-bank render.

PR requested

## 22 November 2026 — Devices handoff for exact ADI-Shaper musical divisions

The clock correction for DEV-31 is parked for Devices at normalized parameter delivery in `builtin_host.cpp` and the canonical shaper adapter in `native_devices.cpp`. A registered Volume ramp tested all 23 panel divisions at 48 kHz and 120 BPM. Main `87ea349c` and the held panel top `3e8e20f5` produced identical results in Pd32 and Pd64. Thirteen stored divisions decoded to a different float from the nearest float to the intended value. Fifteen differed from the exact musical ratio. At a 24-hour timeline position the largest phase error was 0.04119879 cycles for a half-beat period.

The factory stores the four-beat knob position as 0.5833333134651184 and delivers 3.9999992847442627 beats. At quarter-note position 172801.5 the measured phase was 0.38272482 against the four-beat oracle of 0.375. Full-precision grid automation reached 0.375. Automation immediately below and above the stored normalized code delivered the same physical float as that code. A raw Pd `p` message already supplies a physical value. The DSP therefore needs an explicit musical-division signal from the registered path to preserve intentionally authored continuous values.

A private prototype recognized only exact normalized grid codes at the shared delivery site and passed the chosen ratio into the shaper clock. All 23 divisions matched the long-position oracle within 5.59 × 10⁻⁹ cycles. All 88 tested neighboring knob and automation cases retained their previous samples. Six saved-state recalls preserved their stored normalized positions, the free-Hz control matched for 3,072 samples, and stopped and relocated clocks passed in both Pd precisions. On the pushed Pan repair `5c554c59`, the registered kick-and-bass RMS error fell from 2.51 × 10⁻⁵ left and 5.08 × 10⁻⁵ right to at most 1.14 × 10⁻⁸ and 3.75 × 10⁻⁹ at 256 and 2,048 frames. ADI reported zero latency and the reference reported 16 frames.

Devices is asked to provide or grant a shaper-specific way to deliver the exact normalized selection before conversion to a physical float. Clones can supply the division table, shaper clock handling and regression tests. The registered readout should use the same division mapping. The tested prototype is private and the shared implementation awaits the owner's review.

## 22 November 2026 — Sweep stereo offset across flanger and phaser bands

Of the 873 paired programs, 116 use a noncentered offset in an audible flanger or phaser band, including eleven of the 81 earlier regressions. The control is bipolar, from −100% to +100% with a centered default. A signed setting x shifts the left band's center by 60x|x| semitones and the right band's center by the opposite interval. At +50% this is +15 semitones on the left and −15 on the right.

We measured nine settings at three center frequencies and three sample rates, independently recovering the flanger's center from its first two delayed impulse taps and the four-stage phaser's center from its direct gain and following sample. The flanger stops at the ends of its ten-octave range. The phaser extends below the nominal minimum and above the nominal maximum. Its upper frequency stopped at 0.4999 times the sample rate at 44.1, 48 and 96 kHz. For 60 additional stereo impulses with both polarities, nonzero feedback and all five response types, the 9,216-sample channels matched separate centered captures at their predicted in-range positions sample for sample.

For DEV-31, the clones lane takes the three appended Sweep parameters and its existing kernel, canvas and tests in the ADI-Shaper files. This work lands after 3 December.

PR requested

## 22 November 2026 — Filter preset coverage and a missing resonant response

On the 873-program bank, 359 had Filter inactive and 437 kept the cutoff centered. Fifty-six requested stereo spread only on response types outside the five landed equivalents. Twenty-one used a mapped response with nonzero spread. With the same seven-stage projection and other controls in each comparison, enabling the measured offsets on `830d3a12` improved two, worsened eleven and left eight within 0.000001 RMS. All 21 chains also depended on another absent stage. None crossed 0.001. Six of the original 81 Trim regressions requested stereo spread on an unsupported Filter response.

One unrepresented resonant low-pass appears on 35 audible bands across 29 of those 81 programs. Fourteen small-signal holdouts at 44.1 and 96 kHz, using the damping measured at 48 kHz, matched a single two-substep state-variable response within 0.000674 percent relative RMS. At musical input levels the response also depends on amplitude, polarity and retained state. At input amplitude 0.5, cutoff 0.81 and resonance 0.73, broadband shape departed from the low-level response by 11.15%, 26.18% and 76.46% at minimum, default and maximum input drive. A quiet probe one sample after a louder impulse deviated by 16.89 percent, decaying to 1.35 percent at 16 frames. The initial drive experiment and the recovered cutoff/resonance law are private fixtures for the remaining state-dependent calibration.

PR requested

## 22 November 2026 — Seven registered stages on stereo kick and bass

For DEV-31, each side processed the same stereo input at 48 kHz, the same selected settings and 98,304 warm-up frames. Each timing used five trials of 2,048 callbacks and each audio comparison used 192,000 stereo frames. The registered 415-control device used the seven stage sources, external and canvas on main at `87ea349c`. The comparison host’s prepare-only lock difference falls outside the timed callback. Table entries are median callback durations in microseconds.

| Stage | ADI at 256 | Reference at 256 | ADI at 2,048 | Reference at 2,048 |
|---|---:|---:|---:|---:|
| Volume, three bands | 20.9 | 42.1 | 161.6 | 245.5 |
| Width | 4.7 | 20.3 | 46.5 | 117.7 |
| Pan | 3.3 | 31.5 | 26.2 | 158.7 |
| Filter | 19.8 | 55.4 | 143.5 | 349.2 |
| Sweep | 13.6 | 26.1 | 106.9 | 122.8 |
| Drive | 61.3 | 96.6 | 518.3 | 728.1 |
| Crush | 27.0 | 41.2 | 199.1 | 228.7 |

ADI used fewer median processing-thread cycles and less median elapsed time in all fourteen cells. At 2,048 frames the Sweep p99 was 149.2 µs against 148.9 µs. Across seven settings and 71,680 callbacks per side at each size, ADI/reference missed 3/2 deadlines at 256 and 1/0 at 2,048. This measures individual settings under the available scheduling load.

Both sides reported 16 frames for Drive. Width and Pan reported 0 frames in ADI and 16 in the reference. All other selected settings reported zero in both. After alignment, stereo residual RMS stayed below 0.000000082 for Volume, Width, Filter and Crush, 0.00000179 for Sweep and 0.0000354 for Drive. Main’s hard Pan transient differed by 0.009065/0.004984 RMS, so the stock Pan speed is a comparison with differing transition audio. The separate Pan correction has its own waveform test.

We repeated the same cells with the two pushed repairs. At 256 and 2,048 frames, the registered Pan on `5c554c59` measured median/p99 14.7/27.0 and 118.3/225.0 µs. The reference measured 31.5/35.1 and 158.7/252.6 µs. With the reference’s 16-frame delay removed, the registered Pan residual reached 0.00002511 L and 0.00005084 R. Both block sizes produced the same ADI samples. The optimized Drive on `44de27f0` measured 43.1/77.0 and 335.7/523.5 µs against the reference’s 96.6/159.4 and 728.1/1,056.0. Drive retained 16 frames of latency, matched the stock capture byte for byte and missed zero deadlines in these single-device runs.

PR requested

## 22 November 2026 — Registered Crush verification and Volume retrigger cost

For Win, `clones/shaper-crush-hold-grid` (`9925a705`) also passed the full rack, dedicated Crush and registered native/edited Pd suites in both signal precisions. This finding affects the existing stage on main. Win can schedule its review independently of the new stages.

On `clones/shaper-audio-retrigger` (`e08b932f`), five alternating trials of 2,048 callbacks per setting used the registered device, stereo kick and bass and 48 kHz. Looping retrigger took 8.8 µs against 6.5 µs in sync at 256 frames, and 68.9 against 49.2 µs at 2,048. One-shot took 8.9 against 6.4 µs and 67.6 against 49.0 µs respectively. Median incremental cost was 0.043–0.047 percent of one callback deadline. No callback missed its deadline. Enabling either measured mode changed the device’s reported compensation from zero to 672 frames.

PR requested

## 22 November 2026 — Stereo cutoff offset for all five calibrated Filter responses

`clones/shaper-filter-stereo` at `830d3a12` — Add independent stereo cutoff offset to the three Filter bands. **Lands after 3 December.** IDs 850–852 control the low, middle and high bands. Existing response IDs and sparse projects retain centered stereo. For each of the five responses, 60 source impulse comparisons in total at three sample rates confirmed that each channel follows the same response with its own shifted cutoff.

With the stereo spread deliberately forced to center, the largest absolute error on the new reference cases was 0.757046. The repair reduced this to 0.000000075. A separate 10,240-frame test moved offset, cutoff and resonance together and reproduced two independently centered ADI channel histories bit for bit. The full rack, Filter and registered native/edited Pd tests passed under MSVC /W4 /WX in both signal precisions, including signed state recall and writes across the unused ID range.

On kick and bass with a moving 12 dB/octave low-pass at 48 kHz, the registered device’s median/p99 at 256 frames was 29.7/46.3 µs centered and 34.6/56.3 µs at +50% offset. At 2,048 frames the readings were 239.9/436.9 and 276.3/485.4 µs. All 40,960 callbacks met their deadlines. Reported latency was zero.

PR requested

## 22 November 2026 — Stage contributions to the 81 preset regressions

For Win, the private bank has 873 stereo comparisons at 48 kHz. The count below 0.001 stereo RMS was 37 before band Trim and 39 after Trim. Eighty-one individual programs became worse. We opened two fresh instances of each original, then repeated the comparison after disabling each active stage separately. That gave 400 one-stage removals. The source and ADI were latency-aligned for each take. We counted a change only when it exceeded both 0.000001 RMS and twice the difference between the two fresh baseline measurements.

| Stage removed | Programs using it | Detectable change in the Trim result | Trim regression reversed |
|---|---:|---:|---:|
| Volume | 77 | 65 | 9 |
| Width | 27 | 20 | 0 |
| Pan | 30 | 23 | 1 |
| Filter | 58 | 49 | 17 |
| Sweep | 19 | 10 | 2 |
| Drive | 35 | 29 | 8 |
| Crush | 15 | 11 | 1 |
| Time | 41 | 34 | 6 |
| Pitch | 16 | 16 | 6 |
| Noise | 31 | 14 | 3 |
| Reverb | 16 | 3 | 0 |
| Reference dynamics stage | 35 | 26 | 11 |

Fifty-one of the 81 regressions reversed on at least one removal. Thirty-nine reversed on exactly one, eleven on two, and one on three. One original result fell inside the fresh-instance repeat tolerance. Each removal keeps every other stage in that program active.

The terminal Reverb port was measurable on 31 programs. Eight improved and 23 worsened. All five in the 81-regression subset worsened. The threshold count stayed at 39. Another 131 had effects after the room, and one terminal program required band solo. For Noise, 216 programs selected recordings outside the procedural port. We rendered the other 32 twice at both 256 and 2,048 frames, with the representable processors in their original order, switching only Noise on or off. Ten improved, 20 worsened and two stayed within 0.000001. None crossed 0.001. Of the four in the 81-regression subset, one improved, two worsened and one stayed within tolerance. Two chains had every original stage represented. The other 30 had documented omissions. Changing block size moved the measured increment by at most 0.0000000171 RMS.

The repaired Reverb also has an original-order comparison for 162 programs. Forty-nine improved, 112 worsened and one stayed within tolerance. One program crossed from 0.00337921 to 0.000433949 RMS, bringing the below-threshold count in this cohort from one to two. Two of the 16 Trim-regression programs improved. Thirteen chains contained every original stage. The greatest change in the incremental result between 256 and 2,048 frames was 0.0000000303. Noise and Reverb overlap on eleven programs.

Two further comparisons used the seven-stage core from main, the same eight-point curves on each side and the source’s rendered audio. Switching on the measured internal Width follower changed six eligible programs. One improved, two worsened and three were within 0.000001. One additional program used automatic release. Replaying the saved order changed the sequence of represented stages for 410 programs. Of these, 124 improved, 134 worsened, and 152 were within 0.000001. Two stayed below 0.001 in both cases. Of 49 programs from the Trim-regression group, eleven improved and 17 worsened. These seven-stage figures have their own baseline and cannot be added to the 39 from the earlier editor build.

Of the paired bank, 44 programs request audio retrigger on an audible Volume band. Twenty-six select an external key and 18 use the internal input. Ten internal programs used the measured detector settings. Their original-order A/B gave two improvements and eight increases at both buffer sizes, with zero threshold crossings. Two chains contained every original stage. The one program from the 81 regressions rose from 0.1568630 to 0.2046753 RMS. We compensated each following clock for upstream delay and aligned the dry output before the master blend. Changing block size moved this incremental result by at most 0.000000000448.

PR requested

## 22 November 2026 — Recall the order of the seven landed stages

`clones/shaper-stage-ranks` at `1719f64e` — Recall the processing order of the seven shaper stages. **Lands after 3 December.** Stable IDs 716–722 address ranks independently. Equal ranks retain the persistent stage identity order. A project saved before ranks existed reopens with the original processing order, including when recalled over a reordered instance.

All 4,096 distinct sampled permutations matched an independent order oracle. Running Volume before and after nonlinear Drive matched the separately evaluated processors in both directions and retained the 16-frame latency. The full rack and registered native/edited Pd tests passed under MSVC /W4 /WX in both signal precisions.

On kick and bass with hard-clipped Drive and constant Volume, the previous and new default sequences took 61.1 and 61.6 µs at 256 frames, and 495.9 and 495.9 µs at 2,048. Moving Drive ahead of Volume took 61.7 and 507.2 µs respectively. These are medians of three ordered sets of nine trials. Thread CPU for the default sequence measured 1.14% and 1.18% of one core on both builds.

PR requested

## 22 November 2026 — Internal envelope follower for each Width band

`clones/shaper-width-follower` at `a1b02971` — Port the per-band Width envelope follower. **Lands after 3 December.** IDs 727–753 control Add/Multiply, amount, depth, offset, threshold, attack, hold and release independently in three bands. Older states restore the follower switched off, including when recalled over a running instance.

The Add side-gain oracle passed within 0.000048. Attack follows 100u³ ms and hold follows 500u² ms. On 192,000 stereo kick-and-bass frames at 48 kHz, peak absolute error was 0.00000056625 at both 256 and 2,048 frames. Stereo RMS error was 0.00000009135 and 0.00000009122 respectively, after aligning each independent reference capture to its recorded start frame. MSVC /W4 /WX passed the full rack DSP and registered native/edited Pd comparisons at both signal precisions.

In nine trials of 1,024 callbacks, median-trial native DSP p50/p99 was 9.8/14.4 µs at 256 frames and 76.4/191.1 µs at 2,048. Neither run missed a deadline. Both reported zero latency.

PR requested

## 22 November 2026 — Keep the moving Crush hold clock continuous across blocks

`clones/shaper-crush-hold-grid` at `9925a705` — Keep the moving Crush hold clock continuous across audio blocks. This fixes the existing stage on main. The preset comparison found a dependence on callback size upstream of the room. An independent broadband test isolated the moving hold-rate control on Crush. The full-rate bypass resets the capture phase. At a contour boundary, the former evaluator could take that reset in one buffer partition and miss it in another.

Main failed 18 new partition checks. The repaired MSVC /W4 /WX rack suite passed for whole, 256, 2,048 and irregular blocks, including continuous and stepped curves, two start phases, full-rate bypass, a held nonzero rate and real transport jumps. On kick and bass, the full-chain 256-versus-2,048 RMS difference fell from 0.000120719 to 0.00000000326. Isolated Crush went from 0.000124318 to sample-identical. The stepped and linear broadband cases went from 0.0638735 and 0.000877294 to sample-identical. All twenty whole-block reference captures retained their previous samples. Latency remained zero.

PR requested

## 22 November 2026 — ADI’s room response and controls for Reverb

`clones/shaper-reverb-port` at `df9f2778` — Port the measured Reverb controls and ADI’s own room response. **Lands after 3 December.** Stable IDs 626–715 add the three bands. Earlier project states restore the same stereo sound with the room disabled. The dedicated control-law suite covers send, return, width, gating, decay, size and predelay. The rack, room and registered native/edited Pd suites passed at both signal precisions under MSVC /W4 /WX.

The private preset replay exposed a missed clear when a return contour reached zero between samples. The old implementation differed across callback sizes by 0.00740 RMS on an authored 120 BPM fixture. The repair reduced that to 0.000000000101, cleared the intervening zero at 119.73 BPM and retained the tail when the contour remained positive. Eight independent contours stayed within 0.000000000118 RMS. The full failing preset fell from 0.000113 to 0.000000000748 RMS between 256 and 2,048 frames.

On stereo kick and bass at 48 kHz, five alternating trials of 2,048 callbacks measured median/p99 85.1/189.6 µs with the room enabled at 256 frames and 765.9/1,493.9 µs at 2,048. With the room off, those timings were 5.2/7.7 and 39.1/54.2 µs. All 40,960 enabled and disabled callbacks met their deadlines. The registered device reported zero latency.

PR requested

## 22 November 2026 — Audio retrigger for the Volume contour

`clones/shaper-audio-retrigger` at `e08b932f` — Retrigger the Volume contour from the internal stereo detector. **Lands after 3 December.** IDs 754–764 select sync, looping retrigger or one-shot for each band, the held endpoint and detector controls. The original 415 controls keep their IDs. Loading an older state over an active detector restores sync and releases the added delay.

A decaying 920 Hz burst on one channel and a pilot on the other exposed the new contour starts at frames 14,671, 54,171 and 82,071 at 48 kHz. The smaller burst at 31,300 left the contour running. Measured and reported delays agreed in all twelve rate/algorithm/smoothing cases at 44.1, 48 and 96 kHz. At 48 kHz the short and extended modes added 672 and 1,152 frames, or 961 and 1,441 with smoothing. The two short profiles share the presently measured internal detector response.

The main DSP failed 39 new assertions. The strict Release tests passed on the new head, including exact native/edited stereo output across the off-grid onset and sparse project recall in both Pd precisions.

PR requested

## 22 November 2026 — Procedural Noise for the built-in ADI-Shaper

`clones/shaper-noise-port` at `5e9b063d` — Port calibrated procedural Noise to ADI-Shaper. **Lands after 3 December.** IDs 536–625 hold the three bands and their controls. The original 415 IDs keep their meanings. A sparse project recalls with Noise off, including when loaded over a running instance with Noise enabled. The stage uses ADI’s procedural pink source.

The white generator runs at 44.1 kHz with interpolation. At 48 kHz, 2,097,152 stereo frames gave RMS 0.022350 and 0.022321, L/R correlation 0.000676, and 95 percent of energy below 21,234.375 Hz on both channels. The 256- and 2,048-frame captures matched byte for byte. The public suite also checks exact mono agreement, filtering, pitch, gain, wet-only behavior and independent seed state. The registered native and edited Pd paths, DSP and state suites passed under MSVC /W4 /WX at both Pd precisions.

On stereo kick and bass, five alternating wet/bypassed pairs of 2,048 callbacks used the 505-control registered device. White's median callback was 22.1 µs at 256 frames and 174.7 µs at 2,048. Pink took 36.0 and 244.3 µs. The incremental cost over bypass was 0.345/0.317 percent of one audio deadline for White and 0.576/0.481 percent for pink. Latency remained zero. The respective p99s were 36.9/1,409.0 µs and 60.6/790.4 µs during concurrent activity.

PR requested

## 22 November 2026 — Measured stereo cutoff offset for Filter

Filter accounted for 12 of the 39 preset regressions that reversed on exactly one stage removal. A separate impulse experiment found a control missing from the seven-stage device. With identical input on both channels and a single low-pass band, the reference distributes its cutoff differently to L and R. For a bipolar setting x from −1 to +1, the left offset is 60x|x| semitones and the right offset has the opposite sign. Each cutoff stays within the original ten-octave frequency range, about 20.6 Hz to 21.1 kHz. The default is centered.

At 44.1, 48 and 96 kHz we measured nine offsets at three cutoff settings. Inverting the first impulse sample of the measured one-pole response recovered all 162 channel offsets within 0.000005 semitone. Main applies one cutoff to both channels. The isolated test establishes the offset law. How much that control accounts for in a complete preset still requires a paired render.

PR requested

## 22 November 2026 — Test the ADI-Shaper beat gate in a compensated Session

`clones/shaper-session-gate` at `046e5248` — Test the ADI-Shaper beat gate in a compensated Session. An upstream 379-frame delay, the ordinary registered device, the Session transport and captured stereo output are all in the test. At 48 kHz the expected open/close frames were 49,531 and 115,067 with compensation, and 49,152 and 114,688 without it. A one-sample allowance at the exact transition accounts for seconds-to-beats rounding.

At each Pd precision the MSVC /W4 /WX suite checked 898,252 output samples across the four 256- and 2,048-frame captures. A control build deliberately feeding the global beat opened 379 frames early and failed the test.

PR requested

## 22 November 2026 — Agree with the devices lane’s exact-position transport

For Devices and Win, both ADI-Shaper hunks in `devices/pd-exact-position` at `0e8cd8a3` are approved as written. The receiver validates the complete seven- or nine-value message before updating state. The nine-value message reconstructs the whole and fractional quarter note in double precision and uses the play state and tempo from that tick. Its history check allows zero whole frames of drift. The old seven-value message retains its 64-frame tolerance and its meter-position limitations.

The registered Pd suite passed on the landing base plus the linear panel in both signal precisions. Eight additional stereo blocks passed on both the native and edited paths at each precision, including quarter notes 0 and 1 sharing the coarse 2/2 stamp, a meter change and a long-running transport position.

PR requested

## 22 November 2026 — Linear rebuild of the four panel branches

For Win, the four pushed heads in order are `clones/shaper-linear-reconcile` (`e8ff2ead`), `clones/shaper-linear-rack` (`aea61bfc`), `clones/shaper-linear-grid` (`ffcc76a3`) and `clones/shaper-linear-trim` (`3e8e20f5`). There are six commits and zero merges after the landing base `e5d3c6dd`. Twenty-five of the 26 changed files match the old Trim top byte for byte. The transport file keeps the landed exact-position helper and the legacy 64-frame tolerance.

The ten registered device suites passed at each Pd precision, including all six ADI-Shaper DSP suites under MSVC /W4 /WX. The ordinary rack/editor replay passed all 46 checks and produced two 1920 × 1080 headless captures. Rebuild complete. Panel landing awaits Win’s #708 ruling and the window owner’s follow-up review.

## 22 November 2026 — External-key Width and note retrigger

DEV-31-KEY-MIDI-CORE is parked until the independent follower and audio-retrigger ports reach main. On their private integration the five key plateaus matched within 0.00005 side gain and all four note-ons restarted at their exact frames in looping and one-shot mode at 256 and 2,048 frames. Devices owns delivery of the two key planes and notes from the host.

## 22 November 2026 — Reduce the four-track moving Drive processing cost

`clones/shaper-drive-deadline` at `44de27f0`. Four registered instances ran the same moving Drive on kick and bass at 48 kHz and 256 frames, nine trials of 8,192 callbacks per build. The median of the nine wall-clock p99s fell from 13.6193 to 4.5697 ms against a 5.3333 ms deadline. Misses fell from 1,209 to 713 of 73,728 callbacks. Three candidate trials individually exceeded the deadline at p99. Median thread cycles fell from 929,891 to 710,893. The four 192,000-frame stereo captures matched byte for byte, all four reported 16 frames of latency, and the measured-response regression suite passed.

PR requested

## 22 November 2026 — Pan switching transient on held curves

`clones/shaper-pan-edge` at `5c554c59` — Reproduce both ears at held Pan transitions. The level at an endpoint already matched. A signed marker showed the outgoing ear also carried 96 frames of earlier input at 48 kHz. The device now keeps that history per band. Its parameter IDs and zero reported latency are preserved.

On 192,000 frames of stereo kick and bass, after removing the reference’s measured 16-frame latency, whole-capture RMS fell from 0.00906528 to 0.000000011302 on L and from 0.00498397 to 0.0000000037333 on R. Peak residual was below 0.00000197. The DC oracle measured 0.0000000138 RMS and unrelated stereo noise stayed below 0.000000649. All three ADI renders were bit-identical at 256 and 2,048 frames. The Release MSVC /W4 /WX suite passed, including dry, neutral and sparse-state cases. Main failed both buffer-size edge oracles and the signed marker.

PR requested

## 22 November 2026 — Two-head delay reader for devices review

For devices and Win. `clones/two-head-reader` at `9b7cd7ca` — Share the two-head delay reader across pitch policies. **Lands after 3 December.** The reader accepts the caller’s interpolator and retains the phase precision, crossfade, wrap and unison behavior of each processor. The public unit test exercises read ages across the history boundary and the two weight and advance laws at 44.1, 48 and 96 kHz. The two shipping devices have no changes on this branch.

Release MSVC /W4 /WX passed the committed test. For the cross-test we compiled both original ADI processors and both reader-backed processors under separate class names. The Shifter source at `7c5d8158` matches current main. Cyclic Pitch came from `380335a2`. Stereo impulses, independent deterministic noise and kick/bass covered the three rates, irregular blocks including 1, 256 and 2,048 frames, parameter changes, a return to unison and reset. All 3,127,711 comparisons of head ages and output samples were bit-identical.

DEV-31-PITCH is parked for devices’ review of this header. Its Shifter adoption and our cyclic Pitch adoption follow that review.

PR requested

## 22 November 2026 — Mix View contract for the layer scope 1.5.03

For the Mix View seat, item 9, second reference in ADR-0247 d5. At 48 kHz, 120 BPM, 4/4, 256-frame blocks, a 1,200-pixel plot at its initial 1.00 scale covered one beat (24,000 frames). An 8 Hz tone filled four cycles. Pulses 2,400 frames apart started at x = 238 and 358 at y = 90. Use 20 frames per pixel in that view. At amplitudes 0.25 and 0.50, the trace spanned y = 78–132 and 52–158. Left-only and right-only 0.50 sines both drew at 0.25 in M. Equal channels drew at 0.50 and opposite channels cancelled. Project each source as (L + R)/2 before adding M traces.

Layers places both projected signals on one baseline. Stack uses one baseline per source. Sum adds signed samples at the same time. One 0.25 source read −12.04 dBFS and two in phase read −6.02. After reversing B, the trace occupied two center rows immediately. The peak read −84.52 dB after 2.13 s and reached −∞ by 4.27 s on one run. Those readings establish independent visual and numerical settling. They do not fit a decay constant. The audio path passed all 204,800 frames per channel exactly at zero reported latency.

For ADI, freeze the same audible interval on A and B, sum both stereo channels before calculating pixel extrema, and keep the same gain for the overlay and sum. Advance only the B preview by the displayed sample count, using the common interval, and recalculate its sum. Keep polarity separate. A broadband duplicate 137 frames late, normal and inverted, identified the same 2.854 ms advance at 48 kHz. Three weak and three ambiguous kick/bass regions, silence and a steady repeating tone must leave manual preview available without suggesting a lag.

The manual describes period = 1/f in Hz mode, additive per-source and global visual offsets in samples or ms, coarse/fine wheel steps of 10/1 samples, and a switch from inter-source mono correlation in Layers/Stack to L/R in Sum or a solo source. Both offset fields were observed at zero. Offset limits, sign and steps, the operand switch, per-bar window, averaging, thresholds, refresh and the analyzer passband await interactive readings. Private replay index `b4df4a7e1ee9` identifies the generators, captures and audio pass-through runs. Item 10–12 procedures follow in the idle-time handoff.

PR requested

## 22 November 2026 — Mix View contract for the alignment scope 2.0.5

For the Mix View seat, item 9, first reference in ADR-0247 d5. At 48 kHz, 120 BPM, 4/4 and 256 frames, the fresh 1/1 plot covered one 2 s bar. An 8 Hz sine filled sixteen cycles across approximately 755 pixels, or 127 samples per pixel. A 0.25 sine occupied 69 vertical pixels between its extreme rows and a 0.50 sine occupied 141. Both used a 1,000 × 375 surface with a baseline near y = 206. L mode followed left-only input and suppressed right-only input. The stored setup used independent 1/16 display and alignment grids, zero correction, freeze on stop on and automatic vertical scaling off. Default processing passed 204,800 stereo frames exactly with zero reported latency.

For alignment, generate a 1,200-frame rectangular pulse 4,800 frames after each quarter note. Change only the source correction, read leading edges at y = 173, then reset. The first four x coordinates were 254/442/631/820 at zero, 235/423/613/801 at +50 ms, 273/462/650/839 at −50 ms and 253/443/631/820 after reset. Positive correction moves the drawing earlier. The documented ±3,000 ms range and first-transient alignment to the preceding grid line remain to verify at the extremes and after transport restart.

The stored spectrum used a 1 kHz pivot, 90/45/20 ms analysis windows and five-pixel bands. The manual places mid vertically and side horizontally on the goniometer, bounds correlation at −1 to +1 and defines collision before tilt. Both sources must reach −60 dBFS in the candidate bin and be within 24 dB of their own peak. With two sources the lower qualifying level sets the shading. The documented momentary and short-term histories are 400 ms and 3 s. A phase-reversed equal pair is an acceptance case for separating spectral overlap from waveform cancellation.

Private replay index `b4df4a7e1ee9` identifies the captures and generator. Two loaded instances exposed one named source in the measured view. The edition and multi-instance Sum, correlation integration, goniometer scale, passband, collision boundary, loudness histories and refresh remain open. The director question above covers the edition.

PR requested

## 22 November 2026 — Queue dependencies and owner reviews

| Queue item | Owner and next action |
|---|---|
| CURVE-EDIT-P99 | Win’s #708 landed the two audio fixes and explicitly retained the panel hold. Repeat the edited performance run on the linear panel top when Win lifts that hold. |
| PSY-OFFSET | Win at director idle time. The isolated wheel and drag left the displayed offset at zero. Measure both sources and the global field separately, with markers 2,400 frames apart at 48 kHz. Enter −2,400, +480 and +2,400 frames, switch to milliseconds (−50, +10, +50), record signed pixel shift relative to the fixed marker and test combined offsets, wheel/fine steps and both clamped extrema. |
| OSC-ANALYSERS | Win at director idle time. The isolated surfaces do not expose the required analyzer selections. Record the response to correlation steps, mid/side anchors at two known amplitudes, a level-matched tone sweep, and collision just below/at/above each of the −60 dBFS and own-peak-minus-24 dB boundaries. Use timed noise on/off for each loudness history and a timestamped capture across changing input for refresh. Record host rate, FFT setup, displayed values and capture cadence separately. |
| PSY-PHASE | Win at director idle time. The isolated Show menu opened but did not activate the analyzer. Use fixed one-beat Layers, Sum and one enabled source. Compare 8 Hz sines at 0°, 90° and 180°, record every bar over one beat, then swap one source’s L/R phase while holding the other mono. Use unequal source gains to keep the anti-phase sum measurable. Record the displayed pair, segment windows, averaging, thresholds and refresh. |
| DF-E43 | The director chooses the devices and ADI names after the scope work. Recommendation under FOR THE DIRECTOR. |
| Window owner review | Window reviewed the two loaned sites at `4b0c6036` and requested token reacquisition after deletion/Undo, control refresh without a scope provider, delivery of refused edits to the status bar and a committed panel-view test. The linear rebuild preserves the original stack. The fixes need their own follow-up on that top and window’s reread before landing. Window has pushed the independent browser-fixture fix. |
| Devices, exact ADI-Shaper divisions | Deliver the normalized musical selection before narrowing to physical float. The registered four-beat ramp drifts by 0.007725 cycles at a 24-hour locate. The private codec and neighboring automation tests are ready for owner review. |
| Devices and Mix View | The scope feed and same-position native Time transport remain with devices. Devices will decide sharing of the repeat history once its capture-direction reading is known. Mix View owns removal of the three old log entries. |

## 22 November 2026 — Public log and parameter-scan correction

Source identities, documentation links and private identifiers have been removed from the public log. Earlier measurements retain their fixture settings and results. Every built-in skips the parameter scan when its revision counter is unchanged. ADI-Shaper’s opt-in for sparse recall is separate. The two questions above affect further measurements and DF-E43.

`clones/log` — Restore the clones review records and scrub the public log.

PR requested

## 22 November 2026 — Restore three review requests to the clones log

The three 20 November entries below identify their original branch heads and test results. For Mix View and Win, the corresponding entries in the Mix View log can now be removed by its owner. #708 has landed both audio fixes. The editor stack awaits Win’s separate ruling.

PR requested

## 22 November 2026 — Volume Trim and Width Gain per band

**PR requested** · `clones/shaper-trim` · **Add per-band Volume Trim and Width Gain to ADI-Shaper** · `69422cef`

For Win. This tip contains the rack opener, clock/grid and band faders in order for one landing. Window reviews the opener loan. The Release application linked under MSVC with warnings as errors. All ten device suites passed in each Pd precision. All six ADI-Shaper suites in the final application configuration passed.

Volume Trim spans −24 to 24 dB. Width Gain spans −12 to 12 dB. Both start at 0.00 dB, act on the selected band before its dry/wet mix and double amplitude for each 6.00 dB shown. The six host parameters follow the existing curve storage. Earlier sessions and presets recall unity at all six controls. The fader previews each change during a drag and records one project edit on release.

On the final tip the registered rack replay passed 64 Trim checks and 46 clock and window checks. Dragging Volume Trim raised measured stereo gain from 0.250004 to 1.00002 while the mouse was down. One undo restored the original audio and fader pixels in the open window. Redo restored both boosted values. The second device remained independent and subsequently recalled the same persisted parameters from a private preset. The paired full-HD images show 25% Volume, 12.0 dB Trim, a quarter-note cycle and full wet mix.

The source measurement fed different steady levels to the left and right channels. We held its Volume contour at 25% and its Width contour at mono, and varied gain, band mix and master mix independently. Applying gain to the wet band predicted every sample in the final 1,024 stereo frames of all 36 renders. The Width measurement includes the gain of the center signal.

On stereo kick and bass at 48 kHz and 120 BPM, +12.0 dB with a constant quarter-height Volume curve took 5.4 µs median and 7.9 µs at the 99th percentile for 256 frames. The reference took 15.4 and 29.4 µs. At 2,048 frames ADI took 21.3 and 57.1 µs and the reference took 58.4 and 125.2 µs. Each boosted condition timed 20,480 callbacks under shared load. ADI missed zero deadlines. Both processors produced the same samples at the two block sizes. The peak difference between them was 0.0000117, accounted for by the saved curve height's quantization. At unity trim the prior and current device rendered identical stereo samples.

The private preset check found 315 affected settings in 267 programs. All 534 stored states recalled across the two curve encodings on the registered device. Using the editable curves, we rendered four bars at 256 frames and compared the second bar with the source after compensating for reported latency. Enabling the measured settings lowered the stereo RMS error in 186 programs and raised it in 81. Median error fell from 0.105869 to 0.094737. No program changed latency. Other missing stage behavior still affects these comparisons.

The two programs containing only Volume or Width processing both improved. All 81 larger errors occurred in chains with additional active processing. We retested the two worst cases' gain settings in isolation. Width matched the source sample for sample. Volume differed by at most 0.00000018. Attributing an individual chain's error would require measuring the other stages separately.

For the window lane. The broader shell test crashes in `test_ui_shell.cpp`, in `browserDevices`. In a fresh profile the stock convolution and algorithmic reverb is available and inserts successfully on track 4, contrary to that test's expectation. Its plug-in subcase then clears the selected track. the stock filter and Utility are both present, and insertion requires a selected track. The test continues after the failed insertion and dereferences the empty strip. The browser and this test match the main branch at the loan. Please update the stock convolution and algorithmic reverb assertion, isolate its insertion in the history, restore track 4 before testing the next device and guard the strip access. The failure reproduces without opening ADI-Shaper.

With those fixture corrections applied privately, the full shell suite passed 2,925 checks with zero failures on both the rack opener and the final stack, each in a fresh profile. The published shell suite still contains the original fixture.

## 22 November 2026 — Step through the sync divisions and read the curve grid

**PR requested** · `clones/shaper-clock-grid` · **Step ADI-Shaper's synced curve by one division** · `80e2ad08`

The arrows beside the current division use the existing 23-entry menu order. Clicking the value opens the chooser. Each accepted step is one device edit. At either end an outward click adds no history. If automation has placed the period between entries, the arrow chooses the nearest listed period in its direction. The endpoint and between-entry rules are ADI choices. Free rate retains its continuous Hz drag.

The quarter-note and one-bar source captures both show sixteen vertical amplitude intervals with stronger quarter and center guides. The one-bar ruler ends at 4/4. The headless widget test checks all 23 steps, menu selection, terminal clicks, a between-entry value, continuous rate and all fifteen interior guides. The paired full-HD captures show the same four-point quarter-note Volume curve on four bars of stereo kick and bass at 48 kHz and 120 BPM.

This review stacks on the rack opener. The Release widget and its current DSP passed the MSVC headless suite with warnings as errors. The ADI capture feeds the widget directly from that DSP. The registered rack's separate capture documents its current waveform-provider status.

## 22 November 2026 — Open ADI-Shaper from the device rack

**PR requested** · `clones/shaper-rack-editor` · **Open ADI-Shaper from its rack wrench** · `4b0c6036`

For Win and the window lane. The device rack shows the wrench on an unfolded ADI-Shaper. The ordinary editor window opens the panel for the selected internal device UID and closes when its owning instance disappears. Renaming the device preserves that association.

The headless project used two ADI-Shaper instances and an unrelated built-in, including deliberate display-name collisions. Pointer input hit the production rack wrench. The normal floating host opened the correct editor, honored pin and multiple-window settings, and closed the removed owner's view. A Master Mix drag reached only its owner before release and one undo restored the previous gain.

The two full-HD pictures show the same eight-point, quarter-note Volume curve at 49% and full wet mix. Four bars of registered kick and bass passed through ADI. Its stock host wrapper still lacks the per-instance scope feed, so the ADI rack picture shows an empty input/output trace and idle output meters. The wrapper request stays with devices. Win owns the installed-original pen test during director idle time.

The Release build compiled both loaned translation units with MSVC warnings as errors. The registered rack and floating-editor test passed all 46 checks, including closing the title bar, undoing removal and reopening, and project teardown. Window reviews the two loaned sites in the panel stack.

## 20 November 2026 — Panel reconciliation

`clones/shaper-panel-audio-reconcile-20261120` at `e37c8d4e`. The panel capture takes sanitized input and each Time history writer discards invalid samples on its own channel. Two writer regressions failed before the repair. All six Release DSP suites, the Pd, wave, panel and panel-session tests passed. The production shell compiled with its panel sources. The two audio fixes precede this head.

PR requested

## 20 November 2026 — DF-E60

`clones/df-e60-shaper-drive-continuity-20261120` at `f8ef8af0`. The first 3.125% of Drive blends the transfer in on the oversampled path, ending at the first compensation knot. The baseline failed 36 assertions near zero on hot stereo input. The five Release DSP suites passed across three rates, all styles, two trims, rising and falling curves and established nonzero settings.

PR requested

## 20 November 2026 — DF-E59

`clones/df-e59-shaper-nonfinite-20261120` at `ff0c9a15`. The seven-stage rack treats a non-finite input sample as zero and recovers the affected stage at a block boundary. The unaffected channel and the delayed dry path stay aligned. The baseline failed 63 fault and recovery checks and two further delayed-dry checks. All five DSP suites passed on the repaired head.

PR requested

## 19 November 2026 — Draw and repeat Line, Arc and S curves

**PR requested** · `clones/shaper-pens` · **Add three drawing pens to the ADI-Shaper editor** · `20b5f3b4`

Win, this tip contains the full ADI-Shaper stack for one landing review. The three pens use the existing device parameters. Hovering previews a snapped shape. A drag auditions it, release records one undo step, and clicking again repeats in the last direction. The editor preserves both heights at a repeat boundary, stops at the cycle edge and cancels a stroke that exceeds the point pool. The installed editor's three tooltips specify Ctrl for temporary pointer mode. ADI also accepts Shift. The default stamp size, arc and S control positions and edge rules are ADI choices pending the private source strokes requested above.

In a registered project, the S pen expanded a four-point Volume contour to eight. Before release, 24,000 frames of kick and bass followed the new drawing to within 0.0000005 RMS. The previous drawing differed by 0.04623 RMS. One undo restored the original four points and redo restored all eight. With the same eight points loaded into the installed editor, the full-wet quarter-note drawing read 49% in both 1920 × 1080 captures. The capture confirms the stored waveform. The source toolbar did not select a pen during isolated input.

Across 192,000 stereo frames at 48 kHz and 120 BPM, the source and ADI differed by 0.000000446 stereo RMS. Each processor returned identical audio at 256 and 2,048 frames. At 256, median and 99th-percentile callbacks in microseconds were 4.0 and 9.3 for ADI, then 17.8 and 50.0 for the reference. At 2,048 they were 29.5 and 83.8 for ADI, then 64.3 and 223.2 for the reference. Each condition timed 10,240 callbacks with the editor closed and missed zero deadlines.

The Release build passed the ten device suites in both Pd precisions, both headless production editor tests and the shell's three editor translation units with warnings as errors. The shared window and transport hooks remain with their owners.

## 19 November 2026 — Lower CPU cost for repeating a weighted curve

**PR requested** · `clones/shaper-curve-cpu` · **Reuse calculated curve samples during ADI-Shaper playback** · `f1c0ce15`

Review after point selection. ADI-Shaper reserves two curve pages on preparation and calculates individual grid samples as playback requests them. A changed curve invalidates its own page. Repeated edits use the uncached evaluator. Other active curves continue to play when both pages are occupied. The prepared audio cache takes 67,640 bytes and makes no allocations in the callback. The panel uses the same evaluator without that memory reservation.

With the same full-wet, quarter-note Volume curve at 48 kHz, the final registered ADI and reference median callbacks were 7.0 and 18.3 microseconds at 256 frames, then 54.2 and 98.5 at 2,048. The corresponding 99th percentiles were 10.1 and 52.1, then 273.4 and 672.2. Each warm condition used 30,720 callbacks with the order rotated. ADI missed zero deadlines in those warm runs. Its previous medians under the same conditions were 9.0 and 96.1. Editing one point every 256-frame callback lowered the median from 21.3 to 19.9 but raised the 99th percentile from 117.5 to 127.1. Deadline misses rose from 9 to 15 in 30,720 callbacks. At 2,048 frames continuous edits improved both median and 99th percentile.

All 40 warm, cold, edited and dry channel captures matched the previous implementation byte for byte. The matched 1920 × 1080 registered editor captures before and after optimization also had identical pixels. The director has the separate original editor capture in the same state. Release device tests and both Pd precisions passed with warnings as errors.

The final registered device recalled 873 editable programs and rendered four bars each. The audited second bar matched the preceding ADI build bit for bit for 840 programs. The greatest peak change on that bar was 0.000005663, the largest stereo RMS change 0.000000625. All reported latencies matched. Relative to the original device, 37 of the 873 conversions measured below 0.001 RMS on this input. The private catalog identifies the active source stages and omitted controls for each program. Both production editor tests and all three shell editor translation units passed the strict gate.

## 19 November 2026 — Move and scale a group of curve points

**PR requested** · `clones/shaper-point-selection` · **Select and transform groups of ADI-Shaper points** · `eeb76d1f`

Review after weighted curves. Dragging an empty region selects its points and creates no project edit. Dragging inside that box moves the selected points together and auditions the changes. Moving both selected points up to the top of the graph preserves their relative heights and stops the entire group. Releasing commits one undo step. A hard, medium or soft point keeps its weight. Scaling uses the side and corner handles. The controls support axis locks. Tests cover temporary snapping, scaling about the center and skew. Point identities survive crossings. An undo, preset recall or rejected edit can clear an obsolete selection. A motionless click on a legacy contour leaves its compact storage intact.

On the reference’s full-wet quarter-note Volume contour, selecting two of four points left all four untouched. Moving the pair changed their horizontal coordinates from 5,099 and 6,878 to 3,576 and 5,355, in units of 1/8,192 of a cycle. Their linear gains changed together from 0.05 and 0.65 to 0.40 and 1.00. Both other points and all four weights stayed fixed. The source’s eight handles and the group move are visible in the paired 1920 × 1080 captures. The resulting Volume readout is 60% in both editors. The stage readout truncates fractional percentages. At an integer boundary it favors an exact whole-percentage entry when that entry and a slightly lower gain encode to the same 16-bit height. The source documents its scaling modifiers. The actual source-side resize attempt produced no measurable coordinate change.

With the selection shown or cleared, 384,000 stereo frames per buffer size were byte-identical. Three alternating passes gave average callback costs of 0.301% and 0.320% of the 256-frame budget, then 0.243% and 0.236% at 2,048. The real project restored and reapplied the entire two-point move with one undo and redo.

Devices and Win, the existing ADI-Shaper rollup opts into resetting omitted parameters on load. Both local preset editions already exercise the partial-state case in DF-E57. The same registry test can accompany your general fix. DF-E50's graph automation remains with the devices lane. The editor's live drag is verified through its direct device-parameter gesture.

## 19 November 2026 — Weighted curve drawings and editor efficiency

**PR requested** · `clones/shaper-corners` · **Match weighted ADI-Shaper curves and cache the editor drawing** · `afcad928`

Review after the panel. The curve editor and audio processor use the same periodic soft, medium and hard points. Points at the same time retain a hard discontinuity. The old hold, line and bend parameters still recall. The beat clock offers all 23 measured durations, including dotted and triplet choices. A point or clock edit auditions immediately and releases as one undo step. On the matched quarter-note drawing both editors show 43%. The director has the two 1920 × 1080 captures.

In 34 measured contours, the maximum gain discrepancy across 816,000 samples was below 0.00000787. For the mixed-weight Volume drawing on stereo kick and bass at 48 kHz, the worst audio difference across 192,000 frames per channel at each buffer size was below 0.00000482. The maximum RMS difference was 0.000000703. Median and 99th-percentile callback times in microseconds were 9.0 and 19.7 for ADI versus 24.1 and 97.5 for the reference at 256 frames. At 2,048 they were 93.6 and 515.6 versus 64.3 and 228.4. The larger buffer is the next CPU case.

The editor now caches its static drawing and redraws live signal and meters separately. On the same full-band gate, the open editor used 0.117% of the 256-frame audio budget and 0.073% at 2,048. Offscreen paint at 1,300 × 794 pixels fell from 15.15 to 10.47 ms and from 13.76 to 10.35 ms. The open and closed stereo outputs matched.

The registered device recalled all 873 private editable programs, checked 105,984 explicit values, reset every omitted parameter and rendered finite stereo. The alternative compatible drawings recall the same programs. The private catalog covers all 1,048 source programs. The ten Release device and Pd suites passed in each Pd sample precision with warnings as errors. With JUCE and Pd enabled, both the registered panel-widget test and the project editor test passed through their production build targets. On one matched bar of stereo audio, 37 local programs had a stereo RMS error below 0.001. The private catalog includes each program’s result and its missing controls. The existing requests for the window opener, instance waveform feed and transport forwarding remain with Win.

## 19 November 2026 — ADI-Shaper panel and local preset recall

**PR requested** · `clones/shaper-panel` · **Add the ADI-Shaper curve editor and live waveform** · `afe32533`

This review is stacked on Time. The panel draws its own stage tabs, band display, contour points and tools, stage and master wet controls, output meters and the input and output waveforms. Selected bands expose their crossover and clock controls. The device has 512 shared point slots and preserves all 463 existing parameter identities. The full-range Volume and Time comparisons use matching 1920 × 1080 renders of both editors on the same kick and bass. The Time picture uses the registered processor’s actual stereo output.

Dragging changes the audio before release. One project undo restores the whole edit, including a multi-point curve or a preset. Tests open a real project, recall sparse states, undo and redo, cancel a preview, reopen the project and refuse an editor whose device was removed. The combined Release build passed all ten device and Pd suites with warnings as errors. The device panel can load local programs from its preset directory. Verification opened 873 local state files on the integrated processor, read back 231,922 explicit settings, verified omitted settings returned to defaults and rendered finite stereo. The private catalog also identifies programs whose active effects need additional stages.

Opening the Volume editor enabled real waveform capture and a 2,048-frame analysis window. Across three alternating passes at 48 kHz the median callbacks closed/open were 1.5/4.5 microseconds at 256 frames and 9.5/34.6 at 2,048 frames. Average callback budget use open was 0.137% and 0.080%, respectively. Software painting at the comparison size took about 15.15 and 13.76 ms per displayed frame. A dependent piece measures the effect of caching the drawing.

The shared opener and instance-feed hunks are with Win for review. Both shared window translation units passed the strict compiler check. A private headless host exercised the registered device, actual project state and editor render. The ordinary wrench click still requires the shared opener to land. The same-position Time transport regression also fails before the proposed forwarding change and passes with it. The core test distinguishes a new transport revision at exactly the same sample and quarter-note position.

## 19 November 2026 — Time stage ready for Win

**PR requested** · `clones/shaper-time` · **Add sample-history Time processing to ADI-Shaper** · `72cd8281`

Review after `clones/shaper-rollup`. Time adds 48 device parameters after the first 415. Each band reads the stereo input history against its contour. A constant tap uses contiguous runs from the same history. The native device, editable Pd path, saved-state round trip, marker oracle and catalog suites passed in Release in both Pd precisions with warnings as errors.

On the same stereo kick and bass at 48 kHz and 120 BPM, the one-bar, fully wet setting delayed both channels by exactly 96,000 samples. Both devices matched the oracle on 192,000 output frames per channel at both block sizes and reported zero processing latency. Across 10,240 callbacks per size, median and 99th-percentile elapsed times in microseconds were 0.9 and 1.7 for ADI versus 16.1 and 41.1 for the reference at 256 frames, then 7.8 and 13.5 versus 51.0 and 217.4 at 2,048 frames. The matched 1920 × 1080 editor captures are with the director. Exact same-position locate handling in the registered wrapper still requires the requested transport line.

**Devices lane, Pitch.** A shared two-head reader now reproduces your pending Shifter and the measured cyclic Pitch processor on 3,127,711 bit-exact head and output comparisons across three rates, impulses, independent stereo signals, kick and bass, pitch/window edits and irregular blocks. Each caller retains its own head phase, interpolation, window law and unison behavior. The candidate extraction is ready for your review. Please confirm the owner and public include for the common reader before either device takes it.

## 19 November 2026 — Panel, Time and Pitch ownership

The director has assigned the built panel to clones under ADR-0244 d4. The earlier Window sheet request is closed. Clones takes the `adi_shaper*` panel, drawing, device-specific build files, tests, curve storage and readouts on `clones/shaper-panel`. The reference and ADI renders will use the same transport, input and active controls. Our own controls draw the graph, the band display and the two audio traces. Edits audition during the gesture and persist in one undo step.

Time on `clones/shaper-time` takes the previously measured Time DSP, device parameters, canvas, external transport helper and shaper tests, preserving the first 415 automation IDs. The calibrated ADI pink source is retained. The user-impulse reverb comparison is closed for this pass.

**Devices lane, Pitch.** The current main Shifter is spectral. Your Shifter at `7c5d8158` uses two half-cycle delay heads, already restores clean unison and retains its independent stereo phases. Please agree a shared two-head delay reader with per-caller interpolation, weighting, unison and latency policies on that implementation. The ADI cyclic mode needs sinc reads and its existing transient/sideband checks. We can supply an extraction and cross-tests. Your seat keeps the Shifter file edits and its measured behavior. The real-valued state, catalog/count reconciliation and OneShot's two-argument native process override follow your review. Every built-in skips the parameter scan when its revision counter is unchanged.

## for after 3 December

Devices lane — DEV-22, the repeat effect, after the stock limiter. Please fold the following into your existing list of reference host readings.

**Repeat Cell test method.** Feed independently seeded, uncorrelated left and right markers. Keep an exact frame oracle outside the processor that identifies the expected source frame on each channel at each capture, wrap and return to input. Compare every output frame at 44.1, 48 and 96 kHz, both with whole Pd ticks and irregular host block lengths. Align the buffered adapter’s measured 64-frame displacement before comparison. The private fixture applied controls on tick boundaries. Extend it with capture, Grid and release events at frame offsets 1, 17 and 63 in a 64-frame tick, keeping each absolute event position fixed across both block arrangements. Compare the actual first affected frame with the event stamp. Off-tick response is a prospective test.

**reference host readings.** Place distinct markers immediately before and after capture to establish the capture direction and whether the trigger frame belongs to the slice. Change Grid at a marked frame during an active repeat and identify the first affected slice and the position where playback resumes. Use a tempo that gives a non-integer slice length, then index successive wraps to distinguish carried fractional phase from per-slice rounding. Cross a join between two marked levels and measure the transition length and amplitude weights. Locate while repeating, including a jump back to the same musical position, and identify which pre-locate frames remain eligible. At 20 BPM, exercise the longest reachable capture interval and report the oldest input frame actually selected, in frames and seconds at each sample rate. These readings are still to be taken in the reference host.

**Question to devices.** Should the DEV-22 repeat effect’s input history and `AdiShaperTime` share one two-channel history kernel, with separate storage per instance? Reuse would fit if the measured write source, retention across a locate and oldest readable frame agree. Each caller can keep its own slice boundaries and playback phase.

## 19 November 2026 — Mix View lane, two-source Focus contract

Mix View’s published log at `883236e5` records the signed two-channel Sum at `0504da1d`, its above-unity indication at `d6b67334` and preservation of the cancellation line at `e5176331`. Its newer Compare fix at `3774e736` retains a frozen B while A is invalid, releases demand during that interval and waits until A has fresh input. A queued same-position locate can still leave old captures visible to Learn Match pending an acknowledged transport revision. The owner tracks that separately from Focus timing preview.

### alignment scope 2.0.5

Two-channel sources received rectangular pulses and 8 Hz sines at 48 kHz, 256 frames per callback, 120 BPM, 4/4. Playback position and signal generation used the same sample clock. In the fresh 1/1 view, 16 cycles filled about 755 pixels, giving one 2 s bar at about 127 samples per pixel. At the initial magnification the plot moved about 140 pixels per unit sample amplitude. Doubling the sine from ±0.25 to ±0.5 doubled its height. The selected L channel followed a left-only signal and disappeared on right-only input. The initial state had a 1/16 musical grid and separate 1/16 alignment grid, zero correction, freeze on stop enabled and automatic vertical scale disabled.

Pulse edges originally 100 ms after each quarter note moved about 19 pixels earlier with +50 ms of source correction and about 19 pixels later with −50 ms. Reset recovered their positions to within one pixel. The manual gives a ±3,000 ms range and describes aligning the first transient after a transport restart to the preceding alignment-grid line. The inspected view exposed one named source. Inter-instance summation and its edition-dependent controls are documented by the vendor. alignment scope timing and edition controls

The documented goniometer puts mid on the vertical axis and side on the horizontal. The L/R correlation readout spans −1 to +1. Spectrum collision uses individual spectra before tilt. A frequency qualifies if at least two sources reach −60 dBFS there and each is within 24 dB of its own spectral peak. With two sources, the shading follows the lower qualifying level. The stored analyzer setup had a 1 kHz pivot and three spectral windows of 90, 45 and 20 ms. Pixel-spaced analysis was selected at five pixels per band, so those window durations do not establish a fixed update interval. The enabled short-term loudness history uses 3 s according to the manual. The documented momentary history uses 400 ms. The analysis passband limits, correlation integration and display refresh still lack independent calibration. alignment scope analyzers and loudness

### layer scope 1.5.03

With the same clock and tones, the fresh one-beat view showed four cycles across 1,200 pixels, or 20 samples per pixel. Pulses 2,400 samples apart appeared 120 pixels apart. At the initial display scale of 1.00, the vertical slope was approximately 105 pixels per unit sample amplitude. In the selected mid channel, a ±0.5 sine on either input alone drew ±0.25. Equal channels drew ±0.5 and opposing channels cancelled, establishing mid as `(L+R)/2`.

In Layers, independent sources shared a baseline. Stack gave each its own baseline. Sum added the two projected signals. One ±0.25 source read −12.04 dB and two in phase read −6.02 dB. Reversing one source collapsed the waveform to the centre immediately. After reversal the numerical peak was still −84.52 dB at 2.13 s and reached −∞ by 4.27 s in one run. These observations distinguish sample addition from peak release without fitting a release law. layer scope display modes

Beat sync uses the host tempo and Hz sync uses reciprocal period. The manual describes additive per-source and global visual offsets, both initially zero, with ten-sample wheel steps and one-sample fine steps. The numerical field accepts samples or milliseconds. Only the zero state was observed. The offset bounds and its movement under a nonzero setting remain unmeasured. The fixed-view phase bars compare the mono components of the first two enabled sources in Layers or Stack. When displaying a single source or Sum they compare its L and R. These operand rules are documented. The coefficient window, averaging, thresholds and refresh remain unmeasured. layer scope timing controls · Phase comparison

The documented Delta waveform subtracts the second enabled sound’s mono component from the first and can display the residual spectrum. When isolating a frequency band, IIR analysis introduces frequency-dependent phase shift. Comp mode corrects an estimated time offset, while linear FIR analysis costs a constant buffering interval reported in samples. These control laws come from the manual. Difference display · Analysis controls

### Timing preview acceptance

With a frozen pair on the same audible timeline, keep A fixed. If B arrives `d` samples late, preview `B[n+d]` beside A and recompute their two-channel sum on the overlapping interval. Label the advance in samples and milliseconds. Use a separate polarity control. Discard the estimate when the capture revision changes. Preview must leave playback and saved track timing intact. Both reference effects reported zero samples of processing compensation and passed 204,800 two-channel frames exactly at their measured defaults, tested with silence, noise and displaced impulses.

The independent fixture recovered a 137-sample advance (2.854 ms at 48 kHz) from a delayed broadband copy, including an inverted copy. The six 4,096-frame kick-and-bass regions yielded three weak and three ambiguous results. All six should retain their waveform and levels with no suggested shift. Silence and a steady repeating tone also supply no unique lag. As a CPU baseline, the older component at `f7fab268` used 2.15% of one core during updates alone and 10.74% during updates plus normal clipped software painting at 30 Hz. That baseline predates the signed Sum, headroom indicator and flat-path change. The timing preview is still an owner implementation item.

**Current kick-and-bass cost.** At `3774e736`, three independent software-rendered runs each exercised 480 updates per condition at 30 Hz and 48 kHz. All four source traces and both Sum paths were present. An independent sum over the first 1,024 frames gave L/R peaks 0.669833 / 0.674723 and RMS 0.350962 / 0.354846, which the component reproduced. The 1,046 × 361 image matched across all three runs. Relative to suspended Focus, update-only cost was 2.54% of one core. Updating and painting the normal 1,025 × 288 repaint area cost 15.63% (three runs 15.43–16.11%). Median paint time was 4.45 ms and p95 was 6.72 ms. These are message-thread costs per simulated 30 Hz refresh budget. Audio rendering and the proposed timing preview are outside this measurement.

## 19 November 2026 — Window lane, ADI-Shaper proposal sheet requested

Please draw the DEV-31 built-in two-channel audio effect in Device View at its actual strip size. Show an initialized instance and one with two active stages. In the active example, show where the user reads the signal order, selects Sweep and a band, and sees which part of the band’s curve is playing. Show how the same view presents a held segment and a ramp, and how a focused point exposes its time and value. Indicate which controls remain visible at the minimum strip width. Please return the sheet to the director and link the review in the Window log.

The review stack exposes 415 parameters across seven stages in fixed signal order. The three bands can use independent curves. Each curve has eight heights at fixed, equally spaced positions in the cycle. The panel can show the time of the selected point and edit its height. Clones supplies the stable parameter indices and test fixtures. Window owns the panel proposal under ADR-0244 d3/d9 and ADR-0248 d11. Implementation of the curve editor follows the director’s review of the sheet.

## 19 November 2026 — DEV-31 seven-stage review

**PR requested** · `clones/shaper-rollup` · **Add ADI-Shaper as a seven-stage built-in Pd audio effect** · `e87f45dd`

Win: one commit on main `12aab48a` carries the original Volume, Width and Pan implementation through Filter, Sweep, Drive and Crush. The device opens in ADI → Audio Effects. Its editable canvas, compiled-in external and canonical native processor use the same 415 parameter indices. Eight equally spaced heights and a choice of hold or linear interpolation per curve preserve the state contract described below. The panel sheet belongs to Window.

MSVC x64 Release with /W4 /WX passed eleven CTest suites in each Pd precision. The catalog, five DSP suites, editable versus canonical parity, native devices, Pd engine, Pd device and zero-delay host suites ran at both precisions. An independently altered canvas swaps the two output connections and checks its stereo output against known samples. Sparse recall into an active instance restores omitted stages to their defaults and releases Drive compensation. Stored beat information survives a missing graph transport, while an exact segment takes priority. Both builds embed the same 61,101 normalized canvas bytes, verified against the shipped file. The Session regression with upstream compensation is due when DF-E29 lands.

**Kick and bass, 48 kHz.** The registered native device and the reference shaper 3.6 processed identical stereo samples at 120 BPM with their editors closed. Each pair ran serially on the same logical processor, after 98,304 warm-up frames. The table gives callback median / p99 in microseconds over 10,240 calls per device, configuration and block size.

| Active configuration | ADI at 256 | Reference at 256 | ADI at 2,048 | Reference at 2,048 |
|---|---:|---:|---:|---:|
| Three-band Volume | 24.1 / 52.2 | 40.6 / 92.5 | 113.2 / 301.2 | 249.4 / 488.6 |
| Width, mono | 1.7 / 2.2 | 19.6 / 49.2 | 6.3 / 18.3 | 76.0 / 189.9 |
| Pan, alternating hard endpoints | 0.9 / 1.6 | 30.0 / 73.0 | 6.6 / 11.8 | 165.3 / 407.1 |
| Filter | 23.9 / 62.9 | 80.0 / 208.6 | 187.2 / 402.3 | 348.5 / 838.5 |
| Sweep | 7.1 / 23.7 | 42.4 / 91.1 | 54.3 / 134.7 | 118.2 / 285.9 |
| Drive | 52.2 / 144.9 | 131.3 / 971.8 | 436.9 / 1115.1 | 797.7 / 3177.9 |
| Crush | 21.1 / 43.0 | 66.4 / 208.6 | 166.3 / 331.8 | 241.1 / 781.0 |

Median thread cycles per block decreased by 37.6–96.6% across the fourteen stage and block-size comparisons, including input restoration and timing overhead. Callback medians decreased by 31.0–97.0%. At 256 frames the 5.33 ms deadline was exceeded on 9 of 71,680 ADI calls (Width 1, Filter 2, Drive 4, Crush 2), and 16 reference calls. Both recorded zero misses at 2,048 frames.

The four-second stereo captures used the same absolute start. Both devices reported 16 samples for Drive and zero for the Volume, Filter, Sweep and Crush settings. The largest channel RMS difference among those five cases was 0.00003535 for Drive. On the mono Width and alternating hard Pan settings, ADI reported zero and the running reference reported 16. The older reference readings of zero for these settings were obtained at startup. After compensating those 16 samples, both Width channels agreed within 2.2 × 10⁻¹¹ RMS. The held Pan levels followed the independent endpoint oracle within 6 × 10⁻⁹ RMS when measured at least 128 samples away from an edge. The switching transients differ: the compensated whole-capture RMS residual was 0.00907 left and 0.00498 right. These tests cover one selected setting per stage.

## 2026-11-19 — DEV-31 review scope

Win, devices and Window: the single review branch will be `clones/shaper-rollup`, rebuilt from the original Volume, Width and Pan implementation through Filter, Sweep, Drive and Crush. Earlier per-stage PR requests in this log are superseded. Clones takes the ADI-Shaper DSP, Pd external, canvas, per-device build manifest and device tests. The shared edits are confined to device registration, catalog capabilities, native transport dispatch, state recall, canvas embedding and their registration tests. Window owns the panel proposal and Mix View owns the Focus implementation.

The initial state format keeps eight equally spaced height controls per band with a held or linear segment law. Their stable parameter indices allow automation and project recall. A transient knee between grid points, arbitrary point timing and a separately curved segment cannot be represented exactly. Extending the state later requires an explicit version and migration.

## 2026-11-19 — Kick and bass in the actual Focus component

For the Mix View seat. The clones seat built the published `f7fab268` Focus UI on its own assembly from `origin/main`. The Focus source, sum helper and member-paint paths also match the owner’s `bf221a35` release. This exercise used the existing project and real Focus component on a software peer, with its two stereo post-fader taps driven by the eight-second kick and bass. Every active update advanced on the project’s sample clock. The captured component is `ADi-Focus-kick-bass-component.png` (1,046 × 361). Its six independent renders matched byte-for-byte.

The opening 1,024-frame window showed both stereo member paths. Its live sum peak was 0.6698329002 left and 0.6747228503 right. Its RMS was 0.3509617560 left and 0.3548456961 right. The four values matched an independent sample loop. The published paint code displays the summed level numerically; the separate sum waveform and the non-destructive timing preview remain with Mix View. The musical fixture withheld an automatic shift on all six selected regions. The independently calibrated delayed-copy fixture recovered a 137-sample advance at 48 kHz.

At 48 kHz and 30 updates per second, the actual component’s normal redraw rectangle was (13,34), size 1,025 × 288. Each condition used 480 updates per trial, across three fresh trials per painting method, with the same tap writes and root setup. Subtracting suspended Focus gave a median **10.74% of one core** with normal clipped software repaint (paired range 10.45–11.82%). Updating without drawing added **2.15%**. The clipped raster added **8.59%** and took median **2.74 ms** per paint, p95 **4.59 ms**. Repainting the entire component gave **15.23%** above suspended, median **4.23 ms** per paint and p95 **6.58 ms**. These are measured thread CPU and unpaced software-peer timings. Desktop compositing and GPU presentation remain outside the test. The earlier 512 × 180 rendering benchmark covers proposed sum/preview paths at a different size. The two sets of percentages must be interpreted at their respective sizes and paths.

The dedicated scratch fixture compiled with MSVC Release strict warnings, used the published peer’s public capture inputs, and added no product changes. The reproducer is `ADi-Focus-component-check.zip`.

## 2026-11-19 — Four-track shaper CPU and independently extracted Pd kit

The Windows review at `592f6ba7` also has a 37,205-byte editable Pd kit. The canonical canvas and its parameter abstraction match the commit byte-for-byte. The complete 753-parameter device passed the strict MSVC native-versus-editable suite with both search paths pointing at the extracted archive. Tests exercised state, transport, stage order, nonlinear latency and stereo split-band envelopes. `ADI-Shaper-Pd.zip` SHA-256 `a615d29774d99042e15c7983bcee7f0b28560c20a812e7bbfdca2be7dc16c125`.

### Four tracks at the 256-frame buffer

Four independent ADI-Shaper instances or four independent reference instances processed the same stereo kick and bass on four tracks. The host called the four instances serially on one pinned render thread. At 48 kHz each combined callback has 5.333 ms to complete. The table gives medians across nine trials of 8,192 callbacks each. The thread CPU percentages are CPU time divided by represented audio duration; they include identical input staging, timing and output checks. Cycle percentiles surround the four processing calls and their timing queries.

| Four-track load | ADI / reference render-thread CPU, % of one core | ADI / reference median cycles | ADI / reference p99 cycles | ADI / reference p99 wall time |
|---|---:|---:|---:|---:|
| Moving hard-clip Drive | 6.044% / 11.337% | 1,163,585 / 2,099,943 | 1,425,479 / 2,839,714 | 7.911 / 9.433 ms |
| Internal Width follower | 0.465% / 2.325% | 68,594 / 364,706 | 113,087 / 615,134 | 0.045 / 1.116 ms |

The four-instance moving Drive used 44.59% fewer median cycles and Width used 81.19% fewer. **Both four-Drive p99 wall times exceeded the 5.333 ms deadline under the shared workload.** Each instance's two-channel capture matched its corresponding single-instance output bit-for-bit. The moving Drive fixture reported 16 samples of latency in every instance, with worst stereo sample error 0.00020603 against the reference. Width reported zero samples, with worst stereo sample error 0.0000005663.

## 2026-11-19 — ADI-Shaper Windows review build and internal Width follower

**PR requested** — `clones/adi-shaper-follower` at `592f6ba7`. Title **Add a measured stereo envelope follower to each shaper Width band**. Review after `clones/adi-shaper-order` at `f820812f`, the cumulative head containing the previously requested editor and faster Drive. This is one review chain. The new commit appends 27 parameters after the existing 726. Each band has its own Add/Multiply, amount, depth, offset, threshold, attack, hold and release. Older project states start with the followers off.

All twelve registered DSP/device MSVC Release suites and the separate real curve-editor CTest passed with strict warnings. The final desktop application and required scanner linked. The x64 review ZIP embeds the matching source bundle, guide, Pd canvas, and license notices. The exact normalized canvas from this commit was verified inside the linked executable. The desktop application was not launched. Windows review SHA-256 `62a12b708d49dcebd96f4299b96220c6252072d1825dd730ae8af8abd093e3ee`. Matching source SHA-256 `8f55f78675d42b3ea27a3d213f4a21b6a2a67ef9a0d42692972c72da42315aa1`.

On the stereo kick and bass the Width follower used 90.48%, 78.53% and 61.58% fewer processing-thread cycles than the measured reference at 64, 256 and 1,024 frames. Worst stereo sample difference was 0.0000005663 and worst-channel RMS difference was 0.00000009136. Both reported zero latency. Each point used nine trials of 8,192 callbacks at 48 kHz. At 256 frames the release also used 76.7% fewer cycles for one-bar Time, 53.4% fewer for Crush, 31.2% fewer for static hard clipping and 55.3% fewer for moving hard clipping. Those four native renders reproduced the previous rack's signed stereo samples exactly. The preceding rack matrix covered nine reference-matched settings at three callback sizes. All 27 used fewer measured cycles than the reference (minimum 14.9% for 1,024-frame independent stereo white noise). The room used its own response and has a separate absolute cost. These are editor-closed, one-instance results. Multi-instance and visible-editor comparisons remain open.

The follower's lower attack boundary fitted 5.0018 µs from the original first step sample at 48 and 96 kHz. Release interpolates logarithmically between twelve measured 48 kHz durations. Its modeled maximum is 1.46% longer than the measured maximum at 96 kHz. Adaptive release, detector filtering and external-key routing remain separate from the internal follower. Window owns mounting the existing curve editor. Mix View owns the summed trace and alignment preview.

## 2026-11-19 — External Width detector and fresh drawing-kit reproduction

The reference accepted a separately enabled stereo key bus. The table below supersedes the earlier host's failed four-input test for Width. It gives devices a positive routing fixture while the internal follower topic is in validation.

### External stereo key, isolated from the input

The reference's native plug-in interface enumerated a stereo main input, a separately named auxiliary input initially disabled, and one stereo output. Explicitly enabling the second bus in stereo negotiated four input channels. The source switch was the only change between the two saved Width states. This resolves the earlier two-channel host limitation for the Width follower. External audio triggering for the LFO is a separate measurement.

For every 256-frame callback at 48 kHz, the main input stayed fixed at L = +0.01, R = −0.01. Five auxiliary plateaus lasted 245,760 samples apiece. The table gives mean side gain over the final 2,048 samples in each plateau. Base Width was 50% side gain, Add amount 75%, neutral offset, threshold zero and Attack/Hold/Release at 50%/0%/50%. Both modes reported zero samples of delay.

| Key L / R | Internal source | External source |
|---|---:|---:|
| 0 / 0 | 0.51999944 | 0.49999999 |
| +0.20 / 0 | 0.51999944 | 0.89999102 |
| 0 / −0.40 | 0.51999944 | 1.29998205 |
| +0.15 / −0.15 | 0.51999944 | 0.79999100 |
| 0 / 0 after release | 0.51999944 | 0.50000690 |

The detector follows the greater absolute amplitude of the source pair in these cases. Inverting one key channel still drives Width. The music stays on the main input and its side gain changes independently of the key signal. The internal follower topic implements the band's own signal; four-input Width routing remains a devices-lane integration point.

### Focus drawing kit reproduction for Mix View

The standalone drawing kit was rebuilt from its distributed archive using its own documented Release configuration, pinned graphics library and MSVC strict warnings. The program reproduced every geometric count on the two eight-second stems: 4,026,353 retained vertices of 26,910,660, largest observed deviation 0.199997 pixel. Its displayed preview remains a fixture for the owner's implementation. Its CPU tables describe its software rendering path.

## 2026-11-19 — Visible scope calibration for the Mix View owner

Handoff to **Mix View**, synchronized against its DF-M26 log at `133b2aa2`. The clones seat worked from published refs in its own checkouts. Mix View owns the summed trace, timing preview and their visible integration. On the shared eight-second kick/bass fixture, render the raw stereo sum before binning and suppress the timing suggestion on all six weak or ambiguous regions. A broadband delayed copy should suggest advancing B by 137 samples (2.854 ms at 48 kHz). The paired drawing test and the acceptance cases are in the earlier entries. The reproducible kits are named `ADi-Focus-validation.zip` and `ADi-Focus-drawing.zip`.

### Two-track Focus waveform calibration

alignment scope 2.0.5 and layer scope 1.5.03 each received native stereo VST3 audio in 256-frame blocks at 48 kHz, 120 BPM and 4/4. Playback position came from the same sample counter as the input. Each editor rendered on an isolated, non-interactive display. An 8 Hz sine, four channel/polarity combinations and rectangular pulses distinguished live traces from static controls. All pixel readings below refer to these dimensions and zoom settings.

| Fresh view | alignment scope 2.0.5 | layer scope 1.5.03 |
|---|---|---|
| Time | The 1/1 view contained 16 cycles of the 8 Hz tone across about 755 px. One bar occupied 2 s, about 127 samples/px. The musical grid indicated 1/16. | The beat view contained four cycles across 1,200 px. One beat occupied 0.5 s, or 20 samples/px. Adjacent clean crests were 300 px apart. |
| Vertical scale | In the 1,000 × 375 surface, the trace centred near y = 206. The coloured core of a ±0.25 sine covered rows 171–240 and ±0.5 covered 136–276. This is about 140–142 px per unit amplitude. | In the 1,200 × 300 surface, the centre line occupied rows 104–105. A ±0.25 sine covered 78–132 and ±0.5 covered 52–158. This is about 105 px per unit amplitude. The visible gain was 1.00. |
| Channels | L/R and L were selected. Identical L/R, left-only and opposed L/R gave the same repeating trace at left amplitude. Right-only suppressed the repeating wave. | Sum and M were selected with offset 0. Left-only or right-only at ±0.5 drew the ±0.25 trace. Matched ±0.5 channels drew ±0.5. Opposed channels collapsed to the centre line. This measures the arithmetic mid, `(L+R)/2`. |

**Two independent sources in layer scope.** With one ±0.25 sine and a silent second source, the Sum trace spanned rows 78–132 and the peak read −12.04 dB. With both sources in phase it spanned 52–158 and read −6.02 dB. Inverting the second source collapsed the trace to rows 104–105. On one run its numeric peak still read −34.48 dB after 1.07 s of reversed input. In a second run it read −84.52 dB after 2.13 s and −∞ by 4.27 s. The rolling readout and the visible summed trace settle at different times.

Two equal-duration pulses of +0.25 and +0.5 began 2,400 samples apart. In Sum, their filled spans at y = 90 began at x = 238 and 358. The 120-pixel separation equals 50 ms; their 1,200-sample durations correspond to 60 pixels before line rounding. Selecting Layers placed the two coloured pulses on one baseline. Selecting Stack placed them on separate baselines. The displayed names matched the two input tracks.

**alignment scope source alignment.** Repeating +0.5 pulses began 4,800 samples after each quarter note. Changing only the saved source-compensation control from 0 to +50 ms moved three leading edges from x = 254/442/631 to 235/423/613. At −50 ms the same edges landed at 273/462/650. Restoring 0 returned them to 253/443/631. Positive correction advances the captured source. Pixel rounding accounts for roughly one pixel of uncertainty. The manual gives a ±3,000 ms range and a separate grid for transient detection. The two calibrated settings and the reset were measured. alignment scope manual

**Remaining reference limits.** Two loaded alignment scope instances yielded one named track in the measured editor. Its multi-instance Sum could not be calibrated in that session. The manual distinguishes a fixed main/sidechain pair from assignable multitrack stacks across editions. No edition was inferred. In layer scope the wheel and drag attempts left the latency field at zero, so its step sizes and sign remain documented controls. The phase-correlation bar window, averaging, numeric thresholds and display refresh rate need separate calibration. Only bypass was exposed as a named alignment scope automation parameter. layer scope exposed bypass and 2,080 generic MIDI mappings, with no named waveform scale or latency parameter. alignment scope edition and stack controls · layer scope timing controls · layer scope phase display

**ADI acceptance.** Earlier kick/bass checks establish exact sample summing, extrema, estimated lag and CPU cost on their specified paths. Their author-drawn raster fixture applies its own pixel transform and clips displayed coordinates above unity. Use the actual Focus component to check the overlay, summed trace and timing preview under its own zoom, stereo routing and headroom. A matched pair should double the displayed amplitude inside the visible range. An opposed pair should collapse immediately to the centre. With separated pulses, shifting B by 50 ms should move B's pulse in the preview and the sum, while A's pulse stays fixed. Keep numeric peak release and correlation confidence as separate assertions.

**Phase test for the owner.** In layer scope, the two source names, Layers selection and the Show menu's Analyser entry were visible. The isolated session opened that menu, but the phase display could not be activated with the available input path. To calibrate it, use a session selected for interactive testing, select Layers and the phase display under Show → Analyser, then play the same 8 Hz inputs at 0°, 90° and 180°. Record the named pair and the displayed bar values along one beat. Repeat with Sum and with one sound selected to verify the documented switch to left/right channel comparison. Phase controls

## 2026-11-19 — Internal envelope and trigger measurements for ADI-Shaper

The clones seat reserves `clones/adi-shaper-follower` for the Width band's internal follower. It extends only the existing ADI-Shaper DSP, compiled external bridge if needed, canonical patch, device-specific build file and shaper tests. This topic starts from the eleven-stage order and editor integration at `f820812f`. External key routing, MIDI plumbing and audible trigger lookahead remain separate devices and engine integration topics. No Mix View or shared Window file is reserved.

### Envelope detection and curve combination

The Volume stage provides a triggered level contour. Its initialized band exposes a volume contour and gain. The Width contour provides a separate, directly measurable envelope follower, while the reference offers a separate compressor for automatic level reduction. These distinctions matter when assigning a follower to an ADI control. Reference manual, pp. 14, 25, 47–55

The following Width measurements used a running 120 BPM transport, 48 kHz stereo and 256-frame callbacks. The contour was flat at height 0.25, giving a side gain of 0.5 with the follower off. Side gain was read as the output channel difference divided by the input channel difference. Seven stereo polarity/level pairs gave a steady detector level equal to the larger of the absolute channel amplitudes. For example, either channel at −0.2 paired with +0.02 on the other gave side gain 0.89999 at the tested positive amount. An opposite-polarity pair at ±0.15 gave 0.79999. No measured follower setting reported host latency.

Let `h` be the contour height, `e` the detected peak amplitude, `a` normalized Amount, `o` normalized Offset and `d` Depth index 0, 1 or 2. For Width, clamp the resulting height to [0,1], then multiply by two to get the stereo side gain.

| Mode | Height before clamping |
| --- | --- |
| Add | `h + 2(o − 0.5) + e × (2a − 1) × 2^(d+1)` |
| Multiply | `h × e × 2a × 2^d + 2(o − 0.5)` |

Five Amount settings, three Depth choices, three Offsets and seven input plateaus verified the Add law to within 0.000048 absolute side gain. In Multiply, the same base height and a level of 0.2 gave side gain 0, 0.1000 and 0.2000 at Amount 0, 0.5 and 1. The default Depth corresponds to an Add Amount span of −200% through +200%. Depth indices 1 and 2 double and quadruple this span. The initialized follower was off, in Add mode, with Amount 0.5, Offset 0.5, minimum Threshold, Attack 0.1, Hold 0, Release 0.2, adaptive release off, band detection, external key off and the detector filters fully open.

At normalized Threshold 0.25, input peaks through 0.21 left the base side gain at 0.5, while peaks 0.41 and 0.71 received the same modulation as they did with the gate open. Threshold 0.5 admitted 0.71 and excluded 0.41. These are steady-state gate observations. Threshold-transition hysteresis, adaptive release and detector filter slopes remain open.

### Attack, hold and release

A 0.01-amplitude side pilot remained audible throughout an independent 1.5-second common-mode step of 0.18. This changed the loudest input channel from 0.01 to 0.19. Measurements used the first sample crossing each threshold relative to the input edge.

| Normalized control | Attack time to 63.2% (ms) | Added hold time (ms) | Release time constant (ms) |
| ---: | ---: | ---: | ---: |
| 0 | First sample | 0 | 5.0 |
| 0.25 | 1.562 | 31.25 | 120.3 |
| 0.50 | 12.500 | 125.00 | 477.4 |
| 0.75 | 42.188 | 281.25 | 1,588.7 |
| 1 | 99.938 | 500.00 | approximately 5,065 |

The attack follows a one-pole rise with `100u³` milliseconds as its time constant. Its time to 90% at quarter, half and three-quarter positions was 3.604, 28.792 and 97.125 ms. Hold shifted every measured release crossing by `500u²` milliseconds. Release fits exponential decay of the stored absolute peak down to the live input floor. Its time constant was computed at several fractions using the actual amplitudes 0.19 and 0.01. At the initialized Attack and Release, the fitted times were 0.104 and 85.8 ms respectively. The release control's exact interpolation between measured positions remains open.

A separate 440 Hz, 0.18-peak sine burst confirmed the response on program-like excitation. At minimum Attack/Hold/Release, side gain swung between 0.8163 and 0.8800 near the end of the burst. With Attack and Release both at 0.5 it varied between 0.8450 and 0.8456. With Hold alone at 0.5, the gain remained 0.8800 at 100 ms after the burst stopped.

### Volume contour triggering

A linear ramp from zero to unity lasted 12,000 frames at 48 kHz and 120 BPM. A right-channel pilot exposed the instantaneous gain. Decaying 920 Hz cosine bursts on the left began at samples 14,000, 31,300, 53,500 and 81,400 with peaks 0.7, 0.045, 0.7 and 0.7. Audio detector filters, threshold, detail and shift started at their initialized values. Global timing reservation and optional trigger smoothing were off.

| Mode | Measured phase behavior |
| --- | --- |
| Sync | Gain wrapped at 12,000, 24,000 and the following cycle boundaries regardless of burst timing. Reported latency was zero. |
| Audio retrigger, looping | The louder bursts restarted the curve at 14,671, 54,171 and 82,071. The quiet burst caused no restart. Between triggers it wrapped exactly 12,000 frames after the preceding restart. With no bursts it kept the transport-synced cycle. |
| Audio retrigger, one-shot | The same three bursts restarted the curve. Before the first burst and after reaching the selected endpoint the gain held there. Endpoints 0.25, 0.5 and 0.75 gave held gains 0.25003, 0.50006 and 0.75009. The initialized stored endpoint 0.984375 reached the graph's top and held gain 1 in this ramp. An otherwise identical render without bursts remained at the endpoint. |
| MIDI retrigger | Four note-ons commanded at the same four sample positions restarted the curve at exactly 14,000, 31,300, 53,500 and 81,400. The 30 ms note lengths did not truncate the ramp. Both free continuation after restart and holding the one-shot endpoint were verified. Reported latency was zero with smoothing off. |

The flat-unity audio control located the actual dry path 672 frames later at 48 kHz. The two lower-latency audio algorithms timed the strong burst restart one sample before that delayed dry onset. Moving Trigger Shift from its midpoint to its low/high limits moved each restart by −192/+192 samples, or ±4 ms. The higher-latency algorithm reported 1,152 frames and detected these same bursts 1,153 frames after input. The audio detector default was inactive, Threshold 0.0883883, Detail 0.5, Shift 0.5, algorithm index 0, internal main input, HP 0.2 and LP 1. The reference manual describes three algorithms and their intended material. Reference manual, pp. 42–44

With isolated decaying bursts from 0.01 through 0.90 peak, thresholds 0, 0.02 and 0.0883883 at midpoint Detail detected 0.16, 0.32, 0.64 and 0.90. Threshold 0.2 detected the last three and 0.5 detected the last two. Minimum Detail suppressed every test burst. Maximum Detail detected the same four as midpoint. The observed onset decisions depend on transient shape and Detail as well as peak level. Detector equivalence on other sounds remains open.

Separate flat-unity impulse renders measured the following actual audio delay. The value reported to the host matched the observed offset in all twelve cases. Trigger smoothing added approximately 6 ms. Normalized minimum/maximum shift altered the detector decision time independently of this reported delay.

| Rate | Algorithm 0, smoothing off / on | Algorithm 2, smoothing off / on |
| --- | ---: | ---: |
| 44.1 kHz | 617 / 882 | 1,058 / 1,323 |
| 48 kHz | 672 / 961 | 1,152 / 1,441 |
| 96 kHz | 1,344 / 1,921 | 2,304 / 2,881 |

External sidechain behavior remains unverified. The reference host exposed a two-channel main input, declined the requested four-input/two-output configuration, and still advertised two inputs after enabling all buses. It rejected the two additional key-channel connections. The internal-input, running transport and MIDI controls above all produced positive results in the same environment.

### ADI integration ownership

The existing shaper core can own envelope state and per-band clocks. Internal triggering can derive its detector from the current stereo input. The lookahead changes actual host delay, so enabling it requires aligned dry and wet paths plus a latency notification. The native and editable paths should be tested for all enabling/disabling combinations with Drive's existing 16-sample latency.

The devices seat owns the Pd-tier integration. ADI-Shaper currently declares two audio inputs and MIDI off in `src/adi/builtin_catalog.cpp`. For external keys, coordinate the four-input catalog declaration and the canonical factory in `src/adi/pd_builtins/native_devices.cpp`, then connect channels 3/4 from the patch and compiled external to the detector. The engine already carries `NodeIo::sidechain` and maps the optional extra planes for four-input built-ins. the stock filter is a working canvas precedent. For note triggering, coordinate the catalog MIDI flag, a shaper-specific note-on handler in the native factory and a `notein` connection for editable Pd. The existing Shifter device and `src/juce/pd_engine.cpp` demonstrate those paths. Basic note and sidechain delivery fit the existing host interfaces. Changes to event scheduling, transport packet fields or graph-wide routing would also involve the engine owner.

## 2026-11-19 — Stereo Drive oversampling refinement

**PR requested** — `clones/adi-shaper-performance` at `b8f22f7d`. Title **Reduce idle shaper overhead, calibrate Crush and accelerate stereo Drive**. This update belongs to the existing performance request. All twelve focused MSVC Release tests passed with strict warnings, including both Pd paths. The following comparison isolates the effect of this change inside ADI.

### Drive oversampling cost

Drive now processes the two channels' FIR histories together. The center and half-sample interpolation phases use symmetric coefficient pairs. At 4× oversampling, the two odd phases use the sum and difference of their reversed coefficients. Decimation pairs taps around the middle coefficient. The existing nonlinear transfer, oversampling factors and 16-frame delay are preserved.

Each cell gives the median reduction in processor-thread cycles per stereo frame against the previous ADi Drive kernel. Each pair ran the same two-second stereo kick-and-bass excerpt after 4,096 warm-up frames. Eleven pairs alternated execution order per mode and rate, on one pinned thread with precise floating-point arithmetic.

| Sample rate | Saturate | Hard clip | Rectify |
| --- | ---: | ---: | ---: |
| 44.1 kHz | 36.97% | 35.53% | 36.09% |
| 48 kHz | 38.46% | 35.33% | 35.50% |
| 96 kHz | 23.69% | 23.27% | 21.87% |

Both signed output channels matched bit-for-bit on the music at a fixed drive setting of 0.75, and on a second pass with moving drive, mode changes, trim changes and resets. Signed impulse, 199 Hz tone and 9 kHz alias probes also matched bit-for-bit at all three rates and in all three modes. The existing independent reference-response suite passed, including its 16-frame peak-location check, with maximum errors of 0.0002912 on its impulse matrix, 0.0000563 on its 199 Hz harmonic matrix and 0.0000302 on its 9 kHz alias fixture. These timings cover the Drive DSP kernel only.

## 2026-11-19 — Programmable processing order ready for review

**PR requested** — `clones/adi-shaper-order` at `98df1ab9`. Title **Add recallable processing order for the eleven shaper stages**. Review after `clones/adi-shaper-performance`. IDs 716–726 set one integer rank per stage. Lower ranks process earlier; tied ranks use the original stage order. The existing 715 IDs and initial routing are preserved. A stage carries its controls and delay history with it across a move. The existing 16-sample Drive compensation follows the active stage in either position.

All twelve focused MSVC Release tests passed with warnings-as-errors. They cover 4,096 sampled permutations, tied and extreme ranks, nonlinear Volume-before/after-Drive output, old states loaded into a used instance, live Time history and both native and editable Pd paths at irregular callback sizes.

## 2026-11-19 — Shaper performance and live state restore

**PR requested** — `clones/adi-shaper-performance` at `4ee156c0`. Title **Reduce idle shaper overhead and match the long-running Crush clock**. Review after `clones/adi-shaper-sources`. All twelve focused Release suites passed under strict MSVC warnings. Restoring an earlier state over a running shaper clears parameters introduced in later versions. Every built-in checks its parameter revision counter before scanning unchanged parameters. On this ADI-Shaper head that skips up to 715 atomic reads per callback. At 64/256/1,024 frames the measured neutral median callback fell from 1.6/1.7/2.1 µs to 0.4/0.5/1.3 µs in the two passes.

After calibrating the common hold curve and bit quantizer, full registered stereo rendering reduced the previous 0.09781 Crush peak error to 0.000004493 or less at all three block sizes. At 256 frames the registered Crush used 56.2% fewer processing-thread cycles than the original with the same input. The independent 48 kHz sparse-impulse and quantization evidence follows below.

The one-bar Time render matched both reference channels sample-for-sample at 64, 256 and 1,024. At 256, it used 79.7% fewer cycles; cyclic Pitch at +12 used 69.7% fewer; independent procedural stereo white used 46.3% fewer. Pitch's peak sample difference on this music was 0.000233. Noise is compared statistically. The original and ADI room use different impulse responses, so the room cost is recorded separately. The optimized host pass reused the immediately preceding reference timing results. An isolated 1,024-frame Drive reading was within 0.5% of its reference cycle count; it does not establish a large advantage for that setting.
### Crush clock and quantization precision

At 48 kHz, a four-second kick-and-bass render with Bits at 0.25, Resample at 0.5 and dither off exposed a growing difference in the fraction of each sample captured by the hold. Captures with 64- and 256-frame callbacks started after 96,000 frames of live input. The 1,024-frame capture started at 96,256. Removing the 10 Hz DC blocker for the measurement exposed the two successive output levels at each fractional wrap.

Across both channels and all three callback sizes, the reference clock at midpoint was 3,000.00500679 Hz. An exact 3,000 Hz clock put each wrap 0.160 samples late after two seconds and 0.481 samples late after six. Preserving the correct capture epoch, an independent replay of the earlier implementation matched it within 3.0e−8 peak. Changing the common exponential clock anchor alone reduced the 256-frame music RMS difference from 0.006587/0.006689 to 0.0000442/0.0000362, left/right. Further whole-curve checks measured 644.322453, 1,449.600047, 5,985.006099 and 11,732.046636 Hz at heights 0.25, 0.375, 0.625 and 0.75. The analytic curve retains small residuals at other settings and sample rates.

Fine input-step measurements refined the quantizer level count to `1 + a·u + (32767 − a)·u³²`, where `a = 14 × 32767/32768`. At 0.25 this gives 4.49989319 levels. An independent fit to the music gave 4.49989323. The existing output amplitude law matched. At heights 0.125 and 0.25, inputs 0.1818200 and 0.1111125 respectively remained just below the first threshold.

The standalone regression uses single markers after a quiet, continuous input to keep the clock active. At midpoint, a +0.5 marker on frame 96,015 yielded initial/held amplitudes 0.067283/0.419878. At frame 288,015 the same marker gave 0.124813/0.259661. Additional markers exercise 0.25, 0.375 and 0.75. With both common-law refinements, the independent musical replay across 64/256/1,024 frames had worst-channel RMS 8.0e−8 and peak 4.5e−6. These replay numbers describe the isolated mathematical check.

## 2026-11-19 — Curve editor ready for Window

**PR requested** — `clones/adi-shaper-editor` at `b34eb089`. Title **Add an accessible ADI-Shaper curve editor for the device strip**. The component was built with MSVC Release strict warnings. Offscreen tests passed for actual stroke placement, eight points plus their wrap, held and linear curves, mouse and keyboard editing, screen-reader values, cancellation and rebinding.

Window can link `adi_shaper_curve` and mount `adi::ui::AdiShaperCurve`. Bind a live device ID, stage, band and contour to the eight normalized point values, unit and display formatter. The synchronous edit callback identifies the point and original target for Begin, Value, End or Cancel. A Cancel restores the original value and closes the gesture. Refresh from the published model and unbind when the owner changes. The strip's stage/band selection and the common Undo transaction remain Window's integration points. The component is available to the owner; the director's desktop was not used for testing.

## 2026-11-19 — ADI-Shaper stage-order evidence and file reservation

The clones seat takes the shaper routing loop, its stage-order parameter declarations and the associated shaper tests on `clones/adi-shaper-order`. The branch adds an append-only order control per shaper; it also preserves every prior control ID. Sorting runs at a segment boundary only after a changed order control. It does not require a Mix View or Window change.

# Programmable stage order

The reference presents loaded stages in signal-flow order. Dragging a stage changes its position. A new initialized instance has all stages off. The initialized scalar parameters for a stage remain independent of its position. The complete saved device state includes an ordered vector of sixteen slot identifiers, including unselected slots. The manual describes chain editing and whole-device preset recall. Reference manual, pp. 29–30

## Acoustic test

Two full-range stages were enabled, with the master and band mixes at unity. Volume applied a constant gain of 0.25. Drive used symmetric hard clipping at a constant drive height of 0.5 and neutral input/output trims. The left input was a 400 Hz sine with 0.42 peak and the right a phase-offset 733 Hz sine with 0.28 peak. Each render used 48 kHz stereo and 256-frame callbacks. The first 4,096 frames were excluded from steady-state comparisons.

Two independent controls identified the order. One drove the isolated nonlinear stage with a quarter-amplitude source. The other attenuated the isolated nonlinear stage's output by 0.25. The predictions differ because attenuating before clipping changes which samples reach the clipping threshold.

| Saved order | Left / right output RMS | Left / right peak | Maximum error against matching control | Maximum error against opposite order | Reported latency |
| --- | --- | --- | ---: | ---: | ---: |
| Volume then Drive | 0.196160 / 0.186027 | 0.221243 / 0.217897 | 0 | 0.166183 | 16 frames |
| Drive then Volume | 0.052437 / 0.051897 | 0.055996 / 0.055913 | 0 | 0.166183 | 16 frames |

Enabling these two stages without changing the initialized slot vector produced Volume then Drive. Reversing the vector produced Drive then Volume. Moving each of the sixteen slot identifiers individually to the head and then the tail gave 32 further tests. Moving identifier 9 to the head or identifier 5 to the tail exchanged the measured order. Every other individual move retained it. This independently identifies Volume as slot 5 and Drive as slot 9 in the tested state format.

On saving, the reference placed the two loaded slots at the front of its returned order vector. The leading identifiers were 5, 9 for Volume then Drive and 9, 5 for Drive then Volume. In both saves the remaining fourteen inactive identifiers followed in ascending order. Each returned state was loaded into a fresh instance. Both reproduced the entire original stereo render sample-for-sample, including startup. This establishes recall for both active orders. The default order above is the initialized slot vector with these two stages enabled together. Adding the same stages one by one through the editor was outside this measurement.

## Proposed ADI storage and processing

- Append eleven integer parameters, one per existing stage, after current ID 715. Suggested range 0–127, with defaults 8, 18, 28, 38, 48, 58, 68, 78, 88, 98 and 108 for Volume, Width, Pan, Filter, Sweep, Drive, Crush, Time, Pitch, Noise and Reverb. Label each control with the stage and “Order”. Lower values run earlier. Gaps allow most moves by changing a single control. A future drag editor can assign these same values to the desired sequence in one undoable gesture.
- Decode ranks into a permutation of the eleven stage identities. Sort by ascending rank, settling ties by the stage's permanent identity in the legacy ADI chain. A collision always produces a complete deterministic order. A change to one stage's rank never deactivates or duplicates another stage. Any of the 39,916,800 permutations is representable. Existing projects that lack all eleven keys take the old signal path. Partially populated keys can be restored in any sequence.
- Existing `adi.param` integer declarations already expose stable numeric identifiers, quantization, editable generic strip controls, project recall and automation in both the native and editable Pd paths. Each control can use an ordinary integer range. `adi.array` is for display reads. A convenient drag list would need a dedicated UI hook owned by the corresponding seat. A single full-permutation rank exceeds the exact consecutive-integer range of a 32-bit Pd float.
- Cache the route in a fixed eleven-entry array. Mark it dirty only when a quantized rank changes and rebuild once at the next segment boundary, after pending parameter deliveries. An in-place insertion sort needs at most 55 comparisons and 55 shifts. Run each active stage's existing block renderer once in the resulting order. Dispatch belongs outside the sample loops. Preserve a specialized fast path for the old order if the CPU benchmark justifies it. The current 16-sample Drive host compensation is independent of position. Other stage latencies follow their existing contracts.
- Retain each stage's clock, settings and DSP state with its identity across an order change. This avoids clearing delay histories, interrupting free-running contours or allocating a second rack for a crossfade. Live-tail handling on reference drag and click suppression remain unmeasured. The proposed recall guarantee applies to the order and controls. Audio already in a previous route can affect subsequent tails after a live move.
- Acceptance tests should cover direct nonlinear reference predictions in both orders, all eleven modules exactly once under tied and extreme ranks, exact legacy order on old-state restore, forward and reverse order of parameter delivery, a reorder that moves Drive while PDC stays 16, and native/editable Pd output through odd host segments. Benchmark unchanged default routing and a noncanonical active chain separately.

## 2026-11-19 — Eleven-stage ADI-Shaper ready for review

**PR requested** — `clones/adi-shaper-sources` at `380335a2`. Title **Add cyclic Pitch, generated Noise and an original convolution room to ADI-Shaper**. Review after `clones/adi-shaper-time`. All 12 affected Release suites passed under MSVC strict warnings. The three modules extend the parameter map from 463 to 715 and have zero additional host latency. The editable Pd patch and compiled processing path agree on the irregular-block stereo tests. Earlier projects retain their stored indices.

- Pitch implements the cyclic grain mode, with a 1–100 ms window and ±24 semitone contour. All eleven unison peaks across the three tested rates arrived on the measured frame. Fixed stereo tests measured at most 0.000100 sample error. A moving two-octave sweep gave peak error 0.000129. Polyphonic, voice and drum algorithms remain reference work.
- Noise generates independent white sequences at 44.1 kHz, interpolated for the session rate. It also offers separately calibrated ADI pink noise. Signal tests cover stereo statistics, same-channel mono, source pitch, level, trim, filter response, warm reset and digital silence. Source noise stays broadband under the musical crossover selection until its own filters are applied.
- Reverb convolves the incoming stereo signal with an original finite room response, factored into two short diffusion filters and sparse reflections. The reference-derived controls include Send, Return, width, gate, decay, size, synchronized/free predelay and Clear Tails. A closed send preserves the tail, a closed return mutes it, and the independent room passes exact predelay-shift checks. Its four routing layouts include the measured 25% default and the full-level dry and wet overlap at 50%. The response is ADI-authored. Importing a user WAV and reproducing other room assets remain separate work.

Additional Time evidence: at three free-Hz positions a separate amplitude cycle measured 30,459.5, 4,687 and 721 samples. The Time Follow read distance gave 30,459, 4,687 and 721 at the same settings. A held beat-rate clock stayed at 96,000 frames in the same test.

## 2026-11-19 — ADI-Shaper device drawing and processor work

For Window, the clones seat takes a standalone JUCE editor component in `src/juce/adi_shaper_curve.*` and a dedicated offscreen drawing/gesture test on `clones/adi-shaper-editor`. Its port accepts the active device parameter model and emits edits through callbacks for the existing gesture transaction. Window owns mounting the view and selection in the shared strip. The device panel, parameter feed, app shell and Mix View files stay with their current owners.

For devices, the clones seat also takes only the ADI-Shaper-specific parameter-read and legacy-state handling in `src/juce/builtin_host.cpp` with the device-specific recall regression and shared unchanged-parameter scan. The large shaper advertises hundreds of independently automatable parameters. A callback with no changed controls can skip reading those targets. Loading a saved earlier rack into a previously used instance should reset parameters added in a later version to their declared defaults. Neither change needs a shared strip or Mix View edit.

The reference analyzer classes enumerate as alignment scope 2.0.5 and layer scope 1.5.03 in the installed VST3 formats. For the other seat's identification question, their module SHA-256 values are `03dc311fa8505bec5f44d10ee75114c52efd4031bfd993242c65a049895e1f65` and `f326b2155d57bc019b368059324bdb4a91f806e291462ee42b908dee1082b0aa`. The measured default audio paths and documented controls are in the Focus handoff. Full-edition entitlement is unverified.

## 2026-11-19 — ADI-Shaper Time ready for review

**PR requested** — `clones/adi-shaper-time` at `9ead5d66`. Title **Add transport-aware multiband Time to ADI-Shaper**. Review after `clones/adi-shaper-drive-crush`. The eight-stage device preserves its first 415 parameters and appends 48. The zero-latency Time stage selects Follow or one of seven fixed ranges independently in each band, with beat/free clocks and mix. It retains recorded input over tempo changes and invalidates on a transport discontinuity. The native path uses the graph's segment sample position and revision. The editable patch has the existing 64-frame adapter latency and a coarser transport stamp.

Additional independent full-wet impulse readings at contour settings requesting 48.125, 48.25, 48.5, 48.75 and 48.875 frames all returned one full-level tap at 48. A requested 3.5, 512.5 and 12,000.5 returned 3, 512 and 12,000. ADi uses integer reads on stationary and stepped contours, and interpolates moving linear ramps. The moving-ramp interpolation law is still a comparison target.

Nine focused MSVC Release CTests passed with warnings-as-errors. Native and editable Pd paths were tested with independent stereo markers, interrupted blocks, partial blend and a seek. Core checks cover two clock modes, the range limits, continuous sample time through tempo ramps, transport revision, rejection of unavailable history, wraps, reprepare and three-band routing. The large Pd canvas is encoded without exceeding a source literal limit.

The shipping resource policy prepares three 16 MiB histories per shaper instance so automation can enable all bands at a sample boundary. These 48 MiB are paid even while Time is off. Preparation acquires and touches the pages off the processing path, and reset invalidates history in constant time. The original showed a roughly 48 MiB increase on enabling Time. Off-thread lazy provisioning could save memory in projects with many instances that never enable it.

## 2026-11-19 — Seven-stage CPU results and Noise acoustic contract

At `a3891a36`, the native shaper used 11.8–92.7% fewer processing-thread cycles than the reference in 24 matched cells (eight fixtures, each at 64, 256 and 1,024 frames, 48 kHz, nine warm trials of 8,192 callbacks each). Hard clip on kick and bass used 39.9%, 11.8% and 18.5% fewer cycles. Crush on the same input used 77.1%, 63.2% and 47.9% fewer. At 256 frames the respective median effect callback durations were 130.5/145.8 µs for Drive and 23.0/66.0 µs for Crush (ADi/reference). The three-band Volume render used 56.1% fewer cycles at 256. Processor-thread cycles include equally replenished input, while callback wall time excludes input preparation. Music captures use identical sample positions.

Across 192,000 observed stereo frames the largest sample errors were 0.000000477 for three-band Volume, 0.0000504 for Drive, and 0.09781 for Crush. Crush met the measured static hold-clock and step tests but this musical capture has a larger time-varying residual. Prior to extending a parity claim, isolate capture phase, integration and warm state with this same input. CPU comparisons use the matched settings above.

### Noise acceptance addendum

The installed reference generated white noise on a silent input. The level and pitch envelopes used running transport with a steady contour. Measurements used one full band, disabled followers, unity band and master mix, 256-frame callbacks, and Filter controls at their outer limits unless specified. A separate 1 kHz cosine with left/right amplitudes +0.15/−0.10 identified dry gain. RMS and correlation below are statistical estimates.

| Control | Acceptance evidence |
| --- | --- |
| Source and sample rate | At full level and nominal zero trim, white RMS was 0.024725, 0.022345 and 0.022329 in the left channel at 44.1, 48 and 96 kHz. Corresponding lag-one correlations were −0.0012, 0.2260 and 0.7298. At 44.1 kHz the spectrum was flat within 0.4 dB at band centers from 125 Hz to 20 kHz, and kurtosis was 3.008. The host reported 0 frames of latency. |
| Independent white model | Independent Gaussian sequences at 44.1 kHz with RMS 0.02475 and four-point Catmull–Rom interpolation predict RMS 0.022334 at 48 and 96 kHz, and lag-one correlation 0.2290 and 0.7310. At 96 kHz the measured/model spectral levels relative to 1 kHz were −2.14/−2.07 dB at 16 kHz, −4.45/−4.43 dB at 20 kHz, −21.04/−20.98 dB at 32 kHz and −44.19/−44.17 dB at 40 kHz. This is a measured-output model. |
| Level and trim | At 48 kHz, normalized level 0, 0.25, 0.5, 0.75 and 1 gave RMS 0, 0.005595, 0.011211, 0.016820 and 0.022345. Linear amplitude agrees with these readings. Trim follows approximately 60u − 48 dB, with 0 dB at 0.8. Relative measurements at 0, 0.2, 0.5, 0.9 and 1 were −48.17, −36.10, −18.04, +6.01 and +12.07 dB. |
| Pitch | Nine positions over the documented ±24-semitone span left procedural white within 0.02232–0.02242 RMS at 48 kHz. The 95%-energy frequency stayed between 21.19 and 21.30 kHz. A pitch-dependent resynthesis or cutoff change would fail these observations. Pitch response for sample playback remains unmeasured. |
| Filters | For the interior range, high-pass and low-pass responses fit two-pole filters with Q near 0.5 and nominal frequency ≈20.58 × 2^(10u) Hz. At normalized positions 0.254, 0.5 and 0.75, 48 kHz fitted HP corners were 120, 657 and 3,712 Hz. LP corners were 121, 661 and 3,776 Hz. The nominal frequency is approximately the −6 dB point. At 0.5, estimated HP/LP corner frequencies were 658/660 Hz at 44.1 kHz and 659/660 Hz at 96 kHz. Fits used ratios of the filtered and wide-open white power spectra. The far stopband at the highest LP settings had greater residual error. |
| Stereo and mixing | Independent stereo white had L/R correlation −0.00125 in the first 48 kHz render. Mono produced exactly equal output samples and about 0.02242 RMS per channel. Normal addition preserved coherent dry amplitude to within 0.001 and retained 0.02234 RMS noise. Noise Only at full mix removed the coherent tone. In that mode, either band mix 0.5 or master mix 0.5 produced coherent dry gain 0.500 and about 0.01116 RMS noise. Selecting Off in Noise Only gave exact silence. |
| Initialized values | The saved all-stages-off state had the noise source off, normalized HP 0.254, LP 1, trim 0.8, mix 1, Mono enabled and Noise Only disabled. The manual also describes the supplied stereo noises and how Mono takes their left channel. The measured identical-channel output establishes the mono result independently. Reference manual, pp. 16–18 |

**Pink.** The reference lists Pink as a downloadable sample. Its file was unavailable, and selecting it in a fully wet Noise Only test produced zero. An independent ADi procedural pink test signal used Gaussian spectral amplitudes weighted by 1/√f above 20 Hz. Its octave-averaged spectral density fell 2.99 dB/octave from 125 Hz to 16 kHz. Stereo correlation was 0.009 over eight seconds. Equal power with the white source, 0.02235 RMS per channel at 48 kHz, was an explicit ADi calibration choice. Matching the factory pink recording's exact level, spectrum and pitch requires a reference instance with its noise library installed.

## 2026-11-19 — Kick-and-bass waveform drawing cost for Mix View

The offscreen benchmark draws the two stereo sources, their sample-aligned arithmetic sum and an optional sum with B advanced 137 samples. Each row measures sampling, per-column extrema, path construction and software rasterization at 512 × 180 physical pixels, 1,024 source frames per window and 30 drawings per represented second. Six paired trials used the same 240 frames, width, strokes and input for both algorithms. Each trial subtracts that renderer's background/centerline thread time. Percentages express one logical CPU core.

| Drawn stereo signals | Full vertex list | Simplified line | Median paired reduction |
|---|---:|---:|---:|
| Kick and bass | 9.96% | 2.15% | 77.5% |
| Kick, bass and sum | 15.04% | 3.61% | 77.5% |
| Kick, bass, sum and shifted sum | 20.51% | 4.59% | 77.7% |

At most 16 consecutive line segments are simplified at a time. Keep both endpoints; split recursively at the original vertex with the greatest vertical deviation from the endpoint line whenever it exceeds 0.2 physical pixel. A vertical chord uses distance to the closed vertical interval. Every source vertex on the fixture was within 0.199997 pixel of its replacement segment, and 4,026,353 of 26,910,660 vertices remained (14.96%). The tolerance applies to geometry before stroking. Test dense noise and other zooms separately. A timing shift on the musical pair here exercises the drawing only; the automatic preview correctly withheld a lag for the six measured kick-and-bass regions.

The capture/broker/measurement test separately took 1.77–1.82% of one core. Its min/max work overlaps this offscreen measurement. The director's window, its compositor and GPU presentation were outside this test. The Mix View seat owns display integration, scheduling and final in-application measurement.

## 2026-11-19 — ADI-Shaper source and cyclic pitch file reservation

The next device branch is `clones/adi-shaper-sources`. The clones seat takes the new `src/adi/dsp/adi_shaper_pitch.*`, `adi_shaper_noise.*`, `adi_shaper_reverb.*` and their own test files. It also retains the shaper-specific integration files and canvas from prior increments. Work starts with independent measured modes. The performance, timing and resource behavior of each stage will be checked after integration. The stock Shifter, Noise-generating devices, Reverb, shared Pd engine and Mix View files remain with their owners.

### ADI-Shaper Pitch reference

Four modes were rendered independently with one full-range stereo band, full wet, fixed pitch, linked formants, reserve latency off and no modulation smoothing. A pitch contour from bottom to top commands −24 to +24 semitones. Its midpoint is unison. The target read-speed ratio is `2^(semitones/12)`. The mixed-material mode has three grain choices. The cyclic mode has a continuous grain control. Voice and drum processing support independent formant contours. Pitch controls · Reference manual

#### Timing and stereo

| Sample rate | Mixed material | Voice | Drums | Cyclic |
|---|---:|---:|---:|---:|
| 44.1 kHz | 1,551 | 2,187 | 2,628 | 0 |
| 48 kHz | 1,687 | 2,381 | 2,861 | 0 |
| 96 kHz | 3,367 | 4,754 | 5,714 | 0 |

Figures are reported latency in frames for 256-frame callbacks. At unison, separate impulses on the two channels arrived at precisely the reported delay in the first three modes. At 48 kHz, independent pseudorandom inputs between −0.25 and +0.25 also matched sample-for-sample after compensating that delay. The cyclic mode reported zero at every grain setting tested. At the middle grain setting its unison peak arrived 575, 625 and 1,243 frames late at the three rates above.

A 0.5-peak click at frame 18,000 of a 48 kHz stream gave the following result. The span includes samples above 0.00001 in absolute amplitude. The opposite channel carried an equal, inverted click. The channels cancelled exactly, and repeating the test with a silent right input gave an exactly silent right output. These checks cover the two octave settings and this transient.

| Mode | −12 peak offset and amplitude | +12 peak offset and amplitude | +12 first to last sample above threshold |
|---|---:|---:|---:|
| Mixed material | 1,687 / 0.4500 | 1,687 / 0.2250 | 1,684–1,690 |
| Voice | 2,382 / 0.5000 | 1,729 / 0.2013 | 1,442–2,383 |
| Drums | 2,861 / 0.5000 | 2,861 / 0.2250 | 2,858–2,864 |
| Cyclic, middle grain | 710 / 0.4268 | 582 / 0.1551 | 270–1,202 |

All three mixed-material grain settings gave the same isolated-click result. Their effective window durations on dense input remain undetermined.

#### Sustained pitch

Each channel carried a different sine, 443 Hz at 0.25 peak on the left and 877 Hz at 0.20 peak on the right. The following readings are the strongest spectral lines in hertz on the left/right channels after settling for one second at 48 kHz. A one-second Hann window and interpolation between frequency bins were used. The right input began at a 0.33-radian phase offset.

| Semitones | Nominal ratio | Mixed material | Voice | Drums | Cyclic, middle grain |
|---:|---:|---:|---:|---:|---:|
| −24 | 0.2500 | 113.5 / 218.0 | 110.7 / 212.5 | 110.7 / 212.5 | 93.5 / 236.2 |
| −12 | 0.5000 | 223.2 / 437.3 | 221.5 / 434.0 | 221.5 / 434.0 | 210.0 / 449.8 |
| −7 | 0.6674 | 296.6 / 584.3 | 295.6 / 582.3 | 295.6 / 582.3 | 288.0 / 592.9 |
| 0 | 1.0000 | 443.0 / 877.0 | 443.0 / 877.0 | 443.0 / 877.0 | 443.0 / 877.0 |
| +7 | 1.4983 | 662.8 / 1,316.7 | 663.7 / 1,318.5 | 663.7 / 1,318.5 | 675.2 / 1,302.7 |
| +12 | 2.0000 | 883.4 / 1,757.7 | 886.0 / 1,763.0 | 886.0 / 1,763.0 | 909.0 / 1,731.4 |
| +24 | 4.0000 | 1,764.1 / 3,519.2 | 1,772.0 / 3,535.1 | 1,772.1 / 3,535.1 | 1,841.1 / 3,440.1 |

#### Cyclic grain model

For normalized grain position `g`, the fitted window length is `W = sampleRate × (0.001 + 0.099 × g²)` frames. It spans 1–100 ms. Unison concentrates the impulse around `7 + W/2`. At 48 kHz, ninth-point measurements from g = 0 to 1 in steps of 1/8 peaked at 31, 68, 179, 365, 625, 959, 1,367, 1,850 and 2,407 frames. At g = 0.5, an input of 0.5 produced a peak of 0.44998 with 14 taps above 0.00001. The signed impulse sum divided by input amplitude was 1.00005. Broadband unison had a 0.09772 maximum residual after integer-delay compensation on the ±0.25 fixture.

An independent low-cost model uses two moving fractional-delay reads, half a window apart. Advance phase by `(1 − pitchRatio)/W` per input sample. Read with offsets `7 + W × phase` and `7 + W × ((phase + 0.5) modulo 1)`. Crossfade with `sin²(π × phase)` and its complement and share the phase between left and right. This predicts sidebands spaced by `2 × abs(1 − pitchRatio) × sampleRate/W` hertz. At +12 the measured left peaks were 909 and 831 Hz, at amplitudes 0.1972 and 0.0494. At −12 they were 210 and 249 Hz, at amplitudes 0.1973 and 0.0524. A pair of analytic sine reads with the above crossfade reproduced the stereo steady-state waveforms to within 0.0046 root-sum-square channel RMS at −24, −12, −7, 0, +7 and +12. At +24 the residual reached 0.0235. At that setting the reference attenuated the 443/877 Hz test components to about 88%/87% of the analytic model.

An initial cyclic implementation can allocate its stereo history once, using about 38 KiB per band at 48 kHz for the 100 ms maximum plus interpolation support. Two short interpolation kernels per channel and prepared weights bound the sample cost independently of grain length. Proposed acceptance gates are zero reported latency, unison crest timing within one frame at all nine grain positions, middle-grain unison impulse peak within 0.005, dominant lines within 1 Hz and the two octave sideband amplitudes above within 0.005. Test the two channels independently and at opposite polarity. Evaluate automated pitch transitions, fractional-delay response above 1 kHz and callback sizes 64/256/1,024 before claiming wider parity or a CPU advantage. Reference CPU cost was outside this measurement.

#### Existing ADI processing

The pitch mode in ADI's existing Shifter is a windowed FFT phase vocoder. ADI's Warp renderer studies a clip, reads it again and renders the result with its existing stretch library. The devices lane has also proposed a live dual-read-head Shifter. Its 10–350 ms window and linear interpolation would require the cyclic calibration above. These are potential points for sharing DSP with the owner.

An independent stereo test of the existing stretch library 4.0's short-window live API found a fixed 512-frame processing block. At 48 kHz it reported 6,784, 3,648, 2,112, 2,880 and 4,031 frames of delay at ratios 0.25, 0.5, 1, 2 and 4. Its unison click peaked at 2,112 samples with amplitude 0.4828 for a 0.5 input. Its ratio-dependent timing and coloration need an explicit adaptation design. ADI lists a second stretch library for future integration. Its realtime capabilities merit a separate audition. The present evidence supports implementing the cyclic mode independently. Polyphonic transient matching, phase continuity, vocal envelope shifting and automatic grain selection still need dedicated fixtures before defining equivalent mixed-material, voice or drum modes. the existing stretch library live API · a second stretch library

## 2026-11-19 — Time reference and next device files

The Time increment is on `clones/adi-shaper-time`. The clones seat takes the ADI-Shaper core, external adapter as needed, canvas, device CMake file and shaper tests from the previous increment. It also takes the large-canvas embedding hunk in `cmake/devices/pd_native.cmake` and the new `adi_shaper_time.*` core and test. Existing parameter indices stay stable. The devices seat retains all other devices and the Pd engine. Mix View owns its Focus surface, sum and alignment-preview integration. Its published Focus review at `2102d4e9` already rejects ambiguous loop windows in its own path. Our separate legacy scope-helper reproductions and rendering measurements remain test evidence for the owner.

### Time stage reference contract

The Time stage plays earlier input from a per-band history. A flat contour at the top passes the current sample. Lowering the contour increases lookback. The host reported zero latency for every tested range, including an intentional 768,000-frame delay. The measurements below used a playing transport, 4/4, both channels enabled and full wet unless stated otherwise.

#### Time range and tempo

At 48 kHz and 120 BPM, a one-bar range delayed an impulse by 96,000, 48,000 and 0 frames at contour heights 0, 0.5 and 1. For a fixed tempo in the measured 4/4 meter, the target offset in frames is `(1 − height) × bars × 4 × 60 × sampleRate / BPM`. In follow mode the selected contour period supplies the bar count.

At 12 kHz and 300 BPM, one bar is 9,600 frames. Fixed ranges of 1, 2, 4 and 8 bars recovered a 0.5 impulse exactly 9,600, 19,200, 38,400 and 76,800 frames late. Follow mode recovered it at these bar fractions and multiples, in control order, with a maximum integer offset error of one sample across those settings.

`1/128, 1/64, 1/32, 1/24, 1/16, 3/32, 1/12, 1/8, 3/16, 1/6, 1/4, 3/8, 1/3, 1/2, 3/4, 1, 3/2, 2, 3, 4, 8, 16, 32`

At 48 kHz, the short fixed ranges labeled 2.5, 10 and 20 ms gave bottom-contour offsets of 239, 959 and 1,919 frames, midpoint offsets of 119, 479 and 959 frames, and top offsets of zero. These values refer to the tested full-height graph and its exported polarity. Other range modes and non-4/4 meter behavior require separate measurements.

A 48 kHz, 64-frame transport render changed from 120 to 60 BPM at sample 48,000. A marker injected at sample 8,192 appeared once at sample 200,192 in both one-bar fixed and one-bar follow modes. The 192,000-frame offset equals one complete bar at the new tempo. Apply current tempo to each read distance and keep previously written input available across tempo changes.

#### Usable history

The evidence bounds history by sample count. Follow at 32 bars, bottom-contour position, successfully recovered a marker 2,000,000 frames late at both 48 and 96 kHz. A requested distance of 2,100,000 frames gave zero throughout at both rates. At 48 kHz the marker returned at 2,096,640 frames with 64-, 256- and 1,024-frame callbacks. At 256-frame callbacks, a request of 2,096,641 rounded to a 2,096,640-frame read. A request of 2,096,656 produced zero. Both 64- and 1,024-frame callbacks produced zero at 2,096,832. Those observations bound the transition and leave its precise fractional threshold open.

At 48 kHz and 120 BPM, the 32-bar range itself spans 3,072,000 frames. In a 65-second render with a continuous 0.0001 input and a 0.5 marker at sample 8,192, contour heights 0 and 0.25 yielded all zero. Heights 0.5, 0.75 and 1 recovered the marker at offsets 1,536,000, 768,000 and 0 and reproduced the steady input after startup. Availability depends on each requested read. A long range remains usable over the part of its contour that reaches valid history.

#### Process memory measurements

Each run started a separate headless process. It prepared the same 65-second 48 kHz stereo source, loaded the device, warmed the disabled state, applied one test state, rendered a short warmup and then rendered the full source. The table gives process memory differences between the disabled warmup and the test warmup. Each tested state ran once. Figures include state deserialization and host activity, so they indicate allocation scale and do not identify a private implementation.

| State | Private commit increase (MiB) | Working set increase (MiB) | Marker offset |
| --- | ---: | ---: | ---: |
| Reload disabled | 1.94 | 1.89 | 0 |
| Fixed 1 bar | 50.43 | 50.28 | 96,000 |
| Fixed 8 bars | 50.16 | 50.14 | 768,000 |
| Follow 32 bars | 50.27 | 50.24 | Outside history |
| Fixed 8 bars, both crossovers active | 50.50 | 50.51 | 768,000 |

Insertion after source preparation added 46.13–46.99 MiB in process private commit and 51.46–52.24 MiB in working set. The enabled configurations added approximately 48.2–48.6 MiB more private commit than the disabled reload. Each subsequent 65-second render added a further 23.85–23.86 MiB across all five configurations, consistent with the host's approximately 23.80 MiB stereo output storage. Three stereo float histories of 2²¹ frames total 48 MiB and are consistent with both the memory observations and the usable tap limit.

#### Recommended ADI implementation

- Use a 2²¹-frame ring per band with two float channels, 16 MiB per stereo band. Support integer read distances from 0 through 2,096,640, corresponding to 43.68 s at 48 kHz and 21.84 s at 96 kHz. Treat the remaining 512 frames as a conservative guard. Track valid history separately from the masked physical index. On startup and reset an unavailable read contributes wet zero. At zero distance read the current input directly. Validate both neighboring positions for a fractional read. The fractional interpolation kernel, seek/reset transients and crossfade response still require calibration.
- Keep the history fixed when the transport tempo, period or contour changes. Snapshot tempo and beat position at segment boundaries, including tempo-map splits. Read each band's delayed contribution from its own split input, then apply the existing dry/wet rule. Preserve the zero-PDC contract for Time. Check interval extremes, partial wet/dry, every split, seeks, tempo changes and odd callback partitions with independently rendered fixtures.
- ADI already prepares its the stock delay and Echo histories off the audio thread. Its Sweep stage invalidates old history with counters. For Time, request a prepared history bank on the control or resource thread when an instance first needs Time. Allocate and touch the complete storage there, publish the ready bank at an audio segment boundary and retire it off the audio thread. Allocate during normal off-thread device preparation when restoring an already-enabled project. Keep an available bank through short bypasses, invalidate by counter when a fresh history is needed and release at teardown or on a control-thread cache eviction. This lets never-enabled instances use no Time history memory. If activation can arrive as audio-rate automation, prime a bank before the event where possible. While waiting, retain the exact dry contribution and let unavailable wet reads contribute zero. An implementation promising sample-exact cold automation needs a prewarmed bank available before the event.
- Virtual address reservation is optional. Windows supports a reserve-only region with no physical or paging-file allocation for the region. Commit and pre-touch pages off the audio thread before publication. POSIX platforms can create an inaccessible anonymous mapping and change its protection before touching it. Commit accounting and residency depend on the operating system. Neither initially reserving addresses nor allowing demand paging guarantees real-time first access. Windows virtual allocation · POSIX memory mapping · POSIX memory protection
- A portable bank can use two correctly sized float vectors or a vector of stereo frames created on the resource thread. C++ vector capacity growth has no virtual-memory or physical-residency contract. Only constructed elements below `size()` are valid sample storage. Move or publish the bank's owning object without copying its sample storage, and keep final destruction on the resource thread. C++ vector capacity · C++ vector data

## 2026-11-19 — ADI-Shaper Drive and Crush ready for review

**PR requested** — `clones/adi-shaper-drive-crush` at `a3891a36`. Title **Add oversampled Drive and fractional Crush to ADI-Shaper**. Review after `clones/adi-shaper-modulation`. The seven-stage rack preserves its first 286 parameters. New indices extend the editable Pd patch and its native processing path through 415 parameters. Drive reports 16 samples and keeps the dry signal aligned. Crush reports zero. The three Drive styles and independent Crush contours were tested at 44.1, 48 and 96 kHz. All eight focused MSVC Release tests passed twice with strict warnings, including Pd parameter routing, device registration and delay. The following measurements cover their implemented settings.

### ADI-Shaper Drive reference

Reference 3.6, stereo 32-bit floating-point processing, 256-frame callbacks. One full-range drive stage, neutral trims, no tone tilt or modulation smoothing, full wet unless stated. Let d run from zero to one, x denote the input and z denote the input after pre-gain. All transfer values below refer to the level before DC removal.

| Property | Measured behavior |
|---|---|
| Symmetric hard clipping | For positive drive, the pre-gain follows `(1+d)^8`. Limit z to [−1,1] and apply the drive-dependent output compensation. At d = 0.25, 0.50, 0.75, 1 the small-signal gains were approximately 1.69, 5.49, 16.59 and 44.2. The positive output plateaus were approximately 0.283, 0.214, 0.189 and 0.173. Negative plateaus have equal magnitude. |
| First saturator | Uses the same measured pre-gain. The positive arm follows `z / (1 + 2z/3)`. For negative z, set t = abs(z). The fitted arm is `−1 + 1/(1+t+t²+t³+t⁴+t⁵)`. A 69-level sweep gave approximately −5/6 at t = 1 and −62/63 at t = 2 before output compensation. At full drive, input 0.001, 0.01, 0.1 and 0.35 produced approximately 0.0377, 0.163, 0.245 and 0.255. |
| Hard rectification | Pre-gain follows `(1+d)^4`. The measured fold is consistent with `clamp(abs(z+1)−1, −1, 1)`, with its own output compensation. At full drive an input of −0.04 produced approximately −0.146. Inputs −0.35 and +0.35 both produced approximately +0.228. The fold passes through zero when z reaches −2. |
| Band blend | At a 400 Hz, 0.2-peak input and d = 0.5, the quarter, half and three-quarter blend waveforms matched the weighted sum of independent dry and full-wet renders to within 1e-6 across all three styles. |
| Trims | Input and output trim both gave unity at normalized position 0.5. At 0, 0.25, 0.5, 0.75 and 1, input gains were 0.25, 0.5, 1, 8 and 64. Output gains were 1/64, 1/8, 1, 2 and 4. A low-level sine determined the gain ratios. A separate 0.2-peak sine exposed the change in harmonic balance with input gain. Output trim scaled the complete wet waveform. |
| DC rejection | Stepped DC inputs decay toward zero even with the drive contour at minimum. Following a 0.1 input step, the zero-drive response after 255 and 1,024 samples at 48 kHz was about 0.0716 and 0.0262. Their ratio gives a corner near 10 Hz. Signed step heights during the first millisecond, calibrated against the minimum-drive response and checked against later decay, reveal the transfer curve. |
| Latency and impulse | The host reported 16 samples at 44.1, 48 and 96 kHz for all three styles. On a 0.125 impulse at 48 kHz, the compensated peak at full drive occurred at the input frame. The first saturator, hard clip and hard rectifier gave peak levels 0.33629, 0.25322 and 0.26107. Their last samples above 1e−6 absolute fell 4,392, 4,243 and 4,603 frames after the input. At minimum drive, the 48 kHz impulse had a central coefficient of 0.81545. Magnitudes at 12, 16.8, 19.2, 21.6 and 24 kHz were 1.0000, 0.8910, 0.5662, 0.1878 and 0.0454. At 96 kHz the Nyquist magnitude was 0.2758. |
| Alias readings | Full drive, 9,973 Hz sine at 0.35 peak, 48 kHz. For the first saturator, hard clip and hard rectifier respectively, fundamental amplitudes were 0.27065, 0.21883 and 0.01953. The 19,946 Hz harmonic was 0.004678, approximately 1e−6 and 0.06286. The folded third harmonic at 18,081 Hz was about 1.0e−5, 7e−6 and 5e−6. Absolute spectral amplitude uses a one-second settled render and coherent FFT bins. |

The manual attributes the Drive delay to oversampling and gives nominal input/output trim spans of −12 to +36 dB and −36 to +12 dB. A compensated delay of 16 samples can arise from different filter lengths and rate factors. Empirical passband, alias rejection, impulse alignment and transition behavior form independent audio targets. Reference manual

**Independent implementation.** Two symmetric, windowed-sinc filters with eight input frames of group delay each reproduce the measured passband and delay. Four substeps at 44.1/48 kHz and two at 96 kHz reproduce the tested spectra. The hard clip averages its transfer over the line segment between successive substep inputs. Applying that averaging to the other two curves changed the measured impulse shapes. The output compensation uses 33 independent low-level readings per curve family. The reference’s private filter coefficients and internal rate factors remain undetermined.

At 48 kHz, ADI-Shaper reproduced the tested alias amplitudes of the full-drive, 9 kHz hard clip within 0.000031 absolute. Across six independent 199 Hz sine tests at three input levels, the first, second, third and fifth harmonic amplitudes agreed within 0.000057. The largest difference in the signed impulse taps tested at 44.1, 48 and 96 kHz was 0.000292. The maximum landed at sample 16 in each case. These figures apply to neutral tone and accent, minimum push and grip, and the three measured styles.

### Crush reference contract

**Measurement.** Stereo floating-point input, zero bias and jitter, prefilter and dither off, treble at its maximum, fully wet single band. Static contours isolate the bit and resample controls. Controls below use normalized height 0–1. Native block size was 256 unless specified. Samples, frequencies and amplitudes below are acoustic results. The reference reports zero host latency.

**Bit resolution.** Ramps of 16,385 small ascending steps, held for four samples, identified first thresholds and output steps at 27 heights. For height `u < 1`, level spacing fits `1/L`, with `L = 1 + 14u + 32753u^32`. Levels are rounded to the nearest integer in the scaled domain. Settled output is `G × floor(Lx + 0.5) / L`, where `G = 1 − 0.25(1−u)^3`. At heights 0, .25, .5, .75 and .99 the measured level counts were 1, 4.5, 8.0, 14.79 and 23,760.16. For `u=1` the bit section passes full precision.

**Why a changing input obscures the staircase.** With dither off, a crossing sample is the area average of the staircase between consecutive input values. The following constant sample gives the settled level. For `k=floor(Lx+0.5)`, the primitive is `P(x)=(k L x − k²/2)/L²`. A transition from a to b returns `G × (P(b)−P(a))/(b−a)`. After inverting the output DC blocker, 21 multi-level changes at four heights matched this rule within 0.000007 on their transition samples. For height 0, changing +.05 to +.65 produces +.1875 then +.75. Changing +.65 to −.65 produces zero on the crossing sample, followed by −.75. The earlier response on a sine combined these inter-level values with high-pass decay.

**Hold clock.** At heights 0/.125/.25/.375/.5/.625/.75/.875/.99 the measured capture rates were 8.82/226.06/644.32/1449.60/3000.00/5985.01/11732.05/22796.85/41836.85 Hz. A parameterization fitting the measurements is `R = (44100−3000)/(3000−8.82)`, `f(r)=8.82 + (44100−8.82)(R^(2r)−1)/(R²−1)`. The same absolute rates held at 44.1, 48 and 96 kHz. Exactly 1 bypassed holding at native rate. Immediately below the top, the clock approached 44.1 kHz.

**Fractional timing.** On reset the held signal is zero. The first capture occurs one period after reset. A wrap partway through an audio sample captures the value interpolated between the preceding and current quantized inputs. The output for that sample is the time-weighted blend of old and new holds. At 48 kHz, height .25 placed the first event between sample indices 73 and 74. A half-amplitude impulse at 73 gave .126574 on output sample 74 then .251569 held; the same impulse at 74 gave .124995 then .248431, after compensating the DC blocker. The reference produced sample-identical results across 64/256/1024-frame host blocks and repeated resets. Independent noise showed that quantization precedes resampling. Over 4,000 samples the quantize-then-hold model had RMS error .00074; the reverse order had RMS error .252. A fitted clock can gradually accumulate fractional-phase error.

**Dry, wet and DC.** Either master mix or band mix at zero reproduced an impulse bit-for-bit. The fully wet active module at both maximum heights still applied a one-pole DC blocker. At 48 kHz the first three coefficients were .99934596, −.00130729, −.00130558. Estimated cutoff across 44.1/48/96 kHz was 10.0002/10.0000/9.9999 Hz. Half master mix at bit height .5, resample height 1 returned .742029 on the impulse sample and .241712 on the following sample.

**Dither.** At the lowest bit depth, with both input channels identical, a constant .25 input produced RMS .13546 after warmup with dither enabled, versus zero at steady state without dither. At .1/.49/.5 input, the enabled RMS was .02688/.29904/.29945. Silence remained exactly zero. Both channels matched sample-for-sample, and repeated reference resets reproduced the same output. A shared deterministic triangular perturbation bounded to ±half a quantization step, inserted before staircase integration, reproduces these statistics. The reference generator, seed and sequences have not been identified. At maximum bit height the dither switch left the impulse response unchanged.

**Scope of the first ADI kernel.** Independent Bits and Resample contours with the behavior above, signal-linked dither, zero bias/jitter, prefilter off and full treble. It allocates no storage dynamically and reports zero latency. Bias, jitter and prefilter controls and variable treble remain separate reference work. Reference manual, Crush module

### Reverb acceptance addendum

A reverb-only factory state produced a wet tail with its installed hall. Selecting the same hall from an initialized state reproduced the result. The control used 48 kHz, 256-frame blocks, one full band, full send and return, fully wet mix, zero pre-delay and disabled latency reservation. Pending load events ran before excitation. Results reproduced after two and four seconds of preparation. The blank IR returned exact silence at full wet. These tests used an independently generated 0.5-amplitude impulse.

| Control | Measured behavior |
| --- | --- |
| Latency and stereo | The host reported 0 frames at 44.1, 48 and 96 kHz. At 48 kHz, the first hall sample above threshold was at offset 1. A left-only impulse peaked at 0.033311 on the left and zero on the right. A right-only impulse peaked at 0.033566 on the right and zero on the left. Centered and opposite-sign stereo impulses preserved the corresponding channel magnitudes. These figures describe this IR at the default width. |
| Send and return | With transport running, flat curve heights 0, 0.25, 0.5, 0.75 and 1 multiplied the wet amplitude by those same amounts. Each control was varied independently with the other at unity. The measured error in relative gain was below 3e−8. A stopped-transport render held unity for the flat-curve changes in this fixture. |
| Tail closure | A single impulse at 85.33 ms preceded a zero-height curve interval from 250 to 375 ms. Closing Send preserved the existing tail throughout. Closing Return silenced the interval. Upon reopening, the remaining tail matched the uninterrupted reference with Clear Tails off and stayed silent with Clear Tails on. The test used adjacent hard points with smoothing off. |
| Mix | At normalized mix 0, 0.25, 0.5, 0.75 and 1, the measured dry/wet amplitude pairs were 1/0, 1/0.5, 1/1, 0.5/1 and 0/1. The measured law is dry = min(1, 2 − 2u), wet = min(1, 2u). The initial mix was 0.25. |
| Pre-delay | Normalized positions from 0 through 0.5 in sixteenths follow 600u² ms within one sample. Positions 0.125, 0.25 and 0.5 added precisely 450, 1,800 and 7,200 frames at 48 kHz. The upper range uses tempo steps. At 120 BPM, 9/16–10/16 gave 125 ms, 11/16–12/16 gave 250 ms, 13/16–14/16 gave 375 ms and 15/16–1 gave 500 ms at the sampled positions. At 90 BPM those same positions gave 166.667, 333.333, 500 and 666.667 ms, respectively, while the free-time points stayed unchanged. The four durations correspond to 0.25, 0.5, 0.75 and 1 quarter note. Exact switch boundaries between the sampled positions remain open. Five compensated impulses matched the undelayed response within 2.1e−7 relative RMS. Initial position was zero. |
| Width | The mid component stays at unity and the side component is multiplied by 2u. Normalized positions 0, 0.25, 0.5, 0.75 and 1 gave side gains 0, 0.5, 1, 1.5 and 2. The width midpoint is neutral and was the initial value. |
| Decay | With the gate off, normalized values 0, 0.25, 0.5, 0.75 and 1 placed 90% of this hall's left-channel energy within 13.52, 93.62, 159.79, 163.85 and 163.88 ms. These are integrated-energy times for this IR. A 1 ms-window amplitude comparison with the full-decay reference crossed 50% near 14, 146, 837 and 2,046 ms for the first four values. The initial decay was 1. |
| Gate | The impulse-envelope fade midpoint occurred at about 100, 550, 1,000, 1,450 and 1,900 ms for decay settings 0, 0.25, 0.5, 0.75 and 1. At decay 0 the measured envelope had gain about 0.84 at 50 ms, 0.49 at 100 ms and 0.15 at 150 ms. The low settings fit a roughly 200 ms fade with its start delayed by 1,800u ms. The initial gate and Clear Tails switches were off. |
| Size | At normalized positions 0, 0.25, 0.5, 0.75 and 1, measured 90%-energy times were 81.94, 115.88, 163.88, 229.10 and 321.48 ms. The observed durations approximately follow 0.5×, 0.707×, 1×, 1.414× and 2× the midpoint duration. The manual describes IR resampling. The initial size was 0.5. |

**Custom IR acceptance remains open.** A self-authored stereo PCM WAV used two taps 1,536 frames apart with distinct channel gains and opposite sign on the second right-channel tap. Placing it in the documented user library and trying ten stored selection spellings produced silence. The working installed hall served as a positive control in the same run. The transient fixture was removed. If exact custom import compatibility is required, the remaining input is an editor-exported fully wet state paired with the self-authored WAV chosen from the User column. The factory wet, clock, width and mix measurements above are independently reproducible. WAV import, the User column, IR resampling and the hard-point tail-clear behavior are documented in the reference manual, pp. 19–20.

## 2026-11-19 — Five-stage CPU and stereo music verification

At `d18b7d11`, three-band Volume on the stereo kick-and-bass program used 81.3%, 61.9% and 51.1% fewer processing-thread cycles than reference shaper 3.6 at blocks of 64, 256 and 1,024 frames. At 256 frames the median callback duration was 14.9 µs for ADI-Shaper and 45.8 µs for the reference. Both processed the same 48 kHz source with 0.35/0.70/0.85 band gains, 659.255 Hz at 12 dB/octave and 3,729.310 Hz at 24 dB/octave. The maximum absolute stereo sample difference was 0.000000477 across 192,000 frames per channel at each block size; there was no time shift.

There were 18 matched CPU cells in all: full-range Volume, Width, their combination, a clean low-pass, a flanger and the three-band music case, each at three block sizes. The five-stage device used 45.8–95.4% fewer cycles in these cells. The smallest advantage was the flanger at 1,024 frames. State loading and file I/O preceded timing, input replenishment was matched, and the individual callback wall intervals excluded input preparation. Nine trials of 8,192 callbacks ran per cell with both editors closed. The multiband music case provides the signal check for nonconstant stereo input. These results apply to the named selections.

## 2026-11-19 — Drive and Crush acoustic acceptance

The Crush measurement used 27 static bit heights, held stereo levels, steps and rate changes across three sample rates. With dither and bias disabled and resampling bypassed, measured plateau spacing and amplitude follow these laws for bit height `u < 1`: level density `L = 1 + 14u + 32753u^32`, output gain `G = 1 - 0.25(1-u)^3`, thresholds `(k+0.5)/L` and step size `G/L`. At the top the quantizer passes full resolution. The decay on a held output at 48 kHz is 0.99869187 per sample, consistent with a 10 Hz DC blocker after quantization. Sampling before this decay is essential to measuring the steps.

The measured sample-hold rate at the midpoint is 3 kHz. Across the control it follows `8.82 + (44100-8.82)(R^(2r)-1)/(R²-1)` Hz, where `R=(44100-3000)/(3000-8.82)`. At its exact maximum the rate reducer bypasses, including at 96 kHz. At master or band blend zero the reference returned the original input bits. Dither at its measured settings preserved complete silence and produced matching channels for identical stereo input.

## 2026-11-19 — ADI-Shaper Filter, flanger and phaser

**PR requested** — `clones/adi-shaper-modulation` at `d18b7d11`. Title **Add measured multiband Filter and flanger/phaser stages to ADI-Shaper**. Review after `clones/adi-shaper` (`295056b6`). Win opens and lands the PR.

Two optional stages extend the rack to five. Each band has a primary and secondary eight-point contour, blend, clock and mode. Five clean filters cover 6 and 12 dB/octave low-pass and high-pass responses and 12 dB/octave band-pass. Sweep includes a flanger and 4-, 8-, 12- or 16-stage phasers, with feedback polarity per band. All 133 earlier parameter IDs retain their meanings. Older stored states open with both new stages disabled.

The clean low-pass matched the six original response gates at 400 Hz and 8 kHz. Five-mode held-out stereo impulse error was below 0.000008 at the tested small-signal levels. The moving cutoff/resonance fixture differed by up to 0.000037. At extreme resonance, a half-amplitude impulse through a gentle filter showed deviations up to 0.01655. Across fifteen flanger center/feedback fixtures the first 5,000 normalized impulse samples differed by less than 0.000075. The highest tested phaser feedback retained a 0.0222 normalized tail discrepancy. These acoustic differences remain reviewable.

Six Release MSVC /W4 /WX suites passed, including genuine editable Pd versus native Filter and Sweep processing across irregular segments. Coverage includes stored state, cancellation of disabled modules, independent bands, sample-rate changes and modulation. The device and UI handoffs remain in their respective owner lanes.

## 2026-11-19 — Drive and Crush measurement reservation

On `clones/adi-shaper-drive-crush` the clones seat takes `src/adi/dsp/adi_shaper_drive.*`, `src/adi/dsp/adi_shaper_crush.*`, their dedicated tests, and the same ADI-Shaper canvas, compiled-in external and test module. Existing device-stage IDs remain stable. Additional level, alias, hold-rate and latency measurements precede the audio implementation.

In measured hard clipping, pre-clip gain follows `(1 + height)^8` across nine heights. The hard rectifier follows `(1 + height)^4`; its central transfer follows `clamp(abs(z+1)-1,-1,1)`. Measurements used signed level steps and independent tones. A DC-coupled transfer read directly from a settled constant input would be misleading because of the observed low-frequency rolloff. These are transfer targets for the measured modes only. The distinct minimum/maximum output, mixture and timing fixtures are in the eleven-stage contract.

## 2026-11-19 — Device curve drawing handoff to Window

The device strip belongs to Window. ADI-Shaper exposes every curve point to the ordinary parameter model; its parameter count exceeds the automatic 64-control display threshold. For an ADI-drawn editor in the strip, scope it to the internal UID `org.adidaw.builtin.adi-shaper` and the live device identity. The existing ADI’s cabinet sample tile is the integration precedent. Keep the existing Configure and fold operations. The Mix View function and EQ page controls have their own owner.

The editor selects a stage, band and contour. The eight evenly spaced horizontal positions are fixed; each draggable height uses its own persisted parameter. For the middle band, the Volume heights are IDs 23–30, Width 65–72 and Pan 107–114. On the modulation branch Filter cutoff uses 170–177 and resonance 178–185. Sweep center uses 245–252 and feedback 253–260. Plot the segment from the last point back to the first for the next cycle. Draw a held or linear segment according to the band's curve setting. Pan at the bottom favors right and at the top favors left.

Use the existing parameter Begin/Value/End gesture protocol and one undoable gesture per drag. A project rebind cancels the capture associated with the old instance. Provide discrete keyboard editing with an accessible name for each point. A software-rendered device strip test can verify the drawn points, focus order, undo, and stale-callback behavior. The clones seat has made no strip or Mix View changes.

## 2026-11-19 — ADI-Shaper CPU and signal check on 295056b6

The registered device and the reference shaper 3.6 both processed the same stereo input, at 48 kHz and 120 BPM, with a single instance, closed editors and zero reported latency. The full-range cases were the same one-bar hard Volume gate, Width at mono, and both stages in series. Both renderers restored identical stereo input before each call. Nine trials of 8,192 callbacks per cell used the same affinity; fixture order was shuffled and processing order alternated.

| Frames | Gate, ADi / reference µs | Width, ADi / reference µs | Gate + Width, ADi / reference µs |
|---:|---:|---:|---:|
| 64 | 1.1 / 24.0 | 1.1 / 21.6 | 1.3 / 27.2 |
| 256 | 1.8 / 34.5 | 2.0 / 30.0 | 2.9 / 37.8 |
| 1024 | 4.8 / 85.4 | 5.4 / 67.4 | 8.8 / 93.3 |

Values are medians of per-trial median callback wall times. Median processing-thread cycles per callback fell by 90.1–95.3% in the nine cells. Each corresponding stereo capture covered 192,000 frames per channel. Width to mono matched directly; every paired gate edge in ADi was 16 samples earlier than the reference. After compensating that constant edge displacement the captures agreed to 0.00001. These CPU measurements cover the three settings above. Multiband phase and pan have separate acoustic fixtures.

## 2026-11-19 — ADI-Shaper Volume, Width and Pan

**PR requested** — `clones/adi-shaper` at `295056b6`. Title **Add ADI-Shaper multiband Volume, Width and Pan to the Pd tier**. Base `main`. Win opens and lands the PR.

The catalog now exposes ADI-Shaper. Each of its three serial stages has a full-range path and independent low/mid/high clocks, blend and eight editable curve heights. Beat and Hz clocks support held and linear segments. Each crossover independently selects 6, 12 or 24 dB/octave. The neutral full-range device preserves the input bits and reports zero latency.

The compiled pan law matched both channels at 32 held-out heights within 0.0000002 gain. The largest sample error measured in 20 isolated-band and all-band impulse comparisons was 0.000000382. The reference checks cover five combinations of crossover slopes at 48 kHz. The registered device uses exact musical positions across graph segments, loops, seeks and meter changes. See the editable-Pd clock handoff below for the current host adapter's different timing contract.

The Release MSVC build used /W4 /WX. All four headless suites passed, covering DSP, registered-device state and transport, editable Pd versus native audio, and catalog/host integration. This piece implements Volume, Width and Pan; the eleven-stage measurement contract is below.

## 2026-11-19 — ADI-Shaper next device increment

On `clones/adi-shaper-modulation` the clones seat takes the shaper's filter and short-delay modulation DSP, `src/adi/dsp/adi_shaper_filter.*`, `src/adi/dsp/adi_shaper_sweep.*`, their dedicated tests and the same ADI-Shaper device files already reserved. The measured targets are the six low-pass amplitudes at 400 Hz/8 kHz, the modulated flanger delay and the four all-pass stage counts. These additions use the shared per-band curves and crossover path. The first Volume/Width/Pan branch remains independently reviewable.

## 2026-11-19 — Editable Pd timing cases for the devices seat

At 48 kHz and 120 BPM, set a four-quarter-note held Volume curve with point 1 muted and point 3 open. In 2/2, QN 0 and QN 1 both produce `(1,120,2,2,1,1,0)` on the seven-field Pd transport. A patch reconstructing the integer quarter from these values replays point 1 at QN 1. After a full 4/4 bar followed by 3/4, the start of bar 2 is QN 4. Reconstruction from the current meter gives QN 3.

For a sub-block case, use a 1/32-quarter cycle with only point 1 muted. Start playback at sample 64 and split a 128-frame block after 64 frames. The expected open transition is at timeline sample 94. A second delivery of the block-start position at sample 128 repeats the first 30 muted frames. Compare after compensating the editable patch's reported adapter latency.

The catalogued ADI-Shaper requests the graph's exact position per segment. For editable patches, the devices seat owns the pending quantum/MIDI adapter work. An optional exact musical position anchored to the first input sample of each Pd tick would preserve existing patch bindings. Include upstream latency and a partial tick across a seek or stop in the test. The native device's regression covers QN 0/1, the meter change, and writing a segment at a nonzero buffer offset.

## 2026-11-19 — ADI-Shaper reference contract

**Target.** The eleven sound-shaping modules in the reference shaper 3.6, each with one to three bands, editable modulation and a reorderable serial signal path. Compressor and Oscilloscope are additional rack tools. ADI-Shaper uses ADI controls, drawing, names for original sounds and independently generated audio assets.

**Method.** Measurements below use a reference instance at 48,000 Hz, stereo float, 256-frame blocks unless specified, full band and full blend unless specified. The reference editor stayed closed. Constant shapes were set by placing two equal-height points at the ends of a cycle. Let `u` be vertical graph position, from 0 at the bottom to 1 at the top. Input fixtures were stereo DC (0.375 left, 0.25 right), a 400 Hz sine (0.35 and 0.25 peak) and a single 0.5-amplitude impulse on both channels at frame 10,000. Measurements give acoustic output or host-reported latency as indicated. Control descriptions and ranges identified as documented come from the manual supplied with 3.6 and the published 3.6.3 manual. The behaviors below are acceptance criteria for the settings actually tested. Reference manual

#### Shared signal path

| Behavior | Observation and acceptance criterion |
|---|---|
| Stage selection | The initial instance reported all eleven stages off and master blend 1. Stages have distinct modulation and split settings. Stage order is serial. The user can move a stage in the chain. |
| Blend | A muted full-band Volume stage with band blend 0.5 and master blend 0.25, 0.5, 0.75 returned respectively 0.875, 0.75, 0.625 times input. The two controls multiply before the band output is calculated. With an active 24 dB/octave split or two active 24 dB/octave splits, zero master or zero band blend retained the crossover phase exactly. Reverb has independent send and return levels. Pan's delay component mixes delayed and direct audio. |
| Split frequency | Start with the middle band spanning the audio range. Move either boundary inward to enable the respective outer band. For the low split at normalized positions 0.25, 0.50, 0.75, the impulse response put the nominal crossover near 116.3, 658.5 and 3,725 Hz. Each quarter-turn multiplies the frequency by approximately 5.66. Extrapolation gives `20.58 × 2^(10x)` Hz for interior positions. The endpoint switches must independently disable their respective outer bands. |
| Crossover magnitude | Isolate the low band by setting its Volume curve to unity and the other bands to mute. At the midpoint split, measured low-band corners were −3 dB near 658 Hz at 6 dB/octave and −6 dB near 658 Hz at 12 and 24 dB/octave. At 1.2/2.4 kHz, low-path attenuation was 6.36/11.60 dB, 12.72/23.20 dB and 21.61/45.21 dB respectively. The initial slope setting was 6 dB/octave. |
| Crossover phase | Compare a full-wet unity Volume stage with one split at midpoint to the input impulse. At 6 dB/octave the sum reproduced the input sample-for-sample. At 12 and 24 dB/octave it had unity magnitude within 0.00001 at 160, 658 and 5,000 Hz but maximum sample differences of 0.145564 and 0.107886 from the input. Adding isolated low and high renders reproduced the corresponding full output within 1e-7 for all three slopes. This gives separate tests for reconstruction and phase. |
| Three-band routing | Set the boundaries to 0.50 and 0.75 and render each isolated band, then all bands at unity. Repeat with slope pairs 12/12, 24/24, 6/24, 24/6 and 12/24 dB/octave. Write L, H and A for the measured low, high and summed transfer functions of each crossover in isolation, with subscripts 1 and 2 for the lower and upper boundary. The three output bands matched L₁A₂, H₁L₂ and H₁H₂ respectively. The combined response matched A₁A₂. The largest unit-impulse errors across the five settings were 2e-8, 3e-8, 1.1e-7 and 1e-7 respectively. Applying the upper crossover’s phase shift to the low band is essential for this result. |
| LFO | Documented beat sync, free frequency and MIDI-note frequency. Beat period spans 1/128 to 32 bars, with straight, dotted and triplet values. Frequency spans 0.02–5,240 Hz. MIDI pitch offset spans ±3 octaves. The starting clock is project sync. Triggering from notes or transients can restart a loop or play one cycle and hold the end value. The detector has internal/external inputs, filtering, sensitivity, threshold and a ±4 ms timing shift. Each band has its own clock with optional linking. |
| Wave editing and follower | On a one-bar Volume triangle with all corners hard, minimum/maximum/minimum at start/midpoint/end, the second cycle’s rising segment returned gains 0.12533, 0.25033, 0.37533, 0.50033, 0.62533, 0.75033 and 0.87533 when sampled 16 frames past each eighth of that segment. With the roundest of the three corner settings at the same positions, gains were 0.12563, 0.24930, 0.36403, 0.46430, 0.54747, 0.61103 and 0.65203. The documented editor has time/value snapping, line/arc/S drawing, separate point weights and output smoothing. A follower can add dynamics-dependent modulation with attack, hold, release, depth, offset and filtered internal or external detection. Burst response, adaptive release and the mapping from smooth-control value to milliseconds remain open. |

#### Individual shapers

| Shaper | Documented controls and measured behavior |
|---|---|
| Volume | On DC, `u = 0, .25, .5, .75, 1` returned gains exactly equal to `u`. The graph midpoint is −6.0206 dB. With the curve at unity the five corresponding output-trim positions returned factors 1/16, 1/4, 1, 4 and 16, consistent with `2^(8t−4)` for normalized trim t. The one-bar hard gate at 120 BPM in 4/4 had a period of 96,000 samples and a rising edge at frame 48,016 in the 64-frame reference capture, with the first half muted. The same test at 256 and 1,024 frames yielded the same plateaus. |
| Width | 0–200% width. The five curve heights gave side gains 0, 0.5, 1, 1.5 and 2. For `M = (L+R)/2`, `S = (L−R)/2`, output was `(M + 2uS, M − 2uS)`. The mono result was 0.3125 on both channels at 64, 256 and 1,024 frames. The five output-trim positions gave factors 0.25, 0.5, 1, 2 and 4, consistent with `2^(4t−2)`. |
| Pan | Documented balance boosts are 0, 3, 4.5 or 6 dB, with a separate combined-input mode and a level/delay blend. Maximum interchannel delay spans 0–40 ms, initially 2 ms. For the default balance law, set r = √2 and `g(u) = u × ((8−4r) + u × ((11r−16) + u × (8−6r)))`. Left and right gains `g(u), g(1−u)` fit 33 equally spaced DC readings and agreed within 9.7e-8 on both channels at 32 interleaved validation positions. At u = 1/8 and 3/8 the gain pairs were (0.285013, 1.385482) and (0.790700, 1.172688). |
| Time | The curve addresses a delay into recent input. With running transport, one bar at 120 BPM and direct step mode, a marked input sample emerged 96,000/48,000/0 frames later at bottom/middle/top. The documented default vertical range follows the LFO length. Fixed alternatives are 1, 2, 4 or 8 bars and three short ranges labeled 2.5, 10 and 20 ms. In the same marker test the three short ranges produced bottom offsets of 239, 959 and 1,919 frames, middle offsets of 119, 479 and 959, and zero at the top. The relation between those labels and the exported curve values needs visual verification before adopting a short-range mapping. For a delay `d(t)` in seconds, read position is `t − d(t)`. An increase of one second per second holds the read head still. A steeper increase reads in reverse. Steps can jump directly or crossfade. |
| Pitch | Transposition ±24 semitones, giving a target frequency ratio of `2^(semitones/12)`. A 400 Hz sine in the default polyphonic mode measured 100, 400 and 1,600 Hz at bottom, middle and top. Four processing modes cover mixed input, voice, drums and cyclic grains. The first offers three grain lengths, the last a continuous grain setting. Voice and drum modes can shift formants independently. At zero transposition, the polyphonic render matched its input after host compensation. The cyclic mode’s zero-transposition impulse peaked 625 samples after the input while reporting zero latency in this fixture. |
| Drive | Twelve nonlinear transfer choices span clipping, square-like transfer, rectification and folding. Input trim spans −12 to +36 dB, output −36 to +12 dB. Drive, low-level attenuation, asymmetry, tone and level compensation affect different aspects of the transfer. At full drive on a 0.35-peak 400 Hz sine, the first clipping style yielded fundamental/second/third amplitudes 0.2706/0.0071/0.0877. The hard-clip selection yielded 0.2197/<0.0001/0.0732. One rectifier yielded 0.0214/0.1037/0.0554. These are single-level harmonic checks. |
| Filter | Independent cutoff and resonance contours, clean and saturating responses, low/high/band-pass, notch and peak families. Nominal cutoff 20.6 Hz–21.1 kHz, opposing stereo offsets up to 60 semitones per channel, and 6/12/24 dB/octave variants. With the first low-pass type and minimum resonance, 400 Hz gain at bottom/middle/top cutoff was 0.05143/0.85489/0.99993. At 8 kHz it was 0.00243/0.07768/0.97447. Obtain these by transforming the output of an isolated impulse and dividing by the input impulse amplitude. Saturating feedback and resonance limiting require independent drive sweeps. |
| Sweep | The flanger mixes a short delay with the input. The phaser mixes an all-pass cascade with input. Center and feedback have separate contours. Feedback polarity changes peak/notch placement. Phaser choices provide 4, 8, 12 or 16 all-pass stages. Stereo center offset reaches ±60 semitones. An off-mode impulse matched exactly. In the flanger, center at bottom/middle/top and feedback contour at center put the largest impulse coefficient 2,330/73/2 samples after input, respectively. The corresponding phaser peak positions were 0/1/3. |
| Crush | Independent bit-depth and sample-hold contours, DC bias, dither, hold jitter, pre-filter and treble. Documented ranges are 1–16 bits with full-resolution pass-through at unmodulated maximum, and 8.82 Hz–44.1 kHz with a full-host-rate pass-through setting. At minimum bit depth with dither and bias disabled, a 0.35-peak 400 Hz sine produced silence. The output did not expose simple uniform steps at the tested intermediate settings. Quantizer thresholds, hold phase, dither distribution and the exact transition into bypass remain open. |
| Noise | Adds a noise source with independent level and pitch contours, follow/duck dynamics and high/low filtering. Documented level spans silence–unity, trim −48 to +12 dB, pitch ±24 semitones and initial high-pass 120 Hz. Filtering the detector band changes what triggers the noise independently of the noise spectrum. In a three-second white-noise-only render with the follower off and mono selected, bottom/mid/top contour levels gave 0/0.01113/0.02221 RMS. The two output channels matched sample-for-sample. An empty-source stage passed the test impulse exactly. |
| Reverb | Convolves incoming audio with an impulse response. Separate send and return contours let a closed send leave a tail playing. Controls include pre-delay, decay or gated duration, impulse resampling, stereo wet width 0–200% and optional tail clear at a zero-level hard point. With a 0.4-peak sine burst and three seconds of subsequent observation, normalized blend 0.25, 0.50, 0.75 and 1.00 returned dry peaks of 0.4, 0.4, 0.2 and 0. In this run the selected hall returned no measurable wet tail. Its convolution timing, send/return law, gate and tail-clear behavior therefore remain acceptance work. An independently authored two-tap impulse can distinguish send closure from return closure. |

#### Latency and oversampling

With beat triggering and reserve latency disabled, the host reported 0 samples for Volume, Pan, Width, Time, Filter, Sweep, Crush, Noise and Reverb. Drive reported 16 samples at 48 kHz. Pitch reported 1,687, 2,381, 2,861 and 0 samples for the polyphonic, voice, drum and cyclic modes. These readings describe what the host can compensate. Delay-line effects and intentional signal displacement need separate impulse tests.

The published manual attributes Drive latency to oversampling. At maximum hard clip, a 9 kHz input sine with 0.35 peak produced a 9 kHz output of 0.21915 at 48 kHz. Spectral amplitudes at 3, 15 and 21 kHz were 0.000183, 0.001001 and 0.000655. The reported Drive latency remained 16 samples at 44.1, 48 and 96 kHz. The oversampling factor, filter response and any internal oversampling in other modules remain undetermined. With one Volume stage at 48 kHz, beat sync reported zero with either trigger-smoothing setting. MIDI triggering reported zero with smoothing off and 289 samples with it on. With audio triggering engaged, the first two detector algorithms reported 672 samples, and the complex detector reported 1,152. With smoothing enabled those readings increased to 961 and 1,441. The documented approximations are 14/24 ms plus about 6 ms for trigger smoothing. Reference manual, latency and lookahead

For ADI-Shaper CPU comparisons, match stage, band, curve, sample rate, block size and input, and record the latency of each device. The earlier full-band Volume, Width and two-stage measurements describe a separate prototype. CPU results for the new device belong with its build.

Remaining numerical ranges include independently shifted formants and the continuous grain window, Drive’s control-to-gain transfer, Filter’s resonance/Q relationship, Sweep’s feedback coefficient, and Reverb pre-delay, decay and impulse stretch. The short Time scales, follower time constants, free-frequency control taper, wet impulse response and internal oversampling factors require further readings. Reference control descriptions are in the module chapters on pp. 10–28 and the clock and trigger chapters on pp. 41–46. Reference manual

### Two-track Focus measurement addendum

**Summed waveform acceptance fixture.** A separate 512-column stereo reducer formed A+B at native scale before taking each column’s extrema. Six musical windows matched an independently binned sample oracle to the last bit. Advancing a 137-sample-late duplicate preserved a ±1.60 sum without clipping, and selecting opposite polarity cancelled it to zero. At 1,024 frames per update and 30 updates/s, the standalone sum-and-bin operation used median **0.0153% of one core** (nine runs of 512 s represented audio, range 0.0092–0.0183%). This cost is separate from the broker benchmark and excludes drawing. The integrated UI remains with the Mix View seat.

## 19 November 2026 — Two-track Focus handoff to the Mix View seat

**Review base.** In `ead20df4`, Scope already draws each member’s unwindowed L/R extrema on an audible, common time axis. A level readout computes actual stereo sum peak and RMS. The paint loop draws member paths only. The outstanding visible features are the summed waveform and a non-destructive timing preview. Mix View owns these changes. Its existing director question concerns committing a timing edit. Preview can proceed independently.

| Reference display | What it measures, controls and scale | Evidence |
| --- | --- | --- |
| alignment scope 2 waveform / stack / sum | Beat grid, loop, previous-cycle context, independent pre/post-roll, Freeze, horizontal/vertical zoom, L/R or M/S split or overlaid. Four Full-edition stack groups, switchable sum and spectral collision view. The sum represents simultaneous sources. Shared spectral energy identifies frequency coincidence without measuring cancellation. | The edition and display rules come from the alignment scope manual. A newly instantiated 2.0.5 serialized state had a 1/16 grid and alignment grid, source compensation 0 ms, Freeze on stop on, pre/post context off, auto scale off, four sums and four collision switches off. These are observed startup settings. Full entitlement and pixel-level response were unverified. |
| alignment scope 2 frequency, stereo and loudness | Waveform focus cutoffs affect analysis. Spectrogram and spectrum can use stereo, L, R, M or S. Adjustable spectral tilt/pivot, frequency or piano scales, stereo correlation/goniometer and LUFS history. | Same primary specification and instance state. Observed 1,000 Hz analyzer pivot, tilt setting 3, spectrogram floor −40, 3 active analysis bands with stored 90/45/20 ms windows and 20/15/10 ms hops. Pixel-hop mode was selected, 5 px for each band, so a fixed elapsed hop cannot be inferred. The short-term LUFS switch was on, momentary and integrated switches off. UI cutoff limits, gain normalization, display refresh and filter/correlation impulse responses remain uncalibrated. |
| layer scope waveform, scale and timing | Individual, overlaid layers, separate stack and arithmetic sum. A sum can sit behind layers or above stack. Beat sync uses the host tempo, custom beats set a cycle, Hz uses reciprocal period. Roll shifts origin, Scroll changes presentation. L/R/M/S and joint LR/MS have separate arrangements. Height scale resets to 1.00; peak fit changes display gain. Per-source and global visual offsets start at 0 samples and combine. Wheel changes 10 samples, fine step 1; numerical entry accepts samples or ms. Region readout gives sample/time endpoints, duration, reciprocal frequency and derived note. Its dB value is the cursor height. | The layer scope manual covers display modes, timing and channels and measurement definitions. |
| layer scope phase, difference, spectrum and filter | In fixed view, Layers/Stack correlate the first two enabled sounds’ mono components in slot order, locally in time. Individual and Sum correlate their own L/R. Delta feeds the mono A−B difference to the FFT. FFT size, window, bounds, decay and octave smoothing affect spectral presentation. The visual filter offers LP/HP/BP/AP and dual-edge band isolation with IIR, delay-estimated IIR or linear-phase FIR; the response graph shows delay in samples. AP retains its phase response. Filter audition uses IIR on individual sounds. | The layer scope manual covers phase, delta, spectrum, filter and stereo meters. The local coefficient window, averaging and exact display bounds/defaults were not independently calibrated. |

**Reference audio path.** Default-state stereo VST3 pass-through measured 204,800 frames/channel, float32, 48 kHz, blocks of 256, with editors closed. The input included equal/opposite channels, displaced impulses and silence. alignment scope 2.0.5 and layer scope 1.5.03 both reported zero latency and returned zero peak and RMS error on both channels. This test establishes the audio path at those settings.

**ADI display and preview contract.** Start with two named post-fader sources, unity display scale, common freeze and preview off. Use the intersection of finite, advancing windows with one sample rate and audible transport revision. A seek, source change, or ambiguous loop history invalidates the comparison. Each horizontal bin contains min/max of the samples it covers. Sum L and R sample-by-sample in unclipped floating point *before* binning; use a common gain for A, B and sum and indicate over-range. Level readouts use `20 log10(amplitude)`. Project to `(L+R)/2` for signed track correlation. A positive delay of d means B arrives d samples late; preview B advanced by d and calculate its sum over the remaining common interval. Preserve the unshifted trace, expose polarity as its own control and label the offset in samples and milliseconds. Weak, silent, steady-periodic, search-edge or unstable multi-window candidates keep their measured levels and disable the automatic shift. Preview changes neither track delay nor audio. Release hidden view subscriptions. **Acceptance still due in the owner’s integrated UI:** summed min/max trace including above-unity sums, frozen A/B and preview, seek/repeat-loop invalidation and end-to-end visible CPU.

**Kick and bass.** Two eight-second stereo stems at 48 kHz produced L/R sum peaks of −2.3323/−2.3930 dBFS and RMS of −14.4892/−14.4852 dBFS. Every 4,096-frame batch and the final partial batch matched an independent sample-by-sample stereo oracle exactly for peak and RMS. Six 4,096-frame mid projections starting at 0, 2,048, 6,000, 12,000, 24,000 and 48,000 samples gave three weak and three ambiguous timing estimates. A seeded broadband duplicate 137 samples late gave +1 correlation; an inverted duplicate gave −1 at the same lag. Advancing the copy 2.854 ms recovered the original over the common interval, with polarity corrected separately in the inverted case.

**Reproducible defect.** The signed-only `bestOffset` returned −960 for a pair of 4,096 zero samples with a ±960 search. It returned −800 samples, r = 1, for identical 60 Hz waveforms repeating every 800 samples. The headless preview prototype withheld both. It compares centered, fixed-length intervals for both signs with at least 64 samples, AC RMS ≥ −120 dBFS, |r| ≥ .70 and .05 separation from the next distinct peak. A search-edge winner is ineligible. Treat these as ADI trial thresholds. Verify sign and repeatability across regions before a project edit.

**CPU boundary.** Serial x64 Release strict-warning build; two 256-frame taps, default 1,024-frame Scope windows, 256-frame hop, 30 updates/s and 512 columns for each of the four traces. Nine pairs alternated baseline and active order after warming; each pass covered 96 s of source audio. Baseline mixed the same stems. Enabling tap writes, shared analysis in manual-pump mode, the actual Focus sum calculation and flat-buffer extrema added median **1.774% of one core**, range **1.595–1.888%**, by measured thread CPU / audio duration. Graphics paths, rasterization, scheduling, and the proposed summed trace are outside this measurement. Separately a single fixed 4,096-frame ±960 search on the preview prototype used mean **8.33 ms thread CPU**, median **9.80 ms elapsed**, across 15 warm runs. Compute on changed/frozen regions off the audio thread.

## 2026-11-19 — ADI-Shaper clock integration

The device requests segment positions from the existing host clock. Additional file claims are the device-specific transport opt-in in `src/juce/builtin_host.cpp` and the device's transport conversion in `src/adi/pd_builtins/adi_shaper_transport.hpp`. The graph and the Mix View remain with their owners. A full-band modulation sweep at 0, 25, 50, 75 and 100% showed unity-scaled gain and twice that scale for stereo side gain. Mid remained constant in the width sweep. A mute at half master and half band mix measured 0.75 times the dry input. This product-of-mixes law applies at each active stage.

## 2026-11-19 — ADI-Shaper measurement contract and file reservation

For the devices seat, the clones lane takes the ADI-Shaper core under `src/adi/dsp/adi_shaper.*`, the compiled-in external `src/adi/pd_builtins/adi_shaper_tilde.cpp`, the canvas `pd/devices/ADI-Shaper.pd`, device module `cmake/devices/adi_shaper.cmake` and dedicated shaper tests on `clones/adi-shaper`. Shared edits are the single device's embedding in `cmake/devices/pd_native.cmake`, native factory in `src/adi/pd_builtins/native_devices.cpp`, catalog in `src/adi/builtin_catalog.cpp`, and registration/fixtures in `tests/test_builtin_devices.cpp` and `tests/test_pd_native_devices.cpp`. The first implementation target is the measured gain, stereo width and pan behavior, a clocked editable curve and independent band settings. Additional module measurements follow.

For the Mix View seat, Focus implementation remains yours, including waveform rendering, the sum and the alignment preview. The clones lane is measuring two-track kick and bass and preparing a written handoff here. Preview and Apply are independent. The Apply and audition decisions are already with the director in `mixview/log`.

### ADI-Shaper first acceptance cases

Reference version 3.6, stereo float at 48 kHz, 120 BPM, 4/4. Use a full-band stage at full mix, zero output trim and no trigger smoothing.

| Setting and input | Measured output |
|---|---|
| One-bar Volume gate, first half at minimum, second at maximum, input L = 0.375 and R = 0.25 | Alternates between silence and the original stereo input. Period 96,000 samples. The observed hard edge was approximately 16 samples after the nominal boundary. Reported latency was zero. |
| Width at minimum on the same input | L = R = 0.3125. This constant render was identical at 64, 256 and 1024 frames per callback. Reported latency was zero. |
| Pan at the hard left and right positions on the same input, reference balance setting | Extremes preserve the corresponding input channel with a gain of √2 and silence the opposite channel. Observed surviving values were 0.5303301 and 0.3535534. Reported latency was zero. |

The documented Volume axis spans silence to unity with half amplitude at its midpoint. Width spans 0–200% with unity side gain at 100%. Pan includes multiple balance laws, combined stereo panning and a separate interchannel delay. The three endpoint cases above are sufficient to test those specific settings. The soft-curve interpolation, interior pan law and crossover response require additional fixtures.

The rack mixes and clocks each band independently. Each stage can add a low and a high band around its starting full-range middle band. Crossover slope choices are 6, 12 and 24 dB/octave. Band blend is scaled by the master blend. A 50% master and 40% band setting yield 20% effective band blend. Phase-locked beat mode follows project position. Sync cycle lengths span 1/128 to 32 bars, including straight, triplet and dotted divisions. Frequency mode spans 0.02 Hz to 5.24 kHz. A MIDI- or audio-triggered one-shot holds its final point. Reference manual, pp. 25–28 and 41–46

## 2026-11-04 — host D/host E depth and Pd fit, milestone 11

Research commit 6f09739fffefef35f6e943381669797882963b2a adds 165 entries: 58 host D editing, 47 export, 40 host E rack and macro container and 20 ADI source-audit findings. host D is v0.5 and host E 14 v0.2. Each entry has evidence, an ADI implication, status and a paired unexecuted case. Six original JSON diagnostic specifications sharpen editing scope, render signal paths, queue revision/publication, macro composition, rack relationships and Pd segment timing. Instruments excluded; FEATURES, decisions and host A parity retained; no new priorities, ADRs or product code.

Useful distinctions: source/take/item/comp and derivative ownership; directed grouped editing and ripple/lock/tempo scope; master-FX-only versus via-master rendering; partial presets, processing/rate/postprocess and output lifecycle; macro domains and discrete targets; graph reparent/duplicate/unwrap; asset and controller scope; feedback/compensation and missing dependencies. host E Shift-move prose and host D repair/pipeline descriptions retain explicit uncertainty.

Correction: M10 R10H079 incorrectly attributed an enqueue-time source snapshot to ADI. ADR0071d5 resolves names at render time; queue.cpp reads a consistent source/range/tempo/name revision at job start. Current notes correct that mapping; ERRATA-11.md supersedes the earlier design-note recommendation. Old bundles remain unchanged. Installed release notes distinguish WITH master FX from VIA master; not a measured result.

The 25 source files match product baseline 5c9e1cd8. At the separate user checkout 9bd55a8e, 7 match, 12 differ and 6 are absent, so this is not a latest-remote implementation claim. The audit identifies lower-level Pd facilities plus normal-loader/parameter/state and nested-rack integration gaps. ADI host owns graph, IDs, mappings, history and assets; Pd/native code handles processing. Canonical native eligibility allows CRLF/LF equivalence, generic WholeBlocks requires every segment to fit its block contract, and feedback/adapter/effect delays remain distinct.

Independent export/Pd review and 102 artifact/source checks passed, including 64 unique local source hashes, exact cases, historical preservation, citation line endpoints and six specification hashes. host D 7.79 and host E 14.1.0.8292 executable hashes unchanged. Guide 7.80 and API 7.83 remain version-qualified. Public-page budgets include retained/new titles and behavior.

E59–E62 retain sources, reviews, corrections, validation and reports. No DAW/plugin execution, GUI capture, audio/CPU, product build/test, new binary-analysis run, app update, push, PR, merge or external message. Win owns the interactive tests; win's earlier host A questions remain open.

## 2026-11-04 — host D and host E, milestone 10

Research commit b01cc9912d66f310a475157a5404a96508491c90 adds 188 entries: 83 host D host, 23 scripted effects/host scripting/developer, 69 host E documentary and 13 installed static findings. host D v0.4 now exceeds the earlier individually referenced scope; host E 14 v0.1 is new. Each entry has a source, ADI consequence, status and one unexecuted case. Other standalone instruments remain excluded. Current FEATURES, decisions and host A parity remain authoritative; no new priority, editor/runtime, ADR or product code.

Installed identities: host D 7.79 and host E 14.1.0.8292; host E registration separately says 14.1.0d100. All 55 new module hashes and 272 prior binary hashes remain unchanged. host D manual 7.80 is newer; 25 host E 14/the rack plug-in 14 web sources and seven current the host D developer developer pages retain version boundaries. The guide’s routing/capacity/pooled-copy/render examples contain contradictions, preserved as tests rather than adopted rules.

Useful ADI contracts: render dependencies, queue snapshots and postprocessing; source/comp/edited-result ownership; nested-rack and project boundaries; generated-MIDI capture without reprocessing; batch/feedback history; parameter display, automation, state and undo separation; shared asset services and timestamp-preserving controller adapters. host E’s Inspector/folders/rack cables do not reopen ADR-0047/0044/0113. Existing Pd/native and MIDI-lane work from milestone 9 remains the implementation starting point.

Binary inspection covered the host D wave module entry point and selected host E REX rate, tempo and preview-batch wrappers. These establish admission/registration/validation interfaces, not complete mixers, scheduler, numerical precision or CPU. Raw dossiers, manual derivatives and shipped source remain private under E55–E58. Independent reviews and 34 artifact checks passed; source hashes, exact case pairing, history/scope and six original input specifications were checked.

the private test plan has 188 unexecuted cases; no GUI/audio/CPU, product test/build or reference app/device execution. Win owns live tests. Earlier host A L01–L10 remain unresolved for win’s measured-evidence relay. No app update, push, PR, merge or external message.

## 2026-11-04 — host B MIDI, Grid and Pd feasibility, milestone 9

Research commit `c532d0597c64c3720ae2fb75bcc265729eeb9a30` adds 173 source-bound entries to host B v0.5: 59 Grid, 71 MIDI/Note FX/modulation/container and 43 non-instrument audio-device contracts. Every entry has a source, ADI implication, status and a paired unexecuted case. The complete local v5.2 Grid chapter and module catalog, MIDI/device chapters and narrow 6.0/6.1 overlays retain their version boundaries. Installed 6.1.0.22652 behavior is unmeasured; other built-in instrument devices remain excluded. Grid primitives and the polyphonic modular editor shared host/voice semantics are included only as needed for the modular architecture.

The feasibility recommendation is native host ownership of event/note/voice identity, scheduling, modulation routes, state/undo and latency, with existing Pd authoring/runtime and selected registered native DSP. This preserves ADR-0145/0177/0188/0203/0207, including immutable parameter IDs, vanilla-compatible parameter export, selected direct-native zero-adapter-delay execution, and the decided precision policy. Grid stereo 4× signals, host-rate mono modulators and current Pd scalar parameters at 64-sample boundaries are distinct. No arbitrary Grid preset import, arbitrary Pd conversion, complete voice bridge or replacement editor is claimed.

The code audit compares baseline 5c9e1cd8, user checkout 9bd55a8e and the separate agent/midi-effects c130f0e7 lane. That lane supplies fourteen native MIDI processor candidates and event-transform hooks but is not an ancestor of the baseline: reconcile it before designing another event architecture. Pd runtime/parser/sample/latency components exist; the inspected loader and PdDevice parameter/state/event interfaces do not establish an integrated user-loadable patch tier. Twelve likely Pd lane refs are baseline ancestors; unseen remote or ignored work remains outside the audit. Full report, 33 capability assessments and 58 hashed source snapshots are indexed at E54.

Independent reviews refined voice lifetime, feedback units, audio-modulation filtering, conversion makeup, signed track delay, signal-rate wording and portability. Thirty-four artifact checks passed; 75 local artifacts were hash-verified. Source metadata spans 189 distinct manual page numbers, not 189 certified complete feature pages. All 272 inventoried installed binary hashes remain unchanged. the private test plan contains 173 cases; ten original JSON diagnostic specifications explicitly remain NOT RUN. No GUI, build, product test, audio/CPU, new binary inspection, product change, app update, push, PR, merge or external message. Existing host A open questions remain unresolved for win's measured-evidence relay.

## 2026-11-04 — the drum rack, Packs and legacy variants, milestone 8

Research commit `8955b800a4d994208ede5c97e4fa1299bcc1e06a` adds 66 entries: 25 the drum rack, 28 Pack-effect, 12 legacy and one installed-location boundary. the drum rack is explicitly included; other instruments remain excluded except the two samplers. Eight pack families have primary-source capability notes; these do not establish complete controls or installed versions. The default factory content directory exists but is empty and Library.cfg's preferred path is blank. Other locations remain unresolved.

Read local Rack chapter pp.471–490, ten primary web sources, and structures of eight hash-bound legacy presets. Every entry links an ADI implication and an unexecuted case in PACK-RACK-MISSIONS-8.md. ADR-0060/0046/0114 review preserves ADI nested-graph latency/tails, open macro ordinals and persistent-routing/nonpersistent-output semantics; host A parameter takeover remains a reference difference. Historical the legacy frequency translator location wording conflicts with the later installed legacy preset family and stays qualified.

Ten artifact checks passed; a final ADI review narrowed the macro comparisons. No GUI/audio/CPU measurements, product changes, app or Pack updates, push, PR, merge or external message. There was no further binary inspection. Exact Pack inventory, legacy migration, complete control/default/text/history registries, DSP and CPU remain open. All original window questions remain unresolved for win's measured-evidence relay. Raw resources stay private; authored notes are versioned.

## 2026-11-04 — host A effects and named samplers, milestone 7

Director expands scope to audio/MIDI devices, explicitly including the two samplers while continuing to exclude other built-in instruments. Research commit `b4c52f0904845f6ff9d18f7f82b0aa8741756aeb` on `clones/daw-reference-reading-20261104` adds 358 contracts across 62 named manual devices: 42 native audio effects, four host-patched audio effects, eight native MIDI effects, six host-patched MIDI effects and the two samplers. Every row retains source, ADI implication, evidence status and a paired unexecuted case. Historical scope exclusions remain historical; four stable the compact sampler index entries are explicitly reopened. No product code, FEATURES, ADR or priority changed.

A7A/A7B cover 201 audio-effect contracts, A7M adds 131 MIDI/sampler contracts, A7R adds 17 preset/schema/the hardware controller findings and A7N records nine bounded native observations. Useful distinctions include generated-note identity/expression, trigger-time sample selection, zone and round-robin state, media reversal/conversion, modulation versus parameter ownership, local/global history, detector/tail/quality modes and parameter display/controller/storage domains. Current ADR-0169/0187 priorities remain; ADR-0203 d7 randomness and ADR-0207 d2 precision are preserved.

Binary inspection covered the physical resonator CalcA and SetSampleRateU routines and selected Amp exports. Evidence includes local scalar/packed float work and unresolved dispatch/quiet/ramp semantics; no whole-engine precision, DSP equivalence or efficiency claim follows. Seven presets and selected schema fields are static resource observations, not factory defaults or hard UI/DSP bounds. All raw proprietary code/resources remain private under E43–E47.

Independent reviews narrowed the looper first-click timing, Chord's inter-note Strum limit, locale restoration, unresolved dispatch meaning and target-ID evidence. The 62-device census retains open control/default/text/history/state/signal/CPU dimensions; 39 source-body subsections absent from the older TOC have separate links. A bounded installed audit found the legacy expression mapper and eight legacy native families beyond the modern manual list, and distinguished 1,225 presets/racks from processor counts. Installed Pack-effect inventory remains open; no Pack root was established.

DEVICE-RESEARCH-MISSIONS-7.md contains 358 future cases plus nine device censuses. Thirteen original audio/MIDI fixtures have hashes and independently checked structure/event sequences, with all host results NOT RUN. All earlier host censuses and L01–L10 remain open; no measured result is ready for win's relay. GUI testing remains assigned to Win by the supplied brief.

All 46 artifact checks passed. Thirty local source artifacts were hash-verified, including ten manual files covering 214 distinct printed pages; two primary web references retain version/retrieval boundaries. All 272 inventoried installed binaries remain unchanged. No DAW GUI/audio/CPU or product test was run. Local manual revision, installed 12.4 behavior, preset creator 12.4d1, historical host A 11 and current MPE documentation remain distinct. No push, PR, merge or external message.

## 2026-11-04 — host A reference in depth, milestone 6

Director requests coverage beyond the named examples across host A, still excluding built-in instrument/effect operation and implementation. Research commit `cfef0a8a1c93ac02ce8ba7b373766c0dfee18f17` on `clones/daw-reference-reading-20261104` adds 445 research contracts: MIDI 92, clips 86, routing/recording/files/Racks 73, controllers/video/accessibility/settings 63, static template/theme/schema 24, release 12, public API 20, tuning formats 14, remaining host/resource/timing 13, editor extension/Stacks 19, current the hardware controller 15 and synchronization/backend/video 14. Each retains four evidence columns and a paired unexecuted case. No product code, ADR or priority changed.

The useful boundaries include note/clip identity and collisions, source/beat/sample time, preview/apply versus playback randomness, UI/API/controller command scope, recording and automation ownership, typed routes, library/project/profile state, format validity versus ADI policy and loss during interchange. Independent reviews narrowed analysis-readiness, multi-clip export, pad replacement and API citations, removed a duplicate fade row and corrected general-history, tuning, precision and service/local-policy mappings. Manual shortcut, Humanize, Swing, preset-save and probability ambiguities remain explicit.

The 610-entry contents inventory keeps stable IDs and links 373 entries to exact new contracts; these are navigation links, not completed-feature counts. Four the compact sampler-operation entries are now explicitly excluded. Private E37–E42 include source manifests, coverage/gap review, the private test plan with 445 cases and ten completeness censuses, and 24 original tuning/Stacks fixtures whose runtime results remain NOT RUN. Current FEATURES/decisions/parity govern; Dream remains director context. The initially identified documentary gaps received additional primary-source work; remaining hardware/API adoption and state behavior stay open.

All 46 artifact checks passed: schema/IDs, four-column tables, exact case pairing, source integrity, ADR existence, earlier-content preservation, scope and unmeasured status. 67 local artifacts were hashed, including 42 manual text files covering 615 distinct printed pages. 29 primary public references are recorded; 8 unavailable raw snapshots have no fabricated hashes. All 272 inventoried installed binaries remain unchanged. This is research-artifact validation, not product CI or host behavioral verification.

No new DAW GUI, audio or CPU measurements. The supplied brief assigns live testing to Win; all original L01–L10 questions remain unresolved for win's evidence relay. Local manual revision is unconfirmed versus installed host A 12.4; API 12.4.5, ASCL 12.1 and release-specific changes remain version-qualified. Static template/theme creators are older and establish neither factory defaults nor the on-screen palette. Further documentary gaps and runtime work are classified separately. No push, PR, merge or external message.

## 2026-11-04 — more host A behavior, segment pitch editing and workspace research, milestone 5

Director explicitly adds host C segment pitch editing, detachable/multi-screen elements, extensive shortcut customization and general preferences, with host A still the primary reference. Research commit `5e2401301bc35c6ecdb09d3946ba8eb4910055bb` on `clones/daw-reference-reading-20261104` adds 172 documentary contracts: host A 107 (A5C/A5H/A5W), host C 65 (C5V/C5W). Each has behavior, source, ADI implication, status and a private measurement case. No product implementation or ADR change.

Host C's 30 segment pitch editing rows cover analysis/segmentation, pitch and timing edits, formants/gain, scale/chord input, MIDI extraction, reset/reanalysis/flatten and interaction with offline processes. The guide's inconsistent flatten scope remains unresolved; shared-source propagation and exact storage/undo still require evidence. Its 35 workspace/settings rows distinguish editor windows, lower zones, linked mixers, focus, project/global workspaces, profiles, command bindings, macros and preference ownership. Multiple windows do not imply arbitrary panel detachment. Settings can change shared-clip processing, MIDI split behavior and group-mute restoration. Monitor disappearance/DPI recovery and macro failure/undo remain open.

Host A receives 49 mixer/freeze/clock/Session/engine/recovery entries, 40 browser/file/default/device/hosting entries and 18 keyboard/window/settings entries. Findings separate containment from routing, frozen derivatives from editable source, file replacement from external editing, parameter values from plug-in configuration, A/B banks from preset storage and visible Undo History from disk recovery inputs. The manual's shortcut contradictions remain explicit. Independent review narrowed clip destination, group-color command, freeze editing and grouped-stem scope, corrected recovery-guide attribution, and qualified host C reanalysis as conditional.

Private E34–E36 contain 172 authored contracts, source manifests and DEEP-HOST-MISSIONS-5.md. All 25 artifact checks passed: row IDs/table structure, exact mission pairing, prior-content preservation, ADR references, documentary status and source integrity. Thirty-three distinct local source files were hash-verified; two official host A support references are recorded without fabricated snapshot hashes. All 272 inventoried installed binaries remain unchanged.

No new DAW GUI, sound or CPU measurements; Win owns live testing under the supplied brief. host C manual 15.0.30 versus installed 15.0.5.114 and unconfirmed host A manual revision versus installed 12.4 remain explicit. All ten original host A window questions and the 610-entry host A base inventory remain open at complete behavioral-verification level. Win relays actual returned evidence into original measurement questions. No push, PR, merge or external message; these checks are research-artifact validation, not product CI or a claim to understand the whole applications.

## 2026-11-04 — export, host B UI and complete host A baseline scope, milestone 4

Director asks specifically about host C export, host B selected-track auto-zoom and complete host A coverage as the base reference. Research commit `a8a96d8c` adds 49 documentary contracts (host C 19, host B 6, host A 24), the full host A host/integration contents inventory and an updated prompt/coverage pointer. host A's A-BASE register accounts for 610 host/integration entries out of 871 contents entries; the remaining 261 are device implementation/credits material. Counts include parent sections/examples and do not represent independent features, body text fully read or completed measurements.

Host C export now distinguishes Quick Export, eligible channels, range sources, processing taps, file naming/conflicts, queue settings/cancellation, format/channel/metadata choices, post-export actions, AAF/OMF/project interchange, ADM and video. The earlier active-marker-track implication is corrected: Audio Mixdown has its own marker-track chooser. Manual inconsistencies about video resolutions, FLAC depths and MP3 bitrate units remain explicit verification gaps.

Official host B 6 documentation confirms Auto-zoom (Shift+Z) for a selected track or automation lane and resizing the selected item to set zoom size. The v5.2 half-height description is marked historical. Selection multiplicity, normal-height restoration, group/filter interactions, state ownership and undo are still unmeasured. host A's new findings distinguish consolidate, individual/group bounce, paste-time rendering, export, session-only Undo History, linked edits, recording, envelope composition/time domains and accessibility.

Private E30–E33 retain source hashes, contracts, public host B snapshots, the topic inventory and 22 passing artifact checks. All 272 inventoried installed binaries remain unchanged. BASE-REFERENCE-MISSIONS.md supplies export, Auto-zoom and full host A audit cases; no GUI, sound or CPU measurements occurred. All ten original host A questions remain open. No product code/ADR changes, push, merge or external message. The complete-base scope is established; complete-base behavioral verification remains unfinished.

## 2026-11-04 — whole-Dream research, milestone 3

Director clarified that the named audio-editing, warp/MIDI and modulation cases are only examples: the research scope is every intended Dream/current ADI feature. Research commit `3ac99a35` on `clones/daw-reference-reading-20261104` adds a 220-requirement register spanning all 15 Dream sections, an independent index of all 319 FEATURES table data rows including metadata, and an integration/contradiction document. This is coverage accounting, not 220 completed contracts. Current repository decisions govern implementation; Dream expands research without overriding them.

Per-app v0.3 notes add 89 source-backed rows: host A 32, host C 30, host B 22 and five host D rows only for explicitly referenced behaviors. New detail covers launch/follow timing, monitoring and compensation, automation ownership, media/browser/transfer, tuning/articulation, racks, arranger/time domains, mixer/cue recall, controllers, hosting, modulation sources and Grid interfaces. Built-in instrument/effect implementations remain excluded. The earlier chapter-level exclusion of all host B chapter 19 was too broad: modulator and Grid interface semantics in §§19.27–19.28 are included. The Score Editor feature/exclusion conflict is recorded rather than silently resolved.

Private E24–E29 evidence includes nine pinned/datestamped primary SDK/format/backup snapshots; structural summaries of 25 shipped host A XML artifacts; twelve installed host B API documents; and a hash-bound reading manifest covering 36 source files. Protocol findings cover gestures, expression identity, process/flush, ARA ownership, interchange, backup and audio-network transport availability. They do not establish installed engine internals or authorize ADR changes. Raw sources were retained privately.

WHOLE-HOST-MISSIONS.md adds ten reference-test families and an ADI-only future acceptance matrix, supplementing both earlier mission documents. No GUI, audio or CPU measurements were performed; all ten original host A window questions remain open for Win evidence and win relay. AI/collaboration, telemetry, remote hosting, mobile and ADiJ are explicitly tracked as independent ADI contract work where reference parity is unsupported.

Validation: 37 research-artifact checks passed, including source/reference integrity, all 272 inventoried installed binaries unchanged and 74 inspected source artifacts unchanged. No product code, ADR, build or CI changes; no push, merge or external message. Full-host research remains incomplete. Continue from the per-requirement open dimensions and ingest actual test results into the exact app/build rows.

## 2026-11-04 — expanded host research, milestone 2

Director expanded the assignment to all host behavior, prioritizing host C F7 DOP/advanced fades, host A warp/MIDI and host B modular/modulation semantics. Built-in effect/instrument implementations are excluded; editor utilities, hosting, routing and Grid semantics remain included. Research commit `905556ef` on `clones/daw-reference-reading-20261104` adds 127 detailed contract rows (host C 45, host A 43, host B 39), per-app remaining-scope queues and an expanded dated prompt. No product code or ADR changed; nothing pushed or merged.

Evidence distinguishes local manuals from installed behavior: host A manual revision unknown, host B manual 5.2 supplemented by official 6.0/6.1 release notes, host C manual 15.0.30 versus installed 15.0.5. Read-only inspection added structural summaries of 25 shipped host A XML Sets, 11 installed host B API documentation files and verified host C DOP-label instruction references. These do not establish complete schemas, engine architecture or runtime behavior. Raw artifacts were retained privately.

Private `DESIGN-IMPLICATIONS.md` maps source findings to existing ADI decisions. `HOST-COVERAGE.md` inventories 187 source chunks, including 162 host chunks; 26 have selected contracts recorded, none comprehensively measured. The separate host C Score Editor source is missing. `DEEP-Win-MISSIONS.md` defines nine missions for ownership, apply/cancel, fades, range edits, warp, MIDI, modulation/Grid, audio events/operators and host export. Eight synthetic MIDI fixtures supplement the 66 WAVs. No automated GUI testing or DAW sound/CPU measurements; all ten host A window questions remain open for Win evidence and win relay.

Validation: existing 26 checks and 28 expanded artifact checks passed; all 272 inventoried installed binaries and 223 inspected resources/manual chunks unchanged; eight MIDI fixtures independently parsed. This is research-artifact validation, not product CI. Full-host completion is not claimed. Next work follows the explicit coverage queue and returned Win measurements; no implementation is authorized by this research milestone.

## 2026-11-04 — director-assigned DAW reference reading, milestone 1

Research only. Current main baseline `5c9e1cd8`; the director clarified that repository project documents govern and Dream Reference v0.2 is personal context. An isolated item branch `clones/daw-reference-reading-20261104` contains commit `ff7a52a8c272608aa82925a17cbb9eb530e432d4`: five app files under `docs/daws/`, the dated reference-reading prompt and a claim. This log stays on `clones/log`. Nothing pushed, no PR or merge, no external messages.

Binary inspection covered selected host A, host B and host C modules, including host B FFT/window wrappers and host A RWDEF exports. Raw evidence, projects and diagnostic scripts were retained privately.

Installed identities: reference host A 12.4 build 2026-04-24_d85a94ab5e; host B 6.1.0.22652, revision 162652; host C 15.0.5.114. SHA-256 recheck: 272 inventoried binaries, zero changed. Local host B manual is 5.2; host C manual is Pro 15.0.30. The docs retain those mismatches and never promote them to measured installed behavior.

No GUI interaction, DAW sound measurements or CPU benchmarks. Sixty-six deterministic audio fixtures and a residual analyzer are prepared; identical and polarity-inverted self-checks passed. Controller 7-bit colours are explicitly not substituted for the 70 measured UI colours. None of the ten normalized host A window questions was closed. `Win-MISSIONS.md` in the private root specifies exact starting states/actions/evidence/acceptance/final states; win must relay actual returned measurements into the original window log and awaiting table.

Validation is limited to research artifacts: table structure, evidence/fixture hashes, successful provider results and analyzer checks. No product code changed; the full ADI build/CI suite was not run and no merge-readiness claim is made. Next: Win's host A UI/automation/undo missions, then initial 48 kHz/256 audio probes. Product implementation still requires the director's explicit instruction.
