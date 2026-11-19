# Window lane: the look, from the agreed sheets (18 November 2026)

From win. Your standing prompt (`2026-10-27-window-lane.md`), the worklist
(`collab/window/worklist-2026-11-05.md`) and the note of 16 November (`2026-11-16-window-lane.md`)
stand. This note says what changed on 17 and 18 November and what comes first now.

You are free to use anything that helps: install tools, download what you need, build, run, probe,
measure, write harnesses, render your own pictures and compare them with the sheets pixel for
pixel. How you do the work is yours.

## What changed

- **The director does not accept the look of the window as built** (17 November, ADR-0243). His
  words: "it looks like an attempt to put the qualities of what i want into the most generic
  looking things possible"; "we look more like Ableton with reaper Collab"; "things should look new
  with design style in mind". Nothing is wrong with what the window does. Live stays the base; how
  it is drawn changes.
- **The look is now settled with him on sheets, and the first five are agreed** (18 November,
  ADR-0244). Read, in this order: `collab/window/design/README.md`; its pictures
  (`language.png`, `arrangement.png`, `mix-view.png`, `focus.png`, `analysers.png`); ADR-0243 and
  ADR-0244 in `docs/DECISIONS.md`; rows DF-W20 to DF-W27 in `docs/DEFERRED.md`.
- **Every number of every picture is in `collab/window/design/src/`.** Each picture is one
  1920x1080 SVG built by an HTML page; open the pages in a browser and read their source.
  `lib.js` holds the shared parts you will build first: the colour table `C`, the ring knob
  (`knob`, with its states), `btn`, `grp` (a rounded group), `pill`, `box`, `chooser`, the icons,
  and `topBars` (the Control Bar as agreed). `language.html` is the language sheet,
  `arrangement.html` the Arrangement.
- **A sheet changes only by the director's word.** Where a sheet and its source do not answer a
  question, ask win for a drawing and say exactly what you need drawn; do not draw your own answer
  into the build. Where the README's words and a picture differ, the words win.
- **The rule for everything ADI clones** (his words, 18 November): "they should look the same as
  the plugins we are cloning (about the same at least)". It reaches you through the Info box's
  analysers and the scope; those components are the Mix View seat's. Do not design a look for one.
- **The Q and L buttons are gone from every channel** (#626). `MixerStrip::switches()` is three
  long and `kMixerTop` is 19. No channel carries a button for a Mix View function, ever.
- **The Clip View and its Note Editor are on main**, built by win (#586 to #624). Their look has no
  sheet yet; leave their layout alone.
- **The Mix View seat rebuilds the Mix View from its sheets.** It takes the whole window, so that
  seat will work in `ui_shell.*` (what the Arrangement | Mix View switch shows), `ui_stack.*`,
  `ui_devices.*` and the `mix_*` files. The switch itself is one control bound to the existing
  `ToggleMixView` command: whichever of you lands first adds it, the other rebases.
- **Your E5 is open as #628** (`window/import`): it had been pushed without a PR, and win opened it
  on 18 November. E3 (#585) and E4 (#590) are on main. Your picture for C5 is in your log; win
  takes it to the director under the same review, and nothing of C5 is built before he answers.

## What to do, in order

One `window/<topic>` PR per step, base main.

1. **The theme and density tables (DF-W24, first).** No colour, radius or size literal stays in the
   window's drawing code: every one comes from one theme table and one density table in
   `src/adi/ui/`. Themes: Dark, the default, as Live 12's dark and as `lib.js`'s `C`; today's mid
   grey; Light; and the darker, cooler set drawn for the Mix View. Densities: Compact, the default,
   with Live 12's sizes; Open. Both are settings (ask win for the registry rows as an N row and
   build against a default meanwhile). The Mix View seat uses the same two tables: say their names
   and how to read them in the PR's first lines, so win can pass it on.
2. **The Control Bar (DF-W24).** Live's controls in Live's order, drawn in rounded groups as on
   `language.png` part A and at the top of `arrangement.png`: the tempo group, the key and scale
   group, the transport, the loop and punch set, undo and redo, the Arrangement | Mix View switch,
   the draw and keys group, MIDI and CPU.
3. **The shared controls (DF-W24),** as `language.png` parts B, C and D: a button and its states
   (rest, pointer over, pressed, on, on for record, on for a value, disabled); the ring knob in
   three sizes with its states (from the centre, automated, modulated, disabled, a second colour);
   the value box, the chooser, the one-of-three, the switch, the tabs. One implementation of each,
   used by every surface: the track rows, the Mixer, the Device View, the Clip View, the Browser,
   the dialogs. Layouts stay; the controls in them change.
4. **The Arrangement from its sheet (DF-W25).** A track inside a group is as wide as its group and
   is not indented; it carries a line in the group's colour at its left as in Live, and a second
   line for a group inside a group. Over the track names: the switch of sides and the "Show"
   chooser of groups, drawn as on the sheet. A clip always shows what is in it; why some clips
   looked empty on 17 November is win's to find (DF-W21), and if the cause is in the drawing, win
   hands it to you with the case.
5. **DF-W9, the browser's Plug-Ins,** as the 16 November note describes it: a MIDI clip is still
   silent until an instrument can be put on its track from the window.
6. **DF-W19** (a parameter shown as a menu in the Device View drops most choices) **and DF-W18**
   (the reference-audition check measures real time).
7. **The rest of the 16 November list,** in its order: E6, recording from the window, C7, B6 and
   B7, the Delay Compensation item, F3, then the surfaces that landed with the engine's stack.

## What waits, and for what

- **A device's own panel** (DF-W23): each Pd device gets a sheet first, one for each device. Until
  then a device panel changes only through the shared controls of step 3. "nothing in adi daw can
  look like a generic design-less device": do not add another panel that is a box of dials.
- **The EQ, the compressor and the limiter in the Device View** (DF-W26): they will be the Mix
  View's own panels, after that seat has built them. The Pd EQ Eight, limiter and compressor stay
  ordinary devices.
- **The Info box** (DF-W22): its analysers and their switch are the Mix View seat's.
- **The Clip View, the Browser's layout, menus, dialogs, Settings:** no sheet yet. They take the
  tables and the shared controls and nothing else.

## How it lands

- As before: headless checks, parity rows, and the PR's first line saying what the director can
  now see or do. For a look PR, put your render of the built surface beside the sheet's picture at
  the same size in the PR, and list every golden you rewrote.
- Win proves each on MSVC `/WX`, merges, logs, and shows the director the built surface against the
  sheet. He rules on what he sees. The app is never driven on his desktop.
- Anything you postpone is a row in `docs/DEFERRED.md` in the same PR (ADR-0239).
