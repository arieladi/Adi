# Clones lane log

## FOR THE DIRECTOR

- **Reference user-IR selection.** Can you supply an editor-saved fully wet Reverb state with a short stereo WAV you authored, plus that WAV? The known hall produced a wet tail in the same test where selecting our two-tap file by stored name gave silence. I recommend using the original ADI room for the current release and parking comparison of the user-import path until this matched pair is available. The file need only contain two marked taps with different left/right levels.
- **Optional noise-library comparison.** The reference lists its pink source as a downloadable recording. Its installed slot returned silence. I recommend keeping the independently calibrated ADI pink source and deferring the vendor recording comparison unless you have a licensed installation with that source available.

## 2026-11-19 — Internal envelope and trigger measurements for ADi-SHAPER

The clones seat reserves `clones/adi-shaper-follower` for the Width band's internal follower. It extends only the existing ADi-SHAPER DSP, compiled external bridge if needed, canonical patch, device-specific build file and shaper tests. This topic starts from the eleven-stage order and editor integration at `f820812f`. External key routing, MIDI plumbing and audible trigger lookahead remain separate devices and engine integration topics. No Mix View or shared Window file is reserved.

### Envelope detection and curve combination

The Volume stage provides a triggered level contour. Its initialized band exposes a volume contour and gain. The Width contour provides a separate, directly measurable envelope follower, while the reference offers a separate compressor for automatic level reduction. These distinctions matter when assigning a follower to an ADI control. [Reference manual, pp. 14, 25, 47–55](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=47)

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

The flat-unity audio control located the actual dry path 672 frames later at 48 kHz. The two lower-latency audio algorithms timed the strong burst restart one sample before that delayed dry onset. Moving Trigger Shift from its midpoint to its low/high limits moved each restart by −192/+192 samples, or ±4 ms. The higher-latency algorithm reported 1,152 frames and detected these same bursts 1,153 frames after input. The audio detector default was inactive, Threshold 0.0883883, Detail 0.5, Shift 0.5, algorithm index 0, internal main input, HP 0.2 and LP 1. The reference manual describes three algorithms and their intended material. [Reference manual, pp. 42–44](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=42)

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

The devices seat owns the Pd-tier integration. ADi-SHAPER currently declares two audio inputs and MIDI off in `src/adi/builtin_catalog.cpp`. For external keys, coordinate the four-input catalog declaration and the canonical factory in `src/adi/pd_builtins/native_devices.cpp`, then connect channels 3/4 from the patch and compiled external to the detector. The engine already carries `NodeIo::sidechain` and maps the optional extra planes for four-input built-ins. Auto Filter is a working canvas precedent. For note triggering, coordinate the catalog MIDI flag, a shaper-specific note-on handler in the native factory and a `notein` connection for editable Pd. The existing Shifter device and `src/juce/pd_engine.cpp` demonstrate those paths. Basic note and sidechain delivery fit the existing host interfaces. Changes to event scheduling, transport packet fields or graph-wide routing would also involve the engine owner.

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

**PR requested** — `clones/adi-shaper-performance` at `4ee156c0`. Title **Reduce idle shaper overhead and match the long-running Crush clock**. Review after `clones/adi-shaper-sources`. All twelve focused Release suites passed under strict MSVC warnings. Restoring an earlier state over a running shaper clears parameters introduced in later versions. A revision counter lets the processing callback skip up to 715 unchanged atomic parameter reads. At 64/256/1,024 frames the measured neutral median callback fell from 1.6/1.7/2.1 µs to 0.4/0.5/1.3 µs in the two passes.

After calibrating the common hold curve and bit quantizer, full registered stereo rendering reduced the previous 0.09781 Crush peak error to 0.000004493 or less at all three block sizes. At 256 frames the registered Crush used 56.2% fewer processing-thread cycles than the original with the same input. The independent 48 kHz sparse-impulse and quantization evidence follows below.

The one-bar Time render matched both reference channels sample-for-sample at 64, 256 and 1,024. At 256, it used 79.7% fewer cycles; cyclic Pitch at +12 used 69.7% fewer; independent procedural stereo white used 46.3% fewer. Pitch's peak sample difference on this music was 0.000233. Noise is compared statistically. The original and ADI room use different impulse responses, so the room cost is recorded separately. The optimized host pass reused the immediately preceding reference timing results. An isolated 1,024-frame Drive reading was within 0.5% of its reference cycle count; it does not establish a large advantage for that setting.
### Crush clock and quantization precision

At 48 kHz, a four-second kick-and-bass render with Bits at 0.25, Resample at 0.5 and dither off exposed a growing difference in the fraction of each sample captured by the hold. Captures with 64- and 256-frame callbacks started after 96,000 frames of live input. The 1,024-frame capture started at 96,256. Removing the 10 Hz DC blocker for the measurement exposed the two successive output levels at each fractional wrap.

Across both channels and all three callback sizes, the reference clock at midpoint was 3,000.00500679 Hz. An exact 3,000 Hz clock put each wrap 0.160 samples late after two seconds and 0.481 samples late after six. Preserving the correct capture epoch, an independent replay of the earlier implementation matched it within 3.0e−8 peak. Changing the common exponential clock anchor alone reduced the 256-frame music RMS difference from 0.006587/0.006689 to 0.0000442/0.0000362, left/right. Further whole-curve checks measured 644.322453, 1,449.600047, 5,985.006099 and 11,732.046636 Hz at heights 0.25, 0.375, 0.625 and 0.75. The analytic curve retains small residuals at other settings and sample rates.

Fine input-step measurements refined the quantizer level count to `1 + a·u + (32767 − a)·u³²`, where `a = 14 × 32767/32768`. At 0.25 this gives 4.49989319 levels. An independent fit to the music gave 4.49989323. The existing output amplitude law matched. At heights 0.125 and 0.25, inputs 0.1818200 and 0.1111125 respectively remained just below the first threshold.

The standalone regression uses single markers after a quiet, continuous input to keep the clock active. At midpoint, a +0.5 marker on frame 96,015 yielded initial/held amplitudes 0.067283/0.419878. At frame 288,015 the same marker gave 0.124813/0.259661. Additional markers exercise 0.25, 0.375 and 0.75. With both common-law refinements, the independent musical replay across 64/256/1,024 frames had worst-channel RMS 8.0e−8 and peak 4.5e−6. These replay numbers describe the isolated mathematical check.

## 2026-11-19 — Curve editor ready for Window

**PR requested** — `clones/adi-shaper-editor` at `b34eb089`. Title **Add an accessible ADi-SHAPER curve editor for the device strip**. The component was built with MSVC Release strict warnings. Offscreen tests passed for actual stroke placement, eight points plus their wrap, held and linear curves, mouse and keyboard editing, screen-reader values, cancellation and rebinding.

Window can link `adi_shaper_curve` and mount `adi::ui::AdiShaperCurve`. Bind a live device ID, stage, band and contour to the eight normalized point values, unit and display formatter. The synchronous edit callback identifies the point and original target for Begin, Value, End or Cancel. A Cancel restores the original value and closes the gesture. Refresh from the published model and unbind when the owner changes. The strip's stage/band selection and the common Undo transaction remain Window's integration points. The component is available to the owner; the director's desktop was not used for testing.

## 2026-11-19 — ADi-SHAPER stage-order evidence and file reservation

The clones seat takes the shaper routing loop, its stage-order parameter declarations and the associated shaper tests on `clones/adi-shaper-order`. The branch adds an append-only order control per shaper; it also preserves every prior control ID. Sorting runs at a segment boundary only after a changed order control. It does not require a Mix View or Window change.

# Programmable stage order

The reference presents loaded stages in signal-flow order. Dragging a stage changes its position. A new initialized instance has all stages off. The initialized scalar parameters for a stage remain independent of its position. The complete saved device state includes an ordered vector of sixteen slot identifiers, including unselected slots. The manual describes chain editing and whole-device preset recall. [Reference manual, pp. 29–30](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=29)

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

- Append eleven integer parameters, one per existing stage, after current ID 715. Suggested range 0–127, with defaults 8, 18, 28, 38, 48, 58, 68, 78, 88, 98 and 108 for Volume, Width, Pan, Filter, Liquid, Drive, Crush, Time, Pitch, Noise and Reverb. Label each control with the stage and “Order”. Lower values run earlier. Gaps allow most moves by changing a single control. A future drag editor can assign these same values to the desired sequence in one undoable gesture.
- Decode ranks into a permutation of the eleven stage identities. Sort by ascending rank, settling ties by the stage's permanent identity in the legacy ADI chain. A collision always produces a complete deterministic order. A change to one stage's rank never deactivates or duplicates another stage. Any of the 39,916,800 permutations is representable. Existing projects that lack all eleven keys take the old signal path. Partially populated keys can be restored in any sequence.
- Existing `adi.param` integer declarations already expose stable numeric identifiers, quantization, editable generic strip controls, project recall and automation in both the native and editable Pd paths. Each control can use an ordinary integer range. `adi.array` is for display reads. A convenient drag list would need a dedicated UI hook owned by the corresponding seat. A single full-permutation rank exceeds the exact consecutive-integer range of a 32-bit Pd float.
- Cache the route in a fixed eleven-entry array. Mark it dirty only when a quantized rank changes and rebuild once at the next segment boundary, after pending parameter deliveries. An in-place insertion sort needs at most 55 comparisons and 55 shifts. Run each active stage's existing block renderer once in the resulting order. Dispatch belongs outside the sample loops. Preserve a specialized fast path for the old order if the CPU benchmark justifies it. The current 16-sample Drive host compensation is independent of position. Other stage latencies follow their existing contracts.
- Retain each stage's clock, settings and DSP state with its identity across an order change. This avoids clearing delay histories, interrupting free-running contours or allocating a second rack for a crossfade. Live-tail handling on reference drag and click suppression remain unmeasured. The proposed recall guarantee applies to the order and controls. Audio already in a previous route can affect subsequent tails after a live move.
- Acceptance tests should cover direct nonlinear reference predictions in both orders, all eleven modules exactly once under tied and extreme ranks, exact legacy order on old-state restore, forward and reverse order of parameter delivery, a reorder that moves Drive while PDC stays 16, and native/editable Pd output through odd host segments. Benchmark unchanged default routing and a noncanonical active chain separately.

## 2026-11-19 — Eleven-stage ADi-SHAPER ready for review

**PR requested** — `clones/adi-shaper-sources` at `380335a2`. Title **Add cyclic Pitch, generated Noise and an original convolution room to ADi-SHAPER**. Review after `clones/adi-shaper-time`. All 12 affected Release suites passed under MSVC strict warnings. The three modules extend the parameter map from 463 to 715 and have zero additional host latency. The editable Pd patch and compiled processing path agree on the irregular-block stereo tests. Earlier projects retain their stored indices.

- Pitch implements the cyclic grain mode, with a 1–100 ms window and ±24 semitone contour. All eleven unison peaks across the three tested rates arrived on the measured frame. Fixed stereo tests measured at most 0.000100 sample error. A moving two-octave sweep gave peak error 0.000129. Polyphonic, voice and drum algorithms remain reference work.
- Noise generates independent white sequences at 44.1 kHz, interpolated for the session rate. It also offers separately calibrated ADI pink noise. Signal tests cover stereo statistics, same-channel mono, source pitch, level, trim, filter response, warm reset and digital silence. Source noise stays broadband under the musical crossover selection until its own filters are applied.
- Reverb convolves the incoming stereo signal with an original finite room response, factored into two short diffusion filters and sparse reflections. The reference-derived controls include Send, Return, width, gate, decay, size, synchronized/free predelay and Clear Tails. A closed send preserves the tail, a closed return mutes it, and the independent room passes exact predelay-shift checks. Its four routing layouts include the measured 25% default and the full-level dry and wet overlap at 50%. The response is ADI-authored. Importing a user WAV and reproducing other room assets remain separate work.

Additional Time evidence: at three free-Hz positions a separate amplitude cycle measured 30,459.5, 4,687 and 721 samples. The Time Follow read distance gave 30,459, 4,687 and 721 at the same settings. A held beat-rate clock stayed at 96,000 frames in the same test.

## 2026-11-19 — ADi-SHAPER device drawing and processor work

For Window, the clones seat takes a standalone JUCE editor component in `src/juce/adi_shaper_curve.*` and a dedicated offscreen drawing/gesture test on `clones/adi-shaper-editor`. Its port accepts the active device parameter model and emits edits through callbacks for the existing gesture transaction. Window owns mounting the view and selection in the shared strip. The device panel, parameter feed, app shell and Mix View files stay with their current owners.

For devices, the clones seat also takes only the ADi-SHAPER-specific parameter-read and legacy-state handling in `src/juce/builtin_host.cpp` with shaper-only regression coverage. The large shaper advertises hundreds of independently automatable parameters. A callback with no changed controls can skip reading those targets. Loading a saved earlier rack into a previously used instance should reset parameters added in a later version to their declared defaults. Neither change needs a shared strip or Mix View edit.

The reference analyzer classes enumerate as OScope 2.0.5 and PsyScope_Pro 1.5.03 in the installed VST3 formats. For the other seat's identification question, their module SHA-256 values are `03dc311fa8505bec5f44d10ee75114c52efd4031bfd993242c65a049895e1f65` and `f326b2155d57bc019b368059324bdb4a91f806e291462ee42b908dee1082b0aa`. The measured default audio paths and documented controls are in the Focus handoff. Full-edition entitlement is unverified.

## 2026-11-19 — ADi-SHAPER Time ready for review

**PR requested** — `clones/adi-shaper-time` at `9ead5d66`. Title **Add transport-aware multiband Time to ADi-SHAPER**. Review after `clones/adi-shaper-drive-crush`. The eight-stage device preserves its first 415 parameters and appends 48. The zero-latency Time stage selects Follow or one of seven fixed ranges independently in each band, with beat/free clocks and mix. It retains recorded input over tempo changes and invalidates on a transport discontinuity. The native path uses the graph's segment sample position and revision. The editable patch has the existing 64-frame adapter latency and a coarser transport stamp.

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
| Initialized values | The saved all-stages-off state had the noise source off, normalized HP 0.254, LP 1, trim 0.8, mix 1, Mono enabled and Noise Only disabled. The manual also describes the supplied stereo noises and how Mono takes their left channel. The measured identical-channel output establishes the mono result independently. [Reference manual, pp. 16–18](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=16) |

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

## 2026-11-19 — ADi-SHAPER source and cyclic pitch file reservation

The next device branch is `clones/adi-shaper-sources`. The clones seat takes the new `src/adi/dsp/adi_shaper_pitch.*`, `adi_shaper_noise.*`, `adi_shaper_reverb.*` and their own test files. It also retains the shaper-specific integration files and canvas from prior increments. Work starts with independent measured modes. The performance, timing and resource behavior of each stage will be checked after integration. The stock Shifter, Noise-generating devices, Reverb, shared Pd engine and Mix View files remain with their owners.

### ADi-SHAPER Pitch reference

Four modes were rendered independently with one full-range stereo band, full wet, fixed pitch, linked formants, reserve latency off and no modulation smoothing. A pitch contour from bottom to top commands −24 to +24 semitones. Its midpoint is unison. The target read-speed ratio is `2^(semitones/12)`. The mixed-material mode has three grain choices. The cyclic mode has a continuous grain control. Voice and drum processing support independent formant contours. [Pitch controls](https://www.cableguys.com/pitchshaper) · [Reference manual](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf)

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

The pitch mode in ADI's existing Shifter is a windowed FFT phase vocoder. ADI's Warp renderer studies a clip, reads it again and renders the result with Rubber Band. The devices lane has also proposed a live dual-read-head Shifter. Its 10–350 ms window and linear interpolation would require the cyclic calibration above. These are potential points for sharing DSP with the owner.

An independent stereo test of Rubber Band 4.0's short-window live API found a fixed 512-frame processing block. At 48 kHz it reported 6,784, 3,648, 2,112, 2,880 and 4,031 frames of delay at ratios 0.25, 0.5, 1, 2 and 4. Its unison click peaked at 2,112 samples with amplitude 0.4828 for a 0.5 input. Its ratio-dependent timing and coloration need an explicit adaptation design. ADI lists Bungee for future integration. Its realtime capabilities merit a separate audition. The present evidence supports implementing the cyclic mode independently. Polyphonic transient matching, phase continuity, vocal envelope shifting and automatic grain selection still need dedicated fixtures before defining equivalent mixed-material, voice or drum modes. [Rubber Band live API](https://breakfastquay.com/rubberband/code-doc/classRubberBand_1_1RubberBandLiveShifter.html) · [Bungee](https://github.com/bungee-audio-stretch/bungee)

## 2026-11-19 — Time reference and next device files

The Time increment is on `clones/adi-shaper-time`. The clones seat takes the ADi-SHAPER core, external adapter as needed, canvas, device CMake file and shaper tests from the previous increment. It also takes the large-canvas embedding hunk in `cmake/devices/pd_native.cmake` and the new `adi_shaper_time.*` core and test. Existing parameter indices stay stable. The devices seat retains all other devices and the Pd engine. Mix View owns its Focus surface, sum and alignment-preview integration. Its published Focus review at `2102d4e9` already rejects ambiguous loop windows in its own path. Our separate legacy scope-helper reproductions and rendering measurements remain test evidence for the owner.

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
- ADI already prepares its Live Delay and Echo histories off the audio thread. Its Liquid stage invalidates old history with counters. For Time, request a prepared history bank on the control or resource thread when an instance first needs Time. Allocate and touch the complete storage there, publish the ready bank at an audio segment boundary and retire it off the audio thread. Allocate during normal off-thread device preparation when restoring an already-enabled project. Keep an available bank through short bypasses, invalidate by counter when a fresh history is needed and release at teardown or on a control-thread cache eviction. This lets never-enabled instances use no Time history memory. If activation can arrive as audio-rate automation, prime a bank before the event where possible. While waiting, retain the exact dry contribution and let unavailable wet reads contribute zero. An implementation promising sample-exact cold automation needs a prewarmed bank available before the event.
- Virtual address reservation is optional. Windows supports a reserve-only region with no physical or paging-file allocation for the region. Commit and pre-touch pages off the audio thread before publication. POSIX platforms can create an inaccessible anonymous mapping and change its protection before touching it. Commit accounting and residency depend on the operating system. Neither initially reserving addresses nor allowing demand paging guarantees real-time first access. [Windows virtual allocation](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) · [POSIX memory mapping](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html) · [POSIX memory protection](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mprotect.html)
- A portable bank can use two correctly sized float vectors or a vector of stereo frames created on the resource thread. C++ vector capacity growth has no virtual-memory or physical-residency contract. Only constructed elements below `size()` are valid sample storage. Move or publish the bank's owning object without copying its sample storage, and keep final destruction on the resource thread. [C++ vector capacity](https://eel.is/c++draft/vector.capacity) · [C++ vector data](https://eel.is/c++draft/vector.data)

## 2026-11-19 — ADi-SHAPER Drive and Crush ready for review

**PR requested** — `clones/adi-shaper-drive-crush` at `a3891a36`. Title **Add oversampled Drive and fractional Crush to ADi-SHAPER**. Review after `clones/adi-shaper-modulation`. The seven-stage rack preserves its first 286 parameters. New indices extend the editable Pd patch and its native processing path through 415 parameters. Drive reports 16 samples and keeps the dry signal aligned. Crush reports zero. The three Drive styles and independent Crush contours were tested at 44.1, 48 and 96 kHz. All eight focused MSVC Release tests passed twice with strict warnings, including Pd parameter routing, device registration and delay. The following measurements cover their implemented settings.

### ADi-SHAPER Drive reference

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

The manual attributes the Drive delay to oversampling and gives nominal input/output trim spans of −12 to +36 dB and −36 to +12 dB. A compensated delay of 16 samples can arise from different filter lengths and rate factors. Empirical passband, alias rejection, impulse alignment and transition behavior form independent audio targets. [Reference manual](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=61)

**Independent implementation.** Two symmetric, windowed-sinc filters with eight input frames of group delay each reproduce the measured passband and delay. Four substeps at 44.1/48 kHz and two at 96 kHz reproduce the tested spectra. The hard clip averages its transfer over the line segment between successive substep inputs. Applying that averaging to the other two curves changed the measured impulse shapes. The output compensation uses 33 independent low-level readings per curve family. The reference’s private filter coefficients and internal rate factors remain undetermined.

At 48 kHz, ADi-SHAPER reproduced the tested alias amplitudes of the full-drive, 9 kHz hard clip within 0.000031 absolute. Across six independent 199 Hz sine tests at three input levels, the first, second, third and fifth harmonic amplitudes agreed within 0.000057. The largest difference in the signed impulse taps tested at 44.1, 48 and 96 kHz was 0.000292. The maximum landed at sample 16 in each case. These figures apply to neutral tone and accent, minimum push and grip, and the three measured styles.

### Crush reference contract

**Measurement.** Stereo floating-point input, zero bias and jitter, prefilter and dither off, treble at its maximum, fully wet single band. Static contours isolate the bit and resample controls. Controls below use normalized height 0–1. Native block size was 256 unless specified. Samples, frequencies and amplitudes below are acoustic results. The reference reports zero host latency.

**Bit resolution.** Ramps of 16,385 small ascending steps, held for four samples, identified first thresholds and output steps at 27 heights. For height `u < 1`, level spacing fits `1/L`, with `L = 1 + 14u + 32753u^32`. Levels are rounded to the nearest integer in the scaled domain. Settled output is `G × floor(Lx + 0.5) / L`, where `G = 1 − 0.25(1−u)^3`. At heights 0, .25, .5, .75 and .99 the measured level counts were 1, 4.5, 8.0, 14.79 and 23,760.16. For `u=1` the bit section passes full precision.

**Why a changing input obscures the staircase.** With dither off, a crossing sample is the area average of the staircase between consecutive input values. The following constant sample gives the settled level. For `k=floor(Lx+0.5)`, the primitive is `P(x)=(k L x − k²/2)/L²`. A transition from a to b returns `G × (P(b)−P(a))/(b−a)`. After inverting the output DC blocker, 21 multi-level changes at four heights matched this rule within 0.000007 on their transition samples. For height 0, changing +.05 to +.65 produces +.1875 then +.75. Changing +.65 to −.65 produces zero on the crossing sample, followed by −.75. The earlier response on a sine combined these inter-level values with high-pass decay.

**Hold clock.** At heights 0/.125/.25/.375/.5/.625/.75/.875/.99 the measured capture rates were 8.82/226.06/644.32/1449.60/3000.00/5985.01/11732.05/22796.85/41836.85 Hz. A parameterization fitting the measurements is `R = (44100−3000)/(3000−8.82)`, `f(r)=8.82 + (44100−8.82)(R^(2r)−1)/(R²−1)`. The same absolute rates held at 44.1, 48 and 96 kHz. Exactly 1 bypassed holding at native rate. Immediately below the top, the clock approached 44.1 kHz.

**Fractional timing.** On reset the held signal is zero. The first capture occurs one period after reset. A wrap partway through an audio sample captures the value interpolated between the preceding and current quantized inputs. The output for that sample is the time-weighted blend of old and new holds. At 48 kHz, height .25 placed the first event between sample indices 73 and 74. A half-amplitude impulse at 73 gave .126574 on output sample 74 then .251569 held; the same impulse at 74 gave .124995 then .248431, after compensating the DC blocker. The reference produced sample-identical results across 64/256/1024-frame host blocks and repeated resets. Independent noise showed that quantization precedes resampling. Over 4,000 samples the quantize-then-hold model had RMS error .00074; the reverse order had RMS error .252. A fitted clock can gradually accumulate fractional-phase error.

**Dry, wet and DC.** Either master mix or band mix at zero reproduced an impulse bit-for-bit. The fully wet active module at both maximum heights still applied a one-pole DC blocker. At 48 kHz the first three coefficients were .99934596, −.00130729, −.00130558. Estimated cutoff across 44.1/48/96 kHz was 10.0002/10.0000/9.9999 Hz. Half master mix at bit height .5, resample height 1 returned .742029 on the impulse sample and .241712 on the following sample.

**Dither.** At the lowest bit depth, with both input channels identical, a constant .25 input produced RMS .13546 after warmup with dither enabled, versus zero at steady state without dither. At .1/.49/.5 input, the enabled RMS was .02688/.29904/.29945. Silence remained exactly zero. Both channels matched sample-for-sample, and repeated reference resets reproduced the same output. A shared deterministic triangular perturbation bounded to ±half a quantization step, inserted before staircase integration, reproduces these statistics. The reference generator, seed and sequences have not been identified. At maximum bit height the dither switch left the impulse response unchanged.

**Scope of the first ADI kernel.** Independent Bits and Resample contours with the behavior above, signal-linked dither, zero bias/jitter, prefilter off and full treble. It allocates no storage dynamically and reports zero latency. Bias, jitter and prefilter controls and variable treble remain separate reference work. [Reference manual, Crush module](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=24)

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

**Custom IR acceptance remains open.** A self-authored stereo PCM WAV used two taps 1,536 frames apart with distinct channel gains and opposite sign on the second right-channel tap. Placing it in the documented user library and trying ten stored selection spellings produced silence. The working installed hall served as a positive control in the same run. The transient fixture was removed. If exact custom import compatibility is required, the remaining input is an editor-exported fully wet state paired with the self-authored WAV chosen from the User column. The factory wet, clock, width and mix measurements above are independently reproducible. WAV import, the User column, IR resampling and the hard-point tail-clear behavior are documented in the [reference manual, pp. 19–20](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=19).

## 2026-11-19 — Five-stage CPU and stereo music verification

At `d18b7d11`, three-band Volume on the stereo kick-and-bass program used 81.3%, 61.9% and 51.1% fewer processing-thread cycles than ShaperBox 3.6 at blocks of 64, 256 and 1,024 frames. At 256 frames the median callback duration was 14.9 µs for ADi-SHAPER and 45.8 µs for the reference. Both processed the same 48 kHz source with 0.35/0.70/0.85 band gains, 659.255 Hz at 12 dB/octave and 3,729.310 Hz at 24 dB/octave. The maximum absolute stereo sample difference was 0.000000477 across 192,000 frames per channel at each block size; there was no time shift.

There were 18 matched CPU cells in all: full-range Volume, Width, their combination, a clean low-pass, a flanger and the three-band music case, each at three block sizes. The five-stage device used 45.8–95.4% fewer cycles in these cells. The smallest advantage was the flanger at 1,024 frames. State loading and file I/O preceded timing, input replenishment was matched, and the individual callback wall intervals excluded input preparation. Nine trials of 8,192 callbacks ran per cell with both editors closed. The multiband music case provides the signal check for nonconstant stereo input. These results apply to the named selections.

## 2026-11-19 — Drive and Crush acoustic acceptance

The Crush measurement used 27 static bit heights, held stereo levels, steps and rate changes across three sample rates. With dither and bias disabled and resampling bypassed, measured plateau spacing and amplitude follow these laws for bit height `u < 1`: level density `L = 1 + 14u + 32753u^32`, output gain `G = 1 - 0.25(1-u)^3`, thresholds `(k+0.5)/L` and step size `G/L`. At the top the quantizer passes full resolution. The decay on a held output at 48 kHz is 0.99869187 per sample, consistent with a 10 Hz DC blocker after quantization. Sampling before this decay is essential to measuring the steps.

The measured sample-hold rate at the midpoint is 3 kHz. Across the control it follows `8.82 + (44100-8.82)(R^(2r)-1)/(R²-1)` Hz, where `R=(44100-3000)/(3000-8.82)`. At its exact maximum the rate reducer bypasses, including at 96 kHz. At master or band blend zero the reference returned the original input bits. Dither at its measured settings preserved complete silence and produced matching channels for identical stereo input.

### ADi-SHAPER Drive reference

Reference 3.6, stereo 32-bit floating-point processing, 256-frame callbacks. One full-range drive stage, neutral trims, no tone tilt or modulation smoothing, full wet unless stated. Let d run from zero to one, x denote the input and z denote the input after pre-gain. All transfer values below refer to the level before DC removal.

| Property | Measured behavior |
|---|---|
| Symmetric hard clipping | For positive drive, the pre-gain follows `(1+d)^8`. Limit z to [−1,1] and apply the drive-dependent output compensation. At d = 0.25, 0.50, 0.75, 1 the small-signal gains were approximately 1.69, 5.49, 16.59 and 44.2. The positive output plateaus were approximately 0.283, 0.214, 0.189 and 0.173. Negative plateaus have equal magnitude. |
| First saturator | Uses the same measured pre-gain. The positive arm closely follows `z / (1 + 2z/3)` before output compensation. At full drive, input 0.001, 0.01, 0.1 and 0.35 produced approximately 0.0377, 0.163, 0.245 and 0.255. The negative knee differs. At −0.001, −0.004, −0.01 and −0.35 the respective levels were approximately −0.044, −0.146, −0.172 and −0.173. |
| Hard rectification | Pre-gain follows `(1+d)^4`. The measured fold is consistent with `clamp(abs(z+1)−1, −1, 1)`, with its own output compensation. At full drive an input of −0.04 produced approximately −0.146. Inputs −0.35 and +0.35 both produced approximately +0.228. The fold passes through zero when z reaches −2. |
| Band blend | At a 400 Hz, 0.2-peak input and d = 0.5, the quarter, half and three-quarter blend waveforms matched the weighted sum of independent dry and full-wet renders to within 1e-6 across all three styles. |
| DC rejection | Stepped DC inputs decay toward zero even with the drive contour at minimum. Following a 0.1 input step, the zero-drive response after 255 and 1,024 samples at 48 kHz was about 0.0716 and 0.0262. Their ratio gives a corner near 10 Hz. Signed step heights during the first millisecond, calibrated against the minimum-drive response and checked against later decay, reveal the transfer curve. |
| Latency and impulse | The host reported 16 samples at 44.1, 48 and 96 kHz for all three styles. On a 0.125 impulse at 48 kHz, the compensated peak at full drive occurred at the input frame. The first saturator, hard clip and hard rectifier gave peak levels 0.33629, 0.25322 and 0.26107. Their last samples above 1e−6 absolute fell 4,392, 4,243 and 4,603 frames after the input. At minimum drive, the 48 kHz impulse had a central coefficient of 0.81545. Magnitudes at 12, 16.8, 19.2, 21.6 and 24 kHz were 1.0000, 0.8910, 0.5662, 0.1878 and 0.0454. At 96 kHz the Nyquist magnitude was 0.2758. |
| Alias readings | Full drive, 9,973 Hz sine at 0.35 peak, 48 kHz. For the first saturator, hard clip and hard rectifier respectively, fundamental amplitudes were 0.27065, 0.21883 and 0.01953. The 19,946 Hz harmonic was 0.004678, approximately 1e−6 and 0.06286. The folded third harmonic at 18,081 Hz was about 1.0e−5, 7e−6 and 5e−6. Absolute spectral amplitude uses a one-second settled render and coherent FFT bins. |

The manual attributes the Drive delay to oversampling and gives nominal input/output trim spans of −12 to +36 dB and −36 to +12 dB. A compensated delay of 16 samples can arise from different filter lengths and rate factors. Empirical passband, alias rejection, impulse alignment and transition behavior form independent audio targets. [Reference manual](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=61)

## 2026-11-19 — ADi-SHAPER Filter, flanger and phaser

**PR requested** — `clones/adi-shaper-modulation` at `d18b7d11`. Title **Add measured multiband Filter and flanger/phaser stages to ADi-SHAPER**. Review after `clones/adi-shaper` (`295056b6`). Win opens and lands the PR.

Two optional stages extend the rack to five. Each band has a primary and secondary eight-point contour, blend, clock and mode. Five clean filters cover 6 and 12 dB/octave low-pass and high-pass responses and 12 dB/octave band-pass. Liquid includes a flanger and 4-, 8-, 12- or 16-stage phasers, with feedback polarity per band. All 133 earlier parameter IDs retain their meanings. Older stored states open with both new stages disabled.

The clean low-pass matched the six original response gates at 400 Hz and 8 kHz. Five-mode held-out stereo impulse error was below 0.000008 at the tested small-signal levels. The moving cutoff/resonance fixture differed by up to 0.000037. At extreme resonance, a half-amplitude impulse through a gentle filter showed deviations up to 0.01655. Across fifteen flanger center/feedback fixtures the first 5,000 normalized impulse samples differed by less than 0.000075. The highest tested phaser feedback retained a 0.0222 normalized tail discrepancy. These acoustic differences remain reviewable.

Six Release MSVC /W4 /WX suites passed, including genuine editable Pd versus native Filter and Liquid processing across irregular segments. Coverage includes stored state, cancellation of disabled modules, independent bands, sample-rate changes and modulation. The device and UI handoffs remain in their respective owner lanes.

## 2026-11-19 — Drive and Crush measurement reservation

On `clones/adi-shaper-drive-crush` the clones seat takes `src/adi/dsp/adi_shaper_drive.*`, `src/adi/dsp/adi_shaper_crush.*`, their dedicated tests, and the same ADi-SHAPER canvas, compiled-in external and test module. Existing device-stage IDs remain stable. Additional level, alias, hold-rate and latency measurements precede the audio implementation.

In measured hard clipping, pre-clip gain follows `(1 + height)^8` across nine heights. The hard rectifier follows `(1 + height)^4`; its central transfer follows `clamp(abs(z+1)-1,-1,1)`. Measurements used signed level steps and independent tones. A DC-coupled transfer read directly from a settled constant input would be misleading because of the observed low-frequency rolloff. These are transfer targets for the measured modes only. The distinct minimum/maximum output, mixture and timing fixtures are in the eleven-stage contract.

## 2026-11-19 — Device curve drawing handoff to Window

The device strip belongs to Window. ADi-SHAPER exposes every curve point to the ordinary parameter model; its parameter count exceeds the automatic 64-control display threshold. For an ADI-drawn editor in the strip, scope it to the internal UID `org.adidaw.builtin.adi-shaper` and the live device identity. The existing Color Cab sample tile is the integration precedent. Keep the existing Configure and fold operations. The Mix View function and EQ page controls have their own owner.

The editor selects a stage, band and contour. The eight evenly spaced horizontal positions are fixed; each draggable height uses its own persisted parameter. For the middle band, the Volume heights are IDs 23–30, Width 65–72 and Pan 107–114. On the modulation branch Filter cutoff uses 170–177 and resonance 178–185. Liquid center uses 245–252 and feedback 253–260. Plot the segment from the last point back to the first for the next cycle. Draw a held or linear segment according to the band's curve setting. Pan at the bottom favors right and at the top favors left.

Use the existing parameter Begin/Value/End gesture protocol and one undoable gesture per drag. A project rebind cancels the capture associated with the old instance. Provide discrete keyboard editing with an accessible name for each point. A software-rendered device strip test can verify the drawn points, focus order, undo, and stale-callback behavior. The clones seat has made no strip or Mix View changes.

## 2026-11-19 — ADi-SHAPER CPU and signal check on 295056b6

The registered device and ShaperBox 3.6 both processed the same stereo input, at 48 kHz and 120 BPM, with a single instance, closed editors and zero reported latency. The full-range cases were the same one-bar hard Volume gate, Width at mono, and both stages in series. Both renderers restored identical stereo input before each call. Nine trials of 8,192 callbacks per cell used the same affinity; fixture order was shuffled and processing order alternated.

| Frames | Gate, ADi / reference µs | Width, ADi / reference µs | Gate + Width, ADi / reference µs |
|---:|---:|---:|---:|
| 64 | 1.1 / 24.0 | 1.1 / 21.6 | 1.3 / 27.2 |
| 256 | 1.8 / 34.5 | 2.0 / 30.0 | 2.9 / 37.8 |
| 1024 | 4.8 / 85.4 | 5.4 / 67.4 | 8.8 / 93.3 |

Values are medians of per-trial median callback wall times. Median processing-thread cycles per callback fell by 90.1–95.3% in the nine cells. Each corresponding stereo capture covered 192,000 frames per channel. Width to mono matched directly; every paired gate edge in ADi was 16 samples earlier than the reference. After compensating that constant edge displacement the captures agreed to 0.00001. These CPU measurements cover the three settings above. Multiband phase and pan have separate acoustic fixtures.

## 2026-11-19 — ADi-SHAPER Volume, Width and Pan

**PR requested** — `clones/adi-shaper` at `295056b6`. Title **Add ADi-SHAPER multiband Volume, Width and Pan to the Pd tier**. Base `main`. Win opens and lands the PR.

The catalog now exposes ADi-SHAPER. Each of its three serial stages has a full-range path and independent low/mid/high clocks, blend and eight editable curve heights. Beat and Hz clocks support held and linear segments. Each crossover independently selects 6, 12 or 24 dB/octave. The neutral full-range device preserves the input bits and reports zero latency.

The compiled pan law matched both channels at 32 held-out heights within 0.0000002 gain. The largest sample error measured in 20 isolated-band and all-band impulse comparisons was 0.000000382. The reference checks cover five combinations of crossover slopes at 48 kHz. The registered device uses exact musical positions across graph segments, loops, seeks and meter changes. See the editable-Pd clock handoff below for the current host adapter's different timing contract.

The Release MSVC build used /W4 /WX. All four headless suites passed, covering DSP, registered-device state and transport, editable Pd versus native audio, and catalog/host integration. This piece implements Volume, Width and Pan; the eleven-stage measurement contract is below.

## 2026-11-19 — ADi-SHAPER next device increment

On `clones/adi-shaper-modulation` the clones seat takes the shaper's filter and short-delay modulation DSP, `src/adi/dsp/adi_shaper_filter.*`, `src/adi/dsp/adi_shaper_liquid.*`, their dedicated tests and the same ADi-SHAPER device files already reserved. The measured targets are the six low-pass amplitudes at 400 Hz/8 kHz, the modulated flanger delay and the four all-pass stage counts. These additions use the shared per-band curves and crossover path. The first Volume/Width/Pan branch remains independently reviewable.

## 2026-11-19 — Editable Pd timing cases for the devices seat

At 48 kHz and 120 BPM, set a four-quarter-note held Volume curve with point 1 muted and point 3 open. In 2/2, QN 0 and QN 1 both produce `(1,120,2,2,1,1,0)` on the seven-field Pd transport. A patch reconstructing the integer quarter from these values replays point 1 at QN 1. After a full 4/4 bar followed by 3/4, the start of bar 2 is QN 4. Reconstruction from the current meter gives QN 3.

For a sub-block case, use a 1/32-quarter cycle with only point 1 muted. Start playback at sample 64 and split a 128-frame block after 64 frames. The expected open transition is at timeline sample 94. A second delivery of the block-start position at sample 128 repeats the first 30 muted frames. Compare after compensating the editable patch's reported adapter latency.

The catalogued ADi-SHAPER requests the graph's exact position per segment. For editable patches, the devices seat owns the pending quantum/MIDI adapter work. An optional exact musical position anchored to the first input sample of each Pd tick would preserve existing patch bindings. Include upstream latency and a partial tick across a seek or stop in the test. The native device's regression covers QN 0/1, the meter change, and writing a segment at a nonzero buffer offset.

## 2026-11-19 — ADi-SHAPER reference contract

**Target.** The eleven sound-shaping modules in ShaperBox 3.6, each with one to three bands, editable modulation and a reorderable serial signal path. Compressor and Oscilloscope are additional rack tools. ADi-SHAPER uses ADI controls, drawing, names for original sounds and independently generated audio assets.

**Method.** Measurements below use a reference instance at 48,000 Hz, stereo float, 256-frame blocks unless specified, full band and full blend unless specified. The reference editor stayed closed. Constant shapes were set by placing two equal-height points at the ends of a cycle. Let `u` be vertical graph position, from 0 at the bottom to 1 at the top. Input fixtures were stereo DC (0.375 left, 0.25 right), a 400 Hz sine (0.35 and 0.25 peak) and a single 0.5-amplitude impulse on both channels at frame 10,000. Measurements give acoustic output or host-reported latency as indicated. Control descriptions and ranges identified as documented come from the manual supplied with 3.6 and the published 3.6.3 manual. The behaviors below are acceptance criteria for the settings actually tested. [Reference manual](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf)

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
| Liquid | The flanger mixes a short delay with the input. The phaser mixes an all-pass cascade with input. Center and feedback have separate contours. Feedback polarity changes peak/notch placement. Phaser choices provide 4, 8, 12 or 16 all-pass stages. Stereo center offset reaches ±60 semitones. An off-mode impulse matched exactly. In the flanger, center at bottom/middle/top and feedback contour at center put the largest impulse coefficient 2,330/73/2 samples after input, respectively. The corresponding phaser peak positions were 0/1/3. |
| Crush | Independent bit-depth and sample-hold contours, DC bias, dither, hold jitter, pre-filter and treble. Documented ranges are 1–16 bits with full-resolution pass-through at unmodulated maximum, and 8.82 Hz–44.1 kHz with a full-host-rate pass-through setting. At minimum bit depth with dither and bias disabled, a 0.35-peak 400 Hz sine produced silence. The output did not expose simple uniform steps at the tested intermediate settings. Quantizer thresholds, hold phase, dither distribution and the exact transition into bypass remain open. |
| Noise | Adds a noise source with independent level and pitch contours, follow/duck dynamics and high/low filtering. Documented level spans silence–unity, trim −48 to +12 dB, pitch ±24 semitones and initial high-pass 120 Hz. Filtering the detector band changes what triggers the noise independently of the noise spectrum. In a three-second white-noise-only render with the follower off and mono selected, bottom/mid/top contour levels gave 0/0.01113/0.02221 RMS. The two output channels matched sample-for-sample. An empty-source stage passed the test impulse exactly. |
| Reverb | Convolves incoming audio with an impulse response. Separate send and return contours let a closed send leave a tail playing. Controls include pre-delay, decay or gated duration, impulse resampling, stereo wet width 0–200% and optional tail clear at a zero-level hard point. With a 0.4-peak sine burst and three seconds of subsequent observation, normalized blend 0.25, 0.50, 0.75 and 1.00 returned dry peaks of 0.4, 0.4, 0.2 and 0. In this run the selected hall returned no measurable wet tail. Its convolution timing, send/return law, gate and tail-clear behavior therefore remain acceptance work. An independently authored two-tap impulse can distinguish send closure from return closure. |

#### Latency and oversampling

With beat triggering and reserve latency disabled, the host reported 0 samples for Volume, Pan, Width, Time, Filter, Liquid, Crush, Noise and Reverb. Drive reported 16 samples at 48 kHz. Pitch reported 1,687, 2,381, 2,861 and 0 samples for the polyphonic, voice, drum and cyclic modes. These readings describe what the host can compensate. Delay-line effects and intentional signal displacement need separate impulse tests.

The published manual attributes Drive latency to oversampling. At maximum hard clip, a 9 kHz input sine with 0.35 peak produced a 9 kHz output of 0.21915 at 48 kHz. Spectral amplitudes at 3, 15 and 21 kHz were 0.000183, 0.001001 and 0.000655. The reported Drive latency remained 16 samples at 44.1, 48 and 96 kHz. The oversampling factor, filter response and any internal oversampling in other modules remain undetermined. With one Volume stage at 48 kHz, beat sync reported zero with either trigger-smoothing setting. MIDI triggering reported zero with smoothing off and 289 samples with it on. With audio triggering engaged, the first two detector algorithms reported 672 samples, and the complex detector reported 1,152. With smoothing enabled those readings increased to 961 and 1,441. The documented approximations are 14/24 ms plus about 6 ms for trigger smoothing. [Reference manual, latency and lookahead](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf#page=61)

For ADi-SHAPER CPU comparisons, match stage, band, curve, sample rate, block size and input, and record the latency of each device. The earlier full-band Volume, Width and two-stage measurements describe a separate prototype. CPU results for the new device belong with its build.

Remaining numerical ranges include independently shifted formants and the continuous grain window, Drive’s control-to-gain transfer, Filter’s resonance/Q relationship, Liquid’s feedback coefficient, and Reverb pre-delay, decay and impulse stretch. The short Time scales, follower time constants, free-frequency control taper, wet impulse response and internal oversampling factors require further readings. Reference control descriptions are in the module chapters on pp. 10–28 and the clock and trigger chapters on pp. 41–46. [Reference manual](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf)

### Two-track Focus measurement addendum

**Summed waveform acceptance fixture.** A separate 512-column stereo reducer formed A+B at native scale before taking each column’s extrema. Six musical windows matched an independently binned sample oracle to the last bit. Advancing a 137-sample-late duplicate preserved a ±1.60 sum without clipping, and selecting opposite polarity cancelled it to zero. At 1,024 frames per update and 30 updates/s, the standalone sum-and-bin operation used median **0.0153% of one core** (nine runs of 512 s represented audio, range 0.0092–0.0183%). This cost is separate from the broker benchmark and excludes drawing. The integrated UI remains with the Mix View seat.

## 19 November 2026 — Two-track Focus handoff to the Mix View seat

**Review base.** In `ead20df4`, Scope already draws each member’s unwindowed L/R extrema on an audible, common time axis. A level readout computes actual stereo sum peak and RMS. The paint loop draws member paths only. The outstanding visible features are the summed waveform and a non-destructive timing preview. Mix View owns these changes. Its existing director question concerns committing a timing edit. Preview can proceed independently.

| Reference display | What it measures, controls and scale | Evidence |
| --- | --- | --- |
| OScope 2 waveform / stack / sum | Beat grid, loop, previous-cycle context, independent pre/post-roll, Freeze, horizontal/vertical zoom, L/R or M/S split or overlaid. Four Full-edition stack groups, switchable sum and spectral collision view. The sum represents simultaneous sources. Shared spectral energy identifies frequency coincidence without measuring cancellation. | Audija’s [display and edition specification](https://audija.com/Oscope). A newly instantiated 2.0.5 serialized state had a 1/16 grid and alignment grid, source compensation 0 ms, Freeze on stop on, pre/post context off, auto scale off, four sums and four collision switches off. These are observed startup settings. Full entitlement and pixel-level response were unverified. |
| OScope 2 frequency, stereo and loudness | Waveform focus cutoffs affect analysis. Spectrogram and spectrum can use stereo, L, R, M or S. Adjustable spectral tilt/pivot, frequency or piano scales, stereo correlation/goniometer and LUFS history. | Same primary specification and instance state. Observed 1,000 Hz analyzer pivot, tilt setting 3, spectrogram floor −40, 3 active analysis bands with stored 90/45/20 ms windows and 20/15/10 ms hops. Pixel-hop mode was selected, 5 px for each band, so a fixed elapsed hop cannot be inferred. The short-term LUFS switch was on, momentary and integrated switches off. UI cutoff limits, gain normalization, display refresh and filter/correlation impulse responses remain uncalibrated. |
| PsyScope Pro waveform, scale and timing | Individual, overlaid layers, separate stack and arithmetic sum. A sum can sit behind layers or above stack. Beat sync uses the host tempo, custom beats set a cycle, Hz uses reciprocal period. Roll shifts origin, Scroll changes presentation. L/R/M/S and joint LR/MS have separate arrangements. Height scale resets to 1.00; peak fit changes display gain. Per-source and global visual offsets start at 0 samples and combine. Wheel changes 10 samples, fine step 1; numerical entry accepts samples or ms. Region readout gives sample/time endpoints, duration, reciprocal frequency and derived note. Its dB value is the cursor height. | FX23’s [display modes](https://fx23.net/psylab/help/psyscope/waveforms/display-modes.html), [timing and channels](https://fx23.net/psylab/help/psyscope/waveforms/sync-and-channels.html) and [measurement definitions](https://fx23.net/psylab/help/psyscope/waveforms/measure.html). |
| PsyScope Pro phase, difference, spectrum and filter | In fixed view, Layers/Stack correlate the first two enabled sounds’ mono components in slot order, locally in time. Individual and Sum correlate their own L/R. Delta feeds the mono A−B difference to the FFT. FFT size, window, bounds, decay and octave smoothing affect spectral presentation. The visual filter offers LP/HP/BP/AP and dual-edge band isolation with IIR, delay-estimated IIR or linear-phase FIR; the response graph shows delay in samples. AP retains its phase response. Filter audition uses IIR on individual sounds. | FX23’s [phase](https://fx23.net/psylab/help/psyscope/waveforms/cycle-and-phase.html), [delta](https://fx23.net/psylab/help/psyscope/analysis/delta.html), [spectrum](https://fx23.net/psylab/help/psyscope/analysis/spectrum.html), [filter](https://fx23.net/psylab/help/psyscope/analysis/filter.html) and [stereo meters](https://fx23.net/psylab/help/psyscope/analysis/meters.html). The local coefficient window, averaging and exact display bounds/defaults were not independently calibrated. |

**Reference audio path.** Default-state stereo VST3 pass-through measured 204,800 frames/channel, float32, 48 kHz, blocks of 256, with editors closed. The input included equal/opposite channels, displaced impulses and silence. OScope 2.0.5 and PsyScope Pro 1.5.03 both reported zero latency and returned zero peak and RMS error on both channels. This test establishes the audio path at those settings.

**ADI display and preview contract.** Start with two named post-fader sources, unity display scale, common freeze and preview off. Use the intersection of finite, advancing windows with one sample rate and audible transport revision. A seek, source change, or ambiguous loop history invalidates the comparison. Each horizontal bin contains min/max of the samples it covers. Sum L and R sample-by-sample in unclipped floating point *before* binning; use a common gain for A, B and sum and indicate over-range. Level readouts use `20 log10(amplitude)`. Project to `(L+R)/2` for signed track correlation. A positive delay of d means B arrives d samples late; preview B advanced by d and calculate its sum over the remaining common interval. Preserve the unshifted trace, expose polarity as its own control and label the offset in samples and milliseconds. Weak, silent, steady-periodic, search-edge or unstable multi-window candidates keep their measured levels and disable the automatic shift. Preview changes neither track delay nor audio. Release hidden view subscriptions. **Acceptance still due in the owner’s integrated UI:** summed min/max trace including above-unity sums, frozen A/B and preview, seek/repeat-loop invalidation and end-to-end visible CPU.

**Kick and bass.** Two eight-second stereo stems at 48 kHz produced L/R sum peaks of −2.3323/−2.3930 dBFS and RMS of −14.4892/−14.4852 dBFS. Every 4,096-frame batch and the final partial batch matched an independent sample-by-sample stereo oracle exactly for peak and RMS. Six 4,096-frame mid projections starting at 0, 2,048, 6,000, 12,000, 24,000 and 48,000 samples gave three weak and three ambiguous timing estimates. A seeded broadband duplicate 137 samples late gave +1 correlation; an inverted duplicate gave −1 at the same lag. Advancing the copy 2.854 ms recovered the original over the common interval, with polarity corrected separately in the inverted case.

**Reproducible defect.** The signed-only `bestOffset` returned −960 for a pair of 4,096 zero samples with a ±960 search. It returned −800 samples, r = 1, for identical 60 Hz waveforms repeating every 800 samples. The headless preview prototype withheld both. It compares centered, fixed-length intervals for both signs with at least 64 samples, AC RMS ≥ −120 dBFS, |r| ≥ .70 and .05 separation from the next distinct peak. A search-edge winner is ineligible. Treat these as ADI trial thresholds. Verify sign and repeatability across regions before a project edit.

**CPU boundary.** Serial x64 Release strict-warning build; two 256-frame taps, default 1,024-frame Scope windows, 256-frame hop, 30 updates/s and 512 columns for each of the four traces. Nine pairs alternated baseline and active order after warming; each pass covered 96 s of source audio. Baseline mixed the same stems. Enabling tap writes, shared analysis in manual-pump mode, the actual Focus sum calculation and flat-buffer extrema added median **1.774% of one core**, range **1.595–1.888%**, by measured thread CPU / audio duration. Graphics paths, rasterization, scheduling, and the proposed summed trace are outside this measurement. Separately a single fixed 4,096-frame ±960 search on the preview prototype used mean **8.33 ms thread CPU**, median **9.80 ms elapsed**, across 15 warm runs. Compute on changed/frozen regions off the audio thread.

## 2026-11-19 — ADi-SHAPER clock integration

The device requests segment positions from the existing host clock. Additional file claims are the ADi-SHAPER-only opt-in in `src/juce/builtin_host.cpp` and the device's transport conversion in `src/adi/pd_builtins/adi_shaper_transport.hpp`. The graph and the Mix View remain with their owners. A full-band modulation sweep at 0, 25, 50, 75 and 100% showed unity-scaled gain and twice that scale for stereo side gain. Mid remained constant in the width sweep. A mute at half master and half band mix measured 0.75 times the dry input. This product-of-mixes law applies at each active stage.

## 2026-11-19 — ADi-SHAPER measurement contract and file reservation

For the devices seat, the clones lane takes the ADi-SHAPER core under `src/adi/dsp/adi_shaper.*`, the compiled-in external `src/adi/pd_builtins/adi_shaper_tilde.cpp`, the canvas `pd/devices/ADi-SHAPER.pd`, device module `cmake/devices/adi_shaper.cmake` and dedicated shaper tests on `clones/adi-shaper`. Shared edits are the single device's embedding in `cmake/devices/pd_native.cmake`, native factory in `src/adi/pd_builtins/native_devices.cpp`, catalog in `src/adi/builtin_catalog.cpp`, and registration/fixtures in `tests/test_builtin_devices.cpp` and `tests/test_pd_native_devices.cpp`. The first implementation target is the measured gain, stereo width and pan behavior, a clocked editable curve and independent band settings. Additional module measurements follow.

For the Mix View seat, Focus implementation remains yours, including waveform rendering, the sum and the alignment preview. The clones lane is measuring two-track kick and bass and preparing a written handoff here. Preview and Apply are independent. The Apply and audition decisions are already with the director in `mixview/log`.

### ADi-SHAPER first acceptance cases

Reference version 3.6, stereo float at 48 kHz, 120 BPM, 4/4. Use a full-band stage at full mix, zero output trim and no trigger smoothing.

| Setting and input | Measured output |
|---|---|
| One-bar Volume gate, first half at minimum, second at maximum, input L = 0.375 and R = 0.25 | Alternates between silence and the original stereo input. Period 96,000 samples. The observed hard edge was approximately 16 samples after the nominal boundary. Reported latency was zero. |
| Width at minimum on the same input | L = R = 0.3125. This constant render was identical at 64, 256 and 1024 frames per callback. Reported latency was zero. |
| Pan at the hard left and right positions on the same input, reference balance setting | Extremes preserve the corresponding input channel with a gain of √2 and silence the opposite channel. Observed surviving values were 0.5303301 and 0.3535534. Reported latency was zero. |

The documented Volume axis spans silence to unity with half amplitude at its midpoint. Width spans 0–200% with unity side gain at 100%. Pan includes multiple balance laws, combined stereo panning and a separate interchannel delay. The three endpoint cases above are sufficient to test those specific settings. The soft-curve interpolation, interior pan law and crossover response require additional fixtures.

The rack mixes and clocks each band independently. Each stage can add a low and a high band around its starting full-range middle band. Crossover slope choices are 6, 12 and 24 dB/octave. Band blend is scaled by the master blend. A 50% master and 40% band setting yield 20% effective band blend. Phase-locked beat mode follows project position. Sync cycle lengths span 1/128 to 32 bars, including straight, triplet and dotted divisions. Frequency mode spans 0.02 Hz to 5.24 kHz. A MIDI- or audio-triggered one-shot holds its final point. [Reference manual, pp. 25–28 and 41–46](https://downloads.cableguys.com/Cableguys-ShaperBox-3-Manual.pdf)

## 2026-11-04 — REAPER/Reason depth and Pd fit, milestone 11

Research commit 6f09739fffefef35f6e943381669797882963b2a adds 165 entries: 58 REAPER editing, 47 export, 40 Reason rack/Combinator and 20 ADI source-audit findings. REAPER is v0.5 and Reason 14 v0.2. Each entry has evidence, an ADI implication, status and a paired unexecuted case. Six original JSON diagnostic specifications sharpen editing scope, render signal paths, queue revision/publication, macro composition, rack relationships and Pd segment timing. Instruments excluded; FEATURES, decisions and Live parity retained; no new priorities, ADRs or product code.

Useful distinctions: source/take/item/comp and derivative ownership; directed grouped editing and ripple/lock/tempo scope; master-FX-only versus via-master rendering; partial presets, processing/rate/postprocess and output lifecycle; macro domains and discrete targets; graph reparent/duplicate/unwrap; asset and controller scope; feedback/compensation and missing dependencies. Reason Shift-move prose and REAPER repair/pipeline descriptions retain explicit uncertainty.

Correction: M10 R10H079 incorrectly attributed an enqueue-time source snapshot to ADI. ADR0071d5 resolves names at render time; queue.cpp reads a consistent source/range/tempo/name revision at job start. Current notes correct that mapping; ERRATA-11.md supersedes the earlier design-note recommendation. Old bundles remain unchanged. Installed release notes distinguish WITH master FX from VIA master; not a measured result.

The 25 source files match product baseline 5c9e1cd8. At the separate user checkout 9bd55a8e, 7 match, 12 differ and 6 are absent, so this is not a latest-remote implementation claim. The audit identifies lower-level Pd facilities plus normal-loader/parameter/state and nested-rack integration gaps. ADI host owns graph, IDs, mappings, history and assets; Pd/native code handles processing. Canonical native eligibility allows CRLF/LF equivalence, generic WholeBlocks requires every segment to fit its block contract, and feedback/adapter/effect delays remain distinct.

Independent export/Pd review and 102 artifact/source checks passed, including 64 unique local source hashes, exact cases, historical preservation, citation line endpoints and six specification hashes. REAPER 7.79 and Reason 14.1.0.8292 executable hashes unchanged. Guide 7.80 and API 7.83 remain version-qualified. Public-page budgets include retained/new titles and behavior.

E59–E62 retain sources, reviews, corrections, validation and reports. No DAW/plugin execution, GUI capture, audio/CPU, product build/test, new binary-analysis run, app update, push, PR, merge or external message. Separate Claude owns live missions; win's earlier Live questions remain open.

## 2026-11-04 — REAPER and Reason, milestone 10

Research commit b01cc9912d66f310a475157a5404a96508491c90 adds 188 entries: 83 REAPER host, 23 JSFX/ReaScript/developer, 69 Reason documentary and 13 installed static findings. REAPER v0.4 now exceeds the earlier individually referenced scope; Reason 14 v0.1 is new. Each entry has a source, ADI consequence, status and one unexecuted case. Other standalone instruments remain excluded. Current FEATURES, decisions and Live parity remain authoritative; no new priority, editor/runtime, ADR or product code.

Installed identities: REAPER 7.79 and Reason 14.1.0.8292; Reason registration separately says 14.1.0d100. All 55 new module hashes and 272 prior binary hashes remain unchanged. REAPER manual 7.80 is newer; 25 Reason 14/Rack Plugin 14 web sources and seven current Cockos developer pages retain version boundaries. The guide’s routing/capacity/pooled-copy/render examples contain contradictions, preserved as tests rather than adopted rules.

Useful ADI contracts: render dependencies, queue snapshots and postprocessing; source/comp/edited-result ownership; nested-rack and project boundaries; generated-MIDI capture without reprocessing; batch/feedback history; parameter display, automation, state and undo separation; shared asset services and timestamp-preserving controller adapters. Reason’s Inspector/folders/rack cables do not reopen ADR-0047/0044/0113. Existing Pd/native and MIDI-lane work from milestone 9 remains the implementation starting point.

Binary inspection covered the REAPER wave module entry point and selected Reason REX rate, tempo and preview-batch wrappers. These establish admission/registration/validation interfaces, not complete mixers, scheduler, numerical precision or CPU. Raw dossiers, manual derivatives and shipped source remain private under E55–E58. Independent reviews and 34 artifact checks passed; source hashes, exact case pairing, history/scope and six original input specifications were checked.

REAPER-REASON-MISSIONS-10.md has 188 unexecuted cases; no GUI/audio/CPU, product test/build or reference app/device execution. The separate Claude session owns live tests. Earlier Live L01–L10 remain unresolved for win’s measured-evidence relay. No app update, push, PR, merge or external message.

## 2026-11-04 — Bitwig MIDI, Grid and Pd feasibility, milestone 9

Research commit `c532d0597c64c3720ae2fb75bcc265729eeb9a30` adds 173 source-bound entries to Bitwig v0.5: 59 Grid, 71 MIDI/Note FX/modulation/container and 43 non-instrument audio-device contracts. Every entry has a source, ADI implication, status and a paired unexecuted case. The complete local v5.2 Grid chapter and module catalog, MIDI/device chapters and narrow 6.0/6.1 overlays retain their version boundaries. Installed 6.1.0.22652 behavior is unmeasured; other built-in instrument devices remain excluded. Grid primitives and Poly Grid shared host/voice semantics are included only as needed for the modular architecture.

The feasibility recommendation is native host ownership of event/note/voice identity, scheduling, modulation routes, state/undo and latency, with existing Pd authoring/runtime and selected registered native DSP. This preserves ADR-0145/0177/0188/0203/0207, including immutable parameter IDs, vanilla-compatible parameter export, selected direct-native zero-adapter-delay execution, and the decided precision policy. Grid stereo 4× signals, host-rate mono modulators and current Pd scalar parameters at 64-sample boundaries are distinct. No arbitrary Grid preset import, arbitrary Pd conversion, complete voice bridge or replacement editor is claimed.

The code audit compares baseline 5c9e1cd8, user checkout 9bd55a8e and the separate codex/midi-effects c130f0e7 lane. That lane supplies fourteen native MIDI processor candidates and event-transform hooks but is not an ancestor of the baseline: reconcile it before designing another event architecture. Pd runtime/parser/sample/latency components exist; the inspected loader and PdDevice parameter/state/event interfaces do not establish an integrated user-loadable patch tier. Twelve likely Pd lane refs are baseline ancestors; unseen remote/ignored ORC-VST work remains outside the audit. Full report, 33 capability assessments and 58 hashed source snapshots are indexed at E54.

Independent reviews refined voice lifetime, feedback units, audio-modulation filtering, conversion makeup, signed track delay, signal-rate wording and portability. Thirty-four artifact checks passed; 75 local artifacts were hash-verified. Source metadata spans 189 distinct manual page numbers, not 189 certified complete feature pages. All 272 inventoried installed binary hashes remain unchanged. BITWIG-GRID-MISSIONS-9.md contains 173 cases; ten original JSON diagnostic specifications explicitly remain NOT RUN. No GUI, build, product test, audio/CPU, new binary inspection, product change, app update, push, PR, merge or external message. Existing Live open questions remain unresolved for win's measured-evidence relay.

## 2026-11-04 — Drum Rack, Packs and legacy variants, milestone 8

Research commit `8955b800a4d994208ede5c97e4fa1299bcc1e06a` adds 66 entries: 25 Drum Rack, 28 Pack-effect, 12 legacy and one installed-location boundary. Drum Rack is explicitly included; other instruments remain excluded except Sampler/Simpler. Eight Pack families have primary-source capability notes; these do not establish complete controls or installed versions. The default Factory Packs directory exists but is empty and Library.cfg's preferred path is blank. Other locations remain unresolved.

Read local Rack chapter pp.471–490, ten primary web sources, and structures of eight hash-bound legacy presets. Every entry links an ADI implication and an unexecuted case in PACK-RACK-MISSIONS-8.md. ADR-0060/0046/0114 review preserves ADI nested-graph latency/tails, open macro ordinals and persistent-routing/nonpersistent-output semantics; Live parameter takeover remains a reference difference. Historical Frequency Shifter location wording conflicts with the later installed legacy preset family and stays qualified.

Ten artifact checks passed; a final ADI review narrowed the macro comparisons. No GUI/audio/CPU measurements, product changes, app or Pack updates, push, PR, merge or external message. There was no further binary inspection. Exact Pack inventory, legacy migration, complete control/default/text/history registries, DSP and CPU remain open. All original window questions remain unresolved for win's measured-evidence relay. Raw resources stay private; authored notes are versioned.

## 2026-11-04 — Ableton effects and named samplers, milestone 7

Director expands scope to audio/MIDI devices, explicitly including Sampler and Simpler while continuing to exclude other built-in instruments. Research commit `b4c52f0904845f6ff9d18f7f82b0aa8741756aeb` on `clones/daw-reference-reading-20261104` adds 358 contracts across 62 named manual devices: 42 native audio effects, four Max audio effects, eight native MIDI effects, six Max MIDI effects and the two samplers. Every row retains source, ADI implication, evidence status and a paired unexecuted case. Historical scope exclusions remain historical; four stable Simpler index entries are explicitly reopened. No product code, FEATURES, ADR or priority changed.

A7A/A7B cover 201 audio-effect contracts, A7M adds 131 MIDI/sampler contracts, A7R adds 17 preset/schema/Push findings and A7N records nine bounded native observations. Useful distinctions include generated-note identity/expression, trigger-time sample selection, zone and round-robin state, media reversal/conversion, modulation versus parameter ownership, local/global history, detector/tail/quality modes and parameter display/controller/storage domains. Current ADR-0169/0187 priorities remain; ADR-0203 d7 randomness and ADR-0207 d2 precision are preserved.

Binary inspection covered the Corpus CalcA and SetSampleRateU routines and selected Amp exports. Evidence includes local scalar/packed float work and unresolved dispatch/quiet/ramp semantics; no whole-engine precision, DSP equivalence or efficiency claim follows. Seven presets and selected schema fields are static resource observations, not factory defaults or hard UI/DSP bounds. All raw proprietary code/resources remain private under E43–E47.

Independent reviews narrowed Looper first-click timing, Chord's inter-note Strum limit, locale restoration, unresolved dispatch meaning and target-ID evidence. The 62-device census retains open control/default/text/history/state/signal/CPU dimensions; 39 source-body subsections absent from the older TOC have separate links. A bounded installed audit found Expression Control Legacy and eight legacy native families beyond the modern manual list, and distinguished 1,225 presets/racks from processor counts. Installed Pack-effect inventory remains open; no Pack root was established.

DEVICE-RESEARCH-MISSIONS-7.md contains 358 future cases plus nine device censuses. Thirteen original audio/MIDI fixtures have hashes and independently checked structure/event sequences, with all host results NOT RUN. All earlier host censuses and L01–L10 remain open; no measured result is ready for win's relay. GUI testing remains assigned to the separate Claude session by the supplied brief.

All 46 artifact checks passed. Thirty local source artifacts were hash-verified, including ten manual files covering 214 distinct printed pages; two primary web references retain version/retrieval boundaries. All 272 inventoried installed binaries remain unchanged. No DAW GUI/audio/CPU or product test was run. Local manual revision, installed 12.4 behavior, preset creator 12.4d1, historical Live 11 and current MPE documentation remain distinct. No push, PR, merge or external message.

## 2026-11-04 — deeper Ableton base reference, milestone 6

Director requests coverage beyond the named examples across the Live host, still excluding built-in instrument/effect operation and implementation. Research commit `cfef0a8a1c93ac02ce8ba7b373766c0dfee18f17` on `clones/daw-reference-reading-20261104` adds 445 research contracts: MIDI 92, clips 86, routing/recording/files/Racks 73, controllers/video/accessibility/settings 63, static template/theme/schema 24, release 12, public API 20, tuning formats 14, remaining host/resource/timing 13, editor extension/Stacks 19, current Push 15 and synchronization/backend/video 14. Each retains four evidence columns and a paired unexecuted case. No product code, ADR or priority changed.

The useful boundaries include note/clip identity and collisions, source/beat/sample time, preview/apply versus playback randomness, UI/API/controller command scope, recording and automation ownership, typed routes, library/project/profile state, format validity versus ADI policy and loss during interchange. Independent reviews narrowed analysis-readiness, multi-clip export, pad replacement and API citations, removed a duplicate fade row and corrected general-history, tuning, precision and service/local-policy mappings. Manual shortcut, Humanize, Swing, preset-save and probability ambiguities remain explicit.

The 610-entry contents inventory keeps stable IDs and links 373 entries to exact new contracts; these are navigation links, not completed-feature counts. Four Simpler-operation entries are now explicitly excluded. Private E37–E42 include source manifests, coverage/gap review, LIVE-BASE-MISSIONS-6.md with 445 cases and ten completeness censuses, and 24 original tuning/Stacks fixtures whose runtime results remain NOT RUN. Current FEATURES/decisions/parity govern; Dream remains director context. The initially identified documentary gaps received additional primary-source work; remaining hardware/API adoption and state behavior stay open.

All 46 artifact checks passed: schema/IDs, four-column tables, exact case pairing, source integrity, ADR existence, earlier-content preservation, scope and unmeasured status. 67 local artifacts were hashed, including 42 manual text files covering 615 distinct printed pages. 29 primary public references are recorded; 8 unavailable raw snapshots have no fabricated hashes. All 272 inventoried installed binaries remain unchanged. This is research-artifact validation, not product CI or host behavioral verification.

No new DAW GUI, audio or CPU measurements. The supplied brief assigns live testing to the separate Claude session; all original L01–L10 questions remain unresolved for win's evidence relay. Local manual revision is unconfirmed versus installed Live 12.4; API 12.4.5, ASCL 12.1 and release-specific changes remain version-qualified. Static template/theme creators are older and establish neither factory defaults nor the on-screen palette. Further documentary gaps and runtime work are classified separately. No push, PR, merge or external message.

## 2026-11-04 — deeper Live host, VariAudio and workspace research, milestone 5

Director explicitly adds Cubase VariAudio, detachable/multi-screen elements, extensive shortcut customization and general preferences, with Ableton still the primary reference. Research commit `5e2401301bc35c6ecdb09d3946ba8eb4910055bb` on `clones/daw-reference-reading-20261104` adds 172 documentary contracts: Live 107 (A5C/A5H/A5W), Cubase 65 (C5V/C5W). Each has behavior, source, ADI implication, status and a private measurement case. No product implementation or ADR change.

Cubase's 30 VariAudio rows cover analysis/segmentation, pitch and timing edits, formants/gain, scale/chord input, MIDI extraction, reset/reanalysis/flatten and interaction with offline processes. The guide's inconsistent flatten scope remains unresolved; shared-source propagation and exact storage/undo still require evidence. Its 35 workspace/settings rows distinguish editor windows, lower zones, linked mixers, focus, project/global workspaces, profiles, command bindings, macros and preference ownership. Multiple windows do not imply arbitrary panel detachment. Settings can change shared-clip processing, MIDI split behavior and group-mute restoration. Monitor disappearance/DPI recovery and macro failure/undo remain open.

Live receives 49 mixer/freeze/clock/Session/engine/recovery entries, 40 browser/file/default/device/hosting entries and 18 keyboard/window/settings entries. Findings separate containment from routing, frozen derivatives from editable source, file replacement from external editing, parameter values from plug-in configuration, A/B banks from preset storage and visible Undo History from disk recovery inputs. The manual's shortcut contradictions remain explicit. Independent review narrowed clip destination, group-color command, freeze editing and grouped-stem scope, corrected recovery-guide attribution, and qualified Cubase reanalysis as conditional.

Private E34–E36 contain 172 authored contracts, source manifests and DEEP-HOST-MISSIONS-5.md. All 25 artifact checks passed: row IDs/table structure, exact mission pairing, prior-content preservation, ADR references, documentary status and source integrity. Thirty-three distinct local source files were hash-verified; two official Ableton support references are recorded without fabricated snapshot hashes. All 272 inventoried installed binaries remain unchanged.

No new DAW GUI, sound or CPU measurements; the separate Claude session owns live testing under the supplied brief. Cubase manual 15.0.30 versus installed 15.0.5.114 and unconfirmed Live manual revision versus installed 12.4 remain explicit. All ten original Live window questions and the 610-entry Live base inventory remain open at complete behavioral-verification level. Win relays actual returned evidence into original measurement questions. No push, PR, merge or external message; these checks are research-artifact validation, not product CI or a claim to understand the whole applications.

## 2026-11-04 — export, Bitwig UI and complete Live baseline scope, milestone 4

Director asks specifically about Cubase export, Bitwig selected-track auto-zoom and complete Ableton coverage as the base reference. Research commit `a8a96d8c` adds 49 documentary contracts (Cubase 19, Bitwig 6, Live 24), the full Live host/integration contents inventory and an updated prompt/coverage pointer. Live's A-BASE register accounts for 610 host/integration entries out of 871 contents entries; the remaining 261 are device implementation/credits material. Counts include parent sections/examples and do not represent independent features, body text fully read or completed measurements.

Cubase export now distinguishes Quick Export, eligible channels, range sources, processing taps, file naming/conflicts, queue settings/cancellation, format/channel/metadata choices, post-export actions, AAF/OMF/DAWproject, ADM and video. The earlier active-marker-track implication is corrected: Audio Mixdown has its own marker-track chooser. Manual inconsistencies about video resolutions, FLAC depths and MP3 bitrate units remain explicit verification gaps.

Official Bitwig 6 documentation confirms Auto-zoom (Shift+Z) for a selected track or automation lane and resizing the selected item to set zoom size. The v5.2 half-height description is marked historical. Selection multiplicity, normal-height restoration, group/filter interactions, state ownership and undo are still unmeasured. Live's new findings distinguish consolidate, individual/group bounce, paste-time rendering, export, session-only Undo History, linked edits, recording, envelope composition/time domains and accessibility.

Private E30–E33 retain source hashes, contracts, public Bitwig snapshots, the topic inventory and 22 passing artifact checks. All 272 inventoried installed binaries remain unchanged. BASE-REFERENCE-MISSIONS.md supplies export, Auto-zoom and full Live host-audit cases; no GUI, sound or CPU measurements occurred. All ten original Live questions remain open. No product code/ADR changes, push, merge or external message. The complete-base scope is established; complete-base behavioral verification remains unfinished.

## 2026-11-04 — whole-Dream research, milestone 3

Director clarified that the named audio-editing, warp/MIDI and modulation cases are only examples: the research scope is every intended Dream/current ADI feature. Research commit `3ac99a35` on `clones/daw-reference-reading-20261104` adds a 220-requirement register spanning all 15 Dream sections, an independent index of all 319 FEATURES table data rows including metadata, and an integration/contradiction document. This is coverage accounting, not 220 completed contracts. Current repository decisions govern implementation; Dream expands research without overriding them.

Per-app v0.3 notes add 89 source-backed rows: Live 32, Cubase 30, Bitwig 22 and five REAPER rows only for explicitly referenced behaviors. New detail covers launch/follow timing, monitoring and compensation, automation ownership, media/browser/transfer, tuning/articulation, racks, arranger/time domains, mixer/cue recall, controllers, hosting, modulation sources and Grid interfaces. Built-in instrument/effect implementations remain excluded. The earlier chapter-level exclusion of all Bitwig chapter 19 was too broad: modulator and Grid interface semantics in §§19.27–19.28 are included. The Score Editor feature/exclusion conflict is recorded rather than silently resolved.

Private E24–E29 evidence includes nine pinned/datestamped primary SDK/format/backup snapshots; structural summaries of 25 shipped Live XML artifacts; twelve installed Bitwig API documents; and a hash-bound reading manifest covering 36 source files. Protocol findings cover gestures, expression identity, process/flush, ARA ownership, interchange, backup and LinkAudio availability. They do not establish installed engine internals or authorize ADR changes. Raw sources were retained privately.

WHOLE-HOST-MISSIONS.md adds ten reference-test families and an ADI-only future acceptance matrix, supplementing both earlier mission documents. No GUI, audio or CPU measurements were performed; all ten original Live window questions remain open for Claude evidence and win relay. AI/collaboration, telemetry, remote hosting, mobile and ADiJ are explicitly tracked as independent ADI contract work where reference parity is unsupported.

Validation: 37 research-artifact checks passed, including source/reference integrity, all 272 inventoried installed binaries unchanged and 74 inspected source artifacts unchanged. No product code, ADR, build or CI changes; no push, merge or external message. Full-host research remains incomplete. Continue from the per-requirement open dimensions and ingest actual test results into the exact app/build rows.

## 2026-11-04 — expanded host research, milestone 2

Director expanded the assignment to all host behavior, prioritizing Cubase F7 DOP/advanced fades, Live warp/MIDI and Bitwig modular/modulation semantics. Built-in effect/instrument implementations are excluded; editor utilities, hosting, routing and Grid semantics remain included. Research commit `905556ef` on `clones/daw-reference-reading-20261104` adds 127 detailed contract rows (Cubase 45, Live 43, Bitwig 39), per-app remaining-scope queues and an expanded dated prompt. No product code or ADR changed; nothing pushed or merged.

Evidence distinguishes local manuals from installed behavior: Live manual revision unknown, Bitwig manual 5.2 supplemented by official 6.0/6.1 release notes, Cubase manual 15.0.30 versus installed 15.0.5. Read-only inspection added structural summaries of 25 shipped Live XML Sets, 11 installed Bitwig API documentation files and verified Cubase DOP-label instruction references. These do not establish complete schemas, engine architecture or runtime behavior. Raw artifacts were retained privately.

Private `DESIGN-IMPLICATIONS.md` maps source findings to existing ADI decisions. `HOST-COVERAGE.md` inventories 187 source chunks, including 162 host chunks; 26 have selected contracts recorded, none comprehensively measured. The separate Cubase Score Editor source is missing. `DEEP-CLAUDE-MISSIONS.md` defines nine missions for ownership, apply/cancel, fades, range edits, warp, MIDI, modulation/Grid, audio events/operators and host export. Eight synthetic MIDI fixtures supplement the 66 WAVs. No Codex GUI testing or DAW sound/CPU measurements; all ten Live window questions remain open for Claude evidence and win relay.

Validation: existing 26 checks and 28 expanded artifact checks passed; all 272 inventoried installed binaries and 223 inspected resources/manual chunks unchanged; eight MIDI fixtures independently parsed. This is research-artifact validation, not product CI. Full-host completion is not claimed. Next work follows the explicit coverage queue and returned Claude measurements; no implementation is authorized by this research milestone.

## 2026-11-04 — director-assigned DAW reference reading, milestone 1

Research only. Current main baseline `5c9e1cd8`; the director clarified that GitHub project documents govern and Dream Reference v0.2 is personal context. An isolated item branch `clones/daw-reference-reading-20261104` contains commit `ff7a52a8c272608aa82925a17cbb9eb530e432d4`: five app files under `docs/daws/`, the dated reference-reading prompt and a claim. This log stays on `clones/log`. Nothing pushed, no PR or merge, no external messages.

Binary inspection covered selected Live, Bitwig and Cubase modules, including Bitwig FFT/window wrappers and Live RWDEF exports. Raw evidence, projects and diagnostic scripts were retained privately.

Installed identities: Live Suite 12.4 build 2026-04-24_d85a94ab5e; Bitwig 6.1.0.22652, revision 162652; Cubase 15.0.5.114. SHA-256 recheck: 272 inventoried binaries, zero changed. Local Bitwig manual is 5.2; Cubase manual is Pro 15.0.30. The docs retain those mismatches and never promote them to measured installed behavior.

No GUI interaction, DAW sound measurements or CPU benchmarks. Sixty-six deterministic audio fixtures and a residual analyzer are prepared; identical and polarity-inverted self-checks passed. Controller 7-bit colours are explicitly not substituted for the 70 measured UI colours. None of the ten normalized Live window questions was closed. `CLAUDE-MISSIONS.md` in the private root specifies exact starting states/actions/evidence/acceptance/final states; win must relay actual returned measurements into the original window log and awaiting table.

Validation is limited to research artifacts: table structure, evidence/fixture hashes, successful provider results and analyzer checks. No product code changed; the full ADI build/CI suite was not run and no merge-readiness claim is made. Next: Claude's Live UI/automation/undo missions, then initial 48 kHz/256 audio probes. Product implementation still requires the director's explicit instruction.
