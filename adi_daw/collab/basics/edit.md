# Basics edit queue

The basics-edit session has claimed the Arrangement edit queue. The item branches form a chain. Code branches stay local pending the required Linux Debug JUCE and acceptance runs. Win handles Windows verification and landing.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-174 | `basics/sc-174-context-selection` `2ad211ff` (local) | Right-click preserves a selected range, picked clips and selected track headers. A popup outside a range or group uses the clicked target without seeking. A single clip still uses the popup point for Split. Window and window-fn. | Shell regressions cover the gestures and the playback position. | blocked pending Linux verification |
| SC-177 | `basics/sc-177-track-header-selection` `ea48ea84` (local) | Track title presses clear the active lane selection and retain the playback starting point. Window and window-fn. | Shell regressions exercise 0, Delete and Cut after selecting tracks. | blocked pending Linux verification |
| SC-175 | `basics/sc-175-clip-body-range` `4390afb5` (local) | A drag in a clip's body selects time from the press. R37 gates the single-click change under ADR-0258 d12. Window and window-fn. | Shell regressions exercise Delete and Split with forward and backward body drags. | blocked pending Linux verification |
| SC-176 | `basics/sc-176-arrangement-context-menus` `e5dcbfac` (local) | The clip and range menus offer applicable editing, loop, fade and Time commands. Arrangement Crop, Consolidate and clip colour are still owed. Window and window-fn. | Shell regressions inspect both menus and invoke Deactivate and Delete. | blocked pending Linux verification |
| SC-394 | — | Default export range. Also tracked by the export session. | The live-readings log and export report are still waiting for R36. | blocked on the Live 12 observation |

| SC-179 | `basics/sc-179-multi-clip-clipboard` `a3c6c8f6` (local) | Copy and Cut capture exactly the picked clips. Paste preserves offsets and gap material. Duplicate makes and selects all copies in one edit. Engine, window and window-fn. | Shell regressions cover sparse Paste, exact Cut and one-step Undo. The changed C++ files passed MSVC Debug syntax checks before the peerless menu follow-up. | blocked pending Linux verification |
| SC-178 | — | Consolidate depends on ED-06 and TM-FC-02. | Existing engine report records the shared pre-effects capture dependency and open Warp and MIDI mapping choices. | blocked on the render and MIDI consolidation contracts |
| SC-180 | `basics/sc-180-arrow-group-move` `2518247d` (local) | Arrows move all picked clips by a common grid step and move the selected material inside a time range in one edit. Engine, window and window-fn. | Shell regressions check offsets, partial range overlap and Undo. | blocked pending Linux verification |
| SC-181 | — | The SC-180 planner supplies the atomic range move. | R39 is still pending for the drag gesture. | blocked on the Live 12 observation |
| SC-182 | `basics/sc-182-play-selection-start` `ed2d96cb` (local) | Space starts a time range at its earliest tick, even when the drag began on the right. Window and window-fn. | Shell regression drives playback through the transport mailbox. | blocked pending Linux verification |
| SC-183 | — | Repeated Paste destination. | R40 is still pending. | blocked on the Live 12 observation |
| SC-185 | — | Arrangement Crop under ED-04. | The engine report lists the open DF-E38(b) questions. The Note Editor command only crops notes in source time. | blocked on the Arrangement Crop contract |
| SC-186 | `basics/sc-186-fragmentary-bar-repair` `f885e45f` (local) | The scrub menu repairs a fragmentary bar using global time edits. Completing a fragment retains its opening meter at the insertion boundary. Engine, window and window-fn. | Shell regressions invoke both menu actions and check the meter, locator and Undo. | blocked pending Linux verification |
| SC-187 | — | Insert Silence with an insert marker alone. | R41 is still pending. | blocked on the Live 12 observation |

## Coordination

- SC-174 owns preserving the context-menu selection and preventing a right-click from seeking. Recording's SC-02 owns playback behavior for a primary click. The SC-02 change to the Arrangement locate helper sits next to the SC-174 selection changes.

## For the director

- SC-178. I recommend deterministically allocating new MIDI note IDs on collisions and repeated loop occurrences, and remapping per-note expression with them. For audio, I agree with the engine report's recommendation to keep consolidated clips warped at the project tempo. The shared pre-effects capture is needed before the audio command can be exposed.
- SC-185. I recommend defining the crop bounds and crossing-note expression behavior before connecting Ctrl+Shift+J in the Arrangement. Retain ED-04's no-media-write rule and keep the original project data in Undo.
