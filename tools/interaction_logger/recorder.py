"""Start/stop screen + input recorder.

Writes a session directory in the layout AIComputerInteractionLogger's
dataset_exporter.py reads:

    <session>/events.csv        Timestamp,EventType,Data
    <session>/screenshots/*.png (or .webp/.jpg plus one frame_reference.png)
    <session>/session.json      our own metadata (description, target window, stats)
    <session>/live_events.jsonl what changed inside Live, when the ADI_Logger
                                remote script is enabled (see livelink.py)

The Data column formats match that repo exactly, so its exporters
(Claude computer-use / OSWorld / HuggingFace) work on these sessions unchanged.

Coordinates: events are stored in *frame* pixels, i.e. translated by the virtual
desktop origin and multiplied by the capture scale, so a recorded (x, y) indexes
straight into the saved screenshot. session.json carries the exact transform to
get back to raw screen coordinates.

The pynput hooks live in this process and only queue events. Frames are grabbed
and encoded in a separate, lower-priority process (capture.py) so a 4K frame
never holds the GIL while a mouse event waits.
"""
from __future__ import annotations

import csv
import fnmatch
import json
import logging
import multiprocessing
import queue
import re
import threading
import time
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from pynput import keyboard, mouse

import capture
import winutil
from livelink import LiveLink

log = logging.getLogger("recorder")

_UNSAFE = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
_WS = re.compile(r"\s+")
_SENTINEL = object()

_FORMATS = {
    "png": ".png",
    "jpeg": ".jpg",
    "webp": ".webp",
}

_FLUSH_EVERY = 2.0  # seconds; a crash loses at most this much of events.csv


def slugify(description: str, max_len: int = 60) -> str:
    """Folder-safe name that keeps the user's words (including non-ASCII ones)."""
    name = _UNSAFE.sub("", description).strip()
    name = _WS.sub("-", name).strip("-. ")
    if len(name) > max_len:
        name = name[:max_len].rstrip("-. ")
    return name or "session"


@dataclass
class RecorderConfig:
    base_output_dir: Path
    screenshot_freq: float = 5.0
    max_frame_width: int = 1920  # 0 disables downscaling
    screenshot_format: str = "png"  # png keeps upstream tooling happy
    png_compress_level: int = 6
    jpeg_quality: int = 88
    webp_quality: int = 85
    webp_method: int = 2
    mouse_move_throttle: float = 0.05
    window_poll_interval: float = 0.5
    capture_mouse_moves: bool = True
    dedupe_screenshots: bool = True
    motion_threshold: float = 0.001  # fraction of the thumbnail that must change
    mask_text_keys: bool = False
    # "interval": screenshot_freq frames a second. "events": a frame after each
    # click/key (at most one per event_frame_min_gap) plus a keyframe.
    capture_trigger: str = "interval"
    event_frame_delay: float = 0.3
    event_frame_min_gap: float = 1.0
    keyframe_interval: float = 60.0
    # fnmatch patterns on the foreground process name. Empty = record everywhere.
    # Otherwise input and frames are only taken while a matching window has
    # focus, and other windows' titles are blanked in window_change rows.
    input_scope: list[str] = field(default_factory=list)
    live_log: str = "off"  # "off" | "simple" | "deep"
    live_control_dir: Path | None = None
    mode: str = "default"


@dataclass
class SessionResult:
    session_dir: Path
    description: str
    started_at: float
    ended_at: float
    stats: Counter = field(default_factory=Counter)
    live: dict | None = None

    @property
    def duration(self) -> float:
        return self.ended_at - self.started_at


class SessionRecorder:
    """One recording. Construct, start(), later stop(). Not reusable."""

    def __init__(
        self,
        config: RecorderConfig,
        description: str,
        target_hwnd: int | None = None,
        target_window: dict | None = None,
    ):
        self.config = config
        self.description = description
        self.target_hwnd = target_hwnd
        self.target_window = target_window

        self.session_dir: Path | None = None
        self.started_at = 0.0
        self.ended_at = 0.0

        self._running = threading.Event()
        self._event_q: queue.Queue = queue.Queue()
        self._threads: list[threading.Thread] = []
        self._mouse_listener: mouse.Listener | None = None
        self._key_listener: keyboard.Listener | None = None

        self._csv_file = None
        self._csv_writer = None
        self._stats = Counter()
        self._stats_lock = threading.Lock()

        # capture process plumbing, created in start()
        self._proc = None
        self._frame_wanted = None
        self._scope_flag = None
        self._stop_capture = None
        self._frames_q = None
        self._in_scope = True
        self._live: LiveLink | None = None
        self._live_result: dict | None = None

        # geometry, resolved in start()
        self._origin = (0, 0)
        self._source_size = (0, 0)
        self._frame_size = (0, 0)
        self._scale = (1.0, 1.0)
        self._reduce_factor = 1
        self._suffix = _FORMATS.get(config.screenshot_format, ".png")

        self._last_move_emit = 0.0
        self._last_window_info: dict | None = None

    # ---------------------------------------------------------------- geometry

    def _resolve_geometry(self) -> None:
        with capture.open_mss() as sct:
            virtual = sct.monitors[0]
        self._origin = (virtual["left"], virtual["top"])
        in_w, in_h = virtual["width"], virtual["height"]
        self._source_size = (in_w, in_h)

        out_w, out_h, factor = in_w, in_h, 1
        limit = self.config.max_frame_width
        if limit and in_w > limit:
            ratio = in_w / limit
            nearest = round(ratio)
            if nearest >= 2 and abs(ratio - nearest) < 0.02:
                # exact integer factor: Image.reduce is ~4x faster than resize
                factor = nearest
                out_w, out_h = in_w // factor, in_h // factor
            else:
                out_w = limit
                out_h = max(1, round(in_h * limit / in_w))

        self._reduce_factor = factor
        self._frame_size = (out_w, out_h)
        self._scale = (out_w / in_w, out_h / in_h)

    def _to_frame(self, x: float, y: float) -> tuple[int, int]:
        return (
            round((x - self._origin[0]) * self._scale[0]),
            round((y - self._origin[1]) * self._scale[1]),
        )

    # ------------------------------------------------------------------- scope

    def _matches_scope(self, info: dict | None) -> bool:
        if not self.config.input_scope:
            return True
        name = ((info or {}).get("process_name") or "").lower()
        return any(fnmatch.fnmatch(name, p.lower()) for p in self.config.input_scope)

    def _set_scope(self, info: dict | None) -> None:
        self._in_scope = self._matches_scope(info)
        if self._scope_flag is not None:
            self._scope_flag.value = int(self._in_scope)

    def _window_row(self, info: dict) -> str:
        if not self._matches_scope(info):
            info = dict(info, title="")  # another app: keep which one, not what is in it
        return json.dumps(info, ensure_ascii=False)

    # --------------------------------------------------------------- lifecycle

    def start(self) -> Path:
        cfg = self.config
        if cfg.screenshot_format not in _FORMATS:
            raise ValueError(f"screenshot_format must be one of {sorted(_FORMATS)}")
        if cfg.capture_trigger not in ("interval", "events"):
            raise ValueError("capture_trigger must be 'interval' or 'events'")

        self._resolve_geometry()

        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.session_dir = cfg.base_output_dir / f"{slugify(self.description)}_{stamp}"
        shots_dir = self.session_dir / "screenshots"
        shots_dir.mkdir(parents=True, exist_ok=True)

        self._csv_file = open(self.session_dir / "events.csv", "w", newline="", encoding="utf-8")
        self._csv_writer = csv.writer(self._csv_file)
        self._csv_writer.writerow(["Timestamp", "EventType", "Data"])

        ctx = multiprocessing.get_context("spawn")
        self._frame_wanted = ctx.Event()
        self._stop_capture = ctx.Event()
        self._scope_flag = ctx.Value("b", 1, lock=False)
        self._frames_q = ctx.Queue()
        self._set_scope(self.target_window or winutil.window_info()[1])
        self._proc = ctx.Process(
            target=capture.run, name="frame-capture", daemon=True,
            args=(self._capture_settings(), str(shots_dir), self._frame_wanted,
                  self._scope_flag, self._stop_capture, self._frames_q))
        self._proc.start()
        self._await_capture_ready()

        self.started_at = time.time()
        self._running.set()

        if self.target_window:
            self._last_window_info = self.target_window
            self._event_q.put((self.started_at, "window_change", self._window_row(self.target_window)))

        self._spawn(self._writer_loop, "writer")
        self._spawn(self._frames_loop, "frames")
        self._spawn(self._window_loop, "window")

        self._mouse_listener = mouse.Listener(
            on_move=self._on_move, on_click=self._on_click, on_scroll=self._on_scroll)
        self._key_listener = keyboard.Listener(
            on_press=self._on_press, on_release=self._on_release)
        self._mouse_listener.start()
        self._key_listener.start()

        if cfg.live_log != "off" and cfg.live_control_dir is not None:
            self._live = LiveLink(cfg.live_control_dir, cfg.mode, cfg.live_log,
                                  self.session_dir, self.description)
            self._live.start()

        self._write_metadata(None)  # a crash still leaves a described session
        log.info("recording %dx%d -> %s", *self._frame_size, self.session_dir)
        return self.session_dir

    def _capture_settings(self) -> dict:
        cfg = self.config
        return {
            "trigger": cfg.capture_trigger,
            "fps": cfg.screenshot_freq,
            "event_delay": cfg.event_frame_delay,
            "min_gap": cfg.event_frame_min_gap,
            "keyframe_interval": cfg.keyframe_interval,
            "frame_size": self._frame_size,
            "reduce_factor": self._reduce_factor,
            "suffix": self._suffix,
            "format": cfg.screenshot_format,
            "png_compress_level": cfg.png_compress_level,
            "jpeg_quality": cfg.jpeg_quality,
            "webp_quality": cfg.webp_quality,
            "webp_method": cfg.webp_method,
            "dedupe": cfg.dedupe_screenshots,
            "motion_threshold": cfg.motion_threshold,
        }

    def _await_capture_ready(self, timeout: float = 20.0) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message = self._frames_q.get(timeout=0.5)
            except queue.Empty:
                if not self._proc.is_alive():
                    break
                continue
            if message[0] == "ready":
                return
            if message[0] == "error":
                log.error("%s", message[1])
        self._proc.kill()
        raise RuntimeError("capture process did not start")

    def stop(self) -> SessionResult:
        if not self._running.is_set():
            raise RuntimeError("recorder is not running")
        self.ended_at = time.time()
        self._running.clear()

        for listener in (self._mouse_listener, self._key_listener):
            if listener:
                listener.stop()

        self._stop_capture.set()
        self._frame_wanted.set()  # wake an event-triggered worker
        # Frames first: the frames thread still needs to queue its CSV rows.
        for name in ("frames", "window"):
            for t in self._threads:
                if t.name == name:
                    t.join(timeout=15)
        self._proc.join(timeout=5)
        if self._proc.is_alive():
            log.warning("capture process did not exit; killing it")
            self._proc.kill()

        self._event_q.put(_SENTINEL)
        for t in self._threads:
            if t.name == "writer":
                t.join(timeout=15)

        if self._csv_file:
            self._csv_file.close()

        if self._live is not None:
            self._live_result = self._live.stop()

        result = SessionResult(
            session_dir=self.session_dir,
            description=self.description,
            started_at=self.started_at,
            ended_at=self.ended_at,
            stats=Counter(self._stats),
            live=self._live_result,
        )
        self._write_metadata(result)
        log.info("stopped after %.1fs -> %s", result.duration, self.session_dir)
        return result

    def _spawn(self, target, name: str) -> None:
        t = threading.Thread(target=target, name=name, daemon=True)
        t.start()
        self._threads.append(t)

    def _count(self, key: str, n: int = 1) -> None:
        with self._stats_lock:
            self._stats[key] += n

    def stats_snapshot(self) -> Counter:
        """Live counters, safe to read from another thread."""
        with self._stats_lock:
            return Counter(self._stats)

    # ------------------------------------------------------------ input events

    def _accepting(self) -> bool:
        if not self._running.is_set():
            return False
        if not self._in_scope:
            self._count("input_out_of_scope")
            return False
        return True

    def _want_frame(self) -> None:
        if self.config.capture_trigger == "events":
            self._frame_wanted.set()

    def _on_move(self, x, y):
        if not (self.config.capture_mouse_moves and self._running.is_set() and self._in_scope):
            return
        now = time.time()
        if now - self._last_move_emit < self.config.mouse_move_throttle:
            return
        self._last_move_emit = now
        fx, fy = self._to_frame(x, y)
        self._event_q.put((now, "mouse_move", f"x={fx}, y={fy}"))

    def _on_click(self, x, y, button, pressed):
        if self._accepting():
            fx, fy = self._to_frame(x, y)
            self._event_q.put((time.time(), "mouse_click",
                               f"x={fx}, y={fy}, button={button}, pressed={pressed}"))
            if not pressed:
                self._want_frame()

    def _on_scroll(self, x, y, dx, dy):
        if self._accepting():
            fx, fy = self._to_frame(x, y)
            self._event_q.put((time.time(), "mouse_scroll",
                               f"x={fx}, y={fy}, dx={dx}, dy={dy}"))
            self._want_frame()

    def _key_repr(self, key) -> str:
        text = str(key)
        if self.config.mask_text_keys and not text.startswith("Key."):
            return "'*'"
        return text

    def _on_press(self, key):
        if self._accepting():
            self._event_q.put((time.time(), "key_press", f"key={self._key_repr(key)}"))
            self._want_frame()

    def _on_release(self, key):
        if self._accepting():
            self._event_q.put((time.time(), "key_release", f"key={self._key_repr(key)}"))

    # ----------------------------------------------------------------- capture

    def _frames_loop(self) -> None:
        """Forward the capture process's reports into the CSV until it says done."""
        while True:
            try:
                message = self._frames_q.get(timeout=1.0)
            except queue.Empty:
                if not self._proc.is_alive():
                    log.error("capture process exited unexpectedly")
                    return
                continue
            kind = message[0]
            if kind == "done":
                return
            if kind == "frame":
                _, stamp, name, saved = message
                self._count("frames_saved" if saved else "frames_deduped")
                self._event_q.put((stamp, "screenshot", name))
            elif kind == "count":
                self._count(message[1])
            elif kind == "error":
                log.error("%s", message[1])

    # ------------------------------------------------------------ window watch

    def _window_loop(self) -> None:
        while self._running.is_set():
            try:
                _, info = winutil.window_info()
                if info and info != self._last_window_info:
                    self._last_window_info = info
                    self._set_scope(info)
                    self._event_q.put((time.time(), "window_change", self._window_row(info)))
            except Exception as exc:
                log.debug("window poll failed: %s", exc)
            time.sleep(self.config.window_poll_interval)

    # ------------------------------------------------------------- csv writing

    def _writer_loop(self) -> None:
        last_flush = time.time()
        while True:
            try:
                item = self._event_q.get(timeout=_FLUSH_EVERY)
            except queue.Empty:
                item = None
            if item is _SENTINEL:
                return
            if item is not None:
                stamp, event_type, data = item
                try:
                    self._csv_writer.writerow([stamp, event_type, data])
                    self._count(event_type)
                except Exception as exc:
                    log.error("failed to write %s: %s", event_type, exc)
            if time.time() - last_flush >= _FLUSH_EVERY:
                self._csv_file.flush()
                last_flush = time.time()

    # ---------------------------------------------------------------- metadata

    def _write_metadata(self, result: SessionResult | None) -> None:
        """Written at start (complete=false) and again at stop."""
        cfg = self.config
        with capture.open_mss() as sct:
            monitors = [dict(m) for m in sct.monitors]
        meta = {
            "description": self.description,
            "session_dir": self.session_dir.name,
            "mode": cfg.mode,
            "complete": result is not None,
            "started_at": self.started_at,
            "started_at_iso": datetime.fromtimestamp(self.started_at, timezone.utc).isoformat(),
            "ended_at": result.ended_at if result else None,
            "duration_seconds": round(result.duration, 3) if result else None,
            "target_window": self.target_window,
            "target_hwnd": self.target_hwnd,
            "capture": {
                "scope": "full_screen",
                "trigger": cfg.capture_trigger,
                "screenshot_freq": cfg.screenshot_freq if cfg.capture_trigger == "interval" else None,
                "event_frame_min_gap": cfg.event_frame_min_gap if cfg.capture_trigger == "events" else None,
                "keyframe_interval": cfg.keyframe_interval if cfg.capture_trigger == "events" else None,
                "screenshot_format": cfg.screenshot_format,
                "source_size": {"width": self._source_size[0], "height": self._source_size[1]},
                "frame_size": {"width": self._frame_size[0], "height": self._frame_size[1]},
                "mouse_move_throttle": cfg.mouse_move_throttle,
                "capture_mouse_moves": cfg.capture_mouse_moves,
                "dedupe_screenshots": cfg.dedupe_screenshots,
                "motion_threshold": cfg.motion_threshold,
                "mask_text_keys": cfg.mask_text_keys,
                "input_scope": cfg.input_scope,
                "audio": False,
                "cursor_in_frames": False,
            },
            "coordinates": {
                "space": "frame_pixels",
                "note": "screen_x = frame_x / scale_x + origin_x",
                "origin": {"x": self._origin[0], "y": self._origin[1]},
                "scale": {"x": self._scale[0], "y": self._scale[1]},
            },
            "virtual_screen": monitors[0] if monitors else None,
            "monitors": monitors[1:],
            "live": result.live if result else {"level": cfg.live_log},
            "format": "AIComputerInteractionLogger events.csv v1",
        }
        if result is not None:
            meta["event_counts"] = dict(sorted(result.stats.items()))
            meta["size_bytes"] = sum(f.stat().st_size for f in self.session_dir.rglob("*") if f.is_file())
        (self.session_dir / "session.json").write_text(
            json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")
        (self.session_dir / "label.txt").write_text(self.description + "\n", encoding="utf-8")
