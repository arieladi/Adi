# The window lane — a fresh seat resumes (3 November 2026)

You are the **window** lane of ADI (`adi_daw` in the `arieladi/Adi` monorepo), in a new session
because the last one reached its usage limit. The standing prompt is unchanged and you work under
it: `adi_daw/collab/prompts/2026-11-01-window-lane.md` (what you own, what you read, your queue,
the rules). This note is only the state you resume from. Read it, then the standing prompt, then
the files it names, in its order, with two files added at the front:

1. **`adi_daw/collab/director.md`, first.** The director opened the Windows build (`add5102`) on
   2 November and wrote seven remarks; win recorded them there with its reading of each and the
   window item each becomes. R4 and R5 are answered by #339, #347 and #350 already in your chain.
   **R1, R2, R3 and R6 are your next items, before anything new** (LANES §4 window item 4: the
   director's remarks on the window come first). In short: a click on a plain track's triangle
   or on the automation and lock buttons must say in the Status Bar what it waits for (N26); the
   divider under each track header and the resize cursor over it (parity 7.3); and the Mix View's
   switch, detach and re-dock as one item: no flash on switch, the detached window keeps its
   place and size, docking returns it where it was, the button and F4 toggle both ways.
2. **`adi_daw/collab/window.md`, your own log and needs table**: resume from its top entry.

**Where your chain stands.** Your branches are all pushed and their PRs open; nothing of yours is
lost. #331 to #352, #356 to #358 and #361 to #370 are a single stack, each on the one before, from
`window/select-all` (#331, on `window/range-selection`, merged) to `window/latency-compensation`
(#370). None has merged since #329 because GitHub's macOS runners are the bottleneck today (a leg
of #331 sat queued eleven hours and was cancelled by the runner; win reran it). Win merges the
stack in order as each PR's last leg goes green, and tells you in `collab/win.md`. Until then,
**branch new work from the top of your stack** (`window/latency-compensation`), not from main, and
name in each PR body the PR it stands on, as you have been doing. When win has merged, rebase the
rest.

**#330 (`window/new-project-root`) is yours to rebase**: it conflicts with main in four
interleaved hunks of `tests/test_ui_shell.cpp` (its `newProject()` beside main's `sendKnobs()`,
`audioDrop()`, `rangeSelection()` and `mixViewShell()` from #325 to #329). Win touched nothing on
the branch; resolve it, push, and win merges it when green.

**Win's three needs are built and in PRs, stacked on each other and on win's #353:**

- **N14, #354:** the settings registry speaks Live's ten tab names (in Live's order, then ADI's
  pages), each `Setting` carries Live's `section` within the tab, Live's labels, and
  `sync.showLinkToggle` is on. `settings_tabs.hpp` already takes the registry's section first; its
  own `groupFor` table is now the fallback for ADI's unsectioned settings only. Three placements
  are win's, marked `win's` in `liveSections` for you to strike or keep: `ui.zoomOnSelection`
  under Display, `ui.showTooltips` under Display Options, the knob, slider and value-box modes
  under Mouse and Pen Input. Your tab order still draws the tab's own group first and the sections
  after; that is your call.
- **N4, #359:** `window.zoom`, a `Table` setting on Display & Input's Display section under Live's
  label Zoom Display: a row per window per display, `"<window>@<display>"`, the display named by
  geometry and DPI exactly as `ui_floating.cpp` names the monitors. `windowZoom` and
  `setWindowZoom` in `store.hpp`. On launch read the zoom for the display the window opens on and
  start there; on Ctrl/Cmd + and - keep it and save; the same for the mixer window, the Mix View
  detached and each floating window under its own key (suggested: `main`, `mixer`, `mixview`,
  `floating:<key>`). The settings row for a table is read-only.
- **N20, #360:** `audio.channelConfig` is now App scope, one record per audio device under the
  device's name, as Live keeps its Input and Output Config (manual 17.2);
  `channel_config.hpp` is the shape: mono channels and stereo pairs per side, numbered from 1, each
  with its switch and name; `defaultChannelConfig(inputs, outputs)` is Live's default (1, 2 and
  1/2 on); `activeInputs()`/`activeOutputs()` are the channels to open the device with;
  `displayName` is Live's "1 (mono)" and "1/2 (stereo)". Your half, after the director's remarks:
  the Audio page's Input Config and Output Config dialogs (a new window, so its pictures go to the
  director first, LANES §5), and `ui_main.cpp` opening the device with the record's active
  channels instead of `initialise(0, 2, ...)`. The engine's half is recording from a mono or a
  stereo input.

Strike N4, N14 and N20 in your needs table the day their PRs merge (win writes the merge in
`collab/win.md`). Everything else in the standing prompt holds: one branch per item, one PR per
item opened as soon as tests are green, never merge, never touch main, never `git add -A`, zero
warnings on GCC, Clang and MSVC `/WX`, the UI reads the snapshot only, log every finished or
blocked item in `collab/window.md` newest first, ask the director nothing LANES §5 does not list.
Usage limits are not blockers: stop cleanly and resume from your log on "continue".
