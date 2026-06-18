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
    """Best-effort display string for an arbitrary parameter."""
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

    # ============================================================ lifecycle
    def setup(self):
        self._listen(self.song.view, "selected_track", self._on_track_changed)
        self._on_track_changed()

    def teardown(self):
        self._remove_device_listeners()
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

    def _eq8_listener(self, key):
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
            "gain": gain.value if gain else 0.0,
            "q": q.value if q else 0.0,
            "type": type_val,
            "type_name": type_name,
            "type_items": type_items,
        }

    def _emit_eq8_full(self):
        bands = [self._band_dict(b) for b in range(1, EQ8_BANDS + 1)]
        out = self._eq8_output()
        self.send({"t": "eq8", "page": self._eq8_focus, "focus": self._eq8_focus,
                   "output": out, "bands": bands})

    def _emit_eq8_band(self, band):
        self.send(dict({"t": "eq8_band"}, **self._band_dict(band)))

    def _eq8_output(self):
        for p in (self._device.parameters if self._device else []):
            if p.name in ("Output Gain", "Gain"):
                return p.value
        return 0.0

    def cmd_eq8_freq_delta(self, band, delta):
        p = self._eq8_get(band, "freq")
        if p is None:
            return
        # geometric (musical) frequency nudge; freq value is always > 0
        new = p.value * (2.0 ** (delta * 4.0))
        self._safe_set(p, new)

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
            self.send({"t": "eq8", "page": self._eq8_focus, "focus": self._eq8_focus,
                       "output": self._eq8_output(),
                       "bands": [self._band_dict(b) for b in range(1, EQ8_BANDS + 1)]})

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
