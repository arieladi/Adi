# Basics recording

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-02, SC-03 | `basics/sc-02-lane-transport` `b98cccb5` | Left and right lane presses move the insert marker during playback, recording and count-in. Continue uses the driver's latest playback stop. Touched window and window-fn transport and arrangement code. | The mailbox probe failed on main and passed with the fix. Added mailbox regressions for queued playback, Stop/Continue in one drain, and count-in. Windows MSVC Debug app build, document, window-state and shell suites passed with warnings as errors. Both WTS-01 suites and the Debug built-in performance acceptance test passed. | PR requested |
| SC-22 | `basics/sc-22-take-latency` `a14d011e` | Arrangement Record captures the driver's input plus output latency at the start of the take and passes it to the existing placement command. Touched window-fn recording and Windows/JUCE audio wiring. | The simulated loopback assertion failed before offset placement and passed with it. Windows MSVC Debug app build, document, recording-command and shell suites, and both WTS-01 suites passed. At 48 kHz, a headless WASAPI VB-CABLE test measured the exact reported 960-sample placement shift. The four returned impulses still had residual offsets of 4,789 to 5,269 samples. | blocked on sample-exact cable alignment |
| SC-245 | `basics/sc-245-monitor-record` `30672e7a` | An armed Auto track plays its input over an existing clip during the slices in which the recorder accepts its audio. Normal monitoring returns as soon as capture ends. The recorder publishes sorted target IDs from its existing callback snapshot. Touched engine and engine-win playback, monitoring and recording. | The new Auto checks failed in both precisions on main and passed with the fix, including a callback split at the count-in boundary. Windows MSVC Debug clip-playback and audio-recording suites and all three tests selected by the acceptance label passed. | PR requested |
| SC-247, SC-254 | `basics/sc-247-preserve-takes` `71350ac5` | Failed placement or writing leaves the original WAV files at their capture path, which the Status Bar reports. If the device stops during Record, the document timer can settle the take without another callback. Touched window-fn recording and engine-win recorder tests. | Main failed to land a take after device loss and removed the WAV after a publication refusal. The fixed regression verified the reported path and the complete retained PCM. An injected write and close failure preserved the first completed WAV block. Windows MSVC Debug document and audio-recording suites and both WTS-01 suites passed. | PR requested |
| SC-20 | `basics/sc-20-recording-waveform` `8a506549` | The recorder's writer computes bounded per-input peaks and publishes immutable snapshots. Arrangement uses the captured start and extent, keeps the preview through Stop, and hands off from the final recorded peaks to the file's own peaks. Touched engine-win recording, window-fn document and window Arrangement code. | The recorder regression failed without peak accumulation. Windows MSVC Debug recorder, document and shell suites, app build and all acceptance tests passed. Headless rendering verified waveform pixels, the Stop-to-clip transition and an allocation-free preview update in a window frame. | PR requested |
| SC-21, SC-246 | `basics/sc-21-metronome-count-in` `34f6f1d7` | The metronome arrow and its right-click menu offer None, 1, 2 and 4 Bars and share the Cue Volume used by the mixer. The transport shows the count-in in its Bar field and clears it on the capture boundary or Stop. Touched the window and window-fn transport, metronome and document code. | The document probe failed seven assertions with count-in feedback disabled. Windows MSVC Debug app, document, window-state and shell, and all three acceptance tests passed. The tests cover the precise 6/8 boundaries, the native-menu seam and the shared gain. | PR requested |

The recording branches now form a chain in queue order. Windows Debug passed the combined recorder,
placement, playback, document and mailbox suites, all acceptance tests, and 3,332 shell checks. The lane
test uses the context-menu seam. The waveform and its landed clip now use the same driver-corrected
start, verified by the document regression. A repeat VB-CABLE run measured the same 960-sample
placement shift and a 5,140-sample residual on each of four clicks.

`basics/recording-chain` `392a6a6e` brings all six item branches through SC-21 and SC-246 into the
Windows landing stack. The combined Debug app, private-desktop Audio Settings smoke, all six touched
suites, and both WTS-01 tests passed. The latest landing update allows rounding in two unrelated SC-82
test assertions. The combined performance acceptance test is running.

Routing has its own claimed log. Under the current handoff, this session continues with recording
wave 2 once the first recording queue is finished.

## For the director

- SC-22. The shared WASAPI cable reported a 20 ms round trip. After that correction the clicks remained about 100–110 ms late. I recommend taking the reported-offset fix and keeping Driver Error Compensation and a repeat cable measurement under SC-255.
- R45 / SC-245. Monitor Off continues to play the earlier clip over the capture. I recommend leaving that named policy in place until the listening check confirms whether the older clip is muted at the punch point. [Live 12 monitoring and recording](https://www.ableton.com/en/live-manual/12/recording-new-clips/)
