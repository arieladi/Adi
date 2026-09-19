"""The Ctrl+F-style command bar: type a description, Enter starts the recording.

Deliberately borderless and short-lived. It is hidden again before capture
starts so it never appears in the recorded frames.
"""
from __future__ import annotations

import tkinter as tk
from tkinter import font as tkfont

BG = "#17181b"
BORDER = "#3a3d44"
FG = "#f2f3f5"
MUTED = "#8a8f98"
ACCENT = "#e5484d"

WIDTH = 640
HEIGHT = 84
TOP_FRACTION = 0.13


class CommandBar:
    """A single reusable bar. show() to open, hide() to close."""

    def __init__(self, root: tk.Tk, on_submit, on_cancel):
        self.root = root
        self.on_submit = on_submit
        self.on_cancel = on_cancel
        self.visible = False

        self.win = tk.Toplevel(root)
        self.win.withdraw()
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg=BORDER)
        self.win.protocol("WM_DELETE_WINDOW", self._cancel)

        body = tk.Frame(self.win, bg=BG)
        body.pack(fill="both", expand=True, padx=1, pady=1)

        top = tk.Frame(body, bg=BG)
        top.pack(fill="x", padx=14, pady=(12, 0))

        tk.Label(top, text="●", bg=BG, fg=ACCENT,
                 font=tkfont.Font(family="Segoe UI", size=11)).pack(side="left")
        tk.Label(top, text="Record", bg=BG, fg=MUTED,
                 font=tkfont.Font(family="Segoe UI", size=10)).pack(side="left", padx=(6, 10))

        self.target_label = tk.Label(top, text="", bg=BG, fg=MUTED, anchor="e",
                                     font=tkfont.Font(family="Segoe UI", size=9))
        self.target_label.pack(side="right")

        entry_font = tkfont.Font(family="Segoe UI", size=13)
        self.entry = tk.Entry(body, bg=BG, fg=FG, insertbackground=FG,
                              relief="flat", font=entry_font, highlightthickness=0, bd=0)
        self.entry.pack(fill="x", padx=14, pady=(4, 2))

        self.hint = tk.Label(body, text="Enter to start  ·  Esc to cancel",
                             bg=BG, fg=MUTED, anchor="w",
                             font=tkfont.Font(family="Segoe UI", size=8))
        self.hint.pack(fill="x", padx=14, pady=(0, 10))

        self.entry.bind("<Return>", self._submit)
        self.entry.bind("<KP_Enter>", self._submit)
        self.entry.bind("<Escape>", self._cancel)

    # ------------------------------------------------------------------ actions

    def show(self, target_title: str = "") -> None:
        self.entry.delete(0, "end")
        self.target_label.config(text=self._shorten(target_title))
        self.hint.config(text="Enter to start  ·  Esc to cancel", fg=MUTED)

        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        x = max(0, (screen_w - WIDTH) // 2)
        y = max(0, int(screen_h * TOP_FRACTION))
        self.win.geometry(f"{WIDTH}x{HEIGHT}+{x}+{y}")

        self.win.deiconify()
        self.win.lift()
        self.visible = True
        # overrideredirect windows do not always get focus on the first ask.
        self.win.after(10, self._grab_focus)

    def hide(self) -> None:
        self.visible = False
        self.win.withdraw()

    def _grab_focus(self) -> None:
        try:
            self.win.focus_force()
            self.entry.focus_force()
        except tk.TclError:
            pass

    def _shorten(self, text: str, limit: int = 46) -> str:
        text = (text or "").strip()
        if len(text) > limit:
            text = text[: limit - 1] + "…"
        return text

    def _submit(self, _event=None) -> str:
        description = self.entry.get().strip()
        if not description:
            self.hint.config(text="Type a description first", fg=ACCENT)
            return "break"
        self.hide()
        self.on_submit(description)
        return "break"

    def _cancel(self, _event=None) -> str:
        self.hide()
        self.on_cancel()
        return "break"
