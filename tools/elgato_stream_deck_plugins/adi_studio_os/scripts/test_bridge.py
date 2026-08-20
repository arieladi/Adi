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
    # Compressor and Glue Compressor are both here on purpose: the V48 tests below
    # need to prove that asking for one never resolves to the other.
    audio = Item("Audio Effects", [
        Item("EQ Eight", loadable=True),
        Item("Compressor", loadable=True),
        Item("Glue Compressor", loadable=True),
    ])
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

# ===========================================================================
# V48 — the unified device key, and the track volume/pan for the idle dials.
# ===========================================================================
print("\n[7] V48: the unified device key (short press)")

class Dev:
    """A device on a track. A VST3 gives us nothing but its NAME — every one of
    them reports class_name 'PluginDevice' — which is why the match is by name."""
    def __init__(self, name, cls="PluginDevice"):
        self.name = name; self.class_name = cls

class KeyBridge(lb.LiveBridge):
    def __init__(self, devices, selected=None, browser_loaded=None):
        self.sent = []; self.msgs = []
        self._loaded = browser_loaded if browser_loaded is not None else []
        self._b = make_browser(self._loaded)
        self.track = types.SimpleNamespace(
            name="Drums", devices=list(devices),
            view=types.SimpleNamespace(selected_device=selected))
        self.song = types.SimpleNamespace(
            view=types.SimpleNamespace(selected_track=self.track,
                                       select_device=self._select))
        self._cs = types.SimpleNamespace(show_message=lambda m: self.msgs.append(m))
    def send(self, m): self.sent.append(m)
    def _browser(self): return self._b
    def _select(self, d): self.track.view.selected_device = d
    def _device_index(self, d):
        try: return list(self.track.devices).index(d)
        except ValueError: return -1

# --- nothing on the track -> insert
loaded = []
br = KeyBridge([], None, loaded)
br.cmd_device_key("FabFilter Pro-Q 3")
ok("no instance on the track -> one is inserted", loaded == ["FabFilter Pro-Q 3"], str(loaded))

# --- one on the track -> focus it, do NOT insert a second
loaded = []
q = Dev("FabFilter Pro-Q 3")
br = KeyBridge([Dev("EQ Eight", "Eq8"), q], None, loaded)
br.cmd_device_key("FabFilter Pro-Q 3")
ok("one instance -> it is focused and nothing is inserted",
   loaded == [] and br.track.view.selected_device is q, str(loaded))

# --- several -> cycle on each press, and wrap
loaded = []
a, b2, c3 = Dev("FabFilter Pro-Q 3"), Dev("FabFilter Pro-Q 3"), Dev("FabFilter Pro-Q 3")
br = KeyBridge([a, b2, c3], a, loaded)
br.cmd_device_key("FabFilter Pro-Q 3")
first = br.track.view.selected_device
br.cmd_device_key("FabFilter Pro-Q 3")
second = br.track.view.selected_device
br.cmd_device_key("FabFilter Pro-Q 3")
third = br.track.view.selected_device
ok("three instances cycle 1->2->3->1",
   (first, second, third) == (b2, c3, a), str([first is b2, second is c3, third is a]))
ok("...and cycling never inserts anything", loaded == [], str(loaded))

# --- selection elsewhere -> focus the first, do not insert
loaded = []
other = Dev("Serum")
q = Dev("FabFilter Pro-Q 3")
br = KeyBridge([other, q], other, loaded)
br.cmd_device_key("FabFilter Pro-Q 3")
ok("selection elsewhere -> the first matching instance is focused",
   br.track.view.selected_device is q and loaded == [], str(loaded))

print("\n[8] V48: long press always inserts")
loaded = []
q = Dev("FabFilter Pro-Q 3")
br = KeyBridge([q], q, loaded)
br.cmd_device_key("FabFilter Pro-Q 3", force_new=True)
ok("a forced insert appends even when one is already focused",
   loaded == ["FabFilter Pro-Q 3"], str(loaded))

print("\n[9] V48: 'Compressor' must not cycle a Glue Compressor")
# THE ORDERING BUG THIS PREVENTS: a contains-pass alone matches "Glue Compressor"
# for "Compressor", so the key would focus the Glue instead of inserting the
# Compressor that was asked for. Exact pass first is what makes it right.
loaded = []
glue = Dev("Glue Compressor")
br = KeyBridge([glue], None, loaded)
br.cmd_device_key("Compressor")
ok("with only a Glue Compressor present, Compressor is INSERTED",
   loaded == ["Compressor"] and br.track.view.selected_device is None, str(loaded))
loaded = []
comp = Dev("Compressor")
br = KeyBridge([glue, comp], None, loaded)
br.cmd_device_key("Compressor")
ok("...and with both present the exact one is focused",
   br.track.view.selected_device is comp and loaded == [], str(loaded))
loaded = []
br = KeyBridge([glue, comp], None, loaded)
br.cmd_device_key("Glue Compressor")
ok("...while Glue Compressor still finds itself",
   br.track.view.selected_device is glue and loaded == [], str(loaded))

print("\n[10] V48: track volume steps EXACTLY 0.5 dB on Live's own grid")

class VolParam:
    """Stands in for Live's volume DeviceParameter.

    The curve is deliberately NOT the one the bridge assumes, because the bridge
    must not assume one: it reads dB from str_for_value and inverts THAT by
    bisection. Any monotonic curve therefore has to work, and this one is chosen to
    be awkward — 0.85 is 0 dB and the top is +6, like Live, but the shape between
    is arbitrary.
    """
    def __init__(self, v=0.85):
        self.value = v; self.min = 0.0; self.max = 1.0
    def str_for_value(self, v):
        if v <= 0.0: return "-inf dB"
        db = 6.0 - 70.0 * ((0.85 - v) ** 2 if v < 0.85 else 0.0) - (0.0 if v >= 0.85 else 0.0)
        # a monotonic, non-linear map from 0..1 to about -70..+6 dB
        db = -70.0 + 76.0 * (v ** 0.35)
        return "%.2f dB" % db

class PanParam:
    def __init__(self, v=0.0):
        self.value = v; self.min = -1.0; self.max = 1.0
    def str_for_value(self, v):
        if abs(v) < 1e-9: return "C"
        return ("%dR" if v > 0 else "%dL") % round(abs(v) * 50)

class MixBridge(lb.LiveBridge):
    def __init__(self, vol=0.85, pan=0.0):
        self.sent = []
        self._mixed = []
        self.vol = VolParam(vol); self.pan = PanParam(pan)
        self.track = types.SimpleNamespace(
            name="Drums",
            mixer_device=types.SimpleNamespace(volume=self.vol, panning=self.pan))
        self.song = types.SimpleNamespace(
            view=types.SimpleNamespace(selected_track=self.track))
    def send(self, m): self.sent.append(m)
    def log(self, m): pass

br = MixBridge()
def db(brg): return brg._db_at(brg.vol, brg.vol.value)
start = db(br)
br.cmd_track_volume_delta(1)
one = db(br)
ok("one detent up is +0.5 dB from the snapped start",
   abs(one - (round(start / 0.5) * 0.5 + 0.5)) < 0.01, "%.3f -> %.3f" % (start, one))
br.cmd_track_volume_delta(-1)
back = db(br)
ok("...and one down returns to it", abs(back - (one - 0.5)) < 0.02, "%.3f" % back)

# EVERY value on the way must sit on the half-dB grid — that is the requirement.
br = MixBridge()
offs = []
for _ in range(12):
    br.cmd_track_volume_delta(1)
    offs.append(abs(db(br) / 0.5 - round(db(br) / 0.5)))
ok("twelve steps all land on the 0.5 dB grid", max(offs) < 0.02, "worst %.4f" % max(offs))

# A fader parked off-grid by a mouse drag must be snapped, not carried.
br = MixBridge(); br.vol.value = br._norm_for_db(br.vol, -6.02)
ok("the stand-in reproduces an off-grid fader", abs(db(br) - (-6.02)) < 0.02, "%.3f" % db(br))
br.cmd_track_volume_delta(1)
ok("stepping from -6.02 dB lands on -5.5, not -5.52",
   abs(db(br) - (-5.5)) < 0.02, "%.3f" % db(br))

print("\n[11] V48: the volume rails hold")
br = MixBridge()
for _ in range(60): br.cmd_track_volume_delta(5)
ok("it cannot be pushed past the top of the fader",
   br.vol.value <= 1.0 and abs(db(br) - br._db_at(br.vol, 1.0)) < 0.01, "%.3f" % db(br))
br = MixBridge()
for _ in range(80): br.cmd_track_volume_delta(-5)
ok("all the way down is -inf, exactly like Live's own fader",
   br.vol.value == 0.0 and br._db_at(br.vol, br.vol.value) == float("-inf"), str(br.vol.value))
br = MixBridge(); br.vol.value = 0.0
br.cmd_track_volume_delta(1)
ok("...and it comes back up off -inf", br.vol.value > 0.0, str(br.vol.value))
br = MixBridge()
br.cmd_track_volume_delta(0)
ok("a zero delta changes nothing", abs(br.vol.value - 0.85) < 1e-12)

print("\n[12] V48: pan is linear and clamps")
br = MixBridge()
br.cmd_track_pan_delta(1)
ok("one detent is one unit of Live's 50L..50R readout",
   abs(br.pan.value - 0.02) < 1e-9, str(br.pan.value))
br = MixBridge()
for _ in range(200): br.cmd_track_pan_delta(1)
ok("hard right clamps at +1", abs(br.pan.value - 1.0) < 1e-9, str(br.pan.value))
for _ in range(400): br.cmd_track_pan_delta(-1)
ok("hard left clamps at -1", abs(br.pan.value + 1.0) < 1e-9, str(br.pan.value))

print("\n[13] V48: the mix state the idle dials read")
br = MixBridge()
br.cmd_get_mix()
m = [x for x in br.sent if x.get("t") == "mix"][-1]
ok("a mix message carries both values and both display strings",
   m.get("has_track") and "vol" in m and "vol_disp" in m and "pan" in m and "pan_disp" in m,
   str(m))
ok("...and the displays are Live's own strings, not ours",
   m["vol_disp"].endswith("dB") and m["pan_disp"] == "C", str(m))
br = MixBridge(); br.song.view.selected_track = None
br.cmd_get_mix()
ok("no track reports has_track false rather than throwing",
   [x for x in br.sent if x.get("t") == "mix"][-1].get("has_track") is False)
br = MixBridge(); br.track.mixer_device = None
br.cmd_track_volume_delta(1); br.cmd_track_pan_delta(1)
ok("a track with no mixer is a no-op, not a crash", True)

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
