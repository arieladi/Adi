# Basics routing

Queue claimed by the basics-routing session. First up are the In/Out choosers, take lanes, and MIDI output, followed by the remaining routing rows in register order.

SC-23 and SC-92 landed on main in #807 and #809. The local SC-261 branch keeps those changes and adds the remaining chooser interactions and Main Out routing. It is in Linux Debug verification with JUCE, Pd, and warnings as errors.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-261 | Local `basics/sc-261-in-out` at `84bec63e` | Audio To selects Main or the track group. Main Out selects an enabled stereo pair. MIDI menus show the routes available now. The change touches engine and window paths. | Earlier base `9ffb657c` passed the peerless Debug JUCE shell, WTS-01 and WTS-01 VST3. Ten core suites passed on the same production code. A native Xvfb gesture switched Audio To from Group to Main and back. The bare Xvfb shell reproduced the known JUCE fake-peer BadWindow failure. The realtime suite reached its 1,800-second timeout in the emulated guest. | blocked pending verification after merging the landed speed work and a longer realtime run |

## For the director
