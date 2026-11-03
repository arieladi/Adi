"""The ADI_Logger remote script against a fake Live, through the real LiveLink.

No Live needed: _Framework.ControlSurface and the Live object model are stubbed
with just enough surface for the script. Checks that it stays idle with no
recording, then logs gestures, structure, transport and notes into the session
folder at the right level, and closes cleanly when the recording stops. Also
parses the script with Python 3.7's grammar, which is what Live 11 runs.

    .venv\\Scripts\\python.exe selftest_live.py
"""
from __future__ import annotations

import ast
import json
import shutil
import sys
import tempfile
import time
import types
from pathlib import Path

from livelink import LiveLink, live_status

HERE = Path(__file__).resolve().parent
SCRIPT_DIR = HERE / "live_remote_script" / "ADI_Logger"


# ------------------------------------------------------------------ fake Live

class Observable:
    """add_X_listener / remove_X_listener / X_has_listener for any X."""

    def __init__(self):
        self._listeners: dict[str, list] = {}

    def __getattr__(self, name):
        if name.startswith("add_") and name.endswith("_listener"):
            return lambda cb, p=name[4:-9]: self._listeners.setdefault(p, []).append(cb)
        if name.startswith("remove_") and name.endswith("_listener"):
            return lambda cb, p=name[7:-9]: self._listeners[p].remove(cb)
        if name.endswith("_has_listener"):
            return lambda cb, p=name[:-13]: cb in self._listeners.get(p, [])
        raise AttributeError(name)

    def fire(self, prop):
        for cb in list(self._listeners.get(prop, [])):
            cb()

    def listener_count(self):
        return sum(len(v) for v in self._listeners.values())


class Param(Observable):
    def __init__(self, name, value, unit=""):
        super().__init__()
        self.name, self.value, self.unit = name, value, unit
        self.automation_state = 0
        self.canonical_parent = None

    def __str__(self):
        return f"{self.value:.1f}{self.unit}"

    def set(self, value):
        self.value = value
        self.fire("value")


class Device(Observable):
    def __init__(self, name, params):
        super().__init__()
        self.name, self.parameters, self.class_name = name, params, name
        self.can_have_chains = False
        for p in params:
            p.canonical_parent = self


class Mixer(Observable):
    def __init__(self):
        super().__init__()
        self.volume = Param("Track Volume", 0.85)
        self.panning = Param("Track Panning", 0.0)
        self.sends = []
        self.track_activator = Param("Track Activator", 1.0)
        self.cue_volume = Param("Cue Volume", 0.85)


class Track(Observable):
    def __init__(self, name, devices):
        super().__init__()
        self.name, self.devices = name, devices
        self.mute = self.solo = self.arm = False
        self.can_be_armed = True
        self.playing_slot_index = self.fired_slot_index = -1
        self.clip_slots = []
        self.mixer_device = Mixer()
        self.view = Observable()
        self.view.selected_device = devices[0] if devices else None


class Clip(Observable):
    def __init__(self, name):
        super().__init__()
        self.name, self.is_midi_clip, self.length, self.looping = name, True, 4.0, True
        self.loop_start, self.loop_end, self.start_marker, self.end_marker = 0.0, 4.0, 0.0, 4.0
        self.canonical_parent = None
        self.notes = [(36, 0.0, 0.25, 100, False), (38, 1.0, 0.25, 90, False)]

    def get_notes_extended(self, from_pitch, pitch_span, from_time, time_span):
        return [types.SimpleNamespace(pitch=p, start_time=s, duration=d, velocity=v, mute=m)
                for p, s, d, v, m in self.notes]


class Song(Observable):
    def __init__(self):
        super().__init__()
        self.threshold = Param("Threshold", 0.8, " dB")
        self.tracks = [Track("Bass", [Device("Compressor", [Param("Device On", 1.0), self.threshold])]),
                       Track("Drums", [])]
        self.return_tracks = [Track("A-Reverb", [])]
        self.master_track = Track("Master", [])
        self.scenes = []
        self.tempo, self.is_playing, self.current_song_time = 128.0, False, 0.0
        self.record_mode = self.session_record = self.metronome = self.loop = False
        self.signature_numerator, self.signature_denominator = 4, 4
        self.view = Observable()
        self.view.selected_track = self.tracks[0]
        self.view.selected_scene = None
        self.view.selected_parameter = None
        self.view.detail_clip = None

    def all_listeners(self):
        objs = [self, self.view]
        for t in self.tracks + self.return_tracks + [self.master_track]:
            m = t.mixer_device
            objs += [t, t.view, m, m.volume, m.panning, m.track_activator, m.cue_volume]
            for d in t.devices:
                objs += [d] + list(d.parameters)
        return sum(o.listener_count() for o in objs)


class App(Observable):
    def __init__(self):
        super().__init__()
        self.view = Observable()
        self.view.focused_document_view = "Session"

    def get_major_version(self): return 11
    def get_minor_version(self): return 3
    def get_bugfix_version(self): return 10


def install_fake_framework(song, app):
    class ControlSurface:
        def __init__(self, c_instance): pass
        def song(self): return song
        def application(self): return app
        def log_message(self, *a): pass
        def update_display(self): pass
        def disconnect(self): pass

    framework = types.ModuleType("_Framework")
    module = types.ModuleType("_Framework.ControlSurface")
    module.ControlSurface = ControlSurface
    sys.modules["_Framework"] = framework
    sys.modules["_Framework.ControlSurface"] = module


# ---------------------------------------------------------------------- test

failures = []


def check(label, ok, detail=""):
    print(f"  [{'ok ' if ok else 'FAIL'}] {label}{(' - ' + str(detail)) if detail else ''}")
    if not ok:
        failures.append(label)


def read_events(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def run_level(level: str, root: Path) -> None:
    print(f"{level} level:")
    control = root / ".live"
    control.mkdir(parents=True, exist_ok=True)
    session = root / f"session-{level}"
    session.mkdir()

    song, app = Song(), App()
    install_fake_framework(song, app)
    shutil.copytree(SCRIPT_DIR, root / f"pkg_{level}" / "ADI_Logger")
    (root / f"pkg_{level}" / "ADI_Logger" / "settings.json").write_text(
        json.dumps({"control_dir": str(control)}), encoding="utf-8")
    sys.path.insert(0, str(root / f"pkg_{level}"))
    for name in [m for m in sys.modules if m.startswith("ADI_Logger")]:
        del sys.modules[name]
    import ADI_Logger
    clock = [time.time()]
    # The script reads the wall clock through its own module; give it a fake one.
    sys.modules["ADI_Logger.live_logger"].time = types.SimpleNamespace(time=lambda: clock[0])
    surface = ADI_Logger.create_instance(None)
    sys.path.pop(0)

    def tick(seconds=0.1, n=1):
        for _ in range(n):
            clock[0] += seconds
            surface._tick(clock[0])

    tick(1.1)
    check("no listeners while nothing records", song.all_listeners() == 0, song.all_listeners())
    check("heartbeat says connected", live_status(control) is not None)

    link = LiveLink(control, "test", level, session, "make a snare riser")
    link.start()
    check("LiveLink saw Live connected", link.connected_at_start)
    tick(1.1)
    check("listeners attached once a recording exists", song.all_listeners() > 10, song.all_listeners())

    # one knob gesture: three moves, then quiet
    if True:
        for value in (0.7, 0.6, 0.52):
            song.threshold.set(value)
            tick(0.1)
        tick(0.1, n=10)  # longer than SETTLE
        song.tracks[0].mute = True
        song.tracks[0].fire("mute")
        song.is_playing = True
        song.fire("is_playing")
        song.threshold.automation_state = 1
        song.threshold.set(0.3)  # automation playback while playing: not a person
        tick(0.1, n=10)
        song.tracks[1].devices.append(Device("EQ Eight", [Param("Device On", 1.0)]))
        song.tracks[1].fire("devices")
        tick(0.1, n=2)
        clip = Clip("Riser")
        song.view.detail_clip = clip
        song.view.fire("detail_clip")
        clip.notes.append((40, 2.0, 0.5, 110, False))
        clip.fire("notes")
        tick(0.1, n=8)

    stats = link.stop(wait=0.1)
    tick(1.1)  # the script notices the withdrawn sink
    check("listeners removed after the recording", song.all_listeners() == 0, song.all_listeners())

    events = read_events(session / "live_events.jsonl")
    kinds = [e["ev"] for e in events]
    check("start record carries the description",
          events and events[0]["ev"] == "start" and events[0]["description"] == "make a snare riser")
    check("snapshot follows", len(events) > 1 and events[1]["ev"] == "snapshot")
    params = [e for e in events if e["ev"] == "param" and e.get("param") == "Threshold"]
    check("one gesture -> one param line", len(params) == 1, len(params))
    if params:
        p = params[0]
        check("gesture from/to/display", p["from"] == 0.8 and p["to"] == 0.52 and p["display"] == "0.5 dB",
              {k: p[k] for k in ("from", "to", "display")})
        check("gesture names its place", p["track"] == "Bass" and p["device"] == "Compressor"
              and p["track_index"] == 0, p)
    moves = [e for e in events if e["ev"] == "param_move"]
    if level == "deep":
        check("deep: movement samples", len(moves) >= 3, len(moves))
        check("deep: snapshot has parameter values",
              "device_params" in events[1]["tracks"][0], list(events[1]["tracks"][0]))
    else:
        check("simple: no movement samples", not moves, len(moves))
    check("automation playback ignored", not any(e.get("to") == 0.3 for e in events))
    check("mute logged", any(e["ev"] == "mute" and e["value"] is True for e in events))
    check("transport logged", any(e["ev"] == "transport" and e["playing"] for e in events))
    check("device added logged", any(e["ev"] == "devices" and "EQ Eight" in e["devices"] for e in events))
    notes = [e for e in events if e["ev"] == "notes"]
    check("notes edit logged", len(notes) == 1 and notes[0]["count"] == 3, notes)
    if notes:
        check(f"{level}: notes list {'included' if level == 'deep' else 'omitted'}",
              ("notes" in notes[0]) == (level == "deep"))
    check("closed with an end line", kinds[-1:] == ["end"] and stats["ended_cleanly"] is not None, kinds[-3:])
    check("timestamps monotonic", all(a["t"] <= b["t"] + 1e-6 for a, b in zip(events, events[1:])))

    surface.disconnect()


def main() -> int:
    print("python 3.7 grammar (Live 11):")
    for path in sorted(SCRIPT_DIR.glob("*.py")):
        try:
            ast.parse(path.read_text(encoding="utf-8"), filename=str(path), feature_version=(3, 7))
            check(path.name, True)
        except SyntaxError as exc:
            check(path.name, False, exc)

    root = Path(tempfile.mkdtemp(prefix="ail_livetest_"))
    try:
        run_level("simple", root)
        run_level("deep", root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("PASS" if not failures else f"{len(failures)} CHECK(S) FAILED: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
