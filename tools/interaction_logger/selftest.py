"""Headless check that capture, writing and the output format all work.

Records a short session without the hotkey or the bar, injects a couple of
synthetic input events, then verifies the session on disk parses the way
AIComputerInteractionLogger's exporter expects.

    .venv\\Scripts\\python.exe selftest.py
"""
from __future__ import annotations

import csv
import json
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image

import winutil
from recorder import RecorderConfig, SessionRecorder, slugify

DURATION = 3.0
FPS = 5.0


def check(label: str, ok: bool, detail: str = "") -> bool:
    print(f"  [{'ok ' if ok else 'FAIL'}] {label}{(' - ' + detail) if detail else ''}")
    return ok


def main() -> int:
    winutil.enable_dpi_awareness()
    out = Path(tempfile.mkdtemp(prefix="ail_selftest_"))
    failures = 0

    print(f"recording {DURATION:g}s at {FPS:g} fps into {out}")
    hwnd, window = winutil.window_info()
    config = RecorderConfig(base_output_dir=out, screenshot_freq=FPS, dedupe_screenshots=True)
    recorder = SessionRecorder(config, "self test / sanity check", hwnd, window)
    session_dir = recorder.start()

    # Synthetic input: the listeners only see real user input, and this test has
    # to be able to run unattended. These are raw screen coordinates; the CSV
    # should hold their frame-space equivalents.
    src_w, src_h = recorder._source_size
    raw_x, raw_y = src_w - 240, src_h - 160
    expect_x, expect_y = recorder._to_frame(raw_x, raw_y)

    time.sleep(DURATION / 2)
    recorder._on_click(raw_x, raw_y, "Button.left", True)
    recorder._on_click(raw_x, raw_y, "Button.left", False)
    recorder._on_scroll(raw_x, raw_y, 0, -2)
    recorder._on_press("Key.ctrl")
    recorder._on_press("'s'")
    recorder._on_release("'s'")
    recorder._on_release("Key.ctrl")
    time.sleep(DURATION / 2)

    result = recorder.stop()
    counts = result.stats
    print(f"stopped after {result.duration:.2f}s: {dict(sorted(counts.items()))}")

    rows = list(csv.DictReader((session_dir / "events.csv").open(encoding="utf-8")))
    shots = sorted((session_dir / "screenshots").glob("*.png"))
    meta = json.loads((session_dir / "session.json").read_text(encoding="utf-8"))

    print("checks:")
    failures += not check("events.csv header", list(rows[0].keys()) == ["Timestamp", "EventType", "Data"]
                          if rows else False, f"{len(rows)} rows")
    failures += not check("screenshot files written", len(shots) >= 1, f"{len(shots)} png")
    failures += not check("screenshot rows reference real files",
                          all((session_dir / "screenshots" / r["Data"]).exists()
                              for r in rows if r["EventType"] == "screenshot"))

    expected_frames = DURATION * FPS
    frame_rows = sum(1 for r in rows if r["EventType"] == "screenshot")
    failures += not check("frame count close to fps * duration",
                          frame_rows >= expected_frames * 0.6,
                          f"{frame_rows} rows vs ~{expected_frames:.0f} expected")
    failures += not check("no frames dropped", counts.get("frames_dropped", 0) == 0,
                          f"{counts.get('frames_dropped', 0)} dropped")

    click = next((r for r in rows if r["EventType"] == "mouse_click"), None)
    failures += not check("click row present", click is not None)
    if click:
        # exactly the regexes src/dataset_exporter.py uses
        x = re.search(r"x=(\d+)", click["Data"])
        y = re.search(r"y=(\d+)", click["Data"])
        failures += not check("click parses with upstream regex", bool(x and y), click["Data"])
        failures += not check("button/pressed readable by upstream",
                              "button=Button.left" in click["Data"] and "pressed=True" in click["Data"])
        if x and y:
            got = (int(x.group(1)), int(y.group(1)))
            failures += not check("coordinates translated into frame space",
                                  got == (expect_x, expect_y),
                                  f"screen ({raw_x},{raw_y}) -> frame {got}, expected {(expect_x, expect_y)}")
            frame_w, frame_h = recorder._frame_size
            failures += not check("coordinates land inside the frame",
                                  0 <= got[0] < frame_w and 0 <= got[1] < frame_h,
                                  f"frame is {frame_w}x{frame_h}")

    key = next((r for r in rows if r["EventType"] == "key_press"), None)
    failures += not check("key row parses with upstream regex",
                          bool(key and re.search(r"key=(.+)", key["Data"])),
                          key["Data"] if key else "no key rows")

    win_row = next((r for r in rows if r["EventType"] == "window_change"), None)
    if win_row:
        info = json.loads(win_row["Data"])
        failures += not check("window_change schema",
                              set(info) == {"title", "process_name", "pid", "bounds"},
                              f"{info['process_name']} / {info['title'][:32]}")
    else:
        failures += not check("window_change row present", False)

    failures += not check("timestamps monotonic per stream",
                          all(float(a["Timestamp"]) <= float(b["Timestamp"])
                              for a, b in zip(rows, rows[1:])
                              if a["EventType"] == b["EventType"]))
    failures += not check("metadata describes the session",
                          meta["description"] == "self test / sanity check"
                          and meta["capture"]["audio"] is False
                          and meta["duration_seconds"] > 0)

    first_shot = Image.open(shots[0]) if shots else None
    failures += not check("saved frames match the recorded frame size",
                          bool(first_shot) and first_shot.size == recorder._frame_size,
                          f"{first_shot.size if first_shot else '-'} vs {recorder._frame_size}")
    failures += not check("metadata round-trips coordinates",
                          bool(first_shot) and abs(
                              (expect_x / meta["coordinates"]["scale"]["x"]
                               + meta["coordinates"]["origin"]["x"]) - raw_x) <= 2,
                          "frame -> screen within 2px")
    failures += not check("folder named after the description",
                          session_dir.name.startswith(slugify("self test / sanity check")),
                          session_dir.name)

    size_mb = sum(f.stat().st_size for f in session_dir.rglob("*")) / 1e6
    print(f"session size: {size_mb:.2f} MB for {result.duration:.1f}s "
          f"({size_mb / max(result.duration, 0.1) * 3600:.0f} MB/hour at this frame rate)")

    shutil.rmtree(out, ignore_errors=True)
    print("PASS" if not failures else f"{failures} CHECK(S) FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
