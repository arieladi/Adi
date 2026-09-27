#!/usr/bin/env python3
"""
Structural checks on the generated device.

None of this proves the DSP is right -- only Max can do that -- but it does
catch the whole class of errors that hand-built or generated patcher JSON is
prone to: dangling patchlines, out-of-range inlet/outlet indices, presentation
rects that overflow Live's 155 px strip, and, most usefully, messages the patch
sends to a js object that the js file has no handler for (and varnames the js
looks up that the patch never defines).

Run:  python3 test/validate.py
"""

import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEV = os.path.join(ROOT, "device")

errors, warnings = [], []


def err(m):
    errors.append(m)


def warn(m):
    warnings.append(m)


def read_amxd(path):
    d = open(path, "rb").read()
    pos, out = 0, None
    while pos < len(d) - 8:
        cid = d[pos:pos + 4]
        size = struct.unpack("<I", d[pos + 8 - 4:pos + 8])[0]
        if cid == b"ptch":
            out = json.loads(d[pos + 8:pos + 8 + size].rstrip(b"\x00\n"))
        pos += 8 + size
    if pos != len(d):
        err("%s: chunk sizes do not sum to file length (%d vs %d)"
            % (os.path.basename(path), pos, len(d)))
    return out


def walk_boxes(patcher):
    for b in patcher.get("boxes", []):
        yield b["box"]


def check_patcher(name, patcher, max_pres=None):
    boxes = {b["id"]: b for b in walk_boxes(patcher)}
    if len(boxes) != len(patcher.get("boxes", [])):
        err("%s: duplicate object ids" % name)

    for ln in patcher.get("lines", []):
        pl = ln["patchline"]
        sid, sout = pl["source"]
        did, din = pl["destination"]
        if sid not in boxes:
            err("%s: patchline from unknown object %s" % (name, sid))
            continue
        if did not in boxes:
            err("%s: patchline to unknown object %s" % (name, did))
            continue
        sb, db = boxes[sid], boxes[did]
        no = sb.get("numoutlets", 1)
        ni = db.get("numinlets", 1)
        if sout >= max(no, 1):
            err("%s: %r outlet %d >= numoutlets %d"
                % (name, sb.get("text", sb.get("maxclass")), sout, no))
        if din >= max(ni, 1):
            err("%s: -> %r inlet %d >= numinlets %d"
                % (name, db.get("text", db.get("maxclass")), din, ni))

    # parameters block must reference real, parameter-enabled boxes
    params = patcher.get("parameters", {})
    for pid, val in params.items():
        if pid in ("parameterbanks", "inherited_shortname"):
            continue
        if pid not in boxes:
            err("%s: parameters block references missing %s" % (name, pid))
        elif not boxes[pid].get("parameter_enable"):
            err("%s: %s in parameters block but parameter_enable is not set"
                % (name, pid))

    # Live's native Int holds only 256 values and it SILENTLY rewrites mmax to
    # mmin+255 on save.  Anything wider must be Float type with an Int unit
    # style, per Max's own parameter documentation.
    for b in boxes.values():
        if not b.get("parameter_enable"):
            continue
        sa = b.get("saved_attribute_attributes", {}).get("valueof", {})
        if sa.get("parameter_type") != 1:
            continue
        lo, hi = sa.get("parameter_mmin"), sa.get("parameter_mmax")
        if lo is None or hi is None:
            continue
        if (hi - lo) > 255:
            err("%s: %s is Int with range %s..%s (%d values); Live silently "
                "clamps Int to 256 values -- use Float type with an Int unit "
                "style" % (name, sa.get("parameter_longname"), lo, hi,
                           hi - lo + 1))

    # every live.* parameter must be Stored Only
    for b in boxes.values():
        if str(b.get("maxclass", "")).startswith("live.") and \
                b.get("parameter_enable"):
            sa = b.get("saved_attribute_attributes", {}).get("valueof", {})
            if sa.get("parameter_invisible") != 2:
                err("%s: %s is not 'Stored Only' (parameter_invisible=%r)"
                    % (name, sa.get("parameter_longname"),
                       sa.get("parameter_invisible")))

    if max_pres:
        mw, mh = max_pres
        for b in boxes.values():
            if not b.get("presentation"):
                continue
            r = b.get("presentation_rect")
            if not r:
                continue
            if r[0] + r[2] > mw + 0.5 or r[1] + r[3] > mh + 0.5:
                err("%s: %s presentation rect %s exceeds %dx%d"
                    % (name, b.get("varname", b.get("maxclass")), r, mw, mh))
    return boxes


def js_handlers(path):
    src = open(path).read()
    return set(re.findall(r"^function\s+([A-Za-z_$][\w$]*)\s*\(", src, re.M))


def js_getnamed(path):
    src = open(path).read()
    names = set(re.findall(r'getnamed\(\s*"([^"]+)"\s*\)', src))
    # names built as prefix + suffix, e.g. "src" + tag / prefix + (i+1)
    return names


def main():
    # ---- files present ----
    need = ["AVC Spectrum Meter.amxd", "AVC Spectrum Meter.maxpat",
            "avc.fftanalysis.maxpat", "avc.specreduce.genjit",
            "avc.capgonio.maxpat", "avc.capscope.maxpat",
            "avc.specui.js", "avc.engine.js", "avc.meters.js"]
    for f in need:
        if not os.path.exists(os.path.join(DEV, f)):
            err("missing file: %s" % f)
    if errors:
        report()
        return

    main_pat = read_amxd(os.path.join(DEV, "AVC Spectrum Meter.amxd"))["patcher"]
    fft_pat = json.load(open(os.path.join(DEV, "avc.fftanalysis.maxpat")))["patcher"]
    gen_pat = json.load(open(os.path.join(DEV, "avc.specreduce.genjit")))["patcher"]

    boxes = check_patcher("device", main_pat, max_pres=(960, 155))
    check_patcher("fftanalysis", fft_pat)
    for nm in ("capgonio", "capscope"):
        check_patcher(nm, json.load(open(
            os.path.join(DEV, "avc.%s.maxpat" % nm)))["patcher"])
    check_patcher("specreduce", gen_pat)

    # ---- .amxd and .maxpat must agree ----
    plain = json.load(open(os.path.join(DEV, "AVC Spectrum Meter.maxpat")))["patcher"]
    if len(plain["boxes"]) != len(main_pat["boxes"]):
        err("the .amxd and .maxpat contain different object counts")

    texts = [b.get("text", "") for b in boxes.values()]

    # ---- audio path: plugin~ must reach plugout~ directly on both channels ----
    pin = [b for b in boxes.values() if b.get("text") == "plugin~"]
    pout = [b for b in boxes.values() if b.get("text") == "plugout~"]
    if len(pin) != 1 or len(pout) != 1:
        err("expected exactly one plugin~ and one plugout~")
    else:
        direct = set()
        for ln in main_pat["lines"]:
            pl = ln["patchline"]
            if pl["source"][0] == pin[0]["id"] and pl["destination"][0] == pout[0]["id"]:
                direct.add((pl["source"][1], pl["destination"][1]))
        if direct != {(0, 0), (1, 1)}:
            err("audio path is not a straight plugin~ -> plugout~: %s" % sorted(direct))
        # nothing else may feed plugout~
        feeders = {ln["patchline"]["source"][0] for ln in main_pat["lines"]
                   if ln["patchline"]["destination"][0] == pout[0]["id"]}
        if feeders != {pin[0]["id"]}:
            err("plugout~ has inputs other than plugin~ -- the device would not "
                "null: %s" % feeders)

    # ---- cross-file: messages sent to js objects must have handlers ----
    js_boxes = {}
    for b in boxes.values():
        t = b.get("text", "")
        if t.startswith("js ") or b.get("maxclass") == "jsui":
            fn = t.split()[1] if t.startswith("js ") else b.get("filename")
            js_boxes[b["id"]] = fn
    handlers = {}
    for fid in set(js_boxes.values()):
        pth = os.path.join(DEV, fid)
        if os.path.exists(pth):
            handlers[fid] = js_handlers(pth)
        else:
            err("js file referenced but missing: %s" % fid)

    # find "prepend X" / message boxes feeding a js object
    id2box = boxes
    for ln in main_pat["lines"]:
        pl = ln["patchline"]
        dst = pl["destination"][0]
        if dst not in js_boxes or pl["destination"][1] != 0:
            continue
        src = id2box.get(pl["source"][0], {})
        txt = src.get("text", "")
        sel = None
        if txt.startswith("prepend "):
            sel = txt.split()[1]
        elif src.get("maxclass") == "message":
            first = txt.split()
            if first and re.match(r"^[A-Za-z_]", first[0]):
                sel = first[0]
        if sel is None:
            continue
        fn = js_boxes[dst]
        hs = handlers.get(fn, set())
        if sel not in hs:
            err("patch sends %r to %s, which has no handler for it" % (sel, fn))

    # ---- cross-file: getnamed() targets must exist as varnames ----
    varnames = {b.get("varname") for b in boxes.values() if b.get("varname")}
    literal = {
        "avc.engine.js": ["srcA", "srcB", "redA", "redB", "selA", "selB",
                          "geomA", "geomB",
                          "spillA0", "spillA1", "spillB0", "spillB1",
                          "matA", "matB"],
        "avc.meters.js": ["wL1", "wL2", "wL3", "wR1", "wR2", "wR3",
                          "kL1", "kL2", "kR1", "kR2"],
    }
    for fn, names in literal.items():
        for n in names:
            if n not in varnames:
                err("%s looks up varname %r, which no object in the device "
                    "defines" % (fn, n))

    # ---- the pfft~ subpatch must poke into the matrix the device declares ----
    fft_boxes = {b["id"]: b for b in walk_boxes(fft_pat)}
    pokes = [b for b in fft_boxes.values()
             if str(b.get("text", "")).startswith("jit.poke~")]
    if len(pokes) != 5:
        err("expected 5 jit.poke~ (one per analysis plane), found %d" % len(pokes))
    for b in pokes:
        parts = b["text"].split()
        if parts[1] != "#1" or parts[2] != "2":
            err("jit.poke~ should target #1 with dim_inputcount 2: %r" % b["text"])
    if not any("jit.matrix #0_specA" in t for t in texts):
        err("device does not declare the #0_specA analysis matrix")

    # ---- gen patcher must declare its namespace, or it silently will not
    # compile and jit.gen degrades to a passthrough ----
    if gen_pat.get("classnamespace") != "jit.gen":
        err("specreduce.genjit is missing classnamespace 'jit.gen' -- jit.gen "
            "would pass its left input straight through")
    for b in boxes.values():
        t = str(b.get("text", ""))
        if t.startswith("jit.gen") and "@gen" not in t:
            err("%r must reference its gen patcher with @gen; a bare argument "
                "does not load it" % t)

    # ---- expr has no ternary operator ----
    # Max's expr documents + - * / %, the bitwise/logical set and comparisons.
    # `?:` is not among them: such an object never outputs, and everything
    # downstream sits at its creation default forever -- silently.
    for b in boxes.values():
        t = str(b.get("text", ""))
        if t.startswith("expr ") and "?" in t:
            err("%r uses a ternary; Max's expr has no ?: operator and the "
                "object will never output" % t)

    # ---- expr feeding an int-only destination must emit an int ----
    # `select` with integer arguments never matches a float, and selector~ /
    # gate / switch only accept ints, so a $f expr silently leaves them at 0.
    INT_ONLY = ("sel ", "select ", "selector~", "gate", "switch")
    for ln in main_pat["lines"]:
        pl = ln["patchline"]
        if pl["destination"][1] != 0:
            continue
        src = boxes.get(pl["source"][0], {})
        dst = boxes.get(pl["destination"][0], {})
        stxt, dtxt = str(src.get("text", "")), str(dst.get("text", ""))
        if not stxt.startswith("expr "):
            continue
        if "$f" not in stxt:
            continue
        if any(dtxt.startswith(k) for k in INT_ONLY):
            err("%r emits a float into %r, which only accepts an int -- use $i"
                % (stxt, dtxt))

    # ---- no exponent notation in object text ----
    # Scientific notation appears nowhere in the documentation for expr (whose
    # function list is spelled out in full) or for object-box arguments.  Two
    # separate faults have now come from assuming it parses: a clip~ limit that
    # would silently become 0, and four expr objects -- correlation, balance
    # and both LUFS meters -- that produced no output at all, which is why the
    # correlation meter sat at pack's default of 0.0 and never moved.
    for bid, b in boxes.items():
        t = str(b.get("text", ""))
        if b.get("maxclass") not in ("newobj", "message"):
            continue
        m = re.search(r"(?<![A-Za-z0-9_.])\d+(?:\.\d+)?[eE][-+]?\d+", t)
        if m:
            err("%r uses exponent notation (%r); Max does not document it for "
                "object arguments -- write the constant out in full"
                % (t, m.group(0)))

    # ---- the visibility gate must actually be wired ----
    # It is the difference between the device costing its full analysis while
    # nobody is looking and costing almost nothing, and when the LiveAPI version
    # of it failed silently there was no way to tell from the outside.  These
    # are the links that carry it.
    texts = {bid: str(b.get("text", "")) for bid, b in boxes.items()}
    # object boxes only -- the comments in this patcher discuss jit.poke~ and
    # friends at length, and matching their text would fire the rules below.
    otexts = [str(b.get("text", "")) for b in boxes.values()
              if b.get("maxclass") == "newobj"]
    def feeds(pred_src, pred_dst):
        for ln in main_pat["lines"]:
            pl = ln["patchline"]
            st_ = texts.get(pl["source"][0], "")
            dt_ = texts.get(pl["destination"][0], "")
            if pred_src(st_) and pred_dst(dt_):
                return True
        return False

    if not any(t.startswith("live.path this_device") for t in texts.values()):
        err("no `live.path this_device`: the visibility gate cannot find its own track")
    # The selection must be watched by pathing straight to it and reading
    # outlet 1.  `live.path live_set view` plus a live.observer looks equivalent
    # and is not: it silently never binds, the observer emits nothing ever, and
    # every feature downstream of the comparison is dead with no error anywhere.
    sel_lp = [bid for bid, t in texts.items()
              if t == "live.path live_set view selected_track"]
    if not sel_lp:
        err("no `live.path live_set view selected_track`: the visibility gate "
            "can never fire")
    else:
        outs = [ln["patchline"]["source"][1] for ln in main_pat["lines"]
                if ln["patchline"]["source"][0] == sel_lp[0]]
        if 1 not in outs:
            err("`live.path live_set view selected_track` outlet 1 is not "
                "connected, so selection changes are never noticed")
    if any(t == "live.path live_set view" for t in texts.values()):
        err("a bare `live.path live_set view` is back; it does not bind")
    if not feeds(lambda t: t.startswith("change"),
                 lambda t: t.startswith("sel 0 1")):
        err("the visibility comparison does not reach the redraw-rate switch")
    if not feeds(lambda t: t.startswith("change"),
                 lambda t: t == "prepend visible"):
        err("the visibility comparison does not reach `prepend visible`")
    if not feeds(lambda t: t == "prepend visible", lambda t: t.startswith("js ")):
        err("`prepend visible` does not reach the engine, so nothing is muted")

    # ---- the device's self-switch ----
    # The construction is copied from Ableton's own BeatSeeker.amxd and every
    # link in it is load-bearing in a way that fails SILENTLY: a missing
    # deferlow means Live simply refuses the change ("changes cannot be
    # triggered by notifications") and the device never switches; a broken
    # `append parameters 0` addresses the DEVICE instead of its Device On
    # parameter, and `set value` on a device is ignored.  Neither posts anything
    # useful, so both are asserted here.
    if not any(t == "append parameters 0" for t in otexts):
        err("no `append parameters 0`: the self-switch cannot address the "
            "Device On parameter")
    if not feeds(lambda t: t == "deferlow", lambda t: t == "set value $1"):
        err("`set value $1` is not fed through deferlow: Live refuses a change "
            "triggered inside an observer notification")
    if not feeds(lambda t: t.startswith("gate"), lambda t: t == "deferlow"):
        err("the self-switch is not gated on AutoOff")
    # ...and the gate must actually be told which way to point.  An unconnected
    # control inlet leaves `gate 1 1` permanently OPEN, so the AutoOff switch
    # would do nothing and the device would keep switching itself off with no
    # way for the user to stop it.
    auto = [bid for bid, b in boxes.items() if b.get("varname") == "AutoOff"]
    if not auto:
        err("no AutoOff control")
    else:
        ctl = [ln for ln in main_pat["lines"]
               if ln["patchline"]["source"][0] == auto[0]
               and texts.get(ln["patchline"]["destination"][0], "").startswith("gate")
               and ln["patchline"]["destination"][1] == 0]
        if not ctl:
            err("AutoOff does not reach a gate's control inlet: the switch is "
                "inert and the device cannot be stopped from self-toggling")
    if not feeds(lambda t: t == "set value $1",
                 lambda t: t.startswith("live.object")):
        err("`set value $1` does not reach a live.object")
    # The live.path that resolves the parameter must be read from OUTLET 1.
    # Outlet 0 fires only for goto/bang/getid, never for a `path` message, so
    # reading it binds live.object to nothing and every `set value` is dropped
    # without a word.  This exact mistake shipped once.
    lp_app = [ln["patchline"]["destination"][0] for ln in main_pat["lines"]
              if texts.get(ln["patchline"]["source"][0]) == "append parameters 0"]
    for bid in lp_app:
        outs = [ln["patchline"]["source"][1] for ln in main_pat["lines"]
                if ln["patchline"]["source"][0] == bid]
        if 0 in outs:
            err("the self-switch reads live.path outlet 0, which only answers "
                "goto/bang/getid -- it must read outlet 1")
        if 1 not in outs:
            err("the self-switch's live.path outlet 1 is not connected")

    # ---- the device's own power switch must be wired ----
    # live.thisdevice's MIDDLE outlet (index 1) reports enable/disable.  Live
    # removes a disabled device's MSP from its graph on its own, but it leaves
    # the patcher running -- so if this outlet is unconnected, switching the
    # device OFF still leaves it repainting at full frame rate forever.  Live's
    # CPU meter measures audio only and cannot show that, so nothing about the
    # symptom points at the cause; the wiring has to be asserted here instead.
    td = [bid for bid, t in texts.items() if t == "live.thisdevice"]
    if not td:
        err("no live.thisdevice")
    else:
        out1 = [ln for ln in main_pat["lines"]
                if ln["patchline"]["source"] == [td[0], 1]]
        if not out1:
            err("live.thisdevice outlet 1 (enabled state) is not connected: "
                "a device switched OFF keeps drawing")
        # ...and it must SLOW the clock, never gate the frame chain shut.  A
        # closed gate turns any stuck power switch into a permanently blank
        # device -- no plot, no meters, no settings screen, so no way to reach
        # the Auto toggle and stop it.  That shipped once and stranded the user.
        for ln in out1:
            dt_ = texts.get(ln["patchline"]["destination"][0], "")
            if dt_.startswith("gate"):
                err("live.thisdevice outlet 1 drives a gate: a stuck power "
                    "switch would leave the display permanently blank")

    # ---- every control on the settings screen must be described ----
    # The settings screen is 33 unlabelled boxes unless this holds.  A control
    # added without a caption and a description is invisible in the worst way:
    # it looks like the others, so the user assumes it is documented somewhere
    # and that they have missed it.  Checked in both directions -- an orphaned
    # description is a control that was renamed or removed and left a lie behind.
    ui_txt = open(os.path.join(DEV, "avc.specui.js")).read()
    on_screen = set()
    for b in boxes.values():
        sa = b.get("saved_attribute_attributes", {}).get("valueof")
        pr = b.get("presentation_rect")
        if sa and pr and pr[1] >= 20:
            on_screen.add(sa["parameter_longname"])
    m = re.search(r"var DESC = \{(.*?)\n\};", ui_txt, re.S)
    described = set(re.findall(r"(\w+)\s*:\s*\"", m.group(1))) if m else set()
    for nm in sorted(on_screen - described):
        err("settings screen: %s has no description in DESC" % nm)
    for nm in sorted(described - on_screen):
        err("DESC describes %s, which is not a control on the settings screen"
            % nm)
    # ...and the patch must actually tell the screen which control moved
    if not any(t.startswith("touchedby ") for t in texts.values()):
        err("no `touchedby` messages: the settings screen cannot say what "
            "the control you just moved does")

    # ---- a stored setting must actually be READ ----
    # The old rule only checked that a handler function with the message's name
    # existed.  Hold and Float both passed it for months while doing nothing at
    # all: the control was wired, the message arrived, the handler stored the
    # value into cfg, and no code ever looked at it again.  A control that is
    # visibly present and inert is worse than a missing one, because the user
    # reasonably concludes the feature is broken rather than absent.
    ui = open(os.path.join(DEV, "avc.specui.js")).read()
    for m in re.finditer(r"function\s+(\w+)\s*\([^)]*\)\s*\{[^}]*?"
                         r"cfg\.(\w+)\s*=", ui):
        fn, field = m.group(1), m.group(2)
        # Occurrences outside the setter itself.  The declaration inside the
        # cfg object literal is written `field: value` with no `cfg.` prefix,
        # so it is already excluded and must NOT be discounted again -- doing
        # that is what made the first version of this rule flag ten working
        # controls whose value happens to be read exactly once.
        real = [mm for mm in re.finditer(r"cfg\.%s\b" % field, ui)
                if not (m.start() < mm.start() < m.end())]
        if not real:
            err("avc.specui.js: %s() stores cfg.%s and nothing ever reads it "
                "-- the control is inert" % (fn, field))

    # ---- every font change must go through setFont() ----
    # Two reasons.  CPU: a sampled profile of Live showed
    # CTFontCreateWithGraphicsFont inside jsui_paint, so a redundant
    # select_font_face is a real cost, and only setFont() skips redundant ones.
    # CORRECTNESS: setFont() also maintains the key tw() caches text widths
    # under, so a direct call silently makes every cached measurement belong to
    # the wrong font -- which misplaces labels rather than crashing, and so
    # would never be noticed.
    ui_src = open(os.path.join(DEV, "avc.specui.js")).read()
    for n, line in enumerate(ui_src.split("\n"), 1):
        code = line.split("//")[0]
        if "*" in line.strip()[:2]:
            continue                      # inside a block comment
        for call in ("select_font_face(", "set_font_size("):
            if call not in code:
                continue
            if code.strip() in ("mgraphics.select_font_face(face);",
                                "mgraphics.set_font_size(size);"):
                continue                  # the two calls inside setFont itself
            err("avc.specui.js:%d bypasses setFont(): %s"
                % (n, line.strip()))

    # ---- a spill must ask for exactly what its matrix holds ----
    # jit.spill @listlength emits that many values whatever the matrix is, so a
    # mismatch does not error -- it silently truncates the capture or pads it
    # with zeros.  The scope matrix just went from 1024 to 1536 cells to carry
    # three bands instead of two channels; had the spill stayed at 1024 the top
    # band would simply have been missing, and the picture would have looked
    # plausible.
    for ln in main_pat["lines"]:
        src = texts.get(ln["patchline"]["source"][0], "")
        dst = texts.get(ln["patchline"]["destination"][0], "")
        if not src.startswith("jit.matrix") or not dst.startswith("jit.spill"):
            continue
        mw = re.search(r"jit\.matrix\s+\S+\s+\d+\s+\w+\s+(\d+)", src)
        sl = re.search(r"@listlength\s+(\d+)", dst)
        if mw and sl and int(mw.group(1)) != int(sl.group(1)):
            err("%s feeds a spill asking for %s values, not %s -- the capture "
                "is silently truncated" % (src, sl.group(1), mw.group(1)))

    # ---- the audio-rate capture voices ----
    # These are the only per-sample Jitter writes in the device, and the only
    # reason they are poly~ at all is that poly~ can be muted.  If a voice ends
    # up wired straight into the patch again, or the JS stops addressing it,
    # the device silently goes back to writing 176000 matrix cells a second for
    # a goniometer nobody is looking at -- which costs CPU and shows no symptom.
    eng_src = open(os.path.join(DEV, "avc.engine.js")).read()
    for nm, mat in (("capgonio", "gonio"), ("capscope", "scope")):
        vox = [t for t in otexts if t.startswith("poly~ avc.%s " % nm)]
        if not vox:
            err("no `poly~ avc.%s`: the capture chain cannot be muted" % nm)
        elif "args #0_%s" % mat not in vox[0]:
            err("`poly~ avc.%s` is not passed the #0_%s matrix name" % (nm, mat))
        if not feeds(lambda t: t == "plugin~",
                     lambda t, n=nm: t.startswith("poly~ avc.%s " % n)):
            err("plugin~ does not reach `poly~ avc.%s`" % nm)
        if ('"%s"' % nm) not in eng_src:
            err("avc.engine.js never names `%s`, so it is never muted" % nm)
    if any(t.startswith("jit.poke~") for t in otexts):
        err("a jit.poke~ is back in the main patcher, outside a mutable voice")

    # ---- average~ window length must arrive as an int ----
    # average~'s documented methods are int / absolute / bipolar / rms /
    # signal.  There is NO float method, so a float window is not rounded --
    # it is rejected outright, and the object silently keeps whatever interval
    # its creation argument installed.  An interval larger than that argument
    # is discarded just as silently.  This is what left a "300 ms" RMS running
    # a 4.35 s window and reading several dB low against every other meter.
    CTRL_SRC = ("route ", "js ", "unpack ", "r ", "receive ", "prepend ")
    for ln in main_pat["lines"]:
        pl = ln["patchline"]
        dst = boxes.get(pl["destination"][0], {})
        dtxt = str(dst.get("text", ""))
        if not dtxt.startswith("average~"):
            continue
        src = boxes.get(pl["source"][0], {})
        stxt = str(src.get("text", ""))
        bad = stxt.startswith(CTRL_SRC)
        if stxt.startswith("expr ") and "$f" in stxt:
            body = stxt[5:].strip()
            bad = bad or not (body.startswith("int(") and body.endswith(")"))
        if bad:
            err("%r feeds a control value straight into %r, which has no float "
                "method and ignores it silently -- coerce with expr int($f1)"
                % (stxt, dtxt))

    # ---- gen codebox sanity ----
    code = [b.get("code", "") for b in walk_boxes(gen_pat)
            if b.get("maxclass") == "codebox"]
    if not code:
        err("specreduce has no codebox")
    else:
        c = code[0]
        for tok in ("Param wid", "Param nbins", "Param binhz", "samplepix",
                    "out1"):
            if tok not in c:
                err("specreduce codebox is missing %r" % tok)

    # ---- unconnected objects are usually a wiring mistake ----
    used = set()
    for ln in main_pat["lines"]:
        used.add(ln["patchline"]["source"][0])
        used.add(ln["patchline"]["destination"][0])
    # autopattr binds by patcher scope, not by patchcord
    NO_CORDS_OK = ("autopattr", "comment")
    for b in boxes.values():
        if b["id"] in used or b.get("maxclass") == "comment":
            continue
        if str(b.get("text", "")).split(" ")[0] in NO_CORDS_OK:
            continue
        warn("unconnected: %s" % (b.get("text") or b.get("maxclass")))

    report()


def report():
    for w in warnings:
        print("WARN  " + w)
    for e in errors:
        print("ERROR " + e)
    print("\n%d error(s), %d warning(s)" % (len(errors), len(warnings)))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
