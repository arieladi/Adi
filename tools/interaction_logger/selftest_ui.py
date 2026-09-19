"""Scripted run of the whole flow: bar opens, description submitted, recording
starts, second toggle stops and saves.

Drives the daemon's own signal queue instead of injecting keystrokes, so nothing
is typed into whatever window you happen to have open. The bar will flash on
screen for a second - that is the point.

    .venv\\Scripts\\python.exe selftest_ui.py
"""
from __future__ import annotations

import json
import shutil
import tempfile
from pathlib import Path

from logger_daemon import LoggerApp, load_settings

DESCRIPTION = "ui flow check"
RECORD_MS = 2500


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="ail_uitest_"))
    settings = load_settings([])
    settings["base_output_dir"] = str(tmp)
    settings["status_interval"] = 0
    settings["screenshot_freq"] = 4

    app = LoggerApp(settings)
    notes: list[str] = []
    failures: list[str] = []

    def record(label: str, ok: bool, detail: str = "") -> None:
        (notes if ok else failures).append(label)
        print(f"  [{'ok ' if ok else 'FAIL'}] {label}{(' - ' + detail) if detail else ''}")

    def step_open():
        app.signals.put("toggle")

    def step_submit():
        record("bar opened on first toggle", app.bar.visible)
        record("bar latched the foreground window", bool(app.pending_window),
               (app.pending_window or {}).get("process_name", "none"))
        app.bar.entry.insert(0, DESCRIPTION)
        app.bar._submit()
        record("bar hidden after submit", not app.bar.visible)

    def step_check_recording():
        record("recording started", app.recorder is not None)

    def step_stop():
        app.signals.put("toggle")

    def step_finish():
        record("recording stopped", app.recorder is None)
        app.signals.put("quit")

    app.root.after(400, step_open)
    app.root.after(1000, step_submit)
    app.root.after(1800, step_check_recording)
    app.root.after(1800 + RECORD_MS, step_stop)
    app.root.after(2600 + RECORD_MS, step_finish)

    print("scripted ui run:")
    app.run()

    sessions = sorted(p for p in tmp.iterdir() if p.is_dir())
    record("exactly one session folder", len(sessions) == 1,
           ", ".join(p.name for p in sessions) or "none")
    if sessions:
        session = sessions[0]
        record("folder named after the typed description",
               session.name.startswith("ui-flow-check"), session.name)
        meta = json.loads((session / "session.json").read_text(encoding="utf-8"))
        record("description stored in metadata", meta["description"] == DESCRIPTION)
        record("frames captured", meta["event_counts"].get("screenshot", 0) > 0,
               f"{meta['event_counts'].get('screenshot', 0)} frames")
        record("target window recorded", bool(meta["target_window"]),
               (meta["target_window"] or {}).get("title", "")[:40])
        record("label.txt written", (session / "label.txt").exists())

    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS" if not failures else f"{len(failures)} CHECK(S) FAILED: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
