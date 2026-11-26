# Basics automation

Queue claimed. SC-334 is ready for Win. SC-335 is next.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

| SC-334 | `basics/sc-334-automation-follows-clips` at `6d39b9cc` | Unlocked move, paste, duplicate, Ctrl-drag, cut and delete edit clips and the associated track, device and send envelopes together. Cross-track placement matches compatible controls. Guard points preserve the outside curve and one undo restores both. Touched engine placement and storage, and window and window-fn arrangement and shell. | Reproduced the refusal on the original planner. Linux Debug with JUCE, Pd and warnings as errors. 11 core suites, window document, state and parameter suites, and all 3 acceptance suites passed. The shell passed 3,327 checks with `DISPLAY` unset as in UI CI. Xvfb shell startup hit `BadWindow`; the native app under Xvfb passed a Ctrl+D gesture that copied the clip and its Volume breakpoint. | PR requested |

## For the director
