# interaction logger

Press a hotkey, a Ctrl+F-style bar appears, type what you are about to do, hit
Enter. Everything on screen and every click/keystroke is recorded until you press
the same hotkey again, and the session is saved in a folder named after your
description.

```
Ctrl+Shift+F9   ->  [ Record:  drag a sample onto track 3        ]  ->  Enter  ->  recording
Ctrl+Shift+F9   ->  saved to  ~/Documents/InteractionLogs/drag-a-sample-onto-track-3_20260919_1412/
```

Output is a drop-in for [AIComputerInteractionLogger](https://github.com/hemangjoshi37a/AIComputerInteractionLogger)'s
`src/dataset_exporter.py`, so its Claude computer-use / OSWorld / HuggingFace
exports work on these sessions (verified, see *Why not upstream's recorder* below).

With Live's `ADI_Logger` control surface enabled, each session also gets
`live_events.jsonl`: what changed *inside* Live — every knob gesture with its
from/to value, devices added, tracks, transport, selection, clip launches, note
edits — on the same clock as `events.csv`.

## Simple and deep logs (the desktop shortcuts)

`install.ps1` puts three shortcuts on the desktop:

| shortcut | does |
| --- | --- |
| **ADI Log - Start Simple** | asks what you are about to do, then records lightly for as long as you work |
| **ADI Log - Start Deep** | asks what you are about to do, then records one task in full detail |
| **ADI Log - Stop** | stops and saves whatever is running (both modes), prints where it went |

Both Start shortcuts open the bar first: type *we are now working on a real
avastha project* or *open Ableton and make a serum acid trance lead*, Enter
starts, Esc cancels. The description names the folder, is kept verbatim in
`label.txt`/`session.json`, and is the first line of `live_events.jsonl`. The
two modes can run at the same time; each records into its own folder.

| | simple | deep |
| --- | --- | --- |
| frames | after each click/key, at most 1/s, plus a keyframe every 60s if the screen changed; 1280 wide WebP | 5 per second, 1920 wide WebP |
| input | clicks, scrolls, keys — **only while Live has focus** | everything, including the mouse path |
| other apps | logged as "chrome.exe had focus", title blanked, no frames | recorded like Live |
| Live log | one line per gesture, structure, transport, selection, note counts | the same plus the gesture's path at 10 Hz, every parameter value at the start, and the notes themselves |
| hotkeys | none (shortcuts only) | `Ctrl+Shift+F9` stops / starts the next task, `Ctrl+Shift+F10` quits |
| sessions | `<base>/simple/` | `<base>/deep/` |

Both run windowless; their console output goes to `<base>/.state/<mode>.log`.
`logger_ctl.py status` says what is running. Recording refuses to start, and a
running one stops, when the disk has less than `min_free_gb` (10) free.

### What it costs

Measured on adi-mobile (i9-11950H, 3840x2560 screen) with `bench.py`:

| | simple | deep |
| --- | --- | --- |
| capture process CPU | 4.5% of one core (0.3% of the machine) | 39% of one core (2.4% of the machine) |
| input-hook delay, max | 3.4 ms | 3.0 ms |
| frame | 1280x853 WebP, ~77 KB | 1920x1280 WebP, ~195 KB |
| disk, screen mostly still | ~160 MB/hour | ~330 MB/hour |
| disk, worst case | ~280 MB/hour (clicking every second) | ~3.5 GB/hour (every frame different — Live playing, meters moving) |

Deep mode while the transport runs is close to its worst case. Dropping
`screenshot_freq` in `modes.deep` scales that linearly (3 fps ≈ 2.1 GB/hour).

Frames are grabbed and encoded in a separate below-normal-priority process.
In the process that owns the input hooks, one 4K grab held Python's GIL long
enough to delay every mouse and keyboard event on the machine (34 ms max
measured at 1080p before the split); the hook delay is now idle-level.
`bench.py` re-measures any machine.

## Live state log (`ADI_Logger`)

`install.ps1` copies `live_remote_script/ADI_Logger` into Live's User Library
(`Remote Scripts/`). Enable it once: Live > Preferences > Link, Tempo & MIDI >
Control Surface: **ADI_Logger**, Input and Output: None.

It is idle — no listeners at all — until a recording starts. The logger drops a
small JSON file into `<base>/.live/` naming its session folder; the script
watches that folder once a second and writes into every session it names, at
that session's level. Highlights of `live_events.jsonl`:

| ev | when |
| --- | --- |
| `start`, `snapshot` | first lines: description, Live version, every track and device (deep: every parameter value) |
| `param` | a knob/fader/tempo/loop-brace gesture ended: `from`, `to`, `display` ("-20.0 dB"), where it lives |
| `param_move` | deep only: the value every ~100 ms while it moves |
| `devices`, `tracks`, `track_name`, `device_name` | structure changed |
| `mute`, `solo`, `arm`, `transport`, `record`, `metronome`, `clip_fired`, `clip_playing` | the obvious |
| `select_track`, `select_device`, `select_param`, `view`, `detail_clip` | what you are looking at |
| `notes` | the open MIDI clip's notes changed (simple: count, deep: the notes) |
| `end` | the recording stopped |

Automation playback is filtered out: a parameter whose automation is playing
while the transport runs is Live moving it, not you. The script runs on Live's
main thread, so listeners only mark values dirty; reading them and writing the
file happen ten times a second, batched. Status (connected, listener count) is
in `<base>/.live/live.json`; problems go to Live's `Log.txt` prefixed
`ADI_Logger:`.

## Setup

```bash
install.cmd -LogDir E:\InteractionLogs
```

Creates `.venv` next to the scripts and installs `mss`, `Pillow`, `pynput`,
`pywin32`, `psutil`; writes `config.local.json` (machine-only settings, not in
git) with the session folder; installs `ADI_Logger` into Live; makes the desktop
shortcuts. Keep `-LogDir` off OneDrive — the installer warns if it is not. Then
check it works:

```bash
.venv\Scripts\python.exe selftest.py
.venv\Scripts\python.exe selftest_modes.py
.venv\Scripts\python.exe selftest_live.py
.venv\Scripts\python.exe selftest_ui.py
```

## Use (hotkey daemon, no mode)

```bash
start_logger.cmd
```

Leave that console window open — **minimise it**, because capture is full screen
and an un-minimised console ends up in your frames. It prints a status line every
15s while recording, and the session summary when you stop.

| key | does |
| --- | --- |
| `Ctrl+Shift+F9` | open the bar / stop the current recording |
| `Enter` | start recording with the typed description |
| `Esc` | close the bar without recording |
| `Ctrl+Shift+F10` | quit the daemon (stops and saves any live recording first) |

Nothing is captured until you press Enter. The bar hides itself and hands focus
back to the window you were in before the first frame is taken, so it never
appears in the recording.

To start it at login: `powershell -ExecutionPolicy Bypass -File install_startup.ps1`
(`-Remove` undoes it). It is not installed unless you run that.

## What a session looks like

```
drag-a-sample-onto-track-3_20260919_1412/
├── events.csv        Timestamp,EventType,Data     <- upstream-compatible
├── screenshots/      screenshot_<epoch_ms>.png
├── session.json      description, target window, geometry, counts
└── label.txt         your description, verbatim
```

`events.csv` event types and `Data` formats, exactly as upstream writes them:

| EventType | Data |
| --- | --- |
| `screenshot` | filename inside `screenshots/` |
| `mouse_move` | `x=960, y=540` |
| `mouse_click` | `x=960, y=540, button=Button.left, pressed=True` |
| `mouse_scroll` | `x=960, y=540, dx=0, dy=-2` |
| `key_press` / `key_release` | `key=Key.ctrl` or `key='s'` |
| `window_change` | JSON: `title`, `process_name`, `pid`, `bounds` |

### Coordinates

Recorded coordinates are **frame pixels**: they index straight into the saved
screenshots, no conversion needed. Your display is 4K at 300% scaling, so frames
are downscaled to 1920 wide and coordinates are scaled to match. `session.json`
carries the exact transform back to raw screen pixels:

```
screen_x = frame_x / coordinates.scale.x + coordinates.origin.x
```

The daemon also opts into per-monitor-v2 DPI awareness. Without it Windows hands
a non-aware process a virtualised 1280x720 desktop while `pynput` reports real
3840x2160 mouse positions, and every recorded click lands 3x off in the frame.

## Disk

Full-screen capture is expensive. Measured on this machine (3840x2160 source):

| setting | per hour at 5 fps |
| --- | --- |
| native 4K PNG | ~12 GB |
| 1920-wide PNG (default) | ~5.1 GB |
| 1920-wide JPEG q88 | ~4.3 GB |
| 1920-wide WebP q85 | ~1.8 GB |
| 1920-wide PNG, mostly static screen | ~0.7 GB |

Identical frames are not rewritten — the tick is still logged, pointing at the
last file — so a mostly-still DAW screen costs very little. For long sessions
drop the frame rate first: `start_logger.cmd --fps 2`. WebP is the only format
that saves real space — JPEG at a quality worth keeping barely beats PNG on UI
screenshots — and it costs 83ms per frame to encode against PNG's 43ms, which is
still comfortably inside a 5 fps budget.

## Options

Edit `config.json` (its `modes` section holds the simple/deep settings), put
machine-only overrides in `config.local.json`, or pass flags (flags win):

| setting / flag | default | notes |
| --- | --- | --- |
| `hotkey`, `quit_hotkey` | `<ctrl>+<shift>+<f9>`, `<ctrl>+<shift>+<f10>` | pynput syntax |
| `screenshot_freq` / `--fps` | 5 | frames per second |
| `max_frame_width` / `--max-frame-width` | 1920 | `0` = native resolution |
| `screenshot_format` / `--format` | `png` | `jpeg`/`webp` are smaller; see note |
| `capture_mouse_moves` / `--no-mouse-moves` | on | off = clicks and scrolls only |
| `dedupe_screenshots` / `--no-dedupe` | on | skip rewriting unchanged frames |
| `motion_threshold` | 0.001 | fraction of the frame that must change to count |
| `mask_text_keys` / `--mask-text` | off | log typed characters as `'*'` |
| `base_output_dir` / `--base-dir` | `~/Documents/InteractionLogs` | |
| `capture_trigger` | `interval` | `events` = frame after clicks/keys + keyframe (simple) |
| `event_frame_delay`, `event_frame_min_gap`, `keyframe_interval` | 0.3, 1.0, 60 | seconds, `events` trigger only |
| `input_scope` | `[]` | process-name patterns; input/frames only while one has focus |
| `live_log` | `off` | `simple` / `deep`: ask ADI_Logger for Live's state |
| `min_free_gb` | 10 | refuse / stop recording below this much free disk |
| `--mode simple\|deep` | none | apply that `modes` section, record into `<base>/<mode>/` |
| `--open-bar`, `--description TEXT` | | ask at launch / start at launch with this description |

With `jpeg`/`webp` the session also gets one `frame_reference.png`, because
upstream's exporter sniffs resolution from the first `.png` it finds.

## Worth knowing

- **Deep (and the no-mode daemon) log every keystroke system-wide while
  recording**, not just in the target window — passwords included if you type
  one. `--mask-text` replaces typed characters with `*`; the safer habit is to
  stop the recording first. Simple mode only logs input while Live has focus.
- The stop hotkey itself lands at the end of the log (`Key.ctrl`, `Key.shift`,
  `Key.f9`). Trim the last few key events if they matter.
- **The mouse cursor is not in the frames** — Windows screen capture omits it.
  The coordinates are in `events.csv`; draw it in if you need it visible.
- Capture is full screen, and the `window_change` log tells you which window was
  focused at any moment. Cropping to one window is a post-process using
  `session.json`'s `target_window` bounds.
- The bar renders Hebrew in logical order without bidi shaping, so an RTL
  description looks reversed while you type it. The saved folder name is correct.
- One recording at a time per mode. A second hotkey press always means "stop".
- Live scans `Remote Scripts` at startup: after installing or updating
  `ADI_Logger`, restart Live.
- No audio is recorded. Live usually runs on an ASIO driver, which bypasses the
  Windows mixer a loopback recorder would listen to.

## Why not upstream's recorder

The repo's capture path does not run, so only its *exporters* are reused here.
Checked against commit `main`, 2026-09-19:

- `src/recorder.py:66` calls `AudioRecorder(self.audio_file)` but
  `src/audio_recorder.py:7` requires `(audio_file, config)`. Every
  `start_recording()` raises immediately, and the surrounding `try/except`
  swallows it into a log file — you get an empty session directory.
- `DatasetRecorder(base_output_dir=…, screenshot_freq=…, audio_channels=0)`
  matches no constructor in the repo. `audio_channels: 0` would also raise
  `wave.Error: bad # of channels`.
- `requirements.txt` pins `PyYAML==5.4.1` and `numpy==1.26.2`, neither of which
  installs on Python 3.14 (PyYAML fails to build: `'build_ext' object has no
  attribute 'cython_sources'`).
- `start_recording(duration)` is a blocking `time.sleep`. There is no stop hook,
  so hotkey start/stop cannot be built on it.
- Screenshots are written as `screenshot_{int(timestamp)}.png` — whole seconds,
  so anything above 1 fps overwrites its own frames.

What upstream is genuinely good for is `src/dataset_exporter.py`. Point it at a
session from here:

```python
from src.dataset_exporter import DatasetExporter
DatasetExporter().export_to_claude_computer_use(session_dir, out_dir)
```

That was run end-to-end against a session from this tool: events parsed,
resolution sniffed as 1920x1080, clicks emitted as
`{"action": "left_click", "coordinate": [600, 450]}`. One upstream quirk: it
stuffs the whole `window_change` JSON blob into `window_title`.

## Porting to macOS

Everything platform-specific lives in `winutil.py` — four functions. The recorder,
the command bar, the event format, the config and both selftests are portable as
written.

| needs a macOS implementation | Windows version uses |
| --- | --- |
| `enable_dpi_awareness()` | `SetProcessDpiAwarenessContext` |
| `window_info()` | `win32gui` + `psutil` for title/process/pid/bounds |
| `focus_window()` | `SetForegroundWindow` |
| `_extended_frame_bounds()` | `DwmGetWindowAttribute` |

On macOS use `Quartz.CGWindowListCopyWindowInfo` (frontmost window's title,
`kCGWindowOwnerPID`, `kCGWindowBounds`) and `NSRunningApplication.activateWithOptions_`
for focus. Two traps worth knowing before you start:

- **Retina scaling is the same bug in a different costume.** `mss` returns backing-store
  pixels (2x) while `pynput` reports logical points, so coordinates need the same
  translate-and-scale treatment `_to_frame()` already does — set `origin`/`scale` from
  the backing scale factor instead of calling a DPI API, and the rest of the pipeline
  works unchanged.
- **Permissions fail silently.** `pynput` listeners and `GlobalHotKeys` need
  Input Monitoring plus Accessibility, and `mss` needs Screen Recording, all granted
  to whichever binary hosts Python. Without them you get a session of zero events and
  no error. Worth an explicit startup check that refuses to run rather than recording
  nothing.

`screenshot_format`, dedupe, the CSV schema and the upstream-exporter compatibility
are all platform-neutral — keep them identical so sessions from both machines land in
one dataset.

## Files

| file | |
| --- | --- |
| `logger_daemon.py` | hotkey listener, modes, state machine, entry point |
| `logger_ctl.py` | what the desktop shortcuts run: start / stop / status |
| `overlay.py` | the command bar |
| `recorder.py` | input hooks, session writer, capture-process owner |
| `capture.py` | frame grab + encode, in its own below-normal process |
| `livelink.py` | desktop half of the Live handshake |
| `live_remote_script/ADI_Logger/` | the Live control surface that writes `live_events.jsonl` |
| `winutil.py` | DPI awareness, window facts, focus restore |
| `install.ps1` / `install.cmd` | venv, local config, Live script, desktop shortcuts |
| `make_icons.py` | draws the shortcut icons |
| `bench.py` | CPU / hook delay / disk per mode on this machine |
| `selftest.py` | headless capture + output-format checks |
| `selftest_modes.py` | simple mode: event frames, focus scope, Live handshake |
| `selftest_live.py` | ADI_Logger against a fake Live, plus Python 3.7 grammar |
| `selftest_ui.py` | scripted bar → record → stop → saved run |
