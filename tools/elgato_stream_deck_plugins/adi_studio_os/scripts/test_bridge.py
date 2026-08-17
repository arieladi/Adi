#!/usr/bin/env python3
"""Headless test for the AdiVST remote script's V30 device loader.

The script only ever runs inside Ableton, so the parts that touch the Live API
have never been testable — and Adi is away from the studio, so the Pro-Q 3 key
cannot be verified on hardware either. What CAN be tested without Live is the
part that actually decides whether the feature works: the browser walk and the
name match. `Live` is stubbed, the browser is a fake tree shaped like the real
one, and load_item() records what it was handed.

Run: python3 scripts/test_bridge.py
"""
import os, sys, types

SCRIPT = os.path.expanduser(
    "~/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/"
    "adi_ableton_vst_controller/ableton/remote_script/AdiVST")

# --- stub the Live module the script imports at load time -------------------
live = types.ModuleType("Live")
live.Application = types.SimpleNamespace(get_application=lambda: None)
sys.modules["Live"] = live
sys.path.insert(0, os.path.dirname(SCRIPT))

pkg = types.ModuleType("AdiVST"); pkg.__path__ = [SCRIPT]
sys.modules["AdiVST"] = pkg
import importlib.util
spec = importlib.util.spec_from_file_location("AdiVST.live_bridge",
                                              os.path.join(SCRIPT, "live_bridge.py"))
lb = importlib.util.module_from_spec(spec); spec.loader.exec_module(lb)

passed = failed = 0
def ok(name, cond, extra=""):
    global passed, failed
    if cond: passed += 1; print("  ok   " + name)
    else:    failed += 1; print("  FAIL %s %s" % (name, extra))

# --- a browser tree shaped like Live's -------------------------------------
class Item:
    def __init__(self, name, children=(), loadable=False):
        self.name = name; self.children = list(children); self.is_loadable = loadable
        self.is_folder = bool(children)

def make_browser(loaded):
    plugins = Item("Plug-Ins", [
        Item("FabFilter", [
            Item("FabFilter Pro-Q 3", loadable=True),
            Item("FabFilter Pro-Q 3 (m/s)", loadable=True),
            Item("FabFilter Pro-C 2", loadable=True),
        ]),
        Item("Valhalla DSP", [Item("ValhallaRoom", loadable=True)]),
    ])
    audio = Item("Audio Effects", [Item("EQ Eight", loadable=True)])
    b = types.SimpleNamespace(plugins=plugins, audio_effects=audio,
                              instruments=Item("Instruments"),
                              midi_effects=Item("MIDI Effects"),
                              user_library=Item("User Library"),
                              packs=Item("Packs"),
                              load_item=lambda it: loaded.append(it.name))
    return b

class Bridge(lb.LiveBridge):
    """Only the collaborators the loader touches; nothing else is constructed."""
    def __init__(self, browser, track_name="Drums"):
        self.sent = []
        self._b = browser
        self.song = types.SimpleNamespace(
            view=types.SimpleNamespace(selected_track=Item(track_name)))
    def send(self, m): self.sent.append(m)
    def _browser(self): return self._b

print("\n[1] name normalisation")
ok("case and punctuation are ignored",
   lb.LiveBridge._norm("FabFilter Pro-Q 3") == lb.LiveBridge._norm("fabfilter proq3"),
   lb.LiveBridge._norm("FabFilter Pro-Q 3"))
ok("different plug-ins stay different",
   lb.LiveBridge._norm("FabFilter Pro-Q 3") != lb.LiveBridge._norm("FabFilter Pro-C 2"))

print("\n[2] the exact device is loaded onto the selected track")
loaded = []; br = Bridge(make_browser(loaded))
br.cmd_load_device("FabFilter Pro-Q 3")
ok("Pro-Q 3 was loaded", loaded == ["FabFilter Pro-Q 3"], str(loaded))
ok("an exact match beats the (m/s) variant", "(m/s)" not in (loaded[0] if loaded else ""))
ok("a confirmation is emitted",
   any(m.get("t") == "device_loaded" for m in br.sent), str(br.sent))

print("\n[3] it searches plugins AND stock devices")
loaded = []; br = Bridge(make_browser(loaded))
br.cmd_load_device("EQ Eight")
ok("a stock audio effect is found too", loaded == ["EQ Eight"], str(loaded))

print("\n[4] a loose spelling still resolves")
loaded = []; br = Bridge(make_browser(loaded))
br.cmd_load_device("fabfilter proq3")
ok("'fabfilter proq3' finds Pro-Q 3", loaded == ["FabFilter Pro-Q 3"], str(loaded))

print("\n[5] failure paths report instead of throwing")
loaded = []; br = Bridge(make_browser(loaded))
br.cmd_load_device("Nonexistent Plugin")
ok("an unknown device errors and loads nothing",
   loaded == [] and any("not found" in str(m.get("message", "")) for m in br.sent), str(br.sent))
loaded = []; br = Bridge(make_browser(loaded))
br.cmd_load_device("")
ok("an empty name is refused", loaded == [] and len(br.sent) == 1, str(br.sent))
loaded = []; br = Bridge(make_browser(loaded)); br.song.view.selected_track = None
br.cmd_load_device("FabFilter Pro-Q 3")
ok("no selected track is refused, not crashed",
   loaded == [] and any("selected track" in str(m.get("message", "")) for m in br.sent), str(br.sent))

print("\n[6] a non-loadable folder of the same name is never loaded")
loaded = []
b = make_browser(loaded)
b.plugins.children.insert(0, Item("FabFilter Pro-Q 3", loadable=False))   # a folder
br = Bridge(b)
br.cmd_load_device("FabFilter Pro-Q 3")
ok("only is_loadable items are handed to load_item", loaded == ["FabFilter Pro-Q 3"], str(loaded))

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
