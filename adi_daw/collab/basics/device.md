# Basics device

The Windows 10 walker's queue (`collab/prompts/2026-11-27-basics-device.md`): the audio device and the
files a user drops in, on a real Windows machine. One chain branch, `basics/device-chain`, from
origin/main; every item a commit with its SC id. Release builds with warnings as errors, 2 jobs beside
another session. Before each push: the five validators, the headless suites, the window suites on a
hidden desktop, and builds of `adi_app` and `adi_play`. Rows marked "at the desk" are run only while the
director has written that the desk is free.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-379 | `basics/device-chain` 12a065e7 | The driver's mode is kept: Settings take "WASAPI Exclusive" and "WASAPI Low Latency" beside "WASAPI", and the audio page writes the mode JUCE opened, so Exclusive comes back as Exclusive. | `test_settings` testDriverModeKept. At the desk: Windows Audio (Exclusive Mode) chosen on the Audio page, written as "WASAPI Exclusive", and shown again after a restart | pushed; desk passed |
| SC-378 (with SC-28) | `basics/device-chain` 0ba3a5d0 | An unplugged interface keeps the user's choice: ADI plays through the system device and says so, and opens the chosen one again when it is back. A pure rule, `deviceChange` (Keep, Hold, Return). | `test_settings` testDeviceUnpluggedAndBack. At the desk: no USB interface here; disabling the VB-Cable device stood in badly (Windows will not disable a device in use, it waits for a reboot), and its flag was put back | pushed; desk test **waiting for the ADI studio PC** to enter the project |
| SC-376 | `basics/device-chain` c67e376a, a1980b33 | M4A and AAC play on Windows 10: the encoder probe asks for 192 kbit/s (the only rates the Windows 10 encoder takes are 96 to 192), an MP4 one tick short of its samples is accepted, and the packets go to Media Foundation with no priming edit (Windows 10 played the priming, 1,024 frames late) while ADI trims it. | `test_aac_import` against FFmpeg's independent decodes, `test_mp4_aac`, both on this Windows 10 | pushed |
| SC-377 | `basics/device-chain` 230111de, e8218e5e, 1c88590d | An hour-long file dropped in is read off the window on a worker, with progress by name ("Reading … (i of n)"), Esc cancelling; the worker opens the project read-write as the material import does. | `test_ui_shell` plainDropReadsOffTheWindow | pushed |
| SC-405 | `basics/device-chain` cbafaaaa | Every switch in the MIDI ports list does something, or is greyed with a tooltip saying why. | `test_ui_shell` MIDI ports checks | pushed |
| SC-380 | `basics/device-chain` a7a470a1, 3b457524 | Hardware Setup on the audio page opens the ASIO driver's own control panel, then reopens the device. | `test_ui_shell` audio page check. At the desk: off with Windows Audio and with no device open, on with Voicemeeter Virtual ASIO open; pressed, nothing showed, as Voicemeeter itself was not running; ASIO4ALL did not start on this PC | pushed; desk partly (a driver's panel not yet seen) |
| SC-381 | `basics/device-chain` 397f9080 | No input is opened until one is wanted: a first run opens output only (Bluetooth headphones stay in stereo), and arming a track opens the default input. | `test_ui_shell` firstRunAudioInput. At the desk: a first run showed No Device for the input; arming an audio track said the system's input was open for it, and the setting stayed empty | pushed; desk passed (no Bluetooth headset here) |
| SC-382 | `basics/device-chain` 4cc7e4fc | A sample-rate change keeps the song where it was: the position is located by time. | `test_session` testRateChangeKeepsThePlace | pushed |
| SC-383 | `basics/device-chain` 975f00fe, 3b457524, 1c88590d | "No Device" for the audio input is kept and stays off. | `test_ui_shell` audio page check | pushed |
| SC-384 | `basics/device-chain` fa156fc5 | A rate or buffer ADI cannot use (outside 44.1 to 768 kHz, over 4096 frames) is said in a plain sentence in the status bar. | `test_ui_document` unusableFormatSaid | pushed |
| SC-385 | `basics/device-chain` fa08362e | Audio Engine On/Off, Ctrl+Alt+Shift+E as in Live: the engine stops and the interface is free, and comes back. | `test_ui_shell` stable id; the three key map goldens | pushed |
| SC-387 | `basics/device-chain` d6e0bea5, 3b457524 | A file with more than two channels is refused in plain words, by its name. | `test_media_ops` moreThanTwoChannelsRefused | pushed |
| SC-388 | `basics/device-chain` 462d6836 | A sample at a path over 260 characters imports: the app and the media suite are long-path aware. | `test_media_ops` longPathImports | pushed |
| SC-389 | `basics/device-chain` 4f150024, 1c88590d | One bad file among ten no longer stops the drop: the nine come in and the bad one is named. | `test_ui_shell` plainDropReadsOffTheWindow | pushed |
| SC-390 | `basics/device-chain` bd215f3d | A file ADI cannot read says why in plain words by its name (a folder, damaged, not audio), never the decoder's words after its full path. | `test_media_ops` plainWordsForUnreadable | pushed |
| SC-391 | `basics/device-chain` 7f883d11, 543beae7 | A download cut short plays as far as it goes, and a WAV whose size fields a crashed recorder left at zero reads to the end of its data. | `test_decode` (truncated FLAC plays; the crashed recorder's 4800 frames); refusals checked with a FLAC holding no audio | pushed |
| SC-428 | `basics/device-chain` 72cd6bda | A plug-in window reopens wholly on a screen that exists: moved, never resized, onto the monitor it stands on most. | `test_ui_window_state` whollyOnScreen | pushed |
| SC-454 | `basics/device-chain` b8a53399b (fix 6fdfceb68) | The 21 catalogued built-ins show their declared controls directly. EQ Eight’s 87 controls and ADI-Shaper’s 418 used to hit the hosted Configure limit. | Linux Debug with JUCE, Pd and warnings as errors. `env -u DISPLAY ctest -LE performance` passed 295/295, including the shell, document and both WTS-01 acceptance tests. | ready for Win’s landing stack |
| SC-455 | `basics/device-log`, review below | Live 12 control inventory for eleven devices and comparison with Win’s first drawing. | Ableton panel images, manual and release notes compared with sheet 1 at `f834e6d40`. | reference review done, sheets agreed in ADR-0267 |
| SC-485 | `basics/device-chain` 98e8123bb, a79f592d6 | The full folded strip unfolds on a double-click. The activator, expansion arrow and rotated name fit in the strip. | The software peer exercises the title, lower rail, arrow, and activator, including a runtime replacement during a pending click. | ready for Win’s landing stack |
| SC-484 | `basics/device-chain` 8dd2b84d6 | Chord Comb explains Color, Saw and Square in Info View. Color Cab shows worker progress and the cost of each selectable Size. | Shell tests cover hover and the build indicator. The document test proves Undo and Redo for a sample replacement, Size and Chord Comb State on the live built-ins. | ready for Win’s landing stack |

Both rows were tested on the merged chain at `c1baac830`, which includes main at `629a48341`.
Linux Debug with JUCE, Pd and warnings as errors built `adi_app` and `adi_play`.
`env -u DISPLAY ctest -LE performance --parallel 4` passed 306/306, including the shell,
document, both WTS-01 acceptance tests and the Linux Node helper tests. All six validators
and 15 Node package tests passed.

## SC-485 folded devices

The folded strip was 22 pixels wide and clipped the expansion arrow. Its double-click handler
recognized the top 17 pixels. The strip is now 32 pixels wide and fits the rotated name between
the title controls and any playback problem marker. The full strip handles double-clicks. A
folded activator uses JUCE’s double-click interval for a mouse single-click, so a double-click
there unfolds the device without writing an activator change. Keyboard activation is immediate.
This row touches the window seat’s Device View and shell test.

## SC-484 Chord Comb and Color Cab

Info View describes how Chord Comb’s Color darkens its ringing and makes high harmonics fade
sooner. Saw rings on every harmonic and Square on the odd ones. The descriptions are attached
to the actual knob and switch.

Color Cab’s prior path used a fixed 256-tap filter and prepared it synchronously from the window.
The Size control now writes the chosen tap count into the sample state. Sample drops, sample
Hot-Swap and Size changes prepare the new colour on a worker. The card shows “Building new
colour…” during preparation. The old kernel plays until the prepared state lands and the
existing 20 ms crossfade begins. The opener defaults to 256 taps. Sample replacement, clearing
and sample-rate changes carry the selected Size forward.

This row touches the window seat’s Device View, application and project document, the
built-in sample host, and the engine seat’s sample slots and snapshot.

The Size choices display their steady-state convolution work relative to 64 taps. Each tap
adds one multiply-and-add per channel per sample. A crossfade briefly evaluates both kernels.

| Size (taps) | Relative FIR work | CPU time per stereo block in the DSP probe |
|---:|---:|---:|
| 64 | 1× | 24.83 µs |
| 128 | 2× | 56.55 µs |
| 256 | 4× | 117.15 µs |
| 512 | 8× | 251.99 µs |
| 1024 | 16× | 495.78 µs |

The probe used 48 kHz, 256-frame blocks, a settled kernel and optimized GCC on one x86-64 thread.
The figures are the median of seven runs and isolate the convolution DSP. The UI uses the
portable tap ratios. The layout reference is ADR-0267 d9 and Win’s eighth sheet. [ADR-0267](https://github.com/arieladi/Adi/blob/f3879fcca2655a784b089ff986ad861792d473f1/adi_daw/docs/DECISIONS.md#L21097) · [Sheet 8](https://github.com/arieladi/Adi/blob/b65fb0bac200689a9464168932e6e8731517ef4c/adi_daw/collab/window/design/proposals/devices-8.png)

## SC-454 device-panel findings

The parameter feed knew that these devices were internal. It still used the hosted panel resolver.
That resolver emptied an unconfigured panel above 64 modifiable parameters. EQ Eight (87) and
ADI-Shaper (418) crossed the cutoff. The other 19 were below it, with Echo the largest at 50.
All 21 offered Configure in the Device View. A Configure list saved by an older build could also
hide controls on any of them.

The feed now publishes each built-in’s declared controls in order, including on projects with
an old Configure list. Device View uses its current control renderer and hides the hosted
Configure commands for these devices. The EQ Eight filter-shape menu uses the device’s declared
choice names. The regression checks cover the catalogue, a legacy empty Configure list and a
hosted plug-in with the same UID as a built-in.

The document test linked with the previous resolver reproduced both empty panels and the legacy
Configure-list failure. The full headless run used the final code at `21f473880`. The subsequent
merge from main added only the Win log and sanity-check register updates. The six validators
passed, and the Debug build produced `adi_app` and `adi_play`.

The chain includes `origin/main` at `e0ecd0026` and `origin/wincheck/basics/device-chain` at
`14cedf3d3`. The merge preserves the background plain-audio drop with main’s sample defaults
and clip replacement, and keeps the MIDI Output Track switch wired for main’s hardware output.
The command-order fixture keeps Audio Engine after Open Backup. The AAC mismatch diagnostic
builds with GCC warnings treated as errors.
SC-454 touches the window seat’s Device View and shell test, the Mix View seat’s parameter feed,
and the built-in host that Win lent to the devices seat.

## SC-455 Live 12 control order and Win’s first sheet

Read groups left to right and the stack within a group top to bottom. The reference is the current
Live 12 manual’s Device View. Release notes identify changes within Live 12 that affect this
review. Sheet 1 draws seven of these devices and assigns Echo, Hybrid Reverb, Overdrive and Redux
to sheet 2. [Win’s first sheet at `f834e6d40`](https://github.com/arieladi/Adi/blob/f834e6d405e0f06f1a91c38c378d8a7f2ec06778/adi_daw/collab/window/design/proposals/devices-1.png)

### Utility

- Live panel, left to right. **Input** has Ø L and Ø R, Channel Mode, Width, Mono, then Bass Mono with cutoff and the headphone Audition switch next to each other. Width has a context-menu Mid/Side mode and loses effect in Left or Right Channel Mode. **Output** has Gain, Balance, then Mute and DC side by side. [Live 12 manual §29.40 and panel](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#utility)
- Win’s sheet preserves both groups and their control order. It shows the Stereo/Width state and labels the headphone switch `Aud.`. The context-menu Mid/Side state is outside that drawing.

### Auto Filter

- Live panel, left to right. The narrow **Sidechain** rail has expand and headphone Listen. Expanded, the rail reveals two columns before the filter. The first has External enable, Source, tap point, Mix, SC Gain. The next has SC Filter enable, Type, Freq and the type-dependent Res/Q or gain. Mono Sidechain is in the context menu. The **Filter** column is Freq, filter-type picker, slope buttons, Res and a filter Morph field for the types that use it. DJ changes Freq to Control; Vowel shows Pitch and Formant. [Live 12 manual §29.2.1 and §29.2.7](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#auto-filter)
- The **center** is the draggable Freq/Res filter graph with the output spectrum and L/R modulation curves. The strip at its bottom follows the selected modulation section. **LFO** shows Phase/Spin mode and its value, Phase Offset, quantization mode (None/Steps/S&H) and its value. **Envelope** shows Attack, Hold, Release, S&H and its rate. The row below the graph has LFO Amt, Rate/Time, then stacked Wave, LFO Morph/Smooth and Mode, with Envelope amount at the right. The **right column** is Drive, circuit picker, Clip with its activity LED, Output level, then Dry/Wet. [Live 12 manual §29.2.2–29.2.6](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#filter-display)
- Win puts type/circuit/slope above the graph and Freq/Res/Drive/filter Morph below it. It puts LFO and Envelope into successive full-height boxes to the graph’s right, with separate Hz/Sync buttons, a Shape dial and Phase/Spin/Steps row, and puts Sidechain in the Output box. Live groups mode-dependent controls around the center graph and places Sidechain at the far left. The sheet also omits Live’s spectrum/L–R curves, Clip/LED, the LFO phase-offset detail and the envelope S&H detail. Live 12.2 redesigned this panel. [Live 12 release notes](https://www.ableton.com/en/release-notes/live-12/)

### Delay

- Live panel, left to right. **Left and Right** each show Sync/Time, a 2×4 sixteenth-note grid (rows 1–2, 3–4, 5–6, 8–16) and Offset while synced, or milliseconds in Time mode. The Stereo Link icon sits between the channels; Feedback and the Freeze icon sit below them. **Filter/LFO** shows the draggable band-pass graph, then Filter enable, Freq and Width. The collapsed LFO strip reads LFO expander/LED, Delay amount, Filter amount. Expanded, its next lines add Rate value and mode at left, Wave and Morph at right. **Smoothing/output** shows Repitch, Fade and Jump vertically, then Ping Pong above Dry/Wet. Hi-Quality and Dry/Wet Equal-Loudness are in the options/context menu. [Live 12 manual §29.11 and panel states](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#delay)
- Win uses two 4×2 grids with Link below both, and separates Feedback/Freeze from timing. It puts Smoothing at the bottom of Timing, separates Mod from the filter graph, orders Mod as Rate, Filter, Time, and puts Dry/Wet above Ping Pong. The expanded LFO’s rate mode, Wave and Morph are absent. Live 12.4 expanded Delay’s LFO. [Live 12 release notes](https://www.ableton.com/en/release-notes/live-12/#Delay)

### Echo

- Live’s persistent **left columns** show Left and Right delay time, linked with the Stereo Link icon between them; each side has Sync, Notes/Triplet/Dotted/16th mode and an Offset. Mid/Side mode relabels the two channels. At the bottom are Input with `D` distortion and Feedback with `Ø` feedback polarity. The persistent **right columns** show Reverb, its location and Decay, with the Stereo/Ping Pong/Mid/Side mode buttons below, alongside Stereo width, Output and Dry/Wet stacked vertically. Dry/Wet Equal-Loudness is a context-menu option. [Live 12 manual §29.14 and main panel](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#echo)
- The **center tabs** read **Echo**, **Modulation**, **Character**. Echo shows the draggable delay tunnel above the collapsible filter graph. Its bottom strip reads graph disclosure, Filter enable, HP, Res, LP, Res. Modulation shows a draggable waveform display and waveform selection, then Sync, Rate/Freq and Phase at left, ×4 and Mod Delay, Mod Filter, then Env Mix. Character is a two-by-two grid with Gate and Ducking across the top (each toggle, Threshold, Release), Noise and Wobble below (each toggle, Amount, Morph), followed by Repitch and Smoothing Time along the bottom. The latter is visible in the manual screenshot and was introduced in Live 12.1. [Live 12 manual tab figures](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#echo-tab) · [Live 12.1 device updates](https://www.ableton.com/en/release-notes/live-12/#Live12.1PublicReleaseNotes-DevicesUpdates)
- Win’s first sheet lists Echo for the second sheet and calls for three pages. Each tab and the persistent side columns still need a drawing for review.

### Reverb

- Live’s upper row runs **Input Filter** (Lo Cut and Hi Cut switches, X/Y filter curve, then editable numeric values for the band’s center frequency on the left and bandwidth on the right. The graph uses frequency for X and bandwidth for Y) → **Early Reflections** (Spin, Amount and Rate, then Shape) → **Diffusion Network** (High switch, high-filter type, high frequency and decay multiplier above the graph, Low switch, low frequency and decay multiplier below it, Diffusion and Scale to its right) → **Chorus** (switch, Amount, Rate). The filter graph shows the response and draggable filter points. The lower global row runs Predelay → Smooth chooser beside Size → Decay → Freeze with Flat and Cut beneath → Stereo → Density chooser. The far-right column is Reflect, Diffuse, Dry/Wet from top to bottom. [Live 12 Reverb and Input Filter](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#input-filter), [Reverb filter-type release notes](https://www.ableton.com/en/release-notes/live-11/)
- Sheet 1 puts Predelay beneath the input graph inside Input Filter and Size, Stereo and Smooth into an intervening Global column. It uses an early-reflections pad in place of Spin Amount and Rate, and a tail bar graph in place of the Diffusion Network’s shelf response. It groups the Chorus toggle with the shelf switches and omits Chorus Amount/Rate, input frequency/bandwidth readouts, the shelf type and shelf values, and Density. It places Decay and Freeze/Flat/Cut in Diffusion Network. Reflect, Diffuse and Dry/Wet already align with Live’s right column.

### Hybrid Reverb

- Live keeps the left input stack and right output stack in view across **Reverb** and **EQ**. Input runs Send → Predelay → ms/beat sync → Feedback. Output runs Stereo → Vintage (Off, Subtle, Old, Older, Extreme) → Bass Mono → Dry/Wet. [Live 12 Hybrid Reverb](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#hybrid-reverb)
- **Reverb** has the impulse-response waveform/drop target and duration readout across the top. Under it on the left sit the convolution envelope switch, Attack, Decay and Size, with Convolution IR category and impulse choosers and previous/next beneath. The middle is Routing (Serial, Parallel, Algorithm, Convolution) above Blend. The right is Algorithm, Freeze In and Freeze, Delay, and algorithm-dependent fields above the algorithm Decay and Size knobs and their neighbors. In the illustrated Shimmer state, the upper fields after Delay are Mod and Diffusion, and the lower sequence after Size is Damping, Pitch, Shimmer. Dark Hall has Mod and Bass X above, Damping, Shape and Bass Mult below. Quartz has Mod and Diffusion above, Damping, Lo Damp and Distance below. Tides places Wave and Phase above, Damping, Tide and Rate below. Prism’s upper row ends at Delay, with Low Mult, High Mult and X over below. Routing dims an inactive engine and uses Blend in Serial or Parallel. [Live 12 Reverb tab and algorithms](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#input-section-1), [Ableton Hybrid Reverb walkthrough showing Dark Hall](https://www.youtube.com/watch?v=yLBIOiM97Vs), [Quartz panel capture](https://www.soundonsound.com/techniques/live-11-audio-midi-effects), [Tides panel capture](https://d29rinwu2hi5i3.cloudfront.net/article_media/a68f71b0-04ba-4c55-972a-f678b50a5dd2/pic-5.png), [Prism panel capture](https://d29rinwu2hi5i3.cloudfront.net/article_media/716eb899-551f-443a-9e81-692c83f02a9b/pic-7.png)
- **EQ** has an enable switch in its tab and a four-point response graph. The bottom row begins with Pre Algo, then shows the selected band’s controls. The outer bands show Type (pass/shelf), Freq and Gain in shelf mode or slope in pass mode. The peak bands show Freq, Gain and Q. [Live 12 Hybrid Reverb EQ](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#eq-section), [Ableton EQ walkthrough](https://www.youtube.com/watch?v=yLBIOiM97Vs&t=261s)
- Sheet 1 leaves Hybrid Reverb to the second sheet. The published Live 12 panel shows Shimmer and a shelf EQ state. The Dark Hall, Quartz, Tides and Prism positions come from Live 11 panel captures and match the mode-specific controls listed for Live 12.

### Saturator

- On Live 12.1 and later the compact panel starts with the Curve Type chooser above the real-time Shaper Curve, with Threshold to its right when Bass Shaper is selected. Below the graph runs Post Clip Mode (No Clip, Soft Clip, Hard Clip) → Color → Amt Lo. Bottom row is Drive, Output, Dry/Wet. Curve choices are Analog Clip, Soft Sine, Bass Shaper, Medium Curve, Hard Curve, Sinoid Fold, Digital Clip and Waveshaper. [Live 12 Saturator](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#saturator), [Live 12.1 Saturator update and Bass Shaper panel](https://www.ableton.com/en/release-notes/live-12/#Live12.1PublicBetaReleaseNotes-UpdatedSaturator)
- The title-bar expander adds an area to the right. At the top it shows the Color Curve graph with draggable points and In/Out spectra, then Amt Hi, Frequency, Width. Below, the Waveshaper section (active for that curve) shows Drive, Curve, Depth across the first row and Linear, Damp, Period across the second. Hi-Quality and Pre-DC Filter are in the device menu. [Live 12 Saturator expanded views](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#saturators-waveshaper-controls)
- Sheet 1 puts the curve chooser below the graph and groups Drive, Output and Dry/Wet in a separate right column. It shows a single Soft Clip switch and an always-present Base/Freq/Width/Depth row. Live uses the three-mode post-clip chooser, keeps Amt Lo on the compact face, and places high-color and Waveshaper controls in the expansion. Bass Shaper Threshold and the expanded Color Curve and Waveshaper sections are absent from the sheet.

### Multiband Dynamics

- Live’s Main view starts with **Split Freq**. Rows read High, Mid, Low from top to bottom. High/Low each have a band-split switch and crossover number. Each band has an activator and Solo plus an adjacent Input knob. Soft Knee and the RMS/Peak switch sit beneath the input rows. Across the middle, Live shows **Below** (threshold/ratio) → dynamics graph → **Above** (threshold/ratio) → **Att/Rel** (attack/release) simultaneously in all three rows. The graph has horizontal dB scaling, input/output level bars, and two editable blocks per band. Drag a block edge horizontally for its threshold, or the block vertically for ratio. The right has three band Output knobs, then global Output, Time and Amount stacked. Live 12.0 release notes confirm the three simultaneous parameter columns. The manual’s T/B/A paragraph describes the older panel. [Live 12 Multiband Dynamics](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#multiband-dynamics), [Live 12.0 Multiband Dynamics update](https://www.ableton.com/en/release-notes/live-12/#Live12PublicReleaseNotes-MultibandDynamics)
- The optional **Sidechain** opens at the far left. The manual image shows the Sidechain enable and headphone audition, then the two Audio From choosers (source track, signal tap), Gain and Mix. The manual prose calls Mix Dry/Wet. Live 12.2 moved the expander to the device’s left-side Sidechain header. The published image still shows the earlier title-bar expander. [Live 12 Multiband Dynamics sidechain](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#sidechain-parameters-4), [Live 12.2 device header changes](https://www.ableton.com/en/release-notes/live-12/)
- Sheet 1 adds a top crossover strip with draggable handles between Low/Mid/High and places band switches and solos inside it. Live puts the two crossover fields by High and Low at the left of the High/Mid/Low processing rows and has separate band activators. The sheet has the same three processing rows and two threshold blocks, but omits the dB axis, separate input/output bars, dedicated Below/Above numeric thresholds and ratios, and the sidechain. It places band Output before Attack/Release and uses a footer for global Time, Amount and Output, where Live puts Att/Rel, band Output and then the global Output/Time/Amount column. Live puts Soft Knee and one RMS/Peak control below the input rows.

### EQ Eight

- **Live 12, normal Device View, left to right.** The selected band’s Freq, Gain, and Q dials with numeric readouts are stacked at left. The center is the draggable numbered-band EQ response graph with a frequency axis, a dB axis, and the output spectrum behind the response when Analyze is on. Below it, bands 1–8 each have a filter-shape chooser above a separate square activator and band number. The chooser order is 48 dB/oct low cut, 12 dB/oct low cut, low shelf, peak, notch, high shelf, 12 dB/oct high cut, 48 dB/oct high cut. The right column starts with Audition (headphones) and Analyze (spectrum), then Mode (Stereo, L/R, M/S), the single Edit toggle, Adapt. Q, Scale, and global Gain. Edit is inactive in Stereo and changes the active curve in L/R or M/S. Scale and global Gain are value fields. Band gain is inactive for low cut, notch, and high cut. [Ableton Live 12 manual and normal-panel image](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#eq-eight)
- **Live 12, expanded and context controls.** The title-bar triangle moves the graph into the main window. In the Device View below it, the Analyze group at left has Block, Refresh, and Avg, followed by eight band columns, each with Freq, Gain, Q, shape, and activator. The global column stays at right. Oversampling is in the title-bar context menu. [Ableton’s expanded panel image](https://ableton-production.imgix.net/live-manual/12/EQEightExpandedViewL12.png) · [Ableton Live 12 manual](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#context-menu-options-for-eq-eight)
- **Windows sheet 1.** The main three groups agree. The eight cells put a numbered switch beside the shape, while Live stacks shape over the separate activator and number. The sheet adds BAND 3 and GLOBAL headings and a caption beneath the bands. The guide lines have no frequency/dB labels and the plotted bands have color-coded dots without band numbers. The right column omits Audition and Analyze, shows separate A and B buttons with A highlighted in a Stereo example, and uses a dial for global Gain. The sheet shows a single inline view without the title-bar expand control or its analyzer and per-band controls. The spectrum drawing illustrates Analyze’s effect. The native track device reference here is EQ Eight. ADR-0266 d5 places ADI’s own EQ in Mix View.

### Overdrive

- **Live 12, top to bottom.** The band-pass X–Y pad is first, with frequency on the horizontal axis and bandwidth on the vertical axis. Directly below are the editable frequency and bandwidth boxes, left to right. Then come Drive and Tone dials side by side, with full-width Dynamics and Dry/Wet value rows underneath in that order. [Ableton Live 12 manual and panel image](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#overdrive)
- **Windows sheet 1.** The footer assigns Overdrive to the second sheet.

### Redux

- **Live 12, left to right.** The Downsampling column stacks Rate and Jitter dials above Filter, with Pre and Post switches side by side and the Post-Filter Octave field underneath. The Bit Reduction column stacks Bits and Shape dials, the DC Shift switch, then the Dry/Wet value field. The screenshot shows Post on and its octave field visible. [Ableton Live 12 manual and panel image](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/#redux)
- **Windows sheet 1.** The footer assigns Redux to the second sheet.

## For the director
- **SC-455, first sheet review.** Win recorded that you liked EQ Eight and Utility in sheet 1 and kept the Multiband
  Dynamics crossover strip. In Live, the High and Low crossover fields sit next to their bands,
  and the central graph edits threshold and ratio. Carry those graph edits into the drawing with
  the crossover strip. ADR-0267 records the subsequent sheet agreement.
- The landing check ran at the chain's head (every item in it), not once per item: a full build here is
  several hours at 2 jobs, and the items touch none of the JUCE modules that take that time.
- **Waiting for the ADI studio PC to enter the project** (the director, 27 November): SC-378's desk
  test needs a real interface unplugged while ADI plays through it and plugged back, and this desk has
  none. The Mac session was weighed and is not used: the fault was read in the Windows path (the system's
  default reopened and saved over the choice), a Mac run goes through Core Audio's device changes instead
  and proves only that path, and it too needs a real device in the director's hands. The test, when the
  studio PC is here: choose its interface as output, unplug it while playing (the Status Bar says it is
  not connected and what ADI plays through; the setting keeps the interface), plug it back (ADI opens it
  again and says it is back), and once more across a restart with the interface left out. **For win, a
  register row:** DF-E (next free) | SC-378's unplug and plug-back at a real desk | the ADI studio PC |
  when it enters the project, before SC-378 closes.
- SC-380's panel waits for the same PC: an ASIO driver whose own panel opens (the studio PC's interface).
  ASIO4ALL does not start here, and Voicemeeter's shows nothing unless Voicemeeter runs. A Mac cannot do
  this one: Core Audio drivers have no panel of their own.
- The VB-Cable device was disabled for the SC-378 try (with your approval); Windows held the disable for
  a reboot, and its flag was set back, so it stays enabled.
- The landing check at the chain's head (1c88590d): the five validators, the build of every suite,
  `adi_app` and `adi_play` with warnings as errors, and 296 of 297 suites pass. `adi_builtin_realtime_tests`
  (labelled performance) misses its latency deadlines on this PC, alone too (24 checks, all p99 or spike
  timings of the built-in devices) while another session builds beside it. The chain touches no device or
  DSP code; `session.cpp` changes only on a sample-rate change. Not proved against origin/main here (a
  full build of main is hours); CI's run of the suite is the one to read.
- Not in this queue: SC-393.
