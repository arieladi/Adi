# Basics recording

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-02, SC-03 | `basics/sc-02-lane-transport` `3a545410` | Left and right lane presses move the insert marker during playback, recording and count-in. Continue uses the driver's latest playback stop. Touched window and window-fn transport and arrangement code. | The mailbox probe failed on main and passed with the fix. Added mailbox regressions for queued playback, Stop/Continue in one drain, and count-in. Windows MSVC Debug app build, document, window-state and shell suites passed with warnings as errors. Both WTS-01 acceptance suites passed on a private desktop. The additional built-in performance test selected by the acceptance label is running. | PR requested |
