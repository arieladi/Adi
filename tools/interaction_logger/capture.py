"""Screen capture worker, run in its own below-normal-priority process.

Grabbing a 4K frame and converting it holds Python's GIL for tens of
milliseconds. In the process that owns the pynput hooks that stalls every mouse
and keyboard event system-wide (measured: up to 34 ms at 1080p, several times
that at 4K). Out here it costs only CPU, at a priority below Live's.

Two triggers:
    interval  a frame every 1/fps seconds (deep recordings)
    events    a frame shortly after each click or key the parent forwards, at
              most one per min_gap, plus a keyframe every keyframe_interval
              (long simple recordings)

Talks to the parent through one queue:
    ("ready", None) once, ("frame", stamp, name, saved), ("count", key), ("error", text),
    ("done", None) on the way out.
"""
from __future__ import annotations

import ctypes
import time
from pathlib import Path

import mss
from PIL import Image, ImageChops

import winutil

_THUMB = (160, 90)  # motion comparison size
_THUMB_PIXELS = _THUMB[0] * _THUMB[1]
_THUMB_NOISE = 8  # per-pixel luma delta ignored as noise
_BELOW_NORMAL_PRIORITY_CLASS = 0x4000


def open_mss():
    """mss 10 renamed the factory; keep working on either."""
    return getattr(mss, "MSS", mss.mss)()


class FrameWriter:
    def __init__(self, cfg: dict, shots_dir: Path):
        self.cfg = cfg
        self.shots_dir = shots_dir
        self.frame_size = tuple(cfg["frame_size"])
        self.reduce_factor = cfg["reduce_factor"]
        self.suffix = cfg["suffix"]
        self.last_thumb: Image.Image | None = None
        self.last_name: str | None = None
        self.wrote_reference_png = False

    def handle(self, shot, stamp: float) -> tuple[str, bool]:
        """Returns (file name, whether a new file was written)."""
        # BGRX straight into PIL: skips mss's pure-Python .rgb shuffle.
        image = Image.frombuffer("RGB", shot.size, shot.bgra, "raw", "BGRX", 0, 1)
        image = self._downscale(image)

        if self.cfg["dedupe"] and self.last_name and self._is_still(image):
            return self.last_name, False  # unchanged screen: reuse the file already on disk

        name = f"screenshot_{int(stamp * 1000)}{self.suffix}"
        self._save(image, self.shots_dir / name)
        if self.suffix != ".png" and not self.wrote_reference_png:
            # Upstream's exporter sniffs resolution from the first *.png it finds;
            # leave it exactly one to find.
            image.save(self.shots_dir / "frame_reference.png", "PNG", compress_level=6)
            self.wrote_reference_png = True
        self.last_name = name
        return name, True

    def _downscale(self, image: Image.Image) -> Image.Image:
        if self.reduce_factor > 1:
            return image.reduce(self.reduce_factor)
        if image.size != self.frame_size:
            return image.resize(self.frame_size, Image.BOX)
        return image

    def _is_still(self, image: Image.Image) -> bool:
        """True when this frame is visually the same as the one we just wrote."""
        thumb = image.resize(_THUMB, Image.BOX).convert("L")
        previous, self.last_thumb = self.last_thumb, thumb
        if previous is None:
            return False
        histogram = ImageChops.difference(thumb, previous).histogram()
        changed = sum(histogram[_THUMB_NOISE + 1:]) / _THUMB_PIXELS
        return changed <= self.cfg["motion_threshold"]

    def _save(self, image: Image.Image, path: Path) -> None:
        fmt = self.cfg["format"]
        if fmt == "png":
            image.save(path, "PNG", compress_level=self.cfg["png_compress_level"])
        elif fmt == "jpeg":
            image.save(path, "JPEG", quality=self.cfg["jpeg_quality"], subsampling=0)
        else:
            image.save(path, "WEBP", quality=self.cfg["webp_quality"], method=self.cfg["webp_method"])


def _lower_priority() -> None:
    kernel32 = ctypes.windll.kernel32
    kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), _BELOW_NORMAL_PRIORITY_CLASS)


def run(cfg: dict, shots_dir: str, frame_wanted, in_scope, stop, out) -> None:
    """Process entry point. Every argument comes from the parent's spawn context."""
    try:
        winutil.enable_dpi_awareness()  # same pixel space as the parent's hooks
        _lower_priority()
        writer = FrameWriter(cfg, Path(shots_dir))
        with open_mss() as sct:
            monitor = sct.monitors[0]  # whole virtual desktop

            def grab() -> None:
                if not in_scope.value:
                    out.put(("count", "frames_out_of_scope"))
                    return
                shot = sct.grab(monitor)
                stamp = time.time()
                name, saved = writer.handle(shot, stamp)
                out.put(("frame", stamp, name, saved))

            out.put(("ready", None))
            if cfg["trigger"] == "events":
                _event_loop(cfg, grab, frame_wanted, stop, out)
            else:
                _interval_loop(cfg, grab, stop, out)
    except Exception as exc:
        out.put(("error", f"capture process died: {exc!r}"))
    finally:
        out.put(("done", None))


def _interval_loop(cfg: dict, grab, stop, out) -> None:
    interval = 1.0 / max(0.1, cfg["fps"])
    next_at = time.perf_counter()
    while not stop.is_set():
        try:
            grab()
        except Exception as exc:
            out.put(("error", f"failed to save frame: {exc!r}"))
        next_at += interval
        delay = next_at - time.perf_counter()
        if delay > 0:
            stop.wait(delay)
        else:
            out.put(("count", "frames_late"))
            next_at = time.perf_counter()  # fell behind; don't burst


def _event_loop(cfg: dict, grab, frame_wanted, stop, out) -> None:
    keyframe = cfg["keyframe_interval"]
    last = time.perf_counter() - cfg["min_gap"]
    grab_now = True  # first frame straight away
    while not stop.is_set():
        if not grab_now:
            remaining = keyframe - (time.perf_counter() - last) if keyframe > 0 else 3600.0
            if frame_wanted.wait(max(0.05, remaining)):
                # Let the UI react to the click before looking at it.
                stop.wait(cfg["event_delay"])
                frame_wanted.clear()
            elif keyframe <= 0:
                continue
            if stop.is_set():
                return
        gap = cfg["min_gap"] - (time.perf_counter() - last)
        if gap > 0:
            stop.wait(gap)
            frame_wanted.clear()  # clicks during the gap are covered by this frame
        try:
            grab()
        except Exception as exc:
            out.put(("error", f"failed to save frame: {exc!r}"))
        last = time.perf_counter()
        grab_now = False
