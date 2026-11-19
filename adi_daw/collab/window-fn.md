# window-fn — log

The window's function seat (ADR-0248 d14): a second Claude seat in the window lane. It makes the window's
existing surfaces work: commands, keys, gestures, undo steps, and the wiring from the window to the
engine. The window seat (`collab/window.md` on `window/log`) keeps the look, built from the sheets the
director agreed (ADR-0243 d1); this seat changes no look, and where an item needs a control that has
none yet it uses the shared painters of `src/juce/ui_controls*` as they are. It works in the window's
paths (`src/adi/ui/**`, `src/juce/ui_*`, `src/juce/app*`) and in nobody else's: the engine's and
engine-win's needs go in the table below, and win tells their logs.

Branches `window-fn/<topic>`, one item each, cut from `origin/main`; this log on `window-fn/log`. Only
the window-fn seat writes to this file. Newest entry first. Each entry ends with "PR requested": the
branch, its title, what was run, and what it depends on.

**Where it runs.** A cloud session (Linux, GCC 13, Ninja). Built as `ui.yml` builds the window:
`-DCMAKE_BUILD_TYPE=Debug -DADI_WITH_JUCE=ON -DADI_WITH_PD=OFF -DADI_WERROR=ON`, the window's suites run
headless. MSVC `/WX` and macOS are not run here; the `Step 7 UI` workflow runs them on each pull request.

---

## FOR THE DIRECTOR

None open.

---

## Needs from other seats (open)

Numbers are asked for in `collab/RESERVATIONS.md` through win before they are used ("Reserve:" below);
until then a need is named by its item.

| Need | Owner | Why the window needs it | What the window does meanwhile |
|---|---|---|---|
| Reserve: one N row. A distinct `scan::State` (or a shared constant) for "the scanner helper could not start", apart from the file it was asked to read | engine-win (`src/adi/plugin_scanner.*`) | DF-W34 (b): the Status Bar names the scanner, not the plug-in, when the helper is missing or blocked | the window matches `Scanner::probe`'s words ("scanner helper unavailable or scan cancelled") on an `Unavailable` failure |
| Reserve: one N row. The snapshot's track fields for recording: `TrackNode::recordArmed`, `monitorMode` and `inputRef`, from `tracks.record_armed`, `monitor_mode` and `input_ref` | engine (`src/adi/engine/snapshot.*`) | MX-09: the strips' Arm and the monitoring switch read the frame's snapshot as every other control does (ADR-0201) | the application hands the window the session's rows (`Session::model()`) once a frame, and an action reads them fresh |
| Reserve: one N row. The input-monitor node of MX-09: a live input per monitored track, from its input (channels 1/2 until `input_ref` has its grammar, MX-25), heard per `monitor_mode` (In always; Auto while armed and no clip plays; Off never) | engine (ADR-0248 d6, item 4) | MX-09: the director hears what he records | Arm and the switch store their choice; nothing is heard of the input; Record records |
| Reserve: one N row. Live's monitoring default for a new track: Auto, where New and `track.create` store 0 (Off) | win (AWAITING 20) and the engine | MX-09: a new audio track should monitor as Live's does once the input-monitor node exists | the switch shows Off on New's tracks, as stored |

---

## 2026-11-19, window-fn — DF-W34: the plug-in scan off the message thread (PR requested)

**Taken over from the window lane, with win's word in ADR-0248 d14:** the function list, starting with
DF-W34 on #676. This branch carries #676's two DF-W9 commits (`e88693f8`, `8d067657`) cut onto main, off
step 4, so `window/plugins-browser` need not be rebased: **#676 can close in favour of it.** Its picture
commit (`38b143ce`) stays with the window lane, which owns the look.

**What it does** (`src/adi/ui/plugin_scan.hpp`, `plugin_listing.hpp`, `src/juce/ui_main.cpp`,
`ui_browser.*`, `ui_settings.*`):
- (a) Choosing Plug-Ins never scans on the message thread. `PluginScanService` runs the app's one
  `JuceDeviceLoader` on a worker; the list's line and the Status Bar say "Scanning plug-ins: N files…"
  (the VST3 files and CLAP bundles it will probe, counted first, filesystem only) and the list fills when
  the scan's end reaches the message thread. An arrow key or a click never waits. The session's loader
  goes through a gate (`gatedLoader`): a VST3 or CLAP load waits for a running scan and holds the loader
  until it is made, so the loader never runs on two threads and no second scanner meets the registry.
  Quit cancels through the helper's cancel flag. The lazy scan inside a project open runs only with no
  worker beside it, and what it lists is adopted.
- (b) A scan that stopped early says why, from the loader's report: the scanner itself (the registry, or
  the helper missing or blocked), or the file that stopped it; "Nothing could be listed." or "The list
  may be incomplete.", and "Rescan from the Settings' Plug-Ins page." It never says "the rest are
  listed", which is kept for a complete scan's per-file failures (now "1 plug-in file", singular).
  **Rescan** is on the Settings' Plug-Ins page, as Live's (the Restore Defaults button's look, beside
  it). A change of the plug-in folders rescans once Plug-Ins has been scanned; choosing Plug-Ins after a
  stopped scan does not rescan by itself, so a missing helper is not probed on every click.
- (c) One folder per vendor ignoring case, named as the first of its plug-ins spells it; no vendor and
  "other" share one.
- (d) The load still waits for N57; its Status Bar line cites "N57".

**Kept from the row:** one loader, one registry, the format and vendor folders, the Status Bar lines,
the shell test's layout. **Not done, and why:** a folder taken out of the Settings stays searched until
restart (the loader adds folders and removes none: engine-win's catalogue settings, N58); a VST3's kind
stays unknown until N58's typed catalogue. When N58 lands, the service wraps its `startDiscovery`,
`discoveryProgress` and `adoptDiscovery` in place of `loader_.scan()`, with per-file progress.

**Review:** an independent review in three dimensions (threads and lifetimes, the row's behaviour, the
tests), each finding checked by a skeptic: 5 confirmed, 8 refuted. All five fixed in `0155622d`: the
loader held for the whole load (a plug-in running its own message loop while it is made could let a
posted restart start a worker mid-load), a missing helper worded as the scanner's, the test scan polled
for its cancel (it could hang), and two checks that could not fail made real. Planted faults fire (the
vendor key; the hold; the generation guard).

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites build and pass; `adi_ui_shell_tests` 2776 checks, 0 failures.

**PR requested.** Branch `window-fn/plugin-scan` at `0155622d`, on main `12aab48a`. Title: **adi_daw:
window-fn: DF-W34, the plug-in scan off the message thread: a worker that owns the loader, a gate for
loads, Rescan, a stopped scan named, one folder per vendor (DF-W9's listing carried from #676)**. Depends
on nothing unlanded; supersedes #676.

**Asked of engine-win (through win):** a distinct state or a shared constant for "the helper could not
start" in `scan::Result` (`Scanner::probe` reports it as `Unavailable` with "scanner helper unavailable or
scan cancelled" and the file's path; the window matches those words today).

---

