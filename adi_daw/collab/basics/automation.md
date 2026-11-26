# Basics automation

Queue claimed. SC-334 and SC-335 are ready for Win. SC-336 is next.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-334 | `basics/sc-334-automation-follows-clips` at `6d39b9cc` | Unlocked move, paste, duplicate, Ctrl-drag, cut and delete edit clips and the associated track, device and send envelopes together. Cross-track placement matches compatible controls. Guard points preserve the outside curve and one undo restores both. Touched engine placement and storage, and window and window-fn arrangement and shell. | Reproduced the refusal on the original planner. Linux Debug with JUCE, Pd and warnings as errors. 11 core suites, window document, state and parameter suites, and all 3 acceptance suites passed. The shell passed 3,327 checks with `DISPLAY` unset as in UI CI. Xvfb shell startup hit `BadWindow`; the native app under Xvfb passed a Ctrl+D gesture that copied the clip and its Volume breakpoint. | PR requested |
| SC-335 | `basics/sc-335-write-arrangement-automation` at `2b113d1a` | Arrangement Record starts automation without an armed audio track. Mixer faders and knobs, device-strip knobs and hosted-editor touches record held values, including through a loop wrap. Release restores envelope playback and commits an undoable pass. Distinct hosted gestures keep separate undo steps. Hardware Latch requires control mapping, and automatic punch boundaries belong to SC-26. Touched engine Session and ParamOps, the window document, and window and window-fn recording wiring. | Reproduced the initial refusal in the document fixture. Linux Debug with JUCE, Pd and warnings as errors. All 14 affected suites, including the peerless shell, and all 3 acceptance suites passed. The document suite passed 406 checks. A real F9 recording could not be exercised because the Linux account lacks playback device access. | PR requested |

## For the director
