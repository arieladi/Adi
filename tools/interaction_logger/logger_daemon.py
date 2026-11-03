"""Hotkey daemon: press the hotkey, describe what you are about to do, press it
again when you are done.

    hotkey  ->  command bar  ->  Enter  ->  recording starts
    hotkey  ->  recording stops, session saved under the description you typed

Run it and leave it running. Nothing is captured until you hit Enter on the bar.

With --mode simple|deep the settings come from config.json's "modes" section and
sessions land in <base_output_dir>/<mode>/. The desktop shortcuts drive it
through logger_ctl.py: one instance per mode, a named event to stop it, and a
small state file that says what it is doing.
"""
from __future__ import annotations

import argparse
import json
import logging
import queue
import shutil
import sys
import threading
import time
import tkinter as tk
from pathlib import Path

from pynput import keyboard

import winutil
from livelink import live_status
from overlay import CommandBar
from recorder import RecorderConfig, SessionRecorder

HERE = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = HERE / "config.json"
LOCAL_CONFIG_PATH = HERE / "config.local.json"  # per-machine overrides, not in git

DEFAULTS = {
    "base_output_dir": str(Path.home() / "Documents" / "InteractionLogs"),
    "hotkeys": True,
    "hotkey": "<ctrl>+<shift>+<f9>",
    "quit_hotkey": "<ctrl>+<shift>+<f10>",
    "screenshot_freq": 5.0,
    "max_frame_width": 1920,
    "screenshot_format": "png",
    "webp_quality": 85,
    "webp_method": 2,
    "mouse_move_throttle": 0.05,
    "window_poll_interval": 0.5,
    "capture_mouse_moves": True,
    "dedupe_screenshots": True,
    "motion_threshold": 0.001,
    "mask_text_keys": False,
    "capture_trigger": "interval",
    "event_frame_delay": 0.3,
    "event_frame_min_gap": 1.0,
    "keyframe_interval": 60.0,
    "input_scope": [],
    "live_log": "off",
    "min_free_gb": 10.0,
    "start_delay": 0.35,
    "status_interval": 15.0,
}

log = logging.getLogger("daemon")


def control_names(mode: str) -> dict:
    """Named kernel objects shared with logger_ctl.py."""
    base = f"Local\\ADIInteractionLogger.{mode}"
    return {"mutex": base, "stop": base + ".stop", "show": base + ".show"}


def state_path(settings: dict) -> Path:
    mode = settings.get("_mode") or "default"
    return Path(settings["base_output_dir"]).expanduser() / ".state" / f"{mode}.json"


class LoggerApp:
    """Owns the hotkey listener, the bar, and at most one live recording."""

    def __init__(self, settings: dict):
        self.settings = settings
        self.mode = settings.get("_mode", "default")
        self.root_dir = Path(settings["base_output_dir"]).expanduser()
        self.base_dir = self.root_dir / self.mode if settings.get("_mode") else self.root_dir
        self.base_dir.mkdir(parents=True, exist_ok=True)
        self.live_dir = self.root_dir / ".live"

        self.recorder_config = RecorderConfig(
            base_output_dir=self.base_dir,
            screenshot_freq=float(settings["screenshot_freq"]),
            max_frame_width=int(settings["max_frame_width"]),
            screenshot_format=str(settings["screenshot_format"]).lower(),
            webp_quality=int(settings["webp_quality"]),
            webp_method=int(settings["webp_method"]),
            mouse_move_throttle=float(settings["mouse_move_throttle"]),
            window_poll_interval=float(settings["window_poll_interval"]),
            capture_mouse_moves=bool(settings["capture_mouse_moves"]),
            dedupe_screenshots=bool(settings["dedupe_screenshots"]),
            motion_threshold=float(settings["motion_threshold"]),
            mask_text_keys=bool(settings["mask_text_keys"]),
            capture_trigger=str(settings["capture_trigger"]),
            event_frame_delay=float(settings["event_frame_delay"]),
            event_frame_min_gap=float(settings["event_frame_min_gap"]),
            keyframe_interval=float(settings["keyframe_interval"]),
            input_scope=list(settings["input_scope"]),
            live_log=str(settings["live_log"]),
            live_control_dir=self.live_dir,
            mode=self.mode,
        )

        self.recorder: SessionRecorder | None = None
        self.pending_hwnd: int | None = None
        self.pending_window: dict | None = None
        self._stopping = False
        self._exit_on_cancel = False
        self._last_result: dict | None = None
        self._mutex = None

        self.signals: queue.Queue = queue.Queue()
        self.root = tk.Tk()
        self.root.withdraw()
        self.bar = CommandBar(self.root, on_submit=self._on_submit, on_cancel=self._on_cancel)
        self.hotkeys: keyboard.GlobalHotKeys | None = None

    # -------------------------------------------------------------------- setup

    def run(self) -> int:
        if self.settings.get("_mode") and not self._claim_instance():
            print(f"a {self.mode} logger is already running")
            return 3
        awareness = winutil.enable_dpi_awareness()
        if self.settings["hotkeys"]:
            try:
                self.hotkeys = keyboard.GlobalHotKeys({
                    self.settings["hotkey"]: lambda: self.signals.put("toggle"),
                    self.settings["quit_hotkey"]: lambda: self.signals.put("quit"),
                })
                self.hotkeys.start()
            except ValueError as exc:
                log.error("bad hotkey definition (%s). Use pynput syntax, e.g. <ctrl>+<shift>+<f9>", exc)
                return 2

        cfg = self.recorder_config
        print(f"interaction logger ready ({self.mode})")
        if self.hotkeys:
            print(f"  {self.settings['hotkey']:<24} start / stop a recording")
            print(f"  {self.settings['quit_hotkey']:<24} quit")
        print(f"  sessions -> {self.base_dir}")
        width = f"<={cfg.max_frame_width}px wide" if cfg.max_frame_width else "native resolution"
        when = (f"{cfg.screenshot_freq:g} fps" if cfg.capture_trigger == "interval" else
                f"after clicks/keys (>= {cfg.event_frame_min_gap:g}s apart) + keyframe every "
                f"{cfg.keyframe_interval:g}s")
        print(f"  frames: {when}, {width}, {cfg.screenshot_format}, dpi={awareness}, audio off")
        if cfg.input_scope:
            print(f"  input and frames only while focused: {', '.join(cfg.input_scope)}")
        if cfg.live_log != "off":
            status = live_status(self.live_dir)
            print(f"  Live log: {cfg.live_log}, " + (
                f"connected (Live {status.get('live_version')})" if status else
                "Live not connected - open Live with the ADI_Logger control surface enabled"))
        if "onedrive" in str(self.root_dir).lower():
            print("  WARNING: sessions are inside OneDrive and will be uploaded; set base_output_dir")

        self._write_state()
        self.root.after(50, self._pump)
        if self.settings.get("_description"):
            self.root.after(150, lambda: self._start_recording(self.settings["_description"]))
        elif self.settings.get("_open_bar"):
            self._exit_on_cancel = True
            self.root.after(150, self._open_bar)
        try:
            self.root.mainloop()
        except KeyboardInterrupt:
            self._shutdown()
        return 0

    def _claim_instance(self) -> bool:
        import win32api
        import win32event
        import winerror
        names = control_names(self.mode)
        self._mutex = win32event.CreateMutex(None, True, names["mutex"])
        if win32api.GetLastError() == winerror.ERROR_ALREADY_EXISTS:
            return False
        stop = win32event.CreateEvent(None, False, False, names["stop"])
        show = win32event.CreateEvent(None, False, False, names["show"])

        def wait():
            while True:
                which = win32event.WaitForMultipleObjects([stop, show], False, win32event.INFINITE)
                if which == win32event.WAIT_OBJECT_0:
                    self.signals.put("quit")
                    return
                self.signals.put("show")

        threading.Thread(target=wait, name="control", daemon=True).start()
        return True

    # ------------------------------------------------------------- signal pump

    def _pump(self) -> None:
        """Drain hotkey signals. Must survive any failure below it: if this loop
        stops being rescheduled the daemon goes deaf to the hotkey."""
        try:
            while True:
                try:
                    signal = self.signals.get_nowait()
                except queue.Empty:
                    break
                if signal == "quit":
                    self._shutdown()
                    return
                try:
                    if signal == "toggle":
                        self._toggle()
                    elif signal == "show" and self.recorder is None and not self.bar.visible:
                        self._open_bar()
                except Exception:
                    log.exception("hotkey handling failed; daemon still listening")
        finally:
            if not self._stopping:
                try:
                    self.root.after(50, self._pump)
                except tk.TclError:
                    pass

    def _toggle(self) -> None:
        if self.recorder is not None:
            self._stop_recording()
        elif self.bar.visible:
            self.bar.hide()
            self._on_cancel()
        else:
            self._open_bar()

    # ---------------------------------------------------------------- bar flow

    def _open_bar(self) -> None:
        # Latch the window the user is actually in, before our bar takes focus.
        self.pending_hwnd, self.pending_window = winutil.window_info()
        title = (self.pending_window or {}).get("title", "")
        process = (self.pending_window or {}).get("process_name", "")
        label = f"{self.mode} log" if self.settings.get("_mode") else ""
        target = f"{title}  ({process})" if title else process
        self.bar.show(target_title=f"{label}  ·  {target}" if label and target else label or target)

    def _on_cancel(self) -> None:
        self.pending_hwnd = self.pending_window = None
        print("cancelled")
        if self._exit_on_cancel:
            self.signals.put("quit")

    def _on_submit(self, description: str) -> None:
        self._exit_on_cancel = False
        # Give focus back so the first recorded frame shows the real target
        # window, not our bar's leftovers, then start after a short settle.
        winutil.focus_window(self.pending_hwnd)
        delay_ms = int(float(self.settings["start_delay"]) * 1000)
        self.root.after(delay_ms, lambda: self._start_recording(description))

    # ------------------------------------------------------------- record flow

    def _start_recording(self, description: str) -> None:
        hwnd, window = self.pending_hwnd, self.pending_window
        self.pending_hwnd = self.pending_window = None
        if self._low_on_disk():
            return
        recorder = SessionRecorder(self.recorder_config, description,
                                   target_hwnd=hwnd, target_window=window)
        try:
            session_dir = recorder.start()
        except Exception as exc:
            log.error("could not start recording: %s", exc)
            print(f"failed to start: {exc}")
            return
        self.recorder = recorder
        print(f'recording "{description}" -> {session_dir}')
        self._write_state()
        self._schedule_status()

    def _low_on_disk(self) -> bool:
        free_gb = shutil.disk_usage(self.base_dir).free / 1e9
        if free_gb < float(self.settings["min_free_gb"]):
            print(f"only {free_gb:.1f} GB free on {self.base_dir.anchor} "
                  f"(min_free_gb is {self.settings['min_free_gb']}); not recording")
            return True
        return False

    def _schedule_status(self) -> None:
        interval = float(self.settings["status_interval"])
        if interval > 0:
            self.root.after(int(interval * 1000), self._status_tick)

    def _status_tick(self) -> None:
        if self.recorder is None:
            return
        if self._low_on_disk():
            print("stopping to protect the disk")
            self._stop_recording()
            return
        elapsed = time.time() - self.recorder.started_at
        stats = self.recorder.stats_snapshot()
        saved = stats.get("frames_saved", 0)
        clicks = stats.get("mouse_click", 0)
        print(f"  {elapsed:6.0f}s  frames={saved}  clicks={clicks}")
        self._schedule_status()

    def _stop_recording(self) -> None:
        recorder, self.recorder = self.recorder, None
        try:
            result = recorder.stop()
        except Exception as exc:
            log.error("error while stopping: %s", exc)
            print(f"stop failed: {exc}")
            self._write_state()
            return
        counts = result.stats
        size_mb = sum(f.stat().st_size for f in result.session_dir.rglob("*") if f.is_file()) / 1e6
        live = result.live or {}
        self._last_result = {
            "description": result.description,
            "session_dir": str(result.session_dir),
            "duration_seconds": round(result.duration, 1),
            "size_mb": round(size_mb, 1),
            "frames_saved": counts.get("frames_saved", 0),
            "clicks": counts.get("mouse_click", 0),
            "live_events": live.get("events", 0),
        }
        print(f'saved "{result.description}"  {result.duration:.1f}s  {size_mb:.1f} MB  -> {result.session_dir}')
        print(f"  frames={counts.get('frames_saved', 0)} "
              f"deduped={counts.get('frames_deduped', 0)} "
              f"late={counts.get('frames_late', 0)} "
              f"clicks={counts.get('mouse_click', 0)} "
              f"keys={counts.get('key_press', 0)} "
              f"windows={counts.get('window_change', 0)}"
              + (f" live_events={live.get('events', 0)}" if live else ""))
        self._write_state()

    def _write_state(self) -> None:
        if not self.settings.get("_mode"):
            return
        import os
        path = state_path(self.settings)
        state = {
            "pid": os.getpid(),
            "mode": self.mode,
            "state": "stopped" if self._stopping else ("recording" if self.recorder else "idle"),
            "session_dir": str(self.recorder.session_dir) if self.recorder else None,
            "description": self.recorder.description if self.recorder else None,
            "started_at": self.recorder.started_at if self.recorder else None,
            "last_session": self._last_result,
            "t": time.time(),
        }
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(state, indent=2, ensure_ascii=False), encoding="utf-8")
        except OSError as exc:
            log.warning("could not write %s: %s", path, exc)

    # ---------------------------------------------------------------- shutdown

    def _shutdown(self) -> None:
        self._stopping = True
        if self.recorder is not None:
            print("stopping active recording before exit...")
            try:
                self._stop_recording()
            except Exception:
                log.exception("failed to stop the active recording cleanly")
        if self.hotkeys:
            try:
                self.hotkeys.stop()
            except Exception:
                log.exception("failed to release the hotkey listener")
        self._write_state()
        try:
            self.root.destroy()
        except tk.TclError:
            pass
        print("bye")


def _read_json(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except Exception as exc:
        print(f"ignoring {path}: {exc}", file=sys.stderr)
        return {}


def load_settings(argv: list[str] | None = None) -> dict:
    settings = dict(DEFAULTS)

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH,
                        help="JSON file with any of the settings (default: config.json beside this script)")
    parser.add_argument("--mode", choices=["simple", "deep"],
                        help="apply that section of the config's \"modes\" and record into <base>/<mode>/")
    parser.add_argument("--open-bar", action="store_true",
                        help="open the description bar at launch; Esc on it quits")
    parser.add_argument("--description", help="start recording at launch with this description, no bar")
    parser.add_argument("--log-file", type=Path, help="append console output to this file")
    parser.add_argument("--base-dir", help="where session folders are written")
    parser.add_argument("--hotkey", help="start/stop hotkey, pynput syntax")
    parser.add_argument("--quit-hotkey", help="quit hotkey, pynput syntax")
    parser.add_argument("--fps", type=float, help="screenshots per second")
    parser.add_argument("--max-frame-width", type=int,
                        help="downscale frames to this width (0 = native resolution)")
    parser.add_argument("--format", choices=["png", "jpeg", "webp"],
                        help="screenshot format (png keeps upstream tooling happy)")
    parser.add_argument("--no-mouse-moves", action="store_true",
                        help="log clicks and scrolls only, not the mouse path")
    parser.add_argument("--no-dedupe", action="store_true",
                        help="write every frame even when the screen did not change")
    parser.add_argument("--mask-text", action="store_true",
                        help="replace typed characters with * in the log")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    # config.json, then config.local.json; each may carry a "modes" section.
    modes: dict = {}
    for path in (args.config, LOCAL_CONFIG_PATH):
        data = _read_json(path) if path else {}
        for name, section in data.pop("modes", {}).items():
            modes.setdefault(name, {}).update(section)
        settings.update(data)
    if args.mode:
        settings.update(modes.get(args.mode, {}))

    if args.base_dir:
        settings["base_output_dir"] = args.base_dir
    if args.hotkey:
        settings["hotkey"] = args.hotkey
    if args.quit_hotkey:
        settings["quit_hotkey"] = args.quit_hotkey
    if args.fps:
        settings["screenshot_freq"] = args.fps
    if args.max_frame_width is not None:
        settings["max_frame_width"] = args.max_frame_width
    if args.format:
        settings["screenshot_format"] = args.format
    if args.no_mouse_moves:
        settings["capture_mouse_moves"] = False
    if args.no_dedupe:
        settings["dedupe_screenshots"] = False
    if args.mask_text:
        settings["mask_text_keys"] = True

    settings["_mode"] = args.mode
    settings["_open_bar"] = args.open_bar
    settings["_description"] = args.description
    settings["_log_file"] = args.log_file
    settings["_verbose"] = args.verbose
    return settings


def main(argv: list[str] | None = None) -> int:
    settings = load_settings(argv)
    log_file = settings.get("_log_file")
    if log_file or sys.stdout is None:
        # pythonw has no console; keep the status lines somewhere readable.
        path = Path(log_file) if log_file else state_path(settings).with_suffix(".log")
        path.parent.mkdir(parents=True, exist_ok=True)
        sys.stdout = sys.stderr = open(path, "a", encoding="utf-8", buffering=1)
        print(f"\n--- {time.strftime('%Y-%m-%d %H:%M:%S')} ---")
    # A piped or legacy console is cp1252 here; never let an encoding error
    # escape into a signal handler.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError, ValueError):
            pass
    logging.basicConfig(
        level=logging.DEBUG if settings.get("_verbose") else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )
    return LoggerApp(settings).run()


if __name__ == "__main__":
    raise SystemExit(main())
