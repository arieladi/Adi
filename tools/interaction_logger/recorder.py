"""Start/stop screen + input recorder.

Writes a session directory in the layout AIComputerInteractionLogger's
dataset_exporter.py reads:

    <session>/events.csv        Timestamp,EventType,Data
    <session>/screenshots/*.png
    <session>/session.json      our own metadata (description, target window, stats)

The Data column formats match that repo exactly, so its exporters
(Claude computer-use / OSWorld / HuggingFace) work on these sessions unchanged.

Coordinates: events are stored in *frame* pixels, i.e. translated by the virtual
desktop origin and multiplied by the capture scale, so a recorded (x, y) indexes
straight into the saved screenshot. session.json carries the exact transform to
get back to raw screen coordinates.
"""
from __future__ import annotations

import csv
import json
import logging
import queue
import re
import threading
import time
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import mss
from PIL import Image, ImageChops
from pynput import keyboard, mouse

import winutil

log = logging.getLogger("recorder")

_UNSAFE = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
_WS = re.compile(r"\s+")
_SENTINEL = object()

_THUMB = (160, 90)  # motion comparison size
_THUMB_PIXELS = _THUMB[0] * _THUMB[1]
_THUMB_NOISE = 8  # per-pixel luma delta ignored as noise

_FORMATS = {
    "png": ".png",
    "jpeg": ".jpg",
    "webp": ".webp",
}


def _open_mss():
    """mss 10 renamed the factory; keep working on either."""
    return getattr(mss, "MSS", mss.mss)()


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
    mouse_move_throttle: float = 0.05
    window_poll_interval: float = 0.5
    capture_mouse_moves: bool = True
    dedupe_screenshots: bool = True
    motion_threshold: float = 0.001  # fraction of the thumbnail that must change
    mask_text_keys: bool = False
    frame_queue_size: int = 8


@dataclass
class SessionResult:
    session_dir: Path
    description: str
    started_at: float
    ended_at: float
    stats: Counter = field(default_factory=Counter)

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
        self._frame_q: queue.Queue = queue.Queue(maxsize=config.frame_queue_size)
        self._threads: list[threading.Thread] = []
        self._mouse_listener: mouse.Listener | None = None
        self._key_listener: keyboard.Listener | None = None

        self._csv_file = None
        self._csv_writer = None
        self._stats = Counter()
        self._stats_lock = threading.Lock()

        # geometry, resolved in start()
        self._origin = (0, 0)
        self._source_size = (0, 0)
        self._frame_size = (0, 0)
        self._scale = (1.0, 1.0)
        self._reduce_factor = 1
        self._suffix = _FORMATS.get(config.screenshot_format, ".png")

        self._last_move_emit = 0.0
        self._last_window_info: dict | None = None
        self._last_thumb: Image.Image | None = None
        self._last_frame_name: str | None = None
        self._wrote_reference_png = False

    # ---------------------------------------------------------------- geometry

    def _resolve_geometry(self) -> None:
        with _open_mss() as sct:
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

    # --------------------------------------------------------------- lifecycle

    def start(self) -> Path:
        if self.config.screenshot_format not in _FORMATS:
            raise ValueError(f"screenshot_format must be one of {sorted(_FORMATS)}")

        self._resolve_geometry()

        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.session_dir = self.config.base_output_dir / f"{slugify(self.description)}_{stamp}"
        (self.session_dir / "screenshots").mkdir(parents=True, exist_ok=True)

        self._csv_file = open(self.session_dir / "events.csv", "w", newline="", encoding="utf-8")
        self._csv_writer = csv.writer(self._csv_file)
        self._csv_writer.writerow(["Timestamp", "EventType", "Data"])

        self.started_at = time.time()
        self._running.set()

        if self.target_window:
            self._last_window_info = self.target_window
            self._event_q.put((self.started_at, "window_change",
                               json.dumps(self.target_window, ensure_ascii=False)))

        self._spawn(self._writer_loop, "writer")
        self._spawn(self._frame_writer_loop, "frame-writer")
        self._spawn(self._capture_loop, "capture")
        self._spawn(self._window_loop, "window")

        self._mouse_listener = mouse.Listener(
            on_move=self._on_move, on_click=self._on_click, on_scroll=self._on_scroll)
        self._key_listener = keyboard.Listener(
            on_press=self._on_press, on_release=self._on_release)
        self._mouse_listener.start()
        self._key_listener.start()

        log.info("recording %dx%d -> %s", *self._frame_size, self.session_dir)
        return self.session_dir

    def stop(self) -> SessionResult:
        if not self._running.is_set():
            raise RuntimeError("recorder is not running")
        self.ended_at = time.time()
        self._running.clear()

        for listener in (self._mouse_listener, self._key_listener):
            if listener:
                listener.stop()

        # Frames first: the frame writer still needs to queue its CSV rows.
        for name in ("capture", "window", "frame-writer"):
            for t in self._threads:
                if t.name == name:
                    t.join(timeout=10)

        self._event_q.put(_SENTINEL)
        for t in self._threads:
            if t.name == "writer":
                t.join(timeout=15)

        if self._csv_file:
            self._csv_file.close()

        result = SessionResult(
            session_dir=self.session_dir,
            description=self.description,
            started_at=self.started_at,
            ended_at=self.ended_at,
            stats=Counter(self._stats),
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

    def _on_move(self, x, y):
        if not (self._running.is_set() and self.config.capture_mouse_moves):
            return
        now = time.time()
        if now - self._last_move_emit < self.config.mouse_move_throttle:
            return
        self._last_move_emit = now
        fx, fy = self._to_frame(x, y)
        self._event_q.put((now, "mouse_move", f"x={fx}, y={fy}"))

    def _on_click(self, x, y, button, pressed):
        if self._running.is_set():
            fx, fy = self._to_frame(x, y)
            self._event_q.put((time.time(), "mouse_click",
                               f"x={fx}, y={fy}, button={button}, pressed={pressed}"))

    def _on_scroll(self, x, y, dx, dy):
        if self._running.is_set():
            fx, fy = self._to_frame(x, y)
            self._event_q.put((time.time(), "mouse_scroll",
                               f"x={fx}, y={fy}, dx={dx}, dy={dy}"))

    def _key_repr(self, key) -> str:
        text = str(key)
        if self.config.mask_text_keys and not text.startswith("Key."):
            return "'*'"
        return text

    def _on_press(self, key):
        if self._running.is_set():
            self._event_q.put((time.time(), "key_press", f"key={self._key_repr(key)}"))

    def _on_release(self, key):
        if self._running.is_set():
            self._event_q.put((time.time(), "key_release", f"key={self._key_repr(key)}"))

    # ----------------------------------------------------------------- capture

    def _capture_loop(self) -> None:
        interval = 1.0 / max(0.1, self.config.screenshot_freq)
        try:
            with _open_mss() as sct:
                monitor = sct.monitors[0]  # whole virtual desktop
                next_at = time.perf_counter()
                while self._running.is_set():
                    shot = sct.grab(monitor)
                    stamp = time.time()
                    try:
                        self._frame_q.put_nowait((stamp, shot.size, bytes(shot.rgb)))
                    except queue.Full:
                        self._count("frames_dropped")
                    next_at += interval
                    delay = next_at - time.perf_counter()
                    if delay > 0:
                        time.sleep(delay)
                    else:
                        next_at = time.perf_counter()  # fell behind; don't burst
        except Exception as exc:
            log.error("capture loop died: %s", exc)

    def _frame_writer_loop(self) -> None:
        shots_dir = self.session_dir / "screenshots"
        while True:
            try:
                stamp, size, rgb = self._frame_q.get(timeout=0.2)
            except queue.Empty:
                if not self._running.is_set():
                    return
                continue
            try:
                name = self._persist_frame(shots_dir, stamp, size, rgb)
                self._event_q.put((stamp, "screenshot", name))
            except Exception as exc:
                log.error("failed to save frame: %s", exc)

    def _downscale(self, image: Image.Image) -> Image.Image:
        if self._reduce_factor > 1:
            return image.reduce(self._reduce_factor)
        if image.size != self._frame_size:
            return image.resize(self._frame_size, Image.BOX)
        return image

    def _is_still(self, image: Image.Image) -> bool:
        """True when this frame is visually the same as the one we just wrote."""
        thumb = image.resize(_THUMB, Image.BOX).convert("L")
        previous, self._last_thumb = self._last_thumb, thumb
        if previous is None:
            return False
        histogram = ImageChops.difference(thumb, previous).histogram()
        changed = sum(histogram[_THUMB_NOISE + 1:]) / _THUMB_PIXELS
        return changed <= self.config.motion_threshold

    def _persist_frame(self, shots_dir: Path, stamp: float, size, rgb: bytes) -> str:
        image = self._downscale(Image.frombytes("RGB", (size.width, size.height), rgb))

        if self.config.dedupe_screenshots and self._last_frame_name and self._is_still(image):
            # Screen did not change: log the tick, reuse the file already on disk.
            self._count("frames_deduped")
            return self._last_frame_name

        name = f"screenshot_{int(stamp * 1000)}{self._suffix}"
        self._save_image(image, shots_dir / name)

        if self._suffix != ".png" and not self._wrote_reference_png:
            # Upstream's exporter sniffs resolution from the first *.png it finds;
            # leave it exactly one to find.
            image.save(shots_dir / "frame_reference.png", "PNG", compress_level=6)
            self._wrote_reference_png = True

        self._last_frame_name = name
        self._count("frames_saved")
        return name

    def _save_image(self, image: Image.Image, path: Path) -> None:
        fmt = self.config.screenshot_format
        if fmt == "png":
            image.save(path, "PNG", compress_level=self.config.png_compress_level)
        elif fmt == "jpeg":
            image.save(path, "JPEG", quality=self.config.jpeg_quality, subsampling=0)
        else:
            image.save(path, "WEBP", quality=self.config.webp_quality, method=2)

    # ------------------------------------------------------------ window watch

    def _window_loop(self) -> None:
        while self._running.is_set():
            try:
                _, info = winutil.window_info()
                if info and info != self._last_window_info:
                    self._last_window_info = info
                    self._event_q.put((time.time(), "window_change",
                                       json.dumps(info, ensure_ascii=False)))
            except Exception as exc:
                log.debug("window poll failed: %s", exc)
            time.sleep(self.config.window_poll_interval)

    # ------------------------------------------------------------- csv writing

    def _writer_loop(self) -> None:
        while True:
            item = self._event_q.get()
            if item is _SENTINEL:
                return
            stamp, event_type, data = item
            try:
                self._csv_writer.writerow([stamp, event_type, data])
                self._count(event_type)
            except Exception as exc:
                log.error("failed to write %s: %s", event_type, exc)

    # ---------------------------------------------------------------- metadata

    def _write_metadata(self, result: SessionResult) -> None:
        with _open_mss() as sct:
            monitors = [dict(m) for m in sct.monitors]
        size_bytes = sum(f.stat().st_size for f in self.session_dir.rglob("*") if f.is_file())
        meta = {
            "description": self.description,
            "session_dir": self.session_dir.name,
            "started_at": self.started_at,
            "started_at_iso": datetime.fromtimestamp(self.started_at, timezone.utc).isoformat(),
            "ended_at": self.ended_at,
            "duration_seconds": round(result.duration, 3),
            "target_window": self.target_window,
            "target_hwnd": self.target_hwnd,
            "capture": {
                "scope": "full_screen",
                "screenshot_freq": self.config.screenshot_freq,
                "screenshot_format": self.config.screenshot_format,
                "source_size": {"width": self._source_size[0], "height": self._source_size[1]},
                "frame_size": {"width": self._frame_size[0], "height": self._frame_size[1]},
                "mouse_move_throttle": self.config.mouse_move_throttle,
                "capture_mouse_moves": self.config.capture_mouse_moves,
                "dedupe_screenshots": self.config.dedupe_screenshots,
                "motion_threshold": self.config.motion_threshold,
                "mask_text_keys": self.config.mask_text_keys,
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
            "event_counts": dict(sorted(result.stats.items())),
            "size_bytes": size_bytes,
            "format": "AIComputerInteractionLogger events.csv v1",
        }
        (self.session_dir / "session.json").write_text(
            json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")
        (self.session_dir / "label.txt").write_text(self.description + "\n", encoding="utf-8")
