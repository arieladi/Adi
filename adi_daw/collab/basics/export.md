# Export basics report

The basics-export session has claimed all nine Export and import rows assigned to `export`. Work starts with wave 1, then proceeds through waves 2 and 3. Win handles Windows verification and landing.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-394 | — | Default render range | The branch log at `80de70f8` has no export reading. | blocked on the Live 12 observation |
| SC-395 | `basics/sc-395-missing-assets` `56797778` (local) | Offline exports allow missing clips and plug-ins to play as offline playback does. Engine and engine-win. | Linux Debug build setup in progress. | blocked pending Linux verification |
| SC-396 | `basics/sc-396-export-settings` `9b202d44` (local) | The export dialog uses Settings defaults, remembers session choices and refreshes the range. Window and window-fn. | Changed translation units passed MSVC Debug syntax checks. | blocked pending Linux verification |
| SC-397 | `basics/sc-397-stem-export` `3eaab215` (local) | The app queues post-fader files for all or selected audio-bearing tracks. Return and Main processing for isolated stems still needs the engine's ViaMain route. Window and window-fn. | Regression cases cover both modes and track selection. | blocked pending Linux verification |
| SC-398 | — | A local draft uses the existing JUCE codecs for AIFF and FLAC. The MP3 encoder still needs a distribution decision. Window, window-fn and engine-win. | Regression cases cover AIFF and FLAC publication. | blocked on MP3 packaging and Linux verification |
| SC-399 | — | Active export safeguards are in progress. | — | In progress |
| SC-400 | — | Export progress through preroll and normalization is in progress. | — | In progress |
| SC-401 | `basics/sc-401-export-messages` `1d7accb3` (local) | Plain-language messages and the three Shaped names from ADR-0250. Window. | UI regression cases cover the renamed options and refusals. | blocked pending Linux verification |
| SC-402 | `basics/sc-402-export-sample-rates` `dccc294a` (local) | 176.4 and 192 kHz are offered for any project. Window. | UI regression case selects both rates in a lower-rate project. | blocked pending Linux verification |

The session is running a temporary Debian VM because the available host is Windows. Item branches
stay local until the required Linux build and suites pass. The landing branches will form a chain
per the updated howto on main.

## For the director

- For SC-398, should installers ship a pinned LAME encoder for MP3, or should MP3 use a
  system-provided encoder? I recommend shipping a pinned LAME build so a normal installation
  can export MP3 on every platform.
