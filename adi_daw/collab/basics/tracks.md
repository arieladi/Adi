# Basics tracks

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-01 | `basics/sc-01-protect-main` `265fbe7f` | The shared track delete op refuses Main, including a cascading group deletion, and the window displays the refusal in the Status Bar. Touched the engine's track image and the window's shell tests. | Track-delete and changeset regressions failed before the fix and passed in Linux Debug with WERROR after it. Debug shell and window-state suites passed. All three acceptance tests passed. In the app on an isolated Xvfb display, Main's context-menu Delete showed the refusal and SQLite still held Main. | PR requested |
| SC-05, SC-260 | `basics/sc-05-selection-delete` `e21381d1` | Edit Delete follows clip, time, note, device and track selection. Delete and Backspace remove selected tracks as one Undo step. A selected group covers its selected descendants. Touched the window and window-fn shell, Arrangement and Mixer. | Six SC-05/260 regressions failed in the Debug WERROR shell on the unfixed baseline. Fixed-suite and acceptance verification is running. | blocked on fixed-suite verification |
