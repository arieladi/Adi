# Basics recording

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-02, SC-03 | `basics/sc-02-lane-transport` `3a545410` | Left and right lane presses move the insert marker during playback, recording and count-in. Continue uses the driver's latest playback stop. Touched window and window-fn transport and arrangement code. | The mailbox probe failed on main and passed with the fix. Added mailbox regressions for queued playback, Stop/Continue in one drain, and count-in. Windows MSVC Debug app build, document, window-state and shell suites passed with warnings as errors. Both WTS-01 suites and the Debug built-in performance acceptance test passed. | PR requested |
| SC-22 | `basics/sc-22-take-latency` `6618b309` | Arrangement Record captures the driver's input plus output latency at the start of the take and passes it to the existing placement command. Touched window-fn recording and Windows/JUCE audio wiring. | The simulated loopback assertion failed before offset placement and passed with it. Windows MSVC Debug app build, document, recording-command and shell suites, and both WTS-01 suites passed. At 48 kHz, a headless WASAPI VB-CABLE test measured the exact reported 960-sample placement shift. The four returned impulses still had residual offsets of 4,789 to 5,269 samples. | blocked on sample-exact cable alignment |

## For the director

- SC-22. The shared WASAPI cable reported a 20 ms round trip. After that correction the clicks remained about 100–110 ms late. I recommend taking the reported-offset fix and keeping Driver Error Compensation and a repeat cable measurement under SC-255.
