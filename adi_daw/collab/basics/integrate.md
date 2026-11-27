# Basics integration

Integration starts from `origin/land/basics-1127i` at `ed0f2aa18795792818fd3581793c6030e2a2a9b1`, ahead of main with the speed chain and two Clang 14 fixes.

| Chain and tip | Integration head | Conflict resolution | Verification |
|---|---|---|---|
| `basics/recording-chain` `d1f309b3830a677e5106b598834f3524a3890da8` | `9893e49a34e1f5bc32995dd59965861803908d19` | Combined all three recording commands/document conflicts. The speed worker publishes placeable tracks, reports and retains source WAVs for skipped tracks, keeps MIDI-only punch fallback, and restages if the track set changes. Preserved loop publication and both sides’ fixtures. Also fixed a copied fixture struct, two Clang 14 window constructs and a shadowed app argument. | Linux GCC 14 Debug Ninja, JUCE and Pd on, Werror, acceptance configured. Built the app and test targets. Passed 18 recording, transport, gesture, settings and op suites, all 18 `adi_ui_` suites with DISPLAY unset, and all five validators. Clang 14 syntax passed on the app and affected recording files. |

## For the director

No open decisions.
