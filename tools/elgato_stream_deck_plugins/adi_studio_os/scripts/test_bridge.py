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
        _song = types.SimpleNamespace(
            view=types.SimpleNamespace(selected_track=Item(track_name)))
        self._cs = types.SimpleNamespace(song=lambda: _song)
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
        _song = types.SimpleNamespace(
            view=types.SimpleNamespace(selected_track=self.track,
                                       select_device=self._select))
        self._cs = types.SimpleNamespace(song=lambda: _song,
                                         show_message=lambda m: self.msgs.append(m))
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
        _song = types.SimpleNamespace(
            view=types.SimpleNamespace(selected_track=self.track))
        self._cs = types.SimpleNamespace(song=lambda: _song)
        # V64 — _emit_mix coalesces against the previous payload now, so a stub
        # that never calls the real __init__ has to declare the field itself.
        self._last_mix = None
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

# ===========================================================================
# V53 — the deep search, and stepping through the whole device tree.
# ===========================================================================
print("\n[14] V53: the rack walk sees every device, in Live's own order")

class Chain:
    def __init__(self, devices): self.devices = list(devices)

class Rack(Dev):
    """A rack. Live exposes `chains` (and `return_chains` for a rack's returns);
    the devices inside can be racks again, which is normal in a drum rack."""
    def __init__(self, name, chains=(), returns=()):
        Dev.__init__(self, name, "RackDevice")
        self.chains = [Chain(c) for c in chains]
        self.return_chains = [Chain(c) for c in returns]

class TreeBridge(lb.LiveBridge):
    def __init__(self, devices, selected=None, loaded=None):
        self.sent = []; self.msgs = []
        self._loaded = loaded if loaded is not None else []
        self._b = make_browser(self._loaded)
        self.track = types.SimpleNamespace(
            name="Drums", devices=list(devices),
            view=types.SimpleNamespace(selected_device=selected))
        _song = types.SimpleNamespace(
            view=types.SimpleNamespace(selected_track=self.track,
                                       select_device=self._sel))
        self._cs = types.SimpleNamespace(song=lambda: _song,
                                         show_message=lambda m: self.msgs.append(m))
    def send(self, m): self.sent.append(m)
    def log(self, m): pass
    def _browser(self): return self._b
    def _sel(self, d): self.track.view.selected_device = d
    def _device_index(self, d):
        try: return list(self.track.devices).index(d)
        except ValueError: return -1

# A realistic tree: a plain device, then a rack with two chains, one of which
# holds a nested rack, plus a return chain.
buried  = Dev("FabFilter Pro-Q 3")
inner   = Rack("Inner Rack", chains=[[buried]])
chainA  = [Dev("Saturator"), inner]
chainB  = [Dev("Compressor")]
ret     = [Dev("ValhallaRoom")]
outer   = Rack("Drum Rack", chains=[chainA, chainB], returns=[ret])
top     = [Dev("EQ Eight"), outer, Dev("Glue Compressor")]

br = TreeBridge(top)
flat = [getattr(d, "name", "?") for d in br._track_devices(br.track)]
EXPECT = ["EQ Eight", "Drum Rack", "Saturator", "Inner Rack",
          "FabFilter Pro-Q 3", "Compressor", "ValhallaRoom", "Glue Compressor"]
ok("the walk finds all eight devices, racks included", len(flat) == 8, str(len(flat)))
ok("...depth-first in chain order, so 'next' descends instead of skipping",
   flat == EXPECT, " -> ".join(flat))
ok("...and a rack's RETURN chain is walked too, not just its chains",
   "ValhallaRoom" in flat, " -> ".join(flat))
ok("...and the racks themselves are in the list, being devices as well",
   "Drum Rack" in flat and "Inner Rack" in flat)

print("\n[15] V53: Smart Focus finds a plugin buried in a rack")
loaded = []
br = TreeBridge(top, None, loaded)
br.cmd_device_key("FabFilter Pro-Q 3")
ok("a Pro-Q 3 two racks deep is FOCUSED, not duplicated",
   br.track.view.selected_device is buried and loaded == [], str(loaded))
# The regression this guards: before V53 the search saw only the top chain, so it
# found nothing and inserted a second copy on every press.
loaded = []
br = TreeBridge([Dev("EQ Eight")], None, loaded)
br.cmd_device_key("FabFilter Pro-Q 3")
ok("...while a track that genuinely lacks one still gets one inserted",
   loaded == ["FabFilter Pro-Q 3"], str(loaded))
loaded = []
buried2 = Dev("FabFilter Pro-Q 3")
br = TreeBridge([Rack("R", chains=[[buried]]), Rack("R2", chains=[[buried2]])], buried, loaded)
br.cmd_device_key("FabFilter Pro-Q 3")
ok("two instances in DIFFERENT racks still cycle between each other",
   br.track.view.selected_device is buried2 and loaded == [], str(loaded))

print("\n[16] V53: the long press still forces a new instance, even when one is buried")
loaded = []
br = TreeBridge(top, buried, loaded)
br.cmd_device_key("FabFilter Pro-Q 3", force_new=True)
ok("a forced insert ignores the buried instance", loaded == ["FabFilter Pro-Q 3"], str(loaded))

print("\n[17] V53: the device-step arrows traverse the tree")
br = TreeBridge(top, None)
br.cmd_device_step(1)
ok("with nothing selected, next starts at the first device",
   br.track.view.selected_device is top[0], getattr(br.track.view.selected_device, "name", "?"))
seen = []
for _ in range(10):
    br.cmd_device_step(1)
    seen.append(getattr(br.track.view.selected_device, "name", "?"))
ok("stepping forward walks INTO the rack, not over it",
   seen[0] == "Drum Rack" and seen[1] == "Saturator" and seen[2] == "Inner Rack"
   and seen[3] == "FabFilter Pro-Q 3", " -> ".join(seen[:5]))
ok("...and back OUT of it to the next top-level device",
   "Glue Compressor" in seen, " -> ".join(seen))
ok("...and it CLAMPS at the end instead of wrapping round",
   seen[-1] == "Glue Compressor" and seen[-2] == "Glue Compressor", " -> ".join(seen[-3:]))

br = TreeBridge(top, None)
br.cmd_device_step(-1)
ok("with nothing selected, prev starts at the LAST device",
   getattr(br.track.view.selected_device, "name", "?") == "Glue Compressor",
   getattr(br.track.view.selected_device, "name", "?"))
back = []
for _ in range(10):
    br.cmd_device_step(-1)
    back.append(getattr(br.track.view.selected_device, "name", "?"))
ok("stepping back descends into the rack from the other side",
   back[0] == "ValhallaRoom" and back[1] == "Compressor", " -> ".join(back[:3]))
ok("...and clamps at the first device", back[-1] == "EQ Eight", " -> ".join(back[-2:]))

print("\n[18] V53: the position message the arrows read")
br = TreeBridge(top, buried)
br._emit_device_pos()
m = [x for x in br.sent if x.get("t") == "device_pos"][-1]
ok("it reports where the selection sits in the FLATTENED tree",
   m["count"] == 8 and m["index"] == 4, str(m))
br = TreeBridge([], None)
br.cmd_device_step(1)
ok("an empty track reports a count of zero rather than throwing",
   [x for x in br.sent if x.get("t") == "device_pos"][-1]["count"] == 0)
br = TreeBridge(top, Dev("Not On This Track"))
br._emit_device_pos()
ok("a selection that is not in the tree reports index -1, not a crash",
   [x for x in br.sent if x.get("t") == "device_pos"][-1]["index"] == -1)

print("\n[19] V53: the depth cap is a rail, not a limit")
deep = Dev("Bottom")
node = deep
for _ in range(30):
    node = Rack("R", chains=[[node]])
br = TreeBridge([node])
n = len(br._track_devices(br.track))
ok("a pathologically deep tree is truncated instead of blowing the stack",
   n > 0 and n <= 40, str(n))


# ===========================================================================
# V64 — THE REAL CONSTRUCTOR, AND THE LIFECYCLE NOBODY WAS TESTING.
#
# Every bridge above overrides __init__ and never calls it, so setup(),
# teardown(), _listen/_unlisten and the whole listener lifecycle had ZERO
# coverage — which is exactly the surface every V64 bug lived on. This block
# instantiates the real LiveBridge against a Live-shaped fake and drives it.
# ===========================================================================
print("\n[20] V64: the real lifecycle")

class FakeParam(object):
    def __init__(self, v=0.5):
        self.value = v; self.min = 0.0; self.max = 1.0; self.is_enabled = True
        self._ls = []
    def add_value_listener(self, fn): self._ls.append(fn)
    def remove_value_listener(self, fn): self._ls.remove(fn)
    def str_for_value(self, v): return "%.2f" % v

class FakeTrack(object):
    """Models Live's listener registry, and its habit of RAISING for an attribute
       a given track type does not have (a return track has no `arm`)."""
    def __init__(self, name="Drums", arm_raises=None):
        self.name = name; self.devices = []
        self.mute = False; self.solo = False
        self._arm_raises = arm_raises
        self._arm = False
        self.mixer_device = types.SimpleNamespace(volume=FakeParam(0.85), panning=FakeParam(0.0))
        self.view = types.SimpleNamespace(selected_device=None)
        self.listeners = {}
    @property
    def arm(self):
        if self._arm_raises is not None: raise self._arm_raises("no arm on this track")
        return self._arm
    def __getattr__(self, n):
        # add_x_listener / remove_x_listener for any property, like Live does.
        if n.startswith("add_") and n.endswith("_listener"):
            key = n[4:-9]
            return lambda fn: self.listeners.setdefault(key, []).append(fn)
        if n.startswith("remove_") and n.endswith("_listener"):
            key = n[7:-9]
            return lambda fn: self.listeners.get(key, []).remove(fn)
        raise AttributeError(n)

class FakeSong(object):
    def __init__(self, track):
        self.is_playing = False; self.loop = False
        self.view = types.SimpleNamespace(selected_track=track)
        self.listeners = {}
        self.tracks = [track]
    def __getattr__(self, n):
        if n.startswith("add_") and n.endswith("_listener"):
            key = n[4:-9]
            return lambda fn: self.listeners.setdefault(key, []).append(fn)
        if n.startswith("remove_") and n.endswith("_listener"):
            key = n[7:-9]
            return lambda fn: self.listeners.get(key, []).remove(fn)
        raise AttributeError(n)

def build(track=None, arm_raises=None):
    tr = track or FakeTrack(arm_raises=arm_raises)
    song = FakeSong(tr)
    cs = types.SimpleNamespace(song=lambda: song, show_message=lambda m: None)
    sent = []
    br = lb.LiveBridge(cs, sent.append, log=lambda *a: None)
    return br, song, tr, sent

# --- the transport, which V61 shipped with no listener and no emit-on-connect
br, song, tr, sent = build()
br.setup()
ok("setup() registers a listener on is_playing", "is_playing" in song.listeners)
ok("...and on loop", "loop" in song.listeners)
sent[:] = []
br.resend_all()
ok("resend_all emits transport, so a reconnect is not blind",
   any(m.get("t") == "transport" for m in sent),
   ",".join(sorted({m.get("t") for m in sent})))
sent[:] = []
song.is_playing = True
for fn in song.listeners["is_playing"]: fn()
ok("pressing play IN LIVE reaches the surface",
   [m for m in sent if m.get("t") == "transport"][-1]["playing"] is True)

# --- mix coalescing: a dial tick used to emit `mix` twice
br, song, tr, sent = build()
br.setup(); sent[:] = []
br.cmd_track_volume_delta(1)
mixes = [m for m in sent if m.get("t") == "mix"]
ok("a volume tick emits `mix` ONCE, not twice", len(mixes) == 1, str(len(mixes)))
sent[:] = []
br.cmd_get_mix()
ok("...but get_mix still forces a restatement",
   len([m for m in sent if m.get("t") == "mix"]) == 1)

# --- the hasattr crash: a return track whose `arm` raises must not abort
for exc in (AttributeError, RuntimeError):
    br, song, tr, sent = build(arm_raises=exc)
    br.setup()
    kinds = {m.get("t") for m in sent}
    ok("a track whose arm raises %s still emits the full state" % exc.__name__,
       {"track", "mix", "device_pos"} <= kinds, ",".join(sorted(kinds)))
    ok("...and arm is reported as unsupported, not False",
       [m for m in sent if m.get("t") == "mix"][-1].get("arm") is None)

# --- a NEW LIVE SET must not wedge the bridge
br, song, tr, sent = build()
br.setup()
tr2 = FakeTrack("Bass")
song2 = FakeSong(tr2)
br._cs.song = lambda: song2          # Live swaps the Song under us
sent[:] = []
br.resend_all()
ok("a new Live Set does not wedge the bridge",
   any(m.get("t") == "track" for m in sent))
ok("...and the new Set's track is what arrives",
   [m for m in sent if m.get("t") == "track"][-1]["name"] == "Bass")

# --- device_pos must follow the SELECTION, not just the list
br, song, tr, sent = build()
br.setup(); sent[:] = []
br._on_device_changed()
ok("selecting a device re-emits device_pos (the stale-caption bug)",
   any(m.get("t") == "device_pos" for m in sent),
   ",".join(sorted({m.get("t") for m in sent})))

# --- teardown must leave nothing behind
br, song, tr, sent = build()
br.setup()
before = len(br._listened)
br.teardown()
ok("teardown removes every registered listener",
   before > 0 and len(br._listened) == 0, "%d -> %d" % (before, len(br._listened)))
ok("...including the song's own",
   all(len(v) == 0 for v in song.listeners.values()),
   str({k: len(v) for k, v in song.listeners.items()}))

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
