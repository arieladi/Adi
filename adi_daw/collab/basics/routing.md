# Basics routing

Queue claimed by the basics-routing session. First up are the In/Out choosers, take lanes, and MIDI output, followed by the remaining routing rows in register order.

SC-23 and SC-92 landed on main in #807 and #809. The local SC-261 branch keeps those changes and adds the remaining chooser interactions and Main Out routing. Linux verification uses Debug with JUCE, Pd, and warnings as errors. Hardware MIDI output (SC-404) follows take lanes.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-261 | Local `basics/sc-261-in-out` at `35151b1f` | Audio To selects Main or the track group. Main Out and Cue Out select enabled stereo pairs independently. MIDI menus show the routes available now. The change touches engine and window paths. | The Debug JUCE app and test targets compiled. Twelve headless suites passed, including Mix Functions, along with WTS-01 and WTS-01 VST3 under Xvfb. The native Xvfb gesture switched Audio To through Group, Main and Group, with all states checked in the project database. The CI-style headless shell ran 3,858 checks with one failure (SC-421, browser latency). The isolated document suite ran 696 checks with two failures (the device-loss publication deadline and SC-415, plug-in owner turn latency). A focused probe confirmed the device-loss take landed successfully without another callback after 5.65 seconds. Realtime verification continues. | blocked on the full Linux Debug window gate, branch local |
| SC-252, SC-253 | Local `basics/sc-252-253-take-lanes` at `3f57b8d9` | Preserve earlier overlapping audio recordings. Arrangement displays and names take lanes, audition switches which lane plays, and a selected span copies to Main in one Undo step. The change touches the engine, window and window functions. | Added recording, comp, audio and MIDI playback, snapshot, and Debug shell regressions. An initial GCC check caught a grid-call mismatch. After the fix, the Arrangement translation unit passed a syntax check with Linux Debug warnings as errors. Host paging is delaying the rest of the build. | blocked on Linux build capacity, branch local |
| SC-404 | Local `basics/sc-404-midi-out` at `17a9c1c0` | Settings' Track switch opens MIDI outputs. Both mixers expose port and channel, and the engine queues notes and controllers for the owner-thread dispatcher. Routes in this draft last for the open document. The change touches the engine, window, and window functions. | Added fixed-port, chooser, and engine delivery regressions. The draft is awaiting native route storage and Linux compilation and tests. | blocked on the schema reservation and Linux verification, branch local |

## Build storage

The active Linux root image is `D:\adi-routing-vm\linux-stable.qcow2` (4,759,355,392 bytes at the latest measurement). The active build volume is `D:\adi-routing-vm\build-routing-verified.qcow2` (2,226,978,816 bytes). Both are standalone. The D: build copy was verified before removal of this session's dedicated C: build original.

The C: bootstrap image `%TEMP%\adi-routing-linux\ubuntu.img` (243,335,168 bytes) is retained because exclusive use could not be established. The D: copy was verified. The checkout and guest build and cache are on D:. New host downloads and temporary staging use `D:\adi-routing-vm\runtime\downloads` and `D:\adi-routing-vm\runtime\tmp`.

The inactive C: image `%TEMP%\adi-routing-linux\build.qcow2` (197,120 bytes) and original read-only seed `%TEMP%\adi-routing-linux\seed.iso` (55,296 bytes) were also retained.

C: had **15,574,282,240 bytes free (14.50 GiB)** at the post-move measurement. The latest check on 27 November at 21:08 local time showed **15,420,190,720 bytes free (14.36 GiB)**.

## For the director

For the SC-261 Linux gate, I recommend running the unchanged window suites on native Linux. The guest measured a 779.89 ms browser search against the 40 ms SC-421 bound and a 752.30 ms owner turn against the SC-415 relative bound. Both fixtures and their feature paths match main. The upstream source equivalent to this item's base passed these suites in the [main Linux UI job](https://github.com/arieladi/Adi/actions/runs/46020442251/job/140578472188). The measurements above are from the item head in this session's guest.

Reserve a schema minor for SC-404's native per-track MIDI output. I recommend an optional track-owned row holding the endpoint identifier, saved endpoint name, and MIDI channel 1–16. The row should cascade on track deletion and participate in Undo and track duplication. The port host, event delivery, and chooser work can proceed while the number is assigned.
