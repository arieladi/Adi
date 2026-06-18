# -*- coding: utf-8 -*-
"""
AdiVST — Ableton Live MIDI Remote Script (control surface) for the Stream Deck
"Ableton VST Controller" plugin.

Responsibilities:
  • run the WebSocket server (on its own thread, never touches Live);
  • own a LiveBridge that does all LOM work on Live's MAIN thread;
  • marshal inbound commands from the socket thread onto the main thread via a
    thread-safe deque drained in update_display() (~10 Hz).

Install: copy the AdiVST folder into Live's "MIDI Remote Scripts" folder and
select "AdiVST" as a Control Surface in Live > Settings > Link/MIDI.
See docs/ABLETON_SETUP.md.
"""
from __future__ import absolute_import

import collections
import json

try:
    from _Framework.ControlSurface import ControlSurface
except Exception:  # pragma: no cover - only importable inside Live
    ControlSurface = object

from .ws_server import WSServer
from .live_bridge import LiveBridge

PORT = 9006
PRESET_FOLDER = "EQ8 Presets"   # a folder inside Live's User Library


class AdiVST(ControlSurface):
    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._inbox = collections.deque()
        self._bridge = None
        self._ws = None
        with self.component_guard():
            self._bridge = LiveBridge(self, self._send, log=self.log_message,
                                      preset_folder=PRESET_FOLDER)
            self._ws = WSServer(port=PORT, on_message=self._on_ws_message,
                                on_connect=self._on_ws_connect, log=self.log_message)
            self._ws.start()
            self._bridge.setup()
        self.log_message("AdiVST loaded — bridge on ws://127.0.0.1:%d" % PORT)
        self.show_message("AdiVST: Stream Deck bridge on port %d" % PORT)

    # ----------------------------------------------------- outbound (main thread)
    def _send(self, msg):
        if self._ws is None:
            return
        try:
            self._ws.broadcast(json.dumps(msg))
        except Exception as e:
            self.log_message("AdiVST send error: %s" % e)

    # ----------------------------------------------------- inbound (socket thread)
    def _on_ws_message(self, text, client):
        try:
            self._inbox.append(json.loads(text))   # deque.append is thread-safe
        except Exception:
            pass

    def _on_ws_connect(self, client):
        # ask the main thread to push a full snapshot to the new client
        self._inbox.append({"c": "subscribe"})

    # ------------------------------------------------------ main-thread pump
    def update_display(self):
        ControlSurface.update_display(self)
        n = 0
        while self._inbox and n < 128:
            n += 1
            try:
                self._dispatch(self._inbox.popleft())
            except Exception as e:
                self.log_message("AdiVST dispatch error: %s" % e)

    def _dispatch(self, m):
        c = m.get("c")
        b = self._bridge
        if b is None:
            return
        if c == "subscribe":
            b.resend_all()
        elif c == "param_delta":
            b.cmd_param_delta(int(m["slot"]), float(m["delta"]))
        elif c == "param_set":
            b.cmd_param_set(int(m["slot"]), float(m["norm"]))
        elif c == "eq8_freq_delta":
            b.cmd_eq8_freq_delta(int(m["band"]), float(m["delta"]))
        elif c == "eq8_toggle_band":
            b.cmd_eq8_toggle_band(int(m["band"]))
        elif c == "eq8_cycle_type":
            b.cmd_eq8_cycle_type(int(m["band"]), int(m.get("dir", 1)))
        elif c == "eq8_page":
            b.cmd_eq8_page(int(m.get("dir", 1)))
        elif c == "eq8_key":
            b.cmd_eq8_key()
        elif c == "eq8_list_presets":
            b.cmd_list_presets()
        elif c == "eq8_load_preset":
            b.cmd_load_preset(int(m["id"]), replace=True)
        elif c == "eq8_new_preset":
            b.cmd_load_preset(int(m["id"]), replace=False)
        elif c == "select_track":
            b.cmd_select_track(int(m.get("dir", 1)))
        elif c == "select_device":
            b.cmd_select_device(int(m.get("dir", 1)))
        elif c == "get_all_params":
            b.cmd_get_all_params()
        elif c == "watch":
            b.cmd_watch([int(x) for x in m.get("indices", [])])
        elif c == "set_index":
            b.cmd_set_index(int(m["i"]), float(m["norm"]))
        elif c == "delta_index":
            b.cmd_delta_index(int(m["i"]), float(m["delta"]))
        elif c == "step_index":
            b.cmd_step_index(int(m["i"]), int(m.get("dir", 1)), int(m.get("steps", 0)))
        elif c == "toggle_index":
            b.cmd_toggle_index(int(m["i"]))
        elif c == "ping":
            b.resend_all()

    # ------------------------------------------------------------- teardown
    def disconnect(self):
        try:
            if self._bridge:
                self._bridge.teardown()
        except Exception:
            pass
        try:
            if self._ws:
                self._ws.stop()
        except Exception:
            pass
        ControlSurface.disconnect(self)
