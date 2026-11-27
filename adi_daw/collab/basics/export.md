# Export basics report

**Newest branch** `basics/sc-402-export-sample-rates` at `8a3fdca0`

The basics-export session has claimed all nine Export and import rows assigned to `export`. Work starts with wave 1, then proceeds through waves 2 and 3. Win handles Windows verification and landing.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-394 | `basics/sc-394-export-range` `0670165d` (local) | The Loop-off fallback is a named choice, with a fixture that distinguishes the brace from the song. Window and window-fn. | The branch log at `80de70f8` has no R36 result. | blocked on the Live 12 observation |
| SC-395 | `basics/sc-395-missing-assets` `d348169c` (pushed) | Offline export uses the playback fallbacks for missing clips and plug-ins. Engine and engine-win. | The regression produced six failures against the original code. Windows Debug passed four affected suites and both acceptance tests. | blocked pending Linux verification |
| SC-396 | `basics/sc-396-export-settings` `d315609f` (pushed) | The export dialog uses Settings defaults, remembers session choices and refreshes the range. Window and window-fn. | Changed translation units compiled with MSVC Debug and warnings as errors before stacking. | blocked pending Linux verification |
| SC-397 | `basics/sc-397-stem-export` `5a0495a5` (pushed) | The app queues post-fader files for all or selected audio-bearing tracks. Return and Main processing for isolated stems still needs the engine's ViaMain route. Window and window-fn. | Regression cases cover both modes and track selection. | blocked pending Linux verification |
| SC-398 | `basics/sc-398-export-formats` `40322435` (pushed) | Framework-free AIFF in 16- and 24-bit PCM and 32-bit float AIFF-C. FLAC and MP3 require the planned encoder pins. Engine-win, window and window-fn. | Windows render queue passed 465 checks. A Linux GCC probe decoded each format back to the original samples at 176.4 kHz. | blocked on the libFLAC and LAME grants and the Linux application and acceptance verification |
| SC-399 | `basics/sc-399-export-guard` `51932405` (pushed) | New, Open and Quit ask before stopping a render. Main window progress stays visible when the dialog is closed. Window and window-fn. | MSVC Debug compiled the changed files before stacking. A Linux GCC probe passed the idle, keep-exporting and cancel-and-continue paths. | blocked pending the Linux window and acceptance suites |
| SC-400 | `basics/sc-400-export-progress` `ad53f1dc` (pushed) | Progress advances through preroll and normalization. Engine-win and window-fn. | Windows Debug passed render queue, normalization and NI Stems on the initial stacked head. A baseline probe produced four expected progress failures. | blocked pending Linux verification |
| SC-401 | `basics/sc-401-export-messages` `c5e311f3` (pushed) | Plain-language messages and the three Shaped names from ADR-0250. Window. | A Linux GCC probe of the production choices failed on the preceding head and passed on the fix. | blocked pending the Linux window and acceptance suites |
| SC-402 | `basics/sc-402-export-sample-rates` `8a3fdca0` (pushed) | 176.4 and 192 kHz are offered for any project. Window. | A Linux GCC probe found the rates missing on the preceding head and present on the fix. | blocked pending the Linux window and acceptance suites |

The eight implementation branches were pushed at Win's request while Linux verification is pending. They form one chain on main through `de2a5b99`, with one branch per item. SC-394 stays local pending R36. The Windows host rebooted during the initial Linux build. The Debian VM and completed objects were recovered and verification resumed on the merged source.

## For the director

- For SC-398, please decide on the libFLAC and LAME dependency grants listed under EX-04 in
  `docs/PLAN.md`. I recommend granting both and following the plan's in-process encoders
  with pinned source, including LAME 3.100 for 320 kbps CBR MP3.
