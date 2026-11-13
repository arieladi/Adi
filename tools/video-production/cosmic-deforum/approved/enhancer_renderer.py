"""Bounded dual-GPU video enhancement. Python 3.12+, PyTorch, Spandrel, PyAV.

Silent output by design: the music master is attached at final assembly.
Existing source files and the approved renderer are never modified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import math
import queue
import subprocess
import threading
import time
import traceback
from dataclasses import asdict, dataclass
from fractions import Fraction
from pathlib import Path
from typing import Callable, TypeVar

import av
import cv2
import numpy as np
import torch
import imageio_ffmpeg

from models import Interpolator, Upscaler

LOG = logging.getLogger(__name__)
T = TypeVar("T")


@dataclass(frozen=True)
class Config:
    input: str
    output: str
    model: str
    rife_root: str
    order: str = "upscale-first"
    upscale_device: str = "cuda:0"
    rife_device: str = "cuda:1"
    fps: str = "60"
    width: int = 3840
    height: int = 2160
    fp16: bool = True
    compile: bool = False
    extra_arches: bool = False
    tile: int = 256
    tile_padding: int = 32
    min_tile: int = 64
    queue_depth: int = 3
    pinned_gb: float = 2.0
    flow_scale: float = 1.0
    cut_threshold: float = 0.30
    encoder: str = "h264_nvenc"
    encode_device: int = 0
    quality: int = 18
    ffmpeg: str = "auto"
    max_frames: int = 0


@dataclass
class Packet:
    pixels: torch.Tensor
    number: int
    thumbnail: np.ndarray
    pool: "Pool"

    def release(self) -> None:
        self.pool.free.put_nowait(self.pixels)


class Cancelled(Exception):
    pass


def take(q: queue.Queue[T], stop: threading.Event) -> T:
    while not stop.is_set():
        try:
            return q.get(timeout=0.2)
        except queue.Empty:
            pass
    raise Cancelled()


def put(q: queue.Queue[T], item: T, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            q.put(item, timeout=0.2)
            return
        except queue.Full:
            pass
    raise Cancelled()


class Pool:
    def __init__(self, count: int, h: int, w: int) -> None:
        self.free: queue.Queue[torch.Tensor] = queue.Queue(count)
        for _ in range(count):
            self.free.put(torch.empty((h, w, 3), dtype=torch.uint8, pin_memory=True))

    def packet(self, number: int, thumb: np.ndarray, stop: threading.Event) -> Packet:
        return Packet(take(self.free, stop), number, thumb, self)


def resolve_device(value: str) -> torch.device:
    if value.startswith("GPU-"):
        matches = [i for i in range(torch.cuda.device_count())
                   if str(torch.cuda.get_device_properties(i).uuid).removeprefix("GPU-").lower() == value[4:].lower()]
        if len(matches) != 1:
            raise ValueError(f"GPU UUID not visible to PyTorch: {value}")
        return torch.device("cuda", matches[0])
    device = torch.device(value)
    if device.type != "cuda" or device.index is None:
        raise ValueError("Device must be cuda:N or a visible GPU UUID")
    torch.cuda.get_device_properties(device)
    return device


def is_cut(left: np.ndarray, right: np.ndarray, threshold: float) -> bool:
    # Compare original decode thumbnails even when RIFE follows upscaling.
    # Heuristic: flashes can be flagged too; this is configurable, not semantic.
    return threshold > 0 and float(np.mean(np.abs(left-right))) >= threshold


def output_positions(source_frames: int, src_fps: Fraction, dst_fps: Fraction) -> list[Fraction]:
    count = math.ceil(Fraction(source_frames) * dst_fps / src_fps)
    return [Fraction(i) * src_fps / dst_fps for i in range(count)]


def validate(c: Config) -> None:
    if c.order not in ("upscale-first", "interpolate-first"):
        raise ValueError("Unknown pipeline order")
    if c.encoder not in ("h264_nvenc", "hevc_nvenc", "libx264"):
        raise ValueError("Unsupported encoder")
    if min(c.width, c.height) < 2 or c.width % 2 or c.height % 2:
        raise ValueError("Output dimensions must be positive even numbers")
    if c.queue_depth < 1 or c.pinned_gb <= 0:
        raise ValueError("Queue depth and pinned-memory budget must be positive")
    if c.min_tile < 16 or c.tile < c.min_tile or c.tile_padding < 0:
        raise ValueError("Invalid tile geometry")
    if c.flow_scale not in (1.0, 0.5, 0.25):
        raise ValueError("flow_scale must be 1, 0.5, or 0.25")
    if Fraction(c.fps) <= 0 or not 0 <= c.cut_threshold <= 1 or c.max_frames < 0:
        raise ValueError("Invalid FPS, cut threshold, or frame limit")
    for p in (Path(c.input), Path(c.model), Path(c.rife_root)/"train_log/flownet.pkl"):
        if not p.is_file():
            raise FileNotFoundError(p)
    if Path(c.output).exists():
        raise FileExistsError(c.output)


def run(c: Config) -> None:
    validate(c)
    torch.set_num_threads(2)
    up_device, rife_device = resolve_device(c.upscale_device), resolve_device(c.rife_device)
    for d in {up_device, rife_device}:
        LOG.info("%s = %s", d, torch.cuda.get_device_name(d))
    with av.open(c.input) as media:
        video = media.streams.video[0]
        if not video.average_rate:
            raise ValueError("Input has no usable frame rate")
        source_fps = Fraction(video.average_rate)
        sw, sh = video.width, video.height
    target_fps = Fraction(c.fps)
    if target_fps < source_fps:
        raise ValueError("This interpolation renderer does not reduce frame rate")
    dimensions = [(sh, sw), (c.height, c.width) if c.order == "upscale-first" else (sh, sw), (c.height, c.width)]
    counts = [c.queue_depth+3] * 3
    locked_bytes = sum(n*h*w*3 for n, (h, w) in zip(counts, dimensions))
    if locked_bytes > c.pinned_gb * 1024**3:
        raise ValueError(f"Pools need {locked_bytes/1024**3:.2f} GiB locked RAM; lower --queue-depth or raise --pinned-gb")
    LOG.info("Reusable pinned buffers: %.1f MiB", locked_bytes / 1024**2)
    pools = [Pool(n, h, w) for n, (h, w) in zip(counts, dimensions)]
    qs: list[queue.Queue[Packet | None]] = [queue.Queue(c.queue_depth) for _ in range(3)]
    stop = threading.Event()
    errors: queue.Queue[str] = queue.Queue()
    stats: dict[str, float | int] = {"decoded": 0, "encoded": 0, "cuts": 0}
    output = Path(c.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.stem + ".partial.mp4")
    if partial.exists():
        raise FileExistsError(f"Inspect previous incomplete output first: {partial}")
    began = time.perf_counter()

    def decode() -> None:
        with av.open(c.input) as media:
            stream = media.streams.video[0]
            first_time: Fraction | None = None
            for n, frame in enumerate(media.decode(stream)):
                if c.max_frames and n >= c.max_frames:
                    break
                if frame.pts is None:
                    raise ValueError("Input missing timestamps")
                stamp = Fraction(frame.pts) * frame.time_base
                if first_time is None:
                    first_time = stamp
                if abs((stamp-first_time)*source_fps-n) > Fraction(1, 10):
                    raise ValueError("Variable-frame-rate input: normalize to CFR before rendering")
                rgb = frame.to_ndarray(format="rgb24")
                thumb = cv2.resize(rgb, (64, 36), interpolation=cv2.INTER_AREA).astype(np.float32)/255
                packet = pools[0].packet(n, thumb, stop)
                packet.pixels.copy_(torch.from_numpy(rgb))
                put(qs[0], packet, stop)
                stats["decoded"] = n+1
        put(qs[0], None, stop)

    def upscale_stage(qi: int) -> None:
        with torch.cuda.device(up_device), torch.cuda.stream(torch.cuda.Stream(device=up_device)):
            up = Upscaler(Path(c.model), up_device, c.fp16, c.compile, c.tile,
                          c.tile_padding, c.min_tile, c.extra_arches)
            while True:
                packet = take(qs[qi], stop)
                if packet is None:
                    break
                out = pools[qi+1].packet(packet.number, packet.thumbnail, stop)
                started = time.perf_counter()
                up.render(packet.pixels, out.pixels)
                stats["upscale_seconds"] = stats.get("upscale_seconds", 0) + time.perf_counter()-started
                packet.release()
                put(qs[qi+1], out, stop)
            stats["final_tile"] = up.tile
            put(qs[qi+1], None, stop)

    def interpolate_stage(qi: int) -> None:
        with torch.cuda.device(rife_device), torch.cuda.stream(torch.cuda.Stream(device=rife_device)):
            rife = Interpolator(Path(c.rife_root), rife_device, c.fp16, c.compile, c.flow_scale)
            left = take(qs[qi], stop)
            if left is None:
                raise ValueError("Empty input")
            left_gpu = rife.upload(left.pixels)
            j = 0
            while True:
                right = take(qs[qi], stop)
                last = right is None
                right_gpu = left_gpu if last else rife.upload(right.pixels)
                cut = False if last else is_cut(left.thumbnail, right.thumbnail, c.cut_threshold)
                if cut:
                    stats["cuts"] += 1
                # Each original frame owns [i,i+1); the final frame holds to duration.
                while Fraction(j)*source_fps/target_fps < left.number+1:
                    fraction = float(Fraction(j)*source_fps/target_fps-left.number)
                    out = pools[qi+1].packet(j, left.thumbnail, stop)
                    started = time.perf_counter()
                    if cut or last or fraction <= 1e-9:
                        out.pixels.copy_(left.pixels)
                    else:
                        rife.render(left_gpu, right_gpu, fraction, out.pixels)
                    stats["rife_seconds"] = stats.get("rife_seconds", 0) + time.perf_counter()-started
                    put(qs[qi+1], out, stop)
                    j += 1
                # Upload must complete before releasing/recycling a pinned source.
                torch.cuda.current_stream(rife_device).synchronize()
                left.release()
                if last:
                    break
                left, left_gpu = right, right_gpu
            put(qs[qi+1], None, stop)

    def encode() -> None:
        executable = imageio_ffmpeg.get_ffmpeg_exe() if c.ffmpeg == "auto" else c.ffmpeg
        args = [executable, "-hide_banner", "-loglevel", "error", "-n", "-f", "rawvideo", "-pix_fmt", "rgb24",
                "-s", f"{c.width}x{c.height}", "-r", str(target_fps), "-i", "pipe:0", "-an", "-c:v", c.encoder]
        if c.encoder == "libx264":
            args += ["-preset", "fast", "-crf", str(c.quality), "-threads", "2"]
        else:
            args += ["-gpu", str(c.encode_device), "-preset", "p5", "-rc", "vbr", "-cq", str(c.quality), "-b:v", "0"]
        args += ["-pix_fmt", "yuv420p", "-movflags", "+faststart", str(partial)]
        with output.with_suffix(".ffmpeg.log").open("wb") as log:
            process = subprocess.Popen(args, stdin=subprocess.PIPE, stderr=log, bufsize=0)
            assert process.stdin is not None
            try:
                while True:
                    packet = take(qs[2], stop)
                    if packet is None:
                        break
                    data = memoryview(packet.pixels.numpy()).cast("B")
                    while data:
                        n = process.stdin.write(data)
                        if not n:
                            raise BrokenPipeError("Encoder stopped accepting frames")
                        data = data[n:]
                    packet.release()
                    stats["encoded"] += 1
                process.stdin.close()
                if process.wait(timeout=120):
                    raise RuntimeError(f"FFmpeg failed; see {output.with_suffix('.ffmpeg.log')}")
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

    def guarded(name: str, fn: Callable[[], None]) -> None:
        try:
            fn()
        except Cancelled:
            pass
        except Exception:
            errors.put(name + ":\n" + traceback.format_exc())
            stop.set()

    stages: list[tuple[str, Callable[[], None]]] = [("decode", decode),
        ("upscale", lambda: upscale_stage(0 if c.order == "upscale-first" else 1)),
        ("rife", lambda: interpolate_stage(1 if c.order == "upscale-first" else 0)), ("encode", encode)]
    workers = [threading.Thread(target=guarded, args=(name, fn), name=name, daemon=True) for name, fn in stages]
    for worker in workers:
        worker.start()
    try:
        last_report = 0.0
        while any(w.is_alive() for w in workers):
            for worker in workers:
                worker.join(timeout=0.1)
            if time.perf_counter()-last_report > 15:
                LOG.info("Progress %s", stats)
                last_report = time.perf_counter()
            if stop.is_set():
                break
    except KeyboardInterrupt:
        stop.set()
        raise
    finally:
        if stop.is_set():
            for worker in workers:
                worker.join(timeout=10)
    if not errors.empty():
        raise RuntimeError(errors.get())
    if stop.is_set():
        raise RuntimeError("Pipeline cancelled")
    expected = math.ceil(Fraction(int(stats["decoded"]))*target_fps/source_fps)
    if stats["encoded"] != expected:
        raise RuntimeError(f"Frame count mismatch: {stats} expected={expected}")
    # Decode the completed result before publishing it as final.
    with av.open(str(partial)) as media:
        stream = media.streams.video[0]
        if (stream.width, stream.height) != (c.width, c.height) or Fraction(stream.average_rate) != target_fps:
            raise RuntimeError("Output geometry/FPS verification failed")
        if sum(1 for _ in media.decode(stream)) != expected:
            raise RuntimeError("Output decode/frame count verification failed")
    with partial.open("rb") as handle:
        digest = hashlib.file_digest(handle, "sha256").hexdigest()
    stats["wall_seconds"] = time.perf_counter()-began
    stats["output_fps_throughput"] = expected / float(stats["wall_seconds"])
    partial.replace(output)
    output.with_suffix(".receipt.json").write_text(json.dumps(
        {"config": asdict(c), "stats": stats, "sha256": digest,
         "upscale_gpu": torch.cuda.get_device_name(up_device), "rife_gpu": torch.cuda.get_device_name(rife_device)}, indent=2))
    LOG.info("VERIFIED %s %s", output, stats)


def parse() -> Config:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path)
    # Every dataclass field is exposed; JSON defaults can be overridden on CLI.
    for name, field in Config.__dataclass_fields__.items():
        flag = "--" + name.replace("_", "-")
        if field.type == "bool":
            parser.add_argument(flag, action=argparse.BooleanOptionalAction, default=argparse.SUPPRESS)
        else:
            parser.add_argument(flag, type={"int": int, "float": float}.get(field.type, str), default=argparse.SUPPRESS)
    args = vars(parser.parse_args())
    config = args.pop("config")
    values = json.loads(config.read_text()) if config else {}
    values.update(args)
    return Config(**values)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(threadName)s %(levelname)s %(message)s")
    run(parse())
