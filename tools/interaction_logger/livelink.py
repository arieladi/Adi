"""Desktop half of the handshake with the ADI_Logger Live remote script.

The protocol is files in one control directory (base_output_dir/.live):

    <mode>-<pid>.json   written here at the start of a recording: session folder,
                        level, description. Touched every few seconds; deleted at
                        stop. The script writes <session>/live_events.jsonl while
                        it exists, and treats a file untouched for 20s as a
                        crashed logger.
    live.json           written by the script every 5s while Live runs with it
                        enabled. Fresh means "connected".
"""
from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path

STATUS_FILE = "live.json"
HEARTBEAT = 5.0
FRESH = 15.0
EVENTS_FILE = "live_events.jsonl"


def live_status(control_dir: Path) -> dict | None:
    """The script's last heartbeat, or None when Live is closed or the script is off."""
    path = control_dir / STATUS_FILE
    try:
        if time.time() - path.stat().st_mtime > FRESH:
            return None
        status = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return status if status.get("connected", True) else None


class LiveLink:
    def __init__(self, control_dir: Path, mode: str, level: str, session_dir: Path, description: str):
        self.control_dir = control_dir
        self.level = level
        self.session_dir = session_dir
        self.description = description
        self.path = control_dir / f"{mode}-{os.getpid()}.json"
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.connected_at_start = False

    def start(self) -> None:
        self.control_dir.mkdir(parents=True, exist_ok=True)
        self.connected_at_start = live_status(self.control_dir) is not None
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps({
            "level": self.level,
            "session_dir": str(self.session_dir),
            "description": self.description,
            "pid": os.getpid(),
            "started_at": time.time(),
        }, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, self.path)
        self._thread = threading.Thread(target=self._heartbeat, name="live-heartbeat", daemon=True)
        self._thread.start()

    def _heartbeat(self) -> None:
        while not self._stop.wait(HEARTBEAT):
            try:
                os.utime(self.path)
            except OSError:
                return

    def stop(self, wait: float = 3.0) -> dict:
        """Withdraw the sink and give the script a moment to write its last lines."""
        self._stop.set()
        try:
            self.path.unlink()
        except OSError:
            pass
        events = self.session_dir / EVENTS_FILE
        deadline = time.time() + wait
        ended = False
        while time.time() < deadline:
            ended = _last_event(events) == "end"
            if ended or not events.exists():
                break
            time.sleep(0.2)
        lines = 0
        if events.exists():
            with events.open(encoding="utf-8") as f:
                lines = sum(1 for _ in f)
        return {
            "level": self.level,
            "connected_at_start": self.connected_at_start,
            "events_file": EVENTS_FILE if events.exists() else None,
            "events": lines,
            "ended_cleanly": ended,
        }


def _last_event(path: Path) -> str | None:
    try:
        with path.open("rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - 4096))
            tail = f.read().decode("utf-8", "replace").strip().splitlines()
        return json.loads(tail[-1]).get("ev") if tail else None
    except (OSError, ValueError, IndexError):
        return None
