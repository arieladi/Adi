"""
maxpat.py -- minimal Max/MSP patcher-JSON writer.

Why this exists: the device is far too large to hand-author as .maxpat JSON
without index errors.  Here connections are made by object *reference*, ids and
patching rects are assigned automatically, and outlet indices are range-checked
against a table of known object arities before anything is written.

Also writes Max for Live .amxd containers.  The .amxd format is a trivial chunk
wrapper around the patcher JSON, verified by round-tripping a real device:

    'ampf' u32(4) 'aaaa'  'meta' u32(4) u32(0)  'ptch' u32(len) <json> NUL

Targets Max 8 JSON (fileversion 1 / appversion 8.3.1), which loads unchanged in
Max 9.  Nothing here emits a Max-9-only key.
"""

import json
import struct

APPVERSION = {
    "major": 8, "minor": 3, "revision": 1,
    "architecture": "x64", "modernui": 1,
}

# ---------------------------------------------------------------------------
# Known object arities.  Used only to catch typos / bad outlet indices at build
# time -- Max itself re-derives these from the object text on load.  Objects not
# listed here are not checked.  (inlets, outlets)
# ---------------------------------------------------------------------------
ARITY = {
    "plugin~": (2, 2), "plugout~": (2, 2), "live.thisdevice": (1, 3),
    "adstatus": (1, 1), "dspstate~": (1, 4),
    "pfft~": (1, 1),               # 1 signal in per fftin~, 1 out per fftout~
    "fftin~": (1, 3), "fftout~": (1, 1), "framedelay~": (2, 1),
    "cartopol~": (2, 2), "poltocar~": (2, 2),
    "jit.poke~": (2, 1), "jit.peek~": (2, 2),
    "peakamp~": (2, 1), "average~": (2, 1), "slide~": (3, 1),
    "edge~": (1, 2), "snapshot~": (2, 1), "number~": (2, 2),
    "biquad~": (6, 1), "sqrt~": (1, 1), "abs~": (1, 1),
    "delay~": (2, 1), "sig~": (1, 1), "selector~": (3, 1),
    "svf~": (3, 4),
    "counter": (5, 4), "metro": (2, 1), "delay": (2, 1),
    "loadbang": (0, 1), "loadmess": (0, 1),
    "thispatcher": (1, 2), "pattrstorage": (1, 4), "autopattr": (1, 3),
    "jit.matrix": (1, 2), "jit.gen": (1, 1), "jit.spill": (1, 2),
    "live.thisdevice ": (1, 3),
}

UI_CLASSES = {
    "live.dial", "live.numbox", "live.slider", "live.text", "live.toggle",
    "live.menu", "live.tab", "live.button", "comment", "jsui", "panel",
    "live.line", "message", "flonum", "number", "toggle", "button",
}


import re as _re


def variadic_arity(text):
    """
    Objects whose inlet/outlet count comes from their arguments.  Getting these
    from the text rather than a fixed table is the whole point of this builder:
    a `t b b` with one declared outlet would silently drop half the patch's
    connections.  Returns (nin, nout) or None.
    """
    parts = [t for t in text.split() if not t.startswith("@")]
    # drop attribute values too
    cleaned, skip = [], False
    for t in text.split():
        if skip:
            skip = False
            continue
        if t.startswith("@"):
            skip = True
            continue
        cleaned.append(t)
    parts = cleaned
    if not parts:
        return None
    name, args = parts[0], parts[1:]
    n = len(args)
    if name in ("t", "trigger"):
        return (1, max(1, n))
    if name in ("sel", "select"):
        return (2, n + 1) if n else (2, 2)
    if name == "route":
        return (2, n + 1) if n else (2, 2)
    if name == "unpack":
        return (1, max(2, n))
    if name == "pack":
        return (max(2, n), 1)
    if name == "pak":
        return (max(2, n), 1)
    if name == "expr":
        idx = [int(m) for m in _re.findall(r"\$[fis](\d+)", text)]
        return (max(1, max(idx) if idx else 1), 1)
    if name == "selector~":
        return ((int(args[0]) + 1) if args and args[0].isdigit() else 2, 1)
    if name == "gate":
        return (2, int(args[0]) if args and args[0].isdigit() else 1)
    return None


class Box:
    """One object in a patcher.  Returned by Patcher.obj()/ui()."""

    __slots__ = ("id", "d", "cls", "text", "nin", "nout", "patcher")

    def __init__(self, oid, d, cls, text, nin, nout, patcher):
        self.id, self.d, self.cls = oid, d, cls
        self.text, self.nin, self.nout = text, nin, nout
        self.patcher = patcher

    def __repr__(self):
        return "<Box %s %s %r>" % (self.id, self.cls, self.text[:40])


class Patcher:
    def __init__(self, rect=(100, 100, 1000, 700), presentation=False,
                 bgcolor=None, is_subpatcher=False, gridsize=8.0):
        self.boxes = []
        self.lines = []
        self.params = {}          # obj-id -> [longname, shortname, 0]
        self._n = 0
        self.rect = list(map(float, rect))
        self.presentation = presentation
        self.bgcolor = bgcolor
        self.is_subpatcher = is_subpatcher
        self.gridsize = gridsize
        self.dependencies = []

    # -- creation ----------------------------------------------------------
    def _newid(self, prefix="obj"):
        self._n += 1
        return "%s-%d" % (prefix, self._n)

    def obj(self, text, x=0, y=0, w=None, h=22.0, nin=None, nout=None, **kw):
        """A regular object box (maxclass 'newobj')."""
        name = text.split()[0] if text.strip() else ""
        a = variadic_arity(text) or ARITY.get(name)
        if nin is None:
            nin = a[0] if a else 1
        if nout is None:
            nout = a[1] if a else 1
        if w is None:
            w = max(40.0, 7.2 * len(text) + 12.0)
        oid = self._newid()
        d = {
            "id": oid, "maxclass": "newobj", "numinlets": nin,
            "numoutlets": nout, "patching_rect": [float(x), float(y), float(w), float(h)],
            "text": text,
        }
        if nout:
            d["outlettype"] = kw.pop("outlettype", [""] * nout)
        d.update(kw)
        self.boxes.append(d)
        b = Box(oid, d, "newobj", text, nin, nout, self)
        return b

    def msg(self, text, x=0, y=0, w=None, h=22.0, **kw):
        if w is None:
            w = max(30.0, 7.2 * len(text) + 12.0)
        oid = self._newid()
        d = {"id": oid, "maxclass": "message", "numinlets": 2, "numoutlets": 1,
             "outlettype": [""], "patching_rect": [float(x), float(y), float(w), float(h)],
             "text": text}
        d.update(kw)
        self.boxes.append(d)
        return Box(oid, d, "message", text, 2, 1, self)

    def comment(self, text, x=0, y=0, w=None, h=20.0, fontsize=None, **kw):
        if w is None:
            w = max(40.0, 6.6 * len(text) + 10.0)
        oid = self._newid()
        d = {"id": oid, "maxclass": "comment", "numinlets": 1, "numoutlets": 0,
             "patching_rect": [float(x), float(y), float(w), float(h)], "text": text}
        if fontsize:
            d["fontsize"] = float(fontsize)
        d.update(kw)
        self.boxes.append(d)
        return Box(oid, d, "comment", text, 1, 0, self)

    def ui(self, maxclass, x=0, y=0, w=50.0, h=20.0, nin=1, nout=1, **kw):
        oid = self._newid()
        d = {"id": oid, "maxclass": maxclass, "numinlets": nin, "numoutlets": nout,
             "patching_rect": [float(x), float(y), float(w), float(h)]}
        if nout:
            d["outlettype"] = kw.pop("outlettype", [""] * nout)
        d.update(kw)
        self.boxes.append(d)
        return Box(oid, d, maxclass, maxclass, nin, nout, self)

    # -- Live parameters ---------------------------------------------------
    def param(self, box, longname, shortname, ptype, *, invisible=2,
              mmin=None, mmax=None, enum=None, initial=None, unitstyle=None,
              modmode=0, defer=0, exponent=None, steps=None):
        """
        Attach Live parameter metadata to a live.* box.

        invisible=2 is "Stored Only": the parameter is saved with the set but
        never appears in Live's automation / MIDI-map lists.  Every parameter in
        this device uses it -- an analyser must not pollute automation lanes.

        ptype: 0=float 1=int 2=enum 3=blob
        """
        sa = box.d.setdefault("saved_attribute_attributes", {})
        v = sa.setdefault("valueof", {})
        v["parameter_longname"] = longname
        v["parameter_shortname"] = shortname
        v["parameter_type"] = ptype
        v["parameter_invisible"] = invisible
        if mmin is not None:
            v["parameter_mmin"] = mmin
        if mmax is not None:
            v["parameter_mmax"] = mmax
        if enum is not None:
            v["parameter_enum"] = list(enum)
        if initial is not None:
            v["parameter_initial"] = list(initial) if isinstance(initial, (list, tuple)) else [initial]
            v["parameter_initial_enable"] = 1
        if unitstyle is not None:
            v["parameter_unitstyle"] = unitstyle
        if exponent is not None:
            v["parameter_exponent"] = exponent
        if steps is not None:
            v["parameter_steps"] = steps
        v["parameter_modmode"] = modmode
        if defer:
            v["parameter_defer"] = 1
        box.d["parameter_enable"] = 1
        box.d["varname"] = longname
        self.params[box.id] = [longname, shortname, 0]
        return box

    # -- wiring ------------------------------------------------------------
    def connect(self, src, sout, dst, din=0):
        if isinstance(src, tuple):
            src, sout = src
        if isinstance(dst, tuple):
            dst, din = dst
        if src.nout is not None and sout >= max(src.nout, 1):
            raise ValueError("outlet %d out of range for %r (has %d)"
                             % (sout, src, src.nout))
        if dst.nin is not None and din >= max(dst.nin, 1):
            raise ValueError("inlet %d out of range for %r (has %d)"
                             % (din, dst, dst.nin))
        self.lines.append({"patchline": {
            "destination": [dst.id, din], "source": [src.id, sout]}})

    def chain(self, *boxes):
        """connect a.0 -> b.0 -> c.0 ..."""
        for a, b in zip(boxes, boxes[1:]):
            self.connect(a, 0, b, 0)
        return boxes[-1]

    # -- output ------------------------------------------------------------
    def to_dict(self, extra=None):
        p = {
            "fileversion": 1,
            "appversion": dict(APPVERSION),
            "classnamespace": "box",
            "rect": self.rect,
            "bglocked": 0,
            "openinpresentation": 1 if self.presentation else 0,
            "default_fontsize": 10.0,
            "default_fontface": 0,
            "default_fontname": "Arial",
            "gridonopen": 1,
            "gridsize": [self.gridsize, self.gridsize],
            "gridsnaponopen": 1,
            "objectsnaponopen": 1,
            "statusbarvisible": 2,
            "toolbarvisible": 1,
            "lefttoolbarpinned": 0,
            "toptoolbarpinned": 0,
            "righttoolbarpinned": 0,
            "bottomtoolbarpinned": 0,
            "toolbars_unpinned_last_save": 0,
            "tallnewobj": 0,
            "boxanimatetime": 500,
            "enablehscroll": 1,
            "enablevscroll": 1,
            "devicewidth": getattr(self, "devicewidth", 0.0),
            "description": "",
            "digest": "",
            "tags": "",
            "style": "",
            "subpatcher_template": "",
            "assistshowspatchername": 0,
            "boxes": [{"box": b} for b in self.boxes],
            "lines": list(self.lines),
        }
        if self.bgcolor:
            p["bgcolor"] = list(self.bgcolor)
        if self.params:
            pr = dict(self.params)
            pr["parameterbanks"] = {"0": {"index": 0, "name": "",
                                          "parameters": ["-"] * 8}}
            pr["inherited_shortname"] = 1
            p["parameters"] = pr
        if not self.is_subpatcher:
            p["dependency_cache"] = list(self.dependencies)
            p["autosave"] = 0
        if extra:
            p.update(extra)
        return {"patcher": p}

    def json(self, extra=None):
        return json.dumps(self.to_dict(extra), indent=1)

    def save_maxpat(self, path, extra=None):
        with open(path, "w") as f:
            f.write(self.json(extra))
        return path

    def save_amxd(self, path, extra=None):
        """
        Write a real Live device.  Chunk layout verified by round-tripping an
        existing .amxd: sizes are little-endian u32 and the ptch payload is the
        JSON text followed by a single NUL, counted in the chunk size.
        """
        payload = self.json(extra).encode("utf-8") + b"\x00"
        blob = (b"ampf" + struct.pack("<I", 4) + b"aaaa"
                + b"meta" + struct.pack("<I", 4) + struct.pack("<I", 0)
                + b"ptch" + struct.pack("<I", len(payload)) + payload)
        with open(path, "wb") as f:
            f.write(blob)
        return path


def read_amxd(path):
    """Parse an .amxd back to a dict -- used by the build's self-check."""
    d = open(path, "rb").read()
    pos, out = 0, None
    while pos < len(d) - 8:
        cid = d[pos:pos + 4]
        size = struct.unpack("<I", d[pos + 4:pos + 8])[0]
        if cid == b"ptch":
            out = json.loads(d[pos + 8:pos + 8 + size].rstrip(b"\x00\n"))
        pos += 8 + size
    if pos != len(d):
        raise ValueError("chunk sizes do not sum to file length")
    return out
