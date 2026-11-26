# Basics recording

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-02, SC-03 | `basics/sc-02-lane-transport` `ac621acb` | Left and right lane presses move the insert marker during playback, recording and count-in. Continue uses the driver's latest playback stop. Touched window and window-fn transport and arrangement code. | A headless mailbox probe failed all three checks on main and passed all three with the fix. Added peerless gesture regressions. | blocked on Windows/MSVC and the shared audio device. Linux Debug and acceptance are building. |
