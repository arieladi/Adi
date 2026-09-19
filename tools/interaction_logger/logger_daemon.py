"""Hotkey daemon: press the hotkey, describe what you are about to do, press it
again when you are done.

    hotkey  ->  command bar  ->  Enter  ->  recording starts
    hotkey  ->  recording stops, session saved under the description you typed

Run it and leave it running. Nothing is captured until you hit Enter on the bar.
"""
from __future__ import annotations

import argparse
import json
import logging
import queue
import sys
import time
import tkinter as tk
from pathlib import Path

from pynput import keyboard

import winutil
from overlay import CommandBar
from recorder import RecorderConfig, SessionRecorder

HERE = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = HERE / "config.json"

DEFAULTS = {
    "base_output_dir": str(Path.home() / "Documents" / "InteractionLogs"),
    "hotkey": "<ctrl>+<shift>+<f9>",
    "quit_hotkey": "<ctrl>+<shift>+<f10>",
    "screenshot_freq": 5.0,
    "max_frame_width": 1920,
    "screenshot_format": "png",
    "mouse_move_throttle": 0.05,
    "window_poll_interval": 0.5,
    "capture_mouse_moves": True,
    "dedupe_screenshots": True,
    "motion_threshold": 0.001,
    "mask_text_keys": False,
    "start_delay": 0.35,
    "status_interval": 15.0,
}

log = logging.getLogger("daemon")


class LoggerApp:
    """Owns the hotkey listener, the bar, and at most one live recording."""

    def __init__(self, settings: dict):
        self.settings = settings
        self.base_dir = Path(settings["base_output_dir"]).expanduser()
        self.base_dir.mkdir(parents=True, exist_ok=True)

        self.recorder_config = RecorderConfig(
            base_output_dir=self.base_dir,
            screenshot_freq=float(settings["screenshot_freq"]),
            max_frame_width=int(settings["max_frame_width"]),
            screenshot_format=str(settings["screenshot_format"]).lower(),
            mouse_move_throttle=float(settings["mouse_move_throttle"]),
            window_poll_interval=float(settings["window_poll_interval"]),
            capture_mouse_moves=bool(settings["capture_mouse_moves"]),
            dedupe_screenshots=bool(settings["dedupe_screenshots"]),
            motion_threshold=float(settings["motion_threshold"]),
            mask_text_keys=bool(settings["mask_text_keys"]),
        )

        self.recorder: SessionRecorder | None = None
        self.pending_hwnd: int | None = None
        self.pending_window: dict | None = None
        self._stopping = False

        self.signals: queue.Queue = queue.Queue()
        self.root = tk.Tk()
        self.root.withdraw()
        self.bar = CommandBar(self.root, on_submit=self._on_submit, on_cancel=self._on_cancel)
        self.hotkeys: keyboard.GlobalHotKeys | None = None

    # -------------------------------------------------------------------- setup

    def run(self) -> int:
        awareness = winutil.enable_dpi_awareness()
        try:
            self.hotkeys = keyboard.GlobalHotKeys({
                self.settings["hotkey"]: lambda: self.signals.put("toggle"),
                self.settings["quit_hotkey"]: lambda: self.signals.put("quit"),
            })
            self.hotkeys.start()
        except ValueError as exc:
            log.error("bad hotkey definition (%s). Use pynput syntax, e.g. <ctrl>+<shift>+<f9>", exc)
            return 2

        print("interaction logger ready")
        print(f"  {self.settings['hotkey']:<24} start / stop a recording")
        print(f"  {self.settings['quit_hotkey']:<24} quit")
        print(f"  sessions -> {self.base_dir}")
        cfg = self.recorder_config
        width = f"<={cfg.max_frame_width}px wide" if cfg.max_frame_width else "native resolution"
        print(f"  {cfg.screenshot_freq:g} fps, full screen, {width}, "
              f"{cfg.screenshot_format}, dpi={awareness}, audio off")

        self.root.after(50, self._pump)
        try:
            self.root.mainloop()
        except KeyboardInterrupt:
            self._shutdown()
        return 0

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
            self._on_cancel()
        else:
            self._open_bar()

    # ---------------------------------------------------------------- bar flow

    def _open_bar(self) -> None:
        # Latch the window the user is actually in, before our bar takes focus.
        self.pending_hwnd, self.pending_window = winutil.window_info()
        title = (self.pending_window or {}).get("title", "")
        process = (self.pending_window or {}).get("process_name", "")
        self.bar.show(target_title=f"{title}  ({process})" if title else process)

    def _on_cancel(self) -> None:
        self.pending_hwnd = self.pending_window = None
        print("cancelled")

    def _on_submit(self, description: str) -> None:
        # Give focus back so the first recorded frame shows the real target
        # window, not our bar's leftovers, then start after a short settle.
        winutil.focus_window(self.pending_hwnd)
        delay_ms = int(float(self.settings["start_delay"]) * 1000)
        self.root.after(delay_ms, lambda: self._start_recording(description))

    # ------------------------------------------------------------- record flow

    def _start_recording(self, description: str) -> None:
        hwnd, window = self.pending_hwnd, self.pending_window
        self.pending_hwnd = self.pending_window = None
        recorder = SessionRecorder(self.recorder_config, description,
                                   target_hwnd=hwnd, target_window=window)
        try:
            session_dir = recorder.start()
        except Exception as exc:
            log.error("could not start recording: %s", exc)
            print(f"failed to start: {exc}")
            return
        self.recorder = recorder
        print(f'recording "{description}" -> {session_dir.name}')
        self._schedule_status()

    def _schedule_status(self) -> None:
        interval = float(self.settings["status_interval"])
        if interval > 0:
            self.root.after(int(interval * 1000), self._status_tick)

    def _status_tick(self) -> None:
        if self.recorder is None:
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
            return
        counts = result.stats
        print(f'saved "{result.description}"  {result.duration:.1f}s  -> {result.session_dir}')
        print(f"  frames={counts.get('frames_saved', 0)} "
              f"deduped={counts.get('frames_deduped', 0)} "
              f"dropped={counts.get('frames_dropped', 0)} "
              f"clicks={counts.get('mouse_click', 0)} "
              f"keys={counts.get('key_press', 0)} "
              f"windows={counts.get('window_change', 0)}")

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
        try:
            self.root.destroy()
        except tk.TclError:
            pass
        print("bye")


def load_settings(argv: list[str] | None = None) -> dict:
    settings = dict(DEFAULTS)

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH,
                        help="JSON file with any of the settings (default: config.json beside this script)")
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

    if args.config and args.config.exists():
        try:
            settings.update(json.loads(args.config.read_text(encoding="utf-8")))
        except Exception as exc:
            print(f"ignoring {args.config}: {exc}", file=sys.stderr)

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

    settings["_verbose"] = args.verbose
    return settings


def main(argv: list[str] | None = None) -> int:
    settings = load_settings(argv)
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
