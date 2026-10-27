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
| N6 | The snapshot to carry what the arrangement draws: a track's `kind` (to place Main last, as Live's arrangement does), a clip's `name`, and the time-signature map (bars in the ruler). #199's `3feee62` adds all three to `engine/snapshot.*` | engine | the ruler and the clip labels | the ruler counts bars in 4/4 and says so in its tooltip; clips are drawn unnamed; Main is listed where the snapshot puts it |

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

**#203, the device strip, now `window/device-strip`, stacked on #251.** Its `ParamOps::uiEvent`
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
