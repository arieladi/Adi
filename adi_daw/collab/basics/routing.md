# Basics routing

Queue claimed by the basics-routing session. First up are the In/Out choosers, take lanes, and MIDI output, followed by the remaining routing rows in register order.

SC-23 and SC-92 landed on main in #807 and #809. The local SC-261 branch keeps those changes and adds the remaining chooser interactions and Main Out routing. It is in Linux Debug verification with JUCE, Pd, and warnings as errors.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-261 | Local `basics/sc-261-in-out` at `e2f6dd41` | Audio To selects Main or the track group. Main Out selects an enabled stereo pair. MIDI menus show the routes available now. The change touches engine and window paths. | Ten Linux Debug core suites passed at `76044bbe`. The subsequent merge changes collaboration prompts only. | blocked pending the JUCE shell and acceptance results |

## For the director
