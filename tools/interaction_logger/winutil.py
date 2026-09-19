"""Windows helpers shared by the command bar and the recorder.

One definition of "which window is the user in", and one pixel coordinate space
for pynput events and mss frames.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes

_PER_MONITOR_AWARE_V2 = -4
_DWMWA_EXTENDED_FRAME_BOUNDS = 9
_SW_RESTORE = 9


def enable_dpi_awareness() -> str:
    """Opt into DPI awareness so mouse coordinates and screenshots share a scale.

    Without this, on a scaled display pynput reports physical pixels while a
    non-aware process sees a virtualised (smaller) screen, and every recorded
    click lands off-target in the captured frame.
    """
    user32 = ctypes.windll.user32
    try:
        user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_ssize_t]
        user32.SetProcessDpiAwarenessContext.restype = ctypes.c_int
        if user32.SetProcessDpiAwarenessContext(_PER_MONITOR_AWARE_V2):
            return "per-monitor-v2"
    except (AttributeError, OSError):
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
        return "per-monitor"
    except (AttributeError, OSError):
        pass
    try:
        user32.SetProcessDPIAware()
        return "system"
    except (AttributeError, OSError):
        return "none"


def _extended_frame_bounds(hwnd: int) -> tuple[int, int, int, int]:
    """Visible frame rect. GetWindowRect includes invisible resize borders."""
    rect = wintypes.RECT()
    res = ctypes.windll.dwmapi.DwmGetWindowAttribute(
        wintypes.HWND(hwnd),
        ctypes.c_uint(_DWMWA_EXTENDED_FRAME_BOUNDS),
        ctypes.byref(rect),
        ctypes.sizeof(rect),
    )
    if res != 0:
        raise OSError(f"DwmGetWindowAttribute failed with 0x{res & 0xFFFFFFFF:08x}")
    return rect.left, rect.top, rect.right, rect.bottom


def window_info(hwnd: int | None = None) -> tuple[int, dict] | tuple[None, None]:
    """Return (hwnd, info); info matches the upstream window_change schema."""
    import win32gui
    import win32process
    import psutil

    if hwnd is None:
        hwnd = win32gui.GetForegroundWindow()
    if not hwnd:
        return None, None

    try:
        title = win32gui.GetWindowText(hwnd)
    except Exception:
        title = ""

    pid = 0
    try:
        _, pid = win32process.GetWindowThreadProcessId(hwnd)
    except Exception:
        pass

    process_name = "Unknown"
    if pid:
        try:
            process_name = psutil.Process(pid).name()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    try:
        left, top, right, bottom = _extended_frame_bounds(hwnd)
    except OSError:
        left, top, right, bottom = win32gui.GetWindowRect(hwnd)

    info = {
        "title": title,
        "process_name": process_name,
        "pid": pid,
        "bounds": {
            "left": left,
            "top": top,
            "width": right - left,
            "height": bottom - top,
        },
    }
    return hwnd, info


def focus_window(hwnd: int | None) -> bool:
    """Hand focus back to the window the user was in before the bar opened.

    Called while our own bar is still the foreground window, which is exactly
    the case where Windows permits SetForegroundWindow -- so no synthetic ALT
    keystroke is needed, and none is injected into the recording.
    """
    user32 = ctypes.windll.user32
    if not hwnd or not user32.IsWindow(hwnd):
        return False
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, _SW_RESTORE)
    return bool(user32.SetForegroundWindow(hwnd))
