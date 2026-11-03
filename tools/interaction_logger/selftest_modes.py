"""The simple-mode recorder: event-triggered frames, focus scope, Live handshake.

Fakes the foreground window so the test does not depend on what you have
open: first "Live" (in scope), then a browser (out of scope).

    .venv\\Scripts\\python.exe selftest_modes.py
"""
from __future__ import annotations

import csv
import json
import shutil
import tempfile
import time
from pathlib import Path

from PIL import Image

import recorder as recorder_module
import winutil
from recorder import RecorderConfig, SessionRecorder

LIVE = {"title": "riser.als - Ableton Live 11 Suite", "process_name": "Ableton Live 11 Suite.exe",
        "pid": 1, "bounds": {"left": 0, "top": 0, "width": 800, "height": 600}}
BROWSER = {"title": "my bank - private", "process_name": "chrome.exe",
           "pid": 2, "bounds": {"left": 0, "top": 0, "width": 800, "height": 600}}

failures = []


def check(label, ok, detail=""):
    print(f"  [{'ok ' if ok else 'FAIL'}] {label}{(' - ' + str(detail)) if detail else ''}")
    if not ok:
        failures.append(label)


def main() -> int:
    winutil.enable_dpi_awareness()
    out = Path(tempfile.mkdtemp(prefix="ail_modes_"))
    foreground = [LIVE]
    recorder_module.winutil.window_info = lambda hwnd=None: (1, foreground[0])

    config = RecorderConfig(
        base_output_dir=out / "simple", capture_trigger="events", event_frame_delay=0.2,
        event_frame_min_gap=0.5, keyframe_interval=60, max_frame_width=1280,
        screenshot_format="webp", webp_quality=70, capture_mouse_moves=False,
        window_poll_interval=0.1, input_scope=["Ableton Live*"], live_log="simple",
        live_control_dir=out / ".live", mode="simple")
    rec = SessionRecorder(config, "we are now working on a real avastha project", 1, LIVE)
    session = rec.start()
    control_files = list((out / ".live").glob("*.json"))

    x, y = rec._source_size[0] // 2, rec._source_size[1] // 2
    time.sleep(1.0)
    rec._on_click(x, y, "Button.left", True)
    rec._on_click(x, y, "Button.left", False)  # release -> one frame ~0.2s later
    rec._on_press("Key.ctrl")
    time.sleep(1.0)
    foreground[0] = BROWSER
    time.sleep(0.4)  # window poll notices
    rec._on_click(x, y, "Button.left", True)
    rec._on_click(x, y, "Button.left", False)
    rec._on_press("'p'")
    time.sleep(1.0)
    result = rec.stop()

    rows = list(csv.DictReader((session / "events.csv").open(encoding="utf-8")))
    by_type = {}
    for r in rows:
        by_type.setdefault(r["EventType"], []).append(r)
    meta = json.loads((session / "session.json").read_text(encoding="utf-8"))
    shots = sorted((session / "screenshots").glob("*.webp"))

    print("checks:")
    check("handshake file present while recording", len(control_files) == 1, [p.name for p in control_files])
    check("handshake file removed at stop", not list((out / ".live").glob("*.json")))
    check("in-scope click + key logged", len(by_type.get("mouse_click", [])) == 2
          and len(by_type.get("key_press", [])) == 1,
          f"{len(by_type.get('mouse_click', []))} clicks, {len(by_type.get('key_press', []))} keys")
    check("out-of-scope input dropped", result.stats.get("input_out_of_scope", 0) == 3,
          result.stats.get("input_out_of_scope", 0))
    frames = by_type.get("screenshot", [])
    check("frames: first one + one after the click, no interval frames", len(frames) == 2, len(frames))
    if len(frames) == 2:
        click_t = float(by_type["mouse_click"][1]["Timestamp"])
        lag = float(frames[1]["Timestamp"]) - click_t
        check("click frame taken after the click settles", 0.15 <= lag <= 0.8, f"{lag:.2f}s")
    windows = [json.loads(r["Data"]) for r in by_type.get("window_change", [])]
    browser = [w for w in windows if w["process_name"] == "chrome.exe"]
    check("other app's window logged with its title blanked", browser and browser[0]["title"] == "", browser)
    check("Live window title kept", any(w["title"] == LIVE["title"] for w in windows))
    check("webp frames written", len(shots) >= 1, len(shots))
    check("reference png for upstream", (session / "screenshots" / "frame_reference.png").exists())
    if shots:
        width = Image.open(shots[0]).size[0]
        check("frames downscaled to 1280 wide", width == min(1280, rec._source_size[0]), width)
    check("session.json complete with mode", meta["complete"] and meta["mode"] == "simple"
          and meta["capture"]["trigger"] == "events")
    check("Live link recorded in metadata", meta["live"]["level"] == "simple"
          and meta["live"]["connected_at_start"] is False, meta["live"])
    check("label keeps the full description",
          (session / "label.txt").read_text(encoding="utf-8").strip()
          == "we are now working on a real avastha project")

    shutil.rmtree(out, ignore_errors=True)
    print("PASS" if not failures else f"{len(failures)} CHECK(S) FAILED: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
