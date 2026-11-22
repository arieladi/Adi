# devices — log

The devices lane (ADR-0208; `docs/LANES.md` §4b). Newest entry first.
Device branches start at `origin/main`; win reviews and merges. This log has
its own `devices/log` branch, following `collab/engine.md`, so independent
device PRs do not conflict on one shared log file.

## FOR THE DIRECTOR

**The rest of Live 12.4's audio effects chapter (DEV-22; asked 22 November).** With the standing queue
built, the catalogue holds 31 of the chapter's audio effects. Eleven are missing: Amp, Cabinet,
Corpus, Drum Buss, Dynamic Tube, Grain Delay, Looper, Pedal, Roar, Spectral Resonator and Spectral
Time. Beat Repeat and External Audio Effect are left out of the count; they are covered elsewhere.
- **Where things stand.** PLAN DEV-22 says the queue follows the manual's order and is checked against
  the chapter. win's check of 20 November named three gaps (Auto Shift, External Audio Effect,
  Spectrum), and those are built now. Nothing names the eleven, and nothing excludes them.
- **Recommendation:** they are in DEV-22's queue, built after 3 December in the manual's order (Amp
  first), each "built, not verified" with its reading on the ONE list.
  - Looper stays out until the engine's recording and transport rows: its core is the engine's, as
    External Audio Effect's routing is.
  - This seat built them in the manual's order, each on its own branch: Amp, Cabinet, Corpus, Drum
    Buss, Dynamic Tube, Grain Delay, Pedal, Roar, Spectral Time and Spectral Resonator. If the answer
    is no, they simply wait.

**The Live readings — the one list (ADR-0248 d8, d14).** Readings 1 to 18 are for the director's
session; 19 to 44 (the post-3-December effects and kernels) may come in the same sitting or later. For the director's own Claude session on
the PC with Live 12.4, in one sitting, from win's prompt. This seat does not drive Live. Each item
is one device, one question. Live 12.4 at 48 kHz, a fresh Set, the device dragged from the browser
(its default), nothing else on the track unless said. Bring back the statement (value, conditions,
method); Live's files and renders stay in the director's research.

1. **Saturator, High Quality at Dry/Wet 50 % (DF-E37 a; blocks #659).** Default Saturator (HQ on),
   Dry/Wet 50 %: render an impulse of 0.5, 100 ms. The first 64 samples' values, and the level of a
   10, 15 and 18 kHz sine through it.
2. **Shifter after a pitch move (DF-E37 b; blocks #660).** Pitch mode, Window 80 ms, Dry/Wet 100 %:
   automate Coarse to +7 st for 40 ms then back to 0, then an impulse of 0.5 one second later.
   Where the impulse comes out and at what level (one peak at 1,920, or two of half?).
3. **Simpler's loop through the release (DF-E37 d).** Classic, Loop on, a 2 s sample with Loop
   Length 5 % (100 ms), Release 1000 ms: a note held 1 s then released. Does the output keep
   looping and fade over the second, or stop at the loop's end?
4. **Arpeggiator style order (DEV-19).** A MIDI clip holding C3 E3 G3 B3 for one bar; Clip View,
   Transform, Arpeggiate, Rate 1/16, Steps 0, Gate 50 %: for each of the 18 styles the 16 pitches
   in order. Then C3 E3 G3 B3 D4 for Converge, Diverge, Con & Diverge, the Pinky and Thumb styles;
   and Up with Steps 1, Distance +12.
5. **Simpler's voices, velocity, envelope and filter (DEV-21).** The Voices chooser's list. Classic,
   a 1 kHz sine sample, Vel > Vol 35 %: peak level at velocities 1, 32, 64, 100, 127. Attack 100 ms
   at slope 0, +1, −1: level at 25, 50, 75 ms. Filter on, Clean, LP 12, 1 kHz, Resonance 0 % and
   100 %: noise through it, the level at 500 Hz, 1 kHz, 2 kHz.
6. **Default values not in Live's preset library (DEV-22).** A fresh Redux, Echo, Resonators,
   Multiband Dynamics and Vocoder: every control's value as shown. (Echo's dry impulse is 0.300 and
   Resonators' 0.500 at their defaults, which no Dry/Wet law explains without these values.)
7. **Saturator's Width, Curve and Period (DEV-22).** Waveshaper mode, Drive 20 dB, a 100 Hz sine at
   −6 dBFS: Width 0 / 0.3 / 1, then Curve 0 / 0.05 / 1, then Period 0 / 0.5 / 1, the others at
   default; for each render the first five harmonics' levels.
8. **Shifter's dry path and sweep phase (DEV-22).** Pitch +7 st, Dry/Wet 50 %, an impulse: is the dry
   impulse at 0 or delayed with the wet? Then the first 2,000 samples of the sweep render at +7 st.
9. **Delay with Feedback 0 (DEV-22).** The default Delay with Feedback 0: the six-tone multitone
   (71, 223, 997, 3001, 7001, 11003 Hz) render, to settle its filter's exact shape.
10. **Chorus-Ensemble's Delay Time list (DEV-22).** The chooser's six entries in order.
11. **Envelope Follower and Shaper (DEV-07).** Envelope Follower mapped to a Utility Gain, Rise and
    Fall 0 then default, a 1 kHz burst of 100 ms: the gain over time. Shaper at its default shape,
    1 Hz, mapped the same way: one cycle.
12. **AU05, automation moved or deleted with its material (DF-E38 a; the engine's).** The value
    left inside a source interval a Move or Delete vacates (the held value, the lane default, or a
    new join); how a paste onto another track maps device and send lanes with no counterpart; a
    move across tempo steps between ticks and samples.
13. **Crop (DF-E38 b; the engine's).** Whether the selection is read in source or arrangement
    coordinates; whether Crop keeps the clip's span and moves the source window, or trims the span;
    what becomes of a note crossing the edge (its length and expression).
15. **Reverb's and Hybrid Reverb's controls of another shape (DEV-22).** A fresh Reverb, as shown:
    Spin's Amount; the input filter's Lo Cut and Hi Cut (the band's frequency and width, or its two
    edges); Diffusion and Scale; the Density chooser's entries and which is selected; the Size
    Smoothing chooser's entries and which is selected. A fresh Hybrid Reverb, as shown: Routing,
    Predelay, the synced predelay value, Decay and Size. And for Hybrid Reverb: the delay it reports, and whether an IR at another sample rate keeps the level of a click on its first sample.

16. **Redux's 64 samples (DEV-22).** A fresh Redux, an impulse of 0.5: where it comes out and the
    first 256 output samples. The research's table says Live reports 64; this shows what the 64
    buys (a filter's shape), before ADI copies the delay.
17. **Compressor, the queue's first new effect (DEV-22).** A fresh Compressor: every control as shown
    (Threshold, Ratio, Attack, Release, Auto Release, Output, Makeup, Knee, Lookahead, the
    Peak/RMS/Expand mode, Lin/Log envelope, Dry/Wet). At those defaults: a 1 kHz sine in 6 dB steps
    from -48 to 0 dBFS, 500 ms each, for the static curve; a 1 kHz sine at -6 dBFS switched on for
    300 ms after silence, then off, for attack and release; an impulse of 0.5 for the delay.
18. **Beat Repeat (ADR-0249 d7; it is built after 3 December, its reading may come now).** A fresh
    Beat Repeat at 48 kHz, every control as shown. Then feed it independently seeded, uncorrelated
    left and right markers (noise, a different seed per channel), so that every frame of each
    channel can be told apart (the clones seat's method; `pd/devices/Beat Repeat.md` holds the same
    items):
    - **capture direction:** the markers immediately before and after the capture point. Does the
      slice start at the trigger frame or end there, and does the trigger frame belong to the slice?
    - **a Grid change while repeating,** at a marked frame: the first slice it affects, and where
      playback resumes;
    - **fractional phase:** at 117 BPM and Grid 1/16 (a slice of 6,153.846 samples), the first frame
      of each of 16 successive wraps, to tell a carried fractional phase from per-slice rounding;
    - **the join:** across a wrap between two marked levels, the transition's length and weights;
    - **locate:** while it repeats, including a jump back to the same position: which pre-locate
      frames it can still play;
    - **the oldest frame:** at 20 BPM with the longest Interval and Grid, the oldest input frame it
      plays, in frames and seconds, at 44.1, 48 and 96 kHz;
    - **the meter:** in 7/4, does 1 Bar span seven quarter notes? And what does 4 Bars do at
      20 BPM in a long meter (shortened, or played in full)?
    - **a held Repeat:** held for a minute, does it keep playing the same slice?
19. **Glue Compressor (ADR-0249 d7; built after 3 December).** A fresh Glue Compressor, every control
    as shown: Threshold, Ratio, Attack, Release (and Auto), Makeup, Range, Dry/Wet, Soft Clip.
    - At those defaults, a 1 kHz sine in 6 dB steps from −48 to 0 dBFS, 500 ms each, for the curve
      and its knee.
    - A −6 dBFS burst of 2 ms and one of 10 s, each followed by silence, at Release Auto: the
      reduction over the 2 s after each, for Auto's law.
    - Soft Clip on, a 1 kHz sine at +6 dBFS: the output's peak and its first five harmonics.
20. **Gate (ADR-0249 d7; built after 3 December).** A fresh Gate, every control as shown: Threshold,
    Return, Attack, Hold, Release, Floor, Flip, Lookahead.
    - A 1 kHz sine stepping −50 → −30 → −42 → −50 dBFS, 500 ms each: when it opens and closes
      (the hysteresis), and the gain's shape over Attack and Release.
    - A 10 ms burst with Hold 100 ms: when Release starts.
    - Flip on with Floor −20 dB: the gain open and closed.
    - Lookahead 10 ms: the delay it reports.
21. **Limiter (ADR-0249 d7; built after 3 December).** A fresh Limiter, every control as shown: Gain,
    Ceiling, Release (and Auto), Lookahead, Stereo/L/R/M/S, the mode.
    - The delay it reports at each Lookahead.
    - +12 dB white noise: the output's sample peak and true peak in each mode.
    - Soft Clip: a 1 kHz sine at −20 dBFS, then at +6 dBFS, the output's peak and its first five
      harmonics.
    - M/S with a hard-left signal over the ceiling: the peak on each channel.
22. **LFO (DEV-07's device side; built after 3 December).** A fresh LFO, every control as shown
    (its shapes, Rate and its synced choices, Phase, Offset, Depth, Jitter, Smooth, and any other).
    - Mapped to a Utility Gain at 1 Hz: one cycle of each shape.
    - Jitter at 50 % and Smooth at 50 % on the Square: two cycles each.
    - Synced at 1/4, 120 BPM: a locate to bar 3 beat 2, the value at the locate and whether the
      phase is the song's or restarts.
23. **Align Delay (built after 3 December).** A fresh Align Delay, every control as shown.
    - An impulse: where it comes out in Time 10 ms, Samples 123, and Distance 3.432 m at its default
      temperature, at 48 kHz.
    - Whether a fractional delay is interpolated or rounded to a sample.
    - Whether Live reports the delay as latency.
    - What a changed delay does to audio in the line.
24. **Auto Pan-Tremolo (built after 3 December).** A fresh Auto Pan-Tremolo, every control as shown,
    in both modes.
    - A 1 kHz sine at 1 Hz, Amount 100 %: each channel's level over one cycle, at Phase 180 and 90,
      in Pan and in Tremolo.
    - Shape at 50 % and 100 %.
    - The level at the crest (unity, or less).
25. **Channel EQ (built after 3 December).** A fresh Channel EQ, every control as shown.
    - Pink noise through each band at its extremes, and through the low cut: the response, to settle
      the corners, slopes and the bell's Q.
    - Whether the Mid band's Q changes with its gain.
26. **EQ Three (built after 3 December).** A fresh EQ Three, every control as shown.
    - Pink noise with each band killed in turn, at 24 and at 48 dB/oct: the response, to settle the
      crossover family.
    - The gain law near its low end.
27. **Tuner (built after 3 December).** A fresh Tuner, every control as shown (the Reference range
    and default, the display modes' names).
    - A 440 Hz sine, then 442 Hz: the note and cents shown.
    - After the sine stops: how long the reading is held, and what it shows then.
28. **Erosion (built after 3 December).** A fresh Erosion, every control as shown.
    - Sine mode, Frequency 5 kHz, Amount at its top, a 1 kHz sine: the levels at 1, 4 and 6 kHz,
      to settle the depth law.
    - Noise and Wide Noise on a 1 kHz sine: the spectrum, and whether the channels differ.
29. **Vinyl Distortion (built after 3 December).** A fresh Vinyl Distortion, every control as shown.
    - A 1 kHz sine at −8 dBFS with Tracing at three drives: the second and third harmonics.
    - Pinch, Soft and Hard, Stereo and Mono: the 3rd and 5th harmonics, and the mid and side.
    - Crackle at full volume: the clicks' count in 10 s, their peak and decay, and their image.
30. **Filter Delay (built after 3 December).** A fresh Filter Delay, every control as shown (each
    line's Freq, Width, time and pan).
    - An impulse through one line, its band off, then on: the echo times and the band's shape.
    - Where the band sits (on the input or in the loop); a time change: a glide or a jump.
31. **External Audio Effect (the engine's build, after 3 December).** A fresh External Audio Effect, as
    shown: Gain's range and default, Hardware Latency's range and steps, the Audio To and Audio From
    lists. With a loop-back cable and Dry/Wet 50 %: does the dry meet the return, or arrive early?
32. **DJ Filter (built after 3 December).** The DJ Filter as Live 12.4 shows it: its controls and
    ranges, and whether it has a resonance of its own. Pink noise at Filter −50 and +50: the response.
33. **Auto Shift (built after 3 December).** A fresh Auto Shift, every control as shown (its scale
    list, the Strength and Smooth ranges, the formant and vibrato sections).
    - A 446 Hz sine through it at its defaults: the output's pitch, and how fast the correction comes.
    - Its reported latency.
34. **Spectrum (the device's controls, after 3 December).** A fresh Spectrum, every control as shown
    (Block's default, Refresh's and Avg's ranges, the Graph and Scale choices). Avg 4 on a tone that
    starts: how the reading rises over the first analyses. How long Max holds a peak.
35. **Amp (built after 3 December).** A fresh Amp, every control as shown: the Amp Type list, the
    knobs' ranges, Dual's default. Per type, a 220 Hz sine at −6 dBFS at Gain 0, 5 and 10: the first
    nine harmonics. A −60 dBFS sine swept from 50 Hz to 10 kHz at the defaults: the response.
36. **Cabinet (built after 3 December).** A fresh Cabinet, every control as shown (the cabinet and
    microphone lists, the mic types, Dual's default). Pink noise through each cabinet with each
    microphone: the response from 40 Hz to 16 kHz.
37. **Corpus (built after 3 December).** A fresh Corpus, every control as shown (the type list,
    Ratio and Opening, the LFO and filter sections, the defaults). For each type at its default, a
    click: the first eight modes' frequencies against the first, and the first mode's decay time at
    Decay 0, 50 and 100 %.
38. **Drum Buss (built after 3 December).** A fresh Drum Buss, every control as shown. A drum loop
    with Comp on, then off: the gain reduction. Drive at 0, 50 and 100 % in each Type, and Crunch at
    0 and 100 %, on a 200 Hz sine at −6 dBFS: the harmonics. A click with Boom at 100 % at three Boom
    Freq and Decay settings: the ring's pitch and length. Transients at ±100 on a kick: the attack.
39. **Dynamic Tube (built after 3 December).** A fresh Dynamic Tube, every control as shown. A
    200 Hz sine at −6 dBFS for each Tube at Drive 0, 12 and 24 dB, Bias 0 and 100 %: the first five
    harmonics. A drum loop with Envelope at ±100 %: how the colour follows the level.
40. **Grain Delay (built after 3 December).** A fresh Grain Delay, every control as shown (the delay's
    range and default, Frequency's, Random Pitch's and Spray's ranges). An impulse at its defaults with
    Pitch 0 and Spray 0: the echo times. A 440 Hz sine with Pitch +12 and −12 at Frequency 20 Hz: the
    spectrum.
41. **Pedal (built after 3 December).** A fresh Pedal, every control as shown. A 220 Hz sine at
    −10 dBFS for each Type at Gain 0, 50 and 100 %: the first nine harmonics, and the output level. A
    −60 dBFS sweep from 30 Hz to 10 kHz with Sub off and on: the response.
42. **Roar (built after 3 December).** A fresh Roar, every control as shown: the routings, the shape
    list, each stage's filter types, the feedback section, the compressor and the modulation matrix.
    A 220 Hz sine at −10 dBFS through each shape at Amount 0, 50 and 100 %: the first nine harmonics.
    Multiband with flat stages on pink noise: does it sum flat?
43. **Spectral Time (built after 3 December).** A fresh Spectral Time, every control as shown (the
    resolution choices, the delay's and the freeze's sections). An impulse through the delay at Time
    250 ms, then with Tilt and Spray: when each band returns. A held chord frozen: its level and pitch
    a few seconds on. The reported latency.
44. **Spectral Resonator (built after 3 December).** A fresh Spectral Resonator, every control as
    shown (its modes, Shift, Unison, the modulation). A burst of noise at its defaults: the partials it
    rings at and how long. A tone between two partials: is it heard?
14. **(Max, not Live; optional, DEV-16.)** A `[line~]` ramp 0 → 1 over 100 ms from a loadbang: the
    value of its first sample. Two boxes at the same x fed from one outlet: which fires first. A lone
    number into `[line~]` (a jump): does its right outlet bang, and when? A list cut short by a new
    one: does the cut list bang? (The translator assumes no to both; DEV-16 step 7.) `[biquad~]`'s
    smooth attribute (on by default): how it smooths, over how long, and whether coefficients given as
    arguments are smoothed in at load. `[slide~]` with a signal in its slide inlets: read per sample?
    A slide below 1, 0 or negative: clamped to 1? `[%]`: is a float input truncated or rounded? The sign
    of `-7 % 3`, and what `% 0` gives. A float `[/]` by 0: −2^31, as the reference seems to say?
    `[selector~]` told a negative choice: silence? A `$1` message box fed a message with a selector
    (not a list): is `$1` the selector or its first argument?

(The Shifter question is answered — Live's way, ADR-0247 d9 — and struck.)

**A question, not a reading: how fine should the export brickwall read? (EX4; see that entry.)**
- The true-peak limiter holds BS.1770's 4x reading at the ceiling.
- An interpolator that shares nothing with it reads the same output higher:
  - +0.15 dB on a quarter-rate sine at 45°, which is 4x's own miss;
  - +1.6 dB on +12 dB white noise.
- The choice:
  - keep the Annex's reading, which matches the meter, and rely on the −1 dBTP ceiling's margin;
  - or have the export's brickwall read finer than the meter, at some CPU cost.
- Nothing changes until you say.

## 2026-11-23 — every open devices branch together on today's main: 303/303 at 32 and 64

**For win (landing windows).** This lane has 43 branches not yet on main. Seven tips contain all the
others:
- `devices/spectral-resonator` (9a6ea54f), the whole effects stack from the Compressor up;
- `devices/amxd-audit` (9fc81e1d), the `.amxd` translator, steps 1 to 10;
- `devices/beat-repeat-contract`;
- `devices/ext-audio-effect-contract`;
- `devices/hybrid-reverb-nupc`, which carries items 1 to 3 and 10 (perf-pass, denormal-tail);
- `devices/pd-midi-out`, which carries `devices/pd-block-quantum`;
- `devices/shifter-nan`.

All seven merged onto main at abbc32ab in a local trial, never pushed, and passed CTest 303/303 at
PD_FLOATSIZE 32 and 64. Every build was clean. Two conflicts came up:
- **`devices/pd-midi-out` (DEV-14) against main** (the AR8 change and the exact-position change):
  four hunks in `pd_device.hpp`, `pd_engine.hpp` and `pd_engine.cpp`.
  - Resolved by merging main into the branch, keeping both sides of each hunk (52782524, pushed).
  - `adi_pd_engine_tests` 715, `adi_pd_tests` 78, native parity 287 and the template, OneShot and
    object-walk suites all pass on it.
  - `devices/pd-block-quantum` alone conflicts the same way. Take `devices/pd-midi-out`: it contains
    pd-block-quantum, already merged with main.
- **`devices/shifter-nan` against the denormal-tail fix (5fc71da8), in `tests/test_shifter.cpp` only.**
  Both branches add a test function at the end of the file and edit the one-line `main()`. Git flags it
  whichever lands second. The resolution: keep both functions, and have `main()` call
  `nonFinite();measured();invariants();liveUnison();unisonAfterAMove();tailAcrossBlocks();`. That is
  how the trial tree is built.

## 2026-11-22 — DEV-16: the audit's last two open points

**PR requested:** head `devices/amxd-audit` (9fc81e1d), in place of 00404789.

**CTest, every head checked, at PD_FLOATSIZE 32 and 64:**
- `devices/amxd-audit` at 9fc81e1d: 270/270.
- `devices/spectral-resonator` at 9a6ea54f, the top of the effects stack with the Auto Shift revert:
  295/295.

Tidy is clean on both.
- **Float-form operators** (`[+ 1.]`, `[* 2.]`) were the bare Pd object, so a `set` into them errored and
  was lost. They now take the int form's front, with `[f]` cells in place of `[i]`. "set 2.5" then a
  bang into `[+ 1.]` gives 3.5.
- **A `set` into `[pack]` is refused.** Pd's `[pack]` cannot store its first slot without output.
- `adi_pd_amxd_tests` 142. The plant fails the `set` check. Its list check passes even with the bare
  object, because Pd's operators already spread a list over their inlets.

**The audit's 28 are now fixed, refused or stated.** What stays as said: Pd's `[t s]` on a number;
a selector message into `[t i]`/`[t f]`; a float `[/]` by 0; live.thisdevice's preset and save
events; and the reading 14 questions.

## 2026-11-22 — the eleventh review: 5 findings; one fix reverted, two trade-offs stated, two fixed

Five findings were confirmed and none refuted. Two of them found that the tenth review's Auto Shift fix
cost more than it bought.
- **Auto Shift (`devices/auto-shift`, 73e35139).**
  - The fault: when the dry signal followed the gliding tap (738a1a59), Dry/Wet 0 stopped being the
    input one reported latency late, for seconds after every correction: 0.73 off on E2. And a ratio
    leaving 1 mid-glide put shifted audio and a splice click into the dry path for 20 ms.
  - Reverted. The trade-off it tried to remove is stated in the header instead:
    - the glide back to the centre takes half a period at 0.1 % (about 1 s at 220 Hz, 6 s at 82 Hz);
    - until it ends, a Dry/Wet below 100 % combs, fading as the glide ends;
    - a faster glide is heard as pitch (0.5 % is 8.6 cents);
    - the dry signal is always the input one latency late.
  - A check now holds that last guarantee through a correction, sample for sample. 738a1a59 fails it.
  - `adi_auto_shift_tests` 126.
- **The detector (`devices/tuner`, 9e314efa).**
  - The faults: the 5 % onset rule drops accurate readings where it is not needed. An E1 dying in
    40 ms keeps 4 of 14 readings at 48 kHz. A vibrato under ±80 % tremolo keeps 72 to 89 of 137 hops.
    The worst error is unchanged in both. The pluck check could not notice lost readings.
  - The rule stays. A dropped reading holds (the Tuner's display, Auto Shift's correction), while a
    wrong one is shown or applied, and the low-note onsets need the rule. The trade-off is now stated
    where the rule is.
  - The tests now count readings:
    - the 300 ms pluck must keep 10 or more (it keeps 53 to 58);
    - deep tremolo must keep half its hops, and ±40 % tremolo nine tenths.
  - The 40 ms E1 pluck is under two of the window's periods before it dies, and gets no reading at
    44.1 kHz.
  - `adi_tuner_tests` 77.
- **The translator (`devices/amxd-audit`, 00404789).**
  - The fault: a `set` arriving at run time (through `[r]` or a gate) into a `---` message box with no
    `$` was still copied into every part.
  - A `[route set]` now leads such a box too.
  - `adi_pd_amxd_tests` 139. The plant fails.

**Rounds so far:** 17, 8, 5, 3, then 5 findings. This round's five were two trade-offs, a too-clever fix,
and one real gap. The reviews have now reached choices rather than faults:
- Dry/Wet combing against pitch accuracy;
- readings dropped against readings wrong.

Both are stated in the code and here. Further rounds wait unless a reading or a ruling changes them.

**New heads:**
- tuner 9e314efa, erosion 10b858ca, vinyl-distortion 8e8c0b4e, filter-delay d231b2c2, dj-filter
  63103b8a, auto-shift 17619945;
- spectrum-device bccefac4, live-amp a5966c33, live-cabinet 1c05af51, corpus 42a7d9f4, drum-buss
  7ff47679, dynamic-tube b3c80c77, grain-delay 39278369, live-pedal 1c7cdac7, roar 331b8ebf,
  spectral-time 0fbeac6f, spectral-resonator 9a6ea54f;
- amxd-audit 00404789.

**CTest:** spectral-resonator at 3788b9ff passed 295/295, and amxd-audit at f162ce3b passed 270/270, at 32
and 64. The heads after those are running.

## 2026-11-22 — the tenth review: 3 findings, all fixed

This round reviewed only the last round's changes. Three findings were confirmed and one refuted: that
high notes' clear onset readings were up to 114 cents off.
- **The detector (`devices/tuner`, 36212226).**
  - The fault: the onset rule measured its quarters after the window's mean was removed. A low note
    starting in the window leaves a large mean, which fills the silent quarter. So bass onsets (E1 at
    40 cents off) and fast plucked decays escaped the rule.
  - Each quarter is now its own variance: its energy about its own mean. A window with a quarter under
    5 % of its loudest counts as a note starting, stopping or dying fast.
  - `adi_tuner_tests` 71 (12 new):
    - E1 and B1 after silence, the onset anywhere in the window, at 44.1, 48 and 96 kHz: worst 26.5
      and 12.0 cents. That is the floor for 3.5 periods of E1 in a 44.1 kHz window; the halves rule
      read 21 to 22.
    - 41.2 Hz plucks: worst 22.2 cents.
    - The previous detector reads E1 40.3 cents off.
- **Auto Shift (`devices/auto-shift`, 738a1a59).**
  - The fault: the 0.1 % glide back to the centre takes seconds on a low note (6 s at 82 Hz). Dry/Wet
    combed until it ended: E2 was 7 dB down at first.
  - While the tap glides, the dry signal now reads the tap as well, crossfaded over 20 ms, so Dry/Wet
    cannot comb. The header now says how long the glide takes, and that both signals are off the
    reported latency until it ends.
  - `adi_auto_shift_tests` 127. E2 and E1 at Dry/Wet 50 % stay within 0.5 dB in every half second.
    The plant fails at 6.6 dB.
  - Merged up the stack to `devices/spectral-resonator` (220043f8).
- **The translator (`devices/amxd-audit`, 745b7c15).**
  - The fault: a `set` into a `$` message box whose `;` parts send to `---` names reached every part.
  - Such a set is now refused, as is a `[prepend set]` feeding that box. A set made at run time is
    dropped.
  - `adi_pd_amxd_tests` 138. The plant fails.

**Rounds so far:** 17, 8, 5, then 3 findings. Each fix has a plant that fails without it.

**New heads:**
- tuner 36212226, erosion fc8aa714, vinyl-distortion 8747f19d, filter-delay ccb36d5d, dj-filter
  a27d28ca, auto-shift 738a1a59;
- spectrum-device 6b0ecf19, live-amp 746623a0, live-cabinet e66c4647, corpus 1ac5587e, drum-buss
  697886df, dynamic-tube 8dd55896, grain-delay a8d7affd, live-pedal f951f52c, roar aac01e9a,
  spectral-time 55a0ec33, spectral-resonator 220043f8;
- amxd-audit 745b7c15.

## 2026-11-22 — the ninth review: 5 findings, all fixed

This round covered only the last round's changes. Five findings were confirmed and one refuted: that
computing the seven copies every sample doubles the CPU at 768 kHz.
- **The detector (`devices/tuner`, b4c027f2).**
  - The fault: the onset rule (halves of the window differing by 1.5×) also caught a sung vibrato's
    tremolo. At ±40 %, 110 Hz read 38 of 137 hops.
  - Every bad onset reading had a near-silent quarter in its window: under 1e-30 of the loudest. A
    ±50 % tremolo keeps every quarter above 13 %. So a window counts as an onset when its quietest
    quarter is under 1 % of its loudest.
  - `adi_tuner_tests` 59. Vibrato with tremolo now reads 137 of 137 hops, and the onsets stay within
    7.8 cents. The halves rule and having no rule both fail.
- **Auto Shift (`devices/auto-shift`, f10ea890).**
  - The fault: the glide back to the centre, at 0.5 %, played an in-tune corrected note up to
    8.6 cents off for 0.2 to 0.9 s.
  - It now glides at 0.1 %: 1.7 cents, below hearing.
  - A check driving a ratio that hovers on a band's top now tests the hysteresis. Before, nothing in
    the suite did: the A3 check's move count could not fail, so it is dropped.
  - `adi_auto_shift_tests` 125. Plants: the 0.5 % glide (8.6 cents off) and no hysteresis (2,250
    moves in 3 s).
  - Merged up the stack to `devices/spectral-resonator` (c20fa92c).
- **The translator (`devices/amxd-audit`, f162ce3b).**
  - `set` into a `$` message box changes its text again, rather than filling in its arguments and
    sending.
  - biquad~'s stability test now runs as Pd's own test, on the numbers Pd will read, in both 32-bit and
    64-bit `t_float`. Coefficients unstable in either are refused. The earlier margin had also
    refused stable resonances below 8 Hz; they now translate.
  - `adi_pd_amxd_tests` 137. Two plants each fail.

**New heads** (in place of the eighth review's):
- `devices/tuner` b4c027f2, `devices/erosion` ecf7feb0, `devices/vinyl-distortion` 26cb6204,
  `devices/filter-delay` a3a6aaa4, `devices/dj-filter` 66d3e8c3, `devices/auto-shift` f10ea890;
- spectrum-device 07b71918, live-amp 66486705, live-cabinet 9c90fd3a, corpus 2e28e86a, drum-buss
  e0238999, dynamic-tube c4cb36d4, grain-delay 67d93e7b, live-pedal fdc69a36, roar 5e37d549,
  spectral-time 01038f92, spectral-resonator c20fa92c;
- `devices/amxd-audit` f162ce3b.

**CTest:** `devices/spectral-resonator` at 16f9631d passed 295/295 and `devices/amxd-audit` at 097e29a7
passed 270/270, at PD_FLOATSIZE 32 and 64. The heads after that are running.

## 2026-11-22 — the eighth review: this round's fixes; 8 findings, all fixed

Two finders covered only the last round's changes, and three skeptics checked every finding. Eight were
confirmed and none refuted.
- **The detector (`devices/tuner`, 9dcd6cd2).**
  - The fault: the range-only cut-off of 743f1e40 let a walk pulled by a note's onset through. A window
    of silence then a tone read clearly sharp, by up to 280 cents. Auto Shift applied that as up to
    49 cents of mis-correction at the start of every note.
  - A window whose two halves differ in energy by more than half again holds a note starting or
    stopping. There, the period must again stay within 1.5 samples of the coarse dip. A steady level
    (vibrato, a glide) keeps the range rule.
  - `adi_tuner_tests` 49:
    - after silence, every clear reading is within 10 cents (worst 7.8);
    - vibrato and glides still read 137 of 137 hops;
    - without the rule the onsets read up to 188 cents off, and with it on every window, vibrato reads
      0 to 18 hops.
  - Two plants each fail. A first attempt at one plant hit MSVC's constant-condition warning and ran
    nothing; it was redone so that it builds.
- **Auto Shift (`devices/auto-shift`, c95041a9).**
  - **Filter bursts.** Its one low-pass was retuned while it ran: a falling ratio burst up to +15 dB.
  - **Stale filtering.** The copy was filtered for the ratio when each sample was written, so a shift up
    aliased fully for one latency (107 ms).
  - Seven fixed filtered copies now replace that low-pass, one per band of ratios (every four
    semitones, to 4.5), each designed once in `prepare()`.
  - **In-tune notes under Correction.** They gave a ratio a hair from 1: the tap never re-centred, and
    the copies flickered with treble notches. A shift within 0.2 cents is now a ratio of exactly 1. The
    input is read directly up to a ratio of 1.05, and a step down a band waits until the ratio is 2 %
    below it.
  - **Decimation.** It rounded down, so 88.2 kHz ran undecimated. It now rounds up.
  - `adi_auto_shift_tests` 121:
    - Shift changes downward never rise past the input's peak.
    - A 15 kHz alias right after a shift up is under −40 dB.
    - An in-tune corrected note at Dry/Wet 50 % keeps the input's level within 0.05 dB.
    - Three plants each fail.
  - Merged up the stack to `devices/spectral-resonator` (3788b9ff).
- **The translator (`devices/amxd-audit`, 1d191219).**
  - A message led by a word, stored for a later bang, is now made a list first, so a box sends
    `hello 3` both when the message arrives and when banged. That `$1` is the first word in Max is
    the reading of three skeptics; the question stays on reading 14.
  - A biquad~ pole on the unit circle, to 1e-6, is refused. There Pd's float test decides differently
    from this one's double test.
  - `adi_pd_amxd_tests` 134. Two plants each fail.

**New heads:** `devices/tuner` 9dcd6cd2, `devices/erosion` 6f515860, `devices/vinyl-distortion` 1f98221e,
`devices/filter-delay` c3382443, `devices/dj-filter` c2589479, `devices/auto-shift` c95041a9, then
spectrum-device 448f1573, live-amp ddf3cecc, live-cabinet fd5eb31e, corpus 94bccf8d, drum-buss 9374445c,
dynamic-tube 297d725a, grain-delay 95125474, live-pedal 8ab3d650, roar 99c9b74c, spectral-time 1559ae3a,
spectral-resonator 3788b9ff; `devices/amxd-audit` 1d191219. CTest of the previous tops is running; the
new tops follow it.

## 2026-11-22 — the seventh review: Auto Shift rebuilt again and the audit fixes; 17 findings, all fixed

The review covered the spliced Auto Shift (b50591ad) and translator steps 9 and 10. Two finders worked on
each, and three skeptics checked every finding. Seventeen were confirmed, some by two lenses; one was
refuted.
- **The detector (`devices/tuner`, 743f1e40).**
  - The cut-off this lane added for the onset stall (a period more than 1.5 samples from the coarse
    dip) threw away most readings of a moving pitch. 110 Hz with ±50 cents of vibrato read 26 of 137
    hops at 48 kHz, and none at 96 kHz. That hit the Tuner and Auto Shift alike.
  - The cut-off is now "a period walked out of the range read". That still ends the stall within its
    bound.
  - `adi_tuner_tests` 43. Vibrato and glides at 82.4 and 110 Hz, at 48 and 96 kHz, read 137 of 137
    hops. The old cut-off reads 0 to 93 of them.
- **Auto Shift (`devices/auto-shift`, 9aaf8b10).**
  - **Low voices.** Periods were capped at 60 Hz's, so 40 Hz at +7 came out at 70 Hz, and a noisy
    220 Hz read at its 7th subharmonic lost its shift. Every period the detector reads, down to
    28.5 Hz, now splices whole.
  - **The return to Shift 0.** After a shift, a ratio back at exactly 1 left the tap off-centre and
    Dry/Wet combed, down to −131 dB. The tap now returns to the centre by whole periods, glides the
    rest away at 0.5 % speed, and then stays there.
  - **Aliasing.** Shifting up aliased: a sawtooth at +24 folded its 50th harmonic down to 4 kHz.
    Shifting up now reads a copy low-passed by an eighth-order Butterworth, and the aliases measure
    −51.5 and −96.3 dB.
  - **The latency budget.** The taps strayed past the stated budget. The ratio is now held through
    each crossfade, crossfades are a quarter period, and a splice that falls due during a filter
    switch is made inside it. The budget is checked with the Shift jumping across its range for
    20 s. The latency is now 5,371 samples at 48 kHz.
  - **Cost at high rates.** Above 48 kHz the detector reads the input averaged down to 48 kHz or less.
    A rate above 800 kHz leaves the device inert.
  - `adi_auto_shift_tests` 112. Five plants each fail. Of the 48 jump checks, the worst uses 0.79 of
    its allowance.
  - Merged up the stack to `devices/spectral-resonator` (16f9631d).
- **The translator (`devices/amxd-audit`, 097e29a7).**
  - Stored `$` arguments: a symbol-led list keeps its `$1`, and symbols are stored.
  - A number box's `set` is truncated and limited.
  - A bang into a message box's right inlet sets it to "bang".
  - `[gate]`'s `next` is refused.
  - `live.thisdevice` re-fires only on a bang or "loadbang", so `[setwidth 300(` fed back into it no
    longer loops.
  - The biquad~ stability test is now Pd's own.
  - `adi_pd_amxd_tests` 132. Six plants each fail.
- **CTest:** the earlier tops, `devices/spectral-resonator` at a478fe52 and
  `devices/amxd-modulo-biquad` at 22fd659f, passed 295/295 and 270/270 at PD_FLOATSIZE 32 and 64. That
  includes the OneShot checks that failed before main was merged in. Both new tops are running.

**PR heads in place of those named below:**

| Branch | Head |
|---|---|
| `devices/tuner` | 743f1e40 |
| `devices/erosion` | 908ac9e5 |
| `devices/vinyl-distortion` | 09d4bc23 |
| `devices/filter-delay` | d62f8cb5 |
| `devices/dj-filter` | 8382342a |
| `devices/auto-shift` | 9aaf8b10 |
| `devices/spectrum-device` | 9600e519 |
| `devices/live-amp` | 9d7c8d7b |
| `devices/live-cabinet` | 230c2de4 |
| `devices/corpus` | 1efa044c |
| `devices/drum-buss` | ed09e822 |
| `devices/dynamic-tube` | 274372f5 |
| `devices/grain-delay` | ce77b777 |
| `devices/live-pedal` | 2f13b2a4 |
| `devices/roar` | c2c29384 |
| `devices/spectral-time` | 0b4e3921 |
| `devices/spectral-resonator` | 16f9631d |
| `devices/amxd-translator` | 74aefaff |
| `devices/amxd-bpatcher` | 97037b35 |
| `devices/amxd-cycle-phase` | 0b89f7b2 |
| `devices/amxd-line-ends` | 539cce65 |
| `devices/amxd-modulo-biquad` | 22fd659f |
| `devices/amxd-audit` | 097e29a7 |

## 2026-11-22 — DEV-16, steps 9 and 10: the translation table audited against Max's reference pages (P2)

**PR requested:** head `devices/amxd-audit` (6049ee08), base `main`, after
`devices/amxd-modulo-biquad`. Two commits; the bodies are the commit messages.
- **The audit.** Four finders read the translation table against docs.cycling74.com, by object group:
  signal objects, arithmetic, flow and messages, and the live.* controls. Three skeptics checked each
  finding, and 28 differences were confirmed: things a translated patch would hear or show differently
  from Max. Two findings were refuted.
- **Step 9 (f2264fe8): defaults, ranges and inlets.**
  - A bare `[/~]` divides by 1, and a bare `[t]` has two int outlets.
  - `[gate]` and `[selector~]` clamp a choice past their count. A bang into `[gate]` reports the open
    outlet.
  - `[loadbang]` and live.thisdevice take a bang in their inlet.
  - plugin~ and plugout~ channel arguments do not route in Max, so only "1 2" or none translate.
  - A message box's `---` names reach `[r ---name]`. Pd 0.56 does not expand `$0` in a `;` receiver,
    so each part becomes its own message into `[s $0-name]`. The box's right inlet sets its contents.
  - Number boxes keep their minimum and maximum, and their Decimal Floating-Point format.
  - The fan-out order follows the destination inlet's x. One outlet into both inlets of `[*]` gives
    9, not 0.
  - Refused, named: quoted messages; a `---` name inside a message; hard sync into `[phasor~]`;
    unstable biquad~ arguments; live.* Steps, and outlets past the first.
- **Step 10 (6049ee08): the messages.**
  - Lists and `set` into an int operator's left inlet.
  - Int division by 0 divides by 1.
  - `set` into `[i]`, `[f]` and number boxes.
  - `set` into `[prepend]` and `[append]`, and a bang there becomes the word "bang".
  - `reset` into `[cycle~]` and `[phasor~]`, and `clear` into biquad~.
  - A list and a bang into `[pack]`, with its int slots truncated on the way out.
  - An `[f]` in front of `[!- n.]`, so a list cannot change its multiplier.
  - A message box keeps its `$` arguments for a later bang.
  - Refused: slide~'s `reset`.
- **Not made explicit, and said so in the header:**
  - A number into Pd's `[t s]` gives "float" (Max gives "").
  - A selector message into `[t i]` or `[t f]` gives 0 in Max and an error in Pd.
  - A float division by 0 gives 0 in Pd (Max gives −2^31, by its reference; added to reading 14).
- **Still open from the audit:**
  - `set` into `[pack]`, and into a float-form operator (`[+ 1.]`);
  - a coefficient list that turns biquad~ unstable at run time. Pd silences it; Max runs away.
  - live.thisdevice also bangs when a preset is loaded or the device saved. ADI has no such event for
    a device.
- **Tests:** `adi_pd_amxd_tests` 124 (37 new), each played in libpd where it can be.
  - 13 plants each fail: 6 for step 9 and 7 for step 10.
  - Every plant build deletes the old binary first, so a build that fails cannot pass as a result.

## 2026-11-22 — the sixth review: the overlap-add Auto Shift was wrong; rebuilt as a spliced delay line. The detector's stall fixed

The review of e387e86e found eight faults, every one confirmed by all three skeptics with their own probe
builds. The overlap-add design itself was wrong.
- **A pure tone shifted lost its level or did not shift at all.** Grains cut from a sine on its own
  period grid cancelled each other shifted up: −6 dB at +7, silence from +12 to +24. An octave or more
  down, the tone came back unshifted, only delayed. The +12 check passed on a peak 40 dB down, because
  it checked only where the peak was.
- **Wet and dry did not line up.** The wet signal sat up to half a period off the dry one, so Dry/Wet
  combed: 100 Hz at −125 dB.
- **Readings came too early.** Each one was applied 25 to 50 ms before the audio it was read from.
- **A downward pitch step jumped.** With a Shift set it jumped 10 to 38 times the input's own step.
  The step checks ran only at Shift 0, where this cannot show.
- **The detector stalled the audio thread at every onset after silence.** Its fine search ran some
  37,000 passes: 0.4 s at 48 kHz, 1.8 s at 96 kHz. The detector is the one the Tuner uses too.
- **An absurd sample rate hung `prepare()`**, in the core and in the detector.

**Fixed:**
- **The detector**, on `devices/tuner` (d8c3fd89):
  - the fine search is bounded at 16 passes of 16 steps, and a walk that leaves the coarse dip ends
    the reading;
  - a rate past any host's leaves it inert;
  - `adi_tuner_tests` 35. Onsets after silence at 44.1, 48 and 96 kHz stay within the bound, and a
    clean 440 Hz still reads within 0.1 cent. The old detector does not finish the new test: its
    `prepare(1e300)` hangs.
- **The core**, on `devices/auto-shift` (b50591ad), rebuilt as a delay line read at the shift's speed:
  - When the read tap has drifted half a period from its centre, a second tap takes over a whole
    number of detected periods away, crossfaded over half a period. A periodic input reads the same
    waveform in both, so the splice is not heard.
  - At a ratio of exactly 1 the tap never moves from the centre, so wet is dry, sample for sample.
  - Readings wait in a queue until the tap is within half a hop of the middle of their window.
  - The latency is fixed and reported: 4,432 samples at 48 kHz.
  - `adi_auto_shift_tests` 97:
    - 220 Hz at eleven Shifts from −24 to +24 comes out at 220 × the ratio, at 0.000 dB, with nothing
      left at 220 Hz.
    - A sawtooth an octave down keeps every harmonic's level.
    - Dry/Wet 50 % at Shift 0 keeps the input's level, and the wet signal is the input one latency
      late, exactly.
    - 446 then 330 Hz, corrected, gives 440.0 up to the step's arrival and 329.63 after it.
    - 48 jump checks pass. The worst uses 0.89 of its allowance.
  - Four plants each fail: readings applied at once (444.8 Hz before the step), splices by 0.9 of a
    period, no crossfade, and the tap half a sample off the centre. Two checks had first passed under
    their plants. Both were tightened after a probe measured where the output's frequency actually
    changes.
- **Merged up the stack and pushed:** from `devices/erosion` through `devices/spectral-resonator`
  (a478fe52).

**The translator branches now include today's main.** On `devices/amxd-line-ends`,
`adi_oneshot_pd_tests` failed six checks at PD_FLOATSIZE 64. It fails the same way on
`devices/amxd-translator`: that branch was cut from an old main, before the OneShot fixes. Main is now
merged into `devices/amxd-translator` (74aefaff), and from there up the chain to
`devices/amxd-modulo-biquad` (22fd659f).

**CTest:** `devices/spectral-resonator` at 57e5c9be (before this round) passed 295/295 at 32 and 64.
Both new tops are running.

**Next:** the translator audit. Four finders went through the translation table against Max's reference
pages, with three skeptics per finding. They confirmed 28 differences that a patch would hear or show.
Some are wrong defaults: a bare `[/~]` divides by 0 instead of 1, and `[t]` with no arguments gives
bangs. Others are missing messages (`set`, `reset`, `clear`), out-of-range choices in `[gate]` and
`[selector~]`, number-box limits, and `plugout~`'s channel arguments. Each is fixed, or refused with
its reason, step by step.

## 2026-11-22 — DEV-16, step 8: Max's `%` is an integer modulo; `biquad~`'s smoothing is not ignored (P2)

**PR requested:** head `devices/amxd-modulo-biquad` (0a1ea57b), base `main`, after
`devices/amxd-line-ends`. Body: the commit message. Both are corrections, read from Max's own
reference.
- **`[%]`.** The translator refused `[% 2.5]` as a float modulo that Pd could not do. Max's reference
  says a float is "converted to int" and the argument is an int, so Max has no float modulo here.
  `[% 2.5]` now takes the integer path: fed 7.3 it gives 7 % 2 = 1.
- **`[biquad~]`.** Max smooths coefficient changes. Its `smooth` attribute is on by default, and the
  reference does not say how. Step 4 changed coefficients at once, which a Max patch would not do with
  smoothing on.
  - Coefficients that change at run time now translate only with smoothing off: `@smooth 0` typed,
    or the attribute saved with the box.
  - Arguments alone still translate.
- **Still refused**, because the reference does not say whether a signal in these inlets is read per
  sample: `[slide~]`'s and `[biquad~]`'s signal-rate inlets.
- **The open points** go to reading 14:
  - `biquad~`'s smoothing;
  - `slide~` with a signal, and below 1;
  - `%` with a float input, a negative input, and 0.
- **Tests:** `adi_pd_amxd_tests` 87.
  - `[% 3]` fed 8.9 gives 2, and `[% 2.5]` fed 7.3 gives 1.
  - The `biquad~` list case with `@smooth 0` still starts at 1 and settles at 2.
  - The default is refused, naming the smoothing. A saved `smooth` 0 translates.
  - Three plants each fail: the old `%` refusal, the smoothing check removed, `@smooth` not read.
  - The first run of the last two plants hit MSVC's constant-condition warning (`if (false && ...)`)
    and ran a stale binary. They were redone with conditions the compiler cannot fold, and both fail.

## 2026-11-22 — DEV-16, step 7: `[line~]`'s ramp time, end bang and initial value (P2)

**PR requested:** head `devices/amxd-line-ends` (106c0232), base `main`, after
`devices/amxd-cycle-phase`. Body: the commit message.
- **Each from Max's reference for `line~`:**
  - The right inlet's time carries the next lone number, then reverts to 0.
  - The right outlet bangs when the whole list has played. This is a `[delay]` of the list's total
    time that every new input reschedules.
  - An argument is the initial value, set before the parent's loadbang.
- **Where the reference is silent**, an assumption, added to reading 14:
  - A lone jump does not bang. A jump that retriggered itself would also hold Pd's scheduler at one
    instant.
  - A sequence cut short does not bang.
- **Tests:** `adi_pd_amxd_tests` 83 (6 new), played in libpd.
  - 100 into the right inlet, then 1: a ramp over 100 ms. A later lone 0 jumps.
  - "0 0 1 100" bangs at 100 ms, not after its first segment.
  - Cut short at 50 ms by "0 100", it bangs once, at 150 ms.
  - A lone jump does not bang.
  - `[line~ 0.5]` starts at 0.5, and a ramp sent from a loadbang starts there.
  - Three plants each fail: the time kept after use, a jump that bangs, a bang after the first
    segment.
- **The translator's refusal list** now holds float modulo, signal-rate biquad~ coefficients and
  slide~ times, live.text's button mode, bpatchers of outside files, and MIDI devices and instruments.

## 2026-11-22 — DEV-16, step 6: `[cycle~]`'s phase inlet, an offset as in Max (P2)

**PR requested:** head `devices/amxd-cycle-phase` (c09df249), base `main`, after
`devices/amxd-bpatcher`. Body: the commit message.
- **The difference.** Max's `[cycle~]` adds a phase offset from its right inlet (0 to 1, a float or a
  signal) to its running phase. Pd's `[osc~]` resets its phase there. Such a `[cycle~]` was refused;
  it now becomes `[phasor~]` → `[+~]` → `[cos~]`.
- **Tests:** `adi_pd_amxd_tests` 77.
  - `[cycle~ 375]` with a signal offset of 0.25 matches cos(2π(ft + offset)) sample for sample in
    libpd (worst under 1e-6).
  - So does a float offset of 0.25 moved to 0.5 at 100 ms. That is half way through a period, where a
    reset would land half a period away.
  - Two plants fail both checks: the offset into `[phasor~]`'s reset inlet (Pd's meaning), and the
    offset dropped.

## 2026-11-22 — DEV-16, step 5: embedded bpatchers in the `.amxd` translator (P2)

**PR requested:** head `devices/amxd-bpatcher` (9def72a5), base `main`, after
`devices/amxd-translator` (steps 3 and 4, not yet on main). Body: the commit message.
- **Why this row.** The queue is worked through; items 4, 5, 6, 13 and 14 stay parked on their
  lines. Their blockers have not cleared on main: no `adi_shaper_scope.hpp`, no `syncAtSample`, no
  DEV-17 or DEV-18. Every M-row of this lane waits on a ruling, a reading or another seat:
  - DEV-09 and DEV-28: win's rulings;
  - DEV-19 and DEV-14: the engine's DEV-17 and DEV-18;
  - DEV-22: the readings and DEV-32's reader;
  - DEV-10, DEV-15, DEV-11 and DEV-13: the engine, ADRs, the director.

  So the next buildable piece in milestone order is DEV-16's.
- **What it does.** A bpatcher whose patcher is embedded becomes a `[pd name]`, as a `[p]` does.
  - Max replaces `#1` to `#9` in its boxes by the bpatcher's arguments, and `#0` by a name unique to
    the instance. The translation does the same, writing `#0` as `$0-bp-<its path>`, so two
    instances never share a send.
  - Its `[p]`s take the same arguments; a bpatcher inside it takes its own.
  - A float argument stays a float: `2.` keeps `[+ #1]` the float form.
- **Refused, named, not guessed:**
  - a `#` with no argument;
  - an argument that is not a number or a plain word;
  - a bpatcher naming a patcher file outside the device. That file is not in the `.amxd`.
- **Tests:** `adi_pd_amxd_tests` 75 (15 new).
  - A gain bpatcher at arguments 0.5 and 0.25 walks clean under PD4. In libpd it plays the input times
    the argument.
  - Two instances' `[s #0-v]` → `[r #0-v]` stay apart.
  - `[+ #1]` fed 0.5 gives 2.5 with the argument 2.0, and 2 with the argument 2.
  - The step-4 translator fails 14 of the checks. Four plants each fail: one `#0` for all, a `[p]`
    without the arguments, a float argument without its point, and a missing argument read as 0.
  - Tidy is clean. One older line in the file had built a `Fragment` with a field missing; it is now
    filled field by field.
- **Left in DEV-16:**
  - MIDI devices (after DEV-14 and DEV-18) and Suite's modulators (after DEV-07);
  - `[line~]`'s ramp-time inlet and end bang, and `[cycle~]`'s phase inlet;
  - parity against Live (readings);
  - the AI path for the rest;
  - becoming a device (DEV-10).

## 2026-11-22 — the fifth review: Roar, Auto Shift and the spectral pair, ten findings, all fixed

Three skeptics per finding. Every fix is merged up the stack and pushed. The new heads are listed
under "Heads" below.
- **Auto Shift, rebuilt (e387e86e, on `devices/auto-shift`).** The review of 53080074 confirmed two
  findings.
  - Its window followed every new pitch reading, so the Shifter's read taps jumped whenever the pitch
    moved. Under vibrato, a step or a glide the largest step was 0.13 to 0.38, against the input's own
    0.013.
  - Letting the correction go at unison dropped the level by up to 23 dB.
  - The core is now a pitch-synchronous overlap-add on the detector's period:
    - output marks lie P / ratio apart;
    - each mark lays a Hann grain of the input around the input mark on the period's grid nearest the
      output's time one latency back;
    - the grains are normalised by their summed windows.
  - The latency is fixed and reported: 4.5 times the longest period shifted (that of 60 Hz), 3,608
    samples at 48 kHz. The dry signal is delayed with it.
  - Tests: `adi_auto_shift_tests` 41.
    - Vibrato, a step and a glide at 44.1, 48 and 192 kHz: no step past the input's own, within half
      again.
    - The correction let go at 48 points: the level never falls 1.5 dB.
    - Dry/Wet 0 is the input one reported latency late, exactly.
    - The old core fails 12 of these; at the let-go it measures −23.0 dB. Four plants each fail: no
      normalisation, the input mark off the grid, half-width grains, an undelayed dry.
  - Reading 33 (Auto Shift) asks for Live's latency and how fast it reacts; this core's are now fixed
    numbers to compare.
- **Roar (87d9de50).** The review of e94e310a confirmed two findings.
  - A change of Shape with a Bias set left the bias offset stale: a thump of up to 0.12 out of silence.
  - A change of routing reset stage 1 although every routing runs it: a step of 0.29 in a sample.
  - Only stages that sat idle are reset now. `adi_roar_tests` 29; the five new checks fail on the old
    core.
- **Spectral Time (2be28788).** The review of b8436141 confirmed three findings.
  - A finite burst near the float's limit overflowed the transform, and the NaN stayed in the
    feedback for good. The input is now bounded at 1e6.
  - The aliased-plane check could not fail; it now compares against a run with separate planes.
  - The inert device reported latency it did not have. Dry/Wet also glides now.
  - `adi_spectral_time_tests` 15.
- **Spectral Resonator (765b3100).** The review of 1802596d confirmed three findings.
  - At 192 kHz the fixed 2,048-sample window let a tone between partials through at −0.1 dB. The
    window now scales with the rate, and the tone stays under −40 dB at 44.1, 96 and 192 kHz.
  - A burst of 1e36 latched a ring at inf. The input is now bounded at 1e6.
  - Dry/Wet stepped; it now glides.
  - `adi_spectral_resonator_tests` 15; the three checks fail on the old core.
- **Heads:**

  | Branch | Head |
  |---|---|
  | `devices/auto-shift` | e387e86e |
  | `devices/spectrum-device` | df4d6917 |
  | `devices/live-amp` | 71fdc362 |
  | `devices/live-cabinet` | ff74bb39 |
  | `devices/corpus` | a26c64bd |
  | `devices/drum-buss` | f2ff124b |
  | `devices/dynamic-tube` | c3a67645 |
  | `devices/grain-delay` | 182aa06b |
  | `devices/live-pedal` | 6ada65ac |
  | `devices/roar` | 6b411b96 |
  | `devices/spectral-time` | d3b4b24f |
  | `devices/spectral-resonator` | 57e5c9be |

  PRs requested at these heads, in place of the heads named in the entries below.
- **CTest.** `devices/spectral-resonator` at 1802596d, before these fixes, passed 295/295 at
  PD_FLOATSIZE 32 and 64, and tidy was clean. The run at 57e5c9be is under way.

## 2026-11-22 — DEV-22, the chapter's last missing effect: Spectral Resonator — lands after 3 December

**PR requested:** head `devices/spectral-resonator` (1802596d), base `main`, after
`devices/spectral-time`. It stops here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.**
  - The input's spectrum rings at the partials of a pitch, set by Frequency, Harmonics, Stretch,
    Decay, Brightness and Dry/Wet.
  - Each partial owns the bins within one bin of it. While a partial rings, its bins advance at the
    partial's own frequency, so the ring is a tone at the partial.
  - The latency is one window, reported.
- **Not built:** its MIDI and internal modes, Shift, Unison and modulation.
- **Tests:**
  - `adi_spectral_resonator_tests` 10. After a burst of noise the output rings at the first four
    partials within 1 Hz. A first version, advancing each bin at its own measured frequency, missed by
    36 Hz.
  - The ring falls 30 dB in a second at Decay 2 s, within 15 %. A tone between partials is not heard.
    Dry/Wet 0 is the input one window late. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 46 built-ins.
- **Reading 44, added to the ONE list.**

**The chapter is built, as far as this lane's part goes.** All of Live 12.4's audio effects are now
in the catalogue except Looper, whose core is the engine's. External Audio Effect's routing is also the
engine's; its contract is written. Beat Repeat waits on reading 18.

**CTest:** `devices/roar` at 46f2a71e passed 293/293 at PD_FLOATSIZE 32 and 64. The new top,
`devices/spectral-resonator`, is running.

## 2026-11-22 — DEV-22, the chapter's next missing effect: Spectral Time — lands after 3 December

**PR requested:** head `devices/spectral-time` (b8436141), base `main`, after `devices/roar`. It stops
here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.**
  - A 2,048-point Hann STFT at a hop of 512, rebuilt exactly.
  - Each bin is delayed by whole frames, shaped by Time, Feedback, Tilt, Spray and Mask.
  - Freeze holds the spectrum through a phase vocoder.
  - The latency is one window, reported, and the dry signal is delayed with it.
- **Not built:** the pitch shift, the resolution choices, stereo spread and sync.
- **Tests:**
  - `adi_spectral_time_tests` 13. An impulse returns after the latency plus the delay, alone and whole
    within 1e-5, and each repeat is Feedback times the last.
  - Dry/Wet 0 is the input one window late, exactly. Tilt, Mask and Spray shape the delays.
  - A frozen tone rings on within 1 dB and 1 Hz. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 45 built-ins.
- **Reading 43, added to the ONE list.**

## 2026-11-22 — the glide sweep, the earlier branches; an Auto Shift jump found and fixed

The same glide and the same test, on the earlier post-3-December cores. `dsp/glide.hpp` is added on
`devices/auto-pan-tremolo` for them; it is identical to the one on `devices/live-amp`, so the merges
are clean. Every branch above is merged up to the top, `devices/roar` (e94e310a). There, the touched
suites, the catalogue and parity all pass (744/744).

**PR requested** (the same branches; new heads):

- **`devices/auto-pan-tremolo` (54eab5aa).** Amount had stepped 0.20 in a sample.
- **`devices/channel-eq` (e5141d7b).** Output had stepped 0.70.
- **`devices/eq-three` (c97c2582).** Each band's gain and its kill (a kill now fades). They had stepped
  0.10 and 0.045.
- **`devices/vinyl-distortion` (32a47da6).** The tracing's amount, the pinch's drive and switch, and the
  crackle volume. They had stepped 0.25 and 0.42. The no-thump check now averages over 20 ms.
- **`devices/filter-delay` (6594e91d).** Dry, each line's Volume and Pan, Feedback and On. They had
  stepped from 0.04 to 0.59.
- **`devices/auto-shift` (53080074).**
  - **Dry/Wet** is now Auto Shift's own glided mix. It was the Shifter's Mix, which stepped.
  - **A jump on a steady tone.** A held 220 Hz tone shifted +7 jumped 0.59 in a sample, repeatedly.
    220 Hz is 5.5 periods in 25 ms, so the window's count flipped with the reading's last decimals.
    Each flip moved the Shifter's taps by a whole period. The count now holds until the reading is
    more than three quarters of a period from it. A plant without the hold jumps 0.59.
  - Each check fails on the old core or on a plant.

Erosion already glided its Amount. Not swept: Align Delay's times (a delay jump is its own question,
a reading), and the Compressor stack (its own smoothing).

**CTest:** `devices/roar` at 46f2a71e passed 293/293 at PD_FLOATSIZE 32. The 64 run, and a run on the
new top, follow.

## 2026-11-22 — the glide sweep: the newest cores' gain controls no longer step

Pedal's review found gain controls applied as instant steps. Amp, Cabinet, Corpus, Drum Buss,
Dynamic Tube, Grain Delay and Roar were built the same way.
- **`dsp/glide.hpp`** (on `devices/live-amp`) is a one-pole glide of about 10 ms.
  - Set before any audio, the value simply lands.
  - Within 1e-12 of its target, it snaps, so a unit gain or a zero mix is exact.
- **What glides now.**
  - Each core's gain, level, output, mix and feedback controls.
  - Drive-like controls (Dynamic Tube's Drive, Roar's Amount) glide over 50 ms. They move a curve's
    DC or 36 dB of level.
- **The test, per core:** moved at a tone's peak, the largest step while the control moves stays
  within 1.5 times the signal's own step, before or after. On the old cores they jumped 0.03 to 1.47
  in a sample.

**PR requested** (the same branches; new heads), each merged up the stack:
- `devices/live-amp` (58c9398b): Gain, Volume, Output, Dry/Wet.
- `devices/live-cabinet` (936afef8): Output, Dry/Wet.
- `devices/corpus` (c9dc2a78): Gain, Bleed, Dry/Wet.
- `devices/drum-buss` (2a52e068): Trim, Drive, Crunch, Transients, Boom, Output, Dry/Wet.
- `devices/dynamic-tube` (6f63ee37): Drive, Bias, Output, Dry/Wet.
- `devices/grain-delay` (90e162b1): Feedback, Dry/Wet.
- `devices/live-pedal` (65d0a2f4): the merges; its own glide came with the fourth review.
- `devices/roar` (46f2a71e): Input, each stage's Amount, Bias and Level, Feedback, Output, Dry/Wet.

**CTest:** `devices/live-pedal` at 77d27c3c (before the fourth review) passed 292/292 at PD_FLOATSIZE
32 and 64. The new top, `devices/roar`, is running.

**Still to sweep, on the earlier branches:**
- Erosion, Vinyl Distortion, Filter Delay, Auto Shift's Dry/Wet, EQ Three, Channel EQ, Align Delay
  and Auto Pan's depth;
- the Compressor stack's makeup and mix.

They are next in this lane's queue.

## 2026-11-22 — Roar (DEV-22); the fourth review: Grain Delay and Pedal, four findings fixed

**PR requested:** head `devices/roar` (0ac41161), base `main`, after `devices/live-pedal`. It stops
here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.**
  - Three shaper stages in five routings: Single, Serial, Parallel, Mid Side, and Multiband (LR4 bands
    that sum flat).
  - Each stage: a shape (six, each with slope 1 at 0 except the crusher), Amount, a Bias with its DC
    blocked, a filter and a level.
  - Input, a bounded feedback path, Output and Dry/Wet.
  - A change of routing restarts the stages.
- **Not built:** Roar's compressor and modulation matrix.
- **Tests:**
  - `adi_roar_tests` 19. Four routings follow their documented structure within 0.05 dB, Mid Side's
    side goes through stage 2, and Multiband with flat stages sums flat.
  - The feedback loop matches its closed form. The shapes check out, and a bias makes a second
    harmonic.
  - A routing change does not replay an idle stage (a plant fails). Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 44 built-ins.
- **Reading 42, added to the ONE list.**

**The fourth review** covered Grain Delay and Pedal (three skeptics each) and confirmed four findings:
- **`devices/grain-delay` (c50bd30a).**
  - **Fast grains read past the write head**, down to −19,195 samples. The clamp counted the
    excursion twice, and the line ignored Random Pitch. The line now holds 7.6 s, and the clamp is
    correct. `lowestDelay()` is at least 1 in six extreme settings.
  - **A Frequency move rescaled running grains:** clicks, and up to twice the level. A new grain now
    rises over its predecessor's fall, so a constant input stays at 1 within 1e-6 through five
    moves.
- **`devices/live-pedal` (41d658de).**
  - **Gain, Output and Dry/Wet stepped at once.** They now glide over 10 ms.
  - **A Type change thumped.** The voices now crossfade over 200 ms. All four new checks fail on the
    old core.

**A lane-wide follow-up, taken next.** Pedal's review found gain controls applied as instant steps.
Amp, Cabinet, Corpus, Drum Buss, Dynamic Tube, Roar and the earlier post-3-December cores are built
the same way, so they need the same glide on their gain, output and mix controls, and the same test.

## 2026-11-22 — the third review: fourteen findings in Amp to Dynamic Tube, all fixed

A third adversarial review covered Amp, Cabinet, Corpus, Drum Buss and Dynamic Tube (a finder for
each, three skeptics for each finding). It confirmed fourteen findings, each by all three skeptics.
Each fix carries a check that fails on the old code, apart from three that only tightened a test.
Each is merged up the stack through Grain Delay and Pedal.

**PR requested** (the same branches; new heads):

- **`devices/live-amp` (b2d4855d).**
  - **Dual off and on replayed the right amp's old state:** a 0.87 thump on silence. The right amp
    now starts from the left's state.
  - **The bias check was decided by drive.** It now compares at equal drive.
  - **The bound check allowed 1e3.** It now holds the output to the power stage's bound at three
    rates.
- **`devices/live-cabinet` (d572786d).**
  - **The same Dual thump** (0.89), fixed the same way.
  - **Mono's mean was never tested.**
- **`devices/corpus` (c812da0a).**
  - **The level grew 6 dB per doubling of the rate.** The input is now scaled by 48 kHz/rate: a
    2 ms hit is within 0.5 dB at every rate, against 12.1 dB before.
  - **A tone held at a mode built up to 1,676 at Decay 100.** The resonance now passes a tanh.
  - **The membrane skipped the Bessel zero j₇₁.** All sixteen modes are now checked against zeros
    found by bisection.
- **`devices/drum-buss` (52772ec5).**
  - **Transients distorted held low tones** (up to 31 % THD at 50 Hz). It now compares a peak
    envelope with its own lagged copy: a held tone passes with under 0.1 % THD.
  - **Crunch lifted the band by up to 8 dB.** It is now the band's square, an octave up, and leaves
    the band's level alone.
  - **Drive cost full scale 20 dB.** It is now matched by peak at −6 dBFS.
- **`devices/dynamic-tube` (5e496bf4).**
  - **A biased tube's DC came out** (up to −4.0). A 5 Hz blocker now removes it.
  - **Normalising by the slope at the bias gained up to +18 dB.** The tube is now divided by the
    drive alone, so it never puts out more than it is given.
- **`devices/grain-delay` (7389daf6) and `devices/live-pedal` (77d27c3c):** the merges above.

**CTest (`ctest -LE acceptance`):**
- `devices/live-amp` at c4d52f4b (the second review's stack): 286/286 at PD_FLOATSIZE 32 and 64.
- `devices/shifter-nan` (14472a18): 270/270 at 32 and 64.
- The top of the stack (`devices/live-pedal`) is running.

## 2026-11-22 — DEV-22, the chapter's next missing effect: Pedal — lands after 3 December

**PR requested:** head `devices/live-pedal` (998ee01f), base `main`, after `devices/grain-delay`. It
stops here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.**
  - Three voices: Overdrive, Distortion and Fuzz.
  - The drive is f(g x + b) − f(b) with the DC blocked, so it gets louder with Gain, as a pedal does.
  - Sub, then the tone section: Bass, Mid at three frequencies, and Treble.
  - Output and Dry/Wet.
- **Tests:**
  - `adi_live_pedal_tests` 15. At −140 dBFS, four settings follow the documented chain within
    0.05 dB, with the drive's slope taken numerically.
  - Gain raises the distortion; Overdrive's bias makes a second harmonic; Fuzz distorts most.
  - Sub keeps 50 Hz, and the output stays bounded. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 43 built-ins.
- **Reading 41, added to the ONE list.**

## 2026-11-22 — DEV-22, the chapter's next missing effect: Grain Delay — lands after 3 December

**PR requested:** head `devices/grain-delay` (6ec98033), base `main`, after `devices/dynamic-tube`. It
stops here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.** Grains start every 1/Frequency, last two periods under a periodic Hann window and
  sum to exactly 1. Each reads at its Pitch, moved by Random Pitch, from the delay lengthened by
  Spray. The controls also include Feedback, Sync or Time, and Dry/Wet. `prepare()` never throws.
- **Tests:**
  - `adi_grain_delay_tests` 16. An impulse returns after exactly the delay, alone, each repeat
    Feedback times the last. Pitch ±12 moves 440 Hz to 880 and 220 Hz. Spray spreads the channels.
    Sync. The longest reach at 192 kHz stays finite. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 42 built-ins.
  - The −12 check caught a first draft that started grains at twice Frequency: its carrier cancelled.
- **Reading 40, added to the ONE list.**

## 2026-11-22 — DEV-22, the chapter's next missing effect: Dynamic Tube — lands after 3 December

**PR requested:** head `devices/dynamic-tube` (2b507061), base `main`, after `devices/drum-buss`. It
stops here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.** Tube saturation. Its operating point is Bias plus Envelope times the input's
  envelope, and it is normalised by its slope there, so a quiet signal passes at unity. Three
  curves, a Tone tilt with its inverse, Output and Dry/Wet.
- **Tests:**
  - `adi_dynamic_tube_tests` 13. A quiet sine passes at Output's gain within 0.05 dB in all 81
    combinations. Each curve's slope matches its numerical derivative. No bias, no second
    harmonic; Bias or Envelope give one. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 41 built-ins.
- **CTest:** the fixed stack at `devices/live-amp` (c4d52f4b) passed 286/286 at 32. The 64 run is
  pending.
- **Reading 39, added to the ONE list.**

## 2026-11-22 — DEV-22, the chapter's next missing effect: Drum Buss — lands after 3 December

**PR requested:** head `devices/drum-buss` (96fd37df), base `main`, after `devices/corpus`. It stops
here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.** Trim, a fixed compressor, Drive into three curves, Crunch above 1 kHz, Damp,
  Transients, Boom, Output and Dry/Wet.
- **Tests:**
  - `adi_drum_buss_tests` 18. A quiet sine is Trim, Damp and Output within 0.05 dB. The
    compressor's pull on a −6 dBFS square is its static curve within 0.05 dB. Boom rings at 60 Hz
    within 1 %. Transients sharpen and soften a hit. Drive and its curves. Crunch brightens 1.5 kHz
    and leaves 100 Hz alone. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 40 built-ins.
- **Reading 38, added to the ONE list.**

## 2026-11-22 — DEV-22, the chapter's next missing effect: Corpus — lands after 3 December

**PR requested:** head `devices/corpus` (eb01d991), base `main`, after `devices/live-cabinet`. It
stops here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.** Sixteen two-pole resonant modes of a beam, a marimba bar, a string, a membrane, a
  plate, a pipe or a tube, each at its textbook ratios. The controls are Tune, Fine, Decay, Material,
  Hit, Brightness, Inharmonics, two listening positions, Bleed, Gain and Dry/Wet.
- **Not built:** Ratio, Opening, the LFO, the filter, MIDI frequency and Spread.
- **Tests:**
  - `adi_corpus_tests` 22. An impulse rings at each model's first four textbook ratios within 0.1 %.
    The first mode's T60 is within 2 %. A node at the hit or the listening position silences a
    string's second mode. Fine, Inharmonics and the rate's limit. Bleed. Block sizes, NaN, no
    allocation.
  - parity max error 0;
  - the catalogue holds 39 built-ins.
- **Reading 37, added to the ONE list.**

## 2026-11-22 — DEV-22, the chapter's next missing effect: Cabinet — lands after 3 December

**PR requested:** head `devices/live-cabinet` (ad19a6b9), base `main`, after `devices/live-amp`. It
stops here if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.** A fixed linear filter: five cabinets (each a low cut, a box resonance, a cone peak
  and a two-section high cut), three microphone positions, two mic types, Output, Dry/Wet, and Mono
  or Dual.
- **Tests:**
  - `adi_live_cabinet_tests` 39. All thirty combinations of cabinet, microphone and mic type match
    the documented cascade within 0.01 dB. The cascade is computed apart from the signal path. Off
    axis and far are duller. Mono and Dual. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 38 built-ins.
- **Reading 36, added to the ONE list.**

## 2026-11-22 — the second review: eight findings, all fixed

A second adversarial review covered Auto Shift, the Spectrum device, the Shifter fix and the first
review's fixes (a finder for each, three skeptics for each finding). It confirmed eight findings,
each by all three skeptics. Each fix carries a test that fails on the old code, or a plant where the
old code cannot build the test. Each is merged up the stack.

**PR requested** (the same branches; new heads):

- **`devices/filter-delay` (2c4a1a15).** `prepare()` could throw from Pd's DSP callback, and it set `size_`
  before the lines were resized. It is now noexcept: a failure leaves the device inert, passing the
  dry signal.
- **`devices/dj-filter` (25c20908).** The DJ type seeded its low pass with the input at any cutoff. After
  `prepare()` at Filter −100, a 1 kHz cosine rang at 0.5. It is now seeded only near the centre.
- **`devices/auto-shift` (1d566e3a).**
  - **A corrected tone dropped 40 dB at every crossfade** for pitches like 420, 460 or 300 Hz: the
    fixed 50 ms window put the Shifter's taps in antiphase. The window now spans whole periods of the
    reading. The level holds within 1 dB, and a plant with the fixed window brings the dips back.
    Shift +12 now gives 880 Hz, not 874.
  - **Detune plus the correction was clamped at ±100 cents.** The shift now goes through Coarse,
    unclamped.
  - **The NaN check looked at a later block.**
- **`devices/spectrum-device` (bc803aef).**
  - **Refresh was held at the averager's 65,536-sample hop.** It is now the hop at every rate, and
    the averager still moves 1/Avg per analysis.
  - **The clamp test started from the value it expected.**
- **`devices/live-amp` (c4d52f4b):** the merges above.
- **`devices/shifter-nan` (14472a18).** A finite 3e38 input still latched Frequency mode at NaN. Inputs are
  now also bounded at 1e6.

The review refuted one finding: that `analysis()` could be read during `set()`.

**CTest:** `devices/auto-shift` at 9149da91 passed 284/284 at PD_FLOATSIZE 64. At 32,
`adi_scan_cache_tests` (plug-in scanning, not this lane's) failed once under load at 48 s. Run alone it
passed, in 16 s. The fixed stack is now running on `devices/live-amp` and `devices/shifter-nan`.

## 2026-11-22 — DEV-22, the chapter's first missing effect: Amp — lands after 3 December

**PR requested:** head `devices/live-amp` (c4d52f4b; first logged as 17130593), base `main`, after `devices/spectrum-device`.
It stops at this branch if the FOR THE DIRECTOR answer on the missing effects is no.
- **What it is.**
  - Seven voices, each with an input high pass and a mid bell, a preamp gain and bias, and a power
    drive.
  - The preamp is tanh(g x + b) − tanh(b), with the DC blocked.
  - The tone controls: Bass, Middle and Treble.
  - The power stage is tanh(d y)/d, driven by Volume.
  - Then Presence, Output, Dry/Wet, and Mono or Dual.
- **Tests:**
  - `adi_live_amp_tests` 17. At −100 dBFS, five settings follow the documented chain within 0.05 dB.
    The chain is computed apart from the signal path, from the filters' designs and the preamp's slope.
  - Gain raises the distortion, and the bias makes a second harmonic.
  - A huge input stays bounded. Mono and Dual. Block sizes, NaN, no allocation.
  - parity max error 0;
  - the catalogue holds 37 built-ins.
- **Not oversampled:** a high voice driven hard aliases.
- **Reading 35, added to the ONE list.**

## 2026-11-22 — PLAN DEV-22: Spectrum as a track device, the lane's side — lands after 3 December

**PR requested:** head `devices/spectrum-device` (a4ec9d18), base `main`, after `devices/auto-shift`.
- **What it is.**
  - `dsp::SpectrumDevice` passes the signal untouched.
  - It turns Block, Channel, Refresh and Avg into the settings for the shared analyser: the FFT size,
    the channel, the hop, and the averager's decay, so that each analysis moves the average 1/Avg of
    the way.
  - The analysis runs on the Mix View's shared analyser (ADR-0183 d7, MX-12, MX-30). The device runs
    no FFT of its own (CTL-07).
  - `pd/devices/Spectrum.md` is the contract and the hand-off: the tap at the device's place, the
    worker, the view, and no analysis while the panel is hidden.
- **Tests:**
  - `adi_spectrum_device_tests` 22. Driven with the device's settings, the analyser reads a sine of
    0.5 at −6.02 dBFS at three sizes. L+R reads one side at half. Avg 4 gives 1 − (3/4)^k after k
    analyses. Pass-through and no allocation.
  - parity max error 0;
  - the catalogue holds 36 built-ins.
- **Needs the Mix View seat (not this seat's path):** the tap and the worker of §4.
- **Reading 34, added to the ONE list.**

## 2026-11-22 — the Shifter takes a non-finite input as silence (found building Auto Shift)

**PR requested:** head `devices/shifter-nan` (90cb51cb), base `main`. A fix to a built device, not
new work, so it is not held for 3 December.
- **What it fixes.** A NaN or infinite input sample stayed in the Shifter's pitch, delay and dry lines,
  so everything after it came out NaN, in every mode. Each input sample is now made finite first.
- **Tests:** `adi_shifter_tests` 53. A NaN block with an inf in it, then a tone, in all three modes
  with and without the delay: the tone comes out finite. The old core fails every case, and every
  earlier measurement is unchanged.

## 2026-11-22 — PLAN DEV-22: Auto Shift on the Tuner's detector and the Shifter — lands after 3 December

**PR requested:** head `devices/auto-shift` (9149da91), base `main`, after `devices/dj-filter`.
- **What it is.** `dsp::AutoShift` reads the pitch with the Tuner's `PitchDetector` and moves it with
  the Shifter's Pitch mode. These are the two cores that PLAN DEV-22 names for it.
  - Every 64th sample it retunes to Shift + Detune + the correction. The correction is Strength times
    the distance to the nearest note of the scale, and Smooth glides it.
  - An unclear reading holds the last correction.
  - Formant, vibrato and the MIDI scale input are not built.
- **Tests:**
  - `adi_auto_shift_tests` 19: 446 Hz comes out at 440 within 1 cent; the 50 % Strength, C major,
    Strength 0, Correction off and Detune cases; Shift +12 commands the right shift; the glide; the
    hold through noise (a plant that resets it fails); block sizes; NaN; no allocation.
  - parity max error 0;
  - the catalogue holds 35 built-ins.
- **Not claimed:** a large Shift sounds as the Shifter does. Its lines sit a crossfade rate apart under
  an envelope at the commanded pitch; at 446 Hz × 2 the strongest line is 886 Hz, not 892. The
  Shifter's own tests cover that sound.
- **Reading 33, added to the ONE list.**

## 2026-11-22 — items 19 to 22 reviewed: eleven findings, all fixed

An adversarial review of the five new cores (a finder for each branch, three skeptics for each
finding) confirmed eleven findings, each by all three skeptics. Each fix is on its own branch, with a
test that fails on the old code. Each lower branch is merged up the stack, so the stacked PRs keep
their order. The heads below replace those logged for items 19, 20 and 22.

**PR requested** (the same branches; new heads):

- **`devices/tuner` (485425e0).**
  - **A DC offset read as a confident false pitch,** and the Tuner held it. Alone or under a low noise
    floor, 56 of 72 cases did; none do now. The window's mean is removed, and a window under
    −80 dBFS RMS reads nothing.
  - **`prepare()` could throw out of Pd's DSP callback.** It is now noexcept: a failed allocation
    leaves the detector inert, and the Tuner passes the signal.
- **`devices/erosion` (993d8b26).**
  - **The noise modulator was up to 12 dB short of its stated RMS.** It was scaled by a small-angle
    bandwidth. It is now scaled by the band pass's exact noise gain, α/(1+α), and is 0.48 (0.5 clipped)
    at every band.
  - **Above about 600 kHz the swing left the ring.** It is now held at 61.5 samples.
  - **The NaN check looked at a later block.**
- **`devices/vinyl-distortion` (d6872b77).**
  - **Stereo pinch kept the mid clean only when L = R.** The pinch now comes from the mid's band.
  - **Moving Tracing Drive gave a 0.48 thump.** The DC is now blocked before Drive scales it.
  - **A 1.5e19 input came out as inf.**
- **`devices/filter-delay` (ecfcad4a).**
  - **Synced times at 20–40 BPM were cut to 6 s.** The lines now hold 12 s.
  - **The exact-arrival check allowed 1e-7.** It is now bit for bit, and fails without the snap.
- **`devices/dj-filter` (664adb8c).**
  - **Leaving the centre, or sweeping across it, clicked** (0.49 on a 0.5 sine). The fix is in Auto
    Filter's DJ type: on a change of side, the SVF takes the new side's state at rest.
  - Auto Filter's 166 checks and parity are unchanged.

**CTest (`ctest -LE acceptance`):**
- `devices/channel-eq` (776ad5eb): 277/277 at 32 and 64.
- `devices/hybrid-reverb-nupc` (fb1c7e9a): 268/268 at 32 and 64.
- `devices/dj-filter` (729bc803, before the fixes): 283/283 at 32 and 64. The fixed stack is being run
  on `devices/auto-shift`.

**Found in passing, not this branch's to change:** `dsp::Shifter` passes a NaN input into its delay
line, where it stays (Auto Shift, next, sanitises its own input). Fixed on `devices/shifter-nan`
(entry above).

## 2026-11-22 — standing queue item 22: the DJ Filter on Auto Filter's cores — lands after 3 December

**PR requested:** head `devices/dj-filter` (729bc803), base `main`, after `devices/filter-delay`.
- **What it is.** `dsp::DjFilter` holds an `AutoFilter` at its DJ type. Filter drives Control, Output
  drives Output, and Dry/Wet drives Mix. Its response is Auto Filter's by construction, so a fix to one
  is a fix to both.
- **Tests:**
  - `adi_dj_filter_tests` 17: the output equals Auto Filter's (Type DJ, the same three controls) bit
    for bit, at seven Filter values and under automation across the centre, at block sizes 1, 64, 333
    and 4096. Another Auto Filter type does not (plant). 0 passes the signal, and each side filters.
  - parity max error 0;
  - the catalogue holds 34 built-ins.
- **Reading 32, added to the ONE list.**

**The standing queue is worked through.** Items 4, 5, 6, 13 and 14 stay parked on the lines logged
earlier. Next: the lane's unbuilt PLAN rows (DEV-22's Auto Shift on the Tuner's pitch detector, then
the Spectrum device's canvas).

## 2026-11-22 — standing queue item 21: External Audio Effect's canvas and panel contract — lands after 3 December

**PR requested:** head `devices/ext-audio-effect-contract` (4054d859, cut from `main`), base `main`. One file,
`pd/devices/External Audio Effect.md`.
- **The canvas.** It passes the dry signal at unity with 0 latency on the Pd path. The engine's node
  takes its place, recognised by the canvas's bytes.
- **The controls and ids:** 1 Gain, 2 Dry/Wet, 3 Hardware Latency, 4 its unit (display only),
  5 Invert. Audio To and Audio From are routing rows the engine owns. They are not parameters.
- **The latency fields:** the hardware's own, the interface's round trip, and the engine's block. Their
  total is reported while bypassed, and the dry is delayed to meet the return.
- **The hand-off to the engine (MX-24, DEV-05):**
  - the routing rows, and the node;
  - compensation;
  - a broken path that reports itself;
  - real-time renders;
  - the meters (the same publishing need as the Tuner's reading).
- **The tests the build must carry.**
- **Reading 31, added to the ONE list.**

## 2026-11-22 — standing queue item 20: Erosion, Vinyl Distortion and Filter Delay, cores and devices — land after 3 December

Three branches, stacked for the catalogue. Each is built, not verified.

**PR requested:** head `devices/erosion` (a1f60f9b), base `main`, after item 19.
- **What it is.** A very short delay, D (1 + m), read by four-point Lagrange interpolation. Its
  modulator is a sine (Sine), or white noise through a band pass at Frequency, Q = 1/Width (Noise: one
  modulator; Wide Noise: one for each channel). At Amount 100 % the delay swings over 0.2 ms.
- **Tests:**
  - `adi_erosion_tests` 21: Sine mode's carrier and first sidebands equal J0 and J1 of β = 2π f D
    within 0.05 dB, at three amounts. Noise peaks at the tone ± Frequency, and a narrow Width gathers
    it. Noise is alike in both channels and Wide Noise is not. Amount 0 is the input itself. Block
    sizes, NaN, no allocation, aliased planes.
  - parity max error 0;
  - the catalogue holds 31 built-ins.

**PR requested:** head `devices/vinyl-distortion` (6f75c80a), base `main`, after `devices/erosion`.
- **What it is.**
  - Tracing: its band b adds 2 Drive b² with the DC blocked, an even distortion.
  - Pinch: its own band adds f(g b)/g − b, with f tanh (Soft) or a hard clip (Hard). Stereo adds it to
    the left and takes it from the right, so the mid stays clean.
  - Crackle: decaying clicks at Density a second, up to Volume.
- **Tests:**
  - `adi_vinyl_distortion_tests` 34: the tracing's second harmonic is Drive A² within 0.05 dB, with
    nothing odd. The pinch's 3rd and 5th harmonics match a numerical Fourier sum of its curve within
    0.05 dB, Soft and Hard. Below the clip, Hard adds nothing. Stereo's mid is clean. Crackle count,
    volume and off. Both Drives at 0 give the input itself.
  - parity max error 0;
  - the catalogue holds 32 built-ins.
- **Not oversampled:** a hard pinch on a high tone aliases. Whether Live oversamples is in reading 29.

**PR requested:** head `devices/filter-delay` (6d8614fe), base `main`, after `devices/vinyl-distortion`.
- **What it is.** Three lines, fed from L, the mono sum and R. Each stores BP(in + Feedback y), so
  every repeat passes its band (Width octaves at Freq). Its time is Sixteenths at the song's tempo, or
  ms. A time change glides over 50 ms to a whole sample. Equal-power Pan, Volume, and a Dry for the
  whole.
- **The canvas** takes the tempo from `adi.transport`, as the Delay's does.
- **Tests:**
  - `adi_filter_delay_tests` 20: echoes at exactly Time, each Feedback times the last. Each line's
    feed. Equal-power pan. On off lets the echoes ring out. Synced times at 120 and 90 BPM, arriving
    at a whole sample. The band peaks at 0 dB at Freq and is 3 dB down at its edges. Feedback 95 %
    stays bounded. Dry with the lines silent is the input itself.
  - parity max error 0;
  - the catalogue holds 33 built-ins.

**CTest (`ctest -LE acceptance`):** `devices/channel-eq` 277/277 at PD_FLOATSIZE 32 and 64. The other
tips are queued.

**Readings 28, 29 and 30, added to the ONE list.**

## 2026-11-22 — standing queue item 19: Tuner, core, shared pitch detector and device — lands after 3 December

**PR requested:** head `devices/tuner` (eb460dc3), base `main`, after item 18 (stacked for the catalogue).
- **What it is.**
  - `dsp::PitchDetector`, for Auto Shift to share later. YIN's normalised difference, from an FFT
    autocorrelation, picks the lag. The exact difference at 1, 4, 16 … times the period, walked to
    its dip and interpolated there, refines it. The window holds two 30 Hz periods: 4,096 samples at
    44.1/48 kHz, 8,192 at 96 kHz. It estimates every quarter window, and `push()` allocates nothing.
  - `dsp::Tuner`: the signal passes untouched; the mono sum's pitch, note and cents against
    Reference are published through atomics for the UI.
- **Tests:**
  - `adi_tuner_tests` 26: 75 sines from 40 Hz to 4 kHz, at 44.1, 48 and 96 kHz, from one window and
    through the Tuner, all within 1 cent (worst 0.005 cents). The design's own 0.05 cent check fails
    with the multiple refinement removed (worst 0.72). A sawtooth and a weak fundamental under its
    octave read the fundamental. Silence and white noise read nothing. No allocation; aliased planes.
  - parity max error 0;
  - the catalogue holds 30 built-ins.
- **Needs the engine (not this seat's path):** a way for a native device to publish a reading to the
  UI (here: hz, note, cents, clarity). The Pd external passes audio only until then.
- **Reading 27, added to the ONE list:** a fresh Tuner as shown; 440 and 442 Hz; the hold after a note.

## 2026-11-22 — standing queue item 18: EQ Three, core and device — lands after 3 December

**PR requested:** head `devices/eq-three` (617cd9e4), base `main`, after item 17 (stacked).
- **What it is.** Linkwitz-Riley crossovers, LR4 (24 dB/oct) or LR8 (48 dB/oct). The low band passes
  the high crossover's allpass, so the three bands sum flat at unity. Each band has a gain from
  −70 dB (silence) to +6 dB, and a kill.
- **Tests:**
  - `adi_eq_three_tests` 19: the flat sum within 0.01 dB at both slopes; kills and gains against a
    complex model of the crossovers within 0.01 dB; the kill's depth; a plant without the allpass
    fails;
  - parity max error 0;
  - the catalogue holds 29 built-ins.
- **Reading 26, added to the ONE list:** a fresh EQ Three as shown. Pink noise with each band killed
  at both slopes, to settle the crossover family. The gain law near −∞.

## 2026-11-22 — standing queue items 11, 12, 15 and 16: the LFO and Shaper kernels, Align Delay, Auto Pan-Tremolo; item 10's review fixed — all land after 3 December

### Item 10's review, fixed

Item 10's review, three skeptics per finding, upheld three findings; all are fixed, each with a check
that fails on the code before it.
- **A Hybrid Reverb stage with no impulse** (a first load while audio runs, or a swap from an impulse
  too short for that stage) skipped its next period, a block-long hole in the tail. It now sums an
  arriving impulse from the whole history, as the head does. The check: a 30,000-tap impulse
  arriving at sample 5,000 must match the full convolution from A + 3,072 on. The error is 2.1e-9
  now, 0.0125 on the earlier code.
- **Delay's parity fixture** had LeftSync switched off, which made LeftNotes and the tempo change
  inert. Sync now stays on, and the slot moves the filter instead.
- **Echo's Post reverb**: see the previous entry.

### Item 11, lfo-kernel: Live's LFO modulator, the kernel

**PR requested:** head `devices/lfo-kernel` (46e5b70e), base `main`.
- **What it is.** DEV-07's device side (ADR-0249 d8): a modulation signal rendered sample by sample
  for the engine's evaluator to route (ADR-0230).
  - Shapes: Sine, Triangle, Saw Up, Saw Down, Square, Random, Wander.
  - Rate in Hz, or synced to a Division of the song.
  - Phase, Offset, Depth, Jitter, Smooth.
- **The same bits at every block size:**
  - free running, the phase advances by the same addition every sample;
  - synced, it is read from the song's sample position within its tempo segment, so a locate lands
    on the song's own phase;
  - random values hash the cycle's index.
- `adi_lfo_modulator_tests` 25; two plants caught. Reading 22, below.

### Item 12, shaper-mod-kernel: Live's Shaper modulator, the kernel

**PR requested:** head `devices/shaper-mod-kernel` (4c8ad6df), base `main`, after item 11 (stacked).
- **What it is.**
  - Up to 64 breakpoints, each with a curve (an exponential of 8 times the curve); the last segment
    runs into the next cycle, and two points at one time make a step.
  - `setPoints()` copies and sorts in a fixed array, so it allocates nothing.
- **The clock is shared.** The phase logic and the cycle hash move to `modulator_clock.hpp`, shared
  with the LFO. The LFO's output is unchanged, bit for bit: an FNV hash over every configuration is
  identical before and after.
- `adi_shaper_modulator_tests` 15; two plants caught. Its default shape is ADI's until reading 11.

### Item 15, align-delay: Align Delay, core and device

**PR requested:** head `devices/align-delay` (caadc5ef), base `main`, after item 9 (stacked).
- **What it is.** Left and right delayed apart in Time (ms), Samples, or Distance (metres or feet,
  at the speed of sound for the set temperature), with Link.
  - The delay is exact: a whole number of samples.
  - **It reports no latency.** The delay is the effect, and a host would compensate it away; whether
    Live reports it is on reading 23.
- **A real defect, found by parity.** The loop wrote the left output before reading the right
  input, and Pd hands an output plane that is an input plane. Both inputs are now read first, and a
  new check fails on the old loop.
- **Tests:** `adi_align_delay_tests` 14; parity max error 0; the catalogue holds 26 built-ins.

### Item 16, auto-pan-tremolo: Auto Pan-Tremolo, core and device

**PR requested:** head `devices/auto-pan-tremolo` (7e3c1e92), base `main`, after item 15 and item 12
(it takes both in by merge).
- **What it is.**
  - Pan: two LFOs Phase apart. Tremolo: one LFO for both channels.
  - Gain 1 − Amount × (1 − (1 + w)/2).
  - Wave Sine, Triangle, Saw Down or Random, with Shape and Invert.
  - Rate in Hz, or synced at the host's tempo.
- **Tests:** `adi_auto_pan_tests` 12; parity max error 0; the catalogue holds 27 built-ins.
- **Need, for the engine:** song position reaches only ADI-Shaper's native path, so a synced Auto
  Pan-Tremolo follows the tempo from its own start, not the bar line. The adapter's position path,
  given to every native core that has a `sync()`, would lock it, and the LFO and Shaper kernels'
  hosts too.

### Item 17, channel-eq: Channel EQ, core and device

**PR requested:** head `devices/channel-eq` (776ad5eb), base `main`, after item 16 (stacked).
- **What it is.** An 80 Hz Butterworth low cut, a 100 Hz low shelf, a bell of Q 0.7 at Mid Freq, an
  8 kHz high shelf and Output, as direct-form-I biquads in double.
- **Tests:**
  - each band's measured response is within a stated 0.01 dB of the design's analytic magnitude,
    and meets its anchor;
  - `adi_channel_eq_tests` 16;
  - parity max error 0;
  - the catalogue holds 28 built-ins.

### Readings added to the ONE list

22 (LFO), 23 (Align Delay), 24 (Auto Pan-Tremolo) and 25 (Channel EQ).

## 2026-11-22 — standing queue items 3, 7, 8, 9 and 10: every core settles after silence; Glue, Gate and Limiter are devices; Hybrid Reverb under the common bound

One review of items 1–3 and 7–9 ran with three skeptics per finding, and six findings were
confirmed. All six are fixed on their branches and merged up each stack; each fix comes with a
check that fails on the code before it. Item 10 had its own review, whose three confirmed findings are fixed too (next entry). CTest 268/268 at PD_FLOATSIZE 32 and 64
on item 10's tip (items 1-3 and 10), 271/271 on the Limiter's (items 7-9); lint clean.

### Item 3, denormal-tail: every built-in core's recursive states settle on exact 0 after silence

**PR requested:** head `devices/denormal-tail` (a53b1e68), base `main`, after items 1 and 2 (it is
stacked on them).
- **The audit.** 119 recursive states in the 16 built-in cores outside ADI-Shaper. Two skeptics
  confirmed each claimed gap, and 67 decayed into subnormals with nothing to stop them. Each now
  settles, in the codebase's own styles:
  - a 1e-30 floor;
  - a snap within 1e-12 of a smoother's target;
  - a snap before a float delay-line write;
  - for states in vendored headers, which stay untouched, a settle on every 64th sample since
    prepare: a grid of absolute positions, so where the host cuts its blocks never changes a
    sample;
  - the Vocoder's band envelopes at 1e-300, since a 1e-30 floor would be audible through
    `pow(sqrt(env), Depth)`.
- **The proof**, Part 3 of `adi_builtin_realtime_tests` (an optimised build):
  - **Settling.** Every built-in, at 64 and 128 frames: 8 s of signal, then 60 s of silence. Over
    the last second, with flush-to-zero off, no operation may read a subnormal. The check is x86's
    denormal-operand flag in the SSE status word.
  - **Cost.** Under the callback's own flush scope, the steady silence (the last 2 s of 30) costs no
    more than the steady signal at p99, within 25 % and 5 µs.
  - **Before the fixes**, 12 of the 21 built-ins read subnormals; after, none does. A plant dropping
    Utility's DC floor alone fails Utility alone.
  - **What the flag can see**, stated exactly in the test:
    - every float-held state, and every double whose time constant is under about 85 ms (a double
      turns subnormal only 708 time constants down);
    - a slower double's floor (Echo's envelope, Resonators' combs at a 2 s decay) is held by the
      audit and the code, not by this check.
- **The review's findings, fixed:**
  - **Echo's room**, the Reverb's float tank, was missed. A new check (Reverb 100 %, Feedback 0)
    reads subnormals from 17 to 22 s into the silence on the earlier code, and none now.
  - **Auto Filter's and Shifter's block-end flushes** made the tail depend on the host's blocks. They
    now settle on the grid. New impulse-into-silence checks at 32, 100 and 4,096 frames: 14 and 2 of
    them fail on the earlier code, none now.
  - **Part 3 now builds off x86.** Its guard excludes ARM64EC, as `floating_point_control.hpp` does.
  - **From item 1:** the measured-count check now takes the catalogue's count apart from the loop,
    and counts the Sampler's 32 slots. Restoring the old canvas filter fails both checks (20 of 21).
- **Found on the way, then fixed:** Echo with Reverb raised at the default Feedback ran away (about
  +23 dB). The Post reverb was fed back into the delay line; it now colours only the output, and
  only the Feedback location puts the room in the loop. A check fails on the earlier code.
- The Debug run skips Part 3; the CTest TIMEOUT is now 1800 s for the optimised run's 6 min.

### Items 7, 8 and 9: the Glue Compressor, Gate and Limiter as loadable built-ins — land after 3 December

Each takes the one before it in by merge, so the catalogue keeps ADR-0249 d7's order: Compressor,
Glue, Gate, Limiter. Each has the same parts:
- an external that prepares its core only when the sample rate changes, so a DSP rebuild keeps the
  core's state, and the Limiter's allocating `prepare()` runs once per rate;
- a canonical canvas, with ids fixed now and ranges ADI's until its reading;
- native dispatch;
- a catalogue entry (Audio Effects, Dynamics, stereo).

- **Item 7, Glue Compressor.** **PR requested:** head `devices/glue-compressor` (98e479db), base
  `main`, after `devices/compressor`.
  - 8 controls; reading 19.
  - Parity max error 0.
- **Item 8, Gate.** **PR requested:** head `devices/live-gate` (c76d2952), base `main`, after item 7.
  - 8 controls; reading 20.
  - It reports its Lookahead as latency: 10 ms is 480 samples at 48 kHz, kept while bypassed. A new
    check; a plant expecting 479 fails it.
- **Item 9, Limiter.** **PR requested:** head `devices/live-limiter` (ac6c8718), base `main`, after
  item 8 and `devices/limiter-offline`.
  - 6 controls; reading 21.
  - **Latency:** every Lookahead in both detector modes reports 72, 144 or 288 samples at 48 kHz,
    plus 10 in True Peak mode, kept while bypassed.
  - **Ceiling:** a -1 dB ceiling holds on +12 dB noise in every link (in M/S, on mid and side).
  - `adi_live_limiter_tests` 58 and `adi_dsp_tests` 433 unchanged.
- **The review's finding, fixed:** the Glue fixture's Release change set its default (Auto), so
  parity never left Auto.
  - The parity harness now checks that every fixture change moves a control off its canvas default.
    That found three older no-op changes, all toggles set to their default: Delay's LeftSync,
    Reverb's SpinOn and Saturator's HiQuality.
  - All four are fixed. Parity stays at max error 0 for every fixture: 427 checks on the Limiter's
    tip.
- The catalogue holds 25 built-ins on the Limiter's tip.

### Item 10, hybrid-reverb-nupc: Hybrid Reverb in non-uniform partitions — lands after 3 December

**PR requested:** head `devices/hybrid-reverb-nupc` (fb1c7e9a), base `main`, after item 3 (stacked
on it).
- **The stages:**
  - **the head:** the impulse's first 2,048 samples in 256-sample partitions, the existing code, so
    every impulse up to 2,048 samples renders exactly as before;
  - **then blocks of 1,024** from 2,048;
  - **then blocks of 4,096** from 8,192.
- **Why no callback carries a spike:** a stage of block B starting at S ≥ 2B − 256 has a whole block
  period to compute in. Its parts are summed by sample position, so every callback size renders the
  same bits. A 5 s impulse costs about 71 multiply-adds a sample, against about 940; the latency
  stays 256.
- **Tests:** impulses of 2,049 to 60,000 samples with taps on every stage edge.
  - They match a direct convolution: largest error 1.3e-8, against a budget of 1.5e-6 to 4.1e-6.
  - They render the same bits at callback sizes 1 to 4,097.
  - Cutting the impulse at either stage edge fails.
  - `adi_hybrid_reverb_tests` 72 (21 new), Pd 11.
- **Measured:** Hybrid Reverb's p99 is 12.6 % of the deadline at 64 frames and 11.3 % at 128 (it was
  17–28 %). Its 25 % exception is removed, and the whole realtime suite passes: 252 checks, 0
  failures.

## 2026-11-22 — standing queue items 1 and 2: the Sampler is timed and made cheap; the Debug run fits its timeout (#706's review)

Win's standing queue (`collab/prompts/2026-11-22-devices-queue.md`) replaces this lane's earlier
lists, and it is worked top to bottom. Items 1 and 2 are the two fixes #706's review asked of
`devices/perf-pass`. Both are before 3 December.

They are cut from #706's integration branch (`land/devices-1122`), because the suite (perf-pass)
and the Sampler (sampler-loadable) first meet there. Once #706 lands, each branch's diff is its own
commits.

### Item 1, perf-sampler: the realtime suite times the Sampler

**PR requested:** head `devices/perf-sampler` (f153556c, two commits on `land/devices-1122`), base
`main`, after #706.
- **The gap.** The suite measured only catalogue entries with a Pd canvas, so the Sampler (native
  only) was skipped. Now every available built-in is measured:
  - the count measured is checked against the count available;
  - the Sampler is named;
  - a plant that drops the Sampler fails both checks.
- **The Sampler, timed.** With its 32 slots filled it took 38.5 % of the deadline at p99 (64
  frames). Each filled slot is a OneShot engine over every key, so every note plays 32 layers.
- **What made it cheap.** Every fix is bit-identical: an FNV checksum over every output sample of
  OneShot and the Sampler, at 64 and 128 frames, matches before and after.
  - **An idle engine plays silence without its sample loop.** OneShot takes this path when no
    voice sounds and no note-on arrives; the events still apply, in order. Each voice's filter is
    its own, so there is no tail. The Sampler skips mixing such a layer. An idle Sampler now costs
    27 µs per 64-frame block, down from 179.
  - **What reads only the parameters is computed per event, not per sample or voice:**
    - the sustain level, Vel > Vol and the output level;
    - the LFO's phase step;
    - the filter's coefficients, when the LFO does not move the cutoff.
  - **Work skipped when its result cannot change:**
    - the LFO's value, when every LFO amount is 0 (each use is a product with 0; only its phase
      and random state advance);
    - a voice's `exp2`, except when its pitch changes;
    - the filter's 1/q and denominator, now shared by both stages and both channels.
  - **The voice loop runs voice by voice** over each stretch between events, so a voice's state
    stays in registers. Each output sample still sums the voices in the same order.
- **Measured** (RelWithDebInfo; this machine runs about 30 % slower this afternoon than this
  morning):
  - Sampler p99 12.9 % of the deadline at 64 frames, 12.4 % at 128;
  - OneShot 0.3 % (it was 1.3 %);
  - no allocations.
- **Tests:**
  - OneShot 359, OneShot Pd 38, Sampler 119, built-in devices 244 and native parity 287 all pass
    unchanged;
  - CTest 268/268 at PD_FLOATSIZE 32 and 64 (MSVC Debug, /W4 /WX);
  - lint clean.

### Item 2, perf-debug-timeout: the Debug run fits its CTest timeout

**PR requested:** head `devices/perf-debug-timeout` (bb7f5c08, one commit on item 1), base `main`.
- **The problem.** A Debug run took about 37 min against the 900 s TIMEOUT.
- **The fix.** A debug build's times are not the product's, so Debug now counts allocations only,
  for a tenth as long:
  - every callback is still counted from the first, the warm-up second included;
  - the timing-only parts run in an optimised build alone: three of the four contention variants,
    and the breakdown from an empty project;
  - the variant with the timer and gestures together still runs in Debug.
- **Measured:** `ctest -L performance` in Debug finishes in 3 min 56 s, with 64 checks passing (44
  of them allocation checks). RelWithDebInfo still runs all 168.
- CTest 268/268 at 32 and 64; lint clean.

### Parked, with one line each (taken when they clear)

- **Item 4, DF-E39** (the MIDI effect kernels): waits on the engine's DEV-17 and DEV-18 reaching
  main.
- **Item 5, shaper-scope-feed:** `adi_shaper_scope.hpp` exists only on the clones panel stack held
  in #708, and the `builtin_host.cpp` loan must be widened by win.
- **Item 6, time-locate-revision:** waits on the clones seat's `AdiShaper::syncAtSample(quarterNotes,
  playing, sample, revision)`, which exists only on `clones/shaper-time` and is in no landing
  window. This lane's hunk is ready to write: in `native_devices.cpp`'s `syncShaper`,
  `core.syncAtSample(position->quarterNotes, position->playing, position->sample,
  io.transport ? io.transport->transportRevision : 0)` in place of `core.sync(...)`. Its test would
  be the clones seat's same-position regression, which is not pushed either. Needs table: clones,
  land `syncAtSample` or hand it over, and push the regression.
- **Item 13, beat-repeat-core:** waits on reading 18 and the contract branch landing.
- **Item 14, two-head-reader-shifter:** waits on the clones seat's extraction (step 1), which exists
  nowhere yet.

## 2026-11-22 — the post-3-December queue: Compressor, Glue Compressor, Gate, Limiter, and Beat Repeat's contract (all land after 3 December)

These five branches are win's item 3 for this week, in ADR-0249 d7's order. **Each lands after
3 December; none is asked to land before.** Each is built, not verified: its ranges, defaults and
laws are ADI's until a Live 12.4 reading. The readings are on the ONE list above:
- 17 for the Compressor;
- 19–21, new, for the Glue Compressor, Gate and Limiter;
- 18, revised to the clones seat's method, for Beat Repeat.

No parity is claimed until a reading agrees.

Four review rounds covered them, each with three skeptics per finding: the cores and the contract;
then the device commit and the fixes; then the second fixes; then the brickwall's re-plan. Every
confirmed finding is fixed on its branch, with a check that fails on the code before the fix. The
last round confirmed one test gap, now closed, and nothing in the code.

### Compressor, a loadable built-in — lands after 3 December

**PR requested:** head `devices/compressor` (3ae7ab4c), base `main`, after `devices/sampler-loadable`
lands: it is stacked on it, and takes `devices/compressor-core` in by merge (6777df67).
- **The device.** The DEV-22 core (`dsp/live_compressor.*`: Peak, RMS and Expand; Lin and Log
  envelopes; lookahead 0, 1 or 10 ms, reported) becomes a built-in:
  - the `[adi.compressor~]` external;
  - the canonical canvas `pd/devices/Compressor.pd`, 11 controls, ids 1–11;
  - native dispatch in the built-in host;
  - the catalogue entry `org.adidaw.builtin.compressor` (Audio Effects, Dynamics, stereo).
  - The catalogue now holds 22 built-ins, with its canvases index-aligned.
- **Fixed after review: the core's recursive states decayed into stuck denormals** after about 35 s
  of silence. Nothing on a built-in's path sets flush-to-zero, and the Compressor is never
  suspended. Each state now settles on exactly 0 once it is negligible; a check fails on the old
  core in all four detector and envelope pairs.
- **Tests:**
  - native against Pd parity, max error 0 (adi_pd_native_devices_tests 300);
  - adi_builtin_devices_tests 231;
  - adi_live_compressor_tests 39;
  - the object walk, 79.
- CTest at PD_FLOATSIZE 32 and 64 (MSVC Debug, /W4 /WX): 268/268 at both; lint clean.

### Glue Compressor, the core — lands after 3 December

**PR requested:** head `devices/glue-compressor` (de5c6bd2, two commits on main), base `main`.
- **What it is.** A stereo-linked bus compressor that works in dB, with zero latency and no
  allocation:
  - Ratio 2, 4 or 10:1;
  - seven Attack choices, 0.01–30 ms;
  - six Release choices, 0.1–1.2 s, or Auto (two stages: fast from a short peak, slow from a held
    level);
  - Makeup, Range, Dry/Wet, and Soft Clip toward −0.5 dBFS.
- **Fixed after review:**
  - switching to Auto mid-release stepped the reduction down about 3.1 dB; Auto now starts from
    the reduction as it is;
  - the states decayed into denormals under the threshold; they now settle on exactly 0.
- adi_glue_compressor_tests 23; plants caught. No canvas or catalogue entry yet: that waits on
  reading 19, as the Compressor's waited on 17. CTest 268/268 at PD_FLOATSIZE 32 and 64; lint clean.

### Gate, the core — lands after 3 December

**PR requested:** head `devices/live-gate` (18cf40a3, two commits on main), base `main`.
- **What it is.** A gate linked on the louder channel:
  - it opens at Threshold and closes below Threshold less Return (the hysteresis), after Hold;
  - Attack and Release are one-poles;
  - Floor's low end is silence;
  - Flip;
  - Lookahead (0, 1 or 10 ms, reported) delays the audio, never the detector.
- **Fixed after review: Flip mixed the smoothed gain with an unsmoothed Floor.** Moving Floor gave
  +6 dB, or silence for a whole Release. The one-pole now runs on the gate's opening (0 to 1), so
  the gain never leaves Floor to unity, and the law at a fixed Floor is unchanged.
- adi_live_gate_tests 49; plants caught. CTest 268/268 at PD_FLOATSIZE 32 and 268/268 at 64; lint
  clean.

### Limiter, Live's, and a change to the brickwall — lands after 3 December

**PR requested:** head `devices/live-limiter` (3a3bce5f), base `main`, after
`devices/limiter-offline` lands: it is stacked on it.
- **What it is.** Live's Limiter on ADI's lookahead brickwall:
  - Gain, Ceiling, Release;
  - lookahead 1.5, 3 or 6 ms, reported, with 10 samples more in True Peak mode at 48 kHz;
  - Link: Stereo, L/R or M/S;
  - Mode: Standard, Soft Clip or True Peak.
  - Under the ceiling, every mode and link passes the input delayed, bit for bit.
- **Fixed after review:**
  - **A Link change spliced in an idle brickwall's stale or empty delay.** Each link now has its
    own brickwalls, mixed by weights that sum to one. A new path waits a latency while its delay
    fills, then rises over at least another while the others give way. A change at any point of a
    fade mixes on from wherever the output is: there is no step, and the ceiling holds.
  - **A lowered Ceiling let the samples already in the delay leave above it.** A clamp overshoots
    between the samples; a scale attenuates twice and then steps. So the fix is in the brickwall
    (`dsp/limiter.*`, LookaheadLimiter), and **win, this touches the export's brickwall too**:
    - the detector keeps each step's level, and the hold is the ceiling over the window's loudest;
      it is the same value as before, and adi_dsp_tests' 433 checks pass unchanged;
    - a lowered ceiling re-plans the gains already set, cutting each boxcar entry to the new
      ceiling over the loudest sample it covers. The guarantee then holds exactly from the next
      sample;
    - quiet material is untouched, the cost is O(lookahead) once per lowering, and nothing is
      allocated.
  - **In M/S, a NaN on one channel silenced the other.** Each channel is now screened before the
    encode.
- adi_live_limiter_tests 58; each fix's checks fail on the code before it; plants caught. CTest 268/268
  at PD_FLOATSIZE 32 and 64; lint clean.

### Beat Repeat, the contract — lands after 3 December

**PR requested:** head `devices/beat-repeat-contract` (e243ab13, three commits on main), base
`main`. Documentation only: `pd/devices/Beat Repeat.md`.
- **What it fixes:**
  - the UID, and 17 control ids, for good (names, ranges and defaults come from reading 18);
  - the core's design;
  - the clones seat's Repeat Cell test method: per-frame markers and a frame oracle;
  - ADR-0249 d7's tests;
  - reading 18's items.
- **After review:**
  - the slice is copied at capture, so a repeat held by Repeat plays the same slice to the end;
  - the input history holds one Grid at most;
  - the reach is stated, since a bar follows the meter: 16 quarter notes at 20 BPM, 48 s, about
    37 MB a buffer at 96 kHz. A longer Grid is shortened from the trigger until reading 18 says
    otherwise.
- **For the clones seat: one history kernel with ADI-Shaper's Time stage?** The answer turns on
  reading 18's capture direction.
  - **If the slice ends at the trigger, yes.** Beat Repeat keeps a backward ring of one Grid. One
    kernel, with the capacity set by each caller, would share the ring's write, its valid count,
    and a whole-frame read at an integer age (Beat Repeat's copy at capture).
    - The fractional read is Time's own; Beat Repeat's read head reads its slice copy.
    - Each caller keeps its oldest-frame rule, its locate policy and its slice storage.
  - **If the slice starts at the trigger, no.** There is no input ring, so nothing to share.
  - **Your three conditions are set aside on purpose; none of them decides it.**
    - The oldest frame and the locate retention are each caller's own policy.
    - The write source is already fixed: ADR-0249 d7 makes the history the input.
  - The kernel would live in `dsp/` under this lane, with you reviewing the hunk Time takes.

## 2026-11-22 — the 3 December performance pass: every built-in's callback at 64 and 128 frames; Hybrid Reverb no longer spikes

**PR requested:** head `devices/perf-pass` (920fac00, two commits on 0ea10b8e), base `main`. It adds
a new suite, `adi_builtin_realtime_tests`, which builds with `-DADI_ACCEPTANCE=ON` and runs under
`ctest -L acceptance` or `-L performance`. It also changes one built-in, Hybrid Reverb.

- **What the suite measures, at 64 and 128 frames.**
  - **Each built-in alone, all 20.** Noise goes in, the first parameter moves every block, and an
    instrument gets a note every 100 ms. Every declared sample slot is filled (Hybrid Reverb gets a
    5 s impulse). ADI-Shaper runs working: every stage on, three bands, moving curves. At its
    defaults it would only copy its input.
  - **WTS-01's project.** It is built through the app's own document calls: OneShot with a sample on
    an armed MIDI track, then Saturator, EQ Eight, Reverb, Delay and ADI-Shaper, worked through the
    device strip's gestures. It plays from the start, with live notes and the app's timer.
  - **A breakdown,** from an empty New project up the chain, one device at a time.
  - **A contended pass.** The callback runs on its own thread, paced like a device, while the timer
    and a control's gestures run on another, as in the app.
- **What it checks.**
  - **Allocation.** No allocation in any callback, from the first one: every `operator new` form,
    plus the CRT hook (`malloc` too) in an MSVC debug build.
  - **Stalls.** No callback over half the deadline that recurs block for block over two runs of
    the same input. That is a device's own stall (a first touch, a page, a lock), not the
    machine's preemption.
  - **Cost.** The p99 of a built-in alone under 15 % of the deadline (Hybrid Reverb 25 %, stated
    below), and the whole project's under 50 %.
  - **Contention.** Under contention, every raw callback inside the deadline.
  - Timings are judged in an optimised build only; Debug checks allocations.
- **Measured** (RelWithDebInfo on this machine).
  - **No built-in allocates, locks or stalls.** The built-ins' audio paths were also audited for
    locks; the only mutex is pd_builtins' registration table.
  - **Alone:**
    - the heaviest by p99 are Saturator (7–10 %), Vocoder (7–9 %), Reverb (6–8 %), Multiband
      Dynamics (5–6 %) and ADI-Shaper working (4 %);
    - everything else is under 4 %.
  - **The project:**
    - p99 is 31–36 % of the deadline at both sizes, recurring worst 456–880 µs;
    - an empty New project already takes 90–150 µs per 64-frame callback, 11–14 % of the deadline.
      That is the engine's own share.
  - **Contended:** the worst callback near a gesture's commit is no worse than one apart from it.
    All four variants are alike, so no lock shows.
- **Fixed: Hybrid Reverb spiked one callback in four.**
  - **The cause.** Its uniformly partitioned convolution did the whole multiply-accumulate over
    every partition at the 256-sample boundary. With a 5 s impulse (937 partitions), p99 was 634 µs
    at 64 frames (48 % of the deadline), with a recurring worst of 838 µs.
  - **The fix.** The next block's older partitions are now summed a share at a time over the 256
    samples before it, on a schedule set by the sample position alone. The boundary keeps the
    newest partition and the FFTs.
  - **Now:** recurring worst 231 µs.
  - Its tests are unchanged and pass: exact against direct convolution, bit-identical across block
    sizes, Pd parity.
- **Left, for win to place:**
  - **Hybrid Reverb's steady cost.** With a 5 s impulse, p99 is still 17–20 % of the deadline at an
    even load. Non-uniform partitioning would cut it; that is a convolution redesign, after
    3 December.
  - **For the engine seat:** an empty New project's 90–150 µs per 64-frame callback is the largest
    share of the project's cost. It is worth profiling before 3 December if the session runs at
    64 frames.
- **Reviewed:** three dimensions with three skeptics per finding. Five confirmed gaps in the first
  version of the suite are fixed in the second commit: ADI-Shaper timed as a copy, missing samples,
  warm-up allocations uncounted, a project that never played, and contention untested.
- CTest 267/267 (MSVC Debug, /W4 /WX); the suite passes in Debug (70 checks) and RelWithDebInfo
  (156).

## 2026-11-20, late — for win: the twelve requested branches together on main, and where the queue stands

**Together.** On 9b35613a (after #697 and #699), all 12 open requests merge cleanly onto main and
with one another, in this log's order:
1. `sampler-loadable`
2. `df-e57` (with DF-E61)
3. `pd-exact-position`
4. `dev09-gaps`
5. `loudness-robust`
6. `limiter-offline`
7. `loudness-reference`
8. `dsp-provenance`
9. `loudness-bench`
10. `pd-ar8`
11. `pd-fuzz`
12. `loudness-ffmpeg`

The local union, built and never pushed, passes:
- CTest 268/268 at PD_FLOATSIZE 32 and 64 (MSVC Debug, /W4 /WX);
- under ASan: Pd engine 702, object walk 97, DSP 433, loudness 87, built-in devices 244.

**The five cross-platform patterns** (win.md, 20 November). There is no GCC or Clang compiler on
this machine, so every changed C++ file on all 12 branches went through Clang's own front end
instead:
- clang-tidy 19 at 64 bit, with `-Wmost -Wextra -Wconversion -Wsign-conversion -Wshadow
  -Wmisleading-indentation -Wunused-function`;
- plus a textual check for a second statement after an unbraced if/for body and for
  `decltype(&fclose)`.
All clean. The one textual hit was a single statement after a `for` header.

**The queue (your order of 20 November).** Done, as far as this lane can take it before rulings or
readings:
- the Sampler (option 1);
- DF-E57, with DF-E61;
- the song-position field;
- this lane's DEFERRED rows;
- the non-new-effect rows of ADR-0248 d8:
  - DEV-09: gaps, AR8, the PD2 fuzz;
  - DEV-28: robustness, the Tech 3341 reference, the benchmark, provenance, LD6;
  - EX4/LD5: the devices half.

What waits:
- **On win's rulings:**
  - the AR8 cross-instance dropout, now measured at about 10 ms per large open;
  - runaway loops;
  - the definition of "LRA not yet stable";
  - whether the export brickwall should read finer than 4x (also under FOR THE DIRECTOR).
- **On win (.github):**
  - an ASan leg;
  - libebur128's pin and leg;
  - an ffmpeg leg with `ADI_REQUIRE_ORACLES=1`.
- **On engine-win:** the export half of EX4/LD5.
- **On the director's session:**
  - readings 1–18;
  - the Release run of `adi_block_benchmark --loudness`.
- **After 3 December:** the new effects (the Compressor core waits on its branch), Auto Shift after
  Tuner, External Audio Effect, Spectrum, Auto Pan-Tremolo, and the LD5/DEV-22 Limiter link.

## 2026-11-20 — DEV-28 / LD6: the one loudness meter against ffmpeg's ebur128 filter, the outside check on its 10 Hz range

**PR requested:** head `devices/loudness-ffmpeg` (a718db94, two commits on d0af0e15), base `main`.
It adds one test, `adi_loudness_ffmpeg_tests`, and its lines in `cmake/devices/loudness.cmake`.

- **Why ffmpeg, beside libebur128.** libebur128 (LD3) hops the short-term window at 1 Hz, so its
  loudness range is not Tech 3342's. ffmpeg's `ebur128` filter hops it at 10 Hz, as ADI's meter
  does. It is the outside check on the 10 Hz LRA.
- **How it measures.**
  - Each signal is written as a 32-bit float WAV, with byte order fixed.
  - The ffmpeg program on PATH runs `ebur128=metadata=1`. It is test-only; nothing of it is linked.
  - ADI's I and LRA are held to the filter's last `lavfi.r128.I` and `.LRA`.
  - Only at 48 kHz, because the filter resamples anything else.
  - Signals: Tech 3342 cases 1–4, a speech-like noise programme with pauses, and a tone between
    silences.
- **Measured.** The range agrees to 0.001 LU on every case, and I to between 0.005 and 0.007 LU.
  The suite holds them to 0.01 and 0.02 LU; the plan asks for 0.1.
- **Plant:** a 94th percentile in ADI's range fails the speech-like case.
- **Without ffmpeg.** The suite exits 77, which CTest reports as Skipped, not Passed.
  `ADI_REQUIRE_ORACLES=1` turns that into a failure. Both paths were checked.
- **Reviewed** on three dimensions with three skeptics per finding. One defect was confirmed: ffmpeg
  ran without `-nostdin`. It is fixed in the second commit, along with the skip code, a guard
  against a throw, and comments that no longer imply a CI leg exists.
- CTest 268/268 at PD_FLOATSIZE 32 and 64. The new suite takes about 15 s.
- **For win (.github):** no runner image ships ffmpeg, so today the check is skipped everywhere.
  One Linux leg needs `apt-get install -y ffmpeg` and `ADI_REQUIRE_ORACLES=1` on its ctest step.

## 2026-11-20 — DEV-09 PD2: a seeded patch-text fuzz, and the two Pd faults it and its review found

**PR requested:** head `devices/pd-fuzz` (56148ce7, two commits), stacked on `devices/pd-ar8`, base
`main`.

- **The fuzz** (plugdata's PD2 scenarios), in `adi_pd_object_walk_tests`.
  - Every shipped patch is mutated: bytes cut, hostile tokens put in, lines duplicated, swapped or
    cut short.
  - It uses a seeded generator of our own over a sorted, LF-normalised corpus, so the mutants are
    the same on every platform.
  - Each mutant goes through the walk, which must return and name every refusal by class and box.
  - Each mutant the walk allows is opened in Pd, prepared and run for 16 blocks, as DEV-10's loader
    would. It must run, or fail with a message; never crash or hang.
- **Found by the fuzz: a crash.**
  - The 12th mutant put `#X connect 8 0 1 0` on a canvas with no objects.
  - Pd's `canvas_connect` walked off its empty object list (NULL `src->g_next`, in g_editor.c) and
    took the process down.
- **Found by the review: a hang.**
  - A box that failed to create (`[osc~ foo]`) is given dummy ports up to a connection's port
    number, and that number comes from the patch unbounded.
  - `#X connect 0 100000000 1 0` made Pd build 1e8 outlets, walking the list for each one. That
    hangs, and once memory runs out it writes through NULL.
- **Both are now fenced** in the g_editor.c overlay, and fail as Pd's own bad connections do, by
  name on the console:
  - an empty canvas;
  - a negative index;
  - a port past 1024 on a box that failed to create.
- **Tests:** `adi_pd_object_walk_tests` 97.
  - The crash case fails before the fix (0xC0000005). The port case has a watchdog: with the bound
    removed, the open hangs.
  - The fuzz: 600 mutants walked, 150 opened. Everything is clean under ASan, here and on two more
    seeds at 4000 mutants and 1500 opens each.
  - CTest 267/267 at PD_FLOATSIZE 32 and 64.
- **Reviewed** on three dimensions with three skeptics per finding. All four confirmed findings
  are fixed in the second commit: the hang, the corpus order, line endings, and a check that could
  not fail.
- **For win.** The walk does not read `#X connect` records or object arguments. Both are Pd's to
  survive, and the fuzz now holds Pd to that.
- **Left on PD2:** the state round-trip scenario. It comes with DEV-10's user patches, which are
  what would carry a patch's state.

## 2026-11-20 — DEV-09 AR8: nothing made inside a tick waits on another Pd device; the cross-instance dropout measured

**PR requested:** head `devices/pd-ar8` (bc48d53f, two commits), stacked on `devices/dev09-gaps`
(which shares its overlay and test files), base `main`.

- **The defect** (gap audit item 3). For an object Pd does not know, `new_anything` swaps the
  tick's read lock for Pd's process-wide write lock (`pd_globallock`) and searches the disk for an
  abstraction. Dynamic patching can do that inside a tick: a `[delay]` firing `[; pd-x.pd obj …(`, or
  a parameter's message. Then the audio thread waits on every other Pd device's tick and on every
  open editor.
  - In the test before the fix, only 3 of 40 segments returned while another device's editor held
    its instance. The rest waited out its 5 s hold.
- **The fix.** `LibPdEngine::lockedTick` now marks its ticks, using a thread-local flag in the
  m_class.c overlay. Inside a marked tick, these are refused by name ("ADI refused creating 'x'
  while audio runs"):
  - an unknown object (`new_anything`);
  - an abstraction whose name is already registered (`do_create_abstraction`, in the s_loader.c
    overlay), which would otherwise be read from disk on the audio thread. That includes a
    `[clone]` resize.
  - The box stays broken, as for any object Pd cannot create, and `pd_newest` is cleared so
    nothing stale is taken for it.
  - Opening a patch, abstractions included, and an editor's own changes are unchanged.
- **The cross-instance dropout** (item 1), **measured for win's ruling.**
  - Another device opened a 1000-object patch 20 times, about 30 ms an open.
  - The longest unbroken run of skipped ticks on this device was 7–10 ms of wall time across two
    runs. That is the write-locked part of the load (`libpd_openfile`).
  - The test checks, without a clock, that this device's ticks are skipped and its segments
    return while the other is still opening, and that nothing skips before or after.
  - **So the ruling's question is concrete:** opening a large Pd patch silences every other Pd
    device for roughly 10 ms. Either record that in the row, or fund an overlay that does not hold
    the write lock across `glob_evalfile`.
- **Tests:** `adi_pd_engine_tests` 676, written first (2 failures before).
  - Plants, each caught: tick not marked (2 failures); abstraction fence removed (1).
  - CTest 267/267 at PD_FLOATSIZE 32 and 64; the Pd engine tests also pass under ASan.
- **Reviewed** on four dimensions with three skeptics per finding: Pd semantics, locking, tests,
  portability. Every finding was refuted as a defect. Two fair points are fixed in the second
  commit:
  - the registered-abstraction path, so whether a creation is refused no longer depends on what
    any device loaded before;
  - an actual check of "never waits".
- **Left on DEV-09 for this lane:** the PD2 fuzz, now done (the entry above); its round trip comes with DEV-10.
- **Still awaiting win:** the runaway-loop ruling, and the ASan CI leg.

## 2026-11-20 — DEV-28: the one loudness meter's cost per callback, measured by the block benchmark

**PR requested:** head `devices/loudness-bench` (82397f06, two commits on f45c6f4a), base `main`.
One file, `tools/benchmark_blocks.cpp`, which is built only with `ADI_BUILD_BENCHMARKS=ON`.

- **What it adds.** `adi_block_benchmark --loudness` times the one meter, `dsp::LoudnessMeter`, at
  steady state, per `process()` call:
  - true peak on, at every block size from 32 to 4096, in stereo and in 5.1 with BS.1770's weights;
  - p50, p99 and max, plus p99 as a percentage of the callback's deadline.
  - It is the meter Main reads once N13 is rewired; until then Main runs its own.
  - 3.1 s of tone run untimed first, so every window, the maxima and the range histogram are
    already working.
  - A row is published only if the tone reads as itself: momentary, short-term and true peak
    correct, and the range histogram in use.
  - It takes only `--iterations`.
- **A local Debug reading, not the measurement.** p99 is about 3.5–5 % of the deadline in stereo and
  10–12 % in 5.1.
- **Reviewed** on two dimensions with three skeptics per finding. One defect was confirmed: the
  first version timed a fresh meter, so the short-term window and LRA never ran in the small-block
  rows. It is fixed in the second commit, along with two points from refuted findings: the meter's
  name, and the option guard.
- **For the director's session (an ADI reading, nothing of Live's).** In a Release build with
  `-DADI_BUILD_BENCHMARKS=ON`, run `adi_block_benchmark --loudness` and bring back its CSV. That is
  ADR-0203 d7's "measured" figure for the always-on meter.

## 2026-11-20 — DEV-28 provenance: libebur128 credited for the K-weighting; what the meter was written from (LD1)

**PR requested:** head `devices/dsp-provenance` (ba851798, one commit on f45c6f4a), base `main`.
Comments only.

- **libebur128.** `AutoGain::kShelf` and `kHighPass`, the meter's K-weighting, use libebur128's
  analog prototypes, with no credit until now.
  - All six constants match `ebur128.c`'s `ebur128_init_filter` at v1.2.6 (67b33abe): f0, G and
    Q for each stage, and the shelf's Vb exponent.
  - So do the bilinear-transform formulas.
  - The source now credits it and carries libebur128's MIT notice, as its licence asks for a
    substantial portion. The notice was checked word for word against its COPYING.
- **LD1.** `loudness.hpp` now says the meter was written here from BS.1770-4 and Tech 3341/3342.
  LD1 named FilmCraft's meter as the base, but its source was not on this machine and none of it is
  in the meter, so no notice is carried for it.
- **For win:**
  - an EXTERNAL-CODE row for libebur128 (MIT), covering the formulas in `auto_gain.cpp` now, and
    the test-only oracle when LD3's pin lands;
  - whether PLAN's "Code taken: FilmCraft's loudness meter as the base (LD1)" should read
    "written from the standards; FilmCraft's meter not used".
- CTest 267/267 at PD_FLOATSIZE 32 and 64.

## 2026-11-20 — DEV-28: Tech 3341's true-peak cases held to an independent reference, the maxima to the reference's 10 ms grid

**PR requested:** head `devices/loudness-reference` (c011d274). It stands on `devices/loudness-robust`
and merges `devices/limiter-offline` (6aede01d), so **win lands it after both of those**. Tests
only; no product code changes.

- **The problem.** This is the DEV-28 gap audit's third devices item. The meter passed Tech 3341 on
  the EBU's tolerances alone: +0.2/−0.4 dB for true peak and ±0.1 for the maxima. That let a true
  peak reading 0.15 dB low, or a maximum 0.05 LU high, pass.
- **The shared reference.** The independent true peak moves into a shared test header,
  `tests/true_peak_reference.hpp`, used by both suites. It now skips a channel shorter than its
  kernel instead of reading out of bounds.
- **Cases 15 to 23, true peak.**
  - **The synthesis first.** The reference reads each synthesized signal's own crest:
    - cases 15 to 19 come within 0.01 dB of the sine's crest;
    - cases 20 to 23 read −0.077 dBTP. That is what the 511-tap low-pass leaves of the EBU's 0.0
      design value.
  - **Then the meter, held to that crest.** It may never read more than 0.01 dB above it, and may
    read below it only by its own bound:
    - **cases 15 to 19:** the 4x grid's miss at each sine's known crest phase. That is 0 for
      case 15, whose crests fall on samples; 0.168 dB for 16 and 19; 0.075 for 17; and 0.042
      for 18.
    - **cases 20 to 23:** the measured miss, rounded up to 0.09 dB (measured 0.013, 0.052, 0.013
      and 0.073). The joins at the burst's ends reach the low-pass's edge, where no sine's bound
      applies.
  - Measured: case 16 reads −6.188 against −6.021, and case 19 reads +2.817 against +2.984.
- **Cases 10, 11, 13 and 14, the maxima.**
  - The brute-force reference now keeps every 10 ms sub-block's M and S. Its window means come from
    running sums, so the suite takes the same time as before (about 60 s in Debug).
  - The meter's maxima, file-based and live, are held within 0.001 LU of the reference's largest.
    Measured: 0.000000.
- **Plants:**
  - true peak 0.15 dB low: 9 failures, every one in the new checks;
  - maxima updated only every 100 ms: 4;
  - max S 0.05 LU high: 2, only in the new checks.
- **Totals:** `adi_loudness_tests` 87, `adi_dsp_tests` 433. CTest 267/267 at PD_FLOATSIZE 32 and 64
  (MSVC Debug, /W4 /WX).
- **Reviewed** on four dimensions (math, test validity, portability, merge structure) with three
  skeptics per finding. All five findings were refuted as defects. Three were fair points and are
  fixed in the review commit:
  - per-case bounds in place of the worst case;
  - an honest, empirical bound for cases 20 to 23;
  - the header's 0.007 dB.

**DEV-28's devices items left:**
- the "LRA not yet stable" flag. **For win:** its definition needs a ruling. One option is fewer
  than N gated short-term values; another is less than T seconds since reset.
- a loudness case in the block benchmark: done (the entry above);
- the ffmpeg LRA oracle: done (LD6, the entry above);
- the provenance comments: done (the entry above).

## 2026-11-20 — DF-E61: a built-in's stored value that went A to B and back to A is heard over the lane

**PR requested:** head `devices/df-e57` (25dbda75), now three commits on dccb6a55: DF-E57's, and
then DF-E61's fix and review, as win asked ("with DF-E57"). It merges cleanly with
`devices/sampler-loadable`.

- **The defect** (from the outside review). `BuiltinDevice` re-sent a parameter only when its stored
  target differed from the value it had last applied. Automation moves the value in force but not
  that record. So if the stored value went A to B and back to A with no audio callback between,
  the change was never applied, and the lane's value kept playing. Ways to do that: two clicks, an
  Undo and a Redo, or a knob change followed by a state holding the old value. In the test: stored
  +3 dB, heard +6 dB.
- **The fix: each parameter counts its edits.**
  - The message side exchanges the stored value, and counts a change only when the value really
    changed.
  - The audio thread applies any parameter whose count has moved since its last scan. It reads
    the count before the value, so an edit landing in between is applied again on the next scan,
    never lost.
  - Writing the value already stored is no edit, and leaves the lane's value in force: a preset
    reload, or a drag that ends where it began.
  - A new format still restarts from the stored values.
- **Not the DEFERRED row's alternative** of comparing with the value in force. That would snap every
  automated parameter back to its stored value whenever any other parameter is edited. A plant of
  it fails the "unchanged write" check.
- **Tests** (written first; 2 failures before the fix). `adi_builtin_devices_tests` 183, on
  Utility's Gain with a lane in force. The cases:
  - A→B→A through setParam;
  - setParam then loadState;
  - two states alone;
  - an unchanged write;
  - a new sample rate.
  - Plants, each caught: every write counted as an edit; prepare not forced; compare with the
    value in force; loadState bypassing the count.
  - CTest 267/267 at PD_FLOATSIZE 32 and 64.
- **Reviewed** on four dimensions with three skeptics per finding: concurrency, semantics, tests and
  portability. The one finding was refuted as a defect; its point, that the loadState half had no
  case of its own, is fixed in the review commit.

## 2026-11-20 — EX4: the brickwall as an offline stage; non-finite input is silence; the 4x gap measured

**PR requested:** head `devices/limiter-offline` (11f539f7, two commits on 7510acd2), base `main`.
This is the devices half of EX4/LD5 that the gap audit left open. With it, the DSP half is done.
The export half is engine-win's (below).

- **`LookaheadLimiter::processOffline()`** runs a whole programme.
  - The latency is trimmed from the front and silence is pushed through the back, so the output
    has the input's length and lines up with it, sample for sample.
  - Below the ceiling the output is the input, bit for bit, in float and double. It is exactly
    `process()` over the input followed by `latencySamples()` of silence, with the first
    `latencySamples()` outputs dropped.
  - It works in place, and each programme starts from silence.
  - It allocates one block of scratch, so it is for the export worker, not the audio thread.
- **Non-finite input.** A NaN or an infinity is now read as silence, by the detector and in the
  delayed audio alike. It cannot leave as a non-finite sample or poison the gain, and the output is
  exactly that of the same programme with zeros there. Finite input is unchanged, bit for bit.
- **Tests** (`adi_dsp_tests` 433; the tests did not compile before the code existed):
  - offline length and alignment at lookahead 0, 64 and 288 in both modes, bit for bit against
    streaming, including an over on the very last sample, which is flushed rather than cut;
  - true peak in float, and at 32 kHz (6x) and 22.05 kHz (8x);
  - in place, mono, and six linked channels;
  - blocks of NaN and ±inf.
  - Plants, each caught: no trim (26 failures), no flush (26), no reset (1), NaN let through (8), NaN
    caught in detection only (8).
  - CTest 267/267 at PD_FLOATSIZE 32 and 64 (MSVC Debug, /W4 /WX).
  - **Reviewed** on five dimensions with three skeptics per finding: offline correctness, the audio
    path, test validity, portability, and the contract. Every finding was refuted as a defect. The
    five that were fair points on wording or coverage are fixed in the second commit:
    - the header's allocation claim;
    - the true-peak residue paragraph, which now says its numbers are the Annex's readings;
    - a stale comment;
    - the noise figure, now called a lower bound;
    - the reference, which now also reads 6x and 8x.
- **The independent reference, and what it found (FOR THE DIRECTOR, via win).** The audit noted
  that the ceiling tests read the output with the limiter's own interpolator. The new test reads it
  with one that shares nothing: a Kaiser-windowed sinc, ±48 samples, read at 32 points per interval,
  and checked first to within 0.01 dB on sines whose crest is known.
  - On the Annex's 4x reading, the limiter holds the ceiling: +0.0002 dB at most.
  - On the reference, the same output reads higher:
    - **+0.15 dB** on the quarter-rate sine at 45°. Its crest falls midway between the 4x
      interpolator's points (the odd eighths of each interval), which is 4x's own analytic miss,
      cos(π/16) = 0.168 dB.
    - **+1.6 dB** on +12 dB white noise, near the Nyquist frequency, where the 16 taps roll off.
    - +0.14 dB on full-scale doublets, and nothing on a 0.23 fs tone.
    - At 32 kHz (6x) and 22.05 kHz (8x) the read points are finer, so the quarter-rate figure
      shrinks to +0.06 and +0.04 dB. Noise stays at +1.6 dB, and doublets read +0.24 and +0.11 dB.
  - The reference rolls off near the Nyquist frequency too, so the noise figure is a lower bound.
  - The test bounds each case at its measured value, so the gap cannot grow unseen.
  - **The question:** should the export's brickwall detect finer than BS.1770's 4x, for example 8x,
    or 4x with points at the samples and the halves? Then an ideal reconstruction would also stay
    under the ceiling, at some CPU cost and a measurement that no longer matches the meter. Or does
    the −1 dBTP ceiling's own margin cover it, as delivery specs assume? This lane will not change
    the shared interpolator without a ruling.
- **For engine-win (the export half, all after 3 December):**
  - **The brickwall stage.** Add `Normalization.brickwall` (default off) and call
    `processOffline()` inside `finish()` on the gained double capture, before dither and quantize.
    The existing length check then holds with the brickwall on.
  - **The Peak-mode guard.** In Peak mode the brickwall never acts.
  - **The refine loop.** At most 4 trials, steps clamped to ±3 dB, and a stop when LUFS changes by
    under 0.02 LU. The encoded TP check stays the final gate.
  - **Measurement.** Measure on `dsp::LoudnessMeter`, with `finish()` replacing the 16-zero
    padding.
  - **Tests and merge order.** Write a byte-identical test of brickwall on against off for a signal
    under the ceiling, and land after or with 7a9fe563.
- **Left for this lane:** the LD5/DEV-22 link (the Limiter device exposing `setTruePeak`) comes when
  the Limiter device is built, after 3 December.

## 2026-11-20 — DEV-28: the loudness meter survives a NaN and reads the programme's last true peak

**PR requested:** head `devices/loudness-robust` (cda11168, one commit on 7510acd2), base `main`.

A gap audit of DEV-28 against main found that two things the row needs were missing. Both are
needed before the one meter replaces Main's N13, which already guards the first.

- **Non-finite input.**
  - The problem: `LoudnessMeter::run()` had no guard. One NaN poisoned the K-weighting state, so M
    and S read NaN, and I and LRA froze until `reset()`.
  - The fix: a non-finite sample is now read as silence, and a finite one is held to ±10⁶
    (+120 dBFS). The true-peak interpolator already guards its own input.
  - Test: a block of NaN measures exactly like a block of zeros, for M, S, I and LRA. A block of
    3·10³⁸ leaves every reading finite.
- **The end of a programme.**
  - The problem: the interpolator answers 8 samples late, so the last samples' overs between the
    samples were never read. Feeding zeros through `process()` to reach them lets the padding
    complete a sub-block and reach I, which the contract forbids.
  - The fix: `finish()` runs the interpolator over its tail with silence and touches no loudness
    state. It allocates nothing.
  - Test: a pair of opposite full-scale steps in the last 4 samples reads +0.1 dB or more with
    `finish()`, the same as padding gives. I, M and S are bit-identical with and without it.
- **Tests:** `adi_loudness_tests` 67, two of them failing before the fix. CTest 267/267 (MSVC
  Debug, /W4 /WX).

**What the audit leaves open.** All of it is after 3 December; nothing in DEV-28 blocks that date,
because `export.normalize` defaults to off.

- **Devices (this lane, in order):**
  - the "LRA not yet stable" flag in `Readings`, with its definition;
  - Tech 3341 cases 20 to 23 held to an independent reference. The reference is now in
    `test_dsp.cpp` (the EX4 entry above), and cases 13 and 14 can be tightened against it;
  - a loudness case in `tools/benchmark_blocks.cpp`;
  - the ffmpeg LRA oracle behind `ADI_REQUIRE_ORACLES`;
  - provenance comments: libebur128's K-weighting constants in `auto_gain.cpp`, and a statement on
    LD1.
- **Engine seat:**
  - rewire N13 (Main's meter) onto `dsp::LoudnessMeter`, with the LD7 snapshot and its validity
    flags;
  - rewire Auto Gain Stage's LUFS mode, and rule on what a selection under 400 ms reads.
- **engine-win:** measure export normalization with `dsp::LoudnessMeter`, using `finish()` instead
  of the 16-zero padding.
- **win:**
  - pin libebur128 v1.2.6 (67b33abe, test-only), with its EXTERNAL-CODE row, and turn on one CI
    leg with `ADI_REQUIRE_LOUDNESS_ORACLE=ON`;
  - strike the XC-46 loan lines in LANES.

## 2026-11-20 — DEV-09: `[; pd compatibility(` is fenced, and the object walk refuses a class named by a path

**PR requested:** head `devices/dev09-gaps` (43e372f8, one commit on 7510acd2), base `main`.

A gap audit of DEV-09 against main found two holes that this lane could close.

- **`compatibility` was not fenced.** It sets `pd_compatibilitylevel`, one process-wide integer
  that every Pd instance reads, so one patch changed how all the others behave. In the test's
  unfixed run the level was left at 47. The selector now joins the m_glob fence's refused
  selectors, by name (`pd_loader_fix.cmake`).
- **PD4's walk took a path as a class name.** Classes like `[/tmp/x]`, `[../x]`, `[C:/x]` and
  `[\\server\x]` were taken as an ordinary unknown or, worse, as a stored abstraction of that
  name.
  - They are now refused as `pd.path_named_class`, named by box.
  - A class the build makes is checked first, because `[/]` is division; the template suite
    caught that.
  - A plain subfolder name such as `[mylib/voice]` stays an ordinary name, resolved against what
    is stored.
- **Tests:** `adi_pd_engine_tests` 668 and `adi_pd_object_walk_tests` 92, written first; a plant
  gave 12 failures. CTest 267/267.

**What the audit leaves open.**

- **For win, two rulings** (also under FOR THE DIRECTOR if win wants the director's word):
  1. **The cross-instance dropout (AR8).**
     - The libpd pin holds a global write lock while a patch opens or closes
       (`z_libpd.c:141-149`, `s_inter.c:1980-2034`), so opening any Pd device silences every other
       Pd device for that long.
     - `new_anything` also takes a blocking write lock inside a tick when a patch creates an
       unknown class at run time. That breaks "the audio thread never waits".
     - The choice: record the dropout in the row, or open without holding the lock across
       `glob_evalfile`, which needs a delicate overlay.
     - Either way, this lane will write the AR8 test: A renders while B opens a 200-object patch,
       and the test reports A's skipped ticks and the length of the silence.
  2. **Runaway loops.** Does DEV-09's "a patch cannot stall ADI" cover an unbounded `[until]` on the
     audio thread? If it does, this lane proposes an overlay that caps `[until]`'s unbounded mode
     with a named refusal, or a per-tick watchdog that marks the device failed.
- **win (.github):** an ASan leg (AR18) that runs at least `adi_pd_engine_tests`. This lane's
  ASan build passes locally.
- **Devices, after 3 December:**
  - a seeded patch-text fuzz through `walkPdObjects` and `LibPdEngine::open`, and a state round
    trip (PD2);
  - wiring the walk into the user-patch loader when DEV-10 comes.
- **win (docs):**
  - DEV-09's "On main" line: AR7, AR17 and #173 are met; AR8 and AR18 are partial.
  - Close DF-E36.
  - Add DEFERRED rows for the AR8 test, the ASan leg, PD2 and the runaway-loop question.

## 2026-11-20 — [adi.transport] gives each Pd tick its exact position (the "editable Pd timing cases")

**PR requested:** head `devices/pd-exact-position` (0e8cd8a3, one commit on dccb6a55), base `main`.
It is the branch this log promised the clones seat once its rollup landed. **For the clones seat's
review:** two small hunks in its files, `adi_shaper_tilde.cpp` and `adi_shaper_transport.hpp`.

- **The problem.** From the seven fields a patch cannot tell where it is. QN 0 and QN 1 in 2/2 send
  the same seven fields, and after a 4/4 bar then a 3/4 bar a reconstruction from the meter is a
  quarter off.
- **What changed.**
  - A patch device whose patch has `[adi.transport]` now asks the graph for its exact position per
    segment (`wantsDeviceTransport`), as does a native core that keeps the graph's position. That
    second case was dead code until the review found it.
  - Before every Pd tick, inside the tick lock and without allocating, `LibPdEngine` sends that
    tick's own position: the position of its first input sample. It is reckoned from the segment's
    `quarterNotes` plus the frames since. A tick that spans calls keeps the position latched when
    it began, so a seek or stop inside a tick does not move it.
- **Two fields, not one.** The position goes out as **whole quarter notes** and the **fraction** of
  the quarter.
  - One absolute position in a 32-bit `t_float` would lose a sample within minutes, about 12
    samples an hour in at 120 BPM. Split, both parts stay exact. This is the same reasoning the
    abstraction already gives for having no absolute tick count.
  - With a position, playing and BPM are also the tick's own, so everything a patch reads about the
    tick agrees. Meter, bar, beat and ticks still describe the host block's start, and the help
    text says so.
  - Without a position, the seven fields go once per change, as before. A patch that unpacks seven
    drops the extra two, with no error.
  - `pd/adi.transport.pd` now has nine outlets. The edited ADI-Shaper takes nine atoms and rebuilds
    the position in double.
- **Tests,** `adi_pd_engine_tests` 692. Expected values come from a per-frame model built in the
  test.
  - Every tick's position, stopped and playing, in awkward segments: tick 375 is QN 1.
  - A tick spanning a cut in its host block, a seek and a stop inside ticks (tick 15 = 3.04 before
    the seek; tick 26 = 10.0276667 before the stop), and engine tiling.
  - What a patch receives, read from the proof patch:
    - in 2/2, QN 0 and QN 1 send the same seven fields but different positions;
    - in bar 2 after 4/4 then 3/4, the position is 4 where the seven fields give 3;
    - QN 7200.37 arrives exact;
    - playing and BPM are the tick's own.
  - A seven-field patch keeps working, and both the patch and the native core opt in.
  - Plants: no latch (5 failures), segment-start position (9), once per change (11), one absolute
    float (1), the block's playing and BPM (1), no native opt-in (1).
  - CTest 267/267 at PD_FLOATSIZE 32 and at 64 (MSVC Debug, /W4 /WX).
- **Reviewed first.** Five reviewers covered timing, delivery, graph integration, portability and
  tests, with three skeptics per finding. The confirmed findings, all fixed here, were:
  - the 32-bit precision loss;
  - playing and BPM taken from the block while the position came from the tick;
  - the dead native branch;
  - two meter tests that compared constants and could not fail.
- **What it does not do.**
  - It reaches the app only once DEV-10's user-patch loader exists; production built-ins run
    natively.
  - Upstream latency comes from the graph's own arrival term, which is not re-tested here through a
    full Graph.
  - The ADR-0188 d3 field list says "exactly" seven, so **win** owns the amendment that records the
    two added fields.

## 2026-11-20 — DEV-21: the Sampler is a loadable instrument with 32 sample slots (win's option 1)

**PR requested:** head `devices/sampler-loadable` (205c6bf9, one commit), stacked on
`devices/hybrid-reverb-loadable` (d72b7cdd; base that branch until it lands, then `main`), so the
catalogue's count lines move in one line. It is on win's loans of `builtin_catalog.cpp` and
`builtin_host.cpp`. **Beyond those loans, for the engine seat's review:** a small change in
`src/adi/engine/snapshot.cpp`, described below.

- **The catalogue.**
  - `org.adidaw.builtin.sampler` is an instrument with notes in and no audio in: 21 entries, all 21
    available.
  - Its declarations are a C++ constant (`adi/sampler_declarations.hpp`), not a canvas: 24
    `[adi.param]` and 32 `[adi.sample]` records that `parsePdDeclarations` reads.
    `builtinDeclarations(uid)` gives them; `builtinCanvas` stays empty for it.
  - The host chooses its native by uid. No Pd path can become the Sampler; a test checks that
    `makeNativePdDevice` refuses its text.
- **Slot mode in the core** (`Sampler(slotBacked)`).
  - 32 fixed layers. Layer i plays slot i+1's prepared sample, borrowed for each call, as OneShot
    borrows slot 1.
  - A filled slot is a zone over every key and velocity, so a dropped sample plays at once. Two
    filled slots layer, as in Live.
  - A slot's new generation resets that slot's voices only, so filling one slot never cuts
    another.
  - Zone shapes are a separate table: `publishSlotZones`, the whole table at once, applied on the
    audio thread. It is ready for the engine's zone op, which is not in this PR.
  - Parameters go by index (`OneShot::setParamAt`, `setRealAt`), with no string lookup per layer
    on the audio thread. The original bank mode shares the same per-layer code (`playLayer`).
- **The sample bridge.**
  - A Sampler slot is a OneShot sample (source kept, restored at any rate).
  - `inspectBuiltinSample` takes slots 1..32 for the Sampler and slot 1 for the others.
  - **One fix found on the way:** capture accepted a stereo source of up to 8M frames, whose state
    restore then refused (its float backup is capped at 8M values, under the host's 40 MiB). It
    now refuses at capture: `builtinSampleStateFits`, 4M stereo frames, 8M mono. This applies to
    OneShot and Hybrid Reverb too.
- **The snapshot (the engine's file).**
  - It accepted only the role `sample:1` and named it "Impulse". Sampler slots 2..32 would have
    reached the window as problem nodes with slot 0.
  - It now reads `sample:N`, accepts N when the device declares it, names the node as declared, and
    orders by the slot's number. Color Cab and Hybrid Reverb stay "Impulse", OneShot becomes
    "Sample" (it was wrongly "Impulse"), and Sampler slots read "Sample 5".
  - A build that cannot read the declarations keeps slot 1. About 30 lines; the engine seat may
    prefer to take it over.
- **Tests.** Each was shown to bite with a plant.
  - Core, `adi_sampler_tests` 119:
    - silence with every slot empty, and no allocation;
    - keys 0, 60 and 127 on one filled slot;
    - two slots layering;
    - filling another slot without cutting a sounding one;
    - emptying one slot;
    - zone ranges, and a refused table changing nothing;
    - the declaration text agreeing with the core, parameter by parameter.
  - Through the product, `adi_builtin_devices_tests` 226:
    - insert on a MIDI track, the sample dropped into slot 5: peak **0.1256**, the 0.5 sine at the
      default −12 dB, sounding at once and again after reopening;
    - slots 2 and 10 layer: **0.2511**;
    - Volume at 0 dB through an ordinary parameter op: **0.4999**;
    - slots 1..32 accepted, 0 and 33 refused;
    - the snapshot naming slots in order;
    - the event capacity.
  - Plants: every layer reset whenever a slot changes (3 failures); only slot 1 read (5); only slot
    1 accepted (9); main's snapshot (3).
  - CTest 267/267 (MSVC Debug, /W4 /WX).
- **Reviewed before the request.** Five reviewers covered real-time safety, correctness,
  integration, portability and tests, and three skeptics checked each finding. Four findings
  survived, and all are fixed here:
  - a block of more than 257 MIDI events could overflow a layer's event list, silently dropping a
    note-off and leaving a queued note gain out of step. The list now holds OneShotPdMidi's 512
    plus the parameters; a gain is queued only for a note-on that fits; a dropped note-off stops
    the layer's voices;
  - the snapshot's `sample:1`;
  - a refused zone table's test that could not see a published one.
- **For later, not this PR:**
  - the engine's zone op (`publishSlotZones` is ready for it);
  - a drop target for the Sampler in the window (OneShot has none either);
  - Live's Sampler voice count is global, ADI's per layer; that is a question for the director's
    sitting, with no new reading.

## 2026-11-20 — DF-E57: a built-in's partial state resets what it omits to the default

**PR requested:** head `devices/df-e57` (d8be42b5, one commit on dccb6a55), base `main`, on win's
loan of `builtin_host.cpp`.
- **The change.** Every built-in now reopens a parameter its state does not set at its default,
  not at the value before. That covers a newer version's added parameters and a partial preset.
  It was ADI-Shaper's `resetMissingParameters`, made general.
  - "Not set" means absent from the state's positions, real values and units alike. DF-E37 (c)
    saves both a position and a real value, and a real value alone restores the parameter.
  - ADI-Shaper keeps its flag for its other half, holding its audio while a whole state applies.
    The other built-ins do not take that hold: on every state load it would pass one block of dry
    audio. They apply within one block, as before.
- **Test, written first.** Utility with two parameters moved, and a state that omits one of them:
  the omitted one returns to its default and the other keeps its value. It failed before the fix.
  `adi_builtin_devices_tests` 168; CTest 267/267 (MSVC Debug, /W4 /WX).
- **The first version of the test was wrong, and is noted here.** It removed the parameter from
  the positions only. Its real value then restored it, so the test failed even with the fix.

## 2026-11-19 — DEV-22: Hybrid Reverb is loadable (its native path and its impulse slot)

**PR requested:** head `devices/hybrid-reverb-loadable` (24f0d83b), stacked on
`devices/oneshot-instrument` (base that branch until it lands, then `main`). The work is on win's
loan of `builtin_catalog.cpp`. DEV-22's row names it: Hybrid Reverb's native factory. It had a
canvas and a core, but its catalogue entry was unavailable.
- **The native path.** `HybridReverbStereo` reads the impulse response from slot 1 per segment, as
  the Pd external does. The core takes it only when it was prepared at the core's rate, and the
  latency is the 256-sample partition. The canvas is embedded and the entry is available. Native
  and Pd renders of the canvas match exactly; it is a new parity fixture.
- **The impulse slot.** An IR is kept as its source (the first two channels, at its own rate) and
  restored at the device's rate. Each channel is resampled and scaled by the inverse of the rate
  ratio, because a resampler keeps a signal's amplitude, which would scale an IR's DC gain by the
  ratio.
  - **One cost, stated:** an IR whose sound starts on its very first sample loses the half of its
    resampled pulse that falls before time zero. That measured 0.118 for 0.125 at 44.1 to 48 kHz,
    about 0.5 dB of that first click. Keeping it would delay every IR, so it is not kept. An IR at
    the session's rate is not resampled.
  - **For win, a question for after 3 December:** Live's own behaviour here, and Hybrid Reverb's
    latency (ADI reports the 256-sample partition; the research's table does not list Hybrid
    Reverb). Both are added to reading 15.
- **Tests, written first.** The device was unavailable before, so they could not pass.
  - In a Session at 48 kHz, through the product's calls (the insert, the IR assigned to its slot as
    one undo step, the Convolution route fully wet): an IR at 48 kHz gives **0.125000** exactly,
    and one at 44.1 kHz **0.124961**. Plant (no gain scaling): 0.136, 1 failure.
  - `adi_builtin_devices_tests` 125, `adi_pd_native_devices_tests` 274; CTest 245/245 (MSVC Debug,
    /W4 /WX).
  - The test that used Hybrid Reverb as its unavailable example no longer can: with Pd every
    catalogue entry is available, and the PD-disabled build refuses them all.
- **Count lines, updated:** this branch makes it 19 entries, all 19 available. With
  `clones/shaper-rollup` it becomes **20 entries, all 20 available** for whichever lands second.

## 2026-11-19 — for the clones seat: one two-head delay reader, agreed (corrects this lane's earlier answer)

**Correction.** The earlier entry's "one pitch kernel: a reason, not a branch" described **main's**
Shifter, a phase vocoder. On this lane's own `devices/shifter-latency` (#660, 7c5d8158), Shifter's
Pitch mode is two delay heads half a cycle apart with sin² crossfades, resting on one tap at half
the Window at unison. That is the shape of `AdiShaperPitch`'s reader, as the clones log says. That
answer is withdrawn.

**Agreed:** one two-head delay reader, shared. The clones seat reports that its extraction
reproduces both #660's Shifter and the cyclic Pitch bit for bit, over 3,127,711 comparisons across
three rates, impulses, independent stereo, kick and bass, edits and irregular blocks.
- **Owner and include:** the devices lane, in `src/adi/dsp/`. The public header is
  `adi/dsp/two_head_reader.hpp`, a name only, numbering nothing. The common part is the history,
  the two head phases and the read at an age. Each caller keeps its own interpolation (Shifter
  linear, the cyclic Pitch its windowed sinc), weighting law, unison rest and latency policy.
- **Order:**
  1. the extraction lands as its own small branch from main, DSP and its cross-tests only (the
     clones seat writes it, this lane reviews it);
  2. Shifter takes it after #660 lands, in a devices-lane commit that keeps every Shifter test
     bit-exact;
  3. ADI-Shaper takes it after its rollup lands.
  Neither device changes sound.
- **The rest of that clones entry:**
  - the real-valued state, the count reconciliation and `OneShotNative`'s two-argument `process`
    are answered in this log (the rollup review entry);
  - the unchanged-parameter scan is agreed for all built-ins;
  - DF-E57 follows the rollup, as proposed in the DF-E50 entry, and the clones seat's registry test
    can come with it.

## 2026-11-19 — for win: the audio-effects queue checked against Live 12.4's own list, three gaps

Win's late entry asks, when this lane reaches item 5, for a check of the whole queue against Live
12.4's audio effects. This compares Live 12.4's browser list of audio effects with what ADI has
built and the queue: the 5 November prompt's order, LANES §2 item 5 and ADR-0249.

- **Built (16 of Live's 48):** Auto Filter, Chorus-Ensemble, Delay, Echo, EQ Eight, Hybrid Reverb, Multiband
  Dynamics, Overdrive, Phaser-Flanger, Redux, Resonators, Reverb, Saturator, Shifter, Utility,
  Vocoder. ADI's own Chord Comb and Color Cab are not Live's.
- **Queued (24):** Compressor (its core is on `devices/compressor-core`), Glue Compressor, Gate, Limiter, Beat Repeat, Drum Buss, Dynamic Tube,
  Channel EQ, EQ Three, Erosion, Filter Delay, Grain Delay, Looper, Pedal, Roar, Amp, Cabinet, Vinyl
  Distortion, Corpus, Spectral Resonator, Spectral Time, Align Delay, Auto Pan, Tuner.
- **Elsewhere by design:**
  - Audio Effect Rack is DEV-01 and DEV-03, the engine's.
  - Envelope Follower, LFO and Shaper are DEV-07's modulators, with this lane's kernels (ADR-0249
    d8).
  - Max Audio Effect is the Max path, rejected (ADR-0203 d8).
- **Gaps, for win to place:**
  1. **Auto Shift**, Live 12's pitch-correction effect. It is in no queue. Recommendation: after
     Tuner, since both track pitch and can share one pitch detector, as the clones seat and this
     lane share one stereo history.
  2. **External Audio Effect**, a hardware insert (send to an output, return from an input, with
     its delay compensated). Recommendation: the engine's, with MX-24's routing and DEV-05's
     compensation, since it is routing and has no DSP of its own. This lane would give only its
     canvas and panel contract.
  3. **Spectrum**, Live's analyser device. Recommendation: the Mix View seat's shared analyser
     (`dsp/spectrum.hpp`, ADR-0183 d7) as a device on a track, with this lane's canvas. Or none, if
     the Mix View's analyser is held to cover it.
- **A name:** Live 12.4 calls Auto Pan "Auto Pan-Tremolo". The queue's "Auto Pan" means that device;
  its uid and browser name should say Auto Pan-Tremolo.

## 2026-11-19 — DF-E46: OneShot's Slice mode; a key with no slice steals nothing, slices stop at End

**PR requested:** head `devices/oneshot-slices` (6625eb37), stacked on `devices/oneshot-instrument`
(base that branch until it lands, then `main`). The row asks for it right after OneShot lands as an
instrument. It is ready now, so it can follow at once.
- **(a)** A note on a key with no slice used to choose and clear a voice before it checked the key.
  The note's region is now decided first (`OneShot::slice`), and a key with no slice returns before
  any voice is touched. It still takes the per-note gain the Sampler queued for it, so the next note
  keeps its own.
- **(b)** Every Slice By mode now refuses a slice that begins at or past End, and cuts one that runs
  past it unless Slice Playback is Thru. Region and Beat already did this; Transient and Manual now
  do too.
- **Tests, written first (5 failures before):**
  - Mono and Poly with a key that has no slice;
  - a Manual slice cut at End, one beginning past End refused, and a Transient one past End
    refused;
  - a refused key's queued gain.
  - `adi_oneshot_tests` 359; CTest 245/245 (MSVC Debug, /W4 /WX).

## 2026-11-19 — DF-E50: automation on a built-in device is heard, at its frame

**PR requested:** head `devices/df-e50` (854050e3, two commits on 8416ed0f), base `main`, the engine
seat reviewing (the row asks for it). It is the 3 December session's "one automated parameter"
whenever that parameter belongs to a built-in. The work is on win's loan of `builtin_host.cpp`
(ADR-0248 d3).
- **The defect, reproduced as the probe found it.** Utility has a stored +6 dB and a lane at 0 dB,
  and it played 0.4988 (+6 dB), live and in an offline export. `BuiltinDevice::process` applied
  only `setParam`'s targets; the MIDI router skipped the lane's `ParamValue`.
- **The fix.** Each `ParamValue` or `ParamMod` addressed to a declared parameter (its id is the
  lane's `param_ref`, parsed) is applied at its frame. The native process is split there, and the
  block's routed notes reach the core before the piece they fall in. `ParamValue` sets the value in
  force; `ParamMod` sets an offset on it, through the declaration's curve. The stored value is
  untouched, and a knob change is heard again until the lane's next event. A block that no lane
  moves runs exactly as before, with no allocation on the audio thread.
- **One choice for the reviewer.** The row says "ParamMod as an offset for the block". Here the
  offset holds until the next `ParamMod`, as CLAP holds a modulation and as `clap_host` passes it
  on. A modulator that sends only on change then keeps working. If the engine's modulators will
  send every block, the two are the same.
- **Tests, written first (5 failures before):**
  - events on Utility directly: a `ParamValue` at frame 32 leaves frame 31 untouched and starts
    the ramp at 32; an unknown id does nothing; `ParamMod` on and off; the knob after automation;
    no allocation;
  - through the product: a lane on Gain at 0 dB over a stored +6 dB plays **0.250000** in a live
    Session and in a render;
  - plant (applied at the segment's start): 1 failure.
  - `adi_builtin_devices_tests` 119; CTest 246/246 (MSVC Debug, /W4 /WX). Merged with
    `oneshot-instrument` and `builtin-real-values`, it passes 130 and 16 checks.
- **Merges.** It merges cleanly with this lane's branches; the test calls sit where
  `oneshot-instrument` does not touch them. With `clones/shaper-rollup` the conflict in `process`
  is two lines: keep the rollup's `!info_.usesTransportPosition` on the tempo line, and keep the
  `MpeOutList` declared outside the `if(info_.midi)`.
- **DF-E57** (a partial state leaves stale values) is the rollup's `resetMissingParameters`, made
  for every built-in instead of ADI-Shaper alone. This lane proposes to do it in one place once the
  rollup lands: drop the flag, and reset the omitted parameters to their defaults for all. Until
  then no state on main omits a parameter, as the row says.

## 2026-11-19 — main's PD_FLOATSIZE=64 leg is red: two fixes, OneShot's outputs and parameters rounded through float

**For win: main's `Pd tier at PD_FLOATSIZE=64` job has failed since `bedee5ab`.** The job's comment
puts the fix at the head of the queue. Both causes are this lane's devices, and both fixes are
ready.

- **PR requested: `devices/oneshot-double`** (552851b2, one commit on 88e2d94e), base `main`. The
  cause: `adi.oneshot~` was the one external that handed Pd's output vectors to its core as
  `float*`. At 64 those vectors are double, so the patch played garbage (`adi_oneshot_pd_tests`, 6
  of 38 failing on main, as in CI). It now takes `FloatSignals`' float views of its two outputs, as
  every other external does; at 32 they are the vectors themselves. Result: 38/38 at 64, and CTest
  246/246 at 64 and at 32 (MSVC Debug, /W4 /WX).
- **PR requested: `devices/pd-param-float`** (ac0b7b1b, one commit on 88e2d94e), base `main`.
  - The cause: at 64, fourteen externals passed a parameter to their core as the double
    `atom_getfloat` returns, where the native path rounds it through float as Pd at 32 does. A
    default that float holds exactly hides this. 0.9 Hz, 0.7 or 0.2977 does not, and the two paths
    then drift apart. That is why `chorus-ensemble-defaults` and `reverb-defaults` fail native
    parity at 64, and why AutoFilter and EQ Eight differed by 2^-25 on main.
  - The fix: every external rounds the value through float.
  - The test is first: the 64-bit parity check, which allowed one float ulp, is now exact as at 32.
    On main it fails for AutoFilter and EQ Eight; with the fix all 17 canvases match bit for bit at
    64. CTest 246/246 at 32. At 64 it is 245/246, the one left being OneShot's, fixed by the branch above. With both fixes and the two defaults branches merged on main, native parity, OneShot, Reverb and Chorus-Ensemble all pass at 64.
- **Order:** both go ahead of this lane's other branches, `pd-param-float` before
  `chorus-ensemble-defaults` and `reverb-defaults`. The combined tree of the 18 branches (entry
  below) passes 253/253 at 32. At 64 it fails exactly these two suites, both fixed here.
- **All of it together (latest):** every pending branch of this lane, 22 heads including DF-E50 and DF-E46, merged onto 8416ed0f (16b92842, a local trial, not pushed). It builds with no warning (MSVC Debug, /W4 /WX), and CTest passes 253/253 at PD_FLOATSIZE=32 and 253/253 at 64.
- **Under ASan:** the same 18 branches plus these two fixes, merged onto 88e2d94e (3a1c822c, a local trial) and built with MSVC's `/fsanitize=address`. 252 of 253 suites pass with no memory error. The one failure is `adi_changeset_tests`'s 60-second wall-clock bound, which ASan's slowdown exceeds (151 s), as on main alone.

## 2026-11-19 — ADR-0249 read: the new effects come after 3 December; Beat Repeat's questions are on the list

ADR-0249 (win's entry of tonight) answers the question to win below. Compressor, Glue Compressor,
Gate and Limiter come after 3 December, then Live's Beat Repeat, then the LFO and Shaper modulator
kernels (DEV-07's device side), then the rest of DEV-22. **Struck:** the question "new effects
before 3 December?". `devices/compressor-core` waits for that turn and for reading 17.
- **Beat Repeat's questions** are reading 18 under FOR THE DIRECTOR, as d7 asks: the capture, a Grid
  change while repeating, the fractional phase of a slice, the joins, a locate, and the longest
  history at 20 BPM. The device is "built, not verified" until a Live 12.4 render agrees.
- **The shared stereo-history kernel** with ADI-Shaper's Time stage: this lane's answer stands (the
  clones entry below). One ring with an integer-age read, separate storage per instance, decided
  when Beat Repeat starts.
- **Item 5's check against the manual's audio effects chapter** (win's late entry) is done when this
  lane reaches item 5, as asked: Live's Limiter, Auto Pan and Tuner are noted as queued.

## 2026-11-19 — for win: this lane's 18 open branches merge onto main together, in this order

A trial merge of every branch this lane has asked to land, onto `bedee5ab`, in the order below.
Three conflicts this lane had made itself are gone:
- **`devices/df-e20` and `devices/true-peak`** both corrected `spectrum.hpp`'s 0 dBFS paragraph.
  `df-e20` (fc1eed41) now carries `true-peak`'s exact text, so they merge in either order.
- **`devices/df-e36` and `devices/pd-object-walk`** both rewrote `pd_declarations.cpp`'s `scan()`.
  `pd-object-walk` (a40613a1) now stands on `df-e36`: a merge commit keeps `pdRecords()` and reads
  `#X declare` as a box. `amxd-translator` (0d1f77e5) and `pd-templates` (39d3e9c3) carry the same
  merge. Each was rebuilt, and CTest passes (247 and 248).
- **`devices/builtin-real-values` and `devices/pd-object-walk`** both added a CMake test block in one
  place. `builtin-real-values` (c3f9636e) registers its tests beside `adi_builtin_devices_tests`
  now (CTest 246/246).

**Order.** The fixes come first:
1. `df-e36`
2. `df-e20`
3. #659 `saturator-latency`
4. #660 `shifter-latency`
5. #656 `oneshot-live-params`, then `oneshot-instrument`
6. `builtin-real-values`

Then the rest:
7. `true-peak`, then `limiter-true-peak`
8. `loudness-oracle`
9. `pd-object-walk`, then `pd-templates` and `amxd-translator` (DEV-16 holds at step 2: take
   fe9fa8af, or the head with steps 3 and 4)
10. the defaults: `auto-filter`, `chorus-ensemble`, `delay`, `phaser-flanger`, `reverb-equal-power`
    then `reverb-defaults`, and `eq-eight`
11. `compressor-core`, when you choose (the question below)

Any order within a group merges cleanly. Later, after 3 December (DF-E40): `pd-block-quantum` (b8ff1a6e) and `pd-midi-out` (stacked on it) now stand on `df-e36` as well, each with a merge commit that keeps both sides' new `PdPatchEngine` virtuals (CTest 246/246 each). The trial's combined tree: all 18 branches merged onto bedee5ab (c7627437, a local trial, not pushed). It builds clean (MSVC Debug, /W4 /WX), and CTest passes 253/253.

**`clones/shaper-rollup`** still conflicts with this set, as expected, in `builtin_catalog.*`,
`native_devices.cpp` and `builtin_host.cpp`. The resolution is in the review entry below: 20
entries and 19 available, both branches' `BuiltinInfo` fields, and `OneShotNative::process` in the
two-argument form. If the rollup lands after `oneshot-instrument`, this lane can push the
resolution as a merge commit on a branch of its own for the clones seat to take; say which you
prefer.

## 2026-11-19 — for the clones seat and win: `clones/shaper-rollup`'s hunks in shared files, read

The devices lane agrees ADI-Shaper's hunks in shared files through the logs. This review read
e87f45dd's changes outside `adi_shaper*` against main and against this lane's open branches. Four
are agreed. One needs a one-line change from whichever branch lands second.

- **Needs a change at merge.** `NativePdDevice::process` now takes `(const engine::NodeIo&, const
  PdTransport* fallback = nullptr)`. `OneShotNative` on `devices/oneshot-instrument` overrides the
  one-argument form, so the second of the two to land stops compiling (`override` that overrides
  nothing). Whichever lands second gives `OneShotNative::process` the two-argument form; OneShot
  ignores the fallback. The devices lane does it if the rollup lands first.
- **Agreed: `builtin_host.cpp`.** This is win's file, lent to this lane, and three branches touch
  it: the rollup, `devices/builtin-real-values` (DF-E37 c: saving and reading real values) and
  `devices/oneshot-instrument` (an instrument consumes notes). The clashes are textual only:
  - `resetMissingParameters` tests `saved.contains(id)`, and the real-values branch keeps the same
    keys.
  - The revision counter (publish the target, then advance; a scan is skipped when nothing
    changed) is sound, and a change during a scan is applied one block later at most. It applies
    to **every** built-in, not only ADI-Shaper, so the clones log's "shaper-only" should say so.
    This lane agrees to it for all.
- **Agreed: `native_devices.*`.** `PdTransport` moves beside `NativePdDevice`, and the capability is
  resolved once at construction, off the audio thread. `syncShaper` reconstructs the position from
  the seven fields only as a fallback. Its 2/2 and meter-change limits are the "Editable Pd timing
  cases" answered in this log (`devices/pd-exact-position`).
- **Agreed: `pd_engine.*`.** A native device now takes the tempo only when it does not use the
  graph's position. `Transport` becomes `PdTransport`. There is no overlap with DF-E36's hunks
  (`devices/df-e36`: refusal queue, `stillOpen`, `pumpConsole`) beyond textual merges.
- **Agreed: `pd_native.cmake`.** Canvases over 16,000 bytes are embedded as a counted `char` array,
  with no terminator and `sizeof` exact, which is MSVC's literal limit handled. Smaller ones keep
  the raw string and its delimiter check.
- **Count lines:** as agreed above, 20 entries and 19 available for whichever lands second.

## 2026-11-19 — DEV-22: Live's Compressor, the core only

**PR requested:** head `devices/compressor-core` (431bc637, two commits on bedee5ab; the second keeps the gcc and clang legs' -Wconversion quiet), base `main`.
This is the default named in the question to win below. The queue's first new effect is built as
a DSP core with its tests, so the device is ready when reading 17 arrives. It claims no parity and
is in no catalogue; win may hold it until then. Files: `dsp/live_compressor.*`,
`tests/test_live_compressor.cpp`, `cmake/devices/live_compressor.cmake`.
- **The core.** Feed-forward in the log domain, stereo-linked on the louder channel.
  - Detection: Peak, RMS, or Expand (downward, below the threshold).
  - A quadratic soft knee.
  - Log ballistics on the reduction in dB, or Lin ballistics on the level.
  - Lookahead of 0, 1 or 10 ms delays the audio and the dry path, never the detector: 480 samples
    at 48 kHz for 10 ms, the delay Live 12.4 reports.
  - Makeup, Output and Dry/Wet.
  - No allocation, and non-finite input is read as silence.
- **Provisional until reading 17:** the defaults, Makeup's law (the curve's reduction at 0 dBFS, in
  full), the RMS window (10 ms) and the Dry/Wet law (linear). Auto Release and the side-chain are
  not in this step.
- **Tests.** Each is checked against a number derived apart from the code:
  - the static curve: 4:1, an infinite ratio, a 12 dB knee at and inside its edges, Expand 1:4 and
    its knee;
  - one time constant of Attack and of Release;
  - 441, 480 and 960 samples of lookahead, an impulse at exactly 480, and a step that leaves the
    delay already reduced;
  - Makeup, Output, Dry/Wet 0 bit-exact, linked channels, a NaN block, a sine's RMS 3 dB under
    its peak, and no allocation.
- **Plants:** Expand's knee sign gives 2 failures (the first version of the test missed it; a check
  inside the knee was added); the lookahead one sample early gives 3; Attack and Release swapped
  give 3. `adi_live_compressor_tests` 35; CTest 247/247 (MSVC Debug, /W4 /WX).

## 2026-11-19 — for win: DEV-22's built devices are at Live's defaults where Live states them; new effects before 3 December?

The lane's ADR-0248 d8 order is done as far as it can go today. (1) DF-E37 and (3) DF-E36 are in
PRs, with AR18 under ASan. (2) OneShot is loadable; the Sampler waits on your word on its sample
path (entry further down: option 1 recommended). DF-E20 (d) and (e) are in a PR. DF-E39 waits for
DEV-17 and DEV-18. (4) DEV-22: every built device whose default Live ships now takes it (Reverb's
and EQ Eight's today). The rest of DEV-22's built devices wait on readings 6 to 10, 15 and 16.

**Question.** DEV-22's row next asks for the queued effects in the manual's order, Compressor first.
None of them is in the session of ADR-0248 d2. The Mix View seat owns the mixer's compressor
functions (DF-M28), so a Live Compressor device would sit beside its Clean compressor. Neither a
default preset nor a render exists for Live's Compressor yet (reading 17).

**Recommendation:** hold new effects until after 3 December, or until reading 17 is in. Meanwhile:
- the Sampler on option 1, the moment you agree it (it is in the session: a keyboard into an
  instrument);
- `devices/pd-exact-position` once `clones/shaper-rollup` lands;
- review hunks for the clones seat (FloatSignals, the count lines).

**What it blocks:** only the order of this lane's next new device. Without your word, this seat
builds Compressor's DSP core and tests (no catalogue entry, no claim of parity), so the device is
ready when reading 17 arrives.

## 2026-11-19 — DEV-22: EQ Eight opens at Live 12.4's default curve

**PR requested:** head `devices/eq-eight-defaults` (0009be42, one commit on bedee5ab), base `main`.
Live 12.4's EQ Eight browser default comes from its default preset in the Core Library, read in
the director's research. It differs from ADI's in 24 stored values; the type numbers follow
Live's menu order, which ADI's menu already has.
- **Curve A.** Band 1 is a low shelf at 30 Hz, bands 2 and 3 bells at 200 Hz and 1 kHz, band 4 a
  high shelf at 5 kHz, all on. Bands 5 to 8 are off, at 100 Hz, 10 kHz and 5 kHz, then a 12 dB high
  cut at 18 kHz.
- **Curve B (L/R and M/S).** Bands 3 to 6 are on, as bells at 100, 500, 2,000 and 10,000 Hz. Bands 1,
  2, 7 and 8 are off: a 12 dB low cut at 40 Hz, a low shelf at 200 Hz, a high shelf at 5 kHz and a
  12 dB high cut at 18 kHz.
- **Already matching:** every gain 0 dB and every Q 0.7071, so the default response stays
  bit-exact unity (tested). Also Stereo, Edit A, Adaptive Q on, Scale 100 %, Output 0 dB and HQ off.
- **Saved projects:** a project saved earlier keeps its own values (ADR-0188 d3). Only a fresh EQ
  Eight opens differently.

The native core and the canvas's `[adi.param]` defaults take the same table; `EqEight::value(n)`
lets a test read a default. The test is first: native and canvas defaults against Live's table,
24 wrong before in each. The Pd suite's peak measurement now sets band 1's type itself.
`adi_eq_eight_tests` 45, `adi_eq_eight_pd_tests` 100; CTest 246/246 (MSVC Debug, /W4 /WX).

Live's Core Library has default presets for ten audio effects. ADI has now taken Auto Filter's,
Chorus-Ensemble's, Delay's, EQ Eight's, Phaser-Flanger's and Reverb's (as far as their controls
allow), Saturator's (on #659; Width, Curve and Period wait on reading 7), and Hybrid Reverb's Dry/Wet. Limiter and Roar are not ADI built-ins. The
devices with no default preset (Compressor, Echo, Redux, Resonators, Multiband Dynamics, Vocoder,
and the queue's new effects) take their defaults from reading 6 and its like.

## 2026-11-19 — DEV-22: Reverb's other defaults, where the control means what Live's does

**PR requested:** head `devices/reverb-defaults` (ee00241c, one commit stacked on `devices/reverb-equal-power`,
69c75a50; review that first), base `main`. The rest of Live 12.4's Reverb browser default, read
from the device's own default state in the director's research. A value is taken only where ADI's
control has Live's unit and range:
- Lo Cut on, Spin on at 0.30 Hz, the high shelf on at 4.5 kHz and 0.70, the low shelf's (off)
  values 90 Hz and 0.75, Chorus on at 0.02 and 0.02 Hz, Predelay 2.5 ms, Decay 1.2 s, Stereo 100.
- Unchanged and confirmed: Hi Cut off, the low shelf off, Size 100, Freeze off, Flat and Cut on,
  Reflect and Diffuse at 0 dB, Dry/Wet 55 %.
- Left at ADI's values: Spin's amount (Live's 17.5 does not fit ADI's 0–5 ms), the input band's two
  frequencies (Live stores a centre of 830 Hz and a width of 5.85), Diffusion and Scale (Live's
  all-pass gain 0.6 and size 0.4), Density (Live's index 1 of a chooser whose entries are not in
  the state). Hybrid Reverb's Routing, Predelay, Decay and Size are in the same position. All of
  them are reading 15 under FOR THE DIRECTOR.

The test is first: `test_reverb.cpp` checks each default against Live's stored value (14 failures before
the fix). The canvas's `[adi.param]` defaults match the native ones. The mechanism tests (Predelay,
Freeze and Cut, Flat) start from the plain tank, with Lo Cut, Spin, Chorus and the high shelf off,
as they did before. The Pd suite's far-bin floor (1e-7) turns Spin and Chorus off: their modulated
delays interpolate above that floor, which is expected, not a fault. `adi_reverb_tests` 51,
`adi_reverb_pd_tests` 43; CTest 245/245 (MSVC Debug, /W4 /WX; the branch's base predates one suite).

## 2026-11-19 — DF-E20 (d) and (e); the clones seat's three items and the count lines

**PR requested:** head `devices/df-e20` (720c4c33, one commit on 12aab48a), base `main`. Two parts of
DF-E20 are this lane's (ADR-0248 d8).

- **(d), the two header comments.** `dsp/spectrum.hpp` now says a full-scale sine reads 0.00 when
  it is *centred on a bin*. Between bins the nearest bin reads lower by the window's scalloping
  loss, the most half a bin off: −1.42 dB for Hann, −3.92 dB with no window. A new test
  (`adi_spectrum_tests`, 107 checks) asserts both at N=1024. It is a comment, so there is no failing
  test first. The new test's figures are what the header now says: Hann −1.42 within 0.01 dB; no
  window −3.909 and −3.936 on the two sides, within 0.02 dB, because the image at −f leaks into
  each side. The other half, `dsp/true_peak.hpp`'s "always at least 176.4 kHz" (it was 128 kHz at
  32 kHz), is corrected on `devices/true-peak` (PR requested earlier), whose factor rule makes the
  comment true: 6x at 32 kHz, 8x at 22.05 kHz. To keep the two from conflicting, it is not
  repeated here.
- **(e), confirmed as built, by design (ADR-0011, ADR-0095).** A patch *device* whose patch failed to
  open is a wire. It passes both channels through sample for sample, at once, in awkward block
  sizes, before and after a prepare at 48 and 44.1 kHz. It reports 0 latency, which is the truth: a
  wire delays nothing, and a nonzero figure would shift every other track against a delay that is
  not there. `openError()` says why it failed. The engine-level test existed; the new
  `testAFailedPatchDeviceIsAWireWithNoDelay` holds the device level (`adi_pd_engine_tests` 641
  checks on main's base). Whether the Devices panel shows `openError()` is the window lane's (no
  change here).
- **Numbers.** CTest 246/246 (MSVC Debug, /W4 /WX).

**AR18 and the whole suite under ASan.** Beyond the Pd engine: every one of the 246 CTest suites was built and run under MSVC ASan the same way. 245 pass. No suite reported a memory error (ASan aborts the suite on the first). The one failure is `adi_changeset_tests`'s the

**The clones seat (ADR-0248 d8).**
- **Count lines.** `main` has 18 entries, 17 available. `clones/shaper-rollup` (ADI-Shaper) and
  `devices/oneshot-instrument` (OneShot as an instrument) each make it 19 and 18. Whichever lands
  second makes it **20 and 19**: `builtinCatalog().size()==20` and `available==19` in
  `test_builtin_devices.cpp`. That branch keeps both branches' `BuiltinInfo` fields after `midi`:
  `instrument` from devices; `usesTransportPosition` and `resetMissingParameters` from clones. The
  entries and `builtinCanvas`'s array stay in the same order, with ADI-Shaper's `adiShaper` and
  OneShot's `oneShot` each at its own entry's index. The entries keep positional initialisers up to
  `midi`, so neither branch's new fields move an existing entry.
- **(Withdrawn: see the entry "one two-head delay reader, agreed".) One pitch kernel: a reason, not a branch.** Shifter's Pitch mode is a phase vocoder: PFFFT,
  windows of 256 to 8,192 points, hop a quarter, matched to Live's Shifter. `AdiShaperPitch` is a
  two-tap cyclic grain delay with a 16-tap windowed-sinc read, matched sample for sample to its
  reference. They share no stage. One kernel would break one of the two matches, so each keeps its
  own. If ADI later wants a time-domain pitch primitive (Beat Repeat's pitch decay, a Grid object),
  it should start from `AdiShaperPitch`.
- **Beat Repeat's history and `AdiShaperTime`.** Yes in principle: one two-channel history ring
  (write, an integer-age read, the oldest readable frame), separate storage per instance, and each
  caller keeping its own slices and phase. It is decided when Beat Repeat starts (DEV-22, after
  3 December), if the three readings the clones seat lists agree. Nothing to number now. The Beat
  Repeat readings join the Live list then, not before, because the list is for the sitting before
  3 December.
- **FloatSignals.** Offered. `adi::device::FloatSignals` (used by `utility~`, `vocoder~`,
  `shifter~`, `multiband_dynamics~`) gives a compiled external float views of Pd's signals in either
  Pd precision, with no allocation in `perform`. `adi_shaper~` may use it as those do; the devices
  lane will review that hunk.
- **"Editable Pd timing cases" (the clones log, 19 November).** Agreed: from the seven fields
  `[adi.transport]` sends (playing, bpm, meter, bar, beat, ticks in the quarter), an editable patch
  cannot recover the quarter. QN 0 and QN 1 in 2/2 send the same fields, and after a 4/4 bar then
  3/4, bar 2 reads as QN 3, where it is QN 4. The answer is a reason and a branch to follow. The fix
  is an optional eighth field, the exact quarter-note position of the first input sample of each Pd
  tick. Existing `[unpack]`s of seven keep working. It needs the graph's exact position per segment,
  which reaches devices through `wantsDeviceTransport` on `clones/shaper-rollup`, not on main. So
  `devices/pd-exact-position` starts when the rollup lands. Its tests are the clones seat's cases:
  QN 0/1 in 2/2, the meter change, a segment at a nonzero buffer offset, upstream latency, and a
  partial tick across a seek and a stop, at awkward block sizes through the engine's adapter.

## 2026-11-19 — DF-E36: a patch cannot close itself, quit, open help, move the search path or write files; AR18 holds under ASan

**PR requested:** head `devices/df-e36` (351917f7, two commits on 12aab48a), base `main`. The lane's
third item (ADR-0248 d8). Each part a failing test first; then the fix in the DEV-09 overlays of
`juce/pd_loader_fix.cmake` (each with its needle check), `juce/pd_engine.*`, `juce/pd_device.hpp`
and `juce/pd_declarations.cpp`.

- **(a) The canvas freed twice.** A `g_editor.c` overlay refuses `menuclose` to a root canvas (force
  0, 1, no argument) and force 2 to any canvas, by name ("ADI refused [menuclose N] (DEV-09)"); force
  0 or 1 to a subpatch, which headless only hides it, stays. `open()` and `close()` also check the
  patch is still in Pd's canvas list before `getdollarzero` and `closefile`. Tests: force 0, 1, 2, no
  argument, the loadbang-only form (inside `libpd_openfile`) and a `[namecanvas]` alias. The unfixed
  suite crashed (access violation) on the first case.
- **The row's "consider menusave and menusaveas":** a real hole, so closed. A patch could write its
  own text to any name in any folder (`[; pd-x.pd savetofile name dir(`: the unfixed test wrote
  `leaked.pd` into another temporary folder), overwrite its own file with what it made of itself at
  run time (`menusave`), or write its data (`write`). A `g_readwrite.c` overlay refuses all three by
  name. `menusaveas` only asks a GUI, of which there is none. Objects that write files (`[text
  define]`, `[array define]`, `[soundfiler]`, `writesf~`) remain the PD4 deny set win owes an ADI
  line for.
- **(b) `menuclose 3`.** Refused in the same overlay; the test runs a second engine whose audio must keep
  flowing, with `pd-A.pd` and `pd-sub` as targets. Plant (force 3
  let through): 3 failures.
- **(c) `help-intro` and `done-popup 2`.** An `s_path.c` overlay of `open_via_helppath` refuses every
  help patch ("ADI refused a help patch: NAME (DEV-09)"); ADI calls it from nowhere, and
  `libpd_openfile` and abstraction loading do not pass there. The test plants `intro-help.pd` and
  `intro.pd` whose `[print leaked]` must not print.
- **(d) The print queue.** Refusals have their own 16-slot MPSC queue, which a flood of `[print]` cannot
  fill; a full console queue writes "[N pieces dropped]" where the gap is. `PdDevice::pumpMainThread`
  now drains the console through a new `PdPatchEngine::pumpConsole()`; pumped every 10 blocks, a
  `[metro 1]` flood drops nothing. AR8's audio-thread allocation count stays at 0. Plant (refusal queue
  off): 1 failure.
- **(e) The search path.** A `g_canvas.c` overlay refuses `declare -path`, `-stdpath`, `-lib` and
  `-stdlib` by name, from the box, the file's `#X declare` record and a run-time message alike; the
  engine's own `libpd_add_to_search_path` is untouched. The static check now reads `#X declare`
  records as `[declare]` boxes. Plant (help and declare overlays off): 2 failures.
- **Numbers.** `adi_pd_engine_tests` 660 checks pass; CTest 246/246 (MSVC Debug, /W4 /WX).

**AR18 under ASan.** The whole Pd engine suite (660 checks) was built with MSVC 14.44's
`/fsanitize=address`. That is a Debug build with `/Zi /Ob0 /Od`, without `/RTC1`, which ASan does not
take; `ASAN_OPTIONS=detect_leaks=0`. It runs clean. With the `#A` unbind in `pd_free` planted out, the
AR18 check fails, and ASan reports the heap use-after-free the overlay exists for: `pd_findbyclass`
(m_pd.c) from `garray_free` (g_array.c:618), on a canvas freed by `glist_clear` from the test's
`clear` message. Restored, it runs clean again. For win: no CI leg runs ASan on Windows. One would
take the two flags above. This is a request only, with no `.github` edit from this lane.

**Next.** DF-E20 (d), the two header comments, and (e), whether a patch device whose patch failed to
open passes audio and reports no latency, now that DF-E36 is in. Then DEV-22, one branch each.

## 2026-11-19 — the Sampler's loadability needs one decision on its sample path (for win)

OneShot plays from a MIDI clip now (entry below). The Sampler is not a canvas: its zones (up to
32, each a sample with key, velocity and select ranges and fades) live in its "component" state,
and #414's slot ops validate a slot against a canvas's `[adi.sample]` declarations
(`sample_slots.cpp`'s `checkedDevice`), so no product call can give it a sample today.
**Two ways, recommendation first:** (1) the Sampler's catalogue entry declares N sample slots
(say 32) through a small canonical declaration text, the slot ops fill them, and each filled slot
is a zone with the full key/velocity range until a zone op (the engine's) edits its ranges —
loadable and audible at once, ranges later; (2) a zone op from the start (engine), the slot ops
untouched. Either way the native Sampler is dispatched directly by `makeBuiltinDevice`. This seat
takes (1) on your word; until then DF-E36 goes first, as ADR-0248 d8's order allows.

## 2026-11-19 — DEV-21: OneShot is a loadable instrument, so a MIDI clip sounds

**`devices/oneshot-instrument` (`0d309d4d`), stacked on #656 (`devices/oneshot-live-params`),
on win's loan of `builtin_catalog.cpp` and `makeBuiltinDevice`'s instrument dispatch.**

- Catalogue: `BuiltinInfo::instrument`; OneShot (`org.adidaw.builtin.oneshot`, Instruments, no
  audio in, MIDI in). **19 entries** with it; **the clones seat's ADI-Shaper makes 20** — whichever
  of us lands second updates the array sizes and `test_builtin_devices`' counts (19→20 entries,
  18→19 available). Clones: please confirm in your log.
- The insert writes subtype `instrument` (`builtin_insert.cpp`); `BuiltinDevice` reports
  `EventFlow::Consume` for an instrument.
- OneShot's canonical canvas is embedded with a native factory (`native_devices.cpp`): canvas
  parameter N → core index N−1 in its units, notes at their block-relative frames, sample from
  slot 1 (#414's slot ops), idempotent prepare.
- **Beyond the named loan, for win's review:** `builtin_samples.cpp` gains OneShot's sample
  (source kept, stereo from the first two channels, at the source's rate, restored through
  `prepareOneShotSample`) — the slot path a OneShot sample needs; `builtin_insert.cpp`'s subtype
  line. Both are small and named in the commit.
- **Test, through the product's calls:** insert on a MIDI track (subtype instrument), assign a
  1 kHz sine to slot 1 (one undo step), ordinary Session, a MIDI clip's C3: sound from frame 1 at
  **0.1256** = 0.5 × OneShot's default −12 dB (Live's). Plant: not marked instrument → 3
  failures. Builtin devices 110; CTest **245/245**.

**PR requested** — title "OneShot is a loadable instrument, so a MIDI clip sounds (DEV-21)";
head `devices/oneshot-instrument` at `0d309d4d`; base `devices/oneshot-live-params` (#656) until it
lands, then `main`. Body: the commit message.

**Next:** the Sampler's loadability — its zones are not a canvas, so it needs its own sample path
(a zone list built from slot assignments); then DF-E36.

## 2026-11-19 — DF-E37 (d) and (f): the looped release; an idempotent OneShot prepare — DF-E37 done

**Both on #656's branch, `devices/oneshot-live-params` (`c8363f0b`, `14181dfb`).**

- **(d) Failing first:** a 2 s 100 Hz sample, Loop Length 5 % (100 ms), Release 1000 ms, note-off
  2.5 s in: 200–300 ms after note-off, rms **0** (stopped at the loop's end). **Fix:** a Classic
  voice with Loop on loops through its release (crossfade included) and ends at zero. Now rms
  0.2123 (expected 0.2121), exact zero after the release, no step above 1.2× the tone's slope;
  Loop off still ends at End; a Sampler zone sounds through a release longer than its loop.
  Live's manual covers Sampler; Simpler is reading 3.
- **(f) Confirmed** (the review's "to confirm"): a same-format `prepare` cut the note. **Fix:**
  early return at an unchanged rate and block size (ADR-0090 d5); `release()` makes the next
  prepare fresh; the Pd external's DSP restart calls `resetVoices()`; the Sampler releases each
  layer before its own fresh prepare (a plant without that fails). OneShot.md updated.
- OneShot 324, Sampler 39; CTest **245/245**. Plants: held-only loop → OneShot and Sampler fail;
  no layer release → Sampler fails.

**DF-E37 is done:** (a) on #659, (b) on #660, (c) on `devices/builtin-real-values` (lands first),
(d) (e) (f) on #656. #656's head is now `14181dfb`.

**Next (ADR-0248 d8 (2)):** OneShot and the Sampler as loadable instruments, on win's loan of
`builtin_catalog.cpp` and `makeBuiltinDevice`'s instrument dispatch; the clones seat adds
ADI-Shaper's entry to the same file, so the count lines are agreed in both logs.

## 2026-11-19 — DF-E37 (e): the Sampler's zone fades are a gain, not a scaled velocity

**On #656's branch, `devices/oneshot-live-params` (`892830ac`).**

- **Failing first, at the default Vel > Vol 35 %:** a linear select fade at w 0.5 played 0.825 of
  the zone; a constant-power one 0.897; a key crossfade's midpoint summed +2 dB.
- **Fix:** the weight is queued on the layer's OneShot (`queueNoteGain`, one per forwarded note-on,
  in order) and latched as the voice's own gain; velocity is left as played. Kept: the velocity
  zone on the unscaled velocity, the selector latch, the laws (ADI's own, labelled), the skip at
  w ≤ 0, no allocation (fixed queue).
- Now 0.5 / 0.707 / 2 × 0.707; velocity 0.5 still plays at 1 − .35 + .35 × .5. Plant: scaled
  velocity → 3 failures. A test-order detail: the Sampler's saved state differs before and after a
  prepare (re-derived zone values); the test saves after a render as it always did — noted, not
  changed here. Sampler 35; CTest **245/245**.

**PR request #656 stands** with this commit (head now `892830ac`); with (c) it waits only for
`devices/builtin-real-values` to land first.

## 2026-11-19 — DF-E37 (c): a built-in's saved values survive a range change

**`devices/builtin-real-values` (`2a4a5044`), from `main`, on win's loan of `builtin_host.cpp`'s
value mapping.** It must land before #656, #659 and #660 (their new ranges).

- The state (still `version: 1`) now also carries `real:{id:value}` and `units:{id:unit}`. On load
  the real value wins while its unit is unchanged: clamped, rounded/snapped, turned back into a
  position in the one `device.loadState`. A unit change falls back to the position. Positions-only
  states keep their positions (pre-release loss, written in ENGINE-BUILTINS.md). The mapping is
  now `juce/builtin_values.*`, tested under two declarations.
- `setParam` honours `value.real` on lin/int/menu/toggle curves only. **For the engine:** the
  parameter mirror's `real` is a linear reading of the position (`param_ops.cpp`'s
  `fillRequest`), wrong on log and exponent curves; honouring it would have set Frequency 1000 Hz
  to about 12.4 kHz on every reopen. So on those curves the position is taken — which means a
  mirror row pushed after a state load (`Session::restoreState` pushes rows whose position
  differs) still moves a log parameter when its range changed. **Request:** compute the mirror's
  `real` with the parameter's curve (the device's own reading, e.g. `getParam(id).real` after the
  set), or compare by real in `restoreState`; then `setParam` can honour real on every curve.
- Tests: Output −12 dB, Frequency 1000 Hz, Window 40 ms come back exactly under the new ranges;
  menu stays; unit change falls back; clamp; refusal leaves nothing to apply; per-curve `setParam`.
  Plant: real ignored → 5 failures. 16 checks; CTest **246/246**.

**PR requested** — title "A built-in's saved values survive a range change (DF-E37 c)"; head
`devices/builtin-real-values` at `2a4a5044`; base `main`. Body: the commit message.

## 2026-11-19 — DF-E37 (b): Shifter returns to a clean unison after any pitch move

**On #660's branch, `devices/shifter-latency` (`7c5d8158`).**

- **Failing first:** +7 st for 1,926 samples and back, then an impulse of 0.5: four nonzero
  samples, half copies around 960 and 2880 (the review's case) — a comb for good.
- **Fix:** at unison with the taps' phase away from rest (0 or 0.5), a 20 ms crossfade
  (`Pitch::kRestSeconds`) from the two taps where they are to the clean Window/2 read, then the
  phase rests; a move away from unison cancels it. No glide, no jump.
- Now: the impulse once, 1920 late, exactly 0.5; a 100 Hz tone through the return steps at most
  1.04× its own steepest slope. Kept: `liveUnison` (240/1920/8400), latency 0, pitch within 0.04
  cents, the +7 st figures, no allocation. Live's own return is reading 2 in the list.
- Plant: no return → fails. Shifter 47; CTest **242/242**.

**PR request #660 stands** with this commit (head now `7c5d8158`). The Shifter question is struck
(answered: Live's way, ADR-0247 d9).

## 2026-11-19 — DF-E37 (a): Saturator HQ no longer comb-filters at a partial Dry/Wet

**On #659's branch, `devices/saturator-latency` (`e539f7dc`).**

- **Failing first:** at 10/50/90 % Dry/Wet the response from 1 to 18 kHz against the 100 %-wet
  one: 1.50 / **12.96** / 1.94 dB off (a 3-sample dry against the 4× minimum-phase wet).
- **Fix:** for 0 < Dry/Wet < 100 % with HQ on, the dry is upsampled by the same filter beside the
  wet and mixed before the down-sampling filter: one phase response for both. 0 % keeps the exact
  3-sample dry (alignment test unchanged); 100 % has no dry. Kept: 3 samples reported, unity DC,
  alias suppression (0.00044), native/Pd parity, no allocation.
- Now **0.000 dB** from the 100 %-wet response at every mix. The resampler's own roll-off (0.9 dB
  at 18 kHz) is reported, not hidden; reading 1 in the list settles whether Live's is the same.
- Plant: the old dry → 3 failures. Saturator 40; CTest **242/242** (that branch's base). The test
  file's comment named a machine; it no longer does.

**PR request #659 stands** with this commit (head now `e539f7dc`).

## 2026-11-19, evening — ADR-0248 read: this lane's order from here

The steering (win's entry of this evening) is this lane's queue now, in its order: (1) DF-E37, each
item a failing test then the fix — (a) Saturator HQ's notch at partial Dry/Wet before #659, (b)
Shifter's lasting comb before #660, (c) saved values under new ranges on win's loan of
`builtin_host.cpp`'s value mapping, (d) the looped release, (e) the Sampler's fades and velocity,
(f) OneShot's idempotent prepare; (2) OneShot and the Sampler as loadable instruments on win's loan
of `builtin_catalog.cpp` and `makeBuiltinDevice`; (3) DF-E36; (4) DEV-22 where it stands (Auto
Filter, Chorus-Ensemble, Delay, the reverbs and Phaser-Flanger branches above are its last
pieces). The Live readings are now one list under FOR THE DIRECTOR, with the engine's DF-E38.

**Already done, on branches:** DF-E20 (d) — both header comments — is on `devices/true-peak`
(`b5ee5aa6`): `true_peak.hpp` now gives the Annex's rule (6x at 32 kHz, so 192 kHz; never below
176.4 kHz from 22.05 kHz up) and `spectrum.hpp` says a sine half a bin off reads −1.42 dB at 1,024
points. When `devices/true-peak` and the engine's f28eacd0 meet, the double path keeps the
non-finite guard. **DEV-16:** steps 3 and 4 (`[line~]`; the director's device's most-used boxes)
were pushed before the steering arrived (`10fb3b4d`, `607d3d5a` on `devices/amxd-translator`);
DEV-16 stops there until DEV-10 exists. Win may take the PR at step 2 (`fe9fa8af`) and leave the
two later commits on the branch, or take all four.

## 2026-11-19 — DEV-22: Phaser-Flanger notches when all wet, and starts at Live 12.4's default

**`devices/phaser-flanger-defaults` (`837700a1`), from `main`.**

- **Reading:** Live's default is 100 % wet (its preset), and its render notches (997 Hz −8.5 dB,
  averaged over the LFO). ADI's phaser wet was the bare all-pass chain — unit magnitude, so at
  100 % it would only shift phase; ADI's notches came from its 50 % mix. **A real defect, found by
  the default:** moving ADI to Live's 100 % without this would have silenced the effect.
- **ADI now:** the wet is the classic sum, (input + chain)/2; defaults from Live's preset (2 notches
  at 400 Hz, Spread 35 %, Flanger 2.5 ms, LFO 0.149 Hz triangle at 75 %, duty 10 %, LFO 2 at 0.5 Hz,
  envelope on 1 ms / 120 ms, Safe Bass 80 Hz, 100 % wet) and ranges where they mean the same.
  Live's 42 notches are not claimed (ADI's core has 8 stages).
- Tests: comparable defaults equal Live's; LFO held, the default dips 40.8 dB between 200–800 Hz;
  the single-notch oracles restated for the classic sum. The 8.5 dB is not compared (LFO phase at
  capture unknown). Plant: bare all-pass wet → 3 failures. Phaser 30, Pd 41; CTest **245/245**.

**PR requested** — title "Phaser-Flanger notches when all wet, and starts at Live 12.4's default
(DEV-22)"; head `devices/phaser-flanger-defaults` at `837700a1`; base `main`. Body: the commit
message.

## 2026-11-19 — DEV-22: Live's Dry/Wet is sin/cos, confirmed at a second point; both reverbs follow

**`devices/reverb-equal-power` (`69c75a50`), from `main`.**

- **Reading (DV-09):** Hybrid Reverb's dry impulse is 0.7071 at its default 50 %; **Reverb's is
  0.64945 at its default 55 %** = cos(0.55·π/2) — which settles the curve (the square-root
  equal-power law gives 0.6708). With Chorus-Ensemble and Delay, four Live 12 devices measured, one
  law.
- **ADI now:** Reverb and Hybrid Reverb crossfade sin/cos (were linear), exact at the ends; Reverb's
  Dry/Wet default 55 % (Live's preset).
- Tests: Reverb's first sample 0.64945 at its default; Hybrid Reverb's dry 0.70711 (IR silent at
  its start). Plant: linear → fails. Reverb 28, Hybrid Reverb 51; CTest **245/245**.

**PR requested** — title "Reverb and Hybrid Reverb mix sin/cos as Live 12.4 measures; Reverb's
default 55 % (DEV-22)"; head `devices/reverb-equal-power` at `69c75a50`; base `main`. Body: the
commit message.

**Not explained by it:** Echo's dry is 0.300 and Resonators' 0.500 at their defaults; their default
Dry/Wet values are not in Live's preset library (the corpus Set deleted each device after
rendering), so they wait for a reading (recipe 5's kind: one look at a fresh device).

## 2026-11-19 — DEV-22: Delay's loop filter is 12 dB/oct, and its default is Live 12.4's

**`devices/delay-defaults` (`92511657`), from `main`.** Third of the corpus devices.

- **Reading (DV-09 default renders, 48 kHz, 120 BPM):** first output sample 0.7071 of an impulse
  (dry, equal-power at 50 %). Six tones (dB/°): 71 −8.53/−27.7, 223 −11.16/+26.9, 997 +2.58/+44.1,
  3001 −9.60/−37.9, 7001 −11.39/−24.5, 11003 −3.23/−52.3.
- **Method:** with Live's default (3/16 = 18 000 samples, Feedback 0.5, Dry/Wet 0.5), each tone
  was solved for the loop filter F under three topologies (filter in the loop, in the feedback
  only, after the loop) and two mix laws. Only in-loop + equal-power gives a clean filter: ~0 dB in
  band, +79° and −2.1 dB at 71 Hz — a second-order high-pass at 1 kHz/2⁴ = 62.5 Hz to 0.05 dB and
  0.5° (a one-pole gives +41°), and a second-order low-pass near 16 kHz.
- **ADI now:** Butterworth TPT high-pass and low-pass in the loop at f·2^(∓W/2) (was one-poles);
  defaults from Live's preset — synced 3/16 (right stores 4 behind Link), 375 ms free, Feedback
  50 %, Filter on, 1 kHz (50..18000 Hz), Width 8 oct, LFO 0.5 Hz triangle, Dry/Wet equal-power.
- Test: first sample 0.70711; six tones measured over the **capture's own signal and window**
  (the probe is six −30 dBFS sines from phase 0; the ratio is taken 1–3 s after onset): within
  **0.30 dB / 1.9°** (bound 0.35 dB / 2°). An exact model of that window reproduces ADI's deviation
  to the fourth decimal, so the rest is the filter's exact shape: a free fit of both corners and
  dampings reaches 0.08 dB at best and identifies no cleaner design. (A second commit, `82e994bb`
  on the same branch, corrects the first's "unsettled capture" explanation.) Oracles that assumed
  the old defaults state their mechanics. Plant: no high-pass → 2.7 dB / 71° off. Delay 93, Pd 44;
  CTest **245/245**.

**PR requested** — title "Delay's loop filter is 12 dB/oct and its default is Live 12.4's
(DEV-22)"; head `devices/delay-defaults` at `82e994bb`; base `main`. Body: the commit message.

**Reading that would settle the decimals:** the six tones with Feedback 0 (the filter alone, once).

## 2026-11-19 — DEV-22: Chorus-Ensemble mixes equal-power, as Live 12.4 measures

**`devices/chorus-ensemble-defaults` (`b285030e`), from `main`.** Second of the corpus devices.

- **Reading (DV-09, default render, impulse 0.5 L / −0.25 R):** the first output sample is
  0.7071 of the input on both sides; the response sums to 1.4211. At Live's default Dry/Wet 50 %
  that is the dry path at an equal-power crossfade. ADI's was linear (0.5).
- **ADI now:** Dry/Wet as cos/sin, exact at both ends, Vibrato all wet; Live's defaults and
  ranges where the control means the same (High-Pass 50 Hz, Rate 0.1..15 Hz at 0.9, Feedback
  0..99 %, Output to +6 dB). Only the 50 % point is measured — the curve between is the common
  equal-power law, not read. Not mapped: Live's Delay Time chooser (6 entries, default index 3;
  its list is a reading). The six tones are LFO-phase dependent and are not compared.
- Tests: first sample 0.70711 (Live's float gain differs by 1.1e-6), sum 1.4143 vs 1.4211; the Pd
  in-phase oracle now √2. Plant: linear law → 2 failures. Chorus-Ensemble 27, Pd 26; CTest
  **245/245**.

**PR requested** — title "Chorus-Ensemble mixes equal-power as Live 12.4 measures, with Live's
defaults (DEV-22)"; head `devices/chorus-ensemble-defaults` at `b285030e`; base `main`. Body: the
commit message.

**A question this raises (win, or the next reading):** if Live's Dry/Wet is equal-power in its
newer devices, the same may hold for Delay (its impulse also starts at 0.7071 at 50 %) — checked
next with Delay's own numbers, not assumed.

## 2026-11-19 — DEV-22: Auto Filter's default is Live 12.4's and nulls its measured response

**`devices/auto-filter-defaults` (`6521366e`), from `main`.** First of the corpus devices in the
manual's order (the DV-09 entry further down), from readings already taken — no new Live.

- **Reading (DV-09, the research's default renders, 48 kHz):** Auto Filter's browser default (LP,
  24 dB, 10 kHz, Resonance 0, Clean; values from Live's own default preset, read privately) gives
  71/223/997/3001/7001/11003 Hz at −0.0006/−0.0063/−0.1255/−1.1305/−6.0077/−14.5236 dB, phase
  −1.39/−4.36/−19.47/−58.15/−130.93/+164.70°; impulse peak +2 samples at 0.2942.
- **The fit:** two cascaded TPT state-variable low-passes, k = 2 (Q 0.5 each), cutoff prewarped
  — to four decimals. That is ADI's Clean core already; ADI's default was 1 kHz, 29 %, 12 dB.
- **ADI now:** Live's defaults, and Live's ranges where the control means the same — Frequency
  20..20000 Hz (10 kHz), Resonance 0, Slope 24 dB, envelope Attack 1 ms (to 100 ms) and Release
  250 ms, LFO 0.1..60 Hz, Steps 2..64, sidechain EQ 30..15000 Hz at 200 Hz, Q 0.1..12, ±15 dB.
  Not claimed: Live 12's separate 12/24 and 6–48 Morph slopes, LFO time modes, other circuits,
  the Resonance law beyond 0 (only k = 2 at 0 is measured).
- Test: six tones within **0.00024 dB / 0.0014°**, impulse peak +2 at 0.2942. Plant: the old
  12 dB default → 3 failures. Auto Filter 166 checks; CTest **245/245**.

**PR requested** — title "Auto Filter's default is Live 12.4's, and nulls its measured response
(DEV-22)"; head `devices/auto-filter-defaults` at `6521366e`; base `main`. Body: the commit message.

**Next:** the same for Chorus-Ensemble, Delay, Echo and on down the corpus list.

## 2026-11-19 — the readings this lane waits on, as recipes (FOR THE DIRECTOR item 2)

One device, one question each, Live 12.4 at 48 kHz, a fresh Set, the device from the browser (no
preset), nothing else on the track. What comes back here is the statement (value, conditions,
method); files stay in the director's research.

1. **Arpeggiator style order (DEV-19, XC-25).** A MIDI clip with C3 E3 G3 B3 held one bar; Clip
   View → Transform → Arpeggiate, Rate 1/16, Steps 0, Gate 50 %. For each of the 18 styles, the
   16 note pitches in order (or the Set saved and its notes read). Also, with C3 E3 G3 B3 D4 (five
   notes), the Converge/Diverge/Con & Diverge/Pinky/Thumb styles; and Up with Steps 1, Distance
   +12, to see whether the transposed copy follows the whole pattern.
2. **Simpler voices and velocity (DEV-21).** The Voices chooser's full list. Then Classic mode, a
   1 kHz sine sample, Vel > Vol 35 %: render one note at each velocity 1, 32, 64, 100, 127 and
   read the peak level of each.
3. **Simpler envelope slope (DEV-21).** Attack 100 ms, slope 0 and slope +1 and −1: render one
   note of a constant-level sample and read the level at 25, 50 and 75 ms; Decay 100 ms to
   sustain −24 dB the same way.
4. **Simpler filter (DEV-21).** Filter on, Clean, LP 12, cutoff 22 kHz then 1 kHz, Resonance 0 %
   and 100 %: white noise through it, the magnitude at 500 Hz, 1 kHz, 2 kHz (a spectrum read).
5. **Redux defaults (DEV-22).** A fresh Redux: every control's value as shown.
6. **Saturator's Width, Curve, Period (DEV-22).** Waveshaper mode, Drive 20 dB, a 100 Hz sine at
   −6 dBFS: render with Width 0 / 0.3 / 1 (others default), then Curve 0 / 0.05 / 1, then Period
   0 / 0.5 / 1; nine renders, each read as its first five harmonic levels.
7. **Shifter's dry path and sweep phase (DEV-22).** Pitch mode +7 st, Dry/Wet 50 %, an impulse:
   is the dry impulse at 0 or delayed with the wet? Then the sweep render that DV-06 used, at
   +7 st, its first 2,000 samples.
8. **Envelope Follower and Shaper (DEV-07, DEV-16).** Envelope Follower mapped to a Utility
   Gain, Rise 0, Fall 0 then defaults, a 1 kHz burst of 100 ms: the mapped value's curve (Utility
   output level over time). Shaper: its default shape at 1 Hz mapped the same way, one cycle.
9. **Max's own `line~` start and `[t]` tie order (DEV-16; Max, not Live).** A `line~` ramp
   0 → 1 over 100 ms from a loadbang: the value at the first sample. Two boxes at the same x fed
   from one outlet: which fires first.

## 2026-11-19 — DEV-16, step 4: what the director's own device uses most

**`devices/amxd-translator` (`607d3d5a`), one more commit on the same PR request.** Chosen by the
box counts of his `AVC Spectrum Meter.amxd` (the entry below on step 3).

- `[prepend]`/`[append]` (list then `[list trim]`, so the first word is the selector, as Max);
  `[f]`, `[i]`, `[delay]`, `[abs~]`; `[!- n]` as (−x) + n, int unless n is a float; `[gate n]` as
  a prefix and a `[route]`; `[selector~ n]` as one `[*~]` per input, summed; `[slide~]` as its own
  recursion in `[fexpr~]`; **`[biquad~]`**: Max's a0 a1 a2 b1 b2 subtract the feedback, Pd's
  fb1 fb2 ff1 ff2 ff3 add it — arguments reordered, a coefficient list reordered and negated on the
  way in (a control source into its left inlet takes that route, a signal goes straight in);
  `live.text` (toggle), `live.menu`, `live.tab` as parameters.
- Played in libpd: `[prepend set]` 3 → "set 3"; `[i]` 2.9 → 2; `[!- 10]` 3.5 → 7, `[!- 10.]` →
  6.5; `[gate 2]` passes 5 out of outlet 2; `[selector~ 2 1]` passes input 1 exactly; `[slide~ 10
  10]` on a step is 1 − 0.9^(n+1) sample for sample; Max's `biquad~` "1 0 0 −0.5 0" starts at 1,
  settles at 2. Plant: feedback not negated → 0.667, fails. 60 checks; CTest **247/247**.

**PR requested** — as step 1's entry (head `devices/amxd-translator`, now `607d3d5a`).

## 2026-11-19 — DEV-10: the three starter templates, proved

**`devices/pd-templates` (`bc3a84de`), stacked on `devices/pd-object-walk`.** M7's "Pd templates"
(the lane table) — DEV-10's "three starter templates".

- `pd/templates/`: **Audio Effect** (`adc~ 1 2` → Gain in dB, smoothed 20 ms → `dac~ 1 2`),
  **Instrument** (`notein` → `[poly 1 1]` → one sine voice, 5 ms rise to the velocity, Release
  fall), **MIDI Effect** (`notein` → Transpose → `clip 0 127` → `noteout`). Vanilla plus one
  `[adi.param]` each, with a comment on where the ports are and what to replace.
- Tests: each parses with no problem, walks clean (PD4) against the running build, opens with
  nothing in the console; the audio effect is the input bit for bit at 0 dB (64 late) and plays
  −6.000 dB when the host sends `$0-adi-1`; the instrument plays A4 at 0.25 peak at full velocity
  and is silent after Release. Plant: a cyclone object → walk and console both fail. 18 checks;
  CTest **247/247**.

**PR requested** — title "DEV-10's three starter templates, proved to parse, walk, open and
play"; head `devices/pd-templates` at `bc3a84de`; base `devices/pd-object-walk` until it lands,
then `main`. Body: the commit message.

**For the engine/window (DEV-10):** offering them is the new-device flow's (identity
`org.adidaw.pd.<kind>`, the 'patch' role with the template's text). The MIDI effect's output is
proved once DEV-14's `midiOut` (`devices/pd-midi-out`) lands.

## 2026-11-19 — DEV-16, step 3: `[line~]`'s chained segments

**`devices/amxd-translator` (`10fb3b4d`), one more commit on the same PR request.**

- Max's `[line~]` plays a list of (target, ms) pairs one after another; Pd's `[vline~]` reads a
  third number as a delay, so a Max list sent straight to it is wrong. `[line~]` becomes a
  `[pd max-line~]` that walks the pairs and schedules each as "target ms delay" with the delay
  accumulated; the first goes with delay 0, which in `[vline~]` cancels what was still scheduled,
  as a new list does in Max. Its ramp-time inlet, end bang and an initial value are refused.
- Played in libpd: "0 0 1 100 0 200" → 0, 0.5, 1, 0.5, 0 at 0, 50, 100, 200, 300 ms within 1e-3.
  `[vline~]` leads the expected ramp by one sample; Max's own `line~` timing is a reading to take
  (Max, not Live). Plant: no accumulated delay → 3 failures. 45 checks; CTest **247/247**.

**PR requested** — as step 1's entry (head `devices/amxd-translator`, now `10fb3b4d`).

**The one real device in reach** is the director's own, in this repository: `adi-m4l-analyzer`'s
`AVC Spectrum Meter.amxd` (plain `ptch`, 541 boxes). It is an analyser on Jitter, `js` and the Live
Object Model (`jit.*`, `js`, `jsui`, `live.object`/`path`/`observer`), so it is the AI path's case,
not the translator's. Its most used boxes set the order for later steps: `prepend` (63), `sel`
(14), `live.menu` (14), `snapshot~` (13), `expr` (12), `biquad~` (10), `live.text` (10), `atodb`,
`change`, `route`, `slide~`, `gate`, `maximum`, `average~`, `selector~`, `delay`, `!-`. Several
need Max's own behaviour read first (int or float in `sel`/`change`, `atodb` at 0, `biquad~`'s
coefficient order) — Max, not Live.

## 2026-11-19 — DEV-16, step 2: Max's integers and subpatchers, made explicit

**`devices/amxd-translator` (`fe9fa8af`), one commit on top of step 1** — the same PR request now
carries both steps.

- Each Max box now maps to a fragment (Pd boxes, their inner connections, where the Max inlets and
  outlets landed), so a Max/Pd difference is written into the patch instead of refused.
- **Integers**: `[+ 1]`, `[/ 2]`, `[% 3]`, comparisons, `[min]`/`[max]`/`[abs]` with an int or no
  argument, `[pack]`/`[unpack]` (default "0 0", and `i` slots), `[trigger]`'s `i`, the number box —
  each int inlet through `[i]` first; integer division and int outlets through `[i]` after. Float
  forms unchanged; float modulo refused.
- **Subpatchers**: `[p name]` → `[pd name]`, recursively; ports keep Max's left-to-right order and
  are `[inlet~]`/`[outlet~]` where a signal crosses; a refusal inside is named by path
  (`p helper/obj-1`).
- Played in libpd: `[+ 1]` fed 2.7 → 3; `[/ 2]` fed 7 → 3; `[> 0]` fed 0.5 → 0; `[pack]` fed 2.9
  → 2 0; `[unpack]` fed 3.7 4.2 → 4 then 3; `[t i f]` fed 2.5 → 2.5 then 2; a signal `[p half]`
  plays × 0.5 exactly; a control `[p inc]` fed 1.5 → 2. Plant: int inlets bypassing `[i]` → 3
  failures. 38 checks; CTest **247/247**.

**PR requested** — as step 1's entry (head `devices/amxd-translator`, now `fe9fa8af`).

**Still to do in DEV-16:** `line~` as segments, bpatchers, MIDI devices (after DEV-14/18), the AI
path for the rest, Suite's modulators (after DEV-07), parity against Live (readings).

## 2026-11-19 — DEV-16, step 1: the `.amxd` translator — reader, pre-flight, the exact subset

**`devices/amxd-translator` (`70c63694`), stacked on `devices/pd-object-walk`.** P2, taken now
because every M-row left in this lane waits on a reading, an ADR or another seat. It needs DEV-10
to become a device; the translation itself needs nobody.

- `translateAmxd(bytes)` (`src/juce/pd_amxd.*`, no libpd): reads the `ampf`/`meta`/`ptch`
  container; Live's encrypted (`ciph`) devices and frozen (`mx@c`) devices are not read. Audio
  effects only in this step.
- **The pre-flight is the translation table**: a Max box translates only where Pd behaves the
  same — signal arithmetic (scalar or signal right inlet chosen from what feeds it), `cycle~`
  (not its phase inlet: an offset in Max, a reset in Pd), `phasor~`, `sig~`, `plugin~`/`plugout~`
  → `adc~`/`dac~`, `loadbang`, `live.thisdevice`'s load bang, `metro` in ms, `trigger` b/f/l/s,
  float arithmetic only (Max's `[+ 1]` is integer), `send`/`receive` (`---x` → `$0-x`), `print`,
  messages (escaped), flonums, comments. `live.dial`/`live.numbox`/`live.slider` → `[adi.param]`
  with range, default, unit and curve. Everything else is refused, box by box, with its reason;
  the result must then pass PD4's walk.
- **Max's right-to-left order made explicit**: each control fan-out goes through `[t a ...]`, its
  right outlet to the rightmost box. Equal x: lower box first — an assumption; Max's tie rule is a
  reading to take (Max, not Live; not run here).
- Tests (every device written by the test): container cases; a gain device and a `live.dial`
  device translate, walk clean against the running build, open in libpd and play input × 0.5 and
  × 0.25 **exactly** at the reported 64 samples; a fan-out prints right before left in Pd. Plant:
  no trigger → 3 failures, one heard at run time (Pd alone fires left first). 25 checks; CTest
  **247/247**.

**PR requested** — title "The .amxd translator, step 1: reader, pre-flight and the exact subset
(DEV-16)"; head `devices/amxd-translator` at `70c63694`; base `devices/pd-object-walk` until it
lands, then `main`. Body: the commit message.

**Still to do in DEV-16:** step 2 (integer arithmetic made explicit with `[i]`, `pack`/`unpack`,
subpatchers as `pd` canvases with `inlet~`/`inlet` chosen by what crosses them, `line~` as
segments); MIDI devices after DEV-14/DEV-18; the AI path for the rest (Propose tier, engine);
Suite's LFO/Envelope Follower/Shaper after DEV-07. The parity test against Live needs readings.

## 2026-11-19 — PD4: the allowed-object walk, with the vocabulary read from the running build

**`devices/pd-object-walk` (`2d949a65`), from `main`.** PD4 serves DEV-09, DEV-10, DEV-11 and
DEV-16; it is this lane's piece of DEV-10 (M7) that needs nobody else.

- `walkPdObjects(patchText, abstractions, vocabulary)` (`src/juce/pd_object_walk.*`, no libpd,
  every ABI): names each box this build cannot make by file (the patch or an abstraction),
  subpatch path, class and object-box number (counted as `parsePdDeclarations` counts). Subpatches
  walked in place; stored abstractions (DEV-10's 'abstractions' role), named as `[voice]` or by
  `[clone]`'s argument (Pd 0.56's own argument rule), walked once each. Refused: unknown class,
  a `$` class name (only known when Pd runs), an abstraction containing itself, nesting past 64,
  a canvas that does not close. `describe()` gives one line per refusal for the device's
  problem list.
- `PdRuntime::creatableClasses()` (`pd_vocabulary.cpp`) reads `pd_objectmaker`'s creators under
  instance 0's lock: the method table **plus the bang/float/symbol/list slots**, where
  `class_doaddmethod` files those selectors (read in m_class.c; the first version, table only,
  refused `Resonators.pd` for its `[list]` boxes — the test caught it). 329 names, ADI's
  compiled-in externals included; no list kept by hand.
- One tokeniser: `pdRecords` is the declaration parse's scanner made public; both read it.
- Tests: walk cases on a hand-made vocabulary; the build has vanilla's objects and ADI's
  externals and no cyclone/zexy; **all 27 shipped patches walk clean**. Walk 77; declarations 76
  unchanged; CTest **246/246**.

**PR requested** — title "The allowed-object walk, with the vocabulary read from the running
build (PD4)"; head `devices/pd-object-walk` at `2d949a65`; base `main`. Body: the commit message.

**For win (a ruling, not the director's unless you say so):** the vocabulary is the build's, so
it includes vanilla's `[netsend]`, `[netreceive]`, `[oscparse]`, `[file]`, `[textfile]`,
`[soundfiler]` (its `write`), `[writesf~]` and `[pdcontrol]`. A user patch (DEV-10) or a
translated `.amxd` (DEV-16) can therefore open sockets and write files. Recommendation: the walk
takes a deny set from one ADR line — network and file-writing classes refused in user patches
unless the user allows them per device — and the fence stays the build's for natives (ADR-0188
d8). Blocks nothing now: the walk has no caller until DEV-10's `loadState`.

**Next:** DEV-10's other devices piece, the three starter templates, waits on its identity and
state roles (the engine's). Then DEV-16's `.amxd` reader on top of this walk (P2, after DEV-10).

## 2026-11-19 — taking Live 12.4: Arpeggiate's style order (DEV-19 / XC-25), MIDI only

One device, one question: the step order each of the 18 Arpeggiator/Arpeggiate styles gives for a
held 4-note chord (and 5 notes where the middle is ambiguous), with Steps/Distance for transposed
repeats. engine-win's log checked first: its current work is the scanner and export, no ASIO run.
No audio is rendered; the reading stays in the director's research, the measured orders come here.

**Not taken:** control of Live was declined on this machine when asked. Nothing was opened or
changed. The 18 style orders stay a reading to take (Converge, Diverge, Con & Diverge, the Pinky and
Thumb styles, and the endpoints of UpDown vs Up & Down are not defined by the manual); the
ordering function waits for it, and DEV-19 waits for DEV-17/18 anyway.

## 2026-11-19 — DEV-28, step 3: the meter agrees with libebur128 to 0.0000 LU; Main's CPU cost

**`devices/loudness-oracle` (`94cfc664`), stacked on `devices/true-peak`.**

- `tests/test_loudness_oracle.cpp`: pink noise, a speech-like signal with pauses, a 20 Hz–20 kHz
  sweep and 5.1 pink noise (BS.1770 surround weights, LFE out) through `dsp::LoudnessMeter` and
  libebur128 1.2.6, at 44.1, 48 and 96 kHz. Integrated, max M, max S and the last M and S agree to
  **0.0000 LU** in every case; the suite holds them to 0.01 (the plan's gate is 0.1).
- LRA: libebur128 hops its short-term window at **1 Hz** (3 s every second; read in its source),
  Tech 3342 at 10 Hz. ADI's own short-term readings taken on the 1 Hz grid and ranked libebur128's
  way agree to 0.0000; ADI's 10 Hz LRA differs from libebur128's by up to 0.45 LU (the sweep), both
  correct for their grid. Held to 0.01 and 0.5. True peak, two interpolators: worst 0.11 dB, held
  to 0.2.
- Test-only: built only when `third_party/libebur128` is fetched, never linked into the app;
  `ADI_REQUIRE_LOUDNESS_ORACLE=ON` makes its absence a configure error. Plant: the relative gate
  at −9.5 → 3 failures. Oracle **96** checks.
- **Main's CPU cost** (the gate asks for it), `/O2`, one thread, 512-frame blocks, stereo, on an
  8-core desktop CPU: 48 kHz **0.33 % of one core** (35.6 µs a
  block), of which true peak 0.29 %; 96 kHz 0.41 %. `readings()` 0.01 µs. libebur128 doing the same
  (I, LRA, TP, histogram mode): 0.46 % and 0.71 %. Method: 120 s of noise, best of 5.

**PR requested** — title "The loudness meter against libebur128, test-only (DEV-28, MX-27)"; head
`devices/loudness-oracle` at `94cfc664`; base `devices/true-peak` until the stack lands, then
`main`. Body: the commit message plus the CPU paragraph above.

**For win — the pin (LD3 says you write the row, I propose it):** `jiixyj/libebur128 libebur128
MIT v1.2.6 67b33abe1558160ed76ada1322329b0e9e058b02`, role `build` so CI's `--build-only` fetch
runs the oracle, plus its `EXTERNAL-CODE.md` row (test-only, never shipped). Until then the suite
runs where it is fetched. ffmpeg's `ebur128` (LD6) stays optional, Linux, behind
`ADI_REQUIRE_ORACLES`; not done here.

## 2026-11-19 — DEV-29 (c): the true-peak brickwall in `LookaheadLimiter`

**`devices/limiter-true-peak` (`d9f8e4d0`), stacked on `devices/true-peak`** (it uses the meter's
interpolator).

- `setTruePeak(bool)`, **off by default**: the detector is `TruePeakMeter`'s own interpolator
  (now `TruePeakMeter::design`/`factorFor`, so a limited signal reads at the ceiling on the
  meter), upsample-and-hold, and each audio sample's target is the ceiling over the largest
  interpolated value within 2 samples of it. Latency = lookahead + `truePeakDelay()` (8 + 2 at
  every oversampled rate, 0 from 176.4 kHz where the Annex oversamples by 1), reported, whole
  samples. `process()` for double as well as float; the delay line is double either way.
- Measured, on the quarter-rate sine at 45° (+6 dBTP), 11 kHz + 997 Hz, hard-gated quarter-rate
  bursts, +12 dB noise and +12 dB doublets, at 44.1/48/96 kHz: every sample at or under the
  ceiling exactly; true peak over the ceiling under 0.001 dB with a lookahead of 64 or more
  (worst 0.0003, noise) and under 0.05 dB with none (worst 0.025, noise). Without the 2-sample
  spread the noise residue is 0.28 dB, so the spread stays. The old sample-peak mode leaves
  +2.96 dB on the quarter-rate sine, asserted.
- Below the ceiling in true peak: the input, exactly `latencySamples()` late, bit for bit, float
  and double. Plant: detection back to sample peak -> 47 failures. DSP **262** (was 151); CTest
  **243/243**.

**PR requested** — title "A true-peak brickwall in LookaheadLimiter, off by default (DEV-29 c,
EX-05 LD5)"; head `devices/limiter-true-peak` at `d9f8e4d0`; base `devices/true-peak` until the
stack lands, then `main`. Body: the commit message.

**For the engine (DEV-29 c's other half):** the refine loop (≤ 4 trials, ±3 dB steps, 0.02 LU
plateau) inside `NormalizationCapture`, with the encoded true-peak check after dither as the final
gate, is yours; turn the brickwall on with `setTruePeak(true)` and report `latencySamples()`.

## 2026-11-19 — DEV-14, step 1: a patch's MIDI output comes back on its frame; the host's reset bang

**`devices/pd-midi-out` (`b046391d`), stacked on `devices/pd-block-quantum`.**

- libpd's per-instance MIDI-out hooks fill a fixed array inside the locked tick; each message is
  stamped with where its tick's audio comes out plus Pd's logical time within the tick, and
  `process()` hands out those due in its segment (`LibPdEngine::midiOut()`, block-relative
  frames). MIDI 1 by construction. Overflow (512 a call) is flagged for the host to release
  everything, and counted.
- `hostReset()`: a bang to `$0-adi-reset` before the next tick; `PdDevice::prepare` sends it;
  `PdDevice::hostReset()` is there for stop, bypass and panic.
- Tests (AR9's timing half): `[notein] -> [+ 12] -> [noteout]` at the input's frames; 64 later
  through the one-block adapter; `[pipe 101]` exactly +4,848, 48 samples into a tick; 600 at once
  -> 512 back, flagged, 88 counted; the reset arrives. Plant: no logical offset fails the pipe
  case. Pd engine **648**; CTest **242/242**.

**PR requested** — title "A patch's MIDI output comes back on its frame; the host's reset bang
(DEV-14 step 1)"; head `devices/pd-midi-out` at `b046391d`; base `devices/pd-block-quantum` until
the stack lands, then `main`. Body: the commit message.

**What DEV-14 still needs from others:** win's ADR (R4, the row says one comes first: kinds,
Consume, `[clone]` voices with the MPE member channel as the voice index, precision); the engine's
DEV-17 (decode `midiOut()` under the device's note prefix, unique ids) and DEV-18 (MIDI-effect
patches in the transform phase); the engine calling `PdDevice::hostReset()` on stop, bypass and
panic. AR9's unique-id half and AR10 (voices, Consume) wait for those.

**Next:** DEV-10 and DEV-15 both wait on the engine and on ADRs (DEV-15: "an ADR answers the gap
review's eight decisions before any Grid-style work"). So queue item 8, this lane's other rows in
milestone order; first the ones that need nobody else.

## 2026-11-19 — DEV-13: user patches add no delay on the 64-sample grid, one reported block otherwise

**`devices/pd-block-quantum` (`83b40061`), stacked on `devices/pd-double` (the DEV-09 stack).**

- `LibPdEngine::BlockMode::Automatic`, to the director's Q-K2 answer: whole Pd ticks with no
  added delay while the buffer (at prepare) and every segment are multiples of 64, each event at
  the start of its tick; a buffer that is not a multiple runs one reported, compensated block;
  a segment inside a tick arriving later falls back to that block once (counted; the adapter's
  latency epoch moves, and `PdDevice::latencyEpoch` now includes it, so the graph re-compensates;
  never silenced). `segmentQuantum()`: 64 for Pd in WholeBlocks/Automatic, 1 otherwise.
- Tests: the segment contract (buffers 64/128/256, events at 1, 63, 64, 65, 127, kept on the
  grid) is an exact null with no delay; 511 is the input 64 late from the start; AR5's 1 + 255 on
  the grid nulls, inside the tick falls back once and runs continuous at 64. Plant: Automatic as
  Buffered fails all four. Pd engine **641**; CTest **242/242**.
- The research's own pd-segment-contract specification (tests/research_intake) is not on main
  yet; when engine-win's PR lands, its numbers become this test's data.

**PR requested** — title "User patches add no delay on the 64-sample grid, one reported block
otherwise (DEV-13, Q-K2)"; head `devices/pd-block-quantum` at `83b40061`; base `devices/pd-double`
until the DEV-09 stack lands, then `main`. Body: the commit message.

**Request to the engine (DEV-13's other half, the graph and `pd_patch_host`):** (1) when a
device's `segmentQuantum()` is above 1 (`PdDevice::segmentQuantum()`; a `DeviceInstance` virtual
is the engine's call), keep that node's block split only at multiples of it, and hand each
segment the events falling inside it (they reach Pd at the tick's start, as now); (2) construct
user patches with `BlockMode::Automatic`. Until then a user patch in Automatic still never goes
silent: the first off-grid split costs one counted fallback to the reported block.

**Next:** DEV-14 (Pd note output, voices and instrument kinds).

## 2026-11-19 — DEV-28, step 2: true peak on every channel, from double too; DF-E20 (d) closed

**`devices/true-peak` (`b5ee5aa6`), stacked on `devices/loudness-meter`.**

- `TruePeakMeter::prepare(rate, channels)`: any channel count (default 8, as before); the
  loudness meter's true peak now covers every channel. A double-input path, reading exactly
  what the float path reads for float-exact input.
- **DF-E20 (d), both halves, in this lane's files** (the register names the engine seat; the
  files are `src/adi/dsp`): the oversampling factor is now the smallest whole number reaching
  176.4 kHz (6× at 32 kHz, 8× at 22.05 kHz; 44.1-192 kHz unchanged), so `true_peak.hpp`'s
  promise holds and is tested; `spectrum.hpp` now says a full-scale sine reads 0.00 when centred
  on a bin and less between bins by the window's scalloping loss. **win:** DF-E20 (d) can move to
  Done with this PR.
- Auto Gain Stage: `dsp::AutoGain` is ADR-0166's per-effect auto gain (a 1 s window by design),
  not an integrated reading, and stays. The Auto Gain Stage's LUFS mode is
  `src/adi/engine/gain_stage.cpp` (engine): **request to the engine** to read it from
  `dsp::LoudnessMeter` with N13 and export normalization.
- dsp **162**, loudness **62**; plant (the old factor rule) fails the new checks; CTest **243/243**.

**PR requested** — title "True peak on any channel count, from double too, and at least
176.4 kHz at every rate (DEV-28, DF-E20 d)"; head `devices/true-peak` at `b5ee5aa6`; base
`devices/loudness-meter` until it lands, then `main`. Body: the commit message.

**Next:** queue item 7, the Pd tier for the Grid: DEV-13 (a patch's delay when blocks do not
divide by 64), on the DEV-09 stack.

## 2026-11-19 — DEV-28, step 1: the one loudness meter, proved against Tech 3341/3342 and a brute-force reference

**`devices/loudness-meter` (`a3d4dfac`), from `origin/main`.** New: `src/adi/dsp/loudness.*`,
`cmake/devices/loudness.cmake`, `tests/test_loudness.cpp`.

- **The meter** follows the row's contract: K-weighting (AutoGain's BS.1770 prototypes at any
  rate); 10 ms sub-blocks (boundaries at round(k·rate/100)); M over 40, S over 300, both and
  their maxima every sub-block, -inf until filled; a 400 ms gating block and a 10 Hz
  short-term value every 100 ms above -70 LUFS; histograms as **Fenwick trees** over 0.01 LU
  bins (-70..+20 LUFS) with counts and exact energy sums, so I and LRA are recomputed every
  100 ms in O(log n) and nothing walks the bins on the audio thread; LRA by Tech 3342's MATLAB
  rule; run and pause (no window holding paused audio feeds I, LRA or the maxima after
  resuming); true peak via `TruePeakMeter` (≤ 8 channels); float and double input; explicit
  channel weights.
- **Proof (62 checks):** EBU Tech 3341 (2023) cases 1-6 (48 kHz; 1 and 5 also at 44.1 and
  96), 9, 12, 10, 13, 11, 14, 15-23 (20-23 synthesised at 4 fs, low-passed, decimated at
  offsets 0-3); Tech 3342 (2023) cases 1-4 (48 kHz; 1 also at 44.1 and 96). Cases 7, 8 (3341)
  and 5, 6 (3342) are the EBU's programme files and are not used. A brute-force reference in
  the test (every gating block, both gates, sorting) agrees within 0.02 LU for I and LRA.
  Block sizes 1 to 4096 give bit-identical readings; process/readings/reset allocate nothing.
  Plants: no relative gate fails 3341 cases 3 and 4; a 5th percentile fails 3342 case 4.
- The test takes 56 s alone in MSVC Debug (74 s in the parallel run); most of it is the
  100-second cases and the reference. MSVC `/W4 /WX` clean; CTest **243/243**.
- The tables were read from the EBU's own Tech 3341 v4 and 3342 v4 PDFs (public); nothing of
  them is in the repository beyond the case numbers and levels the test synthesises.

**PR requested** — title "The one loudness meter, proved against EBU Tech 3341/3342 and a
brute-force reference (DEV-28 step 1)"; head `devices/loudness-meter` at `a3d4dfac`; base
`main`. Body: the commit message.

**Requests (DEV-28's other halves; not this lane's files):**
- **win:** pin **libebur128** in `tools/fetch_external.sh` (test-only, MIT) and its
  EXTERNAL-CODE row, so the meter can be checked against it at 44.1/48/96 kHz (LD3). Its LRA
  (1 Hz hop) is not used as an oracle, per the row.
- **engine:** move N13 (`src/adi/engine/main_loudness.*`, the Main meter) and export
  normalization onto `dsp::LoudnessMeter` (one integrated-LUFS implementation, XC-03). Its
  `readings()` is a plain snapshot: read it on the thread that processes, or publish it.
- This lane next: Auto Gain Stage's LUFS mode onto the meter (`src/adi/dsp/auto_gain.*`,
  ours), and the true-peak meter's double path and more than 8 channels.

## 2026-11-19 — DEV-22: what the research's default-preset corpus says next (no branch)

The research's DV-09 corpus (16 of Live 12.4's effects at their browser defaults, compensated
offline renders of a sweep, an impulse of 0.5 / -0.25 and six tones at 71, 223, 997, 3001,
7001 and 11003 Hz; analysed in the director's research on 14 November) gives acceptance numbers
beyond the latency table. Read privately; the statements:

- **Saturator** (High Quality on, Analog Clip, drive 0): the compensated impulse response is
  causal from 3 samples before the peak (so Live's reported 3 is its true peak delay), peak
  0.569 of the input, 99 % of the energy within 25 samples after the peak, above 1e-8 for 169
  samples after it, DC gain 1.0000. ADI's new minimum-phase filter (`devices/saturator-
  latency`): causal, peak 4 samples in (one later than Live), 0.669 of the input, energy99 at
  +7 after its peak, tail 25 samples. Live's resampler is sharper and longer (an IIR, by its
  tail). **Next step for Saturator:** fit the High Quality filter to that response (peak at 3,
  0.569, energy99 +25, tail ~170) and add those four numbers to its test.
- **Redux** (browser default): a real 64-sample delay (the compensated impulse peaks at 0,
  0.760 of the input, 99 % of the energy within 3 samples), DC gain ~0, and on the six tones
  gain -0.022 / -0.003 / -0.009 / -0.081 / -0.445 / -1.115 dB with phase +4.24 / +1.95 / +3.28 /
  +9.10 / +21.04 / +33.03 degrees: a ~5.3 Hz first-order DC blocker plus a ~0.4-sample advance
  against the 64 and a gentle top-end roll-off; nothing above -124 dBFS outside the tones.
  ADI's Redux default (48 kHz rate, 16 bits, no filters, no delay) is far from that. Live
  ships no Redux default preset, and no research set holds one; the ranges are readable from
  Live's own Redux presets. **Reading to take:** Redux's default control values (one look at
  a fresh Redux in Live 12.4). Then one branch: Live's ranges, defaults, the 64-sample path,
  and the six-tone numbers as the test.
- Overdrive and Saturator above, and Auto Filter, Chorus-Ensemble, Delay, Echo, Hybrid Reverb,
  Multiband Dynamics, Phaser-Flanger, Resonators, Reverb and Vocoder each have the same three
  default renders in the corpus; EQ Eight and Utility null exactly against dry. These are the
  first parity numbers for each built device, one branch each, in the manual's order.

**Next:** queue item 6, DEV-28, the one loudness meter (M4's critical path); DEV-22 resumes
after it, device by device, from the corpus above.

## 2026-11-19 — DEV-22: Overdrive shapes at 4× and reports Live's 5 samples

**`devices/overdrive-latency` (`7d89a70c`), from `origin/main`.**

- **Reading:** DV-08, Overdrive's default preset: "Latency: 5 samples (0.104 ms)", the same
  switched off. Live ships no default-preset file for Overdrive, so its ranges stay a
  reading to take.
- **ADI before:** the OJD shaper at the base rate (aliasing), reporting 0.
- **ADI now:** the shaper at 4× (polyphase interpolation, decimation at phase 0) through a
  185-tap minimum-phase filter: flat to 20 kHz, ≥ 61 dB down past 0.135 of the 4× rate,
  5.02 samples of low-frequency delay. Reports 5; the dry path waits 5. A driven 7 kHz tone's
  folded fifth harmonic: -25 dB before, -83 dB now (plant: base-rate shaping fails the check).
  Overdrive **21** checks, Overdrive Pd **17**, native/Pd parity unchanged; CTest **242/242**.

**PR requested** — title "Overdrive shapes at 4x and reports Live 12.4's 5 samples (DEV-22)";
head `devices/overdrive-latency` at `7d89a70c`; base `main`. Body: the commit message.

## 2026-11-19 — DEV-22: Saturator reports Live's 3 samples; defaults as Live's preset

**`devices/saturator-latency` (`870c0e2f`), from `origin/main`.**

- **Readings:** the latency table (DV-08: Status Bar text of each default preset, 48 kHz):
  Saturator "Latency: 3 samples (0.062 ms)", the same when switched off. Live 12.4's shipped
  default Saturator preset, read from its file: Oversampling (High Quality) on, Color on,
  Bass Shaper threshold -50 dB, Waveshaper Drive 1 (100 %); PreDrive -36..36, BaseDrive (Amt
  Lo) -36..36, ColorDepth (Amt Hi) -24..24, ColorFrequency 30..18500 Hz at 1000, ColorWidth
  0..1 at 0.3, PostDrive (Output) -36..0, Dry/Wet 1; Waveshaper Lin 0.5, Curve 0.05, Damp 0,
  Period 0, Depth 0 (all 0..1).
- **ADI before:** High Quality off by default; with it, two 65-tap linear-phase passes at 4×,
  16 samples delayed and reported (more delay than Live, against ADR-0203 d4).
- **ADI now:** the same filter made minimum phase (same magnitude within 1e-6 through the
  passband; DC group delay 3.56 samples for both passes), reporting Live's 3, dry delayed 3;
  the 0.56-sample remainder is written in the code. Alias suppression unchanged. Defaults:
  High Quality on, Color on, Bass Shaper -50 dB, Waveshaper Drive 100 %. Ranges matched where
  the control means the same: Amt Lo ±36 dB, Frequency 30..18500 Hz, Output -36..0 dB.
- **Left for a reading:** Width, Curve and Period are on Live's 0..1 scales (defaults 0.3,
  0.05, 0); ADI's are octaves and its own units. Mapping them needs a render each.
- Saturator **37**, Saturator Pd **30**; native/Pd parity unchanged; MSVC `/W4 /WX` clean;
  CTest **242/242**. Plant: reporting 16 fails both latency checks.
- **Merge note for win:** `tests/test_saturator_pd.cpp` is also touched by DEV-09 step 3
  (`devices/pd-audio-thread` adds a `block()` before reading the HQ latency). Whichever lands
  second takes both: the `block()` line and `== 3`.

**PR requested** — title "Saturator reports Live 12.4's 3 samples, defaults as Live's preset
(DEV-22)"; head `devices/saturator-latency` at `870c0e2f`; base `main`. Body: the commit message.

**Next:** Overdrive (0 against Live's 5), then Redux (0 against 64), then the Limiter's lookahead
against 64 / 128 / 256.

## 2026-11-19 — DEV-22: Shifter's Pitch mode as Live 12.4 measures it

**`devices/shifter-latency` (`5bd1ec48`), from `origin/main`.** Files: `src/adi/dsp/shifter.*`,
`pd/devices/Shifter.pd`, the two Shifter tests.

- **The reading (DV-06, relayed in RESEARCH-INTAKE 6.2; private notes read again):**
  Shifter's default preset, Pitch, Dry/Wet 100 %, a 0.5 impulse, 48 kHz offline export.
  Status Bar latency 0 at Window 10, 80 and 350 ms; at 0 st the output is the impulse
  alone, 240 / 1920 / 8400 samples late (half the Window), compensation on or off; Window
  10..350 ms, default 80. At +7 st: onset +16 / +187 / +848, peak +337 (0.196) / +1469
  (0.329) / +6454 (0.423), energy centre +263.5 / +1894.5 / +7043.4.
- **ADI before:** a phase vocoder on a power-of-two window, the whole window reported as
  latency, Window 5..170 ms at 40.
- **ADI now:** a rotating-tap delay line (two taps sweeping 0..Window half a cycle apart,
  sin² crossfades summing to one, resting at unison with one tap at Window/2). Unison is
  Live's exactly: a clean copy 240 / 1920 / 8400 samples late; `latency()` 0; Window
  10..350 ms at 80. Pitch accuracy under 0.04 cents from -12 to +12 st.
- **Away from unison:** with a free sweep phase the same model put Live's +7 st onset and
  peak within 1..5 samples at 10 and 80 ms (peak values 0.204 and 0.32 against 0.196 and
  0.329). Live's phase reference is not in the export, so ADI's +7 st numbers are printed
  by the test (onset +54 / +695 / +855 with ADI's rest phase), not claimed.
- Plant: another rest phase fails all three unison checks. MSVC `/W4 /WX` clean; CTest
  **242/242**.
- **Readings still to take (one question each):** Dry/Wet below 100 % (is Live's dry path
  delayed with the wet? ADI's is not); the sweep's phase reference (to claim the shape at
  +7 st).

**PR requested** — title "Shifter's Pitch mode as Live 12.4 measures it: no reported
latency, half a Window late (DEV-22)"; head `devices/shifter-latency` at `5bd1ec48`; base
`main`. Body: the commit message. A question on it is under FOR THE DIRECTOR.

**The rest of RESEARCH-INTAKE 6.2's latency table against what is built:** EQ Eight HQ
16 = Live's 16. **Saturator** reports 16 with High Quality, Live 3: ADI adds more delay
than Live (against ADR-0203 d4), next. **Overdrive** 0 against Live's 5, **Redux** 0
against 64, **Limiter** lookahead to check against 64 / 128 / 256 (+6 with True Peak):
each its own branch after. Compressor, Glue, Gate, Corpus, Erosion, Vinyl, Spectral Time
and Spectral Resonator are not built; their numbers wait for them.

## 2026-11-19 — DEV-21, step 1: OneShot's controls take Live 12.4's ranges, units and defaults

**`devices/oneshot-live-params` (`95a4fe0b`), stacked on `devices/sampler`** (which stands
on #278).

**The reading.** Live 12.4 (build of 24 April 2026) ships a default Simpler preset in its
Core Library; read from its files on this machine, no Live session run. The values, as
statements: Volume -36..36 dB, default -12; Attack 0.1 ms..20 s, 0.1 ms; Decay 1 ms..60 s,
600 ms; Sustain a level 0.000316..1 (shown -70..0 dB), 1 (0 dB); Release 1 ms..60 s, 50 ms;
One-shot Fade In and Fade Out 0..2000 ms, 0.1 ms each; Loop on, Snap on, Retrigger on;
Filter on, Low-pass, 24 dB, 30 Hz..22 kHz at 22 kHz, Resonance 0..1.25 at 0, Drive 0..24 dB;
LFO on, sine, 0.01..30 Hz at 1 Hz, free-running; Vel > Vol 0.35; Transpose ±48 st; Detune
±50 cents; Pan, Spread 0..100 %, Glide (off, 50 ms), pitch-bend range 5 (MPE 48); Voices
the sixth entry of its chooser. The file stays private, the numbers are in the test.

**What changed.** OneShot's matching controls now carry those ranges, units and defaults;
Vel > Vol is new (35 %). The canvas declares the same and a test pins all 28 values.
The oracles test mechanics, so they switch off what they do not test and model Live's
shortest attack and release where they compare samples (an exact 0.1 ms ramp, spectral
windows after it, one more block where a 1 ms release overlaps). Sampler keeps Snap off per
zone and exposes Vel > Vol. OneShot **295**, OneShot Pd **38**, Sampler **31**; MSVC `/W4 /WX`
clean; CTest **245/245**. Plant: the old Volume range fails the Live check and the dB oracle.

**Not claimed (the laws are still ADI's):** linear envelope segments (Live's have slopes,
default decay and release slope 1), the Vel > Vol blend, Resonance-to-Q, the filter circuits
and Morph, Pan, Spread, Glide, Drive (not built). **Readings still to take from Live 12.4,
one question each, before the sound is claimed:** the Voices chooser's list; the velocity
law at 35 %; the envelope slope law; the Clean filter's response at 22 kHz and at a
resonance. Each needs a render from Live; they are this lane's next use of Live and will be
announced here first.

**PR requested** — title "OneShot's controls take Live 12.4's ranges, units and defaults
(DEV-21 step 1)"; head `devices/oneshot-live-params` at `95a4fe0b`; base `devices/sampler`
until #278 and the Sampler PR land, then `main`. Body: the commit message.

**Requests to the engine (DEV-21's other half; LANES: `src/juce/*host*` and the catalogue
are not this lane's).** (1) `builtin_catalog`: entries for `adi.oneshot` and `adi.sampler`
with kind `instrument` (`EventFlow::Consume`), available. (2) `makeBuiltinDevice`: native
dispatch of both to `device::OneShot` / `device::Sampler` (no Pd adapter, ADR-0203 d4).
(3) Their samples through #414's slot ops (the `sample:1` role). The device side is ready
on the stack above; tests inject the loader meanwhile.

**Next:** DEV-19 (item 4) waits on the engine's DEV-17/DEV-18 (see #207 above), so item 5,
DEV-22: first the Shifter's latency against the reading (Live reports 0 and delays by half
its window), then the latency table of RESEARCH-INTAKE 6.2 across the built effects.

## 2026-11-19 — DEV-09, step 5: the Pd tier built with `PD_FLOATSIZE=64`, every suite passing (AR17); DEV-09 complete

**`devices/pd-double` (`d4568d8e`), stacked on `devices/pd-hash-a`.**

- `ADI_PD_FLOATSIZE_64` (off by default, in `pd_loader_fix.cmake`) puts
  `PD_FLOATSIZE=64` PUBLIC on `libpd_static`, so everything that includes Pd agrees on
  the layout.
- **Built that way, 19 of 242 suites failed.** Every ADI external handed Pd's signal
  vectors to its float core as `float*`, which at 64 are doubles. `FloatSignals`
  (`src/adi/pd_builtins/float_signals.hpp`) gives an external float views of its
  signals, converting around its perform routine only when `t_sample` is double (at 32 a
  view is the signal itself and nothing is added); applied to all 18 externals, their
  perform routines untouched. Plus one clamp of a `t_floatarg` against float literals.
- The loader tests now also plant every name Pd itself tries: a 64-bit Pd looks for
  deken names (`.windows-amd64-64.dll`) and never a bare `.dll`.
- Native against real-Pd renders: at 64, AutoFilter and EQ Eight differ by **2^-25**
  (one float ulp at 0.5; the canvas computes a parameter in double before the float core
  rounds it). At 64 one float ulp at full scale is allowed; at 32 the check stays
  bit-exact.
- Result: **both configurations, MSVC `/W4 /WX` clean, CTest 242/242.** A new check pins
  `sizeof(t_float)`/`sizeof(t_sample)` to `PD_FLOATSIZE`. Pd devices keep
  `processesDouble()` false, which stays true of them (the engine adapter hands Pd float
  buffers); a double path end to end would be its own change.
- When #278 lands, OneShot's external (`oneshot_tilde.cpp`) takes `FloatSignals` the same
  way; it is on DEV-21's list.

**PR requested** — title "Pd: a PD_FLOATSIZE=64 build of the Pd tier, every suite passing
in it (DEV-09 step 5, AR17)"; head `devices/pd-double` at `d4568d8e`; base
`devices/pd-hash-a` until it lands, then `main`. Body: the commit message.
**Request to win (CI is yours):** a CI leg configuring with `-DADI_PD_FLOATSIZE_64=ON`
(one Linux leg would do), so the 64-bit Pd tier stays green.

**DEV-09 as a whole**, five PRs in one stack, each naming its base:
`devices/pd-teardown` → `pd-fence` → `pd-audio-thread` → `pd-hash-a` → `pd-double`.
Its contract items: native loading closed at the source (step 2); quit, exit, open and
path changes fenced (2); one trylock tick per Pd tick (3); printing through a queue (3);
SPSC sends (3); arrays read without the lock-leaking path (3); the `#A` overlay (4); the
latency binding unbound on close (1); a `PD_FLOATSIZE=64` build (5). Tests AR7, AR8, AR17,
AR18 and #173's 40-cycle stress are in `test_pd_engine.cpp`. Not done, as the row defers
them: `[pd~]` out-of-process patches; a file and network policy for shared patches.
GCC and Clang have not compiled these branches here (no local GCC or Clang on this
machine); CI's legs are the first to.

**Next:** queue item 3, DEV-21, stacked on `devices/sampler`: OneShot's parameters to
Live 12.4's own ranges, units and defaults, read from the default preset Live ships.

## 2026-11-19 — DEV-09, step 4: `#A` unbound when its owner is freed (AR18, PD2)

**`devices/pd-hash-a` (`fcf0cc75`), stacked on `devices/pd-audio-thread`.**

- **Reproduced first.** A file load saves and restores `#A` around itself, so opening a
  whole patch was never the danger. An abstraction made **at run time** binds `#A` when
  it finishes loading (`canvas_popabstraction`) and nothing unbinds it; cleared, it
  leaves `#A` pointing at freed memory, which the next array or `[text define]` free
  dereferences. The test makes a table and an abstraction by dynamic patching in one
  instance, clears the abstraction, and checks `#A` is bound to nothing: it failed before
  the overlay, passes after. (The MSVC debug heap does not trap that read; ASan would.)
- **The fix** is plugdata-pd `bdb30eedee` ("Correct fix for pd bindlist crash (detected
  by ASAN)", Pd's own BSD-3 licence), as an overlay on the pinned `m_pd.c`: `pd_free`
  unbinds whatever it frees from `#A` first. Attribution is in the overlay's comment; win,
  if `docs/EXTERNAL-CODE.md` should name it too, that file is yours.

`adi_pd_engine_tests` **633**; MSVC `/W4 /WX` whole tree clean; CTest **242/242**.

**PR requested** — title "Pd: unbind #A when its owner is freed (DEV-09 step 4, AR18)";
head `devices/pd-hash-a` at `fcf0cc75`; base `devices/pd-audio-thread` until it lands,
then `main`. Body: the commit message.

**Machine note.** To check new code under clang's `-Wconversion` locally I started a
winget install of LLVM; it raised a Windows elevation prompt that nobody could answer,
so I stopped winget. A UAC prompt for LLVM's installer may still be on the desktop: it is
safe to decline. CI's GCC and Clang legs stay the check for those warnings.

**Next:** DEV-09 step 5, the `PD_FLOATSIZE=64` build for the 64-bit mode (AR17).

## 2026-11-19 — DEV-09, step 3: the audio thread never waits for Pd (AR8)

**`devices/pd-audio-thread` (`b86243f3`), stacked on `devices/pd-fence`.** Files: this
lane's `src/juce/pd_engine.*` and the Pd tests.

- **One `sys_trylock` per Pd tick** replaces `libpd_process_float`, which took
  `sys_lock` (blocking) and ran `sys_pollgui` on the audio thread on every tick. A busy
  lock skips the tick (silence, counted as `skippedTicks`). Measured: a message-thread
  holder of the lock for 50 ms; the slowest 64-frame segment stayed under 10 ms and the
  ticks were counted. Plant (waiting for the lock): 61 ms in one segment, nothing skipped.
- **Every delivery inside that lock**, just before the tick: `sendFloat` from the message
  thread through a single-producer ring (its name resolved under the lock), parameters
  latest-value-wins, transport and routed MIDI (kept over a segment that holds no whole
  tick). A parameter therefore reaches Pd in the next locked tick, still before its block
  (ADR-0188 d3). Two device tests read a latency straight after a parameter without
  rendering; they now render one block first (EQ Eight, Saturator).
- **Printing through a queue:** the print hook pushes into a bounded multi-producer queue
  of fixed slots (no lock, no allocation); the message thread joins the pieces. `[print]`
  on a 1 ms `[metro]`: **0** CRT allocations on the audio thread over ~400 lines (MSVC debug
  heap hook). Plant (the old string append): 3.
- **Arrays read without the lock-leaking path:** `libpd_read_array` returns -2 with the
  instance still locked when a table is shorter than asked. A published table that
  shrinks then hung the instance; the plant trips the test's 10 s watchdog. The tick now
  reads under its own trylock with a bounds check; `readArray` always unlocks.
- `gensym` in `bindParameters`/`bindArrays` now under the lock; `InstanceLock` is the
  message-thread holder an attached editor will use (DEV-11).
- Not serviced any more: `sys_pollgui` (sockets, GUI) never runs. `[netreceive]`-style
  objects therefore get no socket polling; the plan defers a file and network policy for
  shared patches (DEV-09 "Reject or defer"), and it is noted here so nobody is surprised.

`adi_pd_engine_tests` **631** checks. MSVC `/W4 /WX` whole tree clean; CTest **242/242**.

**PR requested** — title "Pd: the audio thread never waits for Pd: trylock tick, queued
deliveries, lock-free print (DEV-09 step 3, AR8)"; head `devices/pd-audio-thread` at
`b86243f3`; base `devices/pd-fence` until it lands, then `main`. Body: the commit message.

**Next:** DEV-09 step 4, the `#A` overlay (plugdata-pd `bdb30eed`, BSD-3 like Pd) with a
dangling-binding test, then step 5, the `PD_FLOATSIZE=64` build.

## 2026-11-19 — DEV-09, step 2: native code refused at Pd's own loader; `[; pd quit]` and kin fenced (AR7)

**`devices/pd-fence` (`b3adb315`), stacked on `devices/pd-teardown`** (both change
`pd_loader_fix.cmake`).

- **The bypass was real, and is now measured.** The engine's guards checked the patch
  folder, the search paths and `[declare]`. A patch naming an object by absolute path,
  by `../` path, or creating one from a message box at run time reached Pd's loader with
  none of them, and Pd called `LoadLibrary` on the planted file. Now Pd's one native
  loader (`sys_do_load_lib_from_file`) refuses every library and names it on the
  console ("ADI refused native code: <dir>/<file>"); the external-scheduler path too.
- **`[; pd quit 7]` ended the process with code 7** (the plant shows it: the test binary
  exits 7 at that section). `quit`, `exit`, `verifyquit`, `open`, `menunew`,
  `add-to-path`, `path-dialog`, `startup-dialog`, load/save/forget preferences, the
  audio and MIDI device dialogs and `fast-forward` are each one refusal naming the
  message. ADI opens patches with `libpd_openfile`, never `open`.
- Tests (AR7): 19 new checks; `adi_pd_engine_tests` **620**. Plants: without the loader
  refusal 6 checks fail (each of the three ways reached `LoadLibrary`); without the
  `quit` fence the binary exits 7. Restored: 0 failures.

MSVC `/W4 /WX`, the whole tree: clean. CTest **242 of 242**. (This build tree now links
with `/INCREMENTAL:NO`: MSVC 19.44's incremental linker failed with LNK1000 on a relink;
a local setting, nothing in the repository.)

**PR requested** — title "Pd: native code refused at Pd's loader; [; pd quit/exit/open/
paths] fenced (DEV-09 step 2, AR7)"; head `devices/pd-fence` at `b3adb315`; base
`devices/pd-teardown` (the step-1 PR) until it lands, then `main`. Body: the commit
message.

**Next:** DEV-09 step 3, the audio thread: one `sys_trylock` per Pd tick (skip and count,
never wait), every delivery inside it, no `sys_pollgui` there, the print hook through a
queue, arrays read without libpd's lock-leaking path (AR8).

## 2026-11-19 — DEV-09, step 1: a closed Pd engine frees everything it allocated

**`devices/pd-teardown` (`a7a95c58`), from `origin/main` `ae09f638`.** Files: this
lane's `src/juce/pd_engine.*`, `src/juce/pd_loader_fix.cmake`, `tests/test_pd_engine.cpp`.

- #173's unbind re-applied (MF4): `libpd_bind` returns the receive object; it is now
  kept and `libpd_unbind`ed while the instance is alive; the transport symbol is
  dropped with the instance.
- #173's forty-cycle stress is back, with a measurement: under the MSVC debug heap the
  live block count across 40 cycles of two engines (the printing one closed first)
  must not grow. It found more than #173 knew. After the unbind, **36 blocks per cycle**
  were still left, traced by allocation number and call stack to pinned Pd itself:
  `pdinstance_free` frees neither the `t_pdinstance` (392 bytes), nor the names of its
  twelve built-in symbols, nor its two 512-byte sound buffers, nor its path list; and
  `[inlet~]`/`[outlet~]` never free their reblocker array (72 bytes each, for every
  signal inlet and outlet of every patch or abstraction closed). Each is freed by an
  overlay in `pd_loader_fix.cmake`; the multi-line needles are matched after CRLF is
  folded to LF, since a Windows checkout of the pinned tree is CRLF.
- Result: **0** blocks over the 40 cycles. Plant: skipping only the unbind gives
  **80** (one per engine) and fails the check; restored, it passes.

MSVC 19.44 Debug `/W4 /WX`, the whole tree: clean. CTest **242 of 242** at `-j 6`;
`adi_pd_engine_tests` 601 checks. The leak count is measured on MSVC only (the debug
heap); elsewhere the cycles run as a stress.

**PR requested** — title "Pd: a closed engine frees all it allocated (DEV-09 step 1,
MF4)"; head `devices/pd-teardown` at `a7a95c58`; base `main`. Body: the commit
message, and "DEV-09 step 1 of 5; the native-loading and global-message fence, the
trylock tick with its print queue, the #A overlay and the 64-bit build follow as
their own PRs. Files taken under LANES §1 (the devices lane's `src/juce/pd_*`)."

**Next:** DEV-09 step 2, the native loader refused at the source and `[; pd quit /
exit / open / paths / preferences]` refused (AR7).

## 2026-11-19 — Queue item 1: #278 brought to main, #208 ported onto it, #207 not landable

**#278 (`devices/oneshot`, now `3892833a`).** `origin/main` (`ae09f638`) merged in
without a conflict. MSVC 19.44 Debug, `ADI_WERROR=ON` (/W4 /WX), the whole tree:
clean. CTest: 243 of 244 suites pass at `-j 8`; `adi_oneshot_tests` **197** and
`adi_oneshot_pd_tests` **38**, as on 28 October. The one failure was
`adi_scan_process_tests` ("timeout terminates child descendants...") under load; it
passes alone (3.5 s) and is the scanner's, not this branch's. The four validators
pass. Its draft gates stand as written on 28 October (Live parity, instrument
dispatch, the Pd instrument's Consume metadata); those are DEV-21's, item 3.

**PR requested** — mark #278 ready for review and land it: head `devices/oneshot`
at `3892833a`, base `main`. Body unchanged, plus: "Brought to main `ae09f638` on
19 November; MSVC /W4 /WX clean; 197 + 38 checks."

**#208 → `devices/sampler` (`a5500cc3`), stacked on `devices/oneshot`.** #208's
Sampler, ported onto #278 instead of the stale #207 stack (PLAN MF2). Changes from
#208: no Session hook (internal-instrument dispatch is DEV-21's with the engine;
the test injects the loader as OneShot's does); no `EventOwner` (a #207 type not on
main); prepare idempotent (ADR-0090 d5), `release()` makes the next one full; stepped
controls store whole steps; the check that failed on Apple clang in #208's CI
("modulation preserves base value", an exact `== 0` on `-90 + n*102`, which a fused
multiply-add does not give) now compares within 1e-9. `adi_sampler_tests` **31**
checks (#208's 28 and three new), OneShot's 197 unchanged. Plants: re-preparing at
the same format and unrounded steps fail the two new checks (2 of 31); restored, 0.

**PR requested** — title "Sampler on OneShot's voices, ported from #208 (DEV-21)";
head `devices/sampler` at `a5500cc3`; base `devices/oneshot` (#278) until #278
lands, then `main`. Body: the commit message. Then close #208 as superseded by it.

**#207 (`codex/midi-effects`) should not land, and nothing of it can land from this
lane.** Measured against main: a trial merge conflicts in 30-odd files, the window's
UI among them (its base is the stale 30 September stack). Its fourteen kernels compile
only against its own engine changes: `EventType::Control = 5` (main has
`Controller = 5`), `MappedValue`/`MappedMod`, `Event::owner` with `EventOwner` on every
`Node`, and `transformsEvents()`/`transformEvents()` added to `DeviceInstance` and run
from `Graph`. The plan rejects exactly those (DEV-18: "#207's EventType::Control = 5
and its per-event EventOwner*"; DEV-17: "#207's nextId * 1024") and gives the
transform phase and note identity to the engine (DEV-18, DEV-17). So the kernels are
ported (DEV-19, MF1: from `7f421f4d` with its listed fixes) once the engine's
transform phase is on main, onto that interface; this lane designs no second
transform path meanwhile. **Request to win:** close #207 as "to be ported (DEV-19 on
DEV-17/DEV-18)", keeping the branch; LANES §4b item 4 already says so.

**Queue:** item 2, DEV-09, next. Its first step (the latency binding unbound on close,
MF4) is on `devices/pd-unbind`, building.

## 2026-11-19 — The lane is taken up again (ADR-0208; ADR-0246 d5, in #641)

A Claude work seat in the director's research (Windows, MSVC 19.44,
Live 12.4 installed) now holds this lane. Its own clone and worktrees are apart
from the research folders and from the engine-win seat. Branches `devices/<name>`
from `origin/main`; this log stays on `devices/log`, newest first.

**The loan ends with this entry** (LANES §1, XC-46): `src/adi/dsp/loudness` and
`src/adi/dsp/limiter` come back to this lane. Nothing was built under the loan
(`origin/main` at `ae09f638` has no `src/adi/dsp/loudness`; `engine/main-loudness`
is the engine's own Main meter wiring and is left to it).

**Queue, in the order of the prompt of 19 November:** (1) the three unlanded
PRs #278, #207, #208; (2) DEV-09; (3) DEV-21; (4) DEV-19; (5) DEV-22; (6) DEV-28
with MX-27; (7) DEV-13, DEV-14, DEV-10, DEV-15; (8) the other rows of this lane
in milestone order.

**Half done, from 28 October:** #278 (draft, `devices/oneshot`); the Compressor's
Live 11.2.7 probes (46 tracks, the director's Mac, private). Taking #278 now.

**Live:** not taken today. When this seat takes Live it says so here first and
checks `collab/engine-win.md` on `engine-win/log`.

## 2026-10-28 — OneShot device recovery open as draft #278

**[PR #278](https://github.com/arieladi/Adi/pull/278)**, `devices/oneshot`,
commit `0161d33`, is based directly on main `a57d47a`. Ten owned files recover
#206's native sampler and #207's independent ParamMod behavior, then add a
compiled-in Pd external, forty-control canvas and immutable prepared-sample
bridge. No engine, host, UI, plugin, workflow or global CMake files changed.
No merge performed.

Sample copying/analysis happens off-thread. Processing borrows prepared data
under the existing sample-slot read scope, with no reference ownership changes;
replacement/clear generations reset voices. The Pd MIDI fallback has a fixed
512-event queue, counted overflow with voice reset, and a repeated-key policy
that releases the previous channel/key identity before replacing it. This
prevents an orphaned loop with Retrigger disabled. Its timing and MIDI 1.0
precision limits remain explicit; the native core uses exact original events.

Validation: dependency pins verified with `fetch_external.sh --build-only`;
strict Debug Ninja/Clang build, `ADI_WERROR=ON`, libc++ Debug hardening.
**8,414 checks / 97 suites**, all four validators and spec layout pass.
Affected suites: native **197**, real Pd **38**. Four compiled source plants
(ignored ParamMod, ignored generation change, omitted repeated-key release,
missing prepared sample) fail **8/2/2/6** checks; sources restored before the
final full build and suite. New OneShot targets have no warnings. The unchanged
`adi_tool` target emits the inherited Apple linker duplicate `libadi_core.a`
warning; this is not suppressed or claimed fixed. GCC/MSVC CI is pending.

Remaining draft gates: measured Live parity and inherited feature/default
gaps; production discovery/import and preparation hook; Pd instrument Consume
metadata; native direct dispatch after the adapter base/instrument integration.
The fallback reports its current 64-sample adapter tick plus zero intrinsic
sampler delay. Generic same-rate host prepare does not reset an already running
Pd graph: explicit `$0-oneshot-reset` or a published empty sample is required
until the owner adds lifecycle notification. Native prepare resets voices.
All these limitations are documented in `pd/devices/OneShot.md`.

Worktree: `work/active.nosync/oneshot`. Evidence in `adi_daw/build-strict/`:
`fetch-pins-final.log`, `build-oneshot.log`, `build-final.log`,
`test-all-final.log`, `oneshot-source-plants.json` and per-plant logs.
Generated synthetic test WAVs are retained under `build-strict/test-renders/`.
Queue continues with the Sampler recovery feasibility audit and Compressor
measurements; no action from Adi is pending.

## 2026-10-28 — Compressor static and dynamics batches measured; longer probes next

The original three isolated Sets have now produced **46 measured tracks** on
Live **11.2.7 (2022-12-08_2509d781e7)**. Calibration passes 61 checks, static
passes 97 integrity checks across 15 tracks, and dynamics passes 139 across
22 tracks. Each batch has captured controls and export settings; every raw
reference matches the original stereo float32 input at zero offset. Static
renders are 918,000 frames; calibration/dynamics are 390,000, all at 48 kHz.

The dynamics parameter verifier initially treated Live's `1.00 s` display as
milliseconds. Corrected unit conversion and display precision, rejected 1 ms
and 1.01 s failure plants, and recomputed all 630 checks from the unchanged
original API records into `dynamics-parameters-v2.json`. The failed original
capture is retained with its hash; this was a verifier bug, not a Live access
or settings problem. Both earlier captures still validate.

Independent calibration review confirms the 4:1 hard-knee operating point:
-6 dBFS input becomes -16.5000039 dBFS at threshold -20 dB, about 3.8e-6 dB
from the documented relationship. A candidate Peak/Lin amplitude detector
using the displayed attack/release times matches held-out timing, sine and
stereo probes within 4.70e-7 absolute audio amplitude. This is evidence for a
model candidate, not a shipped Compressor or full parity claim. RMS transients
are not explained by the same detector; transferring it would be incorrect.

Longer probes are required by the actual curves: RMS/Lin 100/1000 ms has not
settled after a one-second high hold, and Log 100/1000 ms has not recovered
after two seconds. A separate extended Set is being prepared with six-second
holds, ten-second nonzero release tails, two high levels and short/long Auto
Release excitation. The existing Sets, manifests, inputs and renders remain
unchanged. Local analysis and provenance: `work/local.nosync/live11-probe/evidence/`.
No action from Adi is pending.

## 2026-10-28 — Live access working; Compressor calibration measured on Live 11.2.7

Live is available and working. The earlier “access unresolved” note described
an automation targeting problem, not a missing Live installation or a request
for Adi to configure it. Binding the app by bundle identifier resolved control.
Preserved the current Set as `untitled_preserved_2026-10-28.als` in its own
Project, then loaded the isolated `compressor_calibration` Set. About Live
confirmed **11.2.7, build 2022-12-08_2509d781e7**. No input from Adi is pending.

Captured all nine tracks through the local parameter bridge and verified every
Compressor control, mixer unity, centered pan, stopped transport, and restored
focus. Exported All Individual Tracks, Return/Master effects off, start 1.1.1,
length 4.0.1 (8.125 s), stereo WAV, 48 kHz, 32 bit, no dither, normalization or
loop rendering. The WAV parser confirms actual IEEE float32 and 390,000 frames.
**61 calibration checks pass**: both no-device references match their inputs
exactly at zero offset; bypass, three unity-ratio lookaheads, dry-only and fixed
+6 dB controls meet their declared tolerances. The active Compressor step has
finite positive output and measured attenuation. This validates the measurement
path, not full Compressor parity or internal lookahead latency (offline PDC).

Local evidence is under `work/local.nosync/live11-probe/`: actual WAVs in
`out/compressor_calibration/`, parameter capture, validation and export/version
provenance in `evidence/calibration-{parameters,validation,export-settings}.json`.
Source/Set/manifest/output hashes are retained. Seven synthetic validator
failure plants were rejected before accepting the actual renders. The earlier
preparation README and manifests intentionally retain their preparation-time
status; these dated evidence records supersede their no-render checkpoint.
Static and dynamics response batches are next. Live 12 manual evidence remains
separate from these Live 11 measurements.

## 2026-10-28 — Reconciled CI and later work; OneShot recovery resumes

Verified the exact current heads through GitHub: **all 19 checks pass** for
[#250](https://github.com/arieladi/Adi/pull/250) (`d730ad5`), draft
[#253](https://github.com/arieladi/Adi/pull/253) (`234a816`), and
[#259](https://github.com/arieladi/Adi/pull/259) (`1f47b21`). This includes
GCC, Clang, MSVC, strict warning/hardening jobs and both JUCE jobs. Main remains
`a57d47a`; none of these changes has been merged.

The preceding checkpoint omitted two later completed changes:

- Hybrid Reverb now uses PFFFT through `RealFft` in `spectrum.*`, with packed
  DC/Nyquist handling, normalized inverse and preallocated aligned scratch.
  Full strict local validation: **8,304 checks / 97 suites**. Affected suites:
  103 FFT, 50 native Hybrid and 11 Pd checks. Four compiled source plants
  (DC/Nyquist, inverse scale, phase, omitted partition) fail 13/7/13/10 checks;
  restoration passes. The sparse `1e-7` oracle remains; dense full-tail tests
  have an explicit amplitude-scaled float-roundoff budget. The Live parity
  gaps remain; the handwritten FFT debt is resolved.
- The existing ADI limiter retained the previous rate's release coefficient
  across prepare. #259 retains seconds and recomputes the coefficient,
  including values set before first prepare. Strict local validation:
  **8,189 checks / 95 suites**; the original behavior and compiled plant fail
  eight new checks. This is not a Live Limiter implementation or parity claim.

Recovery re-audit: #206–#208 production integration prerequisites remain.
However, OneShot's native DSP plus the independent ParamMod additions can be
recovered within this lane; an explicit test loader can prove playback without
editing Session. Work starts on `devices/oneshot` in a fresh
`work/active.nosync/oneshot` worktree. A Pd sample bridge must publish immutable
prepared data, not call allocating sample analysis from perform. Pd instrument
Consume/event-flow metadata and production discovery remain integration gates.

Additional local measurement preparation was also reconciled: Live **11.2.7**
API controls for Compressor, Glue and Gate are in `work/live11-evidence/`,
explicitly not fresh defaults or audio renders. Three Compressor probe Sets
(calibration/static/dynamics), 46 tracks and seven synthetic input WAVs exist
under `work/local.nosync/live11-probe`. The prior MBD Set was saved in that
folder before calibration was opened; the old preparation README predates
those actions. No rendered outputs exist yet. Capture/validation scripts are
being completed, and current Live/export state must be verified before accepting
any measurement. Current Preferences observation: 48 kHz, 512 samples, no input,
MacBook Air Speakers output; no preferences changed in this resumed turn.

Filesystem correction: iCloud has replaced the earlier `work/pd-zero-delay`
and `work/hybrid-reverb` symlinks with duplicate directories. Do not assume
those aliases still identify the validated checkout; current Git heads and
remote CI are authoritative. Preserve duplicates until their state is audited;
new worktrees are created directly inside `work/active.nosync/`.

## 2026-10-27 — Checkpoint: two device PRs open; no merges

- **[#250](https://github.com/arieladi/Adi/pull/250)** — `devices/pd-zero-delay`,
  `d730ad5f628eb50803424b9e48dbec484f87d8bf`: 17 canonical native devices,
  explicit whole-64-sample user-patch mode, Redux alias fix. Strict local
  Clang build, **8,543 checks / 97 suites**, validators and compiled plants
  pass. CI is pending; GCC/MSVC and JUCE matrix results are not yet claimed.
- **Draft [#253](https://github.com/arieladi/Adi/pull/253)** —
  `devices/hybrid-reverb`, `f1562a89d7f4c2dd58b3158579f9954c9dd807db`:
  eight device-owned files recovered independently from #209. Strict local
  Clang build and **8,207 checks / 97 suites** pass. The draft retains the
  documented Live parity gaps and handwritten-FFT debt; it is not a finished
  Live clone. CI is pending.
- **#206/#207/#208** remain blocked at engine/UI ownership boundaries, as
  detailed below. No forbidden stack changes were imported and no PR merged.
- Compressor and Glue Compressor manual audits are recorded below. New Live
  renders/default measurements remain open; Gate is the next device audit.

Resume from these commits. Check CI on the exact PR heads first and fix any
owned failure. For Hybrid Reverb, replace its handwritten FFT with PFFFT via
the existing DSP wrapper boundary, prove transform packing/normalization and
convolution tolerance with failure plants, then rerun the affected checks.
Integrate native dispatch when the adapter base is available on main. Preserve
the remaining adapter remainders and Live measurement gates below.

Local worktrees and their cached builds are under
`work/local.nosync/{pd-zero-delay,hybrid-reverb,devices-log}`, reached through
stable `work/<name>` symlinks. Build directories: `adi_daw/build` for the
adapter and `adi_daw/build-strict` for Hybrid Reverb. Generated WAVs were moved
to `work/local.nosync/test-renders/`; they are synthetic ADI test outputs,
not Live measurements. Mutation source has been restored. The parallel
workers stopped at the workspace credit limit; this is a clean checkpoint,
not a device blocker. Continue the queue when resumed.

## 2026-10-27 — Hybrid Reverb recovery from #209 — full local validation green, Live parity open

Branch `devices/hybrid-reverb`, based on `origin/main` `a57d47a581157fed3256efb8c5bf40102e98645f`; independent worktree. Recovered the eight device-owned files from #209 (`codex/hybrid-reverb` at `7f421f4`): the native convolution/Surge Reverb1 core, compiled-in Pd external, `Hybrid Reverb.pd`, host-side sample preparation helper, CMake module and two test suites. No OneShot/Sampler/MIDI ancestry, engine/graph/host/UI changes, or historical checklist documents were carried. The original #209 remains the provenance and incomplete-scope record; this recovery does not claim completed Live parity.

Reference read: local Ableton Live 12 manual, §28.23, printed/PDF pp.590–596. This is manual evidence, not a Live render measurement. The director's Mac runs Live 11; no output measured from Live 11 or Live 12 was supplied or created here. Existing parameter ranges/defaults/curves remain ADI values unverified against Live. Missing parity includes the five Live algorithm identities, four-band EQ/Pre Algo, Vintage, Bass Mono, independently retained time/sync predelay feedback, IR browser/editor and host-preparation controls on the panel. Existing engineering limits remain 256-sample native convolution latency, 262144-frame IR cap, independent stereo convolution and partition-boundary replacement without a dual-tail crossfade.

Tests use an independent direct time-domain convolution oracle with absolute tolerance `1e-7`, deterministic synthetic inputs, block-size invariance, stereo/routing/tempo checks, allocation auditing, immutable IR publication and float-WAV round trips. Added negative controls proving the convolution oracle rejects silence, bypass, a 10% gain error and a one-sample timing shift. These are synthetic oracle plants, not substitutes for a version-labelled measured Live render gate.

Known architecture debt carried from #209: `hybrid_reverb.cpp` implements a handwritten double FFT, while the current `spectrum.hpp`/dependency policy says PFFFT is the sole FFT and hides its API in the DSP wrapper. This recovery preserves the existing implementation; replacing it through the shared FFT wrapper is still required. The source retains existing notices and reuses the already present Surge adaptation in `live_reverb.cpp`.

Validation completed so far: build-only dependency pins verified in a separate COW copy; Debug Ninja configure with Apple Clang, `ADI_WERROR=ON` and libc++ Debug hardening; `tools/validate_pd.py` passes. The isolated native core also compiled under strict Clang C++20 Debug/O0 hardening and passed **17 checks, 0 failures**, including the four oracle plants. Full strict CMake build passed. `tools/test_all.sh build-strict` passed **8,207 checks across 97 suites**, all validators and the spec layout check; the Hybrid Pd suite passes **11 checks**. `test_all.sh` is the full suite runtime pass; a duplicate CTest runtime pass is not needed. Recovery commit `f1562a8` is on `devices/hybrid-reverb`, draft PR **[#253](https://github.com/arieladi/Adi/pull/253)**. No merge performed. GCC/MSVC results remain pending CI. Canonical native dispatch integration awaits the adapter base (#250).

## 2026-10-27 — Glue Compressor evidence gate and Live probe availability

Read Live 12 manual §28.21, printed pages 586–588. Confirmed ratio-dependent
knee, two-timescale Auto Release, Range limiting the available compression
(0 dB gives no compression), fixed Soft Clip with nominal -0.5 dB maximum,
2× oversampling and the documented possibility of >0 dB peaks when oversampled,
plus external sidechain gain/blend/EQ/listen. The manual does not give the
complete parameter/default table, numerical knee/ballistics or waveshaper.
No Glue Compressor implementation or version-labelled render fixture is on
main. Keep its parity gate open rather than assigning an unmeasured SSL-like
algorithm; Gate is the next missing-device audit.

The existing `collab/mac/live-probe/` kit is the behavioral measurement route:
it generated the earlier Live **11.2.7** Multiband Dynamics evidence. Its default
`~/adi-live-probe` output directory is absent on this machine. Read-only access
to the local AdiVST WebSocket bridge works and currently identifies Multiband
Dynamics. The handshake reports Live version `?`, so it cannot label a new
measurement. No device was loaded or changed, and no render was made. A new
probe must use an isolated Set, verify the actual Live version and export
settings, and preserve the user's existing work. The kit's driver blindly
assumes remembered export settings; inspect those before running it.

## 2026-10-27 — Compressor evidence gate; next missing effect audit

Read the local Live 12 manual §28.9–28.9.2, printed pages 555–559.
Confirmed behavior includes threshold/ratio transfer, unity ratio, Dry/Wet
endpoints, Knee spanning threshold ± the displayed knee amount (the manual's
10 dB knee / -20 dB threshold example spans -30 to -10 dB), Peak/RMS/Expand,
Lin/Log, Auto Release, 0/1/10 ms Lookahead, sidechain gain/blend/EQ/listen and
Makeup being unavailable with an external sidechain. This is Live 12 manual
evidence only, not a Live 11 or Live 12 measurement.

Main has no Compressor core/external/canvas or version-labelled Live render
fixture. Existing `rmsc` is ring-modulation ducking, not Compressor; generic
limiter and multiband dynamics code does not establish Compressor's behavior.
The manual does not quantify all ranges/defaults, detector ballistics, RMS
integration, Log/Auto release, stereo linking or makeup law. Do not substitute
remembered algorithms and call it Live parity. Compressor's measured parity
gate is therefore open under LANES §5; continue with the next device audit
while the adapter and Hybrid Reverb full suites run.

## 2026-10-27 — Zero-delay adapter: full local validation green

PR **[#250](https://github.com/arieladi/Adi/pull/250)**, head `d730ad5`, is open
from `devices/pd-zero-delay`; never merged. CI is pending.

The branch now passes **102 adapter checks**, **260 native/Pd
comparison checks** and **68 Redux checks** under Apple Clang Debug with
`ADI_WERROR=ON` and libc++ Debug hardening. All 17 canonical native renders
match their real-Pd renders exactly, allowing only IEEE `+0`/`-0` equivalence
because Pd's output summing clears signed zero. A one-ulp oracle plant is
rejected. These are ADI-to-ADI comparisons, not Live measurements.

The comparison exposed a real Redux crossed-buffer defect: Pd may recycle the
right inlet vector for the left outlet. Capture both samples before writing
either output. The standalone original-code plant fails the new regression;
max error in the reproducer changes from 0.375 to zero after the fix.

Three compiled adapter source mutations also fail the test suite: restoring a
fixed 64-sample prime (8 failed checks), halving parameter values (6 failed checks), and dropping
the process block offset (4 failed checks). Every mutation was restored; the restored adapter
passes all 102 checks. An earlier plant attempt timed out on filesystem reads
and is not counted as a successful negative control.

The native allocation audit covers processing, tempo/MIDI/sample handoff and
now parameter sends. Static review found no allocating, locking or logging
path after preparation in the 17 cores. The full strict build passed, and `tools/test_all.sh build` passed **8,543
checks across 97 suites**, every validator and the spec layout check.
`tools/validate_pd.py` also passed separately. No GCC/MSVC result is claimed
before CI. The inherited macOS
linker reports a duplicate `libadi_core.a` link from unchanged shared CMake.

Build environment note: files in the new Documents checkout repeatedly
stalled on iCloud-backed reads. Isolated worktrees now live under
`work/local.nosync/`, with stable symlinks at their original paths. Restored
unchanged tracked files from HEAD, preserving all device edits. No user
checkout or other lane's build was changed.

## 2026-10-27 — Recovery audit: OneShot, MIDI effects and Sampler blocked at lane boundaries

Audited PRs #206–#209 against main `a57d47a`. The old stack is actually
#205 → OneShot → MIDI effects → Sampler → Hybrid Reverb; rebasing the entire
stack would import UI and graph work owned by other lanes.

- **#206 OneShot:** its implementation `072c813` modifies Session and UI
  sample-import code in addition to the DSP/device. Main lacks the UI ancestry.
  Native device files can be recovered, but a full working rebase needs the
  window/engine owners' integration. No Pd patch/external exists in this PR.
- **#207 MIDI effects:** `c130f0e` requires event ownership, mapped/control
  events, graph event transformation, Session factories, and host interfaces
  absent from main. These are engine/host prerequisites; this lane will not
  transplant or edit them. Warning fixes appear only later in #209.
- **#208 Sampler:** `b982fda` depends on both preceding devices and changes
  Session. No Pd patch/external exists. The old stack's macOS failure is its
  `getParam("volume").real == 0` modulation assertion: compare the unchanged
  stored/base value before and after modulation, rather than an assumed exact
  floating-point reconstruction of zero, when this item can be recovered.
- **#209 Hybrid Reverb:** its eight device files are independent of those
  prerequisites and are being recovered onto `devices/hybrid-reverb`. The
  unrelated graph/MIDI/Sampler changes in that PR are excluded.

These existing tests use generated signals/event oracles. None supplies a
version-labelled measured Live render fixture, and the old checklists mark
parameter ranges/defaults and substantial device features unverified. Recovery
and synthetic test success must not be described as Live parity.

## 2026-10-27 — Zero-delay adapter: implementation and verification in progress

Branch `devices/pd-zero-delay`, based on main `a57d47a`.

- Canonical built-ins execute the existing native cores directly, selected by
  complete compiled canvas and resolved helper contents, never by filename.
  Modified canvases/helpers retain Pd execution. Original patches remain
  openable and the existing Pd suites explicitly exercise that path.
- A user patch can select `BlockMode::WholeBlocks` before preparation. Every
  segment must contain a whole number of Pd's 64-sample ticks; the mode returns
  same-frame output without priming. Violations are counted and silenced;
  latency is never silently changed during playback. Oversized valid calls
  are tiled through preallocated scratch instead of dropping samples.
- **Remainder for win/engine:** `maxFrames % 64 == 0` does not establish that
  guarantee: event splitting can produce 100 + 412 from a 512-frame block.
  Arbitrary user-patch segments retain the stable, reported 64-sample adapter.
  Enabling whole-block mode in a caller requires its scheduling guarantee;
  this lane does not change the graph. A streaming one-tick adapter could
  potentially reduce the conservative 64 to 63, but that is not this change.
- **Remaining legacy canvases:** `pd/adi-eq8.pd`, `pd/adi-limiter.pd`,
  `pd/adi-rmsc.pd` and the analyser remain on Pd. They are vanilla programs,
  not the canonical compiled-external canvases covered by the native dispatch;
  substituting similarly named DSP without a wiring/null proof would change
  their sound or array publication.
- **Inherited Pd runtime constraint:** pinned `z_libpd.c` takes `sys_lock`
  inside both process APIs and array reads; `sched_tick` also calls the locked
  message queue. Arbitrary user-patch callbacks can allocate or print. The
  native path bypasses these calls. Whole-block scheduling removes buffering
  delay, but is not a claim that arbitrary vanilla Pd is realtime-safe.
  Removing those runtime locks without preserving Pd clocks, message queues
  and concurrent instance lifetime is separate work, not an unlocked call
  substituted here.

Full verification and PR links are recorded in the checkpoint above.

## 2026-10-27 — Lane bootstrap and source locations

Read the standing prompt and five-lane LANES/ADR-0208 text from
`origin/win/lanes-5` (#238), since they are not yet on main. Read the requested
ADRs, device contract, FEATURES table and EQEight patch/core/external/module/
tests. `pd/README.md` is absent on both branches; the available document is
`pd/devices/README.md`.

The extracted `reference/DOCS/_text/Ableton_live12/` directory is absent on
this Mac. A local Live 12 manual PDF is available under the existing checkout's
`adi_daw/References/live12-manual-en.pdf`; the original is left untouched.
All future measurements must say Live 11 or Live 12. No Live measurement has
been made or inferred in this bootstrap/adapter work.
