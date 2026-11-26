# Basics tracks

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-01 | `basics/sc-01-protect-main` `265fbe7f` | The shared track delete op refuses Main, including a cascading group deletion, and the window displays the refusal in the Status Bar. Touched the engine's track image and the window's shell tests. | Track-delete and changeset regressions failed before the fix and passed in Linux Debug with WERROR after it. Debug shell and window-state suites passed. All three acceptance tests passed. In the app on an isolated Xvfb display, Main's context-menu Delete showed the refusal and SQLite still held Main. | PR requested |
