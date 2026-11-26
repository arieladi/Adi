# Basics recording

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-02, SC-03 | `basics/sc-02-lane-transport` `4eb6353c` | Left and right lane presses move the insert marker during playback, recording and count-in. Continue uses the driver's latest playback stop. Touched window and window-fn transport and arrangement code. | A headless mailbox probe failed all three checks on main and passed all three with the fix. Added peerless gesture regressions and corrected their event coordinate type after the Linux compiler diagnostic. | blocked on Windows/MSVC verification. Linux Debug shell and acceptance verification paused on reassignment to tracks. |
