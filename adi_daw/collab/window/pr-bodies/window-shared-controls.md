The director can now see the agreed buttons, ring knobs, value boxes, choosers, one-of-three, switches and tabs of the language sheet across the window: one painter for each, drawn in every state the sheet draws, in the Mixer, the track rows, the Device View's knobs, the Clip View, the Status Bar, Settings and the Browser (DF-W24 step 3; ADR-0244 d2; `collab/window/design/README.md`, language.png parts B, C and D).

**For the Mix View seat:** the painters are `src/juce/ui_controls.hpp` (namespace `adi::ui::controls`): `button(g, r, state, label)` with `stateOf(on, over, down, enabled, lit)` and `labelColour(state)`; `roundButton`; `group`, `separator`, `groupCell`; `ringKnob(g, centre, radius, value, KnobLook{fromCentre, automated, disabled, over, modulation, colour})`; `valueBox`; `chooser` (with `enabled`); `oneOf` and `oneOfCell`; `toggleSwitch`; `tabs` and `tabCell`. Their colours are the theme's new roles (`buttonRest` to `tabOnEdge` in `ADI_THEME_ROLES`), their sizes the density's. JUCE's own buttons, switches, choosers, rotaries and value bars already draw through them (`LiveLook`), so a panel built of JUCE controls takes them without change.

![The language sheet beside the window as built, both 1920 x 1080](https://github.com/arieladi/Adi/blob/window/shared-controls/adi_daw/collab/window/pictures/2026-11-18-shared-controls.png?raw=true)

Left: the language sheet (`language.png`), 1920 x 1080. Right: the window as built, 1920 x 1080 at real size, Dark, Compact, on a song made in it: Drums (a MIDI beat: kick, snare, hats, a fill in the eighth bar), Bass on the chords' roots, Melody (a phrase in A minor and its answer), Chords (Am, F, C, G, each held a bar), and Drum Loop and Pad, two WAV files dropped on their tracks through the window, so their waveforms are the files'. The Melody track is chosen: the Device View shows its Auto Filter and Echo with the ring knobs; the track controls show the shared choosers, the In / Auto / Off one-of-three, the buttons (the track activators lit, Solo and Arm at rest) and the value boxes. The scene that draws it is `showcase` in `tests/test_ui_shell.cpp`, run with `ADI_UI_SHOWCASE` set to a folder.

What it does
- **The button** (part B): a rounded box, rest, pointer over, pressed, on (the accent), on for record (red), on for a value (blue), disabled; its label in the ink when lit. JUCE's TextButtons draw it; a button lit in a colour of its own (a track's activator) keeps that colour in the same shape.
- **The ring knob** (part C): the value is the ring, from the start or, two-sided (pan), from the centre; a lit dial under the pointer; a red dot when automated; a thin second arc when modulated; grey when disabled; a second colour. Every JUCE rotary draws it. A surface marks a rotary automated, or gives it the second colour, with the slider properties `adi.automated` and `adi.secondColour`; no surface sets them yet, so the Device View's knobs show neither until their panels' sheets.
- **The boxes** (part D): the value box (a dark well filled in the value's colour, every value bar, the Mixer's volume and pan included); the chooser with its arrow (every ComboBox, the In/Out choosers, Rec-Q and Play-Q, the Clip View's); the one-of-three (In, Auto, Off), each choice as wide as its label; the switch (every ToggleButton); the tabs (the Clip View's tool tabs, the Settings pages).
- **The surfaces:** the Mixer (choosers, the monitor's one-of-three, the clip hold, Arm, the ms box, H and W, the selectors, the view strip), the track headers (Rec-Q, Play-Q), the Arrangement's top buttons (Set, the locator arrows, Automation Mode, Lock Envelopes, the project view button), the Clip View (every value box, chooser, switch, tool and tab), the Status Bar (the Clip View selector, the round buttons, the options arrow), Settings (the page list as tabs) and the Browser (its round buttons). Their layouts are unchanged; only how each control is drawn changes.

Not done
- **The Control Bar** is step 2, which waits on win's answers in the log (the key and scale group, where Nudge, Session Record and the key-map Key go, its height).
- **The Mix View seat's files** (`ui_devices.*`, `ui_stack.*`, `ui_mixview.*`, `ui_analysis.*`, `mix_*`) draw their own hand-made controls still; their JUCE controls already take the shared look through LiveLook.
- **Pointer over and pressed** show on every JUCE control; the hand-drawn controls that never tracked the pointer (the Clip View's, the Arrangement's top row) show rest and on only. No mouse tracking was added.
- **Two glyph buttons** (the Mixer's flip and track-height buttons) stay bare glyphs on the band, as the sheet's track row has no box there.
- **The Arrangement's own layout** is step 4: the bar over the track names, the group lines, and "Drop Files and Devices Here", which the picture shows half under the Pad clip.

Goldens rewritten (Dark): `root-default`, `root-playing`, `root-swapped`, `arrangement`, `devices`, `mixer`, `mixer-view`, `live-parity`, `clip-view-midi`, and `root-default-midgrey`, which is now the Mid grey window with the shared controls (step 1 matched the pre-sheet window pixel for pixel; its colours are still those).

Review
An adversarial review (correctness, behaviour, tests, a skeptic per finding) confirmed eight defects. All are fixed:
- **How a lit button is lit:** it was read by comparing the button's stored on colour with the theme's, so after a theme change a lit Solo or Arm fell out of the shared states. A button now says its kind itself (`adi.lit`: "record" or "value"; Solo and Arm do), the colour only as a fallback for buttons that say nothing; the "colour of its own" branch is gone (no button had one).
- **A button's own on-text colour** (the Mix View's tabs set one) was overridden by the ink; it is kept when the button sets it.
- **A disabled switch that is on** (a Settings switch of another scope) looked off; it is now drawn lit and dimmed (`DisabledOn`).
- **The switch's label** moved some 15 px right, cutting labels in layouts sized for the old square; the pill is smaller, its label back near where the layouts put it.
- **The knob's dial in Mid grey and Light** was lit from its darker stop (Dark's values copied); both stops now sit on each theme's ramp, the lit one lighter, checked in every theme.
- **The chooser** ended long labels in "…" where Live cuts them plain ("All Channe", Live 12 manual, Figure 6.1); it cuts them again.
- **The monitor's hit test** still used the old boxes; it uses the cells the one-of draws.
- **A test** checked the density's half of the settings while the density was already Compact; the look goes back to Open first.

Tests
- Pd on:
  - `adi_ui_shell_tests`: 2751 checks, 0 failures: a button's states from what JUCE knows (rest, over, pressed, on in each of its three colours, disabled); a lit button's label in the ink, a resting one's in the text, a disabled one's faint; the one-of-three's cells as wide as their labels, filling the box; a button drawn at rest and on for record in the sheet's colours.
  - `tools/test_all.sh build-juce`: PASS, 59459 checks across 245 suites, validators clean, after merging main.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- The shadow and clang warning sweeps are clean on this PR's lines. In `tests/test_ui_shell.cpp` they flag two of main's own lines (5721, in the Clip View checks; 10845, in the Mix View picture).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
