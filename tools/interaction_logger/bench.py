"""What each mode costs on this machine: CPU, input-hook stalls, disk per hour.

Records the real screen for a few seconds per mode into a temp folder (deleted
afterwards), with the mode's own settings from config.json. Simple mode gets a
synthetic "click" every 2 seconds. Nothing is typed or clicked for real.

    .venv\\Scripts\\python.exe bench.py [seconds-per-mode]
"""
from __future__ import annotations

import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

import psutil

import winutil
from logger_daemon import load_settings
from recorder import RecorderConfig, SessionRecorder


def hook_probe(stop: threading.Event, lates: list) -> None:
    """Stands in for a pynput callback: how late does a 2 ms wake-up run?"""
    while not stop.is_set():
        t0 = time.perf_counter()
        time.sleep(0.002)
        lates.append(time.perf_counter() - t0 - 0.002)


def bench(mode: str, seconds: float, out: Path) -> dict:
    s = load_settings(["--mode", mode])
    config = RecorderConfig(
        base_output_dir=out / mode,
        screenshot_freq=float(s["screenshot_freq"]), max_frame_width=int(s["max_frame_width"]),
        screenshot_format=s["screenshot_format"], webp_quality=int(s["webp_quality"]),
        webp_method=int(s["webp_method"]), capture_mouse_moves=bool(s["capture_mouse_moves"]),
        dedupe_screenshots=bool(s["dedupe_screenshots"]), capture_trigger=s["capture_trigger"],
        event_frame_delay=float(s["event_frame_delay"]), event_frame_min_gap=float(s["event_frame_min_gap"]),
        keyframe_interval=float(s["keyframe_interval"]), input_scope=[], mode=mode)
    rec = SessionRecorder(config, f"bench {mode}")
    session = rec.start()

    me = psutil.Process()
    child = psutil.Process(rec._proc.pid)
    me_cpu0, child_cpu0 = sum(me.cpu_times()[:2]), sum(child.cpu_times()[:2])
    stop, lates = threading.Event(), []
    probe = threading.Thread(target=hook_probe, args=(stop, lates))
    probe.start()
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(2.0 if config.capture_trigger == "events" else 0.5)
        if config.capture_trigger == "events":
            rec._want_frame()
    wall = time.time() - t0
    child_cpu = sum(child.cpu_times()[:2]) - child_cpu0
    me_cpu = sum(me.cpu_times()[:2]) - me_cpu0
    stop.set()
    probe.join()
    result = rec.stop()

    shots = [p for p in (session / "screenshots").iterdir() if p.name != "frame_reference.png"]
    avg_frame = sum(p.stat().st_size for p in shots) / max(1, len(shots))
    total = sum(p.stat().st_size for p in session.rglob("*") if p.is_file())
    lates.sort()
    if config.capture_trigger == "interval":
        worst_rate = config.screenshot_freq  # every frame different
    else:
        worst_rate = 1.0 / config.event_frame_min_gap  # clicking non-stop
    return {
        "mode": mode,
        "frame": f"{rec._frame_size[0]}x{rec._frame_size[1]} {config.screenshot_format}",
        "source": f"{rec._source_size[0]}x{rec._source_size[1]}",
        "capture_cpu_one_core": 100 * child_cpu / wall,
        "capture_cpu_machine": 100 * child_cpu / wall / psutil.cpu_count(),
        "hooks_process_cpu_one_core": 100 * me_cpu / wall,
        "hook_p99_ms": 1000 * lates[int(len(lates) * 0.99)],
        "hook_max_ms": 1000 * lates[-1],
        "frames": result.stats.get("frames_saved", 0),
        "deduped": result.stats.get("frames_deduped", 0),
        "late": result.stats.get("frames_late", 0),
        "avg_frame_kb": avg_frame / 1e3,
        "measured_mb_per_hour": total / wall * 3600 / 1e6,
        "worst_mb_per_hour": avg_frame * worst_rate * 3600 / 1e6,
    }


def main() -> int:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20
    winutil.enable_dpi_awareness()
    out = Path(tempfile.mkdtemp(prefix="ail_bench_"))
    try:
        for mode in ("simple", "deep"):
            r = bench(mode, seconds, out)
            print(f"{r['mode']}: source {r['source']} -> frames {r['frame']}")
            print(f"  capture process   {r['capture_cpu_one_core']:5.1f}% of one core "
                  f"({r['capture_cpu_machine']:.1f}% of the machine), below-normal priority")
            print(f"  hooks process     {r['hooks_process_cpu_one_core']:5.1f}% of one core")
            print(f"  input hook delay  p99 {r['hook_p99_ms']:.2f} ms, max {r['hook_max_ms']:.2f} ms")
            print(f"  frames            {r['frames']} saved, {r['deduped']} unchanged, {r['late']} late; "
                  f"{r['avg_frame_kb']:.0f} KB each")
            print(f"  disk              {r['measured_mb_per_hour']:.0f} MB/hour as measured, "
                  f"{r['worst_mb_per_hour']:.0f} MB/hour if every frame changed")
    finally:
        shutil.rmtree(out, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
