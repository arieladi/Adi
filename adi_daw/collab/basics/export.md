# Export basics report

The basics-export session has claimed all nine Export and import rows assigned to `export`. Work starts with wave 1, then proceeds through waves 2 and 3. Win handles Windows verification and landing.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-394 | — | Default render range | The branch log at `80de70f8` has no export reading. | blocked on the Live 12 observation |
| SC-395 | `basics/sc-395-missing-assets` `56797778` (local) | Offline export uses the playback fallbacks for missing clips and plug-ins. Engine and engine-win. | The regression produced six failures against the original code, then Windows Debug passed clip playback, session, render queue, NI Stems and WTS-01. | blocked pending Linux verification |
| SC-396 | `basics/sc-396-export-settings` `d5c74ff2` (local) | The export dialog uses Settings defaults, remembers session choices and refreshes the range. Window and window-fn. | Changed translation units compiled with MSVC Debug and warnings as errors before stacking. | blocked pending Linux verification |
| SC-397 | `basics/sc-397-stem-export` `b2b641c3` (local) | The app queues post-fader files for all or selected audio-bearing tracks. Return and Main processing for isolated stems still needs the engine's ViaMain route. Window and window-fn. | Regression cases cover both modes and track selection. | blocked pending Linux verification |
| SC-398 | — | Work is moving to the framework-free AIFF writer in EX-04. FLAC and MP3 require the planned encoder pins. Engine-win, window and window-fn. | AIFF regression in progress. | blocked on the libFLAC and LAME dependency grants |
| SC-399 | Local, awaiting the AIFF update in the chain | New, Open and Quit ask before stopping a render. Main window progress stays visible when the dialog is closed. Window and window-fn. | MSVC Debug compiled the changed files before stacking. The interruption test failed when the wait was removed. | blocked pending Linux verification |
| SC-400 | Local, awaiting the AIFF update in the chain | Progress advances through preroll and normalization. Engine-win and window-fn. | Windows Debug passed render queue, normalization and NI Stems on the initial stacked head. | blocked pending Linux verification |
| SC-401 | Local, awaiting the AIFF update in the chain | Plain-language messages and the three Shaped names from ADR-0250. Window. | UI regression cases cover the renamed options and refusals. | blocked pending Linux verification |
| SC-402 | Local, awaiting the AIFF update in the chain | 176.4 and 192 kHz are offered for any project. Window. | UI regression case selects both rates in a lower-rate project. | blocked pending Linux verification |

The session is running a temporary Debian VM because the available host is Windows. Item branches
stay local until the required Linux build and suites pass. The landing branches will form a chain
per the updated howto on main.

## For the director

- For SC-398, please decide on the libFLAC and LAME dependency grants listed under EX-04 in
  `docs/PLAN.md`. I recommend granting both and following the plan's in-process encoders
  with pinned source, including LAME 3.100 for 320 kbps CBR MP3.
