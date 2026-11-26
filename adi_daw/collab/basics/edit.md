# Basics edit queue

The basics-edit session has claimed the Arrangement edit queue. The item branches form a chain. Code branches stay local pending the required Linux Debug JUCE and acceptance runs. Win handles Windows verification and landing.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-174 | `basics/sc-174-context-selection` `3ca679d2` (local) | Right-click preserves a selected range, picked clips and selected track headers. A popup outside a range or group uses the clicked target without seeking. A single clip still uses the popup point for Split. Window and window-fn. | Shell regressions cover the gestures and the playback position. | blocked pending Linux verification |
| SC-177 | `basics/sc-177-track-header-selection` `6cdb0ab7` (local) | Track title presses clear the active lane selection and retain the playback starting point. Window and window-fn. | Shell regressions exercise 0, Delete and Cut after selecting tracks. | blocked pending Linux verification |
| SC-175 | `basics/sc-175-clip-body-range` `afdf57e5` (local) | A drag in a clip's body selects time from the press. R37 gates the single-click change under ADR-0258 d12. Window and window-fn. | Shell regressions exercise Delete and Split with forward and backward body drags. | blocked pending Linux verification |
| SC-176 | `basics/sc-176-arrangement-context-menus` `f19f5732` (local) | The clip and range menus offer applicable editing, loop, fade and Time commands. Arrangement Crop, Consolidate and clip colour are still owed. Window and window-fn. | Shell regressions inspect both menus and invoke Deactivate and Delete. | blocked pending Linux verification |
| SC-394 | — | Default export range. Also tracked by the export session. | The live-readings log and export report are still waiting for R36. | blocked on the Live 12 observation |

## Coordination

- SC-174 owns preserving the context-menu selection and preventing a right-click from seeking. Recording's SC-02 owns playback behavior for a primary click. The SC-02 change to the Arrangement locate helper sits next to the SC-174 selection changes.

## For the director

