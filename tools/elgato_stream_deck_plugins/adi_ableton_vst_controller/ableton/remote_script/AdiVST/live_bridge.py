# -*- coding: utf-8 -*-
"""
LiveBridge — all Live Object Model access for AdiVST.

Everything here runs on Live's MAIN thread (driven from AdiVST.update_display /
scheduled tasks). It never blocks on sockets; it only reads/writes the LOM and
calls `self.send(dict)` to push JSON state out through the WebSocket server.

Targets Live 11/12 (Python 3). Works on native devices and on VST2/VST3/AU
plugins alike, because Live exposes plugin parameters through the same
`device.parameters` API. (Caveat: some VST3s only expose a subset / generically
named parameters until you "Configure" them in Live's device view.)
"""

import math
import re

import Live  # provided by Ableton at runtime

EQ8_CLASS = "Eq8"
GENERIC_SLOTS = 6
EQ8_DIALS = 6
EQ8_BANDS = 8

_BAND_RE = re.compile(r"^(\d+)\s+(Frequency|Gain|Resonance|Filter On|Filter Type)\s+([AB])$")


def _fmt_hz(hz):
    if hz >= 1000:
        return "%.2fk" % (hz / 1000.0)
    return "%d Hz" % int(round(hz))


def _fmt_generic(p):
    """Display string for a parameter.

    Prefer Ableton's OWN formatting via DeviceParameter.str_for_value(): it returns
    the exact text Live shows for the current value (e.g. "47.924 Hz", "0.00 dB",
    "8kHz", "Bell"). This makes the touchscreen mirror Ableton precisely, and is
    correct whether Live reports the raw value in engineering units OR normalized
    0..1 (the conversion happens inside Live). Fall back to a numeric format only
    if str_for_value is unavailable.
    """
    try:
        s = p.str_for_value(p.value)
        if s is not None and str(s) != "":
            return str(s)
    except Exception:
        pass
    try:
        if p.is_quantized and p.value_items:
            return str(p.value_items[int(round(p.value))])
    except Exception:
        pass
    v = p.value
    if abs(v) >= 100:
        return "%d" % int(round(v))
    if abs(v) >= 10:
        return "%.1f" % v
    return "%.2f" % v


class LiveBridge(object):
    def __init__(self, c_surface, send, log=None, preset_folder="EQ8 Presets"):
        self._cs = c_surface
        self.song = c_surface.song()
        self.send = send
        self.log = log or (lambda *a: None)
        self.preset_folder = preset_folder

        self._track = None
        self._device = None
        self._param_map = []          # [(slot, parameter)] for generic mode
        self._eq8_params = {}         # {(band, field): parameter}
        self._eq8_focus = 1           # first band the 6 dials control (1..3)
        self._listened = []           # [(subject, name, fn)] for clean teardown
        self._preset_items = {}       # {id: BrowserItem}
        self._watch = []              # [(parameter, fn)] watched by predefined controllers
        self._mixed = []              # V48 — watched mixer params (volume, panning)

    # ============================================================ lifecycle
    def setup(self):
        self._listen(self.song.view, "selected_track", self._on_track_changed)
        self._on_track_changed()

    def teardown(self):
        self._remove_device_listeners()
        self._unmix_listen()
        for subject, name, fn in self._listened:
            try:
                getattr(subject, "remove_%s_listener" % name)(fn)
            except Exception:
                pass
        self._listened = []

    # --------------------------------------------------------- listener utils
    def _listen(self, subject, name, fn):
        try:
            getattr(subject, "add_%s_listener" % name)(fn)
            self._listened.append((subject, name, fn))
        except Exception as e:
            self.log("listen %s failed: %s" % (name, e))

    def _unlisten(self, subject, name, fn):
        try:
            getattr(subject, "remove_%s_listener" % name)(fn)
        except Exception:
            pass
        self._listened = [t for t in self._listened if t != (subject, name, fn)]

    # ================================================================ tracking
    def _on_track_changed(self):
        # rewire the per-track device listeners
        if self._track is not None:
            self._unlisten(self._track, "devices", self._on_devices_changed)
            try:
                self._unlisten(self._track.view, "selected_device", self._on_device_changed)
            except Exception:
                pass
        self._track = self.song.view.selected_track
        if self._track is not None:
            self._listen(self._track, "devices", self._on_devices_changed)
            self._listen(self._track.view, "selected_device", self._on_device_changed)
            self.send({
                "t": "track",
                "name": self._track.name,
                "index": self._track_index(self._track),
                "color": getattr(self._track, "color", 0),
            })
        # V48 — the idle-state dials follow the selected track.
        self._mix_listen(self._track)
        self._emit_mix()
        self._on_device_changed()

    def _on_devices_changed(self):
        self._emit_eq8_state()
        # selection may now point elsewhere; re-evaluate
        self._on_device_changed()

    def _on_device_changed(self):
        self._remove_device_listeners()
        self._device = self._track.view.selected_device if self._track else None

        if self._device is None:
            self.send({"t": "device", "has_device": False, "controller": "generic",
                       "name": "", "class_name": "", "index": -1, "param_count": 0})
            self._emit_eq8_state()
            return

        is_eq8 = self._device.class_name == EQ8_CLASS
        self.send({
            "t": "device",
            "has_device": True,
            "name": self._device.name,
            "class_name": self._device.class_name,
            "index": self._device_index(self._device),
            "controller": "eq8" if is_eq8 else "generic",
            "param_count": len(self._device.parameters),
        })

        if is_eq8:
            self._build_eq8_model()
            self._emit_eq8_full()
        else:
            self._build_generic_map()
            self._emit_generic_full()
        self._emit_eq8_state()

    def _remove_device_listeners(self):
        for slot, p in self._param_map:
            self._unlisten(p, "value", self._param_listener(slot))
        self._param_map = []
        for key, p in self._eq8_params.items():
            self._unlisten(p, "value", self._eq8_listener(key))
        self._eq8_params = {}
        self._clear_watch()

    # ============================================================ GENERIC mode
    def _build_generic_map(self):
        """First 6 NON-quantized parameters, skipping 'Device On' (index 0)."""
        self._param_map = []
        slot = 0
        for p in self._device.parameters[1:]:
            if slot >= GENERIC_SLOTS:
                break
            try:
                if p.is_quantized:
                    continue
            except Exception:
                pass
            self._param_map.append((slot, p))
            self._listen(p, "value", self._param_listener(slot))
            slot += 1

    def _param_listener(self, slot):
        # one stable bound function per slot (so add/remove match)
        key = ("param", slot)
        fn = self._cache_fn(key, lambda: (lambda: self._emit_param(slot)))
        return fn

    def _emit_generic_full(self):
        params = []
        for slot, p in self._param_map:
            params.append(self._param_dict(slot, p))
        self.send({"t": "params", "page": 0, "pages": 1, "params": params})

    def _param_dict(self, slot, p):
        return {
            "slot": slot, "pidx": list(self._device.parameters).index(p),
            "name": p.name, "value": p.value, "min": p.min, "max": p.max,
            "disp": _fmt_generic(p), "quantized": bool(p.is_quantized),
        }

    def _emit_param(self, slot):
        for s, p in self._param_map:
            if s == slot:
                self.send({"t": "param", "slot": slot, "value": p.value, "disp": _fmt_generic(p)})
                return

    def cmd_param_delta(self, slot, delta):
        for s, p in self._param_map:
            if s == slot:
                rng = (p.max - p.min) or 1.0
                self._safe_set(p, p.value + delta * rng)
                return

    def cmd_param_set(self, slot, norm):
        for s, p in self._param_map:
            if s == slot:
                self._safe_set(p, p.min + max(0.0, min(1.0, norm)) * (p.max - p.min))
                return

    # ============================================================== EQ8 mode
    def _build_eq8_model(self):
        self._eq8_params = {}
        self._eq8_focus = 1            # fresh device → start the 6-dial window at band 1
        for p in self._device.parameters:
            m = _BAND_RE.match(p.name)
            if not m:
                continue
            if m.group(3) != "A":          # use the A edit-channel
                continue
            band = int(m.group(1))
            field = {
                "Frequency": "freq", "Gain": "gain", "Resonance": "q",
                "Filter On": "on", "Filter Type": "type",
            }[m.group(2)]
            self._eq8_params[(band, field)] = p
            self._listen(p, "value", self._eq8_listener((band, field)))
        # Global params (adjustable + displayed). Stored under band 0 so the normal
        # teardown loop removes their listeners too; the listener emits eq8_globals.
        for p in self._device.parameters:
            if p.name == "Output Gain" and (0, "output") not in self._eq8_params:
                self._eq8_params[(0, "output")] = p
                self._listen(p, "value", self._eq8_listener((0, "output")))
            elif p.name == "Scale" and (0, "scale") not in self._eq8_params:
                self._eq8_params[(0, "scale")] = p
                self._listen(p, "value", self._eq8_listener((0, "scale")))

    def _eq8_listener(self, key):
        if key[0] == 0:    # global param (Output Gain / Scale)
            return self._cache_fn(("eq8", key), lambda: (lambda: self._emit_eq8_globals()))
        return self._cache_fn(("eq8", key), lambda: (lambda: self._emit_eq8_band(key[0])))

    def _eq8_get(self, band, field):
        return self._eq8_params.get((band, field))

    def _band_dict(self, band):
        on = self._eq8_get(band, "on")
        freq = self._eq8_get(band, "freq")
        gain = self._eq8_get(band, "gain")
        q = self._eq8_get(band, "q")
        typ = self._eq8_get(band, "type")
        type_items = []
        type_val = 0
        type_name = ""
        if typ is not None:
            try:
                type_items = list(typ.value_items)
            except Exception:
                type_items = []
            type_val = int(round(typ.value))
            if 0 <= type_val < len(type_items):
                type_name = type_items[type_val]
        return {
            "i": band,
            "on": bool(round(on.value)) if on else True,
            "freq": freq.value if freq else 0.0,
            "freq_disp": _fmt_generic(freq) if freq else "",
            "gain": gain.value if gain else 0.0,
            "gain_disp": _fmt_generic(gain) if gain else "",
            "q": q.value if q else 0.0,
            "q_disp": _fmt_generic(q) if q else "",
            "type": type_val,
            "type_name": type_name,
            "type_items": type_items,
        }

    def _eq8_globals_dict(self):
        out = self._eq8_get(0, "output")
        sc = self._eq8_get(0, "scale")
        return {
            "output": out.value if out else 0.0,
            "output_disp": _fmt_generic(out) if out else "",
            "scale": sc.value if sc else 100.0,
            "scale_disp": _fmt_generic(sc) if sc else "",
        }

    def _emit_eq8_full(self):
        bands = [self._band_dict(b) for b in range(1, EQ8_BANDS + 1)]
        msg = {"t": "eq8", "page": self._eq8_focus, "focus": self._eq8_focus, "bands": bands}
        msg.update(self._eq8_globals_dict())
        self.send(msg)

    def _emit_eq8_band(self, band):
        self.send(dict({"t": "eq8_band"}, **self._band_dict(band)))

    def _emit_eq8_globals(self):
        self.send(dict({"t": "eq8_globals"}, **self._eq8_globals_dict()))

    def cmd_eq8_freq_delta(self, band, delta):
        p = self._eq8_get(band, "freq")
        if p is None:
            return
        # geometric (musical) frequency nudge; freq value is always > 0
        new = p.value * (2.0 ** (delta * 4.0))
        self._safe_set(p, new)

    def cmd_eq8_gain_delta(self, band, delta):
        p = self._eq8_get(band, "gain")
        if p is not None:
            self._safe_set(p, p.value + delta * ((p.max - p.min) or 1.0))   # linear (dB)

    def cmd_eq8_q_delta(self, band, delta):
        p = self._eq8_get(band, "q")
        if p is None:
            return
        v = p.value
        if v > 0:
            self._safe_set(p, v * (2.0 ** (delta * 4.0)))                   # geometric (Q)
        else:
            self._safe_set(p, v + delta * ((p.max - p.min) or 1.0))

    def cmd_eq8_global_delta(self, which, delta):
        # which: "scale" or "output" (Output Gain). Both are linear nudges.
        p = self._eq8_get(0, "scale" if which == "scale" else "output")
        if p is not None:
            self._safe_set(p, p.value + delta * ((p.max - p.min) or 1.0))

    def cmd_eq8_toggle_band(self, band):
        p = self._eq8_get(band, "on")
        if p is None:
            return
        self._safe_set(p, 0.0 if round(p.value) else 1.0)

    def cmd_eq8_cycle_type(self, band, direction):
        p = self._eq8_get(band, "type")
        if p is None:
            return
        try:
            n = len(p.value_items)
        except Exception:
            n = int(p.max - p.min) + 1
        v = (int(round(p.value)) + (1 if direction >= 0 else -1)) % max(1, n)
        self._safe_set(p, float(v))

    def cmd_eq8_page(self, direction):
        self._eq8_focus = max(1, min(EQ8_BANDS - EQ8_DIALS + 1,
                                     self._eq8_focus + (1 if direction >= 0 else -1)))
        # re-emit so the client knows the new dial->band window
        if self._device is not None and self._device.class_name == EQ8_CLASS:
            self._emit_eq8_full()

    # =========================================================== EQ8 KEY logic
    def _eq8_instances(self, track):
        return [d for d in track.devices if d.class_name == EQ8_CLASS]

    def cmd_eq8_key(self):
        track = self.song.view.selected_track
        if track is None:
            return
        eq8s = self._eq8_instances(track)
        selected = track.view.selected_device

        if selected is not None and selected.class_name == EQ8_CLASS and len(eq8s) > 1:
            # Condition A: cycle to the next EQ8 on the track
            idx = eq8s.index(selected)
            self._select_device(track, eq8s[(idx + 1) % len(eq8s)])
            self._cs.show_message("EQ8: next instance")
        elif eq8s:
            # Condition B: jump to the EQ8 closest to the current selection
            sel_idx = self._device_index(selected) if selected is not None else 0
            closest = min(eq8s, key=lambda d: abs(self._device_index(d) - sel_idx))
            self._select_device(track, closest)
            self._cs.show_message("EQ8: closest instance")
        else:
            # Condition C: create a new EQ8 on the track
            self._create_eq8(track)
            self._cs.show_message("EQ8: created")
        self._emit_eq8_state()

    def _select_device(self, track, device):
        # Song.View.select_device(device) is the documented selector and also
        # selects the device's track. NOTE: Track.View.selected_device is a
        # READ-ONLY property and Track.View has no select_device(), so the old
        # track.view.* approach silently did nothing.
        self.song.view.selected_track = track
        try:
            self.song.view.select_device(device)
        except Exception as e:
            self.log("select_device failed: %s" % e)

    def _emit_eq8_state(self):
        track = self.song.view.selected_track
        if track is None:
            self.send({"t": "eq8_state", "count": 0, "selected_is_eq8": False, "selected_index": -1})
            return
        eq8s = self._eq8_instances(track)
        sel = track.view.selected_device
        self.send({
            "t": "eq8_state",
            "count": len(eq8s),
            "selected_is_eq8": bool(sel is not None and sel.class_name == EQ8_CLASS),
            "selected_index": self._device_index(sel) if sel is not None else -1,
        })

    # ============================================================ device create
    def _browser(self):
        return Live.Application.get_application().browser

    def _find_item(self, root, predicate, depth=0):
        """Depth-first search for the first BrowserItem matching predicate."""
        if root is None or depth > 6:
            return None
        try:
            children = root.children
        except Exception:
            children = []
        for child in children:
            try:
                if predicate(child):
                    return child
            except Exception:
                pass
            found = self._find_item(child, predicate, depth + 1)
            if found is not None:
                return found
        return None

    def _create_eq8(self, track):
        item = self._find_item(
            self._browser().audio_effects,
            lambda c: c.name == "EQ Eight" and getattr(c, "is_loadable", False),
        )
        if item is None:
            self.send({"t": "error", "message": "EQ Eight not found in browser"})
            return
        self.song.view.selected_track = track
        self._browser().load_item(item)   # loads onto the selected track, selects it

    # -------------------------------------------------------- generic loader
    """Load ANY browser device by name (V30).

    _create_eq8 above searches audio_effects only, which is correct for a stock
    Live device and useless for a plug-in: FabFilter Pro-Q 3 lives under
    browser.plugins, and on another machine the same plug-in may be reached as
    VST3, VST2 or AU. So the search walks every root the browser exposes and
    matches on NAME, which is the one thing stable across all of them.

    Matching is case-insensitive and ignores spaces and hyphens, because the
    browser spells it "FabFilter Pro-Q 3" and a caller may reasonably say
    "fabfilter proq3". Exact-ish first, then a contains pass, so "Pro-Q 3" can
    never be satisfied by "Pro-Q 3 (m/s)" while an exact match exists.
    """

    @staticmethod
    def _norm(name):
        return "".join(ch for ch in str(name).lower() if ch.isalnum())

    def _browser_roots(self):
        b = self._browser()
        roots = []
        # plugins first: a VST is the common case for this verb, and it keeps the
        # walk short for the device we are actually most likely to be asked for.
        for attr in ("plugins", "audio_effects", "instruments",
                     "midi_effects", "user_library", "packs"):
            root = getattr(b, attr, None)
            if root is not None:
                roots.append(root)
        return roots

    def cmd_load_device(self, name):
        want = self._norm(name)
        if not want:
            self.send({"t": "error", "message": "load_device: no name given"})
            return

        track = self.song.view.selected_track
        if track is None:
            self.send({"t": "error", "message": "load_device: no selected track"})
            return

        item = None
        for match in (lambda c: self._norm(getattr(c, "name", "")) == want,
                      lambda c: want in self._norm(getattr(c, "name", ""))):
            for root in self._browser_roots():
                item = self._find_item(
                    root,
                    lambda c, m=match: m(c) and getattr(c, "is_loadable", False),
                )
                if item is not None:
                    break
            if item is not None:
                break

        if item is None:
            self.send({"t": "error",
                       "message": "load_device: '%s' not found in the browser" % name})
            return

        try:
            self.song.view.selected_track = track
            self._browser().load_item(item)
            self.send({"t": "device_loaded", "name": getattr(item, "name", name),
                       "track": getattr(track, "name", "")})
        except Exception as e:
            self.send({"t": "error", "message": "load_device failed: %s" % e})

    # ==================================== V48 — the UNIFIED device key
    """Adi's ruling: "Do not make EQ8 special." Every plugin shortcut on the
    Ableton hub behaves the same way.

        short press  none on the track -> insert; one -> focus it;
                     several -> focus the NEXT one on each press
        long  press  always append a new instance

    This is cmd_eq8_key's logic (Conditions A/B/C) generalised from "devices whose
    class_name is Eq8" to "devices whose NAME matches", which is the only handle a
    VST gives us — every VST3 shares class_name "PluginDevice", so class matching
    cannot tell Pro-Q 3 from Serum. cmd_eq8_key is left exactly as it was; nothing
    calls it any more, but it is verified code and deleting it is not this batch's
    job.
    """

    def _devices_named(self, track, name):
        # EXACT first, then PREFIX — deliberately NOT the contains pass that
        # cmd_load_device uses on the browser.
        #
        # Contains is right for the browser, where "Serum" has to reach the
        # installed "Serum2". On a TRACK it is wrong, and the test caught it:
        # "compressor" is contained in "gluecompressor", so a track holding only a
        # Glue Compressor answered the Compressor key by focusing the Glue and
        # never inserting the Compressor that was asked for.
        #
        # A prefix keeps the leniency that matters — a device named for a later
        # version ("Serum2") still answers to its stem — while a plugin whose name
        # merely ENDS with another's no longer impersonates it.
        want = self._norm(name)
        if not want:
            return []
        devs = list(track.devices)
        exact = [d for d in devs if self._norm(getattr(d, "name", "")) == want]
        if exact:
            return exact
        return [d for d in devs if self._norm(getattr(d, "name", "")).startswith(want)]

    def cmd_device_key(self, name, force_new=False):
        track = self.song.view.selected_track
        if track is None:
            self.send({"t": "error", "message": "device_key: no selected track"})
            return

        if force_new:
            self.cmd_load_device(name)          # long press: always append
            return

        hits = self._devices_named(track, name)
        if not hits:
            self.cmd_load_device(name)          # nothing there: insert one
            return

        sel = track.view.selected_device
        try:
            at = hits.index(sel) if sel is not None else -1
        except ValueError:
            at = -1

        # Already standing on one of them and there are others -> advance.
        # Otherwise focus the first. `at` of -1 covers "selection is elsewhere".
        target = hits[(at + 1) % len(hits)] if (at >= 0 and len(hits) > 1) else hits[0]
        self._select_device(track, target)
        try:
            self._cs.show_message("%s  %d/%d" % (name, hits.index(target) + 1, len(hits)))
        except Exception:
            pass
        self.send({"t": "device_focused", "name": getattr(target, "name", name),
                   "count": len(hits), "index": hits.index(target)})

    # ============================== V48 — track volume and pan (the idle dials)
    """Volume is the awkward one, and it is worth saying why.

    `mixer_device.volume` is a DeviceParameter whose `value` is NORMALISED 0..1 on
    Live's own fader curve. There is no dB setter, and the curve is not something
    we can invert analytically. Adi's requirement is "strictly 0.5 dB increments",
    so approximating the curve is not good enough — the step has to land on the
    same dB grid Live itself displays.

    So the dB is READ FROM THE PARAMETER'S OWN DISPLAY STRING, and the normalised
    value for a target dB is found by BISECTION on that same function. That is
    exact by construction: it agrees with whatever Live shows, on any Live version,
    with no curve constants to go stale. It costs ~24 string formats per dial tick,
    which is nothing beside the round trip that delivered the tick.

    Pan is linear -1..1, so it needs none of this. 0.02 is one unit of Live's own
    50L..C..50R readout.
    """
    VOL_STEP_DB = 0.5
    VOL_MIN_DB = -60.0          # below this Live's fader is -inf, and so is ours
    PAN_STEP = 0.02

    @staticmethod
    def _parse_db(text):
        t = str(text).replace("dB", "").strip()
        if "inf" in t.lower():
            return float("-inf")
        try:
            return float(t)
        except ValueError:
            return float("-inf")

    def _db_at(self, param, value):
        try:
            return self._parse_db(param.str_for_value(value))
        except Exception:
            return float("-inf")

    def _norm_for_db(self, param, db):
        lo, hi = 0.0, 1.0
        # 30, not 24: at 24 the residue was ~0.01 dB, which is visible in a
        # readout printed to two decimals. 30 halvings is free.
        for _ in range(30):
            mid = (lo + hi) / 2.0
            if self._db_at(param, mid) < db:
                lo = mid
            else:
                hi = mid
        return (lo + hi) / 2.0

    def _mixer(self):
        track = self.song.view.selected_track
        if track is None:
            return None, None, None
        m = getattr(track, "mixer_device", None)
        if m is None:
            return track, None, None
        return track, getattr(m, "volume", None), getattr(m, "panning", None)

    def cmd_track_volume_delta(self, steps):
        track, vol, _pan = self._mixer()
        if vol is None:
            return
        n = int(steps)
        if n == 0:
            return
        cur = self._db_at(vol, vol.value)
        # SNAP TO THE GRID FIRST. A fader parked at -6.02 dB by a mouse drag must
        # land on -6.0 and stay on the half-dB grid from then on; stepping from the
        # raw value would carry that 0.02 forever.
        base = self.VOL_MIN_DB if cur == float("-inf") else \
            round(cur / self.VOL_STEP_DB) * self.VOL_STEP_DB
        target = base + n * self.VOL_STEP_DB
        top = self._db_at(vol, 1.0)
        if target > top:
            target = top
        if target <= self.VOL_MIN_DB:
            vol.value = 0.0
        else:
            vol.value = self._norm_for_db(vol, target)
        self._emit_mix()

    def cmd_track_pan_delta(self, steps):
        _track, _vol, pan = self._mixer()
        if pan is None:
            return
        v = pan.value + int(steps) * self.PAN_STEP
        pan.value = max(pan.min, min(pan.max, v))
        self._emit_mix()

    def _emit_mix(self):
        track, vol, pan = self._mixer()
        if track is None:
            self.send({"t": "mix", "has_track": False})
            return
        msg = {"t": "mix", "has_track": True, "track": getattr(track, "name", "")}
        if vol is not None:
            msg["vol"] = vol.value
            msg["vol_disp"] = self._disp(vol)
        if pan is not None:
            msg["pan"] = pan.value
            msg["pan_disp"] = self._disp(pan)
        self.send(msg)

    def _disp(self, param):
        try:
            return str(param.str_for_value(param.value))
        except Exception:
            return ""

    def cmd_get_mix(self):
        self._emit_mix()

    # The dials must not go stale when the fader is moved with the mouse, so the
    # two mixer parameters are watched for the lifetime of the selected track and
    # torn down with it. Same shape as the device listeners above.
    def _mix_listen(self, track):
        self._unmix_listen()
        if track is None:
            return
        _t, vol, pan = self._mixer()
        for p in (vol, pan):
            if p is None:
                continue
            try:
                p.add_value_listener(self._emit_mix)
                self._mixed.append(p)
            except Exception as e:
                self.log("mix listen failed: %s" % e)

    def _unmix_listen(self):
        for p in getattr(self, "_mixed", []):
            try:
                p.remove_value_listener(self._emit_mix)
            except Exception:
                pass
        self._mixed = []

    # ================================================================= presets
    def _find_preset_root(self):
        return self._find_item(
            self._browser().user_library,
            lambda c: c.name == self.preset_folder and getattr(c, "is_folder", False),
        )

    def cmd_list_presets(self):
        self._preset_items = {}
        root = self._find_preset_root()
        items = []
        if root is not None:
            i = 0
            try:
                children = root.children
            except Exception:
                children = []
            for c in children:
                if getattr(c, "is_loadable", False):
                    self._preset_items[i] = c
                    items.append({"id": i, "name": c.name})
                    i += 1
        self.send({"t": "presets", "items": items})

    def cmd_load_preset(self, preset_id, replace=True):
        if preset_id not in self._preset_items:
            self.cmd_list_presets()
        item = self._preset_items.get(preset_id)
        if item is None:
            self.send({"t": "error", "message": "preset not found"})
            return
        track = self.song.view.selected_track
        if track is None:
            return
        self.song.view.selected_track = track

        if replace:
            # "Load onto current EQ8": insert the preset right after the selected
            # EQ8, then delete the old one (the API can't rewrite in place).
            sel = track.view.selected_device
            old_idx = self._device_index(sel) if (sel is not None and sel.class_name == EQ8_CLASS) else None
            self._browser().load_item(item)
            if old_idx is not None:
                try:
                    track.delete_device(old_idx)            # new preset shifts into old slot
                    new_dev = track.devices[old_idx]
                    self._select_device(track, new_dev)
                except Exception as e:
                    self.log("replace-delete failed: %s" % e)
        else:
            # "New instance with preset"
            self._browser().load_item(item)
        self._emit_eq8_state()

    # ============================================================ navigation
    def cmd_select_track(self, direction):
        tracks = list(self.song.tracks)
        cur = self.song.view.selected_track
        try:
            i = tracks.index(cur)
        except ValueError:
            i = 0
        i = max(0, min(len(tracks) - 1, i + (1 if direction >= 0 else -1)))
        self.song.view.selected_track = tracks[i]

    def cmd_select_device(self, direction):
        track = self.song.view.selected_track
        if track is None or not track.devices:
            return
        devs = list(track.devices)
        cur = track.view.selected_device
        try:
            i = devs.index(cur)
        except ValueError:
            i = 0
        i = max(0, min(len(devs) - 1, i + (1 if direction >= 0 else -1)))
        self._select_device(track, devs[i])

    # ===================================================== named-parameter channel
    # Used by predefined VST controllers (e.g. Pulsar Massive) that need the full
    # parameter list and to address parameters by index rather than slot.
    def _param(self, i):
        if not self._device:
            return None
        params = self._device.parameters
        return params[i] if 0 <= i < len(params) else None

    def cmd_get_all_params(self):
        out = []
        if self._device:
            for i, p in enumerate(self._device.parameters):
                try:
                    items = list(p.value_items)
                except Exception:
                    items = []
                out.append({
                    "i": i, "name": p.name, "value": p.value, "min": p.min, "max": p.max,
                    "quantized": bool(getattr(p, "is_quantized", False)), "items": items,
                    "disp": _fmt_generic(p),
                })
        self.send({"t": "all_params", "params": out})

    def cmd_watch(self, indices):
        self._clear_watch()
        for i in indices:
            p = self._param(i)
            if p is None:
                continue
            fn = self._watch_listener(i)
            try:
                p.add_value_listener(fn)
                self._watch.append((p, fn))
            except Exception as e:
                self.log("watch %d failed: %s" % (i, e))

    def _watch_listener(self, i):
        return self._cache_fn(("watch", i), lambda: (lambda: self._emit_p(i)))

    def _emit_p(self, i):
        p = self._param(i)
        if p is not None:
            self.send({"t": "p", "i": i, "value": p.value, "disp": _fmt_generic(p)})

    def _clear_watch(self):
        for p, fn in self._watch:
            try:
                p.remove_value_listener(fn)
            except Exception:
                pass
        self._watch = []

    def cmd_set_index(self, i, norm):
        p = self._param(i)
        if p is not None:
            self._safe_set(p, p.min + max(0.0, min(1.0, norm)) * (p.max - p.min))

    def cmd_delta_index(self, i, delta):
        p = self._param(i)
        if p is not None:
            self._safe_set(p, p.value + delta * ((p.max - p.min) or 1.0))

    def cmd_delta_log_index(self, i, delta):
        # Geometric (musical) nudge — for log-perceived params like frequency / Q.
        # Falls back to linear if the current value is <= 0 (can't scale through 0).
        p = self._param(i)
        if p is None:
            return
        v = p.value
        if v > 0:
            self._safe_set(p, v * (2.0 ** (delta * 4.0)))
        else:
            self._safe_set(p, v + delta * ((p.max - p.min) or 1.0))

    def cmd_step_index(self, i, direction, steps=0):
        p = self._param(i)
        if p is None:
            return
        d = 1 if direction >= 0 else -1
        if getattr(p, "is_quantized", False):
            try:
                n = len(p.value_items)
            except Exception:
                n = 0
            if n <= 0:
                n = int(round(p.max - p.min)) + 1
            cur = int(round(p.value - p.min))
            self._safe_set(p, p.min + ((cur + d) % max(1, n)))      # wrap (cycle)
        elif steps and steps > 1:
            stepsize = ((p.max - p.min) or 1.0) / (steps - 1)
            cur = int(round((p.value - p.min) / stepsize))
            self._safe_set(p, p.min + ((cur + d) % steps) * stepsize)  # wrap
        else:
            self._safe_set(p, p.value + d * ((p.max - p.min) or 1.0) * 0.04)

    def cmd_toggle_index(self, i):
        p = self._param(i)
        if p is not None:
            mid = (p.min + p.max) / 2.0
            self._safe_set(p, p.min if p.value > mid else p.max)

    def resend_all(self):
        try:
            ver = ".".join(str(x) for x in Live.Application.get_application().get_major_minor_version())
        except Exception:
            ver = "?"
        self.send({"t": "hello", "version": "1.0", "live": ver})
        self._on_track_changed()

    # ================================================================== helpers
    def _safe_set(self, p, value):
        try:
            v = max(p.min, min(p.max, value))
            if p.is_enabled:
                p.value = v
        except Exception as e:
            self.log("set %s failed: %s" % (getattr(p, "name", "?"), e))

    def _track_index(self, track):
        try:
            return list(self.song.tracks).index(track)
        except Exception:
            return -1

    def _device_index(self, device):
        if device is None or self._track is None:
            return -1
        try:
            return list(self._track.devices).index(device)
        except Exception:
            return -1

    # small cache so add/remove listener get the SAME bound function object
    _fn_cache = None

    def _cache_fn(self, key, factory):
        if self._fn_cache is None:
            self._fn_cache = {}
        if key not in self._fn_cache:
            self._fn_cache[key] = factory()
        return self._fn_cache[key]
