# Basics routing

Queue claimed by the basics-routing session. First up are the In/Out choosers, take lanes, and MIDI output, followed by the remaining routing rows in register order.

SC-23 and SC-92 landed on main in #807 and #809. The local SC-261 branch keeps those changes and adds the remaining chooser interactions and Main Out routing. It is in Linux Debug verification with JUCE, Pd, and warnings as errors.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-261 | Local `basics/sc-261-in-out` at `35151b1f` | Audio To selects Main or the track group. Main Out and Cue Out select enabled stereo pairs independently. MIDI menus show the routes available now. The change touches engine and window paths. | The Debug JUCE app and shell suite compiled at `35151b1f`. Cue Monitor, Device, Catalog, Snapshot Routing IDs and Gesture passed. A real app gesture under Xvfb switched Audio To through Group, Main and Group, with all three states checked in the project database. The isolated document suite reports a failure in the device-loss take assertion (same fixture and capture code as main). A focused diagnostic is in progress. Earlier base `9ffb657c` passed the peerless shell, WTS-01 and WTS-01 VST3. Its bare Xvfb shell reproduced the JUCE fake-peer BadWindow failure, and its realtime suite reached the 1,800-second timeout in emulation. | blocked on the device-loss document assertion and remaining Debug Linux verification |

## Build storage

The active Linux root image is `D:\adi-routing-vm\linux-stable.qcow2` (3,976,265,728 bytes at the latest measurement). The active build volume is `D:\adi-routing-vm\build-routing-verified.qcow2` (1,706,622,976 bytes). Both are standalone. The D: build copy was verified before removal of this session's dedicated C: build original.

The C: bootstrap image `%TEMP%\adi-routing-linux\ubuntu.img` (243,335,168 bytes) is retained because exclusive use could not be established. The D: copy was verified. The checkout and guest build and cache are on D:. New host downloads and temporary staging use `D:\adi-routing-vm\runtime\downloads` and `D:\adi-routing-vm\runtime\tmp`.

The inactive C: image `%TEMP%\adi-routing-linux\build.qcow2` (197,120 bytes) and original read-only seed `%TEMP%\adi-routing-linux\seed.iso` (55,296 bytes) were also retained.

C: had **15,574,282,240 bytes free (14.50 GiB)** at the post-move measurement. A later check on 27 November at 18:53 local time showed **21,664,202,752 bytes free (20.18 GiB)**.

## For the director
