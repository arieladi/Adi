"""What the desktop shortcuts run.

    logger_ctl.py start simple   launch the simple logger; its bar asks what you are about to do
    logger_ctl.py start deep     same for a deep recording
    logger_ctl.py stop           stop and save whatever is recording, both modes
    logger_ctl.py status

The daemon itself runs windowless (pythonw) and writes its console output to
<base_output_dir>/.state/<mode>.log.
"""
from __future__ import annotations

import ctypes
import json
import msvcrt
import subprocess
import sys
import time
from pathlib import Path

import pywintypes
import win32event

from logger_daemon import control_names, load_settings, state_path

HERE = Path(__file__).resolve().parent
MODES = ("simple", "deep")
_EVENT_MODIFY_STATE = 0x0002
_DETACHED = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP


def _settings(mode: str) -> dict:
    return load_settings(["--mode", mode])


def _running(mode: str):
    """The daemon's mutex handle if a logger in this mode is alive, else None."""
    try:
        return win32event.OpenMutex(win32event.SYNCHRONIZE, False, control_names(mode)["mutex"])
    except pywintypes.error:
        return None


def _signal(mode: str, which: str) -> bool:
    try:
        event = win32event.OpenEvent(_EVENT_MODIFY_STATE, False, control_names(mode)[which])
    except pywintypes.error:
        return False
    win32event.SetEvent(event)
    return True


def _state(mode: str) -> dict:
    try:
        return json.loads(state_path(_settings(mode)).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _hold(seconds: float) -> None:
    """Keep the shortcut's window readable for a moment; any key closes it."""
    end = time.time() + seconds
    while time.time() < end:
        if msvcrt.kbhit():
            msvcrt.getwch()
            return
        time.sleep(0.1)


def _console(show: bool) -> None:
    """Hide this window while the bar is up: the bar would otherwise latch it as
    the window to hand focus back to, and the first frame would show it."""
    hwnd = ctypes.windll.kernel32.GetConsoleWindow()
    if hwnd:
        ctypes.windll.user32.ShowWindow(hwnd, 5 if show else 0)


def start(mode: str) -> int:
    if _running(mode):
        state = _state(mode)
        if state.get("state") == "recording":
            print(f'The {mode} log is already recording: "{state.get("description")}"')
            print("Use Stop first if you want to start a new one.")
            _hold(8)
            return 1
        _console(False)
        _signal(mode, "show")
        return 0

    pythonw = Path(sys.executable).with_name("pythonw.exe")
    if not pythonw.exists():
        pythonw = Path(sys.executable)
    _console(False)
    launched = time.time()
    proc = subprocess.Popen([str(pythonw), str(HERE / "logger_daemon.py"), "--mode", mode, "--open-bar"],
                            cwd=str(HERE), creationflags=_DETACHED, close_fds=True)
    # The daemon writes its state file once it is up; wait for that, not for the user to type.
    while time.time() - launched < 15:
        if proc.poll() is not None:
            break
        if _state(mode).get("t", 0) >= launched:
            return 0  # the bar on screen is the confirmation
        time.sleep(0.2)
    _console(True)
    log_path = state_path(_settings(mode)).with_suffix(".log")
    print(f"The {mode} logger did not start (exit code {proc.poll()}). Last lines of {log_path}:")
    try:
        print("".join(log_path.read_text(encoding="utf-8", errors="replace").splitlines(True)[-15:]))
    except OSError:
        print("  (no log)")
    _hold(60)
    return 1


def stop() -> int:
    stopped = []
    for mode in MODES:
        mutex = _running(mode)
        if mutex is None:
            continue
        print(f"stopping the {mode} log...")
        _signal(mode, "stop")
        result = win32event.WaitForSingleObject(mutex, 90_000)
        if result == win32event.WAIT_TIMEOUT:
            print(f"  the {mode} logger did not exit within 90s; check {state_path(_settings(mode)).with_suffix('.log')}")
            continue
        stopped.append(mode)
        last = _state(mode).get("last_session")
        if last:
            minutes = last["duration_seconds"] / 60
            print(f'  saved "{last["description"]}"  {minutes:.1f} min, {last["size_mb"]} MB, '
                  f'{last["frames_saved"]} frames, {last["clicks"]} clicks, {last["live_events"]} Live events')
            print(f'  -> {last["session_dir"]}')
        else:
            print("  nothing was recording")
    if not stopped:
        print("No logger is running.")
    _hold(15)
    return 0


def status() -> int:
    for mode in MODES:
        state = _state(mode) if _running(mode) else {}
        if state.get("state") == "recording":
            minutes = (time.time() - state["started_at"]) / 60
            print(f'{mode:<7} recording {minutes:.0f} min: "{state["description"]}" -> {state["session_dir"]}')
        elif state:
            print(f"{mode:<7} running, idle")
        else:
            print(f"{mode:<7} not running")
    return 0


def main(argv: list[str]) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError, ValueError):
            pass
    if len(argv) == 2 and argv[0] == "start" and argv[1] in MODES:
        return start(argv[1])
    if argv == ["stop"]:
        return stop()
    if argv == ["status"]:
        return status()
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
