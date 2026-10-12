# SPDX-License-Identifier: GPL-3.0-or-later
"""Build Ableton Live 11 sets: one audio track per probe, each a WAV clip at
bar 1 through its own Multiband Dynamics. Export with 'All Individual Tracks'."""
import copy, gzip, os, struct, xml.etree.ElementTree as ET

APP = "/Applications/Ableton Live 11 Suite.app/Contents/App-Resources"
TEMPLATE = APP + "/Builtin/Templates/DefaultLiveSet.als"
MBD_PRESET = APP + "/Core Library/Devices/Audio Effects/Multiband Dynamics/A Standard Multiband Comp.adv"
CLIP_SRC = APP + "/Core Library/Ableton Folder Info/Sample Reference.als"
SR = 48000
TEMPO = 120.0

# Neutral settings: every band active, ratios 1, gains 0, Live's own defaults otherwise.
NEUTRAL = dict(
    SplitLowMid=120.0, SplitMidHigh=2500.0, SplitLowMidOn=True, SplitMidHighOn=True,
    SoftKnee=False, EnvelopeIsPeak=False, OutputGain=0.0, GlobalAmount=1.0, GlobalTime=1.0,
    SoloLow=False, SoloMid=False, SoloHigh=False,
)
for b in ("Low", "Mid", "High"):
    NEUTRAL.update({f"Gain{b}": 0.0, f"InputGain{b}": 0.0, f"Active{b}": True,
                    f"AboveThreshold{b}": 0.0, f"BelowThreshold{b}": -80.0,
                    f"AboveRatio{b}": 0.0, f"BelowRatio{b}": 0.0,
                    f"Attack{b}": 10.0, f"Release{b}": 100.0})

def _gz(path):
    return ET.fromstring(gzip.open(path).read())

def _fmt(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    return repr(float(v))

def wav_write(path, data, sr=SR):
    """data: list of (L, R) float tuples or numpy (n,2). 32-bit float WAV."""
    import numpy as np
    a = np.asarray(data, dtype=np.float32)
    if a.ndim == 1:
        a = np.stack([a, a], 1)
    n, ch = a.shape
    body = a.tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 3, ch, sr, sr * ch * 4, ch * 4, 32)
    hdr += b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(hdr + body)
    return n

def wav_read(path):
    import numpy as np
    b = open(path, "rb").read()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", path
    i, fmt, data = 12, None, None
    while i + 8 <= len(b):
        cid, sz = b[i:i + 4], struct.unpack("<I", b[i + 4:i + 8])[0]
        c = b[i + 8:i + 8 + sz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", c[:16]); fmtraw = c
        elif cid == b"data":
            data = c
        i += 8 + sz + (sz & 1)
    tag, ch, sr, _, _, bits = fmt
    if tag == 0xFFFE:
        tag = struct.unpack("<H", fmtraw[24:26])[0]
    if bits == 32 and tag == 3:
        a = np.frombuffer(data, dtype="<f4")
    elif bits == 24:
        u = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3)
        x = (u[:, 0].astype(np.int32) | (u[:, 1].astype(np.int32) << 8) | (u[:, 2].astype(np.int32) << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x)
        a = x / float(1 << 23)
    elif bits == 16:
        a = np.frombuffer(data, dtype="<i2") / 32768.0
    elif bits == 32 and tag == 1:
        a = np.frombuffer(data, dtype="<i4") / 2147483648.0
    elif bits == 64:
        a = np.frombuffer(data, dtype="<f8")
    else:
        raise ValueError(f"unsupported wav {fmt}")
    return a.reshape(-1, ch).astype(np.float64), sr


class SetBuilder:
    def __init__(self):
        self.root = _gz(TEMPLATE)
        self.ls = self.root.find("LiveSet")
        tracks = self.ls.find("Tracks")
        self.track_tpl = copy.deepcopy([t for t in tracks if t.tag == "AudioTrack"][0])
        for t in [t for t in tracks if t.tag in ("AudioTrack", "MidiTrack")]:
            tracks.remove(t)
        self.mbd_tpl = copy.deepcopy(_gz(MBD_PRESET).find(".//MultibandDynamics"))
        sref = _gz(CLIP_SRC)
        self.clip_tpl = copy.deepcopy(next(sref.iter("AudioClip")))
        self.tracks = tracks
        self.n = 0
        self.max_beats = 0.0

    def add(self, name, wav_path, params, sidechain=None, device=True, automation=None):
        """automation: {param_name: [(beat, value), ...]} -- step points (Live interpolates
        linearly between events; give two events at one time for a jump)"""
        """sidechain: None, or dict(track=<name of a track in this set>, gain=lin, drywet=0..1)"""
        import numpy as np
        a, sr = wav_read(wav_path)
        assert sr == SR
        frames = a.shape[0]
        secs = frames / SR
        beats = secs * TEMPO / 60.0
        self.max_beats = max(self.max_beats, beats)
        tr = copy.deepcopy(self.track_tpl)
        tid = 100001 + self.n
        tr.set("Id", str(tid))
        nm = tr.find("Name")
        nm.find("EffectiveName").set("Value", name)
        nm.find("UserName").set("Value", name)
        dc = tr.find("DeviceChain")
        dc.find("AudioOutputRouting/Target").set("Value", "AudioOut/Master")
        dc.find("AudioInputRouting/Target").set("Value", "AudioIn/None")
        dc.find("MainSequencer/MonitoringEnum").set("Value", "2")  # Off
        # device
        mbd = copy.deepcopy(self.mbd_tpl)
        mbd.set("Id", "0")
        if not hasattr(self, "_tmp"):
            self._tmp = 10000000
        for e in mbd.iter():
            if e.tag in self._ID_TAGS and "Id" in e.attrib:
                e.set("Id", str(self._tmp)); self._tmp += 1
        p = dict(NEUTRAL); p.update(params)
        for k, v in p.items():
            e = mbd.find(k)
            if e is None:
                raise KeyError(k)
            m = e.find("Manual")
            (m if m is not None else e).set("Value", _fmt(v))
        mbd.find("UserName").set("Value", "")
        lp = mbd.find("LastPresetRef")
        for c in list(lp):
            lp.remove(c)
        ET.SubElement(lp, "Value")
        if sidechain:
            sidechain = dict(sidechain)
            sidechain.setdefault("target", "AudioIn/Track.%d/PostFxOut" % sidechain["track_id"])
            sc = mbd.find("SideChain")
            sc.find("OnOff/Manual").set("Value", "true")
            sc.find("RoutedInput/Routable/Target").set("Value", sidechain["target"])
            sc.find("RoutedInput/Routable/UpperDisplayString").set("Value", sidechain.get("upper", "sc"))
            sc.find("RoutedInput/Routable/LowerDisplayString").set("Value", sidechain.get("lower", "Post FX"))
            sc.find("RoutedInput/Volume/Manual").set("Value", _fmt(sidechain.get("gain", 1.0)))
            sc.find("DryWet/Manual").set("Value", _fmt(sidechain.get("drywet", 1.0)))
            if sidechain.get("listen"):
                mbd.find("SideListen").set("Value", "true")
        if device:
            dc.find("DeviceChain/Devices").append(mbd)
        if automation:
            envs = tr.find("AutomationEnvelopes/Envelopes")
            for k, (pname, pts) in enumerate(automation.items()):
                tgt = mbd.find(pname + "/AutomationTarget")
                env = ET.SubElement(envs, "AutomationEnvelope", Id=str(k))
                ET.SubElement(ET.SubElement(env, "EnvelopeTarget"), "PointeeId", Value=tgt.get("Id"))
                au = ET.SubElement(env, "Automation"); ev = ET.SubElement(au, "Events")
                ET.SubElement(ev, "FloatEvent", Id="0", Time="-63072000", Value=_fmt(pts[0][1]))
                for j, (bt, v) in enumerate(pts):
                    ET.SubElement(ev, "FloatEvent", Id=str(j + 1), Time=repr(float(bt)), Value=_fmt(v))
                tv = ET.SubElement(au, "AutomationTransformViewState")
                ET.SubElement(tv, "IsTransformPending", Value="false"); ET.SubElement(tv, "TimeAndValueTransforms")
        # clip
        clip = copy.deepcopy(self.clip_tpl)
        clip.set("Time", "0")
        clip.find("CurrentStart").set("Value", "0")
        clip.find("CurrentEnd").set("Value", repr(beats))
        lo = clip.find("Loop")
        lo.find("LoopStart").set("Value", "0")
        for k in ("LoopEnd", "OutMarker", "HiddenLoopEnd"):
            lo.find(k).set("Value", repr(secs))
        lo.find("LoopOn").set("Value", "false")
        clip.find("Name").set("Value", name)
        clip.find("IsWarped").set("Value", "false")
        clip.find("Fade").set("Value", "false")
        clip.find("HiQ").set("Value", "true")
        fr = clip.find("SampleRef/FileRef")
        ap = os.path.abspath(wav_path)
        fr.find("RelativePathType").set("Value", "0")
        fr.find("RelativePath").set("Value", "")
        fr.find("Path").set("Value", ap)
        fr.find("Type").set("Value", "1")
        fr.find("LivePackName").set("Value", "")
        fr.find("LivePackId").set("Value", "")
        fr.find("OriginalFileSize").set("Value", str(os.path.getsize(ap)))
        fr.find("OriginalCrc").set("Value", "0")
        sr_ = clip.find("SampleRef")
        sc_ = sr_.find("SourceContext")
        for c in list(sc_):
            sc_.remove(c)
        sr_.find("LastModDate").set("Value", str(int(os.path.getmtime(ap))))
        sr_.find("DefaultDuration").set("Value", str(frames))
        sr_.find("DefaultSampleRate").set("Value", str(SR))
        wm = clip.find("WarpMarkers")
        for c in list(wm):
            wm.remove(c)
        ET.SubElement(wm, "WarpMarker", Id="0", SecTime="0", BeatTime="0")
        ET.SubElement(wm, "WarpMarker", Id="1", SecTime=repr(secs), BeatTime=repr(beats))
        ev = dc.find("MainSequencer/Sample/ArrangerAutomation/Events")
        ev.append(clip)
        # insert before return tracks
        idx = len([t for t in self.tracks if t.tag == "AudioTrack"])
        self.tracks.insert(idx, tr)
        self.n += 1
        return tid

    _ID_TAGS = ("AutomationTarget", "ModulationTarget", "VolumeModulationTarget",
                "TranspositionModulationTarget", "GrainSizeModulationTarget",
                "FluxModulationTarget", "SampleOffsetModulationTarget", "Pointee")

    def _renumber(self):
        """Fresh pointee ids for the tracks we added only; template ids are untouched,
        and any PointeeId inside an added track follows its target."""
        nid = int(self.ls.find("NextPointeeId").get("Value"))
        for t in self.tracks:
            if t.tag != "AudioTrack":
                continue
            remap = {}
            for e in t.iter():
                if e.tag in self._ID_TAGS and "Id" in e.attrib:
                    remap[e.get("Id")] = str(nid)
                    e.set("Id", str(nid))
                    nid += 1
            for e in t.iter("PointeeId"):
                v = e.get("Value")
                if v in remap:
                    e.set("Value", remap[v])
        clip_id = 0
        for c in self.root.iter("AudioClip"):
            c.set("Id", str(clip_id)); clip_id += 1
        self.ls.find("NextPointeeId").set("Value", str(nid + 10))

    def save(self, path, tail_beats=4.0):
        self._renumber()
        tp = self.ls.find("Transport")
        total = self.max_beats + tail_beats
        total = float(int(total * 4 + 1)) / 4.0
        tp.find("LoopStart").set("Value", "0")
        tp.find("LoopLength").set("Value", repr(total))
        tp.find("LoopOn").set("Value", "true")
        ts = self.ls.find("TimeSelection")
        ts.find("AnchorTime").set("Value", "0")
        ts.find("OtherTime").set("Value", repr(total))
        tempo = self.ls.find("MasterTrack/DeviceChain/Mixer/Tempo/Manual")
        tempo.set("Value", repr(TEMPO))
        data = b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(self.root, encoding="utf-8")
        with gzip.open(path, "wb") as f:
            f.write(data)
        return total
