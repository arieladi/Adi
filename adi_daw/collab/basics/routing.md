# Basics routing

Queue claimed by the basics-routing session. First up are the In/Out choosers, take lanes, and MIDI output, followed by the remaining routing rows in register order.

SC-23 and SC-92 landed on main in #807 and #809. The local SC-261 branch keeps those changes and adds the remaining chooser interactions and Main Out routing. It is in Linux Debug verification with JUCE, Pd, and warnings as errors.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-261 | Local `basics/sc-261-in-out` at `84bec63e` | Audio To selects Main or the track group. Main Out selects an enabled stereo pair. MIDI menus show the routes available now. The change touches engine and window paths. | Earlier base `9ffb657c` passed the peerless Debug JUCE shell, WTS-01 and WTS-01 VST3. Ten core suites passed on the same production code. A native Xvfb gesture switched Audio To from Group to Main and back. The bare Xvfb shell reproduced the known JUCE fake-peer BadWindow failure. The realtime suite reached its 1,800-second timeout in the emulated guest. | blocked pending verification after merging the landed speed work and a longer realtime run |

## Build storage

The active Linux root image is `D:\adi-routing-vm\linux-stable.qcow2` (2,464,677,888 bytes) and is self-contained. The active build volume is `D:\adi-routing-vm\build-routing-verified.qcow2` (1,706,164,224 bytes). The D: build copy was verified before removal of this session's dedicated C: build original.

The C: bootstrap image `%TEMP%\adi-routing-linux\ubuntu.img` (243,335,168 bytes) is retained because exclusive use could not be established. The checkout, new build trees, caches, and downloads use D:.

C: had **15,936,638,976 bytes free (14.84 GiB)** at the post-move measurement.

## For the director
