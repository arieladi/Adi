"""What changed inside Live, written next to the desktop logger's events.csv.

Idle until the desktop logger starts a recording. The logger drops a small JSON
file in the control directory naming its session folder and level; from then on
this script appends one JSON object per line to <session>/live_events.jsonl,
stamped with the same wall clock (time.time()) as events.csv:

    {"t": 1792681234.51, "ev": "param", "track": "Bass", "track_index": 2,
     "device": "Compressor", "device_path": "1", "param": "Threshold",
     "from": 0.82, "to": 0.52, "display": "-20.0 dB", "t0": 1792681233.9}

Levels:
    simple  one "param" line per gesture (first value -> settled value), plus
            structure, transport, selection, clip launches and note counts
    deep    all of that, plus "param_move" samples at ~10 Hz while a value
            moves, every parameter value at the start, and the notes themselves

Runs on Live's Python (3.7 in Live 11) on Live's main thread, so listeners only
mark things dirty. Values are read and the file is written from update_display
(~10 Hz), batched. With no recording running there are no listeners at all.
Automation playback is not logged: a parameter whose automation is playing
while the transport runs is Live moving it, not you.
"""
from __future__ import absolute_import, print_function, unicode_literals

import json
import os
import time

from _Framework.ControlSurface import ControlSurface

HERE = os.path.dirname(os.path.abspath(__file__))
STATUS_FILE = "live.json"

SINK_POLL = 1.0       # seconds between control-directory scans
SINK_STALE = 20.0     # a sink file untouched this long belongs to a dead logger
STATUS_EVERY = 5.0    # heartbeat the desktop logger reads as "connected"
FLUSH_EVERY = 0.5
SETTLE = 0.75         # a value quiet this long ends one gesture
SAMPLE_EVERY = 0.1    # deep: the path of a gesture, not just its ends
NOTES_SETTLE = 0.5
MAX_DEPTH = 4         # rack nesting followed
AUTOMATION_PLAYING = 1


def _control_dir():
    try:
        with open(os.path.join(HERE, "settings.json"), encoding="utf-8-sig") as f:
            path = json.load(f).get("control_dir")
        if path:
            return os.path.expanduser(path)
    except (IOError, OSError, ValueError):
        pass
    return os.path.join(os.path.expanduser("~"), "Documents", "InteractionLogs", ".live")


def _num(value):
    """LOM values are floats, ints or bools; keep the log free of 0.30000000000000004."""
    if isinstance(value, float):
        return round(value, 6)
    return value


class Sink(object):
    """One desktop recording we are writing into."""

    def __init__(self, path, info, mtime):
        self.path = path
        self.mtime = mtime
        self.level = info.get("level", "simple")
        self.deep = self.level == "deep"
        self.session_dir = info["session_dir"]
        self.out = open(os.path.join(self.session_dir, "live_events.jsonl"), "a", encoding="utf-8")
        self.buf = []
        self.written = 0

    def emit(self, record):
        self.buf.append(record)

    def flush(self):
        if not self.buf:
            return
        self.out.write("".join(
            json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n" for r in self.buf))
        self.out.flush()
        self.written += len(self.buf)
        self.buf = []

    def close(self):
        self.emit({"t": time.time(), "ev": "end", "events": self.written + len(self.buf)})
        self.flush()
        self.out.close()


class Listeners(object):
    """Every listener added, so each can be removed again."""

    def __init__(self):
        self.items = []

    def add(self, obj, prop, callback):
        try:
            getattr(obj, "add_%s_listener" % prop)(callback)
        except (AttributeError, RuntimeError):
            return  # property absent here, e.g. arm on a return track
        self.items.append((obj, prop, callback))

    def clear(self):
        for obj, prop, callback in self.items:
            try:
                if getattr(obj, "%s_has_listener" % prop)(callback):
                    getattr(obj, "remove_%s_listener" % prop)(callback)
            except Exception:
                pass  # object already deleted with its track or device
        self.items = []


class Watched(object):
    """A continuous value: read() -> (value, display), where() -> location fields."""

    __slots__ = ("read", "where", "value", "display", "gesture")

    def __init__(self, read, where, value):
        self.read = read
        self.where = where
        self.value = value
        self.display = None
        self.gesture = None


class Group(object):
    """Listeners and watched values that are rebuilt together."""

    def __init__(self, owner=None):
        self.owner = owner
        self.listeners = Listeners()
        self.keys = []


class LiveLogger(ControlSurface):

    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._control_dir = _control_dir()
        self._sinks = {}
        self._watched = {}
        self._next_key = 0
        self._dirty = {}
        self._gestures = set()
        self._song_group = None
        self._track_groups = []
        self._clip_group = None
        self._rebuild_all = False
        self._rebuild_tracks = []
        self._notes_dirty_at = None
        self._playing = False
        self._next_poll = 0.0
        self._next_flush = 0.0
        self._next_status = 0.0
        self._errors = 0
        self.log_message("ADI_Logger: control dir %s" % self._control_dir)

    # ----------------------------------------------------------- Live hooks

    def disconnect(self):
        try:
            self._teardown()
            for sink in list(self._sinks.values()):
                sink.close()
            self._sinks = {}
            self._write_status(time.time(), connected=False)
        finally:
            ControlSurface.disconnect(self)

    def update_display(self):
        ControlSurface.update_display(self)
        try:
            self._tick(time.time())
        except Exception as exc:
            # Never let a bug here spam Live's log ten times a second.
            self._errors += 1
            if self._errors <= 20:
                self.log_message("ADI_Logger: tick failed: %r" % (exc,))

    def _tick(self, now):
        if now >= self._next_poll:
            self._next_poll = now + SINK_POLL
            self._poll_sinks(now)
        if now >= self._next_status:
            self._next_status = now + STATUS_EVERY
            self._write_status(now)
        if not self._sinks:
            return
        if self._rebuild_all:
            self._build()
        elif self._rebuild_tracks:
            pending, self._rebuild_tracks = self._rebuild_tracks, []
            for group in pending:
                if group in self._track_groups:
                    self._build_track(group)
        self._process_values(now)
        self._process_notes(now)
        if now >= self._next_flush:
            self._next_flush = now + FLUSH_EVERY
            for sink in self._sinks.values():
                sink.flush()

    # ---------------------------------------------------------------- sinks

    def _poll_sinks(self, now):
        seen = {}
        try:
            names = os.listdir(self._control_dir)
        except OSError:
            names = []
        for name in names:
            if not name.endswith(".json") or name == STATUS_FILE:
                continue
            path = os.path.join(self._control_dir, name)
            try:
                seen[path] = os.path.getmtime(path)
            except OSError:
                pass

        was_armed = bool(self._sinks)
        for path in list(self._sinks):
            sink = self._sinks[path]
            mtime = seen.get(path)
            if mtime is not None:
                sink.mtime = mtime
            if mtime is None or now - sink.mtime > SINK_STALE:
                del self._sinks[path]
                sink.close()

        for path, mtime in seen.items():
            if path in self._sinks or now - mtime > SINK_STALE:
                continue
            try:
                with open(path, encoding="utf-8-sig") as f:
                    info = json.load(f)
                sink = Sink(path, info, mtime)
            except Exception as exc:
                self.log_message("ADI_Logger: cannot open sink %s: %r" % (path, exc))
                continue
            self._sinks[path] = sink
            if not self._song_group:
                self._build()
            self._open_sink(sink, info)

        if was_armed and not self._sinks:
            self._teardown()

    def _open_sink(self, sink, info):
        app = self.application()
        sink.emit({
            "t": time.time(), "ev": "start", "level": sink.level,
            "description": info.get("description", ""),
            "live_version": "%d.%d.%d" % (app.get_major_version(), app.get_minor_version(),
                                          app.get_bugfix_version()),
        })
        sink.emit(self._snapshot(sink.deep))

    def _emit(self, record, deep_only=False):
        record["t"] = time.time()
        for sink in self._sinks.values():
            if sink.deep or not deep_only:
                sink.emit(record)

    def _write_status(self, now, connected=True):
        try:
            if not os.path.isdir(self._control_dir):
                return
            app = self.application()
            status = {
                "t": now, "connected": connected, "pid": os.getpid(),
                "live_version": "%d.%d.%d" % (app.get_major_version(), app.get_minor_version(),
                                              app.get_bugfix_version()),
                "watched_values": len(self._watched),
                "listeners": self._listener_count(),
                "sinks": [s.session_dir for s in self._sinks.values()],
            }
            tmp = os.path.join(self._control_dir, STATUS_FILE + ".tmp")
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(status, f)
            os.replace(tmp, os.path.join(self._control_dir, STATUS_FILE))
        except Exception:
            pass

    def _listener_count(self):
        groups = [self._song_group, self._clip_group] + self._track_groups
        return sum(len(g.listeners.items) for g in groups if g is not None)

    # ------------------------------------------------------------ structure

    def _teardown(self):
        for group in [self._song_group, self._clip_group] + self._track_groups:
            if group is not None:
                self._drop(group)
        self._song_group = self._clip_group = None
        self._track_groups = []
        self._watched = {}
        self._dirty = {}
        self._gestures = set()
        self._rebuild_all = False
        self._rebuild_tracks = []
        self._notes_dirty_at = None

    def _drop(self, group):
        group.listeners.clear()
        for key in group.keys:
            self._watched.pop(key, None)
            self._dirty.pop(key, None)
            self._gestures.discard(key)
        group.keys = []

    def _build(self):
        self._teardown()
        song = self.song()
        self._playing = bool(song.is_playing)
        self._song_group = group = Group()
        on = self._discrete

        def changed_tracks():
            self._rebuild_all = True
            self._emit({"ev": "tracks", "names": [t.name for t in self.song().tracks]})

        group.listeners.add(song, "tracks", changed_tracks)
        group.listeners.add(song, "return_tracks", changed_tracks)
        group.listeners.add(song, "is_playing", self._on_playing)
        on(group, song, "record_mode", lambda: {"ev": "record", "value": song.record_mode})
        on(group, song, "session_record", lambda: {"ev": "session_record", "value": song.session_record})
        on(group, song, "metronome", lambda: {"ev": "metronome", "value": song.metronome})
        on(group, song, "loop", lambda: {"ev": "arrangement_loop", "value": song.loop})
        on(group, song, "signature_numerator",
           lambda: {"ev": "signature", "value": "%d/%d" % (song.signature_numerator,
                                                           song.signature_denominator)})
        on(group, song, "scenes", lambda: {"ev": "scenes", "count": len(song.scenes)})
        self._watch(group, song, "tempo", lambda: (song.tempo, "%.2f BPM" % song.tempo),
                    lambda: {"param": "Tempo", "device": "Song"})

        view = song.view
        on(group, view, "selected_track", lambda: dict(ev="select_track", **self._where(view.selected_track)))
        on(group, view, "selected_scene",
           lambda: {"ev": "select_scene", "scene": self._index(song.scenes, view.selected_scene),
                    "name": view.selected_scene.name})
        on(group, view, "selected_parameter", self._selected_parameter)
        group.listeners.add(view, "detail_clip", self._on_detail_clip)

        app_view = self.application().view
        on(group, app_view, "focused_document_view",
           lambda: {"ev": "view", "view": app_view.focused_document_view})

        for track in song.tracks:
            self._add_track(track, "track")
        for track in song.return_tracks:
            self._add_track(track, "return")
        self._add_track(song.master_track, "master")
        self._attach_clip(view.detail_clip)

    def _add_track(self, track, kind):
        group = Group(owner=(track, kind))
        self._track_groups.append(group)
        self._build_track(group)

    def _build_track(self, group):
        self._drop(group)
        track, kind = group.owner
        on = self._discrete
        where = lambda: self._where(track)

        def changed_devices():
            if group not in self._rebuild_tracks:
                self._rebuild_tracks.append(group)
            self._emit(dict(ev="devices", devices=self._device_names(track.devices), **where()))

        group.listeners.add(track, "devices", changed_devices)
        on(group, track, "name", lambda: dict(ev="track_name", **where()))
        if kind != "master":
            on(group, track, "mute", lambda: dict(ev="mute", value=track.mute, **where()))
            on(group, track, "solo", lambda: dict(ev="solo", value=track.solo, **where()))
        if kind == "track":
            if track.can_be_armed:
                on(group, track, "arm", lambda: dict(ev="arm", value=track.arm, **where()))
            on(group, track, "playing_slot_index",
               lambda: dict(ev="clip_playing", **self._slot(track, track.playing_slot_index)))
            on(group, track, "fired_slot_index",
               lambda: dict(ev="clip_fired", **self._slot(track, track.fired_slot_index)))
        on(group, track.view, "selected_device",
           lambda: dict(ev="select_device", device=getattr(track.view.selected_device, "name", None),
                        **where()))

        mixer = track.mixer_device
        params = [mixer.volume, mixer.panning] + list(mixer.sends)
        params.append(getattr(mixer, "cue_volume" if kind == "master" else "track_activator", None))
        for param in params:
            if param is not None:
                self._watch_param(group, track, None, "mixer", param)
        group.listeners.add(mixer, "sends", changed_devices)

        for index, device in enumerate(track.devices):
            self._add_device(group, track, device, str(index), 0, changed_devices)

    def _add_device(self, group, track, device, path, depth, changed):
        self._discrete(group, device, "name",
                       lambda: dict(ev="device_name", device=device.name, device_path=path,
                                    **self._where(track)))
        group.listeners.add(device, "parameters", changed)
        for param in device.parameters:
            self._watch_param(group, track, device, path, param)
        if depth >= MAX_DEPTH or not getattr(device, "can_have_chains", False):
            return
        group.listeners.add(device, "chains", changed)
        for c, chain in enumerate(device.chains):
            chain_path = "%s/c%d" % (path, c)
            group.listeners.add(chain, "devices", changed)
            mixer = getattr(chain, "mixer_device", None)
            if mixer is not None:
                for param in (mixer.volume, mixer.panning):
                    self._watch_param(group, track, chain, chain_path + "/mixer", param)
            for d, inner in enumerate(chain.devices):
                self._add_device(group, track, inner, "%s/%d" % (chain_path, d), depth + 1, changed)

    # ------------------------------------------------------------ detail clip

    def _on_detail_clip(self):
        clip = self.song().view.detail_clip
        self._attach_clip(clip)
        if clip is None:
            self._emit({"ev": "detail_clip", "clip": None})
            return
        record = {"ev": "detail_clip", "clip": clip.name, "is_midi": clip.is_midi_clip,
                  "length": _num(clip.length), "looping": clip.looping}
        record.update(self._clip_where(clip))
        self._emit(record)

    def _attach_clip(self, clip):
        if self._clip_group is not None:
            self._drop(self._clip_group)
        self._notes_dirty_at = None
        if clip is None:
            self._clip_group = None
            return
        self._clip_group = group = Group(owner=clip)
        self._discrete(group, clip, "name", lambda: {"ev": "clip_name", "clip": clip.name})
        self._discrete(group, clip, "looping", lambda: {"ev": "clip_looping", "value": clip.looping})
        for prop in ("loop_start", "loop_end", "start_marker", "end_marker"):
            self._watch(group, clip, prop, self._clip_reader(clip, prop),
                        lambda prop=prop: {"param": prop, "device": "Clip", "clip": clip.name})
        if clip.is_midi_clip:
            group.listeners.add(clip, "notes", self._on_notes)

    def _clip_reader(self, clip, prop):
        def read():
            value = getattr(clip, prop)
            return value, "%.3f beats" % value
        return read

    def _on_notes(self):
        self._notes_dirty_at = time.time()

    def _process_notes(self, now):
        if self._notes_dirty_at is None or now - self._notes_dirty_at < NOTES_SETTLE:
            return
        self._notes_dirty_at = None
        clip = self._clip_group.owner if self._clip_group else None
        if clip is None:
            return
        notes = self._read_notes(clip)
        # Simple sinks get the count, deep sinks the notes too.
        for sink in self._sinks.values():
            record = {"t": now, "ev": "notes", "clip": clip.name, "count": len(notes)}
            if sink.deep:
                record["notes"] = notes
            sink.emit(record)

    def _read_notes(self, clip):
        try:
            found = clip.get_notes_extended(0, 128, 0.0, clip.length)
            return [[n.pitch, _num(n.start_time), _num(n.duration), _num(n.velocity), bool(n.mute)]
                    for n in found]
        except AttributeError:
            found = clip.get_notes(0.0, 0, clip.length, 128)  # pre-Live-11 tuple API
            return [[n[0], _num(n[1]), _num(n[2]), _num(n[3]), bool(n[4])] for n in found]

    def _any_deep(self):
        return any(s.deep for s in self._sinks.values())

    # ----------------------------------------------------------- listeners

    def _discrete(self, group, obj, prop, make_record):
        def changed():
            try:
                record = make_record()
            except Exception:
                return
            self._emit(record)
        group.listeners.add(obj, prop, changed)

    def _watch(self, group, obj, prop, read, where):
        try:
            value = read()[0]
        except Exception:
            return
        key = self._next_key
        self._next_key += 1
        self._watched[key] = Watched(read, where, _num(value))
        group.keys.append(key)

        def changed():
            self._dirty.setdefault(key, time.time())
        group.listeners.add(obj, prop, changed)

    def _watch_param(self, group, track, device, path, param):
        key = self._next_key
        self._next_key += 1
        try:
            value = _num(param.value)
        except Exception:
            return

        def read():
            return param.value, str(param)

        def where():
            fields = self._where(track)
            fields["device"] = getattr(device, "name", "Mixer") if device is not None else "Mixer"
            fields["device_path"] = path
            fields["param"] = param.name
            return fields

        self._watched[key] = Watched(read, where, value)
        group.keys.append(key)

        def changed():
            try:
                if self._playing and param.automation_state == AUTOMATION_PLAYING:
                    return  # automation playback, not a person
            except AttributeError:
                pass
            self._dirty.setdefault(key, time.time())
        group.listeners.add(param, "value", changed)

    def _on_playing(self):
        self._playing = bool(self.song().is_playing)
        self._emit({"ev": "transport", "playing": self._playing,
                    "song_time": _num(self.song().current_song_time)})

    def _selected_parameter(self):
        param = self.song().view.selected_parameter
        if param is None:
            return {"ev": "select_param", "param": None}
        return {"ev": "select_param", "param": param.name,
                "device": getattr(param.canonical_parent, "name", None)}

    # ---------------------------------------------------------- value flow

    def _process_values(self, now):
        deep = self._any_deep()
        dirty, self._dirty = self._dirty, {}
        for key, first in dirty.items():
            watched = self._watched.get(key)
            if watched is None:
                continue
            try:
                value, display = watched.read()
            except Exception:
                continue
            value = _num(value)
            gesture = watched.gesture
            if gesture is None:
                gesture = watched.gesture = {"t0": first, "from": watched.value,
                                             "from_display": watched.display, "sampled": 0.0}
                self._gestures.add(key)
            gesture["to"], gesture["display"], gesture["last"] = value, display, now
            if deep and now - gesture["sampled"] >= SAMPLE_EVERY * 0.9:  # Live's tick jitters
                gesture["sampled"] = now
                record = {"ev": "param_move", "value": value, "display": display}
                record.update(watched.where())
                self._emit(record, deep_only=True)

        for key in list(self._gestures):
            watched = self._watched.get(key)
            if watched is None or watched.gesture is None:
                self._gestures.discard(key)
                continue
            gesture = watched.gesture
            if now - gesture["last"] < SETTLE:
                continue
            self._gestures.discard(key)
            watched.gesture = None
            watched.value, watched.display = gesture["to"], gesture["display"]
            if gesture["to"] == gesture["from"]:
                continue  # wiggled and came back
            record = {"ev": "param", "from": gesture["from"], "to": gesture["to"],
                      "display": gesture["display"], "t0": gesture["t0"]}
            if gesture["from_display"] is not None:
                record["from_display"] = gesture["from_display"]
            record.update(watched.where())
            self._emit(record)

    # ------------------------------------------------------------- snapshot

    def _snapshot(self, deep):
        song = self.song()
        tracks = []
        kinds = [(t, "track") for t in song.tracks] + [(t, "return") for t in song.return_tracks]
        kinds.append((song.master_track, "master"))
        for track, kind in kinds:
            entry = {"name": track.name, "kind": kind, "devices": self._device_names(track.devices)}
            if deep:
                entry["mixer"] = self._values_of(
                    [track.mixer_device.volume, track.mixer_device.panning] + list(track.mixer_device.sends))
                entry["device_params"] = [self._device_values(d) for d in track.devices]
            tracks.append(entry)
        if deep:
            # Displays read for the snapshot become the "from" text of later gestures.
            for watched in self._watched.values():
                try:
                    watched.display = watched.read()[1]
                except Exception:
                    pass
        selected = song.view.selected_track
        return {"t": time.time(), "ev": "snapshot", "deep": deep, "tempo": _num(song.tempo),
                "playing": bool(song.is_playing), "view": self.application().view.focused_document_view,
                "selected_track": selected.name if selected else None, "tracks": tracks}

    def _device_values(self, device, depth=0):
        entry = {"device": device.name, "class": getattr(device, "class_name", ""),
                 "params": self._values_of(device.parameters)}
        if depth < MAX_DEPTH and getattr(device, "can_have_chains", False):
            entry["chains"] = [{"chain": chain.name,
                                "devices": [self._device_values(d, depth + 1) for d in chain.devices]}
                               for chain in device.chains]
        return entry

    def _values_of(self, params):
        out = {}
        for param in params:
            try:
                out[param.name] = [_num(param.value), str(param)]
            except Exception:
                pass
        return out

    def _device_names(self, devices, depth=0):
        names = []
        for device in devices:
            if depth < MAX_DEPTH and getattr(device, "can_have_chains", False):
                names.append({device.name: [self._device_names(c.devices, depth + 1) for c in device.chains]})
            else:
                names.append(device.name)
        return names

    # ------------------------------------------------------------- location

    def _where(self, track):
        if track is None:
            return {"track": None}
        song = self.song()
        index = self._index(song.tracks, track)
        if index is not None:
            return {"track": track.name, "track_index": index}
        index = self._index(song.return_tracks, track)
        if index is not None:
            return {"track": track.name, "return_index": index}
        return {"track": track.name, "master": True}

    def _index(self, items, item):
        for i, candidate in enumerate(items):
            if candidate == item:
                return i
        return None

    def _slot(self, track, index):
        fields = self._where(track)
        fields["slot"] = index
        if index is not None and index >= 0:
            clip = track.clip_slots[index].clip
            fields["clip"] = clip.name if clip is not None else None
        return fields

    def _clip_where(self, clip):
        parent = clip.canonical_parent           # ClipSlot (session) or Track (arrangement)
        track = getattr(parent, "canonical_parent", None)
        if track is not None and hasattr(track, "clip_slots"):
            fields = self._where(track)
            fields["slot"] = self._index(track.clip_slots, parent)
            return fields
        fields = self._where(parent)
        fields["arrangement"] = True
        return fields
