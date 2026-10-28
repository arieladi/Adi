# window — log

The window lane (ADR-0208, `docs/LANES.md` §3b): a Claude work seat of its own, building step 7,
the first window. It owns `src/adi/ui/**`, `src/juce/ui_*`, `src/juce/app*`, their tests,
`docs/STEP-7-PLAN.md` and `docs/UI-ARCHITECTURE.md`. Standing prompt:
`collab/prompts/2026-10-27-window-lane.md`. Branches `window/<topic>`, one per item, cut from
`origin/main`; the six rebased PRs stack in the plan's order, each on the one before. Only the
window lane writes to this file. Newest entry at the top.

**Why this log has its own branch and PR (`window/log`).** As the engine lane's: item PRs never
touch this file, so no merge of one turns the next red. Each entry names its PR.

**Where it runs.** The director's Mac (Apple Clang, macOS 26, 8 cores, 8 GB), in a worktree
outside the OneDrive-synced folder (`~/adi-window`). Built as `ci.yml` builds on macOS: the
default generator (Makefiles; there is no Ninja on this Mac and the workflow's macOS legs do not
use it), `-DCMAKE_BUILD_TYPE=Debug -DADI_WERROR=ON`, and a second tree with `-DADI_WITH_JUCE=ON`.
MSVC `/WX` and Linux are not run here; the `Step 7 UI` workflow runs all three.

**The Live 12 manual.** `reference/DOCS/_text/Ableton_live12/` is not on this Mac. The parity
checklists are drafted from `adi_daw/References/live12-manual-en.pdf` in the director's main
checkout, read to text locally; pages cited are the PDF's. Where the A2000's rows exist, they win.
The checklists live in `collab/window/parity-*.md`, one per item, in that item's PR.

---

## Needs from other lanes (open)

| # | Need | Owner | Why the window needs it | What the window does meanwhile |
|---|---|---|---|---|
| N1 | `Store::create` writes no `project` row, so `project.setName` updates nothing on a new file and the snapshot falls back to 48 kHz (ADR-0203 d21 says 44.1 kHz) | engine | STEP-7-PLAN §3.0, still open on main `a57d47a` | New makes no project row; the Take Snapshot prompt names the project after its file |
| N2 | A bootstrap for a new project (the project row and the Main track) that is the history's root, not an undo step (Live manual 5.4.2) | engine | #198 wrote both rows with raw SQL from the UI, which the plan forbids | New makes the Main track through `track.create` and the UI's `OpSubmitter::protectRoot` refuses to undo that step |
| N3 | Save As, a true one that writes a new `.adi` and carries on in it (ADR-0203 d6, LANES §3 item 6) | engine | Ctrl/Cmd+Shift+S | the key and the menu item exist; the Status Bar says Save As waits for the engine |
| N4 | An app-scoped home for each window's interface zoom, per window and per display (ADR-0129 d3; ADR-0203 d11 "kept with the application") | win (the settings registry) | Ctrl/Cmd + and - zoom each window, but the zoom is not remembered across launches | held per window for the session |
| N5 | An undoable path from a file and a track to a clip that plays: `importMedia` that captures without committing (so media and clip are one undo step) and fills `sample_rate`/`channels`/`frames`, and an op pair that attaches an `audio_clips` row (STEP-7-PLAN §3.0). #199's commit `3feee62` has a working version (`prepareImport`, `clip.attachAudio`/`clip.detachAudio`) in `media_ops.*` and `ops_catalog.cpp`, for the engine lane to lift | engine | dropping audio onto a track, 7.3's headline gesture | the arrangement accepts the drop and the Status Bar says it waits for the engine; nothing is written |
| N7 | A UI producer into the edit capture: `ParamOps` has one ring per plug-in and none for the window, so a drag in the device strip cannot become an op whose first touch emits its opener (ADR-0124, ADR-0181 d5). #203's `537c56d` has `ParamOps::uiEvent` (its own `ParamEditCapture`, begin/value/end, reusing the opener machinery) for the engine lane to lift | engine | editing a parameter from the strip, 7.4 | the strip shows every value; a gesture is refused and the Status Bar says why |
| N8 | A plug-in's editor from the hosts: VST3 (`IPlugView`, through JUCE's `createEditor`) and CLAP (`clap_plugin_gui`), as a component the floating host can place, and the editor's own size | engine (`src/juce/*host*`) | Live's wrench toggle and Ctrl/Cmd+Alt+P; the floating host's main users (ADR-0076) | the host is built and tested; nothing opens a plug-in window |
| N9 | The scalar meter of UI-ARCHITECTURE §9, one `{peak, rms}` per strip written by the audio thread (#205's `9e4f4af` has a `StripMeter` in `engine/mixer.*`) | engine | metering a hundred strips without reading a tap each | each visible row reads the newest 1024 frames of its track's post-fader tap |
| N10 | A Learn target for a mixer control that has no automation lane yet (#205's `learnControllerTarget` in `controller_maps.*`) | engine | MIDI Learn on lane headers, P2 (ADR-0203 d7) | nothing; Live learns through MIDI Map mode, which comes later |
| N11 | The snapshot to carry the send rows (`routing` with `kind='send'`: source, return, gain, pre/post) | engine | one send knob per return on every row (ADR-0203 d3) | no send knobs; the routing ops exist and are ready to be called |
| N12 | Split Stereo Pan: a pan mode and two pans per strip in the format and the mixer (ADR-0203 d9) | engine | the pan control's context menu | the menu shows the mode disabled |
| N13 | A loudness meter's DSP: momentary, short-term and integrated LUFS and true peak, cheap enough for the Main track at all times (ADR-0203 d14) | engine or devices | the Main track's loudness meter | none yet |
| N14 | The registry's pages in Live's ten tab names and order, then ADI's own (ADR-0203 d11, Master Reference V.2): the Display & Input / Theme & Colors split of "Look & Feel", Link, Tempo & MIDI, Record, Warp & Launch, Licenses & Updates | win (`src/adi/settings/registry.cpp`) | the Settings window lists `settings::pages()` as they are, by design: one source for the left-hand list | the window shows the registry's pages in the registry's order |
| N15 | ADR-0104's library index (the app-scoped store of samples, presets and devices) and a catalogue of ADI's own devices by category, with a way to insert one (`device.insert` needs a plug-in reference row and the track's chain, neither of which the UI can make) | engine, devices | the browser's list (Live's folders under each category, double-click or drag to load) | the browser shows its sidebar and the list's header; a click on a category says it is not listed yet |
| N16 | The channel slots for ADI-Q and ADI-L on every channel and group (ADR-0204 d1, LANES §3 item 7): present but off, their on/off and ADI-L's mode in the snapshot, an op to switch each, and the engaged EQ's curve for the window to draw | engine (slots), clones (the plug-ins) | the Channel EQ window's switches and its display; the track rows' Q and L | every slot is shown off; a switch says what it waits for |
| N17 | The ops of phase C's window parts: clip fades (handles, Create Fades on Clip Edges off, d7), Insert Silence, and the import and bounce prompts' actions (import needs N5; bounce needs the render path) | engine | range selection's commands, fade handles, the two prompts (LANES §3b item 3) | nothing drawn that cannot act; the parts wait for their ops |
| N18 | A device's copy the window can read: what `device.remove`'s inverse captures (the plug-in reference, the stored params, the state's hashes, the panel and the route), from the snapshot or a read the UI may call, so a paste is a `device.insert` with a new id in front of the selected device (Live manual §23, p. 443) | engine | Cut, Copy, Paste and Duplicate in a device's menu and the Edit menu (§23.2.1) | the menu lists Delete and Rename; the four wait |
| N19 | The transport's metronome and loop: `transport.setMetronome` and `transport.setLoop` are in the catalogue but apply nothing (`transportNoop`); the snapshot carries neither the metronome's state nor the loop region, and nothing clicks or loops | engine | the control bar's Metronome and Loop switches, Live's O key (§42.20) and Ctrl/Cmd+L, the loop brace (§6.6, §42.16) | the switches are drawn and say they are not built; nothing is journaled for them |
| N6 | The snapshot to carry what the arrangement draws: a track's `kind` (to place Main last, as Live's arrangement does), a clip's `name`, and the time-signature map (bars in the ruler). #199's `3feee62` adds all three to `engine/snapshot.*` | engine | the ruler and the clip labels | the ruler counts bars in 4/4 and says so in its tooltip; clips are drawn unnamed; Main is listed where the snapshot puts it |

---

## 2026-10-28, cloud, night, later — Live's arrangement keys [#282](https://github.com/arieladi/Adi/pull/282) `window/arrangement-keys`

**#282, stacked on #281** (parity-7.3-arrangement rows 24 and 25, new). Two rows of §42.16 that
need no op:
- **Move Insert Marker to Playhead Position, Ctrl/Cmd+Shift+Space.** The insert marker becomes a
  point at the playhead, with no clip selected, so Space plays from there.
- **Adjust Height of Selected Tracks, Alt/Option + and -.** As Alt+wheel: the selected track, or
  every track with selected content. A 16-unit step, provisional; win to measure Live's.

Checks: shell 340, document 38, window state 49; `test_all.sh` 8266 across 97 suites PASS;
zero warnings; the shadow sweep is clean.

**CI at 07:29Z on #274 to #282:** no failure, no cancellation. Every finished leg passed: all
the Linux legs, Windows (MSVC and its GUI shell), and the macOS GUI shell on #274 to #277. The
macOS core legs wait 30 to 50 minutes for runners; #281 and #282 had just started.

**The stack from this session**, each on the one before and on #273:

| PR | Branch | What |
|---|---|---|
| #274 | `window/menu-bar` | Live's menus in Live's order, only commands that exist; Settings in Options off the Mac and in the Mac's application menu; Delete Track in Edit |
| #275 | `window/navigate-menu` | Navigate's focus moves: Control Bar, Arrangement, Device View, Browser; arrows between devices and sidebar labels |
| #276 | `window/menu-keys` | Alt and a menu's letter opens it, off the Mac |
| #277 | `window/browser-search` | Ctrl/Cmd+F to the browser's search |
| #279 | `window/settings-apply` | a fix: Settings changes reach the open windows at once |
| #280 | `window/tab-focus` | Use Tab Key to Move Focus, off by default |
| #281 | `window/follow` | Live's Follow |
| #282 | `window/arrangement-keys` | insert marker to playhead; Alt +/- track heights |

**Where to resume:** `window/arrangement-keys` (#282) is the tip. What the window can still do
alone:
- **The Mixer's keyboard:** Alt/Option+Shift+M to the selected track's row; Next and Previous
  Neighbor (Ctrl/Option+Tab) in the Navigate menu; a ring on its switches. Keys that change a
  value wait for win's ruling on how a non-parameter edit coalesces (parity-7.6 row 3).
- **The Audio page's Channel Configuration** (parity-settings row 7). Its Input and Output Config
  dialogs are a new window, so their pictures go to the director before more is built on them.

Phase C's window parts still wait for N17.

---

## 2026-10-28, cloud, night — Tab's switch [#280](https://github.com/arieladi/Adi/pull/280); Follow [#281](https://github.com/arieladi/Adi/pull/281)

**#280 `window/tab-focus`, stacked on #279** (parity-7.2-shell rows 13 and 21).
- Live 12's Use Tab Key to Move Focus (online §41.1.1) closes the Navigate menu, ticked from
  `lookfeel.tabMovesFocus` and writing it.
- Off, Live's default, the root takes plain Tab and Shift+Tab and moves nothing. Live uses the
  key for Session View, which is P2. On, JUCE's focus order moves on, as before.
- The Settings window's own Tab keys work either way.
- Wrap Tab Navigation has no setting yet (win's registry). Next and Previous Neighbor come with
  the Mixer's keyboard.

**#281 `window/follow`, stacked on #280** (parity-7.3-arrangement row 23, new). Live's Follow
(§6.2), which the control bar drew but answered "not built yet".
- The switch is lit when on. It is also a command in the Options menu, so the Mac has an
  Options menu now. It is kept per window in `ui_view`, off by default as the bar was drawn.
- While playing, the arrangement follows by `lookfeel.followMode`. By page, it jumps to put the
  playhead at the left once it leaves the lanes; by scroll, the playhead stays in the middle.
- **Pauses:** an edit through the window (arrangement, mixer, track commands, Undo and Redo), a
  sideways wheel scroll, an Overview drag, a click on the ruler.
- **Resumes:** playback starting or stopping, a click in the lanes or the scrub area.
- **Keys:** Option+Shift+F on the Mac. Off the Mac both Ctrl+Shift+F and Alt+Shift+F, since the
  manual gives Windows both.
- A device's edit does not pause it yet.
- **For win to measure:** Live's default for Follow in a new Set, which Windows key Live 12
  answers, and how Live shows Follow paused.

Checks: shell 325 (#280) and 337 (#281), window state 49 on #281, document 38; `test_all.sh`
8265 and then 8266 across 97 suites PASS; zero warnings; the shadow sweep is clean. Follow's
checks run with a real engine playing, with real clicks through the fake peer on the ruler and
the scrub area.

**CI** at 07:14Z: no failure on #274 to #280; macOS legs queued.

**N19 (new):** the transport's metronome and loop. `transport.setMetronome` and
`transport.setLoop` are in the catalogue but apply nothing, and the snapshot carries neither. So
the Metronome and Loop switches, the O key, Ctrl/Cmd+L and the loop brace wait for the engine.

**Where to resume** (superseded by the entry above): `window/follow` (#281) was the tip.

---

## 2026-10-28, cloud, evening — Ctrl/Cmd+F [#277](https://github.com/arieladi/Adi/pull/277); Settings that apply at once [#279](https://github.com/arieladi/Adi/pull/279)

**#277 `window/browser-search`, stacked on #276** (parity-7.2-shell row 20, new). Online §41.4.1.6
and §42.22: Ctrl/Cmd+F brings the focus to the browser's search from anywhere.
- It shows a hidden browser, then focuses the field with its words selected to type over.
- The field's hint now names the platform's key (Ctrl off the Mac).
- Partial: Live also selects the sidebar's All label, which ADI's Live 11 sidebar has not. The
  results wait for N15.

**#279 `window/settings-apply`, stacked on #277: a fix to #267.** The Settings window set and
wrote each change at once, but `AppCommands` read its five settings only at launch. So the Info
View, tooltips and their delay, Zoom on Selection and the shortcut preset did nothing until a
relaunch.
- `SettingsPanel::onChange` now runs after each value set and after Restore Defaults. The
  application answers it with `AppCommands::reload`.
- Every native window keeps its tooltips to the commands at each frame (`followTooltips`).
- Parity-settings row 6 now says so.
- #278 is not this lane's; the number was taken between the two.

Checks: shell 317 (#277) and 322 (#279), document 38, window state 48; `test_all.sh` 8265 across
97 suites PASS on each; zero warnings; the shadow sweep is clean.

**CI:** #274 to #277 had no failure at 07:01Z; macOS legs were still queued behind macOS runners.

**Findings in the settings registry (win's), for the record. No ruling is asked; the window
builds on what is there.**
- `lookfeel.tabMovesFocus` (default off) is Live 12's "Use Tab Key to Move Focus", an entry of
  Live's Navigate menu (§41.1.1). Today the main window moves the focus on Tab whatever it says,
  because JUCE moves it on any unused Tab. That is the next item.
- `lookfeel.focusOutline` (default on) says "Outline the view that has keyboard focus, with the
  view indicator of ADR-0112". ADR-0112 does not describe that indicator. The rings of #275 and
  of the Settings window are per control and do not read it.
- `lookfeel.followMode` (page or scroll) is Live 12's Follow Behavior. Whether Follow is on is
  not a setting.
- **A conflict in the manual itself:** Follow's key on Windows is Alt+Shift+F in §42.9 but
  Ctrl+Shift+F in §42.11, §42.12 and §42.16. Option+Shift+F on the Mac in all four.

**Where to resume** (superseded by the entry above): `window/settings-apply` (#279) was the tip.

---

## 2026-10-28, cloud, later still — Alt and a menu's letter [#276](https://github.com/arieladi/Adi/pull/276) `window/menu-keys`

**#276, stacked on #275** (parity-7.2-application row 13, new). Online §42.28: on Windows, Alt and
a menu's first letter opens that menu.
- Off the Mac, the window's key listener asks `menuForKey` which menu the key names, and opens
  it in the menu bar. JUCE's menus already take the arrows (through a menu and on to its
  neighbours) and Enter.
- Only Alt alone counts. Navigate's Alt+digit keys are untouched.
- On the Mac, Live's equivalent is Cmd+? menu search. macOS puts it in an application's Help
  menu, which ADI does not have yet, so row 13 is open there.

**Seen in the app itself**, the first time this lane has driven the app on Linux. ADI ran under
`Xvfb` with keys sent by `xdotool` and captures by ImageMagick's `import`, with `HOME` in the
scratchpad and the project from `adi_ui_document_tests --fixture`:
- Alt+E opens Edit (Undo, Redo, Delete Track, Rename); then Right opens Create.
- Alt+N shows Navigate's four entries with their keys.
- Alt+0 rings Link; Alt+5 then Down twice rings the sidebar and selects Sounds.

This is a cheap way to check, without the Mac, what a peerless test cannot (popups, real
focus). The captures stay in the session's scratchpad; nothing is committed.

Checks: shell 316, document 38, window state 48; `test_all.sh` 8265 across 97 suites PASS; zero
warnings; the shadow sweep is clean.

**CI:** on #274 every Linux leg and all three GUI-shell legs passed, the Mac's included, so the
Mac-only menu expectations hold. Its MSVC and macOS core legs were still running, and #275's
had just started.

**Where to resume** (superseded by the entries above): `window/menu-keys` (#276) was the tip.

---

## 2026-10-28, cloud, later — Live's Navigate menu [#275](https://github.com/arieladi/Adi/pull/275) `window/navigate-menu`

**#275, stacked on #274** (parity-7.2-shell rows 18 and 19, parity-7.4-devices row 15). Live 12's
Navigate menu (online §41.4.1, §42.2), with the four views ADI has. Option replaces Alt on the
Mac.
- **Control Bar (Alt+0).** The focus returns to the control that last had it, Link until one
  has (§41.4.1.1).
- **Arrangement View (Alt+2).** The canvas, whose arrows already move.
- **Device View (Alt+4).** The first device. The left and right arrows then move between
  devices (§41.4.1.5).
- **Browser (Alt+5).** The browser now takes the keyboard. Up and down move between the
  sidebar's labels, as a click selects (§41.4.1.6).
- A hidden Device View or browser is shown first.
- Where the keyboard put the focus, a control bar item or the browser's sidebar is ringed in
  the accent, as the Settings window's controls; a click leaves none.
- Session View, Clip View, the Groove Pool and the Learn View come with those views. The Mixer
  (Alt+Shift+M) comes when its controls take the keyboard.
- **For win to measure:** does Live show a hidden view on Alt+4 or Alt+5, and what colour is
  its focus ring?

Checks: shell 314, document 38, window state 48; `test_all.sh` 8265 across 97 suites PASS;
zero warnings; the shadow sweep is clean (it caught a shadowed `alt` in the new test).

**Found on the way:** JUCE calls `focusOfChildComponentChanged` only when the focus enters or
leaves a parent, not when it moves between the parent's children. So the control bar learns
which control had the focus from each item, not from its own callback.

**Where to resume** (superseded by the entry above): `window/navigate-menu` (#275) was the tip.

---

## 2026-10-28, cloud — Live's menu bar [#274](https://github.com/arieladi/Adi/pull/274) `window/menu-bar`

**CI on #265 to #273, read first.** Nothing is red. Every Linux, Windows, s390x, i386, fuzz,
validator and GUI-shell leg passed on all nine. At 06:20Z only macOS legs were still queued or
running, waiting on macOS runners. No fix was needed, so nothing was pushed to the stack.

**#274, stacked on #273** (parity-7.2-application row 11). Live 12's menus in Live's order, each
listing only the commands ADI has. They are one table in `ui_shell.cpp` (`menuBar`).
- **Settings moves to Options off the Mac.** The online manual §2.3 says "Settings can be found
  in the Options menu on Windows and in the Live menu on macOS". ADI had it in the File menu.
- **Settings on the Mac.** The Mac listed Settings in no menu; only Cmd+, reached it. It is now
  the application menu's (`applicationMenu`, through `setMacMainMenu`).
- **The Edit menu gains Delete Track before Rename** (§18.2: "Tracks are deleted using the Edit
  menu's Delete command"). It has no Delete key: Live's Delete acts on whatever is selected,
  clips included.
- **Menus with nothing to list are left out:** Options on the Mac; Navigate until its focus moves
  exist (§41.4.1, Alt+0 to Alt+8); Help until there is a Learn View (§2.2.1).

Checks: shell 303 (296 on #273 here), document 38, window state 48; `test_all.sh` 8265 across 97
suites PASS; zero warnings; the clang `-Wshadow-all` sweep is clean. The Mac-only lines compile
only in CI's macOS legs.

**How the cloud builds.**
- Ubuntu 24.04, GCC 13.3, 4 cores, Ninja, with the apt packages `ui.yml` installs.
- `fetch_external.sh --with-juce`, then one tree, `adi_daw/build-juce`, configured
  `-DCMAKE_BUILD_TYPE=Debug -DADI_WITH_JUCE=ON -DADI_WERROR=ON`.
- The UI tests run with `DISPLAY` unset, as `ui.yml` does. JUCE asserts in
  `juce_Messaging_linux.cpp:87` about 2000 times there, a full message queue in a peerless run;
  the checks pass.
- The tip's numbers match the Mac's: 8265 checks across 97 suites.

**Found, for the engine lane (not the window's to fix).** With `-DADI_WERROR=ON` and GCC 13,
four targets fail inside JUCE's own sources: `adi_audio_probe`, `adi_vst3_probe`, `adi_play` and
`adi_play_render_tests`.
- They compile JUCE's module sources with the project's warning flags, and GCC's `-Wconversion`
  fires in JUCE's headers (`juce_UMPUtils.h:78` and others).
- No CI leg builds them that way: the JUCE legs run on macOS and Windows without `-Werror`.
- The UI targets are unaffected. `test_all.sh` counts the same 97 suites: it globs the build
  directory, and the four would land in their own `*_artefacts` folders.

**The Live 12 manual here.** Ableton's online edition (2026-10-01). Its keyboard shortcuts are
chapter 42, the PDF's 41; rows written here cite "online §". The text is read locally with curl,
never committed.

**Where to resume** (superseded by the entry above): `window/menu-bar` (#274) was the tip.
- Next: Navigate's focus moves for the views ADI has, Live's Alt/Option+0 (Control Bar), +2
  (Arrangement), +4 (Device View), +5 (Browser) and Alt/Option+Shift+M (Mixer). Then Live's
  Alt+letter that opens a menu on Windows (§42.28).
- Phase C's window parts still wait for N17.

---

## 2026-10-28, late — the lane moves to a cloud session

The director is moving the window lane off the Mac to a Claude Code cloud session (environment
"Adi", this repository). Everything is pushed: the stack #246 to #273 and this log. Nothing was left
uncommitted in `~/adi-window` or `~/adi-window-log`.

**What the cloud does not have, and what follows from it:**
- **Live 11 on the director's Mac.** No new captures of Live, so a new window's pictures beside
  Live wait for the Mac. Pictures of ADI alone render headless through `ADI_UI_PARITY`.
- **The Live 12 manual's PDF.** It is in the director's main checkout, not in the repository, and
  the repository is public, so it stays out. Use Ableton's online manual
  (ableton.com/en/live-manual/12/); the checklists cite the PDF's page numbers.
- **The native macOS app.** `--smoke-settings` and the Mac's own menu run on the Mac. Everything
  else builds and tests on Linux as `ui.yml`'s Linux leg does, and CI runs all three platforms.

**Where to resume:** unchanged from the floating-zoom entry. First read CI on #265 to #273, where
the fix for the Linux and Windows failures was pushed. Then the menu bar's other menus (7.2 row 11).

---

## 2026-10-28, night — the stack's CI on Linux and Windows (#265 to #273)

CI showed what the Mac cannot:
- **Windows, #265 and #267 to #272.** MSVC's C4996 refused `std::getenv` in the parity render's
  hook, with `/WX`. It now reads `ADI_UI_PARITY` through `juce::SystemStats`.
- **Linux, #265 only.** The browser's last sidebar row ("Add Folder") was drawn under the AI tab,
  outside the text masks, so the fonts differed by 32 to 217 pixels in seven goldens. #266 already
  clips the rows to the sidebar; #265 now does too, and its goldens are re-recorded.

**How it went up the stack.** One fix commit on `window/live-look`. Each branch above got the same
`getenv` replacement, then a merge of the branch below with `-s ours`: each already carries the
clip and its own goldens, so its tree is kept. Nothing was force-pushed. Each branch's change
against what was pushed is only the `getenv` lines; each passes its shell checks (#266 265, #267
277, #268 281, #269 284, #270 285, #271 291, #272 291, #273 295). The tip passes `test_all.sh` and
the shadow sweep. #246 to #264 were green already.

**The lesson:** the Mac's Clang does not warn on `getenv`. In window tests, read the environment
through `juce::SystemStats`.

---

## 2026-10-28, night — a floating window's own zoom [#273](https://github.com/arieladi/Adi/pull/273) `window/floating-zoom`

**#273, stacked on #272** (ADR-0129 d3, parity-7.5 row 10). Ctrl/Cmd + and - pressed in the
Channel EQ window zoomed the main window, because a floating window passed every key to the main
one.
- Now an ADI view's window zooms itself, on the main window's steps (50 % to 200 %). Every other
  key there is still the main window's.
- The view lays out in logical units under a scale, as `ZoomHost` holds the main root. The steps
  are one function, `nextZoom`, which both use.
- The zoom is held for the session, as the main window's is (N4).

Checks: 295 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean (it
caught a `window` shadow in the new test before push).

**Where to resume:** `~/adi-window` on `window/floating-zoom`, the tip of the stack. Item 3's phase
C parts still wait for N17. The next piece the window can finish alone is the menu bar's other
menus (7.2 row 11), as far as there are commands to list. Also: the native Settings smoke in
`ui.yml` (win's file) once win agrees.

---

## 2026-10-28, evening — the keyboard and the device menu [#269](https://github.com/arieladi/Adi/pull/269) [#270](https://github.com/arieladi/Adi/pull/270) [#271](https://github.com/arieladi/Adi/pull/271) [#272](https://github.com/arieladi/Adi/pull/272)

**The director approved the Settings and Channel EQ windows (#267, #268):** "The Settings and
Channel EQ windows look right. Continue with the next window items."

The three pieces the last entry named, each its own branch and PR, stacked in this order on #268:
- **#269 `window/settings-arrows`** (manual §40.1.2). With a control focused, the arrows switch a
  switch or step a value box, and Enter switches a switch; each change is applied and written at
  once. A value box now takes the focus, so Tab reaches it. The keyboard's focus is outlined in the
  accent; a click's is not. Settings row 9 is built.
- **#270 `window/quit-key`** (§41.3). A Quit command: Ctrl+Q and the foot of the File menu off the
  Mac; on the Mac Cmd+Q stays the application menu's. Both reach `systemRequestedQuit`, which asks
  nothing. Application row 10 is built.
- **#271 `window/device-rename`** (§23.2.1). The device menu in Live's order (Delete; Rename; Fold,
  Configure). Rename, from the menu or Ctrl/Cmd+R with the device focused, is typed in the title
  bar and commits one `device.rename`. The Edit menu's Rename acts on the focused device, else the
  selected track. Cut, Copy, Paste and Duplicate wait for a device's copy (N18, new).
- **#272 `window/parity-shell-space`**, a checklist correction: 7.2-shell rows 10 and 11 (Space,
  Shift+Space) were built with the arrangement but still read open.

Checks on #271, the tip: 291 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow
sweep is clean. It caught a `host` shadow in #269's new test before push (MSVC C4456).

Resumed the same evening with a floating window's zoom (#273, above).

---

## 2026-10-28, later — the Channel EQ window [#268](https://github.com/arieladi/Adi/pull/268) `window/channel-eq`

**#268, stacked on #267.** ADR-0204 d1's one EQ window: every channel's ADI-Q and ADI-L in one
view, FabFilter's "show all instances" made native.
- EQ Eight's display, where each engaged EQ's curve will be drawn in its track's colour.
- One row per channel with its two switches and ADI-L's mode.
- It opens from the View menu's Channel EQ and from a track row's Q or L, in the floating host,
  one per project.
- Its slots are present and off until the engine's channel slots and the clones' plug-ins
  arrive (N16, new).

Checks: 281 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean.

**Item 3's state:**
- The Settings window (#267) and the EQ/limiter window (#268) are done.
- Phase C's window parts wait for their ops: fades, Insert Silence, and the import and bounce
  actions (N17, new). They are not drawn before they can act.

**Where to resume:** `~/adi-window` on `window/channel-eq`, the tip of the stack. With item 3
blocked, the next pieces are the parity rows marked partial that the window alone can finish:
- arrows on a focused Settings switch and value box;
- Ctrl+Q on Windows;
- Cut, Copy and Rename in a device's menu, where ops exist.
Also: the native Settings smoke in `ui.yml` (win's file) once win agrees.

---

## 2026-10-28 — the Settings window in Live's shape [#267](https://github.com/arieladi/Adi/pull/267) `window/settings`

**#267, stacked on #266. LANES §3b item 3 begins.** Measured against Live 11.2.7's Preferences
window, captured on the director's Mac once the macOS prompt that covered Live was answered.
- Live's tabs beside a rounded page panel. Rows are typed from the registry at Live's measures
  (names at 17, controls at 191, 197 and 97 wide, a 21-unit pitch).
- Every change is applied and written at once, with no OK button.
- REAPER's Find box, a help line at the foot, Restore Defaults per page, and the Project and
  Device marks.
- Live's keys: Ctrl+Tab and Option+Tab, the arrows on the tab list, and Esc.
- The Audio page leads with Live's Audio Device, Sample Rate and Latency sections over the
  device manager (`app_audio_page`). This replaces the JUCE selector dialog Cmd+, opened.
- `--smoke-settings` renders that page natively.
- The director has the side-by-side pictures of the Settings window and the Mixer View.

Checks: 277 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean
(it caught an MSVC C4456 in the new test before push). `parity-settings.md` lists what waits:
the registry's tab names and sections (N14).

**Next in item 3:** the EQ/limiter window (ADR-0204 d1). Then the phase C window parts: their
ops (fades, Insert Silence, the import and bounce prompts) are not in the catalogue yet, so they
will be logged as needs where they block.

---

## 2026-10-27, night — the Mixer View in Live's shape and its peak levels [#266](https://github.com/arieladi/Adi/pull/266) `window/peak-indicators`

**#266, stacked on #265.**
- The Mixer View is measured on Live 11's Session mixer (Live 11's Mixer View): 93-unit
  strips, the name in the track's colour, and ADI-Q and ADI-L where Live has the sends.
- The peak level pill (manual 18.1) is resettable by a click and turns red once the track
  clips. That was 7.6's last open row (13b).
- The meter and the fader share Live's measured scale, so the ◁ sits at the meter's dB.
- Checks: 265 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean.

**§3b item 2 is done** as far as nothing blocks it. What remains open there waits on other
lanes:
- the audio drop (N5);
- plug-in windows (N8);
- returns, Main last and the meters' sources (N6, N9, N11);
- the browser's list (N15, new).

**Next: item 3, starting with the Settings window in Live's shape** (ADR-0203 d11, Master
Reference Part V). Its pages are the registry's; their names in Live's ten tabs are win's
(N14, new).

**Driving Live:** a macOS prompt ("claude" would like to access data from other apps) sits over
Live and blocks clean captures of Live's Preferences. It is the director's to answer; nothing
clicks it.

---

## 2026-10-27, last — Live's look [#265](https://github.com/arieladi/Adi/pull/265) `window/live-look`; approved

**Approved by the director, 27 October:** "Approved, the look is close enough to Live. Continue
with the next window items." The hold on new windows is lifted. Each new window is measured
against Live 11 the same way, and the director sees pictures as each one lands.

**The director's ruling (27 October):** the window "looks very far from Ableton". It must look
like Live, plus what the Master Reference adds. He approves from pictures, and no new window is
built until he does. Item 3 and the rest of item 2 are held until then.

**What was done:**
- Measured Live 11.2.7 on his Mac (1440 × 900, default theme) region by region: colours by
  histogram, edges pixel by pixel.
- Restyled the window to those measures, now #265, stacked on #264.
- The numbers and the gaps that remain are in `collab/window/look-live11.md`.
- `liveParity` renders Live's default set as ADI at the same size. It is the golden
  `live-parity.png`. `ADI_UI_PARITY=<folder>` writes a 2x render to lay beside a capture.
- Sent the director side-by-side pictures: the whole window, the track headers, the detail row,
  and the control bar with the browser.

**What the measures changed:**
- The device strip's floor is 196, the whole Device View panel; its body is about 170. The
  director's 169 is the body. Both docs now say so.
- The frame is 8 units between panels, with 4-unit panel edges. The Status Bar is 38.
- A track is 88 tall. New tracks are named "# Audio" and "# MIDI", as Live's.
- The default panel widths are 316 for the browser and 221 for the track controls.

**What is not like Live, and what it waits on:**
- The browser's list: its content model.
- Returns and Main shown last, and MIDI tracks drawn as MIDI: N6 and N11.
- The loop brace: the transport's loop.
- Live's own device displays: ADR-0150.
- The typeface.

**Checks:**
- 259 shell checks; all goldens re-recorded, with masks over every new place that shows words.
- `test_all.sh`: 8265 checks across 97 suites, PASS; window state 48, document 38.
- The Clang `-Wshadow-all` sweep is clean.
- Found on the way: a negative-size fill in the mixer panel when the tracks run past it. It is
  fixed, and the JUCE assertion it raised is gone.

**Where to resume:** `~/adi-window` on `window/live-look`, the tip of the stack. Once the
director rules on the pictures, apply his corrections here first, then carry on with §3b item 2
and item 3.

---

## 2026-10-27, later still — the Mixer View [#263](https://github.com/arieladi/Adi/pull/263) `window/mixer-view`

**The Mixer View, now #263, stacked on #262.** Live 12's Mixer in the Arrangement: vertical
strips at the arrangement's foot, from Ctrl/Cmd+Alt+M, the View menu's Mixer, or the Status Bar's
new bottom-right toggle.
- It is the same `MixerPanel` in a vertical mode, so edits, meters and Auto Gain are shared.
- Only the columns on screen are realised, and the wheel scrolls it sideways.
- Dragged past 260 units tall, it shows ticks every 6 dB and a numeric volume field.
- The manual settles parity-7.2 row 6: Ctrl/Cmd+Alt+M is the Mixer View's. The side panel of
  track controls keeps a View-menu item, with no key.
- Found by the render: the Track Activator showed the track's id. It now shows its place.

Checks: 244 shell checks, with a new golden; `test_all.sh` 8265 across 97 suites.

**The shadowing sweep earned its keep twice more:** an outer `order` and an `a` hidden by locals
were both caught before push.

**CI at this entry:** #246 and #248 are fully green; #251 to #262 have no failures and are still
running.

**The scrub area, now [#264](https://github.com/arieladi/Adi/pull/264) `window/scrub`, stacked
on #263.**
- A 14-unit strip under the ruler; a click there plays from that point, or jumps there while
  playing, through `AdiRootComponent::playFrom`.
- Unquantized until the global launch quantization exists; hold-to-loop waits for a transport
  loop.
- Checks: 250 shell checks, a real click through the fake peer; `test_all.sh` 8265 across 97
  suites.

**CI:** no failures on #251 to #264; the queued macOS legs are still running.

**Where to resume:** the worktree is `~/adi-window`, on `window/scrub`, the tip of the stack.

**Next in §3b item 2:**
1. The browser's content.
2. Live's resettable peak indicators on a tall Mixer View.

After those, item 3: the window parts of phase C, the Settings window in Live's shape (ADR-0203
d11), and the EQ/limiter window (ADR-0204 d1).

---

## 2026-10-27, later — Rename [#260](https://github.com/arieladi/Adi/pull/260), selection [#261](https://github.com/arieladi/Adi/pull/261), the canvas's accessibility [#262](https://github.com/arieladi/Adi/pull/262)

**CI.** #246 and #248 are green on all 22 legs. Three failures elsewhere in the stack, all fixed
on the branch that introduced them, with the branches above restacked:
- #256: MSVC C4456. The floating-host test's `host` hid the `ZoomHost host`.
- #257 and #258: GCC `-Wsign-conversion` on `size_t += cond ? 1 : 0`.
- #261: Windows allocated in the frame that presents a new selection, where the device strip
  follows the track. The check now audits the frames after it.

Before each push, a Clang `-Wshadow-all` sweep now runs over the UI sources and the shell test,
through `build-juce/compile_commands.json`. It caught two more shadowed loop variables before
#262 went up. GCC 16 is installed (`/opt/homebrew/bin/g++-16`), but it cannot parse JUCE's
headers on macOS, so GCC-only warnings still surface in CI.

**Rename, now #260, stacked on #258.** Ctrl/Cmd+R, the Edit menu or the header's menu opens an
editor in the track's title bar.
- Enter commits one undo step; Esc cancels; Tab and Shift+Tab move on through a series.
- A leading # shows the track's number and follows it, with a # per digit (manual 18.2). New
  tracks are named "#-Audio" and "#-MIDI".
- Checks: 219.

**Selection, now #261, stacked on #260.** Selection is session state.
- `adi::ui::SessionState` holds the project's one selection in `session_state`. It is never an op.
- Every window presents a copy per frame.
- Edits carry it as `sel_before`, and undo restores it.
- Checks: 224 shell and 48 headless (+10, every ABI). `test_all.sh` 8265 across 97 suites.

**The canvas's accessibility, now #262, stacked on #261.** Each clip on screen gets a transparent
`ClipAccessible`, recycled as the view scrolls.
- Its listItem title is built only when a reader asks; its press action selects the clip.
- Keyboard moves are announced.
- Checks: 229.

**Next in §3b item 2:**
1. The Mixer View of vertical strips (Live 12 18.1, p. 390, 393).
2. The scrub area and its click-to-play.
3. The browser's content.

---

## 2026-10-27, night — LANES §3b item 2 begins: the Overview → [#258](https://github.com/arieladi/Adi/pull/258) `window/overview`

**The Overview, now #258, stacked on #257.** It is a 16-unit strip above the ruler: every track's
clips in miniature over the whole span, and an outline around the part on screen.
- Dragging sideways scrolls by the Overview's own scale; dragging vertically zooms.
- A double-click zooms out to the whole arrangement.
- The lanes, the playhead, the mixer's rows and the strip's ceiling now start below it
  (`kTopHeight`).

Checks: 212 shell checks, with real mouse dispatch through the fake peer; `test_all.sh` 8255
across 97 suites. Parity: row 20 of `parity-7.3-arrangement.md` is built.

**Where to resume** (superseded by the entry above for the branch). The worktree is `~/adi-window`, on `window/overview`. Its trees are
`adi_daw/build` (core, `-DADI_WERROR=ON`) and `adi_daw/build-juce` (`-DADI_WITH_JUCE=ON`). This
log lives in `~/adi-window-log` on `window/log` (#252). The next items of §3b item 2, in order:
1. Rename (Ctrl/Cmd+R) and the double-click on a track's name: `track.rename` exists.
2. The scrub area and its click-to-play: the locators wait for their ops.
3. Selection as session state, which UI-ARCHITECTURE §2 requires. The arrangement still keeps it
   in view geometry, as #199 did.
4. The canvas's accessibility layer, one virtual child per clip (UI-ARCHITECTURE §6).
5. The Mixer View of vertical strips.
6. The browser's content.

Then item 3: the window parts of phase C, the Settings window in Live's shape, and the EQ/limiter
window. Before any of that, check CI on #246, #248, #251, #254, #256, #257 and #258; their reruns
after the restack were pending at this entry.

---

## 2026-10-27, late — #204 → [#256](https://github.com/arieladi/Adi/pull/256) `window/floating-windows`; #205 → [#257](https://github.com/arieladi/Adi/pull/257) `window/mixer`; the six are through

**#204, the floating host, now #256, stacked on #254.**
- **Kept:** the host and the analyser views, with their tests. Undocking reparents; a second view
  shares one model; the monitor clamp; one clock per window; keys reach the app's commands; a
  window closes when its device is deleted and does not reopen on undo.
- **Gone:** the "Analyse" button on every device. Live has none; the analyser's big window is the
  analyser device's, and plug-in editors need the hosts (N8).
- **Added:** tooltips in every floating window.

Checks: 161. Parity: `parity-7.5-floating.md` (11 rows).

**#205, the mixer, now #257, stacked on #256.** Its engine half is left out (N9, N10).
- **Live's Arrangement Track Controls:** one row per track, level with the lane and scrolling with
  it, in the mixer slot. Each row holds the meter (from the track's post-fader tap), volume, a pan
  knob, the numbered Track Activator and Solo.
- **The ADI-Q and ADI-L slots** sit on every row, present and off.
- **Auto Gain Stage** is in the right-click menu.
- **Split Stereo Pan** is shown disabled (N12).
- **Open:** send knobs, return strips and the Main track's loudness meter (N6, N11, N13).
- **Per-strip Learn is gone:** Live learns through MIDI Map mode.

Checks: 208. Parity: `parity-7.6-mixer.md` (15 rows).

**MSVC caught one, and the stack was restacked.** #248's Windows GUI leg failed C4456: the
shell test's second `opCount` hid the first. It was fixed on `window/application`, and a lambda's
local `id` in the device panel's Configure menu was renamed against C4458. The four branches
above were rebased onto that fix and force-pushed with lease: #251, #254, #256 and #257. The tip
passes 208 shell checks.

**The queue moves on to item 2:** what 7.2 to 7.6 lists and no PR covers. That is the Overview;
the scrub area and locators; the Mixer View of vertical strips; Rename (Ctrl/Cmd+R); the
DetailEditor (the Clip View); the browser's content; the Ctrl/Cmd+Alt+M Mixer section's
show/hide options; and the ADR-0129 gate rows not yet built.

---

## 2026-10-27, evening — #199 → [#251](https://github.com/arieladi/Adi/pull/251) `window/arrangement`; #203 → `window/device-strip`

**#199, the arrangement, now #251, stacked on #248.** Its engine half is left out (N5, N6). The
rulings applied:
- **Live's layout:** the headers sit right of the clips.
- **Zoom around the selection:** + - and Ctrl/Cmd+wheel anchor on the selection's middle, else
  the insert marker, else the playhead. The cursor anchor is used only with
  `lookfeel.zoomOnSelection` off.
- **Live's keys and menus:** Z X H W, Home, Shift+Space, Ctrl/Cmd+T and +Shift+T, a Create menu,
  and right-click menus. They replace #199's buttons.
- **Track ids** come from the current cell, not from a raw `MAX(id)` read of the store.

Checks: 131 shell checks, with real mouse dispatch through #199's fake peer;
`test_all.sh` 8255 across 97 suites. Parity: `parity-7.3-arrangement.md` (22 rows).

**#203, the device strip, now [#254](https://github.com/arieladi/Adi/pull/254), stacked on #251.** Its `ParamOps::uiEvent`
is left out (N7), so the strip shows values and refuses edits, saying so in the Status Bar. The
rulings applied:
- **Live's Device View:** devices run left to right, and the controls sit in columns that fit
  the strip's height. #203 stacked them in a column that overflowed the floor.
- **The title bar:** the activator, the name and Configure. A double-click folds the device to a
  narrow bar. A right-click menu offers Fold, Configure and Delete.
- **The floor** comes from `DesktopDefaults`; the top edge drags the strip upward only.
- **Kept from #203:** the document's tick calls `ParamOps::drain`. It is named in the PR because
  STEP-7-PLAN §7 item 5 leaves "who calls drain" open.

Checks: 146 shell checks. Parity: `parity-7.4-devices.md` (14 rows).

**The first window.** The `ADI` app builds and opens its native window on macOS from a real
project file. LANES §5 makes "the first window" the director's gate "when 7.2 is merged". #246
and #248 are that, and they wait on win's review.

**The Mac was shared** with the devices lane's Codex build and Ableton all evening: load averages
of 50 to 190 on 8 cores, 8 GB. `adi_library_tests` (SQLite under fsync) ran for half an hour in
one harness run, but it passed; nothing of ours was implicated.

---

## 2026-10-27 — LANES §3b item 1: #196 → [#246](https://github.com/arieladi/Adi/pull/246) `window/root-window`; #198 → [#248](https://github.com/arieladi/Adi/pull/248) `window/application`

**#196, the root window, now #246.** Cut from `origin/main` (`a57d47a`, 142 commits past #196's
base). The claims-table edit and the win_codex handoff note were dropped. The rulings applied:
- Live's keys, §41:
  - Esc stops nothing.
  - Hide/Show Browser on Ctrl/Cmd+Alt+B and +5, Mixer on +M, Device View on +4.
  - Info View on Shift+?.
  - Zoom In/Out Window on Ctrl/Cmd + and -, each window with its own interface zoom.
- The Status Bar.
- The Info View, bottom-left beside the Device View, app-scoped.
- The AI tab collapsed at the browser's foot.
- The device strip's floor of 169 units, unverified, as a desktop default; it grows upward only.
- Floating tooltips after 600 ms.

Checks: `adi_ui_shell_tests` 82, `adi_ui_window_state_tests` 38; `test_all.sh` 8217 checks across 96
suites. Parity: `collab/window/parity-7.2-shell.md` (17 rows; 4 to measure).

**#198, the application, now #248, stacked on #246.** The rulings applied:
- **No Save.** Every op is committed as it happens (ADR-0203 d13), and quitting asks nothing. #198's
  Save (a WAL checkpoint on Ctrl+S) is gone.
- **Ctrl/Cmd+S takes a named snapshot** through `History::takeSnapshot`. The prompt is pre-filled
  with the project's name and the local time.
- **Ctrl/Cmd+Shift+S is Save As**, which waits for the engine (N3).
- **Ctrl/Cmd+, opens the Settings' Audio page**, the device chooser.
- **New makes no rows of its own.** #198 inserted the project row and the master with raw SQL;
  the Main track is now made through `track.create`, and the history's first step is protected
  (N1, N2).
- **The menus are Live's, as far as they go:** File, Edit, View. On the Mac they are the main menu.
  The Edit menu names the step it would undo.
- **A refused command is handled, not asserted.** JUCE asserted when a command was refused, or
  when commands were re-registered under changed names. A refused command now says why in the
  Status Bar.
