The director can now see the Arrangement's bar over the track names as the Arrangement sheet draws it, and its groups drawn as the sheet draws them: a track in a group as wide as the group, with a line in the group's colour at its left, and a second line for a group inside a group (DF-W24 step 4, DF-W25; ADR-0244 d8; `collab/window/design/README.md`, "The Arrangement"). Stacked on `window/shared-controls` (step 3), whose painters it draws with; it carries main's merge.

![The Arrangement sheet beside the window as built, both 1920 x 1080](https://github.com/arieladi/Adi/blob/window/arrangement-sheet/adi_daw/collab/window/pictures/2026-11-18-arrangement-sheet.png?raw=true)

Left: the Arrangement sheet (`arrangement.png`), 1920 x 1080. Right: the window as built, 1920 x 1080 at real size, Dark, Compact, on the same song as step 3's picture, now grouped. **Drums** is a group holding **Kit** (the MIDI beat) and a group inside it, **Loops**, which holds **Drum Loop** (a WAV file dropped through the window); **Bass** and **Melody** stand alone; **Keys** is a group holding **Chords** (held chords) and **Pad** (a second WAV file). Kit and Loops carry Drums' pink line at their left; Drum Loop carries two lines, Drums' and Loops'; Chords and Pad carry Keys' green. Over the names, level with the beat ruler, the bar: Flip Sides, Show ("All groups"), the I·O, S, R and M sections lit and D not, H and W, Automation Mode, Lock Envelopes. The scene is `showcase` in `tests/test_ui_shell.cpp` (`ADI_UI_SHOWCASE`), which writes `adi-arrangement.png`.

What it does
- **The bar over the track names** (`NamesBar`, `src/juce/ui_arrangement.hpp`): one row over the names, their controls and the strip down the edge, 26 high as the sheet's, its buttons the shared ones (language.png part B). Left to right:
  - **Flip Sides**, which was the strip's arrows: the track names to the window's other side (ADR-0080).
  - **Show**, which was Bitwig's project view button: the shared chooser, "Show" then the view chosen, "All groups" or the group's name (numbered as its title is when the name begins with #), in the accent while a group is chosen; its menu as before.
  - **The sections**, one group: I·O, S (the sends, new here: Hide/Show Sends), R, M (shows its state only, as before) and D, which were the strip's four round selectors.
  - **H and W**, one group: Live's Optimize Arrangement Height and Width. They were drawn at the Overview's end and could not be clicked; now they act.
  - **Automation Mode and Lock Envelopes**, which were drawn under the names: at rest, as the sheet draws Lock; a click says they wait for the engine's automation lanes, as before.
  - **Narrow:** a bar too narrow for every part (the track controls hidden, a small window) keeps Flip and Show and leaves out from the end: Automation Mode and Lock, then H and W, then the sections. Show drops its "Show" when it has less than 80 px.
- **The groups' lines:** 4 wide at a step of 5, as the sheet's, down the whole row, so a group's line runs unbroken past its tracks' feet.
- **The Overview** runs over the lanes only, as on the sheet.
- **The drop area's words** ("Drop Files and Devices Here") are faint, as the sheet draws them, and only where the area holds them: they were drawn half under the last clip when the tracks filled the lanes.
- **The strip down the edge** keeps its two view switches and Bitwig's track height button, under the bar.

Not done
- **Where the sheet is silent** (in the window log, for the director): Set and the locator arrows stay under the bar where Live has them; the strip down the edge stays until the Control Bar's Arrangement | Mix View switch (step 2); the names column and the track controls keep Live's widths (95 and about 190), narrower than the sheet's 128 and 264, so the bar's buttons are a little narrower than drawn.
- **The row's own layout** (the In/Out choosers, the mixer buttons, the meters as the sheet draws them in the header) is the track rows' and the Mixer's sheet, still to come.
- **The Info box and the Device View's panels** are DF-W22 and DF-W26.

Goldens rewritten (Dark): `root-default`, `root-default-midgrey`, `root-playing`, `root-swapped`, `arrangement`, `devices`, `mixer`, `mixer-view`, `live-parity`.

Review
An adversarial review (correctness, behaviour, tests, a skeptic per finding) confirmed eight defects. All are fixed:
- **A narrow bar** (the track controls hidden, three findings): Show had no width and the parts after it fell outside the bar, so the project view could not be opened. The bar now leaves out from the end and keeps Show at least 80 px where it can; a check hides the track controls and looks.
- **Show named a group made by Group Tracks "# Group"** where its title and the menu say "3 Group": it numbers it as they do.
- **Automation Mode's tooltip promised the A key**, which nothing binds: gone.
- **The bar covered Set's top 2 px**: it ends above it.
- **A test variable hid the scene's own `x`**, which MSVC's /W4 rejects as C4456 under `/WX`: renamed.
- **The H check passed without H doing anything** (it shrank one track and measured another): it measures the track it shrinks, and checks the height changed first.

Tests
- Pd on:
  - `adi_ui_shell_tests`: 2757 checks, 0 failures: the bar spans the names, their controls and the strip, level with the ruler, Flip first and saying Flip sides, flipped and back; its parts in the sheet's order inside it; S hides and shows the sends and is lit while they show; W and H do what Optimize Arrangement Width and Height do; Show says All groups, and the group's name, lit, when one is chosen; with the track controls hidden, Flip and Show stay and every part kept is inside the bar; Automation Mode and Lock Envelopes say what they wait for; a frame after a new generation allocates nothing, groups and all.
  - `tools/test_all.sh build-juce`: PASS, 59459 checks across 245 suites, validators clean, after merging main.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- The shadow and clang warning sweeps are clean on this PR's lines. In `tests/test_ui_shell.cpp` they flag main's own two lines, the same as on step 3 (5764 and 10929 here, in the Clip View checks and the Mix View picture).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
