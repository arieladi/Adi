# Basics tracks

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-01 | `basics/sc-01-protect-main` `265fbe7f` | The shared track delete op refuses Main, including a cascading group deletion, and the window displays the refusal in the Status Bar. Touched the engine's track image and the window's shell tests. | Track-delete and changeset regressions failed before the fix and passed in Linux Debug with WERROR after it. Shell and session acceptance verification is running. | blocked on pending Debug shell and acceptance verification |
